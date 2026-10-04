//! Approved eight-card Settings overlay (`SettingsOverlay.vue`). With a live
//! backend every control performs the same request/IPC as the Electron app.
use crate::live::{self, error_text, is_ok};
use crate::page_palette::PagePalette;
use crate::toast;
use gpui::{
    AppContext, Context, EventEmitter, FocusHandle, FontWeight, Render, Window, div, prelude::*,
    px, rgb, rgba,
};
use serde_json::{Value, json};

use std::collections::HashMap;

const LANGUAGES: [(&str, &str); 20] = [
    ("en", "English"),
    ("zh-CN", "简体中文"),
    ("es", "Español"),
    ("hi", "हिन्दी"),
    ("ar", "العربية"),
    ("pt-BR", "Português (Brasil)"),
    ("bn", "বাংলা"),
    ("ru", "Русский"),
    ("ja", "日本語"),
    ("pa", "ਪੰਜਾਬੀ"),
    ("de", "Deutsch"),
    ("jv", "Basa Jawa"),
    ("ko", "한국어"),
    ("fr", "Français"),
    ("te", "తెలుగు"),
    ("vi", "Tiếng Việt"),
    ("tr", "Türkçe"),
    ("ur", "اردو"),
    ("it", "Italiano"),
    ("mr", "मराठी"),
];

#[derive(Clone, Debug)]
pub enum SettingsPreviewEvent {
    Close,
    ReopenSetup,
    LanguageChanged(&'static str),
    StartUpdate(&'static str),
    UpdateStatus(Value),
    SteamApiKeySaved(String, Option<Vec<crate::library_model::LibGame>>),
    ReloadLibrary,
    RefreshLaunchers,
    DeveloperMode(bool),
    LowPerformance(bool),
    DeviceName(String),
    BackendRestarted,
}

/// App-level state the Electron overlay injects from `App.vue`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct AppState {
    pub live: bool,
    pub steam_api_key: Option<String>,
    pub device_name: String,
    pub wine_steam_installed: bool,
    pub wine_steam_running: bool,
    pub mac_steam_installed: bool,
    pub mac_steam_running: bool,
    pub backend_connected: bool,
    pub backend_version: Option<String>,
    pub update_status: Option<Value>,
    pub update_downloading: bool,
    pub update_progress: f32,
    pub update_message: String,
    pub developer_mode: bool,
    pub low_performance: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConfirmAction {
    FexUpdate,
    SwitchToMac,
    ForceKill,
    Uninstall,
}

pub struct SettingsPreview {
    pub palette: PagePalette,
    pub language: &'static str,
    focus: Option<FocusHandle>,
    locales: HashMap<String, HashMap<String, String>>,
    narrow: bool,
    app: AppState,
    steam_key_input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    gamesdb_key_input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    gamesdb_configured: bool,
    graphics_logs: bool,
    retina: bool,
    retina_busy: bool,
    exclude_native: bool,
    native_busy: bool,
    backend_restarting: bool,
    shader_cache: Option<Value>,
    pipeline_cache: Option<Value>,
    confirm: Option<ConfirmAction>,
    language_menu: bool,
    notice: Option<&'static str>,
}

impl Default for SettingsPreview {
    fn default() -> Self {
        Self::new()
    }
}

fn format_cache(cache: Option<&Value>) -> (String, bool) {
    let Some(cache) = cache else {
        return ("...".into(), false);
    };
    match cache.get("status").and_then(Value::as_str) {
        Some("missing") => ("Missing".into(), false),
        Some("empty") => ("Empty".into(), false),
        _ => (
            format!(
                "{} · {} files",
                format_bytes(cache.get("bytes").and_then(Value::as_u64).unwrap_or(0)),
                cache.get("files").and_then(Value::as_u64).unwrap_or(0)
            ),
            true,
        ),
    }
}

impl SettingsPreview {
    pub fn new() -> Self {
        Self {
            palette: PagePalette::default(),
            language: "en",
            focus: None,
            locales: serde_json::from_str(include_str!("../assets/settings-locales.json"))
                .expect("Settings locale data"),
            narrow: false,
            app: AppState::default(),
            steam_key_input: None,
            gamesdb_key_input: None,
            gamesdb_configured: false,
            graphics_logs: false,
            retina: false,
            retina_busy: false,
            exclude_native: false,
            native_busy: false,
            backend_restarting: false,
            shader_cache: None,
            pipeline_cache: None,
            confirm: None,
            language_menu: false,
            notice: Some(
                "SAFE PREVIEW · Settings are simulated in memory; no backend, files, processes, or credentials are accessed.",
            ),
        }
    }

    /// Create the credential inputs (live builds only).
    pub fn attach_live(&mut self, cx: &mut Context<Self>) {
        if crate::live::Live::get(cx).is_none() {
            return;
        }
        self.notice = None;
        let make = |placeholder: &str, cx: &mut Context<Self>| {
            cx.new(|cx| {
                let mut input = crate::search_input::SearchInput::new(cx);
                input.placeholder = placeholder.to_owned().into();
                input.secret = true;
                input
            })
        };
        let steam_placeholder = self.tr("ui.settings.apiKey").to_string();
        let gamesdb_placeholder = self.tr("setup.theGamesDbApiPlaceholder").to_string();
        self.steam_key_input = Some(make(&steam_placeholder, cx));
        self.gamesdb_key_input = Some(make(&gamesdb_placeholder, cx));
    }

    /// Returns true when visible state changed.
    pub fn sync_app_state(&mut self, app: AppState) -> bool {
        if self.app == app {
            return false;
        }
        self.app = app;
        true
    }

    pub fn open(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.focus.is_none() {
            self.focus = Some(cx.focus_handle());
        }
        if let Some(focus) = &self.focus {
            window.focus(focus);
        }
        if self.app.live {
            // SettingsOverlay onMounted.
            if let (Some(input), Some(key)) =
                (&self.steam_key_input, self.app.steam_api_key.clone())
            {
                input.update(cx, |input, cx| {
                    input.content = key.into();
                    cx.notify();
                });
            }
            self.refresh_gamesdb_status(cx);
            self.refresh_config(cx);
            self.refresh_cache_sizes(cx);
        }
    }

    fn tr(&self, key: &str) -> gpui::SharedString {
        self.locales
            .get(self.language)
            .and_then(|locale| locale.get(key))
            .or_else(|| self.locales["en"].get(key))
            .cloned()
            .unwrap_or_else(|| key.to_owned())
            .into()
    }

    fn refresh_gamesdb_status(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/sharp-library/epic/thegamesdb-api-key",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.gamesdb_configured = r
                    .as_ref()
                    .and_then(|r| r.get("configured"))
                    .and_then(Value::as_bool)
                    == Some(true);
                cx.notify();
            },
        );
    }

