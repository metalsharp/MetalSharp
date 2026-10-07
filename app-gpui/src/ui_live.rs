//! Production behaviour for the approved GPUI shell, ported 1:1 from the
//! Electron renderer (`App.vue`, `SetupWizard.vue`, `LibraryView.vue`,
//! `LibraryTopbar.vue`, `StreamingOverlay.vue`) and main process.
use super::*;
use crate::library_model::{self, LibGame};
use crate::live::{self, Live, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant, SystemTime};

const MS: fn(u64) -> Duration = Duration::from_millis;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(super) enum LogClass {
    Info,
    Success,
    Warn,
    Error,
    Active,
}

/// SetupWizard.vue local state.
#[derive(Default)]
pub(super) struct SetupFlow {
    pub install_progress: usize,
    pub install_status: String,
    pub installing: bool,
    pub install_logs: Vec<(String, LogClass)>,
    pub install_current: String,
    pub install_failed: bool,
    pub log_open: bool,
    pub steam_failed: bool,
    pub finishing: bool,
    pub steam_installed: bool,
    pub steam_installing: bool,
    pub steam_install_stage: String,
    /// Reopened from Settings: may be closed without finishing.
    pub dismissible: bool,
    pub generation: u64,
    /// Steam installer Wine processes already brought to the front once.
    pub installer_fronted: HashSet<i64>,
}

impl SetupFlow {
    pub fn runtime_ready(&self) -> bool {
        self.install_status == "complete"
    }
}

#[derive(Default, Clone)]
pub(super) struct StreamingState {
    pub status: Option<crate::streaming::StreamingStatus>,
    pub pairing: bool,
    pub launching: bool,
    pub stopping: bool,
    pub installing: bool,
    pub session: u64,
    pub polling: bool,
}

/// App.vue + LibraryView.vue reactive state.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum SteamPending {
    Starting,
    Stopping,
}

pub(super) struct LiveState {
    pub enabled: bool,
    /// App.vue `showStartupVideo`: attach the native overlay on next render.
    pub intro_pending: bool,
    /// LibraryView localStorage `defaultRulesAppliedKey`.
    pub default_rules_applied: bool,
    /// Run the startup update check once the first library load returns.
    pub update_check_after_library: bool,
    pub booting: bool,
    pub boot_error: Option<String>,
    pub migration: bool,
    pub backend_connected: bool,
    pub backend_version: Option<String>,
    pub wine_steam_installed: bool,
    pub wine_steam_running: bool,
    /// A Start/Stop Steam request is in flight; buttons show it and ignore clicks.
    pub steam_pending: Option<SteamPending>,
    pub mac_steam_installed: bool,
    pub mac_steam_running: bool,
    pub ubisoft_installed: bool,
    pub ubisoft_running: bool,
    pub ubisoft_installing: bool,
    /// Ubisoft Connect was started with Launch Ubisoft, so the header's
    /// primary button is Stop Ubisoft until Connect stops or is seen closed.
    pub ubisoft_primary: bool,
    pub library: Vec<LibGame>,
    pub library_loaded: bool,
    pub installed_count: usize,
    library_in_flight: bool,
    pending_force_reload: bool,
    pub steam_api_key: Option<String>,
    pub device_name: String,
    pub update_status: Option<Value>,
    pub update_downloading: bool,
    pub update_progress: f32,
    pub update_message: String,
    pub update_dismissed: bool,
    pub update_confirm: Option<&'static str>,
    pub running: HashMap<u64, u64>,
    pub launching: Option<u64>,
    running_poll_in_flight: bool,
    pub selected_pipeline: String,
    pub pipeline_saving: bool,
    pub metal_fx_mode: &'static str,
    pub metal_fx_busy: bool,
    pub controller_input: &'static str,
    pub controller_busy: bool,
    pub msync: bool,
    pub msync_busy: bool,
    pub steam_emu_active: bool,
    pub steam_emu_busy: bool,
    pub collection_saving: HashSet<u64>,
    /// steamcmd install jobs by appid (latest `/steamcmd/installs` entry).
    pub steamcmd_installs: HashMap<u64, Value>,
    pub steamcmd_polling: bool,
    pub play_history: HashMap<String, i64>,
    watch_in_flight: bool,
    health_started: bool,
    steamapps_signature: Option<Vec<(String, u64)>>,
    steamapps_reload_at: Option<Instant>,
    grid_art_signature: Option<Vec<(std::path::PathBuf, u128)>>,
    gj_downloads: HashMap<u64, (Option<u64>, String, std::path::PathBuf, Option<u64>)>,
    pub steam_fix_busy: bool,
    icon_refresh_attempts: u32,
    icon_refresh_scheduled: bool,
    featured_key: Option<(bool, u64)>,
    pub streaming: StreamingState,
    pub setup: SetupFlow,
}

impl LiveState {
    /// Header shows Stop Ubisoft as its primary button (Steam moves into the
    /// dropdown) while Connect, started by Launch Ubisoft, runs or installs.
    pub(super) fn ubisoft_is_primary(&self) -> bool {
        self.ubisoft_primary && (self.ubisoft_running || self.ubisoft_installing)
    }

    /// Apply an Ubisoft Connect status poll; a closed Connect restores Start Steam.
    fn apply_ubisoft_status(&mut self, installed: bool, running: bool, installing: bool) {
        self.ubisoft_installed = installed;
        self.ubisoft_running = running;
        self.ubisoft_installing = installing;
        if !running && !installing {
            self.ubisoft_primary = false;
        }
    }
}

impl Default for LiveState {
    fn default() -> Self {
        Self {
            enabled: false,
            intro_pending: false,
            default_rules_applied: false,
            update_check_after_library: false,
            booting: false,
            boot_error: None,
            migration: false,
            backend_connected: false,
            backend_version: None,
            wine_steam_installed: false,
            wine_steam_running: false,
            steam_pending: None,
            mac_steam_installed: false,
            mac_steam_running: false,
            ubisoft_installed: false,
            ubisoft_running: false,
            ubisoft_installing: false,
            ubisoft_primary: false,
            library: Vec::new(),
            library_loaded: false,
            installed_count: 0,
            library_in_flight: false,
            pending_force_reload: false,
            steam_api_key: None,
            device_name: String::new(),
            update_status: None,
            update_downloading: false,
            update_progress: 0.0,
            update_message: String::new(),
            update_dismissed: false,
            update_confirm: None,
            running: HashMap::new(),
            launching: None,
            running_poll_in_flight: false,
            selected_pipeline: "auto".into(),
            pipeline_saving: false,
            metal_fx_mode: "2.0",
            metal_fx_busy: false,
            controller_input: "off",
            controller_busy: false,
            msync: true,
            msync_busy: false,
            steam_emu_active: false,
            steam_emu_busy: false,
            collection_saving: HashSet::new(),
            steamcmd_installs: HashMap::new(),
            steamcmd_polling: false,
            play_history: HashMap::new(),
            watch_in_flight: false,
            health_started: false,
            steamapps_signature: None,
            steamapps_reload_at: None,
            grid_art_signature: None,
            gj_downloads: HashMap::new(),
            steam_fix_busy: false,
            icon_refresh_attempts: 0,
            icon_refresh_scheduled: false,
            featured_key: None,
            streaming: StreamingState::default(),
            setup: SetupFlow::default(),
        }
    }
}

fn ui_state_path(home: &std::path::Path) -> std::path::PathBuf {
    home.join("gpui-ui-state.json")
}

impl MetalSharpApp {
    // ───────────────────────────── boot ─────────────────────────────

    /// Production launch: own backend on :9274 with `~/.metalsharp`.
    pub fn new_live(
        config: crate::backend_host::HostConfig,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut app = Self::new();
        app.games.clear();
        app.live.enabled = true;
        app.live.booting = true;
        app.show_setup = false;
        app.step = 0;
        let live = Live::new(config);
        cx.set_global(live.clone());
        app.load_ui_state(&live);
        // Re-render when cached artwork resolves.
        if let Some(cache) = crate::artwork::cache(cx) {
            app.observers
                .push(cx.observe(&cache, |_, _, cx| cx.notify()));
        }
        // Window focus reloads the library (App.vue wireSteamappsRefresh).
        cx.observe_window_activation(window, |this, window, cx| {
            if window.is_window_active() && this.live.health_started {
                this.live.steamapps_reload_at = Some(Instant::now() + MS(500));
                let _ = cx;
            }
        })
        .detach();
        cx.on_app_quit(|_, cx| {
            // Electron cleanup(): terminate the owned backend before exiting,
            // without holding the closing window open while it shuts down.
            if let Some(live) = Live::get(cx) {
                live.stop_backend_detached();
            }
            async {}
        })
        .detach();
        cx.spawn(async move |this, cx| {
            let started = {
                let live = live.clone();
                cx.background_executor()
                    .spawn(async move {
                        // Electron retries the backend start once (index.ts:1373-1384).
                        live.start_backend().or_else(|_| live.start_backend())
                    })
                    .await
            };
            let _ = this.update(cx, |this, cx| match started {
                Ok(()) => this.after_backend_started(cx),
                Err(error) => {
                    this.live.booting = false;
                    this.live.boot_error = Some(error);
                    cx.notify();
                }
            });
        })
        .detach();
        app.start_tick_loop(cx);
        app
    }

    fn load_ui_state(&mut self, live: &Live) {
        let Ok(text) = std::fs::read_to_string(ui_state_path(&live.home())) else {
            return;
        };
        let Ok(value) = serde_json::from_str::<Value>(&text) else {
            return;
        };
        if let Some(history) = value.get("playHistory").and_then(Value::as_object) {
            self.live.play_history = history
                .iter()
                .filter_map(|(k, v)| v.as_i64().map(|v| (k.clone(), v)))
                .collect();
        }
        if let Some(theme) = value.get("theme").and_then(Value::as_str) {
            if let Some(found) = PreviewTheme::ALL.iter().find(|t| t.storage_id() == theme) {
                self.theme = *found;
            }
        }
        if let Some(language) = value.get("language").and_then(Value::as_str) {
            if let Some((code, _)) = LANGUAGES.iter().find(|(code, _)| *code == language) {
                self.selected_language = code;
                if let Some(copy) = self.locales.get(*code) {
                    self.copy = copy.clone();
                }
            }
        }
        self.developer_mode = value
            .get("developerMode")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        self.low_performance = value
            .get("lowPerformanceMode")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        self.startup_video_seen = value
            .get("startupVideoSeen")
            .and_then(Value::as_bool)
            .unwrap_or(false);
        self.live.default_rules_applied = value
            .get("defaultRulesApplied")
            .and_then(Value::as_bool)
            .unwrap_or(false);
    }

    pub(super) fn save_ui_state(&self, cx: &App) {
        let Some(live) = Live::get(cx) else { return };
        let state = json!({
            "playHistory": self.live.play_history,
            "theme": self.theme.storage_id(),
            "language": self.selected_language,
            "developerMode": self.developer_mode,
            "lowPerformanceMode": self.low_performance,
            "startupVideoSeen": self.startup_video_seen,
            "defaultRulesApplied": self.live.default_rules_applied,
        });
        let path = ui_state_path(&live.home());
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let _ = std::fs::write(path, serde_json::to_vec_pretty(&state).unwrap_or_default());
    }

