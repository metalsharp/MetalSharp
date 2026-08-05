use serde_json::{json, Value};
use std::path::{Component, Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};

static STEAM_INSTALLING: AtomicBool = AtomicBool::new(false);
const STEAMWEBHELPER_WRAPPER_MAX_BYTES: u64 = 100_000;
const STEAMWEBHELPER_WRAPPER_SHA256: &str = "f46a1e8c39c850ba22861f63559f13b4f68557acf04a92e6d1b899769b2ea1f9";
const STEAM_D3D12_GUARD_APPS: &[&str] = &["Steam.exe", "steamwebhelper.exe", "steamwebhelper_real.exe"];
const STEAM_D3D12_GUARD_DLLS: &[&str] = &["d3d12", "d3d12core", "d3d12SDKLayers", "dxcore"];

fn ms_wine() -> PathBuf {
    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");
    crate::platform::runtime_wine_binary(&ms_root)
}

fn steam_prefix() -> PathBuf {
    crate::platform::metalsharp_home_dir().join("prefix-steam")
}

fn steam_exe_path() -> PathBuf {
    resolve_steam_dir().join("Steam.exe")
}

fn resolve_steam_dir() -> PathBuf {
    let primary = steam_prefix().join("drive_c").join("Program Files (x86)").join("Steam");
    if primary.join("Steam.exe").exists() {
        return primary;
    }

    if let Ok(bottles_root) = crate::platform::metalsharp_home_dir().join("bottles").read_dir() {
        for entry in bottles_root.flatten() {
            let candidate = entry.path().join("prefix").join("drive_c").join("Program Files (x86)").join("Steam");
            if candidate.join("Steam.exe").exists() {
                eprintln!("steam: found Steam in Sharp Library bottle: {:?}", candidate);
                return candidate;
            }
        }
    }

    primary
}

fn macos_steam_app() -> Option<PathBuf> {
    let home = dirs::home_dir().unwrap_or_default();
    let candidates = [
        PathBuf::from("/Applications/Steam.app"),
        home.join("Applications/Steam.app"),
        home.join("Library/Application Support/Steam/Steam.AppBundle/Steam/Steam.app"),
    ];
    candidates.into_iter().find(|p| p.exists())
}

fn macos_steam_install_url() -> &'static str {
    "https://store.steampowered.com/about/"
}

pub fn status() -> Value {
    let home = dirs::home_dir().unwrap_or_default();

    let wine_steam_dir = steam_prefix().join("drive_c").join("Program Files (x86)").join("Steam");
    let wine_steam_exe = wine_steam_dir.join("Steam.exe");
    let windows_installed = wine_steam_exe.exists();
    let windows_path = if windows_installed { Some(wine_steam_dir.to_string_lossy().to_string()) } else { None };

    let login_state = detect_login_state();

    let mac_paths = [
        home.join(".steam/steam/steamapps"),
        home.join(".local/share/Steam/steamapps"),
        home.join("Library/Application Support/Steam/steamapps"),
    ];
    let mac_app = macos_steam_app();
    let mac_installed = mac_app.is_some() || mac_paths.iter().any(|p| p.exists());
    let mac_running = is_macos_steam_running();

    let running = is_wine_steam_running();
    let ms_available = ms_wine().exists();
    let installing = is_installing_steam();

    json!({
        "installed": windows_installed,
        "path": windows_path,
        "login_state": login_state,
        "mac_installed": mac_installed,
        "mac_path": mac_app.map(|p| p.to_string_lossy().to_string()),
        "mac_install_url": macos_steam_install_url(),
        "mac_running": mac_running,
        "running": running,
        "metalsharp_wine_available": ms_available,
        "installing": installing
    })
}

pub fn is_wine_steam_running() -> bool {
    process_lines()
        .iter()
        .filter_map(|line| parse_process_line(line))
        .any(|(_, command)| is_wine_steam_owner_command(command))
}

fn process_lines() -> Vec<String> {
    Command::new("ps")
        .args(["axo", "pid=,command="])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.lines().map(str::to_string).collect())
        .unwrap_or_default()
}

fn is_wine_steam_process_line(line: &str) -> bool {
    parse_process_line(line).map(|(_, command)| is_wine_steam_owner_command(command)).unwrap_or(false)
}

fn is_wine_steam_owner_command(command: &str) -> bool {
    if command.contains(" rg ") || command.contains("rg -i") || command.contains("ps axo") {
        return false;
    }
    if command.contains("Steam.app/Contents/MacOS") || command.contains("steam_osx") {
        return false;
    }

    let prefix = steam_prefix().to_string_lossy().to_string();
    let exe = steam_exe_path().to_string_lossy().to_string();
    let lower = command.to_lowercase();

    command.contains(&exe)
        || (command.contains(&prefix) && (command.contains("Steam.exe") || command.contains("steam.exe")))
        || (lower.contains("c:\\program files (x86)\\steam")
            && (lower.contains("steam.exe")
                || lower.contains("steamservice.exe")
                || lower.contains("steamerrorreporter")))
}

fn is_wine_steam_cleanup_command(command: &str) -> bool {
    if is_wine_steam_owner_command(command) {
        return true;
    }
    if command.contains(" rg ") || command.contains("rg -i") || command.contains("ps axo") {
        return false;
    }
    if command.contains("Steam.app/Contents/MacOS") || command.contains("steam_osx") {
        return false;
    }

    let prefix = steam_prefix().to_string_lossy().to_string();
    let ms_home = crate::platform::metalsharp_home_dir().to_string_lossy().to_string();
    let lower = command.to_lowercase();

    // Isolation contract: generic wine helper tokens (wineserver, wineloader,
    // winedevice.exe) must NOT be killed on their own — a foreign Wine
    // launcher (CrossOver, SakuraGiri, Whisky, GPTK) also runs those names.
    // Only treat them as MetalSharp Steam processes when the command line
    // also references the MetalSharp home (runtime/prefix/bottles all live
    // under it), the MS wine runtime, or a Steam path inside the MS prefix.
    let ms_owned = command.contains(&prefix)
        || command.contains(&ms_home)
        || lower.contains("metalsharp-wine")
        || lower.contains("c:\\program files (x86)\\steam");

    command.contains(&prefix)
        || lower.contains("c:\\program files (x86)\\steam")
        || lower.contains("steamwebhelper.exe")
        || lower.contains("steamwebhelper_real.exe")
        || lower.contains("c:\\windows\\system32\\explorer.exe /desktop")
        || (lower.contains("c:\\windows\\system32\\conhost.exe") && lower.contains("--headless"))
        || (ms_owned
            && (lower.contains("winedevice.exe") || lower.contains("wineserver") || lower.contains("wineloader")))
}

fn wine_steam_cleanup_pids() -> Vec<u32> {
    let this_pid = std::process::id();
    process_lines()
        .iter()
        .filter_map(|line| parse_process_line(line))
        .filter_map(|(pid, command)| (pid != this_pid && is_wine_steam_cleanup_command(command)).then_some(pid))
        .collect()
}

pub fn is_macos_steam_running() -> bool {
    process_lines()
        .iter()
        .filter_map(|line| parse_process_line(line))
        .any(|(_, command)| is_macos_steam_active_command(command))
}

fn latest_macos_steam_pid() -> u32 {
    macos_steam_process_pids().into_iter().max().unwrap_or(0)
}

fn parse_process_line(line: &str) -> Option<(u32, &str)> {
    let line = line.trim_start();
    let mut parts = line.splitn(2, char::is_whitespace);
    let pid = parts.next()?.parse::<u32>().ok()?;
    let command = parts.next().unwrap_or("").trim_start();
    Some((pid, command))
}

fn is_macos_steam_search_command(command: &str) -> bool {
    if command.contains(" rg ") || command.contains("rg -i") || command.contains("ps axo") {
        return true;
    }

    false
}

fn is_macos_steam_active_command(command: &str) -> bool {
    if is_macos_steam_search_command(command) {
        return false;
    }

    command.contains("/Steam.app/Contents/MacOS/steam_osx")
        || command.ends_with("/steam_osx")
        || command.contains("Steam Helper.app/Contents/MacOS")
}

