use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const GPTK_SEED_TIMEOUT: Duration = Duration::from_secs(2 * 60 * 60);
const GPTK_VCRUN_MIN_SIZE: u64 = 1_000_000;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostPlatform {
    Macos,
    Linux,
    Other,
}

pub fn current() -> HostPlatform {
    if cfg!(target_os = "macos") {
        HostPlatform::Macos
    } else if cfg!(target_os = "linux") {
        HostPlatform::Linux
    } else {
        HostPlatform::Other
    }
}

pub fn name() -> &'static str {
    match current() {
        HostPlatform::Macos => "macos",
        HostPlatform::Linux => "linux",
        HostPlatform::Other => "other",
    }
}

pub fn app_resources_dir() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    app_resources_dir_for_exe(&exe)
}

pub fn metalsharp_home_dir() -> PathBuf {
    metalsharp_home_dir_for(&dirs::home_dir().unwrap_or_default())
}

pub fn metalsharp_home_dir_for<T: AsRef<std::path::Path> + ?Sized>(home: &T) -> PathBuf {
    if let Ok(value) = std::env::var("METALSHARP_HOME") {
        let trimmed = value.trim();
        if !trimmed.is_empty() {
            return PathBuf::from(trimmed);
        }
    }
    home.as_ref().join(".metalsharp")
}

fn app_resources_dir_for_exe(exe: &std::path::Path) -> Option<PathBuf> {
    let exe_dir = exe.parent()?;

    if cfg!(target_os = "macos") {
        return macos_resources_dir_for_exe(exe);
    }

    if exe_dir.file_name().and_then(|n| n.to_str()) == Some("resources") {
        return Some(exe_dir.to_path_buf());
    }

    Some(exe_dir.to_path_buf())
}

fn macos_resources_dir_for_exe(exe: &std::path::Path) -> Option<PathBuf> {
    let mut dir = exe.parent()?;
    loop {
        if dir.file_name().and_then(|n| n.to_str()) == Some("Resources") {
            return Some(dir.to_path_buf());
        }

        let contents = dir.parent()?;
        if contents.file_name().and_then(|n| n.to_str()) == Some("Contents") {
            return Some(contents.join("Resources"));
        }

        dir = contents;
    }
}

pub fn runtime_library_env(ms_root: &std::path::Path) -> Option<(&'static str, String)> {
    let value = format!(
        "{}:{}",
        ms_root.join("lib").to_string_lossy(),
        ms_root.join("lib").join("wine").join("x86_64-unix").to_string_lossy()
    );

    match current() {
        HostPlatform::Macos => Some(("DYLD_FALLBACK_LIBRARY_PATH", value)),
        HostPlatform::Linux => Some(("LD_LIBRARY_PATH", value)),
        HostPlatform::Other => None,
    }
}

pub fn set_runtime_library_env(cmd: &mut Command, ms_root: &std::path::Path) {
    if let Some((key, value)) = runtime_library_env(ms_root) {
        cmd.env(key, value);
    }
}

pub fn runtime_wine_binary(ms_root: &std::path::Path) -> PathBuf {
    let wrapped = ms_root.join("bin").join("metalsharp-wine");
    if wrapped.exists() {
        return wrapped;
    }

    ms_root.join("bin").join("wine")
}

pub fn runtime_wineserver(ms_root: &std::path::Path) -> PathBuf {
    ms_root.join("bin").join("wineserver")
}

pub fn gptk_staged_app_root(home: &Path) -> PathBuf {
    metalsharp_home_dir_for(home).join("runtime").join("gptk").join("Game Porting Toolkit.app")
}

pub fn gptk_staged_wine_root(home: &Path) -> PathBuf {
    gptk_staged_app_root(home).join("Contents").join("Resources").join("wine")
}

pub fn gptk_homebrew_app_root() -> PathBuf {
    PathBuf::from("/Applications/Game Porting Toolkit.app")
}

pub fn gptk_homebrew_wine_root() -> PathBuf {
    gptk_homebrew_app_root().join("Contents").join("Resources").join("wine")
}

pub fn gptk_wine_root_for_home(_home: &Path) -> PathBuf {
    // GPTK is Homebrew-owned. MetalSharp must not stage or prefer a private
    // ~/.metalsharp/runtime/gptk copy, because the route DLLs/framework must
    // stay ABI-matched with Homebrew's GPTK Wine build.
    gptk_homebrew_wine_root()
}

pub fn gptk_wine_root() -> PathBuf {
    dirs::home_dir().map(|home| gptk_wine_root_for_home(&home)).unwrap_or_else(gptk_homebrew_wine_root)
}

pub fn gptk_prefix_path(home: &Path) -> PathBuf {
    metalsharp_home_dir_for(home).join("prefix-gptk")
}

/// Canonical home of the shared Sharp Library prefix. Sharp Library apps
/// that don't have their own installer bottle install into this prefix so
/// the launcher, runtime components, and auto-sync can treat them as one
/// consistent bucket. Existing per-installer-bottle prefixes under
/// `~/.metalsharp/bottles/installer_<id>/prefix/` remain valid for
/// isolation; this is the parallel shared root.
pub fn sharp_prefix_path(home: &Path) -> PathBuf {
    metalsharp_home_dir_for(home).join("sharp-prefix")
}

pub fn gptk_seed_log_path(home: &Path) -> PathBuf {
    metalsharp_home_dir_for(home).join("logs").join("gptk-prefix-seed.log")
}

fn gptk_seed_failure_path(home: &Path) -> PathBuf {
    gptk_prefix_path(home).join(".gptk-seed-failed")
}

fn gptk_steam_exe(prefix: &Path) -> PathBuf {
    gptk_steam_dir(prefix).join("Steam.exe")
}

fn gptk_steam_dir(prefix: &Path) -> PathBuf {
    prefix.join("drive_c").join("Program Files (x86)").join("Steam")
}

pub fn gptk_disabled_steam_exe(prefix: &Path) -> PathBuf {
    gptk_steam_dir(prefix).join("Steam.exe.metalsharp-offline-disabled")
}

fn gptk_steam_launcher_present(prefix: &Path) -> bool {
    gptk_steam_exe(prefix).is_file() || gptk_disabled_steam_exe(prefix).is_file()
}

pub fn disable_gptk_steam_launcher_for_offline(prefix: &Path) -> Result<bool, Box<dyn std::error::Error>> {
    let steam_exe = gptk_steam_exe(prefix);
    let disabled = gptk_disabled_steam_exe(prefix);
    if disabled.exists() {
        return Ok(false);
    }
    if steam_exe.exists() {
        std::fs::rename(&steam_exe, &disabled)
            .map_err(|e| format!("disable GPTK Steam launcher {}: {}", steam_exe.display(), e))?;
        return Ok(true);
    }
    Ok(false)
}

pub fn restore_gptk_steam_launcher(prefix: &Path) -> Result<bool, Box<dyn std::error::Error>> {
    let steam_exe = gptk_steam_exe(prefix);
    let disabled = gptk_disabled_steam_exe(prefix);
    if steam_exe.exists() || !disabled.exists() {
        return Ok(false);
    }
    std::fs::rename(&disabled, &steam_exe)
        .map_err(|e| format!("restore GPTK Steam launcher {}: {}", steam_exe.display(), e))?;
    Ok(true)
}

