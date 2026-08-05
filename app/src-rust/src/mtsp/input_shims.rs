//! Controller input shim deployment for the sidebar [x]/[d] selector.
//!
//! The selector persists a `controllerInput` value in config.json
//! (`"off" | "x" | "d"`, default `"off"`). At launch time MetalSharp
//! deploys the corresponding PE shims — Wine's builtin XInput DLLs
//! (`xinput1_1.dll` … `xinput1_4.dll`, `xinput9_1_0.dll`) or DInput DLLs
//! (`dinput.dll`, `dinput8.dll`) — into the game directory and the Steam
//! prefix. Switching between x/d (or back to off) removes the previously
//! deployed set first, restoring any user file that was overwritten.
//!
//! Sources come from the Wine runtime's PE DLL directory
//! (`runtime/wine/lib/wine/x86_64-windows/`), which ships both sets as
//! Wine builtins. Missing sources are skipped with a warning — shims are
//! additive and must never block a launch.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};

pub const XINPUT_DLLS: &[&str] =
    &["xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll"];

pub const DINPUT_DLLS: &[&str] = &["dinput.dll", "dinput8.dll"];

const GAME_MANIFEST_NAME: &str = "input-shims.json";
const PREFIX_MARKER_NAME: &str = ".metalsharp-input-shims.json";
const ORIGINALS_DIR_NAME: &str = "input-shim-originals";

/// The active controller input mode. Mirrors `launch::controller_input_mode`
/// so callers can resolve the mode without importing launch.
pub fn current_mode() -> String {
    crate::launch::controller_input_mode()
}

pub fn xinput_dlls() -> &'static [&'static str] {
    XINPUT_DLLS
}

pub fn dinput_dlls() -> &'static [&'static str] {
    DINPUT_DLLS
}

/// DLLs belonging to the given mode (empty for off).
pub fn dlls_for_mode(mode: &str) -> &'static [&'static str] {
    match mode {
        "x" => XINPUT_DLLS,
        "d" => DINPUT_DLLS,
        _ => &[],
    }
}

/// Source directories for the PE shims, per target architecture.
///
/// Preferred source is the committed `lib/metalsharp/<arch>-windows/` set
/// (bundled into `runtime/wine/lib/metalsharp/<arch>-windows/` by
/// create-split-bundles.py, like metalsharp_ntdll_hook.dll), falling back to
/// the Wine runtime's own PE DLL directory
/// (`runtime/wine/lib/wine/<arch>-windows/`) for older runtime bundles.
pub fn shim_source_dirs(ms_root: &Path, arch: &str) -> Vec<PathBuf> {
    let bundled = ms_root.join("lib").join("metalsharp").join(arch);
    let runtime = ms_root.join("lib").join("wine").join(arch);
    vec![bundled, runtime]
}

fn shim_source_path(ms_root: &Path, arch: &str, dll: &str) -> Option<PathBuf> {
    shim_source_dirs(ms_root, arch).into_iter().map(|dir| dir.join(dll)).find(|p| p.is_file())
}

fn shim_marker(target_dir: &Path) -> PathBuf {
    target_dir.join(".metalsharp").join(GAME_MANIFEST_NAME)
}

fn originals_dir_for(target_dir: &Path) -> PathBuf {
    target_dir.join(".metalsharp").join(ORIGINALS_DIR_NAME)
}

/// DLLs currently deployed by MetalSharp into a game dir (from the manifest).
fn deployed_dlls_from_manifest(game_dir: &Path) -> Vec<String> {
    std::fs::read_to_string(shim_marker(game_dir))
        .ok()
        .and_then(|contents| serde_json::from_str::<Value>(&contents).ok())
        .and_then(|v| v.get("dlls").cloned())
        .and_then(|dlls| serde_json::from_value::<Vec<String>>(dlls).ok())
        .unwrap_or_default()
}

fn write_game_manifest(game_dir: &Path, mode: &str, deployed: &[String]) {
    let marker = shim_marker(game_dir);
    if let Some(parent) = marker.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let manifest = json!({
        "mode": mode,
        "dlls": deployed,
        "updated_at_unix": std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs(),
    });
    let _ = std::fs::write(marker, serde_json::to_string_pretty(&manifest).unwrap_or_default());
}