fn is_macos_steam_cleanup_command(command: &str) -> bool {
    if is_macos_steam_search_command(command) {
        return false;
    }

    is_macos_steam_active_command(command) || command.contains("Steam.AppBundle/Steam/Contents/MacOS/ipcserver")
}

fn macos_steam_process_pids() -> Vec<u32> {
    process_lines()
        .iter()
        .filter_map(|line| parse_process_line(line))
        .filter_map(|(pid, command)| is_macos_steam_cleanup_command(command).then_some(pid))
        .collect()
}

pub fn is_installing_steam() -> bool {
    STEAM_INSTALLING.load(Ordering::SeqCst)
}

fn cef_subdirs(steam_dir: &Path) -> Vec<PathBuf> {
    let cef_root = steam_dir.join("bin").join("cef");
    let mut dirs = Vec::new();
    let priority = ["cef.win64", "cef.win7x64", "cef.win7"];
    for name in &priority {
        let d = cef_root.join(name);
        if d.is_dir() {
            dirs.push(d);
        }
    }
    if let Ok(entries) = std::fs::read_dir(&cef_root) {
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_dir() && !dirs.contains(&p) {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.starts_with("cef.") {
                    dirs.push(p);
                }
            }
        }
    }
    dirs
}

fn ensure_steam_launch_ready(steam_dir: &PathBuf) {
    for cef_dir in cef_subdirs(steam_dir) {
        let wrapper = cef_dir.join("steamwebhelper.exe");
        let real = cef_dir.join("steamwebhelper_real.exe");

        let wrapper_size = std::fs::metadata(&wrapper).map(|m| m.len()).unwrap_or(0);
        let real_size = std::fs::metadata(&real).map(|m| m.len()).unwrap_or(0);

        let wrapper_missing = wrapper_size == 0;
        let wrapper_overwritten = wrapper_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES;
        let real_missing_or_bad = real_size > 0 && real_size < STEAMWEBHELPER_WRAPPER_MAX_BYTES;

        if wrapper_missing || wrapper_overwritten || real_missing_or_bad {
            deploy_steamwebhelper_wrapper(steam_dir);
            return;
        }
    }
}

fn build_steam_d3d12_guard_reg() -> String {
    let mut reg = String::from("Windows Registry Editor Version 5.00\r\n");
    for app in STEAM_D3D12_GUARD_APPS {
        reg.push_str(&format!("\r\n[HKEY_CURRENT_USER\\Software\\Wine\\AppDefaults\\{}\\DllOverrides]\r\n", app));
        for dll in STEAM_D3D12_GUARD_DLLS {
            reg.push_str(&format!("\"{}\"=\"builtin\"\r\n", dll));
        }
    }
    reg
}

fn seed_steam_d3d12_guard(prefix: &Path, ms_root: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let wine = crate::platform::runtime_wine_binary(ms_root);
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let reg_file_name = "metalsharp-steam-d3d12-guard.reg";
    let reg_file = prefix.join("drive_c").join(reg_file_name);
    std::fs::write(&reg_file, build_steam_d3d12_guard_reg())?;
    let reg_file_win = format!("C:\\{}", reg_file_name);
    let mut cmd = Command::new(&wine);
    cmd.arg("reg")
        .arg("import")
        .arg(&reg_file_win)
        .env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut cmd, ms_root);
    let status = cmd.status()?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("regedit exited with {}", status).into())
    }
}

fn resolve_steam_prefix() -> PathBuf {
    let steam_dir = resolve_steam_dir();
    if steam_dir.starts_with(steam_prefix()) {
        return steam_prefix();
    }
    if let Ok(entries) = crate::platform::metalsharp_home_dir().join("bottles").read_dir() {
        for entry in entries.flatten() {
            let bottle_prefix = entry.path().join("prefix");
            let bottle_steam = bottle_prefix.join("drive_c").join("Program Files (x86)").join("Steam");
            if bottle_steam == steam_dir {
                return bottle_prefix;
            }
        }
    }
    steam_prefix()
}

fn spawn_wine_steam(args: &[&str]) -> Result<u32, Box<dyn std::error::Error>> {
    spawn_wine_steam_with_env(args, &[])
}

fn use_legacy_compatible_opengl_context(cmd: &mut Command) {
    // Wine's macOS driver treats the presence of this variable as a request
    // for a forward-compatible context. Applying it to Steam globally also
    // forces every child game into core-profile GLSL semantics, which breaks
    // legacy OpenGL titles. Route-specific env can opt back in below.
    cmd.env_remove("MS_FWD_COMPAT_GL_CTX");
}

fn steam_wine_debug_value(configured: Option<String>) -> String {
    configured.filter(|value| !value.trim().is_empty()).unwrap_or_else(|| "-all".to_string())
}

fn spawn_wine_steam_with_env(args: &[&str], extra_env: &[(String, String)]) -> Result<u32, Box<dyn std::error::Error>> {
    let wine = ms_wine();
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let steam_dir = resolve_steam_dir();
    let exe = steam_dir.join("Steam.exe");
    let prefix = resolve_steam_prefix();

    if !exe.exists() || !steam_dir.join("steamui.dll").exists() {
        return Err("Steam is not installed — use the setup wizard to install it first".into());
    }

    ensure_steam_launch_ready(&steam_dir);

    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");
    if let Err(err) = seed_steam_d3d12_guard(&prefix, &ms_root) {
        eprintln!("steam: WARNING — failed to seed Steam D3D12 guard AppDefaults: {}", err);
    }
    if let Err(err) = crate::mtsp::input_shims::deploy_current_for_prefix(&prefix) {
        eprintln!("steam: WARNING — controller input shim deploy into prefix failed: {}", err);
    }
    if !crate::installer::moltenvk_ready(&ms_root) {
        eprintln!("steam: WARNING — MoltenVK not found in Wine runtime, Steam webhelper UI may fail to render");
    }

    let prefix_str = prefix.to_string_lossy().to_string();

    let log_dir = crate::platform::metalsharp_home_dir().join("logs");
    let _ = std::fs::create_dir_all(&log_dir);
    let log_path = log_dir.join(format!(
        "steam-{}.log",
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs()
    ));
    let log_file = match std::fs::File::create(&log_path) {
        Ok(f) => {
            eprintln!("steam: tracing to {}", log_path.display());
            Some(f)
        },
        Err(e) => {
            eprintln!("steam: could not create log file {}: {}", log_path.display(), e);
            None
        },
    };

    let mut cmd = Command::new(&wine);
    cmd.current_dir(&steam_dir)
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", steam_wine_debug_value(std::env::var("METALSHARP_WINEDEBUG").ok()))
        .env("WINEDEBUGGER", "none")
        .env("STEAM_RUNTIME", "0")
        .env(
            "WINEDLLOVERRIDES",
            "dxgi,d3d11,d3d10core=n,b;bcrypt=b;ncrypt=b;gameoverlayrenderer,gameoverlayrenderer64=d",
        )
        .arg(&exe)
        .args(args)
        .stdout(std::process::Stdio::null());

    if let Some(f) = log_file {
        cmd.stderr(std::process::Stdio::from(f));
    } else {
        cmd.stderr(std::process::Stdio::null());
    }

    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
    use_legacy_compatible_opengl_context(&mut cmd);

    for (key, val) in extra_env {
        cmd.env(key, val);
    }

    if std::env::var_os("MS_LAUNCH_TRACE").is_some() {
        eprintln!("[trace] wine_bin={}", wine.display());
        eprintln!("[trace] prefix={}", prefix_str);
        eprintln!("[trace] cwd={}", steam_dir.display());
        eprintln!("[trace] parent WINEPREFIX={:?}", std::env::var("WINEPREFIX"));
        eprintln!("[trace] parent WINEDLLPATH={:?}", std::env::var("WINEDLLPATH"));
        eprintln!("[trace] parent DYLD_FALLBACK_LIBRARY_PATH={:?}", std::env::var("DYLD_FALLBACK_LIBRARY_PATH"));
        eprintln!("[trace] parent DYLD_LIBRARY_PATH={:?}", std::env::var("DYLD_LIBRARY_PATH"));
        eprintln!("[trace] parent WINESERVER={:?}", std::env::var("WINESERVER"));
        eprintln!("[trace] parent WINELOADER={:?}", std::env::var("WINELOADER"));
    }

    let child = cmd.spawn()?;

    Ok(child.id())
}

