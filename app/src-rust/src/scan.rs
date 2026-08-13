use serde_json::{json, Value};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use walkdir::WalkDir;

#[derive(serde::Serialize, Clone)]
pub struct Game {
    pub id: String,
    pub name: String,
    pub exe_path: String,
    pub platform: String,
    pub steam_app_id: Option<u32>,
    pub size_bytes: Option<u64>,
    pub metalsharp_compatible: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NonSteamShortcut {
    pub name: String,
    pub exe_path: PathBuf,
    pub start_dir: Option<PathBuf>,
    pub launch_args: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct DualGameDir {
    pub appid: u32,
    pub macos_dir: Option<PathBuf>,
    pub wine_dir: Option<PathBuf>,
    pub macos_app: Option<PathBuf>,
    pub has_native_build: bool,
}

pub fn macos_steam_library_paths() -> Vec<PathBuf> {
    let home = dirs::home_dir().unwrap_or_default();
    let candidates = vec![
        home.join("Library/Application Support/Steam/steamapps"),
        home.join(".steam/steam/steamapps"),
        home.join(".local/share/Steam/steamapps"),
    ];
    let mut paths = Vec::new();
    for mac_path in &candidates {
        if mac_path.exists() {
            push_unique_steamapps_path(&mut paths, mac_path.clone());
            for library_path in parse_library_folders(mac_path) {
                push_unique_steamapps_path(&mut paths, library_path);
            }
        }
    }
    paths
}

/// Return every Steam library visible from the native and Wine Steam
/// configurations. Native macOS Steam's `libraryfolders.vdf` is the source of
/// truth for external volumes, while Wine Steam can maintain a separate list
/// of Windows game libraries. Keep both sets so an install on either internal
/// or external storage is visible to the library and watcher paths.
pub fn steam_library_paths() -> Vec<PathBuf> {
    let mut paths = macos_steam_library_paths();
    for library_path in wine_steam_library_paths() {
        push_unique_steamapps_path(&mut paths, library_path);
    }
    paths
}

fn push_unique_steamapps_path(paths: &mut Vec<PathBuf>, path: PathBuf) {
    if paths.iter().any(|existing| existing == &path) {
        return;
    }
    paths.push(path);
}

pub fn resolve_dual_game_dir(appid: u32) -> DualGameDir {
    let manifest_name = format!("appmanifest_{}.acf", appid);
    let mut macos_dir: Option<PathBuf> = None;
    let mut wine_dir: Option<PathBuf> = None;
    let mut install_dir_names: Vec<String> = Vec::new();

    for steamapps in macos_steam_library_paths() {
        let manifest_path = steamapps.join(&manifest_name);
        if let Ok(contents) = std::fs::read_to_string(&manifest_path) {
            if let Some(dir_name) = parse_installdir_from_acf(&contents) {
                let dir = steamapps.join("common").join(&dir_name);
                if dir.exists() {
                    classify_dual_game_dir(&dir, &mut macos_dir, &mut wine_dir);
                    push_unique_install_dir(&mut install_dir_names, dir_name);
                }
            }
        }
    }

    for steamapps in wine_steam_library_paths() {
        let mut found_from_known_dir = false;
        for dir_name in &install_dir_names {
            let dir = steamapps.join("common").join(dir_name);
            if dir.exists() {
                classify_dual_game_dir(&dir, &mut macos_dir, &mut wine_dir);
                found_from_known_dir = true;
            }
        }
        if !found_from_known_dir {
            let manifest_path = steamapps.join(&manifest_name);
            if let Ok(contents) = std::fs::read_to_string(&manifest_path) {
                if let Some(dir_name) = parse_installdir_from_acf(&contents) {
                    let dir = steamapps.join("common").join(&dir_name);
                    if dir.exists() {
                        classify_dual_game_dir(&dir, &mut macos_dir, &mut wine_dir);
                        push_unique_install_dir(&mut install_dir_names, dir_name);
                    }
                }
            }
        }
    }

    let macos_app = macos_dir.as_ref().and_then(|d| find_macos_app(d));
    let has_native_build = macos_app.is_some();

    DualGameDir { appid, macos_dir, wine_dir, macos_app, has_native_build }
}

fn push_unique_install_dir(names: &mut Vec<String>, name: String) {
    if !names.iter().any(|existing| existing.eq_ignore_ascii_case(&name)) {
        names.push(name);
    }
}

fn classify_dual_game_dir(dir: &Path, macos_dir: &mut Option<PathBuf>, wine_dir: &mut Option<PathBuf>) {
    if is_windows_game_dir(dir) && wine_dir.is_none() {
        *wine_dir = Some(dir.to_path_buf());
    }
    if find_macos_app(dir).is_some() && macos_dir.is_none() {
        *macos_dir = Some(dir.to_path_buf());
    }
    if wine_dir.is_none() && macos_dir.is_none() {
        *macos_dir = Some(dir.to_path_buf());
    }
}

pub fn is_windows_game_dir(dir: &Path) -> bool {
    find_windows_exe_in_dir(dir, 5).is_some()
}

pub fn find_windows_exe_in_dir(dir: &Path, max_depth: usize) -> Option<PathBuf> {
    for entry in WalkDir::new(dir).max_depth(max_depth).into_iter().flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        if path.extension().map(|ext| ext.to_string_lossy().eq_ignore_ascii_case("exe")) != Some(true) {
            continue;
        }
        let name = entry.file_name().to_string_lossy();
        if is_valid_game_exe(&name) {
            return Some(path.to_path_buf());
        }
    }
    None
}

pub fn find_macos_app(dir: &Path) -> Option<PathBuf> {
    for entry in std::fs::read_dir(dir).ok()? {
        let entry = entry.ok()?;
        let path = entry.path();
        if path.extension().map(|e| e == "app").unwrap_or(false) && path.is_dir() {
            return Some(path);
        }
    }
    for entry in WalkDir::new(dir).max_depth(2).into_iter().flatten() {
        let path = entry.path();
        if path.extension().map(|e| e == "app").unwrap_or(false) && path.is_dir() {
            return Some(path.to_path_buf());
        }
    }
    None
}

fn parse_installdir_from_acf(contents: &str) -> Option<String> {
    for line in contents.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with("\"installdir\"") {
            let parts: Vec<&str> = trimmed.splitn(2, ['\t', ' ']).collect();
            return parts.last().map(|s| s.trim().trim_matches('"').to_string());
        }
    }
    None
}

