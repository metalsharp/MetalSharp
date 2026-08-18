use serde_json::{json, Map, Value};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

fn mac_cmd(name: &str) -> Command {
    let path = match name {
        "curl" => "/usr/bin/curl",
        "which" => "/usr/bin/which",
        "clang" => "/usr/bin/clang",
        "file" => "/usr/bin/file",
        "install_name_tool" => "/usr/bin/install_name_tool",
        "codesign" => "/usr/bin/codesign",
        "bash" => "/bin/bash",
        "pgrep" => "/usr/bin/pgrep",
        _ => name,
    };
    Command::new(path)
}

const DEFAULT_AGILITY_PACKAGE_VERSION: &str = "1.619.3";

pub fn state() -> Value {
    let home = dirs::home_dir().unwrap_or_default();
    let config_path = crate::platform::metalsharp_home_dir_for(&home).join("setup.json");
    let dxmt_runtime = crate::installer::dxmt_runtime_status();
    let dxmt_current = dxmt_runtime.get("current").and_then(|v| v.as_bool()).unwrap_or(false);
    let dxmt_m12_current = dxmt_runtime.get("m12Current").and_then(|v| v.as_bool()).unwrap_or(false);
    let wine_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let metalsharp_runtime_lib_ready = crate::installer::metalsharp_runtime_lib_ready(&wine_dir);
    let runtime_current = dxmt_current && dxmt_m12_current && metalsharp_runtime_lib_ready;

    if config_path.exists() {
        if let Ok(contents) = std::fs::read_to_string(&config_path) {
            if let Ok(cfg) = serde_json::from_str::<Map<String, Value>>(&contents) {
                let saved_completed = cfg.get("completed").and_then(|v| v.as_bool()).unwrap_or(false);
                return json!({
                    "ok": true,
                    "completed": saved_completed && runtime_current,
                    "savedCompleted": saved_completed,
                    "step": cfg.get("step").and_then(|v| v.as_u64()).unwrap_or(0),
                    "deviceName": cfg.get("deviceName").and_then(|v| v.as_str()).unwrap_or(""),
                    "steamApiKeySet": cfg.get("steamApiKeySet").and_then(|v| v.as_bool()).unwrap_or(false),
                    "runtimeMigrationRequired": saved_completed && !runtime_current,
                    "dxmtRuntime": dxmt_runtime,
                    "metalsharpRuntimeLibReady": metalsharp_runtime_lib_ready,
                });
            }
        }
    }

    json!({
        "ok": true,
        "completed": false,
        "savedCompleted": false,
        "step": 0,
        "deviceName": "",
        "steamApiKeySet": false,
        "runtimeMigrationRequired": false,
        "dxmtRuntime": dxmt_runtime,
        "metalsharpRuntimeLibReady": metalsharp_runtime_lib_ready,
    })
}

pub fn save_step(body: &Map<String, Value>) -> Result<Value, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let config_dir = crate::platform::metalsharp_home_dir_for(&home);
    std::fs::create_dir_all(&config_dir)?;
    let config_path = config_dir.join("setup.json");

    let mut cfg: Map<String, Value> = if config_path.exists() {
        std::fs::read_to_string(&config_path).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default()
    } else {
        Map::new()
    };

    if let Some(step) = body.get("step").and_then(|v| v.as_u64()) {
        cfg.insert("step".into(), json!(step));
    }
    if let Some(completed) = body.get("completed").and_then(|v| v.as_bool()) {
        cfg.insert("completed".into(), json!(completed));
    }
    if let Some(name) = body.get("deviceName").and_then(|v| v.as_str()) {
        cfg.insert("deviceName".into(), json!(name));
    }
    if let Some(set) = body.get("steamApiKeySet").and_then(|v| v.as_bool()) {
        cfg.insert("steamApiKeySet".into(), json!(set));
    }
    std::fs::write(&config_path, serde_json::to_string_pretty(&cfg)?)?;

    Ok(state())
}

pub fn generate_device_name() -> String {
    let adjectives = [
        "Swift", "Crimson", "Silent", "Bright", "Shadow", "Frost", "Ember", "Storm", "Lunar", "Solar", "Nova", "Pixel",
        "Cyber", "Iron", "Neon", "Blaze", "Drift", "Pulse", "Glitch", "Volt",
    ];
    let nouns = [
        "Wolf", "Falcon", "Tiger", "Raven", "Phoenix", "Cobra", "Panther", "Hawk", "Lynx", "Viper", "Fox", "Bear",
        "Eagle", "Shark", "Dragon", "Knight", "Blade", "Spark", "Forge", "Core",
    ];

    let mut buf: [u8; 4] = [0; 4];
    if let Ok(mut f) = std::fs::File::open("/dev/urandom") {
        use std::io::Read;
        let _ = f.read_exact(&mut buf);
    } else {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos().to_ne_bytes();
        for (i, b) in nanos.iter().take(4).enumerate() {
            buf[i] = *b;
        }
    }
    let adj_idx = (u32::from_be_bytes([0, buf[0], buf[1], buf[2]]) as usize) % adjectives.len();
    let noun_idx = (buf[3] as usize) % nouns.len();

    format!("{}-{}", adjectives[adj_idx], nouns[noun_idx])
}

pub fn dependencies() -> Value {
    let home = dirs::home_dir().unwrap_or_default();

    let mono = check_command("mono") || check_path(&PathBuf::from("/opt/homebrew/bin/mono"));
    let rosetta = check_rosetta();
    let xcode_cli = check_command("clang") || check_command("xcodebuild");
    let steam = check_path(&home.join("Library/Application Support/Steam/Steam.app/Contents/MacOS/steam_osx"))
        || check_path(&PathBuf::from("/Applications/Steam.app/Contents/MacOS/steam_osx"));
    let homebrew = check_command("brew");
    let moltenvk = check_path(&PathBuf::from("/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"));
    let metalsharp_wine = check_path(&crate::platform::metalsharp_home_dir_for(&home).join("runtime/wine/bin/wine"))
        || check_path(&crate::platform::metalsharp_home_dir_for(&home).join("runtime/wine/bin/metalsharp-wine"));
    let host_runtime = host_runtime_installed(&home);
    let dxmt_status = crate::installer::dxmt_runtime_status();
    let dxmt_runtime = dxmt_status
        .get("dxmt")
        .and_then(|lane| lane.get("current"))
        .or_else(|| dxmt_status.get("current"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false);
    let dxmt_m12_runtime = dxmt_status
        .get("dxmt_m12")
        .and_then(|lane| lane.get("current"))
        .or_else(|| dxmt_status.get("m12Current"))
        .and_then(|v| v.as_bool())
        .unwrap_or(false);

    let all_ok =
        homebrew && rosetta && xcode_cli && metalsharp_wine && host_runtime && dxmt_runtime && dxmt_m12_runtime;

    json!({
        "ok": true,
        "allInstalled": all_ok,
        "platform": crate::platform::name(),
        "dependencies": [
            {
                "id": "homebrew",
                "name": "Homebrew",
                "desc": "Package manager — required to install other dependencies",
                "installed": homebrew,
                "required": true,
                "installCmd": "/bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"",
            },
            {
                "id": "xcode_cli",
                "name": "Xcode Command Line Tools",
                "desc": "Provides clang for building native shims (CSteamworks, gdiplus stub)",
                "installed": xcode_cli,
                "required": true,
                "installCmd": "xcode-select --install",
            },
            {
                "id": "rosetta",
                "name": "Rosetta 2",
                "desc": "x86_64 translation layer — needed for 32-bit Windows games and x86 mono",
                "installed": rosetta,
                "required": true,
                "installCmd": "softwareupdate --install-rosetta --agree-to-license",
            },
            {
                "id": "metalsharp_wine",
                "name": "MetalSharp Wine",
                "desc": "From-source Wine 11.5 with DXMT Metal D3D11, gnutls TLS, MoltenVK. Runs Windows Steam and launches games with native Metal rendering.",
                "installed": metalsharp_wine,
                "required": true,
                "installCmd": "metalsharp-setup-wine",
            },
            {
                "id": "metalsharp_host_runtime",
                "name": "MetalSharp Host Runtime ABI",
                "desc": "Bottle-aware native host service ABI used by Wine shims and launch routes.",
                "installed": host_runtime,
                "required": true,
                "installCmd": "metalsharp-setup-host-runtime",
            },
            {
                "id": "dxmt_runtime",
                "name": "DXMT M9-M11 Runtime",
                "desc": format!("Bundled D3D9/D3D10/D3D11-to-Metal runtime ({}) staged under runtime/wine/lib/dxmt.", crate::installer::DXMT_BUNDLED_RUNTIME_VERSION),
                "installed": dxmt_runtime,
                "required": true,
                "installCmd": "metalsharp-setup-dxmt",
                "status": dxmt_status.get("dxmt").cloned().unwrap_or_else(|| dxmt_status.clone()),
            },
            {
                "id": "dxmt_m12_runtime",
                "name": "DXMT M12 Runtime",
                "desc": "Isolated D3D12-to-Metal runtime staged under runtime/wine/lib/dxmt_m12 with its own DLLs and winemetal.so sidecars.",
                "installed": dxmt_m12_runtime,
                "required": true,
                "installCmd": "metalsharp-setup-dxmt-m12",
                "status": dxmt_status.get("dxmt_m12").cloned().unwrap_or_else(|| dxmt_status.clone()),
                "path": dxmt_status
                    .get("dxmt_m12")
                    .and_then(|lane| lane.get("path"))
                    .cloned()
                    .or_else(|| dxmt_status.get("m12Path").cloned())
                    .unwrap_or(json!(null)),
            },
            {
                "id": "mono",
                "name": "Mono Runtime (arm64)",
                "desc": "Required for Terraria and other arm64 FNA/XNA games",
                "installed": mono,
                "required": false,
                "installCmd": "brew install mono",
            },
            {
                "id": "moltenvk",
                "name": "MoltenVK",
                "desc": "Vulkan→Metal translation. Optional fallback graphics backend.",
                "installed": moltenvk,
                "required": false,
                "installCmd": "brew install molten-vk",
            },
            {
                "id": "steam",
                "name": "Steam Client (macOS)",
                "desc": "Provides native macOS libraries (libsteam_api.dylib) for FNA games. Install Terraria for macOS to get the best compatibility.",
                "installed": steam,
                "required": false,
                "installCmd": "https://store.steampowered.com/about/",
            },
        ],
    })
}

fn host_runtime_installed(home: &Path) -> bool {
    let host = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("host");
    file_nonempty(&host.join("manifest.json"))
        && file_nonempty(&host.join("HostRuntimeABI.h"))
        && (file_nonempty(&host.join("libmetalsharp_host_runtime.dylib"))
            || file_nonempty(&host.join("libmetalsharp_host_runtime.so"))
            || file_nonempty(&host.join("metalsharp_host_runtime.dll")))
}

fn file_nonempty(path: &Path) -> bool {
    path.metadata().map(|meta| meta.is_file() && meta.len() > 0).unwrap_or(false)
}

pub fn install_dependencies(body: &Map<String, Value>) -> Result<Value, Box<dyn std::error::Error>> {
    let ids: Vec<&str> = body
        .get("ids")
        .and_then(|v| v.as_array())
        .map(|a| a.iter().filter_map(|v| v.as_str()).collect())
        .unwrap_or_default();

    let _home = dirs::home_dir().ok_or("no home dir")?;
    let mut results = Vec::new();

    for id in ids {
        let result = match id {
            "mono" => run_brew_install("mono"),
            "sdl3" => run_brew_install("sdl3"),
            _ => json!({"id": id, "ok": false, "error": "unknown dependency"}),
        };
        results.push(result);
    }

    let all_ok = results.iter().all(|r| r.get("ok").and_then(|v| v.as_bool()).unwrap_or(false));

    Ok(json!({
        "ok": all_ok,
        "results": results,
    }))
}

fn run_brew_install(package: &str) -> Value {
    let output = std::process::Command::new("brew")
        .args(["install", package])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .output();

    match output {
        Ok(o) => {
            let success = o.status.success();
            let stdout = String::from_utf8_lossy(&o.stdout).to_string();
            let stderr = String::from_utf8_lossy(&o.stderr).to_string();
            let combined = format!("{}{}", stdout, stderr);

            if success || combined.contains("already installed") {
                json!({"id": package, "ok": true})
            } else {
                json!({"id": package, "ok": false, "error": combined.lines().last().unwrap_or("unknown error")})
            }
        },
        Err(e) => json!({"id": package, "ok": false, "error": e.to_string()}),
    }
}

pub fn resolve_game_dir(appid: u32) -> Option<PathBuf> {
    let home = dirs::home_dir()?;

    let local_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());
    if local_dir.join(".metalsharp_prepared").exists() {
        return Some(local_dir);
    }

    let dual = crate::scan::resolve_dual_game_dir(appid);

    if let Some(ref wine_dir) = dual.wine_dir {
        if wine_dir.exists() {
            return Some(wine_dir.clone());
        }
    }

    if let Some(ref macos_dir) = dual.macos_dir {
        if macos_dir.exists() {
            return Some(macos_dir.clone());
        }
    }

    if local_dir.exists() {
        return Some(local_dir);
    }

    None
}