pub fn launch_wine_steam() -> Result<Value, Box<dyn std::error::Error>> {
    launch_wine_steam_with_env(&[])
}

pub fn launch_wine_steam_with_env(extra_env: &[(String, String)]) -> Result<Value, Box<dyn std::error::Error>> {
    let wine = ms_wine();
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let steam_dir = resolve_steam_dir();
    let exe = steam_dir.join("Steam.exe");

    if !exe.exists() || !steam_dir.join("steamui.dll").exists() {
        return Err("Steam is not installed — use the setup wizard to install it first".into());
    }

    if is_wine_steam_running() {
        if !extra_env.is_empty() {
            return Err(
                "Wine Steam is already running; per-game environment cannot be inherited without restarting Steam"
                    .into(),
            );
        }
        return Ok(json!({
            "ok": true,
            "message": "Steam already running"
        }));
    }

    ensure_steam_launch_ready(&steam_dir);

    let _ = crate::kernel_translation::ipc_bridge::start_ipc_listener();

    let pid = spawn_wine_steam_with_env(
        &["-no-cef-sandbox", "-cef-single-process", "-noverifyfiles", "-no-dwrite"],
        extra_env,
    )?;

    Ok(json!({"ok": true, "pid": pid}))
}

pub fn launch_macos_steam() -> Result<Value, Box<dyn std::error::Error>> {
    if macos_steam_app().is_none() {
        return Err("macOS Steam is not installed".into());
    }
    if is_wine_steam_running() {
        return Err("Wine Steam is running. Stop Wine Steam before launching macOS Steam.".into());
    }

    let child = Command::new("open")
        .args(["-a", "Steam", "steam://open/library"])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()?;

    Ok(json!({"ok": true, "pid": latest_macos_steam_pid().max(child.id())}))
}

pub fn install_macos_steam() -> Result<Value, Box<dyn std::error::Error>> {
    if let Some(app) = macos_steam_app() {
        return Ok(json!({"ok": true, "installed": true, "path": app.to_string_lossy()}));
    }

    let child = Command::new("open")
        .arg(macos_steam_install_url())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()?;

    Ok(json!({"ok": true, "installed": false, "pid": child.id(), "url": macos_steam_install_url()}))
}

