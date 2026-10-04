//! Opt-in connected application. The default synthetic preview never constructs
//! this view, a backend host, or any credential field.
use crate::{
    backend::{
        BackendClient, BackendError, Game, InstallProgress, Launcher, LauncherStatus,
        SetupDependencies, SetupState,
    },
    backend_host::{BackendHost, HostConfig},
    configuration::{
        ControllerInput, GameResolution, PreferenceChange, RuntimePreferences, WindowMode,
    },
    search_input::SearchInput,
};
use gpui::{AppContext, Context, Render, Window, div, prelude::*, px, rgb, rgba};
use serde_json::{Value, json};
use std::time::Duration;

#[derive(Default)]
struct Snapshot {
    setup: SetupState,
    steam: LauncherStatus,
    ubisoft: LauncherStatus,
    progress: InstallProgress,
    preferences: RuntimePreferences,
    dependencies: SetupDependencies,
    games: Vec<Game>,
    running: std::collections::HashSet<u64>,
}
impl Snapshot {
    fn load_setup(client: &BackendClient) -> Result<Self, BackendError> {
        Ok(Self {
            setup: client.setup_state()?,
            steam: client.launcher_status(Launcher::Steam)?,
            progress: client.install_progress()?,
            dependencies: client.setup_dependencies()?,
            ..Default::default()
        })
    }
    fn load_status(client: &BackendClient) -> Result<Self, BackendError> {
        let setup = client.setup_state()?;
        let steam = client.launcher_status(Launcher::Steam)?;
        let ubisoft = client.launcher_status(Launcher::Ubisoft)?;
        let progress = client.install_progress()?;
        let preferences = client.configuration()?;
        let result: Value = client.get("/game/running")?;
        let running = result
            .get("running")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter_map(|game| game.get("appid").and_then(Value::as_u64))
            .collect();
        Ok(Self {
            setup,
            steam,
            ubisoft,
            progress,
            preferences,
            dependencies: Default::default(),
            running,
            games: Vec::new(),
        })
    }
    fn load(client: &BackendClient) -> Result<Self, BackendError> {
        let mut snapshot = Self::load_status(client)?;
        snapshot.dependencies = client.setup_dependencies()?;
        let mut games = client.library(Launcher::Steam)?.games;
        let mut ubisoft_games = client.library(Launcher::Ubisoft)?.games;
        for game in &mut ubisoft_games {
            game.source = "ubisoft".into();
        }
        games.extend(ubisoft_games);
        snapshot.games = games;
        Ok(snapshot)
    }
}