    pub(super) fn after_backend_started(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        ensure_metalsharp_dirs(&live.home());
        // App.vue onMounted: checkBackend → migration → first launch → setup/initApp.
        self.check_backend(cx);
        let home = live.home();
        cx.spawn(async move |this, cx| {
            let migration = cx
                .background_executor()
                .spawn({
                    let live = live.clone();
                    async move { check_needs_migration(&live) }
                })
                .await;
            let _ = this.update(cx, |this, cx| {
                if migration {
                    this.live.booting = false;
                    this.live.migration = true;
                    this.start_migration(cx);
                    cx.notify();
                    return;
                }
                let first_launch = is_first_launch(&home);
                if first_launch && !this.startup_video_seen {
                    this.live.intro_pending = true;
                }
                live::call(
                    cx,
                    "GET",
                    "/setup/state",
                    None,
                    live::DEFAULT_TIMEOUT,
                    move |this, state, cx| {
                        if let Some(name) = state
                            .as_ref()
                            .and_then(|s| s.get("deviceName"))
                            .and_then(Value::as_str)
                            .filter(|n| !n.is_empty())
                        {
                            this.live.device_name = name.to_owned();
                        }
                        let migration_required = state
                            .as_ref()
                            .and_then(|s| s.get("runtimeMigrationRequired"))
                            .and_then(Value::as_bool)
                            == Some(true);
                        this.live.booting = false;
                        if first_launch || migration_required {
                            this.open_setup(false, cx);
                        } else {
                            // App.vue awaits initApp (library load) before
                            // checkForUpdates; on the single-threaded backend a
                            // network update check must not delay the library.
                            this.live.update_check_after_library = true;
                            this.init_app(cx);
                        }
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    /// Attach the startup video (needs the window) and watch for its end.
    pub(super) fn show_startup_video(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.live.intro_pending = false;
        if !crate::intro_video::show(window) {
            self.finish_startup_video(cx);
            return;
        }
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(MS(50)).await;
                if crate::intro_video::poll_finished() {
                    let _ = this.update(cx, |this, cx| this.finish_startup_video(cx));
                    break;
                }
            }
        })
        .detach();
    }

    /// `finishStartupVideo`.
    fn finish_startup_video(&mut self, cx: &mut Context<Self>) {
        self.startup_video_seen = true;
        self.save_ui_state(cx);
        cx.notify();
    }

    pub(super) fn open_setup(&mut self, dismissible: bool, cx: &mut Context<Self>) {
        let generation = self.live.setup.generation + 1;
        self.live.setup = SetupFlow {
            dismissible,
            generation,
            ..Default::default()
        };
        self.show_setup = true;
        self.step = 0;
        self.settings_open = false;
        self.streaming_open = false;
        self.active_tab = LibraryTab::Play;
        cx.notify();
    }

    pub(super) fn check_backend(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/status",
            None,
            live::DEFAULT_TIMEOUT,
            |this, status, cx| {
                this.live.backend_connected = status.as_ref().is_some_and(is_ok);
                if let Some(version) = status
                    .as_ref()
                    .and_then(|s| s.get("version"))
                    .and_then(Value::as_str)
                {
                    this.live.backend_version = Some(version.to_owned());
                }
                cx.notify();
            },
        );
    }

    /// App.vue `initApp`.
    pub(super) fn init_app(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/steam/api-key",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                this.live.steam_api_key = result
                    .as_ref()
                    .and_then(|r| r.get("key"))
                    .and_then(Value::as_str)
                    .map(str::to_owned);
                this.load_library(false, cx);
                live::call(
                    cx,
                    "GET",
                    "/setup/state",
                    None,
                    live::DEFAULT_TIMEOUT,
                    |this, state, cx| {
                        if let Some(name) = state
                            .as_ref()
                            .and_then(|s| s.get("deviceName"))
                            .and_then(Value::as_str)
                            .filter(|n| !n.is_empty())
                        {
                            this.live.device_name = name.to_owned();
                        }
                        this.start_health_polling();
                        cx.notify();
                    },
                );
            },
        );
        self.load_game_settings(cx);
    }

    fn start_health_polling(&mut self) {
        // Interval work runs from the shared tick loop once this flag is set.
        self.live.health_started = true;
    }

    // ─────────────────────────── tick loop ───────────────────────────

    fn start_tick_loop(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            let mut tick: u64 = 0;
            loop {
                cx.background_executor().timer(MS(500)).await;
                tick += 1;
                let alive = this.update(cx, |this, cx| {
                    if crate::lifecycle::quit_requested() {
                        if let Some(live) = Live::get(cx) {
                            live.stop_backend_detached();
                        }
                        cx.quit();
                        return;
                    }
                    this.on_tick(tick, cx);
                });
                if alive.is_err() {
                    break;
                }
            }
        })
        .detach();
    }

    fn on_tick(&mut self, tick: u64, cx: &mut Context<Self>) {
        if !self.live.enabled || self.live.booting || self.live.boot_error.is_some() {
            // Never replay a Cmd+Option+Q pressed before the backend was ready.
            let _ = crate::hotkeys::take_force_quit_request();
            return;
        }
        if crate::hotkeys::take_force_quit_request() {
            self.force_quit_running_games(cx);
        }
        self.pump_gamejolt_downloads(cx);
        let library_mounted = !self.show_setup
            && !self.live.migration
            && matches!(self.active_tab, LibraryTab::Play | LibraryTab::Collection);
        // LibraryView: /game/running every 2 s while mounted.
        if library_mounted && tick % 4 == 0 {
            self.refresh_running_games(cx);
        }
        if !self.live.health_started {
            return;
        }
        if tick % 10 == 0 {
            self.refresh_steam_status(cx);
        }
        if tick % 30 == 0 {
            self.watch_steamapps(cx);
        }
        if tick % 240 == 0 {
            self.health_check(cx);
        }
        if tick % 1800 == 0 {
            self.check_for_updates(cx);
        }
        // main-process steamapps fs.watch equivalent (2 s debounce + 2.5 s renderer debounce).
        if tick % 4 == 0 {
            self.poll_steamapps_dir(cx);
            self.poll_grid_art(cx);
        }
        if let Some(due) = self.live.steamapps_reload_at {
            if Instant::now() >= due {
                self.live.steamapps_reload_at = None;
                self.load_library(false, cx);
            }
        }
    }

    fn poll_steamapps_dir(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        let dir = live
            .home()
            .join("prefix-steam/drive_c/Program Files (x86)/Steam/steamapps");
        let mut signature: Vec<(String, u64)> = match std::fs::read_dir(&dir) {
            Ok(entries) => entries
                .filter_map(Result::ok)
                .filter_map(|entry| {
                    let name = entry.file_name().to_string_lossy().into_owned();
                    if !(name.starts_with("appmanifest_") && name.ends_with(".acf")) {
                        return None;
                    }
                    let modified = entry
                        .metadata()
                        .ok()
                        .and_then(|m| m.modified().ok())
                        .and_then(|t| t.duration_since(SystemTime::UNIX_EPOCH).ok())
                        .map(|d| d.as_millis() as u64)
                        .unwrap_or(0);
                    Some((name, modified))
                })
                .collect(),
            Err(_) => return,
        };
        signature.sort();
        match &self.live.steamapps_signature {
            Some(previous) if *previous != signature => {
                self.live.steamapps_reload_at = Some(Instant::now() + MS(4500));
            }
            _ => {}
        }
        self.live.steamapps_signature = Some(signature);
    }

    fn health_check(&mut self, cx: &mut Context<Self>) {
        let previous = self.live.backend_connected;
        let alive = Live::get(cx).is_some_and(|live| live.backend_alive());
        live::call(
            cx,
            "GET",
            "/status",
            None,
            MS(1500),
            move |this, status, cx| {
                let connected = alive || status.as_ref().is_some_and(is_ok);
                this.live.backend_connected = connected;
                this.live.backend_version = status
                    .as_ref()
                    .and_then(|s| s.get("version"))
                    .and_then(Value::as_str)
                    .map(str::to_owned)
                    .filter(|_| connected);
                if previous && !connected {
                    toast::error(cx, "Backend connection lost");
                } else if !previous && connected {
                    toast::success(cx, "Backend connected");
                }
                cx.notify();
            },
        );
    }

    /// Electron `handleGameJoltDownload` + main `organizeGameJoltDownload`.
    fn pump_gamejolt_downloads(&mut self, cx: &mut Context<Self>) {
        if let Some(live) = Live::get(cx) {
            // Follow storage changes made while the browser is open.
            crate::mini_browser::set_gamejolt_download_dir(
                crate::host_actions::gamejolt_download_dir(&live.home()),
            );
        }
        use crate::mini_browser::GameJoltDownloadEvent as E;
        for event in crate::mini_browser::take_gamejolt_download_events() {
            match event {
                E::Started {
                    id,
                    filename,
                    path,
                    total,
                } => {
                    let toast = toast::show_download(cx, format!("Downloading {filename}..."), 0.0);
                    self.live
                        .gj_downloads
                        .insert(id, (toast, filename, path, total));
                }
                E::Finished { id, filename, path } => {
                    let toast = self.live.gj_downloads.remove(&id).and_then(|entry| entry.0);
                    if let Some(toast) = toast {
                        toast::update_download(cx, toast, format!("Finishing {filename}..."), 0.99);
                    }
                    let Some(live) = Live::get(cx) else { continue };
                    let dir = crate::host_actions::gamejolt_download_dir(&live.home());
                    cx.spawn(async move |this, cx| {
                        let (organized, synced) = cx
                            .background_executor()
                            .spawn(async move {
                                let organized = crate::host_actions::organize_gamejolt_download(
                                    &path, &filename, &dir,
                                );
                                let synced = live
                                    .request("POST", "/gamejolt/sync", None, live::DEFAULT_TIMEOUT)
                                    .map(live::unwrap_data)
                                    .map(|r| r.get("ok").and_then(Value::as_bool) != Some(false))
                                    .unwrap_or(true);
                                (
                                    organized
                                        .map(|_| filename.clone())
                                        .map_err(|e| (filename, e)),
                                    synced,
                                )
                            })
                            .await;
                        let _ = this.update(cx, |this, cx| {
                            match (&organized, synced, toast) {
                                (Ok(name), true, Some(t)) => toast::finish_download(
                                    cx,
                                    t,
                                    format!("{name} downloaded"),
                                    true,
                                ),
                                (Ok(name), false, Some(t)) => toast::finish_download(
                                    cx,
                                    t,
                                    format!("Could not download {name}"),
                                    false,
                                ),
                                (Err((_, error)), _, Some(t)) => {
                                    toast::finish_download(cx, t, error.clone(), false)
                                }
                                _ => {}
                            }
                            if let Some(sharp) = this.sharp_preview.clone() {
                                sharp.update(cx, |sharp, cx| sharp.reload_gamejolt(cx));
                            }
                        });
                    })
                    .detach();
                }
                E::Failed {
                    id,
                    filename,
                    error,
                } => {
                    if let Some((Some(toast), ..)) = self.live.gj_downloads.remove(&id) {
                        let message = if error.is_empty() {
                            format!("Could not download {filename}")
                        } else {
                            error
                        };
                        toast::finish_download(cx, toast, message, false);
                    }
                }
            }
        }
        for (toast, filename, path, total) in self.live.gj_downloads.values() {
            let (Some(toast), Some(total)) = (toast, total) else {
                continue;
            };
            let received = std::fs::metadata(path).map(|m| m.len()).unwrap_or(0);
            let progress = (received as f32 / *total as f32).min(0.99);
            toast::update_download(
                cx,
                *toast,
                format!(
                    "Downloading {filename} — {}%",
                    (received as f64 / *total as f64 * 100.0).round() as u64
                ),
                progress,
            );
        }
    }

    /// main `forceQuitRunningGames` (global Cmd+Option+Q).
    fn force_quit_running_games(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .spawn(async move {
                    let post = |path: &str, timeout: u64| {
                        let _ = live.request("POST", path, None, MS(timeout));
                    };
                    post("/sharp-library/stop-all", 10_000);
                    post("/gamejolt/stop-all", 2_000);
                    let running: Vec<u64> = live
                        .request("GET", "/game/running", None, MS(2_000))
                        .ok()
                        .map(live::unwrap_data)
                        .filter(is_ok)
                        .and_then(|r| r.get("running").and_then(Value::as_array).cloned())
                        .unwrap_or_default()
                        .iter()
                        .filter_map(|g| g.get("appid").and_then(Value::as_u64))
                        .collect();
                    // EVE Online, Odyssey, Marvel Rivals, Baldur's Gate 3 use per-game stop paths.
                    for appid in [8500u64, 812140, 2767030, 1086940] {
                        if running.contains(&appid) {
                            let _ = live.request(
                                "POST",
                                "/kill",
                                Some(&json!({"appid": appid})),
                                MS(5_000),
                            );
                        }
                    }
                    post("/games/force-quit", 30_000);
                    post("/sharp-library/epic/stop-all", 30_000);
                    post("/sharp-library/gog/stop-all", 30_000);
                })
                .await;
            let _ = this.update(cx, |this, cx| {
                this.refresh_running_games(cx);
            });
        })
        .detach();
    }

    // ─────────────────────────── library ───────────────────────────

    /// App.vue `refreshSteamStatus`.
    pub(super) fn refresh_steam_status(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/steam/status",
            None,
            live::DEFAULT_TIMEOUT,
            |this, status, cx| {
                if let Some(status) = status {
                    let flag =
                        |key: &str| status.get(key).and_then(Value::as_bool).unwrap_or(false);
                    this.live.wine_steam_installed = flag("installed");
                    this.live.wine_steam_running = flag("running");
                    this.live.mac_steam_installed = flag("mac_installed");
                    this.live.mac_steam_running = flag("mac_running");
                    cx.notify();
                }
            },
        );
        live::call(
            cx,
            "GET",
            "/ubisoft/status",
            None,
            live::DEFAULT_TIMEOUT,
            |this, status, cx| {
                if let Some(status) = status {
                    let flag = |key: &str| status.get(key).and_then(Value::as_bool) == Some(true);
                    this.live.apply_ubisoft_status(
                        flag("installed"),
                        flag("running"),
                        flag("installing"),
                    );
                    cx.notify();
                }
            },
        );
    }

    fn watch_steamapps(&mut self, cx: &mut Context<Self>) {
        if self.live.watch_in_flight {
            return;
        }
        self.live.watch_in_flight = true;
        live::call(
            cx,
            "GET",
            "/steam/watch-steamapps",
            None,
            MS(30_000),
            |this, result, cx| {
                this.live.watch_in_flight = false;
                let changed = result
                    .as_ref()
                    .and_then(|r| r.get("new_appids"))
                    .and_then(Value::as_array)
                    .is_some_and(|ids| !ids.is_empty());
                // Ubisoft installs have no watcher in the backend; the renderer
                // relied on focus refreshes. Keep Connect installs syncing too.
                if changed || this.live.ubisoft_running {
                    this.load_library(false, cx);
                }
            },
        );
    }

    /// App.vue `loadLibrary`: coalesced Steam + Ubisoft load and merge.
    pub(super) fn load_library(&mut self, force: bool, cx: &mut Context<Self>) {
        if self.live.library_in_flight {
            if force {
                self.live.pending_force_reload = true;
            }
            return;
        }
        let Some(live) = Live::get(cx) else { return };
        self.live.library_in_flight = true;
        self.live.pending_force_reload = false;
        cx.spawn(async move |this, cx| {
            let mut refresh = force;
            loop {
                let (steam, ubisoft) = {
                    let live = live.clone();
                    cx.background_executor()
                        .spawn(async move {
                            let steam_path = if refresh {
                                "/steam/library?refresh=1"
                            } else {
                                "/steam/library"
                            };
                            let (a, b) = std::thread::scope(|scope| {
                                let s = scope
                                    .spawn(|| live.request("GET", steam_path, None, MS(120_000)));
                                let u = scope.spawn(|| {
                                    live.request("GET", "/ubisoft/library", None, MS(120_000))
                                });
                                (s.join().ok(), u.join().ok())
                            });
                            (
                                a.and_then(Result::ok).map(live::unwrap_data),
                                b.and_then(Result::ok).map(live::unwrap_data),
                            )
                        })
                        .await
                };
                if refresh {
                    let live = live.clone();
                    cx.background_executor()
                        .spawn(async move {
                            let _ = live.request("GET", "/scan", None, MS(120_000));
                        })
                        .await;
                }
                let again = this
                    .update(cx, |this, cx| {
                        if let Some(steam_games) = library_model::parse_library(steam.as_ref()) {
                            let ubisoft_games =
                                library_model::parse_library(ubisoft.as_ref()).unwrap_or_default();
                            let games = library_model::merge_libraries(steam_games, ubisoft_games);
                            this.live.installed_count =
                                games.iter().filter(|g| g.installed).count();
                            this.live.library = games;
                            this.live.library_loaded = true;
                            this.rebuild_display_games(cx);
                            // LibraryView-only: never while setup/migration covers it.
                            if !this.live.library.is_empty()
                                && !this.show_setup
                                && !this.live.migration
                            {
                                this.apply_default_rules_once(cx);
                            }
                        }
                        this.refresh_steam_status(cx);
                        if std::mem::take(&mut this.live.update_check_after_library) {
                            this.check_for_updates(cx);
                        }
                        let again = this.live.pending_force_reload;
                        this.live.pending_force_reload = false;
                        if !again {
                            this.live.library_in_flight = false;
                        }
                        cx.notify();
                        again
                    })
                    .unwrap_or(false);
                if !again {
                    break;
                }
                refresh = true;
            }
        })
        .detach();
    }

    /// LibraryView `applyDefaultRulesOnce`: earlier builds seeded every Steam
    /// bottle with an explicit vkd3d override that masked the backend's
    /// recommended route. Reset untouched vkd3d seeds once per machine.
    fn apply_default_rules_once(&mut self, cx: &mut Context<Self>) {
        if self.live.default_rules_applied {
            return;
        }
        self.live.default_rules_applied = true;
        self.save_ui_state(cx);
        let repairs: Vec<(String, String, String)> = self
            .live
            .library
            .iter()
            .filter(|g| !g.is_ubisoft() && g.installed && g.preferred_pipeline() == "vkd3d")
            .filter_map(|g| {
                let recommended = g
                    .available_pipelines
                    .as_ref()?
                    .iter()
                    .find(|p| p.recommended)?
                    .id
                    .clone()
                    .filter(|id| !id.is_empty() && id != "vkd3d")?;
                Some((g.bottle_id(), g.name.clone(), recommended))
            })
            .collect();
        if repairs.is_empty() {
            return;
        }
        let Some(live) = Live::get(cx) else { return };
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .spawn(async move {
                    std::thread::scope(|scope| {
                        for (id, name, pipeline) in &repairs {
                            let live = &live;
                            scope.spawn(move || {
                                let _ = live.request(
                                    "POST",
                                    "/bottles/edit",
                                    Some(&json!({
                                        "id": id,
                                        "name": name,
                                        "preferredPipeline": pipeline,
                                    })),
                                    live::DEFAULT_TIMEOUT,
                                );
                            });
                        }
                    });
                })
                .await;
            let _ = this.update(cx, |this, cx| this.load_library(false, cx));
        })
        .detach();
    }

    /// Recompute the displayed (installed, ordered) list and its artwork.
    pub(super) fn rebuild_display_games(&mut self, cx: &mut Context<Self>) {
        let games = library_model::live_games(&self.live.library, &self.live.play_history);
        let previous = self.selected_key();
        self.games = games;
        if let Some(key) = previous {
            if let Some(index) = self.games.iter().position(|g| g.key() == key) {
                self.selected_game = index;
            } else {
                self.selected_game = 0;
            }
        }
        self.selected_game = self.selected_game.min(self.games.len().saturating_sub(1));
        // The hero shows the first search match when the selection is filtered
        // out; keep handlers (EXE, Steam Emu, Bottle) pointed at that same game.
        let matches = self.matching_games();
        if !matches.is_empty() && !matches.contains(&self.selected_game) {
            self.selected_game = matches[0];
        }
        for game in self.games.clone() {
            self.request_art(&game, cx);
        }
        // Ubisoft icon extraction retry (LibraryView: 1.5 s × 12).
        let pending = self.games.iter().any(|g| g.is_ubisoft() && g.icon_pending);
        if !pending {
            self.live.icon_refresh_attempts = 0;
        } else if !self.live.icon_refresh_scheduled && self.live.icon_refresh_attempts < 12 {
            self.live.icon_refresh_scheduled = true;
            cx.spawn(async move |this, cx| {
                cx.background_executor().timer(MS(1500)).await;
                let _ = this.update(cx, |this, cx| {
                    this.live.icon_refresh_scheduled = false;
                    this.live.icon_refresh_attempts += 1;
                    this.load_library(false, cx);
                });
            })
            .detach();
        }
        self.on_featured_changed(cx);
        // Electron recomputes the Bottle selector from the reloaded game.
        if !self.live.pipeline_saving {
            if let Some(game) = self.games.get(self.selected_game) {
                self.live.selected_pipeline = library_model::effective_pipeline(game);
            }
        }
    }

    pub(super) fn selected_key(&self) -> Option<(bool, u64)> {
        self.games.get(self.selected_game).map(LibGame::key)
    }

    pub(super) fn request_art(&self, game: &LibGame, cx: &mut Context<Self>) {
        let Some(cache) = crate::artwork::cache(cx) else {
            return;
        };
        use crate::artwork::{Candidate as C, steam_cdn, store_background};
        let appid = game.appid;
        let embedded = game.embedded_icon_path.clone().map(C::LocalFile);
        let (card, hero): (Vec<C>, Vec<C>) = if game.is_ubisoft() {
            let urls: Vec<C> = [
                game.ubisoft_artwork_url.clone(),
                game.cover_url.clone(),
                game.header_url.clone(),
            ]
            .into_iter()
            .flatten()
            .filter(|u| !u.is_empty())
            .map(C::Url)
            .collect();
            let mut card = urls.clone();
            card.push(C::SteamGridDb {
                name: game.name.clone(),
                hero: false,
            });
            card.extend(embedded.clone());
            let mut hero = urls;
            hero.push(C::SteamGridDb {
                name: game.name.clone(),
                hero: true,
            });
            hero.extend(embedded);
            (card, hero)
        } else {
            let primary = game
                .cover_url
                .clone()
                .filter(|u| !u.is_empty())
                .unwrap_or_else(|| steam_cdn(appid, "library_600x900_2x"));
            let header = game
                .header_url
                .clone()
                .filter(|u| !u.is_empty())
                .unwrap_or_else(|| steam_cdn(appid, "header"));
            let mut card = vec![
                C::Backend(format!("/art/grid/{appid}/poster")),
                C::Backend(format!("/art/grid/{appid}/header")),
                C::Url(primary.clone()),
                C::Url(format!("https://steamdb.info/resize/600x900/{primary}")),
                C::Url(steam_cdn(appid, "library_hero")),
                C::Url(header.clone()),
                C::Url(store_background(appid)),
            ];
            card.extend(embedded);
            let hero = vec![
                C::Backend(format!("/art/grid/{appid}/hero")),
                C::Url(steam_cdn(appid, "library_hero")),
                C::Url(primary),
                C::Url(header),
                C::StoreDetails { appid, hero: true },
                C::Url(store_background(appid)),
            ];
            (card, hero)
        };
        let source = if game.is_ubisoft() {
            "ubisoft"
        } else {
            "steam"
        };
        let ubisoft_icon_ready = game.is_ubisoft() && game.embedded_icon_path.is_some();
        cache.update(cx, |cache, cx| {
            if ubisoft_icon_ready {
                // The extracted EXE icon arrived after an earlier miss: retry.
                for kind in ["card", "hero"] {
                    let key = format!("{source}-{appid}-{kind}");
                    if cache.state(&key) == Some(&crate::artwork::ArtState::Missing) {
                        cache.forget(&key, cx);
                    }
                }
            }
            cache.resolve(format!("{source}-{appid}-card"), card, cx);
            cache.resolve(format!("{source}-{appid}-hero"), hero, cx);
        });
    }

    pub(super) fn tilted_art(
        &self,
        game: &LibGame,
        angle: i32,
        cx: &mut Context<Self>,
    ) -> Option<PathBuf> {
        let source = if game.is_ubisoft() {
            "ubisoft"
        } else {
            "steam"
        };
        let key = format!("{source}-{}-card", game.appid);
        crate::artwork::cache(cx)?.update(cx, |cache, cx| cache.tilted(&key, angle, cx))
    }

    pub(super) fn art_path(&self, game: &LibGame, hero: bool, cx: &App) -> Option<PathBuf> {
        if let Some((cover, hero_art)) = game.preview_art {
            return Some(asset_path(if hero { hero_art } else { cover }));
        }
        let source = if game.is_ubisoft() {
            "ubisoft"
        } else {
            "steam"
        };
        let kind = if hero { "hero" } else { "card" };
        let cache = crate::artwork::cache(cx)?;
        let cache = cache.read(cx);
        cache
            .path(&format!("{source}-{}-{kind}", game.appid))
            .or_else(|| {
                if hero {
                    cache.path(&format!("{source}-{}-card", game.appid))
                } else {
                    None
                }
            })
    }

    // ─────────────────────── running / play / stop ───────────────────────

    pub(super) fn is_running(&self, appid: u64) -> bool {
        self.live.running.contains_key(&appid)
    }

    fn refresh_running_games(&mut self, cx: &mut Context<Self>) {
        if self.live.running_poll_in_flight {
            return;
        }
        self.live.running_poll_in_flight = true;
        live::call(
            cx,
            "GET",
            "/game/running",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                this.live.running_poll_in_flight = false;
                let Some(result) = result.filter(is_ok) else {
                    return;
                };
                let Some(list) = result.get("running").and_then(Value::as_array) else {
                    return;
                };
                let mut next = HashMap::new();
                for entry in list {
                    let appid = entry.get("appid").and_then(Value::as_u64).unwrap_or(0);
                    let pid = entry.get("pid").and_then(Value::as_u64).unwrap_or(0);
                    if appid > 0 && pid > 0 {
                        next.insert(appid, pid);
                    }
                }
                let stopped = this
                    .live
                    .running
                    .keys()
                    .any(|appid| !next.contains_key(appid));
                if next != this.live.running {
                    this.live.running = next;
                    cx.notify();
                }
                if stopped {
                    this.load_library(false, cx);
                }
            },
        );
    }

    pub(super) fn stop_game(&mut self, game: LibGame, cx: &mut Context<Self>) {
        let body = json!({"appid": game.appid});
        live::call(
            cx,
            "POST",
            "/kill",
            Some(body),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if !result.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| format!("Failed to stop {}", game.name)),
                    );
                    return;
                }
                this.live.running.remove(&game.appid);
                toast::success(cx, format!("Stopped {}", game.name));
                this.load_library(false, cx);
                cx.notify();
            },
        );
    }

    /// LibraryView `launchGame` (Play doubles as Stop while running).
    pub(super) fn launch_game(&mut self, game: LibGame, cx: &mut Context<Self>) {
        if !self.live.enabled {
            return;
        }
        if self.is_running(game.appid) {
            self.stop_game(game, cx);
            return;
        }
        if self.live.launching == Some(game.appid) {
            return;
        }
        self.live.launching = Some(game.appid);
        cx.notify();
        let (endpoint, body) = library_model::launch_request(&game);
        live::call(
            cx,
            "POST",
            endpoint,
            Some(body),
            MS(600_000),
            move |this, result, cx| {
                this.live.launching = None;
                if result.as_ref().is_some_and(is_ok) {
                    let result = result.unwrap();
                    if let Some(pid) = result.get("pid").and_then(Value::as_u64).filter(|p| *p > 0)
                    {
                        this.live.running.insert(game.appid, pid);
                    }
                    let now = SystemTime::now()
                        .duration_since(SystemTime::UNIX_EPOCH)
                        .map(|d| d.as_millis() as i64)
                        .unwrap_or(0);
                    this.live.play_history.insert(game.appid.to_string(), now);
                    this.save_ui_state(cx);
                    if result
                        .get("launch_mode")
                        .and_then(Value::as_str)
                        .is_some_and(|mode| mode.starts_with("ubisoft_first_run"))
                    {
                        toast::success(cx, "Launching Through Steam with D3DMetal");
                    } else {
                        toast::success(cx, format!("Launched {}", game.name));
                    }
                    show_launch_quit_hint(game.name.clone(), cx);
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| format!("Failed to launch {}", game.name)),
                    );
                }
                cx.notify();
            },
        );
    }

    // ───────────────────────── hero settings ─────────────────────────

    /// LibraryView `watch(featuredGame)`.
    pub(super) fn on_featured_changed(&mut self, cx: &mut Context<Self>) {
        let key = self.selected_key();
        if key == self.live.featured_key {
            return;
        }
        self.live.featured_key = key;
        let Some(game) = self.games.get(self.selected_game).cloned() else {
            return;
        };
        self.live.selected_pipeline = library_model::effective_pipeline(&game);
        if !self.live.enabled {
            return;
        }
        self.load_game_settings(cx);
        if game.installed && !game.is_ubisoft() {
            let appid = game.appid;
            live::call(
                cx,
                "GET",
                format!("/goldberg/status?appid={appid}"),
                None,
                live::DEFAULT_TIMEOUT,
                move |this, result, cx| {
                    if this.selected_key() != Some((false, appid)) {
                        return;
                    }
                    if let Some(result) = result.filter(is_ok) {
                        this.live.steam_emu_active = result
                            .get("goldberg_active")
                            .and_then(Value::as_bool)
                            .unwrap_or(false);
                        cx.notify();
                    }
                },
            );
        } else {
            self.live.steam_emu_active = false;
        }
    }

    pub(super) fn load_game_settings(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/config",
            None,
            live::DEFAULT_TIMEOUT,
            |this, config, cx| {
                if let Some(config) = config.filter(is_ok) {
                    this.live.controller_input =
                        match config.get("controllerInput").and_then(Value::as_str) {
                            Some("x") => "x",
                            Some("d") => "d",
                            _ => "off",
                        };
                    this.live.msync = config.get("msync").and_then(Value::as_bool) != Some(false);
                    cx.notify();
                }
            },
        );
        live::call(
            cx,
            "GET",
            "/metalfx/state",
            None,
            live::DEFAULT_TIMEOUT,
            |this, state, cx| {
                if let Some(state) = state.filter(is_ok) {
                    let factor = state.get("factor").and_then(Value::as_f64).unwrap_or(2.0);
                    this.live.metal_fx_mode =
                        if state.get("enabled").and_then(Value::as_bool) == Some(false) {
                            "off"
                        } else if (factor - 1.75).abs() < 0.01 {
                            "1.75"
                        } else {
                            "2.0"
                        };
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn set_metal_fx(&mut self, mode: &'static str, cx: &mut Context<Self>) {
        if !self.live.enabled {
            return;
        }
        if self.live.metal_fx_busy {
            return;
        }
        self.live.metal_fx_busy = true;
        let body = if mode == "off" {
            json!({"enabled": false})
        } else {
            json!({"enabled": true, "factor": if mode == "1.75" { 1.75 } else { 2.0 }})
        };
        live::call(
            cx,
            "POST",
            "/metalfx/toggle",
            Some(body),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if result.as_ref().is_some_and(is_ok) {
                    this.live.metal_fx_mode = mode;
                } else {
                    toast::error(cx, "Failed to update MetalFX");
                }
                this.live.metal_fx_busy = false;
                cx.notify();
            },
        );
    }

    pub(super) fn set_controller(&mut self, mode: &'static str, cx: &mut Context<Self>) {
        if !self.live.enabled {
            return;
        }
        if self.live.controller_busy {
            return;
        }
        self.live.controller_busy = true;
        live::call(
            cx,
            "POST",
            "/config",
            Some(json!({"controllerInput": mode})),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if result.as_ref().is_some_and(is_ok) {
                    this.live.controller_input = mode;
                } else {
                    toast::error(cx, "Failed to update controller input");
                }
                this.live.controller_busy = false;
                cx.notify();
            },
        );
    }

    pub(super) fn set_msync(&mut self, enabled: bool, cx: &mut Context<Self>) {
        if !self.live.enabled {
            return;
        }
        if self.live.msync_busy {
            return;
        }
        self.live.msync_busy = true;
        live::call(
            cx,
            "POST",
            "/config",
            Some(json!({"msync": enabled})),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if result.as_ref().is_some_and(is_ok) {
                    this.live.msync = enabled;
                } else {
                    toast::error(cx, "Failed to update msync");
                }
                this.live.msync_busy = false;
                cx.notify();
            },
        );
    }

    pub(super) fn set_steam_emu(&mut self, enabled: bool, cx: &mut Context<Self>) {
        if !self.live.enabled {
            return;
        }
        let Some(game) = self.games.get(self.selected_game).cloned() else {
            return;
        };
        if self.live.steam_emu_busy || !game.installed {
            return;
        }
        self.live.steam_emu_busy = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/goldberg/toggle",
            Some(json!({"appid": game.appid, "enable": enabled})),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if result.as_ref().is_some_and(is_ok) {
                    let result = result.unwrap();
                    this.live.steam_emu_active = result
                        .get("goldberg_active")
                        .and_then(Value::as_bool)
                        .unwrap_or(false);
                    toast::success(
                        cx,
                        if enabled {
                            if result.get("cache_files_ok").and_then(Value::as_bool) == Some(false)
                            {
                                "Steam Emu enabled, but no backup cache found — restore from OFF may rely on .orig files only"
                            } else {
                                "Steam Emu enabled; original Steam DLLs cached for safe restore"
                            }
                        } else {
                            "Steam Emu disabled; original Steam DLLs restored"
                        },
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Failed to toggle Steam Emu".into()),
                    );
                }
                this.live.steam_emu_busy = false;
                cx.notify();
            },
        );
    }

    /// LibraryView `chooseFeaturedExecutable`.
    pub(super) fn choose_featured_executable(&mut self, cx: &mut Context<Self>) {
        let Some(game) = self.games.get(self.selected_game).cloned() else {
            return;
        };
        if game.has_native_build || !game.installed {
            return;
        }
        let default_dir = [&game.executable_path, &game.game_dir, &game.wine_game_path]
            .into_iter()
            .flatten()
            .find(|p| !p.is_empty())
            .map(|p| {
                let path = std::path::PathBuf::from(p);
                if path.is_file() {
                    path.parent().map(|p| p.to_path_buf()).unwrap_or(path)
                } else {
                    path
                }
            });
        cx.spawn(async move |this, cx| {
            let mut dialog = rfd::AsyncFileDialog::new()
                .set_title("Choose game executable")
                .add_filter("Windows executable", &["exe"]);
            if let Some(dir) = default_dir.filter(|d| d.is_dir()) {
                dialog = dialog.set_directory(dir);
            }
            let Some(file) = dialog.pick_file().await else {
                return;
            };
            let selected = file.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                let (endpoint, body) = if game.is_ubisoft() {
                    (
                        "/ubisoft/save-executable",
                        json!({"ubisoft_id": game.ubisoft_id, "executablePath": selected}),
                    )
                } else {
                    (
                        "/steam/save-executable",
                        json!({"appid": game.appid, "executablePath": selected}),
                    )
                };
                live::call(
                    cx,
                    "POST",
                    endpoint,
                    Some(body),
                    live::DEFAULT_TIMEOUT,
                    move |this, result, cx| {
                        if !result.as_ref().is_some_and(is_ok) {
                            toast::error(
                                cx,
                                error_text(result.as_ref()).unwrap_or_else(|| {
                                    format!("Could not save {}'s executable", game.name)
                                }),
                            );
                            return;
                        }
                        let saved = result
                            .as_ref()
                            .and_then(|r| r.get("executablePath"))
                            .and_then(Value::as_str)
                            .map(str::to_owned)
                            .unwrap_or(selected);
                        for list in [&mut this.games, &mut this.live.library] {
                            if let Some(entry) = list.iter_mut().find(|g| g.key() == game.key()) {
                                entry.executable_path = Some(saved.clone());
                            }
                        }
                        toast::success(cx, format!("{}: launch executable saved", game.name));
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    /// LibraryView `savePipeline` (hero) and `saveCollectionPipeline`.
    pub(super) fn save_pipeline(
        &mut self,
        game: LibGame,
        pipeline: String,
        collection: bool,
        cx: &mut Context<Self>,
    ) {
        let previous_raw = if !game.preferred_pipeline().is_empty() {
            game.preferred_pipeline().to_owned()
        } else {
            game.launch_method().to_owned()
        };
        if !self.live.enabled {
            return;
        }
        if collection {
            if game.has_native_build || pipeline.is_empty() || pipeline == previous_raw {
                return;
            }
            self.live.collection_saving.insert(game.appid);
        } else {
            // A <select> change never fires for the current value.
            if self.live.pipeline_saving || pipeline == self.live.selected_pipeline {
                return;
            }
            self.live.pipeline_saving = true;
            self.live.selected_pipeline = pipeline.clone();
        }
        // Optimistic update, as the renderer mutates the game object.
        for list in [&mut self.games, &mut self.live.library] {
            if let Some(entry) = list.iter_mut().find(|g| g.key() == game.key()) {
                entry.preferred_pipeline = Some(pipeline.clone());
                entry.launch_method = Some(pipeline.clone());
                entry.launch_method_name = Some(library_model::pipeline_label(&pipeline));
            }
        }
        cx.notify();
        let (path, body, timeout) = library_model::pipeline_save_request(&game, &pipeline);
        live::call(
            cx,
            "POST",
            path,
            Some(body),
            MS(timeout),
            move |this, result, cx| {
                let ok = result.as_ref().is_some_and(is_ok);
                if collection {
                    this.live.collection_saving.remove(&game.appid);
                } else {
                    this.live.pipeline_saving = false;
                }
                if ok {
                    toast::success(
                        cx,
                        format!(
                            "{}: {} selected",
                            game.name,
                            library_model::pipeline_label(&pipeline)
                        ),
                    );
                    this.load_library(false, cx);
                } else {
                    let previous = if collection {
                        previous_raw.clone()
                    } else {
                        library_model::normalize_pipeline(&previous_raw)
                    };
                    for list in [&mut this.games, &mut this.live.library] {
                        if let Some(entry) = list.iter_mut().find(|g| g.key() == game.key()) {
                            entry.preferred_pipeline = if previous.is_empty() || previous == "auto"
                            {
                                None
                            } else {
                                Some(previous.clone())
                            };
                            entry.launch_method = Some(previous.clone())
                                .filter(|p| !p.is_empty())
                                .or(entry.launch_method.clone());
                        }
                    }
                    if !collection {
                        this.live.selected_pipeline = previous;
                    }
                    let fallback = if collection {
                        format!("Failed to update {}", game.name)
                    } else {
                        "Could not save bottle route".into()
                    };
                    toast::error(cx, error_text(result.as_ref()).unwrap_or(fallback));
                    if collection {
                        this.load_library(false, cx);
                    }
                }
                cx.notify();
            },
        );
    }

    // ───────────────────────── Steam / Ubisoft ─────────────────────────

    /// LibraryTopbar / empty-hero `toggleSteam`.
    /// LibraryTopbar `toggleSteam`: stop when running, otherwise start.
    pub(super) fn toggle_steam(&mut self, reload: bool, cx: &mut Context<Self>) {
        if !self.live.enabled {
            self.steam_running = !self.steam_running;
            cx.notify();
            return;
        }
        if self.live.wine_steam_running {
            self.stop_steam(reload, cx);
        } else {
            self.start_steam(reload, cx);
        }
    }

    /// Start (or bring forward) Wine Steam. The library's empty-state button
    /// is labelled "Start Steam", so it always starts; toggling there stopped
    /// an already-running Steam behind a success toast.
    pub(super) fn start_steam(&mut self, reload: bool, cx: &mut Context<Self>) {
        if !self.live.enabled {
            self.steam_running = true;
            cx.notify();
            return;
        }
        if self.live.steam_pending.is_some() {
            return;
        }
        self.live.steam_pending = Some(SteamPending::Starting);
        cx.notify();
        live::call(
            cx,
            "POST",
            "/steam/launch",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                this.live.steam_pending = None;
                let ok = result.as_ref().is_some_and(is_ok);
                if ok {
                    this.live.wine_steam_running = true;
                }
                toast::show(
                    cx,
                    error_text(result.as_ref()).unwrap_or_else(|| "Starting Wine Steam...".into()),
                    if ok {
                        toast::ToastKind::Success
                    } else {
                        toast::ToastKind::Error
                    },
                );
                if reload {
                    this.load_library(false, cx);
                }
                cx.notify();
            },
        );
    }

    fn stop_steam(&mut self, reload: bool, cx: &mut Context<Self>) {
        if self.live.steam_pending.is_some() {
            return;
        }
        self.live.steam_pending = Some(SteamPending::Stopping);
        cx.notify();
        live::call(
            cx,
            "POST",
            "/steam/stop",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                this.live.steam_pending = None;
                let ok = result.as_ref().is_some_and(is_ok);
                if ok {
                    this.live.wine_steam_running = false;
                }
                toast::show(
                    cx,
                    error_text(result.as_ref()).unwrap_or_else(|| "Wine Steam stopped".into()),
                    if ok {
                        toast::ToastKind::Success
                    } else {
                        toast::ToastKind::Error
                    },
                );
                if reload {
                    this.load_library(false, cx);
                }
                cx.notify();
            },
        );
    }

    /// LibraryTopbar `toggleUbisoft`.
    pub(super) fn toggle_ubisoft(&mut self, cx: &mut Context<Self>) {
        if self.live.ubisoft_installing {
            return;
        }
        self.launcher_menu_open = false;
        if self.live.ubisoft_running {
            live::call(
                cx,
                "POST",
                "/ubisoft/stop",
                None,
                live::DEFAULT_TIMEOUT,
                |this, result, cx| {
                    let ok = result.as_ref().is_some_and(is_ok);
                    if ok {
                        this.live.ubisoft_running = false;
                        this.live.ubisoft_primary = false;
                    }
                    toast::show(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Ubisoft Connect stopped".into()),
                        if ok {
                            toast::ToastKind::Success
                        } else {
                            toast::ToastKind::Error
                        },
                    );
                    cx.notify();
                },
            );
            return;
        }
        live::call(
            cx,
            "POST",
            "/ubisoft/launch",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                let ok = result.as_ref().is_some_and(is_ok);
                let installing = result
                    .as_ref()
                    .and_then(|r| r.get("installing"))
                    .and_then(Value::as_bool)
                    == Some(true);
                if ok && installing {
                    this.live.ubisoft_installing = true;
                    this.live.ubisoft_primary = true;
                    toast::info(cx, "Downloading and installing Ubisoft Connect…");
                } else if ok {
                    this.live.ubisoft_running = true;
                    this.live.ubisoft_primary = true;
                    toast::success(cx, "Starting Ubisoft Connect with D3DMetal…");
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Could not start Ubisoft Connect".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    /// LibraryTopbar `runSteamFix` → main-process `app:run-steam-fix`.
    pub(super) fn run_steam_fix(&mut self, cx: &mut Context<Self>) {
        if self.live.steam_fix_busy || !self.live.enabled {
            return;
        }
        self.steam_options_open = false;
        self.live.steam_fix_busy = true;
        toast::info(cx, "Fixing Steam...");
        let Some(live) = Live::get(cx) else { return };
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = cx
                .background_executor()
                .spawn(
                    async move { run_bash_script(include_str!("../assets/steam-fix.sh"), &live) },
                )
                .await;
            let _ = this.update(cx, |this, cx| {
                this.live.steam_fix_busy = false;
                match result {
                    Ok(_) => toast::success(cx, "Fix Steam"),
                    Err(error) => toast::error(
                        cx,
                        if error.is_empty() {
                            "Fix Steam".into()
                        } else {
                            error
                        },
                    ),
                }
                cx.notify();
            });
        })
        .detach();
    }

    /// LibraryView `openArtManager` → main `app:open-steam-art-manager`.
    pub(super) fn open_steam_art_manager(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        match crate::host_actions::open_steam_art_manager(&live.resources(), &live.home()) {
            Ok(Some(path)) => {
                cx.write_to_clipboard(gpui::ClipboardItem::new_string(path.clone()));
                let detail = path;
                cx.background_executor()
                    .spawn(async move {
                        rfd::MessageDialog::new()
                            .set_title("Steam Art Manager")
                            .set_description(format!(
                                "If Steam Art Manager asks for your Steam install path, paste (⌘V) the path below — it has been copied to your clipboard:\n\n{detail}"
                            ))
                            .set_level(rfd::MessageLevel::Info)
                            .show();
                    })
                    .detach();
            }
            Ok(None) => {}
            Err(error) => eprintln!("Steam Art Manager could not be launched: {error}"),
        }
    }

    fn poll_grid_art(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        let Some(cache) = crate::artwork::cache(cx) else {
            return;
        };
        let signature = crate::host_actions::grid_art_signature(&live.home());
        // First poll of a session compares against the signature saved with
        // the artwork cache, so art changed while MetalSharp was closed counts.
        let previous = match self.live.grid_art_signature.take() {
            Some(previous) => Some(previous),
            None => cache.update(cx, |cache, cx| cache.saved_grid_signature(cx)),
        };
        if let Some(previous) = previous.filter(|previous| *previous != signature) {
            let appids = crate::host_actions::grid_art_changed_appids(&previous, &signature);
            cache.update(cx, |cache, cx| {
                for appid in appids {
                    for kind in ["card", "hero"] {
                        cache.revalidate(&format!("steam-{appid}-{kind}"), cx);
                    }
                }
            });
        }
        cache.update(cx, |cache, cx| {
            cache.save_grid_signature(signature.clone(), cx)
        });
        self.live.grid_art_signature = Some(signature);
    }

    // ─────────────────────────── setup wizard ───────────────────────────

    /// SetupWizard `startInstall`.
    pub(super) fn setup_start_install(&mut self, cx: &mut Context<Self>) {
        let flow = &mut self.live.setup;
        if flow.installing || flow.runtime_ready() {
            return;
        }
        flow.installing = true;
        flow.install_logs.clear();
        flow.install_progress = 0;
        flow.install_failed = false;
        flow.install_current.clear();
        let generation = flow.generation;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/setup/install-all",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, started, cx| {
                if this.live.setup.generation != generation {
                    return;
                }
                if !started.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(started.as_ref())
                            .unwrap_or_else(|| this.copy.install_failed.clone()),
                    );
                    this.live.setup.installing = false;
                    cx.notify();
                    return;
                }
                let text = this.copy.starting_installation.clone();
                this.live.setup.install_logs.push((text, LogClass::Info));
                cx.notify();
                this.poll_install_progress(generation, -1, String::new(), cx);
            },
        );
    }

    fn poll_install_progress(
        &mut self,
        generation: u64,
        last_step: i64,
        last_status: String,
        cx: &mut Context<Self>,
    ) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(500)).await;
            let _ = this.update(cx, move |this, cx| {
                if this.live.setup.generation != generation {
                    return;
                }
                live::call(
                    cx,
                    "GET",
                    "/setup/install-progress",
                    None,
                    live::DEFAULT_TIMEOUT,
                    move |this, progress, cx| {
                        if this.live.setup.generation != generation {
                            return;
                        }
                        let Some(progress) = progress else {
                            this.poll_install_progress(generation, last_step, last_status, cx);
                            return;
                        };
                        let step = progress.get("step").and_then(Value::as_i64).unwrap_or(0);
                        let total = progress.get("total").and_then(Value::as_i64).unwrap_or(0);
                        let status = progress
                            .get("status")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        let log = progress
                            .get("log")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        let current = progress
                            .get("current")
                            .and_then(Value::as_str)
                            .unwrap_or("");
                        let flow = &mut this.live.setup;
                        flow.install_progress = if total > 0 {
                            ((step as f64 / total as f64) * 100.0)
                                .round()
                                .clamp(0.0, 100.0) as usize
                        } else {
                            0
                        };
                        if !current.is_empty() {
                            flow.install_current = current.to_owned();
                        }
                        let mut finished = false;
                        if step != last_step || status != last_status {
                            match status.as_str() {
                                "done" => flow.install_logs.push((log, LogClass::Success)),
                                "skipped" => flow.install_logs.push((log, LogClass::Warn)),
                                "error" => {
                                    flow.install_logs.push((log, LogClass::Error));
                                    if let Some(error) =
                                        progress.get("error").and_then(Value::as_str)
                                    {
                                        flow.install_logs
                                            .push((format!("Error: {error}"), LogClass::Error));
                                    }
                                    flow.installing = false;
                                    flow.install_failed = true;
                                    flow.log_open = true;
                                    finished = true;
                                }
                                "installing" if step != last_step => {
                                    flow.install_logs.push((log, LogClass::Active))
                                }
                                "complete" => {
                                    let text = this.copy.install_complete.clone();
                                    let flow = &mut this.live.setup;
                                    flow.install_logs.push((text.clone(), LogClass::Success));
                                    flow.install_progress = 100;
                                    flow.installing = false;
                                    flow.install_status = "complete".into();
                                    finished = true;
                                    toast::success(cx, text);
                                    this.setup_check_steam(cx, |_, _, _| {});
                                }
                                _ => {}
                            }
                        }
                        cx.notify();
                        if !finished {
                            this.poll_install_progress(generation, step, status, cx);
                        }
                    },
                );
            });
        })
        .detach();
    }

    fn setup_check_steam(
        &mut self,
        cx: &mut Context<Self>,
        then: impl FnOnce(&mut Self, Option<Value>, &mut Context<Self>) + 'static,
    ) {
        live::call(
            cx,
            "GET",
            "/steam/status",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, status, cx| {
                if let Some(s) = status.as_ref() {
                    let installed = s.get("installed").and_then(Value::as_bool) == Some(true);
                    let installing = s.get("installing").and_then(Value::as_bool) == Some(true);
                    let flow = &mut this.live.setup;
                    flow.steam_installed = installed && !installing;
                    flow.steam_installing = installing;
                    if let Some(stage) = s.get("install_stage").and_then(Value::as_str) {
                        flow.steam_install_stage = stage.to_owned();
                    }
                } else {
                    this.live.setup.steam_installed = false;
                    this.live.setup.steam_installing = false;
                }
                cx.notify();
                then(this, status, cx);
            },
        );
    }

    /// SetupWizard `installSteam`.
    pub(super) fn setup_install_steam(&mut self, cx: &mut Context<Self>) {
        let flow = &mut self.live.setup;
        if !flow.runtime_ready() || flow.steam_installing || flow.steam_installed {
            return;
        }
        flow.steam_failed = false;
        flow.steam_installing = true;
        flow.steam_install_stage = "downloading".into();
        let generation = flow.generation;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/steam/install",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if this.live.setup.generation != generation {
                    return;
                }
                if !result.as_ref().is_some_and(is_ok) {
                    let flow = &mut this.live.setup;
                    flow.steam_installing = false;
                    flow.steam_failed = true;
                    flow.steam_install_stage = "failed".into();
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| this.copy.steam_install_failed.clone()),
                    );
                    cx.notify();
                    return;
                }
                this.poll_steam_install(generation, Instant::now(), cx);
            },
        );
    }

    fn poll_steam_install(&mut self, generation: u64, started: Instant, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(1000)).await;
            let _ = this.update(cx, move |this, cx| {
                if this.live.setup.generation != generation {
                    return;
                }
                this.setup_check_steam(cx, move |this, status, cx| {
                    if this.live.setup.generation != generation {
                        return;
                    }
                    let installed = status.as_ref().is_some_and(|s| {
                        s.get("installed").and_then(Value::as_bool) == Some(true)
                            && s.get("installing").and_then(Value::as_bool) != Some(true)
                    });
                    if installed {
                        let flow = &mut this.live.setup;
                        flow.steam_installed = true;
                        flow.steam_installing = false;
                        flow.steam_install_stage = "complete".into();
                        toast::success(cx, this.copy.steam_installed.clone());
                        // Steam is marked installed: end the Wine Steam session
                        // (the backend's pkill-equivalent) instead of leaving
                        // the freshly installed client running.
                        this.stop_installed_steam_session(0, cx);
                        cx.notify();
                    } else if started.elapsed() > MS(300_000) {
                        let flow = &mut this.live.setup;
                        flow.steam_installing = false;
                        flow.steam_failed = true;
                        flow.steam_install_stage = "failed".into();
                        toast::error(cx, this.copy.steam_install_timed_out.clone());
                        cx.notify();
                    } else {
                        this.bring_steam_installer_forward(cx);
                        this.poll_steam_install(generation, started, cx);
                    }
                });
            });
        })
        .detach();
    }

    /// Wine windows open behind MetalSharp (macOS will not let a background
    /// process take focus), so hand activation to each Steam installer GUI
    /// process until it has been frontmost once; after that the user decides.
    fn bring_steam_installer_forward(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/steam/stop-targets",
            None,
            MS(5000),
            |this, targets, _| {
                let flow = &mut this.live.setup;
                if !flow.steam_installing {
                    return;
                }
                let pids = targets
                    .as_ref()
                    .and_then(|t| t.get("targeted"))
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(|t| t.get("pid").and_then(Value::as_i64));
                for pid in pids {
                    if !flow.installer_fronted.contains(&pid) && activate_wine_app(pid) {
                        flow.installer_fronted.insert(pid);
                    }
                }
            },
        );
    }

    /// End the Wine session SteamSetup left behind and confirm it is gone
    /// (`running: false`), retrying so a stray wineserver cannot block the
    /// first Start Steam.
    fn stop_installed_steam_session(&mut self, attempt: u32, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/steam/stop",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                let stopped = result.as_ref().is_some_and(|r| {
                    is_ok(r) && r.get("running").and_then(Value::as_bool) == Some(false)
                });
                if stopped {
                    this.live.wine_steam_running = false;
                } else if attempt < 3 {
                    cx.spawn(async move |this, cx| {
                        cx.background_executor().timer(MS(1000)).await;
                        let _ = this.update(cx, |this, cx| {
                            this.stop_installed_steam_session(attempt + 1, cx)
                        });
                    })
                    .detach();
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref()).unwrap_or_else(|| {
                            "Wine processes from the Steam install are still running".into()
                        }),
                    );
                }
                cx.notify();
            },
        );
    }

    /// SetupWizard `goToDoneStep`.
    pub(super) fn setup_go_to_done(&mut self, cx: &mut Context<Self>) {
        self.step = 2;
        cx.notify();
        live::call(
            cx,
            "GET",
            "/setup/device-name",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                if let Some(name) = result
                    .as_ref()
                    .and_then(|r| r.get("name"))
                    .and_then(Value::as_str)
                    .filter(|n| !n.is_empty())
                {
                    let name = name.to_owned();
                    if let Some(input) = this.setup_inputs.as_ref().map(|i| i[0].clone()) {
                        input.update(cx, |input, cx| {
                            input.content = name.clone().into();
                            cx.notify();
                        });
                    }
                    this.live.device_name = name;
                }
                cx.notify();
            },
        );
    }

    /// SetupWizard `finish`.
    pub(super) fn setup_finish(&mut self, cx: &mut Context<Self>) {
        if self.live.setup.finishing {
            return;
        }
        self.live.setup.finishing = true;
        cx.notify();
        let (name, key, gamesdb) = match self.setup_inputs.as_ref() {
            Some(inputs) => (
                inputs[0].read(cx).content.trim().to_owned(),
                inputs[1].read(cx).content.trim().to_owned(),
                inputs[2].read(cx).content.trim().to_owned(),
            ),
            None => Default::default(),
        };
        let name = if name.is_empty() {
            self.live.device_name.clone()
        } else {
            name
        };
        live::call(
            cx,
            "POST",
            "/steam/ensure-launch-ready",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, wrappers, cx| {
                if !wrappers.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(wrappers.as_ref())
                            .unwrap_or_else(|| this.copy.wrapper_warning.clone()),
                    );
                }
                let body = json!({"step": 2, "deviceName": name, "completed": true});
                live::call(
                    cx,
                    "POST",
                    "/setup/save",
                    Some(body),
                    live::DEFAULT_TIMEOUT,
                    move |this, _, cx| {
                        this.live.device_name = name;
                        this.setup_save_keys(key, gamesdb, cx);
                    },
                );
            },
        );
    }

    fn setup_save_keys(&mut self, key: String, gamesdb: String, cx: &mut Context<Self>) {
        if !key.is_empty() {
            let body = json!({"key": key});
            live::call(
                cx,
                "POST",
                "/steam/save-api-key",
                Some(body),
                live::DEFAULT_TIMEOUT,
                move |this, result, cx| {
                    if !result.as_ref().is_some_and(is_ok) {
                        toast::error(
                            cx,
                            error_text(result.as_ref())
                                .unwrap_or_else(|| this.copy.api_key_save_failed.clone()),
                        );
                        this.live.setup.finishing = false;
                        cx.notify();
                        return;
                    }
                    this.live.steam_api_key = Some(key.clone());
                    if let Some(input) = this.setup_inputs.as_ref().map(|i| i[1].clone()) {
                        input.update(cx, |input, cx| input.clear(cx));
                    }
                    let result = result.unwrap();
                    if let Some(games) = library_model::parse_library(result.get("library")) {
                        this.live.library = games;
                        this.rebuild_display_games(cx);
                    }
                    let missing_id = result
                        .get("sync")
                        .and_then(|s| s.get("steam_id_detected"))
                        .and_then(Value::as_bool)
                        == Some(false);
                    if missing_id {
                        toast::error(cx, this.copy.api_key_steam_id_missing.clone());
                    } else {
                        toast::success(cx, "Steam API key saved");
                    }
                    this.setup_save_keys(String::new(), gamesdb, cx);
                },
            );
            return;
        }
        if !gamesdb.is_empty() {
            let body = json!({"key": gamesdb});
            live::call(
                cx,
                "POST",
                "/sharp-library/epic/thegamesdb-api-key",
                Some(body),
                live::DEFAULT_TIMEOUT,
                move |this, result, cx| {
                    if !result.as_ref().is_some_and(is_ok) {
                        toast::error(
                            cx,
                            error_text(result.as_ref()).unwrap_or_else(|| {
                                this.copy.the_games_db_api_key_save_failed.clone()
                            }),
                        );
                        this.live.setup.finishing = false;
                        cx.notify();
                        return;
                    }
                    if let Some(input) = this.setup_inputs.as_ref().map(|i| i[2].clone()) {
                        input.update(cx, |input, cx| input.clear(cx));
                    }
                    toast::success(cx, "TheGamesDB API key saved");
                    this.setup_save_keys(String::new(), String::new(), cx);
                },
            );
            return;
        }
        // emit("done") → App.onSetupDone → initApp().
        self.live.setup.finishing = false;
        self.live.setup.generation += 1;
        self.show_setup = false;
        self.step = 0;
        self.init_app(cx);
        cx.notify();
    }

    /// Dismissible wizard close (App.onSetupClosed).
    pub(super) fn setup_close(&mut self, cx: &mut Context<Self>) {
        self.live.setup.generation += 1;
        self.show_setup = false;
        self.step = 0;
        if !self.live.health_started {
            self.start_health_polling();
        }
        cx.notify();
    }

    // ───────────────────────────── updates ─────────────────────────────

    pub(super) fn check_for_updates(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/update/check",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                if let Some(result) = result {
                    if is_ok(&result)
                        && result.get("available").and_then(Value::as_bool) == Some(true)
                    {
                        let version = result
                            .get("latest_version")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        toast::success(cx, format!("Update available: v{version}"));
                    }
                    this.live.update_status = Some(result);
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn update_available(&self) -> bool {
        self.live
            .update_status
            .as_ref()
            .is_some_and(|s| is_ok(s) && s.get("available").and_then(Value::as_bool) == Some(true))
    }

    pub(super) fn update_field(&self, key: &str) -> Option<String> {
        self.live
            .update_status
            .as_ref()
            .and_then(|s| s.get(key))
            .and_then(Value::as_str)
            .map(str::to_owned)
    }

    /// App.vue `startUpdateDownload` (opens the confirmation).
    pub(super) fn start_update_download(&mut self, variant: &'static str, cx: &mut Context<Self>) {
        if self.live.update_downloading {
            return;
        }
        if variant == "fex"
            && self
                .live
                .update_status
                .as_ref()
                .and_then(|s| s.get("fex_supported"))
                .and_then(Value::as_bool)
                != Some(true)
        {
            let detected = self
                .live
                .update_status
                .as_ref()
                .and_then(|s| s.get("macos_major"))
                .and_then(Value::as_u64)
                .filter(|v| *v > 0);
            toast::error(
                cx,
                match detected {
                    Some(v) => format!("FEX requires macOS 27 or newer (macOS {v} detected)"),
                    None => "FEX requires macOS 27 or newer".into(),
                },
            );
            return;
        }
        self.live.update_confirm = Some(variant);
        cx.notify();
    }

    /// App.vue `beginUpdateDownload`.
    pub(super) fn begin_update_download(&mut self, variant: &'static str, cx: &mut Context<Self>) {
        self.live.update_confirm = None;
        if self.live.update_downloading {
            return;
        }
        let Some(live) = Live::get(cx) else { return };
        let script = match crate::updater_bridge::ensure_ready(&live) {
            Ok(script) => script,
            Err(error) => {
                toast::error(cx, error);
                return;
            }
        };
        let Some(pid) = live.backend_pid() else {
            toast::error(cx, "Cannot get backend PID");
            return;
        };
        let Some(target) = self
            .update_field("latest_version")
            .filter(|v| !v.is_empty())
        else {
            toast::error(cx, "Update version is unavailable");
            return;
        };
        let url_key = if variant == "fex" {
            "fex_download_url"
        } else {
            "download_url"
        };
        if self
            .update_field(url_key)
            .filter(|u| !u.is_empty())
            .is_none()
        {
            toast::error(
                cx,
                if variant == "fex" {
                    "FEX update DMG is unavailable"
                } else {
                    "Update DMG asset is unavailable"
                },
            );
            return;
        }
        self.live.update_downloading = true;
        self.live.update_progress = 0.0;
        self.live.update_message = "Starting download...".into();
        crate::updater_bridge::clear_install_status(&live.home());
        cx.notify();
        live::call(
            cx,
            "POST",
            "/update/start",
            Some(json!({"variant": variant})),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                if !result.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Failed to start download".into()),
                    );
                    this.live.update_downloading = false;
                    cx.notify();
                    return;
                }
                this.poll_update_download(variant, script, pid, target, 0, cx);
            },
        );
    }

    fn poll_update_download(
        &mut self,
        variant: &'static str,
        script: std::path::PathBuf,
        pid: u32,
        target: String,
        polls: u32,
        cx: &mut Context<Self>,
    ) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(500)).await;
            let _ = this.update(cx, move |_, cx| {
                live::call(
                    cx,
                    "GET",
                    "/update/progress",
                    None,
                    live::DEFAULT_TIMEOUT,
                    move |this, progress, cx| {
                        let polls = polls + 1;
                        let Some(progress) = progress else {
                            this.poll_update_download(variant, script, pid, target, polls, cx);
                            return;
                        };
                        this.live.update_progress = progress
                            .get("percent")
                            .and_then(Value::as_f64)
                            .unwrap_or(0.0)
                            as f32;
                        this.live.update_message = progress
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        let status = progress.get("status").and_then(Value::as_str).unwrap_or("");
                        cx.notify();
                        if status == "downloaded" || status == "complete" {
                            live::call(
                                cx,
                                "GET",
                                format!("/update/dmg-path?variant={variant}"),
                                None,
                                live::DEFAULT_TIMEOUT,
                                move |this, dmg, cx| {
                                    let Some(path) = dmg
                                        .as_ref()
                                        .and_then(|d| d.get("path"))
                                        .and_then(Value::as_str)
                                        .map(str::to_owned)
                                    else {
                                        toast::error(cx, "Download complete but DMG not found");
                                        this.live.update_downloading = false;
                                        cx.notify();
                                        return;
                                    };
                                    let Some(live) = Live::get(cx) else { return };
                                    if let Err(error) = crate::updater_bridge::spawn_install(
                                        &live, &script, &path, pid, &target, variant,
                                    ) {
                                        toast::error(cx, error);
                                        this.live.update_downloading = false;
                                        cx.notify();
                                        return;
                                    }
                                    this.live.update_message = "Installing update...".into();
                                    this.live.update_progress = 90.0;
                                    cx.notify();
                                    this.poll_update_install(0, cx);
                                },
                            );
                        } else if status == "error" {
                            this.live.update_downloading = false;
                            toast::error(
                                cx,
                                progress
                                    .get("error")
                                    .and_then(Value::as_str)
                                    .unwrap_or("Download failed")
                                    .to_owned(),
                            );
                            cx.notify();
                        } else if polls > 3600 {
                            this.live.update_downloading = false;
                            toast::error(cx, "Update download timed out");
                            cx.notify();
                        } else {
                            this.poll_update_download(variant, script, pid, target, polls, cx);
                        }
                    },
                );
            });
        })
        .detach();
    }

    fn poll_update_install(&mut self, polls: u32, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(1000)).await;
            let _ = this.update(cx, move |this, cx| {
                let polls = polls + 1;
                let Some(live) = Live::get(cx) else { return };
                let Some(status) = crate::updater_bridge::read_install_status(&live.home()) else {
                    if polls > 180 {
                        this.live.update_downloading = false;
                        toast::error(cx, "Installer did not report status");
                        cx.notify();
                    } else {
                        this.poll_update_install(polls, cx);
                    }
                    return;
                };
                this.live.update_progress = status
                    .get("percent")
                    .and_then(Value::as_f64)
                    .unwrap_or(90.0) as f32;
                this.live.update_message = status
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("Installing...")
                    .to_owned();
                match status.get("phase").and_then(Value::as_str) {
                    Some("complete") => {
                        this.live.update_downloading = false;
                        this.live.update_progress = 100.0;
                        this.live.update_message.clear();
                        toast::success(cx, "Update installed — restarting...");
                        cx.notify();
                        cx.spawn(async move |_, cx| {
                            cx.background_executor().timer(MS(2000)).await;
                            let _ = cx.update(|cx| cx.quit());
                        })
                        .detach();
                    }
                    Some("error") => {
                        this.live.update_downloading = false;
                        let message = status
                            .get("error")
                            .and_then(Value::as_str)
                            .or_else(|| status.get("message").and_then(Value::as_str))
                            .unwrap_or("Install failed")
                            .to_owned();
                        toast::error(cx, message);
                        cx.notify();
                    }
                    _ => {
                        cx.notify();
                        this.poll_update_install(polls, cx);
                    }
                }
            });
        })
        .detach();
    }

    // ───────────────────────────── migration ─────────────────────────────

    fn start_migration(&mut self, cx: &mut Context<Self>) {
        self.migration_view.status = "running".into();
        self.migration_attempt(0, cx);
    }

    fn migration_attempt(&mut self, attempt: u32, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/update/migrate/start",
            None,
            MS(10_000),
            move |this, result, cx| {
                let ok = result.as_ref().is_some_and(is_ok);
                let error = error_text(result.as_ref()).unwrap_or_default();
                if ok || error.contains("migration already in progress") {
                    this.poll_migration(cx);
                    return;
                }
                let transient = result.is_none()
                    || [
                        "ECONNREFUSED",
                        "timeout",
                        "did not start in time",
                        "Migration backend unavailable",
                    ]
                    .iter()
                    .any(|needle| error.contains(needle));
                if transient && attempt < 20 {
                    cx.spawn(async move |this, cx| {
                        cx.background_executor().timer(MS(500)).await;
                        let _ = this.update(cx, |this, cx| this.migration_attempt(attempt + 1, cx));
                    })
                    .detach();
                    return;
                }
                this.migration_view.status = "error".into();
                this.migration_view.error = if error.is_empty() {
                    "Migration could not start".into()
                } else {
                    error
                };
                cx.notify();
            },
        );
    }

    fn poll_migration(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(500)).await;
            let _ = this.update(cx, |_, cx| {
                live::call(
                    cx,
                    "GET",
                    "/update/migrate/progress",
                    None,
                    live::DEFAULT_TIMEOUT,
                    |this, progress, cx| {
                        if let Some(progress) = progress {
                            let view = &mut this.migration_view;
                            view.status = progress
                                .get("status")
                                .and_then(Value::as_str)
                                .unwrap_or("running")
                                .to_owned();
                            view.step = progress.get("step").and_then(Value::as_u64).unwrap_or(0);
                            view.total = progress.get("total").and_then(Value::as_u64).unwrap_or(0);
                            view.message = progress
                                .get("message")
                                .and_then(Value::as_str)
                                .unwrap_or("")
                                .to_owned();
                            view.error = progress
                                .get("error")
                                .and_then(Value::as_str)
                                .unwrap_or("")
                                .to_owned();
                            cx.notify();
                            if view.status == "complete" || view.status == "error" {
                                return;
                            }
                        }
                        this.poll_migration(cx);
                    },
                );
            });
        })
        .detach();
    }

    /// main `app:restart-after-migration`.
    /// main `app:restart-after-migration`: stop managed Wine (fail closed),
    /// clean updater leftovers, clear the marker, then relaunch the installed app.
    pub(super) fn restart_after_migration(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        cx.spawn(async move |_, cx| {
            let result = cx
                .background_executor()
                .spawn(async move {
                    let stop = live
                        .request("POST", "/steam/stop", None, MS(15_000))
                        .map(live::unwrap_data)
                        .map_err(|error| format!("Could not stop managed Wine processes: {error}"))?;
                    if !is_ok(&stop) || stop.get("running").and_then(Value::as_bool) != Some(false) {
                        return Err(error_text(Some(&stop)).unwrap_or_else(|| {
                            "Managed Wine processes did not stop; MetalSharp was not relaunched.".into()
                        }));
                    }
                    let _ = live.request("POST", "/update/cleanup", None, MS(10_000));
                    let home = live.home();
                    if let Some(dmg) = crate::updater_bridge::read_install_status(&home)
                        .and_then(|s| s.get("dmg_path").and_then(Value::as_str).map(std::path::PathBuf::from))
                        .filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("dmg")))
                    {
                        let inside_updates = dmg.starts_with(home.join("cache/updates"));
                        let ours = dmg
                            .file_name()
                            .is_some_and(|n| n.to_string_lossy().to_lowercase().starts_with("metalsharp-"));
                        if inside_updates || ours {
                            let _ = std::fs::remove_file(&dmg);
                        }
                    }
                    crate::updater_bridge::clear_install_status(&home);
                    if let Ok(entries) = std::fs::read_dir("/Applications") {
                        for entry in entries.flatten() {
                            let name = entry.file_name().to_string_lossy().into_owned();
                            let stale = [".MetalSharp.app.previous.", ".MetalSharp.app.update."]
                                .iter()
                                .any(|prefix| {
                                    name.strip_prefix(prefix)
                                        .is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit()))
                                });
                            if stale {
                                crate::host_actions::remove_path(&entry.path());
                            }
                        }
                    }
                    let _ = std::fs::remove_file(home.join(".post-update-migration"));
                    live.stop_backend();
                    Ok(())
                })
                .await;
            let _ = cx.update(|cx| match result {
                Ok(()) => {
                    relaunch_after_exit();
                    cx.quit();
                }
                Err(error) => toast::error(cx, error),
            });
        })
        .detach();
    }

    // ───────────────────────────── streaming ─────────────────────────────

    pub(super) fn open_streaming_panel(&mut self, cx: &mut Context<Self>) {
        self.close_library_menus();
        self.streaming_open = true;
        self.streaming_unpair_confirm = false;
        if self.live.enabled {
            // StreamingOverlay remounts with fresh local state.
            self.live.streaming.session += 1;
            self.live.streaming.polling = false;
            self.live.streaming.installing = false;
            self.live.streaming.launching = false;
            self.live.streaming.stopping = false;
            self.live.streaming.pairing = false;
            self.streaming_refresh(cx, |_, _, _| {});
        }
        cx.notify();
    }

    pub(super) fn streaming_refresh(
        &mut self,
        cx: &mut Context<Self>,
        then: impl FnOnce(&mut Self, Option<crate::streaming::StreamingStatus>, &mut Context<Self>)
        + 'static,
    ) {
        live::call(
            cx,
            "GET",
            "/streaming/status",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, status, cx| {
                let parsed = status
                    .filter(is_ok)
                    .and_then(|s| {
                        serde_json::from_value::<crate::streaming::StreamingStatus>(s).ok()
                    })
                    .map(crate::streaming::StreamingStatus::sanitize);
                if let Some(status) = &parsed {
                    this.live.streaming.status = Some(status.clone());
                }
                cx.notify();
                then(this, parsed, cx);
            },
        );
    }

    fn streaming_begin_polling(&mut self, cx: &mut Context<Self>) {
        let session = self.live.streaming.session;
        if self.live.streaming.polling {
            return;
        }
        self.live.streaming.polling = true;
        self.streaming_poll(session, cx);
    }

    fn streaming_poll(&mut self, session: u64, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(1500)).await;
            let _ = this.update(cx, move |this, cx| {
                if session != this.live.streaming.session || !this.streaming_open {
                    this.live.streaming.polling = false;
                    return;
                }
                this.streaming_refresh(cx, move |this, status, cx| {
                    let Some(status) = status else {
                        this.streaming_poll(session, cx);
                        return;
                    };
                    if status.installing {
                        this.streaming_poll(session, cx);
                        return;
                    }
                    this.live.streaming.polling = false;
                    this.live.streaming.installing = false;
                    match status.progress_status.as_deref() {
                        Some("complete") => {
                            toast::success(cx, "Sunshine installed — launching it now");
                            this.streaming_launch(cx);
                        }
                        Some("error") => toast::error(
                            cx,
                            status
                                .progress_detail
                                .clone()
                                .unwrap_or_else(|| "Sunshine installation failed".into()),
                        ),
                        _ => {}
                    }
                });
            });
        })
        .detach();
    }

    pub(super) fn streaming_install(&mut self, cx: &mut Context<Self>) {
        if self.live.streaming.installing {
            return;
        }
        self.live.streaming.installing = true;
        live::call(
            cx,
            "POST",
            "/streaming/install",
            None,
            MS(30_000),
            |this, result, cx| {
                if !result.as_ref().is_some_and(is_ok) {
                    this.live.streaming.installing = false;
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Failed to start the Sunshine download".into()),
                    );
                    cx.notify();
                    return;
                }
                this.streaming_refresh(cx, |this, _, cx| this.streaming_begin_polling(cx));
            },
        );
    }

    pub(super) fn streaming_launch(&mut self, cx: &mut Context<Self>) {
        if self.live.streaming.launching {
            return;
        }
        self.live.streaming.launching = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/streaming/launch",
            None,
            MS(75_000),
            |this, result, cx| {
                this.live.streaming.launching = false;
                if !result.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Could not launch Sunshine".into()),
                    );
                    cx.notify();
                    return;
                }
                if result
                    .as_ref()
                    .and_then(|r| r.get("creds_recognized"))
                    .and_then(Value::as_bool)
                    == Some(false)
                {
                    toast::error(
                        cx,
                        "Sunshine is running, but it uses different web credentials — manage them in the Sunshine web UI",
                    );
                } else {
                    toast::success(cx, "Sunshine is running — pair your device below");
                }
                this.streaming_refresh(cx, |_, _, _| {});
            },
        );
    }

    pub(super) fn streaming_stop(&mut self, cx: &mut Context<Self>) {
        if self.live.streaming.stopping {
            return;
        }
        self.live.streaming.stopping = true;
        live::call(
            cx,
            "POST",
            "/streaming/stop",
            None,
            live::DEFAULT_TIMEOUT,
            |this, _, cx| {
                this.live.streaming.stopping = false;
                this.streaming_refresh(cx, |_, _, _| {});
            },
        );
    }

    pub(super) fn streaming_pair(&mut self, cx: &mut Context<Self>) {
        if self.live.streaming.pairing {
            return;
        }
        let Some(pin_input) = self.streaming_pin.clone() else {
            return;
        };
        let pin = pin_input.read(cx).content.trim().to_owned();
        if pin.len() != 4 || !pin.bytes().all(|b| b.is_ascii_digit()) {
            toast::error(cx, "Enter the 4-digit PIN shown in Moonlight");
            return;
        }
        self.live.streaming.pairing = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/streaming/pin",
            Some(json!({"pin": pin})),
            MS(95_000),
            move |this, result, cx| {
                this.live.streaming.pairing = false;
                if result.as_ref().is_some_and(is_ok) {
                    pin_input.update(cx, |input, cx| input.clear(cx));
                    toast::success(
                        cx,
                        "Paired! Open Moonlight on your device and tap your Mac to start streaming",
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref()).unwrap_or_else(|| "Pairing failed".into()),
                    );
                }
                this.streaming_refresh(cx, |_, _, _| {});
            },
        );
    }

    pub(super) fn streaming_unpair_all(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/streaming/unpair-all",
            None,
            MS(30_000),
            |this, result, cx| {
                if result.as_ref().is_some_and(is_ok) {
                    toast::success(cx, "All paired devices removed");
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Could not unpair devices".into()),
                    );
                }
                this.streaming_refresh(cx, |_, _, _| {});
            },
        );
    }
}

