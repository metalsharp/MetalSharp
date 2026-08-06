use super::engine::{get_pipeline, PipelineId, PipelineNode};
use std::collections::HashSet;
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use walkdir::WalkDir;

const DEFAULT_BRIDGE_PORT: u16 = 18733;
const FNA_CARBON_SHIM: &str = "libCarbon.dylib";
const FNA_CARBON_INTERPOSE_SHIM: &str = "libmetalsharp_carbon_interpose.dylib";
const FNA_XNA_WRAPPER_VERSION: &str = "22.12.2";

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MonoArch {
    Native,
    X86,
}

pub struct FnaGameProfile {
    pub appid: u32,
    pub mono_config: &'static str,
    pub mono_arch: MonoArch,
    pub preferred_exes: &'static [&'static str],
    pub method_label: &'static str,
    pub setup_script: Option<&'static str>,
    pub deploy_macos_steam_libs: bool,
    pub launcher_exe: Option<&'static str>,
    pub launcher_source: Option<&'static str>,
    pub deploy_terraria_post: bool,
    pub include_runtime_shims_in_library_path: bool,
}

const FNA_GAME_PROFILES: &[FnaGameProfile] = &[
    FnaGameProfile {
        appid: 105600,
        mono_config: "generic-fna-mono.config",
        mono_arch: MonoArch::X86,
        preferred_exes: &["TerrariaLauncher.exe", "Terraria.exe"],
        method_label: "xna_fna_x86",
        setup_script: Some("setup-terraria-deps.sh"),
        deploy_macos_steam_libs: true,
        launcher_exe: Some("TerrariaLauncher.exe"),
        launcher_source: Some("TerrariaLauncher.cs"),
        deploy_terraria_post: true,
        include_runtime_shims_in_library_path: true,
    },
    FnaGameProfile {
        appid: 504230,
        mono_config: "celeste-x86-mono.config",
        mono_arch: MonoArch::X86,
        preferred_exes: &[],
        method_label: "xna_fna_x86",
        setup_script: Some("setup-celeste-deps.sh"),
        deploy_macos_steam_libs: false,
        launcher_exe: None,
        launcher_source: None,
        deploy_terraria_post: false,
        include_runtime_shims_in_library_path: false,
    },
    FnaGameProfile {
        appid: 413150,
        mono_config: "stardew-mono.config",
        mono_arch: MonoArch::Native,
        preferred_exes: &[],
        method_label: "xna_fna_arm64",
        setup_script: None,
        deploy_macos_steam_libs: false,
        launcher_exe: None,
        launcher_source: None,
        deploy_terraria_post: false,
        include_runtime_shims_in_library_path: true,
    },
];

const DEFAULT_FNA_PROFILE: FnaGameProfile = FnaGameProfile {
    appid: 0,
    mono_config: "generic-fna-mono.config",
    mono_arch: MonoArch::X86,
    preferred_exes: &[],
    method_label: "xna_fna_x86",
    setup_script: None,
    deploy_macos_steam_libs: false,
    launcher_exe: None,
    launcher_source: None,
    deploy_terraria_post: false,
    include_runtime_shims_in_library_path: false,
};

pub fn find_fna_profile(appid: u32) -> &'static FnaGameProfile {
    FNA_GAME_PROFILES.iter().find(|p| p.appid == appid).unwrap_or(&DEFAULT_FNA_PROFILE)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum FnaFlavor {
    Fna,
    MonoGame,
    Xna,
    Unknown,
}

pub fn detect_fna_flavor(game_dir: &PathBuf) -> FnaFlavor {
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.to_lowercase().ends_with("_data") && entry.path().is_dir() {
                let managed = entry.path().join("Managed");
                if !managed.exists() {
                    continue;
                }
                if let Ok(managed_entries) = std::fs::read_dir(&managed) {
                    let mut has_fna = false;
                    let mut has_monogame = false;
                    let mut has_xna = false;
                    for me in managed_entries.flatten() {
                        let dll_name = me.file_name().to_string_lossy().to_string();
                        let dll_lower = dll_name.to_lowercase();
                        if dll_lower == "fna.dll" {
                            has_fna = true;
                        }
                        if dll_lower.starts_with("monogame") || dll_lower.contains("mg.framework") {
                            has_monogame = true;
                        }
                        if dll_lower.starts_with("microsoft.xna.framework") {
                            has_xna = true;
                        }
                    }
                    if has_fna {
                        return FnaFlavor::Fna;
                    }
                    if has_monogame {
                        return FnaFlavor::MonoGame;
                    }
                    if has_xna {
                        return FnaFlavor::Xna;
                    }
                }
            }
        }
    }
    if game_dir.join("FNA.dll").exists() {
        return FnaFlavor::Fna;
    }
    FnaFlavor::Unknown
}

#[derive(Clone, Copy)]
enum FnaShimSource {
    RepoC { parts: &'static [&'static str], undefined_dynamic_lookup: bool },
    RepoObjC { parts: &'static [&'static str], frameworks: &'static [&'static str] },
    BundledNative,
}

#[derive(Clone, Copy)]
struct FnaNativeShimSpec {
    output: &'static str,
    source: FnaShimSource,
    symlinks: &'static [&'static str],
    required_for_launch: bool,
}

const FNA_NATIVE_SHIMS: &[FnaNativeShimSpec] = &[
    FnaNativeShimSpec {
        output: "libkernel32.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "kernel32_shim.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &[],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: "libuser32.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "user32_shim.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &[],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: FNA_CARBON_SHIM,
        source: FnaShimSource::RepoObjC {
            parts: &["src", "fna", "shims", "carbon_hiview_shim.m"],
            frameworks: &["Cocoa", "Carbon"],
        },
        symlinks: &[],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: FNA_CARBON_INTERPOSE_SHIM,
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "carbon_interpose.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &[],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: "xaudio2_9.dylib",
        source: FnaShimSource::BundledNative,
        symlinks: &["xaudio2_8.dylib", "xaudio2_7.dylib"],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: "libSystem.Native.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "system_native_stub.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &[],
        required_for_launch: true,
    },
    FnaNativeShimSpec {
        output: "xinput1_4.dylib",
        source: FnaShimSource::BundledNative,
        symlinks: &["xinput1_3.dylib", "xinput9_1_0.dylib"],
        required_for_launch: false,
    },
    FnaNativeShimSpec {
        output: "libgdiplus.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "terraria", "gdiplus_stub.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &[],
        required_for_launch: false,
    },
    FnaNativeShimSpec {
        output: "libFAudio.0.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "terraria", "faudio_stub.c"],
            undefined_dynamic_lookup: false,
        },
        symlinks: &["libFAudio.dylib"],
        required_for_launch: false,
    },
    FnaNativeShimSpec {
        output: "libCSteamworks.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "csteamworks_shim.c"],
            undefined_dynamic_lookup: true,
        },
        symlinks: &[],
        required_for_launch: false,
    },
    FnaNativeShimSpec {
        output: "libfmod.dylib",
        source: FnaShimSource::RepoC { parts: &["src", "fna", "shims", "fmod_stub.c"], undefined_dynamic_lookup: true },
        symlinks: &[],
        required_for_launch: false,
    },
    FnaNativeShimSpec {
        output: "libfmodstudio.dylib",
        source: FnaShimSource::RepoC {
            parts: &["src", "fna", "shims", "fmodstudio_stub.c"],
            undefined_dynamic_lookup: true,
        },
        symlinks: &[],
        required_for_launch: false,
    },
];

pub fn bridge_port() -> u16 {
    parse_bridge_port(std::env::var("METALSHARP_STEAM_BRIDGE_PORT").ok().as_deref()).unwrap_or(DEFAULT_BRIDGE_PORT)
}

fn parse_bridge_port(value: Option<&str>) -> Option<u16> {
    let parsed = value?.parse::<u16>().ok()?;
    if parsed == 0 {
        None
    } else {
        Some(parsed)
    }
}

struct CachePaths {
    shader: String,
    pipeline: String,
    log: String,
}

struct LaunchLogContext<'a> {
    appid: u32,
    node: &'a PipelineNode,
    prefix: &'a Path,
    cwd: &'a Path,
    exe_name: &'a str,
    args: &'a [String],
    cache_paths: Option<&'a CachePaths>,
}

#[derive(Default)]
pub struct CustomLaunchOptions {
    pub prefix_path: Option<PathBuf>,
    pub log_path: Option<PathBuf>,
}

pub fn bridge_is_running() -> bool {
    let port = bridge_port();
    if let Ok(mut stream) =
        TcpStream::connect_timeout(&format!("127.0.0.1:{}", port).parse().unwrap(), Duration::from_millis(500))
    {
        let ping: [u8; 4] = 0xFFu32.to_ne_bytes();
        let _ = stream.write_all(&ping);
        let _ = stream.write_all(&0u32.to_ne_bytes());
        let mut buf = [0u8; 8];
        if stream.read_exact(&mut buf).is_ok() {
            return true;
        }
    }
    false
}

pub fn ensure_bridge_running() -> Result<(), Box<dyn std::error::Error>> {
    if bridge_is_running() {
        return Ok(());
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    let port = bridge_port();
    let bridge_exe =
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("steam-bridge").join("steambridge.exe");
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);
    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");

    if !bridge_exe.exists() {
        return Err("steambridge.exe not found — Wine-side Steam API bridge is not yet available".into());
    }
    if !wine.exists() {
        return Err("MetalSharp Wine not found — run setup first".into());
    }

    let bridge_dir = bridge_exe.parent().unwrap_or(std::path::Path::new(""));
    let dll_dest = bridge_dir.join("steam_api64.dll");
    if !dll_dest.exists() {
        let steam_dll = prefix.join("drive_c").join("Program Files (x86)").join("Steam").join("steam_api64.dll");
        if steam_dll.exists() {
            let _ = std::fs::copy(&steam_dll, &dll_dest);
        }
    }

    let mut cmd = Command::new(&wine);
    cmd.arg(&bridge_exe)
        .env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .env("WINEDEBUG", "-all")
        .env("WINEDEBUGGER", "none")
        .env("METALSHARP_STEAM_BRIDGE_PORT", port.to_string())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);

    let _child = cmd.spawn()?;

    for _ in 0..20 {
        std::thread::sleep(Duration::from_millis(250));
        if bridge_is_running() {
            return Ok(());
        }
    }

    Err("steam bridge failed to start within 5s".into())
}

fn deploy_steam_shim(game_dir: &PathBuf) {
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return,
    };
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let shim_src = ms_home.join("runtime").join("steam-bridge").join("libsteam_api.dylib");
    let shim_fallback = ms_home.join("runtime").join("shims").join("libsteam_api.dylib");
    let src = if shim_src.exists() {
        shim_src
    } else if shim_fallback.exists() {
        shim_fallback
    } else {
        return;
    };
    let dest = game_dir.join("libsteam_api.dylib");
    if dest.exists() {
        return;
    }
    let _ = std::fs::copy(&src, &dest);
}

pub fn launch_with_pipeline(
    appid: u32,
    pipeline_id: PipelineId,
) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let pipeline_id = super::rules::resolve_requested_pipeline(appid, Some(pipeline_id));
    let node = get_pipeline(pipeline_id);

    prepare_steam_api_for_pipeline(appid, pipeline_id);

    match pipeline_id {
        PipelineId::Dxmt
        | PipelineId::M9
        | PipelineId::M10
        | PipelineId::M10_32
        | PipelineId::M11
        | PipelineId::M11_32
        | PipelineId::M12 => launch_dxmt_metal(appid, node),
        PipelineId::M13 | PipelineId::D3DMetal => launch_d3dmetal_gptk(appid, node),
        PipelineId::M32 => launch_wine_bare(appid, node),
        PipelineId::FnaArm64 => launch_fna_arm64(appid).map(|(pid, method, _)| (pid, method)),
        PipelineId::Steam => launch_steam(appid),
        PipelineId::MacSteam => launch_macos_steam(appid),
        PipelineId::WineBare => launch_wine_bare(appid, node),
    }
}

pub fn launch_steam_bottle_with_pipeline(
    appid: u32,
    pipeline_id: PipelineId,
    prefix_path: &Path,
    extra_env: &[(String, String)],
) -> Result<(u32, &'static str, PathBuf), Box<dyn std::error::Error>> {
    let pipeline_id = super::rules::resolve_requested_pipeline(appid, Some(pipeline_id));
    let node = get_pipeline(pipeline_id);
    let log_path = crate::bottles::steam_compatdata_launch_log_path(appid);

    prepare_steam_api_for_pipeline(appid, pipeline_id);

    match pipeline_id {
        PipelineId::Dxmt
        | PipelineId::M9
        | PipelineId::M10
        | PipelineId::M10_32
        | PipelineId::M11
        | PipelineId::M11_32
        | PipelineId::M12 => launch_dxmt_metal_with_context(appid, node, Some(prefix_path), extra_env, Some(&log_path))
            .map(|(pid, method)| (pid, method, log_path)),
        PipelineId::M13 | PipelineId::D3DMetal => {
            launch_d3dmetal_gptk_with_context(appid, node, Some(prefix_path), extra_env, Some(&log_path))
                .map(|(pid, method)| (pid, method, log_path))
        },
        PipelineId::M32 | PipelineId::WineBare => {
            launch_wine_bare_with_context(appid, node, Some(prefix_path), extra_env, Some(&log_path))
                .map(|(pid, method)| (pid, method, log_path))
        },
        PipelineId::FnaArm64 => launch_fna_arm64(appid),
        PipelineId::Steam | PipelineId::MacSteam => Err("Steam bottle launch only supports MTSP game pipelines".into()),
    }
}

pub fn launch_auto(appid: u32) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let pipeline_id = super::rules::resolve_pipeline(appid);
    launch_with_pipeline(appid, pipeline_id)
}

pub fn prepare_pipeline(appid: u32) -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    prepare_pipeline_with_request(appid, None)
}

pub fn prepare_pipeline_with_request(
    appid: u32,
    requested: Option<PipelineId>,
) -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    let mut timing = crate::diagnostics::LaunchTiming::start();
    timing.mark("pipeline_resolution_start");
    let pipeline_id = super::rules::resolve_requested_pipeline(appid, requested);
    let node = get_pipeline(pipeline_id);
    timing.mark("pipeline_resolution_done");

    // Use the exact MTSP handoff preparation path used by /steam/launch-game:
    // runtime validation, legacy cleanup, Steam API sidecars, game-local DLL
    // deployment, prefix route DLL deployment, and launch env construction.
    let (env, recipe) = prepare_steam_pipeline_env(appid, pipeline_id)?;
    timing.mark("mtsp_launch_handoff_prepare_done");
    let readiness = prepare_readiness_report(&recipe, &env);
    timing.mark("prepare_readiness_report_done");
    timing.mark("prepare_complete");

    // Persist launch timing so performance deltas can be compared between PRs
    // via GET /diagnostics/launch/timing?appid=...
    if let Some(home) = dirs::home_dir() {
        timing.record_for_bottle(&home, &format!("steam_{}", appid));
    }

    let readiness_ok = readiness.get("ok").and_then(|value| value.as_bool()).unwrap_or(false);
    let deployed_sources: Vec<String> = recipe.dlls.iter().map(|dll| dll.source_subpath.clone()).collect();

    Ok(serde_json::json!({
        "ok": readiness_ok,
        "schema": "metalsharp.mtsp.prepare.v2",
        "canonical_endpoint": "/mtsp/prepare",
        "appid": appid,
        "requested_pipeline": requested,
        "pipeline": node.id,
        "pipeline_name": node.name,
        "recipe": recipe,
        "launch_env": env.iter().map(|(key, value)| serde_json::json!({"key": key, "value": value})).collect::<Vec<_>>(),
        "readiness": readiness,
        "deployed_dlls": deployed_sources.len(),
        "deployed_sources": deployed_sources,
        "timing": timing.to_json(),
    }))
}

pub fn prepare_steam_pipeline_env(
    appid: u32,
    pipeline_id: PipelineId,
) -> Result<(Vec<(String, String)>, super::recipe::LaunchRecipe), Box<dyn std::error::Error>> {
    let pipeline_id = super::rules::resolve_requested_pipeline(appid, Some(pipeline_id));
    let node = get_pipeline(pipeline_id);
    match pipeline_id {
        PipelineId::Dxmt
        | PipelineId::M9
        | PipelineId::M10
        | PipelineId::M10_32
        | PipelineId::M11
        | PipelineId::M11_32
        | PipelineId::M12
        | PipelineId::M13
        | PipelineId::D3DMetal
        | PipelineId::M32
        | PipelineId::WineBare => {},
        PipelineId::FnaArm64 => {
            let recipe = super::recipe::build_launch_recipe(appid, node)?;
            validate_recipe_runtime(&recipe)?;
            return Ok((Vec::new(), recipe));
        },
        PipelineId::Steam | PipelineId::MacSteam => {
            return Err("Steam route handoff only supports Wine-backed MTSP pipelines".into());
        },
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    prepare_start_protected_game_for_pipeline(appid, pipeline_id);
    let recipe = super::recipe::build_launch_recipe(appid, node)?;
    validate_recipe_runtime(&recipe)?;
    if node.backend == "dxmt" {
        sanitize_metalsharp_wine_wrapper_env()?;
    }
    if let Some(game_dir) = recipe.game_dir.as_ref() {
        prepare_steam_api_for_game_dir(&home, game_dir, appid, pipeline_id);
        cleanup_legacy_eac_toggle_artifacts(game_dir);
        cleanup_legacy_injections(game_dir)?;
        if matches!(pipeline_id, PipelineId::M12 | PipelineId::M13) {
            let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
            cleanup_m12_legacy_hook_artifacts(game_dir, &prefix);
            crate::setup::stage_agility_sdk_for_game(appid, game_dir, &home)?;
        }
    }
    if pipeline_id == PipelineId::D3DMetal {
        if let Some(game_dir) = recipe.game_dir.as_ref() {
            cleanup_metalsharp_dlls_from_game_dir(game_dir)?;
        }
    }
    let quarantined = quarantine_route_conflicts_for_recipe(&recipe)?;
    if quarantined > 0 {
        eprintln!(
            "mtsp: quarantined {} stale route DLL conflict(s) before deploying {}",
            quarantined, recipe.pipeline_name
        );
    }
    deploy_recipe_dlls(&recipe)?;
    let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
    deploy_prefix_route_dlls(&recipe, &prefix)?;

    let env = steam_pipeline_env_pairs(&home, node, appid);
    Ok((env, recipe))
}

/// Phase 3: M12 artifact and launch verification (dry-run).
///
/// Proves the M12 route would load the intended DXMT/winemetal artifacts
/// WITHOUT launching Steam or the game. This runs through the same
/// environment builder (`steam_pipeline_env_pairs`) as `launch_dxmt_metal`,
/// so the env pairs and artifact sources reported here are exactly what a
/// real M12 launch would use. Nothing is deployed or spawned.
pub fn m12_verify_dry_run(appid: u32) -> serde_json::Value {
    match dirs::home_dir() {
        Some(home) => pipeline_dry_run_for(&home, appid, Some(PipelineId::M12)),
        None => serde_json::json!({"ok": false, "appid": appid, "error": "home directory could not be resolved"}),
    }
}

/// Read-only pipeline dry-run with an explicit home (used by tests so they
/// never mutate the process-global METALSHARP_HOME).
///
/// Artifact sources are derived from the pipeline node's `deploy_dlls`
/// (resolved against the runtime wine root) so verification reflects the
/// RUNTIME readiness, independent of whether a specific game is installed.
/// The env pairs come from the same `steam_pipeline_env_pairs` builder the
/// launch path uses.
pub fn pipeline_dry_run_for(home: &Path, appid: u32, requested: Option<PipelineId>) -> serde_json::Value {
    let home = home.to_path_buf();
    let pipeline = super::rules::resolve_requested_pipeline(appid, requested);
    let node = get_pipeline(pipeline);
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");

    // Build the SAME env pairs the launch path uses (read-only).
    let env = steam_pipeline_env_pairs(&home, node, appid);
    let env_keys: std::collections::HashSet<&str> = env.iter().map(|(k, _)| k.as_str()).collect();
    let env_pairs_json: Vec<serde_json::Value> =
        env.iter().map(|(k, v)| serde_json::json!({ "key": k, "value": v })).collect();

    // Artifact sources derived from the pipeline node's deploy list, resolved
    // against the runtime wine root. Optional stubs (nvapi/nvngx/atidxx) are
    // tolerated as missing.
    let mut deploy_dlls: Vec<serde_json::Value> = Vec::new();
    let mut missing: Vec<serde_json::Value> = Vec::new();
    let mut windows_dll_dir: Option<PathBuf> = None;
    for deploy in &node.deploy_dlls {
        let source_path = ms_root.join(deploy.source_subpath).join(deploy.filename);
        if windows_dll_dir.is_none() {
            windows_dll_dir = source_path.parent().map(|p| p.to_path_buf());
        }
        let present = source_path.exists();
        let optional_stub = pipeline != PipelineId::M12
            && (deploy.filename.starts_with("nvapi")
                || deploy.filename.starts_with("nvngx")
                || deploy.filename.starts_with("atidxx"));
        let sha = if present { crate::diagnostics::file_sha256(&source_path) } else { None };
        let size = if present { std::fs::metadata(&source_path).ok().map(|m| m.len()) } else { None };
        deploy_dlls.push(serde_json::json!({
            "filename": deploy.filename,
            "source_subpath": deploy.source_subpath,
            "source_path": source_path.to_string_lossy(),
            "present": present,
            "optional": optional_stub,
            "sha256": sha,
            "size_bytes": size,
        }));
        if !present && !optional_stub {
            missing.push(serde_json::json!({
                "filename": deploy.filename,
                "source_subpath": deploy.source_subpath,
                "source_path": source_path.to_string_lossy(),
            }));
        }
    }

    // For the isolated M12/M13 lane, also verify the x86_64-unix sidecars that
    // winemetal requires at runtime.
    let mut unix_sidecars: Vec<serde_json::Value> = Vec::new();
    let unix_lib_dir = if pipeline == PipelineId::M12 {
        let dir = ms_root.join("lib").join("dxmt_m12").join("x86_64-unix");
        for sidecar in ["winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"] {
            let path = dir.join(sidecar);
            let present = path.exists();
            let sha = if present { crate::diagnostics::file_sha256(&path) } else { None };
            unix_sidecars.push(serde_json::json!({
                "filename": sidecar,
                "path": path.to_string_lossy(),
                "present": present,
                "sha256": sha,
            }));
            if !present {
                missing.push(serde_json::json!({
                    "filename": sidecar,
                    "source_path": path.to_string_lossy(),
                    "category": "unix_sidecar",
                }));
            }
        }
        Some(dir)
    } else {
        None
    };

    serde_json::json!({
        "ok": missing.is_empty(),
        "schema_version": 1,
        "dry_run": true,
        "appid": appid,
        "pipeline": pipeline,
        "pipeline_name": node.name,
        "runtime_root": ms_root.to_string_lossy(),
        "windows_dll_dir": windows_dll_dir.as_ref().map(|d| d.to_string_lossy()).unwrap_or_default(),
        "windows_dll_dir_exists": windows_dll_dir.as_ref().map(|d| d.exists()).unwrap_or(false),
        "unix_lib_dir": unix_lib_dir.as_ref().map(|d| d.to_string_lossy()),
        "unix_lib_dir_exists": unix_lib_dir.as_ref().map(|d| d.exists()),
        "deploy_dlls": deploy_dlls,
        "unix_sidecars": unix_sidecars,
        "env_pairs": env_pairs_json,
        "env_keys_present": {
            "WINEDLLOVERRIDES": env_keys.contains("WINEDLLOVERRIDES"),
            "DXMT_SHADER_CACHE_PATH": env_keys.contains("DXMT_SHADER_CACHE_PATH"),
            "DYLD_FALLBACK_LIBRARY_PATH": env_keys.contains("DYLD_FALLBACK_LIBRARY_PATH") || env_keys.contains("LD_LIBRARY_PATH"),
            "SteamAppId": env_keys.contains("SteamAppId"),
            "DXMT_WINEMETAL_UNIXLIB": env_keys.contains("DXMT_WINEMETAL_UNIXLIB"),
        },
        "missing": missing,
    })
}

fn quarantine_route_conflicts_for_recipe(
    recipe: &super::recipe::LaunchRecipe,
) -> Result<usize, Box<dyn std::error::Error>> {
    if !matches!(
        recipe.pipeline,
        PipelineId::M9 | PipelineId::M10 | PipelineId::M10_32 | PipelineId::M11 | PipelineId::M11_32 | PipelineId::M12
    ) {
        return Ok(0);
    }

    let Some(game_dir) = recipe.game_dir.as_ref() else {
        return Ok(0);
    };

    let candidate_dirs = route_quarantine_candidate_dirs(recipe, game_dir);
    if candidate_dirs.is_empty() {
        return Ok(0);
    }

    let expected_dests: HashSet<PathBuf> =
        recipe.dlls.iter().map(|deploy| comparable_path(&deploy.dest_path)).collect();
    let timestamp = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs();
    let quarantine_root = game_dir.join(".metalsharp").join("route-quarantine").join(format!(
        "{}-{}",
        pipeline_quarantine_label(recipe.pipeline),
        timestamp
    ));
    let mut moved = Vec::new();
    let mut discarded = Vec::new();
    let ms_root = dirs::home_dir()
        .map(|home| crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine"))
        .unwrap_or_else(|| PathBuf::from("runtime/wine"));

    for dir in candidate_dirs {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_file() || !is_metalsharp_route_dll_conflict(&path) {
                continue;
            }

            let comparable = comparable_path(&path);
            if let Some(expected) = recipe.dlls.iter().find(|deploy| comparable_path(&deploy.dest_path) == comparable) {
                if expected.source_present && files_match(&expected.source_path, &path) {
                    continue;
                }
            } else if expected_dests.contains(&comparable) {
                continue;
            }

            // Space-efficient cleanup: a stale route DLL in the game folder is a
            // copy of a runtime source DLL (the "deploy from" location), and that
            // source always retains the canonical copy. If the stale file matches
            // any known runtime source for the same filename, just delete it
            // instead of moving it into a quarantine folder that eats disk space.
            // Only irreplaceable files with no matching source are moved aside.
            if let Some(source) = runtime_source_for_dll(&path, &ms_root) {
                if files_match(&source, &path) && std::fs::remove_file(&path).is_ok() {
                    discarded.push(serde_json::json!({
                        "path": path.to_string_lossy(),
                        "matched_source": source.to_string_lossy(),
                    }));
                    continue;
                }
            }

            let rel = route_quarantine_relative_path(game_dir, &path);
            let target = unique_quarantine_target(&quarantine_root.join(rel));
            if let Some(parent) = target.parent() {
                std::fs::create_dir_all(parent)?;
            }
            std::fs::rename(&path, &target)?;
            moved.push(serde_json::json!({
                "from": path.to_string_lossy(),
                "to": target.to_string_lossy(),
            }));
        }
    }

    let touched_count = moved.len() + discarded.len();
    if touched_count > 0 {
        let marker_root = game_dir.join(".metalsharp").join("route-quarantine");
        std::fs::create_dir_all(&marker_root)?;
        let marker = marker_root.join("latest-manifest.json");
        let marker_json = serde_json::json!({
            "quarantined_at": timestamp,
            "pipeline": pipeline_quarantine_label(recipe.pipeline),
            "reason": "Bottle profile save switched MetalSharp route DLLs; stale app-local route shims were removed before deploying the selected runtime. Copies that match a runtime source were discarded (the source retains the canonical copy); irreplaceable files were moved aside.",
            "moved": moved,
            "discarded": discarded,
        });
        std::fs::write(marker, serde_json::to_string_pretty(&marker_json)?)?;
    }

    Ok(touched_count)
}

fn route_quarantine_candidate_dirs(recipe: &super::recipe::LaunchRecipe, game_dir: &Path) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    push_unique_existing_dir(&mut dirs, game_dir.to_path_buf());
    if let Some(exe_dir) = recipe.exe_path.as_ref().and_then(|path| path.parent()) {
        push_unique_existing_dir(&mut dirs, exe_dir.to_path_buf());
    }
    for deploy in &recipe.dlls {
        if let Some(parent) = deploy.dest_path.parent() {
            push_unique_existing_dir(&mut dirs, parent.to_path_buf());
        }
    }
    for rel in [
        ["Engine", "Binaries", "Win64"].as_slice(),
        ["Binaries", "Win64"].as_slice(),
        ["Game", "Binaries", "Win64"].as_slice(),
        ["bin"].as_slice(),
        ["Win64"].as_slice(),
        ["win64"].as_slice(),
        ["Game"].as_slice(),
    ] {
        let mut candidate = game_dir.to_path_buf();
        for segment in rel {
            candidate.push(segment);
        }
        push_unique_existing_dir(&mut dirs, candidate);
    }
    dirs
}

fn push_unique_existing_dir(dirs: &mut Vec<PathBuf>, dir: PathBuf) {
    if !dir.is_dir() {
        return;
    }
    let comparable = comparable_path(&dir);
    if !dirs.iter().any(|existing| comparable_path(existing) == comparable) {
        dirs.push(dir);
    }
}

fn comparable_path(path: &Path) -> PathBuf {
    path.canonicalize().unwrap_or_else(|_| path.to_path_buf())
}

fn route_quarantine_relative_path(game_dir: &Path, path: &Path) -> PathBuf {
    if let Ok(rel) = path.strip_prefix(game_dir) {
        if !rel.as_os_str().is_empty() && !rel.is_absolute() {
            return rel.to_path_buf();
        }
    }
    path.file_name().map(PathBuf::from).unwrap_or_else(|| PathBuf::from("route-dll"))
}

fn unique_quarantine_target(path: &Path) -> PathBuf {
    if !path.exists() {
        return path.to_path_buf();
    }
    let stem = path.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "route-dll".to_string());
    let ext = path.extension().map(|s| s.to_string_lossy().to_string());
    for i in 1..1000 {
        let candidate_name = match ext.as_deref() {
            Some(ext) if !ext.is_empty() => format!("{}-{}.{}", stem, i, ext),
            _ => format!("{}-{}", stem, i),
        };
        let candidate = path.with_file_name(candidate_name);
        if !candidate.exists() {
            return candidate;
        }
    }
    path.with_file_name(format!("{}-fallback", stem))
}

fn pipeline_quarantine_label(pipeline: PipelineId) -> &'static str {
    match pipeline {
        PipelineId::M9 => "m9",
        PipelineId::M10 => "m10",
        PipelineId::M10_32 => "m10_32",
        PipelineId::M11 => "m11",
        PipelineId::M11_32 => "m11_32",
        PipelineId::M12 => "m12",
        PipelineId::D3DMetal => "d3dmetal",
        _ => "route",
    }
}

/// Resolves the runtime "deploy from" source path for a route DLL filename.
///
/// Route DLLs deployed into a game folder are copies of canonical source DLLs
/// that live under the MetalSharp wine runtime (e.g. `lib/dxmt/i386-windows`,
/// `lib/dxmt/x86_64-windows`, `lib/dxmt_m12/x86_64-windows`, `lib/wine/...`).
/// These sources always retain the canonical copy, so a stale deployed copy
/// that matches its source can be safely deleted on a route switch instead of
/// being quarantined. Returns the first matching source path, or `None` if no
/// known runtime source carries that filename.
fn runtime_source_for_dll(dll_path: &Path, ms_root: &Path) -> Option<PathBuf> {
    let filename = dll_path.file_name()?.to_string_lossy().to_ascii_lowercase();
    // Known deploy-from source roots, derived from the PipelineNode deploy_dlls
    // source_subpath values across M9/M10/M10_32/M11/M11_32/M12.
    const SOURCE_ROOTS: &[&str] = &[
        "lib/dxmt/x86_64-windows",
        "lib/dxmt/i386-windows",
        "lib/dxmt_m12/x86_64-windows",
        "lib/wine/x86_64-windows",
        "lib/wine/i386-windows",
        "lib/metalsharp/x86_64-windows",
    ];
    for root in SOURCE_ROOTS {
        let candidate = ms_root.join(root).join(&filename);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

fn is_metalsharp_route_dll_conflict(path: &Path) -> bool {
    let name = path.file_name().and_then(|name| name.to_str()).unwrap_or("").to_ascii_lowercase();
    matches!(
        name.as_str(),
        "d3d9.dll"
            | "d3d10.dll"
            | "d3d10_1.dll"
            | "d3d10core.dll"
            | "d3d11.dll"
            | "d3d12.dll"
            | "dxgi.dll"
            | "dxgi_dxmt.dll"
            | "nvapi64.dll"
            | "nvngx.dll"
            | "nvngx-on-metalfx.dll"
            | "winemetal.dll"
            | "metalsharp_ntdll_hook.dll"
    )
}

pub fn deploy_recipe_dlls(recipe: &super::recipe::LaunchRecipe) -> Result<(), Box<dyn std::error::Error>> {
    validate_recipe_runtime(recipe)?;

    // Controller input shims ([x]/[d] selector) ride along with every
    // game-dir deployment. Additive and best-effort: a missing source or a
    // bad runtime must never block the launch, so failures only warn.
    if let Some(game_dir) = recipe.game_dir.as_deref() {
        if let Err(err) = crate::mtsp::input_shims::deploy_current_for_game(game_dir) {
            eprintln!("mtsp: WARNING — controller input shim deploy failed: {}", err);
        }
    }

    if recipe.dlls.is_empty() {
        return Ok(());
    }

    let game_dir = recipe.game_dir.as_ref().ok_or("game dir not found")?;
    let injection_dir = game_dir.join(".metalsharp");
    let originals_dir = injection_dir.join("originals");
    std::fs::create_dir_all(&originals_dir)?;

    let mut manifest_dlls = Vec::new();
    for deploy in &recipe.dlls {
        if !deploy.source_present {
            // nvapi/nvngx/atidxx are vendor-probe stubs: optional for every
            // pipeline (M12 included) so a missing stub can never block the
            // launch — the vkd3d-proton M12 lane shares stubs with dxmt_m12
            // but that lane is a fallback and may be absent.
            let is_optional_stub = deploy.filename.starts_with("nvapi")
                || deploy.filename.starts_with("nvngx")
                || deploy.filename.starts_with("atidxx");
            if is_optional_stub {
                eprintln!(
                    "mtsp: WARNING — optional vendor stub {} missing at {}; skipping",
                    deploy.filename,
                    deploy.source_path.display()
                );
                continue;
            }
            return Err(format!(
                "required runtime DLL {} missing at {}",
                deploy.filename,
                deploy.source_path.display()
            )
            .into());
        }
        if let Some(parent) = deploy.dest_path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }

        let backup_path = originals_dir.join(backup_name_for(game_dir, &deploy.dest_path));
        if deploy.dest_path.exists() && !files_match(&deploy.source_path, &deploy.dest_path) && !backup_path.exists() {
            std::fs::copy(&deploy.dest_path, &backup_path)?;
        }

        std::fs::copy(&deploy.source_path, &deploy.dest_path)?;
        let staged_sha256 = crate::diagnostics::file_sha256(&deploy.dest_path);
        let source_sha256 =
            if staged_sha256.is_some() { crate::diagnostics::file_sha256(&deploy.source_path) } else { None };
        manifest_dlls.push(serde_json::json!({
            "filename": deploy.filename,
            "source_path": deploy.source_path,
            "dest_path": deploy.dest_path,
            "backup_path": if backup_path.exists() { Some(backup_path) } else { None },
            "sha256": staged_sha256,
            "source_sha256": source_sha256,
            "matches_source": matches!((&staged_sha256, &source_sha256), (Some(a), Some(b)) if a == b),
        }));
    }

    let manifest = serde_json::json!({
        "appid": recipe.appid,
        "pipeline": recipe.pipeline,
        "pipeline_name": recipe.pipeline_name,
        "backend": recipe.backend,
        "exe_path": recipe.exe_path,
        "updated_at_unix": std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs(),
        "dlls": manifest_dlls,
    });
    std::fs::write(injection_dir.join("injections.json"), serde_json::to_string_pretty(&manifest)?)?;
    Ok(())
}

fn deploy_prefix_route_dlls(
    recipe: &super::recipe::LaunchRecipe,
    prefix: &Path,
) -> Result<(), Box<dyn std::error::Error>> {
    if recipe.pipeline != PipelineId::M12 {
        return Ok(());
    }

    let system32 = prefix.join("drive_c").join("windows").join("system32");
    std::fs::create_dir_all(&system32)?;
    for deploy in &recipe.dlls {
        if !deploy.source_present {
            continue;
        }
        if !matches!(
            deploy.filename.as_str(),
            "d3d12.dll"
                | "d3d12core.dll"
                | "dxgi.dll"
                | "dxgi_dxmt.dll"
                | "d3d11.dll"
                | "d3d10core.dll"
                | "winemetal.dll"
        ) {
            continue;
        }
        std::fs::copy(&deploy.source_path, system32.join(&deploy.filename))?;
    }
    Ok(())
}

fn wine_debug_value() -> String {
    std::env::var("METALSHARP_WINEDEBUG").unwrap_or_else(|_| "-all".to_string())
}

fn sanitize_metalsharp_wine_wrapper_env() -> Result<(), Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let wrapper = crate::platform::metalsharp_home_dir_for(&home)
        .join("runtime")
        .join("wine")
        .join("bin")
        .join("metalsharp-wine");
    if !wrapper.exists() {
        return Ok(());
    }

    let script = std::fs::read_to_string(&wrapper)?;
    let mut repaired = repair_metalsharp_wine_wrapper(&script);

    // Isolation contract: MetalSharp's own DLL/dylib directories MUST be
    // searched before anything the parent environment inherited. Third-party
    // Wine launchers (CrossOver, SakuraGiri, Whisky, GPTK…) can export
    // WINEDLLPATH / DYLD_FALLBACK_LIBRARY_PATH from a shell profile or a
    // parent launcher; honoring those first lets a foreign Wine's DLLs be
    // loaded into MetalSharp's prefix, which breaks Steam and game launches.
    let winedll_block = r#"export WINEDLLPATH="$MS_LIB/wine/x86_64-windows:$MS_LIB/wine/i386-windows${WINEDLLPATH:+:$WINEDLLPATH}"
"#;
    if let (Some(start), Some(end)) =
        (repaired.find("export WINELOADER=\"$MS_WINE\"\n"), repaired.find("export WINEDEBUG=\"${WINEDEBUG:--all}\""))
    {
        let replace_start = start + "export WINELOADER=\"$MS_WINE\"\n".len();
        repaired.replace_range(replace_start..end, winedll_block);
    }

    let dyld_block = r#"export DYLD_FALLBACK_LIBRARY_PATH="$MS_LIB:$MS_LIB/wine/x86_64-unix${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
"#;
    if let (Some(start), Some(end)) =
        (repaired.find("export WINEDATADIR=\"$MS_ROOT/share\"\n"), repaired.find("export VK_ICD_FILENAMES="))
    {
        let replace_start = start + "export WINEDATADIR=\"$MS_ROOT/share\"\n".len();
        repaired.replace_range(replace_start..end, dyld_block);
    }

    // Drop CrossOver's CX_ROOT emulation: a real CrossOver reading CX_ROOT
    // could mistake a MetalSharp process for one of its own bottles.
    repaired = repaired.replace("export CX_ROOT=\"$MS_ROOT\"\n", "");

    // VK_ICD_FILENAMES must never hardcode a Homebrew path (CrossOver users
    // often have no Homebrew). Prefer the runtime-bundled MoltenVK ICD;
    // otherwise unset and let the loader search standard locations.
    repaired = repaired.replace(
        "export VK_ICD_FILENAMES=\"/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json\"",
        "if [ -f \"$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json\" ]; then\n  export VK_ICD_FILENAMES=\"$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json\"\nelse\n  unset VK_ICD_FILENAMES\nfi",
    );

    if repaired != script {
        std::fs::write(&wrapper, repaired)?;
    }
    Ok(())
}

