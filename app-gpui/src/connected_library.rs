//! Connected, source-qualified Steam/Ubisoft Play and Collection shell.
//! All IO remains outside this entity: callbacks enqueue typed intents for ConnectedApp.
use crate::{
    backend::{BackendError, Game, Launcher},
    search_input::SearchInput,
};
use gpui::{Context, ObjectFit, Render, Window, div, img, prelude::*, px, rgb};
use serde::{Deserialize, Serialize};
use std::collections::HashSet;

#[derive(Clone, Debug, Deserialize, Serialize, Eq, Hash, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct GameId {
    pub source: LibrarySource,
    pub id: String,
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize, Eq, Hash, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum LibrarySource {
    Steam,
    Ubisoft,
}
/// Validate data-backed artwork against the exact public image origins allowed by LibraryView CSP.
/// Steam paths use numeric appids; Ubisoft URLs are the backend's fixed official-art mapping.
pub fn artwork_url(game: &Game, hero: bool) -> Option<String> {
    if game.source == "ubisoft" {
        let raw = game.ubisoft_artwork_url.as_deref()?;
        let parsed = url::Url::parse(raw).ok()?;
        return (parsed.scheme() == "https"
            && parsed.host_str() == Some("staticctf.ubisoft.com")
            && parsed.username().is_empty()
            && parsed.password().is_none())
        .then(|| raw.to_owned());
    }
    if game.appid == 0 || game.appid > i32::MAX as u64 {
        return None;
    }
    let raw = if hero {
        game.header_url.as_deref()
    } else {
        game.cover_url.as_deref()
    };
    if let Some(raw) = raw {
        let parsed = url::Url::parse(raw).ok()?;
        if parsed.scheme() == "https"
            && matches!(
                parsed.host_str(),
                Some(
                    "steamcdn-a.akamaihd.net"
                        | "cdn.cloudflare.steamstatic.com"
                        | "cdn.akamai.steamstatic.com"
                )
            )
            && parsed.username().is_empty()
            && parsed.password().is_none()
        {
            return Some(raw.to_owned());
        }
        return None;
    }
    Some(format!(
        "https://cdn.cloudflare.steamstatic.com/steam/apps/{}/{}.jpg",
        game.appid,
        if hero { "header" } else { "library_600x900_2x" }
    ))
}
impl GameId {
    pub fn of(game: &Game) -> Self {
        Self {
            source: if game.source == "ubisoft" {
                LibrarySource::Ubisoft
            } else {
                LibrarySource::Steam
            },
            id: if game.source == "ubisoft" {
                game.ubisoft_id
                    .clone()
                    .unwrap_or_else(|| game.appid.to_string())
            } else {
                game.appid.to_string()
            },
        }
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LibraryLoadState {
    Loading,
    Ready,
    Empty,
    Error,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LibraryTab {
    Play,
    Collection,
}
#[derive(Clone, Debug, PartialEq)]
pub enum LibraryAction {
    Refresh,
    Select(GameId),
    Search(String),
    Tab(LibraryTab),
    Theme(usize),
    Launcher(Launcher, bool),
    Launch(GameId),
    Install(GameId),
    Stop(GameId),
    SavePipeline(GameId, String),
    ChooseExecutable(GameId),
}
#[derive(Clone, Debug)]
pub enum LibraryActionResult {
    UiOnly,
    Refreshed(Vec<Game>),
    Completed,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", tag = "kind", content = "value")]
pub enum LibraryDispatchResult {
    UiOnly,
    Games(Vec<Game>),
    Accepted(Vec<Game>),
    AcceptedWithoutSnapshot,
    ChooseExecutable(GameId),
}
impl LibraryAction {
    /// Execute on ConnectedApp's background executor; path picker completion is passed back
    /// as `executable_path`, never invoked by this view or on the GPUI thread.
    pub fn execute(
        &self,
        client: &crate::backend::BackendClient,
        state: &ConnectedLibraryState,
        executable_path: Option<&str>,
    ) -> Result<LibraryActionResult, BackendError> {
        let game_for = |id: &GameId| {
            state
                .games
                .iter()
                .find(|game| GameId::of(game) == *id)
                .ok_or(BackendError::InvalidInput)
        };
        match self {
            Self::Refresh => {
                let mut games = client.library(Launcher::Steam)?.games;
                let mut ubi = client.library(Launcher::Ubisoft)?.games;
                for game in &mut ubi {
                    game.source = "ubisoft".into();
                }
                games.extend(ubi);
                Ok(LibraryActionResult::Refreshed(games))
            }
            Self::Launcher(launcher, running) => {
                client.set_launcher_running(*launcher, *running)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::Launch(id) => {
                client.launch_game(game_for(id)?)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::Install(id) => {
                let game = game_for(id)?;
                if game.source == "ubisoft" {
                    return Err(BackendError::InvalidInput);
                }
                client.install_game(game.appid)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::Stop(id) => {
                let game = game_for(id)?;
                if game.source == "ubisoft" {
                    return Err(BackendError::InvalidInput);
                }
                client.stop_game(game.appid)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::SavePipeline(id, pipeline) => {
                client.save_pipeline(game_for(id)?, pipeline)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::ChooseExecutable(id) => {
                let path = executable_path.ok_or(BackendError::InvalidInput)?;
                client.save_executable(game_for(id)?, path)?;
                Ok(LibraryActionResult::Completed)
            }
            Self::Select(_) | Self::Search(_) | Self::Tab(_) | Self::Theme(_) => {
                Ok(LibraryActionResult::UiOnly)
            }
        }
    }
}
#[derive(Clone, Debug)]
pub struct ConnectedLibraryState {
    pub games: Vec<Game>,
    pub state: LibraryLoadState,
    pub error: Option<BackendError>,
    pub query: String,
    pub selected: Option<GameId>,
    pub running: HashSet<GameId>,
    pub downloading: HashSet<GameId>,
    pub installing: HashSet<GameId>,
    pub steam_running: bool,
    pub ubisoft_running: bool,
    pub tab: LibraryTab,
    pub theme: usize,
    pub busy: HashSet<GameId>,
    pub notice: Option<String>,
}
impl Default for ConnectedLibraryState {
    fn default() -> Self {
        Self {
            games: vec![],
            state: LibraryLoadState::Loading,
            error: None,
            query: String::new(),
            selected: None,
            running: HashSet::new(),
            downloading: HashSet::new(),
            installing: HashSet::new(),
            steam_running: false,
            ubisoft_running: false,
            tab: LibraryTab::Play,
            theme: 0,
            busy: HashSet::new(),
            notice: None,
        }
    }
}
impl ConnectedLibraryState {
    pub fn replace(&mut self, mut games: Vec<Game>) {
        for game in &mut games {
            if game.source == "ubisoft" {
                game.source = "ubisoft".into();
            } else {
                game.source = "steam".into();
            }
        }
        self.games = games;
        self.state = if self.games.is_empty() {
            LibraryLoadState::Empty
        } else {
            LibraryLoadState::Ready
        };
        self.error = None;
        if !self
            .games
            .iter()
            .any(|g| Some(GameId::of(g)) == self.selected)
        {
            self.selected = self.visible_games().first().map(|g| GameId::of(g));
        }
    }
    pub fn fail(&mut self, error: BackendError) {
        self.state = LibraryLoadState::Error;
        self.error = Some(error);
    }
    pub fn visible_games(&self) -> Vec<&Game> {
        let q = self.query.trim().to_lowercase();
        self.games
            .iter()
            .filter(|g| {
                q.is_empty()
                    || g.name.to_lowercase().contains(&q)
                    || g.appid.to_string().contains(&q)
                    || g.source.to_lowercase().contains(&q)
                    || g.ubisoft_id.as_deref().is_some_and(|id| id.contains(&q))
            })
            .collect()
    }
    pub fn selected_game(&self) -> Option<&Game> {
        self.games
            .iter()
            .find(|g| Some(GameId::of(g)) == self.selected)
    }
    pub fn choose(&mut self, id: GameId) {
        if self.games.iter().any(|g| GameId::of(g) == id) {
            self.selected = Some(id);
        }
    }
}

/// Renderable UI component; IO-free. Host calls take_actions(), performs each request on its
/// background executor, then calls apply_* to publish the result.
pub struct ConnectedLibraryView {
    pub state: ConnectedLibraryState,
    actions: Vec<LibraryAction>,
    search: gpui::Entity<SearchInput>,
    busy: bool,
}
impl ConnectedLibraryView {
    pub fn new(cx: &mut Context<Self>, state: ConnectedLibraryState) -> Self {
        let search = cx.new(SearchInput::new);
        cx.observe(&search, |this, input, cx| {
            let value = input.read(cx).content.to_string();
            if this.state.query != value {
                this.state.query = value.clone();
                this.actions.push(LibraryAction::Search(value));
                cx.notify();
            }
        })
        .detach();
        Self {
            state,
            actions: vec![],
            search,
            busy: false,
        }
    }
    pub fn take_actions(&mut self) -> Vec<LibraryAction> {
        std::mem::take(&mut self.actions)
    }
    pub fn take_backend_action(&mut self) -> Option<LibraryAction> {
        let mut selected = None;
        self.actions.retain(|action| {
            let backend = matches!(
                action,
                LibraryAction::Launcher(..)
                    | LibraryAction::Launch(_)
                    | LibraryAction::Install(_)
                    | LibraryAction::Stop(_)
                    | LibraryAction::SavePipeline(..)
                    | LibraryAction::ChooseExecutable(_)
                    | LibraryAction::Refresh
            );
            if !backend {
                return false;
            }
            if selected.is_none() {
                selected = Some(action.clone());
                false
            } else {
                true
            }
        });
        selected
    }
    pub fn submit_action(&mut self, action: LibraryAction, cx: &mut Context<Self>) {
        self.intent(action, cx);
    }
    pub fn set_launchers(&mut self, steam: bool, ubisoft: bool, cx: &mut Context<Self>) {
        self.state.steam_running = steam;
        self.state.ubisoft_running = ubisoft;
        cx.notify();
    }
    pub fn set_running_appids(&mut self, appids: &HashSet<u64>, cx: &mut Context<Self>) {
        self.state.running = self
            .state
            .games
            .iter()
            .filter(|g| g.source != "ubisoft" && appids.contains(&g.appid))
            .map(GameId::of)
            .collect();
        cx.notify();
    }
    pub fn set_busy(&mut self, busy: bool, cx: &mut Context<Self>) {
        self.busy = busy;
        cx.notify();
    }
    pub fn apply_games(&mut self, games: Vec<Game>, cx: &mut Context<Self>) {
        self.busy = false;
        self.state.replace(games);
        cx.notify();
    }
    pub fn apply_error(&mut self, error: BackendError, cx: &mut Context<Self>) {
        self.busy = false;
        self.state.fail(error);
        cx.notify();
    }
    fn intent(&mut self, action: LibraryAction, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        self.actions.push(action);
        cx.notify();
    }
    fn button(
        id: impl Into<gpui::ElementId>,
        label: impl Into<gpui::SharedString>,
        active: bool,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(id)
            .px(px(12.))
            .py(px(8.))
            .rounded(px(7.))
            .border_1()
            .border_color(rgb(if active { 0x74d2c8 } else { 0x515456 }))
            .bg(rgb(if active { 0x263431 } else { 0x171a1e }))
            .text_color(rgb(0xf2efe6))
            .cursor_pointer()
            .child(label.into())
    }
}
impl Render for ConnectedLibraryView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let width = f32::from(window.viewport_size().width);
        let h = f32::from(window.viewport_size().height);
        let games = self.state.visible_games();
        let selected = self.state.selected_game().cloned();
        let themes = [
            "Dark",
            "Light",
            "Skeleton",
            "Forest",
            "Orange Peel",
            "Dragonfruit",
            "Lava",
        ];
        let accents = [
            0xe8d6b7, 0x4db8ff, 0xd6d0c4, 0x6fce88, 0xff9a45, 0xff66aa, 0xff6b52,
        ];
        let accent = accents[self.state.theme % accents.len()];
        let mut header = div()
            .h(px(56.))
            .min_h(px(56.))
            .flex()
            .items_center()
            .gap(px(10.))
            .px(px(16.))
            .bg(rgb(0x191c1f))
            .border_b_1()
            .border_color(rgb(0x34383a));
        header = header.child(
            div()
                .font_weight(gpui::FontWeight::SEMIBOLD)
                .child("MetalSharp"),
        );
        for (idx, (label, launcher, running)) in [
            ("Steam", Launcher::Steam, self.state.steam_running),
            ("Ubisoft", Launcher::Ubisoft, self.state.ubisoft_running),
        ]
        .into_iter()
        .enumerate()
        {
            header = header.child(
                Self::button(
                    ("launcher", idx),
                    if running {
                        format!("Stop {label}")
                    } else {
                        format!("Start {label}")
                    },
                    running,
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.intent(LibraryAction::Launcher(launcher, !running), cx)
                })),
            );
        }
        let search = self.search.clone();
        header = header.child(
            div()
                .flex_1()
                .min_w_0()
                .max_w(px(600.))
                .mx(px(16.))
                .h(px(38.))
                .flex()
                .items_center()
                .gap(px(8.))
                .px(px(12.))
                .rounded(px(8.))
                .border_1()
                .border_color(rgb(0x515456))
                .bg(rgb(0x080a0d))
                .child("⌕")
                .child(search),
        );
        let theme_label = themes[self.state.theme % themes.len()];
        header =
            header
                .child(
                    Self::button("library-theme", theme_label, false).on_click(cx.listener(
                        |this, _, _, cx| {
                            this.state.theme = (this.state.theme + 1) % 7;
                            this.intent(LibraryAction::Theme(this.state.theme), cx);
                        },
                    )),
                )
                .child(Self::button("library-refresh", "Refresh", false).on_click(
                    cx.listener(|this, _, _, cx| this.intent(LibraryAction::Refresh, cx)),
                ));
        let tabs = div()
            .flex()
            .gap(px(6.))
            .child(
                Self::button("tab-play", "Play", self.state.tab == LibraryTab::Play).on_click(
                    cx.listener(|this, _, _, cx| {
                        this.state.tab = LibraryTab::Play;
                        this.intent(LibraryAction::Tab(LibraryTab::Play), cx)
                    }),
                ),
            )
            .child(
                Self::button(
                    "tab-collection",
                    "Collection",
                    self.state.tab == LibraryTab::Collection,
                )
                .on_click(cx.listener(|this, _, _, cx| {
                    this.state.tab = LibraryTab::Collection;
                    this.intent(LibraryAction::Tab(LibraryTab::Collection), cx)
                })),
            );
        let mut body = div()
            .id("connected-library-body")
            .flex_1()
            .min_h_0()
            .w_full()
            .relative()
            .overflow_hidden()
            .bg(rgb(0x111416));
        match self.state.state {
            LibraryLoadState::Loading => body=body.child(div().absolute().top(px(32.)).left(px(24.)).text_color(rgb(0xc2bda9)).child("Loading your Steam and Ubisoft libraries…")),
            LibraryLoadState::Error => body=body.child(div().absolute().top(px(32.)).left(px(24.)).text_color(rgb(0xff9b91)).child(format!("Library unavailable: {}",self.state.error.as_ref().map(ToString::to_string).unwrap_or_else(||"unknown error".into())))),
            LibraryLoadState::Empty => body=body.child(div().absolute().top(px(32.)).left(px(24.)).text_color(rgb(0xc2bda9)).child("No owned games returned. Sign in, then refresh; preview titles are never substituted.")),
            LibraryLoadState::Ready => {
                if games.is_empty() { body=body.child(div().absolute().top(px(32.)).left(px(24.)).child("No games match your search.")); }
                if self.state.tab == LibraryTab::Play {
                    let hero_h=if h<600. {(h*0.36).clamp(160.,220.)} else {(h*0.54).clamp(300.,480.)};
                    let dock_h=if h<600. {130.} else {205.};
                    let card_h=if h<600. {108.} else {174.};
                    let hero = selected.as_ref().map(|g| {
                        let id=GameId::of(g); let title=g.name.clone(); let installed=g.installed; let art=artwork_url(g,true);
                        let is_running=self.state.running.contains(&id); let is_downloading=self.state.downloading.contains(&id);
                        let is_installing=self.state.installing.contains(&id); let label=if is_running {"Running"} else if is_installing {"Installing"} else if is_downloading {"Downloading"} else if installed {"Installed"} else {"Not installed"};
                        let identity=if id.source==LibrarySource::Steam {"STEAM"} else {"UBISOFT"};
                        let play_id=id.clone(); let stop_id=id.clone();
                        div().id("connected-library-hero").relative().w_full().h(px(hero_h)).min_h(px(hero_h)).flex_none().flex().items_end().overflow_hidden().bg(rgb(0x242629)).children(art.into_iter().map(|url|img(url).absolute().inset_0().w_full().h_full().object_fit(ObjectFit::Cover).opacity(0.62)))
                            .child(div().absolute().inset_0().bg(gpui::linear_gradient(0.,gpui::linear_color_stop(rgb(0x080a0d),0.),gpui::linear_color_stop(rgb(0x33383a),1.))))
                            .child(div().relative().flex().flex_col().gap(px(12.)).p(px(28.)).child(div().text_size(px(30.)).font_weight(gpui::FontWeight::BOLD).child(title))
                                .child(div().text_color(rgb(0xc5ccca)).child(format!("{identity} · {label}")))
                                .child(div().flex().gap(px(9.)).child(Self::button("hero-play",if is_running {"Running"} else if installed {"Play"} else {"Install"},true).on_click(cx.listener(move|this,_,_,cx|this.intent(if installed {LibraryAction::Launch(play_id.clone())} else {LibraryAction::Install(play_id.clone())},cx))))
                                .child(Self::button("hero-stop","Stop",false).on_click(cx.listener(move|this,_,_,cx|this.intent(LibraryAction::Stop(stop_id.clone()),cx))))))
                    });
                    let selected_index=games.iter().position(|game|Some(GameId::of(game))==self.state.selected).unwrap_or(0);
                    let dock_start=selected_index.saturating_sub(2).min(games.len().saturating_sub(5));
                    body=body.child(div().id("play-page").size_full().flex().flex_col().children(hero).child(div().mt_auto().h(px(dock_h)).min_h(px(dock_h)).flex_none().flex().items_center().justify_center().gap(px(12.)).px(px(18.)).overflow_hidden().children(games.iter().skip(dock_start).take(5).enumerate().map(|(i,g)| {
                        let id=GameId::of(g); let chosen=Some(&id)==self.state.selected.as_ref(); let title=g.name.clone(); let source=if id.source==LibrarySource::Steam{"STEAM"}else{"UBISOFT"}; let art=artwork_url(g,false); let lift=(i.abs_diff(2) as f32*7.)-if chosen{16.}else{0.};
                        div().id(("library-dock-card",i)).mt(px(lift.max(0.))).w(px(((width-252.)/5.).clamp(60.,176.))).h(px(card_h)).min_h(px(card_h)).max_h(px(card_h)).flex_none().rounded(px(8.)).border_1().border_color(rgb(if chosen{accent}else{0x515456})).bg(rgb(if chosen{0x30383a}else{0x202326})).shadow_lg().relative().overflow_hidden().children(art.into_iter().map(|url|img(url).absolute().inset_0().w_full().h_full().object_fit(ObjectFit::Cover).opacity(0.7))).flex().flex_col().justify_between().p(px(12.)).cursor_pointer().on_click(cx.listener(move|this,_,_,cx|this.intent(LibraryAction::Select(id.clone()),cx))).child(div().text_size(px(10.)).text_color(rgb(accent)).child(source)).child(div().text_size(px(14.)).font_weight(gpui::FontWeight::SEMIBOLD).child(title)).child(div().text_size(px(11.)).text_color(rgb(0xb9c1c5)).child(if g.installed{"Installed"}else{"Install"}))
                    }))));
                } else {
                    let available=(width-64.).min(1400.); let cols=(((available+18.)/208.).floor().max(1.)) as usize; let cw=((available-(cols-1) as f32*18.)/cols as f32).min(220.);
                    let cards=games.iter().enumerate().map(|(i,g)| { let id=GameId::of(g); let chosen=Some(&id)==self.state.selected.as_ref(); let title=g.name.clone(); let state=if self.state.running.contains(&id){"Running"}else if self.state.installing.contains(&id){"Installing"}else if self.state.downloading.contains(&id){"Downloading"}else if g.installed{"Installed"}else{"Not installed"}; div().id(("collection-card",i)).w(px(cw)).h(px(250.)).rounded(px(8.)).border_1().border_color(rgb(if chosen{accent}else{0x363a3d})).bg(rgb(0x202326)).p(px(12.)).flex().flex_col().justify_between().cursor_pointer().on_click(cx.listener(move|this,_,_,cx|this.intent(LibraryAction::Select(id.clone()),cx))).child(div().text_color(rgb(accent)).text_size(px(10.)).child(if g.source=="ubisoft"{"UBISOFT"}else{"STEAM"})).child(div().text_size(px(15.)).child(title)).child(div().text_color(rgb(0xb9c1c5)).child(state)) });
                    body=body.child(div().id("collection-grid").size_full().overflow_y_scroll().flex().flex_wrap().content_start().gap(px(18.)).p(px(24.)).children(cards));
                }
            }
        }
        let selected_controls = selected.map(|g| {
            let id = GameId::of(&g);
            let executable_id = id.clone();
            let choices: &[&str] = if g.source == "ubisoft" {
                &["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"]
            } else {
                &["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9", "fna_arm64"]
            };
            let cur = if g.preferred_pipeline.is_empty() {
                g.launch_method.as_str()
            } else {
                g.preferred_pipeline.as_str()
            };
            let next = choices
                [(choices.iter().position(|x| *x == cur).unwrap_or(0) + 1) % choices.len()]
            .to_string();
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(
                    Self::button(
                        "pipeline-control",
                        format!("Pipeline · {} → {next}", cur),
                        false,
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.intent(LibraryAction::SavePipeline(id.clone(), next.clone()), cx)
                    })),
                )
                .child(
                    Self::button(
                        "executable-control",
                        if g.executable_path.is_some() {
                            "Choose executable · saved"
                        } else {
                            "Choose executable"
                        },
                        false,
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.intent(LibraryAction::ChooseExecutable(executable_id.clone()), cx)
                    })),
                )
        });
        let footer = div()
            .h(px(58.))
            .min_h(px(58.))
            .flex()
            .items_center()
            .justify_between()
            .px(px(24.))
            .border_t_1()
            .border_color(rgb(0x34383a))
            .bg(rgb(0x1b1e20))
            .text_color(rgb(0xd8dad9))
            .child(format!(
                "{} games · connected library",
                self.state.games.len()
            ))
            .child(self.state.notice.clone().unwrap_or_else(|| "Ready".into()));
        div()
            .id("connected-library")
            .size_full()
            .flex()
            .flex_col()
            .overflow_hidden()
            .font_family("Rethink Sans")
            .bg(rgb(0x111416))
            .text_color(rgb(0xf2efe6))
            .child(header)
            .child(tabs.px(px(18.)).py(px(8.)).bg(rgb(0x191c1f)))
            .child(body)
            .children(selected_controls)
            .child(footer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn game(appid: u64, source: &str, uid: Option<&str>, name: &str) -> Game {
        Game {
            appid,
            name: name.into(),
            installed: false,
            has_native_build: false,
            launch_method: "auto".into(),
            preferred_pipeline: String::new(),
            ubisoft_id: uid.map(str::to_owned),
            bottle_id: None,
            wine_game_path: None,
            executable_path: None,
            game_dir: None,
            source: source.into(),
            cover_url: None,
            header_url: None,
            ubisoft_artwork_url: None,
            embedded_icon_path: None,
        }
    }
    #[test]
    fn ubisoft_stop_rejects_appid_only_contract_before_backend_request() {
        let game = game(7, "ubisoft", Some("u-007"), "Ubisoft Game");
        let id = GameId::of(&game);
        let mut state = ConnectedLibraryState::default();
        state.replace(vec![game]);
        let client = crate::backend::BackendClient::for_port(1).unwrap();
        assert_eq!(
            LibraryAction::Stop(id)
                .execute(&client, &state, None)
                .unwrap_err(),
            BackendError::InvalidInput
        );
    }
    #[test]
    fn dispatch_result_roundtrips_source_qualified_game_identity() {
        let result = LibraryDispatchResult::ChooseExecutable(GameId {
            source: LibrarySource::Ubisoft,
            id: "u-007".into(),
        });
        let json = serde_json::to_vec(&result).unwrap();
        let decoded: LibraryDispatchResult = serde_json::from_slice(&json).unwrap();
        assert!(
            matches!(decoded,LibraryDispatchResult::ChooseExecutable(GameId{source:LibrarySource::Ubisoft,id}) if id=="u-007")
        );
    }
    #[test]
    fn identity_keeps_cross_launcher_collisions_distinct_and_stable() {
        let a = GameId::of(&game(7, "steam", None, "S"));
        let b = GameId::of(&game(7, "ubisoft", Some("007"), "U"));
        assert_ne!(a, b);
        assert_eq!(b.id, "007");
    }
    #[test]
    fn filter_selection_and_refresh_preserve_source_qualified_selection() {
        let steam = game(7, "steam", None, "Control");
        let ubi = game(7, "ubisoft", Some("007"), "Control Ultimate");
        let mut s = ConnectedLibraryState::default();
        s.replace(vec![steam.clone(), ubi.clone()]);
        let uid = GameId::of(&ubi);
        s.choose(uid.clone());
        s.query = "007".into();
        assert_eq!(s.visible_games().len(), 1);
        assert_eq!(s.selected, Some(uid.clone()));
        s.query.clear();
        s.replace(vec![steam, ubi]);
        assert_eq!(s.selected, Some(uid));
    }
    #[test]
    fn artwork_rejects_unapproved_hosts_and_uses_renderer_cdn_contracts() {
        let mut steam = game(42, "steam", None, "Steam");
        assert_eq!(
            artwork_url(&steam, false).as_deref(),
            Some("https://cdn.cloudflare.steamstatic.com/steam/apps/42/library_600x900_2x.jpg")
        );
        steam.header_url = Some("https://attacker.invalid/image.jpg".into());
        assert_eq!(artwork_url(&steam, true), None);
        steam.header_url =
            Some("https://cdn.cloudflare.steamstatic.com/steam/apps/42/header.jpg".into());
        assert!(artwork_url(&steam, true).unwrap().ends_with("header.jpg"));
        let mut ubi = game(99, "ubisoft", Some("0042"), "Ubisoft");
        ubi.ubisoft_artwork_url = Some("https://staticctf.ubisoft.com/art.jpg".into());
        assert!(artwork_url(&ubi, false).is_some());
        ubi.ubisoft_artwork_url = Some("https://example.com/art.jpg".into());
        assert_eq!(artwork_url(&ubi, true), None);
    }
    #[test]
    fn state_actions_and_pipeline_choices_cover_distinct_sources() {
        let a = GameId::of(&game(7, "steam", None, "S"));
        let b = GameId::of(&game(7, "ubisoft", Some("7"), "U"));
        let steam = ["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9", "fna_arm64"];
        let ubi = ["d3dmetal", "dxmt", "dxmt_32", "vkd3d", "d3d9"];
        assert!(steam.contains(&"fna_arm64"));
        assert!(!ubi.contains(&"fna_arm64"));
        let mut s = ConnectedLibraryState::default();
        s.replace(vec![]);
        assert_eq!(s.state, LibraryLoadState::Empty);
        s.fail(BackendError::Transport);
        assert_eq!(s.state, LibraryLoadState::Error);
        s.running.insert(a.clone());
        s.downloading.insert(b.clone());
        s.installing.insert(b.clone());
        assert!(s.running.contains(&a) && s.downloading.contains(&b) && s.installing.contains(&b));
    }
    #[gpui::test]
    fn component_renders_virtual_loading_empty_error_and_ready(cx: &mut gpui::TestAppContext) {
        let window =
            cx.add_window(|_, cx| ConnectedLibraryView::new(cx, ConnectedLibraryState::default()));
        window.update(cx, |_, _, _| {}).unwrap();
        window
            .update(cx, |v, _, cx| {
                v.state.replace(vec![game(4, "steam", None, "Fixture")]);
                cx.notify();
            })
            .unwrap();
        window.update(cx, |_, _, _| {}).unwrap();
        window
            .update(cx, |v, _, cx| {
                v.state.fail(BackendError::Transport);
                cx.notify();
            })
            .unwrap();
        window.update(cx, |_, _, _| {}).unwrap();
    }
}