// ───────────────────────────── host helpers ─────────────────────────────

/// main `isFirstLaunch`: `setup.json` missing, unparsable or not completed.
/// `metalsharp-activate-pid` in-process: bring a regular GUI app's windows to
/// the front. Returns true once that process is the active application.
/// Non-GUI Wine processes (wineserver, services) have no running application.
#[cfg(target_os = "macos")]
fn activate_wine_app(pid: i64) -> bool {
    use objc2::msg_send;
    use objc2::runtime::{AnyClass, AnyObject, Bool, Sel};
    let Ok(pid) = i32::try_from(pid) else {
        return false;
    };
    let Some(class) = AnyClass::get(c"NSRunningApplication") else {
        return false;
    };
    unsafe {
        let app: *mut AnyObject = msg_send![class, runningApplicationWithProcessIdentifier: pid];
        let Some(app) = app.as_ref() else {
            return false;
        };
        // NSApplicationActivationPolicyRegular only: skip Wine's background agents.
        let policy: isize = msg_send![app, activationPolicy];
        if policy != 0 {
            return false;
        }
        let active: Bool = msg_send![app, isActive];
        if active.as_bool() {
            return true;
        }
        // macOS 14 cooperative activation: the active app yields first.
        if let Some(ns_app_class) = AnyClass::get(c"NSApplication") {
            let ns_app: *mut AnyObject = msg_send![ns_app_class, sharedApplication];
            if let Some(ns_app) = ns_app.as_ref() {
                let yield_sel = Sel::register(c"yieldActivationToApplication:");
                let responds: Bool = msg_send![ns_app, respondsToSelector: yield_sel];
                if responds.as_bool() {
                    let _: () = msg_send![ns_app, yieldActivationToApplication: app];
                }
            }
        }
        // NSApplicationActivateAllWindows
        let _: Bool = msg_send![app, activateWithOptions: 1usize << 0];
    }
    false
}

