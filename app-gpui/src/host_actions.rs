//! Native equivalents of Electron main-process IPC handlers that are not
//! backend routes (`app:open-steam-art-manager`, `app:open-*-folder`,
//! `app:repair-data-access`, `app:uninstall`).
use serde_json::{Value, json};
use std::path::{Path, PathBuf};

fn user_home() -> PathBuf {
    std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default()
}

fn read_json(path: &Path) -> Option<serde_json::Map<String, Value>> {
    let text = std::fs::read_to_string(path).ok()?;
    match serde_json::from_str::<Value>(&text).ok()? {
        Value::Object(map) => Some(map),
        _ => None,
    }
}

fn is_empty_value(value: Option<&Value>) -> bool {
    match value {
        None | Some(Value::Null) => true,
        Some(Value::String(s)) => s.is_empty(),
        Some(Value::Object(map)) => map.is_empty(),
        _ => false,
    }
}

pub fn steam_art_manager_app(resources: &Path) -> PathBuf {
    resources.join("tools/steam-art-manager/Steam Art Manager.app")
}

/// `app:open-steam-art-manager`. Returns `Some(path)` when the first-run
/// clipboard hint should be shown for the Wine Steam path.
pub fn open_steam_art_manager(resources: &Path, home: &Path) -> Result<Option<String>, String> {
    let app = steam_art_manager_app(resources);
    if !app.exists() {
        return Err("Steam Art Manager is not installed".into());
    }
    let wine_steam = home.join("prefix-steam/drive_c/Program Files (x86)/Steam");
    let sam_dir = user_home().join("Library/Application Support/dev.tormak.steam-art-manager");
    let sam_settings = sam_dir.join("settings.json");
    let backup_dir = home.join("config");
    let backup_path = backup_dir.join("steam-art-manager-settings.json");
    let keys = ["steamInstallPath", "steamGridDbApiKey", "steamApiKeyMap"];
    let sam = read_json(&sam_settings);
    let backup = read_json(&backup_path).unwrap_or_default();
    let mut mutated = sam.is_none();
    let mut merged = sam.unwrap_or_else(|| {
        let mut map = serde_json::Map::new();
        map.insert("version".into(), json!("3.19.1"));
        map
    });
    for key in keys {
        if is_empty_value(merged.get(key)) && !is_empty_value(backup.get(key)) {
            merged.insert(key.into(), backup[key].clone());
            mutated = true;
        }
    }
    if is_empty_value(merged.get("steamInstallPath")) && wine_steam.exists() {
        merged.insert(
            "steamInstallPath".into(),
            json!(wine_steam.to_string_lossy()),
        );
        mutated = true;
    }
    if mutated {
        let _ = std::fs::create_dir_all(&sam_dir);
        let _ = std::fs::write(
            &sam_settings,
            serde_json::to_vec_pretty(&Value::Object(merged.clone())).unwrap_or_default(),
        );
    }
    let mut backed_up = serde_json::Map::new();
    backed_up.insert(
        "version".into(),
        merged.get("version").cloned().unwrap_or(json!("3.19.1")),
    );
    backed_up.insert("updatedAt".into(), json!(now_iso()));
    let mut changed = !backup_path.exists();
    for key in keys {
        let value = if is_empty_value(merged.get(key)) {
            backup.get(key).cloned().unwrap_or(Value::Null)
        } else {
            merged[key].clone()
        };
        if !changed && Some(&value) != backup.get(key) {
            changed = true;
        }
        backed_up.insert(key.into(), value);
    }
    if changed {
        let _ = std::fs::create_dir_all(&backup_dir);
        let _ = std::fs::write(
            &backup_path,
            serde_json::to_vec_pretty(&Value::Object(backed_up)).unwrap_or_default(),
        );
    }
    let intro = home.join(".steam-art-manager-intro-shown");
    let hint =
        (!intro.exists() && wine_steam.exists()).then(|| wine_steam.to_string_lossy().into_owned());
    if hint.is_some() {
        let _ = std::fs::write(&intro, now_iso());
    }
    // Reset obviously-corrupt SAM window geometry.
    let window_state = sam_dir.join(".window-state.json");
    if window_state.exists() {
        let corrupt = read_json(&window_state)
            .and_then(|state| state.get("main").cloned())
            .map(|main| {
                let w = main.get("width").and_then(Value::as_f64).unwrap_or(-1.0);
                let h = main.get("height").and_then(Value::as_f64).unwrap_or(-1.0);
                w <= 0.0 || h <= 0.0 || w > 10000.0 || h > 10000.0
            })
            .unwrap_or(true);
        if corrupt {
            let _ = std::fs::remove_file(&window_state);
        }
    }
    std::process::Command::new("/usr/bin/open")
        .arg(&app)
        .spawn()
        .map_err(|e| e.to_string())?;
    Ok(hint)
}

