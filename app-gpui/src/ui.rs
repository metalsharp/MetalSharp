use gpui::{
    ClickEvent, Context, Focusable, FontWeight, ObjectFit, Render, Window, div, img,
    linear_color_stop, linear_gradient, prelude::*, px, relative, rgb, rgba,
};
use serde::Deserialize;
use std::collections::HashMap;
use std::path::PathBuf;

const PAGE_BG: u32 = 0x080a0d;
const PANEL_BG: u32 = 0x101316;
const TEXT: u32 = 0xf2efe6;
const MUTED: u32 = 0x9aa09e;
const DOCK_VISIBLE_CARDS: usize = 5;
// Bundled sample artwork is visual-only, not a claim of game/runtime compatibility.
const LIBRARY_GAMES: [(&str, &str, &str); 20] = [
    ("Cyberpunk 2077", "cyberpunk.jpg", "cyberpunk-hero.jpg"),
    ("Hades", "hades.jpg", "hades-hero.jpg"),
    ("ELDEN RING", "elden-ring.jpg", "elden-ring-hero.jpg"),
    ("Stray", "stray.jpg", "stray-hero.jpg"),
    ("Balatro", "balatro.jpg", "balatro-hero.jpg"),
    ("Portal 2", "portal-2.jpg", "portal-2-hero.jpg"),
    (
        "Hollow Knight",
        "hollow-knight.jpg",
        "hollow-knight-hero.jpg",
    ),
    (
        "Stardew Valley",
        "stardew-valley.jpg",
        "stardew-valley-hero.jpg",
    ),
    ("The Witcher 3", "witcher-3.jpg", "witcher-3-hero.jpg"),
    ("No Man’s Sky", "no-mans-sky.jpg", "no-mans-sky-hero.jpg"),
    (
        "Baldur’s Gate 3",
        "baldurs-gate-3.jpg",
        "baldurs-gate-3-hero.jpg",
    ),
    (
        "Red Dead Redemption 2",
        "red-dead-redemption-2.jpg",
        "red-dead-redemption-2-hero.jpg",
    ),
    ("Skyrim Special Edition", "skyrim.jpg", "skyrim-hero.jpg"),
    ("Dead Cells", "dead-cells.jpg", "dead-cells-hero.jpg"),
    ("Celeste", "celeste.jpg", "celeste-hero.jpg"),
    ("Cuphead", "cuphead.jpg", "cuphead-hero.jpg"),
    ("Ori and the Blind Forest", "ori.jpg", "ori-hero.jpg"),
    ("DOOM Eternal", "doom-eternal.jpg", "doom-eternal-hero.jpg"),
    (
        "Disco Elysium",
        "disco-elysium.jpg",
        "disco-elysium-hero.jpg",
    ),
    ("Terraria", "terraria.jpg", "terraria-hero.jpg"),
];
// Budget includes dock padding, arrow controls, card gaps, and room for the tilted edges.
fn dock_card_width(window_width: f32) -> f32 {
    ((window_width - 252.0) / DOCK_VISIBLE_CARDS as f32).clamp(60.0, 176.0)
}

fn collection_card_layout(window_width: f32) -> (usize, f32) {
    let available = (window_width - 64.0).min(1400.0);
    let columns = ((available + 18.0) / 208.0).floor().max(1.0) as usize;
    let card_width = ((available - (columns - 1) as f32 * 18.0) / columns as f32).min(220.0);
    (columns, card_width)
}

fn library_search_placeholder(window_width: f32) -> &'static str {
    if window_width <= 800.0 {
        "Search"
    } else {
        "Search games, genres, or tags..."
    }
}

fn dock_card_lift(index: usize, selected: usize) -> f32 {
    let center = DOCK_VISIBLE_CARDS / 2;
    let arch = index.abs_diff(center) as f32 * 7.0;
    arch - if index == selected { 16.0 } else { 0.0 }
}

fn dock_card_angle(index: usize) -> i32 {
    let center = DOCK_VISIBLE_CARDS as i32 / 2;
    ((index as i32 - center) * 3).clamp(-12, 12)
}

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

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SetupCopy {
    language_label: String,
    step_of: String,
    steps: Vec<String>,
    titles: Vec<String>,
    taglines: Vec<String>,
    lede: String,
    runtime_lede: String,
    bundled_tools: String,
    tool_extraction: String,
    tool_rar: String,
    tool_icons: String,
    tool_archives: String,
    install_runtime: String,
    install_complete: String,
    install_log: String,
    preparing: String,
    installing_steam: String,
    install_steam: String,
    steam_installed: String,
    start_steam_hint: String,
    device_name: String,
    device_placeholder: String,
    device_hint: String,
    api_key: String,
    api_placeholder: String,
    api_hint: String,
    the_games_db_api_key: String,
    the_games_db_api_placeholder: String,
    the_games_db_api_hint: String,
    launch: String,
    back: String,
    next: String,
    directx: String,
    directx_desc: String,
    fna: String,
    fna_desc: String,
    steam: String,
    steam_desc: String,
    get_started: String,
}