const GPTK_WINEBOOT_REQUIRED_PATHS: &[(&str, bool)] = &[
    ("drive_c", true),
    ("drive_c/windows", true),
    ("drive_c/windows/system32", true),
    ("drive_c/windows/system32/kernel32.dll", false),
    ("drive_c/windows/system32/ntdll.dll", false),
    ("drive_c/windows/system32/d3d10.dll", false),
    ("drive_c/windows/system32/d3d11.dll", false),
    ("drive_c/windows/system32/d3d10.dll", false),
    ("drive_c/windows/system32/d3d11.dll", false),
    ("drive_c/windows/system32/d3d12.dll", false),
    ("drive_c/windows/system32/dxgi.dll", false),
    ("drive_c/windows/system32/nvapi64.dll", false),
    ("drive_c/windows/system32/nvngx-on-metalfx.dll", false),
    ("dosdevices", true),
    ("dosdevices/c:", false),
];

const GPTK_ROUTE_DLLS: &[&str] =
    &["d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll", "nvngx-on-metalfx.dll"];

fn missing_gptk_wineboot_paths(prefix: &Path) -> Vec<String> {
    GPTK_WINEBOOT_REQUIRED_PATHS
        .iter()
        .filter_map(|(relative, is_dir)| {
            let path = prefix.join(relative);
            let exists = if *is_dir { path.is_dir() } else { path.exists() };
            if exists {
                None
            } else {
                Some((*relative).to_string())
            }
        })
        .collect()
}

fn gptk_wineboot_runtime_ready(prefix: &Path) -> bool {
    missing_gptk_wineboot_paths(prefix).is_empty()
}

fn gptk_runtime_wine_root(home: &Path) -> PathBuf {
    metalsharp_home_dir_for(home).join("runtime").join("wine")
}

fn gptk_runtime_pe_dir(home: &Path) -> PathBuf {
    let local_legacy = gptk_runtime_wine_root(home).join("lib").join("gptk").join("x86_64-windows");
    if cfg!(test) && local_legacy.is_dir() {
        return local_legacy;
    }
    gptk_homebrew_wine_root().join("lib").join("wine").join("x86_64-windows")
}

fn file_nonempty(path: &Path) -> bool {
    path.metadata().map(|meta| meta.is_file() && meta.len() > 0).unwrap_or(false)
}

fn gptk_seed_winedllpath(home: &Path) -> Result<String, Box<dyn std::error::Error>> {
    let gptk_pe_dir = gptk_runtime_pe_dir(home);
    let missing: Vec<&str> =
        GPTK_ROUTE_DLLS.iter().copied().filter(|dll| !file_nonempty(&gptk_pe_dir.join(dll))).collect();
    if !missing.is_empty() {
        return Err(format!(
            "Homebrew GPTK D3DMetal route DLLs missing ({}): reinstall Homebrew GPTK first",
            missing.join(", ")
        )
        .into());
    }

    let metalsharp_pe_dir = gptk_runtime_wine_root(home).join("lib").join("metalsharp").join("x86_64-windows");
    let mut dirs = vec![gptk_pe_dir.to_string_lossy().to_string()];
    if metalsharp_pe_dir.is_dir() {
        dirs.push(metalsharp_pe_dir.to_string_lossy().to_string());
    }
    Ok(dirs.join(":"))
}

