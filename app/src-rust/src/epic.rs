use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Mutex;

const VERSION: &str = "0.21.0";
const URL: &str = "https://github.com/legendary-gl/legendary/releases/download/0.21.0/legendary_macOS_arm64";
const SHA256: &str = "28f5f7d0eb8c029679d4faaa483ec85888af17a9a75977ae9170c21d8ce3428b";
const MAX_TOOL_SIZE: u64 = 64 * 1024 * 1024;
static LEGENDARY_COMMAND_LOCK: Mutex<()> = Mutex::new(());

fn home() -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&dirs::home_dir().unwrap_or_default())
}

fn root() -> PathBuf {
    home().join("epic")
}

fn config() -> PathBuf {
    root().join("legendary")
}

fn logs() -> PathBuf {
    root().join("logs")
}

fn library_cache() -> PathBuf {
    root().join("library.json")
}

fn tool() -> PathBuf {
    std::env::var_os("METALSHARP_EPIC_LEGENDARY_BIN")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join("tools/legendary").join(format!("legendary-{VERSION}")))
}

fn valid_app_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 128
        && name.bytes().all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

fn app_name(body: &Value) -> Option<&str> {
    let name = body.get("appName")?.as_str()?;
    valid_app_name(name).then_some(name)
}

fn valid_pipeline(value: &str) -> bool {
    matches!(value, "auto" | "d3dmetal" | "vkd3d" | "m11" | "m11_32" | "m10" | "m10_32" | "m9" | "fna_arm64")
}

fn valid_mouse_mode(value: &str) -> bool {
    matches!(value, "no-recenter" | "auto")
}

fn game_root() -> PathBuf {
    let location = home().join("launcher-games/epic/location.txt");
    if let Ok(text) = fs::read_to_string(location) {
        let path = PathBuf::from(text.trim());
        let home = home();
        if path.is_absolute() && (path.starts_with("/Volumes") || path.starts_with(&home)) {
            let _ = fs::create_dir_all(&path);
            return path;
        }
    }
    let fallback = home().join("launcher-games/epic/library");
    let _ = fs::create_dir_all(&fallback);
    fallback
}

fn install_root(body: &Value) -> Option<PathBuf> {
    let Some(requested) = body.get("installPath").and_then(Value::as_str) else { return Some(game_root()) };
    let path = fs::canonicalize(requested).ok()?;
    let user_home = dirs::home_dir().unwrap_or_default();
    (path.is_dir() && (path.starts_with("/Volumes") || path.starts_with(home()) || path.starts_with(user_home)))
        .then_some(path)
}

fn binary_shape(path: &Path) -> bool {
    let mut header = [0u8; 8];
    File::open(path).and_then(|mut file| file.read_exact(&mut header)).is_ok()
        && header == [0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0x00, 0x00, 0x01]
}

fn digest_hex(digest: impl AsRef<[u8]>) -> String {
    digest.as_ref().iter().map(|byte| format!("{byte:02x}")).collect()
}

fn digest_matches(path: &Path) -> bool {
    let Ok(mut file) = File::open(path) else { return false };
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        match file.read(&mut buffer) {
            Ok(0) => break,
            Ok(count) => hasher.update(&buffer[..count]),
            Err(_) => return false,
        }
    }
    digest_hex(hasher.finalize()) == SHA256
}

fn tool_available() -> bool {
    let tool = tool();
    binary_shape(&tool)
        && digest_matches(&tool)
        && fs::metadata(tool).map(|m| m.permissions().mode() & 0o111 != 0).unwrap_or(false)
}

fn account_name() -> Option<String> {
    let value: Value = serde_json::from_slice(&fs::read(config().join("user.json")).ok()?).ok()?;
    value.get("displayName")?.as_str().map(str::to_string)
}

pub fn status() -> Value {
    let account = account_name();
    json!({
        "ok": true,
        "toolAvailable": tool_available(),
        "toolVersion": VERSION,
        "toolPath": tool(),
        "authenticated": account.is_some(),
        "account": account,
        "configPath": config(),
        "gameRoot": game_root(),
    })
}

