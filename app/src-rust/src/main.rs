#![allow(
    clippy::ptr_arg,
    clippy::unnecessary_unwrap,
    clippy::useless_vec,
    clippy::manual_div_ceil,
    clippy::redundant_closure,
    clippy::bool_assert_comparison,
    clippy::needless_bool,
    clippy::manual_strip,
    clippy::let_unit_value,
    clippy::char_lit_as_u8,
    clippy::type_complexity,
    clippy::single_match,
    clippy::match_single_binding,
    clippy::redundant_pattern_matching,
    dead_code,
    unused_variables
)]

mod anticheat;
mod backend_auth;
mod binding_contract;
mod bottles;
mod command_contract;
mod d3d12_runtime_doctor;
mod d3dmetal_gptk;
mod diagnostics;
mod fna_profile;
mod gog;
mod installer;
mod kernel_translation;
mod launch;
mod launcher_evidence;
mod metalfx;
mod migrate;
mod mono;
mod mono_profile;
mod mtsp;
mod platform;
mod scan;
mod setup;
mod sharp_library;
mod steam;
mod updater;

use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::Read;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use tiny_http::{Header, Method, Response, Server};

static RUNNING_GAMES: OnceLock<Mutex<HashMap<u32, i32>>> = OnceLock::new();
static RUNNING_SHARP_APPS: OnceLock<Mutex<HashMap<String, i32>>> = OnceLock::new();
static ISSUE_LOG_COUNTER: AtomicU64 = AtomicU64::new(0);

const API_TOKEN_ENV: &str = "METALSHARP_API_TOKEN";
const TRUSTED_DEV_ORIGINS: [&str; 2] = ["http://localhost:5173", "http://127.0.0.1:5173"];

fn running_games() -> &'static Mutex<HashMap<u32, i32>> {
    RUNNING_GAMES.get_or_init(|| Mutex::new(HashMap::new()))
}

fn running_sharp_apps() -> &'static Mutex<HashMap<String, i32>> {
    RUNNING_SHARP_APPS.get_or_init(|| Mutex::new(HashMap::new()))
}

pub(crate) fn register_game_pid(appid: u32, pid: u32) {
    if let Ok(mut map) = running_games().lock() {
        map.insert(appid, pid as i32);
    }
}

fn unregister_game_pid(appid: u32) {
    if let Ok(mut map) = running_games().lock() {
        map.remove(&appid);
    }
}

fn registered_game_pid(games: &HashMap<u32, i32>, appid: u32) -> Option<i32> {
    games.get(&appid).copied().filter(|pid| *pid > 0)
}

fn registered_game_appid_for_pid(games: &HashMap<u32, i32>, pid: i32) -> Option<u32> {
    games.iter().find_map(|(&appid, &registered_pid)| (registered_pid == pid).then_some(appid))
}

fn get_game_pid(appid: u32) -> Option<i32> {
    running_games().lock().ok().and_then(|games| registered_game_pid(&games, appid))
}

fn get_game_appid_for_pid(pid: i32) -> Option<u32> {
    running_games().lock().ok().and_then(|games| registered_game_appid_for_pid(&games, pid))
}

fn register_sharp_pid(app_id: &str, pid: u32) {
    if pid == 0 || app_id.is_empty() {
        return;
    }
    if let Ok(mut map) = running_sharp_apps().lock() {
        map.insert(app_id.to_string(), pid as i32);
    }
}

fn unregister_sharp_pid(app_id: &str) {
    if let Ok(mut map) = running_sharp_apps().lock() {
        map.remove(app_id);
    }
}

fn get_sharp_pid(app_id: &str) -> Option<i32> {
    running_sharp_apps().lock().ok()?.get(app_id).copied().filter(|pid| *pid > 0)
}

fn prune_inactive_game_pids() {
    if let Ok(mut map) = running_games().lock() {
        map.retain(|_, &mut pid| launch::is_process_active(pid));
    }
}

/// Return only live, MetalSharp-registered game process roots. This is the
/// trust boundary for global game-stop actions: arbitrary system PIDs must never
/// be accepted here.
fn active_game_targets(games: &HashMap<u32, i32>) -> Vec<(u32, i32)> {
    let mut targets: Vec<_> = games.iter().filter_map(|(&appid, &pid)| (pid > 0).then_some((appid, pid))).collect();
    targets.sort_unstable_by_key(|(appid, _)| *appid);
    targets
}

fn stop_active_games() -> Value {
    prune_inactive_game_pids();
    let targets = running_games().lock().map(|games| active_game_targets(&games)).unwrap_or_default();
    let mut stopped = Vec::new();
    let mut errors = Vec::new();

    for (appid, pid) in targets {
        if !is_metalsharp_owned_process(pid) {
            app_log(&format!(
                "[STOP FAILED] appid {} | pid {} | source global-shortcut | process ownership check failed",
                appid, pid
            ));
            errors.push(json!({"appid": appid, "pid": pid}));
            continue;
        }
        match launch::kill_game_with_pid(appid, pid) {
            Ok(_) => {
                unregister_game_pid(appid);
                app_log(&format!("[STOPPED] appid {} | pid {} | source global-shortcut", appid, pid));
                stopped.push(json!({ "appid": appid, "pid": pid }));
            },
            Err(error) => {
                app_log(&format!(
                    "[STOP FAILED] appid {} | pid {} | source global-shortcut | error: {}",
                    appid, pid, error
                ));
                errors.push(json!({ "appid": appid, "pid": pid }));
            },
        }
    }

    json!({
        "ok": errors.is_empty(),
        "active": !stopped.is_empty() || !errors.is_empty(),
        "stopped": stopped,
        "errors": errors,
    })
}

fn stop_registered_sharp_app(app_id: &str) -> Result<i32, String> {
    let pid = get_sharp_pid(app_id).ok_or_else(|| "Sharp app is not registered as running".to_string())?;
    if !is_metalsharp_owned_process(pid) {
        unregister_sharp_pid(app_id);
        return Err("registered Sharp app process is no longer a MetalSharp-owned target".to_string());
    }

    launch::kill_process_tree(pid).map_err(|error| error.to_string())?;
    unregister_sharp_pid(app_id);
    Ok(pid)
}

enum RouteResponse {
    Json(u16, Vec<u8>),
    Raw(u16, Vec<u8>, String),
}

/// Maximum JSON request body accepted by routes that consume a body.
///
/// The backend is a local HTTP service, so a renderer or another local caller
/// can connect directly. Keep the bound explicit and enforce it before JSON
/// parsing so a caller cannot make the backend allocate an unbounded buffer.
const MAX_REQUEST_BODY_BYTES: usize = 16 * 1024 * 1024;

#[derive(Debug)]
enum RequestBodyError {
    TooLarge,
    Read(std::io::Error),
    InvalidJson(serde_json::Error),
}

fn main() {
    let port = std::env::var("METALSHARP_PORT").unwrap_or_else(|_| "9274".into());
    let addr = format!("127.0.0.1:{}", port);
    let backend_auth = match backend_auth::BackendAuth::create(&crate::platform::metalsharp_home_dir()) {
        Ok(auth) => Arc::new(auth),
        Err(error) => {
            eprintln!("failed to initialize backend authentication: {error}");
            std::process::exit(1);
        },
    };
    let server = Arc::new({
        let mut attempts = 0u32;
        let max_attempts = 30u32;
        loop {
            match Server::http(&addr) {
                Ok(s) => break s,
                Err(e) => {
                    attempts += 1;
                    if attempts >= max_attempts {
                        eprintln!("failed to bind {} after {} attempts: {}", addr, max_attempts, e);
                        std::process::exit(1);
                    }
                    eprintln!("bind {} attempt {}/{} failed: {} — retrying in 500ms", addr, attempts, max_attempts, e);
                    std::thread::sleep(std::time::Duration::from_millis(500));
                },
            }
        }
    });

    ctrlc::set_handler(move || {
        app_log("Shutting down — cleaning up running games");
        if let Ok(map) = running_games().lock() {
            for (&appid, &pid) in map.iter() {
                app_log(&format!("Killing game appid={} pid={}", appid, pid));
                if is_metalsharp_owned_process(pid) {
                    let _ = launch::kill_process_tree(pid);
                } else {
                    app_log(&format!("Skipping unowned game pid={} during shutdown", pid));
                }
            }
        }
        std::process::exit(0);
    })
    .unwrap_or_else(|e| eprintln!("ctrlc handler warning: {}", e));

    // Log panics from ANY thread into the app log so a crash is diagnosable
    // even though stderr is invisible from a Finder-launched app.
    std::panic::set_hook(Box::new(|info| {
        eprintln!("metalsharp-backend panic: {}", info);
        let msg = info.to_string();
        let _ = std::fs::create_dir_all(logs_dir());
        let log_path = logs_dir().join(format!("{}.log", chrono_date()));
        let line = format!("[{}] backend panic: {}\n", chrono_now(), msg);
        let _ = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&log_path)
            .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
    }));

    eprintln!("metalsharp-backend listening on {}", addr);
    app_log(&format!("MetalSharp v{} backend started on {}", env!("CARGO_PKG_VERSION"), addr));

    if crate::steam::is_wine_steam_running() {
        let _ = kernel_translation::ipc_bridge::start_ipc_listener();
    }

    let json_header = Header::from_bytes(&b"Content-Type"[..], &b"application/json"[..]).unwrap();
    let api_token = configured_api_token();
    if api_token.is_none() {
        eprintln!("{API_TOKEN_ENV} is missing or invalid; refusing backend requests");
    }

    loop {
        let mut request = match server.recv() {
            Ok(r) => r,
            Err(_) => break,
        };

        // Handlers run synchronously on the main thread (tiny_http), so a
        // panic in ANY handler previously terminated the whole backend.
        // Catch panics per-request: a single bad request must never take the
        // process down (the app has no live-session supervisor).
        let outcome =
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Option<Response<std::io::Cursor<Vec<u8>>>> {
                let is_public_health_check = is_public_health_request(request.method(), request.url());
                // Accept the current-process bearer token for compatibility with
                // existing clients, while requiring the persisted token for the
                // new updater and migration clients.
                if !is_public_health_check
                    && !is_authorized_request(&request, api_token.as_deref())
                    && !backend_auth.is_authorized(&request)
                {
                    return Some(
                        Response::from_data(br#"{"ok":false,"error":"unauthorized"}"#.to_vec())
                            .with_header(json_header.clone())
                            .with_status_code(401),
                    );
                }

                // Local-API origin guard (H9): Electron reaches the backend
                // through the main-process bridge and sends no Origin header.
                // If a browser client is used during development, only the
                // fixed Vite origins are accepted. `null`, `file://`, arbitrary
                // localhost ports, and attacker-controlled origins are rejected.
                let origin = request
                    .headers()
                    .iter()
                    .find(|h| h.field.equiv("Origin"))
                    .map(|h| h.value.as_str().trim().to_string())
                    .filter(|o| !o.is_empty());
                if let Some(origin) = origin.as_deref() {
                    if !is_trusted_local_origin(origin) {
                        eprintln!("rejected cross-origin request from {origin}");
                        return Some(
                            Response::from_data(br#"{"ok":false,"error":"cross-origin request rejected"}"#.to_vec())
                                .with_header(json_header.clone())
                                .with_status_code(403),
                        );
                    }
                }

                Some(response_for_route(route(&mut request), &json_header, origin.as_deref()))
            }));
        match outcome {
            Ok(Some(resp)) => {
                let _ = request.respond(resp);
            },
            Ok(None) => {},
            Err(payload) => {
                let msg = panic_payload_message(&payload);
                eprintln!("metalsharp-backend: request handler panicked: {}", msg);
                app_log(&format!("request handler panicked: {}", msg));
                let safe = msg.replace('"', "'").replace('\\', "/");
                let body = format!(r#"{{"ok":false,"error":"internal handler panic: {}"}}"#, safe);
                let resp =
                    Response::from_data(body.into_bytes()).with_header(json_header.clone()).with_status_code(500);
                let _ = request.respond(resp);
            },
        }
    }
}

fn panic_payload_message(payload: &Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        s.to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "unknown panic payload".to_string()
    }
}

fn configured_api_token() -> Option<String> {
    let token = std::env::var(API_TOKEN_ENV).ok()?;
    if token.len() < 32 || !token.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    Some(token)
}

fn request_authorization(request: &tiny_http::Request) -> Option<&str> {
    request
        .headers()
        .iter()
        .find(|header| header.field.equiv("Authorization"))
        .map(|header| header.value.as_str().trim())
}

fn constant_time_token_eq(expected: &str, provided: &str) -> bool {
    let expected_bytes = expected.as_bytes();
    let provided_bytes = provided.as_bytes();
    let max_len = expected_bytes.len().max(provided_bytes.len());
    let mut difference = expected_bytes.len() ^ provided_bytes.len();
    for index in 0..max_len {
        let left = expected_bytes.get(index).copied().unwrap_or(0);
        let right = provided_bytes.get(index).copied().unwrap_or(0);
        difference |= usize::from(left ^ right);
    }
    difference == 0
}

fn is_authorized_bearer(authorization: Option<&str>, expected_token: Option<&str>) -> bool {
    let Some(expected_token) = expected_token else {
        return false;
    };
    let Some(authorization) = authorization else {
        return false;
    };
    let Some(provided_token) = authorization.strip_prefix("Bearer ") else {
        return false;
    };
    constant_time_token_eq(expected_token, provided_token)
}

fn is_authorized_request(request: &tiny_http::Request, expected_token: Option<&str>) -> bool {
    is_authorized_bearer(request_authorization(request), expected_token)
}

fn is_public_health_request(method: &Method, url: &str) -> bool {
    method == &Method::Get && url.split('?').next().unwrap_or(url) == "/health"
}

fn response_with_trusted_origin(
    response: Response<std::io::Cursor<Vec<u8>>>,
    origin: Option<&str>,
) -> Response<std::io::Cursor<Vec<u8>>> {
    if let Some(origin) = origin.filter(|origin| is_trusted_local_origin(origin)) {
        if let Ok(header) = Header::from_bytes(&b"Access-Control-Allow-Origin"[..], origin.as_bytes()) {
            return response.with_header(header);
        }
    }
    response
}

fn response_for_route(
    route_response: RouteResponse,
    json_header: &Header,
    origin: Option<&str>,
) -> Response<std::io::Cursor<Vec<u8>>> {
    match route_response {
        RouteResponse::Json(code, body) => response_with_trusted_origin(
            Response::from_data(body).with_header(json_header.clone()).with_status_code(code),
            origin,
        ),
        RouteResponse::Raw(code, data, mime) => {
            let content_header =
                Header::from_bytes(&b"Content-Type"[..], mime.as_bytes()).unwrap_or_else(|_| json_header.clone());
            response_with_trusted_origin(
                Response::from_data(data).with_header(content_header).with_status_code(code),
                origin,
            )
        },
    }
}

/// True only for the fixed Vite development origins. Packaged Electron
/// requests use the main-process bridge and intentionally carry no Origin.
fn is_trusted_local_origin(origin: &str) -> bool {
    TRUSTED_DEV_ORIGINS.iter().any(|trusted| origin.eq_ignore_ascii_case(trusted))
}

fn request_body_error_response(error: RequestBodyError) -> RouteResponse {
    match error {
        RequestBodyError::TooLarge => resp(
            413,
            json!({
                "ok": false,
                "error": format!("request body exceeds {} byte limit", MAX_REQUEST_BODY_BYTES),
            }),
        ),
        RequestBodyError::Read(error) => resp(
            400,
            json!({
                "ok": false,
                "error": format!("unable to read request body: {}", error),
            }),
        ),
        RequestBodyError::InvalidJson(error) => resp(
            400,
            json!({
                "ok": false,
                "error": format!("invalid JSON request body: {}", error),
            }),
        ),
    }
}

macro_rules! read_body_or_return {
    ($request:expr) => {{
        match read_body($request) {
            Ok(body) => body,
            Err(error) => return request_body_error_response(error),
        }
    }};
}