pub fn scan_all() -> Result<Value, Box<dyn std::error::Error>> {
    let mut games: Vec<Game> = Vec::new();

    if let Some(steam) = detect_windows_steam() {
        games.push(steam);
    }

    if let Ok(steam_games) = scan_steam_library() {
        games.extend(steam_games);
    }

    if let Ok(local_games) = scan_local_exes() {
        games.extend(local_games);
    }

    for shortcut in scan_non_steam_shortcuts() {
        games.push(Game {
            id: format!("non_steam_{}", shortcut_id(&shortcut.name, &shortcut.exe_path)),
            name: shortcut.name,
            exe_path: shortcut.exe_path.to_string_lossy().to_string(),
            platform: "non_steam".into(),
            steam_app_id: None,
            size_bytes: shortcut.start_dir.as_ref().and_then(dir_size),
            metalsharp_compatible: true,
        });
    }

    let steam_status = super::steam::status();
    Ok(json!({
        "ok": true,
        "data": {
            "games": games,
            "steam": steam_status
        }
    }))
}

fn detect_windows_steam() -> Option<Game> {
    let home = dirs::home_dir()?;
    let steam_exe = home
        .join(".metalsharp")
        .join("prefix-steam")
        .join("drive_c")
        .join("Program Files (x86)")
        .join("Steam")
        .join("steam.exe");

    if !steam_exe.exists() {
        return None;
    }

    Some(Game {
        id: "windows_steam".into(),
        name: "Steam (Windows)".into(),
        exe_path: steam_exe.to_string_lossy().to_string(),
        platform: "steam".into(),
        steam_app_id: None,
        size_bytes: dir_size(&steam_exe.parent()?.to_path_buf()),
        metalsharp_compatible: true,
    })
}