fn command() -> Option<Command> {
    if !tool_available() {
        return None;
    }
    let mut command = Command::new(tool());
    command.env("LEGENDARY_CONFIG_PATH", config());
    Some(command)
}

fn run(args: &[&str], log_name: &str) -> Result<String, String> {
    let _guard = LEGENDARY_COMMAND_LOCK.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    fs::create_dir_all(config()).map_err(|error| error.to_string())?;
    fs::create_dir_all(logs()).map_err(|error| error.to_string())?;
    let log =
        OpenOptions::new().create(true).append(true).open(logs().join(log_name)).map_err(|error| error.to_string())?;
    let mut command = command().ok_or_else(|| "Legendary is not installed".to_string())?;
    let output = command
        .args(args)
        .stdout(Stdio::piped())
        .stderr(Stdio::from(log))
        .output()
        .map_err(|error| error.to_string())?;
    if !output.status.success() {
        return Err(format!("Legendary exited with {}", output.status));
    }
    String::from_utf8(output.stdout).map_err(|error| error.to_string())
}

pub fn install_tool() -> Value {
    if tool_available() {
        return status();
    }
    let destination = tool();
    let Some(parent) = destination.parent() else {
        return json!({"ok": false, "error": "invalid Legendary tool path"});
    };
    if let Err(error) = fs::create_dir_all(parent) {
        return json!({"ok": false, "error": error.to_string()});
    }
    let temporary = destination.with_extension(format!("part.{}", std::process::id()));
    let result = (|| -> Result<(), String> {
        let response = ureq::get(URL)
            .header("User-Agent", concat!("MetalSharp/", env!("CARGO_PKG_VERSION")))
            .call()
            .map_err(|error| error.to_string())?;
        let mut reader = response.into_body().into_reader().take(MAX_TOOL_SIZE + 1);
        let mut bytes = Vec::new();
        reader.read_to_end(&mut bytes).map_err(|error| error.to_string())?;
        if bytes.len() as u64 > MAX_TOOL_SIZE {
            return Err("Legendary download exceeded its size limit".to_string());
        }
        let digest = digest_hex(Sha256::digest(&bytes));
        if digest != SHA256 || bytes.get(..8) != Some(&[0xcf, 0xfa, 0xed, 0xfe, 0x0c, 0, 0, 1]) {
            return Err("Legendary download failed integrity validation".to_string());
        }
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o755)
            .open(&temporary)
            .map_err(|error| error.to_string())?;
        file.write_all(&bytes).and_then(|_| file.sync_all()).map_err(|error| error.to_string())?;
        fs::rename(&temporary, &destination).map_err(|error| error.to_string())?;
        Ok(())
    })();
    let _ = fs::remove_file(temporary);
    match result {
        Ok(()) => status(),
        Err(error) => json!({"ok": false, "error": error}),
    }
}

pub fn auth(body: &Value) -> Value {
    let Some(code) = body.get("code").and_then(Value::as_str).filter(|code| (8..=4096).contains(&code.len())) else {
        return json!({"ok": false, "error": "a valid Epic authorization code is required"});
    };
    match run(&["auth", "--code", code], "legendary-auth.log") {
        Ok(_) if account_name().is_some() => status(),
        _ => json!({"ok": false, "error": "Epic authentication failed; sign in again or inspect legendary-auth.log"}),
    }
}

pub fn logout() -> Value {
    match run(&["auth", "--delete"], "legendary-auth.log") {
        Ok(_) => {
            let _ = fs::remove_file(library_cache());
            status()
        },
        Err(error) => json!({"ok": false, "error": error}),
    }
}

fn installed_map() -> Vec<Value> {
    let Ok(value) = serde_json::from_slice::<Value>(&fs::read(config().join("installed.json")).unwrap_or_default())
    else {
        return Vec::new();
    };
    match value {
        Value::Array(values) => values,
        Value::Object(values) => values.into_values().collect(),
        _ => Vec::new(),
    }
}