pub fn resolve_windows_game_dir(appid: u32) -> Option<PathBuf> {
    let home = dirs::home_dir()?;

    let local_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());
    if local_dir.join(".metalsharp_prepared").exists() && crate::scan::is_windows_game_dir(&local_dir) {
        return Some(local_dir);
    }

    let dual = crate::scan::resolve_dual_game_dir(appid);
    if let Some(ref wine_dir) = dual.wine_dir {
        if wine_dir.exists() && crate::scan::is_windows_game_dir(wine_dir) {
            return Some(wine_dir.clone());
        }
    }

    if local_dir.exists() && crate::scan::is_windows_game_dir(&local_dir) {
        return Some(local_dir);
    }

    None
}

pub fn resolve_native_game_dir(appid: u32) -> Option<PathBuf> {
    crate::scan::resolve_dual_game_dir(appid).macos_dir
}

pub fn prepare_game(appid: u32) -> Result<Value, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let game_dir = resolve_game_dir(appid).ok_or_else(|| format!("game directory not found for appid {}", appid))?;

    let marker = game_dir.join(".metalsharp_prepared");

    let is_dotnet = detect_dotnet_game(&game_dir);
    let pipeline =
        if is_dotnet { crate::mtsp::engine::PipelineId::FnaArm64 } else { crate::mtsp::rules::resolve_pipeline(appid) };
    let fna_profile = crate::mtsp::launcher::find_fna_profile(appid);
    let game_type = match appid {
        945360 | 1139900 => "steam",
        620 | 265930 => "dxmt",
        _ => {
            if is_dotnet {
                fna_profile.method_label
            } else {
                pipeline.to_legacy_method()
            }
        },
    };

    if !marker.exists() {
        let _ = std::fs::write(game_dir.join("steam_appid.txt"), appid.to_string());
    }

    match appid {
        945360 | 1139900 => prepare_metalsharp_game(&game_dir, &home, appid)?,
        620 | 265930 => prepare_goldberg_game(&game_dir, &home, appid)?,
        _ => {
            if is_dotnet {
                prepare_fna_game(appid, &game_dir, &home)?;
            } else if pipeline.is_graphics_route() {
                prepare_graphics_pipeline(appid, &game_dir, &home, pipeline)?;
            }
        },
    }

    let node = crate::mtsp::engine::get_pipeline(pipeline);
    if let Some(subdir) = node.shader_cache_subdir {
        crate::mtsp::shader_cache::deploy_preset_cache(&home, subdir, appid);
    }

    std::fs::write(&marker, format!("prepared: game_type={}", game_type))?;

    Ok(json!({
        "ok": true,
        "alreadyPrepared": false,
        "gameType": game_type,
        "appid": appid,
    }))
}

fn prepare_fna_game(appid: u32, game_dir: &PathBuf, home: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let profile = crate::mtsp::launcher::find_fna_profile(appid);

    if let Some(script) = profile.setup_script {
        let _ = crate::launch::run_game_setup_script(appid);
        let _ = script;
    }

    if profile.appid == 105600 {
        prepare_terrarria(game_dir, home)?;
    } else if profile.appid == 504230 {
        prepare_celeste(game_dir, home)?;
    } else {
        let _flavor = crate::mtsp::launcher::detect_fna_flavor(game_dir);
        setup_fna_runtime(game_dir, home)?;
    }

    Ok(())
}

fn prepare_terrarria(game_dir: &PathBuf, home: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let mac_libs =
        home.join("Library/Application Support/Steam/steamapps/common/Terraria/Terraria.app/Contents/MacOS/osx");

    if mac_libs.exists() {
        for lib in
            &["libsteam_api.dylib", "libSDL2-2.0.0.dylib", "libFAudio.0.dylib", "libFNA3D.0.dylib", "libnfd.dylib"]
        {
            let src = mac_libs.join(lib);
            if src.exists() {
                let _ = std::fs::copy(&src, game_dir.join(lib));
            }
        }
        let _ = std::os::unix::fs::symlink("libSDL2-2.0.0.dylib", game_dir.join("libSDL2.dylib"));
        let _ = std::os::unix::fs::symlink("libFAudio.0.dylib", game_dir.join("libFAudio.dylib"));
        let _ = std::os::unix::fs::symlink("libFNA3D.0.dylib", game_dir.join("libFNA3D.dylib"));
    }

    let gdiplus = game_dir.join("libgdiplus.dylib");
    if !gdiplus.exists() {
        let ms_home = crate::platform::metalsharp_home_dir_for(home);
        let cached = ms_home.join("runtime").join("shims").join("libgdiplus.dylib");
        if cached.exists() {
            let _ = std::fs::copy(&cached, &gdiplus);
        } else {
            let repo = home.join("metalsharp");
            let stub_src = repo.join("src/fna/terraria/gdiplus_stub.c");
            if stub_src.exists() {
                let _ = mac_cmd("clang")
                    .args(["-shared", "-arch", "arm64", "-arch", "x86_64", "-o"])
                    .arg(&gdiplus)
                    .arg(&stub_src)
                    .args(["-install_name", "@loader_path/libgdiplus.dylib"])
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .status();
            }
        }
    }

    let launcher = game_dir.join("TerrariaLauncher.exe");
    if !launcher.exists() {
        let repo = home.join("metalsharp");
        let src = repo.join("src/fna/terraria/TerrariaLauncher.cs");
        if src.exists() {
            let _ = std::process::Command::new("mcs")
                .args(["-out"])
                .arg(&launcher)
                .args(["-target:winexe"])
                .arg(&src)
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
        }
    }

    let pipeline = game_dir.join("Microsoft.Xna.Framework.Content.Pipeline.dll");
    if !pipeline.exists() {
        let repo = home.join("metalsharp");
        let src = repo.join("src/fna/terraria/ContentPipelineStub.cs");
        if src.exists() {
            let _ = std::process::Command::new("mcs")
                .args(["-out"])
                .arg(&pipeline)
                .args(["-target:library"])
                .arg(&src)
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
        }
    }

    let xact = game_dir.join("Microsoft.Xna.Framework.Xact.dll");
    if !xact.exists() {
        let repo = home.join("metalsharp");
        let src = repo.join("src/fna/terraria/Microsoft.Xna.Framework.Xact.dll");
        if src.exists() {
            let _ = std::fs::copy(&src, &xact);
        }
    }

    Ok(())
}

