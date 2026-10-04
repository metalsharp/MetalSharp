//! Sharp Library installer tooling ported from `SharpView.vue`: app bottles and
//! the D3DMetal launch path, Launch Doctor and recent diagnostics, inline app /
//! GameJolt renames, and cover framing.
use super::*;
use crate::live::{self, error_text, is_ok};
use crate::toast;
use serde_json::{Map, Value, json};
use std::time::Duration;

const LONG: Duration = Duration::from_secs(10 * 60);

fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn b(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}

/// Inline rename in progress (Sharp app or GameJolt game).
pub(super) struct NameEdit {
    pub gamejolt: bool,
    pub id: String,
    pub input: gpui::Entity<crate::search_input::SearchInput>,
    pub saving: bool,
    _blur: Option<gpui::Subscription>,
}

/// `bottleBadgeLabel`.
pub(super) fn bottle_badge_label(bottle: &Value) -> &'static str {
    match s(bottle, "health") {
        "ready" => "Installed",
        "needs_repair" => "Bottle needs repair",
        "partial" => "Partial install",
        _ => "Not installed",
    }
}

/// `doctorActionLabel`.
pub(super) fn doctor_action_label(check: &Value, app: &Value) -> &'static str {
    let id = s(check, "id");
    if id == "runtime_assets" || id == "dll_sources" {
        return "Install runtime";
    }
    if id == "exe_route" {
        return if s(app, "engine") == "auto" {
            "Switch to Wine"
        } else {
            "Switch to Auto"
        };
    }
    if s(check, "detail").to_lowercase().contains("steam") {
        return "Restart Steam";
    }
    "Open logs"
}

/// `sharpAppExeAbsolute`.
fn app_exe_absolute(app: &Value) -> String {
    let exe = s(app, "exe_path");
    if exe.starts_with('/') {
        return exe.to_owned();
    }
    format!(
        "{}/{}",
        s(app, "install_dir").trim_end_matches('/'),
        exe.strip_prefix("./").unwrap_or(exe)
    )
}

/// `d3dmetalActionRoute`.
fn d3dmetal_action_route(id: &str) -> &'static str {
    match id {
        "install_homebrew_gptk" => "/d3dmetal/bottles/install-homebrew-gptk",
        "install_rosetta" => "/d3dmetal/bottles/install-rosetta",
        "repair_gptk_payload" => "/d3dmetal/bottles/repair-gptk-payload",
        "install_x64_redist" => "/d3dmetal/bottles/install-x64-redist",
        "seed_prefix" => "/d3dmetal/bottles/seed-prefix",
        _ => "/d3dmetal/bottles/play",
    }
}

fn steam_app_id(bottle: &Value) -> Option<u64> {
    bottle
        .get("steam_app_id")
        .and_then(Value::as_u64)
        .filter(|id| *id != 0)
}

/// main `app:open-in-finder`: only paths under the home directory.
pub(super) fn open_in_finder(path: &str) {
    let Some(home) = std::env::var_os("HOME").map(std::path::PathBuf::from) else {
        return;
    };
    let resolved = match path.strip_prefix('~') {
        Some(rest) => format!("{}{rest}", home.display()),
        None => path.to_owned(),
    };
    let full = std::path::PathBuf::from(&resolved);
    if !full.is_absolute() || !full.starts_with(&home) {
        return;
    }
    if !full.exists() {
        let _ = std::fs::create_dir_all(&full);
    }
    let _ = std::process::Command::new("/usr/bin/open")
        .arg(&full)
        .spawn();
}

impl SharpPreview {
    fn tools(&mut self) -> &mut super::sharp_live::SharpLive {
        self.live.as_mut().expect("live sharp state")
    }

    pub(super) fn bottle_for_app(&self, app: &Value) -> Option<Value> {
        let id = s(app, "bottle_id");
        if id.is_empty() {
            return None;
        }
        self.live
            .as_ref()?
            .bottles
            .iter()
            .find(|bottle| s(bottle, "id") == id)
            .cloned()
    }

