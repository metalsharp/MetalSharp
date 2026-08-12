use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};
use walkdir::WalkDir;

const VC_REDIST_X64_REQUIRED_DLLS: &[&str] = &["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"];
const VC_REDIST_X86_REQUIRED_DLLS: &[&str] = &["vcruntime140.dll", "msvcp140.dll"];
const VC_REDIST_SEED_DLLS: &[&str] = &[
    "concrt140.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll",
    "vcomp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
];
const GPTK_ROUTE_DLLS: &[&str] =
    &["d3d10.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "nvapi64.dll", "nvngx-on-metalfx.dll"];
const GAME_LOCAL_ROUTE_DLLS: &[&str] = &[
    "d3d10.dll",
    "d3d10core.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dxgi.dll",
    "nvapi64.dll",
    "nvngx.dll",
    "nvngx-on-metalfx.dll",
    "winemetal.dll",
];
const GPTK_EXTERNAL_PAYLOAD_FILES: &[&str] = &["libd3dshared.dylib", "D3DMetal.framework/Versions/A/D3DMetal"];
const GPTK_OVERRIDES: &str =
    "d3d10,d3d11,d3d12,dxgi,nvapi64,nvngx-on-metalfx=n,b;gameoverlayrenderer,gameoverlayrenderer64=d";

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum D3DMetalStepState {
    Missing,
    Installing,
    Installed,
    Updating,
    Updated,
    Seeding,
    Seeded,
    Failed,
    RepairRequired,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct D3DMetalGptkState {
    pub schema: u32,
    pub bottle_id: String,
    pub appid: u32,
    pub name: String,
    pub game_dir: String,
    pub game_exe: Option<String>,
    pub gptk_homebrew: D3DMetalStepState,
    pub rosetta: D3DMetalStepState,
    pub gptk_payload: D3DMetalStepState,
    pub x64_redist: D3DMetalStepState,
    pub seed: D3DMetalStepState,
    pub play_ready: bool,
    pub last_error: Option<String>,
    #[serde(default)]
    pub last_launch_pid: Option<u32>,
    #[serde(default)]
    pub last_launch_log: Option<String>,
    #[serde(default)]
    pub last_launch_status: Option<String>,
    pub updated_at: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct D3DMetalAction {
    pub id: String,
    pub label: String,
    pub enabled: bool,
    pub state: D3DMetalStepState,
    pub detail: String,
}

pub fn handle_status(body: &serde_json::Map<String, Value>) -> Value {
    match state_from_request(body) {
        Ok(state) => {
            let state = refresh_status_state(state);
            json!({"ok": true, "state": state, "actions": actions_for(&state)})
        },
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_save(body: &serde_json::Map<String, Value>) -> Value {
    match save_d3dmetal_bottle(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_install_homebrew_gptk(body: &serde_json::Map<String, Value>) -> Value {
    match install_homebrew_gptk(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_install_rosetta(body: &serde_json::Map<String, Value>) -> Value {
    match install_rosetta(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_repair_gptk_payload(body: &serde_json::Map<String, Value>) -> Value {
    match repair_gptk_payload(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_install_x64_redist(body: &serde_json::Map<String, Value>) -> Value {
    match install_x64_redist(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_seed_prefix(body: &serde_json::Map<String, Value>) -> Value {
    match seed_prefix(body) {
        Ok(state) => json!({"ok": true, "state": state, "actions": actions_for(&state)}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

pub fn handle_play(body: &serde_json::Map<String, Value>) -> Value {
    match play_d3dmetal(body) {
        Ok(report) => json!({"ok": true, "launch": report}),
        Err(e) => json!({"ok": false, "error": e}),
    }
}

fn save_d3dmetal_bottle(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let appid = request_appid(body)?;
    let bottle_id = request_bottle_id(body, appid)?;
    let game_dir = request_game_dir(body)?;
    let name = body
        .get("name")
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
        .unwrap_or("D3DMetal Game")
        .to_string();
    crate::bottles::load_bottle(&bottle_id)
        .map_err(|e| format!("D3DMetal save requires an existing bottle manifest for {}: {}", bottle_id, e))?;

    let mut state = load_state(&bottle_id).unwrap_or_else(|| new_state(&bottle_id, appid, &name, &game_dir));
    state.appid = appid;
    state.name = name;
    state.game_dir = game_dir.to_string_lossy().to_string();
    state.game_exe = find_game_exe(&game_dir);
    state.last_error = None;
    state.gptk_homebrew = D3DMetalStepState::Installing;
    state.rosetta = D3DMetalStepState::Installing;
    state.gptk_payload = D3DMetalStepState::Updating;
    save_state(&state)?;

    if let Err(e) = ensure_homebrew_gptk_trusted_and_installed() {
        state.gptk_homebrew = D3DMetalStepState::Failed;
        state.last_error = Some(e.clone());
        state.play_ready = false;
        save_state(&state)?;
        persist_d3dmetal_bottle_manifest_best_effort(&state);
        return Err(e);
    }
    state.gptk_homebrew = D3DMetalStepState::Installed;

    if let Err(e) = ensure_rosetta() {
        state.rosetta = D3DMetalStepState::Failed;
        state.last_error = Some(e.clone());
        state.play_ready = false;
        save_state(&state)?;
        persist_d3dmetal_bottle_manifest_best_effort(&state);
        return Err(e);
    }
    state.rosetta = D3DMetalStepState::Installed;

    if let Err(e) = ensure_homebrew_gptk_payload_ready() {
        state.gptk_payload = D3DMetalStepState::Failed;
        state.last_error = Some(e.clone());
        state.play_ready = false;
        save_state(&state)?;
        persist_d3dmetal_bottle_manifest_best_effort(&state);
        return Err(e);
    }
    state.gptk_payload = D3DMetalStepState::Updated;
    if state.x64_redist == D3DMetalStepState::Installed && state.seed == D3DMetalStepState::Seeded {
        state.play_ready = verify_seed(&state).is_ok();
    } else {
        state.play_ready = false;
    }
    state.updated_at = now_secs();
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn refresh_status_state(mut state: D3DMetalGptkState) -> D3DMetalGptkState {
    let original = state.clone();
    state.gptk_homebrew =
        if homebrew_gptk_installed() { D3DMetalStepState::Installed } else { D3DMetalStepState::Missing };
    state.gptk_payload = if state.gptk_homebrew == D3DMetalStepState::Installed {
        if verify_homebrew_gptk_payload() {
            D3DMetalStepState::Updated
        } else {
            D3DMetalStepState::RepairRequired
        }
    } else {
        D3DMetalStepState::Missing
    };
    state.rosetta = if rosetta_installed() { D3DMetalStepState::Installed } else { D3DMetalStepState::Missing };
    let x64_verified = verify_vc_redist_seeded().is_ok();
    if x64_verified {
        state.x64_redist = D3DMetalStepState::Installed;
    } else if matches!(
        state.x64_redist,
        D3DMetalStepState::Installed
            | D3DMetalStepState::Installing
            | D3DMetalStepState::Failed
            | D3DMetalStepState::RepairRequired
    ) {
        state.x64_redist = D3DMetalStepState::RepairRequired;
    }
    if verify_seed(&state).is_ok() && x64_verified {
        state.seed = D3DMetalStepState::Seeded;
    } else if matches!(
        state.seed,
        D3DMetalStepState::Seeded
            | D3DMetalStepState::Seeding
            | D3DMetalStepState::Failed
            | D3DMetalStepState::RepairRequired
    ) {
        state.seed = D3DMetalStepState::RepairRequired;
    }
    state.play_ready = state.gptk_payload == D3DMetalStepState::Updated
        && state.rosetta == D3DMetalStepState::Installed
        && state.x64_redist == D3DMetalStepState::Installed
        && state.seed == D3DMetalStepState::Seeded
        && verify_seed(&state).is_ok()
        && verify_vc_redist_seeded().is_ok();
    if state.play_ready {
        state.last_error = None;
    }
    if state.gptk_homebrew != original.gptk_homebrew
        || state.gptk_payload != original.gptk_payload
        || state.rosetta != original.rosetta
        || state.x64_redist != original.x64_redist
        || state.seed != original.seed
        || state.play_ready != original.play_ready
        || state.last_error != original.last_error
    {
        let _ = save_state(&state);
        persist_d3dmetal_bottle_manifest_best_effort(&state);
    }
    state
}

fn install_homebrew_gptk(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let mut state = state_from_request(body)?;
    state.gptk_homebrew = D3DMetalStepState::Installing;
    state.gptk_payload = D3DMetalStepState::Updating;
    state.play_ready = false;
    state.last_error = None;
    save_state(&state)?;

    match ensure_homebrew_gptk_trusted_and_installed().and_then(|_| ensure_homebrew_gptk_payload_ready()) {
        Ok(()) => {
            state.gptk_homebrew = D3DMetalStepState::Installed;
            state.gptk_payload = D3DMetalStepState::Updated;
            state.last_error = None;
        },
        Err(e) => {
            state.gptk_homebrew =
                if homebrew_gptk_installed() { D3DMetalStepState::Installed } else { D3DMetalStepState::Failed };
            state.gptk_payload = if state.gptk_homebrew == D3DMetalStepState::Installed {
                D3DMetalStepState::RepairRequired
            } else {
                D3DMetalStepState::Missing
            };
            state.last_error = Some(e.clone());
            save_state(&state)?;
            persist_d3dmetal_bottle_manifest_best_effort(&state);
            return Err(e);
        },
    }
    state.updated_at = now_secs();
    state = refresh_status_state(state);
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn install_rosetta(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let mut state = state_from_request(body)?;
    state.rosetta = D3DMetalStepState::Installing;
    state.play_ready = false;
    state.last_error = None;
    save_state(&state)?;

    match ensure_rosetta() {
        Ok(()) => {
            state.rosetta = D3DMetalStepState::Installed;
            state.last_error = None;
        },
        Err(e) => {
            state.rosetta = D3DMetalStepState::Failed;
            state.last_error = Some(e.clone());
            save_state(&state)?;
            persist_d3dmetal_bottle_manifest_best_effort(&state);
            return Err(e);
        },
    }
    state.updated_at = now_secs();
    state = refresh_status_state(state);
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn repair_gptk_payload(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let mut state = state_from_request(body)?;
    state.gptk_payload = D3DMetalStepState::Updating;
    state.play_ready = false;
    state.last_error = None;
    save_state(&state)?;

    match ensure_homebrew_gptk_payload_ready() {
        Ok(()) => {
            state.gptk_homebrew = D3DMetalStepState::Installed;
            state.gptk_payload = D3DMetalStepState::Updated;
            state.last_error = None;
        },
        Err(e) => {
            state.gptk_payload = D3DMetalStepState::RepairRequired;
            state.last_error = Some(e.clone());
            save_state(&state)?;
            persist_d3dmetal_bottle_manifest_best_effort(&state);
            return Err(e);
        },
    }
    state.updated_at = now_secs();
    state = refresh_status_state(state);
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn install_x64_redist(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let mut state = state_from_request(body)?;
    state.x64_redist = D3DMetalStepState::Installing;
    state.play_ready = false;
    state.last_error = None;
    save_state(&state)?;

    let result = (|| -> Result<(), String> {
        ensure_homebrew_gptk_ready_for_actions()?;
        ensure_gptk_prefix_winebooted()?;
        seed_vc_redist_dlls_and_registry()?;
        verify_vc_redist_seeded()?;
        write_redist_marker(&state)?;
        Ok(())
    })();

    match result {
        Ok(()) => {
            state.x64_redist = D3DMetalStepState::Installed;
            state.last_error = None;
        },
        Err(e) => {
            state.x64_redist = D3DMetalStepState::RepairRequired;
            state.last_error = Some(e.clone());
            save_state(&state)?;
            persist_d3dmetal_bottle_manifest_best_effort(&state);
            return Err(e);
        },
    }
    state.play_ready = state.seed == D3DMetalStepState::Seeded && verify_seed(&state).is_ok();
    state.updated_at = now_secs();
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn seed_prefix(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let mut state = state_from_request(body)?;
    state.seed = D3DMetalStepState::Seeding;
    state.play_ready = false;
    state.last_error = None;
    save_state(&state)?;

    let result = (|| -> Result<(), String> {
        if state.x64_redist != D3DMetalStepState::Installed {
            return Err("Run Repair Redist before seeding the D3DMetal GPTK prefix".to_string());
        }
        verify_vc_redist_seeded()?;
        ensure_homebrew_gptk_ready_for_actions()?;
        ensure_gptk_prefix_winebooted()?;
        seed_homebrew_gptk_route_dlls_into_prefix()?;
        stage_game_local_d3dmetal_route_dlls(&state)?;
        seed_steam_user_and_game_files(&state)?;
        verify_seed(&state)?;
        Ok(())
    })();

    match result {
        Ok(()) => {
            state.gptk_payload = D3DMetalStepState::Updated;
            state.seed = D3DMetalStepState::Seeded;
            state.play_ready = true;
            state.last_error = None;
        },
        Err(e) => {
            state.seed = D3DMetalStepState::RepairRequired;
            state.play_ready = false;
            state.last_error = Some(e.clone());
            save_state(&state)?;
            persist_d3dmetal_bottle_manifest_best_effort(&state);
            return Err(e);
        },
    }
    state.updated_at = now_secs();
    save_state(&state)?;
    persist_d3dmetal_bottle_manifest(&state)?;
    Ok(state)
}

fn play_d3dmetal(body: &serde_json::Map<String, Value>) -> Result<Value, String> {
    let mut state = state_from_request(body)?;
    if !state.play_ready {
        return Err("D3DMetal bottle is not ready; seed VC runtime DLLs and seed prefix first".to_string());
    }
    let game_exe = request_play_exe(body, &state)?;
    state.game_exe = Some(game_exe.to_string_lossy().to_string());
    stage_game_local_d3dmetal_route_dlls_for_exe(&state, &game_exe)?;
    verify_seed(&state)?;
    verify_game_local_d3dmetal_route_dlls_for_exe(&state, &game_exe)?;
    verify_vc_redist_seeded()?;
    if !rosetta_installed() {
        return Err(
            "Rosetta is required for D3DMetal GPTK play; save the D3DMetal bottle to install Rosetta".to_string()
        );
    }
    let launch_args = request_launch_args(body);

    let log_path = state_dir(&state.bottle_id).join("logs").join(format!("play-{}.log", now_secs()));
    if let Some(parent) = log_path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    let log = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .map_err(|e| format!("open {}: {}", log_path.display(), e))?;
    let log_err = log.try_clone().map_err(|e| format!("clone log handle: {}", e))?;

    let appid = state.appid.to_string();
    let mut cmd = Command::new(homebrew_wine64());
    cmd.arg(&game_exe)
        .args(&launch_args)
        .current_dir(game_exe.parent().unwrap_or_else(|| Path::new("/")))
        .env("WINEPREFIX", gptk_prefix())
        .env("WINEARCH", "win64")
        .env("WINEDEBUG", "-all")
        .env("WINEDLLOVERRIDES", GPTK_OVERRIDES)
        .env("D3DMETAL_FRAMEWORK_PATH", d3dmetal_framework_path())
        .env("WINEESYNC", "1")
        .env("SteamAppId", &appid)
        .env("SteamGameId", &appid)
        .env("SteamOverlayGameId", &appid)
        .env("SteamAppUser", "MetalSharp")
        .env("DYLD_FALLBACK_LIBRARY_PATH", gptk_dyld_path())
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(log_err));
    let child = cmd.spawn().map_err(|e| format!("launch D3DMetal game exe: {}", e))?;
    let pid = child.id();
    crate::register_game_pid(state.appid, pid);
    state.last_launch_pid = Some(pid);
    state.last_launch_log = Some(log_path.to_string_lossy().to_string());
    state.last_launch_status = Some("running".to_string());
    state.updated_at = now_secs();
    let persistence_error = save_state(&state).and_then(|_| persist_d3dmetal_launch_manifest(&state)).err();
    Ok(json!({
        "pid": pid,
        "appid": state.appid,
        "bottle_id": state.bottle_id,
        "game_exe": game_exe.to_string_lossy(),
        "launch_args": launch_args,
        "log_path": log_path.to_string_lossy(),
        "wine": homebrew_wine64().to_string_lossy(),
        "prefix": gptk_prefix().to_string_lossy(),
        "overrides": GPTK_OVERRIDES,
        "d3dmetal_framework_path": d3dmetal_framework_path().to_string_lossy(),
        "wineesync": "1",
        "launch_mode": "d3dmetal_direct_game_exe",
        "persistence_error": persistence_error
    }))
}

fn actions_for(state: &D3DMetalGptkState) -> Vec<D3DMetalAction> {
    let seed_repair = matches!(state.seed, D3DMetalStepState::RepairRequired | D3DMetalStepState::Failed);
    vec![
        D3DMetalAction {
            id: "install_homebrew_gptk".to_string(),
            label: "Repair Homebrew GPTK".to_string(),
            enabled: state.gptk_homebrew != D3DMetalStepState::Installed,
            state: state.gptk_homebrew.clone(),
            detail: "Tap gcenx/wine and install the Homebrew-owned game-porting-toolkit cask".to_string(),
        },
        D3DMetalAction {
            id: "install_rosetta".to_string(),
            label: "Repair Rosetta".to_string(),
            enabled: state.rosetta != D3DMetalStepState::Installed,
            state: state.rosetta.clone(),
            detail: "Install or verify Apple Rosetta 2 for x86_64 GPTK Wine".to_string(),
        },
        D3DMetalAction {
            id: "repair_gptk_payload".to_string(),
            label: "Repair GPTK Payload".to_string(),
            enabled: state.gptk_homebrew == D3DMetalStepState::Installed
                && state.gptk_payload != D3DMetalStepState::Updated,
            state: state.gptk_payload.clone(),
            detail: "Verify Homebrew GPTK route DLLs and D3DMetal.framework payload files".to_string(),
        },
        D3DMetalAction {
            id: "install_x64_redist".to_string(),
            label: "Repair Redist".to_string(),
            enabled: state.gptk_homebrew == D3DMetalStepState::Installed
                && state.gptk_payload == D3DMetalStepState::Updated,
            state: state.x64_redist.clone(),
            detail:
                "Copy MetalSharp-bundled VC runtime DLLs into GPTK system32/syswow64 and write runtime registry keys"
                    .to_string(),
        },
        D3DMetalAction {
            id: "seed_prefix".to_string(),
            label: if seed_repair { "Repair Seed" } else { "Seed Prefix" }.to_string(),
            enabled: state.x64_redist == D3DMetalStepState::Installed,
            state: state.seed.clone(),
            detail: "Wineboot the GPTK prefix, copy Homebrew GPTK route DLLs into system32, quarantine app-local shims, and seed launch material".to_string(),
        },
        D3DMetalAction {
            id: "play_d3dmetal".to_string(),
            label: "Play D3DMetal".to_string(),
            enabled: state.play_ready,
            state: if state.play_ready { D3DMetalStepState::Installed } else { D3DMetalStepState::Missing },
            detail: "Launch the game exe directly through Homebrew GPTK Wine with Homebrew-matched D3DMetal route DLLs".to_string(),
        },
    ]
}

fn new_state(bottle_id: &str, appid: u32, name: &str, game_dir: &Path) -> D3DMetalGptkState {
    D3DMetalGptkState {
        schema: 1,
        bottle_id: bottle_id.to_string(),
        appid,
        name: name.to_string(),
        game_dir: game_dir.to_string_lossy().to_string(),
        game_exe: find_game_exe(game_dir),
        gptk_homebrew: D3DMetalStepState::Missing,
        rosetta: D3DMetalStepState::Missing,
        gptk_payload: D3DMetalStepState::Missing,
        x64_redist: D3DMetalStepState::Missing,
        seed: D3DMetalStepState::Missing,
        play_ready: false,
        last_error: None,
        last_launch_pid: None,
        last_launch_log: None,
        last_launch_status: None,
        updated_at: now_secs(),
    }
}

fn state_from_request(body: &serde_json::Map<String, Value>) -> Result<D3DMetalGptkState, String> {
    let appid = optional_request_appid(body)?;
    let bottle_id = body.get("bottleId").or_else(|| body.get("bottle_id")).and_then(Value::as_str).map(str::to_string);
    if let Some(id) = bottle_id {
        let id = validate_d3dmetal_bottle_id(&id)?;
        return active_d3dmetal_state(&id)
            .or_else(|| state_from_bottle_manifest(&id, appid))
            .ok_or_else(|| format!("D3DMetal GPTK state not found for {}", id));
    }
    if let Some(appid) = appid {
        let id = format!("steam_{}", appid);
        return active_d3dmetal_state(&id)
            .or_else(|| state_from_bottle_manifest(&id, Some(appid)))
            .ok_or_else(|| format!("D3DMetal GPTK state not found for appid {}", appid));
    }
    Err("appid or bottleId required".to_string())
}

fn active_d3dmetal_state(bottle_id: &str) -> Option<D3DMetalGptkState> {
    if !bottle_manifest_is_d3dmetal(bottle_id) {
        return None;
    }
    load_state(bottle_id)
}

fn bottle_manifest_is_d3dmetal(bottle_id: &str) -> bool {
    crate::bottles::load_bottle(bottle_id)
        .map(|manifest| {
            manifest.runtime_profile == crate::bottles::RuntimeProfile::D3DMetal
                || manifest.preferred_pipeline.as_deref() == Some("d3dmetal")
        })
        .unwrap_or(false)
}

fn state_from_bottle_manifest(bottle_id: &str, request_appid: Option<u32>) -> Option<D3DMetalGptkState> {
    let manifest = crate::bottles::load_bottle(bottle_id).ok()?;
    let is_d3dmetal = manifest.runtime_profile == crate::bottles::RuntimeProfile::D3DMetal
        || manifest.preferred_pipeline.as_deref() == Some("d3dmetal");
    if !is_d3dmetal {
        return None;
    }
    let appid = manifest.steam_app_id.or(request_appid)?;
    let game_dir = manifest.game_install_path.as_ref().map(PathBuf::from)?;
    if !game_dir.is_dir() {
        return None;
    }
    Some(new_state(bottle_id, appid, &manifest.name, &game_dir))
}

fn optional_request_appid(body: &serde_json::Map<String, Value>) -> Result<Option<u32>, String> {
    body.get("appid")
        .map(|value| {
            value
                .as_u64()
                .filter(|v| *v > 0 && *v <= u32::MAX as u64)
                .map(|v| v as u32)
                .ok_or_else(|| "appid must be a positive u32".to_string())
        })
        .transpose()
}

fn request_appid(body: &serde_json::Map<String, Value>) -> Result<u32, String> {
    optional_request_appid(body)?.ok_or_else(|| "appid required".to_string())
}

fn request_bottle_id(body: &serde_json::Map<String, Value>, appid: u32) -> Result<String, String> {
    let id = body
        .get("bottleId")
        .or_else(|| body.get("bottle_id"))
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| format!("steam_{}", appid));
    validate_d3dmetal_bottle_id(&id)
}

fn validate_d3dmetal_bottle_id(id: &str) -> Result<String, String> {
    let valid = !id.is_empty()
        && id.len() <= 128
        && id.bytes().all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'));
    if valid {
        Ok(id.to_string())
    } else {
        Err("invalid D3DMetal bottleId".to_string())
    }
}

fn request_play_exe(body: &serde_json::Map<String, Value>, state: &D3DMetalGptkState) -> Result<PathBuf, String> {
    let raw = body
        .get("gameExe")
        .or_else(|| body.get("game_exe"))
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
        .map(PathBuf::from)
        .or_else(|| state.game_exe.as_ref().map(PathBuf::from))
        .ok_or_else(|| "D3DMetal state has no game exe".to_string())?;
    if !raw.is_file() {
        return Err(format!("game exe missing: {}", raw.display()));
    }
    let exe_canonical = raw.canonicalize().map_err(|e| format!("canonicalize game exe {}: {}", raw.display(), e))?;
    let game_dir_canonical = Path::new(&state.game_dir)
        .canonicalize()
        .map_err(|e| format!("canonicalize game dir {}: {}", state.game_dir, e))?;
    if !exe_canonical.starts_with(&game_dir_canonical) {
        return Err(format!(
            "D3DMetal play exe must be inside saved game dir: {} not under {}",
            exe_canonical.display(),
            game_dir_canonical.display()
        ));
    }
    Ok(exe_canonical)
}

fn request_launch_args(body: &serde_json::Map<String, Value>) -> Vec<String> {
    body.get("launchArgs")
        .or_else(|| body.get("launch_args"))
        .and_then(Value::as_array)
        .map(|items| items.iter().filter_map(Value::as_str).map(str::to_string).collect())
        .unwrap_or_default()
}

fn request_game_dir(body: &serde_json::Map<String, Value>) -> Result<PathBuf, String> {
    let raw = body
        .get("gameDir")
        .or_else(|| body.get("game_dir"))
        .or_else(|| body.get("gamePath"))
        .or_else(|| body.get("game_path"))
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
        .ok_or_else(|| "gameDir required for explicit D3DMetal save".to_string())?;
    let path = expand_tilde(raw);
    if path.is_dir() {
        Ok(path)
    } else {
        Err(format!("gameDir does not exist: {}", path.display()))
    }
}

fn load_state(bottle_id: &str) -> Option<D3DMetalGptkState> {
    let path = state_path(bottle_id);
    let text = fs::read_to_string(path).ok()?;
    serde_json::from_str(&text).ok()
}

fn save_state(state: &D3DMetalGptkState) -> Result<(), String> {
    let mut state = state.clone();
    state.updated_at = now_secs();
    let path = state_path(&state.bottle_id);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    let text = serde_json::to_string_pretty(&state).map_err(|e| format!("serialize D3DMetal state: {}", e))?;
    fs::write(&path, text).map_err(|e| format!("write {}: {}", path.display(), e))
}

fn persist_d3dmetal_bottle_manifest(state: &D3DMetalGptkState) -> Result<(), String> {
    let mut manifest = crate::bottles::load_bottle(&state.bottle_id)
        .map_err(|e| format!("load bottle manifest {} for D3DMetal persistence: {}", state.bottle_id, e))?;
    manifest.name = state.name.clone();
    manifest.runtime_profile = crate::bottles::RuntimeProfile::D3DMetal;
    manifest.arch = crate::bottles::BottleArch::Win64;
    manifest.preferred_pipeline = Some("d3dmetal".to_string());
    manifest.steam_app_id = Some(state.appid);
    manifest.game_install_path = Some(state.game_dir.clone());
    manifest.installed_components = d3dmetal_manifest_components(state);
    manifest.health =
        if state.play_ready { crate::bottles::BottleHealth::Ready } else { crate::bottles::BottleHealth::NeedsRepair };
    crate::bottles::save_bottle(&manifest)
        .map_err(|e| format!("save bottle manifest {} for D3DMetal persistence: {}", state.bottle_id, e))
}

fn d3dmetal_manifest_components(state: &D3DMetalGptkState) -> Vec<crate::bottles::RuntimeComponent> {
    use crate::bottles::{ComponentState, RuntimeComponent};
    let step_state = |step: &D3DMetalStepState| match step {
        D3DMetalStepState::Installed | D3DMetalStepState::Updated | D3DMetalStepState::Seeded => {
            ComponentState::Installed
        },
        D3DMetalStepState::Failed | D3DMetalStepState::RepairRequired => ComponentState::NeedsRepair,
        D3DMetalStepState::Missing => ComponentState::Missing,
        D3DMetalStepState::Installing | D3DMetalStepState::Updating | D3DMetalStepState::Seeding => {
            ComponentState::Unknown
        },
    };
    let gptk_state = match (&state.gptk_homebrew, &state.gptk_payload) {
        (D3DMetalStepState::Installed, D3DMetalStepState::Updated) => ComponentState::Installed,
        (D3DMetalStepState::Failed, _) | (_, D3DMetalStepState::Failed | D3DMetalStepState::RepairRequired) => {
            ComponentState::NeedsRepair
        },
        (D3DMetalStepState::Missing, _) => ComponentState::Missing,
        _ => ComponentState::Unknown,
    };
    vec![
        RuntimeComponent { id: "gptk".to_string(), state: gptk_state },
        RuntimeComponent { id: "rosetta".to_string(), state: step_state(&state.rosetta) },
        RuntimeComponent { id: "gptk_prefix".to_string(), state: step_state(&state.seed) },
        RuntimeComponent { id: "vcrun2019_x64".to_string(), state: step_state(&state.x64_redist) },
        RuntimeComponent { id: "vcrun2019_x86".to_string(), state: step_state(&state.x64_redist) },
    ]
}

fn persist_d3dmetal_bottle_manifest_best_effort(state: &D3DMetalGptkState) {
    let _ = persist_d3dmetal_bottle_manifest(state);
}

fn persist_d3dmetal_launch_manifest(state: &D3DMetalGptkState) -> Result<(), String> {
    let mut manifest = crate::bottles::load_bottle(&state.bottle_id)
        .map_err(|e| format!("load bottle manifest {} for D3DMetal launch: {}", state.bottle_id, e))?;
    manifest.runtime_profile = crate::bottles::RuntimeProfile::D3DMetal;
    manifest.preferred_pipeline = Some("d3dmetal".to_string());
    manifest.installed_components = d3dmetal_manifest_components(state);
    manifest.health =
        if state.play_ready { crate::bottles::BottleHealth::Ready } else { crate::bottles::BottleHealth::NeedsRepair };
    manifest.last_launch_pid = state.last_launch_pid;
    manifest.last_launch_log = state.last_launch_log.clone();
    manifest.last_launch_status = state.last_launch_status.clone();
    manifest.last_launch_finished_at = None;
    crate::bottles::save_bottle(&manifest)
        .map_err(|e| format!("save bottle manifest {} for D3DMetal launch: {}", state.bottle_id, e))
}

fn state_path(bottle_id: &str) -> PathBuf {
    state_dir(bottle_id).join("state.json")
}

fn state_dir(bottle_id: &str) -> PathBuf {
    metalsharp_home().join("d3dmetal-gptk").join("bottles").join(bottle_id)
}

fn metalsharp_home() -> PathBuf {
    std::env::var_os("METALSHARP_HOME")
        .map(PathBuf::from)
        .or_else(|| dirs::home_dir().map(|home| home.join(".metalsharp")))
        .unwrap_or_else(|| PathBuf::from(".metalsharp"))
}

fn ensure_homebrew_gptk_ready_for_actions() -> Result<(), String> {
    if !homebrew_gptk_installed() {
        return Err("Homebrew GPTK is missing; save the D3DMetal bottle first".to_string());
    }
    if !verify_homebrew_gptk_payload() {
        return Err("Homebrew GPTK payload is incomplete; reinstall Homebrew GPTK and save the D3DMetal bottle again"
            .to_string());
    }
    Ok(())
}

fn ensure_homebrew_gptk_trusted_and_installed() -> Result<(), String> {
    let brew = find_brew()?;
    let tap = Command::new(&brew)
        .args(["tap", "gcenx/wine"])
        .output()
        .map_err(|e| format!("brew tap gcenx/wine failed: {}", e))?;
    let tap_text = command_text(&tap);
    if !tap.status.success() && !tap_text.contains("already tapped") {
        return Err(format!(
            "brew tap gcenx/wine failed: {}",
            tap_text.lines().last().unwrap_or("unknown brew tap error")
        ));
    }

    let trust = Command::new(&brew)
        .args(["trust", "--cask", "gcenx/wine/game-porting-toolkit"])
        .output()
        .map_err(|e| format!("brew trust failed: {}", e))?;
    let trust_text = command_text(&trust);
    if !trust.status.success() && !trust_text.contains("Trusted cask") && !trust_text.contains("already trusted") {
        return Err(format!(
            "brew trust --cask gcenx/wine/game-porting-toolkit failed: {}",
            trust_text.lines().last().unwrap_or("unknown brew trust error")
        ));
    }

    let output = Command::new(&brew)
        .args(["install", "--cask", "gcenx/wine/game-porting-toolkit"])
        .output()
        .map_err(|e| format!("brew install --cask gcenx/wine/game-porting-toolkit failed: {}", e))?;
    let text = command_text(&output);
    if !output.status.success()
        && !text.contains("already installed")
        && !text.contains("Not upgrading game-porting-toolkit")
    {
        return Err(format!(
            "brew install --cask gcenx/wine/game-porting-toolkit failed: {}",
            text.lines().last().unwrap_or("unknown brew install error")
        ));
    }
    if !homebrew_gptk_installed() {
        return Err(
            "brew install --cask gcenx/wine/game-porting-toolkit completed but Homebrew GPTK wine64/wineserver were not found"
                .to_string(),
        );
    }
    Ok(())
}

fn rosetta_installed() -> bool {
    Command::new("/usr/sbin/pkgutil")
        .args(["--pkg-info", "com.apple.pkg.RosettaUpdateAuto"])
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
        || Command::new("/usr/bin/arch")
            .args(["-x86_64", "/usr/bin/true"])
            .status()
            .map(|status| status.success())
            .unwrap_or(false)
}

fn ensure_rosetta() -> Result<(), String> {
    let output = Command::new("/usr/sbin/softwareupdate")
        .args(["--install-rosetta", "--agree-to-license"])
        .output()
        .map_err(|e| format!("softwareupdate --install-rosetta failed: {}", e))?;
    let text = command_text(&output);
    if output.status.success()
        || text.contains("Rosetta 2 is already installed")
        || text.contains("Install of Rosetta 2 finished successfully")
    {
        Ok(())
    } else {
        Err(format!(
            "softwareupdate --install-rosetta failed: {}",
            text.lines().last().unwrap_or("unknown Rosetta error")
        ))
    }
}

fn ensure_homebrew_gptk_payload_ready() -> Result<(), String> {
    if !homebrew_gptk_installed() {
        return Err("Homebrew GPTK is not installed".to_string());
    }
    if verify_homebrew_gptk_payload() {
        Ok(())
    } else {
        Err("Homebrew GPTK payload is incomplete; reinstall game-porting-toolkit and seed again".to_string())
    }
}

fn verify_homebrew_gptk_payload() -> bool {
    let wine_dll_dir = homebrew_gptk_route_dll_dir();
    let external = homebrew_wine_root().join("lib").join("external");
    GPTK_ROUTE_DLLS.iter().all(|dll| file_nonempty(&wine_dll_dir.join(dll)))
        && GPTK_EXTERNAL_PAYLOAD_FILES.iter().all(|rel| file_nonempty(&external.join(rel)))
        && framework_ready(&external.join("D3DMetal.framework"))
}

fn seed_homebrew_gptk_route_dlls_into_prefix() -> Result<(), String> {
    ensure_homebrew_gptk_payload_ready()?;
    let src_dir = homebrew_gptk_route_dll_dir();
    let dst_dir = gptk_prefix().join("drive_c").join("windows").join("system32");
    fs::create_dir_all(&dst_dir).map_err(|e| format!("create {}: {}", dst_dir.display(), e))?;
    for dll in GPTK_ROUTE_DLLS {
        copy_file_checked(&src_dir.join(dll), &dst_dir.join(dll))?;
    }
    verify_prefix_route_dlls()
}

fn verify_prefix_route_dlls() -> Result<(), String> {
    let src_dir = homebrew_gptk_route_dll_dir();
    let dst_dir = gptk_prefix().join("drive_c").join("windows").join("system32");
    let mismatched: Vec<&str> =
        GPTK_ROUTE_DLLS.iter().copied().filter(|dll| !same_file_hash(&src_dir.join(dll), &dst_dir.join(dll))).collect();
    if mismatched.is_empty() {
        Ok(())
    } else {
        Err(format!("GPTK prefix route DLLs are missing or do not match Homebrew GPTK: {}", mismatched.join(", ")))
    }
}

fn stage_game_local_d3dmetal_route_dlls(state: &D3DMetalGptkState) -> Result<(), String> {
    let game_exe = state
        .game_exe
        .as_ref()
        .map(PathBuf::from)
        .ok_or_else(|| "no game exe detected for D3DMetal route DLL staging".to_string())?;
    stage_game_local_d3dmetal_route_dlls_for_exe(state, &game_exe)
}

fn stage_game_local_d3dmetal_route_dlls_for_exe(state: &D3DMetalGptkState, game_exe: &Path) -> Result<(), String> {
    ensure_homebrew_gptk_payload_ready()?;
    let exe_dir =
        game_exe.parent().ok_or_else(|| format!("game exe has no parent directory: {}", game_exe.display()))?;
    if !exe_dir.is_dir() {
        return Err(format!("game exe directory missing: {}", exe_dir.display()));
    }

    let src_dir = homebrew_gptk_route_dll_dir();
    let quarantine_root =
        Path::new(&state.game_dir).join(".metalsharp").join("d3dmetal-quarantine").join(now_secs().to_string());
    let mut moved = Vec::new();
    let mut deployed = Vec::new();

    for dll in GAME_LOCAL_ROUTE_DLLS {
        let path = exe_dir.join(dll);
        let is_gptk_route_dll = GPTK_ROUTE_DLLS.iter().any(|candidate| candidate.eq_ignore_ascii_case(dll));
        if is_gptk_route_dll {
            let src = src_dir.join(dll);
            if path.exists() && !same_file_hash(&src, &path) {
                let target = unique_quarantine_target(&quarantine_root.join(quarantine_relative_path(state, &path)));
                if let Some(parent) = target.parent() {
                    fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
                }
                fs::rename(&path, &target).map_err(|e| {
                    format!("quarantine stale app-local route DLL {} -> {}: {}", path.display(), target.display(), e)
                })?;
                moved.push(json!({"from": path.to_string_lossy(), "to": target.to_string_lossy()}));
            }
            copy_file_checked(&src, &path)?;
            deployed.push(json!({"filename": dll, "source": src.to_string_lossy(), "dest": path.to_string_lossy()}));
        } else if path.exists() {
            let target = unique_quarantine_target(&quarantine_root.join(quarantine_relative_path(state, &path)));
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
            }
            fs::rename(&path, &target).map_err(|e| {
                format!("quarantine stale app-local Vkd3d route DLL {} -> {}: {}", path.display(), target.display(), e)
            })?;
            moved.push(json!({"from": path.to_string_lossy(), "to": target.to_string_lossy()}));
        }
    }

    if !moved.is_empty() || !deployed.is_empty() {
        let marker_root = Path::new(&state.game_dir).join(".metalsharp").join("d3dmetal-quarantine");
        fs::create_dir_all(&marker_root).map_err(|e| format!("create {}: {}", marker_root.display(), e))?;
        let marker = marker_root.join("latest-manifest.json");
        let manifest = json!({
            "quarantined_at": now_secs(),
            "reason": "D3DMetal/GPTK lane replaces app-local Vkd3d/DXMT route DLLs with Homebrew-matched D3DMetal route DLLs",
            "moved": moved,
            "deployed": deployed,
        });
        fs::write(&marker, serde_json::to_string_pretty(&manifest).unwrap_or_default())
            .map_err(|e| format!("write {}: {}", marker.display(), e))?;
    }

    verify_game_local_d3dmetal_route_dlls_for_exe(state, game_exe)
}

fn verify_game_local_d3dmetal_route_dlls(state: &D3DMetalGptkState) -> Result<(), String> {
    let Some(game_exe) = state.game_exe.as_ref().map(PathBuf::from) else {
        return Err("D3DMetal seed game exe missing".to_string());
    };
    verify_game_local_d3dmetal_route_dlls_for_exe(state, &game_exe)
}

fn quarantine_relative_path(state: &D3DMetalGptkState, path: &Path) -> PathBuf {
    if let Ok(rel) = path.strip_prefix(&state.game_dir) {
        if !rel.as_os_str().is_empty() && !rel.is_absolute() {
            return rel.to_path_buf();
        }
    }
    if let (Ok(base), Ok(canonical_path)) = (Path::new(&state.game_dir).canonicalize(), path.canonicalize()) {
        if let Ok(rel) = canonical_path.strip_prefix(base) {
            if !rel.as_os_str().is_empty() && !rel.is_absolute() {
                return rel.to_path_buf();
            }
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

fn verify_game_local_d3dmetal_route_dlls_for_exe(_state: &D3DMetalGptkState, game_exe: &Path) -> Result<(), String> {
    let Some(exe_dir) = game_exe.parent() else {
        return Err(format!("game exe has no parent directory: {}", game_exe.display()));
    };
    let src_dir = homebrew_gptk_route_dll_dir();
    let mut bad = Vec::new();
    for dll in GPTK_ROUTE_DLLS {
        let src = src_dir.join(dll);
        let dst = exe_dir.join(dll);
        if !same_file_hash(&src, &dst) {
            bad.push((*dll).to_string());
        }
    }
    let stale_vkd3d: Vec<String> = GAME_LOCAL_ROUTE_DLLS
        .iter()
        .filter(|dll| !GPTK_ROUTE_DLLS.iter().any(|candidate| candidate.eq_ignore_ascii_case(dll)))
        .map(|dll| exe_dir.join(dll))
        .filter(|path| path.is_file())
        .map(|path| path.to_string_lossy().to_string())
        .collect();
    if bad.is_empty() && stale_vkd3d.is_empty() {
        Ok(())
    } else {
        Err(format!(
            "app-local D3DMetal route DLLs are not current; mismatched=[{}] stale_vkd3d=[{}]",
            bad.join(", "),
            stale_vkd3d.join(", ")
        ))
    }
}

fn is_d3dmetal_route_conflict(path: &Path) -> bool {
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
    GAME_LOCAL_ROUTE_DLLS.iter().any(|dll| dll.eq_ignore_ascii_case(&name))
}

fn ensure_gptk_prefix_winebooted() -> Result<(), String> {
    let prefix = gptk_prefix();
    fs::create_dir_all(&prefix).map_err(|e| format!("create GPTK prefix {}: {}", prefix.display(), e))?;
    let _ = Command::new(homebrew_wineserver()).env("WINEPREFIX", &prefix).arg("-k").status();
    let output = Command::new(homebrew_wine64())
        .env("WINEPREFIX", &prefix)
        .env("WINEARCH", "win64")
        .env("WINEDEBUG", "-all")
        .env("DYLD_FALLBACK_LIBRARY_PATH", gptk_dyld_path())
        .env("D3DMETAL_FRAMEWORK_PATH", d3dmetal_framework_path())
        .arg("wineboot")
        .arg("--init")
        .output()
        .map_err(|e| format!("GPTK wineboot failed: {}", e))?;
    if !output.status.success() {
        return Err(format!("GPTK wineboot --init failed: {}", command_text(&output)));
    }
    let wait = Command::new(homebrew_wineserver())
        .env("WINEPREFIX", &prefix)
        .arg("-w")
        .status()
        .map_err(|e| format!("GPTK wineserver -w failed: {}", e))?;
    if !wait.success() {
        return Err(format!("GPTK wineserver -w exited with {:?}", wait.code()));
    }
    for rel in ["drive_c", "system.reg", "user.reg", "dosdevices"] {
        if !prefix.join(rel).exists() {
            return Err(format!("GPTK prefix wineboot missing {}", rel));
        }
    }
    Ok(())
}

fn seed_vc_redist_dlls_and_registry() -> Result<(), String> {
    let runtime_wine = metalsharp_home().join("runtime").join("wine").join("lib").join("wine");
    let prefix = gptk_prefix();
    let system32 = prefix.join("drive_c").join("windows").join("system32");
    let syswow64 = prefix.join("drive_c").join("windows").join("syswow64");

    seed_vc_redist_arch(&runtime_wine.join("x86_64-windows"), &system32, VC_REDIST_X64_REQUIRED_DLLS)?;
    seed_vc_redist_arch(&runtime_wine.join("i386-windows"), &syswow64, VC_REDIST_X86_REQUIRED_DLLS)?;
    write_vc_redist_registry_keys()?;
    Ok(())
}

fn seed_vc_redist_arch(src_dir: &Path, dst_dir: &Path, required: &[&str]) -> Result<(), String> {
    if !src_dir.is_dir() {
        return Err(format!("MetalSharp VC runtime source directory missing: {}", src_dir.display()));
    }
    fs::create_dir_all(dst_dir).map_err(|e| format!("create {}: {}", dst_dir.display(), e))?;
    for dll in required {
        let src = src_dir.join(dll);
        if !file_nonempty(&src) {
            return Err(format!("MetalSharp VC runtime source DLL missing: {}", src.display()));
        }
    }
    for dll in VC_REDIST_SEED_DLLS {
        let src = src_dir.join(dll);
        if file_nonempty(&src) {
            copy_file_checked(&src, &dst_dir.join(dll))?;
        }
    }
    Ok(())
}

fn write_vc_redist_registry_keys() -> Result<(), String> {
    let system_reg = gptk_prefix().join("system.reg");
    if let Some(parent) = system_reg.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    let timestamp = now_secs();
    let mut file = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&system_reg)
        .map_err(|e| format!("open {}: {}", system_reg.display(), e))?;
    write!(
        file,
        r#"
[Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64] {}
"Bld"=dword:0000611c
"Installed"=dword:00000001
"Major"=dword:0000000e
"Minor"=dword:0000002c
"Rbld"=dword:00000000
"Version"="v14.44.24828.0"

[Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x86] {}
"Bld"=dword:0000611c
"Installed"=dword:00000001
"Major"=dword:0000000e
"Minor"=dword:0000002c
"Rbld"=dword:00000000
"Version"="v14.44.24828.0"
"#,
        timestamp, timestamp
    )
    .map_err(|e| format!("write {}: {}", system_reg.display(), e))
}

fn verify_vc_redist_seeded() -> Result<(), String> {
    let prefix = gptk_prefix();
    let system32 = prefix.join("drive_c").join("windows").join("system32");
    let syswow64 = prefix.join("drive_c").join("windows").join("syswow64");
    let missing_x64: Vec<&str> =
        VC_REDIST_X64_REQUIRED_DLLS.iter().copied().filter(|dll| !file_nonempty(&system32.join(dll))).collect();
    let missing_x86: Vec<&str> =
        VC_REDIST_X86_REQUIRED_DLLS.iter().copied().filter(|dll| !file_nonempty(&syswow64.join(dll))).collect();
    if !missing_x64.is_empty() || !missing_x86.is_empty() {
        return Err(format!(
            "VC runtime seed did not verify in GPTK prefix; missing x64 [{}], x86 [{}]",
            missing_x64.join(", "),
            missing_x86.join(", ")
        ));
    }
    let system_reg = fs::read_to_string(prefix.join("system.reg")).unwrap_or_default();
    let x64 = vcredist_registry_installed(&system_reg, "x64");
    let x86 = vcredist_registry_installed(&system_reg, "x86");
    if x64 && x86 {
        Ok(())
    } else {
        Err(format!("VC runtime registry seed did not verify in GPTK prefix (x64={}, x86={})", x64, x86))
    }
}

fn vcredist_registry_installed(system_reg: &str, arch: &str) -> bool {
    let needle = format!("software\\microsoft\\visualstudio\\14.0\\vc\\runtimes\\{}", arch);
    let wow_needle = format!("software\\wow6432node\\microsoft\\visualstudio\\14.0\\vc\\runtimes\\{}", arch);
    let mut in_runtime_key = false;
    for raw_line in system_reg.lines() {
        let line = raw_line.trim().to_ascii_lowercase().replace("\\\\", "\\");
        if let Some(section_end) = line.strip_prefix('[').and_then(|rest| rest.find(']').map(|idx| idx + 1)) {
            let section = &line[..section_end];
            in_runtime_key = section.contains(&needle) || section.contains(&wow_needle);
            continue;
        }
        if in_runtime_key && line == "\"installed\"=dword:00000001" {
            return true;
        }
    }
    false
}

fn write_redist_marker(state: &D3DMetalGptkState) -> Result<(), String> {
    let marker = state_dir(&state.bottle_id).join("vc-runtime-seeded.json");
    if let Some(parent) = marker.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    let prefix = gptk_prefix();
    let mut copied = Vec::new();
    for (arch, dir) in [
        ("x64", prefix.join("drive_c").join("windows").join("system32")),
        ("x86", prefix.join("drive_c").join("windows").join("syswow64")),
    ] {
        for dll in VC_REDIST_SEED_DLLS {
            let path = dir.join(dll);
            if file_nonempty(&path) {
                copied.push(json!({
                    "arch": arch,
                    "dll": dll,
                    "path": path.to_string_lossy(),
                    "sha256": file_sha256(&path).unwrap_or_default()
                }));
            }
        }
    }
    let value = json!({
        "seeded_at": now_secs(),
        "source": metalsharp_home().join("runtime").join("wine").join("lib").join("wine").to_string_lossy(),
        "method": "copy_dlls_and_write_registry",
        "prefix": prefix.to_string_lossy(),
        "copied_dlls": copied
    });
    fs::write(&marker, serde_json::to_string_pretty(&value).unwrap_or_default())
        .map_err(|e| format!("write {}: {}", marker.display(), e))
}

fn seed_steam_user_and_game_files(state: &D3DMetalGptkState) -> Result<(), String> {
    let seed_root = gptk_prefix().join("drive_c").join("metalsharp").join("d3dmetal").join(state.appid.to_string());
    fs::create_dir_all(&seed_root).map_err(|e| format!("create seed root {}: {}", seed_root.display(), e))?;

    let steam_prefix = metalsharp_home().join("prefix-steam");
    let steam_user_src = steam_prefix.join("drive_c").join("users");
    let steam_user_dst = seed_root.join("steam-users");
    if steam_user_src.is_dir() {
        copy_dir_replace(&steam_user_src, &steam_user_dst)?;
    }

    let steam_config_src = steam_prefix.join("drive_c").join("Program Files (x86)").join("Steam").join("config");
    let steam_config_dst = seed_root.join("steam-config");
    if steam_config_src.is_dir() {
        copy_dir_replace(&steam_config_src, &steam_config_dst)?;
    }

    let game_exe = state.game_exe.as_ref().ok_or_else(|| "no game exe detected for D3DMetal seed".to_string())?;
    let game_exe = PathBuf::from(game_exe);
    if !game_exe.is_file() {
        return Err(format!("game exe missing for D3DMetal seed: {}", game_exe.display()));
    }
    if let Some(game_exe_dir) = game_exe.parent() {
        fs::write(game_exe_dir.join("steam_appid.txt"), state.appid.to_string())
            .map_err(|e| format!("write D3DMetal steam_appid.txt: {}", e))?;
    }

    let launch = json!({
        "appid": state.appid,
        "name": state.name,
        "game_dir": state.game_dir,
        "game_exe": game_exe.to_string_lossy(),
        "prefix": gptk_prefix().to_string_lossy(),
        "overrides": GPTK_OVERRIDES,
        "d3dmetal_framework_path": d3dmetal_framework_path().to_string_lossy(),
        "route_dll_source": homebrew_gptk_route_dll_dir().to_string_lossy(),
        "route_dll_destination": gptk_prefix().join("drive_c").join("windows").join("system32").to_string_lossy(),
        "steam_identity_env": ["SteamAppId", "SteamGameId", "SteamOverlayGameId"],
        "seeded_at": now_secs()
    });
    fs::write(seed_root.join("launch.json"), serde_json::to_string_pretty(&launch).unwrap_or_default())
        .map_err(|e| format!("write D3DMetal launch seed: {}", e))?;

    let game_config_dst = seed_root.join("game-config");
    copy_probable_game_configs(Path::new(&state.game_dir), &game_config_dst)?;
    Ok(())
}

fn verify_seed(state: &D3DMetalGptkState) -> Result<(), String> {
    ensure_homebrew_gptk_payload_ready()?;
    verify_prefix_route_dlls()?;
    verify_game_local_d3dmetal_route_dlls(state)?;
    let prefix = gptk_prefix();
    for rel in ["drive_c", "system.reg", "user.reg", "dosdevices"] {
        if !prefix.join(rel).exists() {
            return Err(format!("GPTK prefix missing {}", rel));
        }
    }
    let seed_root = prefix.join("drive_c").join("metalsharp").join("d3dmetal").join(state.appid.to_string());
    if !seed_root.join("launch.json").is_file() {
        return Err("D3DMetal seed launch.json missing".to_string());
    }
    if !state.game_exe.as_ref().map(|p| Path::new(p).is_file()).unwrap_or(false) {
        return Err("D3DMetal seed game exe missing".to_string());
    }
    if !seed_root.join("steam-users").is_dir() && !seed_root.join("steam-config").is_dir() {
        return Err("D3DMetal seed missing Steam user/config material".to_string());
    }
    Ok(())
}

fn copy_probable_game_configs(game_dir: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("create {}: {}", dst.display(), e))?;
    let mut copied = 0usize;
    for entry in WalkDir::new(game_dir).max_depth(3).into_iter().flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
        let is_config = matches!(
            path.extension().and_then(|e| e.to_str()).map(|s| s.to_ascii_lowercase()).as_deref(),
            Some("ini" | "cfg" | "json" | "xml" | "toml")
        ) || name.contains("config")
            || name.contains("settings");
        if !is_config {
            continue;
        }
        let rel = path.strip_prefix(game_dir).unwrap_or(path);
        let target = dst.join(rel);
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
        }
        fs::copy(path, &target)
            .map_err(|e| format!("copy config {} -> {}: {}", path.display(), target.display(), e))?;
        copied += 1;
        if copied >= 256 {
            break;
        }
    }
    fs::write(dst.join(".metalsharp-config-seed"), format!("copied={}\n", copied))
        .map_err(|e| format!("write config seed marker: {}", e))
}

fn find_game_exe(game_dir: &Path) -> Option<String> {
    let mut candidates: Vec<PathBuf> = WalkDir::new(game_dir)
        .max_depth(5)
        .into_iter()
        .flatten()
        .map(|entry| entry.into_path())
        .filter(|path| path.is_file())
        .filter(|path| {
            path.extension().and_then(|e| e.to_str()).map(|e| e.eq_ignore_ascii_case("exe")).unwrap_or(false)
        })
        .filter(|path| {
            let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("").to_ascii_lowercase();
            !name.contains("redist")
                && !name.contains("setup")
                && !name.contains("installer")
                && !name.contains("crash")
        })
        .collect();
    candidates.sort_by_key(|path| {
        let text = path.to_string_lossy().to_ascii_lowercase();
        (if text.contains("shipping") { 0 } else { 1 }, if text.contains("binaries/win64") { 0 } else { 1 }, text.len())
    });
    candidates.first().map(|p| p.to_string_lossy().to_string())
}

fn copy_file_checked(src: &Path, dst: &Path) -> Result<(), String> {
    if !file_nonempty(src) {
        return Err(format!("source payload missing: {}", src.display()));
    }
    if let Some(parent) = dst.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
    }
    fs::copy(src, dst).map_err(|e| format!("copy {} -> {}: {}", src.display(), dst.display(), e))?;
    if same_file_hash(src, dst) {
        Ok(())
    } else {
        Err(format!("copy verification failed: {} -> {}", src.display(), dst.display()))
    }
}

fn copy_dir_replace(src: &Path, dst: &Path) -> Result<(), String> {
    if !src.is_dir() {
        return Err(format!("source directory missing: {}", src.display()));
    }
    let tmp = dst.with_extension(format!("metalsharp-tmp-{}", now_secs()));
    let _ = fs::remove_dir_all(&tmp);
    copy_dir_recursive(src, &tmp)?;
    let _ = fs::remove_dir_all(dst);
    fs::rename(&tmp, dst).map_err(|e| format!("replace {} with {}: {}", dst.display(), tmp.display(), e))
}

fn copy_dir_recursive(src: &Path, dst: &Path) -> Result<(), String> {
    fs::create_dir_all(dst).map_err(|e| format!("create {}: {}", dst.display(), e))?;
    for entry in WalkDir::new(src).follow_links(false).into_iter().flatten() {
        let path = entry.path();
        let rel = path.strip_prefix(src).map_err(|e| format!("strip prefix {}: {}", path.display(), e))?;
        if rel.as_os_str().is_empty() {
            continue;
        }
        let target = dst.join(rel);
        if entry.file_type().is_dir() {
            fs::create_dir_all(&target).map_err(|e| format!("create {}: {}", target.display(), e))?;
        } else if entry.file_type().is_file() {
            if let Some(parent) = target.parent() {
                fs::create_dir_all(parent).map_err(|e| format!("create {}: {}", parent.display(), e))?;
            }
            fs::copy(path, &target).map_err(|e| format!("copy {} -> {}: {}", path.display(), target.display(), e))?;
        } else if entry.file_type().is_symlink() {
            let link = fs::read_link(path).map_err(|e| format!("readlink {}: {}", path.display(), e))?;
            #[cfg(unix)]
            std::os::unix::fs::symlink(link, &target).map_err(|e| format!("symlink {}: {}", target.display(), e))?;
        }
    }
    Ok(())
}

fn framework_ready(framework: &Path) -> bool {
    file_nonempty(&framework.join("Versions").join("A").join("D3DMetal"))
        && [framework.join("Resources"), framework.join("Versions").join("A").join("Resources")].iter().any(|dir| {
            fs::read_dir(dir).ok().into_iter().flatten().flatten().any(|entry| {
                entry.path().extension().and_then(|e| e.to_str()) == Some("dylib") && file_nonempty(&entry.path())
            })
        })
}

fn same_file_hash(a: &Path, b: &Path) -> bool {
    file_nonempty(a) && file_nonempty(b) && file_sha256(a).ok() == file_sha256(b).ok()
}

fn file_sha256(path: &Path) -> Result<String, String> {
    let mut file = fs::File::open(path).map_err(|e| format!("open {}: {}", path.display(), e))?;
    let mut hasher = Sha256::new();
    let mut buf = [0u8; 64 * 1024];
    loop {
        let n = file.read(&mut buf).map_err(|e| format!("read {}: {}", path.display(), e))?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
    }
    Ok(bytes_to_hex(&hasher.finalize()))
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut out, "{byte:02x}");
    }
    out
}

fn file_nonempty(path: &Path) -> bool {
    path.metadata().map(|m| m.is_file() && m.len() > 0).unwrap_or(false)
}

fn homebrew_gptk_installed() -> bool {
    homebrew_wine64().is_file() && homebrew_wineserver().is_file()
}

fn homebrew_app_root() -> PathBuf {
    PathBuf::from("/Applications/Game Porting Toolkit.app")
}

fn homebrew_wine_root() -> PathBuf {
    homebrew_app_root().join("Contents").join("Resources").join("wine")
}

fn homebrew_wine64() -> PathBuf {
    homebrew_wine_root().join("bin").join("wine64")
}

fn homebrew_gptk_route_dll_dir() -> PathBuf {
    homebrew_wine_root().join("lib").join("wine").join("x86_64-windows")
}

fn d3dmetal_framework_path() -> PathBuf {
    homebrew_wine_root().join("lib").join("external").join("D3DMetal.framework").join("D3DMetal")
}

fn homebrew_wineserver() -> PathBuf {
    homebrew_wine_root().join("bin").join("wineserver")
}

fn gptk_prefix() -> PathBuf {
    metalsharp_home().join("prefix-gptk")
}

fn gptk_dyld_path() -> String {
    let root = homebrew_wine_root();
    [
        root.join("lib"),
        root.join("lib").join("wine").join("x86_64-unix"),
        root.join("lib").join("wine").join("x86_32on64-unix"),
        root.join("lib").join("external"),
        homebrew_app_root().join("Contents").join("Resources").join("lib"),
    ]
    .into_iter()
    .filter(|path| path.is_dir())
    .map(|path| path.to_string_lossy().to_string())
    .collect::<Vec<_>>()
    .join(":")
}

fn find_brew() -> Result<PathBuf, String> {
    for path in [PathBuf::from("/opt/homebrew/bin/brew"), PathBuf::from("/usr/local/bin/brew")] {
        if path.is_file() {
            return Ok(path);
        }
    }
    let output = Command::new("/usr/bin/which").arg("brew").output().map_err(|e| format!("which brew: {}", e))?;
    if output.status.success() {
        let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
        if !path.is_empty() {
            return Ok(PathBuf::from(path));
        }
    }
    Err("Homebrew not found".to_string())
}

fn command_text(output: &std::process::Output) -> String {
    format!("{}{}", String::from_utf8_lossy(&output.stdout), String::from_utf8_lossy(&output.stderr))
}

fn expand_tilde(path: &str) -> PathBuf {
    if path == "~" {
        return dirs::home_dir().unwrap_or_else(|| PathBuf::from(path));
    }
    if let Some(rest) = path.strip_prefix("~/") {
        if let Some(home) = dirs::home_dir() {
            return home.join(rest);
        }
    }
    PathBuf::from(path)
}

fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn action_labels_flip_for_repairs() {
        let mut state = new_state("steam_1", 1, "Game", Path::new("/tmp/game"));
        state.gptk_homebrew = D3DMetalStepState::Installed;
        state.gptk_payload = D3DMetalStepState::Updated;
        state.x64_redist = D3DMetalStepState::RepairRequired;
        state.seed = D3DMetalStepState::RepairRequired;
        let actions = actions_for(&state);
        assert!(actions.iter().any(|a| a.id == "install_homebrew_gptk" && a.label == "Repair Homebrew GPTK"));
        assert!(actions.iter().any(|a| a.id == "install_rosetta" && a.label == "Repair Rosetta"));
        assert!(actions.iter().any(|a| a.id == "repair_gptk_payload" && a.label == "Repair GPTK Payload"));
        assert!(actions.iter().any(|a| a.id == "install_x64_redist" && a.label == "Repair Redist"));
        assert!(actions.iter().any(|a| a.id == "seed_prefix" && a.label == "Repair Seed"));
    }

    #[test]
    fn fresh_action_labels_are_honest() {
        let mut state = new_state("steam_1", 1, "Game", Path::new("/tmp/game"));
        state.gptk_homebrew = D3DMetalStepState::Installed;
        state.gptk_payload = D3DMetalStepState::Updated;
        state.rosetta = D3DMetalStepState::Installed;
        let actions = actions_for(&state);
        assert!(actions.iter().any(|a| a.id == "install_homebrew_gptk" && !a.enabled));
        assert!(actions.iter().any(|a| a.id == "install_rosetta" && !a.enabled));
        assert!(actions.iter().any(|a| a.id == "repair_gptk_payload" && !a.enabled));
        assert!(actions.iter().any(|a| a.id == "install_x64_redist" && a.label == "Repair Redist"));
        assert!(actions.iter().any(|a| a.id == "seed_prefix" && a.label == "Seed Prefix"));
    }

    #[test]
    fn manual_repair_actions_enable_for_missing_d3dmetal_prerequisites() {
        let mut state = new_state("steam_1", 1, "Game", Path::new("/tmp/game"));
        state.gptk_homebrew = D3DMetalStepState::Missing;
        state.gptk_payload = D3DMetalStepState::Missing;
        state.rosetta = D3DMetalStepState::Missing;
        let actions = actions_for(&state);
        assert!(actions.iter().any(|a| a.id == "install_homebrew_gptk" && a.enabled));
        assert!(actions.iter().any(|a| a.id == "install_rosetta" && a.enabled));

        state.gptk_homebrew = D3DMetalStepState::Installed;
        state.gptk_payload = D3DMetalStepState::RepairRequired;
        let actions = actions_for(&state);
        assert!(actions.iter().any(|a| a.id == "repair_gptk_payload" && a.enabled));
    }

    #[test]
    fn d3dmetal_appids_reject_zero_and_overflow() {
        let zero = serde_json::json!({"appid": 0}).as_object().unwrap().clone();
        assert!(request_appid(&zero).is_err());
        let overflow = serde_json::json!({"appid": (u32::MAX as u64) + 1}).as_object().unwrap().clone();
        assert!(request_appid(&overflow).is_err());
        let valid = serde_json::json!({"appid": 1962700}).as_object().unwrap().clone();
        assert_eq!(request_appid(&valid).unwrap(), 1962700);
    }

    #[test]
    fn d3dmetal_bottle_ids_reject_path_traversal() {
        assert!(validate_d3dmetal_bottle_id("steam_1962700").is_ok());
        assert!(validate_d3dmetal_bottle_id("../../escape").is_err());
        assert!(validate_d3dmetal_bottle_id("/tmp/escape").is_err());
    }

    #[test]
    fn vcredist_registry_check_is_scoped_to_x64_runtime_key() {
        let unrelated_installed = r#"
[Software\\Other]
"Installed"=dword:00000001
[Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64]
"Version"="14.44.35211.0"
"#;
        assert!(!vcredist_registry_installed(unrelated_installed, "x64"));

        let x64_installed = r#"
[Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64] 1782809006
"Version"="14.44.35211.0"
"Installed"=dword:00000001
"#;
        assert!(vcredist_registry_installed(x64_installed, "x64"));
    }

    #[test]
    fn detects_app_local_d3dmetal_route_conflicts() {
        assert!(is_d3dmetal_route_conflict(Path::new("dxgi.dll")));
        assert!(is_d3dmetal_route_conflict(Path::new("d3d12.dll")));
        assert!(is_d3dmetal_route_conflict(Path::new("d3d10core.dll")));
        assert!(!is_d3dmetal_route_conflict(Path::new("d3dcompiler_47.dll")));
        assert!(!is_d3dmetal_route_conflict(Path::new("d3dx9_43.dll")));
        assert!(is_d3dmetal_route_conflict(Path::new("nvapi64.dll")));
        assert!(is_d3dmetal_route_conflict(Path::new("nvngx.dll")));
        assert!(is_d3dmetal_route_conflict(Path::new("winemetal.dll")));
        assert!(!is_d3dmetal_route_conflict(Path::new("sl.interposer.dll")));
        assert!(!is_d3dmetal_route_conflict(Path::new("steam_api64.dll")));
    }
}