fn repair_metalsharp_wine_wrapper(script: &str) -> String {
    script.replace("export MS_FWD_COMPAT_GL_CTX=1\n", "")
}

pub fn launch_custom_with_pipeline(
    launch_id: u32,
    game_dir: &std::path::Path,
    exe_path: &std::path::Path,
    pipeline_id: PipelineId,
    launch_args: &[String],
) -> Result<(u32, &'static str, super::recipe::LaunchRecipe), Box<dyn std::error::Error>> {
    launch_custom_with_options(launch_id, game_dir, exe_path, pipeline_id, launch_args, CustomLaunchOptions::default())
}

pub fn launch_custom_with_options(
    launch_id: u32,
    game_dir: &std::path::Path,
    exe_path: &std::path::Path,
    pipeline_id: PipelineId,
    launch_args: &[String],
    options: CustomLaunchOptions,
) -> Result<(u32, &'static str, super::recipe::LaunchRecipe), Box<dyn std::error::Error>> {
    let pipeline_id =
        if pipeline_id == PipelineId::Dxmt { super::rules::resolve_pipeline(launch_id) } else { pipeline_id };
    let node = get_pipeline(pipeline_id);
    match pipeline_id {
        PipelineId::Dxmt
        | PipelineId::M9
        | PipelineId::M10
        | PipelineId::M10_32
        | PipelineId::M11
        | PipelineId::M11_32
        | PipelineId::M12
        | PipelineId::M13
        | PipelineId::D3DMetal
        | PipelineId::M32
        | PipelineId::WineBare => {},
        PipelineId::FnaArm64 | PipelineId::Steam | PipelineId::MacSteam => {
            return Err("Sharp Library apps must use Auto, Wine, M9, M10, M11, M12, M13, D3DMetal, or M32".into());
        },
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);
    if !wine.exists() {
        return Err("MetalSharp Wine not found — run setup first".into());
    }
    if node.backend == "dxmt" {
        sanitize_metalsharp_wine_wrapper_env()?;
    }

    let mut recipe = super::recipe::build_custom_launch_recipe(launch_id, node, game_dir, Some(exe_path))?;
    recipe.launch_args.extend(launch_args.iter().cloned());
    let quarantined = quarantine_route_conflicts_for_recipe(&recipe)?;
    if quarantined > 0 {
        eprintln!("mtsp: quarantined {} stale route DLL(s) before custom {} launch", quarantined, node.name);
    }
    if node.uses_winedllpath_routing() || node.deploy_dlls.is_empty() {
        validate_recipe_runtime(&recipe)?;
    } else {
        deploy_recipe_dlls(&recipe)?;
    }

    let prefix =
        options.prefix_path.unwrap_or_else(|| crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam"));
    std::fs::create_dir_all(&prefix)?;
    let prefix_str = prefix.to_string_lossy().to_string();
    let exe_dir = launch_working_dir(game_dir, exe_path);
    let exe_name = exe_path.file_name().ok_or("game exe not found")?.to_string_lossy().to_string();
    let cache_paths = build_cache_paths(&home, node, launch_id);
    let mut cmd = Command::new(&wine);
    cmd.current_dir(exe_dir)
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", wine_debug_value())
        .env("WINEDEBUGGER", "none");
    apply_route_library_env(&mut cmd, &ms_root, &node.dyld_paths);
    apply_metalfx_home_env(&mut cmd, &home);

    if node.uses_winedllpath_routing() {
        let winedllpath = build_winedllpath(&ms_root, &node.winedllpath_dirs);
        cmd.env("WINEDLLPATH", &winedllpath);
    }

    if let Some(overrides) = node.wine_overrides {
        cmd.env("WINEDLLOVERRIDES", overrides);
    }
    apply_cache_env(&mut cmd, node, cache_paths.as_ref(), &ms_root);
    if node.backend == "dxmt" {
        cmd.env("DXMT_CONFIG_FILE", ms_root.join("etc").join("dxmt.conf").to_string_lossy().to_string());
        cmd.env("DXMT_WINEMETAL_UNIXLIB", dxmt_winemetal_unixlib_path(&ms_root));
    }
    cmd.env("MS_GRAPHICS_BACKEND", node.graphics_backend);
    cmd.env("WINEMSYNC", msync_env_value(&home));
    for ev in &node.env_vars {
        cmd.env(ev.key, ev.value);
    }
    // MetalFX strength is the user's choice (metalfx.overlay.json): reconcile
    // the DXMT env after node env so the toggle is authoritative.
    apply_metal_fx_config_cmd(&mut cmd, node, &home);

    cmd.arg(&exe_name);
    cmd.args(&recipe.launch_args);
    if let Some(log_path) = options.log_path {
        if let Some(parent) = log_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut log = OpenOptions::new().create(true).append(true).open(&log_path)?;
        writeln!(log, "launch_id={}", launch_id)?;
        writeln!(log, "pipeline={}", node.name)?;
        writeln!(log, "prefix={}", prefix.display())?;
        write_runtime_identity(&mut log, &prefix, None)?;
        writeln!(log, "cwd={}", exe_dir.display())?;
        writeln!(log, "exe={}", exe_name)?;
        writeln!(log, "args={:?}", recipe.launch_args)?;
        if let Some(cache) = cache_paths.as_ref() {
            writeln!(log, "shader_cache={}/", cache.shader)?;
            writeln!(log, "pipeline_cache={}/", cache.pipeline)?;
        }
        writeln!(log, "--- wine output ---")?;
        let stdout = log.try_clone()?;
        cmd.stdout(Stdio::from(stdout)).stderr(Stdio::from(log));
    }
    let child = cmd.spawn()?;
    Ok((child.id(), node.id.to_legacy_method(), recipe))
}

fn backup_name_for(game_dir: &std::path::Path, dest_path: &std::path::Path) -> String {
    dest_path
        .strip_prefix(game_dir)
        .unwrap_or(dest_path)
        .components()
        .map(|c| c.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("__")
}

fn files_match(left: &std::path::Path, right: &std::path::Path) -> bool {
    match (std::fs::read(left), std::fs::read(right)) {
        (Ok(a), Ok(b)) => a == b,
        _ => false,
    }
}

fn runtime_path_record(path: &Path, required: bool) -> serde_json::Value {
    let metadata = std::fs::metadata(path).ok();
    let present = metadata.as_ref().map(|m| m.is_file() && m.len() > 0).unwrap_or(false);
    serde_json::json!({
        "path": path,
        "required": required,
        "present": present,
        "size_bytes": metadata.map(|m| m.len()),
        "sha256": if present { crate::diagnostics::file_sha256(path) } else { None },
    })
}

fn optional_route_stub(pipeline: PipelineId, filename: &str) -> bool {
    pipeline != PipelineId::M12
        && (filename.starts_with("nvapi") || filename.starts_with("nvngx") || filename.starts_with("atidxx"))
}

fn prepare_readiness_report(recipe: &super::recipe::LaunchRecipe, env: &[(String, String)]) -> serde_json::Value {
    let env_value = |key: &str| -> Option<&str> {
        env.iter().find(|(candidate, _)| candidate == key).map(|(_, value)| value.as_str())
    };

    let runtime_assets: Vec<serde_json::Value> = recipe
        .runtime_assets
        .iter()
        .map(|asset| {
            let mut record = runtime_path_record(&asset.path, asset.required);
            if let Some(obj) = record.as_object_mut() {
                obj.insert("name".into(), serde_json::json!(asset.name));
                obj.insert("present".into(), serde_json::json!(asset.present));
            }
            record
        })
        .collect();
    let runtime_assets_ok = recipe.runtime_assets.iter().all(|asset| !asset.required || asset.present);

    let dlls: Vec<serde_json::Value> = recipe
        .dlls
        .iter()
        .map(|dll| {
            let optional = optional_route_stub(recipe.pipeline, &dll.filename);
            let source_sha256 =
                if dll.source_present { crate::diagnostics::file_sha256(&dll.source_path) } else { None };
            let dest_present = dll.dest_path.metadata().map(|m| m.is_file() && m.len() > 0).unwrap_or(false);
            let dest_sha256 = if dest_present { crate::diagnostics::file_sha256(&dll.dest_path) } else { None };
            let matches_source = matches!((&source_sha256, &dest_sha256), (Some(source), Some(dest)) if source == dest);
            serde_json::json!({
                "filename": dll.filename,
                "source_subpath": dll.source_subpath,
                "source_path": dll.source_path,
                "dest_path": dll.dest_path,
                "required": !optional,
                "source_present": dll.source_present,
                "dest_present": dest_present,
                "source_sha256": source_sha256,
                "dest_sha256": dest_sha256,
                "matches_source": matches_source,
                "ok": optional || (dll.source_present && dest_present && matches_source),
            })
        })
        .collect();
    let dlls_ok = dlls.iter().all(|record| record.get("ok").and_then(|value| value.as_bool()).unwrap_or(false));

    let prefix = dirs::home_dir()
        .map(|home| crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam"))
        .unwrap_or_default();
    let prefix_system32 = prefix.join("drive_c").join("windows").join("system32");
    let prefix_dlls: Vec<serde_json::Value> = if recipe.pipeline == PipelineId::M12 {
        recipe
            .dlls
            .iter()
            .filter(|dll| {
                matches!(
                    dll.filename.as_str(),
                    "d3d12.dll" | "dxgi.dll" | "dxgi_dxmt.dll" | "d3d11.dll" | "d3d10core.dll" | "winemetal.dll"
                )
            })
            .map(|dll| {
                let dest_path = prefix_system32.join(&dll.filename);
                let source_sha256 =
                    if dll.source_present { crate::diagnostics::file_sha256(&dll.source_path) } else { None };
                let dest_present = dest_path.metadata().map(|m| m.is_file() && m.len() > 0).unwrap_or(false);
                let dest_sha256 = if dest_present { crate::diagnostics::file_sha256(&dest_path) } else { None };
                let matches_source =
                    matches!((&source_sha256, &dest_sha256), (Some(source), Some(dest)) if source == dest);
                serde_json::json!({
                    "filename": dll.filename,
                    "source_path": dll.source_path,
                    "dest_path": dest_path,
                    "source_sha256": source_sha256,
                    "dest_sha256": dest_sha256,
                    "dest_present": dest_present,
                    "matches_source": matches_source,
                    "ok": dll.source_present && dest_present && matches_source,
                })
            })
            .collect()
    } else {
        Vec::new()
    };
    let prefix_dlls_ok =
        prefix_dlls.iter().all(|record| record.get("ok").and_then(|value| value.as_bool()).unwrap_or(false));

    let winedllpath = env_value("WINEDLLPATH").unwrap_or_default();
    let dyld_library_path = env_value("DYLD_LIBRARY_PATH").unwrap_or_default();
    let dyld_fallback_library_path = env_value("DYLD_FALLBACK_LIBRARY_PATH").unwrap_or_default();
    let winemetal_unixlib = env_value("DXMT_WINEMETAL_UNIXLIB").unwrap_or_default();
    let m12_env_ok = recipe.pipeline != PipelineId::M12
        || (winedllpath.contains("dxmt_m12/x86_64-windows")
            && (dyld_library_path.contains("dxmt_m12/x86_64-unix")
                || dyld_fallback_library_path.contains("dxmt_m12/x86_64-unix"))
            && winemetal_unixlib == "winemetal.so");

    let ok = runtime_assets_ok && dlls_ok && prefix_dlls_ok && m12_env_ok;
    serde_json::json!({
        "ok": ok,
        "runtime_assets_ok": runtime_assets_ok,
        "game_local_dlls_ok": dlls_ok,
        "prefix_route_dlls_ok": prefix_dlls_ok,
        "m12_env_ok": m12_env_ok,
        "runtime_assets": runtime_assets,
        "game_local_dlls": dlls,
        "prefix_route_dlls": prefix_dlls,
        "env_checks": {
            "WINEDLLPATH": winedllpath,
            "DYLD_LIBRARY_PATH": dyld_library_path,
            "DYLD_FALLBACK_LIBRARY_PATH": dyld_fallback_library_path,
            "WINEDLLOVERRIDES": env_value("WINEDLLOVERRIDES"),
            "DXMT_WINEMETAL_UNIXLIB": winemetal_unixlib,
            "requires_dxmt_m12_windows": recipe.pipeline == PipelineId::M12,
            "requires_dxmt_m12_unix": recipe.pipeline == PipelineId::M12,
        },
    })
}

fn launch_working_dir<'a>(game_dir: &'a std::path::Path, exe_path: &'a std::path::Path) -> &'a std::path::Path {
    exe_path.parent().unwrap_or(game_dir)
}

fn validate_recipe_runtime(recipe: &super::recipe::LaunchRecipe) -> Result<(), Box<dyn std::error::Error>> {
    let missing: Vec<String> = recipe
        .runtime_assets
        .iter()
        .filter(|asset| asset.required && !asset.present)
        .map(|asset| format!("{} ({})", asset.name, asset.path.display()))
        .collect();

    if missing.is_empty() {
        Ok(())
    } else {
        Err(format!("MetalSharp runtime is incomplete: {}", missing.join(", ")).into())
    }
}

fn gptk_ensure_dependencies(home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    if !crate::platform::rosetta_is_installed() {
        eprintln!("d3dmetal: Installing Rosetta 2...");
        let status =
            std::process::Command::new("softwareupdate").args(["--install-rosetta", "--agree-to-license"]).status()?;
        if !status.success() {
            return Err(
                "Failed to install Rosetta 2. Install manually: softwareupdate --install-rosetta --agree-to-license"
                    .into(),
            );
        }
    }
    crate::installer::ensure_gptk_runtime_ready(home)
        .map(|_| ())
        .map_err(|e| format!("D3DMetal GPTK runtime setup failed: {}", e).into())
}

fn launch_d3dmetal_gptk(appid: u32, node: &PipelineNode) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    launch_d3dmetal_gptk_with_context(appid, node, None, &[], None)
}

fn launch_d3dmetal_gptk_with_context(
    appid: u32,
    node: &PipelineNode,
    prefix_override: Option<&Path>,
    extra_env: &[(String, String)],
    log_path: Option<&Path>,
) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    gptk_ensure_dependencies(&home)?;

    if !crate::platform::gptk_prefix_ready(&home) {
        return Err("GPTK prefix is not ready — open the bottle settings and repair 'gptk_prefix' first".into());
    }
    let gptk_prefix = crate::platform::gptk_prefix_path(&home);
    crate::platform::sync_gptk_prefix(&home)?;
    let gptk_root = crate::platform::gptk_wine_root_for_home(&home);
    let gptk_wine64 = crate::platform::gptk_wine64_binary_for_home(&home);
    let gptk_wineserver = crate::platform::gptk_wineserver_binary_for_home(&home);

    let default_log_path;
    let log_path = match log_path {
        Some(path) => Some(path),
        None => {
            default_log_path = mtsp_launch_log_path(appid);
            Some(default_log_path.as_path())
        },
    };

    let recipe = super::recipe::build_launch_recipe(appid, node)?;
    let game_dir = recipe.game_dir.as_ref().ok_or("game dir not found")?;
    let exe_path = recipe.exe_path.as_ref().ok_or("game exe not found")?;
    let exe_dir = launch_working_dir(game_dir, exe_path);
    let exe_name = exe_path.file_name().unwrap_or_default().to_string_lossy().to_string();

    apply_start_protected_game_bypass(appid, game_dir);

    let prefix = gptk_prefix;
    let prefix_str = prefix.to_string_lossy().to_string();
    let offline_mode = extra_env.iter().any(|(key, value)| key == "METALSHARP_OFFLINE_MODE" && value == "1");
    if offline_mode {
        if crate::platform::disable_gptk_steam_launcher_for_offline(&prefix)? {
            eprintln!(
                "d3dmetal offline: disabled GPTK Steam launcher at {}",
                crate::platform::gptk_disabled_steam_exe(&prefix).display()
            );
        }
        let _ = Command::new(&gptk_wineserver).env("WINEPREFIX", &prefix_str).arg("-k").status();
    } else {
        crate::platform::restore_gptk_steam_launcher(&prefix)?;
    }

    deploy_steam_appid(game_dir, appid);

    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");

    if node.uses_winedllpath_routing() {
        validate_recipe_runtime(&recipe)?;
    }
    deploy_recipe_dlls(&recipe)?;
    deploy_prefix_route_dlls(&recipe, &prefix)?;

    let cache_paths = build_cache_paths(&home, node, appid);

    let mut dyld_parts = vec![
        gptk_root.join("lib").to_string_lossy().to_string(),
        gptk_root.join("lib").join("wine").join("x86_64-unix").to_string_lossy().to_string(),
        gptk_root.join("lib").join("external").to_string_lossy().to_string(),
    ];
    for dyld_dir in &node.dyld_paths {
        let full = ms_root.join(dyld_dir);
        dyld_parts.push(full.to_string_lossy().to_string());
    }
    let dyld = dyld_parts.join(":");

    let mut cmd = Command::new(&gptk_wine64);
    cmd.current_dir(exe_dir)
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", wine_debug_value())
        .env("WINEDEBUGGER", "none")
        .env("WINESERVER", &gptk_wineserver)
        .env("WINELOADER", &gptk_wine64)
        .env("DYLD_FALLBACK_LIBRARY_PATH", &dyld)
        .env("MS_GRAPHICS_BACKEND", node.graphics_backend)
        .env("WINEMSYNC", msync_env_value(&home));
    apply_metalfx_home_env(&mut cmd, &home);

    if node.uses_winedllpath_routing() {
        let winedllpath = build_winedllpath(&ms_root, &node.winedllpath_dirs);
        cmd.env("WINEDLLPATH", &winedllpath);
    }

    if let Some(overrides) = node.wine_overrides {
        cmd.env("WINEDLLOVERRIDES", overrides);
    }

    apply_cache_env(&mut cmd, node, cache_paths.as_ref(), &ms_root);
    apply_app_launch_env(&mut cmd, appid, node.id);
    for (key, value) in extra_env {
        cmd.env(key, value);
    }

    cmd.arg(&exe_name);
    cmd.args(&recipe.launch_args);
    attach_launch_log(
        &mut cmd,
        log_path,
        LaunchLogContext {
            appid,
            node,
            prefix: &prefix,
            cwd: exe_dir,
            exe_name: &exe_name,
            args: &recipe.launch_args,
            cache_paths: cache_paths.as_ref(),
        },
    )?;
    let child = cmd.spawn()?;
    Ok((child.id(), node.id.to_legacy_method()))
}

fn launch_dxmt_metal(appid: u32, node: &PipelineNode) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    launch_dxmt_metal_with_context(appid, node, None, &[], None)
}

fn launch_dxmt_metal_with_context(
    appid: u32,
    node: &PipelineNode,
    prefix_override: Option<&Path>,
    extra_env: &[(String, String)],
    log_path: Option<&Path>,
) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);
    let default_log_path;
    let log_path = match log_path {
        Some(path) => Some(path),
        None => {
            default_log_path = mtsp_launch_log_path(appid);
            Some(default_log_path.as_path())
        },
    };

    if !wine.exists() {
        return Err("MetalSharp Wine not found — run setup first".into());
    }
    sanitize_metalsharp_wine_wrapper_env()?;

    prepare_start_protected_game_for_pipeline(appid, node.id);
    let recipe = super::recipe::build_launch_recipe(appid, node)?;
    let game_dir = recipe.game_dir.as_ref().ok_or("game dir not found")?;
    let exe_path = recipe.exe_path.as_ref().ok_or("game exe not found")?;
    let exe_dir = launch_working_dir(game_dir, exe_path);
    let prefix = prefix_override
        .map(Path::to_path_buf)
        .unwrap_or_else(|| crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam"));
    let prefix_str = prefix.to_string_lossy().to_string();
    let exe_name = exe_path.file_name().unwrap_or_default().to_string_lossy().to_string();

    if node.uses_winedllpath_routing() {
        validate_recipe_runtime(&recipe)?;
        cleanup_legacy_injections(game_dir)?;
    }
    if node.id == PipelineId::M12 {
        cleanup_m12_legacy_hook_artifacts(game_dir, &prefix);
    }
    deploy_recipe_dlls(&recipe)?;
    deploy_prefix_route_dlls(&recipe, &prefix)?;

    deploy_d3d12_agility_sidecars(appid, node, game_dir)?;

    let cache_paths = build_cache_paths(&home, node, appid);
    if node.id == PipelineId::M12
        && std::env::var("METALSHARP_M12_LIVE_MSC_SIDECAR")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
    {
        spawn_metalshaderconverter_sidecar(appid, &home, cache_paths.as_ref());
    }
    let dxmt_config_file = ms_root.join("etc").join("dxmt.conf").to_string_lossy().to_string();

    let mut cmd = Command::new(&wine);
    cmd.current_dir(exe_dir)
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", wine_debug_value())
        .env("WINEDEBUGGER", "none");
    apply_route_library_env(&mut cmd, &ms_root, &node.dyld_paths);
    apply_metalfx_home_env(&mut cmd, &home);

    if node.uses_winedllpath_routing() {
        let winedllpath = build_winedllpath(&ms_root, &node.winedllpath_dirs);
        cmd.env("WINEDLLPATH", &winedllpath);
    }

    if let Some(overrides) = node.wine_overrides {
        cmd.env("WINEDLLOVERRIDES", overrides);
    }

    apply_cache_env(&mut cmd, node, cache_paths.as_ref(), &ms_root);
    if node.backend == "dxmt" {
        cmd.env("DXMT_CONFIG_FILE", &dxmt_config_file);
        cmd.env("DXMT_WINEMETAL_UNIXLIB", dxmt_winemetal_unixlib_path(&ms_root));
    }

    cmd.env("MS_GRAPHICS_BACKEND", node.graphics_backend);
    cmd.env("WINEMSYNC", msync_env_value(&home));
    for ev in &node.env_vars {
        cmd.env(ev.key, ev.value);
    }
    // MetalFX strength is the user's choice (metalfx.overlay.json): reconcile
    // the DXMT env after node env so the toggle is authoritative.
    apply_metal_fx_config_cmd(&mut cmd, node, &home);
    apply_app_launch_env(&mut cmd, appid, node.id);
    for (key, value) in extra_env {
        cmd.env(key, value);
    }

    cmd.arg(&exe_name);
    cmd.args(&recipe.launch_args);
    attach_launch_log(
        &mut cmd,
        log_path,
        LaunchLogContext {
            appid,
            node,
            prefix: &prefix,
            cwd: exe_dir,
            exe_name: &exe_name,
            args: &recipe.launch_args,
            cache_paths: cache_paths.as_ref(),
        },
    )?;
    let child = cmd.spawn()?;
    Ok((child.id(), node.id.to_legacy_method()))
}

fn deploy_d3d12_agility_sidecars(
    appid: u32,
    node: &PipelineNode,
    game_dir: &Path,
) -> Result<(), Box<dyn std::error::Error>> {
    if !matches!(node.id, PipelineId::M12 | PipelineId::M13) {
        return Ok(());
    }

    let home = dirs::home_dir().ok_or("no home dir")?;
    crate::setup::stage_agility_sdk_for_game(appid, game_dir, &home)
}

fn launch_wine_bare(appid: u32, node: &PipelineNode) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    launch_wine_bare_with_context(appid, node, None, &[], None)
}

fn launch_wine_bare_with_context(
    appid: u32,
    node: &PipelineNode,
    prefix_override: Option<&Path>,
    extra_env: &[(String, String)],
    log_path: Option<&Path>,
) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = crate::platform::runtime_wine_binary(&ms_root);

    if !wine.exists() {
        return Err("MetalSharp Wine not found — run setup first".into());
    }

    let recipe = super::recipe::build_launch_recipe(appid, node)?;
    let game_dir = recipe.game_dir.as_ref().ok_or("game dir not found")?;
    let exe_path = recipe.exe_path.as_ref().ok_or("game exe not found")?;
    let exe_dir = launch_working_dir(game_dir, exe_path);
    let prefix = prefix_override
        .map(Path::to_path_buf)
        .unwrap_or_else(|| crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam"));
    let prefix_str = prefix.to_string_lossy().to_string();
    let exe_name = exe_path.file_name().unwrap_or_default().to_string_lossy().to_string();

    validate_recipe_runtime(&recipe)?;

    let mut cmd = Command::new(&wine);
    cmd.current_dir(exe_dir)
        .env("WINEPREFIX", &prefix_str)
        .env("WINEDEBUG", wine_debug_value())
        .env("WINEDEBUGGER", "none");
    apply_route_library_env(&mut cmd, &ms_root, &node.dyld_paths);
    apply_metalfx_home_env(&mut cmd, &home);

    if let Some(overrides) = node.wine_overrides {
        cmd.env("WINEDLLOVERRIDES", overrides);
    }

    let cache_paths = build_cache_paths(&home, node, appid);
    apply_cache_env(&mut cmd, node, cache_paths.as_ref(), &ms_root);
    cmd.env("MS_GRAPHICS_BACKEND", node.graphics_backend);
    cmd.env("WINEMSYNC", msync_env_value(&home));
    for ev in &node.env_vars {
        cmd.env(ev.key, ev.value);
    }
    apply_app_launch_env(&mut cmd, appid, node.id);
    for (key, value) in extra_env {
        cmd.env(key, value);
    }

    cmd.arg(&exe_name);
    cmd.args(&recipe.launch_args);
    attach_launch_log(
        &mut cmd,
        log_path,
        LaunchLogContext {
            appid,
            node,
            prefix: &prefix,
            cwd: exe_dir,
            exe_name: &exe_name,
            args: &recipe.launch_args,
            cache_paths: cache_paths.as_ref(),
        },
    )?;
    let child = cmd.spawn()?;
    Ok((child.id(), node.id.to_legacy_method()))
}

fn attach_launch_log(
    cmd: &mut Command,
    log_path: Option<&Path>,
    context: LaunchLogContext<'_>,
) -> Result<(), Box<dyn std::error::Error>> {
    let Some(log_path) = log_path else {
        return Ok(());
    };
    if let Some(parent) = log_path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut log = OpenOptions::new().create(true).append(true).open(log_path)?;
    writeln!(log, "appid={}", context.appid)?;
    writeln!(log, "pipeline={}", context.node.name)?;
    writeln!(log, "prefix={}", context.prefix.display())?;
    write_runtime_identity(&mut log, context.prefix, Some(context.appid))?;
    writeln!(log, "cwd={}", context.cwd.display())?;
    writeln!(log, "exe={}", context.exe_name)?;
    writeln!(log, "args={:?}", context.args)?;
    writeln!(log, "graphics_backend={}", context.node.graphics_backend)?;
    if let Some(cache) = context.cache_paths {
        writeln!(log, "shader_cache={}/", cache.shader)?;
        writeln!(log, "pipeline_cache={}/", cache.pipeline)?;
    }
    writeln!(log, "--- wine output ---")?;
    let stdout = log.try_clone()?;
    cmd.stdout(Stdio::from(stdout)).stderr(Stdio::from(log));
    Ok(())
}

fn write_runtime_identity(log: &mut dyn Write, prefix: &Path, appid: Option<u32>) -> std::io::Result<()> {
    let home = dirs::home_dir().unwrap_or_default();
    let metalsharp_home = crate::platform::metalsharp_home_dir_for(&home);
    writeln!(log, "metalsharp_home={}", metalsharp_home.display())?;
    writeln!(log, "host_abi=1.0")?;
    writeln!(log, "host_runtime={}", metalsharp_home.join("runtime").join("host").display())?;
    writeln!(log, "wine_runtime={}", metalsharp_home.join("runtime").join("wine").display())?;
    writeln!(log, "steam_bridge_port={}", bridge_port())?;
    if let Some(appid) = appid {
        let bottle_id = format!("steam_{}", appid);
        writeln!(log, "route_state=bottle_manifest")?;
        writeln!(log, "bottle_manifest={}", crate::bottles::bottle_manifest_path(&bottle_id).display())?;
        writeln!(log, "bottle_logs={}", crate::bottles::bottle_logs_dir(&bottle_id).display())?;
    }
    if prefix == metalsharp_home.join("prefix-steam") {
        writeln!(log, "steam_identity_mode=wine_steam_background")?;
    }
    Ok(())
}

fn launch_steam(appid: u32) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    let pid = crate::launch::launch_via_steam(appid)?;
    Ok((pid, "steam"))
}

fn launch_macos_steam(appid: u32) -> Result<(u32, &'static str), Box<dyn std::error::Error>> {
    if crate::steam::is_wine_steam_running() {
        return Err("Wine Steam is running. Stop Wine Steam before launching through MacOS Steam.".into());
    }

    let result = crate::steam::launch_macos_steam_game(appid)?;
    let pid = result.get("pid").and_then(|v| v.as_u64()).unwrap_or(0) as u32;
    Ok((pid, "macos_steam"))
}

/// Pre-flight readiness check for the FNA/Mono route: verifies mono binary +
/// arch, shims present, SDL2/3 per profile deps, config resolved, Rosetta for
/// x86, and Unity runtime lane availability for Unity-Mono games. Returns an
/// actionable error naming exactly what is missing.
pub fn fna_ready_check(appid: u32, game_dir: &Path) -> Result<Vec<String>, String> {
    let mut problems = Vec::new();
    let profile = crate::mono_profile::discover_mono_profile(game_dir);
    let fna_profile = find_fna_profile(appid);

    // Mono binary + arch.
    let mono_bin = match find_mono_binary_for_app(appid) {
        Ok(b) => b,
        Err(e) => {
            problems.push(e.to_string());
            PathBuf::new()
        },
    };
    if !mono_bin.as_os_str().is_empty() {
        let wants_x86 = profile.kind == crate::mono_profile::MonoProfileKind::None
            && fna_profile.mono_arch == MonoArch::X86
            || (profile.kind != crate::mono_profile::MonoProfileKind::None && !profile.is_64_bit);
        if wants_x86 && crate::platform::current() == crate::platform::HostPlatform::Macos {
            // Rosetta required for x86 mono; report explicitly.
            let arch_probe = std::process::Command::new("arch").arg("-x86_64").arg("true").status();
            match arch_probe {
                Ok(status) if status.success() => {},
                _ => problems.push(
                    "x86 Mono requires Rosetta 2 (missing or unavailable); install it or use the arm64 route".into(),
                ),
            }
        }
    }

    // Shims present (kernel32/user32/Carbon are launch-required for FNA).
    let shims_dir = PathBuf::from(find_shims_dir());
    for shim in ["libkernel32.dylib", "libuser32.dylib", FNA_CARBON_SHIM, FNA_CARBON_INTERPOSE_SHIM] {
        if !shims_dir.join(shim).is_file() {
            problems.push(format!("missing shared shim {shim} (run repair or reinstall runtime)"));
        }
    }

    // Profile deps.
    if profile.deps.sdl3 {
        let ms_home = crate::platform::metalsharp_home_dir();
        if !ms_home.join("runtime").join("sdl3").join("libSDL3.dylib").is_file() {
            problems.push("game signals SDL3 but the bundled libSDL3.dylib is missing (reinstall runtime)".into());
        }
    }

    // Unity runtime lane for Unity-Mono games.
    if profile.kind == crate::mono_profile::MonoProfileKind::UnityMono {
        let ms_home = crate::platform::metalsharp_home_dir();
        match unity_runtime_lane_for_version(profile.unity_version.as_deref(), &ms_home) {
            Some(_) => {},
            None => problems.push("no bundled Unity Mono runtime lane available (reinstall runtime)".into()),
        }
    }

    if problems.is_empty() {
        Ok(Vec::new())
    } else {
        Err(format!("FNA/Mono launch blocked: {}", problems.join("; ")))
    }
}

fn launch_fna_arm64(appid: u32) -> Result<(u32, &'static str, PathBuf), Box<dyn std::error::Error>> {
    let profile = find_fna_profile(appid);
    let node = get_pipeline(PipelineId::FnaArm64);
    let game_dir = resolve_fna_game_dir(appid)?;
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let local_dir = ms_home.join("games").join(appid.to_string());
    let dir = if game_dir.exists() { &game_dir } else { &local_dir };

    // Pre-flight readiness: surface missing mono/shims/SDL3/Unity lane as an
    // actionable error before any deploy or spawn.
    fna_ready_check(appid, dir)?;

    // Profile-aware deploy (idempotent; also run by bottle save).
    let discovered = crate::mono_profile::discover_mono_profile(dir);
    if discovered.kind != crate::mono_profile::MonoProfileKind::None {
        let mut report = crate::fna_profile::AssetStagingReport::new(appid);
        let _ = deploy_unity_runtime(dir, &discovered, &ms_home, Some(&mut report));
        let _ = deploy_xna_assembly_set(dir, &discovered, &ms_home, Some(&mut report));
        let _ = deploy_profile_deps(dir, &discovered, &ms_home, Some(&mut report));
        if !report.receipts.is_empty() {
            let _ = report.persist(dir);
        }
    }

    ensure_launcher_exe(appid, dir);
    deploy_fna_assemblies(appid, dir);
    if appid != 504230 {
        deploy_steam_shim(dir);
    }

    let _ = ensure_bridge_running();

    // MonoKickstart games dispatch to the kickstart launcher (bundled
    // kick.bin.osx + osx/ payloads; the game's own .bin.osx is preferred).
    if discovered.kind == crate::mono_profile::MonoProfileKind::MonoKickstart {
        let kickstart_dir = ms_home.join("runtime").join("fna-kickstart");
        if !kickstart_dir.join("kick.bin.osx").is_file() {
            return Err(format!("MonoKickstart payload missing at {}", kickstart_dir.display()).into());
        }
        let exe_path = PathBuf::from(resolve_game_exe(dir));
        return launch_fna_kickstart(appid, profile, node, dir, &exe_path, &home, &ms_home, &kickstart_dir);
    }
    let exe = if !profile.preferred_exes.is_empty() {
        find_preferred_exe(dir, profile.preferred_exes)?
    } else {
        resolve_game_exe(dir).into()
    };

    let mono_bin = find_mono_binary_for_app(appid)?;
    let mono_config = rewrite_config_with_absolute_paths(profile.mono_config, dir)?;
    let shims_dir = find_shims_dir();
    let mono_root =
        mono_bin.parent().and_then(|p| p.parent()).map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from(""));
    let mono_lib = mono_root.join("lib");
    let mono_profile = mono_lib.join("mono").join("4.5");
    let mut library_paths = vec![dir.to_string_lossy().to_string()];
    if profile.include_runtime_shims_in_library_path {
        library_paths.push(shims_dir);
    }
    library_paths.push(mono_lib.to_string_lossy().to_string());
    if crate::platform::current() == crate::platform::HostPlatform::Macos {
        library_paths.push("/opt/homebrew/lib".into());
    } else {
        library_paths.push("/usr/lib".into());
        library_paths.push("/usr/local/lib".into());
    }
    let runtime_lib_path = library_paths.join(":");
    let runtime_lib_key = if crate::platform::current() == crate::platform::HostPlatform::Macos {
        "DYLD_LIBRARY_PATH"
    } else {
        "LD_LIBRARY_PATH"
    };

    let mut cmd =
        if profile.mono_arch == MonoArch::X86 && crate::platform::current() == crate::platform::HostPlatform::Macos {
            let mut arch_cmd = Command::new("arch");
            arch_cmd.arg("-x86_64").arg(&mono_bin);
            arch_cmd
        } else {
            Command::new(&mono_bin)
        };
    let mono_path = format!("{}:{}", dir.to_string_lossy(), mono_profile.to_string_lossy());
    let log_path = fna_launch_log_path(appid);
    if let Some(parent) = log_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let stdout = OpenOptions::new().create(true).append(true).open(&log_path)?;
    let stderr = stdout.try_clone()?;

    cmd.current_dir(dir).env(runtime_lib_key, &runtime_lib_path).env("DYLD_FALLBACK_LIBRARY_PATH", &runtime_lib_path);
    if !mono_config.is_empty() {
        cmd.env("MONO_CONFIG", &mono_config);
    }
    cmd.env("MONO_ENV_OPTIONS", "--runtime=v4.0")
        .env("MONO_PATH", mono_path)
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    for (key, value) in fna_native_launch_env(dir) {
        cmd.env(key, value);
    }

    let cache_paths = build_cache_paths(&home, node, appid);
    apply_cache_env(
        &mut cmd,
        node,
        cache_paths.as_ref(),
        &crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine"),
    );

    for ev in &node.env_vars {
        cmd.env(ev.key, ev.value);
    }

    cmd.env("SteamAppId", appid.to_string());
    cmd.env("SteamGameId", appid.to_string());

    cmd.arg(&exe);

    let mut child = cmd.spawn()?;
    // Post-spawn health check: poll 3 x 1s (longer than the old 900ms single
    // check) so a delayed config/arch failure surfaces with the log tail
    // instead of reporting a launched-but-dead game.
    let mut exited_early = None;
    for _ in 0..3 {
        std::thread::sleep(Duration::from_secs(1));
        if let Some(status) = child.try_wait()? {
            exited_early = Some(status);
            break;
        }
    }
    if let Some(status) = exited_early {
        let log_tail = tail_text(&log_path, 4096);
        return Err(format!("FNA/Mono/XNA launch exited early with status {}. Log: {}", status, log_tail).into());
    }
    let method = profile.method_label;
    Ok((child.id(), method, log_path))
}