fn asset_path(name: &str) -> PathBuf {
    let bundled = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(PathBuf::from))
        .map(|exe_dir| exe_dir.join("../Resources/assets").join(name));
    if let Some(path) = bundled.filter(|path| path.is_file()) {
        return path;
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("assets")
        .join(name)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PreviewTheme {
    Dark,
    Light,
    Skeleton,
    Forest,
    OrangePeel,
    Dragonfruit,
    Lava,
}

impl PreviewTheme {
    const ALL: [Self; 7] = [
        Self::Dark,
        Self::Light,
        Self::Skeleton,
        Self::Forest,
        Self::OrangePeel,
        Self::Dragonfruit,
        Self::Lava,
    ];

    fn label(self) -> &'static str {
        match self {
            Self::Dark => "Dark",
            Self::Light => "Light",
            Self::Skeleton => "Skeleton",
            Self::Forest => "Forest",
            Self::OrangePeel => "Orange Peel",
            Self::Dragonfruit => "Dragonfruit",
            Self::Lava => "Lava",
        }
    }

    fn render_icon(self, color: u32) -> gpui::AnyElement {
        let asset = match self {
            Self::Skeleton => Some("theme-skeleton.svg"),
            Self::Forest => Some("theme-forest.svg"),
            Self::OrangePeel => Some("theme-orange.svg"),
            _ => None,
        };
        if let Some(asset) = asset {
            gpui::svg()
                .path(asset)
                .size(px(17.0))
                .text_color(rgb(color))
                .into_any_element()
        } else {
            div()
                .text_color(rgb(color))
                .child(match self {
                    Self::Dark => "☾",
                    Self::Light => "☼",
                    Self::Dragonfruit => "✦",
                    Self::Lava => "♨",
                    _ => unreachable!(),
                })
                .into_any_element()
        }
    }

    fn button_border(self) -> u32 {
        if self == Self::Light {
            0xffffff
        } else {
            self.accent()
        }
    }

    fn menu_bg(self) -> u32 {
        if self == Self::Light {
            0xffffff
        } else {
            self.control_bg()
        }
    }

    fn menu_hover(self) -> u32 {
        if self == Self::Light {
            0xececec
        } else {
            self.control_hover()
        }
    }

    fn accent(self) -> u32 {
        match self {
            Self::Dark => 0xe8d6b7,
            Self::Light => 0x4db8ff,
            Self::Skeleton => 0xd6d0c4,
            Self::Forest => 0x6fce88,
            Self::OrangePeel => 0xff9a45,
            Self::Dragonfruit => 0xff66aa,
            Self::Lava => 0xff6b52,
        }
    }

    fn dock_glow(self) -> u32 {
        match self {
            Self::Dark => 0x2aa1ff,
            Self::Light => 0xfff3dc,
            Self::Skeleton => 0xd6d0c4,
            Self::Forest => 0x6fce88,
            Self::OrangePeel => 0xff7a1a,
            Self::Dragonfruit => 0xff2e88,
            Self::Lava => 0xff4a3d,
        }
    }

    fn control_bg(self) -> u32 {
        match self {
            Self::Dark => 0x080a0d,
            Self::Light => 0xfdfbf7,
            Self::Skeleton => 0x242424,
            Self::Forest => 0x182219,
            Self::OrangePeel => 0x231610,
            Self::Dragonfruit => 0x2c182a,
            Self::Lava => 0x2b0d12,
        }
    }

    fn control_hover(self) -> u32 {
        match self {
            Self::Dark => 0x171a1e,
            Self::Light => 0xffffff,
            Self::Skeleton => 0x303030,
            Self::Forest => 0x1e2b20,
            Self::OrangePeel => 0x2b1b12,
            Self::Dragonfruit => 0x361e33,
            Self::Lava => 0x3b1117,
        }
    }

    fn control_text(self) -> u32 {
        match self {
            Self::Light => 0x000000,
            Self::Skeleton => 0xeeeeee,
            Self::Forest => 0xdce8dc,
            Self::OrangePeel => 0xf2e4d8,
            Self::Dragonfruit => 0xf8e4f0,
            Self::Lava => 0xfff2ee,
            Self::Dark => 0xffffff,
        }
    }

    fn border(self) -> u32 {
        match self {
            Self::Dark => 0x515456,
            Self::Light => 0xffffff,
            Self::Skeleton => 0x555555,
            Self::Forest => 0x45634b,
            Self::OrangePeel => 0x65452f,
            Self::Dragonfruit => 0x70405e,
            Self::Lava => 0x713a34,
        }
    }

    fn page_palette(self) -> crate::page_palette::PagePalette {
        crate::page_palette::PagePalette {
            accent: self.accent(),
            border: self.border(),
            control_border: match self {
                Self::Dark => 0xffffff38,
                Self::Light => 0x1e27323d,
                Self::Skeleton => 0xeeeeee3d,
                Self::Forest => 0x8cbe963d,
                Self::OrangePeel => 0xffaa783d,
                Self::Dragonfruit => 0xffaad23d,
                Self::Lava => 0xff6e5a4d,
            },
            control_bg: self.control_bg(),
            control_text: self.control_text(),
            hover: self.control_hover(),
            menu_bg: self.menu_bg(),
            menu_hover: self.menu_hover(),
            light: self == Self::Light,
        }
    }

    fn hero_title(self) -> u32 {
        if self == Self::Skeleton {
            0xeeeeee
        } else {
            0xefcf9d
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum LibraryTab {
    Play,
    Collection,
    SharpLibrary,
    Logs,
}

const PREVIEW_PIPELINES: [&str; 6] = ["D3DMetal", "VKD3D", "D3D9", "DXMT", "DXMT(32)", "Mono/FNA"];

struct PreviewGameSettings {
    pipeline: usize,
    metal_fx: usize,
    controller: usize,
    msync: bool,
    executable: Option<String>,
}

impl Default for PreviewGameSettings {
    fn default() -> Self {
        Self {
            pipeline: 0,
            metal_fx: 1,
            controller: 0,
            msync: true,
            executable: None,
        }
    }
}

pub struct MetalSharpApp {
    connected: Option<gpui::Entity<crate::connected::ConnectedApp>>,
    connected_subscription: Option<gpui::Subscription>,
    connected_events: Option<gpui::Subscription>,
    streaming_unpair_confirm: bool,
    streaming_unpair_deadline: Option<std::time::Instant>,
    show_setup: bool,
    theme: PreviewTheme,
    theme_menu_open: bool,
    tab_menu_open: bool,
    launcher_menu_open: bool,
    steam_options_open: bool,
    settings_open: bool,
    settings_preview: Option<gpui::Entity<crate::settings_preview::SettingsPreview>>,
    streaming_open: bool,
    streaming_installed: bool,
    streaming_running: bool,
    steam_running: bool,
    sharp_preview: Option<gpui::Entity<crate::sharp_preview::SharpPreview>>,
    logs_preview: Option<gpui::Entity<crate::logs_preview::LogsPreview>>,
    search_input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    search_query: String,
    selected_game: usize,
    dock_start: usize,
    running_game: Option<usize>,
    pipeline_menu_open: bool,
    game_settings_open: bool,
    game_preferences: Vec<PreviewGameSettings>,
    hovered_card: Option<usize>,
    active_tab: LibraryTab,
    step: usize,
    selected_language: &'static str,
    language_menu_open: bool,
    locales: HashMap<String, SetupCopy>,
    copy: SetupCopy,
    runtime_installing: bool,
    runtime_started: bool,
    runtime_installed: bool,
    runtime_progress: usize,
    install_log_open: bool,
    steam_installing: bool,
    steam_installed: bool,
}

impl MetalSharpApp {
    pub fn new() -> Self {
        let locales: HashMap<String, SetupCopy> =
            serde_json::from_str(include_str!("../assets/setup-locales.json"))
                .expect("setup locale data must be valid JSON");
        let copy = locales
            .get("en")
            .expect("English setup locale must be present")
            .clone();
        Self {
            connected: None,
            connected_subscription: None,
            connected_events: None,
            streaming_unpair_confirm: false,
            streaming_unpair_deadline: None,
            show_setup: false,
            theme: PreviewTheme::Dark,
            theme_menu_open: false,
            tab_menu_open: false,
            launcher_menu_open: false,
            steam_options_open: false,
            settings_open: false,
            settings_preview: None,
            streaming_open: false,
            streaming_installed: false,
            streaming_running: false,
            steam_running: false,
            sharp_preview: None,
            logs_preview: None,
            search_input: None,
            search_query: String::new(),
            selected_game: 0,
            dock_start: 0,
            running_game: None,
            pipeline_menu_open: false,
            game_settings_open: false,
            game_preferences: (0..LIBRARY_GAMES.len())
                .map(|_| PreviewGameSettings::default())
                .collect(),
            hovered_card: None,
            active_tab: LibraryTab::Play,
            step: 2,
            selected_language: "en",
            language_menu_open: false,
            locales,
            copy,
            runtime_installing: false,
            runtime_started: false,
            runtime_installed: false,
            runtime_progress: 0,
            install_log_open: false,
            steam_installing: false,
            steam_installed: false,
        }
    }
}

impl MetalSharpApp {
    /// Opt-in real setup; never substitutes fixture titles after completion.
    pub fn new_connected_setup(
        config: crate::backend_host::HostConfig,
        cx: &mut Context<Self>,
    ) -> Self {
        let session = cx.new(|cx| crate::connected::ConnectedApp::new(config, cx));
        let mut app = Self::new();
        app.show_setup = true;
        app.step = 0;
        app.connected_subscription = Some(cx.observe(&session, |this, session, cx| {
            let state = session.read(cx).setup_view();
            if !session
                .read(cx)
                .streaming_view()
                .0
                .is_some_and(|status| status.can_pair())
                || this
                    .streaming_unpair_deadline
                    .is_some_and(|deadline| std::time::Instant::now() >= deadline)
            {
                this.streaming_unpair_confirm = false;
                this.streaming_unpair_deadline = None;
            }
            this.runtime_installed = state.runtime_ready;
            this.runtime_installing = state.runtime_installing;
            this.runtime_started = state.runtime_started;
            this.runtime_progress = state.percent;
            this.steam_installed = state.steam_installed;
            this.steam_installing = state.steam_installing;
            if state.completed {
                this.show_setup = false;
            }
            cx.notify();
        }));
        app.connected_events = Some(cx.subscribe(&session, |this, session, event, cx| {
            match event {
                crate::connected::ConnectedEvent::OpenStreaming
                    if session.read(cx).streaming_visible() =>
                {
                    this.streaming_open = true;
                    this.streaming_unpair_confirm = false;
                }
                crate::connected::ConnectedEvent::OpenStreaming => {}
                crate::connected::ConnectedEvent::OpenSetup => {
                    this.show_setup = true;
                    this.streaming_open = false;
                    this.streaming_unpair_confirm = false;
                    this.streaming_unpair_deadline = None;
                }
            }
            cx.notify();
        }));
        app.connected = Some(session);
        app
    }
    pub fn new_connected_workbench(
        config: crate::backend_host::HostConfig,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut app = Self::new_connected_setup(config, cx);
        app.show_setup = false;
        app
    }
    fn close_streaming_panel(&mut self, cx: &mut Context<Self>) {
        self.streaming_open = false;
        self.streaming_unpair_confirm = false;
        self.streaming_unpair_deadline = None;
        if let Some(session) = self.connected.clone() {
            session.update(cx, |session, cx| session.close_streaming(cx));
        }
        cx.notify();
    }
    fn setup_can_advance(&self, cx: &gpui::App) -> bool {
        self.connected.as_ref().is_none_or(|session| {
            let state = session.read(cx).setup_view();
            state.ready && state.runtime_ready && state.steam_installed && !state.steam_installing
        })
    }
}
impl Render for MetalSharpApp {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if !self.show_setup {
            if let Some(session) = &self.connected {
                return div()
                    .size_full()
                    .relative()
                    .child(session.clone())
                    .children(self.streaming_open.then(|| {
                        gpui::deferred(self.render_streaming_overlay(window.viewport_size(), cx))
                            .with_priority(100)
                    }));
            }
            return self.render_library(window.viewport_size(), cx);
        }
        let asset = |name: &str| asset_path(name);

        div()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgb(PAGE_BG))
            .font_family("Rethink Sans")
            .child(
                div()
                    .size_full()
                    .flex()
                    .overflow_hidden()
                    .rounded(px(18.0))
                    .border_1()
                    .border_color(rgb(0x292b2d))
                    .bg(rgb(PANEL_BG))
                    .shadow_lg()
                    .child(render_visual(asset, self.copy.steps.clone(), self.step))
                    .child(self.render_setup_page(cx)),
            )
    }
}

fn render_visual(
    asset: impl Fn(&str) -> PathBuf,
    steps: Vec<String>,
    current_step: usize,
) -> gpui::Div {
    let cover = |name: &str| {
        img(asset(name))
            .w(px(96.0))
            .h(px(138.0))
            .object_fit(ObjectFit::Cover)
            .rounded(px(6.0))
    };

    div()
        .relative()
        .w(relative(0.52))
        .flex_none()
        .min_w_0()
        .h_full()
        .flex()
        .flex_col()
        .overflow_hidden()
        .bg(rgb(0x0b0e12))
        .child(
            img(asset("setup-backdrop.png"))
                .absolute()
                .top(px(0.0))
                .right(px(0.0))
                .bottom(px(0.0))
                .left(px(0.0))
                .object_fit(ObjectFit::Cover),
        )
        .child(
            div()
                .absolute()
                .top(px(54.0))
                .left(px(0.0))
                .right(px(0.0))
                .h(px(330.0))
                .flex()
                .items_center()
                .justify_center()
                .child(
                    img(asset("setup-hero-logo.png"))
                        .w(px(330.0))
                        .h(px(330.0))
                        .object_fit(ObjectFit::Contain),
                ),
        )
        .child(
            div()
                .relative()
                .mt_auto()
                .flex()
                .items_end()
                .justify_center()
                .gap(px(14.0))
                .px(px(30.0))
                .pb(px(26.0))
                .child(cover("cyberpunk.jpg").mt(px(11.0)))
                .child(
                    cover("elden-ring.jpg")
                        .mb(px(14.0))
                        .w(px(108.0))
                        .h(px(154.0)),
                )
                .child(cover("hades.jpg").mb(px(5.0)))
                .child(cover("stray.jpg").mt(px(8.0))),
        )
        .child(render_steps(steps, current_step))
}

fn render_steps(labels: Vec<String>, current_step: usize) -> gpui::Div {
    let mut row = div()
        .relative()
        .flex()
        .items_start()
        .px(px(40.0))
        .pb(px(26.0));

    for (index, label) in labels.iter().enumerate() {
        let current = index == current_step;
        let done = index < current_step;
        let step = div()
            .flex()
            .flex_col()
            .items_center()
            .gap(px(8.0))
            .child(
                div()
                    .size(px(13.0))
                    .rounded_full()
                    .border_1()
                    .border_color(rgb(if current || done { 0xefe6d3 } else { 0x6a6a68 }))
                    .bg(rgb(if current || done { 0xefe6d3 } else { 0x15181b })),
            )
            .child(
                div()
                    .text_size(px(11.0))
                    .font_weight(if current {
                        FontWeight::SEMIBOLD
                    } else {
                        FontWeight::NORMAL
                    })
                    .text_color(rgb(if current { 0xffffff } else { 0x8c8e8d }))
                    .child(label.clone()),
            );
        row = row.child(step);
        if index < labels.len() - 1 {
            row = row.child(div().flex_1().h(px(1.0)).mt(px(6.0)).mx(px(12.0)).bg(rgb(
                if index < current_step {
                    0x8e8065
                } else {
                    0x454747
                },
            )));
        }
    }
    row
}

impl MetalSharpApp {
    fn render_library(
        &mut self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        if let Some(settings) = &self.settings_preview {
            let palette = self.theme.page_palette();
            let language = self.selected_language;
            settings.update(cx, |settings, cx| {
                if settings.palette != palette || settings.language != language {
                    settings.palette = palette;
                    settings.language = language;
                    cx.notify();
                }
            });
        }
        if self.sharp_preview.is_none() {
            self.sharp_preview = Some(cx.new(|_| {
                let mut sharp = crate::sharp_preview::SharpPreview::new();
                sharp.asset_root = asset_path("");
                sharp
            }));
        }
        if let Some(sharp) = &self.sharp_preview {
            let palette = self.theme.page_palette();
            sharp.update(cx, |sharp, cx| {
                if sharp.palette != palette {
                    sharp.palette = palette;
                    cx.notify();
                }
            });
        }
        if self.logs_preview.is_none() {
            self.logs_preview = Some(cx.new(|_| crate::logs_preview::LogsPreview::new()));
        }
        if let Some(logs) = &self.logs_preview {
            let palette = self.theme.page_palette();
            logs.update(cx, |logs, cx| {
                if logs.palette != palette {
                    logs.palette = palette;
                    cx.notify();
                }
            });
        }
        let show_search = matches!(self.active_tab, LibraryTab::Play | LibraryTab::Collection);
        if self.search_input.is_none() {
            let input = cx.new(crate::search_input::SearchInput::new);
            cx.observe(&input, |this, input, cx| {
                let query = input.read(cx).content.to_string();
                if this.search_query != query {
                    this.search_query = query;
                    this.dock_start = 0;
                    let matches = this.matching_preview_games();
                    if !matches.contains(&this.selected_game) {
                        if let Some(first) = matches.first() {
                            this.selected_game = *first;
                        }
                    }
                    this.select_preview_game(this.selected_game);
                    cx.notify();
                }
            })
            .detach();
            self.search_input = Some(input);
        }
        let search_input = self.search_input.clone().unwrap();
        let theme = self.theme;
        search_input.update(cx, |input, _| {
            input.placeholder = library_search_placeholder(f32::from(viewport.width)).into();
            input.text_color = theme.control_text();
            input.placeholder_color = if theme == PreviewTheme::Light {
                0x000000
            } else {
                0xaaaaaa
            };
        });
        let control_bg = theme.control_bg();
        let control_text = theme.control_text();
        let current_tab_label = match self.active_tab {
            LibraryTab::Play => "Play",
            LibraryTab::Collection => "Collection",
            LibraryTab::SharpLibrary => "Sharp Library",
            LibraryTab::Logs => "Logs",
        };

        let mut theme_menu = None;
        let mut tab_menu = None;
        let mut steam_menu = None;
        if self.theme_menu_open {
            let mut menu = div()
                .absolute()
                .top(px(42.0))
                .right(px(0.0))
                .w(px(160.0))
                .flex()
                .flex_col()
                .gap(px(2.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(rgb(theme.button_border()))
                .bg(rgb(theme.menu_bg()))
                .shadow_lg()
                .p(px(6.0));
            for (index, option) in PreviewTheme::ALL.iter().copied().enumerate() {
                let selected = option == theme;
                menu = menu.child(
                    div()
                        .id(("library-theme-option", index))
                        .h(px(29.0))
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .px(px(8.0))
                        .rounded(px(5.0))
                        .bg(rgb(if selected {
                            theme.menu_hover()
                        } else {
                            theme.menu_bg()
                        }))
                        .text_size(px(11.0))
                        .text_color(rgb(control_text))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(theme.menu_hover())))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.theme = option;
                            this.theme_menu_open = false;
                            cx.notify();
                        }))
                        .child(div().size(px(17.0)).child(option.render_icon(control_text)))
                        .child(option.label()),
                );
            }
            theme_menu = Some(gpui::deferred(menu.id("theme_menu").occlude()).with_priority(10));
        }

        if self.tab_menu_open {
            let options = [
                (LibraryTab::Play, "⌂", "Play"),
                (LibraryTab::Collection, "▦", "Collection"),
                (LibraryTab::SharpLibrary, "⬇", "Sharp Library"),
                (LibraryTab::Logs, "▤", "Logs"),
            ];
            let mut menu = div()
                .absolute()
                .top(px(42.0))
                .right(px(0.0))
                .w(px(174.0))
                .flex()
                .flex_col()
                .gap(px(2.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(rgb(theme.button_border()))
                .bg(rgb(theme.menu_bg()))
                .shadow_lg()
                .p(px(6.0));
            for (index, (tab, icon, label)) in options.into_iter().enumerate() {
                let selected = tab == self.active_tab;
                menu = menu.child(
                    div()
                        .id(("library-tab-option", index))
                        .h(px(31.0))
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .px(px(9.0))
                        .rounded(px(5.0))
                        .bg(rgb(if selected {
                            theme.menu_hover()
                        } else {
                            theme.menu_bg()
                        }))
                        .text_size(px(12.0))
                        .text_color(rgb(control_text))
                        .cursor_pointer()
                        .hover(move |style| style.bg(rgb(theme.menu_hover())))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.active_tab = tab;
                            this.tab_menu_open = false;
                            cx.notify();
                        }))
                        .child(icon)
                        .child(label),
                );
            }
            tab_menu = Some(gpui::deferred(menu.id("tab_menu").occlude()).with_priority(10));
        }

        if self.launcher_menu_open || self.steam_options_open {
            let is_launcher = self.launcher_menu_open;
            let mut menu = div()
                .absolute()
                .top(px(42.0))
                .right(px(if is_launcher { 27.0 } else { 0.0 }))
                .min_w(px(178.0))
                .flex()
                .flex_col()
                .gap(px(2.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(rgb(theme.button_border()))
                .bg(rgb(theme.menu_bg()))
                .shadow_lg()
                .p(px(5.0));
            let options: &[&str] = if is_launcher {
                &["Launch Ubisoft", "Local preview only"]
            } else {
                &["Fix Steam", "Steam options"]
            };
            for (index, label) in options.iter().enumerate() {
                menu = menu.child(
                    div()
                        .id(("steam-menu-option", index))
                        .h(px(30.0))
                        .flex()
                        .items_center()
                        .px(px(10.0))
                        .rounded(px(5.0))
                        .text_size(px(12.5))
                        .text_color(rgb(control_text))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(theme.menu_hover())))
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.launcher_menu_open = false;
                            this.steam_options_open = false;
                            cx.notify();
                        }))
                        .child(*label),
                );
            }
            steam_menu = Some(gpui::deferred(menu.id("steam_menu").occlude()).with_priority(10));
        }

        let mut header = div()
            .relative()
            .w_full()
            .flex_none()
            .h(px(56.0))
            .min_h(px(56.0))
            .flex()
            .items_center()
            .gap(px(10.0))
            .px(px(16.0))
            .bg(rgb(0x191c1f))
            .border_b_1()
            .border_color(rgba(0xe7eaec24));

        header = header
            .child(
                div()
                    .flex()
                    .flex_none()
                    .items_center()
                    .gap(px(10.0))
                    .text_size(px(15.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(0xf2f2f1))
                    .child(
                        img(asset_path("metalsharp-logo.png"))
                            .size(px(27.0))
                            .object_fit(ObjectFit::Contain),
                    )
                    .child("MetalSharp"),
            )
            .child(
                div()
                    .relative()
                    .flex()
                    .flex_none()
                    .child(
                        div()
                            .id("library-steam-button")
                            .h(px(36.0))
                            .flex()
                            .items_center()
                            .gap(px(8.0))
                            .px(px(15.0))
                            .rounded_tl(px(8.0))
                            .rounded_bl(px(8.0))
                            .border_1()
                            .border_color(rgb(theme.button_border()))
                            .text_size(px(13.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(0xffffff))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.steam_running = !this.steam_running;
                                cx.notify();
                            }))
                            .child("◉")
                            .child(if self.steam_running {
                                "Stop Steam"
                            } else {
                                "Start Steam"
                            }),
                    )
                    .child(
                        div()
                            .id("library-launcher-menu-toggle")
                            .h(px(36.0))
                            .w(px(25.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .border_1()
                            .border_l_0()
                            .border_color(rgb(theme.border()))
                            .bg(rgb(control_bg))
                            .text_color(rgb(control_text))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let open = !this.launcher_menu_open;
                                this.close_library_menus();
                                this.launcher_menu_open = open;
                                this.steam_options_open = false;
                                cx.notify();
                            }))
                            .child("⌄"),
                    )
                    .child(
                        div()
                            .id("library-steam-options-toggle")
                            .h(px(36.0))
                            .w(px(27.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded_tr(px(8.0))
                            .rounded_br(px(8.0))
                            .border_1()
                            .border_l_0()
                            .border_color(rgb(theme.border()))
                            .bg(rgb(control_bg))
                            .text_color(rgb(control_text))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let open = !this.steam_options_open;
                                this.close_library_menus();
                                this.steam_options_open = open;
                                this.launcher_menu_open = false;
                                cx.notify();
                            }))
                            .child("⚙"),
                    )
                    .children(steam_menu),
            )
            .child(if show_search {
                div()
                    .flex_1()
                    .min_w_0()
                    .px(px(20.0))
                    .flex()
                    .justify_center()
                    .child(
                        div()
                            .id("library-search")
                            .flex_1()
                            .min_w_0()
                            .max_w(px(720.0))
                            .overflow_hidden()
                            .h(px(38.0))
                            .flex()
                            .items_center()
                            .gap(px(11.0))
                            .px(px(12.0))
                            .rounded(px(8.0))
                            .border_1()
                            .border_color(rgb(theme.border()))
                            .bg(rgb(control_bg))
                            .text_size(px(14.0))
                            .text_color(rgb(if theme == PreviewTheme::Light {
                                0x000000
                            } else {
                                0xaaaaaa
                            }))
                            .cursor(gpui::CursorStyle::IBeam)
                            .on_click(cx.listener(|this, _, window, cx| {
                                if let Some(input) = &this.search_input {
                                    window.focus(&input.read(cx).focus_handle(cx));
                                }
                                this.close_library_menus();
                                cx.notify();
                            }))
                            .child("⌕")
                            .child(div().flex_1().min_w_0().child(search_input)),
                    )
            } else {
                div().flex_1()
            })
            .child(
                div()
                    .relative()
                    .flex_none()
                    .child(
                        div()
                            .id("library-theme-toggle")
                            .h(px(30.0))
                            .w(px(28.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded(px(5.0))
                            .border_1()
                            .border_color(rgb(theme.button_border()))
                            .text_size(px(17.0))
                            .text_color(rgb(0xffffff))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let open = !this.theme_menu_open;
                                this.close_library_menus();
                                this.theme_menu_open = open;
                                this.tab_menu_open = false;
                                cx.notify();
                            }))
                            .child(theme.render_icon(0xffffff)),
                    )
                    .children(theme_menu),
            )
            .child(
                div()
                    .relative()
                    .flex_none()
                    .child(
                        div()
                            .id("library-tab-toggle")
                            .h(px(36.0))
                            .flex()
                            .items_center()
                            .gap(px(8.0))
                            .px(px(13.0))
                            .rounded(px(7.0))
                            .border_1()
                            .border_color(rgb(theme.button_border()))
                            .text_size(px(13.0))
                            .text_color(rgb(0xffffff))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let open = !this.tab_menu_open;
                                this.close_library_menus();
                                this.tab_menu_open = open;
                                this.theme_menu_open = false;
                                cx.notify();
                            }))
                            .child(match self.active_tab {
                                LibraryTab::Play => "⌂",
                                LibraryTab::Collection => "▦",
                                LibraryTab::SharpLibrary => "⬇",
                                LibraryTab::Logs => "▤",
                            })
                            .child(current_tab_label)
                            .child("⌄"),
                    )
                    .children(tab_menu),
            )
            .child(
                div().relative().flex_none().child(
                    div()
                        .id("library-settings-toggle")
                        .size(px(34.0))
                        .ml(px(7.0))
                        .flex()
                        .items_center()
                        .justify_center()
                        .rounded(px(8.0))
                        .border_1()
                        .border_color(rgb(theme.button_border()))
                        .text_size(px(18.0))
                        .text_color(rgb(0xffffff))
                        .cursor_pointer()
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.open_settings_preview(window, cx);
                        }))
                        .child("⚙"),
                ),
            );

        let body = match self.active_tab {
            LibraryTab::Play => self.render_library_play(viewport, cx),
            LibraryTab::Collection => self.render_library_collection(viewport, cx),
            LibraryTab::Logs => div()
                .id("logs-page-body")
                .relative()
                .flex_1()
                .min_h_0()
                .w_full()
                .overflow_hidden()
                .child(self.logs_preview.clone().unwrap()),
            LibraryTab::SharpLibrary => div()
                .id("sharp-page-body")
                .relative()
                .flex_1()
                .min_h_0()
                .w_full()
                .overflow_hidden()
                .child(self.sharp_preview.clone().unwrap()),
        };

        let footer = div()
            .h(px(68.0))
            .min_h(px(68.0))
            .flex()
            .items_center()
            .justify_between()
            .gap(px(22.0))
            .px(px(26.0))
            .border_t_1()
            .border_color(rgba(if theme == PreviewTheme::Light {
                0xffffffff
            } else {
                0xe7eaec24
            }))
            .bg(rgb(0x1b1e20))
            .text_color(rgb(0xd8dad9))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(11.0))
                    .child(
                        div()
                            .size(px(25.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .text_size(px(20.0))
                            .text_color(rgb(0x74d28a))
                            .child("✓"),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .text_size(px(13.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(rgb(0xe2e4e3))
                                    .child("Preview library ready"),
                            )
                            .child(div().text_size(px(11.0)).text_color(rgb(0x969b9a)).child(
                                format!("{} sample games · UI preview only", LIBRARY_GAMES.len()),
                            )),
                    ),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.0))
                    .px(px(14.0))
                    .py(px(7.0))
                    .rounded(px(10.0))
                    .border_1()
                    .border_color(rgba(if theme == PreviewTheme::Light {
                        0xffffffff
                    } else {
                        0xe7eaec24
                    }))
                    .bg(rgba(0xffffff08))
                    .text_color(rgb(0xd8dad9))
                    .id("footer-stream")
                    .cursor_pointer()
                    .hover(|style| {
                        style
                            .border_color(rgba(if theme == PreviewTheme::Light {
                                0xffffffff
                            } else {
                                0x74d2c873
                            }))
                            .bg(rgba(0x74d2c80f))
                    })
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.close_library_menus();
                        this.streaming_open = true;
                        cx.notify();
                    }))
                    .child(
                        div()
                            .relative()
                            .size(px(18.0))
                            .child(
                                gpui::svg()
                                    .path("stream-tv.svg")
                                    .size(px(18.0))
                                    .text_color(rgb(0x74d2c8)),
                            )
                            .child(
                                div()
                                    .absolute()
                                    .right(px(-5.0))
                                    .bottom(px(-3.0))
                                    .size(px(14.0))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .rounded_full()
                                    .bg(rgb(0x74d2c8))
                                    .child(
                                        gpui::svg()
                                            .path("stream-wifi.svg")
                                            .size(px(9.0))
                                            .text_color(rgb(0x0f1214)),
                                    ),
                            ),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .child(
                                div()
                                    .text_size(px(12.5))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child("Stream"),
                            )
                            .child(
                                div()
                                    .text_size(px(10.5))
                                    .text_color(rgb(0x8b9290))
                                    .child("To phone / tablet"),
                            ),
                    ),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_end()
                    .gap(px(11.0))
                    .child(
                        div()
                            .size(px(30.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded_full()
                            .border_1()
                            .border_color(rgba(if theme == PreviewTheme::Light {
                                0xffffffff
                            } else {
                                0xe7eaec2e
                            }))
                            .bg(rgb(0x282c2d))
                            .text_size(px(16.0))
                            .text_color(rgb(0x777d7b))
                            .child("↓"),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .text_size(px(13.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(rgb(0xe2e4e3))
                                    .child("Up to date"),
                            )
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .text_color(rgb(0x969b9a))
                                    .child("Preview build · updater disabled"),
                            ),
                    ),
            );

        div()
            .relative()
            .size_full()
            .flex()
            .flex_col()
            .overflow_hidden()
            .font_family("Rethink Sans")
            .bg(rgb(0x111416))
            .child(header)
            .child(body)
            .child(footer)
            .children(
                self.settings_open.then(|| {
                    gpui::deferred(self.settings_preview.clone().unwrap()).with_priority(200)
                }),
            )
            .children(self.streaming_open.then(|| {
                gpui::deferred(self.render_streaming_overlay(viewport, cx)).with_priority(100)
            }))
    }

    fn render_library_play(
        &mut self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let matches = self.matching_preview_games();
        if matches.is_empty() {
            return div()
                .id("library-no-search-results")
                .flex_1()
                .flex()
                .items_center()
                .justify_center()
                .text_color(rgb(0xd4d5d4))
                .child("No matching preview games");
        }
        let width = f32::from(viewport.width);
        let hero_height = (f32::from(viewport.height) * 0.54).clamp(300.0, 520.0);
        let card_width = dock_card_width(width);
        let card_height = card_width * 1.35 + 12.0;
        let selected = self.selected_game.min(LIBRARY_GAMES.len() - 1);
        let (game_name, _, hero_art) = LIBRARY_GAMES[selected];
        let theme = self.theme;
        let accent = theme.accent();
        let control_bg = theme.control_bg();
        let control_text = theme.control_text();
        let is_running = self.running_game == Some(selected);
        let pipeline = PREVIEW_PIPELINES[self.game_preferences[selected].pipeline];
        let pipeline_menu = self
            .pipeline_menu_open
            .then(|| gpui::deferred(self.render_pipeline_menu(cx)).with_priority(20));

        let game_settings_menu = self
            .game_settings_open
            .then(|| gpui::deferred(self.render_game_settings(cx)).with_priority(20));
        let hero = div()
            .relative()
            .w_full()
            .h(px(hero_height))
            .min_h(px(hero_height))
            .flex_none()
            .flex()
            .items_center()
            .overflow_hidden()
            .bg(rgb(0x242629))
            .child(
                img(asset_path(hero_art))
                    .w_full()
                    .h_full()
                    .absolute()
                    .top(px(0.0))
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .object_fit(ObjectFit::Cover)
                    .opacity(0.62),
            )
            .child(
                div()
                    .absolute()
                    .top(px(0.0))
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .bg(rgba(0x06080950)),
            )
            .child(
                div()
                    .absolute()
                    .top(px(0.0))
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .bg(linear_gradient(
                        90.0,
                        linear_color_stop(rgba(0x060809d8), 0.0),
                        linear_color_stop(rgba(0x06080925), 1.0),
                    )),
            )
            .child(
                div()
                    .absolute()
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .h(px(245.0))
                    .bg(linear_gradient(
                        180.0,
                        linear_color_stop(rgba(0x11141600), 0.0),
                        linear_color_stop(rgb(0x111416), 1.0),
                    )),
            )
            .child(
                div()
                    .relative()
                    .w_full()
                    .pr(px(24.0))
                    .pl(px(if width < 900.0 { 28.0 } else { 52.0 }))
                    .flex()
                    .flex_col()
                    .items_start()
                    .child(
                        div()
                            .mb(px(12.0))
                            .text_size(px(39.0))
                            .line_height(px(38.0))
                            .font_family("Georgia")
                            .text_color(rgb(theme.hero_title()))
                            .child(game_name),
                    )
                    .child(
                        div()
                            .w_full()
                            .mt(px(22.0))
                            .flex()
                            .items_center()
                            .justify_between()
                            .gap(px(24.0))
                            .child(
                                div()
                                    .id("library-play-action")
                                    .w(px(204.0))
                                    .h(px(48.0))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .gap(px(10.0))
                                    .rounded(px(8.0))
                                    .border_1()
                                    .border_color(rgb(theme.border()))
                                    .bg(rgb(if is_running { 0xa52d2d } else { control_bg }))
                                    .text_size(px(15.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(if is_running {
                                        0xffffff
                                    } else {
                                        control_text
                                    }))
                                    .cursor_pointer()
                                    .hover(|style| style.bg(rgb(theme.control_hover())))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.running_game = if this.running_game == Some(selected) {
                                            None
                                        } else {
                                            Some(selected)
                                        };
                                        this.log_preview_event(format!("[PREVIEW] [{}] {} · simulated library action; no process launched", if this.running_game.is_some() { "LAUNCHED" } else { "STOPPED" }, game_name), cx);
                                        cx.notify();
                                    }))
                                    .child(if is_running { "■" } else { "▶" })
                                    .child(if is_running { "Stop" } else { "Play" }),
                            )
                            .child(
                                div()
                                    .flex_none()
                                    .flex()
                                    .items_center()
                                    .gap(px(9.0))
                                    .child(
                                        div()
                                            .id("library-art-manager")
                                            .h(px(42.0))
                                            .px(px(12.0))
                                            .flex()
                                            .items_center()
                                            .justify_center()
                                            .rounded(px(8.0))
                                            .border_1()
                                            .border_color(rgba(0xe7eaec4d))
                                            .bg(rgba(0x0c0f10b8))
                                            .text_size(px(16.0))
                                            .text_color(rgb(0xf0f0ee))
                                            .cursor_pointer()
                                            .child("✎"),
                                    )
                                    .child(
                                        div()
                                            .relative()
                                            .flex_none()
                                            .child(
                                                div()
                                                    .id("library-pipeline-select")
                                                    .h(px(42.0))
                                                    .flex()
                                                    .items_center()
                                                    .gap(px(9.0))
                                                    .px(px(13.0))
                                                    .rounded(px(8.0))
                                                    .border_1()
                                                    .border_color(rgba(0xe7eaec4d))
                                                    .bg(rgba(0x0c0f10b8))
                                                    .text_size(px(12.0))
                                                    .text_color(rgb(0xf0f0ee))
                                                    .cursor_pointer()
                                                    .on_click(cx.listener(|this, _, _, cx| {
                                                        let open = !this.pipeline_menu_open;
                                                        this.close_library_menus();
                                                        this.pipeline_menu_open = open;
                                                        cx.notify();
                                                    }))
                                                    .child("Bottle")
                                                    .child(pipeline)
                                                    .child("⌄"),
                                            )
                                            .children(pipeline_menu),
                                    )
                                    .child(
                                        div()
                                            .relative()
                                            .flex_none()
                                            .child(
                                                div()
                                                    .id("library-game-settings")
                                                    .size(px(42.0))
                                                    .flex()
                                                    .items_center()
                                                    .justify_center()
                                                    .rounded(px(8.0))
                                                    .border_1()
                                                    .border_color(rgba(0xe7eaec4d))
                                                    .bg(rgba(0x0c0f10b8))
                                                    .text_size(px(17.0))
                                                    .text_color(rgb(0xf0f0ee))
                                                    .cursor_pointer()
                                                    .on_click(cx.listener(|this, _, _, cx| {
                                                        let open = !this.game_settings_open;
                                                        this.close_library_menus();
                                                        this.game_settings_open = open;
                                                        cx.notify();
                                                    }))
                                                    .child("⚙"),
                                            )
                                            .children(game_settings_menu),
                                    ),
                            ),
                    )
                    .child(
                        gpui::deferred(
                            div()
                                .id("library-view-all")
                                .occlude()
                                .absolute()
                                .top(relative(1.0))
                                .right(px(24.0))
                                .mt(px(32.0))
                                .h(px(28.0))
                                .flex()
                                .items_center()
                                .gap(px(4.0))
                                .text_size(px(13.0))
                                .text_color(rgb(0xd4d5d4))
                                .cursor_pointer()
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.active_tab = LibraryTab::Collection;
                                    cx.notify();
                                }))
                                .child("View all")
                                .child("›"),
                        )
                        .with_priority(5),
                    ),
            );

        let mut cards = div()
            .flex_1()
            .min_w_0()
            .flex()
            .items_end()
            .justify_center()
            .gap(px(16.0))
            .px(px(10.0))
            .pt(px(48.0))
            .pb(px(20.0));
        let dock_start = self
            .dock_start
            .min(matches.len().saturating_sub(DOCK_VISIBLE_CARDS));
        let visible_count = matches
            .len()
            .saturating_sub(dock_start)
            .min(DOCK_VISIBLE_CARDS);
        let first_slot = (DOCK_VISIBLE_CARDS - visible_count) / 2;
        for (slot, index) in matches
            .iter()
            .copied()
            .skip(dock_start)
            .take(DOCK_VISIBLE_CARDS)
            .enumerate()
        {
            let (_, cover, _) = LIBRARY_GAMES[index];
            let selected_card = index == selected;
            let arch_slot = first_slot + slot;
            let angle = dock_card_angle(arch_slot);
            let card_art = if angle == 0 {
                cover.to_string()
            } else {
                format!("dock/{}-{angle}.png", cover.trim_end_matches(".jpg"))
            };
            let hovered = self.hovered_card == Some(index);
            let running = self.running_game == Some(index);
            let card = div()
                .id(("showcase-card", index))
                .relative()
                .w(px(card_width))
                .h(px(card_height))
                .flex_none()
                .top(px(dock_card_lift(
                    arch_slot,
                    if selected_card { arch_slot } else { usize::MAX },
                )))
                .cursor_pointer()
                .on_hover(cx.listener(move |this, is_hovered, _, cx| {
                    this.hovered_card = if *is_hovered { Some(index) } else { None };
                    cx.notify();
                }))
                .on_click(cx.listener(move |this, _, _, cx| {
                    this.selected_game = index;
                    this.active_tab = LibraryTab::Play;
                    this.close_library_menus();
                    cx.notify();
                }))
                .child(
                    div()
                        .absolute()
                        .top(px(14.0))
                        .right(px(6.0))
                        .bottom(px(20.0))
                        .left(px(6.0))
                        .rounded(px(7.0))
                        .shadow(vec![gpui::BoxShadow {
                            color: rgba(
                                theme.dock_glow() << 8 | if selected_card { 0xb0 } else { 0x78 },
                            )
                            .into(),
                            offset: gpui::point(px(0.0), px(16.0)),
                            blur_radius: px(38.0),
                            spread_radius: px(9.0),
                        }]),
                )
                .child(
                    div()
                        .absolute()
                        .top(px(0.0))
                        .right(px(0.0))
                        .bottom(px(12.0))
                        .left(px(0.0))
                        .when(angle == 0, |frame| {
                            frame
                                .overflow_hidden()
                                .rounded(px(7.0))
                                .border_1()
                                .border_color(rgb(if selected_card { accent } else { 0x54595b }))
                                .bg(rgb(0x242729))
                                .shadow_lg()
                        })
                        .child(
                            img(asset_path(&card_art))
                                .w_full()
                                .h_full()
                                .object_fit(ObjectFit::Cover)
                                .when(angle != 0, |image| {
                                    image
                                        .relative()
                                        .w(relative(1.4))
                                        .h(relative(630.0 / 486.0))
                                        .left(px(-card_width * 0.2))
                                        .top(px(-(card_height - 12.0) * 72.0 / 486.0))
                                        .object_fit(ObjectFit::Contain)
                                }),
                        )
                        .child(
                            div()
                                .absolute()
                                .top(px(0.0))
                                .right(px(0.0))
                                .bottom(px(0.0))
                                .left(px(0.0))
                                .bg(linear_gradient(
                                    180.0,
                                    linear_color_stop(rgba(0x00000000), 0.0),
                                    linear_color_stop(
                                        rgba(if angle == 0 { 0x00000066 } else { 0x00000000 }),
                                        1.0,
                                    ),
                                )),
                        )
                        .child(if hovered || running {
                            div()
                                .id(("showcase-play-overlay", index))
                                .absolute()
                                .left(px(0.0))
                                .right(px(0.0))
                                .bottom(px(16.0))
                                .flex()
                                .justify_center()
                                .child(
                                    div()
                                        .id(("showcase-play", index))
                                        .h(px(34.0))
                                        .flex()
                                        .items_center()
                                        .gap(px(7.0))
                                        .px(px(12.0))
                                        .rounded(px(6.0))
                                        .border_1()
                                        .border_color(rgba(0xffffff80))
                                        .bg(rgb(if running { 0xa52d2d } else { 0xeee5d6 }))
                                        .text_size(px(12.0))
                                        .font_weight(FontWeight::BOLD)
                                        .text_color(rgb(if running { 0xffffff } else { 0x171819 }))
                                        .on_click(cx.listener(move |this, _, _, cx| {
                                            this.running_game = if this.running_game == Some(index)
                                            {
                                                None
                                            } else {
                                                Some(index)
                                            };
                                            cx.notify();
                                        }))
                                        .child(if running { "■" } else { "▶" })
                                        .child(if running { "Stop" } else { "Play" }),
                                )
                        } else {
                            div().id(("showcase-play-hidden", index)).h(px(0.0))
                        }),
                );
            cards = cards.child(card);
        }

        let can_scroll_back = dock_start > 0;
        let can_scroll_forward = dock_start + DOCK_VISIBLE_CARDS < matches.len();
        let dock = div()
            .relative()
            .w_full()
            .flex_none()
            .mt(px(-hero_height * 0.40))
            .px(px(24.0))
            .pt(px(18.0))
            .pb(px(70.0))
            .bg(linear_gradient(
                180.0,
                linear_color_stop(rgba(0x11141600), 0.0),
                linear_color_stop(rgb(0x111416), 1.0),
            ))
            // Preserve dock spacing; View All is anchored to the hero controls above.
            .child(div().h(px(27.0)))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.0))
                    .mx_auto()
                    .max_w(px(1480.0))
                    .child(
                        div()
                            .id("dock-previous")
                            .w(px(38.0))
                            .h(px(66.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .flex_none()
                            .rounded(px(8.0))
                            .border_1()
                            .border_color(rgb(accent))
                            .bg(rgb(0x292d2f))
                            .opacity(if can_scroll_back { 1.0 } else { 0.28 })
                            .text_size(px(20.0))
                            .text_color(rgb(0xffffff))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.scroll_preview_dock(-1);
                                cx.notify();
                            }))
                            .child("‹"),
                    )
                    .child(cards)
                    .child(
                        div()
                            .id("dock-next")
                            .w(px(38.0))
                            .h(px(66.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .flex_none()
                            .rounded(px(8.0))
                            .border_1()
                            .border_color(rgb(accent))
                            .bg(rgb(0x292d2f))
                            .opacity(if can_scroll_forward { 1.0 } else { 0.28 })
                            .text_size(px(20.0))
                            .text_color(rgb(0xffffff))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.scroll_preview_dock(1);
                                cx.notify();
                            }))
                            .child("›"),
                    ),
            );

        let page = div()
            .id("library-play-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .bg(rgb(0x111416))
            .flex()
            .flex_col()
            .child(hero)
            .child(dock);

        page
    }

    fn render_streaming_overlay(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let live = self
            .connected
            .as_ref()
            .map(|session| session.read(cx).streaming_view());
        let status = live.as_ref().and_then(|(status, _, _, _)| status.as_ref());
        let ready = live
            .as_ref()
            .is_none_or(|(status, busy, _, _)| status.is_some() && !*busy);
        let installed = if live.is_some() {
            status.is_some_and(|status| status.installed)
        } else {
            self.streaming_installed
        };
        let running = if live.is_some() {
            status.is_some_and(|status| status.running)
        } else {
            self.streaming_running
        };
        let installing = status.is_some_and(|status| status.installing);
        let title = |label: &'static str| {
            div()
                .text_size(px(14.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xe6e8e7))
                .child(label)
        };
        let card = || {
            div()
                .flex_none()
                .p(px(16.0))
                .px(px(18.0))
                .rounded(px(12.0))
                .border_1()
                .border_color(rgba(0xffffff12))
                .bg(rgba(0xffffff05))
                .text_color(rgb(0x9aa09e))
        };
        let button = |id: &'static str, label: &'static str, primary: bool| {
            div()
                .id(id)
                .flex()
                .items_center()
                .gap(px(7.0))
                .px(px(15.0))
                .py(px(8.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(rgba(if primary { 0x74d2c8ff } else { 0xffffff24 }))
                .bg(rgba(if primary { 0x74d2c8ff } else { 0xffffff0a }))
                .text_color(rgb(if primary { 0x10201d } else { 0xe2e4e3 }))
                .text_size(px(12.5))
                .font_weight(FontWeight::SEMIBOLD)
                .cursor_pointer()
                .child(label)
        };
        let mut host = card().child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .gap(px(12.0))
                .mb(px(10.0))
                .child(title("Sunshine Host (this Mac)"))
                .child(
                    div()
                        .px(px(9.0))
                        .py(px(3.0))
                        .rounded_full()
                        .text_size(px(11.0))
                        .text_color(rgb(if running {
                            0x7ce0a3
                        } else if installed {
                            0xffd47f
                        } else {
                            0xb9bfbd
                        }))
                        .bg(rgba(if running {
                            0x74d28a1f
                        } else if installed {
                            0xffc14d1a
                        } else {
                            0xffffff0f
                        }))
                        .child(if live.is_some() && status.is_none() {
                            if live.as_ref().is_some_and(|(_, busy, _, _)| *busy) {
                                "Checking…"
                            } else {
                                "Status unavailable"
                            }
                        } else if installing {
                            "Installing…"
                        } else if running {
                            "Connected"
                        } else if installed {
                            "Installed — Offline"
                        } else {
                            "Not installed"
                        }),
                ),
        );
        if !installed {
            host = host.child(div().mb(px(12.0)).line_height(px(19.4)).child("Sunshine captures this Mac's screen and streams it over your local network. Install it once — about a 40 MB download from LizardByte."));
        }
        let mut actions = div().flex().flex_wrap().gap(px(9.0));
        if !installed && !installing {
            actions = actions.child(
                button("stream-install", "↓  Install Sunshine", true).on_click(cx.listener(
                    |this, _, _, cx| {
                        if let Some(session) = this.connected.clone() {
                            session.update(cx, |session, cx| {
                                session.streaming_command(
                                    crate::streaming::StreamingAction::Install,
                                    cx,
                                )
                            });
                        } else {
                            this.streaming_installed = true;
                        }
                        cx.notify();
                    },
                )),
            );
        } else if !running && !installing {
            actions = actions.child(
                button("stream-start", "▶  Start Streaming Host", true).on_click(cx.listener(
                    |this, _, _, cx| {
                        if let Some(session) = this.connected.clone() {
                            session.update(cx, |session, cx| {
                                session
                                    .streaming_command(crate::streaming::StreamingAction::Start, cx)
                            });
                        } else {
                            this.streaming_running = true;
                        }
                        cx.notify();
                    },
                )),
            );
        } else if running {
            actions =
                actions
                    .child(
                        button("stream-stop", "■  Stop", false).on_click(cx.listener(
                            |this, _, _, cx| {
                                if let Some(session) = this.connected.clone() {
                                    session.update(cx, |session, cx| {
                                        session.streaming_command(
                                            crate::streaming::StreamingAction::Stop,
                                            cx,
                                        )
                                    });
                                } else {
                                    this.streaming_running = false;
                                }
                                cx.notify();
                            },
                        )),
                    )
                    .child(button("stream-web", "↗  Sunshine Web UI", false).on_click(
                        cx.listener(|this, _, _, cx| {
                            // External system browser, not a privacy-rule bypass in WKWebView.
                            if this.connected.as_ref().is_some_and(|session| {
                                session
                                    .read(cx)
                                    .streaming_view()
                                    .0
                                    .is_some_and(|status| status.running)
                            }) {
                                cx.open_url("https://localhost:47990");
                            }
                            cx.notify();
                        }),
                    ));
        }
        host = host.child(actions.opacity(if ready { 1.0 } else { 0.5 }));
        if let Some(status) = status {
            if status.installing || status.progress_detail.is_some() {
                host = host.child(div().mt(px(10.)).child(format!(
                    "{} · {}",
                    status.progress_status.as_deref().unwrap_or("Status"),
                    status.progress_detail.as_deref().unwrap_or("")
                )));
            }
        }
        if installed {
            host = host.child(
                div()
                    .mt(px(10.0))
                    .text_size(px(11.5))
                    .text_color(rgb(0x7d8381))
                    .child(if live.is_some() {
                        format!(
                            "Version {} · https://localhost:47990",
                            status
                                .map(|status| status.version.as_str())
                                .filter(|version| !version.is_empty())
                                .unwrap_or("unknown")
                        )
                    } else {
                        "Version preview · https://localhost:47990".into()
                    }),
            );
        }
        let moonlight_instruction = "1. Install Moonlight on your phone or tablet — App Store / Google Play (Works best on the same Wi-Fi.)";
        let links_start = moonlight_instruction.find("App Store").unwrap();
        let moonlight_line = gpui::StyledText::new(moonlight_instruction).with_highlights(vec![(
            links_start..links_start + "App Store / Google Play".len(),
            gpui::HighlightStyle {
                color: Some(rgb(0x74d2c8).into()),
                ..Default::default()
            },
        )]);
        let can_pair = ready && status.is_some_and(crate::streaming::StreamingStatus::can_pair);
        let pin = div()
            .w(px(110.0))
            .rounded(px(8.0))
            .border_1()
            .border_color(rgba(0xffffff24))
            .bg(rgba(0xffffff0a))
            .children(live.as_ref().map(|(_, _, pin, _)| pin.clone()))
            .children(live.is_none().then(|| {
                div()
                    .py(px(8.))
                    .px(px(12.))
                    .text_size(px(16.))
                    .text_color(rgb(0x9aa09e))
                    .child("P I N")
            }));
        let mut pairing = card().opacity(if running { 1.0 } else { 0.55 })
            .child(div().mb(px(10.0)).child(title("Pair your device")))
            .child(div().flex().flex_col().gap(px(5.0)).mb(px(14.0)).line_height(px(20.6))
                .child(div().child(moonlight_line))
                .children(live.is_some().then(||div().flex().gap(px(9.)).child(button("stream-app-store","↗ App Store",false).on_click(cx.listener(|_,_,_,cx|cx.open_url("https://apps.apple.com/app/moonlight-game-streaming/id1000551566")))).child(button("stream-play-store","↗ Google Play",false).on_click(cx.listener(|_,_,_,cx|cx.open_url("https://play.google.com/store/apps/details?id=com.limelight"))))))
                .child("2. Start playing your game in MetalSharp on this Mac.")
                .child("3. Open Moonlight and tap this Mac — it shows a 4-digit PIN.")
                .child("4. Enter the PIN below to pair, then tap the game in Moonlight to start streaming."))
            .child(div().flex().gap(px(9.0))
                .child(pin)
                .child(button("stream-pair", "Pair Device", true).opacity(if can_pair {1.0}else{0.5}).on_click(cx.listener(|this,_,_,cx|{if let Some(session)=this.connected.clone(){session.update(cx,|session,cx|session.streaming_pair(cx));}}))));
        // The preview never accepts a real pairing PIN or transmits credentials.
        if running && live.is_none() {
            pairing = pairing.child(
                div()
                    .mt(px(10.0))
                    .text_size(px(11.0))
                    .child("Pairing is disabled in the isolated preview."),
            );
        }
        if let Some(status) = status {
            pairing = pairing.child(div().mt(px(10.)).child(
                if status.running && !status.creds_valid {
                    "Host is starting — pairing credentials are not ready yet".into()
                } else {
                    format!(
                        "{} waiting to pair: {}",
                        status.pairing_count, status.pairings_summary
                    )
                },
            ));
        }
        let mut notes = card().child(div().mb(px(10.0)).child(title("Good to know")))
            .child(div().flex().flex_col().gap(px(5.0)).text_size(px(12.0)).line_height(px(20.4))
                .child("• Sunshine on macOS is experimental: gamepads aren't supported yet — use touch controls in Moonlight.")
                .child("• macOS asks for Screen Recording permission the first time you stream. Approve it once.")
                .child("• Keep both devices on the same network; ports 47984–48010 must be reachable."));
        if installed {
            notes = notes.child(
                button(
                    "stream-unpair",
                    if self.streaming_unpair_confirm {
                        "Confirm — unpair ALL devices"
                    } else {
                        "Unpair all devices"
                    },
                    false,
                )
                .mt(px(12.0))
                .text_color(rgb(0xff9d9d))
                .border_color(rgba(0xff7a7a4d))
                .on_click(cx.listener(|this, _, _, cx| {
                    if let Some(session) = this.connected.clone() {
                        let (status, busy, _, _) = session.read(cx).streaming_view();
                        if !busy && status.is_some_and(|status| status.can_pair()) {
                            if this.streaming_unpair_confirm
                                && this
                                    .streaming_unpair_deadline
                                    .is_some_and(|deadline| std::time::Instant::now() < deadline)
                            {
                                this.streaming_unpair_confirm = false;
                                session.update(cx, |session, cx| {
                                    session.streaming_command(
                                        crate::streaming::StreamingAction::UnpairAll,
                                        cx,
                                    )
                                });
                            } else {
                                this.streaming_unpair_confirm = true;
                                this.streaming_unpair_deadline = Some(
                                    std::time::Instant::now() + std::time::Duration::from_secs(10),
                                );
                            }
                        }
                    }
                    cx.notify();
                })),
            );
            if self.streaming_unpair_confirm {
                notes = notes.child(
                    button("stream-unpair-cancel", "Cancel unpair", false)
                        .mt(px(8.))
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.streaming_unpair_confirm = false;
                            cx.notify();
                        })),
                );
            }
        }
        if let Some((_, _, _, notice)) = &live {
            notes = notes.child(div().mt(px(10.)).child(notice.clone()));
        }
        div().id("streaming-overlay").occlude().absolute().top(px(0.0)).left(px(0.0))
            .size_full().flex().items_center().justify_center().p(px(24.0)).bg(rgba(0x040608b8))
            .on_click(cx.listener(|this, _, _, cx| {this.close_streaming_panel(cx);}))
            .child(div().id("streaming-panel").occlude().w(px(680.0)).max_w_full()
                .h(px((f32::from(viewport.height) * 0.86).min(680.0)))
                .flex().flex_col().rounded(px(16.0)).overflow_hidden()
                .border_1().border_color(rgba(0xffffff17)).bg(rgb(0x14171a)).shadow_lg()
                .text_size(px(12.5))
                .on_click(|_, _, cx| cx.stop_propagation())
                .child(div().flex_none().flex().items_center().justify_between().gap(px(14.0))
                    .py(px(18.0)).px(px(22.0)).border_b_1().border_color(rgba(0xffffff12))
                    .child(div().flex().items_center().gap(px(13.0))
                        .child(div().flex_none().size(px(40.0)).flex().items_center().justify_center()
                            .rounded(px(11.0)).border_1().border_color(rgba(0x74d2c859)).bg(rgba(0x74d2c814))
                            .child(gpui::svg().path("stream-tv.svg").size(px(20.0)).text_color(rgb(0x74d2c8))))
                        .child(div().min_w_0().flex().flex_col().gap(px(2.0))
                            .child(div().text_size(px(18.0)).font_weight(FontWeight::SEMIBOLD).text_color(rgb(0xeef0ef)).child("Game Streaming"))
                            .child(div().text_size(px(12.0)).text_color(rgb(0x989e9c))
                                .child("Stream your MetalSharp games to a phone or tablet with Sunshine + Moonlight"))))
                    .child(div().id("stream-close").flex_none().size(px(30.0)).rounded(px(8.0))
                        .border_1().border_color(rgba(0xffffff24)).flex().items_center().justify_center()
                        .text_size(px(20.0)).text_color(rgba(0xffffff99)).cursor_pointer().child("×")
                        .on_click(cx.listener(|this, _, _, cx| {this.close_streaming_panel(cx);}))))
                .child(div().id("streaming-body").min_h_0().flex_1().overflow_y_scroll()
                    .flex().flex_col().gap(px(14.0)).pt(px(18.0)).px(px(22.0)).pb(px(22.0))
                    .child(host).child(pairing).child(notes)
                    .child(div().flex_none().text_size(px(10.0)).text_color(rgb(0x7d8381))
                        .child(if live.is_some(){"CONNECTED · Host actions affect Sunshine on this Mac only when explicitly requested."}else{"LOCAL PREVIEW · Install/Start/Stop are simulated. No downloads, networking, or pairing."}))))
    }

    fn log_preview_event(&self, message: String, cx: &mut Context<Self>) {
        if let Some(logs) = &self.logs_preview {
            logs.update(cx, |logs, cx| {
                logs.append_preview_event(message);
                cx.notify();
            });
        }
    }

    fn matching_preview_games(&self) -> Vec<usize> {
        let query = self.search_query.trim().to_lowercase();
        LIBRARY_GAMES
            .iter()
            .enumerate()
            .filter_map(|(index, (name, _, _))| {
                name.to_lowercase().contains(&query).then_some(index)
            })
            .collect()
    }

    fn select_preview_game(&mut self, index: usize) {
        self.selected_game = index.min(LIBRARY_GAMES.len() - 1);
        let matches = self.matching_preview_games();
        if let Some(position) = matches.iter().position(|game| *game == self.selected_game) {
            if position < self.dock_start {
                self.dock_start = position;
            } else if position >= self.dock_start + DOCK_VISIBLE_CARDS {
                self.dock_start = position + 1 - DOCK_VISIBLE_CARDS;
            }
        }
        self.dock_start = self
            .dock_start
            .min(matches.len().saturating_sub(DOCK_VISIBLE_CARDS));
        self.hovered_card = None;
        self.close_library_menus();
    }

    fn scroll_preview_dock(&mut self, direction: i32) {
        let matches = self.matching_preview_games();
        let last_start = matches.len().saturating_sub(DOCK_VISIBLE_CARDS);
        let next_start = if direction > 0 {
            (self.dock_start + 1).min(last_start)
        } else {
            self.dock_start.saturating_sub(1)
        };
        if next_start == self.dock_start {
            return;
        }
        self.dock_start = next_start;
        let position = matches
            .iter()
            .position(|index| *index == self.selected_game)
            .unwrap_or(next_start);
        let next_position = if direction > 0 {
            position + 1
        } else {
            position.saturating_sub(1)
        };
        let next_position = next_position.clamp(next_start, next_start + DOCK_VISIBLE_CARDS - 1);
        self.select_preview_game(matches[next_position]);
    }

    fn open_settings_preview(&mut self, window: &mut gpui::Window, cx: &mut Context<Self>) {
        self.close_library_menus();
        self.streaming_open = false;
        if self.settings_preview.is_none() {
            let settings = cx.new(|_| crate::settings_preview::SettingsPreview::new());
            cx.subscribe(&settings, |this, _, event, cx| {
                use crate::settings_preview::SettingsPreviewEvent;
                match event {
                    SettingsPreviewEvent::Close => this.settings_open = false,
                    SettingsPreviewEvent::ReopenSetup => {
                        this.settings_open = false;
                        this.show_setup = true;
                        this.step = 0;
                    }
                    SettingsPreviewEvent::LanguageChanged(code) => {
                        this.selected_language = *code;
                        if let Some(copy) = this.locales.get(*code) {
                            this.copy = copy.clone();
                        }
                    }
                }
                cx.notify();
            })
            .detach();
            self.settings_preview = Some(settings);
        }
        let palette = self.theme.page_palette();
        let language = self.selected_language;
        self.settings_preview
            .as_ref()
            .unwrap()
            .update(cx, |settings, cx| {
                settings.palette = palette;
                settings.language = language;
                settings.open(window, cx);
            });
        self.settings_open = true;
        cx.notify();
    }

    fn close_library_menus(&mut self) {
        self.theme_menu_open = false;
        self.tab_menu_open = false;
        self.launcher_menu_open = false;
        self.steam_options_open = false;
        self.settings_open = false;
        self.game_settings_open = false;
        self.pipeline_menu_open = false;
    }

    fn render_pipeline_menu(&self, cx: &mut Context<Self>) -> gpui::Stateful<gpui::Div> {
        let game = self.selected_game;
        let mut menu = div()
            .id("pipeline-menu")
            .occlude()
            .absolute()
            .top(px(50.0))
            .right(px(0.0))
            .w(px(166.0))
            .flex()
            .flex_col()
            .gap(px(3.0))
            .p(px(6.0))
            .rounded(px(8.0))
            .border_1()
            .border_color(rgb(self.theme.button_border()))
            .bg(rgb(self.theme.menu_bg()))
            .shadow_lg()
            .text_size(px(12.0))
            .text_color(rgb(self.theme.control_text()));
        for (index, label) in PREVIEW_PIPELINES.into_iter().enumerate() {
            menu = menu.child(
                div()
                    .id(("pipeline-option", index))
                    .h(px(32.0))
                    .px(px(9.0))
                    .flex()
                    .items_center()
                    .justify_between()
                    .rounded(px(5.0))
                    .bg(rgb(if self.game_preferences[game].pipeline == index {
                        self.theme.menu_hover()
                    } else {
                        self.theme.menu_bg()
                    }))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(self.theme.menu_hover())))
                    .child(label)
                    .child(if self.game_preferences[game].pipeline == index {
                        "✓"
                    } else {
                        ""
                    })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.game_preferences[game].pipeline = index;
                        this.pipeline_menu_open = false;
                        cx.notify();
                    })),
            );
        }
        menu
    }

    fn render_game_settings(&self, cx: &mut Context<Self>) -> gpui::Stateful<gpui::Div> {
        let game = self.selected_game;
        let prefs = &self.game_preferences[game];
        let mut menu = div()
            .id("game-settings-popover")
            .occlude()
            .absolute()
            .top(px(50.0))
            .right(px(0.0))
            .w(px(365.0))
            .flex()
            .flex_col()
            .gap(px(12.0))
            .rounded(px(10.0))
            .border_1()
            .border_color(rgb(self.theme.button_border()))
            .bg(rgb(self.theme.menu_bg()))
            .shadow_lg()
            .p(px(14.0))
            .text_size(px(12.0))
            .text_color(rgb(self.theme.control_text()))
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .child(
                                div()
                                    .text_size(px(10.0))
                                    .text_color(rgb(if self.theme == PreviewTheme::Light {
                                        0x000000
                                    } else {
                                        0x939a9b
                                    }))
                                    .child("GAME SETTINGS"),
                            )
                            .child(LIBRARY_GAMES[game].0),
                    )
                    .child(
                        div()
                            .id("close-game-settings")
                            .cursor_pointer()
                            .px(px(8.0))
                            .child("×")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.game_settings_open = false;
                                cx.notify();
                            })),
                    ),
            );
        for (category, label, choices, current) in [
            (
                0usize,
                "MetalFX",
                vec!["1.75×", "2×", "Off"],
                prefs.metal_fx,
            ),
            (
                1usize,
                "Controller input",
                vec!["Off", "XInput", "DInput"],
                prefs.controller,
            ),
            (2usize, "Msync", vec!["Off", "On"], usize::from(prefs.msync)),
        ] {
            let mut options = div().flex().gap(px(4.0));
            for (choice, text) in choices.into_iter().enumerate() {
                options = options.child(
                    div()
                        .id(("game-setting-option", category * 3 + choice))
                        .px(px(8.0))
                        .py(px(6.0))
                        .rounded(px(5.0))
                        .border_1()
                        .border_color(rgb(if current == choice {
                            self.theme.button_border()
                        } else {
                            self.theme.border()
                        }))
                        .bg(rgb(if current == choice {
                            self.theme.menu_hover()
                        } else {
                            self.theme.menu_bg()
                        }))
                        .cursor_pointer()
                        .child(text)
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let prefs = &mut this.game_preferences[game];
                            match category {
                                0 => prefs.metal_fx = choice,
                                1 => prefs.controller = choice,
                                _ => prefs.msync = choice == 1,
                            }
                            cx.notify();
                        })),
                );
            }
            menu = menu.child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap(px(10.0))
                    .child(label)
                    .child(options),
            );
        }
        let executable_name = prefs
            .executable
            .as_deref()
            .and_then(|path| std::path::Path::new(path).file_name())
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| "Automatically detected".to_string());
        menu.child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .gap(px(12.0))
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .overflow_hidden()
                        .flex()
                        .flex_col()
                        .gap(px(4.0))
                        .child("Launch executable")
                        .child(
                            div()
                                .text_size(px(10.0))
                                .text_color(rgb(if self.theme == PreviewTheme::Light {
                                    0x000000
                                } else {
                                    0x939a9b
                                }))
                                .child(executable_name),
                        ),
                )
                .child(
                    div()
                        .id("choose-game-executable")
                        .flex_none()
                        .cursor_pointer()
                        .px(px(9.0))
                        .py(px(6.0))
                        .rounded(px(5.0))
                        .border_1()
                        .border_color(rgb(self.theme.border()))
                        .bg(rgb(self.theme.menu_bg()))
                        .child("Choose EXE")
                        .on_click(cx.listener(move |_, _, _, cx| {
                            // Store only the chosen path in preview memory; never execute it or save to the backend.
                            cx.spawn(async move |this, cx| {
                                let selected = rfd::AsyncFileDialog::new()
                                    .set_title("Choose launch executable — local preview only")
                                    .add_filter("Windows executable", &["exe"])
                                    .pick_file()
                                    .await;
                                if let Some(file) = selected {
                                    let path = file.path().to_string_lossy().into_owned();
                                    let _ = this.update(cx, |this, cx| {
                                        this.game_preferences[game].executable = Some(path);
                                        cx.notify();
                                    });
                                }
                            })
                            .detach();
                        })),
                ),
        )
        .child(
            div()
                .text_size(px(10.0))
                .text_color(rgb(if self.theme == PreviewTheme::Light {
                    0x000000
                } else {
                    0x939a9b
                }))
                .child("Preview only · settings kept in memory, executable never launched"),
        )
    }

    fn render_library_collection(
        &mut self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let (_, card_width) = collection_card_layout(f32::from(viewport.width));
        let theme = self.theme;
        let accent = theme.accent();
        let control_bg = theme.control_bg();
        let mut grid = div()
            .w_full()
            .min_w_0()
            .flex_none()
            .flex()
            .flex_wrap()
            .gap(px(18.0))
            .max_w(px(1400.0));
        let matches = self.matching_preview_games();
        for index in matches.iter().copied() {
            let (name, cover, _) = LIBRARY_GAMES[index];
            grid = grid.child(
                div()
                    .id(("collection-card", index))
                    .relative()
                    .w(px(card_width))
                    .h(px(card_width * 1.36))
                    .flex_none()
                    .overflow_hidden()
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(rgba(0xe0e2e03b))
                    .bg(rgb(0x242729))
                    .shadow_lg()
                    .child(
                        img(asset_path(cover))
                            .w_full()
                            .h_full()
                            .object_fit(ObjectFit::Cover),
                    )
                    .child(
                        div()
                            .absolute()
                            .top(px(0.0))
                            .right(px(0.0))
                            .bottom(px(0.0))
                            .left(px(0.0))
                            .bg(linear_gradient(
                                180.0,
                                linear_color_stop(rgba(0x00000000), 0.34),
                                linear_color_stop(rgba(0x050708e6), 1.0),
                            )),
                    )
                    .child(
                        div()
                            .absolute()
                            .left(px(12.0))
                            .right(px(12.0))
                            .bottom(px(12.0))
                            .flex()
                            .flex_col()
                            .gap(px(7.0))
                            .child(
                                div()
                                    .text_size(px(14.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(0xf0f0ed))
                                    .child(name),
                            )
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(6.0))
                                    .child(
                                        div()
                                            .h(px(24.0))
                                            .px(px(7.0))
                                            .flex()
                                            .items_center()
                                            .rounded(px(5.0))
                                            .border_1()
                                            .border_color(rgb(theme.border()))
                                            .bg(rgba(control_bg << 8 | 0xd8))
                                            .text_size(px(10.5))
                                            .font_weight(FontWeight::SEMIBOLD)
                                            .text_color(rgb(self.theme.control_text()))
                                            .child("D3DMetal  ⌄"),
                                    )
                                    .child(
                                        div()
                                            .id(("collection-play", index))
                                            .h(px(28.0))
                                            .px(px(11.0))
                                            .flex()
                                            .items_center()
                                            .gap(px(5.0))
                                            .rounded(px(6.0))
                                            .border_1()
                                            .border_color(rgb(accent))
                                            .bg(rgb(accent))
                                            .text_size(px(11.0))
                                            .font_weight(FontWeight::BOLD)
                                            .text_color(rgb(control_bg))
                                            .cursor_pointer()
                                            .on_click(cx.listener(move |this, _, _, cx| {
                                                this.select_preview_game(index);
                                                this.active_tab = LibraryTab::Play;
                                                this.running_game = Some(index);
                                                this.log_preview_event(format!("[PREVIEW] [LAUNCHED] {name} · simulated collection action; no process launched"), cx);
                                                cx.notify();
                                            }))
                                            .child("▶ Play"),
                                    ),
                            ),
                    ),
            );
        }

        div()
            .id("library-collection-scroll")
            .flex_1()
            .min_h_0()
            .flex()
            .flex_col()
            .overflow_y_scroll()
            .bg(rgb(0x111416))
            .px(px(32.0))
            .pt(px(36.0))
            .pb(px(60.0))
            .child(
                div()
                    .id("collection-heading")
                    .relative()
                    .pr(px(160.0))
                    .w_full()
                    .flex_none()
                    .flex()
                    .flex_col()
                    .items_start()
                    .bg(rgb(0x111416))
                    .mb(px(30.0))
                    .child(
                        div()
                            .mb(px(12.0))
                            .text_size(px(11.0))
                            .text_color(rgb(0xb9b0a1))
                            .child("STEAM COLLECTION"),
                    )
                    .child(
                        div()
                            .text_size(px(54.0))
                            .line_height(px(54.0))
                            .font_family("Georgia")
                            .text_color(rgb(0xeee9dd))
                            .child("Installed games"),
                    )
                    .child(
                        div()
                            .mt(px(13.0))
                            .text_size(px(14.0))
                            .text_color(rgb(0xaeb3b2))
                            .child(format!(
                                "{} preview games ready in your MetalSharp library.",
                                matches.len()
                            )),
                    )
                    .child(
                        div()
                            .id("collection-back-to-play")
                            .absolute()
                            .right(px(0.0))
                            .top(px(34.0))
                            .h(px(38.0))
                            .flex()
                            .items_center()
                            .gap(px(8.0))
                            .px(px(14.0))
                            .rounded(px(8.0))
                            .border_1()
                            .border_color(rgba(0xe7eaec3b))
                            .bg(rgb(0x292d2f))
                            .text_size(px(13.0))
                            .text_color(rgb(0xe1e3e2))
                            .cursor_pointer()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.active_tab = LibraryTab::Play;
                                cx.notify();
                            }))
                            .child("⌂  Back to Play"),
                    ),
            )
            .child(grid)
    }

    fn render_setup_page(&mut self, cx: &mut Context<Self>) -> gpui::Div {
        let selected_language = self.selected_language;
        let copy = self.copy.clone();
        let current_step = self.step.min(2);
        let language_menu_open = self.language_menu_open;
        let selected_name = LANGUAGES
            .iter()
            .find(|(code, _)| *code == selected_language)
            .map(|(_, name)| *name)
            .unwrap_or("English");
        let mut language_picker = div()
            .absolute()
            .top(px(18.0))
            .right(px(18.0))
            .flex()
            .flex_col()
            .items_end()
            .child(
                div()
                    .id("language-picker-toggle")
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(rgb(0x343638))
                    .bg(rgb(0x1a1d20))
                    .px(px(11.0))
                    .py(px(7.0))
                    .text_size(px(11.0))
                    .text_color(rgb(0xc5c5c1))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(0x25282b)))
                    .on_click(cx.listener(|this, _: &ClickEvent, _, cx| {
                        this.language_menu_open = !this.language_menu_open;
                        cx.notify();
                    }))
                    .child("◎")
                    .child(copy.language_label.clone())
                    .child(selected_name)
                    .child(if language_menu_open { "⌃" } else { "⌄" }),
            );

        if language_menu_open {
            let mut menu = div()
                .id("language-menu")
                .mt(px(6.0))
                .w(px(194.0))
                .max_h(px(310.0))
                .overflow_y_scroll()
                .rounded(px(8.0))
                .border_1()
                .border_color(rgb(0x343638))
                .bg(rgb(0x101316))
                .shadow_lg()
                .p(px(4.0));
            for (index, (code, native_name)) in LANGUAGES.iter().copied().enumerate() {
                let is_selected = code == selected_language;
                menu = menu.child(
                    div()
                        .id(("language-option", index))
                        .w_full()
                        .rounded(px(5.0))
                        .px(px(10.0))
                        .py(px(8.0))
                        .text_size(px(12.0))
                        .text_color(rgb(if is_selected { 0xefe7d6 } else { 0xd3d3cf }))
                        .bg(rgb(if is_selected { 0x2b2d2e } else { 0x101316 }))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(0x303336)))
                        .on_click(cx.listener(move |this, _: &ClickEvent, _, cx| {
                            this.selected_language = code;
                            if let Some(copy) = this.locales.get(code).cloned() {
                                this.copy = copy;
                            }
                            this.language_menu_open = false;
                            cx.notify();
                        }))
                        .child(native_name),
                );
            }
            language_picker = language_picker.child(menu);
        }

        let title = copy.titles[current_step].clone();
        let tagline = copy.taglines[current_step].clone();
        let step_label = copy
            .step_of
            .replace("{step}", &(current_step + 1).to_string())
            .replace("{total}", &copy.steps.len().to_string());
        let can_advance = self.setup_can_advance(cx);
        let page_body = match current_step {
            0 => render_welcome_body(&copy),
            1 => self.render_runtime_body(cx, copy.clone()),
            _ => render_done_body(
                &copy,
                self.connected
                    .as_ref()
                    .map(|session| session.read(cx).setup_inputs()),
                self.connected.clone(),
            ),
        };
        let page_actions = if current_step == 0 {
            div().mt_auto().flex().justify_end().child(
                div()
                    .id("get-started")
                    .flex()
                    .items_center()
                    .justify_center()
                    .gap(px(10.0))
                    .h(px(52.0))
                    .px(px(32.0))
                    .rounded(px(10.0))
                    .bg(rgb(0xefe7d6))
                    .text_size(px(16.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(0x14161a))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(0xf7efdf)))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.step = 1;
                        cx.notify();
                    }))
                    .child("▶")
                    .child(copy.get_started.clone()),
            )
        } else if current_step == 1 {
            div()
                .mt_auto()
                .flex()
                .items_center()
                .justify_between()
                .gap(px(10.0))
                .child(
                    div()
                        .id("setup-back")
                        .flex()
                        .items_center()
                        .justify_center()
                        .h(px(44.0))
                        .px(px(18.0))
                        .rounded(px(8.0))
                        .border_1()
                        .border_color(rgb(0x414345))
                        .text_size(px(14.0))
                        .text_color(rgb(0xd8d5cc))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(0x222528)))
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.step = 0;
                            cx.notify();
                        }))
                        .child(copy.back.clone()),
                )
                .child(
                    div()
                        .id("setup-next")
                        .opacity(if can_advance { 1.0 } else { 0.4 })
                        .flex()
                        .items_center()
                        .justify_center()
                        .h(px(52.0))
                        .px(px(32.0))
                        .rounded(px(10.0))
                        .bg(rgb(0xefe7d6))
                        .text_size(px(16.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(rgb(0x14161a))
                        .cursor_pointer()
                        .hover(|style| style.bg(rgb(0xf7efdf)))
                        .on_click(cx.listener(|this, _, _, cx| {
                            if this.setup_can_advance(cx) {
                                this.step = 2;
                            }
                            cx.notify();
                        }))
                        .child(copy.next.clone()),
                )
        } else {
            div().mt_auto().flex().justify_end().child(
                div()
                    .id("setup-launch")
                    .opacity(if can_advance { 1.0 } else { 0.4 })
                    .flex()
                    .items_center()
                    .justify_center()
                    .h(px(52.0))
                    .px(px(32.0))
                    .rounded(px(10.0))
                    .bg(rgb(0xefe7d6))
                    .text_size(px(16.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(0x14161a))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(0xf7efdf)))
                    .on_click(cx.listener(|this, _, _, cx| {
                        if let Some(session) = this.connected.clone() {
                            session.update(cx, |session, cx| session.setup_finish(cx));
                        } else {
                            this.show_setup = false;
                        }
                        cx.notify();
                    }))
                    .child(copy.launch.clone()),
            )
        };

        div()
            .relative()
            .w(relative(0.48))
            .flex_none()
            .min_w_0()
            .h_full()
            .flex()
            .flex_col()
            .border_l_1()
            .border_color(rgb(0x242628))
            .bg(linear_gradient(
                180.0,
                linear_color_stop(rgb(0x171a1e), 0.0),
                linear_color_stop(rgb(0x131518), 1.0),
            ))
            .px(px(49.0))
            .pt(px(49.0))
            .pb(px(28.0))
            .child(
                div()
                    .text_size(px(11.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(0x999994))
                    .child(step_label),
            )
            .child(
                div()
                    .mt(px(18.0))
                    .text_size(px(37.0))
                    .line_height(px(40.0))
                    .font_family("Georgia")
                    .font_weight(FontWeight::NORMAL)
                    .text_color(rgb(TEXT))
                    .child(title),
            )
            .child(
                div()
                    .mt(px(12.0))
                    .text_size(px(18.0))
                    .text_color(rgb(0xe9e7e0))
                    .child(tagline),
            )
            .child(
                div()
                    .id("setup-content-scroll")
                    .flex_1()
                    .min_h_0()
                    .overflow_y_scroll()
                    .child(page_body),
            )
            .child(if let Some(session)=&self.connected {
                let state=session.read(cx).setup_view();
                div().mt(px(12.)).text_size(px(11.)).text_color(rgb(MUTED))
                    .child("Connected setup: install buttons perform real operations; no sample library will be shown.")
                    .child(div().id("setup-connection-notice").max_h(px(60.)).overflow_y_scroll().child(state.notice))
                    .child(div().id("setup-backend-retry").mt(px(6.)).cursor_pointer().child("Restart owned backend / retry connection").on_click(cx.listener(|this,_,_,cx|{
                        if let Some(session)=this.connected.clone(){session.update(cx,|session,cx|session.setup_retry_backend(cx));}
                    })))
            }else{div()})
            .child(page_actions)
            .child(language_picker)
    }

    fn render_runtime_body(&mut self, cx: &mut Context<Self>, copy: SetupCopy) -> gpui::Div {
        let runtime_ready = self.runtime_installed;
        let runtime_installing = self.runtime_installing;
        let runtime_progress = self.runtime_progress;
        let runtime_started = self.runtime_started;
        let install_log_open = self.install_log_open;
        let steam_installed = self.steam_installed;
        let steam_installing = self.steam_installing;
        let runtime_label = if runtime_ready {
            copy.install_complete.clone()
        } else if runtime_installing {
            copy.preparing.clone()
        } else {
            copy.install_runtime.clone()
        };
        let steam_label = if steam_installed {
            copy.steam_installed.clone()
        } else if steam_installing {
            copy.installing_steam.clone()
        } else {
            copy.install_steam.clone()
        };

        div()
            .mt(px(8.0))
            .flex()
            .flex_col()
            .child(
                div()
                    .mt(px(18.0))
                    .mb(px(22.0))
                    .text_size(px(13.5))
                    .line_height(px(20.0))
                    .text_color(rgb(MUTED))
                    .child(copy.runtime_lede.clone()),
            )
            .child(
                div()
                    .mt(px(6.0))
                    .mb(px(10.0))
                    .text_size(px(10.5))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(0x999994))
                    .child(copy.bundled_tools.clone()),
            )
            .child(render_tool_row(
                "zstd / unzstd",
                copy.tool_extraction.clone(),
                false,
            ))
            .child(render_tool_row("unrar", copy.tool_rar.clone(), true))
            .child(render_tool_row(
                "wrestool / icotool",
                copy.tool_icons.clone(),
                true,
            ))
            .child(render_tool_row(
                "lsar / unar",
                copy.tool_archives.clone(),
                true,
            ))
            .child(
                div()
                    .mt(px(18.0))
                    .flex()
                    .items_start()
                    .gap(px(12.0))
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(10.0))
                            .child(
                                render_install_button(
                                    "install-runtime",
                                    runtime_label,
                                    runtime_installing,
                                    runtime_ready,
                                    runtime_progress,
                                )
                                .on_click(cx.listener(
                                    |this, _, _, cx| {
                                        this.start_runtime_install(cx);
                                    },
                                )),
                            )
                            .child(if runtime_started {
                                div()
                                    .id("install-log-toggle")
                                    .h(px(38.0))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .gap(px(7.0))
                                    .rounded(px(8.0))
                                    .border_1()
                                    .border_color(rgb(if install_log_open {
                                        0xefe6d3
                                    } else {
                                        0x414345
                                    }))
                                    .text_size(px(12.0))
                                    .text_color(rgb(if install_log_open {
                                        0xefe6d3
                                    } else {
                                        0xcfd2cf
                                    }))
                                    .cursor_pointer()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.install_log_open = !this.install_log_open;
                                        cx.notify();
                                    }))
                                    .child("▤")
                                    .child(copy.install_log.clone())
                            } else {
                                div().id("install-log-placeholder").h(px(0.0))
                            }),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(10.0))
                            .child(
                                render_simple_install_button(
                                    "install-steam",
                                    steam_label,
                                    steam_installed,
                                    !runtime_ready || steam_installing || steam_installed,
                                )
                                .on_click(cx.listener(
                                    |this, _, _, cx| {
                                        this.start_steam_install(cx);
                                    },
                                )),
                            )
                            .child(if !steam_installed {
                                div()
                                    .text_size(px(12.5))
                                    .text_color(rgb(MUTED))
                                    .child(copy.start_steam_hint.clone())
                            } else {
                                div().h(px(15.0))
                            }),
                    ),
            )
            .child(if install_log_open {
                div()
                    .id("setup-install-log")
                    .mt(px(14.0))
                    .max_h(px(110.0))
                    .overflow_y_scroll()
                    .rounded(px(10.0))
                    .border_1()
                    .border_color(rgb(0x292b2d))
                    .bg(rgb(0x0c0f11))
                    .px(px(14.0))
                    .py(px(12.0))
                    .text_size(px(11.5))
                    .line_height(px(18.0))
                    .font_family("SF Mono")
                    .text_color(rgb(if runtime_ready { 0x7cbf6a } else { 0xefe6d3 }))
                    .child(if runtime_ready {
                        copy.install_complete.clone()
                    } else {
                        copy.install_runtime.clone()
                    })
            } else {
                div().id("setup-install-log-placeholder").h(px(0.0))
            })
    }

    fn start_runtime_install(&mut self, cx: &mut Context<Self>) {
        if let Some(session) = self.connected.clone() {
            session.update(cx, |session, cx| session.setup_install_runtime(cx));
            return;
        }
        if self.runtime_installing || self.runtime_installed {
            return;
        }
        // This page is a UI preview: installation progress is simulated and
        // never invokes the real runtime installer or modifies user data.
        self.runtime_installing = true;
        self.runtime_started = true;
        self.runtime_progress = 0;
        cx.notify();
        cx.spawn(async move |this, cx| {
            for progress in [12, 27, 46, 68, 84, 100] {
                cx.background_executor()
                    .timer(std::time::Duration::from_millis(450))
                    .await;
                let _ = this.update(cx, |this, cx| {
                    this.runtime_progress = progress;
                    cx.notify();
                });
            }
            let _ = this.update(cx, |this, cx| {
                this.runtime_installing = false;
                this.runtime_installed = true;
                cx.notify();
            });
        })
        .detach();
    }

    fn start_steam_install(&mut self, cx: &mut Context<Self>) {
        if let Some(session) = self.connected.clone() {
            session.update(cx, |session, cx| session.setup_install_steam(cx));
            return;
        }
        if !self.runtime_installed || self.steam_installing || self.steam_installed {
            return;
        }
        // Steam is likewise simulated; the preview never downloads or runs it.
        self.steam_installing = true;
        cx.notify();
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .timer(std::time::Duration::from_secs(2))
                .await;
            let _ = this.update(cx, |this, cx| {
                this.steam_installing = false;
                this.steam_installed = true;
                cx.notify();
            });
        })
        .detach();
    }
}