fn artwork(game: &Value) -> Option<&str> {
    let images = game.get("metadata")?.get("keyImages")?.as_array()?;
    for preferred in ["DieselGameBoxTall", "OfferImageTall", "Thumbnail"] {
        if let Some(url) = images
            .iter()
            .find_map(|image| (image.get("type")?.as_str()? == preferred).then(|| image.get("url")?.as_str()).flatten())
        {
            return Some(url);
        }
    }
    images.iter().find_map(|image| image.get("url")?.as_str())
}

fn bottle(name: &str) -> PathBuf {
    home().join("bottles").join(format!("epic_{name}"))
}

fn prefix(name: &str) -> PathBuf {
    bottle(name).join("prefix")
}

fn bottle_manifest(name: &str) -> Option<Value> {
    serde_json::from_slice(&fs::read(bottle(name).join("bottle.json")).ok()?).ok()
}

fn games_response(catalog: &[Value]) -> Value {
    let installed = installed_map();
    let games: Vec<Value> = catalog
        .iter()
        .filter_map(|game| {
            let name = game.get("app_name")?.as_str()?;
            let title = game.get("app_title")?.as_str()?;
            if !valid_app_name(name) {
                return None;
            }
            let local = installed.iter().find(|item| item.get("app_name").and_then(Value::as_str) == Some(name));
            let manifest = bottle_manifest(name);
            let pipeline = manifest
                .as_ref()
                .and_then(|value| value.get("preferred_pipeline"))
                .and_then(Value::as_str)
                .filter(|value| valid_pipeline(value))
                .unwrap_or("auto");
            let mouse_mode = manifest
                .as_ref()
                .and_then(|value| value.get("mouse_mode"))
                .and_then(Value::as_str)
                .filter(|value| valid_mouse_mode(value))
                .unwrap_or("no-recenter");
            Some(json!({
                "appName": name,
                "title": title,
                "version": game.get("asset_infos").and_then(|v| v.get("Windows")).and_then(|v| v.get("build_version")),
                "artworkUrl": artwork(game),
                "installed": local.is_some(),
                "bottleInitialized": prefix(name).join("system.reg").is_file() && manifest.is_some(),
                "pipeline": pipeline,
                "mouseMode": mouse_mode,
                "running": live_pid(name, "launch.pid").is_some(),
                "downloading": live_pid(name, "pid").is_some(),
                "installPath": local.and_then(|v| v.get("install_path")),
                "executable": local.and_then(|v| v.get("executable")),
                "installSize": local.and_then(|v| v.get("install_size")).and_then(Value::as_u64).unwrap_or(0),
            }))
        })
        .collect();
    let account = account_name();
    json!({"ok": true, "authenticated": account.is_some(), "account": account, "gameRoot": game_root(), "games": games})
}

fn cached_catalog() -> Option<Vec<Value>> {
    serde_json::from_slice(&fs::read(library_cache()).ok()?).ok()
}

fn write_library_cache(raw: &str) -> Result<(), String> {
    fs::create_dir_all(root()).map_err(|error| error.to_string())?;
    let destination = library_cache();
    let temporary = destination.with_extension(format!("tmp.{}", std::process::id()));
    let result = (|| -> Result<(), String> {
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(&temporary)
            .map_err(|error| error.to_string())?;
        file.write_all(raw.as_bytes()).and_then(|_| file.sync_all()).map_err(|error| error.to_string())?;
        fs::rename(&temporary, &destination).map_err(|error| error.to_string())
    })();
    let _ = fs::remove_file(temporary);
    result
}