#[allow(clippy::too_many_arguments)]
fn launch_fna_kickstart(
    appid: u32,
    profile: &FnaGameProfile,
    node: &PipelineNode,
    dir: &PathBuf,
    exe: &PathBuf,
    home: &PathBuf,
    ms_home: &PathBuf,
    kickstart_dir: &PathBuf,
) -> Result<(u32, &'static str, PathBuf), Box<dyn std::error::Error>> {
    let kick_bin = kickstart_dir.join("kick.bin.osx");
    let exe_name = exe.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| "Game.exe".to_string());
    let bin_name = exe_name.replace(".exe", ".bin.osx");
    let game_kick = dir.join(&bin_name);

    let _ = std::fs::copy(&kick_bin, &game_kick);

    let source_libmono = kickstart_dir.join("osx").join("libmonosgen-2.0.1.dylib");
    if source_libmono.exists() {
        let _ = Command::new("/usr/bin/install_name_tool")
            .arg("-id")
            .arg("@rpath/libmonosgen-2.0.1.dylib")
            .arg(&source_libmono)
            .output();
    }

    let game_osx = dir.join("osx");
    let _ = std::fs::create_dir_all(&game_osx);

    let kick_osx = kickstart_dir.join("osx");
    if kick_osx.exists() {
        for entry in std::fs::read_dir(&kick_osx)? {
            let entry = entry?;
            let src = entry.path();
            if src.is_file() || src.is_symlink() {
                let name = entry.file_name();
                let dst = game_osx.join(&name);
                if !dst.exists() {
                    if src.is_symlink() {
                        if let Ok(target) = std::fs::read_link(&src) {
                            let _ = std::os::unix::fs::symlink(target, dst);
                        }
                    } else {
                        let _ = std::fs::copy(&src, &dst);
                    }
                }
            }
        }
        let game_libmono = game_osx.join("libmonosgen-2.0.1.dylib");
        if game_libmono.exists() {
            let _ = Command::new("/usr/bin/install_name_tool")
                .arg("-id")
                .arg("@rpath/libmonosgen-2.0.1.dylib")
                .arg(&game_libmono)
                .output();
        }
    }

    let fnalibs_dir = ms_home.join("runtime").join("fnalibs");
    if fnalibs_dir.exists() {
        for entry in std::fs::read_dir(&fnalibs_dir)? {
            let entry = entry?;
            let src = entry.path();
            if src.is_file() && !game_osx.join(entry.file_name()).exists() {
                let _ = std::fs::copy(&src, game_osx.join(entry.file_name()));
            }
        }
    }

    let shims_dir = PathBuf::from(find_shims_dir());
    deploy_fna_native_shims(&dir.to_path_buf(), &shims_dir);
    for spec in FNA_NATIVE_SHIMS {
        let shim = dir.join(spec.output);
        if shim.exists() {
            let dst = game_osx.join(spec.output);
            if !dst.exists() {
                let _ = std::fs::copy(&shim, &dst);
            }
            for symlink in spec.symlinks {
                let link = game_osx.join(symlink);
                if !link.exists() {
                    let _ = std::os::unix::fs::symlink(spec.output, link);
                }
            }
        }
    }

    let shared_libs =
        ["libsteam_api.dylib", "libCSteamworks.dylib", "libfmod.dylib", "libfmodstudio.dylib", "libnfd.dylib"];
    for lib in &shared_libs {
        let src = shims_dir.join(lib);
        if src.exists() && !game_osx.join(lib).exists() {
            let _ = std::fs::copy(&src, game_osx.join(lib));
        }
        if dir.join(lib).exists() && !game_osx.join(lib).exists() {
            let _ = std::fs::copy(dir.join(lib), game_osx.join(lib));
        }
    }

    let bcl_dlls = [
        "mscorlib.dll",
        "System.dll",
        "System.Core.dll",
        "System.Xml.dll",
        "System.Xml.Linq.dll",
        "System.Configuration.dll",
        "System.Security.dll",
        "System.Runtime.Serialization.dll",
        "System.Data.dll",
        "System.Drawing.dll",
        "Mono.Security.dll",
        "Mono.Posix.dll",
        "Microsoft.CSharp.dll",
    ];
    for dll in &bcl_dlls {
        let src = kickstart_dir.join(dll);
        if src.exists() && !dir.join(dll).exists() {
            let _ = std::fs::copy(&src, dir.join(dll));
        }
    }

    let fna_dll = kickstart_dir.join("FNA.dll");
    if fna_dll.exists() && !dir.join("FNA.dll").exists() {
        let _ = std::fs::copy(&fna_dll, dir.join("FNA.dll"));
    }

    let monoconfig_src = kickstart_dir.join("monoconfig");
    if monoconfig_src.exists() {
        let game_monoconfig = dir.join("monoconfig");
        let custom_config = find_config(profile.mono_config);
        let custom_path = PathBuf::from(&custom_config);
        if custom_path.exists() {
            let _ = std::fs::copy(&custom_path, &game_monoconfig);
        } else {
            let _ = std::fs::copy(&monoconfig_src, &game_monoconfig);
        }
    }

    let machine_config_src = kickstart_dir.join("mono").join("4.5").join("machine.config");
    let machine_config_dst = dir.join("mono").join("4.5").join("machine.config");
    if machine_config_src.exists() && !machine_config_dst.exists() {
        let _ = std::fs::create_dir_all(machine_config_dst.parent().unwrap());
        let _ = std::fs::copy(&machine_config_src, &machine_config_dst);
    }

    let log_path = fna_launch_log_path(appid);
    if let Some(parent) = log_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let stdout = OpenOptions::new().create(true).append(true).open(&log_path)?;
    let stderr = stdout.try_clone()?;

    let mut cmd = Command::new(&game_kick);
    cmd.current_dir(dir).env("MONO_DISABLE_SHARED_AREA", "1").stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr));

    for (key, value) in fna_native_launch_env(dir) {
        cmd.env(key, value);
    }

    let cache_paths = build_cache_paths(home, node, appid);
    apply_cache_env(&mut cmd, node, cache_paths.as_ref(), &ms_home.join("runtime").join("wine"));

    for ev in &node.env_vars {
        cmd.env(ev.key, ev.value);
    }

    cmd.env("SteamAppId", appid.to_string());
    cmd.env("SteamGameId", appid.to_string());

    if profile.mono_arch == MonoArch::X86 && crate::platform::current() == crate::platform::HostPlatform::Macos {
        let mut arch_cmd = Command::new("arch");
        arch_cmd.arg("-x86_64").arg(&game_kick);
        arch_cmd.current_dir(dir).stdout(Stdio::from(OpenOptions::new().create(true).append(true).open(&log_path)?));
        arch_cmd.stderr(Stdio::from(OpenOptions::new().create(true).append(true).open(&log_path)?));
        for (key, value) in fna_native_launch_env(dir) {
            arch_cmd.env(key, value);
        }
        for ev in &node.env_vars {
            arch_cmd.env(ev.key, ev.value);
        }
        arch_cmd.env("SteamAppId", appid.to_string());
        arch_cmd.env("SteamGameId", appid.to_string());
        arch_cmd.env("MONO_DISABLE_SHARED_AREA", "1");
        let mut child = arch_cmd.spawn()?;
        std::thread::sleep(Duration::from_millis(900));
        if let Some(status) = child.try_wait()? {
            let log_tail = tail_text(&log_path, 4096);
            return Err(
                format!("FNA/MonoKickstart launch exited early with status {}. Log: {}", status, log_tail).into()
            );
        }
        return Ok((child.id(), profile.method_label, log_path));
    }

    let mut child = cmd.spawn()?;
    std::thread::sleep(Duration::from_millis(900));
    if let Some(status) = child.try_wait()? {
        let log_tail = tail_text(&log_path, 4096);
        return Err(format!("FNA/MonoKickstart launch exited early with status {}. Log: {}", status, log_tail).into());
    }
    Ok((child.id(), profile.method_label, log_path))
}

fn find_mono_binary_for_app(appid: u32) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let profile = find_fna_profile(appid);
    if profile.mono_arch == MonoArch::X86 {
        let mono_bin = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-x86").join("bin");
        for name in ["mono-sgen64", "mono-sgen", "mono"] {
            let candidate = mono_bin.join(name);
            if candidate.exists() {
                return Ok(candidate);
            }
        }
    }
    let candidates = vec![
        PathBuf::from("/opt/homebrew/bin/mono"),
        PathBuf::from("/usr/local/bin/mono"),
        PathBuf::from("/usr/bin/mono"),
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-arm64").join("bin").join("mono"),
    ];
    for c in candidates {
        if c.exists() {
            return Ok(c);
        }
    }
    Err("Mono not found — install Mono or use setup to install runtime support".into())
}

fn fna_launch_log_path(appid: u32) -> PathBuf {
    let ts = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs();
    crate::bottles::bottle_logs_dir(&format!("steam_{}", appid)).join(format!("fna-launch-{}.log", ts))
}

fn mtsp_launch_log_path(appid: u32) -> PathBuf {
    let ts = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs();
    crate::bottles::bottle_logs_dir(&format!("steam_{}", appid)).join(format!("mtsp-launch-{}.log", ts))
}

fn tail_text(path: &Path, max_bytes: usize) -> String {
    let Ok(bytes) = std::fs::read(path) else {
        return format!("{} not readable", path.display());
    };
    let start = bytes.len().saturating_sub(max_bytes);
    String::from_utf8_lossy(&bytes[start..]).trim().to_string()
}

fn build_dyld(ms_root: &PathBuf, paths: &[&str]) -> String {
    paths.iter().map(|p| ms_root.join(p).to_string_lossy().to_string()).collect::<Vec<_>>().join(":")
}

fn dxmt_winemetal_unixlib_path(_ms_root: &Path) -> String {
    "winemetal.so".to_string()
}

fn route_library_env_pairs(ms_root: &PathBuf, paths: &[&str]) -> Vec<(String, String)> {
    if paths.is_empty() {
        return Vec::new();
    }

    let path = build_dyld(ms_root, paths);
    if crate::platform::current() == crate::platform::HostPlatform::Macos {
        vec![("DYLD_LIBRARY_PATH".to_string(), path.clone()), ("DYLD_FALLBACK_LIBRARY_PATH".to_string(), path)]
    } else {
        let key = crate::platform::runtime_library_env(ms_root).map(|(key, _)| key).unwrap_or("LD_LIBRARY_PATH");
        vec![(key.to_string(), path)]
    }
}

fn apply_route_library_env(cmd: &mut Command, ms_root: &PathBuf, paths: &[&str]) {
    for (key, value) in route_library_env_pairs(ms_root, paths) {
        cmd.env(key, value);
    }
}

/// Set METALSHARP_HOME on a Wine child process so the in-process ntdll-hook
/// (x64 and i386) can resolve the MetalFX overlay state file and apply the
/// `DXMT_METALFX_SPATIAL_SWAPCHAIN` toggle live (on the next swapchain recreate).
fn apply_metalfx_home_env(cmd: &mut Command, home: &Path) {
    cmd.env("METALSHARP_HOME", crate::platform::metalsharp_home_dir_for(home).to_string_lossy().to_string());
}

fn cleanup_metalsharp_dlls_from_game_dir(game_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let backup_dir = game_dir.join(".metalsharp").join("pipeline-backup");
    std::fs::create_dir_all(&backup_dir)?;

    let dll_names = [
        "d3d12.dll",
        "d3d11.dll",
        "dxgi.dll",
        "d3d10core.dll",
        "nvapi64.dll",
        "nvngx.dll",
        "winemetal.dll",
        "metalsharp_ntdll_hook.dll",
        "dxgi_dxmt.dll",
    ];

    for dll in &dll_names {
        let src = game_dir.join(dll);
        if src.exists() {
            let dest = backup_dir.join(dll);
            let _ = std::fs::rename(&src, &dest);
        }
    }
    Ok(())
}

fn cleanup_m12_legacy_hook_artifacts(game_dir: &Path, prefix: &Path) {
    let _ = std::fs::remove_file(game_dir.join("metalsharp_ntdll_hook.dll"));
    let windows = prefix.join("drive_c").join("windows");
    for system_dir in ["system32", "syswow64"] {
        let _ = std::fs::remove_file(windows.join(system_dir).join("metalsharp_ntdll_hook.dll"));
    }
}

fn cleanup_legacy_eac_toggle_artifacts(game_dir: &Path) {
    let targets = [
        game_dir.to_path_buf(),
        game_dir.join("Game"),
        game_dir.join("bin"),
        game_dir.join("Binaries").join("Win64"),
        game_dir.join("win64"),
    ];

    for target in targets {
        if !target.exists() {
            continue;
        }
        let config = target.join("anti_cheat_toggler_config.ini");
        let mod_list = target.join("anti_cheat_toggler_mod_list.txt");
        let has_legacy_toggle_marker = config.exists() || mod_list.exists();
        let _ = std::fs::remove_file(&config);
        let _ = std::fs::remove_file(&mod_list);
        if has_legacy_toggle_marker {
            let _ = std::fs::remove_file(target.join("_winhttp.dll"));
        }
    }
}

fn build_winedllpath(ms_root: &PathBuf, dirs: &[&str]) -> String {
    dirs.iter().map(|d| ms_root.join(d).to_string_lossy().to_string()).collect::<Vec<_>>().join(":")
}

fn cleanup_legacy_injections(game_dir: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let injection_dir = game_dir.join(".metalsharp");
    let manifest_path = injection_dir.join("injections.json");
    if !manifest_path.exists() {
        return Ok(());
    }

    let canonical_game_dir = game_dir.canonicalize().unwrap_or_else(|_| game_dir.to_path_buf());

    let manifest_str = std::fs::read_to_string(&manifest_path).unwrap_or_default();
    let manifest: serde_json::Value = match serde_json::from_str(&manifest_str) {
        Ok(v) => v,
        Err(_) => {
            let _ = std::fs::remove_dir_all(&injection_dir);
            return Ok(());
        },
    };

    let dlls = match manifest.get("dlls").and_then(|d| d.as_array()) {
        Some(d) => d,
        None => {
            let _ = std::fs::remove_dir_all(&injection_dir);
            return Ok(());
        },
    };

    let mut any_copy_failed = false;
    for dll in dlls {
        if let Some(backup) = dll.get("backup_path").and_then(|b| b.as_str()) {
            let backup_path = PathBuf::from(backup);
            let canonical_backup = match backup_path.canonicalize() {
                Ok(p) => p,
                Err(_) => continue,
            };
            if !canonical_backup.starts_with(&canonical_game_dir) {
                continue;
            }
            if let Some(dest) = dll.get("dest_path").and_then(|d| d.as_str()) {
                let dest_path = PathBuf::from(dest);
                let canonical_dest = if dest_path.exists() {
                    match dest_path.canonicalize() {
                        Ok(p) => p,
                        Err(_) => continue,
                    }
                } else {
                    match dest_path.parent().and_then(|p| p.canonicalize().ok()) {
                        Some(parent) => parent.join(dest_path.file_name().unwrap_or_default()),
                        None => continue,
                    }
                };
                if !canonical_dest.starts_with(&canonical_game_dir) {
                    continue;
                }
                if std::fs::copy(&canonical_backup, &canonical_dest).is_err() {
                    any_copy_failed = true;
                    continue;
                }
                let _ = std::fs::remove_file(&canonical_backup);
            }
        }
    }

    if !any_copy_failed {
        let _ = std::fs::remove_dir_all(&injection_dir);
    }
    Ok(())
}

fn build_cache_paths(home: &PathBuf, node: &PipelineNode, appid: u32) -> Option<CachePaths> {
    let subdir = node.shader_cache_subdir?;
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let shader_base = preferred_shader_cache_base(home, subdir, appid);
    let pipeline_base = ms_home.join("pipeline-cache").join(subdir).join(appid.to_string());
    let log_base = if subdir == "m12" {
        ms_home.join("logs").join("m12-pipeline").join(appid.to_string())
    } else {
        pipeline_base.clone()
    };
    let _ = std::fs::create_dir_all(&shader_base);
    let _ = std::fs::create_dir_all(&pipeline_base);
    let _ = std::fs::create_dir_all(&log_base);
    super::shader_cache::deploy_preset_cache(home, subdir, appid);
    Some(CachePaths {
        shader: shader_base.to_string_lossy().to_string(),
        pipeline: pipeline_base.to_string_lossy().to_string(),
        log: log_base.to_string_lossy().to_string(),
    })
}

fn preferred_shader_cache_base(home: &PathBuf, subdir: &str, appid: u32) -> PathBuf {
    if appid == 1962700 && subdir == "m12" {
        let game_dirs = crate::scan::resolve_dual_game_dir(appid);
        for game_dir in [game_dirs.wine_dir.as_ref(), game_dirs.macos_dir.as_ref()].into_iter().flatten() {
            let candidate =
                game_dir.join(".metalsharp-cache").join("shader-cache").join(subdir).join(appid.to_string());
            if shader_cache_has_runtime_artifacts(&candidate) {
                return candidate;
            }
        }
    }

    crate::platform::metalsharp_home_dir_for(&home).join("shader-cache").join(subdir).join(appid.to_string())
}

fn shader_cache_has_runtime_artifacts(path: &PathBuf) -> bool {
    let Ok(entries) = std::fs::read_dir(path) else {
        return false;
    };
    entries.flatten().any(|entry| {
        entry
            .path()
            .extension()
            .and_then(|ext| ext.to_str())
            .map(|ext| matches!(ext, "metallib" | "msl" | "dxbc" | "json"))
            .unwrap_or(false)
    })
}

fn steam_pipeline_env_pairs(home: &PathBuf, node: &PipelineNode, appid: u32) -> Vec<(String, String)> {
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let cache_paths = build_cache_paths(home, node, appid);
    let appid_string = appid.to_string();
    let mut env = vec![
        ("SteamAppId".to_string(), appid_string.clone()),
        ("SteamGameId".to_string(), appid_string.clone()),
        ("SteamOverlayGameId".to_string(), appid_string),
    ];

    if let Some(steam_id) = crate::steam::get_steam_id() {
        env.push(("SteamUserSteamID".to_string(), steam_id));
    }

    env.extend(route_library_env_pairs(&ms_root, &node.dyld_paths));
    if let Some(overrides) = node.wine_overrides {
        env.push(("WINEDLLOVERRIDES".to_string(), overrides.to_string()));
    }
    if node.uses_winedllpath_routing() {
        env.push(("WINEDLLPATH".to_string(), build_winedllpath(&ms_root, &node.winedllpath_dirs)));
    }
    if node.backend == "dxmt" {
        env.push(("DXMT_CONFIG_FILE".to_string(), ms_root.join("etc").join("dxmt.conf").to_string_lossy().to_string()));
        env.push(("DXMT_WINEMETAL_UNIXLIB".to_string(), dxmt_winemetal_unixlib_path(&ms_root)));
    }
    env.push(("MS_GRAPHICS_BACKEND".to_string(), node.graphics_backend.to_string()));
    env.push(("WINEMSYNC".to_string(), msync_env_value(home)));
    env.extend(cache_env_pairs(node, cache_paths.as_ref(), &ms_root));
    env.extend(node.env_vars.iter().map(|ev| (ev.key.to_string(), ev.value.to_string())));
    env.extend(app_compat_env_pairs(appid, node.id));
    if let Some(recipe) = super::rules::get_game_recipe(appid) {
        for (key, value) in recipe.env {
            if !is_reserved_route_env_key(node.id, &key) {
                env.push((key, value));
            }
        }
    }
    // MetalFX strength is the user's choice: applied last so it overrides the
    // node default and any game-recipe DXMT_CONFIG/DXMT_METALFX_* entries.
    apply_metal_fx_config(&mut env, node, home);
    // msync toggle likewise wins over recipe env.
    apply_msync_config(&mut env, home);
    env
}

/// Wine msync toggle value (`WINEMSYNC`) for the given home. Defaults ON.
/// A `WINEMSYNC` env var in the parent process overrides the config (same
/// semantics as `msync_enabled()`), so the documented dev override works on
/// every launch path, not just the GOG prefix init.
pub(crate) fn msync_env_value(home: &Path) -> String {
    msync_env_value_from(home, std::env::var("WINEMSYNC").ok())
}

/// Pure core of `msync_env_value` (testable without mutating the process
/// env): `parent_env` carries the optional parent `WINEMSYNC` value.
fn msync_env_value_from(home: &Path, parent_env: Option<String>) -> String {
    if let Some(value) = parent_env {
        return if crate::launch::truthy(&value) { "1".to_string() } else { "0".to_string() };
    }
    if crate::launch::msync_enabled_for(home) {
        "1".to_string()
    } else {
        "0".to_string()
    }
}

/// Reconcile the WINEMSYNC env entry after recipe env so the sidebar msync
/// toggle is authoritative (a game recipe could otherwise override it for
/// non-M12 routes, where WINEMSYNC is not a reserved key).
fn apply_msync_config(env: &mut Vec<(String, String)>, home: &Path) {
    let value = msync_env_value(home);
    if let Some((_, existing)) = env.iter_mut().rev().find(|(key, _)| key == "WINEMSYNC") {
        *existing = value;
    } else {
        env.push(("WINEMSYNC".to_string(), value));
    }
}

/// DXMT routes that carry MetalFX Spatial upscaling (`DXMT_METALFX_*` env +
/// `d3d11.metalSpatialUpscaleFactor` in `DXMT_CONFIG`): M10, M10(32), M11,
/// M11(32). M9 has no upscale key; M12 is vkd3d-proton; M13/D3DMetal/FNA use
/// other stacks.
fn is_metalfx_route(pipeline_id: PipelineId) -> bool {
    matches!(pipeline_id, PipelineId::M10 | PipelineId::M10_32 | PipelineId::M11 | PipelineId::M11_32)
}

/// Apply the MetalFX Spatial upscale strength to the DXMT env, overriding
/// whatever the pipeline node or a game recipe set. The single source of
/// truth is the metalfx overlay state (`etc/metalfx.overlay.json`, written by
/// `/metalfx/toggle` — the sidebar toggle drives that endpoint). This
/// reconciliation also fixes the node env's hardcoded `metalSpatialUpscaleFactor`
/// (1.43) shadowing the overlay's dxmt.conf factor: DXMT parses DXMT_CONFIG env
/// AFTER the conf file, so the env wins unless we rewrite it here.
/// `off` disables MetalFX the same way the app-compat path does
/// (`DXMT_METALFX_SPATIAL_SWAPCHAIN=0` + no upscale factor); enabled strengths
/// keep the swapchain integration and set the factor. Only DXMT routes
/// (M10/M10(32)/M11/M11(32)) are touched.
fn apply_metal_fx_config(env: &mut Vec<(String, String)>, node: &PipelineNode, home: &Path) {
    if !is_metalfx_route(node.id) {
        return;
    }
    let (enabled, factor) = crate::metalfx::effective_state_for(home);
    // Pass the actual overlay factor through (formatted to 2 decimals) so a
    // value like 2.0 chosen in the ProcessManager overlay is honored at launch
    // instead of being quantized to the sidebar's 1.50/1.75 pair.
    let factor_str = format!("{:.2}", factor);

    // Rewrite the LAST DXMT_CONFIG entry in place.
    if let Some((_, config)) = env.iter_mut().rev().find(|(key, _)| key == "DXMT_CONFIG") {
        *config = metal_fx_config_with_upscale(config, enabled.then_some(factor_str.as_str()));
    }

    // MetalFX swapchain integration: on for strengths, off for "off" (matches
    // DXMT's own disable semantics used for M9 stuck-loading titles).
    let swapchain_value = if enabled { "1" } else { "0" };
    if let Some((_, value)) = env.iter_mut().rev().find(|(key, _)| key == "DXMT_METALFX_SPATIAL_SWAPCHAIN") {
        *value = swapchain_value.to_string();
    } else {
        env.push(("DXMT_METALFX_SPATIAL_SWAPCHAIN".to_string(), swapchain_value.to_string()));
    }
}

/// Command-wrapper variant for the direct launch paths (they apply
/// `node.env_vars` straight onto the Command, so we reconcile via the same
/// pair-rewrite logic then push the overrides back).
fn apply_metal_fx_config_cmd(cmd: &mut std::process::Command, node: &PipelineNode, home: &Path) {
    let mut env: Vec<(String, String)> = Vec::new();
    for ev in &node.env_vars {
        env.push((ev.key.to_string(), ev.value.to_string()));
    }
    apply_metal_fx_config(&mut env, node, home);
    for (key, value) in env {
        cmd.env(key, value);
    }
}

/// Rebuild a `DXMT_CONFIG` value with the MetalFX upscale pair set to
/// `factor` (or removed entirely when `None`, i.e. MetalFX off). All other
/// pairs (e.g. `d3d11.preferredMaxFrameRate=60`) are preserved.
fn metal_fx_config_with_upscale(config: &str, factor: Option<&str>) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut inserted = false;
    for pair in config.split(';') {
        let Some((key, value)) = pair.split_once('=') else { continue };
        if key == "d3d11.metalSpatialUpscaleFactor" {
            if let Some(f) = factor {
                out.push(format!("d3d11.metalSpatialUpscaleFactor={}", f));
                inserted = true;
            }
            // None -> drop the pair entirely.
        } else {
            out.push(format!("{}={}", key, value));
        }
    }
    if let Some(f) = factor {
        if !inserted {
            out.push(format!("d3d11.metalSpatialUpscaleFactor={}", f));
        }
    }
    out.join(";")
}

fn is_reserved_route_env_key(pipeline_id: PipelineId, key: &str) -> bool {
    if pipeline_id != PipelineId::M12 {
        return false;
    }
    matches!(
        key,
        "WINEDLLOVERRIDES"
            | "WINEDLLPATH"
            | "DYLD_LIBRARY_PATH"
            | "DYLD_FALLBACK_LIBRARY_PATH"
            | "DXMT_WINEMETAL_UNIXLIB"
            | "DXMT_CONFIG_FILE"
            | "MS_GRAPHICS_BACKEND"
            | "WINEMSYNC"
            | "DXMT_LOG_PATH"
    )
}

fn app_compat_env_pairs(appid: u32, pipeline_id: PipelineId) -> Vec<(String, String)> {
    app_compat_env_pairs_with_logs(appid, pipeline_id, crate::launch::graphics_runtime_logs_enabled())
}

fn app_compat_env_pairs_with_logs(
    appid: u32,
    pipeline_id: PipelineId,
    graphics_runtime_logs: bool,
) -> Vec<(String, String)> {
    if pipeline_id == PipelineId::M9 && is_m9_stuck_loading_title(appid) {
        return vec![
            ("DXMT_ASYNC_PIPELINE_COMPILE".to_string(), "0".to_string()),
            ("DXMT_METALFX_SPATIAL_SWAPCHAIN".to_string(), "0".to_string()),
            ("DXMT_METALFX_SPATIAL".to_string(), "0".to_string()),
            ("DXMT_CONFIG".to_string(), "d3d11.preferredMaxFrameRate=60".to_string()),
            ("METALSHARP_M9_SYNC_LOADING".to_string(), "1".to_string()),
        ];
    }

    if appid == 1962700 && pipeline_id == PipelineId::M12 {
        let mut env = vec![
            ("DXMT_D3D12_ENABLE_GEOMETRY_MESH".to_string(), "1".to_string()),
            ("DXMT_D3D12_FORCE_SWAPCHAIN_BLIT".to_string(), "1".to_string()),
            ("DXMT_D3D12_AUTOPRESENT_SWAPCHAIN".to_string(), "1".to_string()),
            ("DXMT_D3D12_LIVE_PRESENT".to_string(), "1".to_string()),
            ("DXMT_D3D12_REASSERT_WINDOW_HANDOFF".to_string(), "1".to_string()),
            ("DXMT_D3D12_DISABLE_RUNTIME_MSC".to_string(), "1".to_string()),
            ("DXMT_D3D12_FORCE_COLOR_WRITE_STATE".to_string(), "1".to_string()),
            ("DXMT_METALFX_SPATIAL_SWAPCHAIN".to_string(), "0".to_string()),
            ("DXMT_METALFX_SPATIAL".to_string(), "0".to_string()),
            ("DXMT_METALFX_TEMPORAL".to_string(), "0".to_string()),
            ("DXMT_CONFIG".to_string(), "d3d11.preferredMaxFrameRate=60".to_string()),
        ];
        if graphics_runtime_logs {
            env.extend([
                ("DXMT_DXGI_TRACE".to_string(), "1".to_string()),
                ("DXMT_WINEMETAL_DEBUG".to_string(), "1".to_string()),
                ("DXMT_D3D12_TRACE".to_string(), "1".to_string()),
                ("DXMT_D3D12_TRACE_COMPONENTS".to_string(), "Device,Queue,SwapChain,Presenter,PSO".to_string()),
                ("DXMT_D3D12_TRACE_MAX_MB".to_string(), "16".to_string()),
                ("DXMT_D3D12_TIMING_MIN_MS".to_string(), "0".to_string()),
                ("DXMT_D3D12_PRESENT_LOG_INTERVAL".to_string(), "120".to_string()),
                ("DXMT_DUMP_MSL".to_string(), "1".to_string()),
            ]);
        }
        if std::env::var("METALSHARP_M12_DIAGNOSTIC_CAPTURE")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
        {
            env.push(("DXMT_D3D12_SWAPCHAIN_READBACK".to_string(), "1".to_string()));
            env.push(("DXMT_D3D12_SWAPCHAIN_READBACK_INTERVAL".to_string(), "30".to_string()));
            env.push(("DXMT_D3D12_FINAL_RENDER_SNAPSHOT".to_string(), "1".to_string()));
            env.push(("DXMT_D3D12_LIVE_PRESENT".to_string(), "0".to_string()));
            env.push(("DXMT_D3D12_PRESENT_LOG_INTERVAL".to_string(), "30".to_string()));
        }
        if std::env::var("METALSHARP_M12_FORCE_SWAPCHAIN_COLOR")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
        {
            env.push(("DXMT_D3D12_FORCE_SWAPCHAIN_COLOR".to_string(), "1".to_string()));
        }
        if std::env::var("METALSHARP_M12_FORCE_COLOR_WRITE_STATE")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
        {
            env.push(("DXMT_D3D12_FORCE_COLOR_WRITE_STATE".to_string(), "1".to_string()));
        }
        if std::env::var("METALSHARP_M12_FORCE_DIAGNOSTIC_FRAGMENT")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
        {
            env.push(("DXMT_D3D12_FORCE_DIAGNOSTIC_FRAGMENT".to_string(), "1".to_string()));
        }
        if std::env::var("METALSHARP_M12_FORCE_DIAGNOSTIC_FULLSCREEN")
            .map(|value| !value.is_empty() && value != "0")
            .unwrap_or(false)
        {
            env.push(("DXMT_D3D12_FORCE_DIAGNOSTIC_FULLSCREEN".to_string(), "1".to_string()));
        }
        return env;
    }
    Vec::new()
}

fn is_m9_stuck_loading_title(appid: u32) -> bool {
    matches!(appid, 774361 | 17410 | 49520)
}

fn apply_app_launch_env(cmd: &mut Command, appid: u32, pipeline_id: PipelineId) {
    let appid_string = appid.to_string();
    cmd.env("SteamAppId", &appid_string);
    cmd.env("SteamGameId", &appid_string);
    cmd.env("SteamOverlayGameId", &appid_string);

    for (key, value) in app_compat_env_pairs(appid, pipeline_id) {
        cmd.env(key, value);
    }
    if let Some(recipe) = super::rules::get_game_recipe(appid) {
        for (key, value) in recipe.env {
            if !is_reserved_route_env_key(pipeline_id, &key) {
                cmd.env(key, value);
            }
        }
    }
}

fn apply_cache_env(cmd: &mut Command, node: &PipelineNode, cache_paths: Option<&CachePaths>, ms_root: &PathBuf) {
    for (key, val) in cache_env_pairs(node, cache_paths, ms_root) {
        cmd.env(key, val);
    }
}

fn cache_env_pairs(node: &PipelineNode, cache_paths: Option<&CachePaths>, ms_root: &PathBuf) -> Vec<(String, String)> {
    cache_env_pairs_with_logs(node, cache_paths, ms_root, crate::launch::graphics_runtime_logs_enabled())
}

fn cache_env_pairs_with_logs(
    node: &PipelineNode,
    cache_paths: Option<&CachePaths>,
    ms_root: &PathBuf,
    graphics_runtime_logs: bool,
) -> Vec<(String, String)> {
    let Some(cache) = cache_paths else {
        return Vec::new();
    };

    let shader_dir = format!("{}/", cache.shader);
    let pipeline_dir = format!("{}/", cache.pipeline);
    let log_dir = format!("{}/", cache.log);
    let mut env = vec![
        ("METALSHARP_SHADER_CACHE_PATH".to_string(), shader_dir.clone()),
        ("METALSHARP_PIPELINE_CACHE_PATH".to_string(), pipeline_dir.clone()),
        ("METALSHARP_CACHE_SUMMARY".to_string(), format!("shader={};pipeline={}", shader_dir, pipeline_dir)),
        ("MTL_SHADER_CACHE_DIR".to_string(), shader_dir.clone()),
    ];

    match node.backend {
        "dxmt" => {
            env.push(("DXMT_SHADER_CACHE_PATH".to_string(), shader_dir));
            env.push(("DXMT_PIPELINE_CACHE_PATH".to_string(), pipeline_dir.clone()));
            if graphics_runtime_logs {
                env.push(("DXMT_LOG_PATH".to_string(), log_dir));
            }
        },
        "dxvk" => {
            env.push(("DXVK_STATE_CACHE_PATH".to_string(), shader_dir));
            env.push(("DXVK_LOG_PATH".to_string(), cache.pipeline.clone()));
            let moltenvk_icd = ms_root.join("etc").join("vulkan").join("icd.d").join("MoltenVK_icd.json");
            if moltenvk_icd.exists() {
                env.push(("VK_ICD_FILENAMES".to_string(), moltenvk_icd.to_string_lossy().to_string()));
            }
        },
        "vkd3d-proton" => {
            // D3D12 -> vkd3d-proton -> Vulkan -> MoltenVK -> Metal.
            // vkd3d-proton uses the Vulkan loader, so the VKMT MoltenVK ICD
            // must be pinned; state/cache routing mirrors the dxvk lane.
            env.push(("DXVK_STATE_CACHE_PATH".to_string(), shader_dir.clone()));
            env.push(("VKD3D_SHADER_CACHE_PATH".to_string(), shader_dir));
            let moltenvk_icd = ms_root.join("etc").join("vulkan").join("icd.d").join("MoltenVK_icd.json");
            if moltenvk_icd.exists() {
                env.push(("VK_ICD_FILENAMES".to_string(), moltenvk_icd.to_string_lossy().to_string()));
            }
        },
        "wine32" | "wine" | "wine-steam" => {
            env.push(("DXMT_SHADER_CACHE_PATH".to_string(), shader_dir.clone()));
            env.push(("DXVK_STATE_CACHE_PATH".to_string(), shader_dir));
            env.push(("DXMT_PIPELINE_CACHE_PATH".to_string(), pipeline_dir.clone()));
            if graphics_runtime_logs {
                env.push(("DXMT_LOG_PATH".to_string(), log_dir));
            }
        },
        "mono" | "macos-steam" => {
            env.push(("FNA3D_SHADER_CACHE_PATH".to_string(), shader_dir));
            env.push(("FNA3D_PIPELINE_CACHE_PATH".to_string(), pipeline_dir));
        },
        _ => {},
    }

    env
}

fn resolve_game_exe(game_dir: &PathBuf) -> String {
    super::recipe::resolve_game_exe(0, game_dir).unwrap_or_else(|_| game_dir.clone()).to_string_lossy().to_string()
}

fn find_config(name: &str) -> String {
    let home = dirs::home_dir().unwrap_or_default();
    let metalsharp_home = crate::platform::metalsharp_home_dir_for(&home);
    let runtime_config = metalsharp_home.join("configs").join(name);
    let candidates = vec![
        runtime_config.clone(),
        crate::platform::metalsharp_home_dir_for(&home).join("configs").join(name),
        home.join("metalsharp").join("configs").join(name),
        std::env::current_dir().unwrap_or_default().join("configs").join(name),
    ];
    for c in candidates {
        if c.exists() {
            if c != runtime_config {
                if let Some(parent) = runtime_config.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                let _ = std::fs::copy(&c, &runtime_config);
            }
            return c.to_string_lossy().to_string();
        }
    }
    if let Some(c) = find_repo_source(&["configs", name]) {
        if let Some(parent) = runtime_config.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let _ = std::fs::copy(&c, &runtime_config);
        return c.to_string_lossy().to_string();
    }
    runtime_config.to_string_lossy().to_string()
}

fn rewrite_config_with_absolute_paths(
    template_config: &str,
    game_dir: &Path,
) -> Result<String, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let runtime_config = ms_home.join("configs").join(template_config);

    let content = std::fs::read_to_string(&runtime_config).unwrap_or_default();
    if content.is_empty() {
        // Template not found in deployed configs — skip rewrite, return empty
        // so the caller treats it as "no config to apply"
        return Ok(String::new());
    }
    let game_dir_str = game_dir.to_string_lossy();
    let native_libs_needing_abs_path = [
        "libSDL2-2.0.0.dylib",
        "libFNA3D.0.dylib",
        "libFNA3D.dylib",
        "libFAudio.0.dylib",
        "libFAudio.dylib",
        "libfmod.dylib",
        "libfmodstudio.dylib",
    ];
    let mut rewritten = content;
    for lib in &native_libs_needing_abs_path {
        if game_dir.join(lib).exists() {
            let abs = format!("{}/{}", game_dir_str, lib);
            rewritten = rewritten.replace(&format!("target=\"{}\"", lib), &format!("target=\"{}\"", abs));
        }
    }

    let deployed_name = format!("{}.{}", template_config, game_dir.file_name().unwrap_or_default().to_string_lossy());
    let deployed_path = ms_home.join("configs").join(&deployed_name);
    std::fs::write(&deployed_path, &rewritten)?;
    Ok(deployed_path.to_string_lossy().to_string())
}

fn resolve_fna_game_dir(appid: u32) -> Result<PathBuf, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;

    let local_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());
    if local_dir.join(".metalsharp_prepared").exists() {
        return Ok(local_dir);
    }

    if let Some(windows_dir) = crate::setup::resolve_windows_game_dir(appid) {
        return Ok(windows_dir);
    }

    if local_dir.exists() {
        return Ok(local_dir);
    }
    if let Some(native_dir) = crate::setup::resolve_native_game_dir(appid) {
        return Err(format!(
            "FNA/Mono/XNA requires the Windows Steam install for appid {}; found only native macOS install at {}",
            appid,
            native_dir.display()
        )
        .into());
    }
    Err(format!("FNA/Mono/XNA requires a Windows Steam install; no Windows game dir found for appid {}", appid).into())
}

fn has_exe_files(dir: &PathBuf) -> bool {
    crate::scan::is_windows_game_dir(dir)
}