fn prepare_celeste(game_dir: &PathBuf, home: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let mono_x86 =
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-x86").join("bin").join("mono");
    if !mono_x86.exists() {
        let _ = crate::launch::run_game_setup_script(504230);
    }

    let shims_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("shims");
    let _ = std::fs::create_dir_all(&shims_dir);

    let steam_dylib = find_steam_api(home);
    if let Some(ref src) = steam_dylib {
        let _ = std::fs::copy(src, game_dir.join("libsteam_api.dylib"));
        let _ = std::fs::copy(src, shims_dir.join("libsteam_api.dylib"));
    }

    let csteamworks = game_dir.join("libCSteamworks.dylib");
    if !csteamworks.exists() {
        let cached = shims_dir.join("libCSteamworks.dylib");
        if cached.exists() {
            let _ = std::fs::copy(&cached, &csteamworks);
        } else {
            let repo = home.join("metalsharp");
            let shim_src = repo.join("src/fna/shims/csteamworks_shim.c");
            if shim_src.exists() {
                let _ = mac_cmd("clang")
                    .args(["-shared", "-arch", "arm64", "-arch", "x86_64"])
                    .arg("-o")
                    .arg(&csteamworks)
                    .arg(&shim_src)
                    .args(["-undefined", "dynamic_lookup", "-install_name", "@loader_path/libCSteamworks.dylib"])
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .status();
                let _ = std::fs::copy(&csteamworks, shims_dir.join("libCSteamworks.dylib"));
            }
        }
    }

    for fmod in &["libfmod.dylib", "libfmodstudio.dylib"] {
        let dst = game_dir.join(fmod);
        if !dst.exists() {
            let cached = shims_dir.join(fmod);
            if cached.exists() {
                let _ = std::fs::copy(&cached, &dst);
            } else {
                let stub_name = fmod.replace("lib", "").replace(".dylib", "_stub.c");
                let repo = home.join("metalsharp");
                let stub_src = repo.join("src/fna/shims").join(&stub_name);
                if stub_src.exists() {
                    let _ = mac_cmd("clang")
                        .args(["-shared", "-arch", "arm64", "-arch", "x86_64"])
                        .arg("-o")
                        .arg(&dst)
                        .arg(&stub_src)
                        .args(["-undefined", "dynamic_lookup", "-install_name", &format!("@loader_path/{}", fmod)])
                        .stdout(std::process::Stdio::null())
                        .stderr(std::process::Stdio::null())
                        .status();
                    let _ = std::fs::copy(&dst, shims_dir.join(fmod));
                }
            }
        }
    }

    Ok(())
}

fn prepare_graphics_pipeline(
    appid: u32,
    game_dir: &PathBuf,
    home: &PathBuf,
    pipeline: crate::mtsp::engine::PipelineId,
) -> Result<(), Box<dyn std::error::Error>> {
    let marker = game_dir.join(".metalsharp_prepared");
    if pipeline == crate::mtsp::engine::PipelineId::Vkd3d {
        crate::installer::ensure_vkd3d_runtime_ready(home)?;
        crate::mtsp::launcher::prepare_steam_pipeline_env(appid, pipeline)?;
    }
    stage_packaged_steam_runtime_for_game(appid, game_dir)?;
    if pipeline == crate::mtsp::engine::PipelineId::M12 {
        stage_agility_sdk_for_game(appid, game_dir, home)?;
    }
    if !marker.exists() {
        let _ = std::fs::write(&marker, pipeline.to_legacy_method());
    }
    Ok(())
}

fn stage_packaged_steam_runtime_for_game(appid: u32, game_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let exe_path = crate::mtsp::recipe::resolve_game_exe(appid, game_dir)?;
    let exe_dir = exe_path.parent().ok_or("game exe parent not found")?;
    let appid_text = appid.to_string();

    stage_text_file_if_needed(&game_dir.join("steam_appid.txt"), &appid_text)?;
    stage_text_file_if_needed(&exe_dir.join("steam_appid.txt"), &appid_text)?;

    if let Some(steam_api64) = find_packaged_steam_api64(game_dir) {
        let dest = exe_dir.join("steam_api64.dll");
        if !dest.exists() || !same_file_contents(&steam_api64, &dest) {
            stage_sdk_file(&steam_api64, &dest)?;
        }
    }

    Ok(())
}

fn stage_text_file_if_needed(dest: &Path, contents: &str) -> Result<(), Box<dyn std::error::Error>> {
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)?;
    }
    if std::fs::read_to_string(dest).ok().as_deref() == Some(contents) {
        return Ok(());
    }
    std::fs::write(dest, contents)?;
    Ok(())
}

fn find_packaged_steam_api64(game_dir: &Path) -> Option<PathBuf> {
    let candidates = [
        game_dir
            .join("Engine")
            .join("Binaries")
            .join("ThirdParty")
            .join("Steamworks")
            .join("Steamv157")
            .join("Win64")
            .join("steam_api64.dll"),
        game_dir.join("steam_api64.dll"),
    ];

    candidates.into_iter().find(|path| path.exists())
}

#[derive(Debug, Clone)]
struct AgilitySdkRequirement {
    sdk_version: Option<u32>,
    sdk_path: String,
}

#[derive(Debug, Clone)]
pub(crate) struct AgilityStageReport {
    pub(crate) package_version: String,
    pub(crate) sdk_version: Option<u32>,
    pub(crate) sdk_path: String,
    pub(crate) target_dirs: Vec<PathBuf>,
    pub(crate) staged_files: Vec<PathBuf>,
    pub(crate) app_local_sidecars_skipped: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct AgilitySdkInspection {
    pub(crate) package_version: String,
    pub(crate) sdk_version: Option<u32>,
    pub(crate) sdk_path: String,
    pub(crate) target_dirs: Vec<PathBuf>,
    pub(crate) missing_files: Vec<PathBuf>,
    pub(crate) present_files: Vec<PathBuf>,
    pub(crate) shared_payload_ready: bool,
    pub(crate) shared_repair_marker_ready: bool,
    pub(crate) app_local_sidecars_present: bool,
    pub(crate) app_local_sidecars_skipped: bool,
}

impl AgilitySdkInspection {
    pub(crate) fn installed(&self) -> bool {
        if self.app_local_sidecars_skipped {
            return self.shared_payload_ready && self.shared_repair_marker_ready && !self.app_local_sidecars_present;
        }
        !self.target_dirs.is_empty() && self.missing_files.is_empty()
    }

