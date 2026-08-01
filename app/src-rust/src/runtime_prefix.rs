use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const XTAJIT64_SHA256: &str = "7b9f55ceabe971ffa1f514570bb54ed7b5640959e4440e7f8a013e9af13ab7e6";
const XTAJIT_SHA256: &str = "7d2ac83d2c0935e04d033d609c42d8307294225dcb4cb16b88af849e95c694ab";

pub(crate) fn runtime_root() -> PathBuf {
    crate::platform::metalsharp_home_dir().join("runtime")
}

pub(crate) fn wine_root() -> PathBuf {
    runtime_root().join("wine")
}

pub(crate) fn apply_complete_runtime_env(command: &mut Command, prefix: &Path) {
    let root = runtime_root();
    command
        .env("WINEPREFIX", prefix)
        .env("WINEBUILDDIR", root.join("wine/build-ec"))
        .env("FEX_TSOENABLED", "0")
        .env("FEX_VECTORTSOENABLED", "0")
        .env("FEX_MEMCPYSETTSOENABLED", "0");
    crate::launch::apply_wine_runtime_preferences(command);
    crate::platform::set_runtime_library_env(command, &root.join("wine"));
}

fn copy_file_required(source: &Path, destination: &Path, label: &str) -> Result<(), String> {
    if !source.is_file() {
        return Err(format!("complete runtime is missing {label}: {}", source.display()));
    }
    if let Some(parent) = destination.parent() {
        std::fs::create_dir_all(parent).map_err(|error| format!("failed to create {}: {error}", parent.display()))?;
    }
    std::fs::copy(source, destination)
        .map_err(|error| format!("failed to stage {label} from {}: {error}", source.display()))?;
    Ok(())
}

fn stage_i386_builtins(build: &Path, prefix: &Path) -> Result<usize, String> {
    let dll_root = build.join("dlls");
    let syswow64 = prefix.join("drive_c/windows/syswow64");
    std::fs::create_dir_all(&syswow64).map_err(|error| format!("failed to create {}: {error}", syswow64.display()))?;
    let mut staged = 0usize;

    let components =
        std::fs::read_dir(&dll_root).map_err(|error| format!("failed to enumerate {}: {error}", dll_root.display()))?;
    for component in components {
        let component = component.map_err(|error| format!("failed to read Wine DLL entry: {error}"))?;
        let lane = component.path().join("i386-windows");
        if !lane.is_dir() {
            continue;
        }
        let entries =
            std::fs::read_dir(&lane).map_err(|error| format!("failed to enumerate {}: {error}", lane.display()))?;
        for entry in entries {
            let entry = entry.map_err(|error| format!("failed to read i386 Wine DLL entry: {error}"))?;
            let path = entry.path();
            if path.extension().and_then(|ext| ext.to_str()).is_some_and(|ext| ext.eq_ignore_ascii_case("dll")) {
                std::fs::copy(&path, syswow64.join(entry.file_name()))
                    .map_err(|error| format!("failed to stage {}: {error}", path.display()))?;
                staged += 1;
            }
        }
    }

    if staged == 0 {
        return Err("complete runtime contains no i386 Wine builtins to stage into the prefix".to_string());
    }
    Ok(staged)
}

fn run_runtime_provider_stage(prefix: &Path, verify_only: bool) -> Result<(), String> {
    let root = runtime_root();
    let script = root.join("scripts/stage-runtime-providers.sh");
    let xtajit64 = root.join("providers/xtajit64-arm64ec-known-good.dll");
    let xtajit = root.join("providers/xtajit-arm64-known-good.dll");
    if !script.is_file() {
        return Err(format!("runtime provider staging script is missing: {}", script.display()));
    }

    let output = Command::new("/bin/bash")
        .arg(&script)
        .arg(if verify_only { "--verify-prefix" } else { "--prefix" })
        .arg(prefix)
        .env("WINEBUILDDIR", root.join("wine/build-ec"))
        .env("VKMT_XTAJIT64_SOURCE", &xtajit64)
        .env("VKMT_XTAJIT_SOURCE", &xtajit)
        .env("VKMT_XTAJIT64_SHA256", XTAJIT64_SHA256)
        .env("VKMT_XTAJIT_SHA256", XTAJIT_SHA256)
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .output()
        .map_err(|error| format!("failed to run runtime provider staging: {error}"))?;
    if !output.status.success() {
        return Err(format!(
            "runtime provider {} failed: {}",
            if verify_only { "verification" } else { "staging" },
            String::from_utf8_lossy(&output.stderr).lines().last().unwrap_or("unknown error")
        ));
    }
    Ok(())
}

pub(crate) fn all_arch_ready(prefix: &Path) -> bool {
    [
        "system.reg",
        "user.reg",
        "drive_c/windows/system32/kernel32.dll",
        "drive_c/windows/system32/ntdll.dll",
        "drive_c/windows/system32/wow64.dll",
        "drive_c/windows/system32/wow64win.dll",
        "drive_c/windows/system32/xtajit64.dll",
        "drive_c/windows/system32/xtajit.dll",
        "drive_c/windows/syswow64/kernel32.dll",
        "drive_c/windows/syswow64/ntdll.dll",
        ".vkmt/gstreamer-runtime.sha256",
    ]
    .iter()
    .all(|relative| prefix.join(relative).is_file())
}