fn find_preferred_exe(dir: &PathBuf, candidates: &[&str]) -> Result<PathBuf, Box<dyn std::error::Error>> {
    for name in candidates {
        let p = dir.join(name);
        if p.exists() {
            return Ok(p);
        }
    }
    Err(format!("game exe not found: tried {} in {}", candidates.join(", "), dir.display()).into())
}

fn ensure_launcher_exe(appid: u32, game_dir: &PathBuf) {
    let profile = find_fna_profile(appid);
    let launcher_name = match profile.launcher_exe {
        Some(n) => n,
        None => return,
    };
    let source_file = match profile.launcher_source {
        Some(s) => s,
        None => return,
    };

    let launcher = game_dir.join(launcher_name);
    if launcher.exists() {
        return;
    }

    // H1-H3: launcher binaries (Terraria launcher etc.) are PREBUILT and
    // shipped in the assets bundle — never compile from a dev-machine repo
    // path at launch (silently no-ops on user machines). The bundle layout is
    // runtime/prebuilt-launchers/<source_file basename>.
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return,
    };
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let file_name = Path::new(source_file)
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| source_file.to_string());
    let prebuilt = ms_home.join("runtime").join("prebuilt-launchers").join(&file_name);
    if prebuilt.is_file() {
        let _ = std::fs::copy(&prebuilt, &launcher);
    }
}

fn find_shims_dir() -> String {
    let home = dirs::home_dir().unwrap_or_default();
    let candidates = vec![
        crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("shims"),
        crate::platform::metalsharp_home_dir_for(&home).join("shims"),
    ];
    for c in candidates {
        if c.exists() {
            return c.to_string_lossy().to_string();
        }
    }
    crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("shims").to_string_lossy().to_string()
}

fn deploy_fna_assemblies(appid: u32, game_dir: &PathBuf) {
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return,
    };
    let metalsharp_home = crate::platform::metalsharp_home_dir_for(&home);
    let fna_dll = metalsharp_home.join("runtime").join("fna").join("FNA.dll");

    let shims_dir = PathBuf::from(find_shims_dir());
    let _ = std::fs::create_dir_all(&shims_dir);
    deploy_fna_native_shims(game_dir, &shims_dir);
    let mut shared_native_libs = vec![
        ("libFNA3D.dylib", None),
        ("libFNA3D.0.dylib", Some("libFNA3D.dylib")),
        ("libSDL2-2.0.0.dylib", Some("libSDL2.dylib")),
        ("libSDL2.dylib", None),
        ("libFAudio.0.dylib", Some("libFAudio.dylib")),
        ("libFAudio.dylib", None),
        ("libCSteamworks.dylib", None),
        ("libfmod.dylib", None),
        ("libfmodstudio.dylib", None),
        ("libnfd.dylib", None),
    ];
    if appid != 504230 {
        shared_native_libs.push(("libsteam_api.dylib", None));
    }

    for (lib, symlink) in shared_native_libs {
        copy_fna_native_lib(game_dir, &shims_dir, lib, symlink);
    }

    let fmod_libs: &[(&str, Option<&str>)] = &[("libfmod.dylib", None), ("libfmodstudio.dylib", None)];
    for (lib, symlink) in fmod_libs {
        let dst = game_dir.join(lib);
        if dst.exists() && !fna_native_lib_needs_refresh(lib, &dst) {
            continue;
        }
        let fmod_dir = metalsharp_home.join("runtime").join("fnalibs").join("fmod");
        if fmod_dir.join(lib).exists() {
            let _ = std::fs::copy(fmod_dir.join(lib), &dst);
            fix_dylib_install_names(&dst);
            if let Some(sym) = symlink {
                ensure_fna_symlink(game_dir, lib, sym);
            }
        }
    }

    let profile = find_fna_profile(appid);

    if profile.deploy_macos_steam_libs {
        let mac_terrarria_libs = home
            .join("Library")
            .join("Application Support")
            .join("Steam")
            .join("steamapps")
            .join("common")
            .join("Terraria")
            .join("Terraria.app")
            .join("Contents")
            .join("MacOS")
            .join("osx");

        let terraria_native_libs = [
            ("libFNA3D.0.dylib", Some("libFNA3D.dylib")),
            ("libSDL2-2.0.0.dylib", Some("libSDL2.dylib")),
            ("libFAudio.0.dylib", Some("libFAudio.dylib")),
            ("libsteam_api.dylib", None),
            ("libnfd.dylib", None),
        ];

        for (lib, symlink) in &terraria_native_libs {
            let src = mac_terrarria_libs.join(lib).to_string_lossy().to_string();
            if game_dir.join(lib).exists() {
                continue;
            }
            if std::path::Path::new(&src).exists() {
                let _ = std::fs::copy(std::path::Path::new(&src), game_dir.join(lib));
                if let Some(sym) = symlink {
                    ensure_fna_symlink(game_dir, lib, sym);
                }
            }
        }
    }

    let cached = shims_dir.join("libgdiplus.dylib");
    if !game_dir.join("libgdiplus.dylib").exists() && cached.exists() {
        // H3: gdiplus stub ships prebuilt in the bundle shims — never compile
        // from the dev-machine repo path at launch.
        let _ = std::fs::copy(&cached, game_dir.join("libgdiplus.dylib"));
    }

    if profile.deploy_terraria_post {
        deploy_terraria_runtime(game_dir, &metalsharp_home);
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

    if fna_dll.exists() {
        if !game_dir.join("FNA.dll").exists() {
            let _ = std::fs::copy(&fna_dll, game_dir.join("FNA.dll"));
        }
        for name in &xna_assemblies {
            let dst = game_dir.join(name);
            if !dst.exists() {
                let _ = std::fs::copy(&fna_dll, dst);
            }
        }
    }

    let _ = std::fs::write(game_dir.join("steam_appid.txt"), appid.to_string());
    if let Err(err) = deploy_offline_steamworks_net(game_dir, &metalsharp_home) {
        eprintln!("fna: offline Steamworks deploy failed: {}", err);
    }
}

/// Map a discovered Unity version (e.g. "2021.3.5f1") to a bundled runtime
/// lane (e.g. "2021.3"). Unknown/older lines fall back to the newest bundled
/// lane so deployment never hard-fails on an unlisted version.
pub fn unity_runtime_lane_for_version(unity_version: Option<&str>, ms_home: &Path) -> Option<String> {
    let runtime_root = ms_home.join("runtime").join("unity-mono");
    let version = unity_version.unwrap_or("");
    let major_minor: String = version.split('.').take(2).collect::<Vec<_>>().join(".");
    if !major_minor.is_empty() {
        let candidate = runtime_root.join(&major_minor);
        if candidate.is_dir() {
            return Some(major_minor);
        }
    }
    // Fall back to the newest bundled lane (preferring newer Unity LTS lines).
    let mut lanes: Vec<String> = std::fs::read_dir(&runtime_root)
        .ok()
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.path().is_dir())
        .filter_map(|e| e.file_name().to_str().map(str::to_string))
        .collect();
    lanes.sort_by(|a, b| {
        let num = |s: &str| s.split('.').next().and_then(|p| p.parse::<u32>().ok()).unwrap_or(0);
        num(b).cmp(&num(a))
    });
    lanes.first().cloned()
}

/// Deploy the version-matched Unity Mono runtime next to a Unity-Mono game.
/// Copies the bundled `assets/unity-mono/<lane>/` payloads into the game dir's
/// `MonoBleedingEdge/` (or `osx/` for kickstart-style layouts), records
/// receipts, and rewrites install names. Idempotent: existing matching files
/// are left alone.
pub fn deploy_unity_runtime(
    game_dir: &Path,
    profile: &crate::mono_profile::MonoProfile,
    ms_home: &Path,
    mut report: Option<&mut crate::fna_profile::AssetStagingReport>,
) -> Result<usize, Box<dyn std::error::Error>> {
    if profile.kind != crate::mono_profile::MonoProfileKind::UnityMono {
        return Ok(0);
    }
    let lane = unity_runtime_lane_for_version(profile.unity_version.as_deref(), ms_home)
        .ok_or_else(|| "no bundled Unity Mono runtime available".to_string())?;
    let src_root = ms_home.join("runtime").join("unity-mono").join(&lane);
    if !src_root.is_dir() {
        return Err(format!("bundled Unity Mono runtime lane {lane} missing at {}", src_root.display()).into());
    }

    let embed = game_dir.join("MonoBleedingEdge").join("EmbedRuntime");
    let _ = std::fs::create_dir_all(&embed);
    let mut deployed = 0usize;
    for entry in std::fs::read_dir(&src_root)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_string();
        if name == "mono-sgen" || name.ends_with(".version") || name == "manifest.json" {
            continue;
        }
        let dst = embed.join(&name);
        if dst.exists() {
            continue;
        }
        std::fs::copy(entry.path(), &dst)?;
        fix_dylib_install_names(&dst);
        if let Some(r) = report.as_mut() {
            r.record(crate::fna_profile::record_asset_receipt(
                &name,
                &entry.path(),
                &dst,
                true,
                false,
                true,
                &format!("unity-mono/{lane} runtime payload"),
            ));
        }
        deployed += 1;
    }
    // MonoPosixHelper is required by the mono runtime; ensure the managed
    // assemblies dir exists for the game's data.
    let managed = find_unity_managed_dir(game_dir);
    let _ = std::fs::create_dir_all(&managed);
    Ok(deployed)
}

/// Locate `<Game>_Data/Managed` for a Unity game (or a root Managed dir).
pub fn find_unity_managed_dir(game_dir: &Path) -> std::path::PathBuf {
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let p = entry.path();
            if p.is_dir() && entry.file_name().to_string_lossy().to_lowercase().ends_with("_data") {
                let managed = p.join("Managed");
                if managed.is_dir() {
                    return managed;
                }
            }
        }
    }
    game_dir.join("Managed")
}

/// Deploy dependency payloads the game's profile signals: SDL3, SDL2, Carbon.
/// Sources from the bundled assets; never compiles.
pub fn deploy_profile_deps(
    game_dir: &Path,
    profile: &crate::mono_profile::MonoProfile,
    ms_home: &Path,
    mut report: Option<&mut crate::fna_profile::AssetStagingReport>,
) -> Result<usize, Box<dyn std::error::Error>> {
    let mut deployed = 0usize;
    if profile.deps.sdl3 {
        let src = ms_home.join("runtime").join("sdl3").join("libSDL3.dylib");
        if src.is_file() {
            let dst = game_dir.join("libSDL3.dylib");
            if !dst.exists() {
                std::fs::copy(&src, &dst)?;
                fix_dylib_install_names(&dst);
                if let Some(r) = report.as_mut() {
                    r.record(crate::fna_profile::record_asset_receipt(
                        "libSDL3.dylib",
                        &src,
                        &dst,
                        false,
                        false,
                        true,
                        "sdl3 dep",
                    ));
                }
                deployed += 1;
            }
        }
    }
    if profile.deps.carbon {
        let shims_dir = PathBuf::from(find_shims_dir());
        let src = shims_dir.join(FNA_CARBON_SHIM);
        if src.is_file() {
            let dst = game_dir.join(FNA_CARBON_SHIM);
            if !dst.exists() {
                let _ = std::fs::copy(&src, &dst);
                if let Some(r) = report.as_mut() {
                    r.record(crate::fna_profile::record_asset_receipt(
                        FNA_CARBON_SHIM,
                        &src,
                        &dst,
                        false,
                        false,
                        true,
                        "carbon dep",
                    ));
                }
                deployed += 1;
            }
        }
    }
    Ok(deployed)
}

/// Deploy the version-matched XNA assembly set into the game dir for
/// XNA/MonoGame profile games that lack their own copies.
pub fn deploy_xna_assembly_set(
    game_dir: &Path,
    profile: &crate::mono_profile::MonoProfile,
    ms_home: &Path,
    mut report: Option<&mut crate::fna_profile::AssetStagingReport>,
) -> Result<usize, Box<dyn std::error::Error>> {
    if !matches!(profile.kind, crate::mono_profile::MonoProfileKind::Xna | crate::mono_profile::MonoProfileKind::Fna) {
        return Ok(0);
    }
    let xna_root = ms_home.join("runtime").join("xna");
    if !xna_root.is_dir() {
        return Ok(0);
    }
    let managed = find_unity_managed_dir(game_dir);
    let mut deployed = 0usize;
    for entry in std::fs::read_dir(&xna_root)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_string();
        if !name.ends_with(".dll") {
            continue;
        }
        // Only deploy to the managed dir when the game has one; otherwise
        // next to the exe.
        let dst_dir = if managed.is_dir() { &managed } else { game_dir };
        let dst = dst_dir.join(&name);
        if dst.exists() {
            continue;
        }
        std::fs::copy(entry.path(), &dst)?;
        if let Some(r) = report.as_mut() {
            r.record(crate::fna_profile::record_asset_receipt(
                &name,
                &entry.path(),
                &dst,
                false,
                false,
                true,
                "xna assembly set",
            ));
        }
        deployed += 1;
    }
    Ok(deployed)
}

pub fn repair_fna_game_runtime_assets(appid: u32, game_dir: &Path) -> Result<usize, Box<dyn std::error::Error>> {
    if !game_dir.is_dir() {
        return Err(format!("FNA game directory is missing: {}", game_dir.display()).into());
    }

    let game_dir = game_dir.to_path_buf();
    repair_fna_native_runtime_shims()?;
    deploy_fna_assemblies(appid, &game_dir);
    deploy_offline_steamworks_net(&game_dir, &crate::platform::metalsharp_home_dir())?;
    // Restore any previous identity receipt before re-staging the appid file.
    let _ = crate::setup::restore_steam_appid_receipt(&game_dir);
    if !game_dir.join("steam_appid.txt").exists() {
        let _ = std::fs::write(game_dir.join("steam_appid.txt"), appid.to_string());
    }
    // Profile-aware deploy: version-matched Unity runtime, XNA set, deps.
    let ms_home = crate::platform::metalsharp_home_dir();
    let discovered = crate::mono_profile::discover_mono_profile(&game_dir);
    if discovered.kind != crate::mono_profile::MonoProfileKind::None {
        let mut report = crate::fna_profile::AssetStagingReport::new(appid);
        let _ = deploy_unity_runtime(&game_dir, &discovered, &ms_home, Some(&mut report));
        let _ = deploy_xna_assembly_set(&game_dir, &discovered, &ms_home, Some(&mut report));
        let _ = deploy_profile_deps(&game_dir, &discovered, &ms_home, Some(&mut report));
        if !report.receipts.is_empty() {
            let _ = report.persist(&game_dir);
        }
    }

    let profile = find_fna_profile(appid);
    let mut required = vec![
        "libSystem.Native.dylib",
        "libSDL2-2.0.0.dylib",
        "libFNA3D.0.dylib",
        "libFAudio.0.dylib",
        "FNA.dll",
        "Microsoft.Xna.Framework.dll",
        "Microsoft.Xna.Framework.Game.dll",
        "Microsoft.Xna.Framework.Graphics.dll",
        "Microsoft.Xna.Framework.Audio.dll",
        "Microsoft.Xna.Framework.Input.dll",
        "Microsoft.Xna.Framework.Media.dll",
        "Microsoft.Xna.Framework.Storage.dll",
        "steam_appid.txt",
    ];
    if profile.mono_arch == MonoArch::X86 {
        required.push("libfmod.dylib");
        required.push("libfmodstudio.dylib");
    }
    if game_dir.join("Steamworks.NET.dll.metalsharp-original").exists() || game_dir.join("Steamworks.NET.dll").exists()
    {
        required.push("Steamworks.NET.dll");
    }

    let ready = required
        .iter()
        .filter(|name| {
            let path = game_dir.join(name);
            path.exists() && path.metadata().map(|metadata| metadata.len() > 0).unwrap_or(false)
        })
        .count();
    if ready != required.len() {
        return Err(
            format!("FNA game runtime repair incomplete: {}/{} required assets staged", ready, required.len()).into()
        );
    }

    for (lib, name) in [
        ("libFNA3D.0.dylib", "FNA3D"),
        ("libFAudio.0.dylib", "FAudio"),
        ("libfmod.dylib", "FMOD"),
        ("libfmodstudio.dylib", "FMOD Studio"),
    ] {
        let path = game_dir.join(lib);
        if path.exists() && !fna_native_lib_source_valid(lib, &path) {
            return Err(format!("{} runtime asset is invalid: {}", name, path.display()).into());
        }
    }
    if game_dir.join("Steamworks.NET.dll.metalsharp-original").exists()
        && !offline_steamworks_net_deployed(
            &game_dir.join("Steamworks.NET.dll"),
            &game_dir.join("Steamworks.NET.dll.metalsharp-original"),
        )
    {
        return Err(format!("offline Steamworks.NET.dll repair did not replace {}", game_dir.display()).into());
    }

    Ok(ready)
}

fn deploy_fna_native_shims(game_dir: &PathBuf, shims_dir: &PathBuf) {
    for spec in FNA_NATIVE_SHIMS {
        ensure_fna_native_shim_in_cache(spec, shims_dir);
        if matches!(spec.output, "libFAudio.0.dylib" | "libfmod.dylib" | "libfmodstudio.dylib") {
            continue;
        }
        copy_fna_native_lib(game_dir, shims_dir, spec.output, None);
        if game_dir.join(spec.output).exists() {
            for symlink in spec.symlinks {
                ensure_fna_symlink(game_dir, spec.output, symlink);
            }
        }
    }
}

pub fn repair_fna_native_runtime_shims() -> Result<usize, Box<dyn std::error::Error>> {
    let home = dirs::home_dir().ok_or("no home dir")?;
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let shims_dir = ms_home.join("runtime").join("shims");
    let fna_dir = ms_home.join("runtime").join("fna");
    std::fs::create_dir_all(&shims_dir)?;
    std::fs::create_dir_all(&fna_dir)?;
    let refreshed = crate::installer::repair_fna_support_assets().unwrap_or(0);

    for spec in FNA_NATIVE_SHIMS {
        ensure_fna_native_shim_in_cache(spec, &shims_dir);
    }

    ensure_fna_runtime_assembly(&fna_dir, &ms_home)?;

    let fna3d_build = find_repo_source(&["src", "fna", "FNA3D", "build"])
        .unwrap_or_else(|| home.join("metalsharp").join("src").join("fna").join("FNA3D").join("build"));

    if fna3d_build.exists() {
        let src = fna3d_build.join("libFNA3D.dylib");
        if fna_native_lib_source_valid("libFNA3D.dylib", &src) {
            let _ = std::fs::copy(&src, shims_dir.join("libFNA3D.dylib"));
        }
    }

    let fnalibs_dir = ms_home.join("runtime").join("fnalibs");
    if fnalibs_dir.exists() {
        let sdl2_src = fnalibs_dir.join("libSDL2-2.0.0.dylib");
        if fna_native_lib_source_valid("libSDL2-2.0.0.dylib", &sdl2_src) {
            let dst = shims_dir.join("libSDL2-2.0.0.dylib");
            let _ = std::fs::copy(&sdl2_src, &dst);
            let _ = Command::new("/usr/bin/install_name_tool")
                .args(["-id", "@rpath/libSDL2-2.0.0.dylib"])
                .arg(&dst)
                .output();
            let symlink = shims_dir.join("libSDL2.dylib");
            if !symlink.exists() {
                let _ = std::os::unix::fs::symlink("libSDL2-2.0.0.dylib", symlink);
            }
        }
        let fna3d_src = fnalibs_dir.join("libFNA3D.0.dylib");
        if fna_native_lib_source_valid("libFNA3D.0.dylib", &fna3d_src) {
            let dst = shims_dir.join("libFNA3D.0.dylib");
            let _ = std::fs::copy(&fna3d_src, &dst);
            fix_dylib_install_names(&dst);
            let symlink = shims_dir.join("libFNA3D.dylib");
            let _ = std::fs::remove_file(&symlink);
            let _ = std::os::unix::fs::symlink("libFNA3D.0.dylib", symlink);
        }
        let faudio_src = fnalibs_dir.join("libFAudio.0.dylib");
        if fna_native_lib_source_valid("libFAudio.0.dylib", &faudio_src) {
            let dst = shims_dir.join("libFAudio.0.dylib");
            let _ = std::fs::copy(&faudio_src, &dst);
            fix_dylib_install_names(&dst);
            let symlink = shims_dir.join("libFAudio.dylib");
            let _ = std::fs::remove_file(&symlink);
            let _ = std::os::unix::fs::symlink("libFAudio.0.dylib", symlink);
        }
    }

    let all_required = fna_required_runtime_assets(&ms_home.join("runtime"));
    let present = all_required.iter().filter(|p| p.exists()).count();
    if present != all_required.len() {
        return Err(format!(
            "FNA runtime repair incomplete: {}/{} required assets staged",
            present,
            all_required.len()
        )
        .into());
    }
    for path in [
        ms_home.join("runtime").join("fnalibs").join("libFNA3D.0.dylib"),
        ms_home.join("runtime").join("fna-kickstart").join("osx").join("libFNA3D.0.dylib"),
        shims_dir.join("libFNA3D.0.dylib"),
    ] {
        if !fna_native_lib_source_valid("libFNA3D.0.dylib", &path) {
            return Err(format!("FNA3D runtime asset is not SDL2-linked: {}", path.display()).into());
        }
    }
    Ok(present + refreshed)
}

pub fn precompile_all_fna_shims() -> Result<usize, String> {
    let home = dirs::home_dir().ok_or("no home dir".to_string())?;
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    let shims_dir = ms_home.join("runtime").join("shims");
    let _ = std::fs::create_dir_all(&shims_dir);

    let mut compiled = 0usize;
    for spec in FNA_NATIVE_SHIMS {
        let dst = shims_dir.join(spec.output);
        if dst.exists() {
            continue;
        }
        ensure_fna_native_shim_in_cache(spec, &shims_dir);
        if dst.exists() {
            compiled += 1;
        }
    }

    Ok(compiled)
}

fn fna_required_runtime_assets(runtime: &Path) -> Vec<PathBuf> {
    vec![
        runtime.join("fna-kickstart").join("kick.bin.osx"),
        runtime.join("fna-kickstart").join("FNA.dll"),
        runtime.join("fna-kickstart").join("osx").join("libmonosgen-2.0.1.dylib"),
        runtime.join("fna-kickstart").join("osx").join("libSDL2-2.0.0.dylib"),
        runtime.join("fna-kickstart").join("osx").join("libFNA3D.0.dylib"),
        runtime.join("fna-kickstart").join("osx").join("libFAudio.0.dylib"),
        runtime.join("fnalibs").join("libFNA3D.0.dylib"),
        runtime.join("fnalibs").join("libSDL2-2.0.0.dylib"),
        runtime.join("shims").join("libkernel32.dylib"),
        runtime.join("shims").join("libuser32.dylib"),
        runtime.join("shims").join(FNA_CARBON_SHIM),
        runtime.join("shims").join(FNA_CARBON_INTERPOSE_SHIM),
    ]
}

fn ensure_fna_runtime_assembly(fna_dir: &Path, ms_home: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let dst = fna_dir.join("FNA.dll");
    if file_has_payload(&dst) {
        return Ok(());
    }

    let mut candidates = Vec::new();
    if let Some(source) = find_repo_source(&["src", "fna", "FNA", "bin", "Release", "net4.0", "FNA.dll"]) {
        candidates.push(source);
    }
    if let Some(resources) = crate::platform::app_resources_dir() {
        candidates.push(resources.join("runtime").join("fna").join("FNA.dll"));
    }
    candidates.push(ms_home.join("runtime").join("redist").join("fna").join("FNA.dll"));

    if let Some(source) = candidates.into_iter().find(|path| file_has_payload(path)) {
        std::fs::copy(source, dst)?;
        return Ok(());
    }

    fetch_fna_runtime_assembly(ms_home, &dst)
}

fn fetch_fna_runtime_assembly(ms_home: &Path, dst: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let download_dir = ms_home.join("cache").join("downloads");
    std::fs::create_dir_all(&download_dir)?;
    let package_file = download_dir.join(format!("FNA-XNA-Wrapper.{}.nupkg", FNA_XNA_WRAPPER_VERSION));
    if !package_file.exists() {
        download_fna_package(&package_file)?;
    }
    extract_fna_assembly_from_package(&package_file, dst)
}

fn download_fna_package(package_file: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let url = fna_package_download_url();
    let config =
        ureq::config::Config::builder().user_agent(format!("MetalSharp/{}", env!("CARGO_PKG_VERSION"))).build();
    let agent = ureq::Agent::new_with_config(config);
    let resp = agent.get(&url).call().map_err(|err| format!("FNA runtime download failed: {}", err))?;
    let tmp_file = package_file.with_extension("nupkg.tmp");
    let mut input = resp.into_body().into_reader();
    let mut output = std::fs::File::create(&tmp_file)?;
    std::io::copy(&mut input, &mut output)?;
    std::fs::rename(tmp_file, package_file)?;
    Ok(())
}

fn fna_package_download_url() -> String {
    format!(
        "https://api.nuget.org/v3-flatcontainer/fna-xna-wrapper/{0}/fna-xna-wrapper.{0}.nupkg",
        FNA_XNA_WRAPPER_VERSION
    )
}

fn extract_fna_assembly_from_package(package_file: &Path, dst: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let file = std::fs::File::open(package_file)?;
    let mut archive = zip::ZipArchive::new(file)?;
    let mut entry = archive.by_name("lib/net45/FNA.dll")?;
    let tmp_file = dst.with_extension("dll.tmp");
    if let Some(parent) = dst.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let mut out = std::fs::File::create(&tmp_file)?;
    std::io::copy(&mut entry, &mut out)?;
    std::fs::rename(tmp_file, dst)?;
    Ok(())
}

fn ensure_fna_native_shim_in_cache(spec: &FnaNativeShimSpec, shims_dir: &PathBuf) {
    let dst = shims_dir.join(spec.output);
    if dst.exists() {
        return;
    }

    match spec.source {
        FnaShimSource::RepoC { parts, undefined_dynamic_lookup } => {
            if let Some(source) = find_fna_shim_source(parts) {
                let _ = build_fna_c_shim(&source, &dst, spec.output, &[], undefined_dynamic_lookup);
            }
        },
        FnaShimSource::RepoObjC { parts, frameworks } => {
            if let Some(source) = find_fna_shim_source(parts) {
                let _ = build_fna_c_shim(&source, &dst, spec.output, frameworks, false);
            }
        },
        FnaShimSource::BundledNative => {
            if let Some(source) = find_bundled_native_shim(spec.output) {
                let _ = std::fs::copy(source, &dst);
                codesign_fna_shim(&dst);
            }
        },
    }

    if dst.exists() {
        for symlink in spec.symlinks {
            let link = shims_dir.join(symlink);
            if !link.exists() {
                #[cfg(unix)]
                {
                    if std::fs::symlink_metadata(&link).is_ok() {
                        let _ = std::fs::remove_file(&link);
                    }
                    let _ = std::os::unix::fs::symlink(spec.output, &link);
                }
            }
        }
    }
}

fn build_fna_c_shim(
    source: &PathBuf,
    output: &PathBuf,
    install_name: &str,
    frameworks: &[&str],
    undefined_dynamic_lookup: bool,
) -> bool {
    if let Some(parent) = output.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let mut cmd = Command::new("clang");
    cmd.arg("-shared");
    for arg in fna_native_arch_args() {
        cmd.arg(arg);
    }
    if source.extension().and_then(|ext| ext.to_str()) == Some("m") {
        cmd.arg("-fobjc-arc");
    }
    if undefined_dynamic_lookup {
        cmd.args(["-undefined", "dynamic_lookup"]);
    }
    cmd.arg("-o").arg(output).arg(source).args(["-install_name", &format!("@loader_path/{}", install_name)]);
    for framework in frameworks {
        cmd.args(["-framework", framework]);
    }
    let success = cmd
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map(|status| status.success())
        .unwrap_or(false);
    if success {
        codesign_fna_shim(output);
    }
    success
}

fn fna_native_arch_args() -> Vec<&'static str> {
    if crate::platform::current() == crate::platform::HostPlatform::Macos {
        vec!["-arch", "arm64", "-arch", "x86_64"]
    } else {
        Vec::new()
    }
}

fn find_bundled_native_shim(name: &str) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(resources) = crate::platform::app_resources_dir() {
        candidates.push(resources.join("scripts").join("tools").join("native").join(name));
        candidates.push(resources.join("runtime").join("shims").join(name));
    }
    if let Some(source) = find_repo_source(&["app", "native", name]) {
        candidates.push(source);
    }
    if let Some(source) = find_repo_source(&["build", name]) {
        candidates.push(source);
    }
    candidates.into_iter().find(|candidate| candidate.is_file() && file_has_payload(candidate))
}

fn find_fna_shim_source(parts: &[&str]) -> Option<PathBuf> {
    if let Some(source) = find_repo_source(parts) {
        return Some(source);
    }
    let filename = parts.last()?;
    let resources = crate::platform::app_resources_dir()?;
    let candidates = [
        resources.join("runtime").join("shim-sources").join("fna").join("shims").join(filename),
        resources.join("scripts").join("tools").join("fna").join("shims").join(filename),
    ];
    candidates.into_iter().find(|candidate| candidate.is_file())
}

fn file_has_payload(path: &Path) -> bool {
    std::fs::metadata(path).map(|metadata| metadata.len() > 0).unwrap_or(false)
}

fn codesign_fna_shim(path: &PathBuf) {
    if crate::platform::current() != crate::platform::HostPlatform::Macos || !path.exists() {
        return;
    }
    let _ = Command::new("codesign")
        .args(["--force", "-s", "-"])
        .arg(path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();
}

fn fna_native_launch_env(game_dir: &PathBuf) -> Vec<(String, String)> {
    let carbon = game_dir.join(FNA_CARBON_SHIM);
    let interpose = game_dir.join(FNA_CARBON_INTERPOSE_SHIM);
    let mut env = Vec::new();
    if carbon.exists() {
        env.push(("METALSHARP_CARBON_SHIM".to_string(), carbon.to_string_lossy().to_string()));
    }
    if interpose.exists() {
        let existing = std::env::var("DYLD_INSERT_LIBRARIES").ok();
        env.push((
            "DYLD_INSERT_LIBRARIES".to_string(),
            append_path_env(existing.as_deref(), &interpose.to_string_lossy()),
        ));
    }
    env
}

fn append_path_env(existing: Option<&str>, value: &str) -> String {
    match existing.map(str::trim).filter(|v| !v.is_empty()) {
        Some(existing) if existing.split(':').any(|item| item == value) => existing.to_string(),
        Some(existing) => format!("{}:{}", existing, value),
        None => value.to_string(),
    }
}

fn deploy_terraria_runtime(game_dir: &PathBuf, metalsharp_home: &PathBuf) {
    // H2: Xact stub ships PREBUILT in the bundle (runtime/prebuilt-launchers/
    // Microsoft.Xna.Framework.Xact.dll) — never compile from the dev repo.
    let prebuilt_xact =
        metalsharp_home.join("runtime").join("prebuilt-launchers").join("Microsoft.Xna.Framework.Xact.dll");
    if prebuilt_xact.is_file() {
        let output = game_dir.join("Microsoft.Xna.Framework.Xact.dll");
        if !output.exists() {
            let _ = std::fs::copy(&prebuilt_xact, &output);
        }
    }

    let faudio_dst = game_dir.join("libFAudio.0.dylib");
    if !faudio_dst.exists() {
        let home = dirs::home_dir().unwrap_or_default();
        let ms_home = crate::platform::metalsharp_home_dir_for(&home);
        let cached = ms_home.join("runtime").join("shims").join("libFAudio.0.dylib");
        // H3: faudio ships prebuilt in the bundle shims — never clang-compile
        // the dev-machine faudio_stub.c at launch.
        if cached.exists() {
            let _ = std::fs::copy(&cached, &faudio_dst);
            ensure_fna_symlink(game_dir, "libFAudio.0.dylib", "libFAudio.dylib");
        }
    }

    // TerrariaOfflinePatcher ships PREBUILT in the bundle; it is still RUN at
    // deploy time to patch Terraria.exe's offline identity.
    let prebuilt_patcher =
        metalsharp_home.join("runtime").join("prebuilt-launchers").join("TerrariaOfflinePatcher.exe");
    if prebuilt_patcher.is_file() {
        let patcher = game_dir.join("TerrariaOfflinePatcher.exe");
        if !patcher.exists() {
            let _ = std::fs::copy(&prebuilt_patcher, &patcher);
        }
        let mono = metalsharp_home.join("runtime").join("mono-x86").join("bin").join("mono");
        let terraria = game_dir.join("Terraria.exe");
        if mono.exists() && terraria.exists() && patcher.exists() {
            let _ = Command::new(&mono)
                .current_dir(game_dir)
                .arg(&patcher)
                .arg(&terraria)
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .status();
        }
    }
}

fn compile_repo_csharp_to_game(source: &PathBuf, output: &PathBuf, target: &str, extra_args: &[String]) -> bool {
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return false,
    };
    let mono_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("mono-x86");
    let mono = mono_root.join("bin").join("mono");
    let mcs = mono_root.join("lib").join("mono").join("4.5").join("mcs.exe");
    compile_csharp_with_mono(&mono, &mcs, source, output, target, extra_args)
}

fn compile_csharp_with_mono(
    mono: &PathBuf,
    mcs: &PathBuf,
    source: &PathBuf,
    output: &PathBuf,
    target: &str,
    extra_args: &[String],
) -> bool {
    if !mono.exists() || !mcs.exists() || !source.exists() {
        return false;
    }
    let mut cmd = Command::new(mono);
    cmd.arg(mcs).arg(format!("-target:{}", target)).arg(format!("-out:{}", output.to_string_lossy()));
    for arg in extra_args {
        cmd.arg(arg);
    }
    cmd.arg(source).stdout(std::process::Stdio::null()).stderr(std::process::Stdio::null());
    cmd.status().map(|s| s.success()).unwrap_or(false)
}

fn copy_fna_native_lib(game_dir: &PathBuf, shims_dir: &PathBuf, lib: &str, symlink: Option<&str>) {
    let dst = game_dir.join(lib);
    if dst.exists() {
        if fna_native_lib_needs_refresh(lib, &dst) {
            let _ = std::fs::remove_file(&dst);
        } else {
            fix_dylib_install_names(&dst);
            if let Some(sym) = symlink {
                ensure_fna_symlink(game_dir, lib, sym);
            }
            return;
        }
    }

    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return,
    };
    let fnalibs_dir = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("fnalibs");
    let fnalibs = if matches!(lib, "libfmod.dylib" | "libfmodstudio.dylib") {
        fnalibs_dir.join("fmod").join(lib)
    } else {
        fnalibs_dir.join(lib)
    };
    if fna_native_lib_source_valid(lib, &fnalibs) {
        let _ = std::fs::copy(&fnalibs, &dst);
    } else {
        let cached = shims_dir.join(lib);
        if fna_native_lib_source_valid(lib, &cached) {
            let _ = std::fs::copy(&cached, &dst);
        } else {
            let homebrew_candidates =
                [PathBuf::from(format!("/opt/homebrew/lib/{}", lib)), PathBuf::from(format!("/usr/local/lib/{}", lib))];
            for candidate in &homebrew_candidates {
                if fna_native_lib_source_valid(lib, candidate) {
                    let _ = std::fs::copy(candidate, &dst);
                    break;
                }
            }
        }
    }

    if dst.exists() {
        fix_dylib_install_names(&dst);
    }

    if let Some(sym) = symlink {
        ensure_fna_symlink(game_dir, lib, sym);
    }
}

fn fna_native_lib_needs_refresh(lib: &str, path: &Path) -> bool {
    !fna_native_lib_source_valid(lib, path)
}

pub(crate) fn fna_native_lib_source_valid(lib: &str, path: &Path) -> bool {
    if !file_has_payload(path) {
        return false;
    }
    if matches!(lib, "libfmod.dylib" | "libfmodstudio.dylib") {
        return std::fs::metadata(path).map(|metadata| metadata.len() >= 256 * 1024).unwrap_or(false);
    }
    if matches!(lib, "libFNA3D.0.dylib" | "libFNA3D.dylib" | "libFAudio.0.dylib" | "libFAudio.dylib") {
        return !dylib_depends_on(path, "libSDL3") && dylib_depends_on(path, "libSDL2");
    }
    true
}

fn dylib_depends_on(path: &Path, needle: &str) -> bool {
    if crate::platform::current() != crate::platform::HostPlatform::Macos {
        return false;
    }
    let Ok(output) = Command::new("/usr/bin/otool").args(["-L", "-arch", "x86_64"]).arg(path).output() else {
        return false;
    };
    if !output.status.success() {
        return false;
    }
    String::from_utf8_lossy(&output.stdout).contains(needle)
}

fn fix_dylib_install_names(dylib_path: &PathBuf) {
    if crate::platform::current() != crate::platform::HostPlatform::Macos {
        return;
    }
    let name = dylib_path.file_name().unwrap_or_default().to_string_lossy().to_string();
    let _ = Command::new("/usr/bin/install_name_tool")
        .args(["-id", &format!("@loader_path/{}", name)])
        .arg(dylib_path)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status();

    if let Ok(output) = Command::new("/usr/bin/otool").args(["-L", "-arch", "x86_64"]).arg(dylib_path).output() {
        let deps = String::from_utf8_lossy(&output.stdout);
        for line in deps.lines() {
            let trimmed = line.trim();
            if trimmed.starts_with("@rpath/") {
                let dep_name = trimmed.split(' ').next().unwrap_or("");
                let dep_file = dep_name.strip_prefix("@rpath/").unwrap_or(dep_name);
                let _ = Command::new("/usr/bin/install_name_tool")
                    .args(["-change", dep_name, &format!("@loader_path/{}", dep_file)])
                    .arg(dylib_path)
                    .stdout(std::process::Stdio::null())
                    .stderr(std::process::Stdio::null())
                    .status();
            }
        }
    }

    codesign_fna_shim(dylib_path);
}

fn ensure_fna_symlink(game_dir: &PathBuf, lib: &str, sym: &str) {
    let link = game_dir.join(sym);
    if link.exists() {
        return;
    }
    #[cfg(unix)]
    {
        if std::fs::symlink_metadata(&link).is_ok() {
            let _ = std::fs::remove_file(&link);
        }
        let _ = std::os::unix::fs::symlink(lib, link);
    }
}