fn route(req: &mut tiny_http::Request) -> RouteResponse {
    let method = req.method().clone();
    let url = req.url().to_string();
    let path = url.split('?').next().unwrap_or(&url);

    match (method, path) {
        (Method::Get, "/health") => resp(200, json!({"ok": true, "version": env!("CARGO_PKG_VERSION")})),
        (Method::Get, "/status") => {
            app_log("Backend status checked");
            resp(
                200,
                json!({
                    "ok": true,
                    "version": env!("CARGO_PKG_VERSION"),
                    "pid": std::process::id(),
                    "dev_mode": std::env::var("METALSHARP_DEV").map(|v| v == "1").unwrap_or(false),
                    "metalsharp_home": crate::platform::metalsharp_home_dir().to_string_lossy().to_string(),
                }),
            )
        },
        (Method::Get, "/runtime/host-abi") => resp(
            200,
            json!({
                "ok": true,
                "magic": "MSAB",
                "version": {"major": 1, "minor": 0},
                "services": [
                    "process",
                    "paths",
                    "logging",
                    "steam",
                    "graphics",
                    "audio",
                    "input",
                    "managed_runtime"
                ],
                "steam_bridge": {
                    "default_port": 18733,
                    "active_port": mtsp::launcher::bridge_port(),
                    "env": "METALSHARP_STEAM_BRIDGE_PORT"
                },
                "managed_runtime_env": [
                    "METALSHARP_MONO_LIB",
                    "METALSHARP_MONO_ROOT",
                    "METALSHARP_MONO_ASSEMBLY_DIR",
                    "METALSHARP_MONO_CONFIG_DIR"
                ]
            }),
        ),
        (Method::Get, "/update/check") => resp(200, updater::check_for_update()),
        (Method::Post, "/update/start") => match updater::start_update() {
            Ok(v) => resp(200, v),
            Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
        },
        (Method::Get, "/update/progress") => resp(200, updater::read_update_progress()),
        (Method::Get, "/update/dmg-path") => match updater::get_downloaded_dmg() {
            Some(download) => resp(
                200,
                json!({
                    "ok": true,
                    "path": download.path,
                    "version": download.version,
                    "size": download.size,
                    "sha256": download.sha256,
                }),
            ),
            None => resp(200, json!({"ok": false, "error": "no downloaded DMG"})),
        },
        (Method::Post, "/update/cleanup") => resp(200, updater::cleanup_downloaded_dmgs()),
        (Method::Get, "/update/migrate/check") => resp(200, migrate::needs_migration()),
        (Method::Post, "/update/migrate/start") => match migrate::start_migration() {
            Ok(v) => resp(200, v),
            Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
        },
        (Method::Get, "/update/migrate/progress") => resp(200, migrate::read_migrate_progress()),
        // Phase 2: report what the last migration preserved, skipped, and why.
        (Method::Get, "/update/migrate/report") => resp(200, migrate::latest_migration_report()),
        (Method::Post, "/update/migrate/cleanup-preserved") => resp(200, migrate::cleanup_preserved_temp_dirs()),
        (Method::Get, "/setup/state") => resp(200, setup::state()),
        (Method::Post, "/setup/save") => {
            let body = read_body_or_return!(req);
            match setup::save_step(&body) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Get, "/setup/device-name") => resp(
            200,
            json!({
                "ok": true,
                "name": setup::generate_device_name(),
            }),
        ),
        (Method::Get, "/setup/dependencies") => resp(200, setup::dependencies()),
        (Method::Get, "/setup/agility-versions") => resp(200, setup::agility_known_sdk_versions()),
        (Method::Post, "/setup/install-deps") => {
            let body = read_body_or_return!(req);
            match setup::install_dependencies(&body) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/setup/install-all") => match installer::start_install_all() {
            Ok(v) => resp(200, v),
            Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
        },
        (Method::Get, "/setup/install-progress") => resp(200, installer::read_progress()),
        (Method::Get, "/setup/installing") => resp(200, json!({"installing": installer::is_installing()})),
        (Method::Post, "/setup/install-vcpp-x64") => {
            let home = dirs::home_dir().unwrap_or_default();
            let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
            if !prefix.join("drive_c/windows/system32").exists() {
                resp(400, json!({"ok": false, "error": "Wine prefix not ready — install runtime and Steam first"}))
            } else {
                match bottles::vcpp_ensure_and_install_x64(&prefix) {
                    Ok(()) => resp(200, json!({"ok": true})),
                    Err(e) => resp(500, json!({"ok": false, "error": e})),
                }
            }
        },
        (Method::Post, "/setup/install-vcpp-x86") => {
            let home = dirs::home_dir().unwrap_or_default();
            let prefix = crate::platform::metalsharp_home_dir_for(&home).join("prefix-steam");
            if !prefix.join("drive_c/windows/system32").exists() {
                resp(400, json!({"ok": false, "error": "Wine prefix not ready — install runtime and Steam first"}))
            } else {
                match bottles::vcpp_ensure_and_install_x86(&prefix) {
                    Ok(()) => resp(200, json!({"ok": true})),
                    Err(e) => resp(500, json!({"ok": false, "error": e})),
                }
            }
        },
        (Method::Post, "/game/prepare") => {
            let body = read_body_or_return!(req);
            let id = match parse_request_appid(&body) {
                Ok(id) => id,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let requested_pipeline = body.get("pipeline").or_else(|| body.get("launchMethod")).and_then(|v| v.as_str());
            let requested_pipeline = match requested_pipeline {
                Some(value) => match mtsp::engine::PipelineId::from_str_flexible(value) {
                    Some(pipeline) => Some(pipeline),
                    None => return resp(400, json!({"ok": false, "error": format!("unknown pipeline: {}", value)})),
                },
                None => None,
            };
            let effective_pipeline = bottles::resolve_steam_pipeline_for_request(id, requested_pipeline);
            let mtsp_prepare_supported = !matches!(
                effective_pipeline,
                mtsp::engine::PipelineId::FnaArm64
                    | mtsp::engine::PipelineId::Steam
                    | mtsp::engine::PipelineId::MacSteam
            );
            if mtsp_prepare_supported {
                app_log(&format!(
                    "Preparing game runtime via MTSP: appid {}, requested={:?}, effective={:?}",
                    id, requested_pipeline, effective_pipeline
                ));
                match mtsp::launcher::prepare_pipeline_with_request(id, Some(effective_pipeline)) {
                    Ok(mut v) => {
                        if let Some(obj) = v.as_object_mut() {
                            obj.insert("deprecated_endpoint".into(), json!("/game/prepare"));
                            obj.insert("canonical_endpoint".into(), json!("/mtsp/prepare"));
                        }
                        resp(200, v)
                    },
                    Err(e) => {
                        resp(500, json!({"ok": false, "error": e.to_string(), "canonical_endpoint": "/mtsp/prepare"}))
                    },
                }
            } else {
                app_log(&format!("Preparing legacy game runtime: appid {}, effective={:?}", id, effective_pipeline));
                match setup::prepare_game(id) {
                    Ok(mut v) => {
                        if let Some(obj) = v.as_object_mut() {
                            obj.insert("legacy_prepare".into(), json!(true));
                            obj.insert("deprecated_endpoint".into(), json!("/game/prepare"));
                        }
                        resp(200, v)
                    },
                    Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
                }
            }
        },
        (Method::Post, "/game/resolve-routing") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let pipeline = bottles::resolve_steam_pipeline_for_request(appid, None);
            let node = mtsp::engine::get_pipeline(pipeline);
            let recipe = mtsp::rules::get_game_recipe(appid);
            let preferred_pipeline = bottles::preferred_pipeline_for_steam_app(appid);
            resp(
                200,
                json!({
                    "ok": true,
                    "appid": appid,
                    "pipeline": pipeline,
                    "pipeline_name": node.name,
                    "preferred_pipeline": preferred_pipeline.map(|p| p.to_legacy_method().to_string()),
                    "graphics_backend": node.graphics_backend,
                    "backend": node.backend,
                    "offline_capable": recipe.as_ref().map(|r| r.offline_capable).unwrap_or(false),
                    "recipe": recipe,
                }),
            )
        },
        (Method::Get, "/scan") => {
            let mut timing = diagnostics::LaunchTiming::start();
            app_log("Scanning for installed games...");
            timing.mark("scan_start");
            let result = scan::scan_all();
            timing.mark("scan_all_done");
            if let Some(home) = dirs::home_dir() {
                diagnostics::record_scan_timing(&home, "scan_all", &timing);
            }
            match result {
                Ok(result) => resp(200, result),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Get, "/steam/status") => resp(200, steam::status()),
        (Method::Get, "/steam/library") => {
            let mut timing = diagnostics::LaunchTiming::start();
            app_log("Loading Steam library...");
            timing.mark("library_load_start");
            let result = steam::library();
            timing.mark("library_load_done");
            if let Some(home) = dirs::home_dir() {
                diagnostics::record_scan_timing(&home, "steam_library", &timing);
            }
            app_log(&format!("Loaded {} games", result.get("total").and_then(|t| t.as_u64()).unwrap_or(0)));
            resp(200, result)
        },
        (Method::Get, "/steam/api-key") => resp(200, steam::get_api_key()),
        (Method::Post, "/steam/save-api-key") => {
            let body = read_body_or_return!(req);
            let key = body.get("key").and_then(|v| v.as_str()).unwrap_or("");
            app_log("Steam API key saved");
            match steam::save_api_key(key) {
                Ok(_) => {
                    let library = steam::library();
                    let total = library.get("total").and_then(|t| t.as_u64()).unwrap_or(0);
                    app_log(&format!("Steam API key sync loaded {} games", total));
                    resp(
                        200,
                        json!({
                            "ok": true,
                            "library": library,
                            "sync": steam::api_key_sync_state(),
                        }),
                    )
                },
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/steam/install") => match steam::install_steam() {
            Ok(p) => resp(200, json!({"ok": true, "path": p})),
            Err(e) => {
                app_issue_log("steam-install", "wine-steam", &e.to_string(), &[]);
                resp(500, json!({"ok": false, "error": e.to_string()}))
            },
        },
        (Method::Post, "/steam/launch") => {
            app_log("Launching Wine Steam...");
            match steam::launch_wine_steam() {
                Ok(v) => resp(200, v),
                Err(e) => {
                    app_issue_log("steam-launch", "wine-steam", &e.to_string(), &[]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/steam/stop") => {
            app_log("Stopping Wine Steam...");
            match steam::stop_wine_steam() {
                Ok(v) => resp(200, v),
                Err(e) => {
                    app_issue_log("steam-stop", "wine-steam", &e.to_string(), &[]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/steam/mac-launch") => {
            app_log("Launching macOS Steam...");
            match steam::launch_macos_steam() {
                Ok(v) => resp(200, v),
                Err(e) => {
                    app_issue_log("steam-launch", "macos-steam", &e.to_string(), &[]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/steam/mac-install") => {
            app_log("Opening macOS Steam installer...");
            match steam::install_macos_steam() {
                Ok(v) => resp(200, v),
                Err(e) => {
                    app_issue_log("steam-install", "macos-steam", &e.to_string(), &[]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/steam/mac-stop") => {
            app_log("Stopping macOS Steam...");
            match steam::stop_macos_steam() {
                Ok(v) => resp(200, v),
                Err(e) => {
                    app_issue_log("steam-stop", "macos-steam", &e.to_string(), &[]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/steam/mac-launch-game") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            app_log(&format!("Launching game via macOS Steam: appid {}", appid));
            match steam::launch_macos_steam_game(appid) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Get, "/steam/is-running") => resp(200, json!({"ok": true, "running": steam::is_wine_steam_running()})),
        (Method::Get, "/steam/bridge-status") => {
            let running = mtsp::launcher::bridge_is_running();
            resp(200, json!({"ok": true, "running": running, "port": mtsp::launcher::bridge_port()}))
        },
        (Method::Post, "/steam/bridge-start") => match mtsp::launcher::ensure_bridge_running() {
            Ok(_) => resp(200, json!({"ok": true, "port": mtsp::launcher::bridge_port()})),
            Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
        },
        (Method::Get, "/steam/watch-steamapps") => {
            let new_ids = steam::watch_steamapps();
            resp(200, json!({"ok": true, "new_appids": new_ids}))
        },
        (Method::Post, "/steam/install-game") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            app_log(&format!("Installing game via Wine Steam: appid {}", appid));
            match steam::install_game_via_steam(appid) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/steam/launch-game") => {
            let body = read_body_or_return!(req);
            match parse_request_appid(&body) {
                Ok(id) => {
                    let launch_method = body
                        .get("launchMethod")
                        .or_else(|| body.get("pipeline"))
                        .and_then(|v| v.as_str())
                        .unwrap_or("steam");
                    let requested_route_pipeline = match mtsp::engine::PipelineId::from_str_flexible(launch_method) {
                        Some(mtsp::engine::PipelineId::Steam) => None,
                        Some(pipeline) => Some(bottles::resolve_steam_pipeline_for_request(id, Some(pipeline))),
                        None if launch_method.eq_ignore_ascii_case("steam") => None,
                        None => Some(bottles::resolve_steam_pipeline_for_request(id, None)),
                    };
                    let eac_enabled = anticheat::eac_enabled(id);
                    let route_pipeline = match requested_route_pipeline {
                        Some(pipeline) => Some(anticheat::eac_pipeline_for_request(id, pipeline)),
                        None if eac_enabled => Some(anticheat::eac_pipeline_for_request(
                            id,
                            bottles::resolve_steam_pipeline_for_request(id, None),
                        )),
                        None => None,
                    };
                    app_log(&format!(
                        "Launching game via Wine Steam: appid {}, route {}, eac_substrate={}",
                        id, launch_method, eac_enabled
                    ));
                    let launch_result = match route_pipeline {
                        Some(pipeline) => {
                            let bottle = match bottles::prepare_steam_game_launch(id, pipeline) {
                                Ok(bottle) => bottle,
                                Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                            };
                            let (mut env, launch_recipe) =
                                match mtsp::launcher::prepare_steam_pipeline_env(id, pipeline) {
                                    Ok(prepared) => prepared,
                                    Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                                };
                            let offline_direct = bottles::steam_pipeline_defaults_offline(pipeline);
                            if offline_direct {
                                let Some(game_dir) = launch_recipe.game_dir.as_ref() else {
                                    return resp(404, json!({"ok": false, "error": "Game directory not found"}));
                                };
                                if let Some(home) = dirs::home_dir() {
                                    crate::mtsp::launcher::deploy_goldberg_for_pipeline(&home, game_dir, id, pipeline);
                                }
                                env.push(("SteamAppId".to_string(), id.to_string()));
                                env.push(("SteamGameId".to_string(), id.to_string()));
                                env.push(("METALSHARP_OFFLINE_MODE".to_string(), "1".to_string()));
                            }
                            let is_gptk_direct = matches!(pipeline, mtsp::engine::PipelineId::D3DMetal);
                            // All MTSP routes launch the game directly (never
                            // steam://run). The game process gets the route env
                            // applied to it and talks to the real Steam client
                            // running in the background; the real Steam user
                            // files (steamclient64.dll, steam_api64,
                            // GameOverlayRenderer*) are deployed into the game
                            // folder by prepare_steam_pipeline_env for the
                            // routes that use the real Steam model (M12 always).
                            let steam_started = if is_gptk_direct {
                                false
                            } else {
                                match steam::ensure_wine_steam_ready_for_game_launch() {
                                    Ok(started) => started,
                                    Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                                }
                            };
                            let bottle_prefix = std::path::PathBuf::from(&bottle.prefix_path);
                            mtsp::launcher::launch_steam_bottle_with_pipeline(id, pipeline, &bottle_prefix, &env).map(
                                |(pid, game_type, log_path)| {
                                    register_game_pid(id, pid);
                                    let compatdata = bottles::set_launch_started(&bottle.id, pid, &log_path)
                                        .ok()
                                        .and_then(|manifest| bottles::save_steam_compatdata(&manifest, pipeline).ok());
                                    json!({
                                        "ok": true,
                                        "pid": pid,
                                        "appid": id,
                                        "gameType": game_type,
                                        "bottle_id": bottle.id,
                                        "bottle_prefix": bottle.prefix_path,
                                        "launch_log": log_path.to_string_lossy().to_string(),
                                        "compatdata": compatdata,
                                        "pipeline": pipeline,
                                        "recipe": launch_recipe,
                                        "steam_started": steam_started,
                                        "steam_runtime": if offline_direct { "offline" } else { "background" },
                                        "offline_mode": offline_direct,
                                        "eac_substrate": eac_enabled,
                                        "env_applied_to": "game_process",
                                        "env_handoff": env.iter().map(|(k, _)| k).collect::<Vec<_>>(),
                                    })
                                },
                            )
                        },
                        None => {
                            let pipeline = bottles::resolve_steam_pipeline_for_request(id, None);
                            let bottle = match bottles::prepare_steam_game_launch(id, pipeline) {
                                Ok(bottle) => bottle,
                                Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                            };
                            let compatdata = bottles::save_steam_compatdata(&bottle, pipeline).ok();
                            steam::launch_game_via_steam(id).map(|mut v| {
                                if let Some(obj) = v.as_object_mut() {
                                    obj.insert("bottle_id".into(), json!(bottle.id));
                                    obj.insert("bottle_prefix".into(), json!(bottle.prefix_path));
                                    obj.insert("compatdata".into(), json!(compatdata));
                                }
                                v
                            })
                        },
                    };
                    match launch_result {
                        Ok(v) => {
                            if let Some(pid) =
                                v.get("pid").and_then(|value| value.as_u64()).and_then(|pid| u32::try_from(pid).ok())
                            {
                                register_game_pid(id, pid);
                            }
                            resp(200, v)
                        },
                        Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
                    }
                },
                Err(error) => resp(400, json!({"ok": false, "error": error})),
            }
        },
        (Method::Post, "/steam/launch-offline") => {
            let body = read_body_or_return!(req);
            match parse_request_appid(&body) {
                Ok(id) => {
                    let recipe = mtsp::rules::get_game_recipe(id);
                    let pipeline = bottles::resolve_steam_pipeline_for_request(id, None);
                    let node = mtsp::engine::get_pipeline(pipeline);

                    if !bottles::steam_pipeline_defaults_offline(pipeline)
                        && !recipe.as_ref().map(|r| r.offline_capable).unwrap_or(false)
                    {
                        return resp(
                            400,
                            json!({
                                "ok": false,
                                "error": "Game is not marked as offline-capable",
                                "hint": "Use D3DMetal or set offline_capable = true in mtsp-rules.toml for this appid"
                            }),
                        );
                    }

                    let game_dir = crate::setup::resolve_game_dir(id);
                    let Some(ref dir) = game_dir else {
                        return resp(404, json!({"ok": false, "error": "Game directory not found"}));
                    };

                    if let Some(home) = dirs::home_dir() {
                        crate::mtsp::launcher::deploy_goldberg_for_pipeline(
                            &home,
                            &std::path::PathBuf::from(dir),
                            id,
                            pipeline,
                        );
                    }

                    let bottle = match bottles::prepare_steam_game_launch(id, pipeline) {
                        Ok(bottle) => bottle,
                        Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                    };

                    let (mut env, launch_recipe) = match mtsp::launcher::prepare_steam_pipeline_env(id, pipeline) {
                        Ok(prepared) => prepared,
                        Err(e) => return resp(500, json!({"ok": false, "error": e.to_string()})),
                    };

                    env.push(("SteamAppId".to_string(), id.to_string()));
                    env.push(("SteamGameId".to_string(), id.to_string()));
                    env.push(("METALSHARP_OFFLINE_MODE".to_string(), "1".to_string()));

                    let bottle_prefix = std::path::PathBuf::from(&bottle.prefix_path);
                    let launch_result =
                        mtsp::launcher::launch_steam_bottle_with_pipeline(id, pipeline, &bottle_prefix, &env);

                    app_log(&format!(
                        "[OFFLINE] appid={} pipeline={} backend={} offline_runtime=goldberg",
                        id, node.name, node.graphics_backend
                    ));

                    match launch_result {
                        Ok((pid, game_type, log_path)) => {
                            register_game_pid(id, pid);
                            resp(
                                200,
                                json!({
                                    "ok": true,
                                    "pid": pid,
                                    "appid": id,
                                    "gameType": game_type,
                                    "bottle_id": bottle.id,
                                    "bottle_prefix": bottle.prefix_path,
                                    "launch_log": log_path.to_string_lossy().to_string(),
                                    "pipeline": pipeline,
                                    "graphics_backend": node.graphics_backend,
                                    "offline_mode": true,
                                }),
                            )
                        },
                        Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
                    }
                },
                Err(error) => resp(400, json!({"ok": false, "error": error})),
            }
        },
        (Method::Post, "/steam/view-game") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            app_log(&format!("Opening game in Steam library: appid {}", appid));
            match steam::view_game_in_steam(appid) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Get, "/logs") => {
            let home = dirs::home_dir().unwrap_or_default();
            let log_path = crate::platform::metalsharp_home_dir_for(&home).join("logs");
            let mut entries = Vec::new();
            let mut files: Vec<std::path::PathBuf> = if log_path.exists() {
                walkdir::WalkDir::new(&log_path)
                    .max_depth(2)
                    .into_iter()
                    .flatten()
                    .filter(|entry| entry.path().extension().map(|e| e == "log").unwrap_or(false))
                    .map(|entry| entry.path().to_path_buf())
                    .collect()
            } else {
                Vec::new()
            };
            files.sort_by_key(|path| {
                std::fs::metadata(path).and_then(|meta| meta.modified()).unwrap_or(std::time::UNIX_EPOCH)
            });
            files.reverse();
            for p in files.iter().take(8) {
                if let Ok(content) = std::fs::read_to_string(p) {
                    let lines: Vec<&str> = content.lines().rev().take(500).collect();
                    entries.push(json!({
                        "name": p.strip_prefix(&log_path).unwrap_or(p).to_string_lossy(),
                        "lines": lines.into_iter().rev().collect::<Vec<&str>>(),
                    }));
                }
            }
            if entries.is_empty() {
                entries.push(json!({
                    "name": "app.log",
                    "lines": ["No logs yet. Logs will appear here as you use MetalSharp."],
                }));
            }
            resp(200, json!({"ok": true, "logs": entries}))
        },
        (Method::Get, "/logs/stream") => {
            let home = dirs::home_dir().unwrap_or_default();
            let log_dir = crate::platform::metalsharp_home_dir_for(&home).join("logs");
            let url_str = req.url().to_string();
            let after: usize = url_str
                .split("after=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let log_path = log_dir.join(format!("{}.log", chrono_date()));
            let log_name = log_path.file_name().unwrap_or_default().to_string_lossy().to_string();
            if let Ok(content) = std::fs::read_to_string(&log_path) {
                let all_lines: Vec<&str> = content.lines().collect();
                let total = all_lines.len();
                let new_lines: Vec<&str> = if after < total { all_lines[after..].to_vec() } else { Vec::new() };
                resp(
                    200,
                    json!({
                        "ok": true,
                        "name": log_name,
                        "total": total,
                        "lines": new_lines,
                    }),
                )
            } else {
                resp(200, json!({"ok": true, "name": log_name, "total": 0, "lines": []}))
            }
        },
        (Method::Get, "/logs/crash-reports") => {
            let home = dirs::home_dir().unwrap_or_default();
            let ms_home = crate::platform::metalsharp_home_dir_for(&home);
            let reports = collect_crash_reports(&ms_home);
            resp(200, json!({"ok": true, "reports": reports}))
        },
        (Method::Post, "/logs/crash-report") => {
            let body = read_body_or_return!(req);
            let Some(file) = body.get("file").and_then(|value| value.as_str()) else {
                return resp(400, json!({"ok": false, "error": "file required"}));
            };
            let home = dirs::home_dir().unwrap_or_default();
            let ms_home = crate::platform::metalsharp_home_dir_for(&home);
            let reports = collect_crash_reports(&ms_home);
            if !crash_report_is_enumerated(&reports, file) {
                return resp(404, json!({"ok": false, "error": "crash report not found"}));
            }
            match crash_report_preview(&ms_home, std::path::Path::new(file)) {
                Ok(lines) => resp(200, json!({"ok": true, "lines": lines})),
                Err(error) => resp(400, json!({"ok": false, "error": error})),
            }
        },
        // Open a launch log / report in the default macOS viewer. Path must
        // resolve under the MetalSharp home (mirrors crash_report_preview's
        // containment check) so the local API can't be used to open arbitrary
        // files.
        (Method::Post, "/diagnostics/open") => {
            let body = read_body_or_return!(req);
            let Some(path) = body.get("path").and_then(|value| value.as_str()) else {
                return resp(400, json!({"ok": false, "error": "path required"}));
            };
            let home = dirs::home_dir().unwrap_or_default();
            let ms_home = crate::platform::metalsharp_home_dir_for(&home);
            match open_path_under_home(&ms_home, std::path::Path::new(path)) {
                Ok(()) => resp(200, json!({"ok": true, "path": path})),
                Err(error) => resp(400, json!({"ok": false, "error": error})),
            }
        },
        (Method::Get, "/config") => resp(200, launch::get_config()),
        (Method::Post, "/config") => {
            let body = read_body_or_return!(req);
            match launch::set_config(&body) {
                Ok(cfg) => resp(200, cfg),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Get, "/mtsp/default-rules") => resp(200, mtsp::default_rules::handle_default_rules_catalog()),
        (Method::Get, "/mtsp/launch-shape") => {
            let url = req.url();
            let appid: u32 = query_param(url, "appid").and_then(|v| v.parse().ok()).unwrap_or(0);
            let pipeline_str = query_param(url, "pipeline").unwrap_or_else(|| "auto".to_string());
            let pipeline = if pipeline_str.eq_ignore_ascii_case("auto") {
                mtsp::engine::PipelineId::Dxmt
            } else {
                mtsp::engine::PipelineId::from_str_flexible(&pipeline_str).unwrap_or(mtsp::engine::PipelineId::Dxmt)
            };
            resp(200, mtsp::default_rules::handle_launch_shape(appid, pipeline))
        },
        (Method::Get, "/mtsp/pipelines") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let pipeline_id = bottles::resolve_steam_pipeline_for_request(appid, None);
            let node = mtsp::engine::get_pipeline(pipeline_id);
            let preferred_pipeline = bottles::preferred_pipeline_for_steam_app(appid);
            let all_pipelines: Vec<serde_json::Value> = mtsp::engine::pipelines()
                .iter()
                .filter(|p| p.id.is_user_selectable())
                .map(|p| {
                    serde_json::json!({
                        "id": p.id.user_selectable_id().unwrap_or("auto"),
                        "name": p.id.user_selectable_name().unwrap_or(p.name),
                        "description": p.description,
                        "backend": p.backend,
                        "experimental": p.experimental,
                        "requires_wine": p.requires_wine,
                    })
                })
                .collect();
            resp(
                200,
                json!({
                    "ok": true,
                    "appid": appid,
                    "recommended": pipeline_id.user_selectable_id().unwrap_or("auto"),
                    "recommended_name": pipeline_id.user_selectable_name().unwrap_or("Auto"),
                    "preferred": preferred_pipeline.and_then(|p| p.user_selectable_id().map(|id| id.to_string())),
                    "preferred_name": preferred_pipeline.and_then(|p| {
                        p.user_selectable_name().map(|name| name.to_string())
                    }),
                    "pipelines": all_pipelines,
                }),
            )
        },
        (Method::Post, "/mtsp/prepare") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let requested_pipeline = body.get("pipeline").or_else(|| body.get("launchMethod")).and_then(|v| v.as_str());
            let requested_pipeline = match requested_pipeline {
                Some(value) => match mtsp::engine::PipelineId::from_str_flexible(value) {
                    Some(pipeline) => Some(pipeline),
                    None => return resp(400, json!({"ok": false, "error": format!("unknown pipeline: {}", value)})),
                },
                None => None,
            };
            match mtsp::launcher::prepare_pipeline_with_request(appid, requested_pipeline) {
                Ok(v) => resp(200, v),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/mtsp/recipe") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let method = body.get("launchMethod").and_then(|v| v.as_str()).unwrap_or("auto");
            let pipeline =
                bottles::resolve_steam_pipeline_for_request(appid, mtsp::engine::PipelineId::from_str_flexible(method));
            let node = mtsp::engine::get_pipeline(pipeline);
            match mtsp::recipe::build_launch_recipe(appid, node) {
                Ok(recipe) => resp(200, json!({"ok": true, "appid": appid, "recipe": recipe})),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/mtsp/doctor") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let method = body.get("launchMethod").and_then(|v| v.as_str()).unwrap_or("auto");
            let pipeline =
                bottles::resolve_steam_pipeline_for_request(appid, mtsp::engine::PipelineId::from_str_flexible(method));
            let node = mtsp::engine::get_pipeline(pipeline);
            let report = mtsp::recipe::diagnose_launch_request(appid, node);
            resp(200, json!({"ok": true, "appid": appid, "report": report}))
        },
        (Method::Get, "/goldberg/status") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let game_dir =
                crate::setup::resolve_windows_game_dir(appid).or_else(|| crate::setup::resolve_game_dir(appid));
            let pipeline = bottles::resolve_steam_pipeline_for_request(appid, None);
            let active = game_dir
                .as_ref()
                .and_then(|d| {
                    dirs::home_dir().map(|home| {
                        mtsp::launcher::goldberg_status_for_pipeline_with_appid(&home, d, Some(appid), pipeline)
                    })
                })
                .unwrap_or(false);
            let metadata = mtsp::launcher::read_goldberg_metadata(appid);
            let backed_up_at = metadata.as_ref().and_then(|m| m.backed_up_at);
            let persisted_active = metadata.as_ref().map(|m| m.active).unwrap_or(false);
            let cache_root = mtsp::launcher::goldberg_cache_dir(appid);
            let cache_files = if cache_root.is_dir() {
                std::fs::read_dir(&cache_root)
                    .ok()
                    .map(|entries| {
                        entries.flatten().filter_map(|e| e.file_name().to_str().map(str::to_string)).collect::<Vec<_>>()
                    })
                    .unwrap_or_default()
            } else {
                Vec::new()
            };
            let pipeline_id = pipeline.user_selectable_id().unwrap_or_else(|| pipeline.to_legacy_method());
            let cache_files_ok = cache_files.contains(&"steam_api64.dll".to_string());
            resp(
                200,
                json!({
                    "ok": true,
                    "appid": appid,
                    "goldberg_active": active,
                    "persisted_active": persisted_active,
                    "cache_files_ok": cache_files_ok,
                    "backed_up_at": backed_up_at,
                    "cache_files": cache_files,
                    "pipeline": pipeline_id,
                }),
            )
        },
        (Method::Get, "/eac/status") => {
            let appid = req
                .url()
                .split("appid=")
                .nth(1)
                .and_then(|value| value.split('&').next())
                .and_then(|value| value.parse::<u32>().ok());
            match appid {
                Some(id) if id > 0 => resp(200, anticheat::handle_eac_status_for_appid(id)),
                _ => resp(400, json!({"ok": false, "error": "appid required"})),
            }
        },
        (Method::Post, "/eac/toggle") => {
            let body = read_body_or_return!(req);
            let enabled = body.get("enable").and_then(|value| value.as_bool()).unwrap_or(true);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            app_log(&format!(
                "[EAC] {} per-game substrate for appid {}",
                if enabled { "enabled" } else { "disabled" },
                appid
            ));
            let result = anticheat::handle_eac_toggle(&body);
            if result.get("ok").and_then(Value::as_bool) == Some(true) {
                resp(200, result)
            } else {
                resp(400, result)
            }
        },
        (Method::Post, "/goldberg/toggle") => {
            let body = read_body_or_return!(req);
            let enable = body.get("enable").and_then(|v| v.as_bool()).unwrap_or(true);
            let aid = match parse_request_appid(&body) {
                Ok(aid) => aid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let game_dir = crate::setup::resolve_windows_game_dir(aid).or_else(|| crate::setup::resolve_game_dir(aid));
            match game_dir {
                Some(dir) if dir.exists() => {
                    let pipeline = bottles::resolve_steam_pipeline_for_request(aid, None);
                    let pipeline_id = pipeline.user_selectable_id().unwrap_or_else(|| pipeline.to_legacy_method());
                    if enable {
                        let home = dirs::home_dir().unwrap_or_default();
                        mtsp::launcher::deploy_goldberg_for_pipeline(&home, &dir.to_path_buf(), aid, pipeline);
                        app_log(&format!("[STEAM_EMU] enabled for appid {}", aid));
                        let cache_root = mtsp::launcher::goldberg_cache_dir(aid);
                        let cache_files: Vec<String> = if cache_root.is_dir() {
                            std::fs::read_dir(&cache_root)
                                .ok()
                                .map(|entries| {
                                    entries
                                        .flatten()
                                        .filter_map(|e| e.file_name().to_str().map(str::to_string))
                                        .collect()
                                })
                                .unwrap_or_default()
                        } else {
                            Vec::new()
                        };
                        let cache_files_ok = cache_files.contains(&"steam_api64.dll".to_string());
                        let metadata = mtsp::launcher::read_goldberg_metadata(aid);
                        let backed_up_at = metadata.as_ref().and_then(|m| m.backed_up_at);
                        resp(
                            200,
                            json!({
                                "ok": true,
                                "goldberg_active": true,
                                "cache_files_ok": cache_files_ok,
                                "backed_up_at": backed_up_at,
                                "cache_files": cache_files,
                                "pipeline": pipeline_id,
                            }),
                        )
                    } else {
                        let home = dirs::home_dir().unwrap_or_default();
                        mtsp::launcher::cleanup_goldberg_for_pipeline(&home, &dir, aid, pipeline);
                        app_log(&format!("[STEAM_EMU] disabled for appid {}", aid));
                        let cache_root = mtsp::launcher::goldberg_cache_dir(aid);
                        let cache_files: Vec<String> = if cache_root.is_dir() {
                            std::fs::read_dir(&cache_root)
                                .ok()
                                .map(|entries| {
                                    entries
                                        .flatten()
                                        .filter_map(|e| e.file_name().to_str().map(str::to_string))
                                        .collect()
                                })
                                .unwrap_or_default()
                        } else {
                            Vec::new()
                        };
                        let cache_files_ok = cache_files.contains(&"steam_api64.dll".to_string());
                        resp(
                            200,
                            json!({
                                "ok": true,
                                "goldberg_active": false,
                                "cache_files_ok": cache_files_ok,
                                "cache_files": cache_files,
                                "pipeline": pipeline_id,
                            }),
                        )
                    }
                },
                _ => resp(404, json!({"ok": false, "error": "game directory not found"})),
            }
        },
        (Method::Get, "/sharp-library") => resp(200, sharp_library::handle_get_library()),
        (Method::Get, "/bottles") => resp(200, bottles::handle_list_bottles()),
        (Method::Post, "/d3dmetal/bottles/save") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_save(&body))
        },
        (Method::Post, "/d3dmetal/bottles/status") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_status(&body))
        },
        (Method::Post, "/d3dmetal/bottles/install-homebrew-gptk") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_install_homebrew_gptk(&body))
        },
        (Method::Post, "/d3dmetal/bottles/install-rosetta") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_install_rosetta(&body))
        },
        (Method::Post, "/d3dmetal/bottles/repair-gptk-payload") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_repair_gptk_payload(&body))
        },
        (Method::Post, "/d3dmetal/bottles/install-x64-redist") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_install_x64_redist(&body))
        },
        (Method::Post, "/d3dmetal/bottles/seed-prefix") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_seed_prefix(&body))
        },
        (Method::Post, "/d3dmetal/bottles/play") => {
            let body = read_body_or_return!(req);
            resp(200, d3dmetal_gptk::handle_play(&body))
        },
        (Method::Get, "/bottles/profiles") => resp(200, bottles::handle_list_runtime_profiles()),
        // Phase 2: declarative Steam route contract table (protected + first-class lanes).
        (Method::Get, "/bottles/route-contracts") => {
            resp(200, json!({ "ok": true, "contracts": bottles::steam_route_contracts() }))
        },
        (Method::Get, "/bottles/compatibility-matrix") => resp(200, bottles::handle_compatibility_matrix()),
        (Method::Get, "/bottles/redist-sources") => resp(200, bottles::handle_redist_sources()),
        (Method::Post, "/bottles/record-compatibility") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_record_compatibility_case(&body))
        },
        (Method::Post, "/bottles/sync-steam") => resp(200, bottles::handle_sync_steam_bottles()),
        (Method::Post, "/bottles/get") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_get_bottle(&body))
        },
        (Method::Post, "/bottles/refresh") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_refresh_bottle(&body))
        },
        (Method::Post, "/bottles/doctor") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_diagnose_bottle(&body))
        },
        (Method::Post, "/bottles/prepare") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_prepare_bottle(&body))
        },
        (Method::Post, "/bottles/repair-component") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_repair_component(&body))
        },
        (Method::Post, "/bottles/set-runtime-profile") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_set_runtime_profile(&body))
        },
        (Method::Post, "/bottles/edit") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_edit_bottle(&body))
        },
        (Method::Post, "/bottles/set-windows-version") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_set_windows_version(&body))
        },
        (Method::Post, "/bottles/relaunch-installer") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_relaunch_bottle_installer(&body))
        },
        (Method::Post, "/bottles/apply-font-subs") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_apply_font_substitutions(&body))
        },
        (Method::Post, "/bottles/seed-post-wineboot") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_seed_post_wineboot(&body))
        },
        (Method::Post, "/bottles/verify-directx") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_verify_directx(&body))
        },
        (Method::Post, "/steam/install-recipe-deps") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_install_recipe_deps(&body))
        },
        (Method::Post, "/steam/runtime-doctor") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_steam_runtime_doctor(&body))
        },
        (Method::Post, "/steam/d3d12-runtime-doctor") => {
            let body = read_body_or_return!(req);
            resp(200, d3d12_runtime_doctor::handle_steam_d3d12_runtime_doctor(&body))
        },
        // Phase 1: baseline launch observability. Stable JSON diagnostic that
        // reports the resolved pipeline, runtime profile, wine path, prefix,
        // artifact sources (with content hashes), staged DLL hashes, and cache
        // directories for an appid. No launch behavior changes.
        (Method::Get, "/diagnostics/launch") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let requested_pipeline = url_str
                .split("pipeline=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(crate::mtsp::engine::PipelineId::from_str_flexible);
            resp(200, diagnostics::build_launch_diagnostic(appid, requested_pipeline))
        },
        (Method::Get, "/diagnostics/launch/timing") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let home = dirs::home_dir().unwrap_or_default();
            let bottle_id = format!("steam_{}", appid);
            match diagnostics::latest_launch_timing(&home, &bottle_id) {
                Some(timing) => {
                    resp(200, json!({ "ok": true, "appid": appid, "bottle_id": bottle_id, "timing": timing }))
                },
                None => resp(
                    200,
                    json!({ "ok": false, "appid": appid, "bottle_id": bottle_id, "error": "no launch timing recorded for this bottle yet" }),
                ),
            }
        },
        // Phase 3: M12 artifact + launch verification (dry-run). Reports the
        // exact env pairs and artifact hashes M12 would load, without
        // launching Steam or the game. Uses the same env builder as launch.
        (Method::Get, "/diagnostics/m12/dry-run") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            resp(200, mtsp::launcher::m12_verify_dry_run(appid))
        },
        (Method::Get, "/diagnostics/pipeline/dry-run") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let requested_pipeline = url_str
                .split("pipeline=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(crate::mtsp::engine::PipelineId::from_str_flexible);
            let home = dirs::home_dir().unwrap_or_default();
            resp(200, mtsp::launcher::pipeline_dry_run_for(&home, appid, requested_pipeline))
        },
        // Phase 4: shader/PSO/cache diagnostics.
        (Method::Get, "/diagnostics/cache-doctor") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            resp(200, mtsp::shader_cache::cache_doctor(appid))
        },
        (Method::Get, "/diagnostics/pso-manifests") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let limit = url_str
                .split("limit=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse::<usize>().ok())
                .unwrap_or(20)
                .min(200);
            let requested_pipeline = url_str
                .split("pipeline=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(crate::mtsp::engine::PipelineId::from_str_flexible);
            let pipeline = bottles::resolve_steam_pipeline_for_request(appid, requested_pipeline);
            let home = dirs::home_dir().unwrap_or_default();
            let manifests = mtsp::shader_cache::recent_pso_manifests(&home, pipeline, appid, limit);
            resp(
                200,
                json!({ "ok": true, "appid": appid, "pipeline": pipeline, "count": manifests.len(), "manifests": manifests }),
            )
        },
        // Phase 5: descriptor / root-signature binding contract validator.
        // Accepts a root signature manifest JSON and (optionally) reflection
        // bindings, returns a structured pass/fail against Metal's direct-
        // binding limits and D3D12 ABI rules.
        (Method::Post, "/diagnostics/binding-contract/validate") => {
            let body = read_body_or_return!(req);
            let manifest_json = body.get("root_signature").cloned().unwrap_or(json!(null));
            let reflection_json = body.get("reflection").cloned().unwrap_or(json!([]));
            match serde_json::from_value::<binding_contract::RootSignatureManifest>(manifest_json) {
                Ok(manifest) => {
                    let reflection: Vec<binding_contract::ReflectionBinding> =
                        serde_json::from_value(reflection_json).unwrap_or_default();
                    let report = binding_contract::validate_root_signature_with(
                        &manifest,
                        &binding_contract::ReflectionBindingSet::from_bindings(reflection),
                        binding_contract::BindingLimits::default(),
                    );
                    resp(200, serde_json::to_value(report).unwrap_or(json!({"ok": false, "error": "serialize failed"})))
                },
                Err(e) => resp(400, json!({ "ok": false, "error": format!("invalid root signature manifest: {}", e) })),
            }
        },
        // Phase 6: command replay / barriers / resource visibility validator.
        // Accepts a recorded command-list trace JSON and returns a structured
        // pass/fail against encoder-lifetime, render-pass, and transition rules.
        (Method::Post, "/diagnostics/command-replay/validate") => {
            let body = read_body_or_return!(req);
            let trace_json = body.get("trace").cloned().unwrap_or(json!([]));
            match serde_json::from_value::<Vec<command_contract::CommandOp>>(trace_json) {
                Ok(ops) => {
                    let report = command_contract::validate_command_trace(&ops);
                    resp(200, serde_json::to_value(report).unwrap_or(json!({"ok": false, "error": "serialize failed"})))
                },
                Err(e) => resp(400, json!({ "ok": false, "error": format!("invalid command trace: {}", e) })),
            }
        },
        // Phase 7: runtime artifact verification (presence + sha256 per file),
        // wineboot state, and stop-Wine-Steam target report.
        (Method::Get, "/diagnostics/runtime-artifacts") => resp(200, installer::runtime_artifact_report()),
        (Method::Get, "/diagnostics/wineboot-state") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let verifying = url_str.contains("verifying=true");
            resp(200, bottles::steam_prefix_wineboot_state(appid, verifying))
        },
        (Method::Get, "/steam/stop-targets") => resp(200, steam::stop_wine_steam_targets()),
        // Phase 8: Mono/FNA/XNA flavor detection, profile explanation, and
        // conservative unproven-game classification. These explain the lane
        // selection without changing pinned known-good behavior.
        (Method::Get, "/diagnostics/fna/signals") => {
            let url_str = req.url().to_string();
            let game_dir = url_str
                .split("gameDir=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .map(|s| url_decode(s))
                .unwrap_or_default();
            let path = match resolve_fna_game_dir(&game_dir) {
                Ok(path) => path,
                Err(error) => return resp(400, json!({ "ok": false, "error": error })),
            };
            resp(200, serde_json::to_value(fna_profile::detect_fna_signals(&path)).unwrap())
        },
        (Method::Get, "/diagnostics/fna/explain") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let game_dir = url_str
                .split("gameDir=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .map(|s| url_decode(s))
                .unwrap_or_default();
            let path = match resolve_fna_game_dir(&game_dir) {
                Ok(path) => path,
                Err(error) => return resp(400, json!({ "ok": false, "error": error })),
            };
            resp(200, serde_json::to_value(fna_profile::explain_profile(appid, &path)).unwrap())
        },
        (Method::Get, "/diagnostics/fna/classify") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            let game_dir = url_str
                .split("gameDir=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .map(|s| url_decode(s))
                .unwrap_or_default();
            let path = match resolve_fna_game_dir(&game_dir) {
                Ok(path) => path,
                Err(error) => return resp(400, json!({ "ok": false, "error": error })),
            };
            resp(200, serde_json::to_value(fna_profile::classify_unproven_fna_game(appid, &path)).unwrap())
        },
        (Method::Post, "/steam/compatdata") => {
            let body = read_body_or_return!(req);
            resp(200, bottles::handle_steam_compatdata(&body))
        },
        // Anti-cheat evidence and host-contract probes are intentionally
        // observational: they report the protected launcher/module boundary
        // without changing vendor binaries, identity exports, or launch
        // policy.
        (Method::Post, "/steam/anticheat-evidence") => {
            let body = read_body_or_return!(req);
            resp(200, anticheat::handle_steam_anticheat_evidence(&body))
        },
        (Method::Post, "/steam/anticheat-probe") => {
            let body = read_body_or_return!(req);
            resp(200, anticheat::handle_steam_anticheat_probe(&body))
        },
        (Method::Post, "/steam/anticheat-delta-audit") => {
            let body = read_body_or_return!(req);
            resp(200, anticheat::handle_steam_anticheat_delta_audit(&body))
        },
        (Method::Post, "/steam/anticheat-substrate-decision") => {
            let body = read_body_or_return!(req);
            resp(200, anticheat::handle_steam_anticheat_substrate_decision(&body))
        },
        (Method::Post, "/steam/anticheat-contract-probe") => {
            let body = read_body_or_return!(req);
            resp(200, anticheat::handle_steam_anticheat_contract_probe(&body))
        },
        (Method::Post, "/kernel-translation/probe") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_kernel_translation_probe(&body))
        },
        (Method::Post, "/kernel-translation/host-probe") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::probe::handle_kernel_probe(&body))
        },
        (Method::Post, "/kernel-translation/handle/create") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_create(&body))
        },
        (Method::Post, "/kernel-translation/handle/close") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_close(&body))
        },
        (Method::Post, "/kernel-translation/handle/duplicate") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_duplicate(&body))
        },
        (Method::Post, "/kernel-translation/handle/query") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_query(&body))
        },
        (Method::Post, "/kernel-translation/handle/enumerate") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_enumerate(&body))
        },
        (Method::Post, "/kernel-translation/handle/system-info") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_system_handle_information(&body))
        },
        (Method::Post, "/kernel-translation/handle/table-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_table_status(&body))
        },
        (Method::Post, "/kernel-translation/handle/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_table::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/handle/enumerate-fds") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_bridge::handle_enumerate_fds(&body))
        },
        (Method::Post, "/kernel-translation/handle/enumerate-ports") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_bridge::handle_enumerate_ports(&body))
        },
        (Method::Post, "/kernel-translation/handle/unified-snapshot") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_bridge::handle_unified_snapshot(&body))
        },
        (Method::Post, "/kernel-translation/handle/snapshot-all") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_bridge::handle_snapshot_all(&body))
        },
        (Method::Post, "/kernel-translation/integrity/query-signing-level") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_query_signing_level(&body))
        },
        (Method::Post, "/kernel-translation/integrity/query-process-signing") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_query_process_signing(&body))
        },
        (Method::Post, "/kernel-translation/integrity/register-pe-module") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_register_pe_module(&body))
        },
        (Method::Post, "/kernel-translation/integrity/register-macho-module") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_register_macho_module(&body))
        },
        (Method::Post, "/kernel-translation/integrity/set-cached-signing-level") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_set_cached_signing_level(&body))
        },
        (Method::Post, "/kernel-translation/integrity/list-modules") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_list_signed_modules(&body))
        },
        (Method::Post, "/kernel-translation/integrity/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::code_integrity::handle_seed_integrity_demo(&body))
        },
        (Method::Post, "/kernel-translation/apc/queue") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_queue_apc(&body))
        },
        (Method::Post, "/kernel-translation/apc/test-alert") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_test_alert(&body))
        },
        (Method::Post, "/kernel-translation/apc/wait-alertable") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_wait_alertable(&body))
        },
        (Method::Post, "/kernel-translation/apc/allocate-trampoline") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_allocate_trampoline(&body))
        },
        (Method::Post, "/kernel-translation/apc/suspend-thread") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_suspend_thread(&body))
        },
        (Method::Post, "/kernel-translation/apc/get-thread-context") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_get_thread_context(&body))
        },
        (Method::Post, "/kernel-translation/apc/set-thread-context") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_set_thread_context(&body))
        },
        (Method::Post, "/kernel-translation/apc/inject-sequence") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_inject_apc_sequence(&body))
        },
        (Method::Post, "/kernel-translation/apc/queue-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_apc_queue_status(&body))
        },
        (Method::Post, "/kernel-translation/apc/trampoline-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::apc::handle_trampoline_status(&body))
        },
        (Method::Post, "/kernel-translation/es/register-callback") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_register_callback(&body))
        },
        (Method::Post, "/kernel-translation/es/unregister-callback") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_unregister_callback(&body))
        },
        (Method::Post, "/kernel-translation/es/list-callbacks") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_list_callbacks(&body))
        },
        (Method::Post, "/kernel-translation/es/fire-process-event") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_fire_process_event(&body))
        },
        (Method::Post, "/kernel-translation/es/fire-thread-event") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_fire_thread_event(&body))
        },
        (Method::Post, "/kernel-translation/es/fire-image-event") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_fire_image_event(&body))
        },
        (Method::Post, "/kernel-translation/es/process-events") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_process_events(&body))
        },
        (Method::Post, "/kernel-translation/es/thread-events") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_thread_events(&body))
        },
        (Method::Post, "/kernel-translation/es/image-events") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_image_events(&body))
        },
        (Method::Post, "/kernel-translation/es/create-ipc-channel") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_create_ipc_channel(&body))
        },
        (Method::Post, "/kernel-translation/es/ipc-channels") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_ipc_channels(&body))
        },
        (Method::Post, "/kernel-translation/es/status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_es_status(&body))
        },
        (Method::Post, "/kernel-translation/es/detect-events") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_detect_events(&body))
        },
        (Method::Post, "/kernel-translation/es/nt-callback-bridge") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_nt_callback_bridge(&body))
        },
        (Method::Post, "/kernel-translation/es/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_bridge::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/thread/snapshot") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_snapshot_threads(&body))
        },
        (Method::Post, "/kernel-translation/thread/compute-delta") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_compute_delta(&body))
        },
        (Method::Post, "/kernel-translation/thread/create-watcher") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_create_watcher(&body))
        },
        (Method::Post, "/kernel-translation/thread/destroy-watcher") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_destroy_watcher(&body))
        },
        (Method::Post, "/kernel-translation/thread/list-watchers") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_list_watchers(&body))
        },
        (Method::Post, "/kernel-translation/thread/poll-watcher") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_poll_watcher(&body))
        },
        (Method::Post, "/kernel-translation/thread/info") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_thread_info(&body))
        },
        (Method::Post, "/kernel-translation/thread/list-deltas") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_list_deltas(&body))
        },
        (Method::Post, "/kernel-translation/thread/configure-notifications") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_configure_notifications(&body))
        },
        (Method::Post, "/kernel-translation/thread/mechanism-survey") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_mechanism_survey(&body))
        },
        (Method::Post, "/kernel-translation/thread/watcher-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_watcher_status(&body))
        },
        (Method::Post, "/kernel-translation/thread/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::thread_notify::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/ob/register-callback") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_register_callback(&body))
        },
        (Method::Post, "/kernel-translation/ob/unregister-callback") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_unregister_callback(&body))
        },
        (Method::Post, "/kernel-translation/ob/list-registrations") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_list_registrations(&body))
        },
        (Method::Post, "/kernel-translation/ob/protect-process") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_protect_process(&body))
        },
        (Method::Post, "/kernel-translation/ob/unprotect-process") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_unprotect_process(&body))
        },
        (Method::Post, "/kernel-translation/ob/list-protected") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_list_protected(&body))
        },
        (Method::Post, "/kernel-translation/ob/simulate-operation") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_simulate_operation(&body))
        },
        (Method::Post, "/kernel-translation/ob/pre-operations") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_pre_operations(&body))
        },
        (Method::Post, "/kernel-translation/ob/post-operations") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_post_operations(&body))
        },
        (Method::Post, "/kernel-translation/ob/access-log") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_access_log(&body))
        },
        (Method::Post, "/kernel-translation/ob/capability-survey") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_capability_survey(&body))
        },
        (Method::Post, "/kernel-translation/ob/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::handle_callbacks::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/driver/load") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_load_driver(&body))
        },
        (Method::Post, "/kernel-translation/driver/unload") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_unload_driver(&body))
        },
        (Method::Post, "/kernel-translation/driver/list") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_list_drivers(&body))
        },
        (Method::Post, "/kernel-translation/driver/create-device") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_create_device(&body))
        },
        (Method::Post, "/kernel-translation/driver/list-devices") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_list_devices(&body))
        },
        (Method::Post, "/kernel-translation/driver/dispatch-irp") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_dispatch_irp(&body))
        },
        (Method::Post, "/kernel-translation/driver/list-irps") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_list_irps(&body))
        },
        (Method::Post, "/kernel-translation/driver/register-ioctl") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_register_ioctl(&body))
        },
        (Method::Post, "/kernel-translation/driver/decode-ioctl") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_decode_ioctl(&body))
        },
        (Method::Post, "/kernel-translation/driver/list-ioctls") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_list_ioctls(&body))
        },
        (Method::Post, "/kernel-translation/driver/type-mapping-survey") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_type_mapping_survey(&body))
        },
        (Method::Post, "/kernel-translation/driver/extension-template") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_extension_template(&body))
        },
        (Method::Post, "/kernel-translation/driver/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::driver_model::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/simulate-check") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_simulate_check(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/run-all-checks") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_run_all_checks(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/check-results") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_check_results(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/hw-breakpoint-map") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_hw_breakpoint_map(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/full-breakpoint-map") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_full_breakpoint_map(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/module-sanitize") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_module_sanitize(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/add-sanitize-rule") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_add_sanitize_rule(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/timing-analysis") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_timing_analysis(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/filesystem-check") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_filesystem_check(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/status-survey") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_status_survey(&body))
        },
        (Method::Post, "/kernel-translation/anti-debug/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::anti_debug::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/integration/extension-install") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_extension_install(&body))
        },
        (Method::Post, "/kernel-translation/integration/extension-activate") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_extension_activate(&body))
        },
        (Method::Post, "/kernel-translation/integration/extension-deactivate") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_extension_deactivate(&body))
        },
        (Method::Post, "/kernel-translation/integration/extension-crash") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_extension_simulate_crash(&body))
        },
        (Method::Post, "/kernel-translation/integration/extension-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_extension_status(&body))
        },
        (Method::Post, "/kernel-translation/integration/simulate-pipeline") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_simulate_pipeline(&body))
        },
        (Method::Post, "/kernel-translation/integration/bottle-configure") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_bottle_configure(&body))
        },
        (Method::Post, "/kernel-translation/integration/bottle-get-config") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_bottle_get_config(&body))
        },
        (Method::Post, "/kernel-translation/integration/bottle-list-configs") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_bottle_list_configs(&body))
        },
        (Method::Post, "/kernel-translation/integration/runtime-doctor") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_runtime_doctor(&body))
        },
        (Method::Post, "/kernel-translation/integration/log-translation") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_log_translation(&body))
        },
        (Method::Post, "/kernel-translation/integration/query-translation-log") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_query_translation_log(&body))
        },
        (Method::Post, "/kernel-translation/integration/register-multi-ac") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_register_multi_ac(&body))
        },
        (Method::Post, "/kernel-translation/integration/list-multi-ac") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_list_multi_ac(&body))
        },
        (Method::Post, "/kernel-translation/integration/simulate-conflict") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_simulate_conflict(&body))
        },
        (Method::Post, "/kernel-translation/integration/performance-profile") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_performance_profile(&body))
        },
        (Method::Post, "/kernel-translation/integration/list-performance") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_list_performance(&body))
        },
        (Method::Post, "/kernel-translation/integration/fallback-mode") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_fallback_mode_status(&body))
        },
        (Method::Post, "/kernel-translation/integration/full-stack-status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_full_stack_status(&body))
        },
        (Method::Post, "/kernel-translation/integration/seed-demo") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::integration::handle_seed_demo(&body))
        },
        (Method::Post, "/kernel-translation/ipc/start") => match kernel_translation::ipc_bridge::start_ipc_listener() {
            Ok(()) => {
                resp(200, serde_json::json!({"ok": true, "bind_addr": kernel_translation::ipc_bridge::IPC_BIND_ADDR}))
            },
            Err(e) => resp(500, serde_json::json!({"ok": false, "error": e})),
        },
        (Method::Post, "/kernel-translation/ipc/stop") => {
            resp(200, kernel_translation::ipc_bridge::stop_ipc_listener())
        },
        (Method::Get, "/kernel-translation/ipc/status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::ipc_bridge::handle_ipc_status(&body))
        },
        (Method::Get, "/kernel-translation/ipc/handles") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::ipc_bridge::handle_ipc_handles(&body))
        },
        (Method::Post, "/kernel-translation/es-live/start") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_live::handle_es_live_start(&body))
        },
        (Method::Post, "/kernel-translation/es-live/stop") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_live::handle_es_live_stop(&body))
        },
        (Method::Get, "/kernel-translation/es-live/status") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_live::handle_es_live_status(&body))
        },
        (Method::Get, "/kernel-translation/es-live/events") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_live::handle_es_live_events(&body))
        },
        (Method::Get, "/kernel-translation/es-live/processes") => {
            let body = read_body_or_return!(req);
            resp(200, kernel_translation::es_live::handle_es_live_processes(&body))
        },
        (Method::Post, "/launcher/evidence") => {
            let body = read_body_or_return!(req);
            resp(200, launcher_evidence::handle_launcher_evidence(&body))
        },
        (Method::Get, "/sharp-library/gog/status") => resp(200, gog::handle_status()),
        (Method::Post, "/sharp-library/gog/initialize-prefix") => resp(200, gog::handle_initialize_prefix()),
        (Method::Post, "/sharp-library/gog/remove-prefix") => resp(200, gog::handle_remove_prefix()),
        (Method::Post, "/sharp-library/gog/auth-code") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_auth_code(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/logout") => resp(200, gog::handle_logout()),
        (Method::Post, "/sharp-library/gog/sync") => resp(200, gog::handle_sync()),
        (Method::Get, "/sharp-library/gog/games") => resp(200, gog::handle_games()),
        (Method::Post, "/sharp-library/gog/install") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_install(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/import") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_import(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/progress") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_progress(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/play") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_play(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/stop") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_stop(&Value::Object(body)))
        },
        (Method::Post, "/sharp-library/gog/uninstall") => {
            let body = read_body_or_return!(req);
            resp(200, gog::handle_uninstall(&Value::Object(body)))
        },
        (Method::Get, "/wine-mono/status") => {
            let prefix = query_param(req.url(), "prefix").unwrap_or_else(|| "gog".to_string());
            resp(200, mono::handle_status(&prefix))
        },
        (Method::Post, "/wine-mono/install") => {
            let body = read_body_or_return!(req);
            let prefix = body.get("prefix").and_then(|v| v.as_str()).unwrap_or("gog").to_string();
            resp(200, mono::handle_install(&prefix))
        },
        (Method::Post, "/wine-mono/reset") => {
            let body = read_body_or_return!(req);
            let prefix = body.get("prefix").and_then(|v| v.as_str()).unwrap_or("gog").to_string();
            resp(200, mono::handle_reset(&prefix))
        },
        (Method::Post, "/sharp-library/install") => {
            let body = read_body_or_return!(req);
            app_log(&format!("[SHARP-LIB] install: {}", body.get("srcPath").and_then(|v| v.as_str()).unwrap_or("?")));
            resp(200, sharp_library::handle_install(&body))
        },
        (Method::Post, "/sharp-library/import-bottle-app") => {
            let body = read_body_or_return!(req);
            app_log(&format!(
                "[SHARP-LIB] import bottle app: {}",
                body.get("bottleId").and_then(|v| v.as_str()).unwrap_or("?")
            ));
            resp(200, sharp_library::handle_import_bottle_app(&body))
        },
        (Method::Post, "/sharp-library/uninstall") => {
            let body = read_body_or_return!(req);
            let id = body.get("id").and_then(|v| v.as_str()).unwrap_or("?");
            app_log(&format!("[SHARP-LIB] uninstall: {}", id));
            resp(200, sharp_library::handle_uninstall(&body))
        },
        (Method::Post, "/sharp-library/launch") => {
            let body = read_body_or_return!(req);
            let id = body.get("id").and_then(|v| v.as_str()).unwrap_or("?");
            let engine = body.get("engine").and_then(|v| v.as_str()).unwrap_or("wine_bare");
            app_log(&format!("[SHARP-LIB] launch: {} engine: {}", id, engine));
            let result = sharp_library::handle_launch(&body);
            if result.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) {
                if let Some(pid) = result.get("pid").and_then(|v| v.as_u64()).and_then(|pid| u32::try_from(pid).ok()) {
                    register_sharp_pid(id, pid);
                }
                app_log(&format!(
                    "[SHARP-LIB] launched pid {}",
                    result.get("pid").and_then(|v| v.as_u64()).unwrap_or(0)
                ));
            } else {
                let error = result.get("error").and_then(|v| v.as_str()).unwrap_or("unknown launch error");
                app_issue_log("sharp-launch", id, error, &[format!("engine={}", engine), format!("request_id={}", id)]);
            }
            resp(200, result)
        },
        (Method::Post, "/sharp-library/stop") => {
            let body = read_body_or_return!(req);
            let id = body.get("id").and_then(|v| v.as_str()).unwrap_or("").trim();
            if id.is_empty() {
                return resp(400, json!({"ok": false, "error": "id required"}));
            }
            match stop_registered_sharp_app(id) {
                Ok(pid) => {
                    app_log(&format!("[SHARP-LIB] stopped {} pid {}", id, pid));
                    resp(200, json!({"ok": true, "pid": pid}))
                },
                Err(error) => {
                    app_issue_log("sharp-stop", id, &error, &[]);
                    resp(409, json!({"ok": false, "error": error}))
                },
            }
        },
        (Method::Post, "/sharp-library/doctor") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_doctor(&body))
        },
        (Method::Post, "/sharp-library/set-cover") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_set_cover(&body))
        },
        (Method::Post, "/sharp-library/add-asset") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_add_asset(&body))
        },
        (Method::Post, "/sharp-library/set-cover-position") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_set_cover_position(&body))
        },
        (Method::Post, "/sharp-library/set-engine") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_set_engine(&body))
        },
        (Method::Post, "/sharp-library/set-launch-args") => {
            let body = read_body_or_return!(req);
            resp(200, sharp_library::handle_set_launch_args(&body))
        },
        (Method::Get, "/sharp-library/cover") => {
            let url_str = req.url().to_string();
            let id = url_str.split("id=").nth(1).and_then(|v| v.split('&').next()).unwrap_or("");
            match sharp_library::get_cover_path(id) {
                Some(path) => {
                    let data = std::fs::read(&path).unwrap_or_default();
                    let ext = path.extension().and_then(|ext| ext.to_str()).unwrap_or_default();
                    let mime = if ext.eq_ignore_ascii_case("png") {
                        "image/png"
                    } else if ext.eq_ignore_ascii_case("svg") {
                        "image/svg+xml"
                    } else if ext.eq_ignore_ascii_case("webp") {
                        "image/webp"
                    } else {
                        "image/jpeg"
                    };
                    resp_raw(200, data, mime)
                },
                None => resp(404, json!({"ok": false, "error": "cover not found"})),
            }
        },
        (Method::Post, "/launch") => {
            let body = read_body_or_return!(req);
            let exe = body.get("exePath").and_then(|v| v.as_str()).unwrap_or("");
            let steam_app_id = match parse_optional_request_steam_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let resolved = if let Some(sid) = steam_app_id {
                if !exe.contains(".exe") {
                    resolve_game_exe(sid)
                } else {
                    exe.to_string()
                }
            } else {
                exe.to_string()
            };
            app_log(&format!("Launching: {}", resolved));

            let mut game_type = "native";
            if let Some(sid) = steam_app_id {
                let home = dirs::home_dir().unwrap_or_default();
                let marker = crate::platform::metalsharp_home_dir_for(&home)
                    .join("games")
                    .join(sid.to_string())
                    .join(".metalsharp_prepared");
                if let Ok(content) = std::fs::read_to_string(&marker) {
                    if content.contains("is_dotnet=true") {
                        app_log("Detected XNA/FNA game — using mono runtime");
                        game_type = "xna_fna";
                    }
                }
            }

            match launch::launch(&resolved, game_type) {
                Ok(pid) => {
                    app_log(&format!("Process started: pid {}", pid));
                    resp(200, json!({"ok": true, "pid": pid}))
                },
                Err(e) => {
                    app_log(&format!("Launch failed: {}", e));
                    app_issue_log("launch", &resolved, &e.to_string(), &[format!("game_type={}", game_type)]);
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Post, "/game/launch-auto") => {
            let body = read_body_or_return!(req);
            let id = match parse_request_appid(&body) {
                Ok(id) => id,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            let launch_method = body.get("launchMethod").and_then(|v| v.as_str()).unwrap_or("auto");
            let resolved_pipeline = Some(crate::mtsp::rules::resolve_requested_pipeline(
                id,
                crate::mtsp::engine::PipelineId::from_str_flexible(launch_method),
            ))
            .map(|pipeline| anticheat::eac_pipeline_for_request(id, pipeline));
            let engine_desc =
                resolved_pipeline.map(|p| crate::mtsp::engine::get_pipeline(p).description).unwrap_or("Unknown");
            app_log(&format!("[LAUNCH] appid {} | engine: {} | method: {}", id, engine_desc, launch_method));
            let pipeline = match resolved_pipeline {
                Some(p) => p,
                None => {
                    app_log(&format!("[LAUNCH FAILED] appid {} | no pipeline resolved", id));
                    app_issue_log(
                        "game-launch",
                        &id.to_string(),
                        "no pipeline resolved",
                        &[format!("launch_method={}", launch_method)],
                    );
                    return resp(500, json!({"ok": false, "error": "no pipeline resolved"}));
                },
            };
            let result = crate::mtsp::launcher::launch_with_pipeline(id, pipeline);
            match result {
                Ok((pid, game_type)) => {
                    register_game_pid(id, pid);
                    app_log(&format!("[LAUNCHED] appid {} | pid {} | engine: {}", id, pid, game_type));
                    resp(
                        200,
                        json!({
                            "ok": true,
                            "pid": pid,
                            "gameType": game_type,
                            "appid": id,
                            "engine": engine_desc,
                            "eac_substrate": anticheat::eac_enabled(id),
                        }),
                    )
                },
                Err(e) => {
                    app_log(&format!("[LAUNCH FAILED] appid {} | error: {}", id, e));
                    app_issue_log(
                        "game-launch",
                        &id.to_string(),
                        &e.to_string(),
                        &[format!("engine={}", engine_desc), format!("launch_method={}", launch_method)],
                    );
                    resp(500, json!({"ok": false, "error": e.to_string()}))
                },
            }
        },
        (Method::Get, "/game/running") => {
            prune_inactive_game_pids();
            let map = running_games().lock().unwrap_or_else(|e| e.into_inner());
            let running: Vec<serde_json::Value> =
                map.iter().map(|(&appid, &pid)| json!({"appid": appid, "pid": pid})).collect();
            resp(200, json!({"ok": true, "running": running}))
        },
        (Method::Get, "/game/dual-info") => {
            let url_str = req.url().to_string();
            let appid: u32 = url_str
                .split("appid=")
                .nth(1)
                .and_then(|v| v.split('&').next())
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            if appid == 0 {
                return resp(400, json!({"ok": false, "error": "appid required"}));
            }
            let dual = scan::resolve_dual_game_dir(appid);
            resp(
                200,
                json!({
                    "ok": true,
                    "appid": appid,
                    "has_native_build": dual.has_native_build,
                    "macos_dir": dual.macos_dir.map(|p| p.to_string_lossy().to_string()),
                    "macos_app": dual.macos_app.map(|p| p.to_string_lossy().to_string()),
                    "wine_dir": dual.wine_dir.map(|p| p.to_string_lossy().to_string()),
                }),
            )
        },
        (Method::Post, "/processes/force-kill") => resp(200, force_kill_metalsharp_processes()),
        (Method::Post, "/games/stop-active") => resp(200, stop_active_games()),
        (Method::Post, "/kill") => {
            let body = read_body_or_return!(req);
            let appid = if body.contains_key("appid") {
                match parse_request_appid(&body) {
                    Ok(appid) => Some(appid),
                    Err(error) => return resp(400, json!({"ok": false, "error": error})),
                }
            } else {
                None
            };

            if let Some(aid) = appid {
                // The appid is only an identifier. The PID must come from the
                // backend's registration made at launch; never fall back to a
                // caller-supplied PID when the app is not registered.
                prune_inactive_game_pids();
                let Some(target_pid) = get_game_pid(aid) else {
                    app_log(&format!("[STOP REJECTED] appid {} is not registered", aid));
                    return resp(409, json!({"ok": false, "error": "game is not registered as running"}));
                };

                if !is_metalsharp_owned_process(target_pid) {
                    app_log(&format!(
                        "[STOP REJECTED] appid {} pid {} is not a MetalSharp-owned process",
                        aid, target_pid
                    ));
                    return resp(
                        403,
                        json!({"ok": false, "error": "registered process is not a MetalSharp-owned target"}),
                    );
                }

                match launch::kill_game_with_pid(aid, target_pid) {
                    Ok(_) => {
                        unregister_game_pid(aid);
                        app_log(&format!("[STOPPED] appid {} | pid {}", aid, target_pid));
                        resp(200, json!({"ok": true, "pid": target_pid}))
                    },
                    Err(error) => {
                        app_log(&format!("[STOP FAILED] appid {} | error: {}", aid, error));
                        app_issue_log("stop", &aid.to_string(), &error.to_string(), &[]);
                        resp(500, json!({"ok": false, "error": error.to_string()}))
                    },
                }
            } else {
                let Some(target_pid) =
                    body.get("pid").and_then(|value| value.as_u64()).and_then(|value| i32::try_from(value).ok())
                else {
                    return resp(400, json!({"ok": false, "error": "pid required"}));
                };
                if target_pid <= 0 {
                    return resp(400, json!({"ok": false, "error": "pid required"}));
                }

                // PID-only calls are retained only for registered game roots.
                // In particular, a browser cannot turn an arbitrary PID into a
                // kill target by posting to this legacy endpoint.
                prune_inactive_game_pids();
                let Some(aid) = get_game_appid_for_pid(target_pid) else {
                    app_log(&format!("[STOP REJECTED] pid {} is not a registered game", target_pid));
                    return resp(403, json!({"ok": false, "error": "pid is not a registered MetalSharp game"}));
                };
                if !is_metalsharp_owned_process(target_pid) {
                    app_log(&format!(
                        "[STOP REJECTED] appid {} pid {} is not a MetalSharp-owned process",
                        aid, target_pid
                    ));
                    return resp(
                        403,
                        json!({"ok": false, "error": "registered process is not a MetalSharp-owned target"}),
                    );
                }

                match launch::kill_process_tree(target_pid) {
                    Ok(_) => {
                        unregister_game_pid(aid);
                        app_log(&format!("[STOPPED] appid {} | pid {}", aid, target_pid));
                        resp(200, json!({"ok": true, "pid": target_pid}))
                    },
                    Err(error) => {
                        app_issue_log("stop", &target_pid.to_string(), &error.to_string(), &[]);
                        resp(500, json!({"ok": false, "error": error.to_string()}))
                    },
                }
            }
        },
        (Method::Post, "/steam/uninstall-game") => {
            let body = read_body_or_return!(req);
            let appid = match parse_request_appid(&body) {
                Ok(appid) => appid,
                Err(error) => return resp(400, json!({"ok": false, "error": error})),
            };
            if migrate::is_migrating() {
                return resp(
                    409,
                    json!({"ok": false, "error": "Migration is running. Wait for it to finish before uninstalling games."}),
                );
            }
            app_log(&format!("Uninstalling game: appid {}", appid));
            match steam::uninstall_game(appid) {
                Ok(r) => resp(200, r),
                Err(e) => resp(500, json!({"ok": false, "error": e.to_string()})),
            }
        },
        (Method::Post, "/cache/clear") => {
            let body = read_body_or_return!(req);
            let cache_type = body.get("type").and_then(|v| v.as_str()).unwrap_or("shader");
            let home = dirs::home_dir().unwrap_or_default();
            let target = cache_dir_for_type(&home, cache_type);
            let (total_bytes, file_count) = dir_stats(&target);
            if target.exists() {
                let _ = std::fs::remove_dir_all(&target);
                let _ = std::fs::create_dir_all(&target);
            }
            app_log(&format!("Cleared {} cache: {} files, {} bytes", cache_type, file_count, total_bytes));
            resp(
                200,
                json!({
                    "ok": true,
                    "cache_type": cache_type,
                    "files_removed": file_count,
                    "bytes_freed": total_bytes,
                }),
            )
        },
        (Method::Get, "/cache/size") => {
            let home = dirs::home_dir().unwrap_or_default();
            let shader_dir = cache_dir_for_type(&home, "shader");
            let pipeline_dir = cache_dir_for_type(&home, "pipeline");
            let _ = std::fs::create_dir_all(&shader_dir);
            let _ = std::fs::create_dir_all(&pipeline_dir);
            resp(
                200,
                json!({
                    "ok": true,
                    "shader_cache": cache_summary(&shader_dir),
                    "pipeline_cache": cache_summary(&pipeline_dir),
                }),
            )
        },
        (Method::Get, "/metalfx/state") => resp(200, metalfx::get_state()),
        (Method::Post, "/metalfx/toggle") => {
            let body = read_body_or_return!(req);
            resp(200, metalfx::set_state(&body))
        },
        _ => resp(404, json!({"ok": false, "error": "not found"})),
    }
}

fn resp(code: u16, body: serde_json::Value) -> RouteResponse {
    RouteResponse::Json(code, body.to_string().into_bytes())
}

fn query_param(url: &str, key: &str) -> Option<String> {
    let query = url.split('?').nth(1)?;
    for pair in query.split('&') {
        let mut it = pair.splitn(2, '=');
        if it.next() == Some(key) {
            return it.next().map(|v| v.to_string());
        }
    }
    None
}

fn fna_allowed_roots() -> Vec<std::path::PathBuf> {
    let mut roots = vec![crate::platform::metalsharp_home_dir()];
    roots.extend(
        crate::scan::macos_steam_library_paths()
            .into_iter()
            .chain(crate::scan::wine_steam_library_paths())
            .map(|steamapps| steamapps.join("common")),
    );
    roots
}

fn resolve_existing_dir_under_roots(
    path: &std::path::Path,
    roots: &[std::path::PathBuf],
) -> Result<std::path::PathBuf, String> {
    let resolved = path.canonicalize().map_err(|_| "gameDir is unavailable".to_string())?;
    if !resolved.is_dir() {
        return Err("gameDir is not a directory".into());
    }

    if roots.iter().filter_map(|root| root.canonicalize().ok()).any(|root| resolved.starts_with(&root)) {
        Ok(resolved)
    } else {
        Err("gameDir must be under the MetalSharp home or a Steam library".into())
    }
}

fn resolve_fna_game_dir(raw: &str) -> Result<std::path::PathBuf, String> {
    if raw.trim().is_empty() {
        return Err("gameDir is required".into());
    }
    resolve_existing_dir_under_roots(std::path::Path::new(raw), &fna_allowed_roots())
}

fn force_kill_metalsharp_processes() -> Value {
    let this_pid = std::process::id();
    let home = crate::platform::metalsharp_home_dir();
    let targets: Vec<(u32, String)> = process_lines()
        .into_iter()
        .filter_map(|line| parse_process_line_owned(&line))
        .filter(|(pid, command)| *pid != this_pid && is_force_kill_target(command, &home))
        .collect();

    let mut terminated = Vec::new();
    let mut errors = Vec::new();
    for (pid, command) in &targets {
        match std::process::Command::new("/bin/kill").arg("-TERM").arg(pid.to_string()).status() {
            Ok(status) if status.success() => terminated.push(json!({"pid": pid, "command": command})),
            Ok(status) => {
                errors.push(json!({"pid": pid, "signal": "TERM", "status": status.code(), "command": command}))
            },
            Err(error) => {
                errors.push(json!({"pid": pid, "signal": "TERM", "error": error.to_string(), "command": command}))
            },
        }
    }

    std::thread::sleep(std::time::Duration::from_millis(350));

    let survivors: Vec<(u32, String)> = process_lines()
        .into_iter()
        .filter_map(|line| parse_process_line_owned(&line))
        .filter(|(pid, command)| {
            targets.iter().any(|(target_pid, _)| target_pid == pid) && is_force_kill_target(command, &home)
        })
        .collect();

    let mut killed = Vec::new();
    for (pid, command) in &survivors {
        match std::process::Command::new("/bin/kill").arg("-KILL").arg(pid.to_string()).status() {
            Ok(status) if status.success() => killed.push(json!({"pid": pid, "command": command})),
            Ok(status) => {
                errors.push(json!({"pid": pid, "signal": "KILL", "status": status.code(), "command": command}))
            },
            Err(error) => {
                errors.push(json!({"pid": pid, "signal": "KILL", "error": error.to_string(), "command": command}))
            },
        }
    }

    std::thread::sleep(std::time::Duration::from_millis(100));
    let survivors: Vec<serde_json::Value> = process_lines()
        .into_iter()
        .filter_map(|line| parse_process_line_owned(&line))
        .filter(|(pid, command)| *pid != this_pid && is_force_kill_target(command, &home))
        .map(|(pid, command)| json!({"pid": pid, "command": command}))
        .collect();

    app_log(&format!(
        "Force killed MetalSharp processes: {} TERM, {} KILL, {} survivors, {} errors",
        terminated.len(),
        killed.len(),
        survivors.len(),
        errors.len()
    ));

    json!({
        "ok": survivors.is_empty(),
        "terminated": terminated,
        "killed": killed,
        "survivors": survivors,
        "errors": errors,
        "backendPid": this_pid,
    })
}

fn process_lines() -> Vec<String> {
    std::process::Command::new("/bin/ps")
        .args(["axo", "pid=,command="])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .map(|output| output.lines().map(str::to_string).collect())
        .unwrap_or_default()
}

fn process_command_from_lines(lines: &[String], pid: i32) -> Option<String> {
    if pid <= 0 {
        return None;
    }
    lines
        .iter()
        .filter_map(|line| parse_process_line_owned(line))
        .find_map(|(candidate_pid, command)| (candidate_pid == pid as u32).then_some(command))
}

fn process_command_for_pid(pid: i32) -> Option<String> {
    process_command_from_lines(&process_lines(), pid)
}

fn parse_process_line_owned(line: &str) -> Option<(u32, String)> {
    let line = line.trim_start();
    let mut parts = line.splitn(2, char::is_whitespace);
    let pid = parts.next()?.parse::<u32>().ok()?;
    let command = parts.next().unwrap_or("").trim_start().to_string();
    Some((pid, command))
}

const METALSHARP_WINE_EXECUTABLES: &[&str] = &[
    "metalsharp-wine",
    "wine",
    "wine64",
    "wine32",
    "wineserver",
    "wineboot",
    "wineboot.exe",
    "wineloader",
    "wine-preloader",
    "wine64-preloader",
    "wine32-preloader",
    "winedevice",
    "winedevice.exe",
    "winedbg",
];

const METALSHARP_RUNTIME_HELPERS: &[&str] = &["gogdl", "heroic_gogdl", "heroic-gogdl"];

fn command_executable(command: &str) -> &str {
    let command = command.trim_start();
    for quote in ['"', '\''] {
        if let Some(quoted) = command.strip_prefix(quote) {
            return quoted.split(quote).next().unwrap_or("");
        }
    }
    command.split_whitespace().next().unwrap_or("")
}

fn is_metalsharp_wine_executable(command: &str, home: &std::path::Path) -> bool {
    let executable = std::path::Path::new(command_executable(command));
    let runtime_bin = home.join("runtime").join("wine").join("bin");
    if !executable.starts_with(&runtime_bin) {
        return false;
    }

    let name = executable.file_name().and_then(|value| value.to_str()).map(|value| value.to_ascii_lowercase());
    name.as_deref().is_some_and(|name| METALSHARP_WINE_EXECUTABLES.contains(&name))
}

fn is_metalsharp_runtime_helper(command: &str, home: &std::path::Path) -> bool {
    let executable = std::path::Path::new(command_executable(command));
    let runtime_root = home.join("runtime");
    let tools_root = home.join("tools");
    let owned_helper_path =
        executable.parent() == Some(runtime_root.as_path()) || executable.parent() == Some(tools_root.as_path());
    if !owned_helper_path {
        return false;
    }

    let name = executable.file_name().and_then(|value| value.to_str()).map(|value| value.to_ascii_lowercase());
    name.as_deref().is_some_and(|name| METALSHARP_RUNTIME_HELPERS.contains(&name))
}

fn is_metalsharp_owned_prefix_executable(command: &str, home: &std::path::Path) -> bool {
    let executable = std::path::Path::new(command_executable(command));
    let Ok(relative) = executable.strip_prefix(home) else {
        return false;
    };
    let Some(root) = relative.components().next() else {
        return false;
    };
    let root = root.as_os_str().to_string_lossy();

    root == "bottles"
        || root == "games"
        || root == "prefix-gptk"
        || root == "prefix-steam"
        || root == "sharp-prefix"
        || root.starts_with("prefix-")
}

fn is_metalsharp_game_command(command: &str, home: &std::path::Path) -> bool {
    if is_force_kill_target(command, home) {
        return true;
    }

    // D3DMetal's optional GPTK lane uses Apple's Wine binary rather than the
    // MetalSharp Wine wrapper. It is still safe here because this matcher is
    // only used after the PID has been registered for a Steam appid.
    let lower = command.to_ascii_lowercase();
    lower.contains("game porting toolkit.app") && lower.contains(".exe")
}

fn is_metalsharp_owned_process(pid: i32) -> bool {
    let home = crate::platform::metalsharp_home_dir();
    process_command_for_pid(pid).map(|command| is_metalsharp_game_command(&command, &home)).unwrap_or(false)
}

fn is_force_kill_target(command: &str, home: &std::path::Path) -> bool {
    if command.is_empty() {
        return false;
    }
    let lower = command.to_lowercase();
    if lower.contains("metalsharp-backend")
        || lower.contains("/contents/macos/metalsharp")
        || lower.contains("/electron.app/contents/macos/electron")
        || lower.contains(" ps axo")
        || lower.contains(" rg ")
        || lower.contains("grep ")
    {
        return false;
    }

    // `ps` returns the executable followed by its arguments. Only the
    // executable may establish ownership; paths in arguments are untrusted.
    // This prevents commands such as `open <home>/runtime/.../some.exe` or
    // `vim <home>/games/.../game.exe` from becoming kill targets merely by
    // mentioning a MetalSharp path or a `.exe` suffix.
    is_metalsharp_wine_executable(command, home)
        || is_metalsharp_runtime_helper(command, home)
        || is_metalsharp_owned_prefix_executable(command, home)
}

/// Minimal percent-decoding for URL query values (e.g. gameDir paths with
/// spaces). Handles %20 and the common %2F. Good enough for diagnostic
/// query params without pulling in a URL crate.
fn url_decode(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() {
            let hi = hex_val(bytes[i + 1]);
            let lo = hex_val(bytes[i + 2]);
            if let (Some(h), Some(l)) = (hi, lo) {
                out.push((h << 4) | l);
                i += 3;
                continue;
            }
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

fn resp_raw(code: u16, data: Vec<u8>, mime: &str) -> RouteResponse {
    RouteResponse::Raw(code, data, mime.to_string())
}

fn app_log(msg: &str) {
    let log_dir = logs_dir();
    let _ = std::fs::create_dir_all(&log_dir);
    let now = chrono_now();
    let line = format!("[{}] {}\n", now, msg);
    let log_path = log_dir.join(format!("{}.log", chrono_date()));
    let _ = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
}

fn app_issue_log(kind: &str, subject: &str, summary: &str, details: &[String]) {
    let log_dir = logs_dir();
    let _ = std::fs::create_dir_all(&log_dir);
    let sequence = ISSUE_LOG_COUNTER.fetch_add(1, Ordering::Relaxed);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or_default();
    let file_name = format!(
        "issue-{}-{}-{:09}-{}-{}-{}.log",
        chrono_file_stamp(),
        std::process::id(),
        nanos,
        sequence,
        slugify(kind),
        slugify(subject),
    );
    let path = log_dir.join(file_name);
    let mut body = vec![
        format!("timestamp: {}", chrono_now()),
        format!("kind: {}", kind),
        format!("subject: {}", subject),
        format!("summary: {}", summary),
        String::new(),
    ];
    body.extend(details.iter().cloned());
    let _ = std::fs::write(&path, body.join("\n"));
    app_log(&format!("[ISSUE] {} | {} | {}", kind, subject, summary));
}

fn logs_dir() -> std::path::PathBuf {
    crate::platform::metalsharp_home_dir().join("logs")
}

fn crash_reports_log_dir() -> std::path::PathBuf {
    logs_dir().join("crash-reports")
}

fn chrono_now() -> String {
    local_date(&["+%Y-%m-%d %H:%M:%S %Z"]).unwrap_or_else(|| {
        let d = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
        format!("{}", d.as_secs())
    })
}

fn chrono_date() -> String {
    local_date(&["+%Y-%m-%d"]).unwrap_or_else(|| {
        let d = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
        format!("{}", d.as_secs() / 86400)
    })
}

fn chrono_file_stamp() -> String {
    local_date(&["+%Y-%m-%d_%H-%M-%S"]).unwrap_or_else(|| {
        let d = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
        d.as_secs().to_string()
    })
}

fn slugify(value: &str) -> String {
    let slug: String =
        value.chars().map(|ch| if ch.is_ascii_alphanumeric() { ch.to_ascii_lowercase() } else { '-' }).collect();
    let trimmed = slug.trim_matches('-');
    if trimmed.is_empty() {
        "unknown".into()
    } else {
        trimmed.chars().take(80).collect()
    }
}

fn local_date(args: &[&str]) -> Option<String> {
    let output = std::process::Command::new("/bin/date").args(args).output().ok()?;
    if !output.status.success() {
        return None;
    }
    Some(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn local_date_for_epoch(secs: u64) -> String {
    local_date(&["-r", &secs.to_string(), "+%Y-%m-%d %H:%M:%S %Z"]).unwrap_or_else(|| secs.to_string())
}

fn cache_dir_for_type(home: &std::path::Path, cache_type: &str) -> std::path::PathBuf {
    match cache_type {
        "pipeline" => crate::platform::metalsharp_home_dir_for(&home).join("pipeline-cache"),
        _ => crate::platform::metalsharp_home_dir_for(&home).join("shader-cache"),
    }
}

fn dir_stats(path: &std::path::Path) -> (u64, u64) {
    let mut bytes = 0;
    let mut files = 0;
    if !path.exists() {
        return (bytes, files);
    }
    for entry in walkdir::WalkDir::new(path).into_iter().flatten() {
        if let Ok(meta) = entry.metadata() {
            if meta.is_file() {
                bytes += meta.len();
                files += 1;
            }
        }
    }
    (bytes, files)
}

fn cache_summary(path: &std::path::Path) -> serde_json::Value {
    let (bytes, files) = dir_stats(path);
    let mut directories = 0u64;
    let mut app_dirs = 0u64;
    let mut newest_modified = 0u64;

    if path.exists() {
        for entry in walkdir::WalkDir::new(path).min_depth(1).into_iter().flatten() {
            if let Ok(meta) = entry.metadata() {
                if meta.is_dir() {
                    directories += 1;
                    if entry.depth() == 2 && entry.file_name().to_string_lossy().chars().all(|c| c.is_ascii_digit()) {
                        app_dirs += 1;
                    }
                }
                if let Ok(modified) = meta.modified() {
                    let secs = modified.duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs();
                    newest_modified = newest_modified.max(secs);
                }
            }
        }
    }

    let status = if !path.exists() {
        "missing"
    } else if files == 0 {
        "empty"
    } else {
        "active"
    };

    json!({
        "bytes": bytes,
        "files": files,
        "directories": directories,
        "apps": app_dirs,
        "path": path.to_string_lossy(),
        "status": status,
        "last_modified": if newest_modified > 0 { json!(local_date_for_epoch(newest_modified)) } else { json!(null) },
    })
}

fn resolve_game_exe(appid: u32) -> String {
    let home = dirs::home_dir().unwrap_or_default();
    let game_dir = crate::platform::metalsharp_home_dir_for(&home).join("games").join(appid.to_string());

    for entry in walkdir::WalkDir::new(&game_dir).max_depth(3).into_iter().flatten() {
        if let Some(ext) = entry.path().extension() {
            if ext == "exe" {
                let name = entry.file_name().to_string_lossy().to_string();
                let name_lower = name.to_lowercase();
                if name_lower.starts_with("terraria") && !name_lower.contains("server")
                    || name_lower.starts_with("hl2") && !name_lower.contains("launcher")
                    || !name_lower.contains("setup")
                        && !name_lower.contains("redist")
                        && !name_lower.contains("dotnet")
                        && !name_lower.contains("installer")
                        && !name_lower.contains("uninstall")
                        && !name_lower.contains("vcredist")
                        && !name_lower.contains("crashhandler")
                {
                    return entry.path().to_string_lossy().to_string();
                }
            }
        }
    }

    for entry in walkdir::WalkDir::new(&game_dir).max_depth(3).into_iter().flatten() {
        if let Some(ext) = entry.path().extension() {
            if ext == "exe" {
                let name = entry.file_name().to_string_lossy().to_lowercase();
                if !name.contains("setup")
                    && !name.contains("redist")
                    && !name.contains("dotnet")
                    && !name.contains("installer")
                    && !name.contains("uninstall")
                    && !name.contains("vcredist")
                    && !name.contains("server")
                    && !name.contains("crashhandler")
                {
                    return entry.path().to_string_lossy().to_string();
                }
            }
        }
    }

    game_dir.to_string_lossy().to_string()
}

fn read_body(req: &mut tiny_http::Request) -> Result<serde_json::Map<String, serde_json::Value>, RequestBodyError> {
    if req.body_length().map(|length| length > MAX_REQUEST_BODY_BYTES).unwrap_or(false) {
        return Err(RequestBodyError::TooLarge);
    }

    read_body_from_reader(req.as_reader())
}

fn read_body_from_reader<R: Read>(reader: R) -> Result<serde_json::Map<String, serde_json::Value>, RequestBodyError> {
    let mut buf = Vec::new();
    reader.take((MAX_REQUEST_BODY_BYTES as u64) + 1).read_to_end(&mut buf).map_err(RequestBodyError::Read)?;

    if buf.len() > MAX_REQUEST_BODY_BYTES {
        return Err(RequestBodyError::TooLarge);
    }

    serde_json::from_slice::<serde_json::Map<String, serde_json::Value>>(&buf).map_err(RequestBodyError::InvalidJson)
}

fn parse_request_u32_value(
    value: Option<&serde_json::Value>,
    missing_error: &'static str,
    invalid_error: &'static str,
    range_error: &'static str,
    zero_error: &'static str,
) -> Result<u32, &'static str> {
    let Some(value) = value else {
        return Err(missing_error);
    };
    let Some(raw) = value.as_u64() else {
        return Err(invalid_error);
    };
    let appid = u32::try_from(raw).map_err(|_| range_error)?;
    if appid == 0 {
        return Err(zero_error);
    }
    Ok(appid)
}

fn parse_request_appid(body: &serde_json::Map<String, serde_json::Value>) -> Result<u32, &'static str> {
    parse_request_u32_value(
        body.get("appid"),
        "appid required",
        "appid must be a positive numeric Steam appid",
        "appid out of range",
        "appid must be greater than zero",
    )
}

fn parse_optional_request_steam_appid(
    body: &serde_json::Map<String, serde_json::Value>,
) -> Result<Option<u32>, &'static str> {
    body.get("steamAppId")
        .map(|value| {
            parse_request_u32_value(
                Some(value),
                "steamAppId required",
                "steamAppId must be a positive numeric Steam appid",
                "steamAppId out of range",
                "steamAppId must be greater than zero",
            )
        })
        .transpose()
}

fn pipeline_label_for(pipeline: crate::mtsp::engine::PipelineId) -> &'static str {
    match pipeline {
        crate::mtsp::engine::PipelineId::M12 => "M12",
        crate::mtsp::engine::PipelineId::M11 => "M11",
        crate::mtsp::engine::PipelineId::M9 => "M9",
        crate::mtsp::engine::PipelineId::FnaArm64 => "FNA/Mono",
        _ => "Other",
    }
}

fn collect_crash_reports(ms_home: &std::path::Path) -> Vec<serde_json::Value> {
    let mut reports = Vec::new();

    let game_base = ms_home.join("games");
    if let Ok(rd) = std::fs::read_dir(&game_base) {
        for entry in rd.flatten() {
            if entry.path().is_dir() {
                let appid_str = entry.file_name().to_string_lossy().to_string();
                let appid: u32 = appid_str.parse().unwrap_or(0);
                let pipeline = if appid > 0 {
                    crate::bottles::resolve_steam_pipeline_for_request(appid, None)
                } else {
                    crate::mtsp::engine::PipelineId::M11
                };
                let pipeline_label = pipeline_label_for(pipeline);
                scan_crash_files(&entry.path(), &appid_str, pipeline_label, &mut reports, 0);
            }
        }
    }

    let bottles_dir = ms_home.join("bottles");
    if let Ok(rd) = std::fs::read_dir(&bottles_dir) {
        for entry in rd.flatten() {
            let bottle_id = entry.file_name().to_string_lossy().to_string();
            let logs_dir = entry.path().join("logs");
            if !logs_dir.is_dir() {
                continue;
            }
            let appid: u32 = bottle_id.strip_prefix("steam_").and_then(|s| s.parse().ok()).unwrap_or(0);
            let pipeline = if appid > 0 {
                crate::bottles::resolve_steam_pipeline_for_request(appid, None)
            } else {
                crate::mtsp::engine::PipelineId::M11
            };
            let pipeline_label = pipeline_label_for(pipeline);
            scan_crash_files(&logs_dir, &bottle_id, pipeline_label, &mut reports, 0);
        }
    }

    let steam_dumps =
        ms_home.join("prefix-steam").join("drive_c").join("Program Files (x86)").join("Steam").join("dumps");
    if steam_dumps.is_dir() {
        scan_steam_dumps(&steam_dumps, &mut reports);
    }

    let prefix = ms_home.join("prefix-steam").join("drive_c");
    for crash_dir in [
        prefix.join("users").join("steamuser").join("AppData").join("Local").join("CrashDumps"),
        prefix.join("ProgramData").join("CrashDumps"),
    ] {
        if crash_dir.is_dir() {
            scan_crash_files(&crash_dir, "system", "System", &mut reports, 0);
        }
    }

    reports.sort_by(|a: &serde_json::Value, b: &serde_json::Value| {
        b.get("timestamp")
            .and_then(|value| value.as_str())
            .unwrap_or("")
            .cmp(a.get("timestamp").and_then(|value| value.as_str()).unwrap_or(""))
    });
    reports
}

fn crash_report_is_enumerated(reports: &[serde_json::Value], requested_file: &str) -> bool {
    reports
        .iter()
        .filter_map(|report| report.get("file").and_then(|value| value.as_str()))
        .any(|file| file == requested_file)
}

fn pipeline_label_for_exe(exe_name: &str) -> &'static str {
    let exe_lower = exe_name.to_lowercase();
    let home = dirs::home_dir().unwrap_or_default();
    let ms_home = crate::platform::metalsharp_home_dir_for(&home);
    if let Ok(rd) = std::fs::read_dir(ms_home.join("bottles")) {
        for entry in rd.flatten() {
            let bottle_id = entry.file_name().to_string_lossy().to_string();
            let appid: u32 = match bottle_id.strip_prefix("steam_").and_then(|s| s.parse().ok()) {
                Some(id) => id,
                None => continue,
            };
            let manifest_path = entry.path().join("bottle.json");
            let manifest: serde_json::Value =
                match std::fs::read_to_string(&manifest_path).ok().and_then(|s| serde_json::from_str(&s).ok()) {
                    Some(v) => v,
                    None => continue,
                };
            if let Some(name) = manifest.get("game_name").and_then(|v| v.as_str()) {
                let name_lower = name.to_lowercase().replace(' ', "");
                let exe_base = exe_lower.trim_end_matches(".exe").replace(' ', "");
                if exe_base.contains(&name_lower) || name_lower.contains(&exe_base) {
                    let pipeline = crate::bottles::resolve_steam_pipeline_for_request(appid, None);
                    return pipeline_label_for(pipeline);
                }
            }
        }
    }
    "System"
}