fn now_iso() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs.to_string()
}

/// Watched grid-art directories for the Electron `grid-art:changed` signal.
pub fn grid_art_signature(home: &Path) -> Vec<(PathBuf, u128)> {
    let users = home.join("prefix-steam/drive_c/Program Files (x86)/Steam/userdata");
    let mut out = Vec::new();
    let Ok(entries) = std::fs::read_dir(users) else {
        return out;
    };
    for entry in entries.flatten() {
        let grid = entry.path().join("config/grid");
        if let Ok(files) = std::fs::read_dir(&grid) {
            for file in files.flatten() {
                let modified = file
                    .metadata()
                    .ok()
                    .and_then(|m| m.modified().ok())
                    .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                    .map(|d| d.as_millis())
                    .unwrap_or(0);
                out.push((file.path(), modified));
            }
        }
    }
    out.sort();
    out
}

/// Steam appids whose grid files (`<appid>p.png`, `<appid>_hero.gif`, ...)
/// were added, removed, or rewritten between two `grid_art_signature`s.
pub fn grid_art_changed_appids(before: &[(PathBuf, u128)], after: &[(PathBuf, u128)]) -> Vec<u64> {
    let before: std::collections::HashSet<_> = before.iter().collect();
    let after_set: std::collections::HashSet<_> = after.iter().collect();
    let mut appids: Vec<u64> = before
        .symmetric_difference(&after_set)
        .filter_map(|(path, _)| {
            let name = path.file_name()?.to_str()?;
            let digits: String = name.chars().take_while(char::is_ascii_digit).collect();
            digits.parse().ok()
        })
        .collect();
    appids.sort_unstable();
    appids.dedup();
    appids
}