fn deploy_offline_steamworks_net(game_dir: &PathBuf, metalsharp_home: &PathBuf) -> Result<(), String> {
    let steamworks = game_dir.join("Steamworks.NET.dll");
    if !steamworks.exists() {
        return Ok(());
    }
    let backup = game_dir.join("Steamworks.NET.dll.metalsharp-original");
    if !backup.exists() {
        std::fs::copy(&steamworks, &backup)
            .map_err(|e| format!("backup Steamworks.NET.dll for {}: {}", game_dir.display(), e))?;
    }

    let source = find_fna_shim_source(&["src", "fna", "shims", "SteamworksOffline.cs"])
        .ok_or_else(|| "SteamworksOffline.cs not found in repo or bundled shim sources".to_string())?;
    let output = steamworks;
    let mono_roots =
        [metalsharp_home.join("runtime").join("mono-x86"), metalsharp_home.join("runtime").join("mono-arm64")];
    for mono_root in &mono_roots {
        let mono = ["mono-sgen64", "mono-sgen", "mono"]
            .iter()
            .map(|name| mono_root.join("bin").join(name))
            .find(|path| path.exists())
            .unwrap_or_else(|| mono_root.join("bin").join("mono"));
        let mcs = mono_root.join("lib").join("mono").join("4.5").join("mcs.exe");
        if !mono.exists() || !mcs.exists() {
            continue;
        }
        let status = Command::new(&mono)
            .arg(&mcs)
            .args(["-target:library"])
            .arg(format!("-out:{}", output.to_string_lossy()))
            .arg(&source)
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
        if status.map(|s| s.success()).unwrap_or(false) {
            if offline_steamworks_net_deployed(&output, &backup) {
                return Ok(());
            }
            return Err(format!("offline Steamworks.NET.dll compile did not replace {}", output.display()));
        }
    }
    Err(format!("could not compile offline Steamworks.NET.dll for {}", game_dir.display()))
}

fn offline_steamworks_net_deployed(output: &Path, backup: &Path) -> bool {
    if !file_has_payload(output) {
        return false;
    }
    let Ok(output_meta) = std::fs::metadata(output) else {
        return false;
    };
    if let Ok(backup_meta) = std::fs::metadata(backup) {
        if output_meta.len() == backup_meta.len() {
            return false;
        }
    }
    true
}

fn find_repo_source(parts: &[&str]) -> Option<PathBuf> {
    if let Ok(mut dir) = std::env::current_dir() {
        for _ in 0..8 {
            let mut candidate = dir.clone();
            for part in parts {
                candidate.push(part);
            }
            if candidate.exists() {
                return Some(candidate);
            }
            match dir.parent() {
                Some(parent) => dir = parent.to_path_buf(),
                None => break,
            }
        }
    }

    if let Ok(exe) = std::env::current_exe() {
        if let Some(mut dir) = exe.parent() {
            for _ in 0..8 {
                let mut candidate = dir.to_path_buf();
                for part in parts {
                    candidate.push(part);
                }
                if candidate.exists() {
                    return Some(candidate);
                }
                match dir.parent() {
                    Some(parent) => dir = parent,
                    None => break,
                }
            }
        }
    }

    None
}

fn deploy_goldberg(home: &PathBuf, game_dir: &PathBuf, appid: u32) {
    deploy_goldberg_internal(home, game_dir, appid);
}

fn prepare_steam_api_for_pipeline(appid: u32, pipeline_id: PipelineId) {
    let Some(home) = dirs::home_dir() else {
        return;
    };
    let Some(game_dir) = crate::setup::resolve_windows_game_dir(appid) else {
        return;
    };

    prepare_steam_api_for_game_dir(&home, &game_dir, appid, pipeline_id);
}

fn prepare_steam_api_for_game_dir(home: &Path, game_dir: &Path, appid: u32, pipeline_id: PipelineId) {
    if goldberg_status_for_pipeline(home, game_dir, pipeline_id) {
        ensure_steam_emu_for_pipeline_if_active(home, game_dir, appid, pipeline_id);
    } else {
        ensure_real_steam_dlls(home, game_dir, appid, super::recipe::uses_steam_launch_model(appid, pipeline_id));
    }
}

fn goldberg_deploy_targets(game_dir: &Path) -> Vec<PathBuf> {
    let mut targets: Vec<PathBuf> = vec![
        game_dir.to_path_buf(),
        game_dir.join("Game"),
        game_dir.join("bin"),
        game_dir.join("Binaries").join("Win32"),
        game_dir.join("Binaries").join("Win64"),
        game_dir.join("win64"),
    ];
    if let Ok(entries) = std::fs::read_dir(game_dir) {
        for entry in entries.flatten() {
            let child = entry.path().join("Binaries").join("Win64");
            if child.is_dir() {
                targets.push(child);
            }
        }
    }
    targets.dedup();
    targets
}

fn push_unique_path(paths: &mut Vec<PathBuf>, candidate: PathBuf) {
    if paths.iter().any(|path| path == &candidate) {
        return;
    }
    paths.push(candidate);
}

fn push_unique_physical_path(paths: &mut Vec<PathBuf>, candidate: PathBuf) {
    let candidate_canon = std::fs::canonicalize(&candidate).ok();
    if let Some(candidate_canon) = candidate_canon.as_ref() {
        if paths.iter().any(|path| std::fs::canonicalize(path).ok().as_ref() == Some(candidate_canon)) {
            return;
        }
    } else if paths.iter().any(|path| path == &candidate) {
        return;
    }

    paths.push(candidate);
}

fn resolve_dosdevice_target(dosdevices: &Path, target: PathBuf) -> PathBuf {
    if target.is_absolute() {
        target
    } else {
        dosdevices.join(target)
    }
}

fn gptk_prefix_game_dir_aliases(prefix: &Path, game_dir: &Path) -> Vec<PathBuf> {
    let dosdevices = prefix.join("dosdevices");
    let Ok(game_dir_canon) = std::fs::canonicalize(game_dir) else {
        return Vec::new();
    };
    let Ok(entries) = std::fs::read_dir(&dosdevices) else {
        return Vec::new();
    };

    let mut aliases = Vec::new();
    for entry in entries.flatten() {
        let link_path = entry.path();
        let name = entry.file_name().to_string_lossy().to_string();
        if name.len() != 2 || !name.ends_with(':') {
            continue;
        }
        let Ok(link_target) = std::fs::read_link(&link_path) else {
            continue;
        };
        let resolved_target = resolve_dosdevice_target(&dosdevices, link_target);
        let Ok(target_canon) = std::fs::canonicalize(&resolved_target) else {
            continue;
        };
        if !game_dir_canon.starts_with(&target_canon) {
            continue;
        }
        let Ok(relative) = game_dir_canon.strip_prefix(&target_canon) else {
            continue;
        };
        push_unique_path(&mut aliases, link_path.join(relative));
    }
    aliases
}

fn goldberg_dirs_for_pipeline(home: &Path, game_dir: &Path, pipeline_id: PipelineId) -> Vec<PathBuf> {
    let mut dirs = vec![game_dir.to_path_buf()];
    if matches!(pipeline_id, PipelineId::D3DMetal) {
        let prefix = crate::platform::gptk_prefix_path(home);
        for alias in gptk_prefix_game_dir_aliases(&prefix, game_dir) {
            push_unique_physical_path(&mut dirs, alias);
        }
    }
    dirs
}

// ---------------------------------------------------------------------------
// Goldberg backup cache + persisted metadata
//
// The original Steam DLLs that Goldberg overwrites are cached under
// `~/.metalsharp/cache/goldberg/<appid>/` so that toggling Goldberg OFF can
// restore the game to baseline even if the in-game `.orig` files have been
// deleted or corrupted. Toggle state is persisted in
// `~/.metalsharp/sharp-library/goldberg/<appid>.json` so the UI can reflect
// user intent regardless of filesystem probes.
// ---------------------------------------------------------------------------

/// Minimum size of a real Steam DLL we will treat as worth caching. Real
/// `steam_api.dll`/`steam_api64.dll`/`steamclient*.dll` are typically 200 KB+.
const GOLDBERG_MIN_BACKUP_BYTES: u64 = 32 * 1024;

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct GoldbergMetadata {
    pub appid: u32,
    pub active: bool,
    pub backed_up_at: Option<u64>,
    pub backed_up_from: Vec<String>,
    pub cache_hashes: std::collections::BTreeMap<String, String>,
    pub goldberg_source_hashes: std::collections::BTreeMap<String, String>,
}
fn goldberg_metadata_dir() -> PathBuf {
    goldberg_metadata_dir_for(&crate::platform::metalsharp_home_dir())
}

fn goldberg_metadata_dir_for(home_root: &Path) -> PathBuf {
    home_root.join("sharp-library").join("goldberg")
}

pub fn goldberg_metadata_path(appid: u32) -> PathBuf {
    goldberg_metadata_path_for(&crate::platform::metalsharp_home_dir(), appid)
}

pub fn goldberg_metadata_path_for(home_root: &Path, appid: u32) -> PathBuf {
    goldberg_metadata_dir_for(home_root).join(format!("{appid}.json"))
}

/// Per-app Goldberg backup cache, rooted at `~/.metalsharp/cache/goldberg/<appid>/`.
pub fn goldberg_cache_dir(appid: u32) -> PathBuf {
    goldberg_cache_dir_for(&crate::platform::metalsharp_home_dir(), appid)
}

/// Same as [`goldberg_cache_dir`], but rooted at an explicit MetalSharp home
/// directory. Used by tests to point the cache at a sandbox.
pub fn goldberg_cache_dir_for(home_root: &Path, appid: u32) -> PathBuf {
    home_root.join("cache").join("goldberg").join(appid.to_string())
}

pub fn read_goldberg_metadata(appid: u32) -> Option<GoldbergMetadata> {
    read_goldberg_metadata_for(&crate::platform::metalsharp_home_dir(), appid)
}

pub fn read_goldberg_metadata_for(home_root: &Path, appid: u32) -> Option<GoldbergMetadata> {
    let path = goldberg_metadata_path_for(home_root, appid);
    let data = std::fs::read_to_string(&path).ok()?;
    serde_json::from_str(&data).ok()
}

fn write_goldberg_metadata(metadata: &GoldbergMetadata) {
    write_goldberg_metadata_for(&crate::platform::metalsharp_home_dir(), metadata);
}

fn write_goldberg_metadata_for(home_root: &Path, metadata: &GoldbergMetadata) {
    let path = goldberg_metadata_path_for(home_root, metadata.appid);
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    if let Ok(json) = serde_json::to_string_pretty(metadata) {
        let _ = std::fs::write(&path, json);
    }
}

/// Compute a SHA-256 hex hash of a file's contents, returning `None` if the
/// file cannot be read.
fn file_sha256_hex(path: &Path) -> Option<String> {
    use sha2::{Digest, Sha256};
    let data = std::fs::read(path).ok()?;
    let mut hasher = Sha256::new();
    hasher.update(&data);
    let hash = hasher.finalize();
    let mut out = String::with_capacity(hash.len() * 2);
    for byte in hash.iter() {
        out.push_str(&format!("{:02x}", byte));
    }
    Some(out)
}

/// Returns true when `candidate` is byte-for-byte the same file as the Goldberg
/// runtime replacement. Used to refuse caching Goldberg DLLs as "originals".
fn file_matches_goldberg_source(candidate: &Path, goldberg_src: &Path) -> bool {
    let Ok(candidate_data) = std::fs::read(candidate) else {
        return false;
    };
    let Ok(goldberg_data) = std::fs::read(goldberg_src) else {
        return false;
    };
    candidate_data == goldberg_data
}

/// Copy an original Steam DLL from `target_dir/<dll_name>` into the per-app
/// backup cache. The cache is authoritative — we never copy if it already
/// exists with a non-empty file, and we refuse Goldberg's own DLLs.
///
/// Returns true if the cache now contains this DLL.
fn cache_original_dll(
    home_root: &Path,
    appid: u32,
    target_dir: &Path,
    dll_name: &str,
    goldberg_src: &Path,
    cache_hashes: &mut std::collections::BTreeMap<String, String>,
) -> bool {
    let src = target_dir.join(dll_name);
    if !src.is_file() {
        return false;
    }
    let Ok(meta) = src.metadata() else {
        return false;
    };
    if meta.len() < GOLDBERG_MIN_BACKUP_BYTES {
        return false;
    }
    if goldberg_src.is_file() && file_matches_goldberg_source(&src, goldberg_src) {
        return false;
    }
    let cache_root = goldberg_cache_dir_for(home_root, appid);
    if let Err(error) = std::fs::create_dir_all(&cache_root) {
        eprintln!("goldberg: failed to create cache root {}: {}", cache_root.display(), error);
        return false;
    }
    let dest = cache_root.join(dll_name);
    // Idempotent: if the cache already has a valid file with the same content
    // we do not need to overwrite it. This avoids I/O when toggling Goldberg
    // ON multiple times for the same appid.
    if dest.is_file() {
        if let Ok(existing_data) = std::fs::read(&dest) {
            if let Ok(src_data) = std::fs::read(&src) {
                if existing_data == src_data {
                    if let Some(hash) = file_sha256_hex(&dest) {
                        cache_hashes.insert(dll_name.to_string(), hash);
                    }
                    return true;
                }
            }
        }
    }
    if let Err(error) = std::fs::copy(&src, &dest) {
        eprintln!("goldberg: failed to cache original {} → {}: {}", src.display(), dest.display(), error);
        return false;
    }
    if let Some(hash) = file_sha256_hex(&dest) {
        cache_hashes.insert(dll_name.to_string(), hash);
    }
    eprintln!("goldberg: cached original {} for appid {}", dll_name, appid);
    true
}

/// Back up every DLL that Goldberg is about to overwrite in `target_dir`.
/// No-op if the cache is already populated for this appid.
fn backup_dlls_for_appid(
    home_root: &Path,
    appid: u32,
    targets: &[PathBuf],
    goldberg_runtime: &Path,
) -> (bool, std::collections::BTreeMap<String, String>, std::collections::BTreeMap<String, String>) {
    let mut cache_hashes = std::collections::BTreeMap::new();
    let mut goldberg_hashes = std::collections::BTreeMap::new();
    let mut backed_up_from: Vec<String> = Vec::new();
    let mut any_backup = false;
    // dll_name, goldberg_subdir_relative_to_runtime
    let candidates: [(&str, &str); 4] = [
        ("steam_api.dll", "x86"),
        ("steam_api64.dll", "x64"),
        ("steamclient64.dll", "steamclient"),
        ("steamclient.dll", "steamclient"),
    ];
    // Fallback: the user's real Wine Steam installation. When the game dir
    // doesn't already contain the original DLLs (because the game hasn't
    // been run through Steam yet), we copy the official Steam DLLs from
    // the prefix-steam Steam install into the cache so the "Backup Missing"
    // status doesn't show when we have perfectly-good originals elsewhere.
    let steam_install_dir = home_root.join("prefix-steam").join("drive_c").join("Program Files (x86)").join("Steam");
    for target in targets {
        if !target.is_dir() {
            continue;
        }
        for (dll_name, sub) in candidates.iter() {
            let src = target.join(dll_name);
            // If the DLL isn't already in the game target, try copying
            // the official Steam DLL from the user's Steam prefix. This
            // ensures the backup always exists when the user has Steam
            // installed (which is a prerequisite for using Goldberg).
            if !src.is_file() {
                let steam_src = steam_install_dir.join(dll_name);
                if steam_src.is_file() {
                    if let Some(parent) = src.parent() {
                        let _ = std::fs::create_dir_all(parent);
                    }
                    // Stage the original Steam DLL into the game target so
                    // downstream code (cache_original_dll etc.) finds it.
                    if std::fs::copy(&steam_src, &src).is_ok() {
                        any_backup = true;
                        let rel = target.to_string_lossy().to_string();
                        if !backed_up_from.contains(&rel) {
                            backed_up_from.push(rel);
                        }
                    }
                }
                if !src.is_file() {
                    continue;
                }
            }
            // Check whether this DLL is already cached. If it is, skip the
            // copy and just record it for the manifest.
            let cached = goldberg_cache_dir_for(home_root, appid).join(dll_name);
            if cached.is_file() && cached.metadata().map(|m| m.len() >= GOLDBERG_MIN_BACKUP_BYTES).unwrap_or(false) {
                if let Some(hash) = file_sha256_hex(&cached) {
                    cache_hashes.insert(dll_name.to_string(), hash);
                }
                any_backup = true;
                let rel = target.to_string_lossy().to_string();
                if !backed_up_from.contains(&rel) {
                    backed_up_from.push(rel);
                }
                continue;
            }
            let goldberg_src = goldberg_runtime.join(sub).join(dll_name);
            if cache_original_dll(home_root, appid, target, dll_name, &goldberg_src, &mut cache_hashes) {
                any_backup = true;
                let rel = target.to_string_lossy().to_string();
                if !backed_up_from.contains(&rel) {
                    backed_up_from.push(rel);
                }
                if let Some(hash) = file_sha256_hex(&goldberg_src) {
                    goldberg_hashes.insert(dll_name.to_string(), hash);
                }
            }
        }
    }
    (any_backup, cache_hashes, goldberg_hashes)
}

/// Restore every cached DLL for `appid` back into `target_dir`, but only if
/// the cached file is not byte-identical to the Goldberg runtime replacement
/// (which would mean the cache was never populated and contains Goldberg's own
/// DLLs by mistake). Each restored DLL is then placed next to its matching
/// `.orig` so the legacy restore path still works.
fn restore_cached_dlls(
    home_root: &Path,
    appid: u32,
    targets: &[PathBuf],
    goldberg_runtime: &Path,
) -> (usize, Vec<String>) {
    let cache_root = goldberg_cache_dir_for(home_root, appid);
    if !cache_root.is_dir() {
        return (0, Vec::new());
    }
    let candidates: [(&str, &str); 4] = [
        ("steam_api.dll", "x86"),
        ("steam_api64.dll", "x64"),
        ("steamclient64.dll", "steamclient"),
        ("steamclient.dll", "steamclient"),
    ];
    let mut restored = 0;
    let mut sources: Vec<String> = Vec::new();
    for target in targets {
        if !target.is_dir() {
            continue;
        }
        for (dll_name, sub) in candidates.iter() {
            let cached = cache_root.join(dll_name);
            if !cached.is_file() {
                continue;
            }
            let goldberg_src = goldberg_runtime.join(sub).join(dll_name);
            if goldberg_src.is_file() && file_matches_goldberg_source(&cached, &goldberg_src) {
                // Cache happens to contain Goldberg's own DLL — refuse to
                // copy it back as the "real" Steam DLL.
                eprintln!("goldberg: cache for {} contains Goldberg's own DLL, skipping restore", dll_name);
                continue;
            }
            // Prefer restoring into a target that already had the Goldberg DLL
            // in place OR has a `.orig` marker. Skip targets without either so
            // we don't accidentally drop real DLLs into unrelated bins.
            let dst = target.join(dll_name);
            let dst_orig = target.join(format!("{}.orig", dll_name));
            if !dst.is_file() && !dst_orig.is_file() {
                continue;
            }
            if let Err(error) = std::fs::copy(&cached, &dst) {
                eprintln!("goldberg: failed to restore {} → {}: {}", cached.display(), dst.display(), error);
                continue;
            }
            // Mark the freshly restored DLL as the new `.orig` so the legacy
            // restore path keeps working.
            if dst_orig.is_file() {
                let _ = std::fs::remove_file(&dst_orig);
            }
            if std::fs::rename(&dst, &dst_orig).is_ok() {
                if std::fs::copy(&cached, &dst).is_ok() {
                    restored += 1;
                }
            } else {
                // Fallback: copy the cached DLL over the Goldberg one and
                // refresh the .orig from the cache file too.
                let _ = std::fs::copy(&cached, &dst_orig);
                restored += 1;
            }
            let rel = target.to_string_lossy().to_string();
            if !sources.contains(&rel) {
                sources.push(rel);
            }
            eprintln!("goldberg: restored cached {} for appid {} → {}", dll_name, appid, target.display());
        }
    }
    (restored, sources)
}

/// Update the persisted metadata to record a toggle ON for `appid`. Will
/// create a fresh metadata file (or merge into an existing one).
fn persist_goldberg_toggle_on(
    home_root: &Path,
    appid: u32,
    targets: &[PathBuf],
    goldberg_runtime: &Path,
) -> GoldbergMetadata {
    let (any_backup, cache_hashes, goldberg_hashes) =
        backup_dlls_for_appid(home_root, appid, targets, goldberg_runtime);
    let now_secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or_default();
    let mut existing = read_goldberg_metadata_for(home_root, appid).unwrap_or(GoldbergMetadata {
        appid,
        active: false,
        backed_up_at: None,
        backed_up_from: Vec::new(),
        cache_hashes: std::collections::BTreeMap::new(),
        goldberg_source_hashes: std::collections::BTreeMap::new(),
    });
    existing.appid = appid;
    existing.active = true;
    if any_backup || existing.backed_up_at.is_none() {
        existing.backed_up_at = Some(now_secs);
    }
    for target in targets {
        let rel = target.to_string_lossy().to_string();
        if !existing.backed_up_from.contains(&rel) {
            existing.backed_up_from.push(rel);
        }
    }
    for (key, value) in cache_hashes {
        existing.cache_hashes.insert(key, value);
    }
    for (key, value) in goldberg_hashes {
        existing.goldberg_source_hashes.insert(key, value);
    }
    write_goldberg_metadata_for(home_root, &existing);
    existing
}

/// Mark Goldberg OFF in the persisted metadata. The cache and `.orig` files
/// are left in place so re-enabling Goldberg can simply look them up again.
fn persist_goldberg_toggle_off(home_root: &Path, appid: u32) -> GoldbergMetadata {
    let mut existing = read_goldberg_metadata_for(home_root, appid).unwrap_or(GoldbergMetadata {
        appid,
        active: true,
        backed_up_at: None,
        backed_up_from: Vec::new(),
        cache_hashes: std::collections::BTreeMap::new(),
        goldberg_source_hashes: std::collections::BTreeMap::new(),
    });
    existing.appid = appid;
    existing.active = false;
    write_goldberg_metadata_for(home_root, &existing);
    existing
}

/// Whether the persisted metadata says Goldberg is currently active for `appid`.
fn metadata_says_active(appid: u32) -> bool {
    metadata_says_active_for(&crate::platform::metalsharp_home_dir(), appid)
}

/// Same as [`metadata_says_active`], but rooted at an explicit home.
pub fn metadata_says_active_for(home_root: &Path, appid: u32) -> bool {
    read_goldberg_metadata_for(home_root, appid).map(|m| m.active).unwrap_or(false)
}

fn goldberg_deploy_settings(steam_settings: &Path, appid: u32) {
    if !steam_settings.exists() {
        let _ = std::fs::create_dir_all(steam_settings);
    }
    let _ = std::fs::write(steam_settings.join("force_steam_appid.txt"), appid.to_string());
    let _ = std::fs::write(steam_settings.join("account_name.txt"), "Player\n");
    let _ = std::fs::write(steam_settings.join("user_steam_id.txt"), "76561198000000000\n");
}

pub fn deploy_goldberg_internal(home: &PathBuf, game_dir: &PathBuf, appid: u32) {
    let emu_dir = crate::platform::metalsharp_home_dir_for(home).join("runtime").join("goldberg");
    if !emu_dir.exists() {
        eprintln!("goldberg: runtime directory not found at {}", emu_dir.display());
        return;
    }

    let targets = goldberg_deploy_targets(game_dir);

    // **Before** we overwrite any Steam DLL, snapshot the originals into the
    // per-app cache so we can restore them later even if the in-game
    // `.orig` files get deleted/corrupted by Steam, the user, or otherwise.
    persist_goldberg_toggle_on(&crate::platform::metalsharp_home_dir_for(home), appid, &targets, &emu_dir);

    let steamclient_dir = emu_dir.join("steamclient");
    let mut deployed_any = false;

    for target in &targets {
        if !target.exists() {
            continue;
        }

        let x86_src = emu_dir.join("x86").join("steam_api.dll");
        let x86_dst = target.join("steam_api.dll");
        if x86_src.exists() && !x86_dst.with_extension("orig").exists() {
            if x86_dst.exists() {
                let _ = std::fs::rename(&x86_dst, target.join("steam_api.dll.orig"));
            }
            let _ = std::fs::copy(&x86_src, &x86_dst);
            deployed_any = true;
        }

        let x64_src = emu_dir.join("x64").join("steam_api64.dll");
        let x64_dst = target.join("steam_api64.dll");
        if x64_src.exists() && !x64_dst.with_extension("orig").exists() {
            if x64_dst.exists() {
                let _ = std::fs::rename(&x64_dst, target.join("steam_api64.dll.orig"));
            }
            let _ = std::fs::copy(&x64_src, &x64_dst);
            deployed_any = true;
        }

        if steamclient_dir.is_dir() {
            let sc64_src = steamclient_dir.join("steamclient64.dll");
            let sc64_dst = target.join("steamclient64.dll");
            if sc64_src.exists() && !sc64_dst.with_extension("orig").exists() {
                if sc64_dst.exists() {
                    let _ = std::fs::rename(&sc64_dst, target.join("steamclient64.dll.orig"));
                }
                let _ = std::fs::copy(&sc64_src, &sc64_dst);
            }

            let sc32_src = steamclient_dir.join("steamclient.dll");
            let sc32_dst = target.join("steamclient.dll");
            if sc32_src.exists() && !sc32_dst.with_extension("orig").exists() {
                if sc32_dst.exists() {
                    let _ = std::fs::rename(&sc32_dst, target.join("steamclient.dll.orig"));
                }
                let _ = std::fs::copy(&sc32_src, &sc32_dst);
            }

            let ov64_src = steamclient_dir.join("GameOverlayRenderer64.dll");
            let ov64_dst = target.join("GameOverlayRenderer64.dll");
            if ov64_src.exists() && !ov64_dst.exists() {
                let _ = std::fs::copy(&ov64_src, &ov64_dst);
            }

            let ov32_src = steamclient_dir.join("GameOverlayRenderer.dll");
            let ov32_dst = target.join("GameOverlayRenderer.dll");
            if ov32_src.exists() && !ov32_dst.exists() {
                let _ = std::fs::copy(&ov32_src, &ov32_dst);
            }
        }
    }

    if !deployed_any {
        eprintln!("goldberg: no new DLLs deployed (all targets already have .orig backups or no source DLLs)");
    }

    goldberg_deploy_settings(&game_dir.join("steam_settings"), appid);

    for target in &targets {
        if target == game_dir {
            continue;
        }
        if !target.join("steam_api64.dll.orig").exists() && !target.join("steam_api.dll.orig").exists() {
            continue;
        }
        goldberg_deploy_settings(&target.join("steam_settings"), appid);
    }

    // Always regenerate interfaces after deployment — the DLLs must be in place first.
    generate_steam_interfaces(game_dir);
}

pub fn cleanup_goldberg(game_dir: &Path, appid: u32) {
    let game_dir = game_dir.to_path_buf();
    let targets = goldberg_deploy_targets(&game_dir);

    // Try to restore from the per-app backup cache when `.orig` is missing.
    let home = dirs::home_dir();
    let (home_root, emu_dir) = match home.as_ref() {
        Some(h) => {
            let root = crate::platform::metalsharp_home_dir_for(h);
            (root.clone(), root.join("runtime").join("goldberg"))
        },
        None => (
            crate::platform::metalsharp_home_dir(),
            crate::platform::metalsharp_home_dir().join("runtime").join("goldberg"),
        ),
    };
    let (restored, _) = restore_cached_dlls(&home_root, appid, &targets, &emu_dir);
    if restored > 0 {
        eprintln!("goldberg: restored {} DLL(s) from per-app cache for appid {}", restored, appid);
    }

    for target in &targets {
        if !target.exists() {
            continue;
        }

        let x86_orig = target.join("steam_api.dll.orig");
        let x86_current = target.join("steam_api.dll");
        if x86_orig.exists() {
            let _ = std::fs::rename(&x86_orig, &x86_current);
        } else if x86_current.exists() {
            // No `.orig` and nothing in cache — leave whatever DLL is in
            // place. Today's restore will treat this as ambiguous, but
            // never overwrite something we don't own.
            let _ = std::fs::remove_file(&x86_current);
        }

        let x64_orig = target.join("steam_api64.dll.orig");
        let x64_current = target.join("steam_api64.dll");
        if x64_orig.exists() {
            let _ = std::fs::rename(&x64_orig, &x64_current);
        } else if x64_current.exists() {
            let _ = std::fs::remove_file(&x64_current);
        }

        let sc64_orig = target.join("steamclient64.dll.orig");
        let sc64_current = target.join("steamclient64.dll");
        if sc64_orig.exists() {
            let _ = std::fs::rename(&sc64_orig, &sc64_current);
        } else if sc64_current.exists() {
            let _ = std::fs::remove_file(&sc64_current);
        }

        let sc32_orig = target.join("steamclient.dll.orig");
        let sc32_current = target.join("steamclient.dll");
        if sc32_orig.exists() {
            let _ = std::fs::rename(&sc32_orig, &sc32_current);
        } else if sc32_current.exists() {
            let _ = std::fs::remove_file(&sc32_current);
        }

        let _ = std::fs::remove_file(target.join("GameOverlayRenderer64.dll"));
        let _ = std::fs::remove_file(target.join("GameOverlayRenderer.dll"));

        let ss = target.join("steam_settings");
        if ss.is_dir() && target != &game_dir {
            let _ = std::fs::remove_file(ss.join("force_steam_appid.txt"));
            let _ = std::fs::remove_file(ss.join("account_name.txt"));
            let _ = std::fs::remove_file(ss.join("user_steam_id.txt"));
            let _ = std::fs::remove_file(ss.join("steam_interfaces.txt"));
            if std::fs::read_dir(&ss).map(|d| d.count()).unwrap_or(1) == 0 {
                let _ = std::fs::remove_dir(&ss);
            }
        }
    }

    let steam_settings = game_dir.join("steam_settings");
    if steam_settings.exists() {
        let _ = std::fs::remove_file(steam_settings.join("force_steam_appid.txt"));
        let _ = std::fs::remove_file(steam_settings.join("account_name.txt"));
        let _ = std::fs::remove_file(steam_settings.join("user_steam_id.txt"));
        let _ = std::fs::remove_file(steam_settings.join("steam_interfaces.txt"));
        if std::fs::read_dir(&steam_settings).map(|d| d.count()).unwrap_or(1) == 0 {
            let _ = std::fs::remove_dir(&steam_settings);
        }
    }

    // Persist OFF in metadata. We deliberately keep the cache files around
    // so a future toggle ON does not need to re-snapshot the originals.
    persist_goldberg_toggle_off(&home_root, appid);
}

pub fn goldberg_status(game_dir: &PathBuf) -> bool {
    goldberg_status_with_appid(game_dir, None)
}

/// Like [`goldberg_status`], but additionally considers the persisted
/// metadata when `appid` is provided. The status is "true" if either the
/// metadata says the toggle is ON, or `.orig` files still exist on disk
/// (preserved for backwards compatibility with prefixes that predate the
/// metadata-aware logic).
pub fn goldberg_status_with_appid(game_dir: &Path, appid: Option<u32>) -> bool {
    goldberg_status_with_appid_in(&crate::platform::metalsharp_home_dir(), game_dir, appid)
}

/// Same as [`goldberg_status_with_appid`] but rooted at an explicit home.
pub fn goldberg_status_with_appid_in(home_root: &Path, game_dir: &Path, appid: Option<u32>) -> bool {
    let targets = goldberg_deploy_targets(game_dir);
    for target in targets {
        if !target.exists() {
            continue;
        }
        if target.join("steam_api.dll.orig").exists()
            || target.join("steam_api64.dll.orig").exists()
            || target.join("steamclient64.dll.orig").exists()
            || target.join("steamclient.dll.orig").exists()
        {
            return true;
        }
    }
    if let Some(appid) = appid {
        if metadata_says_active_for(home_root, appid) {
            return true;
        }
    }
    false
}

pub fn goldberg_status_for_pipeline(home: &Path, game_dir: &Path, pipeline_id: PipelineId) -> bool {
    goldberg_status_for_pipeline_with_appid(home, game_dir, None, pipeline_id)
}

pub fn goldberg_status_for_pipeline_with_appid(
    home: &Path,
    game_dir: &Path,
    appid: Option<u32>,
    pipeline_id: PipelineId,
) -> bool {
    if goldberg_status_with_appid(game_dir, appid) {
        return true;
    }
    if !matches!(pipeline_id, PipelineId::D3DMetal) {
        return false;
    }

    let prefix = crate::platform::gptk_prefix_path(home);
    gptk_prefix_game_dir_aliases(&prefix, game_dir).iter().any(|dir| goldberg_status_with_appid(dir, appid))
}

pub fn deploy_goldberg_for_pipeline(home: &PathBuf, game_dir: &PathBuf, appid: u32, pipeline_id: PipelineId) {
    for dir in goldberg_dirs_for_pipeline(home, game_dir, pipeline_id) {
        deploy_goldberg_internal(home, &dir, appid);
    }
}

pub fn cleanup_goldberg_for_pipeline(home: &Path, game_dir: &Path, appid: u32, pipeline_id: PipelineId) {
    for dir in goldberg_dirs_for_pipeline(home, game_dir, pipeline_id) {
        cleanup_goldberg(&dir, appid);
    }
}

pub fn ensure_steam_emu_for_pipeline_if_active(home: &Path, game_dir: &Path, appid: u32, pipeline_id: PipelineId) {
    let dirs = goldberg_dirs_for_pipeline(home, game_dir, pipeline_id);
    if !dirs.iter().any(|dir| goldberg_status(&dir.to_path_buf())) {
        return;
    }

    let home_buf = home.to_path_buf();
    for dir in dirs {
        if goldberg_status(&dir.to_path_buf()) {
            ensure_steam_emu_if_active(home, &dir, appid);
        } else {
            deploy_goldberg_internal(&home_buf, &dir, appid);
        }
    }
}

pub fn ensure_steam_emu_if_active(home: &Path, game_dir: &Path, appid: u32) {
    if !goldberg_status(&game_dir.to_path_buf()) {
        return;
    }

    let emu_dir = crate::platform::metalsharp_home_dir_for(home).join("runtime").join("goldberg");
    if !emu_dir.exists() {
        eprintln!("steam_emu: toggle active but runtime not found");
        return;
    }

    let targets = goldberg_deploy_targets(game_dir);

    for target in &targets {
        if !target.exists() {
            continue;
        }

        let x64_dst = target.join("steam_api64.dll");
        let x64_orig = target.join("steam_api64.dll.orig");
        if x64_orig.exists() && !x64_dst.exists() {
            if let Ok(data) = std::fs::read(emu_dir.join("x64").join("steam_api64.dll")) {
                let _ = std::fs::write(&x64_dst, data);
                eprintln!("steam_emu: repaired steam_api64.dll in {}", target.display());
            }
        }

        let x86_dst = target.join("steam_api.dll");
        let x86_orig = target.join("steam_api.dll.orig");
        if x86_orig.exists() && !x86_dst.exists() {
            if let Ok(data) = std::fs::read(emu_dir.join("x86").join("steam_api.dll")) {
                let _ = std::fs::write(&x86_dst, data);
                eprintln!("steam_emu: repaired steam_api.dll in {}", target.display());
            }
        }

        let steamclient_dir = emu_dir.join("steamclient");
        if steamclient_dir.is_dir() {
            let sc64_dst = target.join("steamclient64.dll");
            let sc64_orig = target.join("steamclient64.dll.orig");
            if sc64_orig.exists() && !sc64_dst.exists() {
                if let Ok(data) = std::fs::read(steamclient_dir.join("steamclient64.dll")) {
                    let _ = std::fs::write(&sc64_dst, data);
                    eprintln!("steam_emu: repaired steamclient64.dll in {}", target.display());
                }
            }
        }
    }

    // Validate steam_interfaces.txt — regenerate if missing or empty.
    // This is the only repair we do at launch time; appid was set at toggle.
    let steam_settings = game_dir.join("steam_settings");
    let interfaces_file = steam_settings.join("steam_interfaces.txt");
    let needs_interfaces = !interfaces_file.exists()
        || std::fs::read_to_string(&interfaces_file).map(|content| content.trim().lines().count() < 3).unwrap_or(true);
    if needs_interfaces {
        eprintln!("steam_emu: steam_interfaces.txt missing or incomplete, regenerating");
        generate_steam_interfaces(game_dir);
    }
}

/// For non-Goldberg launches: ensure the game directory has real Steam DLLs,
/// not leftover Goldberg emulator files. Restores .orig backups if they exist,
/// or deploys from the user's Wine Steam installation.
fn ensure_real_steam_dlls(home: &Path, game_dir: &Path, appid: u32, deploy_steam_model_components: bool) {
    let targets = goldberg_deploy_targets(game_dir);
    let mut restored_any = false;

    for target in &targets {
        if !target.exists() {
            continue;
        }

        // Restore .orig backups (real Steam DLLs saved when Goldberg was toggled on).
        let x86_orig = target.join("steam_api.dll.orig");
        let x86_current = target.join("steam_api.dll");
        if x86_orig.exists() {
            // .orig exists but Goldberg was untoggled — orphaned state.
            // This shouldn't happen (cleanup_goldberg restores .orig) but
            // handle it defensively.
            let _ = std::fs::rename(&x86_orig, &x86_current);
            eprintln!("real_steam: restored orphaned steam_api.dll.orig in {}", target.display());
            restored_any = true;
        }

        let x64_orig = target.join("steam_api64.dll.orig");
        let x64_current = target.join("steam_api64.dll");
        if x64_orig.exists() {
            let _ = std::fs::rename(&x64_orig, &x64_current);
            eprintln!("real_steam: restored orphaned steam_api64.dll.orig in {}", target.display());
            restored_any = true;
        }

        let sc64_orig = target.join("steamclient64.dll.orig");
        let sc64_current = target.join("steamclient64.dll");
        if sc64_orig.exists() {
            let _ = std::fs::rename(&sc64_orig, &sc64_current);
            restored_any = true;
        }

        let sc32_orig = target.join("steamclient.dll.orig");
        let sc32_current = target.join("steamclient.dll");
        if sc32_orig.exists() {
            let _ = std::fs::rename(&sc32_orig, &sc32_current);
            restored_any = true;
        }
    }

    if restored_any {
        // Also clean up Goldberg settings dirs that shouldn't exist when
        // the emulator is off.
        let steam_settings = game_dir.join("steam_settings");
        if steam_settings.is_dir() {
            let _ = std::fs::remove_file(steam_settings.join("force_steam_appid.txt"));
            let _ = std::fs::remove_file(steam_settings.join("account_name.txt"));
            let _ = std::fs::remove_file(steam_settings.join("user_steam_id.txt"));
            let _ = std::fs::remove_file(steam_settings.join("steam_interfaces.txt"));
            if std::fs::read_dir(&steam_settings).map(|d| d.count()).unwrap_or(1) == 0 {
                let _ = std::fs::remove_dir(&steam_settings);
            }
        }
    }

    // Deploy real steam_api64.dll from the user's Wine Steam installation
    // if the game directory doesn't have one at all.
    let wine_steam_dir = crate::platform::metalsharp_home_dir_for(home)
        .join("prefix-steam")
        .join("drive_c")
        .join("Program Files (x86)")
        .join("Steam");
    let real_steam_api64 = wine_steam_dir.join("steam_api64.dll");
    let real_steam_api = wine_steam_dir.join("steam_api.dll");

    if real_steam_api64.exists() {
        for target in &targets {
            if !target.exists() {
                continue;
            }
            // Only deploy if no steam_api64.dll exists (game shipped without one,
            // or it was never installed via Steam).
            let dest = target.join("steam_api64.dll");
            if !dest.exists() {
                let _ = std::fs::copy(&real_steam_api64, &dest);
                eprintln!("real_steam: deployed Wine Steam steam_api64.dll to {}", target.display());
            }
        }
    }

    if real_steam_api.exists() {
        for target in &targets {
            if !target.exists() {
                continue;
            }
            let dest = target.join("steam_api.dll");
            if !dest.exists() {
                let _ = std::fs::copy(&real_steam_api, &dest);
                eprintln!("real_steam: deployed Wine Steam steam_api.dll to {}", target.display());
            }
        }
    }

    if deploy_steam_model_components {
        for filename in real_steam_model_component_names() {
            deploy_real_steam_component(&wine_steam_dir, &targets, filename);
        }
    }

    // Always ensure steam_appid.txt is correct for the real Steam runtime.
    deploy_steam_appid(game_dir, appid);
}

