//! Own only the C-backend child launched by this application. Never adopt/kill
//! a listener by port, version or process name. Default application mode is offline.
use crate::backend::BackendClient;
use anyhow::{Context, Result, bail};
use std::{
    net::TcpListener,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

#[derive(Clone, Debug)]
pub struct HostConfig {
    pub port: u16,
    pub home: PathBuf,
    pub binary: PathBuf,
    pub resources: PathBuf,
    pub validation: bool,
}
impl HostConfig {
    pub fn from_environment(validation: bool) -> Result<Self> {
        let port = if validation {
            std::env::var("METALSHARP_GPUI_PORT")
                .ok()
                .map(|v| v.parse())
                .transpose()
                .context("Invalid GPUI validation port")?
                .unwrap_or(9276)
        } else {
            9274
        };
        if port == 0 || (validation && port == 9274) {
            bail!("validation must use a non-production, nonzero port");
        }
        let user_home = PathBuf::from(std::env::var_os("HOME").context("HOME unavailable")?);
        let home = if validation {
            std::env::var_os("METALSHARP_GPUI_HOME")
                .map(PathBuf::from)
                .unwrap_or_else(|| std::env::temp_dir().join("metalsharp-gpui-validation"))
        } else {
            user_home.join(".metalsharp")
        };
        if validation {
            validate_home(&home, &user_home)?;
        }
        let executable = std::env::current_exe()?;
        let packaged = executable
            .parent()
            .context("Executable has no parent")?
            .join("../Resources");
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let resources = if packaged.is_dir() {
            packaged.clone()
        } else {
            repo.join("app")
        };
        let binary = if let Some(path) = std::env::var_os("METALSHARP_GPUI_BACKEND") {
            PathBuf::from(path)
        } else if packaged.is_dir() {
            resources.join("runtime/metalsharp-backend")
        } else {
            repo.join("app/src-c/build/metalsharp-backend")
        };
        if !binary.is_file() {
            bail!("Build/package the C backend before enabling connected mode");
        }
        Ok(Self {
            port,
            home,
            binary: binary
                .canonicalize()
                .context("Resolve C backend executable")?,
            resources,
            validation,
        })
    }
}

pub struct BackendHost {
    child: Child,
    client: BackendClient,
    binary_name: String,
}

/// PIDs of `metalsharp-backend` processes listening on `port` (Electron's
/// `getListeningBackendPid`): never a foreign listener.
fn listening_backend_pids(port: u16) -> Vec<libc::pid_t> {
    let Ok(output) = Command::new("/usr/sbin/lsof")
        .args(["-nP", &format!("-tiTCP:{port}"), "-sTCP:LISTEN"])
        .stderr(Stdio::null())
        .output()
    else {
        return Vec::new();
    };
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .filter_map(|pid| pid.parse::<libc::pid_t>().ok())
        .filter(|pid| *pid > 0)
        .filter(|pid| {
            Command::new("/bin/ps")
                .args(["-o", "comm=", "-p", &pid.to_string()])
                .output()
                .map(|out| {
                    String::from_utf8_lossy(&out.stdout)
                        .trim()
                        .ends_with("metalsharp-backend")
                })
                .unwrap_or(false)
        })
        .collect()
}

/// Production launches own a freshly spawned packaged backend, exactly like
/// Electron's BackendBridge.shouldRestart(): an orphan/older backend on the
/// production port is terminated (SIGTERM, then SIGKILL after 3 s).
fn terminate_stale_production_backend(port: u16) {
    for pid in listening_backend_pids(port) {
        unsafe {
            libc::kill(pid, libc::SIGTERM);
        }
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline && unsafe { libc::kill(pid, 0) } == 0 {
            thread::sleep(Duration::from_millis(200));
        }
        unsafe {
            libc::kill(pid, libc::SIGKILL);
        }
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    while Instant::now() < deadline && TcpListener::bind(("127.0.0.1", port)).is_err() {
        thread::sleep(Duration::from_millis(150));
    }
}

impl BackendHost {
    pub fn start(config: &HostConfig) -> Result<Self> {
        if config.port == 0 {
            bail!("Invalid backend port");
        }
        if !config.validation {
            terminate_stale_production_backend(config.port);
        }
        let reservation = TcpListener::bind(("127.0.0.1", config.port)).context(
            "Backend port is already occupied; no foreign process will be adopted or killed",
        )?;
        // Production matches Electron: no session token, because the updater
        // script and other local helpers query :9274/status unauthenticated.
        let token = if config.validation {
            let mut entropy = [0_u8; 32];
            if unsafe { libc::getentropy(entropy.as_mut_ptr().cast(), entropy.len()) } != 0 {
                bail!("Could not generate backend session authentication");
            }
            Some(
                entropy
                    .iter()
                    .map(|byte| format!("{byte:02x}"))
                    .collect::<String>(),
            )
        } else {
            None
        };
        std::fs::create_dir_all(&config.home)?;
        let mut command = Command::new(&config.binary);
        crate::lifecycle::unmask_child_signals(&mut command);
        let installer = if config
            .resources
            .join("scripts/tools/install-homebrew.sh")
            .is_file()
        {
            config.resources.join("scripts/tools/install-homebrew.sh")
        } else {
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .parent()
                .unwrap()
                .join("tools/install-homebrew.sh")
        };
        command.current_dir(&config.resources);
        command.env_remove("METALSHARP_CLIENT_TOKEN");
        if let Some(token) = &token {
            command.env("METALSHARP_CLIENT_TOKEN", token);
        }
        command
            .env("METALSHARP_HOME", &config.home)
            .env("METALSHARP_PORT", config.port.to_string())
            .env("METALSHARP_BUNDLE_DIR", config.resources.join("bundles"))
            .env("METALSHARP_HOMEBREW_INSTALLER", installer)
            .env(
                "PATH",
                std::env::join_paths([
                    config.resources.join("tools"),
                    PathBuf::from("/opt/homebrew/bin"),
                    PathBuf::from("/usr/local/bin"),
                    PathBuf::from("/usr/local/sbin"),
                    PathBuf::from("/usr/bin"),
                    PathBuf::from("/bin"),
                    PathBuf::from("/usr/sbin"),
                    PathBuf::from("/sbin"),
                ])?,
            )
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        // Prevent ambient Electron/dev configuration leaking into the candidate.
        command.env_remove("METALSHARP_DEV");
        for (name, file) in [
            ("ZSTD", "zstd"),
            ("UNZSTD", "unzstd"),
            ("WRESTOOL", "wrestool"),
            ("ICOTOOL", "icotool"),
            ("UNAR", "unar"),
            ("LSAR", "lsar"),
        ] {
            let key = format!("METALSHARP_{name}_PATH");
            let path = config.resources.join("tools").join(file);
            command.env_remove(&key);
            if path.is_file() {
                command.env(&key, path);
            }
        }
        drop(reservation);
        let child = command
            .spawn()
            .context("Could not spawn packaged C backend")?;
        let client = match token {
            Some(token) => BackendClient::for_port(config.port)?.with_client_token(token)?,
            None => BackendClient::for_port(config.port)?,
        };
        // Construct RAII guard immediately, so ALL startup failures reap the child.
        let binary_name = config
            .binary
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        let mut host = Self {
            child,
            client,
            binary_name,
        };
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            if host.child.try_wait()?.is_some() {
                bail!("C backend exited during startup");
            }
            let reply = host
                .client
                .request("GET", "/status", None, Duration::from_millis(500));
            if let Ok(reply) = reply {
                // Port reservation has a TOCTOU window; a healthy stranger must not
                // satisfy readiness. Verify child pid and exact data-home identity.
                if reply.get("ok").and_then(serde_json::Value::as_bool) == Some(true)
                    && reply.get("pid").and_then(serde_json::Value::as_u64)
                        == Some(host.child.id() as u64)
                    && reply
                        .get("metalsharp_home")
                        .and_then(serde_json::Value::as_str)
                        .is_some_and(|p| same_home(Path::new(p), &config.home))
                {
                    return Ok(host);
                }
            }
            if Instant::now() >= deadline {
                bail!("Owned backend did not become ready within 15 seconds");
            }
            thread::sleep(Duration::from_millis(100));
        }
    }
    pub(crate) fn pid(&self) -> u32 {
        self.child.id()
    }
    pub fn client(&self) -> BackendClient {
        self.client.clone()
    }
    pub fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }
    pub fn stop(&mut self) {
        match self.child.try_wait() {
            Ok(None) => {}
            // If another runtime has reaped the child, ownership is uncertain;
            // never signal a PID that could now belong to a different process.
            Ok(Some(_)) | Err(_) => return,
        }
        // SIGTERM gives the C backend its existing graceful-shutdown path.
        #[cfg(unix)]
        unsafe {
            libc::kill(self.child.id() as libc::pid_t, libc::SIGTERM);
        }
        // Electron killProcess(): up to 3 s for graceful shutdown, then SIGKILL.
        let deadline = Instant::now() + Duration::from_secs(3);
        while Instant::now() < deadline {
            match self.child.try_wait() {
                Ok(None) => {}
                Ok(Some(_)) | Err(_) => return,
            }
            thread::sleep(Duration::from_millis(25));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
impl BackendHost {
    /// App-quit path: SIGTERM now and hand the 3 s SIGKILL fallback to a
    /// detached watchdog, so quitting never blocks on a backend that is busy
    /// finishing a long request (its server handles one request at a time).
    pub fn terminate_detached(mut self) {
        if matches!(self.child.try_wait(), Ok(None)) {
            let pid = self.child.id();
            #[cfg(unix)]
            unsafe {
                libc::kill(pid as libc::pid_t, libc::SIGTERM);
            }
            // Only SIGKILL if the pid still runs our backend binary.
            let name = self.binary_name.replace('\'', "");
            let script = format!(
                "i=0; while [ $i -lt 30 ]; do kill -0 {pid} 2>/dev/null || exit 0; sleep 0.1; i=$((i+1)); done; \
                 case \"$(ps -p {pid} -o comm= 2>/dev/null)\" in *'{name}') kill -9 {pid} ;; esac"
            );
            let mut command = Command::new("/bin/sh");
            command
                .arg("-c")
                .arg(script)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null());
            #[cfg(unix)]
            unsafe {
                use std::os::unix::process::CommandExt;
                command.pre_exec(|| {
                    libc::setsid();
                    Ok(())
                });
            }
            let _ = command.spawn();
        }
        // The watchdog owns the fallback; skip Drop's blocking stop().
        std::mem::forget(self);
    }
}
impl Drop for BackendHost {
    fn drop(&mut self) {
        self.stop();
    }
}
fn same_home(a: &Path, b: &Path) -> bool {
    matches!((a.canonicalize(),b.canonicalize()),(Ok(a),Ok(b)) if a == b)
}
fn validate_home(home: &Path, user_home: &Path) -> Result<()> {
    if !home.is_absolute()
        || home
            .components()
            .any(|c| matches!(c, std::path::Component::ParentDir))
    {
        bail!("Use an absolute dedicated validation home without parent traversal");
    }
    if home == Path::new("/")
        || home == user_home
        || home.starts_with(user_home.join(".metalsharp"))
    {
        bail!("Refusing production data as validation home");
    }
    // Reject symlink ancestors, including nonexistent descendants of a symlink.
    for ancestor in home.ancestors() {
        if let Ok(meta) = std::fs::symlink_metadata(ancestor) {
            // /tmp and /var are macOS system aliases; resolve them rather than
            // forbidding all temporary directories.
            if meta.file_type().is_symlink()
                && ![Path::new("/tmp"), Path::new("/var")].contains(&ancestor)
            {
                bail!("Validation home must not traverse user-created symlinks");
            }
        }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validation_rejects_production_paths_and_traversal() {
        let root = Path::new("/Users/fixture");
        for p in [
            "/",
            "/Users/fixture",
            "/Users/fixture/.metalsharp",
            "/Users/fixture/.metalsharp/runtime",
            "/tmp/../Users/fixture/.metalsharp",
            "relative",
        ] {
            assert!(validate_home(Path::new(p), root).is_err(), "{p}");
        }
        assert!(validate_home(Path::new("/tmp/metalsharp-gpui-safe-fixture"), root).is_ok());
    }
    #[test]
    #[ignore = "requires freshly built app/src-c/build/metalsharp-backend; isolated-home only"]
    fn isolated_c_backend_smoke() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let binary = repo.join("app/src-c/build/metalsharp-backend");
        assert!(binary.is_file(), "build the C backend first");
        let reservation = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = reservation.local_addr().unwrap().port();
        drop(reservation);
        let home =
            std::env::temp_dir().join(format!("metalsharp-gpui-c-smoke-{}", std::process::id()));
        assert!(!home.exists(), "fixture directory must be fresh");
        let config = HostConfig {
            port,
            home: home.clone(),
            binary,
            resources: repo.join("app"),
            validation: true,
        };
        let mut host = BackendHost::start(&config).unwrap();
        let client = host.client();
        assert_eq!(client.status().unwrap().pid, Some(host.child.id()));
        let setup = client.setup_state().unwrap();
        assert!(!setup.completed);
        let unauthorized = BackendClient::for_port(port).unwrap();
        assert!(matches!(
            unauthorized.status(),
            Err(crate::backend::BackendError::Http(401))
        ));
        assert!(matches!(
            unauthorized.save_preference(crate::configuration::PreferenceChange::Msync(false)),
            Err(crate::backend::BackendError::Http(401))
        ));
        assert!(client.configuration().unwrap().msync);
        assert!(
            !client
                .launcher_status(crate::backend::Launcher::Steam)
                .unwrap()
                .installed
        );
        assert!(
            !client
                .launcher_status(crate::backend::Launcher::Ubisoft)
                .unwrap()
                .installed
        );
        assert_eq!(client.install_progress().unwrap().status, "idle");
        // External /Volumes Steam libraries are discovered regardless of home,
        // so only require that the isolated library request succeeds.
        assert!(client.library(crate::backend::Launcher::Steam).is_ok());
        assert!(
            client
                .library(crate::backend::Launcher::Ubisoft)
                .unwrap()
                .games
                .is_empty()
        );
        assert!(!client.setup_dependencies().unwrap().all_installed);
        // Read-only Sunshine contract check; never installs, starts, stops or pairs.
        // Its app path is global, so do not assume this machine lacks Sunshine.
        let streaming = client.streaming_status().unwrap();
        assert!(!streaming.creds_configured);
        assert!(!streaming.can_pair());
        assert!(streaming.creds_username.is_empty());
        assert!(matches!(
            unauthorized.streaming_status(),
            Err(crate::backend::BackendError::Http(401))
        ));
        std::fs::write(
            home.join("logs/gpui-fixture.log"),
            "runtime fixture ready\nAuthorization: Bearer fixture-private\n",
        )
        .unwrap();
        let logs = client.diagnostic_logs().unwrap();
        let fixture = logs
            .logs
            .iter()
            .find(|file| file.name == "gpui-fixture.log")
            .unwrap();
        assert_eq!(fixture.lines[0], "runtime fixture ready");
        assert!(!fixture.lines[1].contains("fixture-private"));
        // Persist only a fixture preference and retain unknown future config keys.
        let config_path = home.join("configs/config.json");
        std::fs::create_dir_all(config_path.parent().unwrap()).unwrap();
        std::fs::write(
            &config_path,
            r#"{"msync":true,"futureFixture":{"keep":123}}"#,
        )
        .unwrap();
        assert!(client.configuration().unwrap().msync);
        client
            .save_preference(crate::configuration::PreferenceChange::Msync(false))
            .unwrap();
        client
            .save_preference(crate::configuration::PreferenceChange::ControllerInput(
                crate::configuration::ControllerInput::XInput,
            ))
            .unwrap();
        assert!(!client.configuration().unwrap().msync);
        let saved: serde_json::Value =
            serde_json::from_slice(&std::fs::read(&config_path).unwrap()).unwrap();
        assert_eq!(saved["futureFixture"]["keep"], 123);
        let first_pid = host.child.id();
        assert!(host.is_running());
        host.stop();
        assert!(!host.is_running());
        drop(host);
        let mut restarted = BackendHost::start(&config).unwrap();
        assert_ne!(first_pid, restarted.child.id());
        assert_eq!(
            restarted.client().status().unwrap().pid,
            Some(restarted.child.id())
        );
        assert!(!restarted.client().setup_state().unwrap().completed);
        assert!(matches!(
            client.status(),
            Err(crate::backend::BackendError::Http(401))
        ));
        let persisted = restarted.client().configuration().unwrap();
        assert!(!persisted.msync);
        assert_eq!(
            persisted.controller_input,
            crate::configuration::ControllerInput::XInput
        );
        // Simulate a crash, then prove RAII cleanup cannot kill/adopt a replacement
        // listener that happens to occupy the old port.
        restarted.child.kill().unwrap();
        restarted.child.wait().unwrap();
        assert!(!restarted.is_running());
        let foreign = TcpListener::bind(("127.0.0.1", port)).unwrap();
        drop(restarted);
        assert!(foreign.local_addr().is_ok());
        std::fs::remove_dir_all(home).unwrap();
    }
    #[test]
    #[ignore = "requires freshly built app/src-c/build/metalsharp-backend; temp home only"]
    fn production_spawn_is_unauthenticated_and_replaces_stale_backend() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let binary = repo.join("app/src-c/build/metalsharp-backend");
        assert!(binary.is_file(), "build the C backend first");
        let reservation = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = reservation.local_addr().unwrap().port();
        drop(reservation);
        let home =
            std::env::temp_dir().join(format!("metalsharp-gpui-prod-smoke-{}", std::process::id()));
        let config = HostConfig {
            port,
            home: home.clone(),
            binary,
            resources: repo.join("app"),
            // Production behaviour (Electron parity) without touching ~/.metalsharp.
            validation: false,
        };
        let first = BackendHost::start(&config).unwrap();
        let first_pid = first.child.id();
        // Electron-style: updater scripts can read /status without a token.
        let anonymous = BackendClient::for_port(port).unwrap();
        assert_eq!(anonymous.status().unwrap().pid, Some(first_pid));
        // Leak the first host (simulating an orphan); a new launch must replace it.
        std::mem::forget(first);
        let mut second = BackendHost::start(&config).unwrap();
        assert_ne!(second.child.id(), first_pid);
        assert_eq!(anonymous.status().unwrap().pid, Some(second.child.id()));
        // The leaked child was terminated by the second launch; reap it.
        let mut wait_status = 0;
        let reaped =
            unsafe { libc::waitpid(first_pid as libc::pid_t, &mut wait_status, libc::WNOHANG) };
        assert_eq!(reaped, first_pid as libc::pid_t, "stale backend survived");
        second.stop();
        let _ = std::fs::remove_dir_all(home);
    }
    #[test]
    fn occupied_port_is_never_adopted() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let config = HostConfig {
            port: listener.local_addr().unwrap().port(),
            home: PathBuf::from("/should-not-create"),
            binary: PathBuf::from("/missing"),
            resources: PathBuf::from("/missing"),
            validation: true,
        };
        assert!(BackendHost::start(&config).is_err());
        assert!(listener.local_addr().is_ok());
    }
}