    /// `load()` bottle half: `GET /bottles`.
    pub(super) fn load_bottles(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/bottles",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.tools().bottles = r
                        .get("bottles")
                        .and_then(Value::as_array)
                        .cloned()
                        .unwrap_or_default();
                    cx.notify();
                }
            },
        );
    }

    fn clear_d3dmetal_state(&mut self, bottle_id: &str) {
        let live = self.tools();
        live.d3d_states.remove(bottle_id);
        live.d3d_actions.insert(bottle_id.to_owned(), Vec::new());
    }

    fn store_d3dmetal_state(&mut self, bottle_id: &str, r: &Value) {
        let live = self.tools();
        live.d3d_states
            .insert(bottle_id.to_owned(), r["state"].clone());
        live.d3d_actions.insert(
            bottle_id.to_owned(),
            r.get("actions")
                .and_then(Value::as_array)
                .cloned()
                .unwrap_or_default(),
        );
    }

    /// `loadD3DMetalStatus`.
    fn load_d3dmetal_status(
        &mut self,
        bottle: Value,
        cx: &mut Context<Self>,
        then: impl FnOnce(&mut Self, &mut Context<Self>) + 'static,
    ) {
        let bottle_id = s(&bottle, "id").to_owned();
        if s(&bottle, "runtime_profile") != "d3dmetal" {
            self.clear_d3dmetal_state(&bottle_id);
            then(self, cx);
            return;
        }
        let Some(appid) = steam_app_id(&bottle) else {
            then(self, cx);
            return;
        };
        live::call(
            cx,
            "POST",
            "/d3dmetal/bottles/status",
            Some(json!({"appid": appid, "bottleId": bottle_id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                match r.filter(|r| is_ok(r) && r.get("state").is_some_and(|s| !s.is_null())) {
                    Some(r) => this.store_d3dmetal_state(&bottle_id, &r),
                    None => this.clear_d3dmetal_state(&bottle_id),
                }
                then(this, cx);
                cx.notify();
            },
        );
    }

    /// `runD3DMetalAction`; `done` receives the launch pid when one started.
    fn run_d3dmetal_action(
        &mut self,
        bottle: Value,
        action: Value,
        app: Option<Value>,
        cx: &mut Context<Self>,
        done: impl FnOnce(&mut Self, Option<u64>, &mut Context<Self>) + 'static,
    ) {
        let Some(appid) = steam_app_id(&bottle) else {
            done(self, None, cx);
            return;
        };
        let bottle_id = s(&bottle, "id").to_owned();
        self.tools().bottle_loading.insert(bottle_id.clone());
        let mut body = Map::new();
        body.insert("appid".into(), json!(appid));
        body.insert("bottleId".into(), json!(bottle_id));
        if let Some(dir) = bottle.get("game_install_path") {
            body.insert("gameDir".into(), dir.clone());
        }
        if let Some(app) = &app {
            body.insert("gameExe".into(), json!(app_exe_absolute(app)));
            let args: Vec<Value> = ["launch_args", "user_launch_args"]
                .iter()
                .filter_map(|key| app.get(*key).and_then(Value::as_array))
                .flatten()
                .cloned()
                .collect();
            body.insert("launchArgs".into(), Value::Array(args));
        }
        let action_id = s(&action, "id").to_owned();
        let label = s(&action, "label").to_owned();
        cx.notify();
        live::call(
            cx,
            "POST",
            d3dmetal_action_route(&action_id),
            Some(Value::Object(body)),
            LONG,
            move |this, r, cx| {
                this.tools().bottle_loading.remove(&bottle_id);
                if r.as_ref().is_some_and(is_ok) {
                    let r = r.unwrap();
                    toast::success(
                        cx,
                        if action_id == "play_d3dmetal" {
                            "D3DMetal launch started".to_owned()
                        } else {
                            format!("{label}: complete")
                        },
                    );
                    let pid = r
                        .get("launch")
                        .and_then(|launch| launch.get("pid"))
                        .and_then(Value::as_u64);
                    if r.get("state").is_some_and(|s| !s.is_null()) {
                        this.store_d3dmetal_state(&bottle_id, &r);
                        done(this, pid, cx);
                    } else {
                        this.load_d3dmetal_status(bottle, cx, move |this, cx| done(this, pid, cx));
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("{label} failed")),
                    );
                    this.load_d3dmetal_status(bottle, cx, move |this, cx| done(this, None, cx));
                }
                cx.notify();
            },
        );
    }

    /// `launchApp` D3DMetal branch. Returns false when the regular launch applies.
    pub(super) fn launch_app_d3dmetal(&mut self, app: &Value, cx: &mut Context<Self>) -> bool {
        let engine = s(app, "engine");
        if engine != "d3dmetal" {
            return false;
        }
        let Some(bottle) = self.bottle_for_app(app) else {
            return false;
        };
        if steam_app_id(&bottle).is_none() {
            return false;
        }
        let bottle_id = s(&bottle, "id").to_owned();
        let app = app.clone();
        if self.tools().d3d_states.contains_key(&bottle_id) {
            self.play_d3dmetal(bottle, app, cx);
        } else {
            let pending = bottle.clone();
            self.load_d3dmetal_status(pending, cx, move |this, cx| {
                this.play_d3dmetal(bottle, app, cx)
            });
        }
        true
    }

    fn play_d3dmetal(&mut self, bottle: Value, app: Value, cx: &mut Context<Self>) {
        let bottle_id = s(&bottle, "id").to_owned();
        let live = self.tools();
        let play_ready = live
            .d3d_states
            .get(&bottle_id)
            .is_some_and(|state| b(state, "play_ready"));
        let action = live
            .d3d_actions
            .get(&bottle_id)
            .and_then(|actions| {
                actions
                    .iter()
                    .find(|action| s(action, "id") == "play_d3dmetal")
            })
            .cloned()
            .unwrap_or_else(|| {
                json!({
                    "id": "play_d3dmetal",
                    "label": "Play D3DMetal",
                    "enabled": play_ready,
                    "state": if play_ready { "seeded" } else { "missing" },
                    "detail": "Launch game exe directly through GPTK Wine",
                })
            });
        if !play_ready || !b(&action, "enabled") {
            toast::error(
                cx,
                "D3DMetal bottle is not ready; seed VC runtime DLLs and seed prefix first",
            );
            return;
        }
        let id = s(&app, "id").to_owned();
        let name = s(&app, "name").to_owned();
        self.run_d3dmetal_action(bottle, action, Some(app), cx, move |_, pid, cx| {
            let Some(pid) = pid else { return };
            live::call(
                cx,
                "POST",
                "/sharp-library/track-running",
                Some(json!({"id": id, "pid": pid})),
                live::DEFAULT_TIMEOUT,
                move |this, r, cx| {
                    if r.as_ref().is_some_and(is_ok) {
                        let live = this.tools();
                        live.sharp_running.insert(id.clone(), pid);
                        live.launch_errors.insert(id.clone(), String::new());
                        live.diagnostics_open.remove(&id);
                        crate::launch_overlay::hint_later(name.clone(), cx);
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| format!("Could not track {name} for Stop")),
                        );
                    }
                    cx.notify();
                },
            );
        });
    }

    /// `openBottleLaunchLog`.
    pub(super) fn open_bottle_launch_log(&mut self, path: String, cx: &mut Context<Self>) {
        if path.is_empty() {
            return;
        }
        live::call(
            cx,
            "POST",
            "/diagnostics/open",
            Some(json!({"path": path})),
            live::DEFAULT_TIMEOUT,
            |_, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to open launch log".into()),
                    );
                }
            },
        );
    }

    /// `openAppBottleFolder`.
    pub(super) fn open_app_bottle_folder(&mut self, app: &Value, cx: &mut Context<Self>) {
        match self.bottle_for_app(app) {
            Some(bottle) => open_in_finder(s(&bottle, "prefix_path")),
            None => toast::error(
                cx,
                format!("{} is not associated with an app bottle", s(app, "name")),
            ),
        }
    }

    // ───────────────────────────── Launch Doctor ─────────────────────────────

    /// `openDiagnostics`.
    pub(super) fn open_diagnostics(&mut self, app: Value, cx: &mut Context<Self>) {
        self.tools()
            .diagnostics_open
            .insert(s(&app, "id").to_owned());
        self.run_doctor(app.clone(), cx);
        self.load_recent_diagnostics(app, cx);
    }

    /// `runDoctor`.
    pub(super) fn run_doctor(&mut self, app: Value, cx: &mut Context<Self>) {
        let id = s(&app, "id").to_owned();
        let live = self.tools();
        live.doctor_open.insert(id.clone());
        live.doctor_loading.insert(id.clone());
        live.doctor_reports.remove(&id);
        let mut body = Map::new();
        body.insert("id".into(), json!(id));
        if let Some(engine) = app.get("engine") {
            body.insert("engine".into(), engine.clone());
        }
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/doctor",
            Some(Value::Object(body)),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                this.tools().doctor_loading.remove(&id);
                match r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("report"))
                    .filter(|report| !report.is_null())
                {
                    Some(report) => {
                        this.tools().doctor_reports.insert(id, report.clone());
                    }
                    None => toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Launch Doctor failed".into()),
                    ),
                }
                cx.notify();
            },
        );
    }

    /// `loadRecentDiagnostics`.
    fn load_recent_diagnostics(&mut self, app: Value, cx: &mut Context<Self>) {
        let id = s(&app, "id").to_owned();
        self.tools().diagnostics_loading.insert(id.clone());
        let needles: Vec<String> = ["name", "exe_path", "install_dir"]
            .iter()
            .map(|key| s(&app, key).to_lowercase())
            .collect();
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        cx.spawn(async move |this, cx| {
            let (logs, crashes) = cx
                .background_executor()
                .spawn(async move {
                    let fetch = |path: &str| {
                        live.request("GET", path, None, live::DEFAULT_TIMEOUT)
                            .ok()
                            .map(live::unwrap_data)
                    };
                    (fetch("/logs"), fetch("/logs/crash-reports"))
                })
                .await;
            let _ = this.update(cx, |this, cx| {
                let matches = |text: &str| {
                    let text = text.to_lowercase();
                    needles
                        .iter()
                        .any(|needle| !needle.is_empty() && text.contains(needle))
                };
                let state = this.tools();
                state.diagnostics_loading.remove(&id);
                if let Some(logs) = logs.filter(is_ok) {
                    let all: Vec<String> = logs
                        .get("logs")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .flat_map(|entry| {
                            let name = s(entry, "name").to_owned();
                            entry
                                .get("lines")
                                .and_then(Value::as_array)
                                .into_iter()
                                .flatten()
                                .filter_map(Value::as_str)
                                .map(move |line| format!("[{name}] {line}"))
                                .collect::<Vec<_>>()
                        })
                        .collect();
                    let matching: Vec<String> =
                        all.iter().filter(|line| matches(line)).cloned().collect();
                    let source = if matching.is_empty() { all } else { matching };
                    let start = source.len().saturating_sub(40);
                    state
                        .recent_log_lines
                        .insert(id.clone(), source[start..].to_vec());
                }
                if let Some(crashes) = crashes.filter(is_ok) {
                    let reports: Vec<Value> = crashes
                        .get("reports")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .filter(|report| {
                            matches(&format!(
                                "{} {} {}",
                                s(report, "name"),
                                s(report, "file"),
                                s(report, "source")
                            ))
                        })
                        .take(5)
                        .cloned()
                        .collect();
                    state.recent_crashes.insert(id, reports);
                }
                cx.notify();
            });
        })
        .detach();
    }

    /// `runDoctorAction`.
    pub(super) fn run_doctor_action(&mut self, app: Value, check: Value, cx: &mut Context<Self>) {
        match doctor_action_label(&check, &app) {
            "Install runtime" => live::call(
                cx,
                "POST",
                "/setup/install-all",
                None,
                live::DEFAULT_TIMEOUT,
                |_, r, cx| {
                    if r.as_ref().is_some_and(is_ok) {
                        toast::success(cx, "Runtime install started");
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| "Failed to start runtime install".into()),
                        );
                    }
                },
            ),
            "Restart Steam" => live::call(
                cx,
                "POST",
                "/steam/stop",
                None,
                live::DEFAULT_TIMEOUT,
                |_, _, cx| {
                    live::call(
                        cx,
                        "POST",
                        "/steam/launch",
                        None,
                        live::DEFAULT_TIMEOUT,
                        |_, r, cx| {
                            if r.as_ref().is_some_and(is_ok) {
                                toast::success(cx, "Steam restart requested");
                            } else {
                                toast::error(
                                    cx,
                                    error_text(r.as_ref())
                                        .unwrap_or_else(|| "Failed to restart Steam".into()),
                                );
                            }
                        },
                    )
                },
            ),
            label @ ("Switch to Auto" | "Switch to Wine") => {
                let engine = if label == "Switch to Auto" {
                    "auto"
                } else {
                    "wine_bare"
                };
                let id = s(&app, "id").to_owned();
                let mut next = app;
                next["engine"] = json!(engine);
                self.update_engine_then(id, engine.to_owned(), cx, move |this, cx| {
                    this.run_doctor(next, cx)
                });
            }
            _ => self.open_log_folder(cx),
        }
    }

    /// `updateEngine` with a continuation (Electron awaits it either way).
    pub(super) fn update_engine_then(
        &mut self,
        id: String,
        engine: String,
        cx: &mut Context<Self>,
        then: impl FnOnce(&mut Self, &mut Context<Self>) + 'static,
    ) {
        live::call(
            cx,
            "POST",
            "/sharp-library/set-engine",
            Some(json!({"id": id, "engine": engine})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(app) = this.tools().apps.iter_mut().find(|a| s(a, "id") == id) {
                        app["engine"] = json!(engine);
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Failed to set engine".into()),
                    );
                }
                then(this, cx);
                cx.notify();
            },
        );
    }

    /// `clearShaderCache`.
    pub(super) fn clear_shader_cache(&mut self, name: String, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/cache/clear",
            Some(json!({"type": "shader"})),
            live::DEFAULT_TIMEOUT,
            move |_, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    toast::success(
                        cx,
                        format!("All shader caches cleared before next {name} launch"),
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to clear shader cache".into()),
                    );
                }
            },
        );
    }

    /// main `app:open-logs-folder`.
    pub(super) fn open_log_folder(&mut self, cx: &mut Context<Self>) {
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        if let Err(error) = crate::host_actions::open_folder(&live.home().join("logs")) {
            toast::error(cx, error);
        }
    }

    /// `copyDiagnosticBundle`.
    pub(super) fn copy_diagnostic_bundle(&mut self, app: &Value, cx: &mut Context<Self>) {
        let id = s(app, "id");
        let live = self.tools();
        let report = live
            .doctor_reports
            .get(id)
            .and_then(|report| serde_json::to_string_pretty(report).ok())
            .unwrap_or_else(|| "No doctor report loaded".into());
        let crashes =
            serde_json::to_string_pretty(&live.recent_crashes.get(id).cloned().unwrap_or_default())
                .unwrap_or_else(|_| "[]".into());
        let error = live
            .launch_errors
            .get(id)
            .filter(|e| !e.is_empty())
            .cloned()
            .unwrap_or_else(|| "none".into());
        let payload = [
            "MetalSharp Sharp Library Diagnostic Bundle".to_owned(),
            format!("App: {}", s(app, "name")),
            format!("ID: {id}"),
            format!("Engine: {}", s(app, "engine")),
            format!("EXE: {}/{}", s(app, "install_dir"), s(app, "exe_path")),
            format!("Last launch error: {error}"),
            String::new(),
            "Doctor:".into(),
            report,
            String::new(),
            "Recent crash reports:".into(),
            crashes,
            String::new(),
            "Recent launch log:".into(),
            live.recent_log_lines
                .get(id)
                .map(|lines| lines.join("\n"))
                .unwrap_or_default(),
        ]
        .join("\n");
        cx.write_to_clipboard(gpui::ClipboardItem::new_string(payload));
        toast::success(cx, "Diagnostic bundle copied");
    }

    // ───────────────────────────── renames ─────────────────────────────

    /// `beginSharpAppNameEdit` / `beginGameJoltNameEdit`.
    pub(super) fn begin_name_edit(
        &mut self,
        gamejolt: bool,
        id: String,
        name: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self
            .live
            .as_ref()
            .and_then(|l| l.name_edit.as_ref())
            .is_some_and(|edit| edit.saving)
        {
            return;
        }
        let input = cx.new(|cx| {
            let mut input = crate::search_input::SearchInput::new(cx);
            input.content = name.into();
            input.placeholder = "".into();
            input.text_color = 0xf0f0ed;
            input
        });
        let handle = gpui::Focusable::focus_handle(input.read(cx), cx);
        // GameJolt saves on blur (`@blur="saveGameJoltName(game)"`).
        let blur = gamejolt.then(|| {
            let id = id.clone();
            cx.on_blur(&handle, window, move |this, _, cx| {
                this.save_gamejolt_name(id.clone(), cx)
            })
        });
        self.tools().name_edit = Some(NameEdit {
            gamejolt,
            id,
            input: input.clone(),
            saving: false,
            _blur: blur,
        });
        // Focus and select once the input is part of the rendered frame.
        cx.on_next_frame(window, move |_, window, cx| {
            window.focus(&handle);
            handle.dispatch_action(&crate::search_input::SelectAll, window, cx);
        });
        cx.notify();
    }

    fn name_draft(&self, cx: &gpui::App) -> String {
        self.live
            .as_ref()
            .and_then(|l| l.name_edit.as_ref())
            .map(|edit| edit.input.read(cx).content.trim().to_owned())
            .unwrap_or_default()
    }

    fn editing(&self, gamejolt: bool, id: &str) -> bool {
        self.live
            .as_ref()
            .and_then(|l| l.name_edit.as_ref())
            .is_some_and(|edit| edit.gamejolt == gamejolt && edit.id == id)
    }

    /// `cancelSharpAppNameEdit` / GameJolt Escape.
    pub(super) fn cancel_name_edit(&mut self, cx: &mut Context<Self>) {
        let live = self.tools();
        if live.name_edit.as_ref().is_some_and(|edit| edit.saving) {
            return;
        }
        live.name_edit = None;
        cx.notify();
    }

    /// `saveSharpAppName`.
    pub(super) fn save_app_name(&mut self, id: String, cx: &mut Context<Self>) {
        if !self.editing(false, &id) || self.tools().name_edit.as_ref().unwrap().saving {
            return;
        }
        let name = self.name_draft(cx);
        if name.is_empty() {
            toast::error(cx, "Application name cannot be empty");
            return;
        }
        let current = self
            .tools()
            .apps
            .iter()
            .find(|app| s(app, "id") == id)
            .map(|app| s(app, "name").to_owned());
        if current.as_deref() == Some(name.as_str()) {
            self.cancel_name_edit(cx);
            return;
        }
        self.tools().name_edit.as_mut().unwrap().saving = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/rename",
            Some(json!({"id": id, "name": name})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let live = this.tools();
                if let Some(edit) = live.name_edit.as_mut() {
                    edit.saving = false;
                }
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(app) = live.apps.iter_mut().find(|app| s(app, "id") == id) {
                        app["name"] = json!(name);
                    }
                    if live.name_edit.as_ref().is_some_and(|edit| edit.id == id) {
                        live.name_edit = None;
                    }
                    toast::success(cx, "Application name updated");
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to save application name".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    /// `saveGameJoltName`.
    pub(super) fn save_gamejolt_name(&mut self, id: String, cx: &mut Context<Self>) {
        if !self.editing(true, &id) {
            return;
        }
        let name = self.name_draft(cx);
        let live = self.tools();
        live.name_edit = None;
        cx.notify();
        let current = live
            .gj_games
            .iter()
            .find(|game| s(game, "id") == id)
            .map(|game| s(game, "name").to_owned());
        if name.is_empty() || current.as_deref() == Some(name.as_str()) {
            return;
        }
        live::call(
            cx,
            "POST",
            "/gamejolt/name",
            Some(json!({"id": id, "name": name})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let saved = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("name"))
                    .and_then(Value::as_str)
                    .filter(|name| !name.is_empty())
                    .map(str::to_owned);
                match saved {
                    Some(saved) => {
                        if let Some(game) =
                            this.tools().gj_games.iter_mut().find(|g| s(g, "id") == id)
                        {
                            game["name"] = json!(saved);
                        }
                    }
                    None => toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not save GameJolt name".into()),
                    ),
                }
                cx.notify();
            },
        );
    }

    // ───────────────────────────── cover framing ─────────────────────────────

    /// Slider drag: value from the pointer position over the tracked bounds.
    pub(super) fn drag_cover_position(&mut self, x: f32, cx: &mut Context<Self>) {
        let Some((id, horizontal)) = self.tools().cover_drag.clone() else {
            return;
        };
        let key = format!("{id}:{}", if horizontal { "x" } else { "y" });
        let Some(bounds) = self.tools().slider_bounds.borrow().get(&key).copied() else {
            return;
        };
        let width = f32::from(bounds.size.width).max(1.0);
        let value = (((x - f32::from(bounds.origin.x)) / width) * 100.0)
            .round()
            .clamp(0.0, 100.0);
        let field = if horizontal {
            "cover_position_x"
        } else {
            "cover_position_y"
        };
        if let Some(app) = self.tools().apps.iter_mut().find(|a| s(a, "id") == id) {
            if app.get(field).and_then(Value::as_f64) != Some(value as f64) {
                app[field] = json!(value as u64);
                cx.notify();
            }
        }
    }

    /// Slider `@change` → `updateCoverPosition`.
    pub(super) fn finish_cover_drag(&mut self, cx: &mut Context<Self>) {
        let Some((id, _)) = self.live.as_mut().and_then(|l| l.cover_drag.take()) else {
            return;
        };
        let Some(app) = self.tools().apps.iter().find(|a| s(a, "id") == id).cloned() else {
            return;
        };
        live::call(
            cx,
            "POST",
            "/sharp-library/set-cover-position",
            Some(json!({
                "id": id,
                "x": app.get("cover_position_x").cloned().unwrap_or(Value::Null),
                "y": app.get("cover_position_y").cloned().unwrap_or(Value::Null),
            })),
            live::DEFAULT_TIMEOUT,
            |_, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to save cover position".into()),
                    );
                }
            },
        );
        cx.notify();
    }

    pub(super) fn cover_dimensions(&self, path: &std::path::Path) -> Option<(u32, u32)> {
        let live = self.live.as_ref()?;
        if let Some(dims) = live.cover_dims.borrow().get(path) {
            return *dims;
        }
        let dims = image::image_dimensions(path).ok();
        live.cover_dims
            .borrow_mut()
            .insert(path.to_path_buf(), dims);
        dims
    }

    // ───────────────────────────── GameJolt browser ─────────────────────────────

    /// Ask the parent shell to hide the embedded GameJolt browser while an
    /// app-level overlay (Settings, Streaming, update confirm) covers the page.
    pub fn set_obscured(&mut self, obscured: bool, cx: &mut Context<Self>) {
        let Some(live) = self.live.as_mut() else {
            return;
        };
        if live.embed_obscured != obscured {
            live.embed_obscured = obscured;
            if obscured {
                crate::mini_browser::hide_embedded_gamejolt();
            }
            cx.notify();
        }
    }

    /// Whether the embedded browser may be shown this frame.
    pub(super) fn gamejolt_embed_visible(&self) -> bool {
        let Some(live) = self.live.as_ref() else {
            return false;
        };
        live.active
            && !live.embed_obscured
            && self.state.source == SharpSource::GameJolt
            && live.confirm.is_none()
            && live.emu_confirm.is_none()
            && live.engine_menu.is_none()
            && !self.state.picker
            && !self.state.launch_settings_open
            && self.state.dialog.is_none()
    }
}