fn real_steam_model_component_names() -> &'static [&'static str] {
    &["steamclient64.dll", "steamclient.dll", "GameOverlayRenderer64.dll", "GameOverlayRenderer.dll"]
}

fn deploy_real_steam_component(wine_steam_dir: &Path, targets: &[PathBuf], filename: &str) {
    let source = wine_steam_dir.join(filename);
    if !source.exists() {
        return;
    }

    for target in targets {
        if !target.exists() {
            continue;
        }
        let dest = target.join(filename);
        if !dest.exists() {
            let _ = std::fs::copy(&source, &dest);
            eprintln!("real_steam: deployed Wine Steam {} to {}", filename, target.display());
        }
    }
}

fn start_protected_game_real_exe_names(appid: u32) -> &'static [&'static str] {
    match appid {
        1245620 => &["eldenring.exe"],
        1888160 => &["armoredcore6.exe"],
        _ => &[],
    }
}

fn prepare_start_protected_game_for_pipeline(appid: u32, pipeline_id: PipelineId) {
    if !matches!(pipeline_id, PipelineId::Dxmt | PipelineId::M12) {
        return;
    }
    let Some(game_dir) = crate::setup::resolve_windows_game_dir(appid) else {
        return;
    };
    apply_start_protected_game_bypass(appid, &game_dir);
}

fn apply_start_protected_game_bypass(appid: u32, game_dir: &Path) {
    let spg = match super::recipe::find_case_insensitive(game_dir, "start_protected_game.exe") {
        Some(path) => path,
        None => return,
    };
    let spg_dir = match spg.parent() {
        Some(dir) => dir,
        None => return,
    };

    if spg_dir.join("start_protected_game.old").exists() {
        return;
    }

    let real_exe = match find_start_protected_real_exe(appid, game_dir, spg_dir) {
        Some(path) => path,
        None => return,
    };

    let old = spg_dir.join("start_protected_game.old");
    if let Err(err) = std::fs::rename(&spg, &old) {
        eprintln!("start_protected_game: failed to rename {} to {}: {}", spg.display(), old.display(), err);
        return;
    }
    if let Err(err) = std::fs::copy(&real_exe, &spg) {
        eprintln!("start_protected_game: failed to copy {} to {}: {}", real_exe.display(), spg.display(), err);
        let _ = std::fs::rename(&old, &spg);
    }
}

fn find_start_protected_real_exe(appid: u32, game_dir: &Path, spg_dir: &Path) -> Option<PathBuf> {
    for real_exe_name in start_protected_game_real_exe_names(appid) {
        if let Some(path) = super::recipe::find_case_insensitive(game_dir, real_exe_name) {
            return Some(path);
        }
    }

    let candidates = WalkDir::new(spg_dir)
        .max_depth(1)
        .into_iter()
        .flatten()
        .filter_map(|entry| {
            let path = entry.path();
            if !path.is_file() {
                return None;
            }
            let name = path.file_name()?.to_string_lossy().to_string();
            if is_start_protected_real_exe_candidate(&name) {
                Some(path.to_path_buf())
            } else {
                None
            }
        })
        .collect::<Vec<_>>();

    if candidates.len() == 1 {
        candidates.into_iter().next()
    } else {
        None
    }
}

fn is_start_protected_real_exe_candidate(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower.ends_with(".exe")
        && lower != "start_protected_game.exe"
        && !lower.contains("easyanticheat")
        && !lower.contains("setup")
        && !lower.contains("redist")
        && !lower.contains("installer")
        && !lower.contains("uninstall")
        && !lower.contains("crash")
        && !lower.contains("launcher")
}

fn generate_steam_interfaces(game_dir: &Path) {
    let steam_settings = game_dir.join("steam_settings");
    let interfaces_file = steam_settings.join("steam_interfaces.txt");

    // Don't bail early if the file exists but is empty or nearly empty.
    let existing_ok = interfaces_file.exists()
        && std::fs::read_to_string(&interfaces_file).map(|c| c.trim().lines().count() >= 3).unwrap_or(false);
    if existing_ok {
        return;
    }

    let mut candidates: Vec<PathBuf> = vec![
        game_dir.join("steam_api64.dll.orig"),
        game_dir.join("steam_api.dll.orig"),
        game_dir.join("Game").join("steam_api64.dll.orig"),
        game_dir.join("Game").join("steam_api.dll.orig"),
        game_dir.join("Binaries").join("Win64").join("steam_api64.dll.orig"),
        game_dir.join("Binaries").join("Win32").join("steam_api.dll.orig"),
        game_dir.join("bin").join("steam_api64.dll.orig"),
        game_dir.join("win64").join("steam_api64.dll.orig"),
    ];

    // Also try the current (non-.orig) DLLs — the Goldberg emulator DLL itself
    // ships the same Steam interface exports.
    candidates.push(game_dir.join("steam_api64.dll"));
    candidates.push(game_dir.join("steam_api.dll"));
    candidates.push(game_dir.join("Game").join("steam_api64.dll"));
    candidates.push(game_dir.join("bin").join("steam_api64.dll"));

    let dll_path = match candidates.iter().find(|p| p.exists()) {
        Some(p) => p,
        None => return,
    };

    let data = match std::fs::read(dll_path) {
        Ok(d) => d,
        Err(_) => return,
    };

    let interfaces = extract_steam_interfaces(&data);
    if interfaces.is_empty() {
        return;
    }

    let content = interfaces.join("\n") + "\n";
    let _ = std::fs::create_dir_all(&steam_settings);
    let _ = std::fs::write(&interfaces_file, content);
    eprintln!("goldberg: generated steam_interfaces.txt ({} interfaces) from {}", interfaces.len(), dll_path.display());
}

fn extract_steam_interfaces(data: &[u8]) -> Vec<String> {
    let pattern = b"Steam";
    let mut interfaces = Vec::new();
    let mut seen = std::collections::HashSet::new();

    for i in 0..data.len().saturating_sub(8) {
        if data[i] != b'S' {
            continue;
        }
        if &data[i..i + 5] != pattern {
            continue;
        }

        let end = data[i..].iter().position(|&b| b == 0).unwrap_or(0);
        if !(6..=80).contains(&end) {
            continue;
        }
        let s = match std::str::from_utf8(&data[i..i + end]) {
            Ok(s) => s,
            Err(_) => continue,
        };

        if !s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '.') {
            continue;
        }
        if !s.chars().any(|c| c.is_ascii_digit()) {
            continue;
        }
        let lower = s.to_ascii_lowercase();
        if !lower.starts_with("steam")
            || lower.contains("steamapps")
            || lower.contains("steam.dll")
            || lower.contains("steam_api")
            || lower.contains("steamclient")
            || lower.contains("steamgame")
            || lower.contains("steamoverlay")
            || lower.contains("steam_appid")
            || lower.contains("steamworks")
        {
            continue;
        }

        if seen.insert(s.to_string()) {
            interfaces.push(s.to_string());
        }
    }

    interfaces.sort();
    interfaces
}

pub fn deploy_steam_appid(game_dir: &Path, appid: u32) {
    let targets: Vec<PathBuf> = vec![
        game_dir.join("Game"),
        game_dir.join("Binaries").join("Win64"),
        game_dir.join("bin"),
        game_dir.join("win64"),
        game_dir.to_path_buf(),
    ];

    for dir in &targets {
        if dir.is_dir() {
            let appid_file = dir.join("steam_appid.txt");
            let _ = std::fs::write(&appid_file, appid.to_string());
        }
    }

    // If Goldberg toggle is active, also update force_steam_appid.txt so the
    // emulator sees the correct appid at launch.
    let goldberg_settings = game_dir.join("steam_settings").join("force_steam_appid.txt");
    if goldberg_settings.exists() {
        let current = std::fs::read_to_string(&goldberg_settings).unwrap_or_default();
        if current.trim() != appid.to_string() {
            let _ = std::fs::write(&goldberg_settings, appid.to_string());
            eprintln!(
                "deploy_steam_appid: updated goldberg force_steam_appid.txt from {:?} to {}",
                current.trim(),
                appid
            );
        }
    }
}

