use serde_json::json;
use serde_json::{Map, Value};
use std::path::{Path, PathBuf};
use std::process::Command;

pub fn launch(exe_path: &str, game_type: &str) -> Result<u32, Box<dyn std::error::Error>> {
    match game_type {
        "xna_fna" => launch_via_fna_mono(exe_path),
        _ => launch_via_wine(exe_path),
    }
}

pub fn launch_via_steam(appid: u32) -> Result<u32, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    let prefix_str = prefix.to_string_lossy().to_string();

    if !crate::steam::is_wine_steam_running() {
        crate::steam::launch_wine_steam()?;
        let mut ready = false;
        for _ in 0..12 {
            if crate::steam::is_wine_steam_running() {
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

    let mut cmd = Command::new(&wine);
    cmd.env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .args(["start", &url])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
    spawn_and_reap(cmd)
}

pub fn launch_via_steam_with_env(
    appid: u32,
    extra_env: &[(String, String)],
) -> Result<u32, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);
    if !wine.exists() {
        return Err("MetalSharp Wine not found".into());
    }

    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    let prefix_str = prefix.to_string_lossy().to_string();

    let steam_running = crate::steam::is_wine_steam_running();
    ensure_steam_env_handoff_supported(steam_running, extra_env)?;

    if !steam_running {
        crate::steam::launch_wine_steam_with_env(extra_env)?;
        let mut ready = false;
        for _ in 0..12 {
            if crate::steam::is_wine_steam_running() {
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

    let mut cmd = Command::new(&wine);
    cmd.env("WINEPREFIX", &prefix_str).env("WINEDEBUG", "-all").env("WINEDEBUGGER", "none");
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);

    for (key, val) in extra_env {
        cmd.env(key, val);
    }

    cmd.args(["start", &url]).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    spawn_and_reap(cmd)
}

fn ensure_steam_env_handoff_supported(steam_running: bool, extra_env: &[(String, String)]) -> Result<(), String> {
    if steam_running && !extra_env.is_empty() {
        return Err("Wine Steam is already running; per-game environment cannot be inherited without restarting Steam. Use a direct MTSP pipeline for env-dependent launches.".into());
    }
    Ok(())
}

pub fn kill_process_tree(pid: i32) -> Result<(), Box<dyn std::error::Error>> {
    if pid <= 0 {
        return Ok(());
    }

    let _ = Command::new("pkill")
        .args(["-9", "-P", &pid.to_string()])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    let _ = Command::new("kill")
        .args(["-9", &pid.to_string()])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    std::thread::sleep(std::time::Duration::from_millis(300));

    let _ = Command::new("pkill")
        .args(["-9", "-f", "UnityCrashHandler"])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    Ok(())
}

pub fn kill_game_with_pid(appid: u32, pid: i32) -> Result<(), Box<dyn std::error::Error>> {
    if pid > 0 {
        kill_process_tree(pid)?;
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    let game_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());

    let resolved = crate::setup::resolve_game_dir(appid);
    let dirs_to_check =
        if let Some(ref rd) = resolved { vec![rd.clone(), game_dir.clone()] } else { vec![game_dir.clone()] };

    for dir in &dirs_to_check {
        if dir.exists() {
            if let Ok(output) = Command::new("pgrep").args(["-a", "-f", &dir.to_string_lossy()]).output() {
                for line in String::from_utf8_lossy(&output.stdout).lines() {
                    if let Some(pid_str) = line.split_whitespace().next() {
                        if let Ok(p) = pid_str.parse::<i32>() {
                            if p != pid {
                                let _ = Command::new("kill").args(["-9", &p.to_string()]).status();
                            }
                        }
                    }
                }
            }
        }
    }

    let _ = Command::new("pkill").args(["-9", "-f", "UnityCrashHandler"]).status();

    Ok(())
}

pub fn is_process_active(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }

    match Command::new("ps").args(["-p", &pid.to_string(), "-o", "stat="]).output() {
        Ok(output) if output.status.success() => {
            let stat = String::from_utf8_lossy(&output.stdout);
            !stat.trim().is_empty() && !stat.contains('Z')
        },
        _ => false,
    }
}

fn spawn_and_reap(mut cmd: Command) -> Result<u32, Box<dyn std::error::Error>> {
    let mut child = cmd.spawn()?;
    let pid = child.id();
    std::thread::spawn(move || {
        let _ = child.wait();
    });
    Ok(pid)
}

pub fn get_config() -> Value {
    get_config_for_home(&dirs::home_dir().unwrap_or_default())
}

/// Path-based variant (testable without touching the global METALSHARP_HOME).
pub fn get_config_for_home(home: &Path) -> Value {
    let native_available = find_metalsharp_native().is_ok();
    let mono_available = find_mono().is_ok();
    let graphics_runtime_logs = graphics_runtime_logs_enabled();
    let controller_input = controller_input_mode_for(home);

    json!({
        "ok": true,
        "native_available": native_available,
        "mono_available": mono_available,
        "graphicsRuntimeLogs": graphics_runtime_logs,
        "graphics_runtime_logs": graphics_runtime_logs,
        "controllerInput": controller_input,
        "m12Backend": m12_backend_mode_for(home),
        "msync": msync_enabled_for(home),
    })
}

/// The active M12 graphics backend: `"vkd3d-proton"` (default, D3D12 ->
/// vkd3d-proton -> Vulkan -> MoltenVK -> Metal) or `"dxmt"` (legacy D3D12 ->
/// DXMT -> Metal, kept as rollback). Anything else resolves to the default.
pub fn m12_backend_mode() -> String {
    m12_backend_mode_for(&dirs::home_dir().unwrap_or_default())
}

/// Path-based variant (testable without touching the global METALSHARP_HOME).
pub fn m12_backend_mode_for(home: &Path) -> String {
    read_config_string_for_home(home, "m12Backend")
        .map(|v| v.trim().to_ascii_lowercase())
        .filter(|v| matches!(v.as_str(), "vkd3d-proton" | "dxmt"))
        .unwrap_or_else(|| "vkd3d-proton".to_string())
}

/// Read the persisted controller input shim mode. Valid values are
/// `"off"`, `"x"` (XInput shims), and `"d"` (DInput shims); anything else
/// (missing, empty, or unknown) resolves to `"off"` so the selector is
/// off by default.
pub fn controller_input_mode() -> String {
    controller_input_mode_for(&dirs::home_dir().unwrap_or_default())
}

/// Path-based variant (testable without touching the global METALSHARP_HOME).
pub fn controller_input_mode_for(home: &Path) -> String {
    read_config_string_for_home(home, "controllerInput")
        .map(|v| v.trim().to_ascii_lowercase())
        .filter(|v| matches!(v.as_str(), "off" | "x" | "d"))
        .unwrap_or_else(|| "off".to_string())
}

fn read_config_string(key: &str) -> Option<String> {
    read_config_string_for_home(&dirs::home_dir()?, key)
}

fn read_config_string_for_home(home: &Path, key: &str) -> Option<String> {
    let path = config_path_for_home_unenv(home);
    let contents = std::fs::read_to_string(path).ok()?;
    let value: Value = serde_json::from_str(&contents).ok()?;
    value.get(key).and_then(|v| v.as_str()).map(str::to_string)
}

pub fn graphics_runtime_logs_enabled() -> bool {
    if let Ok(value) = std::env::var("METALSHARP_GRAPHICS_RUNTIME_LOGS") {
        return truthy(&value);
    }
    read_config_bool("graphicsRuntimeLogs").or_else(|| read_config_bool("graphics_runtime_logs")).unwrap_or(false)
}

/// Whether Wine msync (Mach-synchronized Wine sync primitives) is enabled.
/// Default ON (matches the historical `WINEMSYNC=1` the launcher always set).
/// An explicit `WINEMSYNC` env var in the parent process overrides the config
/// so devs/games can force a per-launch value.
pub fn msync_enabled() -> bool {
    if let Ok(value) = std::env::var("WINEMSYNC") {
        return truthy(&value);
    }
    msync_enabled_for(&dirs::home_dir().unwrap_or_default())
}

/// Path-based variant (testable without touching the global METALSHARP_HOME).
pub fn msync_enabled_for(home: &Path) -> bool {
    read_config_bool_for_home(home, "msync").unwrap_or(true)
}

fn read_config_bool_for_home(home: &Path, key: &str) -> Option<bool> {
    let path = config_path_for_home_unenv(home);
    let contents = std::fs::read_to_string(path).ok()?;
    let value: Value = serde_json::from_str(&contents).ok()?;
    value.get(key).and_then(json_bool)
}

fn read_config_bool(key: &str) -> Option<bool> {
    let path = config_path_for_home(&dirs::home_dir()?);
    let contents = std::fs::read_to_string(path).ok()?;
    let value: Value = serde_json::from_str(&contents).ok()?;
    value.get(key).and_then(json_bool)
}

fn json_bool(value: &Value) -> Option<bool> {
    match value {
        Value::Bool(b) => Some(*b),
        Value::Number(n) => n.as_i64().map(|v| v != 0),
        Value::String(s) => Some(truthy(s)),
        _ => None,
    }
}

pub(crate) fn truthy(value: &str) -> bool {
    matches!(value.trim().to_ascii_lowercase().as_str(), "1" | "true" | "yes" | "on")
}

fn config_path_for_home(home: &Path) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(home).join("configs").join("config.json")
}