fn gptk_seed_dyld(home: &Path) -> String {
    let wine_root = gptk_wine_root_for_home(home);
    let mut parts = vec![
        wine_root.join("lib"),
        wine_root.join("lib").join("wine").join("x86_64-unix"),
        wine_root.join("lib").join("wine").join("x86_32on64-unix"),
        wine_root.join("lib").join("external"),
    ];
    parts.retain(|path| path.is_dir());
    parts.iter().map(|path| path.to_string_lossy().to_string()).collect::<Vec<_>>().join(":")
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum GptkPrefixStatus {
    Ready,
    Seeding,
    Failed(String),
    Partial(String),
    Missing,
}

pub fn gptk_prefix_ready(home: &Path) -> bool {
    let prefix = gptk_prefix_path(home);
    if !prefix.join(".gptk-ready").is_file() {
        return false;
    }
    let dosdevices = prefix.join("dosdevices");
    let ready = gptk_steam_launcher_present(&prefix) && dosdevices.is_dir() && gptk_wineboot_runtime_ready(&prefix);
    if ready {
        if prefix.join(".gptk-seeding").exists() {
            let _ = std::fs::remove_file(prefix.join(".gptk-seeding"));
        }
        if gptk_seed_failure_path(home).exists() {
            let _ = std::fs::remove_file(gptk_seed_failure_path(home));
        }
    }
    ready
}

pub fn gptk_prefix_seeding(home: &Path) -> bool {
    gptk_seeding_marker_active(&gptk_prefix_path(home))
}

fn gptk_seeding_marker_active(prefix: &Path) -> bool {
    let marker = prefix.join(".gptk-seeding");
    if !marker.exists() {
        return false;
    }
    if let Ok(meta) = std::fs::metadata(&marker) {
        if let Ok(modified) = meta.modified() {
            if let Ok(age) = modified.elapsed() {
                if age > GPTK_SEED_TIMEOUT {
                    eprintln!("gptk: seeding marker is {}s old — clearing as stale", age.as_secs());
                    let _ = std::fs::remove_file(&marker);
                    return false;
                }
            }
        }
    }
    true
}

pub fn gptk_prefix_status(home: &Path) -> GptkPrefixStatus {
    gptk_prefix_status_for_prefix(&gptk_prefix_path(home), &gptk_seed_failure_path(home))
}

fn gptk_prefix_status_for_prefix(prefix: &Path, failed_marker: &Path) -> GptkPrefixStatus {
    let ready_marker = prefix.join(".gptk-ready");
    let dosdevices = prefix.join("dosdevices");
    if ready_marker.is_file()
        && gptk_steam_launcher_present(prefix)
        && dosdevices.is_dir()
        && gptk_wineboot_runtime_ready(prefix)
    {
        let _ = std::fs::remove_file(prefix.join(".gptk-seeding"));
        let _ = std::fs::remove_file(failed_marker);
        return GptkPrefixStatus::Ready;
    }
    if gptk_seeding_marker_active(prefix) {
        return GptkPrefixStatus::Seeding;
    }
    if failed_marker.is_file() {
        let detail = std::fs::read_to_string(failed_marker)
            .unwrap_or_else(|_| "GPTK prefix seed failed; repair can retry".to_string());
        return GptkPrefixStatus::Failed(detail.trim().to_string());
    }
    if !prefix.exists() {
        return GptkPrefixStatus::Missing;
    }
    let has_entries = std::fs::read_dir(prefix).map(|mut entries| entries.next().is_some()).unwrap_or(false);
    if !has_entries {
        return GptkPrefixStatus::Missing;
    }
    let drive_c = prefix.join("drive_c");
    if !drive_c.is_dir() {
        return GptkPrefixStatus::Partial("GPTK prefix is incomplete: missing drive_c".to_string());
    }
    if !dosdevices.is_dir() {
        return GptkPrefixStatus::Partial("GPTK prefix is incomplete: missing dosdevices".to_string());
    }
    let missing_runtime = missing_gptk_wineboot_paths(prefix);
    if !missing_runtime.is_empty() {
        return GptkPrefixStatus::Partial(format!(
            "GPTK prefix is incomplete: missing Wine runtime files ({})",
            missing_runtime.join(", ")
        ));
    }
    if !gptk_steam_launcher_present(prefix) {
        return GptkPrefixStatus::Partial("GPTK prefix is incomplete: missing Steam.exe".to_string());
    }
    if !ready_marker.is_file() {
        return GptkPrefixStatus::Partial("GPTK prefix has Steam but is missing the ready marker".to_string());
    }
    GptkPrefixStatus::Partial("GPTK prefix is incomplete and needs repair".to_string())
}

fn append_gptk_seed_log(home: &Path, message: &str) {
    let log_path = gptk_seed_log_path(home);
    if let Some(parent) = log_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(&log_path) {
        let _ = writeln!(file, "[{}] {}", now, message);
    }
    eprintln!("gptk: {}", message);
}

fn clear_gptk_seed_failure(home: &Path) {
    let _ = std::fs::remove_file(gptk_seed_failure_path(home));
}

fn record_gptk_seed_failure(home: &Path, error: &str) {
    let failed_marker = gptk_seed_failure_path(home);
    if let Some(parent) = failed_marker.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let _ = std::fs::write(&failed_marker, error);
    append_gptk_seed_log(home, &format!("seed failed: {}", error));
}

fn command_output_text(output: &std::process::Output) -> String {
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    let combined = format!("{}{}", stdout, stderr);
    combined.trim().to_string()
}

pub fn ensure_gptk_dosdevices(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let gptk_prefix = gptk_prefix_path(home);
    let dosdevices = gptk_prefix.join("dosdevices");
    if !dosdevices.is_dir() {
        std::fs::create_dir_all(&dosdevices)?;
    }

    let c_link = dosdevices.join("c:");
    if c_link.exists() {
        if let Ok(target) = std::fs::read_link(&c_link) {
            if target != *Path::new("../drive_c") {
                let _ = std::fs::remove_file(&c_link);
                let _ = std::os::unix::fs::symlink("../drive_c", &c_link);
            }
        }
    } else {
        let _ = std::os::unix::fs::symlink("../drive_c", &c_link);
    }

    ensure_dosdevice(&dosdevices, "y:", home);
    ensure_dosdevice(&dosdevices, "z:", Path::new("/"));

    let steam_prefix = metalsharp_home_dir_for(home).join("prefix-steam");
    let wine_steamapps = steam_prefix.join("drive_c").join("Program Files (x86)").join("Steam").join("steamapps");

    if let Ok(lf_data) = std::fs::read_to_string(wine_steamapps.join("libraryfolders.vdf")) {
        for library_path in parse_library_paths_from_vdf(&lf_data) {
            if library_path.is_absolute() && library_path.is_dir() {
                ensure_dosdevice_for_path(&dosdevices, &library_path);
            }
        }
    }

    if let Ok(mac_lf_paths) = std::fs::read_dir("/Volumes") {
        for entry in mac_lf_paths {
            let Ok(entry) = entry else { continue };
            let vol_path = entry.path();
            if vol_path.is_dir() {
                ensure_dosdevice_for_path(&dosdevices, &vol_path);
            }
        }
    }

    Ok(())
}

fn ensure_dosdevice(dosdevices: &Path, letter: &str, target: &Path) {
    let link = dosdevices.join(letter);
    if link.exists() {
        if let Ok(current) = std::fs::read_link(&link) {
            if current == target {
                return;
            }
        }
        let _ = std::fs::remove_file(&link);
    }
    if let Err(e) = std::os::unix::fs::symlink(target, &link) {
        eprintln!("gptk dosdevices: failed to link {} -> {}: {}", letter, target.display(), e);
    } else {
        eprintln!("gptk dosdevices: {} -> {}", letter, target.display());
    }
}

fn ensure_dosdevice_for_path(dosdevices: &Path, target: &Path) {
    if target.starts_with("/Volumes") {
        let Ok(rd) = std::fs::read_dir(dosdevices) else {
            return;
        };
        for entry in rd.flatten() {
            let name_str = entry.file_name().to_string_lossy().to_string();
            if name_str.len() == 2 && name_str.ends_with(':') {
                if let Ok(current) = std::fs::read_link(entry.path()) {
                    if current == target {
                        return;
                    }
                }
            }
        }
    }

    let existing: std::collections::HashSet<String> = match std::fs::read_dir(dosdevices) {
        Ok(rd) => rd
            .filter_map(|e| e.ok())
            .filter_map(|e| {
                let name = e.file_name().to_string_lossy().to_string();
                if name.len() == 2 && name.ends_with(':') {
                    Some(name)
                } else {
                    None
                }
            })
            .collect(),
        Err(_) => std::collections::HashSet::new(),
    };

    for ch in b'd'..=b'z' {
        let letter = format!("{}:", ch as char);
        if existing.contains(&letter) {
            continue;
        }
        let link = dosdevices.join(&letter);
        match std::os::unix::fs::symlink(target, &link) {
            Ok(()) => {
                eprintln!("gptk dosdevices: {} -> {} (steam library)", letter, target.display());
                return;
            },
            Err(e) => {
                eprintln!("gptk dosdevices: failed {} -> {}: {}", letter, target.display(), e);
                return;
            },
        }
    }
}

fn parse_library_paths_from_vdf(data: &str) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    for line in data.lines() {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.strip_prefix("\"path\"") {
            if let Some(value) = rest.split('"').nth(1) {
                let p = PathBuf::from(value);
                if !paths.contains(&p) {
                    paths.push(p);
                }
            }
        }
    }
    paths
}

fn is_game_storage_volume(name: &str) -> bool {
    if name == "Macintosh HD" {
        return false;
    }
    if name.starts_with("MetalSharp") {
        return false;
    }
    if name.starts_with("Game Porting Toolkit") {
        return false;
    }
    if name.contains("Evaluation environment") {
        return false;
    }
    true
}

pub fn find_external_game_storage() -> Option<PathBuf> {
    let entries = std::fs::read_dir("/Volumes").ok()?;
    for entry in entries {
        let entry = entry.ok()?;
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if !is_game_storage_volume(&name_str) {
            continue;
        }
        let vol_path = entry.path();
        if !vol_path.is_dir() {
            continue;
        }
        let games_dir = vol_path.join("MetalSharp").join("games");
        if std::fs::create_dir_all(&games_dir).is_ok() {
            return Some(games_dir);
        }
    }
    None
}

pub fn is_path_on_external_volume(path: &Path) -> bool {
    let canonical = match std::fs::canonicalize(path) {
        Ok(c) => c,
        Err(_) => return false,
    };
    let canon_str = canonical.to_string_lossy();
    if !canon_str.starts_with("/Volumes/") {
        return false;
    }
    let rest = canon_str.strip_prefix("/Volumes/").unwrap_or("");
    let vol_name = rest.split('/').next().unwrap_or("");
    is_game_storage_volume(vol_name)
}