/// Remove shims from a game dir: restore originals we overwrote, delete the
/// rest, then drop the manifest. No-op when nothing is deployed.
fn remove_from_game_inner(game_dir: &Path) {
    let originals_dir = originals_dir_for(game_dir);
    for dll in deployed_dlls_from_manifest(game_dir) {
        let dest = game_dir.join(&dll);
        let original = originals_dir.join(&dll);
        if original.is_file() {
            let _ = std::fs::copy(&original, &dest);
        } else {
            let _ = std::fs::remove_file(&dest);
        }
    }
    let _ = std::fs::remove_dir_all(&originals_dir);
    let _ = std::fs::remove_file(shim_marker(game_dir));
}

/// Deploy the active shim set into a game directory, removing any previously
/// deployed set first. `mode` is one of `"off" | "x" | "d"`.
pub fn deploy_for_game(game_dir: &Path, mode: &str) -> Result<(), String> {
    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");
    deploy_for_game_with_root(game_dir, &ms_root, mode)
}

/// Core deployment with an explicit runtime root (testable without touching
/// the process-global METALSHARP_HOME).
pub fn deploy_for_game_with_root(game_dir: &Path, ms_root: &Path, mode: &str) -> Result<(), String> {
    if !game_dir.is_dir() {
        return Ok(());
    }

    remove_from_game_inner(game_dir);

    if mode == "off" {
        return Ok(());
    }

    let originals_dir = originals_dir_for(game_dir);
    let mut deployed = Vec::new();
    for dll in dlls_for_mode(mode) {
        let Some(source) = shim_source_path(ms_root, "x86_64-windows", dll) else {
            eprintln!("input-shims: source missing for {dll} — skipping");
            continue;
        };
        let dest = game_dir.join(dll);
        // Preserve a user file we're about to overwrite (only once).
        if dest.is_file() {
            let original = originals_dir.join(dll);
            if !original.exists() {
                let _ = std::fs::create_dir_all(&originals_dir);
                let _ = std::fs::copy(&dest, &original);
            }
        }
        if std::fs::copy(&source, &dest).is_ok() {
            deployed.push(dll.to_string());
        }
    }

    write_game_manifest(game_dir, mode, &deployed);
    Ok(())
}

fn prefix_marker(prefix: &Path) -> PathBuf {
    prefix.join(PREFIX_MARKER_NAME)
}

fn prefix_deployed_dlls(prefix: &Path) -> Vec<String> {
    std::fs::read_to_string(prefix_marker(prefix))
        .ok()
        .and_then(|contents| serde_json::from_str::<Value>(&contents).ok())
        .and_then(|v| v.get("dlls").cloned())
        .and_then(|dlls| serde_json::from_value::<Vec<String>>(dlls).ok())
        .unwrap_or_default()
}

fn write_prefix_marker(prefix: &Path, mode: &str, deployed: &[String]) {
    let marker = prefix_marker(prefix);
    let manifest = json!({
        "mode": mode,
        "dlls": deployed,
        "updated_at_unix": std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs(),
    });
    let _ = std::fs::write(marker, serde_json::to_string_pretty(&manifest).unwrap_or_default());
}

fn prefix_target_dirs(prefix: &Path) -> Vec<PathBuf> {
    let windows = prefix.join("drive_c").join("windows");
    vec![windows.join("system32"), windows.join("syswow64")]
}

fn prefix_originals_dir(prefix: &Path) -> PathBuf {
    prefix.join(".metalsharp-input-originals")
}

/// Remove shims from the Steam prefix (both system32 and syswow64),
/// restoring originals. No-op when nothing is deployed.
fn remove_from_prefix_inner(prefix: &Path) {
    let originals_dir = prefix_originals_dir(prefix);
    let targets = prefix_target_dirs(prefix);
    for dll in prefix_deployed_dlls(prefix) {
        for (target, arch) in [(&targets[0], "x86_64-windows"), (&targets[1], "i386-windows")] {
            let dest = target.join(&dll);
            let original = originals_dir.join(arch).join(&dll);
            if original.is_file() {
                let _ = std::fs::copy(&original, &dest);
            } else {
                let _ = std::fs::remove_file(&dest);
            }
        }
    }
    let _ = std::fs::remove_dir_all(&originals_dir);
    let _ = std::fs::remove_file(prefix_marker(prefix));
}

/// Deploy the active shim set into the Steam prefix (`system32` for 64-bit
/// processes, `syswow64` for 32-bit), removing the previous set first.
pub fn deploy_for_prefix(prefix: &Path, mode: &str) -> Result<(), String> {
    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");
    deploy_for_prefix_with_root(prefix, &ms_root, mode)
}

