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

fn stage_pe_lane(build: &Path, lane_name: &str, destination: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(destination)
        .map_err(|error| format!("failed to create {}: {error}", destination.display()))?;
    let mut staged = 0usize;

    for (root_name, extension) in [("dlls", "dll"), ("programs", "exe")] {
        let root = build.join(root_name);
        let components =
            std::fs::read_dir(&root).map_err(|error| format!("failed to enumerate {}: {error}", root.display()))?;
        for component in components {
            let component = component.map_err(|error| format!("failed to read Wine PE entry: {error}"))?;
            let lane = component.path().join(lane_name);
            if !lane.is_dir() {
                continue;
            }
            let entries =
                std::fs::read_dir(&lane).map_err(|error| format!("failed to enumerate {}: {error}", lane.display()))?;
            for entry in entries {
                let entry = entry.map_err(|error| format!("failed to read Wine PE lane entry: {error}"))?;
                let path = entry.path();
                if path.extension().and_then(|ext| ext.to_str()).is_some_and(|ext| ext.eq_ignore_ascii_case(extension))
                    && should_stage_pe_entry(lane_name, &entry.file_name())
                {
                    std::fs::copy(&path, destination.join(entry.file_name()))
                        .map_err(|error| format!("failed to stage {}: {error}", path.display()))?;
                    staged += 1;
                }
            }
        }
    }

    if staged == 0 {
        return Err(format!("complete runtime contains no {lane_name} Wine PE files to stage"));
    }
    Ok(staged)
}

fn should_stage_pe_entry(lane_name: &str, name: &std::ffi::OsStr) -> bool {
    let name = name.to_string_lossy();
    if lane_name != "aarch64-windows" {
        return true;
    }

    // The custom ARM64 host uses xtajit.dll for i386. Wine's generic
    // wow64cpu.dll is a competing provider; if it is present while wineboot
    // refreshes the prefix, the x86 provider registry value can be rewritten
    // to wow64cpu.dll and every i386 process fails before reaching its entry
    // point. Candidate binaries are retained in the source/runtime tree for
    // development, but are never promoted into a user's System32 directory.
    let lowercase = name.to_ascii_lowercase();
    !(name.eq_ignore_ascii_case("wow64cpu.dll")
        || lowercase.starts_with("xtajit-") && lowercase.ends_with("-candidate.dll"))
}

fn stage_i386_builtins(build: &Path, prefix: &Path) -> Result<usize, String> {
    stage_pe_lane(build, "i386-windows", &prefix.join("drive_c/windows/syswow64"))
}

fn stage_aarch64_builtins(build: &Path, prefix: &Path) -> Result<usize, String> {
    stage_pe_lane(build, "aarch64-windows", &prefix.join("drive_c/windows/system32"))
}