pub fn migrate_game_to_external(
    home: &Path,
    game_dir: &Path,
    appid: u32,
) -> Result<Option<PathBuf>, Box<dyn std::error::Error>> {
    if !game_dir.is_dir() {
        return Ok(None);
    }

    if is_path_on_external_volume(game_dir) {
        eprintln!("gptk migrate: game already on external volume ({})", game_dir.display());
        return Ok(None);
    }

    let storage = match find_external_game_storage() {
        Some(s) => s,
        None => {
            eprintln!("gptk migrate: no writable external SSD found, using internal path");
            return Ok(None);
        },
    };

    let game_name = match game_dir.file_name() {
        Some(n) => n.to_string_lossy().to_string(),
        None => return Ok(None),
    };

    let external_game_dir = storage.join(format!("{}", appid)).join(&game_name);

    if external_game_dir.is_dir() && std::fs::read_dir(&external_game_dir).map(|r| r.count()).unwrap_or(0) > 0 {
        eprintln!("gptk migrate: external copy already exists at {}", external_game_dir.display());
        return Ok(Some(external_game_dir));
    }

    eprintln!("gptk migrate: copying game '{}' (appid {}) from internal to external SSD ...", game_name, appid);

    let external_base = storage.join(format!("{}", appid));
    std::fs::create_dir_all(&external_base)?;

    let tmp_name = format!(".{}.partial", game_name);
    let tmp_dir = external_base.join(&tmp_name);
    if tmp_dir.is_dir() {
        let _ = std::fs::remove_dir_all(&tmp_dir);
    }

    copy_dir_recursive(game_dir, &tmp_dir).map_err(|e| {
        let _ = std::fs::remove_dir_all(&tmp_dir);
        format!("gptk migrate: copy failed: {}", e)
    })?;

    if let Err(e) = verify_dir_copy(game_dir, &tmp_dir) {
        eprintln!("gptk migrate: verification failed, cleaning up: {}", e);
        let _ = std::fs::remove_dir_all(&tmp_dir);
        return Err(e.into());
    }

    if let Err(e) = std::fs::rename(&tmp_dir, &external_game_dir) {
        let _ = std::fs::remove_dir_all(&tmp_dir);
        return Err(format!("gptk migrate: rename failed: {}", e).into());
    }

    eprintln!("gptk migrate: game copied to external SSD at {}", external_game_dir.display());

    ensure_gptk_dosdevices(home)?;

    Ok(Some(external_game_dir))
}

pub fn ensure_gptk_prefix(home: &Path) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let gptk_prefix = gptk_prefix_path(home);

    match gptk_prefix_status(home) {
        GptkPrefixStatus::Ready => {
            sync_gptk_prefix(home)?;
            return Ok(gptk_prefix);
        },
        GptkPrefixStatus::Seeding => {
            return Err("GPTK prefix is still being prepared — try again in a moment".into());
        },
        GptkPrefixStatus::Failed(_) | GptkPrefixStatus::Partial(_) | GptkPrefixStatus::Missing => {},
    }

    let home = home.to_path_buf();
    let gptk_prefix = gptk_prefix_path(&home);
    let _ = std::fs::create_dir_all(&gptk_prefix);
    clear_gptk_seed_failure(&home);
    if let Err(e) = std::fs::write(gptk_prefix.join(".gptk-seeding"), "seeding") {
        return Err(format!("failed to write seeding marker: {}", e).into());
    }
    std::thread::spawn(move || {
        if let Err(e) = seed_gptk_prefix_sync(&home) {
            eprintln!("gptk prefix seed failed: {}", e);
            let marker = gptk_prefix_path(&home).join(".gptk-seeding");
            let _ = std::fs::remove_file(&marker);
        }
    });

    Err("GPTK prefix is being prepared — try again in a moment".into())
}

struct SeedingGuard {
    marker: PathBuf,
    armed: bool,
}

impl SeedingGuard {
    fn new(marker: PathBuf) -> Self {
        Self { marker, armed: true }
    }
    fn disarm(&mut self) {
        self.armed = false;
    }
}

impl Drop for SeedingGuard {
    fn drop(&mut self) {
        if self.armed && self.marker.exists() {
            eprintln!("gptk: cleaning up seeding marker after failure");
            let _ = std::fs::remove_file(&self.marker);
        }
    }
}

fn fast_copy_dir(src: &Path, dst: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let status = std::process::Command::new("ditto").arg(src).arg(dst).stderr(std::process::Stdio::piped()).status()?;
    if !status.success() {
        eprintln!("gptk: ditto failed ({:?}), falling back to recursive copy", status.code());
        let (copied, skipped) = copy_dir_tolerant(src, dst);
        if copied == 0 && skipped > 0 {
            return Err(format!(
                "gptk copy failed: 0 files copied, {} skipped from {} -> {}",
                skipped,
                src.display(),
                dst.display()
            )
            .into());
        }
    }
    Ok(())
}

fn copy_dir_tolerant(src: &Path, dst: &Path) -> (u64, u64) {
    let mut copied = 0u64;
    let mut skipped = 0u64;
    if let Err(e) = std::fs::create_dir_all(dst) {
        eprintln!("gptk copy: cannot create {}: {}", dst.display(), e);
        return (copied, skipped);
    }
    let entries = match std::fs::read_dir(src) {
        Ok(rd) => rd,
        Err(e) => {
            eprintln!("gptk copy: cannot read {}: {}", src.display(), e);
            return (copied, skipped);
        },
    };
    for entry in entries.flatten() {
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        let meta = match std::fs::symlink_metadata(&src_path) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("gptk copy: skip {}: {}", src_path.display(), e);
                skipped += 1;
                continue;
            },
        };
        if meta.file_type().is_symlink() {
            match std::fs::read_link(&src_path) {
                Ok(target) => {
                    let _ = std::fs::remove_file(&dst_path);
                    match std::os::unix::fs::symlink(&target, &dst_path) {
                        Ok(_bytes) => copied += 1,
                        Err(e) => {
                            eprintln!(
                                "gptk copy: skip symlink {} -> {}: {}",
                                src_path.display(),
                                dst_path.display(),
                                e
                            );
                            skipped += 1;
                        },
                    }
                },
                Err(e) => {
                    eprintln!("gptk copy: skip readlink {}: {}", src_path.display(), e);
                    skipped += 1;
                },
            }
        } else if meta.is_dir() {
            let (c, s) = copy_dir_tolerant(&src_path, &dst_path);
            copied += c;
            skipped += s;
        } else {
            match std::fs::copy(&src_path, &dst_path) {
                Ok(_) => copied += 1,
                Err(e) => {
                    eprintln!("gptk copy: skip {} -> {}: {}", src_path.display(), dst_path.display(), e);
                    skipped += 1;
                },
            }
        }
    }
    (copied, skipped)
}

fn reset_gptk_prefix_for_seed(prefix: &Path, seeding_marker: &Path) -> Result<(), Box<dyn std::error::Error>> {
    std::fs::create_dir_all(prefix)?;
    for entry in std::fs::read_dir(prefix)? {
        let entry = entry?;
        let path = entry.path();
        if path == seeding_marker {
            continue;
        }
        let meta = std::fs::symlink_metadata(&path)?;
        if meta.is_dir() && !meta.file_type().is_symlink() {
            std::fs::remove_dir_all(&path)
                .map_err(|e| format!("reset GPTK prefix: remove dir {}: {}", path.display(), e))?;
        } else {
            std::fs::remove_file(&path)
                .map_err(|e| format!("reset GPTK prefix: remove file {}: {}", path.display(), e))?;
        }
    }
    Ok(())
}