pub fn stop_macos_steam() -> Result<Value, Box<dyn std::error::Error>> {
    let _ = Command::new("osascript")
        .args(["-e", "tell application \"Steam\" to quit"])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    std::thread::sleep(std::time::Duration::from_secs(2));

    let term_pids = macos_steam_process_pids();
    if !term_pids.is_empty() {
        for pid in term_pids {
            let _ = Command::new("kill")
                .args(["-TERM", &pid.to_string()])
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }

    let kill_pids = macos_steam_process_pids();
    if !kill_pids.is_empty() {
        for pid in kill_pids {
            let _ = Command::new("kill")
                .args(["-KILL", &pid.to_string()])
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }

    Ok(json!({"ok": true, "running": is_macos_steam_running()}))
}

pub fn launch_macos_steam_game(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    if is_wine_steam_running() {
        return Err("Wine Steam is running. Stop Wine Steam before launching through MacOS Steam.".into());
    }
    if !is_macos_game_installed(appid) {
        return Err(
            "This game is not installed in macOS Steam. Download it through macOS Steam before using the MacOS Steam engine."
                .into(),
        );
    }

    if !is_macos_steam_running() {
        launch_macos_steam()?;
        for _ in 0..20 {
            std::thread::sleep(std::time::Duration::from_millis(500));
            if is_macos_steam_running() {
                break;
            }
        }
    }

    let url = format!("steam://run/{}", appid);
    let child = Command::new("open")
        .arg(&url)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()?;

    Ok(json!({"ok": true, "pid": latest_macos_steam_pid().max(child.id()), "appid": appid}))
}

pub fn stop_wine_steam() -> Result<Value, Box<dyn std::error::Error>> {
    let term_pids = wine_steam_cleanup_pids();
    for pid in term_pids {
        let _ = Command::new("kill")
            .args(["-TERM", &pid.to_string()])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    std::thread::sleep(std::time::Duration::from_secs(1));

    let kill_pids = wine_steam_cleanup_pids();
    for pid in kill_pids {
        let _ = Command::new("kill")
            .args(["-KILL", &pid.to_string()])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    if !wine_steam_cleanup_pids().is_empty() {
        std::thread::sleep(std::time::Duration::from_millis(500));
    }

    let _ = crate::kernel_translation::ipc_bridge::stop_ipc_listener();

    Ok(json!({"ok": true, "running": is_wine_steam_running()}))
}

/// Phase 7: report what `stop_wine_steam` would target WITHOUT actually
/// killing anything. Makes the cleanup filter observable so "stop Wine Steam"
/// is provably scoped to real Wine Steam helper processes (steamwebhelper,
/// winedevice, wineserver, headless conhost, the detached Wine desktop), not
/// a broad destructive process kill. Also lists the processes that are
/// explicitly EXCLUDED (the macOS Steam client, and our own ripgrep/ps).
pub fn stop_wine_steam_targets() -> Value {
    let pids = wine_steam_cleanup_pids();
    let this_pid = std::process::id();
    let lines = process_lines();
    let mut targeted = Vec::new();
    let mut excluded = Vec::new();
    for line in &lines {
        let Some((pid, command)) = parse_process_line(line) else {
            continue;
        };
        if pid == this_pid {
            continue;
        }
        if is_wine_steam_cleanup_command(command) {
            targeted.push(json!({"pid": pid, "command": command}));
        } else if command.contains("Steam.app/Contents/MacOS")
            || command.contains("steam_osx")
            || command.contains(" rg ")
            || command.contains("rg -i")
            || command.contains("ps axo")
        {
            // These are explicitly excluded so a reviewer can prove the stop
            // filter never touches the macOS Steam client or our own tools.
            excluded.push(json!({"pid": pid, "command": command}));
        }
    }
    json!({
        "ok": true,
        "targeted_pid_count": pids.len(),
        "targeted": targeted,
        "excluded": excluded,
        "summary": format!(
            "stop_wine_steam targets {} Wine Steam helper process(es); the macOS Steam client and MetalSharp's own rg/ps invocations are excluded",
            pids.len()
        ),
    })
}

pub fn install_game_via_steam(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    if !is_wine_steam_running() {
        launch_wine_steam()?;
        std::thread::sleep(std::time::Duration::from_secs(5));
    }

    let name = get_game_name_from_manifest(appid).unwrap_or_else(|| format!("Game {}", appid));
    let pipeline = crate::mtsp::rules::resolve_pipeline(appid);
    let bottle = crate::bottles::refresh_steam_game_bottle(appid, &name, None, pipeline).ok();

    Ok(json!({"ok": true, "appid": appid, "method": "steam_ui", "bottle_id": bottle.map(|b| b.id)}))
}

pub fn launch_game_via_steam(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    launch_game_via_steam_with_env(appid, &[])
}

pub fn ensure_wine_steam_ready_for_game_launch() -> Result<bool, Box<dyn std::error::Error>> {
    if is_wine_steam_running() {
        return Ok(false);
    }

    launch_wine_steam()?;
    for _ in 0..12 {
        if is_wine_steam_running() {
            std::thread::sleep(std::time::Duration::from_secs(2));
            return Ok(true);
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }

    Err("Wine Steam was started but did not become ready for game launch".into())
}

pub fn launch_game_via_steam_with_env(
    appid: u32,
    extra_env: &[(String, String)],
) -> Result<Value, Box<dyn std::error::Error>> {
    let wine = ms_wine();
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }
    let steam_running = is_wine_steam_running();
    if steam_running && !extra_env.is_empty() {
        return Err(
            "Wine Steam is already running; route-specific environment cannot be inherited without restarting Steam"
                .into(),
        );
    }
    if !steam_running {
        launch_wine_steam_with_env(extra_env)?;
        let mut ready = false;
        for _ in 0..12 {
            if is_wine_steam_running() {
                std::thread::sleep(std::time::Duration::from_secs(2));
                ready = true;
                break;
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
        if !ready {
            return Err("Wine Steam was started but did not become ready for game launch".into());
        }
    }

    let url = format!("steam://run/{}", appid);
    let pid = spawn_wine_steam_with_env(&[&url], extra_env)?;

    Ok(json!({"ok": true, "pid": pid, "appid": appid}))
}

pub fn view_game_in_steam(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    let wine = ms_wine();
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }
    if !is_wine_steam_running() {
        return Err("Steam is not running".into());
    }

    let url = format!("steam://nav/games/details/{}", appid);
    let _ = spawn_wine_steam(&[&url])?;

    Ok(json!({"ok": true, "appid": appid}))
}

pub fn get_wine_steam_installed_games() -> Vec<u32> {
    let mut appids = Vec::new();

    for steamapps in crate::scan::wine_steam_library_paths() {
        if is_macos_steamapps_path(&steamapps) {
            continue;
        }
        if let Ok(entries) = std::fs::read_dir(&steamapps) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.starts_with("appmanifest_") && name.ends_with(".acf") {
                    if let Some(id_str) = name.strip_prefix("appmanifest_").and_then(|s| s.strip_suffix(".acf")) {
                        if let Ok(id) = id_str.parse::<u32>() {
                            if !appids.contains(&id) {
                                appids.push(id);
                            }
                        }
                    }
                }
            }
        }
    }

    appids
}

pub fn deploy_steamwebhelper_wrapper(steam_dir: &PathBuf) {
    let wrapper = match find_bundled_steamwebhelper_wrapper() {
        Some(wrapper) => wrapper,
        None => match download_steamwebhelper_wrapper_fallback() {
            Some(wrapper) => wrapper,
            None => return,
        },
    };

    let bundled_size = std::fs::metadata(&wrapper).map(|m| m.len()).unwrap_or(0);

    if bundled_size == 0 || bundled_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES || !steamwebhelper_wrapper_valid(&wrapper) {
        return;
    }

    for cef_dir in cef_subdirs(steam_dir) {
        let original = cef_dir.join("steamwebhelper.exe");
        let real = cef_dir.join("steamwebhelper_real.exe");
        let wrapper_marker = cef_dir.join(".ms_wrapper_deployed");

        let original_size = std::fs::metadata(&original).map(|m| m.len()).unwrap_or(0);
        let real_size = std::fs::metadata(&real).map(|m| m.len()).unwrap_or(0);

        if original_size > 0 && original_size <= STEAMWEBHELPER_WRAPPER_MAX_BYTES {
            if !wrapper_marker.exists() {
                let _ = std::fs::write(&wrapper_marker, "deployed");
            }
            continue;
        }

        if real_size < STEAMWEBHELPER_WRAPPER_MAX_BYTES {
            if original_size > STEAMWEBHELPER_WRAPPER_MAX_BYTES {
                let _ = std::fs::remove_file(&real);
                let _ = std::fs::rename(&original, &real);
            } else {
                continue;
            }
        } else if original.exists() {
            let _ = std::fs::remove_file(&original);
        }

        if std::fs::copy(&wrapper, &original).is_ok() {
            let _ = std::fs::write(&wrapper_marker, "deployed");
        }
    }
}

fn find_bundled_steamwebhelper_wrapper() -> Option<PathBuf> {
    find_bundled_steam_asset("steamwebhelper.exe").filter(|path| steamwebhelper_wrapper_valid(path))
}

fn find_bundled_steam_asset(filename: &str) -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("steam");
    let cached = cache_dir.join(filename);
    if cached.exists() {
        return Some(cached);
    }

    let archive = find_steam_bundle_archive()?;
    let _ = std::fs::remove_dir_all(&cache_dir);
    let _ = std::fs::create_dir_all(&cache_dir);
    let tmp = cache_dir.with_extension("download");
    let _ = std::fs::remove_dir_all(&tmp);
    let _ = std::fs::create_dir_all(&tmp);

    let status = Command::new("tar")
        .args(["--use-compress-program=unzstd", "-xf"])
        .arg(&archive)
        .arg("-C")
        .arg(&tmp)
        .status()
        .ok()?;
    if !status.success() {
        let _ = std::fs::remove_dir_all(&tmp);
        return None;
    }

    let steam_root = tmp.join("steam");
    if steam_root.exists() {
        let _ = copy_dir_recursive_steam(&steam_root, &cache_dir);
    }
    let _ = std::fs::remove_dir_all(&tmp);

    cached.exists().then_some(cached)
}

fn find_steam_bundle_archive() -> Option<PathBuf> {
    let filename = "metalsharp-steam.tar.zst";
    if let Some(resources) = crate::platform::app_resources_dir() {
        let archive = resources.join("bundles").join(filename);
        if archive.exists() {
            return Some(archive);
        }
    }
    let dev = PathBuf::from("app/bundles").join(filename);
    if dev.exists() {
        return Some(dev);
    }

    let home = dirs::home_dir()?;
    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("bundles");
    let _ = std::fs::create_dir_all(&cache_dir);
    let cached = cache_dir.join(filename);
    if cached.exists() {
        return Some(cached);
    }

    let url = format!("https://github.com/aaf2tbz/metalsharp/releases/download/bundles/{}", filename);
    let tmp = cached.with_extension("download");
    let output = Command::new("curl")
        .args(["--fail", "--location", "--silent", "--show-error", "--retry", "3", "-o"])
        .arg(&tmp)
        .arg(url)
        .output()
        .ok()?;
    if output.status.success() && tmp.exists() && std::fs::rename(&tmp, &cached).is_ok() {
        return Some(cached);
    }
    let _ = std::fs::remove_file(&tmp);
    None
}

fn download_steamwebhelper_wrapper_fallback() -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("steam");
    let _ = std::fs::create_dir_all(&cache_dir);
    let cached = cache_dir.join("steamwebhelper.exe");
    if cached.exists() && steamwebhelper_wrapper_valid(&cached) {
        return Some(cached);
    }

    let url = "https://github.com/aaf2tbz/metalsharp/releases/download/bundles/metalsharp-steam.tar.zst";
    let archive_tmp = cache_dir.join("metalsharp-steam.tar.zst.download");
    let archive_path = cache_dir.join("metalsharp-steam.tar.zst");

    let output = Command::new("curl")
        .args(["--fail", "--location", "--silent", "--show-error", "--retry", "3", "-o"])
        .arg(&archive_tmp)
        .arg(url)
        .output()
        .ok()?;
    if !output.status.success() || !archive_tmp.exists() {
        let _ = std::fs::remove_file(&archive_tmp);
        return None;
    }
    let _ = std::fs::rename(&archive_tmp, &archive_path);

    let extract_tmp = cache_dir.join("steam-extract-download");
    let _ = std::fs::remove_dir_all(&extract_tmp);
    let _ = std::fs::create_dir_all(&extract_tmp);

    let file = std::fs::File::open(&archive_path).ok()?;
    let mut decoder = zstd::Decoder::new(file).ok()?;

    let mut tar_cmd = Command::new("tar")
        .args(["-xf", "-"])
        .arg("-C")
        .arg(&extract_tmp)
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .ok()?;

    if let Some(mut stdin) = tar_cmd.stdin.take() {
        let _ = std::io::copy(&mut decoder, &mut stdin);
        drop(stdin);
    }

    let status = tar_cmd.wait().ok()?;
    if !status.success() {
        let _ = std::fs::remove_dir_all(&extract_tmp);
        return None;
    }

    let extracted = extract_tmp.join("steam").join("steamwebhelper.exe");
    if extracted.exists() && std::fs::copy(&extracted, &cached).is_ok() && steamwebhelper_wrapper_valid(&cached) {
        let _ = std::fs::remove_dir_all(&extract_tmp);
        return Some(cached);
    }

    let _ = std::fs::remove_dir_all(&extract_tmp);
    None
}

fn copy_dir_recursive_steam(src: &Path, dst: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        if src_path.is_dir() {
            copy_dir_recursive_steam(&src_path, &dst_path)?;
        } else {
            std::fs::copy(&src_path, &dst_path)?;
        }
    }
    Ok(())
}

