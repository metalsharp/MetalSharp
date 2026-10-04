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
use gpui::{AppContext, Context, Render, Window, div, prelude::*, px, rgb};
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
    Stop(u64),
    GogLogin,
    EpicLogin,
    GogInitialize,
    EpicInstallSupport,
    GogCode(String),
    EpicCode(String),
}
impl Operation {
    fn run(self, client: &BackendClient) -> Result<Value, BackendError> {
        match self {
            Self::Refresh | Self::Poll | Self::ReadStreaming => Ok(json!({})),
            Self::Streaming(action) => client.streaming_action(action),
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
    pub completed: bool,
    pub runtime_ready: bool,
    pub runtime_installing: bool,
    pub runtime_started: bool,
    pub percent: usize,
    pub steam_installed: bool,
    pub steam_installing: bool,
    pub notice: String,
}

pub(crate) enum ConnectedEvent {
    OpenStreaming,
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
    notice: String,
    validation: bool,
    device: gpui::Entity<SearchInput>,
    steam_key: gpui::Entity<SearchInput>,
    gamesdb_key: gpui::Entity<SearchInput>,
    epic_code: gpui::Entity<SearchInput>,
    oauth_rx: Option<std::sync::mpsc::Receiver<crate::mini_browser::MiniBrowserResult>>,
    oauth_active: bool,
    diagnostics: crate::diagnostics::DiagnosticLogs,
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
        let view = Self {
            streaming_status: None,
            streaming_watch: Default::default(),
            streaming_pin: input("PIN", true, cx),
            has_snapshot: false,
            config: config.clone(),
            validation: config.validation,
            host: None,
            snapshot: Snapshot::default(),
            busy: true,
            notice: "Starting owned C backend…".into(),
            device: input("Device name", false, cx),
            steam_key: input("Steam Web API key (optional)", true, cx),
            gamesdb_key: input("TheGamesDB key (optional)", true, cx),
            epic_code: input("Epic authorization code", true, cx),
            oauth_rx: None,
            oauth_active: false,
            diagnostics: Default::default(),
        };
        cx.spawn(async move |this, cx| {
            let result = cx.background_executor().spawn(async move { BackendHost::start(&config).map_err(|_| "Could not start the owned C backend. Check resources and port ownership.".to_owned()) }).await;
            let _ = this.update(cx, |this, cx| {
                this.busy = false;
                match result {
                    Ok(host) => {
                        #[cfg(feature="browser-fixture")]
                        println!("GPUI_OWNED_BACKEND_READY {}",host.pid());
                        this.host = Some(host); this.notice="Owned C backend ready".into();this.dispatch(Operation::Refresh,cx);
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
                    if !this.busy && this.host.is_some() {
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
    pub(crate) fn setup_view(&self) -> SetupViewState {
        let migration = self.snapshot.setup.runtime_migration_required;
        SetupViewState {
            ready: self.has_snapshot && self.host.is_some() && !self.busy && !migration,
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
            notice: if migration {
                "Runtime migration required; native migration acceptance is still pending. No repair is performed automatically.".into()
            } else if let Some(error) = self
                .snapshot
                .progress
                .error
                .as_deref()
                .filter(|error| !error.is_empty())
            {
                format!(
                    "Installer error: {}",
                    crate::diagnostics::redact_line(error)
                )
            } else if !self.snapshot.progress.current.is_empty() {
                format!(
                    "{} — {}",
                    self.notice,
                    crate::diagnostics::redact_line(&self.snapshot.progress.current)
                )
            } else {
                self.notice.clone()
            },
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
        if state.ready && !state.runtime_ready && !state.runtime_installing {
            self.dispatch(Operation::InstallRuntime, cx);
        }
    }
    pub(crate) fn setup_install_steam(&mut self, cx: &mut Context<Self>) {
        let state = self.setup_view();
        if state.ready && state.runtime_ready && !state.steam_installing && !state.steam_installed {
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
    pub(crate) fn setup_retry_backend(&mut self, cx: &mut Context<Self>) {
        self.restart_backend(cx);
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
                        this.notice = "Owned C backend ready".into();
                        this.dispatch(Operation::Refresh, cx);
                    }
                    Err(_) => {
                        this.notice =
                            "Backend restart failed; check resources and port ownership".into()
                    }
                }
                cx.notify();
            });
        })
        .detach();
    }
    fn dispatch(&mut self, operation: Operation, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        let Some(host) = self.host.as_mut() else {
            self.streaming_status = None;
            self.streaming_watch
                .failed(self.streaming_watch.generation());
            self.notice = "Backend unavailable; restart connected candidate".into();
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
        let logs = matches!(operation, Operation::ReadDiagnostics);
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
        if !refresh {
            self.notice = "Working…".into();
        }
        cx.notify();
        cx.spawn(async move |this,cx| {
            let result = cx.background_executor().spawn(async move {
                let response = operation.run(&client)?;
                let snapshot = if streaming {None}else{Some(if poll || logs {Snapshot::load_status(&client)}else{Snapshot::load(&client)})};
                let stream_status=if streaming {Some(client.streaming_status())}else{None};
                Ok::<_,BackendError>((response,snapshot,stream_status))
            }).await;
            let _ = this.update(cx, |this,cx| {
                this.busy = false;
                if streaming&&!this.streaming_watch.accepts(stream_generation) {cx.notify();return;}
                let mut auto_launch=false;
                match result {
                    Ok((response,snapshot,stream_status)) => {
                        let snapshot_failed = snapshot.as_ref().is_some_and(Result::is_err)||stream_status.as_ref().is_some_and(Result::is_err);
                        match snapshot {
                            Some(Ok(mut snapshot)) => {
                                if poll || logs {snapshot.games=std::mem::take(&mut this.snapshot.games);snapshot.dependencies=this.snapshot.dependencies.clone();}
                                this.has_snapshot=true;
                                let device = snapshot.setup.device_name.clone();
                                this.snapshot = snapshot;
                                if this.device.read(cx).content.is_empty() && !device.is_empty() { this.device.update(cx,|input,cx|{input.content=device.into();cx.notify();}); }
                            }
                            Some(Err(error)) => this.notice = if refresh {error.to_string()} else {"Operation accepted, but refreshed status is unavailable; do not repeat the operation automatically".into()},
                            None=>{},
                        }
                        if stream_install {this.streaming_watch.accepted_install(stream_generation);}
                        if let Some(status)=stream_status {
                            match status {Ok(status)=>{auto_launch=this.streaming_watch.observe(stream_generation,&status);this.streaming_status=Some(status);},Err(_)=>{this.streaming_status=None;this.notice="Streaming status unavailable; do not repeat an accepted operation automatically".into();}}
                        }
                        if !refresh && !snapshot_failed {
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
                    Err(error) => {if streaming {this.streaming_status=None;if !refresh {this.streaming_watch.failed(stream_generation);}}this.notice = error.to_string();},
                }
                if streaming {this.streaming_watch.defer_poll(std::time::Instant::now(),this.streaming_status.as_ref().is_some_and(|status|status.installing));}
                if auto_launch {this.streaming_command(crate::streaming::StreamingAction::Start,cx);}
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
        root=root.child(div().text_size(px(22.)).child("MetalSharp — connected candidate"))
            .child(div().text_color(rgb(0xc2bda9)).child(if self.validation {"Isolated data home. Install/start/launch buttons perform real operations when clicked; they are not simulations."} else {"Production data mode explicitly enabled. Operations use the existing C backend and ~/.metalsharp."}))
            .child(div().child(self.notice.clone()))
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
        root
    }
}