    pub(crate) fn partially_staged(&self) -> bool {
        !self.present_files.is_empty() || self.shared_payload_ready
    }
}

pub(crate) fn stage_agility_sdk_for_game(
    appid: u32,
    game_dir: &Path,
    home: &Path,
) -> Result<(), Box<dyn std::error::Error>> {
    stage_agility_sdk_for_game_report(appid, game_dir, home).map(|_| ())
}

pub(crate) fn stage_agility_sdk_for_game_report(
    appid: u32,
    game_dir: &Path,
    home: &Path,
) -> Result<AgilityStageReport, Box<dyn std::error::Error>> {
    let exe_path = crate::mtsp::recipe::resolve_game_exe(appid, game_dir)?;
    let exe_dir = exe_path.parent().ok_or("game exe parent not found")?;
    let requirement = read_game_agility_requirement(&exe_path);
    let package_version = agility_package_version_for_requirement(requirement.sdk_version).to_string();
    let agility_bin = ensure_agility_sdk_bin(home, requirement.sdk_version).ok_or_else(|| {
        let version = requirement.sdk_version.map(|value| value.to_string()).unwrap_or_else(|| "default".to_string());
        format!("Agility SDK x64 payload not found for version {}", version)
    })?;
    validate_agility_bin_payload(&agility_bin)?;

    if agility_uses_shared_payload_only(appid) {
        remove_app_local_agility_sidecars(game_dir, exe_dir)?;
        write_shared_agility_repair_marker(
            appid,
            game_dir,
            &package_version,
            requirement.sdk_version,
            &requirement.sdk_path,
        )?;
        return Ok(AgilityStageReport {
            package_version,
            sdk_version: requirement.sdk_version,
            sdk_path: requirement.sdk_path,
            target_dirs: Vec::new(),
            staged_files: required_agility_payload_files()
                .iter()
                .map(|file| agility_bin.join(file))
                .filter(|path| path.exists())
                .collect(),
            app_local_sidecars_skipped: true,
        });
    }

    let dxil_dll = ensure_dxil_dll(home);

    let targets = resolve_agility_target_dirs(exe_dir, &requirement.sdk_path);
    let mut staged_files = Vec::new();

    for target_dir in &targets {
        std::fs::create_dir_all(target_dir)?;
        for dll in required_agility_payload_files() {
            let source = agility_bin.join(dll);
            if !source.exists() {
                return Err(format!("Missing Agility SDK DLL: {}", source.display()).into());
            }
            let dest = target_dir.join(dll);
            stage_sdk_file(&source, &dest)?;
            staged_files.push(dest);
        }

        for optional in ["D3D12StateObjectCompiler.dll", "d3dconfig.exe"] {
            let source = agility_bin.join(optional);
            if source.exists() {
                let dest = target_dir.join(optional);
                stage_sdk_file(&source, &dest)?;
                staged_files.push(dest);
            }
        }

        if let Some(source) = &dxil_dll {
            let dest = target_dir.join("dxil.dll");
            stage_sdk_file(source, &dest)?;
            staged_files.push(dest);
        }
    }

    let inspection = inspect_agility_sdk_for_game(appid, game_dir, home)?;
    if !inspection.installed() {
        let missing =
            inspection.missing_files.iter().map(|path| path.display().to_string()).collect::<Vec<_>>().join(", ");
        return Err(format!("Agility SDK staging incomplete; missing {}", missing).into());
    }

    Ok(AgilityStageReport {
        package_version,
        sdk_version: requirement.sdk_version,
        sdk_path: requirement.sdk_path,
        target_dirs: targets,
        staged_files,
        app_local_sidecars_skipped: false,
    })
}

fn remove_app_local_agility_sidecars(game_dir: &Path, exe_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    for target in app_local_agility_sidecar_dirs(game_dir, exe_dir) {
        if target.is_dir() {
            std::fs::remove_dir_all(target)?;
        }
    }

    Ok(())
}

fn app_local_agility_sidecar_dirs(game_dir: &Path, exe_dir: &Path) -> Vec<PathBuf> {
    let mut roots = vec![exe_dir.to_path_buf()];
    let engine_bin = game_dir.join("Engine").join("Binaries").join("Win64");
    if engine_bin.is_dir() && !roots.iter().any(|root| root == &engine_bin) {
        roots.push(engine_bin);
    }
    roots.into_iter().map(|root| root.join("D3D12")).collect()
}

fn app_local_agility_sidecars_present(game_dir: &Path, exe_dir: &Path) -> bool {
    app_local_agility_sidecar_dirs(game_dir, exe_dir).into_iter().any(|target| target.is_dir())
}

pub(crate) fn inspect_agility_sdk_for_game(
    appid: u32,
    game_dir: &Path,
    home: &Path,
) -> Result<AgilitySdkInspection, Box<dyn std::error::Error>> {
    let exe_path = crate::mtsp::recipe::resolve_game_exe(appid, game_dir)?;
    let exe_dir = exe_path.parent().ok_or("game exe parent not found")?;
    let requirement = read_game_agility_requirement(&exe_path);
    let package_version = agility_package_version_for_requirement(requirement.sdk_version).to_string();
    let shared_payload_ready = find_agility_sdk_bin(home, requirement.sdk_version)
        .as_deref()
        .map(validate_agility_bin_payload)
        .transpose()?
        .is_some();

    if agility_uses_shared_payload_only(appid) {
        let shared_repair_marker_ready =
            shared_agility_repair_marker_matches(appid, game_dir, &package_version, requirement.sdk_version);
        return Ok(AgilitySdkInspection {
            package_version,
            sdk_version: requirement.sdk_version,
            sdk_path: requirement.sdk_path,
            target_dirs: Vec::new(),
            missing_files: Vec::new(),
            present_files: Vec::new(),
            shared_payload_ready,
            shared_repair_marker_ready,
            app_local_sidecars_present: app_local_agility_sidecars_present(game_dir, exe_dir),
            app_local_sidecars_skipped: true,
        });
    }

    let target_dirs = resolve_agility_target_dirs(exe_dir, &requirement.sdk_path);
    let mut missing_files = Vec::new();
    let mut present_files = Vec::new();
    for target_dir in &target_dirs {
        for dll in required_agility_payload_files() {
            let path = target_dir.join(dll);
            if path.exists() {
                present_files.push(path);
            } else {
                missing_files.push(path);
            }
        }
    }

    Ok(AgilitySdkInspection {
        package_version,
        sdk_version: requirement.sdk_version,
        sdk_path: requirement.sdk_path,
        target_dirs,
        missing_files,
        present_files,
        shared_payload_ready,
        shared_repair_marker_ready: false,
        app_local_sidecars_present: false,
        app_local_sidecars_skipped: false,
    })
}

fn agility_uses_shared_payload_only(appid: u32) -> bool {
    appid == 1962700
}

fn required_agility_payload_files() -> &'static [&'static str] {
    &["D3D12Core.dll", "d3d12SDKLayers.dll"]
}

fn validate_agility_bin_payload(dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    for dll in required_agility_payload_files() {
        let source = dir.join(dll);
        if !source.exists() {
            return Err(format!("Missing Agility SDK DLL: {}", source.display()).into());
        }
    }
    Ok(())
}

fn shared_agility_repair_marker_path(appid: u32, game_dir: &Path) -> PathBuf {
    game_dir.join(".metalsharp").join("agility").join(format!("shared-payload-{}.json", appid))
}

fn write_shared_agility_repair_marker(
    appid: u32,
    game_dir: &Path,
    package_version: &str,
    sdk_version: Option<u32>,
    sdk_path: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let marker = shared_agility_repair_marker_path(appid, game_dir);
    if let Some(parent) = marker.parent() {
        std::fs::create_dir_all(parent)?;
    }
    std::fs::write(
        marker,
        serde_json::to_vec_pretty(&json!({
            "appid": appid,
            "package_version": package_version,
            "sdk_version": sdk_version,
            "sdk_path": sdk_path,
            "app_local_sidecars_removed": true,
        }))?,
    )?;
    Ok(())
}

fn shared_agility_repair_marker_matches(
    appid: u32,
    game_dir: &Path,
    package_version: &str,
    sdk_version: Option<u32>,
) -> bool {
    let marker = shared_agility_repair_marker_path(appid, game_dir);
    let Ok(contents) = std::fs::read_to_string(marker) else {
        return false;
    };
    let Ok(value) = serde_json::from_str::<Value>(&contents) else {
        return false;
    };
    let marker_sdk_version = value.get("sdk_version").and_then(Value::as_u64).map(|version| version as u32);
    value.get("appid").and_then(Value::as_u64) == Some(appid as u64)
        && value.get("package_version").and_then(Value::as_str) == Some(package_version)
        && marker_sdk_version == sdk_version
        && value.get("app_local_sidecars_removed").and_then(Value::as_bool).unwrap_or(false)
}

fn stage_sdk_file(source: &Path, dest: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let backup =
        dest.with_file_name(format!("{}.orig", dest.file_name().and_then(|name| name.to_str()).unwrap_or("sdk-file")));
    if dest.exists() && !same_file_contents(source, dest) && !backup.exists() {
        std::fs::copy(dest, &backup)?;
    }
    std::fs::copy(source, dest)?;
    Ok(())
}

fn read_game_agility_requirement(exe_path: &Path) -> AgilitySdkRequirement {
    if let Some(exports) = crate::mtsp::pe::read_agility_exports(exe_path) {
        return AgilitySdkRequirement {
            sdk_version: Some(exports.sdk_version),
            sdk_path: if exports.sdk_path.is_empty() { ".\\D3D12\\".to_string() } else { exports.sdk_path },
        };
    }

    AgilitySdkRequirement {
        sdk_version: crate::mtsp::pe::read_export_u32(exe_path, "D3D12SDKVersion"),
        sdk_path: read_game_agility_path_hint(exe_path).unwrap_or_else(|| ".\\D3D12\\".to_string()),
    }
}

fn read_game_agility_path_hint(exe_path: &Path) -> Option<String> {
    let data = std::fs::read(exe_path).ok()?;
    for needle in [b".\\D3D12\\x64\\".as_slice(), b".\\D3D12\\".as_slice()] {
        if let Some(offset) = find_bytes(&data, needle) {
            return String::from_utf8(data[offset..offset + needle.len()].to_vec()).ok();
        }
    }
    None
}

fn find_bytes(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    haystack.windows(needle.len()).position(|window| window == needle)
}

fn ensure_agility_sdk_bin(home: &Path, required_version: Option<u32>) -> Option<PathBuf> {
    if let Some(found) = find_agility_sdk_bin(home, required_version) {
        return Some(found);
    }

    let package_version = agility_package_version_for_requirement(required_version);
    fetch_agility_sdk_bin(home, package_version, required_version)?;
    find_agility_sdk_bin(home, required_version)
}

fn find_agility_sdk_bin(home: &Path, required_version: Option<u32>) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("METALSHARP_AGILITY_BIN") {
        candidates.push(PathBuf::from(path));
    }

    if let Ok(cwd) = std::env::current_dir() {
        push_agility_candidates(&mut candidates, &cwd);
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_agility_candidates(&mut candidates, dir);
        }
    }

    if let Some(resources) = crate::platform::app_resources_dir() {
        push_agility_candidates(&mut candidates, &resources);
    }

    let package_version = agility_package_version_for_requirement(required_version);
    push_cached_agility_package_candidates(&mut candidates, home, package_version);

    candidates.push(
        home.join("Dev").join("metalsharp").join("tools").join("d3d12-metal-sdk").join("out").join("bin").join("D3D12"),
    );
    candidates.push(
        home.join("repos")
            .join("metalsharp")
            .join("tools")
            .join("d3d12-metal-sdk")
            .join("out")
            .join("bin")
            .join("D3D12"),
    );

    candidates.into_iter().find(|dir| agility_bin_matches(dir, required_version))
}

fn agility_bin_matches(dir: &Path, required_version: Option<u32>) -> bool {
    let core = dir.join("D3D12Core.dll");
    let layers = dir.join("d3d12SDKLayers.dll");
    if !core.exists() || !layers.exists() {
        return false;
    }
    match required_version {
        Some(version) => crate::mtsp::pe::read_export_u32(&core, "D3D12SDKVersion") == Some(version),
        None => true,
    }
}

fn fetch_agility_sdk_bin(home: &Path, package_version: &str, required_version: Option<u32>) -> Option<PathBuf> {
    if let Some(path) = fetch_agility_sdk_bin_with_script(home, package_version, required_version) {
        return Some(path);
    }
    fetch_agility_sdk_bin_native(home, package_version, required_version).ok()
}

fn fetch_agility_sdk_bin_with_script(
    home: &Path,
    package_version: &str,
    required_version: Option<u32>,
) -> Option<PathBuf> {
    let script = find_agility_fetch_script(home)?;
    let output = mac_cmd("bash").arg(&script).arg("--version").arg(package_version).output().ok()?;
    if !output.status.success() {
        return None;
    }
    let path = String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(str::trim)
        .rfind(|line| !line.is_empty())
        .map(PathBuf::from)?;
    agility_bin_matches(&path, required_version).then_some(path)
}