fn parse_library_folders(steamapps: &PathBuf) -> Vec<PathBuf> {
    let lf_path = steamapps.join("libraryfolders.vdf");
    let contents = match std::fs::read_to_string(&lf_path) {
        Ok(c) => c,
        Err(_) => return Vec::new(),
    };

    let mut extra = Vec::new();
    for line in contents.lines() {
        let trimmed = line.trim();
        if let Some(val) = parse_vdf_path(trimmed, "path") {
            let sa = PathBuf::from(val).join("steamapps");
            if sa.exists() && sa != *steamapps {
                extra.push(sa);
            }
        }
    }
    extra
}

fn parse_vdf_path(line: &str, key: &str) -> Option<String> {
    let prefix = format!("\"{}\"", key);
    if !line.starts_with(&prefix) {
        return None;
    }
    let rest = line.trim_start_matches(&prefix).trim();
    let rest = rest.trim_start_matches('\t').trim_start_matches(' ');
    let val = rest.trim_matches('"');
    if val.is_empty() {
        return None;
    }
    let unix_path = val.replace('\\', "/");
    Some(resolve_wine_path(&unix_path))
}

pub fn resolve_wine_path(path: &str) -> String {
    let p = path.replace('\\', "/");
    let resolved = if let Some(rest) = p.strip_prefix("Z:/").or_else(|| p.strip_prefix("z:/")) {
        rest.to_string()
    } else if let Some(rest) = p.strip_prefix("Z:").or_else(|| p.strip_prefix("z:")) {
        rest.to_string()
    } else if let Some(resolved) = resolve_wine_drive_letter(&p) {
        resolved
    } else if let Some(rest) = p.strip_prefix("C:/").or_else(|| p.strip_prefix("c:/")) {
        format!("/{}", rest)
    } else if p.eq_ignore_ascii_case("C:") || p.eq_ignore_ascii_case("C:/") {
        "/".to_string()
    } else {
        p
    };
    // Normalize doubled separators: a libraryfolders.vdf written with
    // over-escaped backslashes (Z:\\Volumes\\AverySSD\\SteamLibrary) resolves
    // to "/Volumes//AverySSD//SteamLibrary". Collapse repeated slashes so
    // bottle manifests carry canonical paths.
    collapse_duplicate_slashes(&resolved)
}

fn collapse_duplicate_slashes(path: &str) -> String {
    let mut out = String::with_capacity(path.len());
    let mut prev_slash = false;
    for ch in path.chars() {
        if ch == '/' {
            if prev_slash {
                continue;
            }
            prev_slash = true;
        } else {
            prev_slash = false;
        }
        out.push(ch);
    }
    out
}

fn resolve_wine_drive_letter(path: &str) -> Option<String> {
    let mut chars = path.chars();
    let drive = chars.next()?;
    if !drive.is_ascii_alphabetic() || drive.eq_ignore_ascii_case(&'c') || drive.eq_ignore_ascii_case(&'z') {
        return None;
    }
    chars.next().filter(|&c| c == ':')?;
    let rest = &path[2..];

    let home = dirs::home_dir()?;
    let dosdevice = home
        .join(".metalsharp")
        .join("prefix-steam")
        .join("dosdevices")
        .join(format!("{}:", drive.to_ascii_lowercase()));
    let target = std::fs::read_link(&dosdevice).ok()?;

    if rest.is_empty() {
        return Some(target.to_string_lossy().to_string());
    }
    let rest_trimmed = rest.trim_start_matches('/');
    Some(format!("{}/{}", target.display(), rest_trimmed))
}