#[cfg(not(target_os = "macos"))]
fn activate_wine_app(_pid: i64) -> bool {
    true
}

fn is_first_launch(home: &std::path::Path) -> bool {
    std::fs::read_to_string(home.join("setup.json"))
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .map(|setup| setup.get("completed").and_then(Value::as_bool) != Some(true))
        .unwrap_or(true)
}

/// main `ensureMetalsharpDirs`.
fn ensure_metalsharp_dirs(home: &std::path::Path) {
    for dir in [
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
    ] {
        let _ = std::fs::create_dir_all(home.join(dir));
    }
}

/// main `checkNeedsMigration` (post-update marker + backend check).
fn check_needs_migration(live: &Live) -> bool {
    let home = live.home();
    let marker_path = home.join(".post-update-migration");
    let clear = || {
        let _ = std::fs::remove_file(&marker_path);
    };
    let Ok(text) = std::fs::read_to_string(home.join("setup.json")) else {
        clear();
        return false;
    };
    let Ok(setup) = serde_json::from_str::<Value>(&text) else {
        clear();
        return false;
    };
    if setup.get("completed").and_then(Value::as_bool) != Some(true)
        && !home.join("prefix-steam").exists()
    {
        clear();
        return false;
    }
    let marker = std::fs::read_to_string(&marker_path)
        .ok()
        .and_then(|t| serde_json::from_str::<Value>(&t).ok());
    let needed = marker
        .as_ref()
        .and_then(|m| m.get("needed"))
        .and_then(Value::as_bool)
        == Some(true);
    if !needed {
        return false;
    }
    match live.request("GET", "/update/migrate/check", None, MS(3000)) {
        Ok(result) => {
            let result = live::unwrap_data(result);
            let needs =
                is_ok(&result) && result.get("needed").and_then(Value::as_bool) == Some(true);
            if !needs {
                clear();
            }
            needs
        }
        Err(_) => needed,
    }
}