fn scan_steam_dumps(dir: &std::path::Path, reports: &mut Vec<serde_json::Value>) {
    if let Ok(rd) = std::fs::read_dir(dir) {
        for entry in rd.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_string();
            let name_lower = name.to_lowercase();
            let is_dump = name_lower.ends_with(".dmp") || name_lower.ends_with(".mdmp") || name_lower.contains("crash");
            if is_dump {
                let metadata = std::fs::metadata(&path).ok();
                let size = metadata.as_ref().map(|m| m.len()).unwrap_or(0);
                let modified = metadata.and_then(|m| m.modified().ok());
                let timestamp = modified
                    .map(|t| {
                        let d = t.duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
                        local_date_for_epoch(d.as_secs())
                    })
                    .unwrap_or_else(|| "unknown".into());

                let exe_name = name.split('_').nth(1).unwrap_or("").to_string();
                let pipeline = if exe_name.is_empty() { "System" } else { pipeline_label_for_exe(&exe_name) };

                reports.push(json!({
                    "file": path.to_string_lossy(),
                    "name": name,
                    "source": "steam-dumps",
                    "pipeline": pipeline,
                    "timestamp": timestamp,
                    "size_bytes": size,
                }));
            }
        }
    }
}

fn scan_crash_files(
    dir: &std::path::Path,
    source: &str,
    pipeline: &str,
    reports: &mut Vec<serde_json::Value>,
    depth: u32,
) {
    if depth > 2 {
        return;
    }
    let crash_patterns = ["crash", ".dmp", ".mdmp", "crashdump", "crash_report"];
    if let Ok(rd) = std::fs::read_dir(dir) {
        for entry in rd.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().to_lowercase();
            let is_crash = crash_patterns.iter().any(|p| name.contains(p));
            if is_crash {
                let metadata = std::fs::metadata(&path).ok();
                let size = metadata.as_ref().map(|m| m.len()).unwrap_or(0);
                let modified = metadata.and_then(|m| m.modified().ok());
                let timestamp = modified
                    .map(|t| {
                        let d = t.duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
                        local_date_for_epoch(d.as_secs())
                    })
                    .unwrap_or_else(|| "unknown".into());
                reports.push(json!({
                    "file": path.to_string_lossy(),
                    "name": entry.file_name().to_string_lossy().to_string(),
                    "source": source,
                    "pipeline": pipeline,
                    "timestamp": timestamp,
                    "size_bytes": size,
                }));
                persist_crash_log(source, &path, &timestamp, size);
            }
            if path.is_dir() {
                let sub_source = source.to_string();
                scan_crash_files(&path, &sub_source, pipeline, reports, depth + 1);
            }
        }
    }
}