/// `shell.openPath` equivalent after `mkdir -p`.
pub fn open_folder(path: &Path) -> Result<(), String> {
    std::fs::create_dir_all(path).map_err(|e| e.to_string())?;
    std::process::Command::new("/usr/bin/open")
        .arg(path)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// main `verifyMetalsharpDataAccess`.
pub fn verify_data_access(home: &Path) -> Value {
    let dirs = [
        "games",
        "cache",
        "logs",
        "sharp-library",
        "shader-cache",
        "pipeline-cache",
        "runtime/fna",
        "runtime/shims",
        "runtime/mono-x86",
        "runtime/dxvk-1.10.3",
    ];
    for dir in dirs {
        let _ = std::fs::create_dir_all(home.join(dir));
    }
    let mut checks = Vec::new();
    let mut ok = true;
    for dir in ["logs", "sharp-library", "cache"] {
        let probe = home.join(dir).join(".metalsharp-access-check");
        let result = std::fs::write(&probe, "ok").and_then(|_| std::fs::remove_file(&probe));
        match result {
            Ok(()) => checks.push(json!({"dir": dir, "ok": true})),
            Err(error) => {
                ok = false;
                checks.push(json!({"dir": dir, "ok": false, "error": error.to_string()}));
            }
        }
    }
    json!({"ok": ok, "path": home.to_string_lossy(), "checks": checks})
}

/// main `metalsharpRelatedDataPaths` (Electron-specific entries map to this app's identifiers).
pub fn related_data_paths(home: &Path) -> Vec<PathBuf> {
    let user = user_home();
    let library = user.join("Library");
    let mut paths = Vec::new();
    // a data folder on an external drive: its folder there, ahead of the link to it
    if std::fs::symlink_metadata(home).is_ok_and(|meta| meta.file_type().is_symlink()) {
        if let Ok(folder) = std::fs::canonicalize(home) {
            paths.push(folder);
        }
    }
    paths.extend([
        home.to_path_buf(),
        user.join(".metalsharp"),
        user.join(".metalsharp-dev"),
        library.join("Application Support/MetalSharp"),
        library.join("Application Support/metalsharp"),
        library.join("Caches/metalsharp"),
        library.join("Caches/MetalSharp"),
        library.join("Logs/MetalSharp"),
        library.join("Preferences/com.metalsharp.app.plist"),
        library.join("Preferences/com.metalsharp.MetalSharp.wineloader.plist"),
        library.join("Saved Application State/com.metalsharp.app.savedState"),
        library.join("HTTPStorages/com.metalsharp.app"),
        library.join("WebKit/com.metalsharp.app"),
    ]);
    if let Ok(entries) = std::fs::read_dir(library.join("Application Support/CrashReporter")) {
        for entry in entries.flatten() {
            if entry
                .file_name()
                .to_string_lossy()
                .to_lowercase()
                .starts_with("metalsharp")
            {
                paths.push(entry.path());
            }
        }
    }
    paths
}

fn make_writable(path: &Path) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        if let Ok(meta) = std::fs::symlink_metadata(path) {
            if meta.file_type().is_symlink() {
                return;
            }
            let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700));
            if meta.is_dir() {
                if let Ok(entries) = std::fs::read_dir(path) {
                    for entry in entries.flatten() {
                        make_writable(&entry.path());
                    }
                }
            }
        }
    }
}

pub fn remove_path(path: &Path) -> bool {
    let attempt = |p: &Path| match std::fs::symlink_metadata(p) {
        Err(_) => true,
        Ok(meta) if meta.is_dir() && !meta.file_type().is_symlink() => {
            std::fs::remove_dir_all(p).is_ok()
        }
        Ok(_) => std::fs::remove_file(p).is_ok(),
    };
    if attempt(path) {
        return true;
    }
    make_writable(path);
    attempt(path)
}