fn fetch_agility_sdk_bin_native(
    home: &Path,
    package_version: &str,
    required_version: Option<u32>,
) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let package_dir = agility_user_package_dir(home, package_version);
    let bin_dir = package_dir.join("build").join("native").join("bin").join("x64");
    if agility_bin_matches(&bin_dir, required_version) {
        return Ok(bin_dir);
    }

    let download_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("downloads");
    std::fs::create_dir_all(&download_dir)?;
    let package_file = download_dir.join(format!("Microsoft.Direct3D.D3D12.{}.nupkg", package_version));
    download_agility_package(package_version, &package_file)?;
    extract_agility_package(&package_file, &package_dir)?;

    if agility_bin_matches(&bin_dir, required_version) {
        Ok(bin_dir)
    } else {
        Err(format!("Agility package {} did not contain matching x64 payload", package_version).into())
    }
}

fn download_agility_package(package_version: &str, package_file: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let url = agility_package_download_url(package_version);
    let config =
        ureq::config::Config::builder().user_agent(format!("MetalSharp/{}", env!("CARGO_PKG_VERSION"))).build();
    let agent = ureq::Agent::new_with_config(config);
    let resp = agent.get(&url).call().map_err(|err| format!("Agility SDK download failed: {}", err))?;
    let tmp_file = package_file.with_extension("nupkg.tmp");
    let mut input = resp.into_body().into_reader();
    let mut output = std::fs::File::create(&tmp_file)?;
    std::io::copy(&mut input, &mut output)?;
    std::fs::rename(tmp_file, package_file)?;
    Ok(())
}

fn agility_package_download_url(package_version: &str) -> String {
    format!(
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/{0}/microsoft.direct3d.d3d12.{0}.nupkg",
        package_version.to_ascii_lowercase()
    )
}

fn extract_agility_package(package_file: &Path, package_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let file = std::fs::File::open(package_file)?;
    let mut archive = zip::ZipArchive::new(file)?;
    let tmp_dir = package_dir.with_extension("extracting");
    if tmp_dir.exists() {
        std::fs::remove_dir_all(&tmp_dir)?;
    }
    std::fs::create_dir_all(&tmp_dir)?;

    for i in 0..archive.len() {
        let mut entry = archive.by_index(i)?;
        let Some(enclosed) = entry.enclosed_name() else {
            continue;
        };
        if !is_agility_runtime_payload(&enclosed) {
            continue;
        }
        let dest = tmp_dir.join(enclosed);
        if entry.is_dir() {
            std::fs::create_dir_all(&dest)?;
            continue;
        }
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut out = std::fs::File::create(&dest)?;
        std::io::copy(&mut entry, &mut out)?;
    }

    if package_dir.exists() {
        std::fs::remove_dir_all(package_dir)?;
    }
    std::fs::rename(tmp_dir, package_dir)?;
    Ok(())
}

fn is_agility_runtime_payload(path: &Path) -> bool {
    let mut parts = path.iter().filter_map(|part| part.to_str());
    matches!(parts.next(), Some("build"))
        && matches!(parts.next(), Some("native"))
        && matches!(parts.next(), Some("bin"))
        && matches!(parts.next(), Some("x64"))
}

fn find_agility_fetch_script(home: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();

    if let Ok(cwd) = std::env::current_dir() {
        push_agility_script_candidates(&mut candidates, &cwd);
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_agility_script_candidates(&mut candidates, dir);
        }
    }

    if let Some(resources) = crate::platform::app_resources_dir() {
        push_agility_script_candidates(&mut candidates, &resources);
    }

    candidates.push(
        home.join("Dev")
            .join("metalsharp")
            .join("tools")
            .join("d3d12-metal-sdk")
            .join("scripts")
            .join("fetch-agility.sh"),
    );

    candidates.into_iter().find(|path| path.exists())
}

fn push_agility_script_candidates(candidates: &mut Vec<PathBuf>, start: &Path) {
    let mut current = Some(start);
    for _ in 0..8 {
        let Some(dir) = current else {
            break;
        };
        candidates.push(dir.join("tools").join("d3d12-metal-sdk").join("scripts").join("fetch-agility.sh"));
        current = dir.parent();
    }
}

fn push_cached_agility_package_candidates(candidates: &mut Vec<PathBuf>, home: &Path, package_version: &str) {
    candidates
        .push(agility_user_package_dir(home, package_version).join("build").join("native").join("bin").join("x64"));
    candidates.push(
        home.join("Dev")
            .join("metalsharp")
            .join("tools")
            .join("d3d12-metal-sdk")
            .join("out")
            .join("agility")
            .join(package_version)
            .join("build")
            .join("native")
            .join("bin")
            .join("x64"),
    );
}

fn agility_user_package_dir(home: &Path, package_version: &str) -> PathBuf {
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("redist").join("agility").join(package_version)
}

fn agility_package_version(sdk_version: u32) -> Option<&'static str> {
    match sdk_version {
        4 => Some("1.4.10"),
        600 => Some("1.600.10"),
        602 => Some("1.602.4"),
        606 => Some("1.606.4"),
        608 => Some("1.608.3"),
        610 => Some("1.610.4"),
        611 => Some("1.611.2"),
        613 => Some("1.613.3"),
        614 => Some("1.614.1"),
        615 => Some("1.615.1"),
        616 => Some("1.616.1"),
        618 => Some("1.618.5"),
        619 => Some("1.619.3"),
        _ => None,
    }
}

fn agility_preview_package_version(sdk_version: u32) -> Option<&'static str> {
    match sdk_version {
        700 => Some("1.700.10-preview"),
        706 => Some("1.706.4-preview"),
        710 => Some("1.710.0-preview"),
        711 => Some("1.711.3-preview"),
        714 => Some("1.714.0-preview"),
        715 => Some("1.715.0-preview"),
        716 => Some("1.716.1-preview"),
        717 => Some("1.717.1-preview"),
        719 => Some("1.719.1-preview"),
        720 => Some("1.720.0-preview"),
        721 => Some("1.721.0-preview"),
        _ => None,
    }
}

fn agility_package_version_for_requirement(required_version: Option<u32>) -> &'static str {
    required_version
        .and_then(agility_package_version)
        .or_else(|| required_version.and_then(agility_preview_package_version))
        .unwrap_or(DEFAULT_AGILITY_PACKAGE_VERSION)
}

fn agility_all_known_retail_versions() -> &'static [(u32, &'static str)] {
    &[
        (4, "1.4.10"),
        (600, "1.600.10"),
        (602, "1.602.4"),
        (606, "1.606.4"),
        (608, "1.608.3"),
        (610, "1.610.4"),
        (611, "1.611.2"),
        (613, "1.613.3"),
        (614, "1.614.1"),
        (615, "1.615.1"),
        (616, "1.616.1"),
        (618, "1.618.5"),
        (619, "1.619.3"),
    ]
}

fn agility_all_known_preview_versions() -> &'static [(u32, &'static str)] {
    &[
        (700, "1.700.10-preview"),
        (706, "1.706.4-preview"),
        (710, "1.710.0-preview"),
        (711, "1.711.3-preview"),
        (714, "1.714.0-preview"),
        (715, "1.715.0-preview"),
        (716, "1.716.1-preview"),
        (717, "1.717.1-preview"),
        (719, "1.719.1-preview"),
        (720, "1.720.0-preview"),
        (721, "1.721.0-preview"),
    ]
}

pub fn agility_known_sdk_versions() -> Value {
    let retail: Vec<Value> = agility_all_known_retail_versions()
        .iter()
        .map(|&(sdk, pkg)| {
            json!({
                "sdk_version": sdk,
                "package_version": pkg,
                "channel": "retail"
            })
        })
        .collect();
    let preview: Vec<Value> = agility_all_known_preview_versions()
        .iter()
        .map(|&(sdk, pkg)| {
            json!({
                "sdk_version": sdk,
                "package_version": pkg,
                "channel": "preview"
            })
        })
        .collect();
    json!({
        "default": DEFAULT_AGILITY_PACKAGE_VERSION,
        "retail": retail,
        "preview": preview
    })
}

fn resolve_agility_target_dirs(exe_dir: &Path, sdk_path: &str) -> Vec<PathBuf> {
    let mut targets = Vec::new();
    let root_target = exe_dir.join("D3D12");
    let resolved = resolve_relative_windows_path(exe_dir, sdk_path);
    targets.push(resolved.clone());
    targets.push(root_target.clone());
    if resolved.file_name().and_then(|name| name.to_str()) == Some("x64") {
        if let Some(parent) = resolved.parent() {
            targets.push(parent.to_path_buf());
        }
    } else {
        targets.push(root_target.join("x64"));
    }

    let mut deduped = Vec::new();
    for path in targets {
        if !deduped.contains(&path) {
            deduped.push(path);
        }
    }
    deduped
}

fn resolve_relative_windows_path(base_dir: &Path, sdk_path: &str) -> PathBuf {
    let trimmed = sdk_path.trim().trim_matches('\0');
    let mut relative = trimmed.replace('/', "\\");
    while let Some(rest) = relative.strip_prefix(".\\") {
        relative = rest.to_string();
    }
    let mut resolved = base_dir.to_path_buf();
    for component in relative.split('\\').filter(|part| !part.is_empty() && *part != ".") {
        resolved.push(component);
    }
    resolved
}

fn ensure_dxil_dll(home: &Path) -> Option<PathBuf> {
    find_dxil_dll(home).or_else(|| fetch_dxil_dll(home))
}