/// Open a path (launch log, report) in the default macOS viewer. The path
/// must resolve under the MetalSharp home so the local API cannot open
/// arbitrary files.
fn open_path_under_home(ms_home: &std::path::Path, file: &std::path::Path) -> Result<(), String> {
    let home = ms_home.canonicalize().map_err(|error| format!("MetalSharp home unavailable: {error}"))?;
    let file = file.canonicalize().map_err(|error| format!("File unavailable: {error}"))?;
    if !file.starts_with(&home) || !file.is_file() {
        return Err("Path is outside the MetalSharp runtime".into());
    }
    let status = std::process::Command::new("open")
        .arg(&file)
        .status()
        .map_err(|error| format!("Failed to launch open(1): {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("open(1) exited with {}", status.code().unwrap_or(-1)))
    }
}

fn crash_report_preview(ms_home: &std::path::Path, file: &std::path::Path) -> Result<Vec<String>, String> {
    let home = ms_home.canonicalize().map_err(|error| format!("MetalSharp home unavailable: {error}"))?;
    let file = file.canonicalize().map_err(|error| format!("Crash report unavailable: {error}"))?;
    if !file.starts_with(&home) || !file.is_file() {
        return Err("Crash report path is outside the MetalSharp runtime".into());
    }

    let mut bytes = Vec::new();
    std::fs::File::open(&file)
        .map_err(|error| format!("Unable to open crash report: {error}"))?
        .take(256 * 1024)
        .read_to_end(&mut bytes)
        .map_err(|error| format!("Unable to read crash report: {error}"))?;
    Ok(crash_preview_lines(&bytes))
}

fn crash_preview_lines(bytes: &[u8]) -> Vec<String> {
    if let Ok(text) = std::str::from_utf8(bytes) {
        let lines: Vec<String> = text.lines().take(200).map(str::to_owned).collect();
        if !lines.is_empty() {
            return lines;
        }
    }

    let mut lines = Vec::new();
    let mut current = Vec::new();
    for &byte in bytes {
        if byte.is_ascii_graphic() || byte == b' ' || byte == b'\t' {
            current.push(byte);
        } else {
            if current.len() >= 4 {
                lines.push(String::from_utf8_lossy(&current).trim().to_string());
                if lines.len() == 200 {
                    break;
                }
            }
            current.clear();
        }
    }
    if lines.len() < 200 && current.len() >= 4 {
        lines.push(String::from_utf8_lossy(&current).trim().to_string());
    }
    lines.retain(|line| !line.is_empty());
    if lines.is_empty() {
        lines.push("No readable text was found in this binary crash report.".into());
    }
    lines
}

fn persist_crash_log(source: &str, path: &std::path::Path, timestamp: &str, size: u64) {
    let log_dir = crash_reports_log_dir();
    let _ = std::fs::create_dir_all(&log_dir);
    let name = path.file_name().unwrap_or_default().to_string_lossy();
    let file_name = format!("crash-{}-{}-{}.log", slugify(source), slugify(&name), slugify(timestamp));
    let log_path = log_dir.join(file_name);
    if log_path.exists() {
        return;
    }
    let body = [
        format!("timestamp: {}", chrono_now()),
        format!("crash_timestamp: {}", timestamp),
        format!("source: {}", source),
        format!("file: {}", path.to_string_lossy()),
        format!("size_bytes: {}", size),
    ]
    .join("\n");
    let _ = std::fs::write(log_path, body);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response_json(response: RouteResponse) -> (u16, serde_json::Value) {
        match response {
            RouteResponse::Json(code, body) => (code, serde_json::from_slice(&body).expect("JSON response")),
            RouteResponse::Raw(_, _, _) => panic!("expected JSON response"),
        }
    }

    #[test]
    fn request_body_accepts_a_valid_json_object() {
        let body = read_body_from_reader(std::io::Cursor::new(br#"{"appid":620}"#)).expect("valid JSON body");

        assert_eq!(body.get("appid").and_then(Value::as_u64), Some(620));
    }

    #[test]
    fn request_body_parse_failures_return_bad_request() {
        let error = read_body_from_reader(std::io::Cursor::new(br#"{"appid":}"#)).expect_err("invalid JSON body");
        let (status, payload) = response_json(request_body_error_response(error));

        assert_eq!(status, 400);
        assert_eq!(payload.get("ok"), Some(&Value::Bool(false)));
        assert!(payload
            .get("error")
            .and_then(Value::as_str)
            .is_some_and(|message| message.starts_with("invalid JSON request body:")));
    }

    #[test]
    fn route_propagates_malformed_json_before_handler_validation() {
        let mut request: tiny_http::Request = tiny_http::TestRequest::new()
            .with_method(Method::Post)
            .with_path("/game/resolve-routing")
            .with_body("{")
            .into();
        let (status, payload) = response_json(route(&mut request));

        assert_eq!(status, 400);
        assert!(payload
            .get("error")
            .and_then(Value::as_str)
            .is_some_and(|message| message.starts_with("invalid JSON request body:")));
    }

    #[test]
    fn request_body_rejects_non_object_json() {
        let error =
            read_body_from_reader(std::io::Cursor::new(br#"[]"#)).expect_err("JSON arrays are not request objects");
        let (status, payload) = response_json(request_body_error_response(error));

        assert_eq!(status, 400);
        assert_eq!(payload.get("ok"), Some(&Value::Bool(false)));
    }

    #[test]
    fn request_body_limit_returns_payload_too_large() {
        let error = read_body_from_reader(std::io::repeat(b'x')).expect_err("unbounded body must be rejected");
        assert!(matches!(&error, RequestBodyError::TooLarge));

        let (status, payload) = response_json(request_body_error_response(error));
        assert_eq!(status, 413);
        assert_eq!(payload.get("ok"), Some(&Value::Bool(false)));
    }

    #[test]
    fn request_body_rejects_known_oversized_content_length_before_reading() {
        let content_length = (MAX_REQUEST_BODY_BYTES + 1).to_string();
        let header =
            Header::from_bytes(&b"Content-Length"[..], content_length.as_bytes()).expect("content length header");
        let mut request: tiny_http::Request =
            tiny_http::TestRequest::new().with_method(Method::Post).with_body("{}").with_header(header).into();

        assert!(matches!(read_body(&mut request), Err(RequestBodyError::TooLarge)));
    }

    #[test]
    fn local_origin_guard_accepts_only_pinned_vite_origins() {
        // Browser access is limited to the actual Vite development server.
        assert!(is_trusted_local_origin("http://localhost:5173"));
        assert!(is_trusted_local_origin("http://127.0.0.1:5173"));

        // Rejected: opaque/file origins, arbitrary localhost ports, and host tricks.
        assert!(!is_trusted_local_origin("file://"));
        assert!(!is_trusted_local_origin("null"));
        assert!(!is_trusted_local_origin("http://localhost:9274"));
        assert!(!is_trusted_local_origin("http://localhost:5174"));
        assert!(!is_trusted_local_origin("https://evil.example"));
        assert!(!is_trusted_local_origin("http://evil.example"));
        assert!(!is_trusted_local_origin("http://localhost.evil.example"));
        assert!(!is_trusted_local_origin("http://localhost:5173.evil.example"));
        assert!(!is_trusted_local_origin("http://localhost:5173/path"));
        assert!(!is_trusted_local_origin("https://localhost:5173"));
    }

    #[test]
    fn api_requires_the_current_session_bearer_token() {
        let token = "0123456789abcdef0123456789abcdef";
        assert!(is_authorized_bearer(Some("Bearer 0123456789abcdef0123456789abcdef"), Some(token)));
        assert!(!is_authorized_bearer(None, Some(token)));
        assert!(!is_authorized_bearer(Some("Bearer wrong"), Some(token)));
        assert!(!is_authorized_bearer(Some("Basic 0123456789abcdef0123456789abcdef"), Some(token)));
        assert!(!is_authorized_bearer(Some("bearer 0123456789abcdef0123456789abcdef"), Some(token)));
        assert!(!is_authorized_bearer(Some("Bearer 0123456789abcdef0123456789abcdef"), None));
    }

    #[test]
    fn only_read_only_health_check_is_public() {
        assert!(is_public_health_request(&Method::Get, "/health"));
        assert!(is_public_health_request(&Method::Get, "/health?probe=1"));
        assert!(!is_public_health_request(&Method::Get, "/status"));
        assert!(!is_public_health_request(&Method::Post, "/health"));
    }

    #[test]
    fn fna_game_dir_resolution_canonicalizes_and_enforces_allowed_roots() {
        let base = std::env::temp_dir().join(format!("metalsharp-fna-containment-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let metalsharp_root = base.join("metalsharp");
        let steam_root = base.join("steam-common");
        let outside = base.join("outside");
        let sibling = base.join("metalsharp-sibling");
        let metalsharp_game = metalsharp_root.join("games").join("fna-game");
        let steam_game = steam_root.join("fna-game");

        std::fs::create_dir_all(&metalsharp_game).unwrap();
        std::fs::create_dir_all(&steam_game).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::create_dir_all(sibling.join("fna-game")).unwrap();

        let roots = vec![metalsharp_root.clone(), steam_root.clone()];
        assert_eq!(
            resolve_existing_dir_under_roots(&metalsharp_game, &roots).unwrap(),
            metalsharp_game.canonicalize().unwrap()
        );
        assert!(resolve_existing_dir_under_roots(&steam_game, &roots).is_ok());
        assert!(resolve_existing_dir_under_roots(&outside, &roots).is_err());
        assert!(resolve_existing_dir_under_roots(&sibling.join("fna-game"), &roots).is_err());
        assert!(resolve_existing_dir_under_roots(&metalsharp_root.join("missing"), &roots).is_err());

        #[cfg(unix)]
        {
            let symlink = metalsharp_root.join("linked-outside");
            std::os::unix::fs::symlink(&outside, &symlink).unwrap();
            assert!(resolve_existing_dir_under_roots(&symlink, &roots).is_err());
        }

        let _ = std::fs::remove_dir_all(base);
    }

    #[test]
    fn active_game_targets_include_only_registered_positive_pids() {
        let targets = active_game_targets(&HashMap::from([(620, 4242), (4000, 0), (1260320, -1)]));

        assert_eq!(targets, vec![(620, 4242)]);
    }

    #[test]
    fn kill_authorization_never_falls_back_to_requested_pid() {
        let games = HashMap::from([(620, 4242), (4000, 0)]);

        assert_eq!(registered_game_pid(&games, 620), Some(4242));
        assert_eq!(registered_game_pid(&games, 4000), None);
        assert_eq!(registered_game_pid(&games, 730), None);
        assert_eq!(registered_game_appid_for_pid(&games, 4242), Some(620));
        assert_eq!(registered_game_appid_for_pid(&games, 9999), None);
    }

    #[test]
    fn process_command_lookup_is_limited_to_requested_pid() {
        let lines = vec![
            "  4242 /Users/test/.metalsharp/runtime/wine/bin/metalsharp-wine game.exe".to_string(),
            "  9999 /Applications/Steam.app/Contents/MacOS/steam_osx".to_string(),
        ];

        assert_eq!(
            process_command_from_lines(&lines, 4242),
            Some("/Users/test/.metalsharp/runtime/wine/bin/metalsharp-wine game.exe".to_string())
        );
        assert_eq!(process_command_from_lines(&lines, 1234), None);
    }

    #[test]
    fn game_process_ownership_allows_metalsharp_and_registered_gptk_commands_only() {
        let home = std::path::Path::new("/Users/test/.metalsharp");

        assert!(is_metalsharp_game_command("/Users/test/.metalsharp/runtime/wine/bin/metalsharp-wine game.exe", home));
        assert!(is_metalsharp_game_command(
            "/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin/wine64 game.exe",
            home
        ));
        assert!(!is_metalsharp_game_command(
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine64 game.exe",
            home
        ));
        assert!(!is_metalsharp_game_command("/Applications/Steam.app/Contents/MacOS/steam_osx", home));
    }

    #[test]
    fn force_kill_targets_owned_executables_but_not_path_arguments() {
        let home = std::env::temp_dir().join("metalsharp-force-kill-test");
        let runtime = |name: &str| home.join("runtime/wine/bin").join(name).to_string_lossy().into_owned();
        let game = || home.join("games/620/game.exe").to_string_lossy().into_owned();

        assert!(is_force_kill_target(&runtime("wineserver"), &home));
        assert!(is_force_kill_target(&format!(r#"{} C:\windows\system32\wineboot.exe"#, runtime("wine64")), &home));
        assert!(is_force_kill_target(&format!("{} Steam.exe", runtime("wine")), &home));
        assert!(is_force_kill_target(
            &home.join("prefix-steam/drive_c/Program Files (x86)/Steam/bin/steamwebhelper.exe").to_string_lossy(),
            &home
        ));
        assert!(is_force_kill_target(&game(), &home));
        assert!(is_force_kill_target(&format!("{} --version", home.join("tools/gogdl").display()), &home));
        assert!(!is_force_kill_target(&runtime("other.exe"), &home));
        assert!(!is_force_kill_target(&format!("{}-evil/runtime/wine/bin/wine64", home.display()), &home));

        // A path in an argument does not identify the executable that would
        // be terminated. These are the false positives from issue #404.
        assert!(!is_force_kill_target(&format!("vim {}.metalsharp-original", game()), &home));
        assert!(!is_force_kill_target(&format!("open {}", runtime("some.exe")), &home));
        assert!(!is_force_kill_target(&format!("/usr/local/bin/wine64 {}", game()), &home));
        assert!(!is_force_kill_target("/Applications/MetalSharp.app/Contents/MacOS/MetalSharp", &home));
        assert!(!is_force_kill_target("/Applications/Steam.app/Contents/MacOS/steam_osx", &home));
    }

    #[test]
    fn force_kill_never_targets_foreign_wine_processes() {
        // Isolation contract: a foreign Wine launcher (CrossOver, SakuraGiri,
        // Whisky, GPTK) must never be killed by MetalSharp cleanup, even when
        // its command line contains wine/wineserver/wineloader tokens.
        let home = std::env::temp_dir().join("metalsharp-force-kill-test");
        let runtime = |name: &str| home.join("runtime/wine/bin").join(name).to_string_lossy().into_owned();

        assert!(!is_force_kill_target(
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wineserver -f",
            &home
        ));
        assert!(!is_force_kill_target(
            "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine64 C:\\windows\\system32\\wineboot.exe",
            &home
        ));
        assert!(!is_force_kill_target(
            "/Applications/SakuraGiri.app/Contents/Resources/engine/wine/bin/wineloader",
            &home
        ));
        assert!(!is_force_kill_target("/opt/homebrew/bin/wineserver", &home));
        assert!(!is_force_kill_target("/usr/local/bin/wine", &home));
        assert!(!is_force_kill_target(
            &format!(
                "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine64 {}",
                home.join("games/620/game.exe").display()
            ),
            &home
        ));

        // MS-owned processes must still be targeted.
        assert!(is_force_kill_target(&format!("{} -f", runtime("wineserver")), &home));
        assert!(is_force_kill_target(&format!("{} Steam.exe", runtime("metalsharp-wine")), &home));
    }

    #[test]
    fn crash_preview_allowlist_rejects_unenumerated_metalsharp_files() {
        let reports = vec![json!({"file": "/tmp/.metalsharp/dumps/crash.dmp"})];

        assert!(crash_report_is_enumerated(&reports, "/tmp/.metalsharp/dumps/crash.dmp"));
        assert!(!crash_report_is_enumerated(&reports, "/tmp/.metalsharp/gog/auth.json"));
    }

    #[test]
    fn crash_preview_keeps_plain_text_lines() {
        assert_eq!(crash_preview_lines(b"first line\nsecond line\n"), ["first line", "second line"]);
    }

    #[test]
    fn crash_preview_extracts_readable_strings_from_binary_dumps() {
        let preview = crash_preview_lines(b"\0\x01crash reason\0\xffmodule.dll\0");
        assert_eq!(preview, ["crash reason", "module.dll"]);
    }

    #[test]
    fn request_appid_rejects_missing_string_zero_and_oversized_values() {
        let missing = serde_json::Map::new();
        assert_eq!(parse_request_appid(&missing), Err("appid required"));

        let mut string_appid = serde_json::Map::new();
        string_appid.insert("appid".into(), json!("620"));
        assert_eq!(parse_request_appid(&string_appid), Err("appid must be a positive numeric Steam appid"));

        let mut negative_appid = serde_json::Map::new();
        negative_appid.insert("appid".into(), json!(-1));
        assert_eq!(parse_request_appid(&negative_appid), Err("appid must be a positive numeric Steam appid"));

        let mut fractional_appid = serde_json::Map::new();
        fractional_appid.insert("appid".into(), json!(620.5));
        assert_eq!(parse_request_appid(&fractional_appid), Err("appid must be a positive numeric Steam appid"));

        let mut zero_appid = serde_json::Map::new();
        zero_appid.insert("appid".into(), json!(0));
        assert_eq!(parse_request_appid(&zero_appid), Err("appid must be greater than zero"));

        let mut oversized_appid = serde_json::Map::new();
        oversized_appid.insert("appid".into(), json!(u64::from(u32::MAX) + 1));
        assert_eq!(parse_request_appid(&oversized_appid), Err("appid out of range"));
    }

    #[test]
    fn request_appid_accepts_u32_range_values() {
        let mut body = serde_json::Map::new();
        body.insert("appid".into(), json!(620));

        assert_eq!(parse_request_appid(&body), Ok(620));

        body.insert("appid".into(), json!(u32::MAX));
        assert_eq!(parse_request_appid(&body), Ok(u32::MAX));
    }

    #[test]
    fn optional_steam_appid_rejects_out_of_range_values() {
        let mut body = serde_json::Map::new();
        assert_eq!(parse_optional_request_steam_appid(&body), Ok(None));

        body.insert("steamAppId".into(), json!(u64::from(u32::MAX) + 1));
        assert_eq!(parse_optional_request_steam_appid(&body), Err("steamAppId out of range"));
    }
}