fn stage_homebrew_gptk_route_dlls_into_prefix(home: &Path, prefix: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let source_dir = gptk_runtime_pe_dir(home);
    let system32 = prefix.join("drive_c").join("windows").join("system32");
    std::fs::create_dir_all(&system32)?;
    for dll in GPTK_ROUTE_DLLS {
        let source = source_dir.join(dll);
        let dest = system32.join(dll);
        if !file_nonempty(&source) {
            return Err(format!("missing Homebrew GPTK route DLL source: {}", source.display()).into());
        }
        std::fs::copy(&source, &dest)
            .map_err(|e| format!("stage Homebrew GPTK route DLL {} -> {}: {}", source.display(), dest.display(), e))?;
    }
    Ok(())
}

pub fn seed_gptk_prefix_sync(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    match seed_gptk_prefix_sync_inner(home) {
        Ok(()) => Ok(()),
        Err(e) => {
            record_gptk_seed_failure(home, &e.to_string());
            Err(e)
        },
    }
}

fn seed_gptk_prefix_sync_inner(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let gptk_prefix = gptk_prefix_path(home);
    let steam_prefix = metalsharp_home_dir_for(home).join("prefix-steam");
    let gptk_wine64 = gptk_wine64_binary_for_home(home);
    let gptk_wineserver = gptk_wineserver_binary_for_home(home);
    if !gptk_wine64.is_file() {
        return Err("GPTK wine64 not found".into());
    }
    let gptk_winedllpath = gptk_seed_winedllpath(home)?;

    let seeding_marker = gptk_prefix.join(".gptk-seeding");
    let _ = std::fs::create_dir_all(&gptk_prefix);
    let _ = std::fs::remove_file(gptk_prefix.join(".gptk-ready"));
    clear_gptk_seed_failure(home);
    std::fs::write(&seeding_marker, "seeding")?;
    let mut guard = SeedingGuard::new(seeding_marker.clone());
    append_gptk_seed_log(home, &format!("seed started for {}", gptk_prefix.display()));

    let _ = std::process::Command::new(&gptk_wineserver).env("WINEPREFIX", &gptk_prefix).arg("-k").status();
    append_gptk_seed_log(home, "wineserver killed before seed");

    append_gptk_seed_log(home, "resetting GPTK prefix before wineboot");
    reset_gptk_prefix_for_seed(&gptk_prefix, &seeding_marker)?;
    std::fs::write(&seeding_marker, "seeding")?;

    let dyld = gptk_seed_dyld(home);

    append_gptk_seed_log(home, "running wineboot --init");
    let wineboot_cwd = if home.is_dir() { home } else { Path::new("/") };
    let output = std::process::Command::new(&gptk_wine64)
        .current_dir(wineboot_cwd)
        .env("WINEPREFIX", &gptk_prefix)
        .env("WINEARCH", "win64")
        .env("WINEDEBUG", "-all")
        .env("DYLD_FALLBACK_LIBRARY_PATH", &dyld)
        .env("WINEDLLPATH", &gptk_winedllpath)
        .env("WINEDLLOVERRIDES", "d3d10,d3d11,d3d12,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
        .arg("wineboot")
        .arg("--init")
        .output()?;
    if !output.status.success() {
        let output_text = command_output_text(&output);
        return Err(format!(
            "GPTK wineboot --init failed with {:?}{}",
            output.status.code(),
            if output_text.is_empty() { String::new() } else { format!(": {}", output_text) }
        )
        .into());
    }
    append_gptk_seed_log(home, "wineboot --init exited successfully; waiting for wineserver");
    let wait_status =
        std::process::Command::new(&gptk_wineserver).env("WINEPREFIX", &gptk_prefix).arg("-w").status()?;
    if !wait_status.success() {
        return Err(format!("GPTK wineserver -w failed after wineboot with {:?}", wait_status.code()).into());
    }
    append_gptk_seed_log(home, "wineserver idle after wineboot");

    append_gptk_seed_log(home, "staging Homebrew GPTK D3DMetal route DLLs into prefix");
    stage_homebrew_gptk_route_dlls_into_prefix(home, &gptk_prefix)?;

    let missing_wineboot_paths = missing_gptk_wineboot_paths(&gptk_prefix);
    if !missing_wineboot_paths.is_empty() {
        return Err(format!(
            "gptk: wineboot --init succeeded but prefix is incomplete (missing: {})",
            missing_wineboot_paths.join(", ")
        )
        .into());
    }

    let drive_c = gptk_prefix.join("drive_c");
    if !steam_prefix.join("drive_c").join("Program Files (x86)").join("Steam").exists() {
        return Err("MetalSharp Wine Steam not installed — install Steam first".into());
    }

    append_gptk_seed_log(home, "copying Steam directory");
    let steam_src = steam_prefix.join("drive_c").join("Program Files (x86)").join("Steam");
    let steam_dst = drive_c.join("Program Files (x86)").join("Steam");
    std::fs::create_dir_all(steam_dst.parent().unwrap())?;
    fast_copy_dir(&steam_src, &steam_dst)?;
    append_gptk_seed_log(home, "Steam copy complete");

    append_gptk_seed_log(home, "copying users directory");
    let users_src = steam_prefix.join("drive_c").join("users");
    let users_dst = drive_c.join("users");
    if users_src.is_dir() {
        if users_dst.is_dir() {
            let _ = std::fs::remove_dir_all(&users_dst);
        }
        fast_copy_dir(&users_src, &users_dst)?;
        append_gptk_seed_log(home, "users copy complete");
    } else {
        append_gptk_seed_log(home, "no users directory to copy");
    }

    append_gptk_seed_log(home, "syncing GPTK dosdevices");
    ensure_gptk_dosdevices(home)?;

    let steam_exe_check = steam_dst.join("Steam.exe");
    if !steam_exe_check.is_file() {
        return Err(format!("gptk: Steam.exe missing after copy — expected at {}", steam_exe_check.display()).into());
    }

    copy_registry_hive(&steam_prefix, &gptk_prefix, "system.reg");
    copy_registry_hive(&steam_prefix, &gptk_prefix, "user.reg");
    copy_registry_hive(&steam_prefix, &gptk_prefix, "userdef.reg");

    let _ = std::process::Command::new(&gptk_wineserver).env("WINEPREFIX", &gptk_prefix).arg("-k").status();

    append_gptk_seed_log(home, "installing GPTK prefix components");
    install_gptk_prefix_components(home)?;
    append_gptk_seed_log(home, "GPTK prefix components verified");

    std::fs::write(gptk_prefix.join(".gptk-ready"), "ready")?;
    guard.disarm();
    let _ = std::fs::remove_file(&seeding_marker);
    clear_gptk_seed_failure(home);
    append_gptk_seed_log(home, "prefix ready");

    Ok(())
}

pub fn sync_gptk_prefix(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let gptk_prefix = gptk_prefix_path(home);
    let steam_prefix = metalsharp_home_dir_for(home).join("prefix-steam");

    let steam_config_src = steam_prefix.join("drive_c").join("Program Files (x86)").join("Steam").join("config");
    let steam_config_dst = gptk_prefix.join("drive_c").join("Program Files (x86)").join("Steam").join("config");

    if steam_config_src.is_dir() {
        for file in &["loginusers.vdf", "config.vdf", "libraryfolders.vdf"] {
            let src = steam_config_src.join(file);
            let dst = steam_config_dst.join(file);
            if src.exists() {
                std::fs::copy(&src, &dst)
                    .map_err(|e| format!("gptk sync: copy {} -> {}: {}", src.display(), dst.display(), e))?;
            }
        }
    }

    copy_registry_hive(&steam_prefix, &gptk_prefix, "user.reg");

    ensure_gptk_dosdevices(home)?;

    Ok(())
}

fn copy_dir_recursive(src: &Path, dst: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        let meta = std::fs::symlink_metadata(&src_path)?;
        if meta.file_type().is_symlink() {
            let target = std::fs::read_link(&src_path)?;
            let _ = std::fs::remove_file(&dst_path);
            std::os::unix::fs::symlink(&target, &dst_path)?;
        } else if meta.is_dir() {
            copy_dir_recursive(&src_path, &dst_path)?;
        } else {
            std::fs::copy(&src_path, &dst_path).map_err(|e| {
                std::io::Error::new(e.kind(), format!("copy {} -> {}: {}", src_path.display(), dst_path.display(), e))
            })?;
        }
    }
    Ok(())
}