fn find_dxil_dll(home: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("METALSHARP_DXIL_DLL") {
        candidates.push(PathBuf::from(path));
    }
    if let Ok(cwd) = std::env::current_dir() {
        push_dxil_candidates(&mut candidates, &cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_dxil_candidates(&mut candidates, dir);
        }
    }
    if let Some(resources) = crate::platform::app_resources_dir() {
        push_dxil_candidates(&mut candidates, &resources);
    }
    candidates.push(
        home.join("Dev")
            .join("metalsharp")
            .join("tools")
            .join("d3d12-metal-sdk")
            .join("out")
            .join("bin")
            .join("dxil.dll"),
    );
    candidates.push(
        home.join("repos")
            .join("metalsharp")
            .join("tools")
            .join("d3d12-metal-sdk")
            .join("out")
            .join("bin")
            .join("dxil.dll"),
    );

    candidates.into_iter().find(|path| path.exists())
}

fn fetch_dxil_dll(home: &Path) -> Option<PathBuf> {
    let script = find_dxc_fetch_script(home)?;
    let output = mac_cmd("bash").arg(&script).output().ok()?;
    if !output.status.success() {
        return None;
    }
    let dir = String::from_utf8_lossy(&output.stdout)
        .lines()
        .map(str::trim)
        .rfind(|line| !line.is_empty())
        .map(PathBuf::from)?;
    let path = dir.join("dxil.dll");
    path.exists().then_some(path)
}

fn find_dxc_fetch_script(home: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        push_dxc_script_candidates(&mut candidates, &cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            push_dxc_script_candidates(&mut candidates, dir);
        }
    }
    if let Some(resources) = crate::platform::app_resources_dir() {
        push_dxc_script_candidates(&mut candidates, &resources);
    }
    candidates.push(
        home.join("Dev").join("metalsharp").join("tools").join("d3d12-metal-sdk").join("scripts").join("fetch-dxc.sh"),
    );
    candidates.into_iter().find(|path| path.exists())
}

fn push_dxc_script_candidates(candidates: &mut Vec<PathBuf>, start: &Path) {
    let mut current = Some(start);
    for _ in 0..8 {
        let Some(dir) = current else {
            break;
        };
        candidates.push(dir.join("tools").join("d3d12-metal-sdk").join("scripts").join("fetch-dxc.sh"));
        current = dir.parent();
    }
}

fn push_dxil_candidates(candidates: &mut Vec<PathBuf>, start: &Path) {
    let mut current = Some(start);
    for _ in 0..8 {
        let Some(dir) = current else {
            break;
        };
        candidates.push(dir.join("tools").join("d3d12-metal-sdk").join("out").join("bin").join("dxil.dll"));
        current = dir.parent();
    }
}

fn push_agility_candidates(candidates: &mut Vec<PathBuf>, start: &Path) {
    let mut current = Some(start);
    for _ in 0..8 {
        let Some(dir) = current else {
            break;
        };
        candidates.push(dir.join("tools").join("d3d12-metal-sdk").join("out").join("bin").join("D3D12"));
        current = dir.parent();
    }
}

fn same_file_contents(left: &Path, right: &Path) -> bool {
    match (std::fs::read(left), std::fs::read(right)) {
        (Ok(a), Ok(b)) => a == b,
        _ => false,
    }
}

fn prepare_metalsharp_game(game_dir: &PathBuf, home: &PathBuf, appid: u32) -> Result<(), Box<dyn std::error::Error>> {
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = ms_root.join("bin").join("wine");

    let prefix = crate::platform::metalsharp_home_dir_for(&home).join(format!("prefix-{}", appid));
    let prefix_str = prefix.to_string_lossy().to_string();

    if !prefix.exists() {
        std::fs::create_dir_all(&prefix)?;
        if wine.exists() {
            let mut cmd = std::process::Command::new(&wine);
            cmd.env("WINEPREFIX", &prefix_str)
                .env("WINEDEBUGGER", "none")
                .arg("wineboot")
                .arg("--init")
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null());
            crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
            let _ = cmd.status();
        }
    }

    let marker = game_dir.join(".metalsharp_prepared");
    if !marker.exists() {
        let _ = std::fs::write(&marker, format!("metalsharp_wine_{}", appid));
    }

    Ok(())
}

fn prepare_goldberg_game(game_dir: &PathBuf, home: &PathBuf, appid: u32) -> Result<(), Box<dyn std::error::Error>> {
    let goldberg_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("goldberg");
    let goldberg_x86 = goldberg_dir.join("x86").join("steam_api.dll");
    let goldberg_x64 = goldberg_dir.join("x64").join("steam_api64.dll");

    if !goldberg_x86.exists() || !goldberg_x64.exists() {
        ensure_goldberg_downloaded(home)?;
    }

    let deploy_dir = match appid {
        620 => {
            let bin = game_dir.join("bin");
            if bin.exists() {
                bin
            } else {
                game_dir.clone()
            }
        },
        265930 => {
            let bin = game_dir.join("Binaries").join("Win32");
            if bin.exists() {
                bin
            } else {
                game_dir.clone()
            }
        },
        _ => game_dir.clone(),
    };

    deploy_goldberg_to_dir(&deploy_dir, &goldberg_dir, appid)?;

    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let ms_wine = crate::platform::runtime_wine_binary(&ms_root);
    if !prefix.join("drive_c/windows/system32").exists() {
        let _ = std::fs::create_dir_all(&prefix);
        if ms_wine.exists() {
            let mut cmd = std::process::Command::new(&ms_wine);
            cmd.env("WINEPREFIX", prefix.to_string_lossy().to_string())
                .env("WINEDEBUGGER", "none")
                .arg("wineboot")
                .arg("--init")
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null());
            crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
            let _ = cmd.status();
        }
    }

    Ok(())
}

fn ensure_goldberg_downloaded(home: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let goldberg_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("goldberg");
    let goldberg_x86 = goldberg_dir.join("x86").join("steam_api.dll");
    let goldberg_x64 = goldberg_dir.join("x64").join("steam_api64.dll");

    if goldberg_x86.exists() && goldberg_x64.exists() {
        return Ok(());
    }

    std::fs::create_dir_all(goldberg_dir.join("x86"))?;
    std::fs::create_dir_all(goldberg_dir.join("x64"))?;

    let tmpdir = crate::platform::metalsharp_home_dir_for(&home).join("tmp").join("goldberg-download");
    let _ = std::fs::create_dir_all(&tmpdir);

    let gbe_fork_url = "https://api.github.com/repos/Detanup01/gbe_fork/releases/latest";
    let output = mac_cmd("curl").args(["-sL", gbe_fork_url]).stdout(std::process::Stdio::piped()).output()?;

    let json_str = String::from_utf8_lossy(&output.stdout);
    let mut download_url: Option<String> = None;

    if let Ok(parsed) = serde_json::from_str::<serde_json::Value>(&json_str) {
        if let Some(assets) = parsed.get("assets").and_then(|a| a.as_array()) {
            for asset in assets {
                if let Some(name) = asset.get("name").and_then(|n| n.as_str()) {
                    if name.contains("win-release") && name.ends_with(".7z") {
                        if let Some(url) = asset.get("browser_download_url").and_then(|u| u.as_str()) {
                            download_url = Some(url.to_string());
                            break;
                        }
                    }
                }
            }
        }
    }

    let url = match download_url {
        Some(u) => u,
        None => "https://gitlab.com/Mr_Goldberg/goldberg_emulator/-/jobs/artifacts/master/download?job=win_release"
            .to_string(),
    };

    let archive_path = tmpdir.join("goldberg.7z");
    let dl_status = mac_cmd("curl")
        .args(["-sL", "-o"])
        .arg(&archive_path)
        .arg(&url)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()?;

    if !dl_status.success() {
        return Err("failed to download Goldberg emulator".into());
    }

    let extract_dir = tmpdir.join("extracted");
    let _ = std::fs::create_dir_all(&extract_dir);

    let has_7z = mac_cmd("which").arg("7z").output().map(|o| o.status.success()).unwrap_or(false);
    let has_bsdtar = mac_cmd("which").arg("bsdtar").output().map(|o| o.status.success()).unwrap_or(false);

    if has_7z {
        let _ = std::process::Command::new("7z")
            .args(["x", "-y"])
            .arg(format!("-o{}", extract_dir.to_string_lossy()))
            .arg(&archive_path)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    } else if has_bsdtar {
        let _ = std::process::Command::new("bsdtar")
            .arg("-xf")
            .arg(&archive_path)
            .arg("-C")
            .arg(&extract_dir)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    } else {
        return Err("need 7z or bsdtar to extract Goldberg archive".into());
    }

    let x86_dll = find_file_recursive(&extract_dir, "steam_api.dll");
    let x64_dll = find_file_recursive(&extract_dir, "steam_api64.dll");

    match (x86_dll, x64_dll) {
        (Some(x86), Some(x64)) => {
            std::fs::copy(&x86, &goldberg_x86)?;
            std::fs::copy(&x64, &goldberg_x64)?;
        },
        _ => return Err("Goldberg DLLs not found in downloaded archive".into()),
    }

    let _ = std::fs::remove_dir_all(&tmpdir);

    Ok(())
}

fn deploy_goldberg_to_dir(
    target_dir: &PathBuf,
    goldberg_dir: &PathBuf,
    appid: u32,
) -> Result<(), Box<dyn std::error::Error>> {
    let goldberg_x86 = goldberg_dir.join("x86").join("steam_api.dll");
    let goldberg_x64 = goldberg_dir.join("x64").join("steam_api64.dll");

    if !goldberg_x86.exists() || !goldberg_x64.exists() {
        return Err("Goldberg DLLs not found — download failed or was skipped".into());
    }

    std::fs::create_dir_all(target_dir)?;

    let x86_dst = target_dir.join("steam_api.dll");
    let (x64_dst, settings_dir) = {
        let win64 = target_dir.join("win64");
        if win64.exists() {
            (win64.join("steam_api64.dll"), target_dir.join("steam_settings"))
        } else {
            (target_dir.join("steam_api64.dll"), target_dir.join("steam_settings"))
        }
    };

    if x86_dst.exists() && !target_dir.join("steam_api.dll.orig").exists() {
        let _ = std::fs::rename(&x86_dst, target_dir.join("steam_api.dll.orig"));
    }
    if x64_dst.exists() && !x64_dst.with_extension("orig").exists() {
        let _ = std::fs::rename(&x64_dst, x64_dst.with_extension("orig"));
    }

    std::fs::copy(&goldberg_x86, &x86_dst)?;
    std::fs::copy(&goldberg_x64, &x64_dst)?;

    std::fs::create_dir_all(&settings_dir)?;
    std::fs::write(settings_dir.join("force_steam_appid.txt"), appid.to_string())?;

    Ok(())
}