pub(super) fn run_bash_script(script: &str, live: &Live) -> Result<String, String> {
    use std::io::Write;
    let tools = live.resources().join("tools");
    let path = std::env::join_paths(
        [tools.to_string_lossy().into_owned()]
            .into_iter()
            .chain(
                [
                    "/opt/homebrew/bin",
                    "/usr/local/bin",
                    "/usr/local/sbin",
                    "/usr/bin",
                    "/bin",
                    "/usr/sbin",
                    "/sbin",
                ]
                .map(str::to_owned),
            )
            .chain(std::env::var("PATH").ok()),
    )
    .map_err(|e| e.to_string())?;
    let mut command = std::process::Command::new("/bin/bash");
    crate::lifecycle::unmask_child_signals(&mut command);
    let mut child = command
        .env("PATH", path)
        .env("METALSHARP_HOME", live.home())
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(script.as_bytes());
    }
    let output = child.wait_with_output().map_err(|e| e.to_string())?;
    let text = format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    )
    .trim()
    .to_owned();
    if output.status.success() {
        Ok(text)
    } else if text.is_empty() {
        Err(format!(
            "Steam fix exited with code {}",
            output.status.code().unwrap_or(-1)
        ))
    } else {
        Err(text)
    }
}

/// `showLaunchQuitHint`: 5 s after launch, a native notice that ⌘Q in the game
/// returns to MetalSharp (main `app:show-launch-overlay`).
fn show_launch_quit_hint(game: String, cx: &mut Context<MetalSharpApp>) {
    cx.spawn(async move |_, cx| {
        cx.background_executor().timer(MS(5000)).await;
        let _ = cx.update(|cx| crate::launch_overlay::show(cx, &game));
    })
    .detach();
}