/// Env-independent config path: joins `home/.metalsharp` directly instead of
/// going through `metalsharp_home_dir_for`, which honors the global
/// METALSHARP_HOME env var (racy under parallel tests).
fn config_path_for_home_unenv(home: &Path) -> PathBuf {
    home.join(".metalsharp").join("configs").join("config.json")
}

fn find_metalsharp_native() -> Result<String, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;

    let candidates = vec![
        PathBuf::from("/Applications/MetalSharp.app/Contents/Resources/metalsharp"),
        crate::platform::metalsharp_home_dir_for(&home).join("metalsharp"),
        home.join("metalsharp/build/metalsharp"),
        PathBuf::from("/usr/local/bin/metalsharp"),
        PathBuf::from("/opt/homebrew/bin/metalsharp"),
    ];

    for c in candidates {
        if c.exists() {
            return Ok(c.to_string_lossy().to_string());
        }
    }

    Err("metalsharp binary not found".into())
}

fn find_scripts_dir() -> Option<PathBuf> {
    let home = dirs::home_dir().unwrap_or_default();
    let candidates =
        vec![home.join("metalsharp").join("scripts"), crate::platform::metalsharp_home_dir_for(&home).join("scripts")];
    candidates.into_iter().find(|p| p.exists())
}

pub fn run_game_setup_script(appid: u32) -> Result<(), Box<dyn std::error::Error>> {
    let script_name = match appid {
        105600 => "setup-terraria-deps.sh",
        504230 => "setup-celeste-deps.sh",
        312520 => "setup-rainworld-deps.sh",
        535520 => "setup-nidhogg2-deps.sh",
        945360 => "setup-amongus-deps.sh",
        620 | 265930 => "setup-goldberg-deps.sh",
        _ => return Ok(()),
    };

    let scripts_dir = match find_scripts_dir() {
        Some(d) => d,
        None => return Err("scripts directory not found".into()),
    };

    let script = scripts_dir.join(script_name);
    if !script.exists() {
        return Err(format!("setup script not found: {}", script.display()).into());
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    let env_path = format!(
        "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:{}",
        home.join(".cargo/bin").to_string_lossy()
    );

    let status = Command::new("/bin/bash")
        .arg(&script)
        .arg(appid.to_string())
        .env("PATH", &env_path)
        .env("HOME", &home)
        .env("METALSHARP_HOME", crate::platform::metalsharp_home_dir_for(&home).to_string_lossy().to_string())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .status()?;

    if !status.success() {
        return Err(format!("setup script {} failed", script_name).into());
    }

    Ok(())
}

fn find_mono() -> Result<String, Box<dyn std::error::Error>> {
    let candidates = vec![PathBuf::from("/opt/homebrew/bin/mono"), PathBuf::from("/usr/local/bin/mono")];

    for c in candidates {
        if c.exists() {
            return Ok(c.to_string_lossy().to_string());
        }
    }

    Err("mono not found — install with: brew install mono".into())
}

fn find_wine() -> Result<String, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    find_wine_at(&ms_root)
}