/// After the app exits, move the enclosing `/Applications/*.app` to the Trash.
/// Returns whether the bundle will be moved to the Trash after exit.
pub fn schedule_bundle_trash() -> bool {
    let Ok(exe) = std::env::current_exe() else {
        return false;
    };
    let Some(bundle) = exe
        .ancestors()
        .find(|p| p.extension().is_some_and(|e| e == "app"))
    else {
        return false;
    };
    if !bundle.starts_with("/Applications") {
        return false;
    }
    let pid = std::process::id();
    let escaped = bundle.to_string_lossy().replace('"', "\\\"");
    let script = format!(
        "while kill -0 {pid} 2>/dev/null; do sleep 0.5; done; osascript -e 'tell application \"Finder\" to delete POSIX file \"{escaped}\"' >/dev/null 2>&1"
    );
    let mut command = std::process::Command::new("/bin/bash");
    crate::lifecycle::unmask_child_signals(&mut command);
    command.arg("-c").arg(script);
    #[cfg(unix)]
    unsafe {
        use std::os::unix::process::CommandExt;
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    command
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .is_ok()
}

/// main `gameJoltDirectoryForDownloads`.
pub fn gamejolt_download_dir(home: &Path) -> PathBuf {
    let root = read_json(&home.join("gamejolt/storage.json"))
        .and_then(|config| {
            config
                .get("rootPath")
                .and_then(Value::as_str)
                .map(str::to_owned)
        })
        .filter(|root| !root.trim().is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home.to_path_buf());
    root.join("GameJolt")
}

/// main `safeDownloadStem`.
fn safe_download_stem(filename: &str) -> String {
    let base: String = Path::new(filename)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
        .chars()
        .map(|c| if "\\/:*?\"<>|".contains(c) { '_' } else { c })
        .collect();
    let base = base.trim().to_owned();
    let base = if base.is_empty() {
        "GameJolt Game".to_owned()
    } else {
        base
    };
    let lower = base.to_lowercase();
    let mut stem = base.clone();
    for ext in [
        ".zip", ".rar", ".tar", ".tgz", ".gz", ".bz2", ".xz", ".7z", ".exe", ".msi", ".dmg",
    ] {
        if lower.ends_with(ext) {
            stem = base[..base.len() - ext.len()].to_owned();
            break;
        }
    }
    if stem.to_lowercase().ends_with(".tar") {
        stem.truncate(stem.len() - 4);
    }
    let stem = stem.trim().to_owned();
    if stem.is_empty() {
        "GameJolt Game".into()
    } else {
        stem
    }
}

/// main `organizeGameJoltDownload` (extraction only; caller syncs). Returns the game's folder.
pub fn organize_gamejolt_download(
    download: &Path,
    filename: &str,
    gamejolt_dir: &Path,
) -> Result<PathBuf, String> {
    let game_dir = gamejolt_dir.join(safe_download_stem(filename));
    std::fs::create_dir_all(&game_dir).map_err(|e| e.to_string())?;
    let lower = filename.to_lowercase();
    let run = |program: &str, args: &[&std::ffi::OsStr]| -> Result<(), String> {
        let output = std::process::Command::new(program)
            .args(args)
            .output()
            .map_err(|e| e.to_string())?;
        if output.status.success() {
            Ok(())
        } else {
            let stderr = String::from_utf8_lossy(&output.stderr).trim().to_owned();
            Err(if stderr.is_empty() {
                format!("{program} failed")
            } else {
                stderr
            })
        }
    };
    let result = if lower.ends_with(".zip") {
        run(
            "/usr/bin/ditto",
            &[
                "-x".as_ref(),
                "-k".as_ref(),
                download.as_os_str(),
                game_dir.as_os_str(),
            ],
        )
    } else if lower.ends_with(".rar") {
        run(
            "/usr/bin/bsdtar",
            &[
                "-xf".as_ref(),
                download.as_os_str(),
                "-C".as_ref(),
                game_dir.as_os_str(),
            ],
        )
    } else if lower.ends_with(".tar") || lower.ends_with(".tar.gz") || lower.ends_with(".tgz") {
        run(
            "/usr/bin/tar",
            &[
                "-xf".as_ref(),
                download.as_os_str(),
                "-C".as_ref(),
                game_dir.as_os_str(),
            ],
        )
    } else {
        let destination = crate::mini_browser::unique_download_path(&game_dir, filename);
        std::fs::rename(download, destination).map_err(|e| e.to_string())
    };
    if result.is_ok() {
        let _ = std::fs::remove_file(download);
    }
    result.map(|_| game_dir)
}

// ─────────────────────────────── Game Jolt art ───────────────────────────────
//
// A game downloaded from the Game Jolt browser gets its card and hero art from its Game Jolt page (#822): the
// thumbnail as its card and the header as its hero, saved in its folder as `.metalsharp/gamejolt-card.<ext>` and
// `.metalsharp/gamejolt-hero.<ext>`, which the backend reports (`cover_path`, `hero_path`) before the executable's
// icon. `.metalsharp/gamejolt.json` keeps the game's Game Jolt id and page.

/// The folder in a Game Jolt game's folder that keeps what MetalSharp knows of it.
const GAMEJOLT_META_DIR: &str = ".metalsharp";
const GAMEJOLT_ART_EXTENSIONS: [&str; 5] = ["jpg", "jpeg", "png", "webp", "gif"];

/// The Game Jolt game id in one of its pages' URLs: `https://gamejolt.com/games/<slug>/<id>`, and the pages under it
/// (its download page among them).
pub fn gamejolt_game_id(page_url: &str) -> Option<u64> {
    let url = url::Url::parse(page_url).ok()?;
    let host = url.host_str()?;
    if url.scheme() != "https" || (host != "gamejolt.com" && !host.ends_with(".gamejolt.com")) {
        return None;
    }
    let mut segments = url.path_segments()?;
    if segments.next()? != "games" {
        return None;
    }
    let _slug = segments.next()?;
    segments.next()?.parse().ok().filter(|id| *id > 0)
}

/// The card (thumbnail) and hero (header) image URLs in Game Jolt's reply for a game
/// (`/site-api/web/discover/games/<id>`); either stands in for the other when the game has only one.
pub fn gamejolt_art_urls(reply: &Value) -> (Option<String>, Option<String>) {
    let game = reply.pointer("/payload/game").or_else(|| reply.get("game"));
    let media = |key: &str| -> Option<String> {
        let item = game?.get(key)?;
        ["img_url", "mediaserver_url"]
            .iter()
            .filter_map(|field| item.get(*field).and_then(Value::as_str))
            .find(|url| url.starts_with("https://"))
            .map(str::to_owned)
    };
    let thumbnail = media("thumbnail_media_item");
    let header = media("header_media_item");
    (
        thumbnail.clone().or_else(|| header.clone()),
        header.or(thumbnail),
    )
}

/// The extension of the image `bytes` are, when it is one an art file may have.
fn image_extension(bytes: &[u8]) -> Option<&'static str> {
    match image::guess_format(bytes).ok()? {
        image::ImageFormat::Png => Some("png"),
        image::ImageFormat::Jpeg => Some("jpg"),
        image::ImageFormat::WebP => Some("webp"),
        image::ImageFormat::Gif => Some("gif"),
        _ => None,
    }
}

