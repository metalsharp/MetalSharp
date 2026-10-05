//! Port of the Electron main-process `UpdaterBridge` (`app/src/main/updater-bridge.ts`).
//! The backend downloads the DMG; this module resolves `update.sh`, spawns it
//! fully detached exactly as Electron did, and reads its JSON status file.
use crate::live::Live;
use serde_json::Value;
use std::path::{Path, PathBuf};

fn status_file(home: &Path) -> PathBuf {
    home.join("update_install_status.json")
}

fn bundled_tools_dir(resources: &Path) -> PathBuf {
    let packaged = resources.join("tools");
    if packaged.join("zstd").exists() {
        return packaged;
    }
    let dev = Path::new(env!("CARGO_MANIFEST_DIR")).join("../app/tools");
    if dev.join("zstd").exists() {
        dev
    } else {
        packaged
    }
}

fn tool_path(resources: &Path) -> String {
    let mut parts = vec![bundled_tools_dir(resources).to_string_lossy().into_owned()];
    parts.extend(
        [
            "/opt/homebrew/bin",
            "/usr/local/bin",
            "/usr/bin",
            "/bin",
            "/usr/sbin",
            "/sbin",
        ]
        .map(str::to_owned),
    );
    parts.join(":")
}

/// `UpdaterBridge.ensureReady`: locate (or extract) `update.sh`.
pub fn ensure_ready(live: &Live) -> Result<PathBuf, String> {
    let resources = live.resources();
    let dev_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../app");
    let candidates = [
        resources.join("scripts/tools/updater/update.sh"),
        resources.join("updater/update.sh"),
        resources.join("app.asar.unpacked/updater/update.sh"),
        dev_root.join("updater/update.sh"),
    ];
    for candidate in &candidates {
        if std::fs::File::open(candidate).is_ok() {
            return Ok(candidate.clone());
        }
    }
    if let Some(script) = extract_bundled_updater(live) {
        return Ok(script);
    }
    Err(format!(
        "Updater install script not found. Checked: {}",
        candidates
            .iter()
            .map(|c| c.to_string_lossy().into_owned())
            .collect::<Vec<_>>()
            .join(", ")
    ))
}

fn extract_bundled_updater(live: &Live) -> Option<PathBuf> {
    let resources = live.resources();
    let bundle = resources.join("bundles/metalsharp-scripts-tools.tar.zst");
    if !bundle.exists() {
        return None;
    }
    let root = live.home().join("cache/updater-tools");
    let script = root.join("scripts/tools/updater/update.sh");
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).ok()?;
    let status = std::process::Command::new("tar")
        .args(["--use-compress-program=unzstd", "-xf"])
        .arg(&bundle)
        .arg("-C")
        .arg(&root)
        .arg("scripts/tools/updater/update.sh")
        .env("PATH", tool_path(&resources))
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .ok();
    let usable = std::fs::metadata(&script)
        .map(|m| m.len() > 0)
        .unwrap_or(false);
    if (status.is_some_and(|s| s.success()) || script.exists()) && usable {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755));
        }
        return Some(script);
    }
    None
}

/// `UpdaterBridge.spawnInstallUpdater`.
pub fn spawn_install(
    live: &Live,
    script: &Path,
    dmg_path: &str,
    backend_pid: u32,
    target_version: &str,
    variant: &str,
) -> Result<(), String> {
    if variant != "regular" && variant != "fex" {
        return Err("Invalid update variant".into());
    }
    if !script.exists() {
        return Err("Updater not ready — update.sh missing".into());
    }
    if !Path::new(dmg_path).exists() {
        return Err(format!("DMG file not found: {dmg_path}"));
    }
    let home = live.home();
    let _ = std::fs::create_dir_all(&home);
    let mut command = std::process::Command::new("/bin/bash");
    crate::lifecycle::unmask_child_signals(&mut command);
    command.arg(script);
    if variant == "regular" {
        command.arg("--recover");
    } else {
        command
            .arg("--dmg")
            .arg(dmg_path)
            .arg("--backend-pid")
            .arg(backend_pid.to_string())
            .arg("--target-version")
            .arg(target_version)
            .arg("--status-file")
            .arg(status_file(&home))
            .arg("--metalsharp-home")
            .arg(&home)
            .arg("--app-pid")
            .arg(std::process::id().to_string());
    }
    command
        .env("METALSHARP_HOME", &home)
        .env("PATH", tool_path(&live.resources()))
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    // Detached (Electron `detached: true` → setsid) so the script survives our quit.
    #[cfg(unix)]
    unsafe {
        use std::os::unix::process::CommandExt;
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    command
        .spawn()
        .map(|_| ())
        .map_err(|error| error.to_string())
}

pub fn read_install_status(home: &Path) -> Option<Value> {
    let text = std::fs::read_to_string(status_file(home)).ok()?;
    serde_json::from_str(&text).ok()
}

pub fn clear_install_status(home: &Path) {
    let _ = std::fs::remove_file(status_file(home));
}