/// Path-based variant for tests: resolves the runtime strictly under
/// `home/.metalsharp` without consulting the process-global METALSHARP_HOME
/// env var (which races under parallel test execution).
fn find_wine_for(home: &Path) -> Result<String, Box<dyn std::error::Error>> {
    find_wine_at(&home.join(".metalsharp").join("runtime").join("wine"))
}

fn find_wine_at(ms_root: &Path) -> Result<String, Box<dyn std::error::Error>> {
    let ms_wine = crate::platform::runtime_wine_binary(ms_root);
    if ms_wine.exists() {
        return Ok(ms_wine.to_string_lossy().to_string());
    }

    // Isolation contract: MetalSharp must NEVER fall back to a system or
    // third-party Wine (Homebrew, GPTK, CrossOver, Whisky, SakuraGiri…).
    // Those runtimes are not ABI-compatible with MetalSharp's prefix and
    // graphics stack, and resolving one of them is exactly how a foreign
    // launcher's Wine ends up running MetalSharp's Steam/prefix. Fail
    // loudly so the user runs setup instead.
    Err(format!(
        "MetalSharp Wine runtime missing at {} — run setup. System/third-party Wine is intentionally not used (found: {:?})",
        ms_wine.display(),
        system_wine_candidates_present()
    )
    .into())
}