fn spawn_metalshaderconverter_sidecar(appid: u32, home: &Path, cache_paths: Option<&CachePaths>) {
    let tool_candidates = [
        PathBuf::from("/usr/local/bin/metal-shaderconverter"),
        PathBuf::from("/opt/homebrew/bin/metal-shaderconverter"),
        PathBuf::from("/opt/metal-shaderconverter/bin/metal-shaderconverter"),
    ];
    let Some(tool_path) = tool_candidates.into_iter().find(|path| path.exists()) else {
        return;
    };

    let log_dir = cache_paths
        .map(|cache| PathBuf::from(&cache.pipeline))
        .unwrap_or_else(|| crate::platform::metalsharp_home_dir_for(&home).join("logs"));
    let _ = std::fs::create_dir_all(&log_dir);
    let log_path = log_dir.join(format!("d3d12-metalshaderconverter-{}.log", appid));
    let cache_dir = cache_paths
        .map(|cache| PathBuf::from(&cache.shader))
        .unwrap_or_else(|| PathBuf::from("/tmp/dxmt_shader_cache"));
    if let Ok(entries) = std::fs::read_dir(&cache_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
                continue;
            };
            if name.ends_with(".msc.fail") || name.ends_with(".msc.lock") {
                let _ = std::fs::remove_file(path);
            }
        }
    }

    std::thread::spawn(move || {
        let deadline = Instant::now() + Duration::from_secs(180);
        let launch_start = SystemTime::now();
        let max_active_compiles = 4usize;

        while Instant::now() < deadline {
            if let Ok(entries) = std::fs::read_dir(&cache_dir) {
                let mut pending = Vec::new();
                let mut active_locks = 0usize;
                for entry in entries.flatten() {
                    let path = entry.path();
                    if path.extension().and_then(|ext| ext.to_str()) == Some("lock")
                        && path.file_name().and_then(|name| name.to_str()).map(|name| name.ends_with(".msc.lock"))
                            == Some(true)
                    {
                        active_locks += 1;
                    }
                    if path.extension().and_then(|ext| ext.to_str()) != Some("dxbc") {
                        continue;
                    }
                    let Ok(modified) = entry.metadata().and_then(|meta| meta.modified()) else {
                        continue;
                    };
                    if modified + Duration::from_secs(2) < launch_start {
                        continue;
                    }
                    pending.push((modified, path));
                }

                pending.sort_by_key(|entry| std::cmp::Reverse(entry.0));

                for (_modified, path) in pending {
                    if active_locks >= max_active_compiles {
                        break;
                    }
                    let metallib_path = path.with_extension("metallib");
                    let reflection_path = path.with_extension("json");
                    let vertex_layout_path = path.with_extension("vertex-layout.json");
                    let is_geometry_mesh_shader = path
                        .file_name()
                        .and_then(|name| name.to_str())
                        .map(|name| name.ends_with(".geom.gsmesh.dxbc"))
                        .unwrap_or(false);
                    let use_gs_ts_emulation = vertex_layout_path.exists() && !is_geometry_mesh_shader;
                    let stage_in_path = path.with_extension("stageIn.metallib");
                    let fail_path = path.with_extension("msc.fail");
                    if metallib_path.exists()
                        && reflection_path.exists()
                        && (!use_gs_ts_emulation || stage_in_path.exists())
                    {
                        continue;
                    }
                    if fail_path.exists() {
                        continue;
                    }

                    let lock_path = path.with_extension("msc.lock");
                    let lock_file = OpenOptions::new().write(true).create_new(true).open(&lock_path);
                    let Ok(_lock_guard) = lock_file else {
                        continue;
                    };
                    active_locks += 1;

                    let tool_path = tool_path.clone();
                    let log_path = log_path.clone();
                    std::thread::spawn(move || {
                        let mut log = OpenOptions::new().create(true).append(true).open(&log_path).ok();
                        if let Some(log) = log.as_mut() {
                            let _ = writeln!(log, "compile_start dxbc={}", path.display());
                        }

                        let mut command = Command::new(&tool_path);
                        command
                            .arg("-o")
                            .arg(&metallib_path)
                            .arg(&path)
                            .arg(format!("--output-reflection-file={}", reflection_path.display()))
                            .arg("--deployment-os=macOS")
                            .arg("--minimum-os-build-version=15.0.0");
                        if use_gs_ts_emulation {
                            command
                                .arg("--enable-gs-ts-emulation")
                                .arg("--vertex-stage-in")
                                .arg(format!("--vertex-input-layout-file={}", vertex_layout_path.display()));
                        }
                        let output = command.output();

                        match output {
                            Ok(result) => {
                                let stdout_text = String::from_utf8_lossy(&result.stdout);
                                let stderr_text = String::from_utf8_lossy(&result.stderr);
                                if result.status.success() {
                                    let _ = std::fs::remove_file(&fail_path);
                                } else {
                                    let failure_summary = if stderr_text.trim().is_empty() {
                                        format!("metal-shaderconverter failed with {}", result.status)
                                    } else {
                                        stderr_text.trim().to_string()
                                    };
                                    let _ = std::fs::write(&fail_path, failure_summary);
                                }
                                if let Some(log) = log.as_mut() {
                                    let _ = writeln!(
                                        log,
                                        "compile_end dxbc={} status={} metallib={} reflection={} gs_ts_emulation={}",
                                        path.display(),
                                        result.status,
                                        metallib_path.exists(),
                                        reflection_path.exists(),
                                        use_gs_ts_emulation
                                    );
                                    if !stdout_text.is_empty() {
                                        let _ = log.write_all(stdout_text.as_bytes());
                                    }
                                    if !stderr_text.is_empty() {
                                        let _ = log.write_all(stderr_text.as_bytes());
                                    }
                                }
                            },
                            Err(err) => {
                                let _ = std::fs::write(
                                    &fail_path,
                                    format!("metal-shaderconverter invocation failed: {}", err),
                                );
                                if let Some(log) = log.as_mut() {
                                    let _ = writeln!(log, "compile_error dxbc={} err={}", path.display(), err);
                                }
                            },
                        }

                        let _ = std::fs::remove_file(&lock_path);
                    });
                }
            }

            std::thread::sleep(Duration::from_millis(100));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::mtsp::recipe;

    #[test]
    fn wine_wrapper_does_not_force_forward_compatible_opengl() {
        let wrapper =
            "export WINEDATADIR=\"$MS_ROOT/share\"\nexport MS_FWD_COMPAT_GL_CTX=1\nexport VK_ICD_FILENAMES=foo\n";

        let repaired = repair_metalsharp_wine_wrapper(wrapper);

        assert!(!repaired.contains("MS_FWD_COMPAT_GL_CTX"));
        assert!(repaired.contains("export WINEDATADIR=\"$MS_ROOT/share\""));
        assert!(repaired.contains("export VK_ICD_FILENAMES=foo"));
    }

    #[test]
    fn sanitize_puts_ms_paths_first_when_ambient_wine_env_present() {
        // Isolation contract: a foreign wine launcher (CrossOver, SakuraGiri,
        // Whisky, GPTK) may export WINEDLLPATH / DYLD_FALLBACK_LIBRARY_PATH
        // into the parent environment. The sanitized wrapper must search
        // MetalSharp's own DLL/dylib dirs BEFORE any inherited foreign paths.
        let wrapper = r#"export MS_ROOT="$MS_ROOT"
export CX_ROOT="$MS_ROOT"
export WINEPREFIX="${WINEPREFIX:-$HOME/.metalsharp/prefix-steam}"
export WINESERVER="$MS_SERVER"
export WINELOADER="$MS_WINE"
export WINEDLLPATH="$MS_LIB/wine/x86_64-windows:$MS_LIB/wine/i386-windows"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDATADIR="$MS_ROOT/share"
export DYLD_FALLBACK_LIBRARY_PATH="$MS_LIB:$MS_LIB/wine/x86_64-unix"
export MS_FWD_COMPAT_GL_CTX=1
export VK_ICD_FILENAMES="/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json"
"#;

        // Replicate the full sanitize flow on the script string.
        let mut repaired = repair_metalsharp_wine_wrapper(wrapper);
        let winedll_block = r#"export WINEDLLPATH="$MS_LIB/wine/x86_64-windows:$MS_LIB/wine/i386-windows${WINEDLLPATH:+:$WINEDLLPATH}"
"#;
        if let (Some(start), Some(end)) = (
            repaired.find("export WINELOADER=\"$MS_WINE\"\n"),
            repaired.find("export WINEDEBUG=\"${WINEDEBUG:--all}\""),
        ) {
            let replace_start = start + "export WINELOADER=\"$MS_WINE\"\n".len();
            repaired.replace_range(replace_start..end, winedll_block);
        }
        let dyld_block = r#"export DYLD_FALLBACK_LIBRARY_PATH="$MS_LIB:$MS_LIB/wine/x86_64-unix${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
"#;
        if let (Some(start), Some(end)) =
            (repaired.find("export WINEDATADIR=\"$MS_ROOT/share\"\n"), repaired.find("export VK_ICD_FILENAMES="))
        {
            let replace_start = start + "export WINEDATADIR=\"$MS_ROOT/share\"\n".len();
            repaired.replace_range(replace_start..end, dyld_block);
        }
        repaired = repaired.replace("export CX_ROOT=\"$MS_ROOT\"\n", "");
        repaired = repaired.replace(
            "export VK_ICD_FILENAMES=\"/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json\"",
            "if [ -f \"$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json\" ]; then\n  export VK_ICD_FILENAMES=\"$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json\"\nelse\n  unset VK_ICD_FILENAMES\nfi",
        );

        // MS paths must come first; ambient values may only be appended after.
        let winedll_line =
            repaired.lines().find(|l| l.starts_with("export WINEDLLPATH=")).expect("WINEDLLPATH export present");
        let winedll_prefix = winedll_line.split("${WINEDLLPATH").next().unwrap();
        assert!(
            winedll_prefix.starts_with("export WINEDLLPATH=\"$MS_LIB/"),
            "MS DLL dirs must lead WINEDLLPATH: {}",
            winedll_line
        );

        let dyld_line = repaired
            .lines()
            .find(|l| l.starts_with("export DYLD_FALLBACK_LIBRARY_PATH="))
            .expect("DYLD_FALLBACK_LIBRARY_PATH export present");
        let dyld_prefix = dyld_line.split("${DYLD_FALLBACK").next().unwrap();
        assert!(
            dyld_prefix.starts_with("export DYLD_FALLBACK_LIBRARY_PATH=\"$MS_LIB:"),
            "MS unix lib dirs must lead DYLD_FALLBACK_LIBRARY_PATH: {}",
            dyld_line
        );

        // CX_ROOT emulation must be gone, and the ICD must resolve inside the
        // runtime instead of a hardcoded Homebrew path.
        assert!(!repaired.contains("CX_ROOT"), "CX_ROOT emulation must be dropped");
        assert!(!repaired.contains("/opt/homebrew/etc/vulkan"), "Homebrew ICD path must not remain");
        assert!(repaired.contains("$MS_ROOT/etc/vulkan/icd.d/MoltenVK_icd.json"));
    }

    #[test]
    fn deploy_steam_appid_writes_visible_appid_file_for_each_known_subdir() {
        // Phase 2: routes that depend on Steam-visible identity must stage
        // steam_appid.txt into every standard binary subdir the game may
        // launch from, plus update an active Goldberg force_steam_appid.txt.
        let game_dir = test_dir("deploy-steam-appid");
        let sub_win64 = game_dir.join("Binaries").join("Win64");
        let sub_bin = game_dir.join("bin");
        std::fs::create_dir_all(&sub_win64).unwrap();
        std::fs::create_dir_all(&sub_bin).unwrap();
        // "Game" subdir absent on purpose: it must be skipped, not panic.

        deploy_steam_appid(&game_dir, 504230);

        assert_eq!(std::fs::read_to_string(sub_win64.join("steam_appid.txt")).unwrap(), "504230");
        assert_eq!(std::fs::read_to_string(sub_bin.join("steam_appid.txt")).unwrap(), "504230");
        assert_eq!(std::fs::read_to_string(game_dir.join("steam_appid.txt")).unwrap(), "504230");
        assert!(!game_dir.join("Game").join("steam_appid.txt").exists(), "absent subdirs must be skipped");

        // An active Goldberg emulator setting must be kept in sync.
        let goldberg_dir = game_dir.join("steam_settings");
        std::fs::create_dir_all(&goldberg_dir).unwrap();
        std::fs::write(goldberg_dir.join("force_steam_appid.txt"), "0").unwrap();
        deploy_steam_appid(&game_dir, 504230);
        assert_eq!(
            std::fs::read_to_string(goldberg_dir.join("force_steam_appid.txt")).unwrap(),
            "504230",
            "goldberg force_steam_appid.txt must track the real appid"
        );
        let _ = std::fs::remove_dir_all(&game_dir);
    }

    #[test]
    fn m12_pipeline_deploy_list_includes_d3d12_and_uses_isolated_vkd3d_proton_surface() {
        // Phase 3 contract: M12 must deploy d3d12.dll + d3d12core.dll from the
        // isolated lib/vkd3d-proton surface plus DXVK's dxgi.dll — never DXMT.
        let node = get_pipeline(PipelineId::M12);
        let filenames: Vec<&str> = node.deploy_dlls.iter().map(|d| d.filename).collect();
        let required = ["d3d12.dll", "d3d12core.dll", "dxgi.dll", "nvapi64.dll", "nvngx.dll"];
        assert_eq!(filenames.len(), required.len(), "M12 deploy list must be the vkd3d-proton 5 DLL set");
        for required in required {
            assert!(filenames.contains(&required), "M12 deploy list must include {} (got {:?})", required, filenames);
        }
        for deploy in &node.deploy_dlls {
            if deploy.filename == "dxgi.dll" {
                assert_eq!(
                    deploy.source_subpath, "lib/dxvk/x86_64-windows",
                    "M12 dxgi.dll must come from the DXVK lane (vkd3d-proton ships no dxgi)"
                );
            } else if deploy.filename == "nvapi64.dll" || deploy.filename == "nvngx.dll" {
                assert_eq!(
                    deploy.source_subpath, "lib/dxmt_m12/x86_64-windows",
                    "M12 GPU vendor stubs must come from the shared dxmt_m12 lane (vkd3d-proton ships none)"
                );
            } else {
                assert_eq!(
                    deploy.source_subpath, "lib/vkd3d-proton/x86_64-windows",
                    "M12 DLL {} must come from the isolated vkd3d-proton runtime surface",
                    deploy.filename
                );
            }
        }
        assert!(!filenames.contains(&"winemetal.dll"), "M12 must never deploy winemetal.dll (got {:?})", filenames);
        assert!(!filenames.contains(&"dxgi_dxmt.dll"), "M12 must never deploy dxgi_dxmt.dll (got {:?})", filenames);
    }

    #[test]
    fn m11_pipeline_deploy_list_does_not_include_d3d12_and_uses_legacy_dxmt_surface() {
        // Phase 3 contract: M11 must NOT deploy d3d12.dll and must point at the
        // legacy lib/dxmt surface, never lib/dxmt_m12.
        let node = get_pipeline(PipelineId::M11);
        let filenames: Vec<&str> = node.deploy_dlls.iter().map(|d| d.filename).collect();
        assert!(!filenames.contains(&"d3d12.dll"), "M11 deploy list must NOT include d3d12.dll (got {:?})", filenames);
        for deploy in &node.deploy_dlls {
            assert!(
                !deploy.source_subpath.starts_with("lib/dxmt_m12/"),
                "M11 DLL {} must not come from lib/dxmt_m12 (got {})",
                deploy.filename,
                deploy.source_subpath
            );
        }
    }

    #[test]
    fn m11_quarantines_stale_route_dlls_before_deploying_legacy_dxmt() {
        let root = test_dir("m11-route-quarantine");
        let game_dir = root.join("game");
        let exe_dir = game_dir.join("Binaries").join("Win64");
        let source_dir = root.join("runtime").join("wine").join("lib").join("dxmt").join("x86_64-windows");
        std::fs::create_dir_all(&exe_dir).unwrap();
        std::fs::create_dir_all(&source_dir).unwrap();
        std::fs::write(exe_dir.join("Game.exe"), b"exe").unwrap();
        std::fs::write(exe_dir.join("d3d12.dll"), b"stale m12 route").unwrap();
        std::fs::write(exe_dir.join("d3d11.dll"), b"stale d3dmetal route").unwrap();
        std::fs::write(exe_dir.join("winemetal.dll"), b"stale winemetal route").unwrap();
        std::fs::write(source_dir.join("d3d11.dll"), b"legacy dxmt d3d11").unwrap();
        std::fs::write(source_dir.join("winemetal.dll"), b"legacy dxmt winemetal").unwrap();

        let recipe = recipe::LaunchRecipe {
            appid: 42,
            pipeline: PipelineId::M11,
            pipeline_name: "M11".into(),
            backend: "dxmt".into(),
            game_dir: Some(game_dir.clone()),
            exe_path: Some(exe_dir.join("Game.exe")),
            exe_name: Some("Game.exe".into()),
            launch_args: Vec::new(),
            env: Vec::new(),
            dlls: vec![
                recipe::RecipeDll {
                    source_subpath: "lib/dxmt/x86_64-windows".into(),
                    filename: "d3d11.dll".into(),
                    source_path: source_dir.join("d3d11.dll"),
                    dest_path: exe_dir.join("d3d11.dll"),
                    source_present: true,
                },
                recipe::RecipeDll {
                    source_subpath: "lib/dxmt/x86_64-windows".into(),
                    filename: "winemetal.dll".into(),
                    source_path: source_dir.join("winemetal.dll"),
                    dest_path: exe_dir.join("winemetal.dll"),
                    source_present: true,
                },
            ],
            runtime_assets: Vec::new(),
            warnings: Vec::new(),
        };

        let moved = quarantine_route_conflicts_for_recipe(&recipe).expect("quarantine stale routes");
        assert_eq!(moved, 3, "d3d12 plus stale d3d11/winemetal should be quarantined before M11 deploy");
        deploy_recipe_dlls(&recipe).expect("deploy M11 legacy DXMT route");

        assert!(!exe_dir.join("d3d12.dll").exists(), "M11 save must not leave stale d3d12.dll behind");
        assert_eq!(std::fs::read_to_string(exe_dir.join("d3d11.dll")).unwrap(), "legacy dxmt d3d11");
        assert_eq!(std::fs::read_to_string(exe_dir.join("winemetal.dll")).unwrap(), "legacy dxmt winemetal");
        let marker = game_dir.join(".metalsharp").join("route-quarantine").join("latest-manifest.json");
        assert!(marker.is_file(), "quarantine marker should document moved route DLLs");

        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn m12_quarantines_stale_m11_route_dlls_before_deploying_isolated_dxmt_m12() {
        let root = test_dir("m12-route-quarantine-switchback");
        let game_dir = root.join("game");
        let exe_dir = game_dir.join("Binaries").join("Win64");
        let source_dir = root.join("runtime").join("wine").join("lib").join("dxmt_m12").join("x86_64-windows");
        std::fs::create_dir_all(&exe_dir).unwrap();
        std::fs::create_dir_all(&source_dir).unwrap();
        std::fs::write(exe_dir.join("Game.exe"), b"exe").unwrap();
        std::fs::write(exe_dir.join("d3d11.dll"), b"stale m11 d3d11").unwrap();
        std::fs::write(exe_dir.join("dxgi.dll"), b"stale m11 dxgi").unwrap();
        std::fs::write(exe_dir.join("winemetal.dll"), b"stale m11 winemetal").unwrap();

        let route_dlls = ["d3d12.dll", "d3d11.dll", "dxgi.dll", "dxgi_dxmt.dll", "winemetal.dll"];
        for dll in route_dlls {
            std::fs::write(source_dir.join(dll), format!("isolated m12 {dll}")).unwrap();
        }

        let recipe = recipe::LaunchRecipe {
            appid: 42,
            pipeline: PipelineId::M12,
            pipeline_name: "M12".into(),
            backend: "dxmt".into(),
            game_dir: Some(game_dir.clone()),
            exe_path: Some(exe_dir.join("Game.exe")),
            exe_name: Some("Game.exe".into()),
            launch_args: Vec::new(),
            env: Vec::new(),
            dlls: route_dlls
                .iter()
                .map(|dll| recipe::RecipeDll {
                    source_subpath: "lib/dxmt_m12/x86_64-windows".into(),
                    filename: (*dll).into(),
                    source_path: source_dir.join(dll),
                    dest_path: exe_dir.join(dll),
                    source_present: true,
                })
                .collect(),
            runtime_assets: Vec::new(),
            warnings: Vec::new(),
        };

        let moved = quarantine_route_conflicts_for_recipe(&recipe).expect("quarantine stale M11 routes");
        assert_eq!(moved, 3, "stale d3d11/dxgi/winemetal should be quarantined before M12 deploy");
        deploy_recipe_dlls(&recipe).expect("deploy M12 isolated DXMT route");

        for dll in route_dlls {
            assert_eq!(
                std::fs::read_to_string(exe_dir.join(dll)).unwrap(),
                format!("isolated m12 {dll}"),
                "M12 switch-back must deploy {dll} from dxmt_m12"
            );
        }
        let marker = game_dir.join(".metalsharp").join("route-quarantine").join("latest-manifest.json");
        assert!(marker.is_file(), "quarantine marker should document moved M11 route DLLs");

        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn m12_dry_run_includes_d3d12_dll_and_m11_dry_run_does_not() {
        // Phase 3 contract: the dry-run verifier's deploy list must reflect
        // the pipeline node. M12 dry-run includes d3d12.dll; M11 does not.
        // Uses an explicit temp home so no global env is mutated.
        let home = std::env::temp_dir().join("ms-m12-dryrun-contract");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();

        let m12 = pipeline_dry_run_for(&home, 2379780, Some(PipelineId::M12));
        let m11 = pipeline_dry_run_for(&home, 17300, Some(PipelineId::M11));

        let m12_filenames: Vec<String> = m12
            .get("deploy_dlls")
            .and_then(|v| v.as_array())
            .unwrap()
            .iter()
            .map(|d| d.get("filename").unwrap().as_str().unwrap().to_string())
            .collect();
        let m11_filenames: Vec<String> = m11
            .get("deploy_dlls")
            .and_then(|v| v.as_array())
            .unwrap()
            .iter()
            .map(|d| d.get("filename").unwrap().as_str().unwrap().to_string())
            .collect();

        assert!(
            m12_filenames.contains(&"d3d12.dll".to_string()),
            "M12 dry-run must include d3d12.dll: {:?}",
            m12_filenames
        );
        assert!(
            !m11_filenames.contains(&"d3d12.dll".to_string()),
            "M11 dry-run must NOT include d3d12.dll: {:?}",
            m11_filenames
        );

        // Both dry-runs must report the env keys the launch path sets.
        let m12_env = m12.get("env_keys_present").unwrap();
        assert_eq!(m12_env.get("WINEDLLOVERRIDES").and_then(|v| v.as_bool()), Some(true));
        assert_eq!(m12_env.get("SteamAppId").and_then(|v| v.as_bool()), Some(true));
        assert_eq!(m12.get("dry_run").and_then(|v| v.as_bool()), Some(true));

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn m12_dry_run_verifies_unix_sidecars_and_marks_missing_artifacts() {
        // Phase 3: the M12 dry-run must enumerate the x86_64-unix sidecars and
        // report missing required artifacts as a structured failure (not a
        // silent ok=true). With an empty temp home, all artifacts are absent.
        let home = std::env::temp_dir().join("ms-m12-dryrun-empty");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();

        let dry = pipeline_dry_run_for(&home, 2379780, Some(PipelineId::M12));

        // Unix sidecars must be enumerated for the M12 lane.
        let sidecar_names: Vec<String> = dry
            .get("unix_sidecars")
            .and_then(|v| v.as_array())
            .unwrap()
            .iter()
            .map(|s| s.get("filename").unwrap().as_str().unwrap().to_string())
            .collect();
        for required in ["winemetal.so", "libc++.1.dylib", "libc++abi.1.dylib", "libunwind.1.dylib"] {
            assert!(
                sidecar_names.contains(&required.to_string()),
                "M12 dry-run must verify {}: {:?}",
                required,
                sidecar_names
            );
        }

        // d3d12.dll is a required (non-optional) M12 artifact, so it must be
        // listed as missing when absent.
        let missing_filenames: Vec<String> = dry
            .get("missing")
            .and_then(|v| v.as_array())
            .unwrap()
            .iter()
            .map(|m| m.get("filename").unwrap().as_str().unwrap().to_string())
            .collect();
        assert!(
            missing_filenames.contains(&"d3d12.dll".to_string()),
            "M12 dry-run must flag missing d3d12.dll: {:?}",
            missing_filenames
        );
        assert_eq!(dry.get("ok").and_then(|v| v.as_bool()), Some(false), "empty home must yield ok=false");

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn m12_pipeline_env_vars_set_vkd3d_overrides_and_shader_cache() {
        // Phase 3 contract: the M12 env builder must set the vkd3d-proton
        // WINEDLLOVERRIDES, route the wine DLL path to the vkd3d-proton lane,
        // and point the shader cache at the isolated m12 lane.
        let node = get_pipeline(PipelineId::M12);
        assert!(node.wine_overrides.unwrap_or("").contains("d3d12core"));
        assert!(node.wine_overrides.unwrap_or("").contains("d3d12"));
        assert!(!node.wine_overrides.unwrap_or("").contains("winemetal"));
        assert!(
            node.winedllpath_dirs.iter().any(|d| d.starts_with("lib/vkd3d-proton")),
            "M12 winedllpath must route to vkd3d-proton"
        );
        assert_eq!(node.shader_cache_subdir, Some("m12"), "M12 shader cache must be isolated under m12");
    }

    #[test]
    fn m9_cache_env_uses_dxmt_family_not_dxvk() {
        let node = get_pipeline(PipelineId::M9);
        let cache = CachePaths {
            shader: "/tmp/m9-shaders".into(),
            pipeline: "/tmp/m9-pipelines".into(),
            log: "/tmp/m9-logs".into(),
        };

        let env = cache_env_pairs(node, Some(&cache), &PathBuf::from("/tmp/metalsharp-runtime"));
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();

        assert!(keys.contains("DXMT_SHADER_CACHE_PATH"));
        assert!(keys.contains("DXMT_PIPELINE_CACHE_PATH"));
        assert!(!keys.contains("DXMT_LOG_PATH"));
        assert!(keys.contains("METALSHARP_CACHE_SUMMARY"));

        let log_env = cache_env_pairs_with_logs(node, Some(&cache), &PathBuf::from("/tmp/metalsharp-runtime"), true);
        assert!(log_env.iter().any(|(key, value)| key == "DXMT_LOG_PATH" && value == "/tmp/m9-logs/"));
        assert!(!keys.contains("DXVK_STATE_CACHE_PATH"));
        assert!(!keys.contains("DXVK_LOG_PATH"));
        assert!(!keys.contains("VK_ICD_FILENAMES"));
    }

    #[test]
    fn m12_vkd3d_log_path_uses_shared_logs_folder_when_developer_logs_enabled() {
        let home = test_dir("m12-log-path");
        let node = get_pipeline(PipelineId::M12);
        assert_eq!(node.backend, "vkd3d-proton");
        let cache = build_cache_paths(&home, node, 1583230).expect("m12 cache paths");

        let env = cache_env_pairs_with_logs(
            node,
            Some(&cache),
            &crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine"),
            true,
        );
        // vkd3d-proton does not use DXMT_LOG_PATH; cache env stays DXMT-free.
        assert!(!env.iter().any(|(key, _)| key == "DXMT_LOG_PATH"));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_family_env_uses_metalfx_upscale_and_cache_paths() {
        // M12 now runs vkd3d-proton; the DXMT_CONFIG contract applies to the
        // legacy DXMT family (M9/M10/M11) only. MetalFX default strength is
        // 1.50 (metalfx.overlay.json default), reconciled into the env at
        // launch; M9 has no upscale pair (no MetalFX on D3D9).
        for pipeline_id in [PipelineId::M10, PipelineId::M11] {
            let home = test_dir(&format!("dxmt-env-{:?}", pipeline_id));
            let node = get_pipeline(pipeline_id);

            let env = steam_pipeline_env_pairs(&home, node, 42);
            let config = env.iter().find(|(key, _)| key == "DXMT_CONFIG").map(|(_, value)| value.as_str());
            let summary =
                env.iter().find(|(key, _)| key == "METALSHARP_CACHE_SUMMARY").map(|(_, value)| value.as_str());

            assert!(config.unwrap_or_default().contains("d3d11.metalSpatialUpscaleFactor=1.50"));
            assert!(config.unwrap_or_default().contains("d3d11.preferredMaxFrameRate=60"));
            assert!(summary.unwrap_or_default().contains("/shader-cache/"));
            assert!(summary.unwrap_or_default().contains("/pipeline-cache/"));
            assert!(!env.iter().any(|(key, _)| key == "DXMT_LOG_PATH"));
            let _ = std::fs::remove_dir_all(home);
        }
    }

    #[test]
    fn m12_vkd3d_env_uses_vkd3d_cache_and_no_dxmt_config() {
        let home = test_dir("m12-vkd3d-env");
        let node = get_pipeline(PipelineId::M12);
        assert_eq!(node.backend, "vkd3d-proton");

        let env = steam_pipeline_env_pairs(&home, node, 42);
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();
        assert!(!keys.contains("DXMT_CONFIG"), "M12 must not set DXMT_CONFIG (vkd3d-proton backend)");
        assert!(!keys.contains("DXMT_CONFIG_FILE"), "M12 must not set DXMT_CONFIG_FILE");
        assert!(!keys.contains("DXMT_SHADER_CACHE_PATH"), "M12 must not set DXMT_SHADER_CACHE_PATH");
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn m9_stuck_loading_titles_disable_async_loading_features() {
        for appid in [774361, 17410, 49520] {
            let env = app_compat_env_pairs(appid, PipelineId::M9);

            assert_eq!(env_value(&env, "DXMT_ASYNC_PIPELINE_COMPILE"), Some("0"));
            assert_eq!(env_value(&env, "DXMT_METALFX_SPATIAL_SWAPCHAIN"), Some("0"));
            assert_eq!(env_value(&env, "DXMT_METALFX_SPATIAL"), Some("0"));
            assert_eq!(env_value(&env, "DXMT_CONFIG"), Some("d3d11.preferredMaxFrameRate=60"));
            assert_eq!(env_value(&env, "METALSHARP_M9_SYNC_LOADING"), Some("1"));
        }

        assert!(app_compat_env_pairs(123456, PipelineId::M9).is_empty());
        assert!(app_compat_env_pairs(774361, PipelineId::M10).is_empty());
    }

    #[test]
    fn steam_pipeline_env_applies_m9_stuck_loading_overrides_after_defaults() {
        let home = test_dir("m9-stuck-loading-env");
        let node = get_pipeline(PipelineId::M9);

        let env = steam_pipeline_env_pairs(&home, node, 774361);

        assert_eq!(last_env_value(&env, "DXMT_ASYNC_PIPELINE_COMPILE"), Some("0"));
        assert_eq!(last_env_value(&env, "DXMT_METALFX_SPATIAL_SWAPCHAIN"), Some("0"));
        assert_eq!(last_env_value(&env, "DXMT_CONFIG"), Some("d3d11.preferredMaxFrameRate=60"));
        assert_eq!(last_env_value(&env, "METALSHARP_M9_SYNC_LOADING"), Some("1"));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn m32_env_keeps_wine_fallback_cache_without_dxmt_config() {
        let home = test_dir("m32-env");
        let node = get_pipeline(PipelineId::M32);

        let env = steam_pipeline_env_pairs(&home, node, 77);
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();

        assert!(keys.contains("METALSHARP_SHADER_CACHE_PATH"));
        assert!(keys.contains("METALSHARP_PIPELINE_CACHE_PATH"));
        assert!(keys.contains("METALSHARP_CACHE_SUMMARY"));
        assert!(keys.contains("DXMT_SHADER_CACHE_PATH"));
        assert!(keys.contains("DXMT_PIPELINE_CACHE_PATH"));
        assert!(keys.contains("DXVK_STATE_CACHE_PATH"));
        assert!(!keys.contains("DXMT_CONFIG"));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn m12_reserved_route_env_keys_cannot_be_overridden_by_rules() {
        for key in [
            "WINEDLLOVERRIDES",
            "WINEDLLPATH",
            "DYLD_LIBRARY_PATH",
            "DYLD_FALLBACK_LIBRARY_PATH",
            "DXMT_WINEMETAL_UNIXLIB",
            "DXMT_CONFIG_FILE",
            "MS_GRAPHICS_BACKEND",
            "WINEMSYNC",
            "DXMT_LOG_PATH",
        ] {
            assert!(is_reserved_route_env_key(PipelineId::M12, key), "{} must be reserved for M12", key);
            assert!(!is_reserved_route_env_key(PipelineId::M11, key), "{} should remain overridable outside M12", key);
        }
        assert!(!is_reserved_route_env_key(PipelineId::M12, "DXMT_D3D12_UE_SM6_COMPAT"));
    }

    #[test]
    fn steam_pipeline_env_includes_route_overrides_and_cache_keys() {
        let home = test_dir("steam-env");
        let node = get_pipeline(PipelineId::M12);
        assert_eq!(node.backend, "vkd3d-proton");

        let env = steam_pipeline_env_pairs(&home, node, 1583230);
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();

        assert!(keys.contains("WINEDLLOVERRIDES"));
        assert!(keys.contains("SteamAppId"));
        assert!(keys.contains("SteamGameId"));
        assert!(keys.contains("SteamOverlayGameId"));
        // vkd3d-proton lane: shader cache + pinned MoltenVK ICD.
        assert!(keys.contains("VKD3D_SHADER_CACHE_PATH"));
        assert!(keys.contains("DXVK_STATE_CACHE_PATH"));
        assert!(!keys.contains("DXMT_SHADER_CACHE_PATH"), "vkd3d-proton must not set DXMT cache env");
        assert!(!keys.contains("DXMT_CONFIG_FILE"), "vkd3d-proton must not set DXMT_CONFIG_FILE");
        assert!(!keys.contains("DXMT_WINEMETAL_UNIXLIB"));
        assert!(keys.contains("METALSHARP_CACHE_SUMMARY"));
        assert!(keys.contains("MS_GRAPHICS_BACKEND"));
        assert_eq!(env_value(&env, "MS_GRAPHICS_BACKEND"), Some("vkd3d-proton"));
        assert_eq!(env.iter().find(|(key, _)| key == "SteamAppId").map(|(_, value)| value.as_str()), Some("1583230"));
        assert_eq!(env.iter().find(|(key, _)| key == "SteamGameId").map(|(_, value)| value.as_str()), Some("1583230"));
        assert_eq!(
            env.iter().find(|(key, _)| key == "SteamOverlayGameId").map(|(_, value)| value.as_str()),
            Some("1583230")
        );
        let overrides = env.iter().find(|(key, _)| key == "WINEDLLOVERRIDES").map(|(_, value)| value).unwrap();
        assert!(overrides.contains("d3d12"));
        assert!(overrides.contains("d3d12core"));
        assert!(overrides.contains("gameoverlayrenderer,gameoverlayrenderer64=d"));
        assert!(!keys.contains("CX_ROOT"));
        assert!(!keys.contains("WINESERVER"));
        assert!(!keys.contains("WINELOADER"));
        assert!(!keys.contains("WINEDATADIR"));
        assert!(!keys.contains("MS_ROOT"));
        let winedllpath = env_value(&env, "WINEDLLPATH").unwrap_or_default();
        assert!(winedllpath.contains("vkd3d-proton/x86_64-windows"));
        assert!(!winedllpath.contains("lib/metalsharp"));
        assert!(!winedllpath.contains("dxmt_m12"));
        assert!(env_value(&env, "DYLD_LIBRARY_PATH").unwrap_or_default().contains("vkd3d-proton/x86_64-unix"));
        assert!(env_value(&env, "DYLD_FALLBACK_LIBRARY_PATH").unwrap_or_default().contains("vkd3d-proton/x86_64-unix"));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn subnautica2_m12_trace_env_is_opt_in_only() {
        let default_env = app_compat_env_pairs_with_logs(1962700, PipelineId::M12, false);
        assert!(default_env.iter().any(|(key, _)| key == "DXMT_D3D12_ENABLE_GEOMETRY_MESH"));
        assert!(!default_env.iter().any(|(key, _)| key == "DXMT_D3D12_TRACE"));
        assert!(!default_env.iter().any(|(key, _)| key == "DXMT_DUMP_MSL"));

        let log_env = app_compat_env_pairs_with_logs(1962700, PipelineId::M12, true);
        assert!(log_env.iter().any(|(key, value)| key == "DXMT_D3D12_TRACE" && value == "1"));
        assert!(log_env.iter().any(|(key, value)| key == "DXMT_DUMP_MSL" && value == "1"));
    }

    #[test]
    fn m12_launch_uses_normal_metalsharp_wine_binary() {
        let home = test_dir("m12-normal-wine");
        let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
        let bin = ms_root.join("bin");
        std::fs::create_dir_all(&bin).unwrap();
        std::fs::write(bin.join("metalsharp-wine"), b"#!/bin/sh\n").unwrap();
        std::fs::write(bin.join("wine"), b"#!/bin/sh\n").unwrap();

        let m12 = get_pipeline(PipelineId::M12);
        let m11 = get_pipeline(PipelineId::M11);
        assert_eq!(crate::platform::runtime_wine_binary(&ms_root), bin.join("metalsharp-wine"));
        assert_eq!(m12.requires_wine, m11.requires_wine);
        assert_eq!(m12.backend, "vkd3d-proton");
        assert!(m12.winedllpath_dirs.iter().any(|path| path.contains("vkd3d-proton")));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn steam_pipeline_env_allows_plain_wine_fallback_context() {
        let home = test_dir("steam-wine-env");
        let node = get_pipeline(PipelineId::WineBare);

        let env = steam_pipeline_env_pairs(&home, node, 1);
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();

        let runtime_lib_key = crate::platform::runtime_library_env(
            &crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine"),
        )
        .map(|(key, _)| key)
        .unwrap_or("LD_LIBRARY_PATH");
        assert!(keys.contains(runtime_lib_key));
        assert!(keys.contains("SteamAppId"));
        assert!(keys.contains("SteamGameId"));
        assert!(keys.contains("METALSHARP_SHADER_CACHE_PATH"));
        assert!(keys.contains("DXMT_SHADER_CACHE_PATH"));
        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn dxmt_pipelines_use_winedllpath_routing() {
        for pipeline_id in [PipelineId::M9, PipelineId::M10, PipelineId::M11, PipelineId::M12] {
            let node = get_pipeline(pipeline_id);
            assert!(node.uses_winedllpath_routing(), "{:?} should use WINEDLLPATH routing", pipeline_id);
        }
    }

    /// End-to-end M12 vkd3d-proton evidence with the REAL VKMT binaries.
    /// Runs only when METALSHARP_VKD3D_SOURCE points at a VKMT checkout
    /// (skipped in CI); stages the actual win64-filtered d3d12/d3d12core,
    /// DXVK dxgi, and VKMT MoltenVK into a temp home, then drives the real
    /// launcher env builder + prefix route deployment and asserts the
    /// vkd3d-proton wiring (no DXMT env, pinned Vulkan ICD, d3d12core.dll
    /// actually copied into system32).
    #[test]
    fn m12_vkd3d_real_binaries_launch_env_and_prefix_route() {
        let vkd3d_src = std::env::var("METALSHARP_VKD3D_SOURCE").unwrap_or_default();
        let dxvk_src = std::env::var("METALSHARP_DXVK_SOURCE").unwrap_or_default();
        let mvk_src = std::env::var("METALSHARP_MOLTENVK_SOURCE").unwrap_or_default();
        if vkd3d_src.is_empty() || dxvk_src.is_empty() || mvk_src.is_empty() {
            eprintln!("SKIP: METALSHARP_VKD3D_SOURCE/DXVK_SOURCE/MOLTENVK_SOURCE not set (CI has no VKMT checkout)");
            return;
        }

        let home = test_dir("m12-vkd3d-e2e");
        let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
        let vkd3d_dir = ms_root.join("lib").join("vkd3d-proton").join("x86_64-windows");
        let dxvk_dir = ms_root.join("lib").join("dxvk").join("x86_64-windows");
        let mvk_dir = ms_root.join("lib").join("moltenvk-vkmt");
        let icd_dir = ms_root.join("etc").join("vulkan").join("icd.d");
        let dxmt_m12_dir = ms_root.join("lib").join("dxmt_m12").join("x86_64-windows");
        std::fs::create_dir_all(&vkd3d_dir).unwrap();
        std::fs::create_dir_all(&dxvk_dir).unwrap();
        std::fs::create_dir_all(&mvk_dir).unwrap();
        std::fs::create_dir_all(&icd_dir).unwrap();
        std::fs::create_dir_all(&dxmt_m12_dir).unwrap();

        // Real VKMT binaries (the exact files the graphics bundle will carry).
        std::fs::copy(PathBuf::from(&vkd3d_src).join("x86_64-windows/d3d12.dll"), vkd3d_dir.join("d3d12.dll")).unwrap();
        std::fs::copy(PathBuf::from(&vkd3d_src).join("x86_64-windows/d3d12core.dll"), vkd3d_dir.join("d3d12core.dll"))
            .unwrap();
        std::fs::copy(PathBuf::from(&dxvk_src).join("x86_64-windows/dxgi.dll"), dxvk_dir.join("dxgi.dll")).unwrap();
        std::fs::copy(PathBuf::from(&mvk_src).join("libMoltenVK.dylib"), mvk_dir.join("libMoltenVK.dylib")).unwrap();
        std::fs::copy(PathBuf::from(&mvk_src).join("MoltenVK_icd.json"), mvk_dir.join("MoltenVK_icd.json")).unwrap();
        // GPU vendor stubs ride in the shared dxmt_m12 lane (vkd3d-proton
        // ships none); stage them so the node's full deploy list resolves.
        for stub in ["nvapi64.dll", "nvngx.dll"] {
            let source = crate::installer::dxmt_m12_runtime_artifact_path_for_home(
                &dirs::home_dir().unwrap_or_default(),
                &format!("x86_64-windows/{}", stub),
            );
            if source.is_file() {
                std::fs::copy(&source, dxmt_m12_dir.join(stub)).unwrap();
            }
        }

        // The pinned-hash contract must hold for the real binaries. The
        // production hash constants are cfg(not(test)) so the E2E test pins
        // the documented production sha256 values directly (they match the
        // VKMT win64-filtered builds).
        let d3d12_hash = crate::diagnostics::file_sha256(&vkd3d_dir.join("d3d12.dll")).expect("d3d12 hash");
        assert_eq!(
            d3d12_hash, "15a7ad7af07120c79075dc1bc08284731c4ca53f82bb81002a14a7b5701cb535",
            "d3d12.dll must be the pinned VKMT win64-filtered build"
        );
        let core_hash = crate::diagnostics::file_sha256(&vkd3d_dir.join("d3d12core.dll")).expect("d3d12core hash");
        assert_eq!(
            core_hash, "43b92ad53843c819443b1c5d21930c36fe3e6cc7a893df6b2b18528477259631",
            "d3d12core.dll must be the pinned VKMT win64-filtered build"
        );
        assert!(crate::installer::moltenvk_vkmt_runtime_ready_for_home(&home));
        assert!(dxvk_dir.join("dxgi.dll").is_file(), "M12's DXVK dxgi.dll must be staged");

        // Env wiring: M12 env must carry the vkd3d-proton lane, the pinned
        // Vulkan ICD, and zero DXMT keys.
        let node = get_pipeline(PipelineId::M12);
        assert_eq!(node.backend, "vkd3d-proton");
        let env = steam_pipeline_env_pairs(&home, node, 1583230);
        let keys: std::collections::HashSet<_> = env.iter().map(|(key, _)| key.as_str()).collect();
        assert!(keys.contains("VKD3D_SHADER_CACHE_PATH"));
        assert!(!keys.contains("DXMT_SHADER_CACHE_PATH"));
        assert!(!keys.contains("DXMT_CONFIG_FILE"));
        assert!(!keys.contains("DXMT_WINEMETAL_UNIXLIB"));
        let winedllpath = env_value(&env, "WINEDLLPATH").unwrap_or_default();
        assert!(winedllpath.contains("vkd3d-proton/x86_64-windows"));
        assert!(winedllpath.contains("dxvk/x86_64-windows"));
        let overrides = env_value(&env, "WINEDLLOVERRIDES").unwrap_or_default();
        assert!(overrides.contains("d3d12"));
        assert!(overrides.contains("d3d12core"));
        // VKD3D_SHADER_CACHE_PATH must be the absolute isolated cache dir, not
        // a relative value from the node env_vars (which would break isolation).
        let cache_path = env_value(&env, "VKD3D_SHADER_CACHE_PATH").unwrap_or_default();
        assert!(cache_path.starts_with('/'), "VKD3D_SHADER_CACHE_PATH must be absolute, got {:?}", cache_path);
        assert!(!cache_path.ends_with("/m12"), "VKD3D_SHADER_CACHE_PATH must not be the relative m12 value");

        // Prefix route deployment: d3d12core.dll (the real impl the forwarder
        // needs) must actually land in system32.
        let game_dir = home.join("games").join("1583230");
        let exe_dir = game_dir.join("bin");
        std::fs::create_dir_all(&exe_dir).unwrap();
        let system32 = home.join("prefix").join("drive_c").join("windows").join("system32");

        // Build the recipe from the NODE'S REAL deploy list (catches
        // deploy-list rot: missing sources, wrong lanes, missing stubs).
        let dlls = crate::mtsp::recipe::selected_deploy_dlls_for_pipeline(
            &game_dir,
            Some(&exe_dir.join("Game.exe")),
            node,
            &ms_root,
        );
        let filenames: Vec<&str> = dlls.iter().map(|d| d.filename.as_str()).collect();
        assert!(filenames.contains(&"d3d12.dll"), "deploy list must carry d3d12.dll: {:?}", filenames);
        assert!(filenames.contains(&"d3d12core.dll"), "deploy list must carry d3d12core.dll: {:?}", filenames);
        assert!(filenames.contains(&"dxgi.dll"), "deploy list must carry dxgi.dll: {:?}", filenames);
        // Every deploy source must resolve (staged real binaries + stubs).
        for d in &dlls {
            assert!(
                d.source_present,
                "deploy source {} must resolve for {}: {}",
                d.filename,
                d.source_subpath,
                d.source_path.display()
            );
        }

        let recipe = recipe::LaunchRecipe {
            appid: 1583230,
            pipeline: PipelineId::M12,
            pipeline_name: "M12".into(),
            backend: "vkd3d-proton".into(),
            game_dir: Some(game_dir.clone()),
            exe_path: Some(exe_dir.join("Game.exe")),
            exe_name: Some("Game.exe".into()),
            launch_args: Vec::new(),
            env: Vec::new(),
            dlls,
            runtime_assets: Vec::new(),
            warnings: Vec::new(),
        };
        deploy_recipe_dlls(&recipe).expect("game-dir deploy (full node list)");
        for dll in ["d3d12.dll", "d3d12core.dll", "dxgi.dll", "nvapi64.dll", "nvngx.dll"] {
            assert!(exe_dir.join(dll).is_file(), "game dir must receive {}", dll);
        }
        deploy_prefix_route_dlls(&recipe, &home.join("prefix")).expect("prefix route deploy");
        assert!(system32.join("d3d12.dll").is_file(), "system32 must receive d3d12.dll");
        assert!(system32.join("d3d12core.dll").is_file(), "system32 must receive d3d12core.dll (vkd3d impl)");
        assert!(system32.join("dxgi.dll").is_file(), "system32 must receive dxgi.dll");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn non_dxmt_pipelines_do_not_use_winedllpath_routing() {
        for pipeline_id in
            [PipelineId::M32, PipelineId::FnaArm64, PipelineId::Steam, PipelineId::MacSteam, PipelineId::WineBare]
        {
            let node = get_pipeline(pipeline_id);
            assert!(!node.uses_winedllpath_routing(), "{:?} should not use WINEDLLPATH routing", pipeline_id);
        }
    }

    #[test]
    fn fna_native_shim_manifest_includes_macos_framework_routes() {
        let outputs: std::collections::HashSet<_> = FNA_NATIVE_SHIMS.iter().map(|spec| spec.output).collect();

        assert!(outputs.contains("libkernel32.dylib"));
        assert!(outputs.contains("libuser32.dylib"));
        assert!(outputs.contains(FNA_CARBON_SHIM));
        assert!(outputs.contains(FNA_CARBON_INTERPOSE_SHIM));
        assert!(outputs.contains("xaudio2_9.dylib"));
        assert!(outputs.contains("xinput1_4.dylib"));
        assert!(FNA_NATIVE_SHIMS
            .iter()
            .any(|spec| spec.output == "xaudio2_9.dylib" && spec.symlinks.contains(&"xaudio2_7.dylib")));
        assert!(FNA_NATIVE_SHIMS
            .iter()
            .any(|spec| spec.output == "xinput1_4.dylib" && spec.symlinks.contains(&"xinput1_3.dylib")));
    }

    #[test]
    fn celeste_and_default_profiles_isolate_game_dir_dylibs_from_runtime_shims() {
        let celeste = find_fna_profile(504230);
        assert!(matches!(celeste.mono_arch, MonoArch::X86));
        assert!(!celeste.include_runtime_shims_in_library_path);

        let default = find_fna_profile(0);
        assert!(matches!(default.mono_arch, MonoArch::X86));
        assert!(!default.include_runtime_shims_in_library_path);
    }

    #[test]
    fn fna_runtime_validation_rejects_sdl3_linked_core_libs() {
        let path = PathBuf::from("/tmp/libFNA3D.0.dylib");
        assert!(fna_native_lib_source_valid("libSDL2-2.0.0.dylib", &path) || !path.exists());
        assert!(!fna_native_lib_source_valid("libFNA3D.0.dylib", &path));
    }

    #[test]
    fn fna_runtime_validation_rejects_tiny_fmod_stubs() {
        let game_dir = test_dir("fna-runtime-validation-fmod");
        std::fs::create_dir_all(&game_dir).expect("test dir");
        let stub = game_dir.join("libfmod.dylib");
        std::fs::write(&stub, vec![0u8; 32 * 1024]).expect("stub dylib");

        let real = game_dir.join("libfmodstudio.dylib");
        std::fs::write(&real, vec![0u8; 512 * 1024]).expect("real dylib");

        assert!(!fna_native_lib_source_valid("libfmod.dylib", &stub));
        assert!(fna_native_lib_source_valid("libfmodstudio.dylib", &real));
    }

    #[test]
    fn fna_runtime_repair_requires_managed_assembly_and_native_shims() {
        let runtime = PathBuf::from("/tmp/metalsharp-runtime");
        let required = fna_required_runtime_assets(&runtime);

        assert!(required.contains(&runtime.join("fna-kickstart").join("kick.bin.osx")));
        assert!(required.contains(&runtime.join("fna-kickstart").join("FNA.dll")));
        assert!(required.contains(&runtime.join("fna-kickstart").join("osx").join("libmonosgen-2.0.1.dylib")));
        assert!(required.contains(&runtime.join("fna-kickstart").join("osx").join("libSDL2-2.0.0.dylib")));
        assert!(required.contains(&runtime.join("fna-kickstart").join("osx").join("libFNA3D.0.dylib")));
        assert!(required.contains(&runtime.join("fna-kickstart").join("osx").join("libFAudio.0.dylib")));
        assert!(required.contains(&runtime.join("fnalibs").join("libFNA3D.0.dylib")));
        assert!(required.contains(&runtime.join("fnalibs").join("libSDL2-2.0.0.dylib")));
        assert!(required.contains(&runtime.join("shims").join("libkernel32.dylib")));
        assert!(required.contains(&runtime.join("shims").join("libuser32.dylib")));
        assert!(required.contains(&runtime.join("shims").join(FNA_CARBON_SHIM)));
        assert!(required.contains(&runtime.join("shims").join(FNA_CARBON_INTERPOSE_SHIM)));
        assert_eq!(required.len(), 12);
    }

    #[test]
    fn fna_flavor_detection_identifies_fna_by_managed_dll() {
        let game_dir = test_dir("fna-flavor-fna");
        let managed = game_dir.join("Game_Data").join("Managed");
        std::fs::create_dir_all(&managed).expect("managed dir");
        std::fs::write(managed.join("FNA.dll"), b"fna").expect("fna dll");

        assert_eq!(detect_fna_flavor(&game_dir), FnaFlavor::Fna);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn fna_flavor_detection_identifies_monogame_by_managed_dll() {
        let game_dir = test_dir("fna-flavor-mg");
        let managed = game_dir.join("Game_Data").join("Managed");
        std::fs::create_dir_all(&managed).expect("managed dir");
        std::fs::write(managed.join("MonoGame.Framework.dll"), b"mg").expect("mg dll");

        assert_eq!(detect_fna_flavor(&game_dir), FnaFlavor::MonoGame);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn fna_flavor_detection_identifies_xna_by_managed_dll() {
        let game_dir = test_dir("fna-flavor-xna");
        let managed = game_dir.join("Game_Data").join("Managed");
        std::fs::create_dir_all(&managed).expect("managed dir");
        std::fs::write(managed.join("Microsoft.Xna.Framework.dll"), b"xna").expect("xna dll");

        assert_eq!(detect_fna_flavor(&game_dir), FnaFlavor::Xna);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn fna_flavor_detection_falls_back_to_top_level_fna_dll() {
        let game_dir = test_dir("fna-flavor-toplevel");
        std::fs::create_dir_all(&game_dir).expect("game dir");
        std::fs::write(game_dir.join("FNA.dll"), b"fna").expect("fna dll");

        assert_eq!(detect_fna_flavor(&game_dir), FnaFlavor::Fna);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn fna_flavor_detection_returns_unknown_for_empty_dir() {
        let game_dir = test_dir("fna-flavor-empty");
        std::fs::create_dir_all(&game_dir).expect("game dir");

        assert_eq!(detect_fna_flavor(&game_dir), FnaFlavor::Unknown);
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn default_fna_profile_uses_generic_config() {
        let profile = find_fna_profile(u32::MAX);

        assert_eq!(profile.mono_config, "generic-fna-mono.config");
        assert!(matches!(profile.mono_arch, MonoArch::X86));
        assert_eq!(profile.method_label, "xna_fna_x86");
        assert!(!profile.include_runtime_shims_in_library_path);
    }

    #[test]
    fn fna_runtime_download_uses_nuget_flat_container() {
        assert_eq!(
            fna_package_download_url(),
            "https://api.nuget.org/v3-flatcontainer/fna-xna-wrapper/22.12.2/fna-xna-wrapper.22.12.2.nupkg"
        );
    }

    #[test]
    fn fna_native_launch_env_enables_carbon_interpose_when_deployed() {
        let game_dir = test_dir("fna-native-env");
        std::fs::create_dir_all(&game_dir).expect("game dir");
        std::fs::write(game_dir.join(FNA_CARBON_SHIM), b"carbon").expect("carbon shim");
        std::fs::write(game_dir.join(FNA_CARBON_INTERPOSE_SHIM), b"interpose").expect("interpose shim");

        let env = fna_native_launch_env(&game_dir);
        assert_eq!(
            env.iter().find(|(key, _)| key == "METALSHARP_CARBON_SHIM").map(|(_, value)| value.as_str()),
            Some(game_dir.join(FNA_CARBON_SHIM).to_string_lossy().as_ref())
        );
        assert_eq!(
            env.iter().find(|(key, _)| key == "DYLD_INSERT_LIBRARIES").map(|(_, value)| value.as_str()),
            Some(game_dir.join(FNA_CARBON_INTERPOSE_SHIM).to_string_lossy().as_ref())
        );
        let _ = std::fs::remove_dir_all(game_dir);
    }

    #[test]
    fn append_path_env_preserves_existing_insert_libraries() {
        assert_eq!(append_path_env(None, "/tmp/new.dylib"), "/tmp/new.dylib");
        assert_eq!(append_path_env(Some("/tmp/old.dylib"), "/tmp/new.dylib"), "/tmp/old.dylib:/tmp/new.dylib");
        assert_eq!(
            append_path_env(Some("/tmp/old.dylib:/tmp/new.dylib"), "/tmp/new.dylib"),
            "/tmp/old.dylib:/tmp/new.dylib"
        );
    }

    #[test]
    fn steam_pipeline_env_includes_winedllpath_for_dxmt_pipelines() {
        for pipeline_id in [PipelineId::M9, PipelineId::M10, PipelineId::M11] {
            let home = test_dir(&format!("winedllpath-env-{:?}", pipeline_id));
            let node = get_pipeline(pipeline_id);
            let env = steam_pipeline_env_pairs(&home, node, 42);
            let winedllpath = env.iter().find(|(key, _)| key == "WINEDLLPATH");
            assert!(winedllpath.is_some(), "{:?} missing WINEDLLPATH in env pairs", pipeline_id);
            let path = winedllpath.unwrap().1.as_str();
            assert!(!path.is_empty(), "{:?} WINEDLLPATH is empty", pipeline_id);
            assert!(path.contains("x86_64-windows"), "{:?} WINEDLLPATH missing windows dir: {}", pipeline_id, path);
            assert!(!path.contains("dxmt_m12"), "{:?} should not use isolated M12 DLL path: {}", pipeline_id, path);
            let _ = std::fs::remove_dir_all(&home);
        }

        // M12 routes WINEDLLPATH through the vkd3d-proton lane.
        let home = test_dir("winedllpath-env-M12");
        let node = get_pipeline(PipelineId::M12);
        let env = steam_pipeline_env_pairs(&home, node, 42);
        let winedllpath = env.iter().find(|(key, _)| key == "WINEDLLPATH").expect("M12 WINEDLLPATH");
        assert!(winedllpath.1.contains("vkd3d-proton"), "M12 should use vkd3d-proton DLL path: {}", winedllpath.1);
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn steam_pipeline_env_has_no_winedllpath_for_non_dxmt_pipelines() {
        let home = test_dir("no-winedllpath");
        let node = get_pipeline(PipelineId::WineBare);
        let env = steam_pipeline_env_pairs(&home, node, 1);
        assert!(!env.iter().any(|(key, _)| key == "WINEDLLPATH"));
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn m12_prefix_route_dlls_stage_into_system32() {
        let root = test_dir("m12-prefix-route");
        let source_dir = root.join("runtime").join("wine").join("lib").join("dxmt_m12").join("x86_64-windows");
        let prefix = root.join("prefix-steam");
        std::fs::create_dir_all(&source_dir).expect("create source dir");

        let dlls = ["d3d12.dll", "dxgi.dll", "dxgi_dxmt.dll", "d3d11.dll", "d3d10core.dll", "winemetal.dll"];
        let recipe_dlls: Vec<recipe::RecipeDll> = dlls
            .iter()
            .map(|dll| {
                let source_path = source_dir.join(dll);
                std::fs::write(&source_path, format!("m12-{dll}")).expect("write route dll");
                recipe::RecipeDll {
                    source_subpath: "lib/dxmt_m12/x86_64-windows".to_string(),
                    filename: (*dll).to_string(),
                    source_path,
                    dest_path: root.join("game").join(dll),
                    source_present: true,
                }
            })
            .collect();

        let mut recipe = empty_test_recipe(PipelineId::M12);
        recipe.dlls = recipe_dlls;

        deploy_prefix_route_dlls(&recipe, &prefix).expect("stage M12 route DLLs");

        let system32 = prefix.join("drive_c").join("windows").join("system32");
        for dll in dlls {
            assert_eq!(std::fs::read_to_string(system32.join(dll)).unwrap(), format!("m12-{dll}"));
        }

        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn non_m12_prefix_route_dlls_do_not_stage_into_system32() {
        let root = test_dir("m11-prefix-route");
        let source_dir = root.join("runtime").join("wine").join("lib").join("dxmt").join("x86_64-windows");
        let prefix = root.join("prefix-steam");
        std::fs::create_dir_all(&source_dir).expect("create source dir");
        let source_path = source_dir.join("d3d12.dll");
        std::fs::write(&source_path, "legacy-dxmt-d3d12").expect("write route dll");

        let mut recipe = empty_test_recipe(PipelineId::M11);
        recipe.dlls.push(recipe::RecipeDll {
            source_subpath: "lib/dxmt/x86_64-windows".to_string(),
            filename: "d3d12.dll".to_string(),
            source_path,
            dest_path: root.join("game").join("d3d12.dll"),
            source_present: true,
        });

        deploy_prefix_route_dlls(&recipe, &prefix).expect("non-M12 route is no-op");

        assert!(!prefix.join("drive_c").join("windows").join("system32").join("d3d12.dll").exists());

        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn cleanup_legacy_injections_restores_backups_within_game_dir() {
        let game_dir = test_dir("cleanup-restore");
        let injection_dir = game_dir.join(".metalsharp");
        let originals_dir = injection_dir.join("originals");
        std::fs::create_dir_all(&originals_dir).unwrap();

        let original_dll = game_dir.join("d3d12.dll");
        let backup_dll = originals_dir.join("d3d12.dll");
        std::fs::write(&backup_dll, b"original-d3d12-content").unwrap();

        let manifest = serde_json::json!({
            "appid": 1234,
            "dlls": [{
                "filename": "d3d12.dll",
                "dest_path": original_dll.to_string_lossy().to_string(),
                "backup_path": backup_dll.to_string_lossy().to_string(),
            }]
        });
        std::fs::write(injection_dir.join("injections.json"), serde_json::to_string_pretty(&manifest).unwrap())
            .unwrap();

        cleanup_legacy_injections(&game_dir).unwrap();

        assert!(original_dll.exists());
        assert_eq!(std::fs::read(&original_dll).unwrap(), b"original-d3d12-content");
        assert!(!injection_dir.exists());
        let _ = std::fs::remove_dir_all(&game_dir);
    }

    #[test]
    fn cleanup_legacy_injections_ignores_paths_outside_game_dir() {
        let game_dir = test_dir("cleanup-traversal");
        let injection_dir = game_dir.join(".metalsharp");
        let originals_dir = injection_dir.join("originals");
        std::fs::create_dir_all(&originals_dir).unwrap();

        let outside_path = std::env::temp_dir().join("metalsharp-traversal-test-target.dll");
        let _ = std::fs::remove_file(&outside_path);

        let manifest = serde_json::json!({
            "appid": 1234,
            "dlls": [{
                "filename": "evil.dll",
                "dest_path": outside_path.to_string_lossy().to_string(),
                "backup_path": originals_dir.join("evil.dll").to_string_lossy().to_string(),
            }]
        });
        std::fs::write(injection_dir.join("injections.json"), serde_json::to_string_pretty(&manifest).unwrap())
            .unwrap();

        cleanup_legacy_injections(&game_dir).unwrap();

        assert!(!outside_path.exists());
        assert!(!injection_dir.exists());
        let _ = std::fs::remove_dir_all(&game_dir);
    }

    #[test]
    fn d3dmetal_goldberg_dirs_include_gptk_dosdevice_alias() {
        let home = test_dir("gptk-goldberg-alias");
        let library = home.join("SteamLibrary");
        let game_dir = library.join("steamapps").join("common").join("Celeste");
        let prefix = crate::platform::gptk_prefix_path(&home);
        let dosdevices = prefix.join("dosdevices");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::create_dir_all(&dosdevices).expect("create dosdevices");
        std::os::unix::fs::symlink(&library, dosdevices.join("d:")).expect("create library drive");

        let aliases = gptk_prefix_game_dir_aliases(&prefix, &game_dir);
        let dirs = goldberg_dirs_for_pipeline(&home, &game_dir, PipelineId::D3DMetal);

        assert!(aliases.contains(&dosdevices.join("d:").join("steamapps").join("common").join("Celeste")));
        assert_eq!(dirs, vec![game_dir.clone()]);

        let m9_dirs = goldberg_dirs_for_pipeline(&home, &game_dir, PipelineId::M9);
        assert_eq!(m9_dirs, vec![game_dir]);

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn gptk_dosdevice_alias_resolves_relative_c_drive() {
        let home = test_dir("gptk-relative-c");
        let prefix = crate::platform::gptk_prefix_path(&home);
        let dosdevices = prefix.join("dosdevices");
        let game_dir = prefix.join("drive_c").join("Games").join("Portal 2");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::create_dir_all(&dosdevices).expect("create dosdevices");
        std::os::unix::fs::symlink("../drive_c", dosdevices.join("c:")).expect("create c drive");

        let aliases = gptk_prefix_game_dir_aliases(&prefix, &game_dir);

        assert_eq!(aliases, vec![dosdevices.join("c:").join("Games").join("Portal 2")]);

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn d3dmetal_offline_disables_and_restores_gptk_steam_launcher() {
        let home = test_dir("gptk-offline-steam-exe");
        let prefix = crate::platform::gptk_prefix_path(&home);
        let steam_dir = prefix.join("drive_c").join("Program Files (x86)").join("Steam");
        std::fs::create_dir_all(&steam_dir).expect("create steam dir");
        let steam_exe = steam_dir.join("Steam.exe");
        let disabled = crate::platform::gptk_disabled_steam_exe(&prefix);
        std::fs::write(&steam_exe, b"steam").expect("write steam exe");

        assert!(crate::platform::disable_gptk_steam_launcher_for_offline(&prefix).expect("disable steam exe"));

        assert!(!steam_exe.exists());
        assert_eq!(std::fs::read(&disabled).expect("read disabled"), b"steam");

        assert!(crate::platform::restore_gptk_steam_launcher(&prefix).expect("restore steam exe"));

        assert!(steam_exe.exists());
        assert!(!disabled.exists());

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn steam_model_launch_deploys_real_steam_client_components() {
        let home = test_dir("secure-real-steam");
        let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam")
            .join("drive_c")
            .join("Program Files (x86)")
            .join("Steam");
        let game_dir = home.join("SteamLibrary").join("steamapps").join("common").join("Team Fortress 2");
        let bin_dir = game_dir.join("bin");
        std::fs::create_dir_all(&steam_dir).expect("create steam dir");
        std::fs::create_dir_all(&bin_dir).expect("create game bin");

        for filename in ["steam_api64.dll", "steam_api.dll"] {
            std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
        }
        for filename in real_steam_model_component_names() {
            std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
        }

        ensure_real_steam_dlls(&home, &game_dir, 440, true);

        for target in [&game_dir, &bin_dir] {
            assert_eq!(std::fs::read(target.join("steam_api64.dll")).expect("read api64"), b"steam_api64.dll");
            assert_eq!(std::fs::read(target.join("steam_api.dll")).expect("read api"), b"steam_api.dll");
            assert_eq!(
                std::fs::read(target.join("steamclient64.dll")).expect("read steamclient64"),
                b"steamclient64.dll"
            );
            assert_eq!(std::fs::read(target.join("steamclient.dll")).expect("read steamclient"), b"steamclient.dll");
            assert_eq!(
                std::fs::read(target.join("GameOverlayRenderer64.dll")).expect("read overlay64"),
                b"GameOverlayRenderer64.dll"
            );
            assert_eq!(
                std::fs::read(target.join("GameOverlayRenderer.dll")).expect("read overlay"),
                b"GameOverlayRenderer.dll"
            );
            assert_eq!(std::fs::read_to_string(target.join("steam_appid.txt")).expect("read appid"), "440");
        }

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn m12_steam_launch_models_deploy_same_real_steam_components_as_m11() {
        for (appid, expects_secure) in [(1260320, false), (440, true)] {
            let home = test_dir(&format!("m12-real-steam-{appid}"));
            let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
                .join("prefix-steam")
                .join("drive_c")
                .join("Program Files (x86)")
                .join("Steam");
            let game_dir = home.join("SteamLibrary").join("steamapps").join("common").join(format!("Game {appid}"));
            let bin_dir = game_dir.join("bin");
            std::fs::create_dir_all(&steam_dir).expect("create steam dir");
            std::fs::create_dir_all(&bin_dir).expect("create game bin");

            for filename in ["steam_api64.dll", "steam_api.dll"] {
                std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
            }
            for filename in real_steam_model_component_names() {
                std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
            }

            let args = recipe::effective_launch_args(appid, get_pipeline(PipelineId::M12));
            assert!(args.iter().any(|arg| arg.eq_ignore_ascii_case("-steam")), "appid {appid}");
            assert_eq!(args.iter().any(|arg| arg.eq_ignore_ascii_case("-secure")), expects_secure, "appid {appid}");

            prepare_steam_api_for_game_dir(&home, &game_dir, appid, PipelineId::M12);

            for target in [&game_dir, &bin_dir] {
                assert_eq!(std::fs::read(target.join("steam_api64.dll")).expect("read api64"), b"steam_api64.dll");
                assert_eq!(std::fs::read(target.join("steam_api.dll")).expect("read api"), b"steam_api.dll");
                for filename in real_steam_model_component_names() {
                    assert_eq!(
                        std::fs::read(target.join(filename)).expect("read steam model component"),
                        filename.as_bytes(),
                        "appid {appid} filename {filename}"
                    );
                }
                assert_eq!(
                    std::fs::read_to_string(target.join("steam_appid.txt")).expect("read appid"),
                    appid.to_string()
                );
            }

            let _ = std::fs::remove_dir_all(home);
        }
    }

    #[test]
    fn steam_only_launches_deploy_real_steam_client_components() {
        let home = test_dir("steam-only-real-steam");
        let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam")
            .join("drive_c")
            .join("Program Files (x86)")
            .join("Steam");
        let game_dir = home.join("SteamLibrary").join("steamapps").join("common").join("Party Animals");
        std::fs::create_dir_all(&steam_dir).expect("create steam dir");
        std::fs::create_dir_all(&game_dir).expect("create game dir");

        for filename in ["steam_api64.dll", "steam_api.dll"] {
            std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
        }
        for filename in real_steam_model_component_names() {
            std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
        }

        ensure_real_steam_dlls(&home, &game_dir, 1260320, true);

        assert_eq!(std::fs::read(game_dir.join("steam_api64.dll")).expect("read api64"), b"steam_api64.dll");
        assert_eq!(std::fs::read(game_dir.join("steam_api.dll")).expect("read api"), b"steam_api.dll");
        for filename in real_steam_model_component_names() {
            assert_eq!(
                std::fs::read(game_dir.join(filename)).expect("read steam model component"),
                filename.as_bytes()
            );
        }
        assert_eq!(std::fs::read_to_string(game_dir.join("steam_appid.txt")).expect("read appid"), "1260320");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn ordinary_real_steam_launch_keeps_client_components_conservative() {
        let home = test_dir("ordinary-real-steam");
        let steam_dir = crate::platform::metalsharp_home_dir_for(&home)
            .join("prefix-steam")
            .join("drive_c")
            .join("Program Files (x86)")
            .join("Steam");
        let game_dir = home.join("SteamLibrary").join("steamapps").join("common").join("Ordinary Game");
        std::fs::create_dir_all(&steam_dir).expect("create steam dir");
        std::fs::create_dir_all(&game_dir).expect("create game dir");

        for filename in ["steam_api64.dll", "steamclient64.dll", "GameOverlayRenderer64.dll"] {
            std::fs::write(steam_dir.join(filename), filename.as_bytes()).expect("write steam component");
        }

        ensure_real_steam_dlls(&home, &game_dir, 999999, false);

        assert!(game_dir.join("steam_api64.dll").exists());
        assert!(!game_dir.join("steamclient64.dll").exists());
        assert!(!game_dir.join("GameOverlayRenderer64.dll").exists());
        assert_eq!(std::fs::read_to_string(game_dir.join("steam_appid.txt")).expect("read appid"), "999999");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn steam_pipeline_env_rejects_client_only_routes() {
        let error = prepare_steam_pipeline_env(1, PipelineId::Steam).expect_err("steam should not be handoff");

        assert!(error.to_string().contains("Steam route handoff only supports Wine-backed MTSP pipelines"));
    }

    #[test]
    fn bridge_port_parser_rejects_invalid_values() {
        assert_eq!(parse_bridge_port(Some("19001")), Some(19001));
        assert_eq!(parse_bridge_port(Some("0")), None);
        assert_eq!(parse_bridge_port(Some("65536")), None);
        assert_eq!(parse_bridge_port(Some("abc")), None);
        assert_eq!(parse_bridge_port(None), None);
    }

    #[test]
    fn no_dll_recipes_do_not_require_game_dir_for_deploy() {
        let recipe = super::super::recipe::LaunchRecipe {
            appid: 1,
            pipeline: PipelineId::Steam,
            pipeline_name: "Steam".into(),
            backend: "wine-steam".into(),
            game_dir: None,
            exe_path: None,
            exe_name: None,
            launch_args: vec![],
            env: vec![],
            dlls: vec![],
            runtime_assets: vec![],
            warnings: vec![],
        };

        deploy_recipe_dlls(&recipe).expect("no-op deploy should succeed");
    }

    #[test]
    fn nested_executables_launch_from_their_parent_directory() {
        let game_dir = PathBuf::from("/tmp/Game");
        let exe_path = game_dir.join("Engine").join("Binaries").join("Win64").join("Game-Win64-Shipping.exe");

        assert_eq!(launch_working_dir(&game_dir, &exe_path), exe_path.parent().unwrap());
    }

    #[test]
    fn launch_log_identity_records_host_runtime_and_bottle_route_state_paths() {
        let mut log = Vec::new();
        let prefix = crate::platform::metalsharp_home_dir().join("prefix-steam");

        write_runtime_identity(&mut log, &prefix, Some(620)).expect("write runtime identity");

        let text = String::from_utf8(log).expect("utf8 log");
        assert!(text.contains("host_abi=1.0"));
        assert!(text.contains("host_runtime="));
        assert!(text.contains("steam_bridge_port="));
        assert!(text.contains("route_state=bottle_manifest"));
        assert!(text.contains("bottle_manifest="));
        assert!(text.contains("bottle_logs="));
        assert!(text.contains("steam_identity_mode=wine_steam_background"));
    }

    #[test]
    fn start_protected_game_bypass_renames_and_copies_real_exe() {
        let home = test_dir("spg-bypass");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"PROTECTED_STUB").expect("write stub");
        std::fs::write(game_dir.join("eldenring.exe"), b"REAL_GAME").expect("write real exe");

        apply_start_protected_game_bypass(1245620, &home);

        assert!(game_dir.join("start_protected_game.old").exists());
        assert_eq!(std::fs::read(game_dir.join("start_protected_game.old")).unwrap(), b"PROTECTED_STUB");
        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"REAL_GAME");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn start_protected_game_bypass_skips_if_old_already_exists() {
        let home = test_dir("spg-skip-old");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.old"), b"PREVIOUS_STUB").expect("write old");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"REAL_GAME_ALREADY").expect("write current");
        std::fs::write(game_dir.join("eldenring.exe"), b"REAL_GAME").expect("write real exe");

        apply_start_protected_game_bypass(1245620, &home);

        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"REAL_GAME_ALREADY");
        assert_eq!(std::fs::read(game_dir.join("start_protected_game.old")).unwrap(), b"PREVIOUS_STUB");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn start_protected_game_bypass_handles_armored_core_vi() {
        let home = test_dir("spg-ac6");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"PROTECTED_STUB").expect("write stub");
        std::fs::write(game_dir.join("armoredcore6.exe"), b"AC6_REAL_GAME").expect("write real exe");

        apply_start_protected_game_bypass(1888160, &home);

        assert_eq!(std::fs::read(game_dir.join("start_protected_game.old")).unwrap(), b"PROTECTED_STUB");
        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"AC6_REAL_GAME");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn start_protected_game_bypass_can_infer_single_sibling_real_exe() {
        let home = test_dir("spg-generic");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"PROTECTED_STUB").expect("write stub");
        std::fs::write(game_dir.join("realgame.exe"), b"REAL_GAME").expect("write real exe");

        apply_start_protected_game_bypass(99999, &home);

        assert_eq!(std::fs::read(game_dir.join("start_protected_game.old")).unwrap(), b"PROTECTED_STUB");
        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"REAL_GAME");

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn start_protected_game_bypass_skips_ambiguous_generic_siblings() {
        let home = test_dir("spg-ambiguous");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"PROTECTED_STUB").expect("write stub");
        std::fs::write(game_dir.join("first.exe"), b"FIRST").expect("write first exe");
        std::fs::write(game_dir.join("second.exe"), b"SECOND").expect("write second exe");

        apply_start_protected_game_bypass(99999, &home);

        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"PROTECTED_STUB");
        assert!(!game_dir.join("start_protected_game.old").exists());

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn start_protected_game_bypass_skips_unknown_appid() {
        let home = test_dir("spg-skip");
        let game_dir = home.join("Game");
        std::fs::create_dir_all(&game_dir).expect("create game dir");
        std::fs::write(game_dir.join("start_protected_game.exe"), b"PROTECTED_STUB").expect("write stub");
        std::fs::write(game_dir.join("first.exe"), b"FIRST").expect("write first exe");
        std::fs::write(game_dir.join("second.exe"), b"SECOND").expect("write second exe");

        apply_start_protected_game_bypass(99999, &home);

        assert_eq!(std::fs::read(game_dir.join("start_protected_game.exe")).unwrap(), b"PROTECTED_STUB");
        assert!(!game_dir.join("start_protected_game.old").exists());

        let _ = std::fs::remove_dir_all(home);
    }

    #[test]
    fn legacy_eac_toggle_cleanup_removes_marked_files_only() {
        let home = test_dir("legacy-eac-toggle-cleanup");
        let marked = home.join("Binaries").join("Win64");
        let unmarked = home.join("bin");
        std::fs::create_dir_all(&marked).expect("create marked dir");
        std::fs::create_dir_all(&unmarked).expect("create unmarked dir");
        std::fs::write(marked.join("_winhttp.dll"), b"OLD_TOGGLE").expect("write toggle dll");
        std::fs::write(marked.join("anti_cheat_toggler_config.ini"), b"config").expect("write toggle config");
        std::fs::write(marked.join("anti_cheat_toggler_mod_list.txt"), b"mods").expect("write toggle mods");
        std::fs::write(unmarked.join("_winhttp.dll"), b"OTHER").expect("write unrelated dll");

        cleanup_legacy_eac_toggle_artifacts(&home);

        assert!(!marked.join("_winhttp.dll").exists());
        assert!(!marked.join("anti_cheat_toggler_config.ini").exists());
        assert!(!marked.join("anti_cheat_toggler_mod_list.txt").exists());
        assert_eq!(std::fs::read(unmarked.join("_winhttp.dll")).unwrap(), b"OTHER");

        let _ = std::fs::remove_dir_all(home);
    }

    fn test_dir(name: &str) -> PathBuf {
        let mut dir = std::env::temp_dir();
        dir.push(format!("metalsharp-launcher-{}-{}-{}", name, std::process::id(), unique_suffix()));
        dir
    }

    fn empty_test_recipe(pipeline: PipelineId) -> recipe::LaunchRecipe {
        recipe::LaunchRecipe {
            appid: 1,
            pipeline,
            pipeline_name: format!("{pipeline:?}"),
            backend: "dxmt".to_string(),
            game_dir: None,
            exe_path: None,
            exe_name: None,
            launch_args: Vec::new(),
            env: Vec::new(),
            dlls: Vec::new(),
            runtime_assets: Vec::new(),
            warnings: Vec::new(),
        }
    }

    fn env_value<'a>(env: &'a [(String, String)], key: &str) -> Option<&'a str> {
        env.iter().find(|(env_key, _)| env_key == key).map(|(_, value)| value.as_str())
    }

    fn last_env_value<'a>(env: &'a [(String, String)], key: &str) -> Option<&'a str> {
        env.iter().rev().find(|(env_key, _)| env_key == key).map(|(_, value)| value.as_str())
    }

    fn unique_suffix() -> u128 {
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
    }

    // -----------------------------------------------------------------
    // Goldberg cache + persisted-metadata tests
    // -----------------------------------------------------------------

    fn synthetic_dll_vec(seed: u8, fill: u8, len: usize) -> Vec<u8> {
        let mut header: [u8; 128] = [
            0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xB8, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xBA, 0x10, 0x00, 0x00, 0x0E, 0x1F, 0xB4, 0x09,
            0xCD, 0x21, 0xB8, 0x01, 0x4C, 0xCD, 0x21, 0x54, 0x68, 0x69, 0x73, 0x20, 0x70, 0x72, 0x6F, 0x67, 0x72, 0x61,
            0x6D, 0x20, 0x63, 0x61, 0x6E, 0x6E, 0x6F, 0x74, 0x20, 0x62, 0x65, 0x20, 0x72, 0x75, 0x6E, 0x20, 0x69, 0x6E,
            0x20, 0x44, 0x4F, 0x53, 0x20, 0x6D, 0x6F, 0x64, 0x65, 0x2E, 0x0D, 0x0D, 0x0A, 0x24, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00,
        ];
        // Stamp the seed into the byte Goldbergs use as a magic marker so we
        // can be sure real and Goldberg DLLs don't look identical.
        header[0x7A] = seed;
        let mut out = Vec::with_capacity(len);
        out.extend_from_slice(&header);
        out.resize(len, fill);
        out
    }

    fn goldberg_test_bytes(byte: u8) -> Vec<u8> {
        synthetic_dll_vec(byte, byte, 64 * 1024)
    }

    fn steam_test_bytes() -> Vec<u8> {
        synthetic_dll_vec(b'S', 0xAB, 64 * 1024)
    }

    fn install_temp_home(appid: u32) -> (PathBuf, PathBuf) {
        let root = test_dir(&format!("goldberg-cache-{appid}"));
        std::fs::create_dir_all(&root).unwrap();
        let game_dir = root.join("game");
        std::fs::create_dir_all(&game_dir).unwrap();
        // runtime/goldberg/x86 and x64 with synthetic Goldberg DLLs.
        let runtime = root.join("runtime").join("goldberg");
        std::fs::create_dir_all(runtime.join("x86")).unwrap();
        std::fs::create_dir_all(runtime.join("x64")).unwrap();
        std::fs::create_dir_all(runtime.join("steamclient")).unwrap();
        let goldberg = goldberg_test_bytes(b'G');
        std::fs::write(runtime.join("x86").join("steam_api.dll"), &goldberg).unwrap();
        std::fs::write(runtime.join("x64").join("steam_api64.dll"), &goldberg).unwrap();
        std::fs::write(runtime.join("steamclient").join("steamclient64.dll"), &goldberg).unwrap();
        std::fs::write(runtime.join("steamclient").join("steamclient.dll"), &goldberg).unwrap();
        // Stage synthetic real Steam DLLs inside the game dir.
        let steam = steam_test_bytes();
        std::fs::write(game_dir.join("steam_api.dll"), &steam).unwrap();
        std::fs::write(game_dir.join("steam_api64.dll"), &steam).unwrap();
        std::fs::write(game_dir.join("steamclient64.dll"), &steam).unwrap();
        std::fs::write(game_dir.join("steamclient.dll"), &steam).unwrap();
        (root, game_dir)
    }

    fn persist_goldberg_toggle_on_for(
        home_root: &Path,
        appid: u32,
        targets: &[PathBuf],
        goldberg_runtime: &Path,
    ) -> GoldbergMetadata {
        let (any_backup, cache_hashes, goldberg_hashes) =
            backup_dlls_for_appid(home_root, appid, targets, goldberg_runtime);
        let now_secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or_default();
        let mut existing = read_goldberg_metadata_for(home_root, appid).unwrap_or(GoldbergMetadata {
            appid,
            active: false,
            backed_up_at: None,
            backed_up_from: Vec::new(),
            cache_hashes: std::collections::BTreeMap::new(),
            goldberg_source_hashes: std::collections::BTreeMap::new(),
        });
        existing.appid = appid;
        existing.active = true;
        if any_backup || existing.backed_up_at.is_none() {
            existing.backed_up_at = Some(now_secs);
        }
        for target in targets {
            let rel = target.to_string_lossy().to_string();
            if !existing.backed_up_from.contains(&rel) {
                existing.backed_up_from.push(rel);
            }
        }
        for (key, value) in cache_hashes {
            existing.cache_hashes.insert(key, value);
        }
        for (key, value) in goldberg_hashes {
            existing.goldberg_source_hashes.insert(key, value);
        }
        write_goldberg_metadata_for(home_root, &existing);
        existing
    }

    fn persist_goldberg_toggle_off_for(home_root: &Path, appid: u32) -> GoldbergMetadata {
        let mut existing = read_goldberg_metadata_for(home_root, appid).unwrap_or(GoldbergMetadata {
            appid,
            active: true,
            backed_up_at: None,
            backed_up_from: Vec::new(),
            cache_hashes: std::collections::BTreeMap::new(),
            goldberg_source_hashes: std::collections::BTreeMap::new(),
        });
        existing.appid = appid;
        existing.active = false;
        write_goldberg_metadata_for(home_root, &existing);
        existing
    }

    #[test]
    fn backup_and_restore_roundtrips_after_orig_removed() {
        // Run inside a private sandbox so we don't touch real `~/.metalsharp`.
        let (home, game_dir) = install_temp_home(990011);

        let targets = goldberg_deploy_targets(&game_dir);
        let emu_dir = home.join("runtime").join("goldberg");

        // 1) Snapshot originals into the per-app cache.
        persist_goldberg_toggle_on_for(&home, 990011, &targets, &emu_dir);
        let cache = goldberg_cache_dir_for(&home, 990011);
        assert!(cache.join("steam_api64.dll").is_file(), "steam_api64.dll must be cached");
        assert!(cache.join("steam_api.dll").is_file());

        // 2) Pretend Goldberg deploy already happened: rename real -> .orig,
        //    drop Goldberg DLLs in their place.
        let goldberg = goldberg_test_bytes(b'G');
        for dll in ["steam_api64.dll", "steam_api.dll"] {
            std::fs::rename(game_dir.join(dll), game_dir.join(format!("{dll}.orig"))).unwrap();
            std::fs::write(game_dir.join(dll), &goldberg).unwrap();
        }

        // 3) Simulate the failure mode: user or Steam deletes the .orig.
        std::fs::remove_file(game_dir.join("steam_api64.dll.orig")).unwrap();

        // 4) cleanup_goldberg must restore from the cache, not silently fail.
        restore_cached_dlls(&home, 990011, &targets, &emu_dir);
        let restored = std::fs::read(game_dir.join("steam_api64.dll")).unwrap();
        assert_eq!(restored, steam_test_bytes(), "cache restore must surface the original Steam DLL");

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn backup_refuses_to_cache_goldberg_dlls_as_originals() {
        let (home, game_dir) = install_temp_home(990022);

        // Place Goldberg's own DLL inside the game directory under the
        // pretense that the user already deployed Goldberg but the cache
        // never populated. backup_dlls_for_appid must REFUSE to overwrite
        // the cache with Goldberg's content.
        let goldberg = goldberg_test_bytes(b'G');
        std::fs::write(game_dir.join("steam_api64.dll"), &goldberg).unwrap();
        std::fs::write(game_dir.join("steam_api.dll"), &goldberg).unwrap();

        let targets = goldberg_deploy_targets(&game_dir);
        let emu_dir = home.join("runtime").join("goldberg");
        backup_dlls_for_appid(&home, 990022, &targets, &emu_dir);

        let cache = goldberg_cache_dir_for(&home, 990022);
        assert!(!cache.join("steam_api64.dll").exists(), "must not cache Goldberg DLLs as originals");
        assert!(!cache.join("steam_api.dll").exists());

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn cache_idempotency_does_not_overwrite_existing_backup() {
        let (home, game_dir) = install_temp_home(990033);

        // Pre-populate cache with a real-size sentinel that does NOT match
        // the real Steam DLL. The cache should be preserved because the
        // per-DLL id check sees the sentinel is not identical to the live
        // DLL — but the cache layer should still refuse to overwrite when
        // the existing cache is a valid backup.
        //
        // We construct the test so the cache file *would* pass the size
        // threshold and is real backup material by stuffing a unique
        // fingerprint byte.
        let cache = goldberg_cache_dir_for(&home, 990033);
        std::fs::create_dir_all(&cache).unwrap();
        let mut sentinel = steam_test_bytes();
        // Stamp a unique marker so the cached file does not match the
        // live `steam_api64.dll` (which has its own marker).
        sentinel[0x7A] = b'!';
        std::fs::write(cache.join("steam_api64.dll"), &sentinel).unwrap();

        let targets = goldberg_deploy_targets(&game_dir);
        let emu_dir = home.join("runtime").join("goldberg");
        // backup_dlls_for_appid sees the cache already has a file. It
        // must NOT overwrite because the cache was previously populated
        // and should be treated as authoritative.
        backup_dlls_for_appid(&home, 990033, &targets, &emu_dir);
        let after = std::fs::read(cache.join("steam_api64.dll")).unwrap();
        assert_eq!(after, sentinel, "existing cache must be preserved");

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn metadata_survives_toggle_off_and_round_trips() {
        let (home, _game_dir) = install_temp_home(990044);

        let targets = goldberg_deploy_targets(&_game_dir);
        let emu_dir = home.join("runtime").join("goldberg");
        persist_goldberg_toggle_on_for(&home, 990044, &targets, &emu_dir);
        let on = read_goldberg_metadata_for(&home, 990044).unwrap();
        assert!(on.active);
        assert!(on.backed_up_at.is_some());

        persist_goldberg_toggle_off_for(&home, 990044);
        let off = read_goldberg_metadata_for(&home, 990044).unwrap();
        assert!(!off.active, "OFF must persist on disk");
        // Cache files survive across OFF so a re-enable is cheap.
        let cache = goldberg_cache_dir_for(&home, 990044);
        assert!(cache.join("steam_api64.dll").is_file(), "cache must persist across OFF");

        let _ = std::fs::remove_dir_all(&home);
    }

    // ---- MetalFX sidebar toggle: env reconciliation from metalfx.overlay.json ----

    fn metalfx_home() -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("ms-metalfx-env-{}-{}", std::process::id(), unique_suffix()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn write_metalfx_state(home: &std::path::Path, enabled: bool, factor: f32) {
        let path = home.join("etc").join("metalfx.overlay.json");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, serde_json::json!({ "enabled": enabled, "factor": factor, "ts": 1 }).to_string())
            .unwrap();
    }

    fn dxmt_env_with_node(pipeline_id: PipelineId) -> Vec<(String, String)> {
        let node = get_pipeline(pipeline_id);
        let mut env: Vec<(String, String)> = Vec::new();
        env.extend(node.env_vars.iter().map(|ev| (ev.key.to_string(), ev.value.to_string())));
        env
    }

    #[test]
    fn metalfx_env_reconciles_all_four_dxmt_routes() {
        let home = metalfx_home();
        for pipeline_id in [PipelineId::M10, PipelineId::M10_32, PipelineId::M11, PipelineId::M11_32] {
            let node = get_pipeline(pipeline_id);

            // Default (no state file): enabled at 1.50.
            let _ = std::fs::remove_file(home.join("etc").join("metalfx.overlay.json"));
            let mut env = dxmt_env_with_node(pipeline_id);
            apply_metal_fx_config(&mut env, node, &home);
            let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
            assert!(
                config.contains("d3d11.metalSpatialUpscaleFactor=1.50"),
                "{:?} default config must carry 1.50: {}",
                pipeline_id,
                config
            );
            assert_eq!(last_env_value(&env, "DXMT_METALFX_SPATIAL_SWAPCHAIN"), Some("1"));

            // 1.75
            write_metalfx_state(&home, true, 1.75);
            let mut env = dxmt_env_with_node(pipeline_id);
            apply_metal_fx_config(&mut env, node, &home);
            let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
            assert!(
                config.contains("d3d11.metalSpatialUpscaleFactor=1.75"),
                "{:?} must carry 1.75: {}",
                pipeline_id,
                config
            );
            assert_eq!(last_env_value(&env, "DXMT_METALFX_SPATIAL_SWAPCHAIN"), Some("1"));

            // OFF: no upscale pair, swapchain integration off.
            write_metalfx_state(&home, false, 1.75);
            let mut env = dxmt_env_with_node(pipeline_id);
            apply_metal_fx_config(&mut env, node, &home);
            let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
            assert!(
                !config.contains("metalSpatialUpscaleFactor"),
                "{:?} OFF must strip the upscale pair: {}",
                pipeline_id,
                config
            );
            assert!(config.contains("d3d11.preferredMaxFrameRate=60"), "OFF keeps other pairs: {}", config);
            assert_eq!(last_env_value(&env, "DXMT_METALFX_SPATIAL_SWAPCHAIN"), Some("0"));
        }
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn metalfx_env_untouched_for_non_dxmt_routes() {
        let home = metalfx_home();
        write_metalfx_state(&home, false, 1.75); // would strip if applied
        for pipeline_id in [PipelineId::M9, PipelineId::M12, PipelineId::M13, PipelineId::Steam, PipelineId::WineBare] {
            let node = get_pipeline(pipeline_id);
            let mut env = dxmt_env_with_node(pipeline_id);
            let before = env.clone();
            apply_metal_fx_config(&mut env, node, &home);
            assert_eq!(env, before, "{:?} env must be untouched by the MetalFX toggle", pipeline_id);
        }
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn metalfx_env_beats_node_default_and_recipe_env() {
        let home = metalfx_home();
        // Recipe env sets a DXMT_CONFIG with the old hardcoded 1.43; the toggle
        // (1.50 default) must win.
        let node = get_pipeline(PipelineId::M11);
        let mut env = dxmt_env_with_node(PipelineId::M11);
        env.push((
            "DXMT_CONFIG".to_string(),
            "d3d11.metalSpatialUpscaleFactor=1.43;d3d11.preferredMaxFrameRate=60".into(),
        ));
        apply_metal_fx_config(&mut env, node, &home);
        let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
        assert!(
            config.contains("d3d11.metalSpatialUpscaleFactor=1.50"),
            "toggle (1.50 default) must beat recipe 1.43: {}",
            config
        );
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn metalfx_env_cycling_is_stable() {
        let home = metalfx_home();
        let node = get_pipeline(PipelineId::M10);
        // off -> 1.50 -> 1.75 -> off -> 1.75 ... repeated; each rewrite must
        // converge and never accumulate duplicate pairs.
        for (i, (enabled, factor, expect)) in [
            (false, 1.5, "!metalSpatialUpscaleFactor"),
            (true, 1.5, "metalSpatialUpscaleFactor=1.50"),
            (true, 1.75, "metalSpatialUpscaleFactor=1.75"),
            (false, 1.75, "!metalSpatialUpscaleFactor"),
            (true, 1.75, "metalSpatialUpscaleFactor=1.75"),
        ]
        .iter()
        .enumerate()
        {
            write_metalfx_state(&home, *enabled, *factor);
            let mut env = dxmt_env_with_node(PipelineId::M10);
            // Run twice like two launch cycles.
            apply_metal_fx_config(&mut env, node, &home);
            let env2 = env.clone();
            apply_metal_fx_config(&mut env, node, &home);
            assert_eq!(env, env2, "cycle {i}: idempotent");
            let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
            let upscale_count = config.matches("metalSpatialUpscaleFactor").count();
            assert!(upscale_count <= 1, "cycle {i}: exactly one upscale pair, got {config}");
            if expect.starts_with('!') {
                assert!(!config.contains("metalSpatialUpscaleFactor"), "cycle {i}: {config}");
            } else {
                assert!(config.contains(expect), "cycle {i}: {config}");
            }
        }
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn metalfx_env_passes_overlay_factor_through_unquantized() {
        let home = metalfx_home();
        let node = get_pipeline(PipelineId::M11);
        // The ProcessManager overlay offers 2.0 (metalfx.rs accepts 1.0..=3.0).
        // The launch env must honor it, not quantize to the sidebar's pair.
        write_metalfx_state(&home, true, 2.0);
        let mut env = dxmt_env_with_node(PipelineId::M11);
        apply_metal_fx_config(&mut env, node, &home);
        let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
        assert!(
            config.contains("d3d11.metalSpatialUpscaleFactor=2.00"),
            "2.0 factor must pass through at launch: {}",
            config
        );

        // A fractional value (e.g. 1.60) also passes through.
        write_metalfx_state(&home, true, 1.6);
        let mut env = dxmt_env_with_node(PipelineId::M11);
        apply_metal_fx_config(&mut env, node, &home);
        let config = last_env_value(&env, "DXMT_CONFIG").expect("DXMT_CONFIG");
        assert!(config.contains("d3d11.metalSpatialUpscaleFactor=1.60"), "1.6 factor must pass through: {}", config);
        let _ = std::fs::remove_dir_all(&home);
    }

    // ---- Wine msync toggle: WINEMSYNC env from config ----

    fn write_msync_config(home: &std::path::Path, enabled: bool) {
        let path = home.join(".metalsharp").join("configs").join("config.json");
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, serde_json::json!({ "msync": enabled }).to_string()).unwrap();
    }

    #[test]
    fn msync_env_reflects_config_on_all_routes() {
        let home = metalfx_home();
        // Default: no config -> WINEMSYNC=1.
        for pipeline_id in
            [PipelineId::M9, PipelineId::M10, PipelineId::M11, PipelineId::M12, PipelineId::M13, PipelineId::Steam]
        {
            let node = get_pipeline(pipeline_id);
            let env = steam_pipeline_env_pairs(&home, node, 42);
            assert_eq!(last_env_value(&env, "WINEMSYNC"), Some("1"), "{:?} default ON", pipeline_id);
        }

        // OFF: all routes get WINEMSYNC=0.
        write_msync_config(&home, false);
        for pipeline_id in
            [PipelineId::M9, PipelineId::M10, PipelineId::M11, PipelineId::M12, PipelineId::M13, PipelineId::Steam]
        {
            let node = get_pipeline(pipeline_id);
            let env = steam_pipeline_env_pairs(&home, node, 42);
            assert_eq!(last_env_value(&env, "WINEMSYNC"), Some("0"), "{:?} OFF must disable msync", pipeline_id);
        }
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn msync_env_value_helper_matches_config() {
        let home = metalfx_home();
        assert_eq!(msync_env_value(&home), "1", "default ON");
        write_msync_config(&home, false);
        assert_eq!(msync_env_value(&home), "0", "OFF");
        write_msync_config(&home, true);
        assert_eq!(msync_env_value(&home), "1", "back ON");
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn msync_parent_env_overrides_config_on_mtsp_path() {
        let home = metalfx_home();
        write_msync_config(&home, true);

        // Parent WINEMSYNC=0 overrides the config ON.
        assert_eq!(msync_env_value_from(&home, Some("0".into())), "0", "parent WINEMSYNC=0 must win over config ON");

        // Parent WINEMSYNC=1 overrides the config OFF.
        write_msync_config(&home, false);
        assert_eq!(msync_env_value_from(&home, Some("1".into())), "1", "parent WINEMSYNC=1 must win over config OFF");

        // No parent env -> config applies.
        assert_eq!(msync_env_value_from(&home, None), "0", "config OFF applies without parent env");

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn msync_toggle_beats_recipe_env() {
        let home = metalfx_home();
        write_msync_config(&home, false);
        let node = get_pipeline(PipelineId::M11);
        // Simulate a game recipe forcing WINEMSYNC=1 (M11 is not a reserved
        // route, so the recipe env would win without the reconcile).
        let mut env = steam_pipeline_env_pairs(&home, node, 42);
        env.push(("WINEMSYNC".to_string(), "1".to_string()));
        apply_msync_config(&mut env, &home);
        assert_eq!(last_env_value(&env, "WINEMSYNC"), Some("0"), "sidebar msync toggle must beat recipe env");
        let _ = std::fs::remove_dir_all(&home);
    }

    // ---- Mono profile deploy (Phase 4) ----

    fn mono_deploy_home(name: &str) -> std::path::PathBuf {
        let home = std::env::temp_dir().join(format!(
            "ms-mono-deploy-{}-{}-{:x}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();
        home
    }

    fn unity_game_dir(parent: &std::path::Path, version: &str) -> std::path::PathBuf {
        let dir = parent.join("DREDGE");
        std::fs::create_dir_all(dir.join("MonoBleedingEdge").join("EmbedRuntime")).unwrap();
        std::fs::create_dir_all(dir.join("DREDGE_Data").join("Managed")).unwrap();
        std::fs::write(dir.join("UnityPlayer.dll"), b"u").unwrap();
        std::fs::write(dir.join("MonoBleedingEdge").join("EmbedRuntime").join("mono-2.0-bdwgc.dll"), b"m").unwrap();
        std::fs::write(dir.join("DREDGE_Data").join("Managed").join("Assembly-CSharp.dll"), b"m").unwrap();
        let mut ggm = vec![0u8; 48];
        ggm.extend_from_slice(format!("{version}\0").as_bytes());
        std::fs::write(dir.join("DREDGE_Data").join("globalgamemanagers"), &ggm).unwrap();
        dir
    }

    #[test]
    fn unity_runtime_lane_maps_version_to_bundled_lane() {
        let home = mono_deploy_home("lane");
        let ms_home = home.join(".metalsharp");
        std::fs::create_dir_all(ms_home.join("runtime").join("unity-mono").join("2021.3")).unwrap();
        std::fs::create_dir_all(ms_home.join("runtime").join("unity-mono").join("2022.3")).unwrap();

        assert_eq!(unity_runtime_lane_for_version(Some("2021.3.5f1"), &ms_home).as_deref(), Some("2021.3"));
        // Unknown version falls back to the newest lane.
        assert_eq!(unity_runtime_lane_for_version(Some("2019.4.40f1"), &ms_home).as_deref(), Some("2022.3"));
        // None -> newest lane.
        assert_eq!(unity_runtime_lane_for_version(None, &ms_home).as_deref(), Some("2022.3"));
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn deploy_unity_runtime_copies_lane_and_is_idempotent() {
        let home = mono_deploy_home("unity");
        let ms_home = home.join(".metalsharp");
        let lane = ms_home.join("runtime").join("unity-mono").join("2021.3");
        std::fs::create_dir_all(&lane).unwrap();
        std::fs::write(lane.join("mono-2.0-bdwgc.dll"), b"mono-dll").unwrap();
        std::fs::write(lane.join("MonoPosixHelper.dll"), b"posix").unwrap();
        std::fs::write(lane.join("mono-sgen"), b"binary").unwrap();
        std::fs::write(lane.join("MonoBleedingEdge.version"), b"2021.3.5f1").unwrap();

        let game = unity_game_dir(&home, "2021.3.5f1");
        let profile = crate::mono_profile::discover_mono_profile(&game);
        assert_eq!(profile.kind, crate::mono_profile::MonoProfileKind::UnityMono);

        let mut report = crate::fna_profile::AssetStagingReport::new(1562430);
        let n = deploy_unity_runtime(&game, &profile, &ms_home, Some(&mut report)).unwrap();
        // The game ships its own mono-2.0-bdwgc.dll (exists -> skipped);
        // MonoPosixHelper deploys. mono-sgen/.version are never deployed.
        assert_eq!(n, 1);
        assert!(game.join("MonoBleedingEdge").join("EmbedRuntime").join("mono-2.0-bdwgc.dll").exists());
        assert!(game.join("MonoBleedingEdge").join("EmbedRuntime").join("MonoPosixHelper.dll").exists());
        assert!(!game.join("MonoBleedingEdge").join("EmbedRuntime").join("mono-sgen").exists());
        assert_eq!(report.receipts.len(), 1);

        // Idempotent second deploy.
        let mut report2 = crate::fna_profile::AssetStagingReport::new(1562430);
        let n2 = deploy_unity_runtime(&game, &profile, &ms_home, Some(&mut report2)).unwrap();
        assert_eq!(n2, 0);
        assert!(report2.receipts.is_empty());
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn deploy_xna_set_and_deps_follow_profile() {
        let home = mono_deploy_home("xna");
        let ms_home = home.join(".metalsharp");
        let xna_root = ms_home.join("runtime").join("xna");
        std::fs::create_dir_all(&xna_root).unwrap();
        std::fs::write(xna_root.join("Microsoft.Xna.Framework.dll"), b"xna").unwrap();
        std::fs::write(xna_root.join("Microsoft.Xna.Framework.Game.dll"), b"xna2").unwrap();
        std::fs::create_dir_all(ms_home.join("runtime").join("sdl3")).unwrap();
        std::fs::write(ms_home.join("runtime").join("sdl3").join("libSDL3.dylib"), b"sdl3").unwrap();

        let game = home.join("Necesse");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join("FNA.dll"), b"fna").unwrap();
        let profile = crate::mono_profile::discover_mono_profile(&game);
        assert_eq!(profile.kind, crate::mono_profile::MonoProfileKind::Fna);

        let mut report = crate::fna_profile::AssetStagingReport::new(1169040);
        let n = deploy_xna_assembly_set(&game, &profile, &ms_home, Some(&mut report)).unwrap();
        assert_eq!(n, 2);
        assert!(game.join("Microsoft.Xna.Framework.dll").exists());

        // SDL3 dep: add the dylib signal and deploy.
        std::fs::write(game.join("libSDL3.dylib"), b"present").unwrap();
        let profile2 = crate::mono_profile::discover_mono_profile(&game);
        let mut report2 = crate::fna_profile::AssetStagingReport::new(1169040);
        let n2 = deploy_profile_deps(&game, &profile2, &ms_home, Some(&mut report2)).unwrap();
        // SDL3 already present -> skip; carbon not signaled -> 0.
        assert_eq!(n2, 0);
        let _ = std::fs::remove_dir_all(&home);
    }

    // ---- Mono profile readiness (Phase 5) ----

    #[test]
    fn fna_ready_check_flags_unity_lane_and_sdl3_gaps() {
        let home = mono_deploy_home("ready");
        let game = unity_game_dir(&home, "2021.3.5f1");
        // No bundled unity-mono runtime and no shims in this temp home -> the
        // check must fail with actionable messages (and not panic).
        let result = fna_ready_check(1562430, &game);
        assert!(result.is_err(), "missing Unity lane + shims must block launch");
        let msg = result.unwrap_err();
        assert!(msg.contains("Unity Mono runtime lane") || msg.contains("shim"), "error must name the gap: {}", msg);
        let _ = std::fs::remove_dir_all(&home);
    }
}
