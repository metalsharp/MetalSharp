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
}
impl BackendHost {
    pub fn start(config: &HostConfig) -> Result<Self> {
        if config.port == 0 {
            bail!("Invalid backend port");
        }
        let reservation = TcpListener::bind(("127.0.0.1", config.port)).context(
            "Backend port is already occupied; no foreign process will be adopted or killed",
        )?;
        std::fs::create_dir_all(&config.home)?;
        let mut command = Command::new(&config.binary);
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
        let client = BackendClient::for_port(config.port)?;
        // Construct RAII guard immediately, so ALL startup failures reap the child.
        let mut host = Self { child, client };
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
    pub fn client(&self) -> BackendClient {
        self.client.clone()
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
        let deadline = Instant::now() + Duration::from_secs(2);
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
        assert!(
            client
                .library(crate::backend::Launcher::Steam)
                .unwrap()
                .games
                .is_empty()
        );
        assert!(
            client
                .library(crate::backend::Launcher::Ubisoft)
                .unwrap()
                .games
                .is_empty()
        );
        host.stop();
        assert!(host.child.try_wait().unwrap().is_some());
        assert!(TcpListener::bind(("127.0.0.1", port)).is_ok());
        std::fs::remove_dir_all(home).unwrap();
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