fn steamwebhelper_wrapper_valid(path: &Path) -> bool {
    let size = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
    if size == 0 || size > STEAMWEBHELPER_WRAPPER_MAX_BYTES {
        return false;
    }

    file_sha256(path).as_deref() == Some(STEAMWEBHELPER_WRAPPER_SHA256)
}

fn file_sha256(path: &Path) -> Option<String> {
    for (program, args) in [("shasum", vec!["-a", "256"]), ("sha256sum", Vec::new())] {
        let Ok(output) = Command::new(program).args(args).arg(path).output() else {
            continue;
        };
        if !output.status.success() {
            continue;
        }
        let stdout = String::from_utf8(output.stdout).ok()?;
        let hash = stdout.split_whitespace().next()?.to_string();
        if hash.len() == 64 {
            return Some(hash);
        }
    }
    None
}

pub fn get_game_name_from_manifest(appid: u32) -> Option<String> {
    let manifest_name = format!("appmanifest_{}.acf", appid);

    for steamapps in crate::scan::wine_steam_library_paths() {
        let manifest_path = steamapps.join(&manifest_name);
        if let Ok(contents) = std::fs::read_to_string(&manifest_path) {
            if let Some(name) = parse_acf_field(&contents, "name") {
                return Some(name);
            }
        }
    }

    None
}

fn parse_acf_field(contents: &str, key: &str) -> Option<String> {
    for line in contents.lines() {
        let mut quoted = line.trim().split('"');
        let _ = quoted.next();
        let Some(parsed_key) = quoted.next() else {
            continue;
        };
        if parsed_key == key {
            let _ = quoted.next();
            if let Some(value) = quoted.next().map(str::trim).filter(|value| !value.is_empty()) {
                return Some(value.to_string());
            }
        }
    }
    None
}

fn is_single_path_component(name: &str) -> bool {
    let mut components = Path::new(name).components();
    matches!(components.next(), Some(Component::Normal(_))) && components.next().is_none()
}

fn canonical_path(path: &Path) -> Option<PathBuf> {
    std::fs::canonicalize(path).ok()
}

fn path_is_under(path: &Path, root: &Path) -> bool {
    match (canonical_path(path), canonical_path(root)) {
        (Some(path), Some(root)) => path != root && path.starts_with(root),
        _ => false,
    }
}

fn is_macos_steamapps_path(path: &Path) -> bool {
    crate::scan::macos_steam_library_paths().iter().any(|macos_path| {
        match (canonical_path(path), canonical_path(macos_path)) {
            (Some(path), Some(macos_path)) => path == macos_path,
            _ => false,
        }
    })
}

fn remove_dir_all_under(target: &Path, root: &Path) -> Result<bool, Box<dyn std::error::Error>> {
    if !target.exists() {
        return Ok(false);
    }
    if !path_is_under(target, root) {
        return Err(format!("Refusing to remove path outside uninstall root: {}", target.display()).into());
    }
    std::fs::remove_dir_all(target)?;
    Ok(true)
}

fn remove_file_under(target: &Path, root: &Path) -> Result<bool, Box<dyn std::error::Error>> {
    if !target.exists() {
        return Ok(false);
    }
    if !path_is_under(target, root) {
        return Err(format!("Refusing to remove file outside uninstall root: {}", target.display()).into());
    }
    std::fs::remove_file(target)?;
    Ok(true)
}

pub fn uninstall_game(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let manifest_name = format!("appmanifest_{}.acf", appid);
    let mut removed_wine = false;
    let mut removed_local = false;

    let _ = crate::launch::kill_game_with_pid(appid, 0);

    for steamapps in crate::scan::wine_steam_library_paths() {
        if is_macos_steamapps_path(&steamapps) {
            eprintln!("Refusing to uninstall appid {} from macOS Steam library {}", appid, steamapps.display());
            continue;
        }

        let manifest_path = steamapps.join(&manifest_name);
        if manifest_path.exists() {
            let contents = std::fs::read_to_string(&manifest_path).unwrap_or_default();
            let install_dir = parse_acf_field(&contents, "installdir");

            if let Some(dir_name) = install_dir {
                if !is_single_path_component(&dir_name) {
                    return Err(format!("Refusing unsafe Steam install dir for appid {}: {}", appid, dir_name).into());
                }
                let game_dir = steamapps.join("common").join(&dir_name);
                if remove_dir_all_under(&game_dir, &steamapps.join("common"))? {
                    removed_wine = true;
                }
            }
            if remove_file_under(&manifest_path, &steamapps)? {
                removed_wine = true;
            }
            break;
        }
    }

    let local_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());
    if remove_dir_all_under(&local_dir, &crate::platform::metalsharp_home_dir_for(&home).join("games"))? {
        removed_local = true;
    }

    if !removed_wine && !removed_local && is_macos_game_installed(appid) {
        return Err("This game is installed in macOS Steam. Uninstall it from macOS Steam, or install the Windows copy before using MetalSharp uninstall.".into());
    }

    if !removed_wine && !removed_local {
        return Err("No Windows Steam or MetalSharp local install was found to uninstall.".into());
    }

    Ok(json!({"ok": true, "appid": appid, "wine_removed": removed_wine, "local_removed": removed_local}))
}

pub fn is_macos_game_installed(appid: u32) -> bool {
    let manifest_name = format!("appmanifest_{}.acf", appid);
    crate::scan::macos_steam_library_paths().iter().any(|steamapps| steamapps.join(&manifest_name).exists())
}

pub fn get_api_key() -> Value {
    let (key, _) = read_steam_config();
    json!({
        "ok": true,
        "key": key.unwrap_or_default(),
    })
}

pub fn save_api_key(key: &str) -> Result<(), Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let config_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache");
    std::fs::create_dir_all(&config_dir)?;
    let config_path = config_dir.join("steam_config.json");

    let steam_id = get_steam_id().unwrap_or_default();

    let config = json!({
        "steam_api_key": key,
        "steam_id": steam_id,
    });

    std::fs::write(&config_path, serde_json::to_string_pretty(&config)?)?;

    let cache_path = config_dir.join("owned_games.json");
    let _ = std::fs::remove_file(cache_path);

    Ok(())
}

pub fn api_key_sync_state() -> Value {
    let (api_key, steam_id) = read_steam_config();
    let cache_path = crate::platform::metalsharp_home_dir().join("cache/owned_games.json");
    json!({
        "api_key_set": api_key.as_deref().map(|k| !k.is_empty()).unwrap_or(false),
        "steam_id_detected": steam_id.as_deref().map(|s| !s.is_empty()).unwrap_or(false),
        "steam_id": steam_id.unwrap_or_default(),
        "owned_games_cache": cache_path.exists(),
    })
}

pub fn get_steam_id() -> Option<String> {
    let home = dirs::home_dir()?;

    let paths = vec![
        home.join("Library/Application Support/Steam/config/loginusers.vdf"),
        crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam/drive_c/Program Files (x86)/Steam/config/loginusers.vdf"),
    ];

    for mac_path in &paths {
        if let Ok(contents) = std::fs::read_to_string(mac_path) {
            for line in contents.lines() {
                let trimmed = line.trim();
                if trimmed.starts_with('"') && trimmed.chars().filter(|c| *c == '"').count() == 2 {
                    let id = trimmed.trim_matches('"').trim();
                    if id.starts_with("7656") {
                        return Some(id.to_string());
                    }
                }
            }
        }
    }

    None
}

