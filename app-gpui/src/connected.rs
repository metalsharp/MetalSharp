//! Opt-in connected application. The default synthetic preview never constructs
//! this view, a backend host, or any credential field.
use crate::{
    backend::{
        BackendClient, BackendError, Game, InstallProgress, Launcher, LauncherStatus, SetupState,
    },
    backend_host::{BackendHost, HostConfig},
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
    games: Vec<Game>,
}
impl Snapshot {
    fn load(client: &BackendClient) -> Result<Self, BackendError> {
        let setup = client.setup_state()?;
        let steam = client.launcher_status(Launcher::Steam)?;
        let ubisoft = client.launcher_status(Launcher::Ubisoft)?;
        let progress = client.install_progress()?;
        let mut games = client.library(Launcher::Steam)?.games;
        let mut ubisoft_games = client.library(Launcher::Ubisoft)?.games;
        for game in &mut ubisoft_games {
            game.source = "ubisoft".into();
        }
        games.extend(ubisoft_games);
        Ok(Self {
            setup,
            steam,
            ubisoft,
            progress,
            games,
        })
    }
}

// Never derive Debug: variants may transiently carry secrets.
enum Operation {
    Refresh,
    InstallRuntime,
    InstallSteam,
    Launcher(Launcher, bool),
    SaveSteamKey(String),
    SaveGamesDbKey(String),
    Finish(String, String, String),
    Launch(Game),
    Stop(u64),
    GogLogin,
    GogInitialize,
    EpicInstallSupport,
    GogCode(String),
    EpicCode(String),
}
impl Operation {
    fn run(self, client: &BackendClient) -> Result<Value, BackendError> {
        match self {
            Self::Refresh => Ok(json!({})),
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
            Self::Stop(appid) => client.stop_game(appid).map(|_| json!({})),
            Self::GogLogin => client.get("/sharp-library/gog/status"),
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

pub struct ConnectedApp {
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
}
impl ConnectedApp {
    pub fn new(config: HostConfig, cx: &mut Context<Self>) -> Self {
        let input = |placeholder: &str, cx: &mut Context<Self>| {
            cx.new(|cx| {
                let mut input = SearchInput::new(cx);
                input.placeholder = placeholder.to_owned().into();
                input
            })
        };
        let view = Self {
            validation: config.validation,
            host: None,
            snapshot: Snapshot::default(),
            busy: true,
            notice: "Starting owned C backend…".into(),
            device: input("Device name", cx),
            steam_key: input("Steam Web API key (optional)", cx),
            gamesdb_key: input("TheGamesDB key (optional)", cx),
            epic_code: input("Epic authorization code", cx),
            oauth_rx: None,
            oauth_active: false,
        };
        cx.spawn(async move |this, cx| {
            let result = cx.background_executor().spawn(async move { BackendHost::start(&config).map_err(|_| "Could not start the owned C backend. Check resources and port ownership.".to_owned()) }).await;
            let _ = this.update(cx, |this, cx| {
                this.busy = false;
                match result {
                    Ok(host) => { this.host = Some(host); this.dispatch(Operation::Refresh,cx); }
                    Err(error) => this.notice = error,
                }
                cx.notify();
            });
            loop {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                if this.update(cx, |this, cx| {
                    if this.busy { return; }
                    if let Some(result) = this.oauth_rx.as_ref().and_then(|rx|rx.try_recv().ok()) {
                        this.oauth_rx = None; this.oauth_active = false;
                        match result {
                            crate::mini_browser::MiniBrowserResult::GogCode(code) => this.dispatch(Operation::GogCode(code),cx),
                            crate::mini_browser::MiniBrowserResult::Cancelled => this.notice = "Sign-in cancelled".into(),
                            _ => this.notice = "Sign-in failed".into(),
                        }
                        cx.notify();
                    }
                    if !this.busy && this.host.is_some() {
                        this.dispatch(Operation::Refresh,cx);
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
    fn dispatch(&mut self, operation: Operation, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        let Some(host) = &self.host else {
            self.notice = "Backend unavailable; restart connected candidate".into();
            cx.notify();
            return;
        };
        let client = host.client();
        let refresh = matches!(operation, Operation::Refresh);
        let gog_login = matches!(operation, Operation::GogLogin);
        if gog_login && self.oauth_active {
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
                let snapshot = Snapshot::load(&client);
                Ok::<_,BackendError>((response,snapshot))
            }).await;
            let _ = this.update(cx, |this,cx| {
                this.busy = false;
                match result {
                    Ok((response,snapshot)) => {
                        let snapshot_failed = snapshot.is_err();
                        match snapshot {
                            Ok(snapshot) => {
                                let device = snapshot.setup.device_name.clone();
                                this.snapshot = snapshot;
                                if this.device.read(cx).content.is_empty() && !device.is_empty() { this.device.update(cx,|input,cx|{input.content=device.into();cx.notify();}); }
                            }
                            Err(error) => this.notice = if refresh {error.to_string()} else {"Operation accepted, but refreshed status is unavailable; do not repeat the operation automatically".into()},
                        }
                        if !refresh && !snapshot_failed {
                            this.notice = if response.get("sync").and_then(|s|s.get("steam_id_detected")).and_then(Value::as_bool)==Some(false) { "Key saved; sign in to Steam before ownership sync can populate the library".into() } else { "Operation accepted; status reflects the backend, not a simulation".into() };
                        }
                        if gog_login {
                            if let Some(url) = response.get("authUrl").and_then(Value::as_str) { this.open_gog(url,cx); } else { this.notice = "GOG status did not provide an authorization URL".into(); }
                        }
                    }
                    Err(error) => this.notice = error.to_string(),
                }
                cx.notify();
            });
        }).detach();
    }
    fn open_gog(&mut self, url: &str, cx: &mut Context<Self>) {
        #[cfg(target_os = "macos")]
        {
            use crate::mini_browser::{BrowserPurpose, MiniBrowserRequest, open_native};
            let request = MiniBrowserRequest::new(BrowserPurpose::GogAuth, url, "Sign in to GOG");
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
                        self.notice = "Complete sign-in in the native GOG window".into();
                    }
                    Err(_) => self.notice = "Could not open native GOG sign-in".into(),
                }
            } else {
                self.notice = "GOG authorization URL rejected".into();
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
            .child(Self::button("refresh","Refresh status",enabled).on_click(cx.listener(|this,_,_,cx|this.dispatch(Operation::Refresh,cx))));
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
            .child(div().child("Epic automatic callback is not ready. Authorize in the provider flow and submit its one-time code manually; no page-script extraction is used."))
            .child(Self::field(&self.epic_code))
            .child(Self::button("epic-code","Submit Epic authorization code",enabled).on_click(cx.listener(|this,_,_,cx|{let code=this.epic_code.read(cx).content.to_string();if !code.trim().is_empty(){this.dispatch(Operation::EpicCode(code),cx);}})))
            .child(div().text_size(px(18.)).child(format!("Steam / Ubisoft library — {} games",self.snapshot.games.len())));
        if self.snapshot.games.is_empty() {
            root=root.child(div().child("No library entries returned. Install/sign in to Steam or Ubisoft and refresh; sample titles are never substituted."));
        }
        for (index, game) in self.snapshot.games.iter().enumerate() {
            let launch = game.clone();
            let appid = game.appid;
            root = root.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .child(div().flex_1().child(format!(
                        "{} · {}",
                        game.name,
                        if game.installed {
                            "Installed"
                        } else {
                            "Not installed"
                        }
                    )))
                    .child(
                        Self::button(("game-launch", index), "Launch", enabled && game.installed)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if launch.installed {
                                    this.dispatch(Operation::Launch(launch.clone()), cx);
                                }
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