fn verify_dir_copy(src: &Path, dst: &Path) -> Result<(), String> {
    let src_count = count_dir_entries(src).map_err(|e| format!("stat src {}: {}", src.display(), e))?;
    let dst_count = count_dir_entries(dst).map_err(|e| format!("stat dst {}: {}", dst.display(), e))?;
    if src_count != dst_count {
        return Err(format!(
            "copy verification failed: src {} has {} entries, dst {} has {} entries",
            src.display(),
            src_count,
            dst.display(),
            dst_count
        ));
    }
    Ok(())
}

fn count_dir_entries(path: &Path) -> std::io::Result<u64> {
    let mut count: u64 = 0;
    for entry in std::fs::read_dir(path)? {
        let _ = entry?;
        count += 1;
    }
    Ok(count)
}

pub fn gptk_vcrun_installed(home: &Path) -> bool {
    let gptk_prefix = gptk_prefix_path(home);
    gptk_vcrun_x64_installed(&gptk_prefix) && gptk_vcrun_x86_installed(&gptk_prefix)
}

fn gptk_vcrun_x64_installed(gptk_prefix: &Path) -> bool {
    let system32 = gptk_prefix.join("drive_c").join("windows").join("system32");
    has_runtime_dll(&system32, "vcruntime140.dll")
        && has_runtime_dll(&system32, "vcruntime140_1.dll")
        && has_runtime_dll(&system32, "msvcp140.dll")
}

fn gptk_vcrun_x86_installed(gptk_prefix: &Path) -> bool {
    let syswow64 = gptk_prefix.join("drive_c").join("windows").join("syswow64");
    has_runtime_dll(&syswow64, "vcruntime140.dll") && has_runtime_dll(&syswow64, "msvcp140.dll")
}

fn has_runtime_dll(dir: &Path, dll: &str) -> bool {
    let p = dir.join(dll);
    p.is_file() && p.metadata().map(|m| m.len()).unwrap_or(0) > 10_000
}

fn valid_gptk_vcrun_redist(path: &Path) -> bool {
    path.is_file() && path.metadata().map(|m| m.len()).unwrap_or(0) > GPTK_VCRUN_MIN_SIZE
}

pub fn install_gptk_prefix_components(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let gptk_prefix = gptk_prefix_path(home);
    if !gptk_prefix.join("drive_c").is_dir() {
        return Err("GPTK prefix has no drive_c — run seed first".into());
    }

    if gptk_vcrun_installed(home) {
        append_gptk_seed_log(home, "vcrun already installed in GPTK prefix (x64+x86)");
        return Ok(());
    }

    let gptk_wine64 = gptk_wine64_binary_for_home(home);
    let gptk_wineserver = gptk_wineserver_binary_for_home(home);
    let gptk_winedllpath = gptk_seed_winedllpath(home)?;
    let dyld = gptk_seed_dyld(home);

    let redist_dir = metalsharp_home_dir_for(home).join("runtime").join("redist").join("vcredist");
    let _ = std::fs::create_dir_all(&redist_dir);

    append_gptk_seed_log(home, "checking VC++ redist payloads before GPTK install");
    let x64 = resolve_or_download_vcrun(home, &redist_dir, "x64")?;
    let x86 = resolve_or_download_vcrun(home, &redist_dir, "x86")?;

    for (arch, path, already_installed) in
        [("x64", &x64, gptk_vcrun_x64_installed(&gptk_prefix)), ("x86", &x86, gptk_vcrun_x86_installed(&gptk_prefix))]
    {
        if already_installed {
            append_gptk_seed_log(home, &format!("VC++ {} runtime already present, skipping installer", arch));
            continue;
        }

        append_gptk_seed_log(
            home,
            &format!("launching interactive VC++ {} redist /install from {}", arch, path.display()),
        );
        let _ = std::process::Command::new(&gptk_wineserver).env("WINEPREFIX", &gptk_prefix).arg("-p").status();
        let status = std::process::Command::new(&gptk_wine64)
            .current_dir(if home.is_dir() { home } else { Path::new("/") })
            .env("WINEPREFIX", &gptk_prefix)
            .env("WINEARCH", "win64")
            .env("WINEDEBUG", "-all")
            .env("DYLD_FALLBACK_LIBRARY_PATH", &dyld)
            .env("WINEDLLPATH", &gptk_winedllpath)
            .env("WINEDLLOVERRIDES", "d3d10,d3d11,d3d12,dxgi=n,b;gameoverlayrenderer,gameoverlayrenderer64=d")
            .stdin(std::process::Stdio::inherit())
            .stdout(std::process::Stdio::inherit())
            .stderr(std::process::Stdio::inherit())
            .arg("start")
            .arg("/wait")
            .arg("/unix")
            .arg(path)
            .arg("/install")
            .status()?;
        let arch_installed = wait_for_gptk_vcrun_arch(&gptk_prefix, arch, Duration::from_secs(45));
        let _ = std::process::Command::new(&gptk_wineserver).env("WINEPREFIX", &gptk_prefix).arg("-k").status();
        if !status.success() {
            append_gptk_seed_log(
                home,
                &format!(
                    "VC++ {} interactive installer exited with {:?}, installed={}",
                    arch,
                    status.code(),
                    arch_installed
                ),
            );
        }
    }

    let installed = gptk_vcrun_installed(home);
    append_gptk_seed_log(home, &format!("VC++ redist install complete, installed={}", installed));
    if !installed {
        return Err("GPTK VC++ redist install did not verify x64+x86 runtime DLLs".into());
    }
    Ok(())
}

fn wait_for_gptk_vcrun_arch(gptk_prefix: &Path, arch: &str, timeout: Duration) -> bool {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        let installed =
            if arch == "x64" { gptk_vcrun_x64_installed(gptk_prefix) } else { gptk_vcrun_x86_installed(gptk_prefix) };
        if installed || std::time::Instant::now() >= deadline {
            return installed;
        }
        std::thread::sleep(Duration::from_millis(500));
    }
}