pub fn games(force: bool) -> Value {
    if account_name().is_none() {
        return json!({"ok": false, "error": "Epic account is not authenticated"});
    }
    if !force {
        if let Some(catalog) = cached_catalog() {
            return games_response(&catalog);
        }
    }
    let mut args = vec!["list", "--platform", "Windows", "--json"];
    if force {
        args.push("--force-refresh");
    }
    let raw = match run(&args, "legendary-sync.log") {
        Ok(output) => output,
        Err(_) => return cached_catalog().map_or_else(
            || json!({"ok": false, "error": "Epic library sync failed; sign in again or inspect legendary-sync.log"}),
            |catalog| games_response(&catalog),
        ),
    };
    let Ok(catalog) = serde_json::from_str::<Vec<Value>>(&raw) else {
        return cached_catalog().map_or_else(
            || json!({"ok": false, "error": "Legendary returned an invalid Epic library response"}),
            |catalog| games_response(&catalog),
        );
    };
    let _ = write_library_cache(&raw);
    games_response(&catalog)
}

pub fn sync_on_startup() {
    std::thread::spawn(|| {
        let _ = games(true);
    });
}

fn process_path(name: &str, suffix: &str) -> PathBuf {
    let directory = root().join("processes");
    let _ = fs::create_dir_all(&directory);
    directory.join(format!("{name}.{suffix}"))
}

fn live_pid(name: &str, suffix: &str) -> Option<u32> {
    let pid = fs::read_to_string(process_path(name, suffix)).ok()?.trim().parse::<u32>().ok()?;
    (pid > 1 && unsafe { libc::kill(pid as i32, 0) } == 0).then_some(pid)
}