/// Paint-time placement of the embedded GameJolt `<webview>`.
pub(super) fn place_gamejolt_embed(bounds: gpui::Bounds<gpui::Pixels>, window: &mut Window) {
    let clip = window.content_mask().bounds;
    let visible = bounds.intersect(&clip);
    if f32::from(visible.size.width) < 2.0 || f32::from(visible.size.height) < 2.0 {
        crate::mini_browser::hide_embedded_gamejolt();
        return;
    }
    crate::mini_browser::show_embedded_gamejolt(
        window,
        [
            f32::from(visible.origin.x) as f64,
            f32::from(visible.origin.y) as f64,
            f32::from(visible.size.width) as f64,
            f32::from(visible.size.height) as f64,
        ],
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn helpers_match_sharp_view() {
        let app = json!({"engine": "auto", "install_dir": "/games/x/", "exe_path": "./bin/x.exe"});
        assert_eq!(app_exe_absolute(&app), "/games/x/bin/x.exe");
        assert_eq!(
            app_exe_absolute(&json!({"exe_path": "/abs.exe"})),
            "/abs.exe"
        );
        let route = json!({"id": "exe_route", "detail": ""});
        assert_eq!(doctor_action_label(&route, &app), "Switch to Wine");
        assert_eq!(
            doctor_action_label(&route, &json!({"engine": "d3dmetal"})),
            "Switch to Auto"
        );
        assert_eq!(
            doctor_action_label(&json!({"id": "x", "detail": "Steam is closed"}), &app),
            "Restart Steam"
        );
        assert_eq!(
            doctor_action_label(&json!({"id": "dll_sources", "detail": ""}), &app),
            "Install runtime"
        );
        assert_eq!(
            d3dmetal_action_route("seed_prefix"),
            "/d3dmetal/bottles/seed-prefix"
        );
        assert_eq!(
            d3dmetal_action_route("play_d3dmetal"),
            "/d3dmetal/bottles/play"
        );
        assert_eq!(
            bottle_badge_label(&json!({"health": "partial"})),
            "Partial install"
        );
        assert_eq!(bottle_badge_label(&json!({})), "Not installed");
        assert_eq!(steam_app_id(&json!({"steam_app_id": 0})), None);
    }
}
