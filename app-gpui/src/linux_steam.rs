//! Linux Steam (the FEX build's experimental Steam client): Valve's Linux client in a small Linux virtual machine,
//! which MetalSharp-Wine.app carries as `share/steamvm/metalsharp-steam`. The backend drives it; the app only stops it
//! when MetalSharp quits (or its data folder moves), before the Wine processes are killed: the virtual machine
//! (krunkit, gvproxy, metalsharp-vmd and the LinuxSteam window app) runs from MetalSharp-Wine.app and is stopped with
//! its own command, never killed (wine_shutdown leaves it alone).
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

/// How long a stop may take before MetalSharp quits anyway.
pub const STOP_TIMEOUT: Duration = Duration::from_secs(60);
/// `status --json` answers at once; this bounds a wedged one.
const STATUS_TIMEOUT: Duration = Duration::from_secs(3);

/// The toast for a stage of Linux Steam's one-time setup (the setup's last "==> " line).
pub fn setup_toast(stage: &str) -> String {
    format!("Setting up Linux Steam: {stage}")
}

/// The installed MetalSharp Wine's metalsharp-steam.
pub fn command(home: &Path) -> PathBuf {
    home.join("runtime/MetalSharp-Wine.app/Contents/share/steamvm/metalsharp-steam")
}

/// The virtual machine's run folder (pid files, sockets): it exists once Linux Steam was started.
fn run_dir(home: &Path) -> PathBuf {
    home.join("run/linux-steam")
}

/// The command with MetalSharp's folders, as the backend runs it.
fn prepared(home: &Path, args: &[&str]) -> Command {
    let mut command = Command::new(command(home));
    command
        .args(args)
        .env("METALSHARP_VM_DIR", home.join("linux-steam"))
        .env("METALSHARP_VM_RUN", run_dir(home))
        .env("METALSHARP_STEAM_PREFIX", home.join("prefix-steam"))
        .env("METALSHARP_GAME_PREFIX", home.join("prefix-steam"))
        .env(
            "METALSHARP_WINE",
            home.join("runtime/MetalSharp-Wine.app/Contents/MacOS/wine"),
        )
        .env("METALSHARP_HOME", home)
        .stdin(Stdio::null());
    command
}

/// Runs it, waiting up to `timeout`: its exit, or None when it could not run or was stopped at the limit.
fn run(mut command: Command, timeout: Duration, capture: bool) -> Option<(bool, String)> {
    command.stdout(if capture {
        Stdio::piped()
    } else {
        Stdio::null()
    });
    command.stderr(Stdio::null());
    let mut child = command.spawn().ok()?;
    let started = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                let mut output = String::new();
                if let Some(mut out) = child.stdout.take() {
                    let _ = std::io::Read::read_to_string(&mut out, &mut output);
                }
                return Some((status.success(), output));
            }
            Ok(None) if started.elapsed() < timeout => {
                std::thread::sleep(Duration::from_millis(100))
            }
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    }
}

/// Whether the virtual machine or Steam runs, by `status --json`; None when the command did not answer.
fn running(home: &Path) -> Option<bool> {
    let (_, output) = run(prepared(home, &["status", "--json"]), STATUS_TIMEOUT, true)?;
    let start = output.find('{')?;
    let end = output.rfind('}')?;
    let state: serde_json::Value = serde_json::from_str(output.get(start..=end)?).ok()?;
    let flag = |key: &str| state.get(key).and_then(serde_json::Value::as_bool) == Some(true);
    Some(flag("vm_running") || flag("steam_running"))
}

/// Stop Linux Steam's virtual machine when it runs, within `timeout`. Nothing when this MetalSharp Wine has no Linux
/// Steam or it was never started. Blocking.
pub fn stop_if_running(home: &Path, timeout: Duration) {
    if !command(home).is_file() || !run_dir(home).is_dir() {
        return;
    }
    if running(home) == Some(false) {
        return;
    }
    let _ = run(prepared(home, &["stop"]), timeout, false);
}

/// The virtual machine's own programs, which run from MetalSharp-Wine.app: stopped with metalsharp-steam, never killed
/// as Wine (a SIGKILLed krunkit can leave the virtual machine's disk inconsistent).
pub fn is_vm_executable(executable: &Path) -> bool {
    let text = executable.to_string_lossy();
    if text.contains("/LinuxSteam.app/") {
        return true;
    }
    let name = executable
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    matches!(
        name.as_str(),
        "krunkit" | "gvproxy" | "metalsharp-vmd" | "metalsharp-steamview" | "LinuxSteam"
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vm_programs_are_told_apart() {
        for path in [
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/MacOS/krunkit",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/bin/krunkit",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/bin/gvproxy",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/bin/metalsharp-vmd",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/share/steamvm/LinuxSteam.app/Contents/MacOS/LinuxSteam",
        ] {
            assert!(is_vm_executable(Path::new(path)), "{path}");
        }
        for path in [
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/MacOS/wine",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver",
            "/Applications/Steam.app/Contents/MacOS/steam_osx",
        ] {
            assert!(!is_vm_executable(Path::new(path)), "{path}");
        }
    }

    // The command is run with MetalSharp's folders, and only when it runs something: a stand-in records its calls.
    #[test]
    fn stops_only_a_running_vm() {
        let base =
            std::env::temp_dir().join(format!("metalsharp-linux-steam-{}", std::process::id()));
        let home = base.join("home");
        let script = command(&home);
        std::fs::create_dir_all(script.parent().unwrap()).unwrap();
        let calls = base.join("calls");
        let state = base.join("state.json");
        std::fs::write(
            &script,
            format!(
                "#!/bin/bash\nprintf '%s|%s|%s\\n' \"$*\" \"$METALSHARP_VM_RUN\" \"$METALSHARP_GAME_PREFIX\" >> '{}'\n\
                 [ \"$1\" = status ] && cat '{}'\nexit 0\n",
                calls.display(),
                state.display()
            ),
        )
        .unwrap();
        std::fs::set_permissions(&script, std::os::unix::fs::PermissionsExt::from_mode(0o755))
            .unwrap();
        // never started: not even a status
        stop_if_running(&home, Duration::from_secs(5));
        assert!(!calls.exists());
        std::fs::create_dir_all(run_dir(&home)).unwrap();
        std::fs::write(
            &state,
            r#"{"installed":true,"vm_running":false,"steam_running":false}"#,
        )
        .unwrap();
        stop_if_running(&home, Duration::from_secs(5));
        let text = std::fs::read_to_string(&calls).unwrap();
        assert!(text.starts_with("status --json|"), "{text}");
        assert!(!text.contains("stop|"), "{text}");
        std::fs::write(
            &state,
            r#"{"installed":true,"vm_running":true,"steam_running":true}"#,
        )
        .unwrap();
        stop_if_running(&home, Duration::from_secs(5));
        let text = std::fs::read_to_string(&calls).unwrap();
        let expected = format!(
            "stop|{}|{}",
            run_dir(&home).display(),
            home.join("prefix-steam").display()
        );
        assert!(text.lines().any(|line| line == expected), "{text}");
        let _ = std::fs::remove_dir_all(&base);
    }
}