pub fn library() -> Value {
    let installed_appids = get_installed_appids();
    let downloaded_appids = get_downloaded_appids();
    let wine_steam_appids = get_wine_steam_installed_games();
    let sync = api_key_sync_state();

    let owned: Vec<(u32, String)> = match fetch_owned_games(get_steam_id().as_deref()) {
        Ok(games) if !games.is_empty() => games,
        _ => {
            let mut fallback: Vec<(u32, String)> = Vec::new();
            for &appid in &wine_steam_appids {
                let name = get_game_name_from_manifest(appid).unwrap_or_else(|| format!("Game {}", appid));
                fallback.push((appid, name));
            }
            for &appid in &downloaded_appids {
                if !fallback.iter().any(|(id, _)| *id == appid) {
                    fallback.push((appid, format!("Game {}", appid)));
                }
            }
            fallback
        },
    };

    let owned_appids: Vec<u32> = owned.iter().map(|(id, _)| *id).collect();
    let mut all_games = owned;
    for &appid in &wine_steam_appids {
        if !owned_appids.contains(&appid) {
            let name = get_game_name_from_manifest(appid).unwrap_or_else(|| format!("Game {}", appid));
            all_games.push((appid, name));
        }
    }
    for &appid in &downloaded_appids {
        if !all_games.iter().any(|(id, _)| *id == appid) {
            all_games.push((appid, format!("Game {}", appid)));
        }
    }

    let games: Vec<Value> = all_games
        .iter()
        .map(|(appid, name)| {
            let is_installed = installed_appids.contains(appid)
                || downloaded_appids.contains(appid)
                || wine_steam_appids.contains(appid);
            let can_uninstall = downloaded_appids.contains(appid) || wine_steam_appids.contains(appid);
            let dual = crate::scan::resolve_dual_game_dir(*appid);
            let pipeline_id = crate::mtsp::rules::resolve_pipeline(*appid);
            let bottle = if is_installed {
                crate::bottles::refresh_steam_game_bottle(*appid, name, dual.wine_dir.as_deref(), pipeline_id).ok()
            } else {
                None
            };
            let effective_pipeline = crate::bottles::resolve_steam_pipeline_for_request(*appid, None);
            let effective_node = crate::mtsp::engine::get_pipeline(effective_pipeline);
            let recommended = pipeline_id.user_selectable_id().unwrap_or("auto");
            let node = crate::mtsp::engine::get_pipeline(pipeline_id);
            let available_pipelines: Vec<serde_json::Value> = std::iter::once(serde_json::json!({
                "id": recommended,
                "name": pipeline_id.user_selectable_name().unwrap_or("Auto"),
                "recommended": true,
            }))
            .chain(node.alternatives.iter().filter(|alt| alt.is_user_selectable()).map(|alt| {
                let alt_node = crate::mtsp::engine::get_pipeline(*alt);
                serde_json::json!({
                    "id": alt_node.id.user_selectable_id().unwrap_or("auto"),
                    "name": alt_node.id.user_selectable_name().unwrap_or(alt_node.name),
                    "recommended": false,
                })
            }))
            .collect();
            json!({
                "appid": appid,
                "name": name,
                "installed": is_installed,
                "state": if is_installed { "installed" } else { "not_installed" },
                "can_uninstall": can_uninstall,
                "launch_method": effective_pipeline.user_selectable_id().unwrap_or(recommended),
                "launch_method_name": effective_pipeline.user_selectable_name().unwrap_or(effective_node.name),
                "preferred_pipeline": bottle.as_ref().and_then(|b| b.preferred_pipeline.clone()),
                "available_pipelines": available_pipelines,
                "has_native_build": dual.has_native_build,
                "native_app_path": dual.macos_app.map(|p| p.to_string_lossy().to_string()),
                "wine_game_path": dual.wine_dir.map(|p| p.to_string_lossy().to_string()),
                "bottle_id": bottle.as_ref().map(|b| b.id.clone()),
                "bottle_health": bottle.as_ref().map(|b| json!(b.health)),
                "bottle_runtime_assets": bottle.as_ref().map(|b| b.runtime_assets.len()).unwrap_or(0),
                "cover_url": format!("https://steamcdn-a.akamaihd.net/steam/apps/{}/library_600x900.jpg", appid),
                "header_url": format!("https://steamcdn-a.akamaihd.net/steam/apps/{}/header.jpg", appid),
            })
        })
        .collect();

    json!({
        "ok": true,
        "total": games.len(),
        "installed_count": games.iter().filter(|g| g["installed"].as_bool().unwrap_or(false)).count(),
        "sync": sync,
        "games": games,
    })
}

fn get_downloaded_appids() -> Vec<u32> {
    let home = dirs::home_dir().unwrap_or_default();
    let games_dir = crate::platform::metalsharp_home_dir_for(&home).join("games");
    let mut appids = Vec::new();

    if let Ok(entries) = std::fs::read_dir(&games_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            if let Some(name) = path.file_name() {
                if let Ok(id) = name.to_string_lossy().parse::<u32>() {
                    let has_exe = walkdir::WalkDir::new(&path)
                        .max_depth(3)
                        .into_iter()
                        .flatten()
                        .any(|e| e.path().extension().map(|ext| ext == "exe").unwrap_or(false));
                    if has_exe {
                        appids.push(id);
                    }
                }
            }
        }
    }

    appids
}

fn read_steam_config() -> (Option<String>, Option<String>) {
    let home = dirs::home_dir().unwrap_or_default();
    let config_path = crate::platform::metalsharp_home_dir_for(&home).join("cache/steam_config.json");
    if let Ok(contents) = std::fs::read_to_string(&config_path) {
        if let Ok(cfg) = serde_json::from_str::<serde_json::Map<String, Value>>(&contents) {
            let key = cfg.get("steam_api_key").and_then(|v| v.as_str()).map(String::from);
            let sid = cfg.get("steam_id").and_then(|v| v.as_str()).map(String::from);
            return (key, sid);
        }
    }
    (None, get_steam_id())
}

fn fetch_owned_games(_steam_id: Option<&str>) -> Result<Vec<(u32, String)>, Box<dyn std::error::Error>> {
    let (api_key, steam_id) = read_steam_config();
    let key = api_key.as_deref().unwrap_or("");
    let sid = steam_id.as_deref().or(_steam_id).unwrap_or("");

    if key.is_empty() || sid.is_empty() {
        return Ok(vec![]);
    }

    let cache_path = crate::platform::metalsharp_home_dir().join("cache/owned_games.json");

    if cache_path.exists() {
        if let Ok(contents) = std::fs::read_to_string(&cache_path) {
            if let Ok(cached) = serde_json::from_str::<serde_json::Map<String, Value>>(&contents) {
                if let Some(ts) = cached.get("timestamp").and_then(|t| t.as_u64()) {
                    let age = std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap_or_default()
                        .as_secs();
                    if age - ts < 3600 {
                        if let Some(arr) = cached.get("games").and_then(|g| g.as_array()) {
                            return Ok(arr
                                .iter()
                                .filter_map(|g| {
                                    let id = g.get("appid")?.as_u64()? as u32;
                                    let name = g.get("name")?.as_str()?.to_string();
                                    Some((id, name))
                                })
                                .collect());
                        }
                    }
                }
            }
        }
    }

    let url = format!(
        "https://api.steampowered.com/IPlayerService/GetOwnedGames/v0001/?key={}&steamid={}&include_appinfo=1&include_played_free_games=1&format=json",
        key, sid
    );

    let output = Command::new("curl").args(["-sL", "-m", "15", &url]).output()?;

    if !output.status.success() {
        return Err("curl failed".into());
    }

    let body: Value = serde_json::from_slice(&output.stdout)?;

    let games_arr = body.get("response").and_then(|r| r.get("games")).and_then(|g| g.as_array());

    let result: Vec<(u32, String)> = match games_arr {
        Some(arr) => arr
            .iter()
            .filter_map(|g| {
                let id = g.get("appid")?.as_u64()? as u32;
                let name = g.get("name")?.as_str()?.to_string();
                Some((id, name))
            })
            .collect(),
        None => vec![],
    };

    let _ = save_cache(&cache_path, &result);

    Ok(result)
}