fn find_file_recursive(dir: &PathBuf, name: &str) -> Option<PathBuf> {
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                if let Some(found) = find_file_recursive(&path, name) {
                    return Some(found);
                }
            } else if path.file_name().map(|n| n == name).unwrap_or(false) {
                return Some(path);
            }
        }
    }
    None
}

pub fn deploy_goldberg_for_launch(
    home: &PathBuf,
    game_dir: &PathBuf,
    appid: u32,
) -> Result<(), Box<dyn std::error::Error>> {
    let goldberg_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("goldberg");
    let goldberg_x86 = goldberg_dir.join("x86").join("steam_api.dll");
    let goldberg_x64 = goldberg_dir.join("x64").join("steam_api64.dll");

    if !goldberg_x86.exists() || !goldberg_x64.exists() {
        ensure_goldberg_downloaded(home)?;
    }

    let deploy_dir = match appid {
        620 => {
            let bin = game_dir.join("bin");
            if bin.exists() {
                bin
            } else {
                game_dir.clone()
            }
        },
        265930 => {
            let bin = game_dir.join("Binaries").join("Win32");
            if bin.exists() {
                bin
            } else {
                game_dir.clone()
            }
        },
        _ => game_dir.clone(),
    };

    let settings_dir = deploy_dir.join("steam_settings");
    let appid_ok = settings_dir.join("force_steam_appid.txt").exists();

    let x86_dst = deploy_dir.join("steam_api.dll");
    let x64_dst = if deploy_dir.join("win64").exists() {
        deploy_dir.join("win64").join("steam_api64.dll")
    } else {
        deploy_dir.join("steam_api64.dll")
    };

    let dlls_ok = x86_dst.exists() && x64_dst.exists();

    if !dlls_ok || !appid_ok {
        deploy_goldberg_to_dir(&deploy_dir, &goldberg_dir, appid)?;
    }

    Ok(())
}

fn find_steam_api(home: &PathBuf) -> Option<PathBuf> {
    let ms_home = crate::platform::metalsharp_home_dir_for(home);
    let candidates = vec![
        // Primary: macOS Steam app dylib
        home.join("Library/Application Support/Steam/steamapps/common/Terraria/Terraria.app/Contents/MacOS/osx/libsteam_api.dylib"),
        home.join("Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib"),
        // Fallback: MetalSharp runtime shims (works without macOS Steam installed)
        ms_home.join("runtime").join("steam-bridge").join("libsteam_api.dylib"),
        ms_home.join("runtime").join("shims").join("libsteam_api.dylib"),
        ms_home.join("cache").join("steam").join("libsteam_api.dylib"),
    ];
    let found = candidates.into_iter().find(|p| p.exists());
    if found.is_none() {
        eprintln!("setup: libsteam_api.dylib not found in any candidate path (Celeste Steam API shim will be missing)");
    }
    found
}

pub fn detect_dotnet_game(game_dir: &PathBuf) -> bool {
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            let name_lower = name.to_lowercase();
            if name_lower.ends_with("_data") && entry.path().is_dir() {
                let managed = entry.path().join("Managed");
                if managed.exists() {
                    return !has_native_windows_dlls(game_dir);
                }
            }
        }
    }

    false
}