// Never derive Debug: variants may transiently carry secrets.
enum Operation {
    Refresh,
    ReadSettings,
    Poll,
    InstallRuntime,
    InstallSteam,
    Launcher(Launcher, bool),
    SaveSteamKey(String),
    SaveGamesDbKey(String),
    Finish(String, String, String),
    Launch(Game),
    InstallGame(u64),
    SavePipeline(Game, String),
    SaveExecutable(Game, String),
    Preference(PreferenceChange),
    ReadDiagnostics,
    ReadStreaming,
    Streaming(crate::streaming::StreamingAction),
    Settings(crate::settings_connected::SettingsIntent),
    Logs(
        crate::logs_connected::LogsIntent,
        crate::logs_connected::LogsController,
    ),
    Recovery(
        crate::update_recovery_ui::RecoveryIntent,
        crate::migration_connected::MigrationSession,
        Option<crate::updater_connected::UpdateSnapshot>,
    ),
    Sharp(crate::sharp_connected::SharpIntent),
    Library(
        crate::connected_library::LibraryAction,
        crate::connected_library::ConnectedLibraryState,
        Option<String>,
    ),
    ReadProcesses(Vec<(u64, String)>),
    Process(
        crate::process_manager_connected::ProcessIntent,
        Vec<(u64, String)>,
    ),
    Stop(u64),
    GogLogin,
    EpicLogin,
    GogInitialize,
    EpicInstallSupport,
    GogCode(String),
    EpicCode(String),
}
impl Operation {
    fn run_settings(
        client: &BackendClient,
        intent: crate::settings_connected::SettingsIntent,
    ) -> Result<Value, BackendError> {
        use crate::settings_connected::{SettingsIntent as I, StorefrontAction as S};
        match intent {
            I::SavePreference(change) => client.save_preference(change).map(|_| json!({})),
            I::SaveSteamKey(key) => client.save_steam_key(&key),
            I::SaveGamesDbKey(key) => client.save_gamesdb_key(&key).map(|_| json!({})),
            I::ChangeDeviceName(name) => client.save_device_name(&name).map(|_| json!({})),
            I::OpenDataFolder => Self::open_owned_folder(client, false),
            I::OpenLogsFolder => Self::open_owned_folder(client, true),
            I::RepairDataAccess => {
                let home = client.backend_home()?;
                let metadata = std::fs::metadata(&home).map_err(|_| BackendError::Rejected)?;
                if !metadata.is_dir() {
                    return Err(BackendError::Rejected);
                }
                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let mode = metadata.permissions().mode();
                    let readable = mode & 0o400 != 0;
                    let writable = mode & 0o200 != 0;
                    let searchable = mode & 0o100 != 0;
                    Ok(
                        json!({"ok":readable&&writable&&searchable,"path":home,"checks":[{"name":"directory","ok":true},{"name":"owner_read","ok":readable},{"name":"owner_write","ok":writable},{"name":"owner_search","ok":searchable}]}),
                    )
                }
                #[cfg(not(unix))]
                {
                    Ok(json!({"ok":!metadata.permissions().readonly(),"path":home,"checks":[]}))
                }
            }

            I::ClearShaderCache => client.post("/cache/clear", json!({"type":"shader"})),
            I::ClearPipelineCache => client.post("/cache/clear", json!({"type":"pipeline"})),
            I::CheckForUpdates => client.get("/update/check"),
            I::ForceKillProcesses => client.post("/games/force-quit", json!({})),
            I::Storefront(S::StartWineSteam) => client
                .set_launcher_running(Launcher::Steam, true)
                .map(|_| json!({})),
            I::Storefront(S::StopWineSteam) => client
                .set_launcher_running(Launcher::Steam, false)
                .map(|_| json!({})),
            I::Storefront(S::StartMacSteam | S::StopMacSteam | S::InstallMacSteam) => {
                Err(BackendError::Rejected)
            }
            I::ReopenSetup
            | I::RestartBackend
            | I::InstallUpdate
            | I::InstallFexUpdate
            | I::Uninstall
            | I::ChangeLanguage(_)
            | I::ChoosePath(_) => Err(BackendError::Rejected),
        }
    }
    fn open_owned_folder(client: &BackendClient, logs: bool) -> Result<Value, BackendError> {
        let home = client.backend_home()?;
        let path = if logs {
            home.join("logs")
        } else {
            home.clone()
        };
        crate::desktop_host::DesktopHost::new([home])
            .and_then(|host| host.reveal(&path))
            .map_err(|_| BackendError::Rejected)?;
        Ok(json!({"ok":true}))
    }
    fn run_recovery(
        client: &BackendClient,
        intent: crate::update_recovery_ui::RecoveryIntent,
        mut migration: crate::migration_connected::MigrationSession,
        mut update: Option<crate::updater_connected::UpdateSnapshot>,
    ) -> Result<Value, BackendError> {
        use crate::update_recovery_ui::RecoveryIntent as I;
        let home = client.backend_home()?;
        let file = crate::updater_connected::InstallStatusFile::new(&home)
            .map_err(|_| BackendError::Rejected)?;
        let api = crate::updater_connected::NativeUpdateAdapter {
            backend: client,
            status_file: &file,
        };
        let mut notice = String::new();
        match intent {
            I::StartUpdate(v) => {
                update = crate::updater_connected::UpdateIntent::Start(v)
                    .execute(&api)
                    .map_err(|_| BackendError::Rejected)?
            }
            I::PollUpdateDownload => {
                update = crate::updater_connected::UpdateIntent::PollDownload(
                    update
                        .as_ref()
                        .map(|s| s.variant)
                        .unwrap_or(crate::updater_connected::UpdateVariant::Regular),
                )
                .execute(&api)
                .map_err(|_| BackendError::Rejected)?
            }
            I::PollUpdateInstall => {
                update = crate::updater_connected::UpdateIntent::PollInstall
                    .execute(&api)
                    .map_err(|_| BackendError::Rejected)?
            }
            I::ClearUpdateStatus => {
                crate::updater_connected::UpdateIntent::ClearStatus
                    .execute(&api)
                    .map_err(|_| BackendError::Rejected)?;
                update = None;
            }
            I::CheckMigration => {
                let check =
                    <BackendClient as crate::migration_connected::MigrationApi>::migration_check(
                        client,
                    )
                    .map_err(|_| BackendError::Rejected)?;
                notice = if check.needed {
                    format!("Migration required: {}", check.reason)
                } else {
                    "No migration required".into()
                };
            }
            I::StartMigration => {
                migration = crate::migration_connected::MigrationSession::start(client)
                    .map_err(|_| BackendError::Rejected)?
            }
            I::PollMigration => {
                migration.poll(client).map_err(|_| BackendError::Rejected)?;
            }
            I::DetachMigrationPolling => migration.detach_polling(),
            I::RestartAfterMigration => return Err(BackendError::Rejected),
        }
        serde_json::to_value((migration, update, notice)).map_err(|_| BackendError::InvalidJson)
    }
    fn run(self, client: &BackendClient) -> Result<Value, BackendError> {
        match self {
            Self::Refresh | Self::Poll | Self::ReadStreaming => Ok(json!({})),
            Self::ReadSettings => {
                let config: Value = client.get("/config")?;
                let gamesdb: Value = client.get("/sharp-library/epic/thegamesdb-api-key")?;
                let preferences: RuntimePreferences = serde_json::from_value(config.clone())
                    .map_err(|_| BackendError::InvalidJson)?;
                serde_json::to_value(crate::settings_connected::SettingsSnapshot {
                    preferences,
                    steam_configured: config
                        .get("steamApiKeySet")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                    gamesdb_configured: gamesdb
                        .get("configured")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                    device_name: String::new(),
                })
                .map_err(|_| BackendError::InvalidJson)
            }
            Self::Streaming(action) => client.streaming_action(action),
            Self::Library(action, state, path) => {
                use crate::connected_library::{
                    LibraryAction as A, LibraryActionResult as R, LibraryDispatchResult as D,
                };
                if let A::ChooseExecutable(id) = &action {
                    if path.is_none() {
                        return serde_json::to_value(D::ChooseExecutable(id.clone()))
                            .map_err(|_| BackendError::InvalidJson);
                    }
                }
                match action.execute(client, &state, path.as_deref())? {
                    R::UiOnly => {
                        serde_json::to_value(D::UiOnly).map_err(|_| BackendError::InvalidJson)
                    }
                    R::Refreshed(games) => {
                        serde_json::to_value(D::Games(games)).map_err(|_| BackendError::InvalidJson)
                    }
                    R::Completed => {
                        let games = match A::Refresh.execute(client, &state, None) {
                            Ok(R::Refreshed(games)) => Some(games),
                            _ => None,
                        };
                        serde_json::to_value(
                            games.map(D::Accepted).unwrap_or(D::AcceptedWithoutSnapshot),
                        )
                        .map_err(|_| BackendError::InvalidJson)
                    }
                }
            }
            Self::ReadProcesses(games) => serde_json::to_value(
                crate::process_manager_connected::ProcessSnapshot::load(client, &games)?,
            )
            .map_err(|_| BackendError::InvalidJson),
            Self::Process(intent, games) => {
                intent.execute(client)?;
                serde_json::to_value(crate::process_manager_connected::ProcessSnapshot::load(
                    client, &games,
                )?)
                .map_err(|_| BackendError::InvalidJson)
            }
            Self::Sharp(intent) => {
                let sharp = crate::sharp_connected::SharpLibraryClient::new(client.clone());
                let result = sharp.execute_intent(&intent)?;
                serde_json::to_value(result).map_err(|_| BackendError::InvalidJson)
            }
            Self::Recovery(intent, migration, update) => {
                Self::run_recovery(client, intent, migration, update)
            }
            Self::Settings(intent) => Self::run_settings(client, intent),
            Self::Logs(intent, controller) => match intent {
                crate::logs_connected::LogsIntent::Refresh { .. } => serde_json::to_value(
                    crate::logs_connected::LogsConnected::execute(client, intent)?,
                )
                .map_err(|_| BackendError::InvalidJson),
                crate::logs_connected::LogsIntent::Export { destination } => {
                    crate::logs_connected::LogsConnected::execute_export(
                        client,
                        &controller,
                        &destination,
                    )
                    .map_err(|_| BackendError::Rejected)?;
                    Ok(json!({"exported":true}))
                }
                crate::logs_connected::LogsIntent::OpenLogsFolder => {
                    let home = client.backend_home()?;
                    crate::desktop_host::DesktopHost::new([home.clone()])
                        .and_then(|host| host.reveal(&home.join("logs")))
                        .map_err(|_| BackendError::Rejected)?;
                    Ok(json!({"opened":true}))
                }
            },
            Self::InstallRuntime => client.install_runtime().map(|_| json!({})),
            Self::InstallSteam => client.install_steam().map(|_| json!({})),
            Self::Launcher(launcher, running) => client
                .set_launcher_running(launcher, running)
                .map(|_| json!({})),
            Self::SaveSteamKey(key) => client.save_steam_key(&key),
            Self::SaveGamesDbKey(key) => client.save_gamesdb_key(&key).map(|_| json!({})),
            Self::Finish(device, steam, gamesdb) => client
                .complete_setup(&device, &steam, &gamesdb)
                .map(|_| json!({})),
            Self::Launch(game) => client.launch_game(&game),
            Self::InstallGame(appid) => client.install_game(appid).map(|_| json!({})),
            Self::SavePipeline(game, pipeline) => {
                client.save_pipeline(&game, &pipeline).map(|_| json!({}))
            }
            Self::SaveExecutable(game, path) => {
                client.save_executable(&game, &path).map(|_| json!({}))
            }
            Self::Preference(change) => client.save_preference(change).map(|_| json!({})),
            Self::ReadDiagnostics => client
                .diagnostic_logs()
                .and_then(|logs| serde_json::to_value(logs).map_err(|_| BackendError::InvalidJson)),
            Self::Stop(appid) => client.stop_game(appid).map(|_| json!({})),
            Self::GogLogin => client.get("/sharp-library/gog/status"),
            Self::EpicLogin => {
                let status: Value = client.get("/sharp-library/epic/status")?;
                if status.get("toolAvailable").and_then(Value::as_bool) != Some(true) {
                    client.post("/sharp-library/epic/install-tool", json!({}))?;
                }
                Ok(json!({"authUrl":"https://legendary.gl/epiclogin"}))
            }
            Self::GogInitialize => client.post("/sharp-library/gog/initialize-prefix", json!({})),
            Self::EpicInstallSupport => client.post("/sharp-library/epic/install-tool", json!({})),
            Self::GogCode(code) => {
                client.post("/sharp-library/gog/auth-code", json!({"code":code}))?;
                client.post("/sharp-library/gog/sync", json!({}))
            }
            Self::EpicCode(code) => {
                client.post("/sharp-library/epic/auth", json!({"code":code}))?;
                client.post("/sharp-library/epic/sync", json!({}))
            }
        }
    }
}

pub(crate) struct SetupViewState {
    pub ready: bool,
    pub install_ready: bool,
    pub action_available: bool,
    pub completed: bool,
    pub runtime_ready: bool,
    pub runtime_installing: bool,
    pub runtime_started: bool,
    pub percent: usize,
    pub steam_installed: bool,
    pub steam_installing: bool,
}