    fn refresh_config(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/config",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.apply_config(&r);
                    cx.notify();
                }
            },
        );
    }

    fn apply_config(&mut self, config: &Value) {
        self.graphics_logs = config
            .get("graphicsRuntimeLogs")
            .or_else(|| config.get("graphics_runtime_logs"))
            .and_then(Value::as_bool)
            .unwrap_or(false);
        self.retina = config.get("retinaMode").and_then(Value::as_bool) == Some(true);
        self.exclude_native = config
            .get("excludeNativeMacSteamGames")
            .and_then(Value::as_bool)
            == Some(true);
    }

    fn refresh_cache_sizes(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/cache/size",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.shader_cache = r.get("shader_cache").cloned();
                    this.pipeline_cache = r.get("pipeline_cache").cloned();
                    cx.notify();
                }
            },
        );
    }

    fn save_api_key(&mut self, cx: &mut Context<Self>) {
        let Some(input) = self.steam_key_input.clone() else {
            return;
        };
        let key = input.read(cx).content.trim().to_owned();
        if key.is_empty() {
            toast::error(cx, "Please enter a Steam API key");
            return;
        }
        live::call(
            cx,
            "POST",
            "/steam/save-api-key",
            Some(json!({"key": key})),
            live::DEFAULT_TIMEOUT,
            move |_, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to save Steam API key".into()),
                    );
                    return;
                }
                let r = r.unwrap();
                let library = r
                    .get("library")
                    .and_then(|l| crate::library_model::parse_library(Some(l)));
                let total = r
                    .get("library")
                    .and_then(|l| l.get("total"))
                    .and_then(Value::as_u64)
                    .unwrap_or(0);
                cx.emit(SettingsPreviewEvent::SteamApiKeySaved(key.clone(), library));
                if r.get("sync")
                    .and_then(|s| s.get("steam_id_detected"))
                    .and_then(Value::as_bool)
                    == Some(false)
                {
                    toast::error(cx, "API key saved, but SteamID was not detected yet");
                } else {
                    toast::success(cx, format!("API key saved — synced {total} games"));
                }
            },
        );
    }

    fn save_gamesdb_key(&mut self, cx: &mut Context<Self>) {
        let Some(input) = self.gamesdb_key_input.clone() else {
            return;
        };
        let key = input.read(cx).content.trim().to_owned();
        if key.is_empty() {
            toast::error(cx, self.tr("ui.settings.theGamesDbKeyRequired").to_string());
            return;
        }
        let failed = self.tr("ui.settings.theGamesDbSaveFailed").to_string();
        let saved = self.tr("ui.settings.theGamesDbKeySavedToast").to_string();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/thegamesdb-api-key",
            Some(json!({"key": key})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(cx, error_text(r.as_ref()).unwrap_or(failed));
                    return;
                }
                this.gamesdb_configured = r
                    .as_ref()
                    .and_then(|r| r.get("configured"))
                    .and_then(Value::as_bool)
                    == Some(true);
                if let Some(input) = &this.gamesdb_key_input {
                    input.update(cx, |input, cx| input.clear(cx));
                }
                toast::success(cx, saved);
                cx.notify();
            },
        );
    }

    fn change_device_name(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/setup/device-name",
            None,
            live::DEFAULT_TIMEOUT,
            |_, r, cx| {
                let Some(name) = r
                    .as_ref()
                    .and_then(|r| r.get("name"))
                    .and_then(Value::as_str)
                    .filter(|name| !name.is_empty())
                    .map(str::to_owned)
                else {
                    return;
                };
                cx.emit(SettingsPreviewEvent::DeviceName(name.clone()));
                live::call(
                    cx,
                    "POST",
                    "/setup/save",
                    Some(json!({"deviceName": name})),
                    live::DEFAULT_TIMEOUT,
                    move |_, _, cx| {
                        toast::success(cx, format!("Device name changed to {name}"));
                    },
                );
            },
        );
    }

    fn toggle_wine_steam(&mut self, cx: &mut Context<Self>) {
        if self.app.wine_steam_running {
            live::call(
                cx,
                "POST",
                "/steam/stop",
                None,
                live::DEFAULT_TIMEOUT,
                |_, r, cx| {
                    let stopped = r.as_ref().is_some_and(is_ok)
                        && r.as_ref()
                            .and_then(|r| r.get("running"))
                            .and_then(Value::as_bool)
                            == Some(false);
                    if stopped {
                        toast::success(cx, "Wine Steam stopped");
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| "Wine Steam is still running".into()),
                        );
                    }
                    cx.emit(SettingsPreviewEvent::RefreshLaunchers);
                },
            );
        } else {
            toast::success(cx, "Starting Steam...");
            live::call(
                cx,
                "POST",
                "/steam/launch",
                None,
                live::DEFAULT_TIMEOUT,
                |_, r, cx| {
                    if r.as_ref().is_some_and(is_ok) {
                        toast::success(cx, "Steam started");
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| "Failed to start Steam".into()),
                        );
                    }
                    cx.emit(SettingsPreviewEvent::RefreshLaunchers);
                },
            );
        }
    }

    fn install_mac_steam(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/steam/mac-install",
            None,
            live::DEFAULT_TIMEOUT,
            |_, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if r.as_ref()
                        .and_then(|r| r.get("installed"))
                        .and_then(Value::as_bool)
                        == Some(true)
                    {
                        toast::success(cx, "macOS Steam is already installed");
                    } else {
                        toast::success(cx, "Steam download page opened");
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not open macOS Steam installer".into()),
                    );
                }
                cx.emit(SettingsPreviewEvent::RefreshLaunchers);
            },
        );
    }

    fn toggle_mac_steam(&mut self, cx: &mut Context<Self>) {
        if self.app.mac_steam_running {
            live::call(
                cx,
                "POST",
                "/steam/mac-stop",
                None,
                live::DEFAULT_TIMEOUT,
                |_, r, cx| {
                    let stopped = r.as_ref().is_some_and(is_ok)
                        && r.as_ref()
                            .and_then(|r| r.get("running"))
                            .and_then(Value::as_bool)
                            == Some(false);
                    if stopped {
                        toast::success(cx, "Mac Steam stopped");
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| "Mac Steam is still running".into()),
                        );
                    }
                    cx.emit(SettingsPreviewEvent::RefreshLaunchers);
                },
            );
        } else if self.app.wine_steam_running {
            self.confirm = Some(ConfirmAction::SwitchToMac);
            cx.notify();
        } else {
            self.launch_mac_steam(cx);
        }
    }

    fn launch_mac_steam(&mut self, cx: &mut Context<Self>) {
        toast::success(cx, "Starting Mac Steam...");
        live::call(
            cx,
            "POST",
            "/steam/mac-launch",
            None,
            live::DEFAULT_TIMEOUT,
            |_, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    toast::success(cx, "Mac Steam started");
                }
                cx.emit(SettingsPreviewEvent::RefreshLaunchers);
            },
        );
    }

    fn switch_to_mac_steam(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/steam/stop",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                let stopped = r.as_ref().is_some_and(is_ok)
                    && r.as_ref()
                        .and_then(|r| r.get("running"))
                        .and_then(Value::as_bool)
                        == Some(false);
                if !stopped {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Wine Steam is still running".into()),
                    );
                    cx.emit(SettingsPreviewEvent::RefreshLaunchers);
                    return;
                }
                this.launch_mac_steam(cx);
            },
        );
    }

    fn restart_backend(&mut self, cx: &mut Context<Self>) {
        if self.backend_restarting {
            return;
        }
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        self.backend_restarting = true;
        toast::success(cx, "Restarting backend...");
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = cx
                .background_executor()
                .spawn(async move {
                    let started = live.start_backend();
                    (started, live.backend_alive())
                })
                .await;
            let _ = this.update(cx, |this, cx| {
                this.backend_restarting = false;
                match result {
                    (Ok(()), true) => toast::success(cx, "Backend restarted"),
                    (Err(error), _) => toast::error(cx, error),
                    _ => toast::error(cx, "Backend did not come back online"),
                }
                cx.emit(SettingsPreviewEvent::BackendRestarted);
                cx.notify();
            });
        })
        .detach();
    }

    fn force_kill(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/processes/force-kill",
            Some(json!({})),
            std::time::Duration::from_millis(15_000),
            |_, r, cx| {
                let Some(r) = r else {
                    toast::error(cx, "Force kill request failed");
                    return;
                };
                let count = r
                    .get("terminated_count")
                    .and_then(Value::as_u64)
                    .unwrap_or_else(|| {
                        r.get("terminated")
                            .and_then(Value::as_array)
                            .map_or(0, |a| a.len() as u64)
                            + r.get("killed")
                                .and_then(Value::as_array)
                                .map_or(0, |a| a.len() as u64)
                    });
                if is_ok(&r) {
                    toast::success(
                        cx,
                        if count > 0 {
                            format!(
                                "Force killed {count} process{}",
                                if count == 1 { "" } else { "es" }
                            )
                        } else {
                            "No MetalSharp runtime processes found".into()
                        },
                    );
                } else {
                    let errors = r
                        .get("errors")
                        .and_then(Value::as_array)
                        .map_or(0, Vec::len);
                    toast::error(
                        cx,
                        error_text(Some(&r)).unwrap_or_else(|| {
                            format!("Force kill completed with {errors} error(s)")
                        }),
                    );
                }
            },
        );
    }

    fn set_graphics_logs(&mut self, enabled: bool, cx: &mut Context<Self>) {
        let previous = self.graphics_logs;
        self.graphics_logs = enabled;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/config",
            Some(json!({"graphicsRuntimeLogs": enabled, "logs": enabled})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.apply_config(&r);
                    toast::success(
                        cx,
                        if this.graphics_logs {
                            "Graphics runtime logs enabled for future launches"
                        } else {
                            "Graphics runtime logs disabled"
                        },
                    );
                } else {
                    this.graphics_logs = previous;
                    toast::error(cx, "Failed to save graphics logging setting");
                }
                cx.notify();
            },
        );
    }

    fn set_retina(&mut self, enabled: bool, cx: &mut Context<Self>) {
        if self.retina_busy || enabled == self.retina {
            return;
        }
        let previous = self.retina;
        self.retina_busy = true;
        self.retina = enabled;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/config",
            Some(json!({"retinaMode": enabled})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.apply_config(&r);
                    toast::success(
                        cx,
                        format!(
                            "Retina rendering {} — restart Wine Steam to apply",
                            if enabled { "enabled" } else { "disabled" }
                        ),
                    );
                } else {
                    this.retina = previous;
                    toast::error(cx, "Failed to update Retina rendering");
                }
                this.retina_busy = false;
                cx.notify();
            },
        );
    }

    fn set_exclude_native(&mut self, enabled: bool, cx: &mut Context<Self>) {
        if self.native_busy || enabled == self.exclude_native {
            return;
        }
        let previous = self.exclude_native;
        self.native_busy = true;
        self.exclude_native = enabled;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/config",
            Some(json!({"excludeNativeMacSteamGames": enabled})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.apply_config(&r);
                    cx.emit(SettingsPreviewEvent::ReloadLibrary);
                } else {
                    this.exclude_native = previous;
                    toast::error(cx, "Failed to update native Steam game visibility");
                }
                this.native_busy = false;
                cx.notify();
            },
        );
    }

    fn open_folder(&mut self, logs: bool, cx: &mut Context<Self>) {
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        let path = if logs {
            live.home().join("logs")
        } else {
            live.home()
        };
        match crate::host_actions::open_folder(&path) {
            Ok(()) => toast::success(
                cx,
                if logs {
                    "Logs folder opened"
                } else {
                    "MetalSharp data folder opened"
                },
            ),
            Err(error) => toast::error(
                cx,
                if error.is_empty() {
                    if logs {
                        "Failed to open logs".into()
                    } else {
                        "Failed to open data folder".into()
                    }
                } else {
                    error
                },
            ),
        }
    }

    fn repair_data_access(&mut self, cx: &mut Context<Self>) {
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        let result = crate::host_actions::verify_data_access(&live.home());
        if is_ok(&result) {
            toast::success(cx, "MetalSharp data access verified");
        } else {
            let first = result
                .get("checks")
                .and_then(Value::as_array)
                .and_then(|checks| {
                    checks
                        .iter()
                        .find(|c| c.get("ok").and_then(Value::as_bool) != Some(true))
                })
                .and_then(|c| c.get("error"))
                .and_then(Value::as_str)
                .map(str::to_owned);
            toast::error(
                cx,
                first.unwrap_or_else(|| "MetalSharp data access needs attention".into()),
            );
        }
    }

    fn clear_cache(&mut self, kind: &'static str, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/cache/clear",
            Some(json!({"type": kind})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let freed =
                        format_bytes(r.get("bytes_freed").and_then(Value::as_u64).unwrap_or(0));
                    toast::success(
                        cx,
                        format!(
                            "{} cache cleared — {freed} freed",
                            if kind == "shader" {
                                "Shader"
                            } else {
                                "Pipeline"
                            }
                        ),
                    );
                }
                this.refresh_cache_sizes(cx);
            },
        );
    }

    fn check_for_updates(&mut self, cx: &mut Context<Self>) {
        toast::success(cx, "Checking for updates...");
        live::call(
            cx,
            "GET",
            "/update/check",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = &r {
                    cx.emit(SettingsPreviewEvent::UpdateStatus(r.clone()));
                    this.app.update_status = Some(r.clone());
                }
                match r.as_ref() {
                    Some(r)
                        if is_ok(r)
                            && r.get("available").and_then(Value::as_bool) == Some(true) =>
                    {
                        toast::success(
                            cx,
                            format!(
                                "Update available: v{}",
                                r.get("latest_version")
                                    .and_then(Value::as_str)
                                    .unwrap_or("")
                            ),
                        )
                    }
                    Some(r) if is_ok(r) => toast::success(cx, "You're up to date!"),
                    _ => toast::error(cx, "Could not check for updates"),
                }
                cx.notify();
            },
        );
    }

    /// main `app:uninstall` after confirmation.
    fn uninstall(&mut self, cx: &mut Context<Self>) {
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        live.stop_backend();
        let mut failures = Vec::new();
        for path in crate::host_actions::related_data_paths(&live.home()) {
            if !crate::host_actions::remove_path(&path) {
                failures.push(path.to_string_lossy().into_owned());
            }
        }
        crate::host_actions::schedule_bundle_trash();
        let message = if failures.is_empty() {
            "MetalSharp data was removed. The app will now close.".to_owned()
        } else {
            format!(
                "Some MetalSharp data could not be removed:\n{}",
                failures.join("\n")
            )
        };
        cx.spawn(async move |_, cx| {
            cx.background_executor()
                .spawn(async move {
                    rfd::MessageDialog::new()
                        .set_title("Uninstall MetalSharp")
                        .set_description(message)
                        .set_level(rfd::MessageLevel::Info)
                        .show();
                })
                .await;
            let _ = cx.update(|cx| cx.quit());
        })
        .detach();
    }

    fn credential_control(&self, epic: bool, cx: &mut Context<Self>) -> gpui::Div {
        let configured = if epic {
            self.gamesdb_configured
        } else {
            self.app
                .steam_api_key
                .as_deref()
                .is_some_and(|k| !k.is_empty())
        };
        let input = if epic {
            self.gamesdb_key_input.clone()
        } else {
            self.steam_key_input.clone()
        };
        let field = div()
            .min_w_0()
            .flex_1()
            .w(px(210.0))
            .h(px(30.0))
            .px(px(10.0))
            .flex()
            .items_center()
            .rounded(px(6.0))
            .border_1()
            .border_color(rgba(self.palette.control_border))
            .bg(rgba((self.palette.control_bg << 8) | 0xb3))
            .text_size(px(12.0))
            .text_color(rgb(self.palette.control_text))
            .overflow_hidden();
        let field = match input {
            Some(input) => field.child(div().w_full().child(input)),
            None => field
                .text_color(rgba((self.palette.control_text << 8) | 0x99))
                .child(self.tr(if epic {
                    "setup.theGamesDbApiPlaceholder"
                } else {
                    "ui.settings.apiKey"
                })),
        };
        div()
            .min_w_0()
            .flex()
            .items_center()
            .flex_wrap()
            .gap(px(8.0))
            .child(
                div()
                    .min_w_0()
                    .w(px(310.0))
                    .max_w_full()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(field)
                    .child(
                        self.action_button(
                            if epic { "epic-save" } else { "steam-save" },
                            if epic {
                                self.tr("actions.save")
                            } else {
                                format!("{} & Sync", self.tr("actions.save")).into()
                            },
                            true,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if epic {
                                this.save_gamesdb_key(cx);
                            } else {
                                this.save_api_key(cx);
                            }
                        })),
                    ),
            )
            .child(self.badge(
                self.tr(if epic {
                    if configured {
                        "ui.settings.theGamesDbKeySaved"
                    } else {
                        "ui.settings.theGamesDbNoKey"
                    }
                } else if configured {
                    "ui.settings.keySaved"
                } else {
                    "ui.settings.noKey"
                }),
                configured,
            ))
    }

    fn language_control(&self, height: f32, cx: &mut Context<Self>) -> gpui::Div {
        let label = LANGUAGES
            .iter()
            .find(|(code, _)| *code == self.language)
            .map(|(_, name)| *name)
            .unwrap_or("English");
        let mut control = div().relative().flex_none().child(
            self.action_button("language-pick", format!("{label}  ⌄"), false)
                .min_w(px(132.0))
                .text_size(px(12.0))
                .on_click(cx.listener(|this, _, _, cx| {
                    this.language_menu = !this.language_menu;
                    cx.notify();
                })),
        );
        if self.language_menu {
            let mut menu = div()
                .id("settings-language-list")
                .occlude()
                .absolute()
                .top(gpui::relative(1.0))
                .right_0()
                .mt(px(4.0))
                .w(px(210.0))
                .max_h(px((height - 300.0).clamp(120.0, 360.0)))
                .overflow_y_scroll()
                .p(px(6.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(rgba(self.palette.control_border))
                .bg(rgb(self.palette.menu_bg))
                .shadow_lg();
            for (code, name) in LANGUAGES {
                menu = menu.child(
                    div()
                        .id(code)
                        .px(px(9.0))
                        .py(px(7.0))
                        .rounded(px(5.0))
                        .text_color(rgb(self.palette.control_text))
                        .text_size(px(12.0))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(self.palette.menu_hover)))
                        .child(name)
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.language = code;
                            this.language_menu = false;
                            cx.emit(SettingsPreviewEvent::LanguageChanged(code));
                            cx.notify();
                        })),
                );
            }
            control = control.child(gpui::deferred(menu).with_priority(230));
        }
        div()
            .flex()
            .items_center()
            .gap(px(8.0))
            .child(
                div()
                    .text_size(px(12.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(self.tr("language.label")),
            )
            .child(control)
    }

    fn confirmation_overlay(&self, cx: &mut Context<Self>) -> gpui::Stateful<gpui::Div> {
        let action = self.confirm.unwrap();
        let (title, copy, confirm) = match action {
            ConfirmAction::ForceKill => (
                "Force Kill Processes",
                "Force kill MetalSharp Wine/runtime processes? This can stop active games, installers, and downloads.",
                "Force Kill",
            ),
            ConfirmAction::Uninstall => (
                "Uninstall MetalSharp?",
                "Permanently delete all Wine prefixes, bottles, Steam installation, Wine runtime, shader caches, and settings? The app will close after cleanup.",
                "Uninstall",
            ),
            ConfirmAction::SwitchToMac => (
                "Switch Steam Client",
                "Stop Wine Steam and start Mac Steam?",
                "OK",
            ),
            ConfirmAction::FexUpdate => (
                "FEX Version Notice",
                "The FEX DMG only works on macOS 27 or newer. The FEX version is experimental, so expect more potential bugs than the baseline MetalSharp version.\n\nSelect OK to continue or Cancel to keep the baseline version.",
                "OK",
            ),
        };
        div()
            .id("settings-confirm-backdrop")
            .occlude()
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .p(px(24.0))
            .bg(rgba(0x00000088))
            .on_click(cx.listener(|this, _, _, cx| {
                this.confirm = None;
                cx.stop_propagation();
                cx.notify();
            }))
            .child(
                div()
                    .id("settings-confirm-dialog")
                    .occlude()
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .w(px(400.0))
                    .max_w_full()
                    .p(px(20.0))
                    .rounded(px(10.0))
                    .border_1()
                    .border_color(rgba(self.palette.control_border))
                    .bg(rgb(self.palette.menu_bg))
                    .text_color(rgb(self.palette.control_text))
                    .flex()
                    .flex_col()
                    .gap(px(14.0))
                    .child(
                        div()
                            .text_size(px(15.0))
                            .font_weight(FontWeight::BOLD)
                            .child(title),
                    )
                    .child(div().text_size(px(12.0)).line_height(px(18.0)).child(copy))
                    .child(
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.0))
                            .child(
                                self.action_button("confirm-cancel", "Cancel", false)
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.confirm = None;
                                        cx.notify();
                                    })),
                            )
                            .child(self.action_button("confirm-ok", confirm, true).on_click(
                                cx.listener(move |this, _, _, cx| {
                                    this.confirm = None;
                                    if this.app.live {
                                        match action {
                                            ConfirmAction::ForceKill => this.force_kill(cx),
                                            ConfirmAction::Uninstall => this.uninstall(cx),
                                            ConfirmAction::SwitchToMac => {
                                                this.switch_to_mac_steam(cx)
                                            }
                                            ConfirmAction::FexUpdate => {
                                                cx.emit(SettingsPreviewEvent::StartUpdate("fex"))
                                            }
                                        }
                                    }
                                    cx.notify();
                                }),
                            )),
                    ),
            )
    }

    fn dismiss(&mut self, cx: &mut Context<Self>) {
        cx.emit(SettingsPreviewEvent::Close);
    }
    fn action_button(
        &self,
        id: &'static str,
        label: impl Into<gpui::SharedString>,
        primary: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let p = self.palette;
        div()
            .id(id)
            .flex_none()
            .flex()
            .items_center()
            .justify_center()
            .h(px(30.))
            .px(px(12.))
            .rounded(px(6.))
            .border_1()
            .border_color(rgba(if primary {
                (p.accent << 8) | 0xff
            } else {
                p.control_border
            }))
            .bg(rgb(if primary { p.accent } else { p.control_bg }))
            .text_color(rgb(if primary {
                p.control_bg
            } else {
                p.control_text
            }))
            .text_size(px(11.5))
            .font_weight(FontWeight::SEMIBOLD)
            .cursor_pointer()
            .hover(move |s| s.bg(rgb(if primary { p.accent } else { p.hover })))
            .child(label.into())
    }
    fn row(
        &self,
        title: impl Into<gpui::SharedString>,
        desc: impl Into<gpui::SharedString>,
        control: impl IntoElement,
    ) -> gpui::Div {
        let p = self.palette;
        div()
            .flex()
            .justify_between()
            .items_start()
            .gap(px(if self.narrow { 8.0 } else { 18.0 }))
            .when(self.narrow, |d| d.flex_col())
            .py(px(11.))
            .border_b_1()
            .border_color(rgba(
                (p.control_border & 0xffffff00)
                    | (((p.control_border & 0xff) as f32 * 0.55) as u32),
            ))
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .child(
                        div()
                            .text_color(rgb(p.control_text))
                            .text_size(px(13.))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(title.into()),
                    )
                    .child(
                        div()
                            .mt(px(3.))
                            .text_color(rgba((p.control_text << 8) | 0x9e))
                            .text_size(px(11.5))
                            .line_height(px(16.675))
                            .child(desc.into()),
                    ),
            )
            .child(
                div()
                    .flex_none()
                    .flex()
                    .flex_wrap()
                    .items_center()
                    .justify_end()
                    .gap(px(8.))
                    .when(!self.narrow, |d| d.max_w(gpui::relative(0.55)))
                    .when(self.narrow, |d| d.w_full().justify_start())
                    .child(control),
            )
    }
    fn card(
        &self,
        title: impl Into<gpui::SharedString>,
        content: impl IntoElement,
        danger: bool,
    ) -> gpui::Div {
        let p = self.palette;
        let title: gpui::SharedString = title.into();
        div()
            .min_w_0()
            .px(px(16.0))
            .pt(px(14.0))
            .pb(px(6.))
            .rounded(px(10.))
            .border_1()
            .border_color(rgba(p.control_border))
            .bg(rgba((p.control_bg << 8) | 0xd1))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(7.))
                    .mb(px(10.))
                    .pb(px(9.))
                    .border_b_1()
                    .border_color(rgba(if danger {
                        0xff5c5c77
                    } else {
                        (p.accent << 8) | 0x73
                    }))
                    .text_color(rgb(if danger { 0xff8585 } else { p.control_text }))
                    .text_size(px(13.))
                    .font_weight(FontWeight::BOLD)
                    .child(title.to_uppercase()),
            )
            .child(content)
    }
    fn badge(&self, text: impl Into<gpui::SharedString>, good: bool) -> gpui::Div {
        let (success, warn, success_alpha) = match self.palette.accent {
            0x4db8ff => (0x438f55, 0xa97818, 0x1a),
            0xd6d0c4 => (0xa3b89f, 0xc4b088, 0x24),
            0x6fce88 => (0x6fce88, 0xd8b24b, 0x24),
            0xff9a45 | 0xff6b52 => (0x7cbf6a, 0xffb84d, 0x24),
            0xff66aa => (0x9be34a, 0xffc44d, 0x1f),
            _ => (0x6bbf7a, 0xd8a84b, 0x24),
        };
        let color = if good { success } else { warn };
        div()
            .flex_none()
            .flex()
            .items_center()
            .min_h(px(22.0))
            .px(px(8.0))
            .py(px(2.0))
            .rounded(px(4.0))
            .bg(rgba(
                (color << 8)
                    | if good {
                        success_alpha
                    } else if self.palette.light {
                        0x1a
                    } else {
                        0x24
                    },
            ))
            .text_color(rgb(color))
            .text_size(px(11.0))
            .font_weight(FontWeight::BOLD)
            .child(text.into())
    }
    fn toggle(
        &self,
        id: &'static str,
        enabled: bool,
        cx: &mut Context<Self>,
        f: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        let p = self.palette;
        div()
            .id(id)
            .flex()
            .items_center()
            .w(px(34.))
            .h(px(19.))
            .px(px(2.))
            .rounded(px(999.))
            .border_1()
            .border_color(rgba(if enabled {
                (p.accent << 8) | 0xff
            } else {
                p.control_border
            }))
            .bg(if enabled {
                rgba((p.accent << 8) | 0x4d)
            } else {
                rgba((p.control_bg << 8) | 0xff)
            })
            .cursor_pointer()
            .on_click(cx.listener(move |this, _, _, cx| f(this, cx)))
            .child(
                div()
                    .size(px(13.))
                    .rounded(px(999.))
                    .bg(rgba(
                        ((if enabled { p.accent } else { p.control_text }) << 8)
                            | if enabled { 0xff } else { 0xb8 },
                    ))
                    .when(enabled, |d| d.ml(px(15.))),
            )
    }
    fn text_control(&self, text: impl Into<gpui::SharedString>) -> gpui::Div {
        div()
            .text_color(rgb(self.palette.control_text))
            .text_size(px(12.))
            .child(text.into())
    }
}
impl EventEmitter<SettingsPreviewEvent> for SettingsPreview {}