fn resolve_or_download_vcrun(
    home: &Path,
    redist_dir: &Path,
    arch: &str,
) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let filename = format!("vc_redist.{}.exe", arch);
    let staged = redist_dir.join(&filename);
    if valid_gptk_vcrun_redist(&staged) {
        append_gptk_seed_log(home, &format!("using staged {} redist", arch));
        return Ok(staged);
    }

    let steam_redist = metalsharp_home_dir_for(home)
        .join("prefix-steam")
        .join("drive_c")
        .join("Program Files (x86)")
        .join("Steam")
        .join("steamapps")
        .join("common")
        .join("Steamworks Shared")
        .join("_CommonRedist");

    for ver in &["2022", "2019", "2017", "2015"] {
        let candidate = steam_redist.join("vcredist").join(ver).join(&filename);
        if valid_gptk_vcrun_redist(&candidate) {
            append_gptk_seed_log(home, &format!("found {} redist in Steam CommonRedist/{}", arch, ver));
            return Ok(candidate);
        }
    }

    append_gptk_seed_log(home, &format!("downloading VC++ {} redist from Microsoft", arch));
    let url = format!("https://aka.ms/vs/17/release/{}", filename);
    let tmp = staged.with_extension("download");
    let config =
        ureq::config::Config::builder().user_agent(format!("MetalSharp/{}", env!("CARGO_PKG_VERSION"))).build();
    let agent = ureq::Agent::new_with_config(config);
    let resp = agent.get(&url).call().map_err(|e| format!("VC++ {} download failed: {}", arch, e))?;
    let mut input = resp.into_body().into_reader();
    let mut output = std::fs::File::create(&tmp)?;
    std::io::copy(&mut input, &mut output)?;
    drop(output);
    if !valid_gptk_vcrun_redist(&tmp) {
        let _ = std::fs::remove_file(&tmp);
        return Err(format!("VC++ {} download was missing or too small", arch).into());
    }
    std::fs::rename(&tmp, &staged)?;
    append_gptk_seed_log(home, &format!("downloaded {} redist to {}", arch, staged.display()));
    Ok(staged)
}

fn copy_registry_hive(src_prefix: &Path, dst_prefix: &Path, hive: &str) {
    let src = src_prefix.join(hive);
    let dst = dst_prefix.join(hive);
    if src.exists() {
        if let Err(e) = std::fs::copy(&src, &dst) {
            eprintln!("gptk: failed to copy {} -> {}: {}", src.display(), dst.display(), e);
        }
    }
}

pub fn gptk_wine64_binary_for_home(home: &Path) -> PathBuf {
    gptk_wine_root_for_home(home).join("bin").join("wine64")
}

pub fn gptk_wineserver_binary_for_home(home: &Path) -> PathBuf {
    gptk_wine_root_for_home(home).join("bin").join("wineserver")
}

pub fn gptk_wine64_binary() -> PathBuf {
    gptk_wine_root().join("bin").join("wine64")
}

pub fn gptk_wineserver_binary() -> PathBuf {
    gptk_wine_root().join("bin").join("wineserver")
}

pub fn gptk_homebrew_installed() -> bool {
    gptk_homebrew_wine_root().join("bin").join("wine64").is_file()
        && gptk_homebrew_wine_root().join("bin").join("wineserver").is_file()
}

pub fn gptk_is_installed_for_home(home: &Path) -> bool {
    gptk_wine64_binary_for_home(home).is_file() && gptk_wineserver_binary_for_home(home).is_file()
}

pub fn gptk_is_installed() -> bool {
    dirs::home_dir().map(|home| gptk_is_installed_for_home(&home)).unwrap_or_else(gptk_homebrew_installed)
}