fn save_cache(path: &PathBuf, games: &[(u32, String)]) -> Result<(), Box<dyn std::error::Error>> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let arr: Vec<Value> = games.iter().map(|(id, name)| json!({"appid": id, "name": name})).collect();
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();
    std::fs::write(path, serde_json::to_string_pretty(&json!({"timestamp": now, "games": arr}))?)?;
    Ok(())
}

fn get_installed_appids() -> Vec<u32> {
    let home = dirs::home_dir().unwrap_or_default();
    let mut appids = Vec::new();

    let mac_dirs = vec![
        home.join("Library/Application Support/Steam/steamapps"),
        home.join(".steam/steam/steamapps"),
        home.join(".local/share/Steam/steamapps"),
    ];

    let mut all_dirs: Vec<PathBuf> = mac_dirs.into_iter().filter(|d| d.exists()).collect();
    all_dirs.extend(crate::scan::wine_steam_library_paths());

    for dir in all_dirs {
        if let Ok(entries) = std::fs::read_dir(&dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_string();
                if name.starts_with("appmanifest_") && name.ends_with(".acf") {
                    if let Some(id_str) = name.strip_prefix("appmanifest_").and_then(|s| s.strip_suffix(".acf")) {
                        if let Ok(id) = id_str.parse::<u32>() {
                            if !appids.contains(&id) {
                                appids.push(id);
                            }
                        }
                    }
                }
            }
        }
    }

    appids
}

fn detect_login_state() -> Value {
    let home = dirs::home_dir().unwrap_or_default();
    let mac_path = home.join("Library/Application Support/Steam/config/loginusers.vdf");
    let wine_path = crate::platform::metalsharp_home_dir_for(&home)
        .join("prefix-steam/drive_c/Program Files (x86)/Steam/config/loginusers.vdf");

    let contents =
        std::fs::read_to_string(&mac_path).or_else(|_| std::fs::read_to_string(&wine_path)).unwrap_or_default();

    if contents.is_empty() {
        return json!({"state": "unknown", "account": null});
    }

    let mut accounts: Vec<Value> = Vec::new();

    for line in contents.lines() {
        let trimmed = line.trim();
        if let Some(name) = parse_vdf_value(trimmed, "PersonaName") {
            let remembered = contents.lines().any(|l| l.contains("RememberPassword") && l.contains("1"));
            accounts.push(json!({
                "name": name,
                "remembered": remembered,
            }));
        }
    }

    if accounts.is_empty() {
        json!({"state": "logged_out", "account": null})
    } else {
        json!({"state": "logged_in", "account": accounts})
    }
}

fn parse_vdf_value(line: &str, key: &str) -> Option<String> {
    let prefix = format!("\"{}\"", key);
    if !line.starts_with(&prefix) {
        return None;
    }
    let rest = line.trim_start_matches(&prefix).trim();
    let rest = rest.trim_start_matches('\t').trim_start_matches(' ');
    Some(rest.trim_matches('"').to_string())
}

pub fn install_steam() -> Result<String, Box<dyn std::error::Error>> {
    let steam_dir = steam_prefix().join("drive_c").join("Program Files (x86)").join("Steam");

    if steam_dir.join("steamui.dll").exists() && steam_exe_path().exists() {
        return Ok("Steam already installed".into());
    }

    if STEAM_INSTALLING.load(Ordering::SeqCst) {
        return Ok("Steam installation already in progress".into());
    }

    if STEAM_INSTALLING.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
        return Ok("Steam installation already in progress".into());
    }

    std::thread::spawn(move || {
        let _ = run_install_steam();
        STEAM_INSTALLING.store(false, Ordering::SeqCst);
    });

    Ok("Steam installation started — polling /steam/status for completion".into())
}

fn run_install_steam() -> Result<String, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let metalsharp_dir = crate::platform::metalsharp_home_dir_for(&home);
    std::fs::create_dir_all(&metalsharp_dir)?;

    let steam_dir = steam_prefix().join("drive_c").join("Program Files (x86)").join("Steam");

    let _ = std::fs::remove_dir_all(steam_prefix());

    let installer = metalsharp_dir.join("SteamSetup.exe");
    let _ = std::fs::remove_file(&installer);

    let url = "https://steamcdn-a.akamaihd.net/client/installer/SteamSetup.exe";
    let output = Command::new("curl").args(["-sL", "-o", &installer.to_string_lossy(), url]).status()?;
    if !output.success() {
        if let Some(bundled) = find_bundled_steam_asset("SteamSetup.exe") {
            let _ = std::fs::copy(&bundled, &installer);
        }
    }
    if !installer.exists() {
        return Err("Failed to download Steam installer".into());
    }

    let wine = ms_wine();
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let prefix = steam_prefix();
    std::fs::create_dir_all(&prefix)?;

    let prefix_str = prefix.to_string_lossy().to_string();

    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");

    let mut wineboot_cmd = Command::new(&wine);
    wineboot_cmd
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "/usr/bin/true")
        .env("WINEDLLOVERRIDES", "winedbg=d")
        .arg("wineboot")
        .arg("--init")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut wineboot_cmd, &ms_root);
    let wineboot_result = wineboot_cmd.status();

    if wineboot_result.is_err() {
        return Err("wineboot --init failed — MetalSharp Wine may not be properly installed".into());
    }

    let windows_dir = prefix.join("drive_c").join("windows").join("system32");
    let mut wineboot_ok = false;
    for _ in 0..30 {
        if windows_dir.exists() {
            wineboot_ok = true;
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(2));
    }
    if !wineboot_ok {
        return Err("wineboot --init timed out — Wine prefix was not created within 60 seconds".into());
    }

    let seed_log = prefix.join("drive_c").join("metalsharp-post-wineboot.log");
    let _ = crate::bottles::seed_post_wineboot_config(&prefix, &seed_log);

    let mut install_cmd = Command::new(&wine);
    install_cmd
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "/usr/bin/true")
        .env("WINEDLLOVERRIDES", "winedbg=d")
        .arg(&installer)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut install_cmd, &ms_root);

    let mut install_child = install_cmd.spawn()?;
    let install_pid = install_child.id();

    let steam_exe = steam_dir.join("Steam.exe");
    let steam_ui_dll = steam_dir.join("steamui.dll");
    let mut install_crashed = false;
    for _ in 0..70 {
        // Check if the installer process is still running before waiting.
        match install_child.try_wait() {
            Ok(Some(status)) => {
                if !status.success() {
                    eprintln!("steam: installer exited early with status {:?}", status.code());
                    install_crashed = true;
                }
                break;
            },
            Ok(None) => {}, // Still running
            Err(e) => {
                eprintln!("steam: failed to check installer status: {}", e);
                break;
            },
        }
        if steam_exe.exists() && steam_ui_dll.exists() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }

    // If installer crashed, wait briefly for any remaining file I/O to settle
    // before checking if Steam was partially installed anyway.
    if install_crashed {
        std::thread::sleep(std::time::Duration::from_secs(3));
        if steam_exe.exists() && steam_ui_dll.exists() {
            eprintln!("steam: installer crashed but Steam files are present — continuing");
        } else {
            // Kill any remaining Wine processes from the failed install
            let _ = Command::new("pkill")
                .args(["-f", &format!("WINEPREFIX={}", prefix_str)])
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
            return Err(format!("Steam installer crashed (pid {}). Check Wine setup and retry.", install_pid).into());
        }
    }

    if steam_exe.exists() && steam_ui_dll.exists() {
        deploy_steamwebhelper_wrapper(&steam_dir);
    }

    for _ in 0..30 {
        if !cef_subdirs(&steam_dir).is_empty() {
            deploy_steamwebhelper_wrapper(&steam_dir);
            break;
        }
        std::thread::sleep(std::time::Duration::from_secs(2));
    }

    Ok("Steam install thread complete".into())
}