impl Render for SettingsPreview {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let p = self.palette;
        self.narrow = f32::from(window.viewport_size().width) <= 900.0;
        let app = self.app.clone();
        let steam_row = self.row(
            self.tr("ui.settings.apiKey"),
            format!(
                "{} steamcommunity.com/dev/apikey.",
                self.tr("ui.settingsDesc.apiKey")
            ),
            self.credential_control(false, cx),
        );
        let device_label = if app.device_name.is_empty() {
            self.tr("ui.settings.notSet").to_string()
        } else {
            app.device_name.clone()
        };
        let device_row = self
            .row(
                self.tr("ui.settings.deviceName"),
                self.tr("ui.settingsDesc.device"),
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .child(self.text_control(device_label))
                    .child(
                        self.action_button("device-change", self.tr("ui.settings.change"), false)
                            .on_click(cx.listener(|this, _, _, cx| this.change_device_name(cx))),
                    )
                    .child(
                        div()
                            .w(px(1.))
                            .h(px(28.))
                            .mx(px(4.))
                            .bg(rgba(p.control_border)),
                    )
                    .child(self.language_control(f32::from(window.viewport_size().height), cx)),
            )
            .border_b_0();
        let epic_row = self
            .row(
                self.tr("ui.settings.theGamesDbApiKey"),
                format!(
                    "{} TheGamesDB.",
                    self.tr("ui.settingsDesc.theGamesDbApiKey")
                ),
                self.credential_control(true, cx),
            )
            .border_b_0();
        let wine_row = self.row(
            self.tr("ui.settings.wineSteam"),
            self.tr("ui.settingsDesc.wineSteam"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(if app.wine_steam_installed {
                    self.badge(self.tr("ui.settings.installed"), true)
                } else {
                    self.badge(self.tr("ui.settings.notInstalled"), false)
                })
                .when(app.wine_steam_installed, |d| {
                    d.child(
                        self.action_button(
                            "wine-toggle",
                            if app.wine_steam_running {
                                self.tr("ui.settings.stopSteam")
                            } else {
                                self.tr("ui.settings.startSteam")
                            },
                            false,
                        )
                        .on_click(cx.listener(|this, _, _, cx| this.toggle_wine_steam(cx))),
                    )
                }),
        );
        let setup_row = self.row(
            "Missing Windows Steam?",
            "Re-run the setup wizard to install or repair the Steam runtime",
            self.action_button("run-setup", "Run Setup Wizard", true)
                .on_click(cx.listener(|_, _, _, cx| cx.emit(SettingsPreviewEvent::ReopenSetup))),
        );
        let mac_row = self.row(
            self.tr("ui.settings.steamMac"),
            self.tr("ui.settingsDesc.macSteam"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(if app.mac_steam_installed {
                    self.badge(self.tr("ui.settings.installed"), true)
                } else {
                    self.badge(self.tr("ui.settings.notInstalled"), false)
                })
                .child(
                    self.action_button(
                        "mac-action",
                        if app.mac_steam_installed {
                            if app.mac_steam_running {
                                self.tr("ui.settings.stopSteamMac")
                            } else {
                                self.tr("ui.settings.startSteamMac")
                            }
                        } else {
                            self.tr("ui.settings.installMacSteam")
                        },
                        !app.mac_steam_installed,
                    )
                    .on_click(cx.listener(|this, _, _, cx| {
                        if !this.app.mac_steam_installed {
                            this.install_mac_steam(cx);
                        } else {
                            this.toggle_mac_steam(cx);
                        }
                    })),
                )
                .when(app.mac_steam_installed, |d| {
                    d.child(self.text_control("Exclude native"))
                        .child(
                            self.toggle("exclude-native", self.exclude_native, cx, |s, cx| {
                                let next = !s.exclude_native;
                                s.set_exclude_native(next, cx)
                            }),
                        )
                }),
        );
        let retina = self.row(
            "High Resolution (Retina)",
            self.tr("ui.settingsDesc.retina"),
            self.toggle("retina-toggle", self.retina, cx, |s, cx| {
                let next = !s.retina;
                s.set_retina(next, cx)
            }),
        );
        let backend = self.row(
            self.tr("ui.settings.backendRuntime"),
            self.tr("ui.settingsDesc.backend"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(if app.backend_connected {
                    self.badge(self.tr("ui.settings.connected"), true)
                } else {
                    self.badge(self.tr("ui.settings.offline"), false)
                })
                .children(
                    app.backend_version
                        .as_ref()
                        .map(|v| self.text_control(format!("v{v}"))),
                ),
        );
        let restart = self.row(
            self.tr("ui.settings.restartBackend"),
            self.tr("ui.settingsDesc.restart"),
            self.action_button(
                "restart-backend",
                if self.backend_restarting {
                    self.tr("ui.settings.restarting")
                } else {
                    self.tr("ui.settings.restartBackend")
                },
                false,
            )
            .on_click(cx.listener(|s, _, _, cx| s.restart_backend(cx))),
        );
        let kill = self.row(
            self.tr("ui.settings.forceKill"),
            self.tr("ui.settingsDesc.forceKill"),
            self.action_button("force-kill", self.tr("ui.settings.forceKill"), false)
                .bg(rgba(0xff5c5c1f))
                .border_color(rgba(0xff5c5c8c))
                .text_color(rgb(0xff8585))
                .on_click(cx.listener(|s, _, _, cx| {
                    s.confirm = Some(ConfirmAction::ForceKill);
                    cx.notify()
                })),
        );
        let perf = self.row(
            self.tr("ui.settings.lowPerformance"),
            self.tr("ui.settingsDesc.lowPerformance"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge(
                    if app.low_performance {
                        "Reduced Effects"
                    } else {
                        "Full Effects"
                    },
                    !app.low_performance,
                ))
                .child(
                    self.toggle("low-performance", app.low_performance, cx, |s, cx| {
                        let next = !s.app.low_performance;
                        s.app.low_performance = next;
                        toast::success(
                            cx,
                            if next {
                                "Low Performance Mode enabled"
                            } else {
                                "Low Performance Mode disabled"
                            },
                        );
                        cx.emit(SettingsPreviewEvent::LowPerformance(next));
                        cx.notify();
                    }),
                ),
        );
        let dev = self.row(
            self.tr("ui.settings.developerTools"),
            self.tr("ui.settingsDesc.developer"),
            self.toggle("developer", app.developer_mode, cx, |s, cx| {
                let next = !s.app.developer_mode;
                s.app.developer_mode = next;
                cx.emit(SettingsPreviewEvent::DeveloperMode(next));
                cx.notify()
            }),
        );
        let logs = self.row(
            self.tr("ui.settings.graphicsLogs"),
            "Opt in to DXMT graphics logs for future launches. Off by default to keep routine launches quiet unless requested.",
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge(
                    if self.graphics_logs {
                        "Logs On"
                    } else {
                        "Default Off"
                    },
                    !self.graphics_logs,
                ))
                .child(self.toggle("graphics-logs", self.graphics_logs, cx, |s, cx| {
                    let next = !s.graphics_logs;
                    s.set_graphics_logs(next, cx)
                })),
        );
        let folders = self.row(
            self.tr("ui.settings.dataFolder"),
            self.tr("ui.settingsDesc.dataFolder"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(
                    self.action_button("open-data", self.tr("ui.settings.openData"), false)
                        .on_click(cx.listener(|s, _, _, cx| s.open_folder(false, cx))),
                )
                .child(
                    self.action_button("open-logs", self.tr("ui.settings.openLogs"), false)
                        .on_click(cx.listener(|s, _, _, cx| s.open_folder(true, cx))),
                ),
        );
        let repair = self.row(
            self.tr("ui.settings.dataAccess"),
            self.tr("ui.settingsDesc.dataAccess"),
            self.action_button("repair-data", self.tr("ui.settings.repairVerify"), true)
                .on_click(cx.listener(|s, _, _, cx| s.repair_data_access(cx))),
        );
        let cache_row = |this: &Self,
                         id: &'static str,
                         title: gpui::SharedString,
                         desc: gpui::SharedString,
                         cache: Option<&Value>,
                         kind: &'static str,
                         apps: bool,
                         cx: &mut Context<Self>| {
            let (status, good) = format_cache(cache);
            let app_count = cache
                .and_then(|c| c.get("apps"))
                .and_then(Value::as_u64)
                .filter(|n| *n > 0);
            let modified = cache
                .and_then(|c| c.get("last_modified"))
                .and_then(Value::as_str)
                .map(str::to_owned);
            this.row(
                title,
                desc,
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .child(this.badge(status, good))
                    .children(
                        app_count
                            .filter(|_| apps)
                            .map(|n| this.text_control(format!("{n} apps"))),
                    )
                    .children(modified.map(|m| this.text_control(m)))
                    .child(
                        this.action_button(id, this.tr("ui.settings.clear"), false)
                            .on_click(cx.listener(move |s, _, _, cx| s.clear_cache(kind, cx))),
                    ),
            )
        };
        let shader_cache = self.shader_cache.clone();
        let pipeline_cache = self.pipeline_cache.clone();
        let shader = cache_row(
            self,
            "clear-shader",
            self.tr("ui.settings.shaderCache"),
            self.tr("ui.settingsDesc.shader"),
            shader_cache.as_ref(),
            "shader",
            true,
            cx,
        );
        let pipeline = cache_row(
            self,
            "clear-pipeline",
            self.tr("ui.settings.pipelineCache"),
            self.tr("ui.settingsDesc.pipeline"),
            pipeline_cache.as_ref(),
            "pipeline",
            false,
            cx,
        );
        let status = app.update_status.clone().unwrap_or(Value::Null);
        let status_ok = is_ok(&status);
        let available = status_ok && status.get("available").and_then(Value::as_bool) == Some(true);
        let latest = status
            .get("latest_version")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned();
        let current = status
            .get("current_version")
            .and_then(Value::as_str)
            .map(str::to_owned);
        let fex_available = status.get("fex_available").and_then(Value::as_bool) == Some(true);
        let version = self.row(
            self.tr("ui.settings.version"),
            if available {
                gpui::SharedString::from(format!(
                    "v{latest} available (current: v{})",
                    current.clone().unwrap_or_default()
                ))
            } else if status_ok || !app.live {
                self.tr("ui.settings.upToDate")
            } else {
                self.tr("ui.settings.couldNotCheck")
            },
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge(
                    format!("v{}", current.unwrap_or_else(|| "unknown".into())),
                    status_ok,
                ))
                .when(!app.update_downloading, |d| {
                    d.child(
                        self.action_button("check-updates", self.tr("ui.settings.checkNow"), false)
                            .on_click(cx.listener(|s, _, _, cx| s.check_for_updates(cx))),
                    )
                }),
        );
        let update =
            (available && !app.update_downloading).then(|| {
                self.row(
                    self.tr("ui.settings.downloadUpdate"),
                    format!("v{latest} is ready to download"),
                    self.action_button(
                        "download-update",
                        self.tr("ui.settings.downloadInstall"),
                        true,
                    )
                    .on_click(cx.listener(|_, _, _, cx| {
                        cx.emit(SettingsPreviewEvent::StartUpdate("regular"))
                    })),
                )
            });
        let fex = (available && fex_available && !app.update_downloading).then(|| {
            self.row(
                self.tr("ui.settings.fexUpdate"),
                "macOS 27+ only · experimental and potentially less stable than baseline",
                self.action_button("update-fex", self.tr("ui.settings.updateFex"), false)
                    .on_click(cx.listener(|s, _, _, cx| {
                        s.confirm = Some(ConfirmAction::FexUpdate);
                        cx.notify();
                    })),
            )
        });
        let progress = app.update_downloading.then(|| {
            self.row(
                if app.update_message.is_empty() {
                    self.tr("ui.settings.updating")
                } else {
                    app.update_message.clone().into()
                },
                "Do not close MetalSharp during an update.",
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .child(
                        div()
                            .w(px(120.))
                            .h(px(7.))
                            .rounded(px(999.))
                            .bg(rgb(p.hover))
                            .child(
                                div()
                                    .h(px(7.))
                                    .w(gpui::relative((app.update_progress / 100.).clamp(0.0, 1.0)))
                                    .rounded(px(999.))
                                    .bg(rgb(p.accent)),
                            ),
                    )
                    .child(self.text_control(format!("{}%", app.update_progress.round() as u32))),
            )
        });
        let uninstall = self.row(
            self.tr("ui.settings.uninstall"),
            self.tr("ui.settingsDesc.uninstall"),
            self.action_button("uninstall", self.tr("ui.settings.uninstall"), false)
                .bg(rgba(0xff5c5c1f))
                .border_color(rgba(0xff5c5c8c))
                .text_color(rgb(0xff8585))
                .on_click(cx.listener(|s, _, _, cx| {
                    s.confirm = Some(ConfirmAction::Uninstall);
                    cx.notify()
                })),
        );
        let mut body = div()
            .id("settings-preview-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .grid()
            .grid_cols(if window.viewport_size().width <= px(900.) {
                1
            } else {
                2
            })
            .gap(px(14.))
            .px(px(20.))
            .py(px(16.));
        body = body
            .child(self.card(
                self.tr("settings.steamIntegration"),
                div().child(steam_row).child(device_row),
                false,
            ))
            .child(self.card(
                self.tr("settings.epicIntegration"),
                div().child(epic_row),
                false,
            ))
            .child(
                self.card(
                    self.tr("settings.steam"),
                    div()
                        .child(wine_row)
                        .when(!app.wine_steam_installed, |d| d.child(setup_row))
                        .child(mac_row)
                        .child(retina),
                    false,
                ),
            )
            .child(
                self.card(
                    self.tr("settings.backend"),
                    div()
                        .child(backend)
                        .child(restart)
                        .child(kill)
                        .child(perf)
                        .child(dev)
                        .when(app.developer_mode, |d| d.child(logs)),
                    false,
                ),
            )
            .child(self.card(
                self.tr("settings.dataPermissions"),
                div().child(folders).child(repair),
                false,
            ))
            .child(self.card(
                self.tr("settings.cache"),
                div().child(shader).child(pipeline),
                false,
            ));
        let mut updates = div().child(version);
        if let Some(r) = update {
            updates = updates.child(r)
        }
        if let Some(r) = fex {
            updates = updates.child(r)
        }
        if let Some(r) = progress {
            updates = updates.child(r)
        }
        body = body
            .child(self.card(self.tr("settings.updates"), updates, false))
            .child(self.card(
                self.tr("ui.settings.dangerZone"),
                div().child(uninstall),
                true,
            ));
        let mut panel = div()
            .id("settings-panel")
            .occlude()
            .on_click(|_, _, cx| cx.stop_propagation())
            .flex()
            .flex_col()
            .w(px(1080.))
            .max_w(gpui::relative(1.))
            .max_h(px((f32::from(window.viewport_size().height)
                - if f32::from(window.viewport_size().width) <= 780.0 {
                    154.0
                } else {
                    92.0
                })
            .max(120.)))
            .rounded(px(14.))
            .border_1()
            .border_color(rgba((p.accent << 8) | 0xff))
            .bg(rgb(p.control_bg))
            .text_color(rgb(p.control_text))
            .shadow(vec![gpui::BoxShadow {
                color: rgba(0x0000008c).into(),
                offset: gpui::point(px(0.), px(22.)),
                blur_radius: px(55.),
                spread_radius: px(0.),
            }]);
        let header = div()
            .flex_none()
            .flex()
            .items_center()
            .justify_between()
            .gap(px(16.))
            .px(px(22.))
            .pt(px(18.))
            .pb(px(14.))
            .border_b_1()
            .border_color(rgba(p.control_border))
            .bg(rgba((p.hover << 8) | 0x66))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(4.))
                    .child(
                        div()
                            .text_color(rgba((p.control_text << 8) | 0x99))
                            .font_family("Georgia")
                            .text_size(px(10.))
                            .child("M E T A L S H A R P"),
                    )
                    .child(
                        div()
                            .font_family("Georgia")
                            .text_size(px(26.))
                            .font_weight(FontWeight::MEDIUM)
                            .line_height(px(26.0))
                            .child(self.tr("settings.title")),
                    ),
            )
            .child(
                self.action_button("close-settings", "×", false)
                    .on_click(cx.listener(|s, _, _, cx| s.dismiss(cx))),
            );
        panel = panel.child(header);
        if let Some(note) = self.notice {
            panel = panel.child(
                div()
                    .flex_none()
                    .px(px(20.))
                    .py(px(7.))
                    .bg(rgba((p.accent << 8) | 0x17))
                    .text_color(rgb(p.control_text))
                    .text_size(px(10.))
                    .child(note),
            );
        }
        panel = panel.child(body);
        let confirmation = self
            .confirm
            .map(|_| gpui::deferred(self.confirmation_overlay(cx)).with_priority(240));
        let bounds = window.viewport_size();
        div()
            .id("settings-overlay")
            .absolute()
            .inset_0()
            .flex()
            .justify_center()
            .items_start()
            .pt(if f32::from(bounds.width) <= 780. {
                px(130.)
            } else {
                px(68.)
            })
            .px(px(24.))
            .pb(px(24.))
            .bg(rgba(0x06080a94))
            .on_click(cx.listener(|this, _, _, cx| this.dismiss(cx)))
            .child(
                panel
                    .track_focus(&self.focus.get_or_insert_with(|| cx.focus_handle()).clone())
                    .on_key_down(cx.listener(|this, event: &gpui::KeyDownEvent, _, cx| {
                        if event.keystroke.key == "escape" {
                            if this.language_menu {
                                this.language_menu = false;
                            } else if this.confirm.is_some() {
                                this.confirm = None;
                            } else {
                                this.dismiss(cx);
                            }
                            cx.notify();
                        }
                    })),
            )
            .children(confirmation)
    }
}