/// A Game Jolt game's saved card (`kind` "card") or hero ("hero") art in its folder.
pub fn gamejolt_saved_art(game_dir: &Path, kind: &str) -> Option<PathBuf> {
    GAMEJOLT_ART_EXTENSIONS
        .iter()
        .map(|ext| {
            game_dir
                .join(GAMEJOLT_META_DIR)
                .join(format!("gamejolt-{kind}.{ext}"))
        })
        .find(|path| path.is_file())
}

/// Save `bytes` as the game's `kind` art, in place of what was saved before.
fn save_gamejolt_art(game_dir: &Path, kind: &str, ext: &str, bytes: &[u8]) -> std::io::Result<()> {
    let dir = game_dir.join(GAMEJOLT_META_DIR);
    std::fs::create_dir_all(&dir)?;
    while let Some(previous) = gamejolt_saved_art(game_dir, kind) {
        std::fs::remove_file(previous)?;
    }
    let path = dir.join(format!("gamejolt-{kind}.{ext}"));
    let partial = dir.join(format!(".gamejolt-{kind}.{ext}.part"));
    std::fs::write(&partial, bytes)?;
    std::fs::rename(partial, path)
}

/// The art of the Game Jolt game whose page `page_url` is, into its folder. Best effort: a game whose page isn't
/// known (a download started elsewhere) keeps its executable's icon, and Change Image still picks another.
pub fn fetch_gamejolt_art(game_dir: &Path, page_url: &str) -> Result<(), String> {
    let id = gamejolt_game_id(page_url).ok_or("not a Game Jolt game page")?;
    let dir = game_dir.join(GAMEJOLT_META_DIR);
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let meta = json!({ "gameId": id, "pageUrl": page_url });
    let _ = std::fs::write(
        dir.join("gamejolt.json"),
        serde_json::to_vec_pretty(&meta).unwrap_or_default(),
    );
    let reply = crate::artwork::download(
        crate::artwork::http_agent(),
        &format!("https://gamejolt.com/site-api/web/discover/games/{id}"),
        None,
    )
    .ok_or("Game Jolt did not answer")?;
    let reply: Value = serde_json::from_slice(&reply).map_err(|e| e.to_string())?;
    let (card, hero) = gamejolt_art_urls(&reply);
    for (kind, url) in [("card", card), ("hero", hero)] {
        let Some(url) = url else { continue };
        let Some(bytes) = crate::artwork::download(crate::artwork::http_agent(), &url, None) else {
            continue;
        };
        // only an image the app can draw replaces the executable's icon, named for what it is
        let Some(ext) = image_extension(&bytes) else {
            continue;
        };
        save_gamejolt_art(game_dir, kind, ext, &bytes).map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[cfg(test)]
mod gamejolt_art_tests {
    use super::*;

    #[test]
    fn a_game_page_names_its_game() {
        assert_eq!(
            gamejolt_game_id("https://gamejolt.com/games/frickbears3/812345"),
            Some(812345)
        );
        // the download page under it, and a query
        assert_eq!(
            gamejolt_game_id(
                "https://gamejolt.com/games/frickbears3/812345/download/build/1234?x=1"
            ),
            Some(812345)
        );
        for other in [
            "https://gamejolt.com/games",
            "https://gamejolt.com/games/frickbears3",
            "https://gamejolt.com/@someone",
            "https://gamejolt.com/games/frickbears3/not-a-number",
            "http://gamejolt.com/games/frickbears3/812345",
            "https://notgamejolt.com/games/frickbears3/812345",
            "not a url",
        ] {
            assert_eq!(gamejolt_game_id(other), None, "{other}");
        }
    }

    #[test]
    fn the_thumbnail_is_the_card_and_the_header_the_hero() {
        let reply = json!({"payload": {"game": {
            "thumbnail_media_item": {"img_url": "https://m.gjcdn.net/game-thumbnail/500/1-a.jpg"},
            "header_media_item": {"mediaserver_url": "https://m.gjcdn.net/game-header/2000/1-b.png"}
        }}});
        assert_eq!(
            gamejolt_art_urls(&reply),
            (
                Some("https://m.gjcdn.net/game-thumbnail/500/1-a.jpg".into()),
                Some("https://m.gjcdn.net/game-header/2000/1-b.png".into())
            )
        );
        // a game with only a thumbnail has it as its hero too, and a URL that isn't https is left out
        let reply = json!({"payload": {"game": {
            "thumbnail_media_item": {"img_url": "https://m.gjcdn.net/t.webp"},
            "header_media_item": {"img_url": "http://m.gjcdn.net/h.jpg"}
        }}});
        assert_eq!(
            gamejolt_art_urls(&reply),
            (
                Some("https://m.gjcdn.net/t.webp".into()),
                Some("https://m.gjcdn.net/t.webp".into())
            )
        );
        assert_eq!(gamejolt_art_urls(&json!({"payload": {}})), (None, None));
        // the art is named for what its bytes are, and only an image the app can draw is kept
        assert_eq!(image_extension(b"\x89PNG\r\n\x1a\n0000"), Some("png"));
        assert_eq!(
            image_extension(b"<svg xmlns='http://www.w3.org/2000/svg'/>"),
            None
        );
    }

    #[test]
    fn saved_art_replaces_what_was_saved_before() {
        let dir = std::env::temp_dir().join(format!("ms-gj-art-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(gamejolt_saved_art(&dir, "card"), None);
        save_gamejolt_art(&dir, "card", "png", b"one").unwrap();
        save_gamejolt_art(&dir, "card", "jpg", b"two").unwrap();
        let card = gamejolt_saved_art(&dir, "card").unwrap();
        assert!(card.ends_with(".metalsharp/gamejolt-card.jpg"));
        assert_eq!(std::fs::read(&card).unwrap(), b"two");
        assert!(!dir.join(".metalsharp/gamejolt-card.png").exists());
        assert_eq!(gamejolt_saved_art(&dir, "hero"), None);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