pub(crate) enum ConnectedEvent {
    OpenStreaming,
    OpenSetup,
}
impl gpui::EventEmitter<ConnectedEvent> for ConnectedApp {}
pub struct ConnectedApp {
    streaming_status: Option<crate::streaming::StreamingStatus>,
    streaming_watch: crate::streaming_watch::StreamingWatch,
    streaming_pin: gpui::Entity<SearchInput>,
    has_snapshot: bool,
    config: HostConfig,
    host: Option<BackendHost>,
    snapshot: Snapshot,
    busy: bool,
    busy_is_poll: bool,
    pending_setup_action: Option<PendingSetupAction>,
    notice: String,
    validation: bool,
    device: gpui::Entity<SearchInput>,
    steam_key: gpui::Entity<SearchInput>,
    gamesdb_key: gpui::Entity<SearchInput>,
    epic_code: gpui::Entity<SearchInput>,
    oauth_rx: Option<std::sync::mpsc::Receiver<crate::mini_browser::MiniBrowserResult>>,
    oauth_active: bool,
    diagnostics: crate::diagnostics::DiagnosticLogs,
    panel: Option<ConnectedPanel>,
    settings_panel: gpui::Entity<crate::settings_connected::SettingsConnected>,
    logs_panel: gpui::Entity<crate::logs_connected::LogsConnected>,
    recovery_panel: gpui::Entity<crate::update_recovery_ui::RecoveryPanel>,
    recovery_sender: std::sync::mpsc::Sender<crate::update_recovery_ui::RecoveryIntent>,
    recovery_receiver: std::sync::mpsc::Receiver<crate::update_recovery_ui::RecoveryIntent>,
    recovery_migration: crate::migration_connected::MigrationSession,
    recovery_update: Option<crate::updater_connected::UpdateSnapshot>,
    process_panel: gpui::Entity<crate::process_manager_connected::ProcessManagerView>,
    library_panel: gpui::Entity<crate::connected_library::ConnectedLibraryView>,
    sharp_panel: gpui::Entity<crate::sharp_connected::SharpConnectedView>,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PendingSetupAction {
    InstallRuntime,
    InstallSteam,
}
fn queue_setup_action(
    busy: bool,
    busy_is_poll: bool,
    pending: Option<PendingSetupAction>,
    operation: &Operation,
) -> Option<PendingSetupAction> {
    if !busy || !busy_is_poll || pending.is_some() {
        return pending;
    }
    match operation {
        Operation::InstallRuntime => Some(PendingSetupAction::InstallRuntime),
        Operation::InstallSteam => Some(PendingSetupAction::InstallSteam),
        _ => None,
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ConnectedPanel {
    Library,
    Settings,
    Sharp,
    Logs,
    Processes,
    Recovery,
}
impl ConnectedApp {
    pub fn new(config: HostConfig, cx: &mut Context<Self>) -> Self {
        let input = |placeholder: &str, secret: bool, cx: &mut Context<Self>| {
            cx.new(|cx| {
                let mut input = SearchInput::new(cx);
                input.placeholder = placeholder.to_owned().into();
                input.secret = secret;
                input
            })
        };
        let settings_panel = cx.new(|cx| {
            crate::settings_connected::SettingsConnected::new(
                crate::settings_connected::SettingsSnapshot::default(),
                crate::page_palette::PagePalette::default(),
                cx,
            )
        });
        let logs_panel = cx.new(crate::logs_connected::LogsConnected::new);
        let (recovery_sender, recovery_receiver) = std::sync::mpsc::channel();
        let recovery_tx = recovery_sender.clone();
        let recovery_panel = cx.new(|_| {
            crate::update_recovery_ui::RecoveryPanel::new(
                crate::update_recovery_ui::RecoverySnapshot::default(),
                move |intent| {
                    let _ = recovery_tx.send(intent);
                },
            )
        });
        let library_panel = cx
            .new(|cx| crate::connected_library::ConnectedLibraryView::new(cx, Default::default()));
        let process_panel = cx
            .new(|_| crate::process_manager_connected::ProcessManagerView::new(Default::default()));
        let sharp_panel = cx.new(|_| {
            crate::sharp_connected::SharpConnectedView::new(
                crate::sharp_connected::SharpSource::Installers,
            )
        });
        let view = Self {
            panel: None,
            settings_panel,
            logs_panel,
            recovery_panel,
            recovery_sender,
            recovery_receiver,
            recovery_migration: Default::default(),
            recovery_update: None,
            process_panel,
            library_panel,
            sharp_panel,
            streaming_status: None,
            streaming_watch: Default::default(),
            streaming_pin: input("PIN", true, cx),
            has_snapshot: false,
            config: config.clone(),
            validation: config.validation,
            host: None,
            snapshot: Snapshot::default(),
            busy: true,
            busy_is_poll: false,
            pending_setup_action: None,
            notice: "Starting owned C backend…".into(),
            device: input("Device name", false, cx),
            steam_key: input("Steam Web API key (optional)", true, cx),
            gamesdb_key: input("TheGamesDB key (optional)", true, cx),
            epic_code: input("Epic authorization code", true, cx),
            oauth_rx: None,
            oauth_active: false,
            diagnostics: Default::default(),
        };
        cx.subscribe(&view.process_panel, |this, _, event, cx| {
            if this.busy {
                return;
            }
            let games = this
                .snapshot
                .games
                .iter()
                .map(|g| (g.appid, g.name.clone()))
                .collect();
            this.process_panel.update(cx, |p, cx| p.set_busy(true, cx));
            this.dispatch(Operation::Process(event.0, games), cx);
        });
        cx.subscribe(&view.settings_panel, |this, _, event, cx| match event {
            crate::settings_connected::SettingsConnectedEvent::Close => {
                this.panel = None;
                cx.notify();
            }
            crate::settings_connected::SettingsConnectedEvent::Intent(intent) => {
                if this.busy {
                    this.settings_panel.update(cx, |panel, cx| {
                        panel.host_operation_finished(
                            Err("Another backend action is in progress; try again shortly".into()),
                            cx,
                        )
                    });
                    return;
                }
                this.settings_panel
                    .update(cx, |panel, cx| panel.host_operation_started(cx));
                if matches!(
                    &intent,
                    crate::settings_connected::SettingsIntent::ReopenSetup
                ) {
                    this.panel = None;
                    cx.emit(ConnectedEvent::OpenSetup);
                    this.settings_panel.update(cx, |panel, cx| {
                        panel.host_operation_finished(Ok("Setup reopened".into()), cx)
                    });
                } else if matches!(
                    &intent,
                    crate::settings_connected::SettingsIntent::RestartBackend
                ) {
                    this.restart_backend(cx);
                } else if matches!(
                    &intent,
                    crate::settings_connected::SettingsIntent::InstallUpdate
                        | crate::settings_connected::SettingsIntent::InstallFexUpdate
                ) {
                    let variant = if matches!(
                        &intent,
                        crate::settings_connected::SettingsIntent::InstallFexUpdate
                    ) {
                        crate::updater_connected::UpdateVariant::Fex
                    } else {
                        crate::updater_connected::UpdateVariant::Regular
                    };
                    this.panel = Some(ConnectedPanel::Recovery);
                    this.update_recovery_panel(cx);
                    let _ = this.recovery_sender.send(
                        crate::update_recovery_ui::RecoveryIntent::StartUpdate(variant),
                    );
                    this.settings_panel.update(cx, |panel, cx| {
                        panel.host_operation_finished(
                            Ok("Update workflow opened; download is queued".into()),
                            cx,
                        )
                    });
                } else {
                    this.dispatch(Operation::Settings(intent.clone()), cx)
                }
            }
        });
        cx.spawn(async move |this, cx| {
            let result = cx.background_executor().spawn(async move { BackendHost::start(&config).map_err(|_| "Could not start the owned C backend. Check resources and port ownership.".to_owned()) }).await;
            let _ = this.update(cx, |this, cx| {
                this.busy = false;
                match result {
                    Ok(host) => {
                        #[cfg(feature="browser-fixture")]
                        println!("GPUI_OWNED_BACKEND_READY {}",host.pid());
                        this.host = Some(host); this.notice.clear();this.dispatch(Operation::Poll,cx);
                    }
                    Err(error) => this.notice = error,
                }
                cx.notify();
            });
            let mut ticks=0u64;
            let mut full_refresh_due=false;
            loop {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                ticks=ticks.wrapping_add(1);
                if ticks%15==0 {full_refresh_due=true;}
                if this.update(cx, |this, cx| {
                    if crate::lifecycle::quit_requested() {cx.quit();return;}
                    if this.busy { return; }
                    if let Some(result) = this.oauth_rx.as_ref().and_then(|rx|rx.try_recv().ok()) {
                        this.oauth_rx = None; this.oauth_active = false;
                        match result {
                            crate::mini_browser::MiniBrowserResult::GogCode(code) => this.dispatch(Operation::GogCode(code),cx),
                            crate::mini_browser::MiniBrowserResult::EpicCode(code) => this.dispatch(Operation::EpicCode(code),cx),
                            crate::mini_browser::MiniBrowserResult::Cancelled => this.notice = "Sign-in cancelled".into(),
                            _ => this.notice = "Sign-in failed".into(),
                        }
                        cx.notify();
                    }
                    if !this.busy&&this.host.is_some() {
                        if let Some(operation)=this.take_panel_operation(cx){this.dispatch(operation,cx);return;}
                        // Progress/status remain lightweight. The single-threaded C
                        // server must not be monopolized by a full library scan every second.
                        this.dispatch(if full_refresh_due {full_refresh_due=false;Operation::Refresh}else if this.streaming_watch.poll_due(std::time::Instant::now()) {Operation::ReadStreaming}else{Operation::Poll},cx);
                    }
                }).is_err() { break; }
            }
        }).detach();
        // GPUI invokes quit callbacks before dropping windows. Do not leave the
        // owned backend alive if the UI entity is retained during application quit.
        cx.on_app_quit(|this, _| {
            if let Some(host) = this.host.as_mut() {
                host.stop();
            }
            async {}
        })
        .detach();
        view
    }
    #[cfg(test)]
    pub(crate) fn fixture_streaming_status(
        &mut self,
        status: Option<crate::streaming::StreamingStatus>,
        cx: &mut Context<Self>,
    ) {
        self.streaming_status = status;
        cx.notify();
    }
    pub(crate) fn streaming_visible(&self) -> bool {
        self.streaming_watch.is_open()
    }
    pub(crate) fn streaming_view(
        &self,
    ) -> (
        Option<crate::streaming::StreamingStatus>,
        bool,
        gpui::Entity<SearchInput>,
        String,
    ) {
        (
            self.streaming_status.clone(),
            self.busy,
            self.streaming_pin.clone(),
            self.notice.clone(),
        )
    }
    pub(crate) fn open_streaming(&mut self, cx: &mut Context<Self>) {
        self.streaming_watch.open();
        self.streaming_status = None;
        self.dispatch(Operation::ReadStreaming, cx);
        cx.emit(ConnectedEvent::OpenStreaming);
    }
    pub(crate) fn close_streaming(&mut self, cx: &mut Context<Self>) {
        self.streaming_watch.close();
        self.streaming_pin.update(cx, |input, cx| input.clear(cx));
    }
    pub(crate) fn streaming_command(
        &mut self,
        action: crate::streaming::StreamingAction,
        cx: &mut Context<Self>,
    ) {
        use crate::streaming::StreamingAction;
        let Some(status) = &self.streaming_status else {
            return;
        };
        let allowed = status.allows(&action);
        if !allowed || self.busy || !self.streaming_watch.is_open() {
            return;
        }
        if let StreamingAction::Pair(pin) = &action {
            if pin.len() != 4 || !pin.bytes().all(|byte| byte.is_ascii_digit()) {
                self.notice = "Enter the four-digit PIN shown in Moonlight".into();
                cx.notify();
                return;
            }
            self.streaming_pin.update(cx, |input, cx| input.clear(cx));
        }
        self.dispatch(Operation::Streaming(action), cx);
    }
    pub(crate) fn streaming_pair(&mut self, cx: &mut Context<Self>) {
        let pin = self.streaming_pin.read(cx).content.to_string();
        self.streaming_command(crate::streaming::StreamingAction::Pair(pin), cx);
    }
    fn take_panel_operation(&mut self, cx: &mut Context<Self>) -> Option<Operation> {
        match self.panel {
            Some(ConnectedPanel::Recovery) => match self.recovery_receiver.try_recv() {
                Ok(crate::update_recovery_ui::RecoveryIntent::RestartAfterMigration) => {
                    if self.recovery_migration.status
                        == crate::migration_connected::MigrationStatus::Complete
                    {
                        self.restart_backend(cx);
                    }
                    None
                }
                Ok(intent) => Some(Operation::Recovery(
                    intent,
                    self.recovery_migration.clone(),
                    self.recovery_update.clone(),
                )),
                Err(_) => None,
            },
            Some(ConnectedPanel::Library) => {
                let panel = self.library_panel.clone();
                panel.update(cx, |panel, _| {
                    panel
                        .take_backend_action()
                        .map(|action| Operation::Library(action, panel.state.clone(), None))
                })
            }
            Some(ConnectedPanel::Sharp) => {
                let panel = self.sharp_panel.clone();
                panel.update(cx, |panel, _| {
                    panel
                        .take_intents()
                        .into_iter()
                        .next()
                        .map(Operation::Sharp)
                })
            }
            Some(ConnectedPanel::Logs) => {
                let panel = self.logs_panel.clone();
                panel.update(cx, |panel, _| {
                    panel
                        .take_intent()
                        .map(|intent| Operation::Logs(intent, panel.controller.clone()))
                })
            }
            _ => None,
        }
    }
    fn handle_sharp_host_effect(
        &mut self,
        effect: crate::sharp_connected::SharpHostEffect,
        cx: &mut Context<Self>,
    ) {
        self.sharp_panel
            .update(cx, |panel, cx| panel.set_busy(false, cx));
        use crate::sharp_connected::{SharpHostEffect as H, SharpSource as S};
        match effect{H::Authenticate(S::Gog)=>self.dispatch(Operation::GogLogin,cx),H::Authenticate(S::Epic)=>self.dispatch(Operation::EpicLogin,cx),H::Authenticate(source)=>self.notice=format!("Authentication is not supported for {source:?}"),H::PickImport{source,kind}=>self.pick_sharp_path(source,kind,false,cx),H::PickLibraryRoot(source)=>self.pick_sharp_path(source,crate::sharp_connected::SharpImportKind::GameFolder,true,cx),H::OpenRuntimeSetup(source)=>{self.notice=format!("{} runtime setup must be completed in the native provider UI; no setup was started",format!("{source:?}"));},H::ChooseInstallDetails{..}=>{self.notice="This provider install requires verified title metadata and an explicit destination; no install was started".into();},H::GameJoltBrowser(crate::sharp_connected::GameJoltBrowserIntent::OpenPersistentWebsite)=>{
            #[cfg(target_os="macos")] {if let (Some(mtm),Ok(request))=(objc2::MainThreadMarker::new(),crate::mini_browser::MiniBrowserRequest::new(crate::mini_browser::BrowserPurpose::GameJolt,crate::sharp_connected::GAMEJOLT_BROWSER_URL,"MetalSharp — GameJolt")){if crate::mini_browser::open_native(mtm,request,Box::new(|_|{})).is_err(){self.notice="Could not open GameJolt browser".into();}else{self.notice="GameJolt opened in its named persistent WebKit store".into();}}else{self.notice="GameJolt browser URL was rejected".into();}}
        },H::GameJoltBrowser(_)=>self.notice="Close the GameJolt browser before requesting explicit named-store cleanup; this capability is not available in the current preview target".into()};
        cx.notify();
    }
    fn pick_library_executable(
        &mut self,
        id: crate::connected_library::GameId,
        cx: &mut Context<Self>,
    ) {
        let panel = self.library_panel.clone();
        let state = panel.read(cx).state.clone();
        let executable = crate::connected_library::LibraryAction::ChooseExecutable(id);
        cx.spawn(async move |this, cx| {
            let selected = rfd::AsyncFileDialog::new().pick_file().await;
            let _ = this.update(cx, |this, cx| {
                if let Some(path) = selected {
                    this.dispatch(
                        Operation::Library(
                            executable,
                            state,
                            Some(path.path().to_string_lossy().into_owned()),
                        ),
                        cx,
                    );
                } else {
                    panel.update(cx, |panel, cx| panel.set_busy(false, cx));
                }
            });
        })
        .detach();
    }
    fn pick_sharp_path(
        &mut self,
        source: crate::sharp_connected::SharpSource,
        kind: crate::sharp_connected::SharpImportKind,
        root: bool,
        cx: &mut Context<Self>,
    ) {
        let panel = self.sharp_panel.clone();
        let folder = root || kind == crate::sharp_connected::SharpImportKind::GameFolder;
        cx.spawn(async move |this, cx| {
            let selected = if folder {
                rfd::AsyncFileDialog::new().pick_folder().await
            } else {
                rfd::AsyncFileDialog::new().pick_file().await
            };
            let Some(selected) = selected else { return };
            let path = selected.path().to_string_lossy().into_owned();
            let intent = crate::sharp_connected::SharpIntent::ImportSelected { source, kind, path };
            let _ = this.update(cx, |_, cx| {
                panel.update(cx, |panel, cx| panel.submit_intent(intent, cx))
            });
        })
        .detach();
    }
    fn update_recovery_panel(&mut self, cx: &mut Context<Self>) {
        let snapshot = crate::update_recovery_ui::RecoverySnapshot {
            update: Some(self.recovery_update.clone().unwrap_or_else(|| {
                crate::updater_connected::UpdateSnapshot::new(
                    crate::updater_connected::UpdateVariant::Regular,
                )
            })),
            migration_status: self.recovery_migration.status,
            migration: self.recovery_migration.progress.clone(),
            notice: self
                .recovery_migration
                .error
                .clone()
                .unwrap_or_else(|| self.notice.clone()),
            busy: self.busy,
        };
        self.recovery_panel
            .update(cx, |panel, cx| panel.apply_snapshot(snapshot, cx));
    }
    pub(crate) fn setup_view(&self) -> SetupViewState {
        let migration = self.snapshot.setup.runtime_migration_required;
        SetupViewState {
            ready: self.has_snapshot && self.host.is_some() && !self.busy && !migration,
            install_ready: self.has_snapshot && self.host.is_some() && !migration,
            action_available: !self.busy
                || (self.busy_is_poll && self.pending_setup_action.is_none()),
            completed: self.has_snapshot && self.snapshot.setup.completed,
            runtime_ready: !migration
                && (self.snapshot.dependencies.all_installed
                    || self.snapshot.progress.status == "complete"),
            runtime_installing: matches!(
                self.snapshot.progress.status.as_str(),
                "running" | "installing"
            ),
            runtime_started: self.snapshot.progress.status != "idle"
                && !self.snapshot.progress.status.is_empty(),
            percent: self.snapshot.progress.percent(),
            steam_installed: self.snapshot.steam.installed,
            steam_installing: self.snapshot.steam.installing,
        }
    }
    pub(crate) fn setup_key_help(&mut self, gamesdb: bool, cx: &mut Context<Self>) {
        if gamesdb {
            self.help(
                crate::mini_browser::BrowserPurpose::TheGamesDbHelp,
                "https://api.thegamesdb.net/key.php",
                cx,
            );
        } else {
            self.help(
                crate::mini_browser::BrowserPurpose::SteamApiKeyHelp,
                "https://steamcommunity.com/dev/apikey",
                cx,
            );
        }
    }
    pub(crate) fn setup_inputs(&self) -> [gpui::Entity<SearchInput>; 3] {
        [
            self.device.clone(),
            self.steam_key.clone(),
            self.gamesdb_key.clone(),
        ]
    }
    pub(crate) fn setup_install_runtime(&mut self, cx: &mut Context<Self>) {
        let state = self.setup_view();
        if state.install_ready && !state.runtime_ready && !state.runtime_installing {
            self.dispatch(Operation::InstallRuntime, cx);
        }
    }
    pub(crate) fn setup_install_steam(&mut self, cx: &mut Context<Self>) {
        let state = self.setup_view();
        if state.install_ready
            && state.runtime_ready
            && !state.steam_installing
            && !state.steam_installed
        {
            self.dispatch(Operation::InstallSteam, cx);
        }
    }
    pub(crate) fn setup_finish(&mut self, cx: &mut Context<Self>) {
        let state = self.setup_view();
        if !state.ready || !state.runtime_ready || !state.steam_installed || state.steam_installing
        {
            return;
        }
        let device = self.device.read(cx).content.to_string();
        let steam = self.steam_key.read(cx).content.to_string();
        let gamesdb = self.gamesdb_key.read(cx).content.to_string();
        self.dispatch(Operation::Finish(device, steam, gamesdb), cx);
    }
    fn choose_executable(&mut self, game: Game, cx: &mut Context<Self>) {
        if self.busy || game.has_native_build || !game.installed {
            return;
        }
        self.busy = true;
        cx.notify();
        cx.spawn(async move |this, cx| {
            let file = rfd::AsyncFileDialog::new()
                .add_filter("Windows executable", &["exe"])
                .pick_file()
                .await;
            let _ = this.update(cx, |this, cx| {
                this.busy = false;
                if let Some(file) = file {
                    this.dispatch(
                        Operation::SaveExecutable(game, file.path().to_string_lossy().into_owned()),
                        cx,
                    );
                }
                cx.notify();
            });
        })
        .detach();
    }
    fn restart_backend(&mut self, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        let active = matches!(
            self.snapshot.progress.status.as_str(),
            "running" | "installing"
        ) || self.snapshot.steam.installing
            || self
                .streaming_status
                .as_ref()
                .is_some_and(|status| status.installing)
            || !self.snapshot.running.is_empty();
        if active && self.host.as_mut().is_some_and(BackendHost::is_running) {
            self.notice="Finish the active installation or stop tracked games before restarting the owned backend".into();
            cx.notify();
            return;
        }
        self.busy = true;
        self.notice = "Restarting owned C backend…".into();
        self.streaming_watch
            .failed(self.streaming_watch.generation());
        self.streaming_status = None;
        let previous = self.host.take();
        let config = self.config.clone();
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = cx
                .background_executor()
                .spawn(async move {
                    drop(previous); // graceful owned-child shutdown, never a port/PID search
                    BackendHost::start(&config)
                })
                .await;
            let _ = this.update(cx, |this, cx| {
                this.busy = false;
                match result {
                    Ok(host) => {
                        this.host = Some(host);
                        this.notice.clear();
                        this.dispatch(Operation::Refresh, cx);
                    }
                    Err(_) => {
                        this.notice =
                            "Backend restart failed; check resources and port ownership".into()
                    }
                }
                let status = if this.host.as_mut().is_some_and(BackendHost::is_running) {
                    Ok("Owned backend restarted".into())
                } else {
                    Err("Backend restart failed; check resources and port ownership".into())
                };
                this.settings_panel
                    .update(cx, |panel, cx| panel.host_operation_finished(status, cx));
                cx.notify();
            });
        })
        .detach();
    }
    fn dispatch(&mut self, operation: Operation, cx: &mut Context<Self>) {
        if self.busy {
            let pending = queue_setup_action(
                self.busy,
                self.busy_is_poll,
                self.pending_setup_action,
                &operation,
            );
            if pending != self.pending_setup_action {
                self.pending_setup_action = pending;
                cx.notify();
            }
            return;
        }
        let Some(host) = self.host.as_mut() else {
            self.streaming_status = None;
            self.streaming_watch
                .failed(self.streaming_watch.generation());
            self.notice = "Owned backend unavailable".into();
            cx.notify();
            return;
        };
        if !host.is_running() {
            self.streaming_status = None;
            self.streaming_watch
                .failed(self.streaming_watch.generation());
            self.host = None;
            self.notice =
                "Owned backend exited; restart it explicitly before sending operations".into();
            cx.notify();
            return;
        }
        let client = host.client();
        let poll = matches!(operation, Operation::Poll);
        let setup_bootstrap = poll && !self.has_snapshot;
        let settings_read = matches!(operation, Operation::ReadSettings);
        let recovery_operation = matches!(operation, Operation::Recovery(..));
        let sharp_operation = matches!(operation, Operation::Sharp(_));
        let library_operation = matches!(operation, Operation::Library(..));
        let library_launcher_update = match &operation {
            Operation::Library(
                crate::connected_library::LibraryAction::Launcher(launcher, running),
                _,
                _,
            ) => Some((*launcher, *running)),
            _ => None,
        };
        let process_operation = matches!(
            operation,
            Operation::ReadProcesses(_) | Operation::Process(..)
        );
        let settings_key_update = match &operation {
            Operation::Settings(crate::settings_connected::SettingsIntent::SaveSteamKey(value)) => {
                Some((true, !value.is_empty()))
            }
            Operation::Settings(crate::settings_connected::SettingsIntent::SaveGamesDbKey(
                value,
            )) => Some((false, !value.is_empty())),
            _ => None,
        };
        let settings_operation = matches!(operation, Operation::Settings(_));
        let logs = matches!(operation, Operation::ReadDiagnostics);
        let logs_refresh = matches!(
            operation,
            Operation::Logs(crate::logs_connected::LogsIntent::Refresh { .. }, _)
        );
        let connected_logs = matches!(operation, Operation::Logs(_, _));
        let streaming = matches!(
            operation,
            Operation::ReadStreaming | Operation::Streaming(_)
        );
        let stream_generation = self.streaming_watch.generation();
        let stream_install = matches!(
            operation,
            Operation::Streaming(crate::streaming::StreamingAction::Install)
        );
        if streaming && !stream_install && !matches!(operation, Operation::ReadStreaming) {
            self.streaming_watch.failed(stream_generation);
        }
        let refresh = poll || matches!(operation, Operation::Refresh | Operation::ReadStreaming);
        let login_purpose = match &operation {
            Operation::GogLogin => Some(crate::mini_browser::BrowserPurpose::GogAuth),
            Operation::EpicLogin => Some(crate::mini_browser::BrowserPurpose::EpicAuth),
            _ => None,
        };
        if login_purpose.is_some() && self.oauth_active {
            return;
        }
        match &operation {
            Operation::SaveSteamKey(_) => self.steam_key.update(cx, |input, cx| input.clear(cx)),
            Operation::SaveGamesDbKey(_) => {
                self.gamesdb_key.update(cx, |input, cx| input.clear(cx))
            }
            Operation::EpicCode(_) => self.epic_code.update(cx, |input, cx| input.clear(cx)),
            Operation::Finish(..) => {
                self.steam_key.update(cx, |input, cx| input.clear(cx));
                self.gamesdb_key.update(cx, |input, cx| input.clear(cx));
            }
            _ => {}
        }
        self.busy = true;
        self.busy_is_poll = poll;
        if sharp_operation {
            self.sharp_panel
                .update(cx, |panel, cx| panel.set_busy(true, cx));
        }
        if library_operation {
            self.library_panel
                .update(cx, |panel, cx| panel.set_busy(true, cx));
        }
        if recovery_operation {
            self.update_recovery_panel(cx);
        }
        if process_operation {
            self.process_panel
                .update(cx, |panel, cx| panel.set_busy(true, cx));
        }
        if !refresh {
            self.notice = "Working…".into();
        }
        cx.notify();
        cx.spawn(async move |this,cx| {
            let result = cx.background_executor().spawn(async move {
                let response = operation.run(&client)?;
                let snapshot = if streaming||connected_logs||settings_read||recovery_operation||sharp_operation||process_operation||library_operation {None}else if setup_bootstrap {Some(Snapshot::load_setup(&client))} else {Some(if poll || logs || settings_operation {Snapshot::load_status(&client)}else{Snapshot::load(&client)})};
                let stream_status=if streaming {Some(client.streaming_status())}else{None};
                Ok::<_,BackendError>((response,snapshot,stream_status))
            }).await;
            let _ = this.update(cx, |this,cx| {
                this.busy = false;
                this.busy_is_poll = false;
                if streaming&&!this.streaming_watch.accepts(stream_generation) {cx.notify();return;}
                let mut auto_launch=false;
                match result {
                    Ok((response,snapshot,stream_status)) => {
                        let snapshot_failed = snapshot.as_ref().is_some_and(Result::is_err)||stream_status.as_ref().is_some_and(Result::is_err);
                        match snapshot {
                            Some(Ok(mut snapshot)) => {
                                if poll || logs {snapshot.games=std::mem::take(&mut this.snapshot.games);if !setup_bootstrap{snapshot.dependencies=this.snapshot.dependencies.clone();}}
                                this.has_snapshot=true;
                                let device = snapshot.setup.device_name.clone();
                                this.snapshot = snapshot;
                                let library_games=this.snapshot.games.clone();let steam_running=this.snapshot.steam.running;let ubisoft_running=this.snapshot.ubisoft.running;let running=this.snapshot.running.clone();
                                this.library_panel.update(cx,|panel,cx|{panel.apply_games(library_games,cx);panel.set_launchers(steam_running,ubisoft_running,cx);panel.set_running_appids(&running,cx);});
                                let preferences=this.snapshot.preferences.clone();
                                this.settings_panel.update(cx,|panel,cx|panel.apply_preferences(preferences,&device,cx));
                                if this.device.read(cx).content.is_empty() && !device.is_empty() { this.device.update(cx,|input,cx|{input.content=device.into();cx.notify();}); }
                            }
                            Some(Err(error)) => this.notice = if refresh || poll {error.to_string()} else {"Operation accepted, but refreshed status is unavailable; do not repeat the operation automatically".into()},
                            None=>{},
                        }
                        if stream_install {this.streaming_watch.accepted_install(stream_generation);}
                        if connected_logs {match serde_json::from_value::<crate::logs_connected::LogsSnapshot>(response.clone()){Ok(snapshot)=>this.logs_panel.update(cx,|panel,cx|panel.apply_refresh(Ok(snapshot),cx)),Err(_)=>this.logs_panel.update(cx,|panel,cx|panel.apply_refresh(Err(BackendError::InvalidJson),cx))}}
                        if let Some(status)=stream_status {
                            match status {Ok(status)=>{auto_launch=this.streaming_watch.observe(stream_generation,&status);this.streaming_status=Some(status);},Err(_)=>{this.streaming_status=None;this.notice="Streaming status unavailable; do not repeat an accepted operation automatically".into();}}
                        }
                        if library_operation{match serde_json::from_value::<crate::connected_library::LibraryDispatchResult>(response.clone()){Ok(crate::connected_library::LibraryDispatchResult::Games(games)|crate::connected_library::LibraryDispatchResult::Accepted(games))=>{this.snapshot.games=games.clone();if let Some((launcher,running))=library_launcher_update{match launcher{Launcher::Steam=>this.snapshot.steam.running=running,Launcher::Ubisoft=>this.snapshot.ubisoft.running=running}}let steam=this.snapshot.steam.running;let ubisoft=this.snapshot.ubisoft.running;this.library_panel.update(cx,|panel,cx|{panel.apply_games(games,cx);panel.set_launchers(steam,ubisoft,cx);});},Ok(crate::connected_library::LibraryDispatchResult::ChooseExecutable(id))=>{this.library_panel.update(cx,|panel,cx|panel.set_busy(false,cx));this.pick_library_executable(id,cx);},Ok(crate::connected_library::LibraryDispatchResult::AcceptedWithoutSnapshot)=>{this.library_panel.update(cx,|panel,cx|panel.set_busy(false,cx));this.notice="Library action was accepted but refreshed status is unavailable; do not repeat it automatically".into();},Ok(crate::connected_library::LibraryDispatchResult::UiOnly)=>this.library_panel.update(cx,|panel,cx|panel.set_busy(false,cx)),Err(_)=>{this.library_panel.update(cx,|panel,cx|panel.set_busy(false,cx));this.notice="Library operation response could not be decoded".into();}}}
                        if process_operation{match serde_json::from_value::<crate::process_manager_connected::ProcessSnapshot>(response.clone()){Ok(snapshot)=>this.process_panel.update(cx,|panel,cx|panel.apply_snapshot(snapshot,cx)),Err(_)=>{this.process_panel.update(cx,|panel,cx|panel.set_busy(false,cx));this.notice="Process snapshot could not be decoded".into();}}}
                        if sharp_operation {this.sharp_panel.update(cx,|panel,cx|panel.set_busy(false,cx));match serde_json::from_value::<crate::sharp_connected::SharpIntentResult>(response.clone()){Ok(crate::sharp_connected::SharpIntentResult::Snapshot(snapshot))=>this.sharp_panel.update(cx,|panel,cx|panel.apply_snapshot(snapshot,cx)),Ok(crate::sharp_connected::SharpIntentResult::Operation(_))=>{let source=this.sharp_panel.read(cx).source();this.sharp_panel.update(cx,|panel,cx|panel.submit_intent(crate::sharp_connected::SharpIntent::Refresh(source),cx));},Ok(crate::sharp_connected::SharpIntentResult::HostEffect(effect))=>this.handle_sharp_host_effect(effect,cx),Err(_)=>this.sharp_panel.update(cx,|panel,cx|panel.apply_error("Sharp provider response could not be decoded".into(),cx))}}
                        if recovery_operation {match serde_json::from_value::<(crate::migration_connected::MigrationSession,Option<crate::updater_connected::UpdateSnapshot>,String)>(response.clone()){Ok((migration,update,notice))=>{this.recovery_migration=migration;this.recovery_update=update;if !notice.is_empty(){this.notice=notice;}this.update_recovery_panel(cx);},Err(_)=>this.notice="Recovery response could not be decoded".into()}}
                        if settings_read {match serde_json::from_value::<crate::settings_connected::SettingsSnapshot>(response.clone()){Ok(mut snapshot)=>{snapshot.device_name=this.snapshot.setup.device_name.clone();this.settings_panel.update(cx,|panel,cx|{panel.apply_snapshot(snapshot,cx);panel.host_operation_finished(Ok("Settings loaded from the owned backend".into()),cx);});},Err(_)=>this.settings_panel.update(cx,|panel,cx|panel.host_operation_finished(Err("Settings response could not be decoded".into()),cx))}}
                        if settings_operation {let accepted=response.get("ok").and_then(Value::as_bool)!=Some(false);this.settings_panel.update(cx,|panel,cx|{panel.host_operation_finished(if accepted{Ok("Settings operation completed".into())}else{Err(response.get("error").and_then(Value::as_str).unwrap_or("Settings operation failed").to_owned())},cx);if accepted{if let Some((steam,configured))=settings_key_update{panel.mark_configured_key(steam,configured,cx);}}});}
                        if !refresh && !poll && !snapshot_failed {
                            this.notice = if response.get("sync").and_then(|s|s.get("steam_id_detected")).and_then(Value::as_bool)==Some(false) { "Key saved; sign in to Steam before ownership sync can populate the library".into() } else { "Operation accepted; status reflects the backend, not a simulation".into() };
                        }
                        if let Some(purpose)=login_purpose {
                            if let Some(url) = response.get("authUrl").and_then(Value::as_str) { this.open_auth(purpose,url,cx); } else { this.notice = "Provider status did not provide an authorization URL".into(); }
                        }
                        if logs {
                            match serde_json::from_value(response) {
                                Ok(bundle)=>{this.diagnostics=bundle;if !snapshot_failed {this.notice="Diagnostics refreshed; credential-bearing lines redacted".into();}},
                                Err(_)=>this.notice="Diagnostics response could not be decoded".into(),
                            }
                        }
                    }
                    Err(error) => {if streaming {this.streaming_status=None;if !refresh {this.streaming_watch.failed(stream_generation);}}let message=error.to_string();if library_operation{this.library_panel.update(cx,|panel,cx|panel.apply_error(error.clone(),cx));}if process_operation{this.process_panel.update(cx,|panel,cx|panel.set_busy(false,cx));}if sharp_operation{this.sharp_panel.update(cx,|panel,cx|panel.apply_error(message.clone(),cx));}if settings_operation {this.settings_panel.update(cx,|panel,cx|panel.host_operation_finished(Err(message.clone()),cx));}if logs_refresh {this.logs_panel.update(cx,|panel,cx|panel.apply_refresh(Err(error),cx));}this.notice = message;if recovery_operation{this.update_recovery_panel(cx);}},
                }
                if streaming {this.streaming_watch.defer_poll(std::time::Instant::now(),this.streaming_status.as_ref().is_some_and(|status|status.installing));}
                if auto_launch {this.streaming_command(crate::streaming::StreamingAction::Start,cx);}
                if let Some(pending) = this.pending_setup_action.take() {
                    match pending {
                        PendingSetupAction::InstallRuntime => this.setup_install_runtime(cx),
                        PendingSetupAction::InstallSteam => this.setup_install_steam(cx),
                    }
                }
                cx.notify();
            });
        }).detach();
    }
    fn open_auth(
        &mut self,
        purpose: crate::mini_browser::BrowserPurpose,
        url: &str,
        cx: &mut Context<Self>,
    ) {
        #[cfg(target_os = "macos")]
        {
            use crate::mini_browser::{MiniBrowserRequest, open_native};
            let request = MiniBrowserRequest::new(purpose, url, "MetalSharp account sign-in");
            if let (Ok(request), Some(mtm)) = (request, objc2::MainThreadMarker::new()) {
                let (tx, rx) = std::sync::mpsc::channel();
                match open_native(
                    mtm,
                    request,
                    Box::new(move |result| {
                        let _ = tx.send(result);
                    }),
                ) {
                    Ok(()) => {
                        self.oauth_rx = Some(rx);
                        self.oauth_active = true;
                        self.notice = "Complete sign-in in the native account window".into();
                    }
                    Err(_) => self.notice = "Could not open native account sign-in".into(),
                }
            } else {
                self.notice = "Provider authorization URL rejected".into();
            }
        }
        cx.notify();
    }
    fn help(
        &mut self,
        purpose: crate::mini_browser::BrowserPurpose,
        url: &str,
        cx: &mut Context<Self>,
    ) {
        #[cfg(target_os = "macos")]
        {
            if let (Ok(request), Some(mtm)) = (
                crate::mini_browser::MiniBrowserRequest::new(
                    purpose,
                    url,
                    "MetalSharp account help",
                ),
                objc2::MainThreadMarker::new(),
            ) {
                if crate::mini_browser::open_native(mtm, request, Box::new(|_| {})).is_err() {
                    self.notice = "Could not open help browser".into();
                }
            }
        }
        cx.notify();
    }
    fn field(input: &gpui::Entity<SearchInput>) -> gpui::Div {
        div()
            .h(px(36.))
            .w_full()
            .rounded(px(6.))
            .border_1()
            .border_color(rgb(0x454849))
            .px(px(10.))
            .py(px(7.))
            .child(input.clone())
    }
    fn button(
        id: impl Into<gpui::ElementId>,
        label: impl Into<gpui::SharedString>,
        enabled: bool,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(id)
            .px(px(15.))
            .py(px(9.))
            .rounded(px(7.))
            .bg(rgb(if enabled { 0x2d3436 } else { 0x171b1d }))
            .text_color(rgb(if enabled { 0xf2efe6 } else { 0x777d7b }))
            .cursor_pointer()
            .child(label.into())
    }
}

#[cfg(test)]
mod setup_install_dispatch_tests {
    use super::{Operation, PendingSetupAction, queue_setup_action};