/// Core prefix deployment with an explicit runtime root (testable without
/// touching the process-global METALSHARP_HOME).
pub fn deploy_for_prefix_with_root(prefix: &Path, ms_root: &Path, mode: &str) -> Result<(), String> {
    if !prefix.is_dir() {
        return Ok(());
    }

    remove_from_prefix_inner(prefix);

    if mode == "off" {
        return Ok(());
    }

    let originals_dir = prefix_originals_dir(prefix);
    let targets = prefix_target_dirs(prefix);
    let mut deployed = Vec::new();
    for dll in dlls_for_mode(mode) {
        // system32 serves 64-bit processes; syswow64 serves 32-bit ones, so
        // each target gets the matching architecture's DLL.
        for (target, arch) in [(&targets[0], "x86_64-windows"), (&targets[1], "i386-windows")] {
            let Some(source) = shim_source_path(ms_root, arch, dll) else {
                eprintln!("input-shims: source missing for {dll} ({arch}) — skipping");
                continue;
            };
            let _ = std::fs::create_dir_all(target);
            let dest = target.join(dll);
            if dest.is_file() {
                let original = originals_dir.join(arch).join(dll);
                if !original.exists() {
                    let _ = std::fs::create_dir_all(original.parent().unwrap_or(&originals_dir));
                    let _ = std::fs::copy(&dest, &original);
                }
            }
            if std::fs::copy(&source, &dest).is_ok() {
                deployed.push(dll.to_string());
            }
        }
    }

    write_prefix_marker(prefix, mode, &deployed);
    Ok(())
}

/// Deploy the current configured mode into a game dir (called at launch).
pub fn deploy_current_for_game(game_dir: &Path) -> Result<(), String> {
    deploy_for_game(game_dir, &current_mode())
}