fn has_native_windows_dlls(game_dir: &PathBuf) -> bool {
    let managed_extensions = ["cs.dll", ".managed.dll", ".net.dll"];
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            if let Some(ext) = path.extension() {
                if ext == "dll" {
                    let name = entry.file_name().to_string_lossy().to_string();
                    let name_lower = name.to_lowercase();
                    let is_managed_wrapper = name_lower.contains("steamworks.net")
                        || name_lower.contains("fmod")
                        || managed_extensions.iter().any(|e| name_lower.ends_with(e));
                    if !is_managed_wrapper {
                        let output = mac_cmd("file").arg(&path).output();
                        if let Ok(o) = output {
                            let desc = String::from_utf8_lossy(&o.stdout);
                            if desc.contains("PE32") && !desc.contains(".Net") && !desc.contains("Mono/.Net") {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    false
}

fn setup_fna_runtime(game_dir: &PathBuf, home: &PathBuf) -> Result<(), Box<dyn std::error::Error>> {
    let fna_src = home.join("metalsharp").join("src").join("fna");

    let fna_build = fna_src.join("FNA").join("bin").join("Release").join("net4.0");
    let fna3d_build = fna_src.join("FNA3D").join("build");
    let shims_src = fna_src.join("shims");

    if fna_build.exists() {
        if let Ok(entries) = std::fs::read_dir(&fna_build) {
            for entry in entries.flatten() {
                let src = entry.path();
                if let Some(ext) = src.extension() {
                    if ext == "dll" {
                        let _ = std::fs::copy(&src, game_dir.join(entry.file_name()));
                    }
                }
            }
        }
    }

    let xna_assemblies = [
        "Microsoft.Xna.Framework.dll",
        "Microsoft.Xna.Framework.Game.dll",
        "Microsoft.Xna.Framework.Graphics.dll",
        "Microsoft.Xna.Framework.Audio.dll",
        "Microsoft.Xna.Framework.Input.dll",
        "Microsoft.Xna.Framework.Media.dll",
        "Microsoft.Xna.Framework.Storage.dll",
    ];

    if fna_build.exists() {
        let fna_dll = fna_build.join("FNA.dll");
        if fna_dll.exists() {
            for name in &xna_assemblies {
                let _ = std::fs::copy(&fna_dll, game_dir.join(name));
            }
        }
    }

    if fna3d_build.exists() {
        let src = fna3d_build.join("libFNA3D.dylib");
        if src.exists() {
            let _ = std::fs::copy(&src, game_dir.join("libFNA3D.dylib"));
        }
    }

    let ms_home = crate::platform::metalsharp_home_dir_for(home);
    let shims_dir = ms_home.join("runtime").join("shims");
    if shims_dir.exists() {
        for shim in &["libCSteamworks.dylib", "libfmod.dylib", "libfmodstudio.dylib"] {
            let cached = shims_dir.join(shim);
            let dst = game_dir.join(shim);
            if !dst.exists() && cached.exists() {
                let _ = std::fs::copy(&cached, &dst);
            }
        }
    }

    if shims_src.exists() {
        for shim in &["csteamworks_shim.c", "fmod_stub.c", "fmodstudio_stub.c"] {
            let src = shims_src.join(shim);
            if src.exists() {
                build_shim(&src, game_dir);
            }
        }
    }

    let fnalibs_dir = ms_home.join("runtime").join("fnalibs");
    let sdl2_src = fnalibs_dir.join("libSDL2-2.0.0.dylib");
    if sdl2_src.exists() {
        let dst = game_dir.join("libSDL2-2.0.0.dylib");
        let _ = std::fs::copy(&sdl2_src, &dst);
        let _ = mac_cmd("install_name_tool").args(["-id", "@loader_path/libSDL2-2.0.0.dylib"]).arg(&dst).output();

        let fna3d = game_dir.join("libFNA3D.0.dylib");
        if fna3d.exists() {
            let _ = mac_cmd("install_name_tool")
                .args(["-change", "@rpath/libSDL2-2.0.0.dylib", "@loader_path/libSDL2-2.0.0.dylib"])
                .arg(&fna3d)
                .output();
        }

        let _ = std::os::unix::fs::symlink("libSDL2-2.0.0.dylib", game_dir.join("libSDL2.dylib"));
    }

    let steam_dylib_candidates = [
        home.join("Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Steam Helper.app/Contents/MacOS/libsteam_api.dylib"),
        home.join("Library/Application Support/Steam/steamapps/common/Nidhogg 2/Nidhogg_2.app/Contents/MacOS/libsteam_api.dylib"),
    ];
    for dylib in &steam_dylib_candidates {
        if dylib.exists() {
            let _ = std::fs::copy(dylib, game_dir.join("libsteam_api.dylib"));
            break;
        }
    }

    let _ = std::fs::write(game_dir.join("steam_appid.txt"), "");

    let _ = mac_cmd("codesign")
        .args(["--force", "-s", "-"])
        .arg(game_dir.join("libCSteamworks.dylib"))
        .arg(game_dir.join("libfmod.dylib"))
        .arg(game_dir.join("libfmodstudio.dylib"))
        .output();

    Ok(())
}

fn build_shim(src: &PathBuf, game_dir: &PathBuf) {
    let name = src.file_stem().unwrap_or_default().to_string_lossy();
    let (output_name, install_name) = if name.contains("csteamworks") {
        ("libCSteamworks.dylib", "@loader_path/libCSteamworks.dylib")
    } else if name.contains("fmodstudio") {
        ("libfmodstudio.dylib", "@loader_path/libfmodstudio.dylib")
    } else if name.contains("fmod") {
        ("libfmod.dylib", "@loader_path/libfmod.dylib")
    } else {
        return;
    };

    let output = game_dir.join(output_name);
    let result = mac_cmd("clang")
        .args(["-shared", "-arch", "arm64", "-arch", "x86_64"])
        .arg("-o")
        .arg(&output)
        .arg(src)
        .arg("-install_name")
        .arg(install_name)
        .output();

    if let Ok(o) = result {
        if o.status.success() {
            let _ = mac_cmd("codesign").args(["--force", "-s", "-"]).arg(&output).output();
        }
    }
}

fn check_command(cmd: &str) -> bool {
    mac_cmd("which").arg(cmd).output().map(|o| o.status.success()).unwrap_or(false)
}

fn check_path(path: &PathBuf) -> bool {
    path.exists()
}

fn check_dylib(home: &PathBuf, name: &str) -> bool {
    let candidates = [
        home.join(format!("lib/{}", name)),
        PathBuf::from(format!("/opt/homebrew/lib/{}", name)),
        PathBuf::from(format!("/usr/local/lib/{}", name)),
    ];
    candidates.iter().any(|p| p.exists())
}

fn check_framework(name: &str) -> bool {
    PathBuf::from(format!("/Library/Frameworks/{}.framework", name)).exists()
}

fn check_brew(formula: &str) -> bool {
    std::process::Command::new("brew")
        .args(["list", formula])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn check_rosetta() -> bool {
    PathBuf::from("/Library/Apple/System/Library/LaunchDaemons/com.apple.oahd.plist").exists()
        || mac_cmd("pgrep").arg("-q").arg("oahd").status().map(|s| s.success()).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        std::env::temp_dir().join(format!("metalsharp-setup-test-{}-{}", name, nanos))
    }

    fn write_cached_agility_payload(home: &Path, package_version: &str) -> PathBuf {
        let bin = crate::platform::metalsharp_home_dir_for(home)
            .join("runtime")
            .join("redist")
            .join("agility")
            .join(package_version)
            .join("build")
            .join("native")
            .join("bin")
            .join("x64");
        std::fs::create_dir_all(&bin).expect("create cached Agility bin");
        for file in ["D3D12Core.dll", "d3d12SDKLayers.dll"] {
            std::fs::write(bin.join(file), file.as_bytes()).expect("write cached Agility payload");
        }
        bin
    }

    fn write_default_cached_agility_payload(home: &Path) -> PathBuf {
        write_cached_agility_payload(home, DEFAULT_AGILITY_PACKAGE_VERSION)
    }

    #[test]
    fn agility_cache_candidates_include_user_runtime_redist() {
        let home = PathBuf::from("/Users/tester");
        let mut candidates = Vec::new();
        push_cached_agility_package_candidates(&mut candidates, &home, DEFAULT_AGILITY_PACKAGE_VERSION);

        assert_eq!(
            candidates.first(),
            Some(
                &home
                    .join(".metalsharp")
                    .join("runtime")
                    .join("redist")
                    .join("agility")
                    .join(DEFAULT_AGILITY_PACKAGE_VERSION)
                    .join("build")
                    .join("native")
                    .join("bin")
                    .join("x64")
            )
        );
    }

    #[test]
    fn agility_default_requirement_uses_fetchable_runtime_package() {
        assert_eq!(agility_package_version_for_requirement(None), "1.619.3");
        assert_eq!(agility_package_version_for_requirement(Some(4)), "1.4.10");
        assert_eq!(agility_package_version_for_requirement(Some(600)), "1.600.10");
        assert_eq!(agility_package_version_for_requirement(Some(602)), "1.602.4");
        assert_eq!(agility_package_version_for_requirement(Some(606)), "1.606.4");
        assert_eq!(agility_package_version_for_requirement(Some(608)), "1.608.3");
        assert_eq!(agility_package_version_for_requirement(Some(610)), "1.610.4");
        assert_eq!(agility_package_version_for_requirement(Some(611)), "1.611.2");
        assert_eq!(agility_package_version_for_requirement(Some(613)), "1.613.3");
        assert_eq!(agility_package_version_for_requirement(Some(614)), "1.614.1");
        assert_eq!(agility_package_version_for_requirement(Some(615)), "1.615.1");
        assert_eq!(agility_package_version_for_requirement(Some(616)), "1.616.1");
        assert_eq!(agility_package_version_for_requirement(Some(618)), "1.618.5");
        assert_eq!(agility_package_version_for_requirement(Some(619)), "1.619.3");
    }

    #[test]
    fn agility_preview_versions_resolve_from_sdk_number() {
        assert_eq!(agility_package_version_for_requirement(Some(700)), "1.700.10-preview");
        assert_eq!(agility_package_version_for_requirement(Some(706)), "1.706.4-preview");
        assert_eq!(agility_package_version_for_requirement(Some(710)), "1.710.0-preview");
        assert_eq!(agility_package_version_for_requirement(Some(711)), "1.711.3-preview");
        assert_eq!(agility_package_version_for_requirement(Some(714)), "1.714.0-preview");
        assert_eq!(agility_package_version_for_requirement(Some(715)), "1.715.0-preview");
        assert_eq!(agility_package_version_for_requirement(Some(716)), "1.716.1-preview");
        assert_eq!(agility_package_version_for_requirement(Some(717)), "1.717.1-preview");
        assert_eq!(agility_package_version_for_requirement(Some(719)), "1.719.1-preview");
        assert_eq!(agility_package_version_for_requirement(Some(720)), "1.720.0-preview");
        assert_eq!(agility_package_version_for_requirement(Some(721)), "1.721.0-preview");
    }

    #[test]
    fn agility_retail_takes_priority_over_preview_for_619() {
        assert_eq!(agility_package_version_for_requirement(Some(619)), "1.619.3");
    }

    #[test]
    fn agility_unknown_sdk_version_falls_back_to_default() {
        assert_eq!(agility_package_version_for_requirement(Some(999)), "1.619.3");
    }

    #[test]
    fn agility_known_versions_listing_is_complete() {
        let listing = agility_known_sdk_versions();
        let retail = listing["retail"].as_array().expect("retail array");
        let preview = listing["preview"].as_array().expect("preview array");
        assert_eq!(retail.len(), 13);
        assert_eq!(preview.len(), 11);
        assert_eq!(listing["default"], "1.619.3");
    }

    #[test]
    fn agility_package_download_uses_nuget_flat_container() {
        assert_eq!(
            agility_package_download_url("1.615.1"),
            "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.615.1/microsoft.direct3d.d3d12.1.615.1.nupkg"
        );
    }

    #[test]
    fn agility_package_extraction_keeps_runtime_bin_payloads_only() {
        assert!(is_agility_runtime_payload(Path::new("build/native/bin/x64/D3D12Core.dll")));
        assert!(!is_agility_runtime_payload(Path::new("build/native/bin/win32/D3D12Core.dll")));
        assert!(!is_agility_runtime_payload(Path::new("build/native/include/d3d12.h")));
        assert!(!is_agility_runtime_payload(Path::new("../build/native/bin/x64/D3D12Core.dll")));
    }

    #[test]
    fn agility_stage_repairs_and_verifies_game_local_targets() {
        let home = test_dir("agility-stage-home");
        let game_dir = test_dir("agility-stage-game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("Game.exe"), b"synthetic pe placeholder").expect("write game exe");
        write_default_cached_agility_payload(&home);

        let report = stage_agility_sdk_for_game_report(123456, &game_dir, &home).expect("stage Agility payload");
        assert_eq!(report.package_version, DEFAULT_AGILITY_PACKAGE_VERSION);
        assert_eq!(report.target_dirs.len(), 2);
        assert!(report.target_dirs.iter().any(|dir| dir == &game_dir.join("D3D12")));
        assert!(report.target_dirs.iter().any(|dir| dir == &game_dir.join("D3D12").join("x64")));

        let inspection =
            inspect_agility_sdk_for_game(123456, &game_dir, &home).expect("inspect staged Agility payload");
        assert!(inspection.installed());
        assert!(inspection.missing_files.is_empty());

        let _ = std::fs::remove_dir_all(home);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn agility_shared_payload_titles_still_verify_cached_runtime() {
        let home = test_dir("agility-shared-home");
        let game_dir = test_dir("agility-shared-game");
        let d3d12_dir = game_dir.join("D3D12");
        std::fs::create_dir_all(&d3d12_dir).expect("create stale sidecar dir");
        std::fs::write(game_dir.join("Game.exe"), b"synthetic pe placeholder").expect("write game exe");
        std::fs::write(d3d12_dir.join("D3D12Core.dll"), b"stale").expect("write stale sidecar");
        write_default_cached_agility_payload(&home);

        let before_repair =
            inspect_agility_sdk_for_game(1962700, &game_dir, &home).expect("inspect unrepaired shared Agility title");
        assert!(!before_repair.installed());
        assert!(before_repair.shared_payload_ready);
        assert!(!before_repair.shared_repair_marker_ready);
        assert!(before_repair.app_local_sidecars_present);

        let report =
            stage_agility_sdk_for_game_report(1962700, &game_dir, &home).expect("stage shared Agility payload");
        assert!(report.app_local_sidecars_skipped);
        assert!(report.target_dirs.is_empty());
        assert!(!d3d12_dir.exists());

        let inspection =
            inspect_agility_sdk_for_game(1962700, &game_dir, &home).expect("inspect shared Agility payload");
        assert!(inspection.installed());
        assert!(inspection.shared_payload_ready);
        assert!(inspection.shared_repair_marker_ready);
        assert!(!inspection.app_local_sidecars_present);

        std::fs::create_dir_all(&d3d12_dir).expect("recreate stale sidecar dir");
        let stale_again =
            inspect_agility_sdk_for_game(1962700, &game_dir, &home).expect("inspect stale shared Agility title");
        assert!(!stale_again.installed());
        assert!(stale_again.shared_repair_marker_ready);
        assert!(stale_again.app_local_sidecars_present);

        let _ = std::fs::remove_dir_all(home);
        let _ = std::fs::remove_dir_all(game_dir);
    }
}