fn render_welcome_body(copy: &SetupCopy) -> gpui::Div {
    div()
        .mt(px(8.0))
        .flex()
        .flex_col()
        .child(
            div()
                .mt(px(18.0))
                .text_size(px(13.5))
                .line_height(px(20.0))
                .text_color(rgb(MUTED))
                .child(copy.lede.clone()),
        )
        .child(
            div()
                .mt(px(14.0))
                .flex()
                .flex_col()
                .child(render_feature(
                    "ϟ",
                    copy.directx.clone(),
                    copy.directx_desc.clone(),
                    false,
                ))
                .child(render_feature(
                    "▤",
                    copy.fna.clone(),
                    copy.fna_desc.clone(),
                    true,
                ))
                .child(render_feature(
                    "⌘",
                    copy.steam.clone(),
                    copy.steam_desc.clone(),
                    true,
                )),
        )
}

fn render_done_body(
    copy: &SetupCopy,
    fields: Option<[gpui::Entity<crate::search_input::SearchInput>; 3]>,
    session: Option<gpui::Entity<crate::connected::ConnectedApp>>,
) -> gpui::Div {
    div()
        .mt(px(8.0))
        .flex()
        .flex_col()
        .child(
            div()
                .size(px(54.0))
                .mb(px(18.0))
                .flex()
                .items_center()
                .justify_center()
                .rounded_full()
                .border_1()
                .border_color(rgb(0x4a7044))
                .bg(rgb(0x17221a))
                .text_size(px(30.0))
                .text_color(rgb(0x7cbf6a))
                .child("✓"),
        )
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(16.0))
                .mt(px(20.0))
                .child(render_done_form_group(
                    copy.device_name.clone(),
                    copy.device_placeholder.clone(),
                    copy.device_hint.clone(),
                    "device-name",
                    fields.as_ref().map(|fields| fields[0].clone()),
                    None,
                ))
                .child(render_done_form_group(
                    copy.api_key.clone(),
                    copy.api_placeholder.clone(),
                    format!("{} steamcommunity.com/dev/apikey", copy.api_hint),
                    "steam-api-key",
                    fields.as_ref().map(|fields| fields[1].clone()),
                    session.as_ref().map(|session| (session.clone(), false)),
                ))
                .child(render_done_form_group(
                    copy.the_games_db_api_key.clone(),
                    copy.the_games_db_api_placeholder.clone(),
                    format!("{} api.thegamesdb.net/key.php", copy.the_games_db_api_hint),
                    "thegamesdb-api-key",
                    fields.as_ref().map(|fields| fields[2].clone()),
                    session.map(|session| (session, true)),
                )),
        )
}