fn system_wine_candidates_present() -> Vec<String> {
    ["/opt/homebrew/bin/wine64", "/usr/bin/wine", "/usr/local/bin/wine"]
        .iter()
        .filter(|path| PathBuf::from(path).exists())
        .map(|path| (*path).to_string())
        .collect()
}

pub fn ensure_wine_prefix(prefix: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let system32 = prefix.join("drive_c").join("windows").join("system32");
    if system32.exists() {
        return Ok(());
    }

    let wine = find_wine()?;
    let ms_root = crate::platform::metalsharp_home_dir().join("runtime").join("wine");
    let mut cmd = Command::new(&wine);
    cmd.env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .arg("wineboot")
        .arg("--init")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
    let status = cmd.status()?;

    if !status.success() {
        return Err("failed to initialize Wine prefix".into());
    }

    Ok(())
}

pub fn set_config(body: &Map<String, Value>) -> Result<Value, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    set_config_for_home(&home, body)
}

/// Path-based variant (testable without touching the global METALSHARP_HOME).
pub fn set_config_for_home(home: &Path, body: &Map<String, Value>) -> Result<Value, Box<dyn std::error::Error>> {
    let path = config_path_for_home_unenv(home);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }

    let mut cfg: Map<String, Value> = std::fs::read_to_string(&path)
        .ok()
        .and_then(|contents| serde_json::from_str(&contents).ok())
        .unwrap_or_default();

    if let Some(value) = body
        .get("graphicsRuntimeLogs")
        .or_else(|| body.get("graphics_runtime_logs"))
        .or_else(|| body.get("logs"))
        .and_then(json_bool)
    {
        cfg.insert("graphicsRuntimeLogs".into(), json!(value));
        cfg.insert("graphics_runtime_logs".into(), json!(value));
    }

    if let Some(value) = body.get("controllerInput").and_then(|v| v.as_str()) {
        let normalized = value.trim().to_ascii_lowercase();
        if matches!(normalized.as_str(), "off" | "x" | "d") {
            cfg.insert("controllerInput".into(), json!(normalized));
        }
    }

    if let Some(value) = body.get("m12Backend").and_then(|v| v.as_str()) {
        let normalized = value.trim().to_ascii_lowercase();
        if matches!(normalized.as_str(), "vkd3d-proton" | "dxmt") {
            cfg.insert("m12Backend".into(), json!(normalized));
        }
    }

    if let Some(value) = body.get("msync").and_then(|v| v.as_bool()) {
        cfg.insert("msync".into(), json!(value));
    }

    std::fs::write(&path, serde_json::to_string_pretty(&cfg)?)?;
    Ok(get_config_for_home(home))
}