fn ensure_unix_root_dosdevice(prefix: &Path) -> Result<(), String> {
    let dosdevices = prefix.join("dosdevices");
    std::fs::create_dir_all(&dosdevices)
        .map_err(|error| format!("failed to create {}: {error}", dosdevices.display()))?;
    let z_drive = dosdevices.join("z:");

    if std::fs::read_link(&z_drive).ok().as_deref() == Some(Path::new("/")) {
        return Ok(());
    }

    match std::fs::symlink_metadata(&z_drive) {
        Ok(metadata) if metadata.file_type().is_symlink() || metadata.is_file() => std::fs::remove_file(&z_drive)
            .map_err(|error| format!("failed to replace stale {}: {error}", z_drive.display()))?,
        Ok(metadata) if metadata.is_dir() => {
            return Err(format!(
                "cannot create the required Wine Z: mapping because {} is a directory",
                z_drive.display()
            ));
        },
        Ok(_) => std::fs::remove_file(&z_drive)
            .map_err(|error| format!("failed to replace stale {}: {error}", z_drive.display()))?,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {},
        Err(error) => return Err(format!("failed to inspect {}: {error}", z_drive.display())),
    }

    std::os::unix::fs::symlink("/", &z_drive)
        .map_err(|error| format!("failed to create required Wine Z: mapping at {}: {error}", z_drive.display()))
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
    let required_files_ready = [
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
    .all(|relative| prefix.join(relative).is_file());
    required_files_ready
        && std::fs::read_link(prefix.join("dosdevices/z:")).ok().as_deref() == Some(Path::new("/"))
        && wow64_x86_provider_ready(prefix)
        && !prefix.join("drive_c/windows/system32/wow64cpu.dll").exists()
        && [
            "drive_c/windows/system32/kernel32.dll",
            "drive_c/windows/system32/ntdll.dll",
            "drive_c/windows/system32/wow64.dll",
            "drive_c/windows/system32/wow64win.dll",
        ]
        .iter()
        .all(|relative| pe_machine(&prefix.join(relative)) == Some(0xaa64))
        && ["drive_c/windows/syswow64/kernel32.dll", "drive_c/windows/syswow64/ntdll.dll"]
            .iter()
            .all(|relative| pe_machine(&prefix.join(relative)) == Some(0x014c))
}

fn wow64_x86_provider_ready(prefix: &Path) -> bool {
    let Ok(registry) = std::fs::read_to_string(prefix.join("system.reg")) else {
        return false;
    };
    let section = "[Software\\\\Microsoft\\\\Wow64\\\\x86]";
    let Some(section_start) = registry.find(section) else {
        return false;
    };
    registry[section_start + section.len()..]
        .lines()
        .take_while(|line| !line.starts_with('['))
        .any(|line| line.trim().eq_ignore_ascii_case("@=\"xtajit.dll\""))
}

fn remove_conflicting_cpu_providers(prefix: &Path) -> Result<(), String> {
    let system32 = prefix.join("drive_c/windows/system32");
    let entries =
        std::fs::read_dir(&system32).map_err(|error| format!("failed to enumerate {}: {error}", system32.display()))?;
    for entry in entries {
        let entry = entry.map_err(|error| format!("failed to inspect System32 provider entry: {error}"))?;
        let name = entry.file_name();
        if !should_stage_pe_entry("aarch64-windows", &name) {
            let path = entry.path();
            std::fs::remove_file(&path)
                .map_err(|error| format!("failed to remove conflicting CPU provider {}: {error}", path.display()))?;
        }
    }
    Ok(())
}

fn configure_wow64_x86_provider(wine: &Path, prefix: &Path) -> Result<(), String> {
    let reg = prefix.join("drive_c/windows/system32/reg.exe");
    if !reg.is_file() {
        return Err(format!("ARM64 registry tool is missing after wineboot: {}", reg.display()));
    }
    let mut command = Command::new(wine);
    command
        .arg(&reg)
        .args(["add", r"HKLM\Software\Microsoft\Wow64\x86", "/ve", "/t", "REG_SZ", "/d", "xtajit.dll", "/f"])
        .env("WINE_NO_EXPLORER", "1")
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    apply_complete_runtime_env(&mut command, prefix);
    let status = command.status().map_err(|error| format!("failed to select the i386 xtajit provider: {error}"))?;
    if !status.success() {
        return Err(format!("selecting the i386 xtajit provider failed with {status}"));
    }
    Ok(())
}

fn pe_machine(path: &Path) -> Option<u16> {
    use std::io::{Read, Seek, SeekFrom};

    let mut file = std::fs::File::open(path).ok()?;
    let mut mz = [0u8; 64];
    file.read_exact(&mut mz).ok()?;
    if &mz[..2] != b"MZ" {
        return None;
    }
    let pe_offset = u32::from_le_bytes(mz[0x3c..0x40].try_into().ok()?) as u64;
    file.seek(SeekFrom::Start(pe_offset)).ok()?;
    let mut header = [0u8; 6];
    file.read_exact(&mut header).ok()?;
    if &header[..4] != b"PE\0\0" {
        return None;
    }
    Some(u16::from_le_bytes([header[4], header[5]]))
}

pub(crate) fn prepare(prefix: &Path) -> Result<(), String> {
    prepare_with_mode(prefix, WinebootMode::Initialize)
}

pub(crate) fn update_existing(prefix: &Path) -> Result<(), String> {
    if !prefix.join("drive_c").is_dir() {
        return Err(format!("existing Wine prefix is missing drive_c: {}", prefix.display()));
    }
    prepare_with_mode(prefix, WinebootMode::Update)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum WinebootMode {
    Initialize,
    Update,
}

impl WinebootMode {
    fn arguments(self) -> &'static [&'static str] {
        match self {
            Self::Initialize => &["--init"],
            // wineboot's update-only path processes HKCU Run and would launch
            // Steam (or any other user startup application) in the middle of
            // migration. Combining init and update performs update_wineprefix
            // while suppressing those user startup entries.
            Self::Update => &["--init", "--update"],
        }
    }
}

fn prepare_with_mode(prefix: &Path, mode: WinebootMode) -> Result<(), String> {
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

    // Wine starts the PE entry point from an absolute Unix path. Its Unix-side
    // startup conversion requires that path to be reachable through a DOS
    // drive; without the canonical Z: mapping Wine 11.12 can produce a null NT
    // image path before wineboot gets control. Migration restores any user's
    // custom Z: mapping after this update has completed.
    ensure_unix_root_dosdevice(prefix)?;

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
    if mode == WinebootMode::Update {
        stage_aarch64_builtins(&build, prefix)?;
    }
    run_runtime_provider_stage(prefix, false)?;

    let wineboot = build.join("programs/wineboot/aarch64-windows/wineboot.exe");
    if !wineboot.is_file() {
        return Err(format!("ARM64 wineboot is missing: {}", wineboot.display()));
    }
    let mut wineboot_command = Command::new(&wine);
    wineboot_command
        .arg(&wineboot)
        .args(mode.arguments())
        .env("WINE_NO_EXPLORER", "1")
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    if mode == WinebootMode::Initialize {
        wineboot_command.env("WINEBOOTSTRAPMODE", "1");
    }
    apply_complete_runtime_env(&mut wineboot_command, prefix);
    let status =
        wineboot_command.status().map_err(|error| format!("failed to run all-architecture wineboot: {error}"))?;
    if !status.success() {
        return Err(format!("all-architecture wineboot failed with {status}"));
    }

    remove_conflicting_cpu_providers(prefix)?;
    configure_wow64_x86_provider(&wine, prefix)?;

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
            "drive_c/windows/system32/xtajit64.dll",
            "drive_c/windows/system32/xtajit.dll",
            ".vkmt/gstreamer-runtime.sha256",
        ] {
            let path = prefix.join(relative);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, b"fixture").unwrap();
        }
        for relative in [
            "drive_c/windows/system32/kernel32.dll",
            "drive_c/windows/system32/ntdll.dll",
            "drive_c/windows/system32/wow64.dll",
            "drive_c/windows/system32/wow64win.dll",
        ] {
            write_pe_fixture(&prefix.join(relative), 0xaa64);
        }
        for relative in ["drive_c/windows/syswow64/kernel32.dll", "drive_c/windows/syswow64/ntdll.dll"] {
            write_pe_fixture(&prefix.join(relative), 0x014c);
        }
        assert!(!all_arch_ready(&prefix), "a prefix without Wine's Unix-root mapping is not launch-ready");
        ensure_unix_root_dosdevice(&prefix).expect("create Wine Z mapping fixture");
        append_xtajit_registry_fixture(&prefix);
        assert!(all_arch_ready(&prefix));

        std::fs::write(prefix.join("drive_c/windows/system32/wow64cpu.dll"), b"competing provider").unwrap();
        assert!(!all_arch_ready(&prefix), "generic wow64cpu must not compete with the custom i386 provider");
        std::fs::remove_file(prefix.join("drive_c/windows/system32/wow64cpu.dll")).unwrap();

        std::fs::write(
            prefix.join("system.reg"),
            "WINE REGISTRY Version 2\n\n[Software\\\\Microsoft\\\\Wow64\\\\x86] 1\n@=\"wow64cpu.dll\"\n",
        )
        .unwrap();
        assert!(!all_arch_ready(&prefix), "the i386 registry contract must select xtajit.dll");
        append_xtajit_registry_fixture(&prefix);
        assert!(all_arch_ready(&prefix));

        std::fs::remove_file(prefix.join("drive_c/windows/system32/xtajit.dll")).unwrap();
        assert!(!all_arch_ready(&prefix));
        let _ = std::fs::remove_dir_all(prefix);
    }

    fn write_pe_fixture(path: &Path, machine: u16) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        let mut bytes = vec![0u8; 0x46];
        bytes[..2].copy_from_slice(b"MZ");
        bytes[0x3c..0x40].copy_from_slice(&(0x40u32).to_le_bytes());
        bytes[0x40..0x44].copy_from_slice(b"PE\0\0");
        bytes[0x44..0x46].copy_from_slice(&machine.to_le_bytes());
        std::fs::write(path, bytes).unwrap();
    }

    fn append_xtajit_registry_fixture(prefix: &Path) {
        std::fs::write(
            prefix.join("system.reg"),
            "WINE REGISTRY Version 2\n\n[Software\\\\Microsoft\\\\Wow64\\\\x86] 1\n@=\"xtajit.dll\"\n",
        )
        .unwrap();
    }

    #[test]
    fn all_arch_gate_rejects_x86_64_system32_builtins() {
        let prefix = std::env::temp_dir().join(format!("metalsharp-runtime-prefix-machine-{}", std::process::id()));
        for relative in [
            "system.reg",
            "user.reg",
            "drive_c/windows/system32/xtajit64.dll",
            "drive_c/windows/system32/xtajit.dll",
            ".vkmt/gstreamer-runtime.sha256",
        ] {
            let path = prefix.join(relative);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, b"fixture").unwrap();
        }
        for relative in [
            "drive_c/windows/system32/kernel32.dll",
            "drive_c/windows/system32/ntdll.dll",
            "drive_c/windows/system32/wow64.dll",
            "drive_c/windows/system32/wow64win.dll",
        ] {
            write_pe_fixture(&prefix.join(relative), 0xaa64);
        }
        for relative in ["drive_c/windows/syswow64/kernel32.dll", "drive_c/windows/syswow64/ntdll.dll"] {
            write_pe_fixture(&prefix.join(relative), 0x014c);
        }
        ensure_unix_root_dosdevice(&prefix).expect("create Wine Z mapping fixture");
        append_xtajit_registry_fixture(&prefix);
        write_pe_fixture(&prefix.join("drive_c/windows/system32/kernel32.dll"), 0x8664);
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

    #[test]
    fn existing_prefix_updates_use_the_wineboot_update_contract() {
        assert_eq!(WinebootMode::Initialize.arguments(), &["--init"]);
        assert_eq!(WinebootMode::Update.arguments(), &["--init", "--update"]);
    }

    #[test]
    fn arm64_staging_excludes_competing_and_candidate_cpu_providers() {
        assert!(!should_stage_pe_entry("aarch64-windows", std::ffi::OsStr::new("wow64cpu.dll")));
        assert!(!should_stage_pe_entry("aarch64-windows", std::ffi::OsStr::new("xtajit-exception-candidate.dll")));
        assert!(should_stage_pe_entry("aarch64-windows", std::ffi::OsStr::new("xtajit.dll")));
        assert!(should_stage_pe_entry("i386-windows", std::ffi::OsStr::new("wow64cpu.dll")));
    }

    #[test]
    fn wineboot_prefix_always_has_a_unix_root_mapping() {
        let prefix = std::env::temp_dir().join(format!(
            "metalsharp-runtime-prefix-z-drive-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|duration| duration.as_nanos())
                .unwrap_or_default()
        ));
        let dosdevices = prefix.join("dosdevices");
        std::fs::create_dir_all(&dosdevices).expect("create dosdevices fixture");
        std::os::unix::fs::symlink("/Volumes/stale", dosdevices.join("z:")).expect("create stale Z fixture");

        ensure_unix_root_dosdevice(&prefix).expect("repair Wine Z mapping");
        assert_eq!(std::fs::read_link(dosdevices.join("z:")).expect("read Wine Z mapping"), Path::new("/"));

        let _ = std::fs::remove_dir_all(prefix);
    }
}