/// main `spawnFreshInstalledAppAfterExit`: open the installed bundle once we exit.
fn relaunch_after_exit() {
    let installed = std::path::Path::new("/Applications/MetalSharp.app");
    let bundle = std::env::current_exe().ok().and_then(|exe| {
        exe.ancestors()
            .find(|p| p.extension().is_some_and(|e| e == "app"))
            .map(|p| p.to_path_buf())
    });
    let target = if installed.exists() {
        installed.to_path_buf()
    } else if let Some(bundle) = bundle.filter(|b| !b.starts_with("/Volumes/")) {
        bundle
    } else {
        relaunch_self();
        return;
    };
    let pid = std::process::id();
    let script = format!(
        "while kill -0 {pid} 2>/dev/null; do sleep 0.2; done; /usr/bin/open -n \"{}\"",
        target.to_string_lossy().replace('"', "\\\"")
    );
    let mut command = std::process::Command::new("/bin/sh");
    command.arg("-c").arg(script);
    crate::lifecycle::unmask_child_signals(&mut command);
    unsafe {
        use std::os::unix::process::CommandExt;
        command.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    let _ = command
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn();
}

pub(super) fn relaunch_self() {
    // Relaunch the enclosing .app bundle (Electron app.relaunch()).
    if let Ok(exe) = std::env::current_exe() {
        let bundle = exe
            .ancestors()
            .find(|p| p.extension().is_some_and(|e| e == "app"))
            .map(|p| p.to_path_buf());
        match bundle {
            Some(bundle) => {
                let _ = std::process::Command::new("/usr/bin/open")
                    .arg("-n")
                    .arg(bundle)
                    .spawn();
            }
            None => {
                let mut command = std::process::Command::new(exe);
                crate::lifecycle::unmask_child_signals(&mut command);
                let _ = command.spawn();
            }
        }
    }
}

#[derive(Default)]
pub(super) struct MigrationViewState {
    pub status: String,
    pub step: u64,
    pub total: u64,
    pub message: String,
    pub error: String,
}

#[cfg(test)]
mod ubisoft_header_tests {
    use super::LiveState;

    #[test]
    fn launch_ubisoft_takes_the_primary_button_until_connect_closes() {
        let mut live = LiveState::default();
        assert!(!live.ubisoft_is_primary());

        // Connect running without Launch Ubisoft (e.g. started for a game) keeps Start Steam.
        live.apply_ubisoft_status(true, true, false);
        assert!(!live.ubisoft_is_primary());

        // Launch Ubisoft: the primary button becomes Stop Ubisoft, also while installing.
        live.ubisoft_primary = true;
        assert!(live.ubisoft_is_primary());
        live.apply_ubisoft_status(false, false, true);
        assert!(live.ubisoft_is_primary());
        live.apply_ubisoft_status(true, true, false);
        assert!(live.ubisoft_is_primary());

        // Connect seen closed: Start Steam returns and stays after a later unrelated start.
        live.apply_ubisoft_status(true, false, false);
        assert!(!live.ubisoft_is_primary());
        live.apply_ubisoft_status(true, true, false);
        assert!(!live.ubisoft_is_primary());
    }
}