/// Deploy the current configured mode into the Steam prefix (called at
/// Steam launch).
pub fn deploy_current_for_prefix(prefix: &Path) -> Result<(), String> {
    deploy_for_prefix(prefix, &current_mode())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn fake_runtime(temp: &Path) -> PathBuf {
        // Returns the wine root (…/runtime/wine); shims live under
        // lib/metalsharp/<arch>-windows (bundled) and
        // lib/wine/<arch>-windows (runtime fallback) inside it.
        let root = temp.join("runtime").join("wine");
        for arch in ["x86_64-windows", "i386-windows"] {
            let bundled = root.join("lib").join("metalsharp").join(arch);
            let fallback = root.join("lib").join("wine").join(arch);
            for dir in [&bundled, &fallback] {
                fs::create_dir_all(dir).unwrap();
                for dll in XINPUT_DLLS.iter().chain(DINPUT_DLLS.iter()) {
                    fs::write(dir.join(dll), format!("fake-{dll}-{arch}")).unwrap();
                }
            }
        }
        root
    }

    #[test]
    fn deploy_x_then_switch_d_removes_x_and_deploys_d() {
        let temp = std::env::temp_dir().join(format!("ms-input-switch-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);

        let game = temp.join("game");
        fs::create_dir_all(&game).unwrap();

        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        for dll in XINPUT_DLLS {
            assert!(game.join(dll).exists(), "{dll} should exist after x");
        }
        for dll in DINPUT_DLLS {
            assert!(!game.join(dll).exists(), "{dll} must not exist in x mode");
        }

        deploy_for_game_with_root(&game, &ms_root, "d").unwrap();
        for dll in XINPUT_DLLS {
            assert!(!game.join(dll).exists(), "{dll} must be removed when switching to d");
        }
        for dll in DINPUT_DLLS {
            assert!(game.join(dll).exists(), "{dll} should exist after d");
        }

        // Switch back: d removed, x restored.
        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        for dll in XINPUT_DLLS {
            assert!(game.join(dll).exists(), "{dll} should exist after switching back to x");
        }
        for dll in DINPUT_DLLS {
            assert!(!game.join(dll).exists(), "{dll} must be removed when switching back to x");
        }

        let _ = fs::remove_dir_all(&temp);
    }

    #[test]
    fn off_removes_everything_and_restores_originals() {
        let temp = std::env::temp_dir().join(format!("ms-input-off-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);

        let game = temp.join("game");
        fs::create_dir_all(&game).unwrap();
        // Pre-existing user DLL that MetalSharp would overwrite.
        fs::write(game.join("xinput1_3.dll"), "user-original").unwrap();

        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        assert_eq!(fs::read_to_string(game.join("xinput1_3.dll")).unwrap(), "fake-xinput1_3.dll-x86_64-windows");

        deploy_for_game_with_root(&game, &ms_root, "off").unwrap();
        // Every dll we deployed is removed...
        for dll in XINPUT_DLLS.iter().chain(DINPUT_DLLS.iter()) {
            if dll == &"xinput1_3.dll" {
                // ...except the user's original, which is restored.
                assert_eq!(fs::read_to_string(game.join(dll)).unwrap(), "user-original");
            } else {
                assert!(!game.join(dll).exists(), "{dll} must be removed in off mode");
            }
        }

        let _ = fs::remove_dir_all(&temp);
    }

    #[test]
    fn missing_sources_are_skipped_without_failing() {
        let temp = std::env::temp_dir().join(format!("ms-input-missing-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);
        // Remove one bundled xinput dll to simulate an incomplete bundle;
        // the runtime fallback still has it.
        fs::remove_file(ms_root.join("lib").join("metalsharp").join("x86_64-windows").join("xinput1_4.dll")).unwrap();

        let game = temp.join("game");
        fs::create_dir_all(&game).unwrap();
        // Should not error; the remaining dlls deploy.
        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        assert!(game.join("xinput1_3.dll").exists());
        assert!(game.join("xinput1_4.dll").exists()); // served by the runtime fallback

        let _ = fs::remove_dir_all(&temp);
    }

    #[test]
    fn prefix_deploy_targets_system32_and_syswow64_and_switches_cleanly() {
        let temp = std::env::temp_dir().join(format!("ms-input-prefix-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);

        let prefix = temp.join("prefix-steam");
        let system32 = prefix.join("drive_c").join("windows").join("system32");
        let syswow64 = prefix.join("drive_c").join("windows").join("syswow64");
        fs::create_dir_all(&system32).unwrap();
        fs::create_dir_all(&syswow64).unwrap();

        deploy_for_prefix_with_root(&prefix, &ms_root, "x").unwrap();
        for target in [&system32, &syswow64] {
            for dll in XINPUT_DLLS {
                assert!(target.join(dll).exists(), "{dll} missing in {}", target.display());
            }
            for dll in DINPUT_DLLS {
                assert!(!target.join(dll).exists(), "{dll} must not exist in x mode");
            }
        }
        // Arch correctness: system32 (64-bit) gets the x64 build, syswow64
        // (32-bit) gets the i386 build.
        assert_eq!(fs::read_to_string(system32.join("xinput1_3.dll")).unwrap(), "fake-xinput1_3.dll-x86_64-windows");
        assert_eq!(fs::read_to_string(syswow64.join("xinput1_3.dll")).unwrap(), "fake-xinput1_3.dll-i386-windows");

        deploy_for_prefix_with_root(&prefix, &ms_root, "d").unwrap();
        for target in [&system32, &syswow64] {
            for dll in XINPUT_DLLS {
                assert!(!target.join(dll).exists(), "{dll} must be removed when switching to d");
            }
            for dll in DINPUT_DLLS {
                assert!(target.join(dll).exists(), "{dll} should exist after d");
            }
        }

        deploy_for_prefix_with_root(&prefix, &ms_root, "off").unwrap();
        for target in [&system32, &syswow64] {
            for dll in XINPUT_DLLS.iter().chain(DINPUT_DLLS.iter()) {
                assert!(!target.join(dll).exists(), "{dll} must be removed in off mode");
            }
        }

        let _ = fs::remove_dir_all(&temp);
    }

    #[test]
    fn deploy_is_idempotent_for_same_mode() {
        let temp = std::env::temp_dir().join(format!("ms-input-idempotent-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);

        let game = temp.join("game");
        fs::create_dir_all(&game).unwrap();
        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        deploy_for_game_with_root(&game, &ms_root, "x").unwrap();
        for dll in XINPUT_DLLS {
            assert!(game.join(dll).exists());
        }
        for dll in DINPUT_DLLS {
            assert!(!game.join(dll).exists());
        }

        let _ = fs::remove_dir_all(&temp);
    }

    #[test]
    fn missing_game_or_prefix_dir_is_a_noop() {
        let temp = std::env::temp_dir().join(format!("ms-input-noop-{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).unwrap();
        let ms_root = fake_runtime(&temp);

        assert!(deploy_for_game_with_root(&temp.join("does-not-exist"), &ms_root, "x").is_ok());
        assert!(deploy_for_prefix_with_root(&temp.join("no-prefix"), &ms_root, "x").is_ok());

        let _ = fs::remove_dir_all(&temp);
    }
}