    #[test]
    fn setup_install_actions_queue_behind_status_polls() {
        assert_eq!(
            queue_setup_action(true, true, None, &Operation::InstallRuntime),
            Some(PendingSetupAction::InstallRuntime)
        );
        assert_eq!(
            queue_setup_action(true, true, None, &Operation::InstallSteam),
            Some(PendingSetupAction::InstallSteam)
        );
    }

    #[test]
    fn setup_install_actions_do_not_queue_behind_other_work_or_duplicate() {
        assert_eq!(
            queue_setup_action(true, false, None, &Operation::InstallRuntime),
            None
        );
        assert_eq!(
            queue_setup_action(
                true,
                true,
                Some(PendingSetupAction::InstallSteam),
                &Operation::InstallRuntime
            ),
            Some(PendingSetupAction::InstallSteam)
        );
    }
}
impl Render for ConnectedApp {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let enabled = !self.busy && self.host.is_some();
        let steam_running = self.snapshot.steam.running;
        let ubisoft_running = self.snapshot.ubisoft.running;
        let mut root = div()
            .id("connected-app")
            .size_full()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap(px(18.))
            .p(px(28.))
            .bg(rgb(0x101316))
            .font_family("Rethink Sans")
            .text_color(rgb(0xf2efe6));
        root=root.child(div().text_size(px(22.)).child("MetalSharp — connected GPUI test"))
            .child(div().text_color(rgb(0xc2bda9)).child(if self.validation {"Isolated test data home. Actions use the bundled backend and perform real operations when clicked."} else {"Production data mode explicitly enabled. Operations use the existing C backend and ~/.metalsharp."}))
            .child(div().child(self.notice.clone()))
            .child(div().flex().gap(px(8.)).flex_wrap()
                .child(Self::button("connected-library","Library",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Library);cx.notify();})))
                .child(Self::button("connected-settings","Settings",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Settings);this.dispatch(Operation::ReadSettings,cx);cx.notify();})))
                .child(Self::button("connected-sharp","Sharp Library",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Sharp);let panel=this.sharp_panel.clone();panel.update(cx,|panel,cx|panel.set_source(crate::sharp_connected::SharpSource::Installers,cx));cx.notify();})))
                .child(Self::button("connected-logs","Logs",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Logs);let panel=this.logs_panel.clone();panel.update(cx,|panel,cx|panel.refresh(cx));cx.notify();})))
                .child(Self::button("connected-recovery","Recovery",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Recovery);this.update_recovery_panel(cx);cx.notify();})))
                .child(Self::button("connected-processes","Processes",true).on_click(cx.listener(|this,_,_,cx|{this.panel=Some(ConnectedPanel::Processes);let games=this.snapshot.games.iter().map(|g|(g.appid,g.name.clone())).collect();this.dispatch(Operation::ReadProcesses(games),cx);cx.notify();}))))
            .child(Self::button("refresh","Refresh status",enabled).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::Refresh,cx))))
            .child(Self::button("backend-restart","Restart owned backend",!self.busy).on_click(cx.listener(|this,_,_,cx|this.restart_backend(cx))));
        root=root.child(div().text_size(px(18.)).child(if self.snapshot.setup.completed {"Setup complete — repair / reinstall"}else{"First-run setup"}))
            .child(div().flex().gap(px(12.))
                .child(Self::button("runtime-install","Install runtime + support assets",enabled).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::InstallRuntime,cx))))
                .child(Self::button("steam-install","Install Wine Steam",enabled&&!self.snapshot.steam.installing).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::InstallSteam,cx)))))
            .child(div().child(format!("Runtime: {} · {}% · {}",self.snapshot.progress.status,self.snapshot.progress.percent(),self.snapshot.progress.current)))
            .child(div().child(format!("Steam: {} · {}",if self.snapshot.steam.installed {"installed"}else{"not installed"},self.snapshot.steam.install_stage)))
            .child(Self::field(&self.device)).child(Self::field(&self.steam_key)).child(Self::field(&self.gamesdb_key))
            .child(div().flex().flex_wrap().gap(px(12.))
                .child(Self::button("save-steam-key","Save Steam key + sync ownership",enabled).on_click(cx.listener(|this,_,_,cx|{let key=this.steam_key.read(cx).content.to_string();this.dispatch(Operation::SaveSteamKey(key),cx);})))
                .child(Self::button("save-gamesdb-key","Save TheGamesDB key",enabled).on_click(cx.listener(|this,_,_,cx|{let key=this.gamesdb_key.read(cx).content.to_string();this.dispatch(Operation::SaveGamesDbKey(key),cx);})))
                .child(Self::button("complete-setup","Complete setup",enabled&&self.snapshot.steam.installed&&!self.snapshot.steam.installing).on_click(cx.listener(|this,_,_,cx|{
                    if !this.snapshot.steam.installed||this.snapshot.steam.installing{return;}
                    let device=this.device.read(cx).content.to_string();let steam=this.steam_key.read(cx).content.to_string();let gamesdb=this.gamesdb_key.read(cx).content.to_string();this.dispatch(Operation::Finish(device,steam,gamesdb),cx);
                }))))
            .child(div().flex().gap(px(12.))
                .child(Self::button("steam-help","Steam key help",true).on_click(cx.listener(|this,_,_,cx|this.help(crate::mini_browser::BrowserPurpose::SteamApiKeyHelp,"https://steamcommunity.com/dev/apikey",cx))))
                .child(Self::button("gamesdb-help","TheGamesDB key help",true).on_click(cx.listener(|this,_,_,cx|this.help(crate::mini_browser::BrowserPurpose::TheGamesDbHelp,"https://api.thegamesdb.net/key.php",cx)))))
            .child(div().flex().gap(px(12.))
                .child(Self::button("steam-launch",if steam_running {"Stop Steam"}else{"Start Steam"},enabled).on_click(cx.listener(move|this,_,_,cx|this.dispatch(Operation::Launcher(Launcher::Steam,!steam_running),cx))))
                .child(Self::button("ubisoft-launch",if ubisoft_running {"Stop Ubisoft"}else{"Start Ubisoft"},enabled).on_click(cx.listener(move|this,_,_,cx|this.dispatch(Operation::Launcher(Launcher::Ubisoft,!ubisoft_running),cx)))))
            .child(div().text_size(px(18.)).child("Accounts"))
            .child(Self::button("gog-initialize","Initialize GOG support",enabled).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::GogInitialize,cx))))
            .child(Self::button("epic-install-support","Install Epic support",enabled).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::EpicInstallSupport,cx))))
            .child(Self::button("gog-login","Sign in to GOG (native browser)",enabled&&!self.oauth_active).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::GogLogin,cx))))
            .child(Self::button("epic-login","Sign in to Epic (native browser)",enabled&&!self.oauth_active).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::EpicLogin,cx))))
            .child(div().child("Epic sign-in reads only the approved JSON result endpoint. Manual one-time-code submission remains a fallback."))
            .child(Self::field(&self.epic_code))
            .child(Self::button("epic-code","Submit Epic authorization code",enabled).on_click(cx.listener(|this,_,_,cx|{let code=this.epic_code.read(cx).content.to_string();if !code.trim().is_empty(){this.dispatch(Operation::EpicCode(code),cx);}})))
            ;
        let prefs = &self.snapshot.preferences;
        let controller = match prefs.controller_input {
            ControllerInput::Off => ControllerInput::XInput,
            ControllerInput::XInput => ControllerInput::DInput,
            ControllerInput::DInput => ControllerInput::Off,
        };
        let window = match prefs.window_mode {
            WindowMode::Default => WindowMode::Windowed,
            WindowMode::Windowed => WindowMode::Fullscreen,
            WindowMode::Fullscreen => WindowMode::Default,
        };
        let resolution = match prefs.game_resolution {
            GameResolution::Default => GameResolution::Hd,
            GameResolution::Hd => GameResolution::FullHd,
            GameResolution::FullHd => GameResolution::Qhd,
            GameResolution::Qhd => GameResolution::Uhd,
            GameResolution::Uhd => GameResolution::Default,
        };
        let changes = [
            (
                "config-logs",
                format!("Runtime logs: {}", prefs.graphics_runtime_logs),
                PreferenceChange::GraphicsRuntimeLogs(!prefs.graphics_runtime_logs),
            ),
            (
                "config-msync",
                format!("Msync: {}", prefs.msync),
                PreferenceChange::Msync(!prefs.msync),
            ),
            (
                "config-retina",
                format!("Retina: {}", prefs.retina_mode),
                PreferenceChange::RetinaMode(!prefs.retina_mode),
            ),
            (
                "config-native",
                format!(
                    "Exclude native Mac titles: {}",
                    prefs.exclude_native_mac_steam_games
                ),
                PreferenceChange::ExcludeNativeMacSteamGames(!prefs.exclude_native_mac_steam_games),
            ),
            (
                "config-controller",
                format!(
                    "Controller: {:?} → {:?}",
                    prefs.controller_input, controller
                ),
                PreferenceChange::ControllerInput(controller),
            ),
            (
                "config-window",
                format!("Window: {:?} → {:?}", prefs.window_mode, window),
                PreferenceChange::WindowMode(window),
            ),
            (
                "config-resolution",
                format!("Resolution: {:?} → {:?}", prefs.game_resolution, resolution),
                PreferenceChange::GameResolution(resolution),
            ),
        ];
        let mut settings = div().flex().flex_wrap().gap(px(12.));
        for (id, label, change) in changes {
            settings = settings.child(Self::button(id, label, enabled).on_click(
                cx.listener(move |this, _, _, cx| this.dispatch(Operation::Preference(change), cx)),
            ));
        }
        root=root.child(div().text_size(px(18.)).child("Persisted runtime preferences"))
            .child(div().child("Changes apply to subsequent launches. Restart Steam/titles yourself when required; these controls never silently terminate them."))
            .child(settings)
            .child(Self::button("open-streaming","Game Streaming",enabled).on_click(cx.listener(|this,_,_,cx|this.open_streaming(cx))))
            .child(div().text_size(px(18.)).child(format!("Steam / Ubisoft library — {} games",self.snapshot.games.len())));
        if self.snapshot.games.is_empty() {
            root=root.child(div().child("No library entries returned. Install/sign in to Steam or Ubisoft and refresh; sample titles are never substituted."));
        }
        root = root
            .child(
                div()
                    .text_size(px(18.))
                    .child("Backend diagnostics — on demand"),
            )
            .child(
                Self::button("read-diagnostics", "Refresh diagnostics", enabled).on_click(
                    cx.listener(|this, _, _, cx| this.dispatch(Operation::ReadDiagnostics, cx)),
                ),
            );
        let mut logs = div()
            .id("connected-diagnostics")
            .max_h(px(300.))
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap(px(8.));
        for file in &self.diagnostics.logs {
            logs = logs.child(div().child(file.name.clone()));
            for line in &file.lines {
                logs = logs.child(
                    div()
                        .font_family("Menlo")
                        .text_size(px(12.))
                        .child(line.clone()),
                );
            }
        }
        root = root.child(logs);
        for (index, game) in self.snapshot.games.iter().enumerate() {
            let launch = game.clone();
            let appid = game.appid;
            let pipeline_game = game.clone();
            let executable_game = game.clone();
            let choices = if game.source == "ubisoft" {
                &["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"][..]
            } else {
                &["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9", "fna_arm64"][..]
            };
            let effective = if game.preferred_pipeline.is_empty() {
                &game.launch_method
            } else {
                &game.preferred_pipeline
            };
            let next = choices
                [(choices.iter().position(|id| *id == effective).unwrap_or(0) + 1) % choices.len()]
            .to_owned();
            root = root.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .flex_wrap()
                    .child(div().flex_1().min_w(px(140.)).child(format!(
                        "{} · {}",
                        game.name,
                        if self.snapshot.running.contains(&game.appid) {
                            "Running"
                        } else if game.installed {
                            "Installed"
                        } else {
                            "Not installed"
                        }
                    )))
                    .child(
                        Self::button(
                            ("game-launch", index),
                            if game.installed {
                                "Launch"
                            } else {
                                "Install via Steam"
                            },
                            enabled,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if launch.installed {
                                this.dispatch(Operation::Launch(launch.clone()), cx);
                            } else if launch.source != "ubisoft" {
                                this.dispatch(Operation::InstallGame(launch.appid), cx);
                            }
                        })),
                    )
                    .child(
                        Self::button(
                            ("game-pipeline", index),
                            format!("Pipeline: {} → {}", effective, next),
                            enabled && !game.has_native_build,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.dispatch(
                                Operation::SavePipeline(pipeline_game.clone(), next.clone()),
                                cx,
                            )
                        })),
                    )
                    .child(
                        Self::button(
                            ("game-executable", index),
                            "Choose EXE",
                            enabled && game.installed && !game.has_native_build,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.choose_executable(executable_game.clone(), cx)
                        })),
                    )
                    .child(
                        Self::button(("game-stop", index), "Stop", enabled).on_click(cx.listener(
                            move |this, _, _, cx| this.dispatch(Operation::Stop(appid), cx),
                        )),
                    ),
            );
        }
        if let Some(panel) = self.panel {
            let close = Self::button("connected-panel-close", "Close", true).on_click(cx.listener(
                |this, _, _, cx| {
                    this.panel = None;
                    cx.notify();
                },
            ));
            let content = match panel {
                ConnectedPanel::Settings => div()
                    .size_full()
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(rgba(0x000000b8))
                    .child(self.settings_panel.clone()),
                ConnectedPanel::Sharp => div()
                    .size_full()
                    .flex()
                    .flex_col()
                    .bg(rgb(0x101316))
                    .child(close)
                    .child(self.sharp_panel.clone()),
                ConnectedPanel::Logs => div()
                    .size_full()
                    .flex()
                    .flex_col()
                    .bg(rgb(0x101316))
                    .child(close)
                    .child(self.logs_panel.clone()),
                ConnectedPanel::Recovery => div().size_full().child(self.recovery_panel.clone()),
                ConnectedPanel::Processes => div().size_full().child(self.process_panel.clone()),
                ConnectedPanel::Library => div()
                    .size_full()
                    .flex()
                    .flex_col()
                    .bg(rgb(0x101316))
                    .child(close)
                    .child(self.library_panel.clone()),
            };
            root = root.child(gpui::deferred(content).with_priority(200));
        }
        root
    }
}