fn format_bytes(bytes: u64) -> String {
    if bytes < 1024 {
        format!("{bytes} B")
    } else if bytes < 1024 * 1024 {
        format!("{:.1} KB", bytes as f64 / 1024.0)
    } else if bytes < 1024 * 1024 * 1024 {
        format!("{:.1} MB", bytes as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn safe_defaults_and_language_fixture() {
        let s = SettingsPreview::new();
        assert_eq!(s.language, "en");
        assert!(!s.retina);
        assert!(!s.app.wine_steam_installed);
        assert_eq!(LANGUAGES.len(), 20);
    }
    #[test]
    fn all_settings_locales_have_source_keys_and_english_fallback() {
        let mut preview = SettingsPreview::new();
        for (language, _) in LANGUAGES {
            preview.language = language;
            assert_eq!(preview.locales[language].len(), 77);
            assert!(!preview.tr("settings.title").is_empty());
        }
        preview.language = "unsupported";
        assert_eq!(preview.tr("settings.title").as_ref(), "Settings");
        assert_eq!(format_bytes(1024), "1.0 KB");
    }
    #[test]
    fn cache_summary_matches_overlay_text() {
        assert_eq!(format_cache(None).0, "...");
        assert_eq!(format_cache(Some(&json!({"status":"empty"}))).0, "Empty");
        assert_eq!(
            format_cache(Some(&json!({"status":"active","bytes":2048,"files":3}))).0,
            "2.0 KB · 3 files"
        );
    }
}