fn render_done_form_group(
    label: String,
    placeholder: String,
    hint: String,
    id: &'static str,
    input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    help: Option<(gpui::Entity<crate::connected::ConnectedApp>, bool)>,
) -> gpui::Div {
    let field = if let Some(input) = input {
        div().w_full().child(input)
    } else {
        div().child(placeholder)
    };
    div()
        .flex()
        .flex_col()
        .gap(px(6.0))
        .child(
            div()
                .text_size(px(12.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xcfd2cf))
                .child(label),
        )
        .child(
            div()
                .id(id)
                .h(px(40.0))
                .flex()
                .items_center()
                .rounded(px(8.0))
                .border_1()
                .border_color(rgb(0x3b3e40))
                .bg(rgb(0x191c1f))
                .px(px(12.0))
                .text_size(px(13.0))
                .text_color(rgb(0x777d7a))
                .child(field),
        )
        .child(
            div()
                .id(gpui::SharedString::from(format!("{id}-hint")))
                .text_size(px(11.5))
                .text_color(rgb(0x838987))
                .when(help.is_some(), |style| style.cursor_pointer().underline())
                .on_click(move |_, _, cx| {
                    if let Some((session, gamesdb)) = &help {
                        session.update(cx, |session, cx| session.setup_key_help(*gamesdb, cx));
                    }
                })
                .child(hint),
        )
}