pub fn wine_steam_library_paths() -> Vec<PathBuf> {
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return Vec::new(),
    };

    let wine_steamapps = home
        .join(".metalsharp")
        .join("prefix-steam")
        .join("drive_c")
        .join("Program Files (x86)")
        .join("Steam")
        .join("steamapps");

    if !wine_steamapps.exists() {
        return Vec::new();
    }

    let mut paths = vec![wine_steamapps.clone()];
    paths.extend(parse_library_folders(&wine_steamapps));
    paths
}

pub fn scan_non_steam_shortcuts() -> Vec<NonSteamShortcut> {
    let mut shortcuts = Vec::new();
    let mut seen = HashSet::new();

    for path in shortcut_vdf_paths() {
        let Ok(data) = std::fs::read(&path) else {
            continue;
        };
        for shortcut in parse_shortcuts_vdf(&data) {
            let key = shortcut.exe_path.to_string_lossy().to_ascii_lowercase();
            if shortcut.exe_path.exists() && seen.insert(key) {
                shortcuts.push(shortcut);
            }
        }
    }

    shortcuts
}

fn shortcut_vdf_paths() -> Vec<PathBuf> {
    let home = dirs::home_dir().unwrap_or_default();
    let mut roots = vec![
        home.join("Library/Application Support/Steam/userdata"),
        home.join(".steam/steam/userdata"),
        home.join(".local/share/Steam/userdata"),
        crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam")
            .join("drive_c")
            .join("Program Files (x86)")
            .join("Steam")
            .join("userdata"),
    ];

    roots.retain(|root| root.exists());

    let mut paths = Vec::new();
    for root in roots {
        let Ok(users) = std::fs::read_dir(root) else {
            continue;
        };
        for user in users.flatten() {
            let path = user.path().join("config").join("shortcuts.vdf");
            if path.exists() {
                paths.push(path);
            }
        }
    }
    paths
}

fn parse_shortcuts_vdf(data: &[u8]) -> Vec<NonSteamShortcut> {
    let mut offset = 0usize;
    let mut current_name: Option<String> = None;
    let mut current_exe: Option<String> = None;
    let mut current_start_dir: Option<String> = None;
    let mut current_launch_options: Option<String> = None;
    let mut shortcut_depth: Option<usize> = None;
    let mut depth = 0usize;
    let mut shortcuts = Vec::new();

    while offset < data.len() {
        let field_type = data[offset];
        offset += 1;

        if field_type == 0x08 {
            if shortcut_depth == Some(depth) {
                if let Some(shortcut) = build_non_steam_shortcut(
                    current_name.take(),
                    current_exe.take(),
                    current_start_dir.take(),
                    current_launch_options.take(),
                ) {
                    shortcuts.push(shortcut);
                }
                shortcut_depth = None;
            }
            depth = depth.saturating_sub(1);
            continue;
        }

        let Some(key) = read_c_string(data, &mut offset) else {
            break;
        };

        match field_type {
            0x00 => {
                depth += 1;
                if depth == 2 && key.as_bytes().iter().all(u8::is_ascii_digit) {
                    shortcut_depth = Some(depth);
                    current_name = None;
                    current_exe = None;
                    current_start_dir = None;
                    current_launch_options = None;
                }
            },
            0x01 => {
                let Some(value) = read_c_string(data, &mut offset) else {
                    break;
                };
                if shortcut_depth == Some(depth) {
                    match key.to_ascii_lowercase().as_str() {
                        "appname" => current_name = Some(value),
                        "exe" => current_exe = Some(value),
                        "startdir" => current_start_dir = Some(value),
                        "launchoptions" => current_launch_options = Some(value),
                        _ => {},
                    }
                }
            },
            0x02 => {
                offset = offset.saturating_add(4);
            },
            _ => break,
        }
    }

    shortcuts
}

fn read_c_string(data: &[u8], offset: &mut usize) -> Option<String> {
    let start = *offset;
    while *offset < data.len() && data[*offset] != 0 {
        *offset += 1;
    }
    if *offset >= data.len() {
        return None;
    }
    let value = String::from_utf8_lossy(&data[start..*offset]).to_string();
    *offset += 1;
    Some(value)
}