fn spawn(
    args: &[&str],
    name: &str,
    suffix: &str,
    log_suffix: &str,
    prefix: Option<&Path>,
    pipeline: Option<&str>,
) -> Result<u32, String> {
    if live_pid(name, suffix).is_some() {
        return Err("an Epic operation is already running for this game".to_string());
    }
    fs::create_dir_all(config()).map_err(|error| error.to_string())?;
    let log_path = process_path(name, log_suffix);
    let stdout = File::create(&log_path).map_err(|error| error.to_string())?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;
    let mut command = command().ok_or_else(|| "Legendary is not installed".to_string())?;
    command.args(args).stdout(stdout).stderr(stderr);
    if let Some(prefix) = prefix {
        command.env("WINEPREFIX", prefix);
    }
    if let Some(pipeline) = pipeline {
        command.env("MS_GRAPHICS_BACKEND", pipeline);
    }
    unsafe {
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    fs::write(process_path(name, suffix), format!("{pid}\n")).map_err(|error| error.to_string())?;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(pid)
}

fn spawn_launch_supervisor(name: &str, wine: &str, prefix: &Path, pipeline: &str) -> Result<u32, String> {
    if live_pid(name, "launch.pid").is_some() {
        return Err("this Epic game is already running".to_string());
    }
    let tool = tool();
    let Some(tool) = tool.to_str() else { return Err("invalid Legendary path".to_string()) };
    let Some(prefix_text) = prefix.to_str() else { return Err("invalid Wine prefix".to_string()) };
    let wineserver_path = wineserver();
    let Some(wineserver) = wineserver_path.to_str() else { return Err("invalid Wineserver path".to_string()) };
    let log_path = process_path(name, "launch.log");
    let stdout = File::create(&log_path).map_err(|error| error.to_string())?;
    let stderr = stdout.try_clone().map_err(|error| error.to_string())?;
    let script = "\"$1\" launch \"$2\" --skip-version-check --wine \"$3\" --wine-prefix \"$4\"; launch_status=$?; WINEPREFIX=\"$4\" \"$5\" -w; exit $launch_status";
    let mut command = Command::new("/bin/sh");
    command
        .args(["-c", script, "metalsharp-epic-supervisor", tool, name, wine, prefix_text, wineserver])
        .env("LEGENDARY_CONFIG_PATH", config())
        .env("WINEPREFIX", prefix)
        .env("MS_GRAPHICS_BACKEND", pipeline)
        .stdout(stdout)
        .stderr(stderr);
    unsafe {
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    let mut child = command.spawn().map_err(|error| error.to_string())?;
    let pid = child.id();
    fs::write(process_path(name, "launch.pid"), format!("{pid}\n")).map_err(|error| error.to_string())?;
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(pid)
}

pub fn install(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let Some(root) = install_root(body) else {
        return json!({"ok": false, "error": "the selected Epic install location is not allowed or writable"});
    };
    let Some(root) = root.to_str() else { return json!({"ok": false, "error": "invalid Epic game root"}) };
    match spawn(
        &["-y", "install", name, "--platform", "Windows", "--base-path", root, "--skip-sdl", "--skip-dlcs"],
        name,
        "pid",
        "install.log",
        None,
        None,
    ) {
        Ok(pid) => json!({"ok": true, "appName": name, "pid": pid, "logPath": process_path(name, "install.log")}),
        Err(error) => json!({"ok": false, "error": error}),
    }
}

fn progress_percent(text: &str) -> f64 {
    text.split('%')
        .filter_map(|left| left.split_whitespace().last()?.parse::<f64>().ok())
        .rfind(|value| (0.0..=100.0).contains(value))
        .unwrap_or(0.0)
}

pub fn progress(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let log_path = process_path(name, "install.log");
    let log = fs::read_to_string(&log_path).unwrap_or_default();
    json!({
        "ok": true,
        "appName": name,
        "active": live_pid(name, "pid").is_some(),
        "percent": progress_percent(&log),
        "message": log.lines().last().unwrap_or(""),
        "logPath": log_path,
    })
}

pub fn cancel(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let stopped = match live_pid(name, "pid") {
        Some(pid) => unsafe {
            libc::kill(-(pid as i32), libc::SIGTERM) == 0 || libc::kill(pid as i32, libc::SIGTERM) == 0
        },
        None => true,
    };
    let _ = fs::remove_file(process_path(name, "pid"));
    if stopped {
        json!({"ok": true})
    } else {
        json!({"ok": false, "error": "could not stop the Epic game download"})
    }
}

fn wine() -> PathBuf {
    home().join("runtime/wine/bin/metalsharp-wine")
}

fn wineserver() -> PathBuf {
    home().join("runtime/wine/bin/wineserver")
}

fn configure_mouse(name: &str, mode: &str) -> bool {
    let mut command = Command::new(wine());
    command.env("WINEPREFIX", prefix(name));
    if mode == "no-recenter" {
        command.args([
            "reg",
            "add",
            r"HKCU\Software\Wine\DirectInput",
            "/v",
            "MouseWarpOverride",
            "/t",
            "REG_SZ",
            "/d",
            "disable",
            "/f",
        ]);
        command.status().map(|status| status.success()).unwrap_or(false)
    } else {
        let _ = command
            .args(["reg", "delete", r"HKCU\Software\Wine\DirectInput", "/v", "MouseWarpOverride", "/f"])
            .status();
        true
    }
}

pub fn initialize(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let Some(pipeline) = body.get("pipeline").and_then(Value::as_str).filter(|value| valid_pipeline(value)) else {
        return json!({"ok": false, "error": "valid Epic app, pipeline, and mouse mode are required"});
    };
    let Some(mouse_mode) = body.get("mouseMode").and_then(Value::as_str).filter(|value| valid_mouse_mode(value)) else {
        return json!({"ok": false, "error": "valid Epic app, pipeline, and mouse mode are required"});
    };
    let prefix = prefix(name);
    let initialized = fs::create_dir_all(&prefix).is_ok()
        && Command::new(wine())
            .args(["wineboot", "-u"])
            .env("WINEPREFIX", &prefix)
            .status()
            .map(|status| status.success())
            .unwrap_or(false)
        && configure_mouse(name, mouse_mode);
    let manifest = json!({
        "id": format!("epic_{name}"),
        "name": name,
        "bottle_type": "epic",
        "prefix_path": prefix,
        "preferred_pipeline": pipeline,
        "mouse_mode": mouse_mode,
    });
    if !initialized
        || fs::write(bottle(name).join("bottle.json"), serde_json::to_vec(&manifest).unwrap_or_default()).is_err()
    {
        return json!({"ok": false, "error": "could not initialize the isolated Epic game bottle"});
    }
    json!({"ok": true, "appName": name, "prefixPath": prefix, "pipeline": pipeline, "mouseMode": mouse_mode})
}

pub fn play(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let prefix = prefix(name);
    if live_pid(name, "launch.pid").is_some() {
        return json!({"ok": false, "error": "this Epic game is already running"});
    }
    let Some(manifest) = bottle_manifest(name) else {
        return json!({"ok": false, "error": "initialize this Epic game bottle before launching"});
    };
    let Some(pipeline) =
        manifest.get("preferred_pipeline").and_then(Value::as_str).filter(|value| valid_pipeline(value))
    else {
        return json!({"ok": false, "error": "initialize this Epic game bottle before launching"});
    };
    let Some(mouse_mode) = manifest.get("mouse_mode").and_then(Value::as_str).filter(|value| valid_mouse_mode(value))
    else {
        return json!({"ok": false, "error": "initialize this Epic game bottle before launching"});
    };
    if !prefix.join("system.reg").is_file() || !configure_mouse(name, mouse_mode) {
        return json!({"ok": false, "error": "could not apply Epic game mouse settings"});
    }
    let wine_path = wine();
    let Some(wine) = wine_path.to_str() else { return json!({"ok": false, "error": "invalid Wine path"}) };
    match spawn_launch_supervisor(name, wine, &prefix, pipeline) {
        Ok(pid) => {
            json!({"ok": true, "appName": name, "pid": pid, "prefixPath": prefix, "logPath": process_path(name, "launch.log")})
        },
        Err(error) => json!({"ok": false, "error": error}),
    }
}

pub fn stop(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    let success = Command::new(wineserver())
        .arg("-k")
        .env("WINEPREFIX", prefix(name))
        .status()
        .map(|status| status.success())
        .unwrap_or(false);
    let _ = fs::remove_file(process_path(name, "launch.pid"));
    if success {
        json!({"ok": true})
    } else {
        json!({"ok": false, "error": "could not stop Epic game bottle"})
    }
}

pub fn stop_all() -> Value {
    let mut stopped = 0u64;
    if let Ok(entries) = fs::read_dir(root().join("processes")) {
        for entry in entries.flatten() {
            let filename = entry.file_name();
            let filename = filename.to_string_lossy();
            let Some(name) = filename.strip_suffix(".launch.pid").filter(|name| valid_app_name(name)) else {
                continue;
            };
            if stop(&json!({"appName": name})).get("ok").and_then(Value::as_bool) == Some(true) {
                stopped += 1;
            }
        }
    }
    json!({"ok": true, "stopped": stopped})
}

pub fn uninstall(body: &Value) -> Value {
    let Some(name) = app_name(body) else { return json!({"ok": false, "error": "invalid Epic app name"}) };
    match run(&["-y", "uninstall", name], "legendary-uninstall.log") {
        Ok(_) => {
            if let Some(bottle) = prefix(name).parent() {
                let _ = fs::remove_dir_all(bottle);
            }
            json!({"ok": true})
        },
        Err(error) => json!({"ok": false, "error": error}),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn app_names_are_allowlisted() {
        assert_eq!(app_name(&json!({"appName": "Fortnite_Release-1"})), Some("Fortnite_Release-1"));
        assert_eq!(app_name(&json!({"appName": "../../escape"})), None);
        assert_eq!(app_name(&json!({"appName": "bad name"})), None);
    }

    #[test]
    fn bottle_options_are_allowlisted() {
        assert!(valid_pipeline("d3dmetal"));
        assert!(valid_pipeline("m11_32"));
        assert!(!valid_pipeline("../../escape"));
        assert!(valid_mouse_mode("no-recenter"));
        assert!(!valid_mouse_mode("warp-anywhere"));
    }

    #[test]
    fn legendary_release_is_pinned() {
        assert_eq!(VERSION, "0.21.0");
        assert!(URL.starts_with("https://github.com/legendary-gl/legendary/releases/download/0.21.0/"));
        assert_eq!(SHA256.len(), 64);
    }
}