pub fn rosetta_is_installed() -> bool {
    std::path::Path::new("/Library/Apple/usr/lib/libRosettaAOT.dylib").exists()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::Path;

    #[test]
    fn macos_resources_dir_accepts_backend_under_resources_runtime() {
        let exe = Path::new("/Applications/MetalSharp.app/Contents/Resources/runtime/metalsharp-backend");

        assert_eq!(
            macos_resources_dir_for_exe(exe),
            Some(PathBuf::from("/Applications/MetalSharp.app/Contents/Resources"))
        );
    }

    #[test]
    fn macos_resources_dir_accepts_app_binary_under_macos() {
        let exe = Path::new("/Applications/MetalSharp.app/Contents/MacOS/MetalSharp");

        assert_eq!(
            macos_resources_dir_for_exe(exe),
            Some(PathBuf::from("/Applications/MetalSharp.app/Contents/Resources"))
        );
    }

    fn test_prefix(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("metalsharp-platform-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create temp prefix");
        dir
    }

    fn create_gptk_wineboot_runtime(prefix: &Path) {
        let system32 = prefix.join("drive_c").join("windows").join("system32");
        fs::create_dir_all(&system32).expect("create system32");
        fs::create_dir_all(prefix.join("dosdevices")).expect("create dosdevices");
        fs::write(system32.join("kernel32.dll"), "kernel32").expect("write kernel32");
        fs::write(system32.join("ntdll.dll"), "ntdll").expect("write ntdll");
        for dll in GPTK_ROUTE_DLLS {
            fs::write(system32.join(dll), format!("gptk-{dll}")).expect("write gptk dll");
        }
        std::os::unix::fs::symlink("../drive_c", prefix.join("dosdevices").join("c:")).expect("link c drive");
    }

    fn create_gptk_4_runtime_sources(home: &Path) {
        let pe_dir =
            metalsharp_home_dir_for(home).join("runtime").join("wine").join("lib").join("gptk").join("x86_64-windows");
        fs::create_dir_all(&pe_dir).expect("create gptk pe dir");
        for dll in GPTK_ROUTE_DLLS {
            fs::write(pe_dir.join(dll), format!("source-{dll}")).expect("write gptk source dll");
        }
    }

    #[test]
    fn gptk_seeding_marker_is_active_before_drive_c_exists() {
        let prefix = test_prefix("gptk-marker-before-drive-c");
        fs::write(prefix.join(".gptk-seeding"), "seeding").expect("write marker");
        let failed = prefix.join(".gptk-seed-failed");

        assert_eq!(gptk_prefix_status_for_prefix(&prefix, &failed), GptkPrefixStatus::Seeding);
        assert!(prefix.join(".gptk-seeding").exists());

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_partial_prefix_reports_missing_drive_c() {
        let prefix = test_prefix("gptk-partial-no-drive-c");
        fs::create_dir_all(prefix.join("dosdevices")).expect("create dosdevices");
        fs::write(prefix.join("system.reg"), "registry").expect("write reg");
        let failed = prefix.join(".gptk-seed-failed");

        assert_eq!(
            gptk_prefix_status_for_prefix(&prefix, &failed),
            GptkPrefixStatus::Partial("GPTK prefix is incomplete: missing drive_c".to_string())
        );

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_partial_prefix_reports_missing_wineboot_runtime_files() {
        let prefix = test_prefix("gptk-partial-no-kernel32");
        let steam_dir = prefix.join("drive_c").join("Program Files (x86)").join("Steam");
        fs::create_dir_all(&steam_dir).expect("create steam dir");
        fs::create_dir_all(prefix.join("drive_c").join("windows").join("system32")).expect("create system32");
        fs::create_dir_all(prefix.join("dosdevices")).expect("create dosdevices");
        fs::write(steam_dir.join("Steam.exe"), "steam").expect("write steam exe");
        fs::write(prefix.join(".gptk-ready"), "ready").expect("write ready");
        let failed = prefix.join(".gptk-seed-failed");

        let status = gptk_prefix_status_for_prefix(&prefix, &failed);
        let GptkPrefixStatus::Partial(detail) = status else {
            panic!("expected partial status, got {:?}", status);
        };
        assert!(detail.contains("drive_c/windows/system32/kernel32.dll"));
        assert!(detail.contains("drive_c/windows/system32/ntdll.dll"));
        assert!(detail.contains("drive_c/windows/system32/d3d12.dll"));
        assert!(detail.contains("drive_c/windows/system32/dxgi.dll"));
        assert!(detail.contains("dosdevices/c:"));

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn reset_gptk_prefix_for_seed_removes_stale_wineboot_state() {
        let prefix = test_prefix("gptk-reset-prefix");
        create_gptk_wineboot_runtime(&prefix);
        fs::write(prefix.join("system.reg"), "stale registry").expect("write system reg");
        fs::write(prefix.join("user.reg"), "stale user reg").expect("write user reg");
        fs::write(prefix.join(".gptk-ready"), "ready").expect("write ready");
        let marker = prefix.join(".gptk-seeding");
        fs::write(&marker, "seeding").expect("write marker");

        reset_gptk_prefix_for_seed(&prefix, &marker).expect("reset prefix");

        assert!(marker.is_file());
        assert!(!prefix.join("drive_c").exists());
        assert!(!prefix.join("dosdevices").exists());
        assert!(!prefix.join("system.reg").exists());
        assert!(!prefix.join("user.reg").exists());
        assert!(!prefix.join(".gptk-ready").exists());

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_seed_winedllpath_prefers_gptk_4_route_dlls() {
        let home = test_prefix("gptk-winedllpath-home");
        create_gptk_4_runtime_sources(&home);
        let metalsharp_pe = metalsharp_home_dir_for(&home)
            .join("runtime")
            .join("wine")
            .join("lib")
            .join("metalsharp")
            .join("x86_64-windows");
        fs::create_dir_all(&metalsharp_pe).expect("create metalsharp pe dir");

        let winedllpath = gptk_seed_winedllpath(&home).expect("build winedllpath");
        let parts: Vec<&str> = winedllpath.split(':').collect();

        assert_eq!(parts[0], gptk_runtime_pe_dir(&home).to_string_lossy());
        assert_eq!(parts[1], metalsharp_pe.to_string_lossy());

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn stage_homebrew_gptk_route_dlls_into_prefix_copies_route_dlls_to_system32() {
        let home = test_prefix("gptk-stage-home");
        let prefix = test_prefix("gptk-stage-prefix");
        fs::create_dir_all(prefix.join("drive_c").join("windows").join("system32")).expect("create system32");
        create_gptk_4_runtime_sources(&home);

        stage_homebrew_gptk_route_dlls_into_prefix(&home, &prefix).expect("stage gptk dlls");

        let system32 = prefix.join("drive_c").join("windows").join("system32");
        for dll in GPTK_ROUTE_DLLS {
            assert_eq!(fs::read_to_string(system32.join(dll)).unwrap(), format!("source-{dll}"));
        }

        let _ = fs::remove_dir_all(home);
        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_vcrun_x64_requires_vcruntime140_1() {
        let prefix = test_prefix("gptk-vcrun-x64");
        let system32 = prefix.join("drive_c").join("windows").join("system32");
        fs::create_dir_all(&system32).expect("create system32");
        let dll_payload = vec![0u8; 20_000];
        fs::write(system32.join("vcruntime140.dll"), &dll_payload).expect("write vcruntime");
        fs::write(system32.join("msvcp140.dll"), &dll_payload).expect("write msvcp");

        assert!(!gptk_vcrun_x64_installed(&prefix));

        fs::write(system32.join("vcruntime140_1.dll"), &dll_payload).expect("write vcruntime140_1");

        assert!(gptk_vcrun_x64_installed(&prefix));

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_vcrun_installed_requires_x64_and_x86_routes() {
        let home = test_prefix("gptk-vcrun-home");
        let prefix = gptk_prefix_path(&home);
        let system32 = prefix.join("drive_c").join("windows").join("system32");
        let syswow64 = prefix.join("drive_c").join("windows").join("syswow64");
        fs::create_dir_all(&system32).expect("create system32");
        fs::create_dir_all(&syswow64).expect("create syswow64");
        let dll_payload = vec![0u8; 20_000];
        fs::write(system32.join("vcruntime140.dll"), &dll_payload).expect("write x64 vcruntime");
        fs::write(system32.join("vcruntime140_1.dll"), &dll_payload).expect("write x64 vcruntime140_1");
        fs::write(system32.join("msvcp140.dll"), &dll_payload).expect("write x64 msvcp");

        assert!(!gptk_vcrun_installed(&home));

        fs::write(syswow64.join("vcruntime140.dll"), &dll_payload).expect("write x86 vcruntime");
        fs::write(syswow64.join("msvcp140.dll"), &dll_payload).expect("write x86 msvcp");

        assert!(gptk_vcrun_installed(&home));

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn gptk_vcrun_redist_validation_rejects_tiny_files() {
        let prefix = test_prefix("gptk-vcrun-redist");
        let tiny = prefix.join("vc_redist.x64.exe");
        fs::write(&tiny, vec![0u8; 100_000]).expect("write tiny redist");

        assert!(!valid_gptk_vcrun_redist(&tiny));

        fs::write(&tiny, vec![0u8; (GPTK_VCRUN_MIN_SIZE + 1) as usize]).expect("write valid redist");

        assert!(valid_gptk_vcrun_redist(&tiny));

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_ready_status_clears_transient_markers() {
        let prefix = test_prefix("gptk-ready-clears-markers");
        let steam_dir = prefix.join("drive_c").join("Program Files (x86)").join("Steam");
        fs::create_dir_all(&steam_dir).expect("create steam dir");
        create_gptk_wineboot_runtime(&prefix);
        fs::write(steam_dir.join("Steam.exe"), "steam").expect("write steam exe");
        fs::write(prefix.join(".gptk-ready"), "ready").expect("write ready");
        fs::write(prefix.join(".gptk-seeding"), "seeding").expect("write seeding");
        let failed = prefix.join(".gptk-seed-failed");
        fs::write(&failed, "old failure").expect("write failed");

        assert_eq!(gptk_prefix_status_for_prefix(&prefix, &failed), GptkPrefixStatus::Ready);
        assert!(!prefix.join(".gptk-seeding").exists());
        assert!(!failed.exists());

        let _ = fs::remove_dir_all(prefix);
    }

    #[test]
    fn gptk_ready_status_accepts_offline_disabled_steam_launcher() {
        let home = test_prefix("gptk-ready-disabled-steam-home");
        let prefix = gptk_prefix_path(&home);
        let steam_dir = prefix.join("drive_c").join("Program Files (x86)").join("Steam");
        fs::create_dir_all(&steam_dir).expect("create steam dir");
        create_gptk_wineboot_runtime(&prefix);
        fs::write(gptk_disabled_steam_exe(&prefix), "steam disabled").expect("write disabled steam exe");
        fs::write(prefix.join(".gptk-ready"), "ready").expect("write ready");
        let failed = prefix.join(".gptk-seed-failed");

        assert!(gptk_prefix_ready(&home));
        assert_eq!(gptk_prefix_status_for_prefix(&prefix, &failed), GptkPrefixStatus::Ready);

        let _ = fs::remove_dir_all(home);
    }
}