pub(crate) fn prepare(prefix: &Path) -> Result<(), String> {
    let root = runtime_root();
    let build = root.join("wine/build-ec");
    let wine = crate::platform::runtime_wine_binary(&root.join("wine"));
    if !wine.is_file() {
        return Err(format!("MetalSharp Wine not found: {}", wine.display()));
    }

    let system32 = prefix.join("drive_c/windows/system32");
    std::fs::create_dir_all(&system32).map_err(|error| format!("failed to create {}: {error}", system32.display()))?;
    std::fs::create_dir_all(prefix.join("drive_c/windows/syswow64"))
        .map_err(|error| format!("failed to create prefix syswow64: {error}"))?;

    copy_file_required(
        &build.join("dlls/wow64/aarch64-windows/wow64.dll"),
        &system32.join("wow64.dll"),
        "ARM64 WoW64 provider",
    )?;
    copy_file_required(
        &build.join("dlls/wow64win/aarch64-windows/wow64win.dll"),
        &system32.join("wow64win.dll"),
        "ARM64 WoW64 windowing provider",
    )?;
    stage_i386_builtins(&build, prefix)?;
    run_runtime_provider_stage(prefix, false)?;

    let wineboot = build.join("programs/wineboot/aarch64-windows/wineboot.exe");
    if !wineboot.is_file() {
        return Err(format!("ARM64 wineboot is missing: {}", wineboot.display()));
    }
    let mut wineboot_command = Command::new(&wine);
    wineboot_command
        .arg(&wineboot)
        .arg("--init")
        .env("WINEBOOTSTRAPMODE", "1")
        .env("WINE_NO_EXPLORER", "1")
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    apply_complete_runtime_env(&mut wineboot_command, prefix);
    let status =
        wineboot_command.status().map_err(|error| format!("failed to run all-architecture wineboot: {error}"))?;
    if !status.success() {
        return Err(format!("all-architecture wineboot failed with {status}"));
    }

    let wineserver = crate::platform::runtime_wineserver(&root.join("wine"));
    let wait_status = Command::new(&wineserver)
        .arg("-w")
        .env("WINEPREFIX", prefix)
        .status()
        .map_err(|error| format!("failed to wait for prefix wineserver: {error}"))?;
    if !wait_status.success() {
        return Err(format!("wineserver -w failed after prefix initialization with {wait_status}"));
    }

    run_runtime_provider_stage(prefix, false)?;
    run_runtime_provider_stage(prefix, true)?;
    if !all_arch_ready(prefix) {
        return Err("prefix was initialized but the ARM64/ARM64EC/x86_64/i386 acceptance gate failed".to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_arch_gate_requires_every_provider_lane() {
        let prefix = std::env::temp_dir().join(format!(
            "metalsharp-runtime-prefix-gate-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|duration| duration.as_nanos())
                .unwrap_or_default()
        ));
        for relative in [
            "system.reg",
            "user.reg",
            "drive_c/windows/system32/kernel32.dll",
            "drive_c/windows/system32/ntdll.dll",
            "drive_c/windows/system32/wow64.dll",
            "drive_c/windows/system32/wow64win.dll",
            "drive_c/windows/system32/xtajit64.dll",
            "drive_c/windows/system32/xtajit.dll",
            "drive_c/windows/syswow64/kernel32.dll",
            "drive_c/windows/syswow64/ntdll.dll",
            ".vkmt/gstreamer-runtime.sha256",
        ] {
            let path = prefix.join(relative);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, b"fixture").unwrap();
        }
        assert!(all_arch_ready(&prefix));
        std::fs::remove_file(prefix.join("drive_c/windows/system32/xtajit.dll")).unwrap();
        assert!(!all_arch_ready(&prefix));
        let _ = std::fs::remove_dir_all(prefix);
    }

    #[test]
    fn complete_runtime_environment_disables_every_tso_mode() {
        let prefix = PathBuf::from("/tmp/metalsharp-gog-prefix-fixture");
        let mut command = Command::new("/usr/bin/true");
        apply_complete_runtime_env(&mut command, &prefix);
        let env = command
            .get_envs()
            .filter_map(|(key, value)| {
                value.map(|value| (key.to_string_lossy().to_string(), value.to_string_lossy().to_string()))
            })
            .collect::<std::collections::HashMap<_, _>>();
        assert_eq!(env.get("WINEPREFIX"), Some(&prefix.to_string_lossy().to_string()));
        assert_eq!(env.get("FEX_TSOENABLED").map(String::as_str), Some("0"));
        assert_eq!(env.get("FEX_VECTORTSOENABLED").map(String::as_str), Some("0"));
        assert_eq!(env.get("FEX_MEMCPYSETTSOENABLED").map(String::as_str), Some("0"));
        assert!(env.get("WINEBUILDDIR").is_some_and(|value| value.ends_with("runtime/wine/build-ec")));
        assert!(env.contains_key("WINEMSYNC"));
        assert!(env.contains_key("METALSHARP_CONTROLLER_MODE"));
    }
}
