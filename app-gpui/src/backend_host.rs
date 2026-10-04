use anyhow::{Context, Result, bail};
use std::{
    net::TcpListener,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

const DEFAULT_VALIDATION_PORT: u16 = 9276;

pub struct BackendHost {
    child: Child,
    base_url: String,
}

impl BackendHost {
    pub fn start() -> Result<Self> {
        let port = std::env::var("METALSHARP_GPUI_PORT")
            .ok()
            .map(|value| value.parse::<u16>())
            .transpose()
            .context("METALSHARP_GPUI_PORT must be a valid TCP port")?
            .unwrap_or(DEFAULT_VALIDATION_PORT);
        if port == 0 {
            bail!("port zero is not valid for the GPUI validation backend");
        }

        // Never adopt or terminate a server we did not launch. Port 9276 is the
        // validation-only default; production remains on 9274.
        let listener = TcpListener::bind(("127.0.0.1", port)).with_context(|| {
            format!(
                "cannot reserve validation backend port {port}; stop the process using it or set METALSHARP_GPUI_PORT"
            )
        })?;
        drop(listener);

        let backend_path = find_backend()?;
        let home = validation_home()?;
        std::fs::create_dir_all(&home).with_context(|| {
            format!("cannot create isolated validation home {}", home.display())
        })?;

        let child = Command::new(&backend_path)
            .env("METALSHARP_HOME", &home)
            .env("METALSHARP_PORT", port.to_string())
            .stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            .spawn()
            .with_context(|| format!("cannot launch backend at {}", backend_path.display()))?;

        let base_url = format!("http://127.0.0.1:{port}");
        let deadline = Instant::now() + Duration::from_secs(15);
        let mut child = child;
        loop {
            if let Some(status) = child.try_wait().context("checking backend process")? {
                bail!("backend exited during startup with {status}");
            }
            if ureq::get(format!("{base_url}/status")).call().is_ok() {
                return Ok(Self { child, base_url });
            }
            if Instant::now() >= deadline {
                let _ = child.kill();
                let _ = child.wait();
                bail!("backend did not become ready at {base_url} within 15 seconds");
            }
            thread::sleep(Duration::from_millis(200));
        }
    }

    pub fn base_url(&self) -> String {
        self.base_url.clone()
    }
}

impl Drop for BackendHost {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn find_backend() -> Result<PathBuf> {
    if let Some(path) = std::env::var_os("METALSHARP_GPUI_BACKEND") {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
        bail!(
            "METALSHARP_GPUI_BACKEND does not name a file: {}",
            path.display()
        );
    }

    let executable = std::env::current_exe().context("locating the GPUI executable")?;
    let executable_dir = executable.parent().unwrap_or(Path::new("."));
    let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let candidates = [
        executable_dir.join("../Resources/runtime/metalsharp-backend"),
        repo_root.join("app/src-c/build/metalsharp-backend"),
        repo_root.join("app/src/main/metalsharp-backend"),
    ];
    candidates
        .iter()
        .find(|path| path.is_file())
        .cloned()
        .with_context(|| {
            format!(
                "MetalSharp backend not found; build it with `make -C app/src-c` or set METALSHARP_GPUI_BACKEND (checked {})",
                candidates_display(repo_root)
            )
        })
}

fn candidates_display(root: &Path) -> String {
    format!(
        "{}, {}",
        root.join("app/src-c/build/metalsharp-backend").display(),
        root.join("app/src/main/metalsharp-backend").display()
    )
}

fn validation_home() -> Result<PathBuf> {
    if let Some(home) = std::env::var_os("METALSHARP_GPUI_HOME") {
        let home = PathBuf::from(home);
        if home == Path::new("/")
            || std::env::var_os("HOME").map(PathBuf::from).as_deref() == Some(home.as_path())
            || std::env::var_os("HOME")
                .map(|root| PathBuf::from(root).join(".metalsharp"))
                .as_deref()
                == Some(home.as_path())
        {
            bail!(
                "refusing unsafe validation home {}; choose a dedicated test directory",
                home.display()
            );
        }
        return Ok(home);
    }

    Ok(std::env::temp_dir().join("metalsharp-gpui-preview"))
}