fn launch_via_wine(exe_path: &str) -> Result<u32, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let wine = find_wine()?;
    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    let prefix_str = prefix.to_string_lossy().to_string();

    if std::env::var_os("MS_LAUNCH_TRACE").is_some() {
        eprintln!("[trace] launch_via_wine wine_bin={}", wine);
        eprintln!("[trace] prefix={}", prefix_str);
        eprintln!("[trace] parent WINEPREFIX={:?}", std::env::var("WINEPREFIX"));
        eprintln!("[trace] parent WINEDLLPATH={:?}", std::env::var("WINEDLLPATH"));
        eprintln!("[trace] parent DYLD_FALLBACK_LIBRARY_PATH={:?}", std::env::var("DYLD_FALLBACK_LIBRARY_PATH"));
        eprintln!("[trace] parent WINESERVER={:?}", std::env::var("WINESERVER"));
    }

    ensure_wine_prefix(&prefix)?;

    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let mut cmd = Command::new(&wine);
    cmd.env("WINEPREFIX", &prefix_str).arg(exe_path);
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
    let child = cmd.spawn()?;

    Ok(child.id())
}

fn launch_via_fna_mono(exe_path: &str) -> Result<u32, Box<dyn std::error::Error>> {
    let mono = find_mono()?;
    let exe = PathBuf::from(exe_path);
    let game_dir = exe.parent().ok_or("no parent dir for exe")?;

    let mut cmd = Command::new(&mono);
    cmd.current_dir(game_dir).env("METAL_DEVICE_WRAPPER_TYPE", "0").arg(&exe);
    if crate::platform::current() == crate::platform::HostPlatform::Macos {
        cmd.env("DYLD_LIBRARY_PATH", ".");
    } else if crate::platform::current() == crate::platform::HostPlatform::Linux {
        cmd.env("LD_LIBRARY_PATH", ".");
    }
    let child = cmd.spawn()?;

    Ok(child.id())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn non_positive_pids_are_not_active() {
        assert!(!is_process_active(0));
        assert!(!is_process_active(-1));
    }

    #[test]
    fn spawned_handoff_process_is_reaped_after_exit() {
        let cmd = Command::new("/usr/bin/true");
        let pid = spawn_and_reap(cmd).expect("spawn handoff process");

        for _ in 0..20 {
            if !is_process_active(pid as i32) {
                return;
            }
            std::thread::sleep(Duration::from_millis(50));
        }

        assert!(!is_process_active(pid as i32));
    }

    #[test]
    fn rejects_env_dependent_handoff_when_steam_is_already_running() {
        let env = vec![("DXMT_SHADER_CACHE_PATH".to_string(), "/tmp/cache".to_string())];

        assert!(ensure_steam_env_handoff_supported(true, &env).is_err());
        assert!(ensure_steam_env_handoff_supported(true, &[]).is_ok());
        assert!(ensure_steam_env_handoff_supported(false, &env).is_ok());
    }

    #[test]
    fn controller_input_defaults_to_off_when_unset_or_unknown() {
        let temp = std::env::temp_dir().join(format!("ms-controller-default-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        // No config file at all -> off
        assert_eq!(controller_input_mode_for(&temp), "off");

        // Unknown / invalid value -> off
        let configs = temp.join(".metalsharp").join("configs");
        std::fs::create_dir_all(&configs).unwrap();
        std::fs::write(configs.join("config.json"), r#"{"controllerInput": "bogus"}"#).unwrap();
        assert_eq!(controller_input_mode_for(&temp), "off");

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn controller_input_accepts_x_d_off() {
        let temp = std::env::temp_dir().join(format!("ms-controller-modes-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        let configs = temp.join(".metalsharp").join("configs");
        std::fs::create_dir_all(&configs).unwrap();

        for (raw, expected) in [("x", "x"), ("X", "x"), ("d", "d"), ("off", "off"), (" OFF ", "off")] {
            std::fs::write(configs.join("config.json"), serde_json::json!({ "controllerInput": raw }).to_string())
                .unwrap();
            assert_eq!(controller_input_mode_for(&temp), expected, "raw={raw}");
        }

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn set_config_persists_and_whitelists_controller_input() {
        let temp = std::env::temp_dir().join(format!("ms-controller-set-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        let mut body = serde_json::Map::new();
        body.insert("controllerInput".into(), json!("x"));
        let result = set_config_for_home(&temp, &body).expect("set_config");
        assert_eq!(result.get("controllerInput").and_then(|v| v.as_str()), Some("x"));

        // Invalid values are rejected (not persisted), stay off.
        let mut bad = serde_json::Map::new();
        bad.insert("controllerInput".into(), json!("hax"));
        let result = set_config_for_home(&temp, &bad).expect("set_config");
        assert_eq!(result.get("controllerInput").and_then(|v| v.as_str()), Some("x"));

        // Switching to d works, then back to off.
        let mut d = serde_json::Map::new();
        d.insert("controllerInput".into(), json!("d"));
        let result = set_config_for_home(&temp, &d).expect("set_config");
        assert_eq!(result.get("controllerInput").and_then(|v| v.as_str()), Some("d"));

        let mut off = serde_json::Map::new();
        off.insert("controllerInput".into(), json!("off"));
        let result = set_config_for_home(&temp, &off).expect("set_config");
        assert_eq!(result.get("controllerInput").and_then(|v| v.as_str()), Some("off"));

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn find_wine_never_falls_back_to_system_wine_when_ms_runtime_missing() {
        // Isolation contract: with no MetalSharp runtime, find_wine must fail
        // loudly instead of resolving a system/third-party wine (CrossOver,
        // SakuraGiri, GPTK/Homebrew all install into the candidate paths).
        let temp = std::env::temp_dir().join(format!("ms-findwine-missing-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        let result = find_wine_for(&temp);
        let err = result.expect_err("find_wine must fail when MS runtime is missing");
        let msg = err.to_string();
        assert!(msg.contains("run setup"), "error must direct user to setup: {}", msg);
        assert!(!msg.contains("wine not found"), "error must not be the old generic message");

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn find_wine_returns_ms_runtime_when_present() {
        let temp = std::env::temp_dir().join(format!("ms-findwine-present-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        let bin = temp.join(".metalsharp").join("runtime").join("wine").join("bin");
        std::fs::create_dir_all(&bin).unwrap();
        let wrapper = bin.join("metalsharp-wine");
        std::fs::write(&wrapper, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&wrapper, std::fs::Permissions::from_mode(0o755)).unwrap();
        }

        let found = find_wine_for(&temp).expect("find_wine must return the MS runtime wrapper");
        assert_eq!(found, wrapper.to_string_lossy().to_string());

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn m12_backend_defaults_to_vkd3d_proton() {
        let temp = std::env::temp_dir().join(format!("ms-m12backend-default-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        // No config -> vkd3d-proton (the new default).
        assert_eq!(m12_backend_mode_for(&temp), "vkd3d-proton");

        // Unknown value -> vkd3d-proton.
        let configs = temp.join(".metalsharp").join("configs");
        std::fs::create_dir_all(&configs).unwrap();
        std::fs::write(configs.join("config.json"), r#"{"m12Backend": "bogus"}"#).unwrap();
        assert_eq!(m12_backend_mode_for(&temp), "vkd3d-proton");

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn m12_backend_accepts_vkd3d_proton_and_dxmt() {
        let temp = std::env::temp_dir().join(format!("ms-m12backend-modes-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        let configs = temp.join(".metalsharp").join("configs");
        std::fs::create_dir_all(&configs).unwrap();

        for (raw, expected) in
            [("vkd3d-proton", "vkd3d-proton"), ("VKD3D-PROTON", "vkd3d-proton"), ("dxmt", "dxmt"), (" DXMT ", "dxmt")]
        {
            std::fs::write(configs.join("config.json"), serde_json::json!({ "m12Backend": raw }).to_string()).unwrap();
            assert_eq!(m12_backend_mode_for(&temp), expected, "raw={raw}");
        }

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn set_config_persists_m12_backend_with_whitelist() {
        let temp = std::env::temp_dir().join(format!("ms-m12backend-set-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        let mut dxmt = serde_json::Map::new();
        dxmt.insert("m12Backend".into(), json!("dxmt"));
        let result = set_config_for_home(&temp, &dxmt).expect("set_config");
        assert_eq!(result.get("m12Backend").and_then(|v| v.as_str()), Some("dxmt"));

        // Invalid value is rejected, keeps dxmt.
        let mut bad = serde_json::Map::new();
        bad.insert("m12Backend".into(), json!("hax"));
        let result = set_config_for_home(&temp, &bad).expect("set_config");
        assert_eq!(result.get("m12Backend").and_then(|v| v.as_str()), Some("dxmt"));

        // Back to default.
        let mut vk = serde_json::Map::new();
        vk.insert("m12Backend".into(), json!("vkd3d-proton"));
        let result = set_config_for_home(&temp, &vk).expect("set_config");
        assert_eq!(result.get("m12Backend").and_then(|v| v.as_str()), Some("vkd3d-proton"));

        let _ = std::fs::remove_dir_all(&temp);
    }

    #[test]
    fn msync_defaults_on_and_persists_off() {
        let temp = std::env::temp_dir().join(format!("ms-msync-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&temp);
        std::fs::create_dir_all(&temp).unwrap();

        // No config -> ON (matches historical WINEMSYNC=1).
        assert!(msync_enabled_for(&temp));

        // OFF persists.
        let mut off = serde_json::Map::new();
        off.insert("msync".into(), json!(false));
        let result = set_config_for_home(&temp, &off).expect("set_config");
        assert_eq!(result.get("msync").and_then(|v| v.as_bool()), Some(false));
        assert!(!msync_enabled_for(&temp));

        // Back ON.
        let mut on = serde_json::Map::new();
        on.insert("msync".into(), json!(true));
        let result = set_config_for_home(&temp, &on).expect("set_config");
        assert_eq!(result.get("msync").and_then(|v| v.as_bool()), Some(true));
        assert!(msync_enabled_for(&temp));

        // Garbage value is rejected, keeps current.
        let mut bad = serde_json::Map::new();
        bad.insert("msync".into(), json!("banana"));
        let result = set_config_for_home(&temp, &bad).expect("set_config");
        assert_eq!(result.get("msync").and_then(|v| v.as_bool()), Some(true));

        let _ = std::fs::remove_dir_all(&temp);
    }
}