fn render_tool_row(title: &'static str, description: String, divider: bool) -> gpui::Div {
    let row = div()
        .flex()
        .items_center()
        .justify_between()
        .gap(px(16.0))
        .py(px(15.0))
        .child(
            div()
                .text_size(px(14.0))
                .text_color(rgb(0xeceae3))
                .child(title),
        )
        .child(
            div()
                .text_size(px(12.5))
                .text_color(rgb(MUTED))
                .text_right()
                .child(description),
        );
    if divider {
        row.border_t_1().border_color(rgb(0x292b2d))
    } else {
        row
    }
}

fn render_install_button(
    id: &'static str,
    label: String,
    installing: bool,
    complete: bool,
    progress: usize,
) -> gpui::Stateful<gpui::Div> {
    let mut button = div()
        .id(id)
        .relative()
        .w_full()
        .min_w_0()
        .h(px(52.0))
        .flex()
        .items_center()
        .justify_center()
        .overflow_hidden()
        .rounded(px(10.0))
        .bg(rgb(if complete { 0x3d9a58 } else { 0xefe7d6 }))
        .text_size(px(14.0))
        .font_weight(FontWeight::BOLD)
        .text_color(rgb(if complete { 0xffffff } else { 0x14161a }));
    if installing && progress > 0 {
        button = button.child(
            div()
                .absolute()
                .top(px(0.0))
                .bottom(px(0.0))
                .left(px(0.0))
                .w(relative(progress as f32 / 100.0))
                .bg(rgb(0xd8d0bc)),
        );
    }
    button.child(div().relative().text_center().child(if complete {
        format!("✓  {label}")
    } else {
        label
    }))
}