fn build_non_steam_shortcut(
    name: Option<String>,
    exe: Option<String>,
    start_dir: Option<String>,
    launch_options: Option<String>,
) -> Option<NonSteamShortcut> {
    let name = name?.trim().to_string();
    if name.is_empty() {
        return None;
    }

    let exe = exe?;
    let exe_path = clean_shortcut_path(&exe)?;
    if exe_path.extension().map(|ext| ext.to_string_lossy().eq_ignore_ascii_case("exe")) != Some(true) {
        return None;
    }

    let start_dir = start_dir.and_then(|dir| clean_shortcut_path(&dir));
    let mut launch_args = extract_shortcut_args(&exe);
    if let Some(options) = launch_options {
        launch_args.extend(split_shortcut_args(&options));
    }
    Some(NonSteamShortcut { name, exe_path, start_dir, launch_args })
}

fn clean_shortcut_path(path: &str) -> Option<PathBuf> {
    let trimmed = extract_shortcut_path(path)?;
    if trimmed.is_empty() {
        return None;
    }

    let normalized = trimmed.replace('\\', "/");
    let home = dirs::home_dir().unwrap_or_default();

    if normalized.len() > 2 && normalized.as_bytes().get(1) == Some(&b':') {
        let drive = normalized.chars().next()?.to_ascii_lowercase();
        let rest = normalized[2..].trim_start_matches('/');
        return match drive {
            'z' => Some(PathBuf::from("/").join(rest)),
            'c' => {
                Some(crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam").join("drive_c").join(rest))
            },
            _ => Some(PathBuf::from("/").join(rest)),
        };
    }

    Some(PathBuf::from(normalized))
}

fn extract_shortcut_path(path: &str) -> Option<String> {
    let trimmed = path.trim();
    if trimmed.is_empty() {
        return None;
    }

    if let Some(rest) = trimmed.strip_prefix('"') {
        let end = rest.find('"').unwrap_or(rest.len());
        return Some(rest[..end].trim().to_string());
    }
    if let Some(rest) = trimmed.strip_prefix('\'') {
        let end = rest.find('\'').unwrap_or(rest.len());
        return Some(rest[..end].trim().to_string());
    }

    let lower = trimmed.to_ascii_lowercase();
    if let Some(idx) = lower.find(".exe") {
        return Some(trimmed[..idx + 4].trim().to_string());
    }

    Some(trimmed.to_string())
}

fn extract_shortcut_args(command: &str) -> Vec<String> {
    let Some(rest) = shortcut_args_suffix(command) else {
        return Vec::new();
    };
    split_shortcut_args(rest)
}

fn shortcut_args_suffix(command: &str) -> Option<&str> {
    let trimmed = command.trim();
    if trimmed.is_empty() {
        return None;
    }

    if let Some(rest) = trimmed.strip_prefix('"') {
        let end = rest.find('"')?;
        return Some(rest[end + 1..].trim());
    }
    if let Some(rest) = trimmed.strip_prefix('\'') {
        let end = rest.find('\'')?;
        return Some(rest[end + 1..].trim());
    }

    let lower = trimmed.to_ascii_lowercase();
    let idx = lower.find(".exe")?;
    Some(trimmed[idx + 4..].trim())
}

fn split_shortcut_args(args: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;

    for ch in args.chars() {
        match (quote, ch) {
            (Some(q), c) if c == q => quote = None,
            (None, '"' | '\'') => quote = Some(ch),
            (None, c) if c.is_whitespace() => {
                if !current.is_empty() {
                    out.push(std::mem::take(&mut current));
                }
            },
            (_, c) => current.push(c),
        }
    }

    if !current.is_empty() {
        out.push(current);
    }

    out
}

fn shortcut_id(name: &str, exe_path: &Path) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in name.as_bytes().iter().chain(b"\0").chain(exe_path.to_string_lossy().as_bytes()) {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{:016x}", hash)
}

fn parse_acf(contents: &str) -> Option<(u32, String, String)> {
    let mut appid: Option<u32> = None;
    let mut name: Option<String> = None;
    let mut install_dir: Option<String> = None;

    for line in contents.lines() {
        let trimmed = line.trim();
        if let Some((k, v)) = parse_kv(trimmed) {
            match k {
                "appid" => appid = v.parse().ok(),
                "name" => name = Some(v.to_string()),
                "installdir" => install_dir = Some(v.to_string()),
                _ => {},
            }
        }
    }

    match (appid, name, install_dir) {
        (Some(a), Some(n), Some(d)) => Some((a, n, d)),
        _ => None,
    }
}

fn parse_kv(line: &str) -> Option<(&str, &str)> {
    let line = line.trim_start_matches('"');
    let parts: Vec<&str> = line.splitn(2, "\"\t\t\"").collect();
    if parts.len() == 2 {
        let key = parts[0].trim();
        let val = parts[1].trim_end_matches('"');
        Some((key, val))
    } else {
        None
    }
}

fn scan_steam_library() -> Result<Vec<Game>, Box<dyn std::error::Error>> {
    let mut games = Vec::new();

    for lib in steam_library_paths() {
        let entries = std::fs::read_dir(&lib)?;
        for entry in entries.flatten() {
            let path = entry.path();
            let fname = path.file_name().unwrap_or_default().to_string_lossy();
            if !fname.starts_with("appmanifest_") || !fname.ends_with(".acf") {
                continue;
            }

            let contents = std::fs::read_to_string(&path)?;
            if let Some((appid, name, install_dir)) = parse_acf(&contents) {
                let game_path = lib.join("common").join(&install_dir);
                let exe = find_exe_in_dir(&game_path);

                games.push(Game {
                    id: format!("steam_{}", appid),
                    name,
                    exe_path: exe.unwrap_or_default(),
                    platform: "steam".into(),
                    steam_app_id: Some(appid),
                    size_bytes: dir_size(&game_path),
                    metalsharp_compatible: true,
                });
            }
        }
    }

    Ok(games)
}

fn is_valid_game_exe(name: &str) -> bool {
    let lower = name.to_lowercase();
    !lower.contains("setup")
        && !lower.contains("redist")
        && !lower.contains("dotnet")
        && !lower.contains("installer")
        && !lower.contains("uninstall")
        && !lower.contains("vcredist")
        && !lower.contains("crashhandler")
        && !lower.contains("server")
}

fn find_exe_in_dir(dir: &PathBuf) -> Option<String> {
    let mut best: Option<String> = None;
    for entry in WalkDir::new(dir).max_depth(3).into_iter().flatten() {
        if let Some(ext) = entry.path().extension() {
            if ext == "exe" {
                let name = entry.file_name().to_string_lossy().to_string();
                if !is_valid_game_exe(&name) {
                    continue;
                }
                let lower = name.to_lowercase();
                let matches_name = lower.starts_with("rain")
                    || lower.starts_with("terraria")
                    || lower.starts_with("hl2")
                    || lower == "game.exe";
                if matches_name {
                    return Some(entry.path().to_string_lossy().to_string());
                }
                if best.is_none() {
                    best = Some(entry.path().to_string_lossy().to_string());
                }
            }
        }
    }
    best
}

fn dir_size(dir: &PathBuf) -> Option<u64> {
    let mut total: u64 = 0;
    for entry in WalkDir::new(dir).into_iter().flatten() {
        if let Ok(m) = entry.metadata() {
            if m.is_file() {
                total += m.len();
            }
        }
    }
    Some(total)
}

fn scan_local_exes() -> Result<Vec<Game>, Box<dyn std::error::Error>> {
    let mut games = Vec::new();
    let home = dirs::home_dir().unwrap_or_default();
    let metalsharp_dir = crate::platform::metalsharp_home_dir_for(&home).join("games");

    if !metalsharp_dir.exists() {
        return Ok(games);
    }

    for entry in std::fs::read_dir(&metalsharp_dir)?.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let name = path.file_name().unwrap_or_default().to_string_lossy().to_string();
        if let Some(exe) = find_exe_in_dir(&path) {
            games.push(Game {
                id: format!("local_{}", name),
                name,
                exe_path: exe,
                platform: "local".into(),
                steam_app_id: None,
                size_bytes: dir_size(&path),
                metalsharp_compatible: true,
            });
        }
    }

    Ok(games)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_binary_non_steam_shortcuts() {
        let exe = if cfg!(windows) { "C:\\Games\\Joy\\Joy.exe" } else { "Z:\\tmp\\Joy\\Joy.exe" };
        let data = test_shortcuts_vdf("Joy of Creation", exe, "Z:\\tmp\\Joy", None, false);

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts.len(), 1);
        assert_eq!(shortcuts[0].name, "Joy of Creation");
        assert!(shortcuts[0].exe_path.ends_with("tmp/Joy/Joy.exe"));
        assert!(shortcuts[0].launch_args.is_empty());
    }

    #[test]
    fn ignores_shortcuts_without_exe_targets() {
        let data = test_shortcuts_vdf("Native App", "/Applications/Foo.app", "/Applications", None, false);

        assert!(parse_shortcuts_vdf(&data).is_empty());
    }

    #[test]
    fn strips_shortcut_launch_args_from_quoted_exe_path() {
        let data =
            test_shortcuts_vdf("DX12 Game", "\"Z:\\tmp\\DxGame\\Game.exe\" -d3d12", "Z:\\tmp\\DxGame", None, false);

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts.len(), 1);
        assert!(shortcuts[0].exe_path.ends_with("tmp/DxGame/Game.exe"));
        assert_eq!(shortcuts[0].launch_args, vec!["-d3d12"]);
    }

    #[test]
    fn preserves_multiple_shortcut_launch_args() {
        let data = test_shortcuts_vdf(
            "DX12 Game",
            "\"Z:\\tmp\\DxGame\\Game.exe\" -d3d12 -windowed \"-profile=high perf\"",
            "Z:\\tmp\\DxGame",
            None,
            false,
        );

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts[0].launch_args, vec!["-d3d12", "-windowed", "-profile=high perf"]);
    }

    #[test]
    fn preserves_launch_options_field() {
        let data = test_shortcuts_vdf(
            "Joy of Creation",
            "\"Z:\\tmp\\Joy\\Joy.exe\"",
            "Z:\\tmp\\Joy",
            Some("-d3d12 -windowed"),
            false,
        );

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts.len(), 1);
        assert_eq!(shortcuts[0].launch_args, vec!["-d3d12", "-windowed"]);
    }

    #[test]
    fn parses_shortcuts_with_steam_key_casing() {
        let data = test_shortcuts_vdf_with_keys(
            [
                ("AppName", "The Joy Of Creation"),
                ("Exe", "\"C:\\users\\alexmondello\\Desktop\\The Joy of Creation Story Mode\\TJoC_SM.exe\""),
                ("StartDir", "C:\\users\\alexmondello\\Desktop\\The Joy of Creation Story Mode\\"),
                ("LaunchOptions", "-d3d12"),
            ],
            false,
        );

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts.len(), 1);
        assert_eq!(shortcuts[0].name, "The Joy Of Creation");
        assert!(shortcuts[0]
            .exe_path
            .ends_with("users/alexmondello/Desktop/The Joy of Creation Story Mode/TJoC_SM.exe"));
        assert_eq!(shortcuts[0].launch_args, vec!["-d3d12"]);
    }

    #[test]
    fn nested_tags_do_not_cancel_shortcut_parsing() {
        let data = test_shortcuts_vdf("Tagged Game", "Z:\\tmp\\Tagged\\Game.exe", "Z:\\tmp\\Tagged", None, true);

        let shortcuts = parse_shortcuts_vdf(&data);

        assert_eq!(shortcuts.len(), 1);
        assert_eq!(shortcuts[0].name, "Tagged Game");
        assert!(shortcuts[0].exe_path.ends_with("tmp/Tagged/Game.exe"));
    }

    #[test]
    fn scan_shortcut_ids_use_explicit_stable_hex_hashes() {
        let path = PathBuf::from("/tmp/Game/Game.exe");

        let first = shortcut_id("Game", &path);
        let second = shortcut_id("Game", &path);
        let changed = shortcut_id("Game", Path::new("/tmp/Game/Other.exe"));

        assert_eq!(first, second);
        assert_eq!(first.len(), 16);
        assert!(first.chars().all(|ch| ch.is_ascii_hexdigit()));
        assert_ne!(first, changed);
    }

    #[test]
    fn windows_exe_marks_shared_steam_dir_as_wine_candidate() {
        let dir = test_dir("shared-windows-depot");
        std::fs::create_dir_all(dir.join("Binaries")).expect("create game dir");
        std::fs::write(dir.join("Binaries").join("Game.exe"), b"exe").expect("write exe");

        let mut macos_dir = None;
        let mut wine_dir = None;
        classify_dual_game_dir(&dir, &mut macos_dir, &mut wine_dir);

        assert_eq!(wine_dir.as_deref(), Some(dir.as_path()));
        assert!(macos_dir.is_none());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn libraryfolders_discovers_external_steam_library() {
        let root = test_dir("external-steam-library");
        let primary = root.join("internal/steamapps");
        let external = root.join("external/steamapps");
        std::fs::create_dir_all(&primary).expect("create primary Steam library");
        std::fs::create_dir_all(&external).expect("create external Steam library");
        std::fs::write(
            primary.join("libraryfolders.vdf"),
            format!(
                "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n",
                external.parent().unwrap().display()
            ),
        )
        .expect("write libraryfolders.vdf");

        let discovered = parse_library_folders(&primary);
        assert_eq!(discovered, vec![external]);

        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn native_app_without_windows_exe_stays_macos_candidate() {
        let dir = test_dir("native-only-depot");
        std::fs::create_dir_all(dir.join("Game.app").join("Contents").join("MacOS")).expect("create app dir");

        let mut macos_dir = None;
        let mut wine_dir = None;
        classify_dual_game_dir(&dir, &mut macos_dir, &mut wine_dir);

        assert_eq!(macos_dir.as_deref(), Some(dir.as_path()));
        assert!(wine_dir.is_none());
        let _ = std::fs::remove_dir_all(dir);
    }

    fn test_dir(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "metalsharp-scan-{}-{}-{}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
        ))
    }

    fn test_shortcuts_vdf(
        name: &str,
        exe: &str,
        start_dir: &str,
        launch_options: Option<&str>,
        include_tags: bool,
    ) -> Vec<u8> {
        let mut fields = vec![("appname", name), ("exe", exe), ("StartDir", start_dir)];
        if let Some(options) = launch_options {
            fields.push(("LaunchOptions", options));
        }
        test_shortcuts_vdf_with_keys(fields, include_tags)
    }

    fn test_shortcuts_vdf_with_keys<'a>(
        fields: impl IntoIterator<Item = (&'a str, &'a str)>,
        include_tags: bool,
    ) -> Vec<u8> {
        let mut data = Vec::new();
        object(&mut data, "shortcuts");
        object(&mut data, "0");
        for (key, value) in fields {
            string_field(&mut data, key, value);
        }
        if include_tags {
            object(&mut data, "tags");
            string_field(&mut data, "0", "favorite");
            data.push(0x08);
        }
        data.push(0x08);
        data.push(0x08);
        data
    }

    fn object(data: &mut Vec<u8>, key: &str) {
        data.push(0x00);
        data.extend_from_slice(key.as_bytes());
        data.push(0);
    }

    fn string_field(data: &mut Vec<u8>, key: &str, value: &str) {
        data.push(0x01);
        data.extend_from_slice(key.as_bytes());
        data.push(0);
        data.extend_from_slice(value.as_bytes());
        data.push(0);
    }
}