pub fn watch_steamapps() -> Vec<u32> {
    let current = get_installed_appids();
    let cached_path = crate::platform::metalsharp_home_dir().join("cache").join("steam_appids.cache");

    let cached: Vec<u32> = if cached_path.exists() {
        std::fs::read_to_string(&cached_path)
            .ok()
            .map(|s| s.lines().filter_map(|l| l.parse::<u32>().ok()).collect())
            .unwrap_or_default()
    } else {
        vec![]
    };

    let mut new_appids = Vec::new();
    for &id in &current {
        if !cached.contains(&id) {
            new_appids.push(id);
        }
    }

    let _ = std::fs::create_dir_all(cached_path.parent().unwrap_or(std::path::Path::new(".")));
    let _ = std::fs::write(&cached_path, current.iter().map(|id| id.to_string()).collect::<Vec<_>>().join("\n"));

    new_appids
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn steam_defaults_to_legacy_compatible_opengl_contexts() {
        let mut command = Command::new("wine");
        command.env("MS_FWD_COMPAT_GL_CTX", "1");

        use_legacy_compatible_opengl_context(&mut command);

        assert!(matches!(command.get_envs().find(|(key, _)| *key == "MS_FWD_COMPAT_GL_CTX"), Some((_, None))));
    }

    #[test]
    fn route_environment_can_opt_into_forward_compatible_opengl() {
        let mut command = Command::new("wine");
        use_legacy_compatible_opengl_context(&mut command);
        command.env("MS_FWD_COMPAT_GL_CTX", "1");

        assert_eq!(
            command.get_envs().find(|(key, _)| *key == "MS_FWD_COMPAT_GL_CTX").and_then(|(_, value)| value),
            Some(std::ffi::OsStr::new("1"))
        );
    }

    #[test]
    fn steam_disables_wine_tracing_by_default() {
        assert_eq!(steam_wine_debug_value(None), "-all");
        assert_eq!(steam_wine_debug_value(Some(String::new())), "-all");
    }

    #[test]
    fn steam_accepts_explicit_wine_debug_override() {
        assert_eq!(steam_wine_debug_value(Some("+opengl".to_string())), "+opengl");
    }

    #[test]
    fn stop_wine_steam_targets_report_has_required_shape() {
        // Phase 7: the stop-targets report must expose the targeted and
        // explicitly-excluded process lists so a reviewer can prove the stop
        // filter never touches the macOS Steam client or our own rg/ps.
        let report = stop_wine_steam_targets();
        assert_eq!(report.get("ok").and_then(|v| v.as_bool()), Some(true));
        assert!(report.get("targeted_pid_count").and_then(|v| v.as_u64()).is_some());
        assert!(report.get("targeted").unwrap().is_array());
        assert!(report.get("excluded").unwrap().is_array());
        let summary = report.get("summary").and_then(|v| v.as_str()).unwrap();
        assert!(summary.contains("Wine Steam helper"), "summary must name the target scope: {}", summary);
    }

    #[test]
    fn parses_acf_quoted_fields() {
        let acf = r#"
"AppState"
{
    "appid"        "123"
    "name"         "Example Game"
    "installdir"   "Example Game"
}
"#;

        assert_eq!(parse_acf_field(acf, "name"), Some("Example Game".to_string()));
        assert_eq!(parse_acf_field(acf, "installdir"), Some("Example Game".to_string()));
    }

    #[test]
    fn rejects_nested_steam_install_dirs() {
        assert!(is_single_path_component("Example Game"));
        assert!(!is_single_path_component("../Steam"));
        assert!(!is_single_path_component("nested/game"));
        assert!(!is_single_path_component("/Applications/Steam.app"));
    }

    #[test]
    fn detects_macos_steam_process_lines_without_matching_shell_searches() {
        let line = "26398 /Users/alex/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/ipcserver";
        let (pid, command) = parse_process_line(line).expect("process line should parse");
        assert_eq!(pid, 26398);
        assert!(!is_macos_steam_active_command(command));
        assert!(is_macos_steam_cleanup_command(command));
        assert!(!is_macos_steam_active_command(
            "/bin/zsh -lc ps axo pid=,command= | rg -i \"Steam.app|steam_osx|ipcserver\"",
        ));
        assert!(!is_macos_steam_cleanup_command(
            "/bin/zsh -lc ps axo pid=,command= | rg -i \"Steam.app|steam_osx|ipcserver\"",
        ));
    }

    #[test]
    fn wine_steam_cleanup_includes_detached_tray_helpers() {
        assert!(is_wine_steam_cleanup_command("1234 C:\\windows\\system32\\explorer.exe /desktop "));
        assert!(is_wine_steam_cleanup_command("1235 C:\\windows\\system32\\conhost.exe --server 0x3c --headless "));
        assert!(!is_wine_steam_cleanup_command(
            "/bin/zsh -lc ps axo pid=,command= | rg -i \"explorer.exe|conhost.exe\"",
        ));
    }

    #[test]
    fn wine_steam_cleanup_ignores_foreign_wine_helpers() {
        // Isolation contract: generic wineserver/wineloader/winedevice tokens
        // are not enough — a foreign Wine launcher (CrossOver, SakuraGiri,
        // Whisky, GPTK) also runs those names. Only MS-owned commands that
        // reference the MS home (runtime/prefix/bottles live under it) or the
        // metalsharp-wine wrapper qualify.
        assert!(!is_wine_steam_cleanup_command(
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver -f",
        ));
        assert!(!is_wine_steam_cleanup_command(
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineloader",
        ));
        assert!(!is_wine_steam_cleanup_command(
            "/Users/test/Library/Application Support/SakuraGiri/engine/wine/bin/winedevice.exe",
        ));
        assert!(!is_wine_steam_cleanup_command("/opt/homebrew/bin/wineserver"));

        // MS-owned wine helpers must still be targeted (paths under the real
        // MetalSharp home, which the ownership check resolves at runtime).
        let ms_home = crate::platform::metalsharp_home_dir();
        let ms_wineserver = ms_home.join("runtime").join("wine").join("bin").join("wineserver");
        assert!(is_wine_steam_cleanup_command(&format!("{} -f", ms_wineserver.display())));
        let ms_wrapper = ms_home.join("runtime").join("wine").join("bin").join("metalsharp-wine");
        assert!(is_wine_steam_cleanup_command(&format!("{} wineloader", ms_wrapper.display())));
    }

    #[test]
    fn steam_d3d12_guard_uses_appdefaults_not_global_overrides() {
        let reg = build_steam_d3d12_guard_reg();

        for app in STEAM_D3D12_GUARD_APPS {
            assert!(
                reg.contains(&format!("Software\\Wine\\AppDefaults\\{}\\DllOverrides", app)),
                "missing AppDefaults guard for {app}: {reg}"
            );
        }
        for dll in STEAM_D3D12_GUARD_DLLS {
            assert!(reg.contains(&format!("\"{}\"=\"builtin\"", dll)), "missing builtin guard for {dll}: {reg}");
        }
        assert!(
            !reg.contains("[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]"),
            "Steam D3D12 guard must not globally disable M12 game routing"
        );
    }

    #[test]
    fn steam_bundle_layout_matches_release_manifest() {
        let manifest = include_str!("../../../tools/bundles/asset-manifest.tsv");
        let steam_row = manifest
            .lines()
            .find(|line| line.starts_with("metalsharp-steam.tar.zst\t"))
            .expect("metalsharp-steam.tar.zst release manifest row");
        let fields: Vec<&str> = steam_row.split('\t').collect();

        assert_eq!(fields.get(1).copied(), Some("steam"));
        assert_eq!(STEAMWEBHELPER_WRAPPER_SHA256.len(), 64);
    }
}