fn render_simple_install_button(
    id: &'static str,
    label: String,
    complete: bool,
    disabled: bool,
) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .w_full()
        .min_w_0()
        .h(px(52.0))
        .flex()
        .items_center()
        .justify_center()
        .rounded(px(10.0))
        .bg(rgb(if complete { 0x3d9a58 } else { 0xefe7d6 }))
        .opacity(if disabled && !complete { 0.4 } else { 1.0 })
        .text_size(px(14.0))
        .font_weight(FontWeight::BOLD)
        .text_color(rgb(if complete { 0xffffff } else { 0x14161a }))
        .child(if complete {
            format!("✓  {label}")
        } else {
            label
        })
}

fn render_feature(
    icon: &'static str,
    title: String,
    description: String,
    divider: bool,
) -> gpui::Div {
    let row = div()
        .flex()
        .items_center()
        .gap(px(18.0))
        .py(px(19.0))
        .child(
            div()
                .size(px(48.0))
                .flex()
                .items_center()
                .justify_center()
                .rounded(px(11.0))
                .border_1()
                .border_color(rgb(0x353638))
                .bg(rgb(0x191b1e))
                .text_size(px(20.0))
                .text_color(rgb(0xd8d5cc))
                .child(icon),
        )
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(3.0))
                .child(
                    div()
                        .text_size(px(15.5))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(rgb(0xeceae3))
                        .child(title),
                )
                .child(
                    div()
                        .text_size(px(13.0))
                        .text_color(rgb(0x9aa09e))
                        .child(description),
                ),
        );
    if divider {
        row.border_t_1().border_color(rgb(0x292b2d))
    } else {
        row
    }
}

#[cfg(test)]
mod tests {
    use super::{LANGUAGES, MetalSharpApp, SetupCopy};
    use std::collections::HashMap;

    #[gpui::test]
    fn connected_setup_never_falls_back_to_synthetic_installation(cx: &mut gpui::TestAppContext) {
        let config = crate::backend_host::HostConfig {
            port: 0,
            home: "/should-not-create/gpui-virtual-fixture".into(),
            binary: "/missing-fixture-backend".into(),
            resources: "/missing-fixture-resources".into(),
            validation: true,
        };
        // Invalid port is rejected before sockets, home creation or child spawn.
        // The virtual GPUI window uses a mock platform, not the desktop.
        let window = cx.add_window(|_, cx| MetalSharpApp::new_connected_setup(config, cx));
        window
            .update(cx, |app, _, cx| {
                let fields = app.connected.as_ref().unwrap().read(cx).setup_inputs();
                assert!(!fields[0].read(cx).secret);
                assert!(fields[1].read(cx).secret && fields[2].read(cx).secret);
                app.step = 1;
                app.start_runtime_install(cx);
                app.start_steam_install(cx);
                assert!(!app.setup_can_advance(cx));
            })
            .unwrap();
        cx.run_until_parked();
        cx.executor()
            .advance_clock(std::time::Duration::from_secs(5));
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                assert!(app.show_setup);
                assert!(!app.runtime_installing);
                assert!(!app.runtime_installed);
                assert!(!app.steam_installed);
                assert_eq!(app.runtime_progress, 0);
                assert!(!app.setup_can_advance(cx));
            })
            .unwrap();
    }
    #[gpui::test]
    fn connected_streaming_never_simulates_unavailable_host_and_clears_pin(
        cx: &mut gpui::TestAppContext,
    ) {
        let config = crate::backend_host::HostConfig {
            port: 0,
            home: "/should-not-create/streaming-fixture".into(),
            binary: "/missing-fixture-backend".into(),
            resources: "/missing-fixture-resources".into(),
            validation: true,
        };
        let window = cx.add_window(|_, cx| MetalSharpApp::new_connected_workbench(config, cx));
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                let session = app.connected.clone().unwrap();
                session.update(cx, |session, cx| session.open_streaming(cx));
                let pin = session.read(cx).streaming_view().2;
                assert!(pin.read(cx).secret);
                pin.update(cx, |input, cx| {
                    input.content = "0042".into();
                    cx.notify();
                });
                session.update(cx, |session, cx| {
                    session.streaming_command(crate::streaming::StreamingAction::Install, cx);
                    session.streaming_command(crate::streaming::StreamingAction::Start, cx);
                    session.streaming_pair(cx);
                    // A remembered successful state must become unavailable when
                    // the retained owned host is absent, not stay actionable.
                    session.fixture_streaming_status(
                        Some(crate::streaming::StreamingStatus {
                            installed: true,
                            running: true,
                            creds_valid: true,
                            ..Default::default()
                        }),
                        cx,
                    );
                    session.streaming_command(crate::streaming::StreamingAction::Stop, cx);
                });
                assert!(session.read(cx).streaming_view().0.is_none());
                assert!(!app.streaming_installed && !app.streaming_running);
                app.close_streaming_panel(cx);
                assert!(pin.read(cx).content.is_empty());
            })
            .unwrap();
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                assert!(!app.streaming_open);
                assert!(
                    app.connected
                        .as_ref()
                        .unwrap()
                        .read(cx)
                        .streaming_view()
                        .0
                        .is_none()
                );
                assert!(!app.show_setup);
            })
            .unwrap();
    }
    #[gpui::test]
    fn unpair_confirmation_is_revoked_on_status_loss_recovery_and_expiry(
        cx: &mut gpui::TestAppContext,
    ) {
        let config = crate::backend_host::HostConfig {
            port: 0,
            home: "/should-not-create/unpair-fixture".into(),
            binary: "/missing-fixture-backend".into(),
            resources: "/missing-fixture-resources".into(),
            validation: true,
        };
        let window = cx.add_window(|_, cx| MetalSharpApp::new_connected_workbench(config, cx));
        cx.run_until_parked();
        let ready = || {
            Some(crate::streaming::StreamingStatus {
                installed: true,
                running: true,
                creds_valid: true,
                ..Default::default()
            })
        };
        window
            .update(cx, |app, _, cx| {
                app.connected.as_ref().unwrap().update(cx, |session, cx| {
                    session.fixture_streaming_status(ready(), cx)
                });
            })
            .unwrap();
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                app.streaming_unpair_confirm = true;
                app.streaming_unpair_deadline =
                    Some(std::time::Instant::now() + std::time::Duration::from_secs(10));
                app.connected
                    .as_ref()
                    .unwrap()
                    .update(cx, |session, cx| session.fixture_streaming_status(None, cx));
            })
            .unwrap();
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                assert!(!app.streaming_unpair_confirm);
                app.connected.as_ref().unwrap().update(cx, |session, cx| {
                    session.fixture_streaming_status(ready(), cx)
                });
            })
            .unwrap();
        cx.run_until_parked();
        window
            .update(cx, |app, _, cx| {
                assert!(!app.streaming_unpair_confirm);
                app.streaming_unpair_confirm = true;
                app.streaming_unpair_deadline =
                    Some(std::time::Instant::now() - std::time::Duration::from_secs(1));
                app.connected
                    .as_ref()
                    .unwrap()
                    .update(cx, |_, cx| cx.notify());
            })
            .unwrap();
        cx.run_until_parked();
        window
            .update(cx, |app, _, _| assert!(!app.streaming_unpair_confirm))
            .unwrap();
    }
    #[test]
    fn narrow_library_uses_short_search_copy() {
        assert_eq!(super::library_search_placeholder(720.0), "Search");
        assert_eq!(super::library_search_placeholder(800.0), "Search");
        assert_eq!(
            super::library_search_placeholder(1040.0),
            "Search games, genres, or tags..."
        );
        let app = MetalSharpApp::new();
        assert!(!app.streaming_open && !app.streaming_installed && !app.streaming_running);
    }

    #[test]
    fn collection_cards_fit_wrapping_rows() {
        assert_eq!(super::collection_card_layout(720.0).0, 3);
        assert_eq!(super::collection_card_layout(1040.0).0, 4);
        for width in [720.0, 1040.0, 1360.0, 1920.0] {
            let (columns, card_width) = super::collection_card_layout(width);
            assert!(card_width >= 190.0 && card_width <= 220.0);
            assert!(
                columns as f32 * card_width + (columns - 1) as f32 * 18.0 <= width - 64.0 + 0.01
            );
        }
    }

    #[test]
    fn light_theme_has_readable_white_menus_and_creamy_glow() {
        let light = super::PreviewTheme::Light;
        assert_eq!(light.control_text(), 0x000000);
        assert_eq!(light.menu_bg(), 0xffffff);
        assert_eq!(light.button_border(), 0xffffff);
        assert_eq!(light.border(), 0xffffff);
        assert_eq!(light.dock_glow(), 0xfff3dc);
        for icon in ["theme-skeleton.svg", "theme-forest.svg", "theme-orange.svg"] {
            assert!(
                std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("assets")
                    .join(icon)
                    .is_file()
            );
        }
    }

    #[test]
    fn preview_search_filters_games_and_scrolls_only_matching_results() {
        let mut app = MetalSharpApp::new();
        app.search_query = "  PORTAL  ".into();
        assert_eq!(app.matching_preview_games(), vec![5]);
        app.select_preview_game(5);
        app.scroll_preview_dock(1);
        assert_eq!((app.selected_game, app.dock_start), (5, 0));
        app.search_query = "e".into();
        let matches = app.matching_preview_games();
        app.select_preview_game(matches[0]);
        for _ in 0..20 {
            app.scroll_preview_dock(1);
        }
        assert_eq!(app.dock_start, matches.len().saturating_sub(5));
        assert!(matches.contains(&app.selected_game));
        app.search_query = "no game matches this".into();
        assert!(app.matching_preview_games().is_empty());
        app.dock_start = 0;
        app.scroll_preview_dock(1);
        assert_eq!(app.dock_start, 0);
    }

    #[test]
    fn sample_library_has_twenty_distinct_games_with_bundled_artwork() {
        let assets = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("assets");
        let mut names = std::collections::HashSet::new();
        let mut covers = std::collections::HashSet::new();
        let mut heroes = std::collections::HashSet::new();
        for (name, cover, hero) in super::LIBRARY_GAMES {
            assert!(names.insert(name), "duplicate game name: {name}");
            assert!(covers.insert(cover), "duplicate cover: {cover}");
            assert!(heroes.insert(hero), "duplicate hero: {hero}");
            for file in [cover, hero] {
                let bytes = std::fs::read(assets.join(file)).expect("bundled image missing");
                assert!(bytes.starts_with(&[0xff, 0xd8]), "not a JPEG: {file}");
            }
        }
        assert_eq!(names.len(), 20);
    }

    #[test]
    fn expanded_preview_dock_scrolls_both_directions_and_stops_at_ends() {
        let mut app = MetalSharpApp::new();
        let last_start = super::LIBRARY_GAMES.len() - super::DOCK_VISIBLE_CARDS;
        assert!(last_start >= 10);
        app.scroll_preview_dock(-1);
        assert_eq!((app.dock_start, app.selected_game), (0, 0));
        for expected in 1..=last_start {
            app.scroll_preview_dock(1);
            assert_eq!(app.dock_start, expected);
            assert!(app.selected_game >= expected && app.selected_game < expected + 5);
        }
        let last_selected = app.selected_game;
        app.scroll_preview_dock(1);
        assert_eq!(
            (app.dock_start, app.selected_game),
            (last_start, last_selected)
        );
        for expected in (0..last_start).rev() {
            app.scroll_preview_dock(-1);
            assert_eq!(app.dock_start, expected);
            assert!(app.selected_game >= expected && app.selected_game < expected + 5);
        }
        app.select_preview_game(super::LIBRARY_GAMES.len() - 1);
        assert_eq!(app.dock_start, last_start);
        app.select_preview_game(0);
        assert_eq!(app.dock_start, 0);
    }

    #[test]
    fn responsive_dock_fits_and_selection_forms_an_arch() {
        for width in [720.0, 840.0, 1040.0, 1360.0, 1920.0] {
            let card = super::dock_card_width(width);
            assert!(card <= 176.0);
            assert!(card * 5.0 + 252.0 <= width + 0.01);
        }
        assert_eq!(super::dock_card_lift(2, 2), -16.0);
        assert_eq!(super::dock_card_lift(1, 2), super::dock_card_lift(3, 2));
        assert!(super::dock_card_lift(0, 2) > super::dock_card_lift(1, 2));
        assert_eq!(super::dock_card_angle(0), -super::dock_card_angle(4));
        assert_eq!(
            (0..5).map(super::dock_card_angle).collect::<Vec<_>>(),
            [-6, -3, 0, 3, 6]
        );
        for selected in 0usize..5 {
            for index in 0usize..5 {
                let baseline = index.abs_diff(2) as f32 * 7.0;
                assert_eq!(
                    super::dock_card_lift(index, selected),
                    baseline - if index == selected { 16.0 } else { 0.0 }
                );
            }
        }
        for (_, cover, _) in super::LIBRARY_GAMES {
            for slot in 0..super::DOCK_VISIBLE_CARDS {
                let angle = super::dock_card_angle(slot);
                if angle == 0 {
                    continue;
                }
                let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("assets/dock")
                    .join(format!("{}-{angle}.png", cover.trim_end_matches(".jpg")));
                assert!(path.is_file(), "missing tilted artwork: {}", path.display());
            }
        }
    }

    #[test]
    fn preview_game_settings_are_independent_and_memory_only() {
        let mut app = MetalSharpApp::new();
        assert_eq!(app.game_preferences.len(), super::LIBRARY_GAMES.len());
        app.game_preferences[0].pipeline = 5;
        app.game_preferences[0].metal_fx = 0;
        app.game_preferences[0].controller = 2;
        app.game_preferences[0].msync = false;
        app.game_preferences[0].executable = Some("preview-only.exe".to_string());
        assert_eq!(app.game_preferences[1].pipeline, 0);
        assert_eq!(app.game_preferences[1].metal_fx, 1);
        assert_eq!(app.game_preferences[1].controller, 0);
        assert!(app.game_preferences[1].msync);
        assert!(app.game_preferences[1].executable.is_none());
        let fresh = MetalSharpApp::new();
        assert!(fresh.game_preferences[0].executable.is_none());
        assert_eq!(fresh.game_preferences[0].pipeline, 0);
        assert_eq!(fresh.game_preferences[0].metal_fx, 1);
    }

    #[test]
    fn setup_page_copy_is_available_for_every_language_option() {
        let locales: HashMap<String, SetupCopy> =
            serde_json::from_str(include_str!("../assets/setup-locales.json")).unwrap();
        assert_eq!(locales.len(), LANGUAGES.len());
        for (code, _) in LANGUAGES {
            let copy = locales
                .get(code)
                .unwrap_or_else(|| panic!("missing {code}"));
            assert_eq!(copy.titles.len(), 3, "invalid titles for {code}");
            assert!(!copy.titles[0].is_empty(), "missing title for {code}");
            assert!(
                !copy.titles[1].is_empty(),
                "missing runtime title for {code}"
            );
            assert!(
                !copy.runtime_lede.is_empty(),
                "missing runtime copy for {code}"
            );
            assert!(
                !copy.device_name.is_empty(),
                "missing device label for {code}"
            );
            assert!(!copy.api_key.is_empty(), "missing API-key label for {code}");
            assert!(
                !copy.the_games_db_api_key.is_empty(),
                "missing GamesDB label for {code}"
            );
            assert!(!copy.launch.is_empty(), "missing launch label for {code}");
            assert!(!copy.get_started.is_empty(), "missing CTA for {code}");
            assert_eq!(copy.steps.len(), 3, "invalid step labels for {code}");
        }
        assert_ne!(locales["en"].titles[0], locales["es"].titles[0]);
    }
}
