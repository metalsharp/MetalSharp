use crate::library_model::{self, LibGame};
use gpui::{
    App, AppContext, ClickEvent, Context, Focusable, FontWeight, ObjectFit, Render, Window, div,
    img, linear_color_stop, linear_gradient, prelude::*, px, relative, rgb, rgba,
};
use serde::Deserialize;
use std::collections::HashMap;
use std::path::PathBuf;

#[path = "ui_live.rs"]
mod live_impl;
#[path = "ui_steamcmd.rs"]
mod steamcmd_impl;
use live_impl::{LiveState, LogClass, MigrationViewState, SteamPending};
use steamcmd_impl::{CollectionView, SteamcmdModal};

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
#[allow(dead_code)]
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
    #[serde(default)]
    steam_failed: String,
    #[serde(default)]
    starting_installation: String,
    #[serde(default)]
    downloading_steam: String,
    #[serde(default)]
    creating_steam_prefix: String,
    #[serde(default)]
    retry_steam: String,
    #[serde(default)]
    preparing_steam: String,
    #[serde(default)]
    steam_install_failed: String,
    #[serde(default)]
    steam_install_timed_out: String,
    #[serde(default)]
    wrapper_warning: String,
    #[serde(default)]
    api_key_save_failed: String,
    #[serde(default)]
    api_key_steam_id_missing: String,
    #[serde(default)]
    the_games_db_api_key_save_failed: String,
    #[serde(default)]
    install_failed: String,
    #[serde(default)]
    start_steam: String,
    #[serde(default)]
    start_steam_text: String,
    #[serde(default)]
    first_launch: String,
    #[serde(default)]
    first_launch_text: String,
    #[serde(default)]
    exit: String,
}

pub(crate) fn asset_path(name: &str) -> PathBuf {
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

impl PreviewTheme {
    /// Electron `useTheme` identifiers (persisted like localStorage `metalsharp-theme`).
    fn storage_id(self) -> &'static str {
        match self {
            Self::Dark => "dark",
            Self::Light => "light",
            Self::Skeleton => "skeleton",
            Self::Forest => "forest",
            Self::OrangePeel => "orange-peel",
            Self::Dragonfruit => "dragonfruit",
            Self::Lava => "lava",
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

fn preview_games() -> Vec<LibGame> {
    LIBRARY_GAMES
        .iter()
        .enumerate()
        .map(|(index, (name, cover, hero))| LibGame {
            appid: index as u64 + 1,
            name: (*name).to_owned(),
            installed: true,
            state: Some("installed".into()),
            launch_method: Some("d3dmetal".into()),
            preview_art: Some((*cover, *hero)),
            ..Default::default()
        })
        .collect()
}

pub struct MetalSharpApp {
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
    streaming_pin: Option<gpui::Entity<crate::search_input::SearchInput>>,
    steam_running: bool,
    sharp_preview: Option<gpui::Entity<crate::sharp_preview::SharpPreview>>,
    logs_preview: Option<gpui::Entity<crate::logs_preview::LogsPreview>>,
    search_input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    setup_inputs: Option<[gpui::Entity<crate::search_input::SearchInput>; 3]>,
    search_query: String,
    /// Displayed (installed, ordered) games: live library or bundled samples.
    games: Vec<LibGame>,
    selected_game: usize,
    dock_start: usize,
    pipeline_menu_open: bool,
    collection_menu: Option<(bool, u64)>,
    collection_view: CollectionView,
    steamcmd_modal: Option<SteamcmdModal>,
    steamcmd_inputs: Option<[gpui::Entity<crate::search_input::SearchInput>; 3]>,
    game_settings_open: bool,
    hovered_card: Option<usize>,
    active_tab: LibraryTab,
    step: usize,
    selected_language: &'static str,
    language_menu_open: bool,
    locales: HashMap<String, SetupCopy>,
    copy: SetupCopy,
    developer_mode: bool,
    low_performance: bool,
    startup_video_seen: bool,
    migration_view: MigrationViewState,
    live: LiveState,
    observers: Vec<gpui::Subscription>,
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
            streaming_pin: None,
            steam_running: false,
            sharp_preview: None,
            logs_preview: None,
            search_input: None,
            setup_inputs: None,
            search_query: String::new(),
            games: preview_games(),
            selected_game: 0,
            dock_start: 0,
            pipeline_menu_open: false,
            collection_menu: None,
            collection_view: CollectionView::Installed,
            steamcmd_modal: None,
            steamcmd_inputs: None,
            game_settings_open: false,
            hovered_card: None,
            active_tab: LibraryTab::Play,
            step: 2,
            selected_language: "en",
            language_menu_open: false,
            locales,
            copy,
            developer_mode: false,
            low_performance: false,
            startup_video_seen: false,
            migration_view: MigrationViewState::default(),
            live: LiveState::default(),
            observers: Vec::new(),
        }
    }

    fn ensure_inputs(&mut self, cx: &mut Context<Self>) {
        if self.setup_inputs.is_none() {
            let make = |placeholder: &str, secret: bool, cx: &mut Context<Self>| {
                cx.new(|cx| {
                    let mut input = crate::search_input::SearchInput::new(cx);
                    input.placeholder = placeholder.to_owned().into();
                    input.secret = secret;
                    input
                })
            };
            let device = make(&self.copy.device_placeholder.clone(), false, cx);
            let steam = make(&self.copy.api_placeholder.clone(), true, cx);
            let gamesdb = make(&self.copy.the_games_db_api_placeholder.clone(), true, cx);
            self.setup_inputs = Some([device, steam, gamesdb]);
        }
        if self.streaming_pin.is_none() {
            self.streaming_pin = Some(cx.new(|cx| {
                let mut input = crate::search_input::SearchInput::new(cx);
                input.placeholder = "PIN".into();
                input.secret = true;
                input
            }));
        }
        if let Some(inputs) = &self.setup_inputs {
            let placeholders = [
                self.copy.device_placeholder.clone(),
                self.copy.api_placeholder.clone(),
                self.copy.the_games_db_api_placeholder.clone(),
            ];
            for (input, placeholder) in inputs.iter().zip(placeholders) {
                input.update(cx, |input, _| {
                    if input.placeholder.as_ref() != placeholder {
                        input.placeholder = placeholder.into();
                    }
                });
            }
        }
    }

    fn close_streaming_panel(&mut self, cx: &mut Context<Self>) {
        self.streaming_open = false;
        self.streaming_unpair_confirm = false;
        self.streaming_unpair_deadline = None;
        self.live.streaming.session += 1;
        if let Some(pin) = &self.streaming_pin {
            pin.update(cx, |input, cx| input.clear(cx));
        }
        cx.notify();
    }

    fn change_language(&mut self, code: &'static str, cx: &mut Context<Self>) {
        self.selected_language = code;
        if let Some(copy) = self.locales.get(code).cloned() {
            self.copy = copy;
        }
        self.save_ui_state(cx);
    }

    fn render_status_screen(
        &self,
        title: String,
        detail: String,
        retry: bool,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        div()
            .size_full()
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap(px(14.0))
            .bg(rgb(PAGE_BG))
            .font_family("Rethink Sans")
            .text_color(rgb(TEXT))
            .child(
                img(asset_path("metalsharp-logo.png"))
                    .size(px(72.0))
                    .object_fit(ObjectFit::Contain),
            )
            .child(
                div()
                    .text_size(px(20.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(title),
            )
            .child(
                div()
                    .max_w(px(560.0))
                    .text_center()
                    .text_size(px(13.0))
                    .line_height(px(19.0))
                    .text_color(rgb(MUTED))
                    .child(detail),
            )
            .children(retry.then(|| {
                div()
                    .id("boot-retry")
                    .mt(px(6.0))
                    .h(px(40.0))
                    .px(px(22.0))
                    .flex()
                    .items_center()
                    .rounded(px(8.0))
                    .bg(rgb(0xefe7d6))
                    .text_color(rgb(0x14161a))
                    .font_weight(FontWeight::BOLD)
                    .cursor_pointer()
                    .on_click(cx.listener(|this, _, _, cx| this.retry_boot(cx)))
                    .child("Retry")
            }))
    }

    fn render_migration(&self, cx: &mut Context<Self>) -> gpui::Div {
        let view = &self.migration_view;
        let complete = view.status == "complete";
        let failed = view.status == "error";
        let percent = if view.total > 0 {
            (view.step as f32 / view.total as f32).clamp(0.0, 1.0)
        } else {
            0.0
        };
        div()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgb(PAGE_BG))
            .font_family("Rethink Sans")
            .text_color(rgb(TEXT))
            .child(
                div()
                    .w(px(520.0))
                    .max_w_full()
                    .p(px(28.0))
                    .rounded(px(14.0))
                    .border_1()
                    .border_color(rgb(0x292b2d))
                    .bg(rgb(PANEL_BG))
                    .flex()
                    .flex_col()
                    .gap(px(14.0))
                    .child(
                        div()
                            .text_size(px(22.0))
                            .font_family("Georgia")
                            .child(if complete {
                                "Update complete"
                            } else if failed {
                                "Migration failed"
                            } else {
                                "Finishing your update…"
                            }),
                    )
                    .child(div().text_size(px(13.0)).text_color(rgb(MUTED)).child(
                        if failed && !view.error.is_empty() {
                            view.error.clone()
                        } else if view.message.is_empty() {
                            "Migrating MetalSharp data to the new version.".to_owned()
                        } else {
                            view.message.clone()
                        },
                    ))
                    .child(
                        div()
                            .h(px(8.0))
                            .w_full()
                            .rounded_full()
                            .bg(rgb(0x2a2d30))
                            .child(
                                div()
                                    .h(px(8.0))
                                    .w(relative(if complete { 1.0 } else { percent }))
                                    .rounded_full()
                                    .bg(rgb(if failed { 0xd66a6a } else { 0xefe7d6 })),
                            ),
                    )
                    .children(complete.then(|| {
                        div()
                            .id("migration-restart")
                            .h(px(44.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded(px(8.0))
                            .bg(rgb(0xefe7d6))
                            .text_color(rgb(0x14161a))
                            .font_weight(FontWeight::BOLD)
                            .cursor_pointer()
                            .on_click(
                                cx.listener(|this, _, _, cx| this.restart_after_migration(cx)),
                            )
                            .child("Restart MetalSharp")
                    })),
            )
    }

    fn render_update_confirm(
        &self,
        variant: &'static str,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let button = |id: &'static str, label: &'static str, primary: bool| {
            div()
                .id(id)
                .h(px(34.0))
                .px(px(16.0))
                .flex()
                .items_center()
                .rounded(px(7.0))
                .border_1()
                .border_color(rgba(if primary { 0xefe7d6ff } else { 0xffffff2e }))
                .bg(rgb(if primary { 0xefe7d6 } else { 0x1d2124 }))
                .text_color(rgb(if primary { 0x14161a } else { 0xe6e8e7 }))
                .text_size(px(13.0))
                .font_weight(FontWeight::SEMIBOLD)
                .cursor_pointer()
                .child(label)
        };
        div()
            .id("update-confirm-backdrop")
            .occlude()
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgba(0x000000a0))
            .on_click(cx.listener(|this, _, _, cx| {
                this.live.update_confirm = None;
                cx.notify();
            }))
            .child(
                div()
                    .id("update-confirm-modal")
                    .occlude()
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .w(px(420.0))
                    .p(px(22.0))
                    .rounded(px(12.0))
                    .border_1()
                    .border_color(rgba(0xffffff1f))
                    .bg(rgb(0x14171a))
                    .text_color(rgb(0xe6e8e7))
                    .flex()
                    .flex_col()
                    .gap(px(14.0))
                    .child(div().text_size(px(17.0)).font_weight(FontWeight::BOLD).child("Confirm Update"))
                    .children((variant == "fex").then(|| {
                        div().text_size(px(12.0)).line_height(px(18.0)).text_color(rgb(0xffd47f)).child(
                            "FEX Version Notice — The FEX DMG only works on macOS 27 or newer. The FEX version is experimental, so expect more potential bugs than the baseline MetalSharp version.",
                        )
                    }))
                    .child(
                        div()
                            .text_size(px(13.0))
                            .line_height(px(19.0))
                            .child("MetalSharp will now close and re-open on it's own to update. Proceed?"),
                    )
                    .child(
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.0))
                            .child(button("update-confirm-cancel", "Cancel", false).on_click(cx.listener(
                                |this, _, _, cx| {
                                    this.live.update_confirm = None;
                                    cx.notify();
                                },
                            )))
                            .child(button("update-confirm-ok", "Ok", true).on_click(cx.listener(
                                move |this, _, _, cx| this.begin_update_download(variant, cx),
                            ))),
                    ),
            )
    }
}

impl Render for MetalSharpApp {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.ensure_inputs(cx);
        if self.live.intro_pending {
            self.show_startup_video(window, cx);
        }
        let toasts = crate::toast::hub(cx);
        let library_visible = !(self.live.enabled && self.live.booting)
            && self.live.boot_error.is_none()
            && !self.live.migration
            && !self.show_setup;
        if !library_visible {
            self.deactivate_polling_pages(cx);
        }
        let body: gpui::AnyElement = if self.live.enabled && self.live.booting {
            self.render_status_screen(
                "Starting MetalSharp…".into(),
                "Starting the MetalSharp backend.".into(),
                false,
                cx,
            )
            .into_any_element()
        } else if let Some(error) = self.live.boot_error.clone() {
            self.render_status_screen("MetalSharp backend unavailable".into(), error, true, cx)
                .into_any_element()
        } else if self.live.migration {
            self.render_migration(cx).into_any_element()
        } else if !self.show_setup {
            self.render_library(window.viewport_size(), cx)
                .into_any_element()
        } else {
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
                .into_any_element()
        };
        div()
            .relative()
            .size_full()
            .child(body)
            .children(self.live.update_confirm.map(|variant| {
                gpui::deferred(self.render_update_confirm(variant, cx)).with_priority(300)
            }))
            .children(
                self.steamcmd_modal
                    .is_some()
                    .then(|| gpui::deferred(self.render_steamcmd_modal(cx)).with_priority(310)),
            )
            .children(toasts.map(|hub| gpui::deferred(hub).with_priority(400)))
    }
}

impl MetalSharpApp {
    /// Logs and Sharp poll the backend only while their tab is on screen.
    fn deactivate_polling_pages(&mut self, cx: &mut Context<Self>) {
        if let Some(sharp) = &self.sharp_preview {
            sharp.update(cx, |sharp, cx| sharp.set_active(false, cx));
        }
        if let Some(logs) = &self.logs_preview {
            logs.update(cx, |logs, cx| logs.set_active(false, cx));
        }
    }

    fn retry_boot(&mut self, cx: &mut Context<Self>) {
        let Some(live) = crate::live::Live::get(cx) else {
            return;
        };
        self.live.boot_error = None;
        self.live.booting = true;
        cx.notify();
        cx.spawn(async move |this, cx| {
            let started = cx
                .background_executor()
                .spawn(async move { live.start_backend() })
                .await;
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
        // These covers are decorative setup artwork, not installed-library data.
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
            let app_state = self.settings_app_state();
            settings.update(cx, |settings, cx| {
                let changed = settings.palette != palette || settings.language != language;
                settings.palette = palette;
                settings.language = language;
                if settings.sync_app_state(app_state) || changed {
                    cx.notify();
                }
            });
        }
        if self.sharp_preview.is_none() {
            self.sharp_preview = Some(cx.new(|cx| {
                let mut sharp = crate::sharp_preview::SharpPreview::new();
                sharp.asset_root = asset_path("");
                sharp.attach_live(cx);
                sharp
            }));
        }
        if let Some(sharp) = &self.sharp_preview {
            let palette = self.theme.page_palette();
            let active = self.active_tab == LibraryTab::SharpLibrary;
            // App-level overlays cover the page; the native GameJolt view must hide.
            let obscured =
                self.settings_open || self.streaming_open || self.live.update_confirm.is_some();
            sharp.update(cx, |sharp, cx| {
                sharp.set_obscured(obscured, cx);
                if sharp.palette != palette {
                    sharp.palette = palette;
                    cx.notify();
                }
                sharp.set_active(active, cx);
            });
        }
        if self.logs_preview.is_none() {
            self.logs_preview = Some(cx.new(|cx| {
                let mut logs = crate::logs_preview::LogsPreview::new();
                logs.attach_live(cx);
                logs
            }));
        }
        if let Some(logs) = &self.logs_preview {
            let palette = self.theme.page_palette();
            let active = self.active_tab == LibraryTab::Logs;
            logs.update(cx, |logs, cx| {
                if logs.palette != palette {
                    logs.palette = palette;
                    cx.notify();
                }
                logs.set_active(active, cx);
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
                    let matches = this.matching_games();
                    if !matches.contains(&this.selected_game) {
                        if let Some(first) = matches.first() {
                            this.selected_game = *first;
                        }
                    }
                    this.select_game(this.selected_game, cx);
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
                            this.save_ui_state(cx);
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
            let (label, disabled): (&str, bool) = if is_launcher {
                if self.live.ubisoft_installing {
                    ("Installing Ubisoft Connect…", true)
                } else if self.live.ubisoft_running {
                    ("Stop Ubisoft", false)
                } else {
                    ("Launch Ubisoft", false)
                }
            } else if self.live.steam_fix_busy {
                ("Fixing Steam...", true)
            } else {
                ("Fix Steam", false)
            };
            menu = menu.child(
                div()
                    .id("steam-menu-option")
                    .h(px(30.0))
                    .flex()
                    .items_center()
                    .px(px(10.0))
                    .rounded(px(5.0))
                    .text_size(px(12.5))
                    .text_color(rgb(control_text))
                    .opacity(if disabled { 0.5 } else { 1.0 })
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(theme.menu_hover())))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if is_launcher {
                            this.toggle_ubisoft(cx);
                        } else {
                            this.run_steam_fix(cx);
                        }
                        this.launcher_menu_open = false;
                        this.steam_options_open = false;
                        cx.notify();
                    }))
                    .child(label.to_owned()),
            );
            steam_menu = Some(gpui::deferred(menu.id("steam_menu").occlude()).with_priority(10));
        }

        let steam_pending = self.live.steam_pending;
        let steam_running = if self.live.enabled {
            self.live.wine_steam_running
        } else {
            self.steam_running
        };
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
                            .when(steam_pending.is_some(), |d| d.opacity(0.7))
                            .when(steam_pending.is_none(), |d| d.cursor_pointer())
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.close_library_menus();
                                this.toggle_steam(false, cx);
                            }))
                            .child("◉")
                            .child(match steam_pending {
                                Some(SteamPending::Starting) => "Starting Steam…",
                                Some(SteamPending::Stopping) => "Stopping Steam…",
                                None if steam_running => "Stop Steam",
                                None => "Start Steam",
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

        let footer = self.render_footer(cx);
        let update_banner = self.render_update_banner(cx);

        div()
            .relative()
            .size_full()
            .flex()
            .flex_col()
            .overflow_hidden()
            .font_family("Rethink Sans")
            .bg(rgb(0x111416))
            .children(update_banner)
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

    /// App.vue update banner.
    fn render_update_banner(&self, cx: &mut Context<Self>) -> Option<gpui::Div> {
        if !self.update_available() || self.live.update_dismissed {
            return None;
        }
        let version = self.update_field("latest_version").unwrap_or_default();
        let notes = self.update_field("release_notes").unwrap_or_default();
        let first_line = notes
            .lines()
            .find(|line| !line.trim().is_empty())
            .unwrap_or("")
            .trim_start_matches('#')
            .replace("**", "")
            .replace('`', "")
            .trim()
            .to_owned();
        let changelog = if first_line.chars().count() <= 20 {
            first_line
        } else {
            format!("{}…", first_line.chars().take(19).collect::<String>())
        };
        let fex = self
            .live
            .update_status
            .as_ref()
            .and_then(|s| s.get("fex_available"))
            .and_then(serde_json::Value::as_bool)
            == Some(true);
        let button = |id: &'static str, label: &'static str, primary: bool| {
            div()
                .id(id)
                .h(px(26.0))
                .px(px(11.0))
                .flex()
                .items_center()
                .rounded(px(6.0))
                .border_1()
                .border_color(rgba(if primary { 0x5fb7e8ff } else { 0xffffff2e }))
                .bg(rgba(if primary { 0x5fb7e833 } else { 0xffffff0a }))
                .text_size(px(11.5))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xe6e8e7))
                .cursor_pointer()
                .child(label)
        };
        let downloading = self.live.update_downloading;
        Some(
            div()
                .flex_none()
                .flex()
                .items_center()
                .justify_center()
                .gap(px(12.0))
                .px(px(16.0))
                .py(px(8.0))
                .bg(rgb(0x15191c))
                .border_b_1()
                .border_color(rgba(0x8caac814))
                .text_size(px(12.0))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(0xe6e8e7))
                .child(if downloading {
                    div().child(self.live.update_message.clone())
                } else {
                    div()
                        .flex()
                        .gap(px(8.0))
                        .child(format!("MetalSharp v{version} is available"))
                        .child(div().text_color(rgb(0x9aa09e)).child(changelog))
                })
                .children(downloading.then(|| {
                    div()
                        .w(px(160.0))
                        .h(px(5.0))
                        .rounded_full()
                        .bg(rgb(0x2a2d30))
                        .child(
                            div()
                                .h(px(5.0))
                                .rounded_full()
                                .bg(rgb(0x5fb7e8))
                                .w(relative(
                                    (self.live.update_progress / 100.0).clamp(0.0, 1.0),
                                )),
                        )
                }))
                .children((!downloading).then(|| {
                    button("update-banner-install", "Download & Install", true).on_click(
                        cx.listener(|this, _, _, cx| this.start_update_download("regular", cx)),
                    )
                }))
                .children((!downloading && fex).then(|| {
                    button("update-banner-fex", "Update to FEX Version", false).on_click(
                        cx.listener(|this, _, _, cx| this.start_update_download("fex", cx)),
                    )
                }))
                .children((!downloading).then(|| {
                    button("update-banner-close", "×", false).on_click(cx.listener(
                        |this, _, _, cx| {
                            this.live.update_dismissed = true;
                            cx.notify();
                        },
                    ))
                })),
        )
    }

    /// LibraryFooter.vue.
    fn render_footer(&self, cx: &mut Context<Self>) -> gpui::Div {
        let theme = self.theme;
        let live = self.live.enabled;
        let (ready_label, ready_detail) = if !live {
            (
                "Preview library ready".to_owned(),
                format!("{} sample games · UI preview only", self.games.len()),
            )
        } else {
            let total = self.live.library.len();
            let installed = self.live.installed_count;
            let label = if self.live.backend_connected {
                "All Games Ready"
            } else {
                "Offline"
            };
            let detail = if self.live.backend_connected {
                "All Games Ready".to_owned()
            } else if let Some(version) = &self.live.backend_version {
                format!("Backend Runtime v{version}")
            } else {
                "Offline".to_owned()
            };
            (
                label.to_owned(),
                format!("{installed} of {total} games · {detail}"),
            )
        };
        let available = self.update_available();
        let downloading = self.live.update_downloading;
        let progress = self.live.update_progress.round().clamp(3.0, 100.0) as u32;
        let (update_title, update_detail) = if downloading {
            (
                format!("Updating… {progress}%"),
                if self.live.update_message.is_empty() {
                    self.update_field("latest_version")
                        .unwrap_or_else(|| "Up To Date".into())
                } else {
                    self.live.update_message.clone()
                },
            )
        } else if available {
            (
                "Update Ready: Download Now?".to_owned(),
                format!(
                    "v{} ready",
                    self.update_field("latest_version").unwrap_or_default()
                ),
            )
        } else if live {
            ("Up To Date".to_owned(), "Up To Date".to_owned())
        } else {
            (
                "Up to date".to_owned(),
                "Preview build · updater disabled".to_owned(),
            )
        };
        div()
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
                                    .child(ready_label),
                            )
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .text_color(rgb(0x969b9a))
                                    .child(ready_detail),
                            ),
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
                    .on_click(cx.listener(|this, _, _, cx| this.open_streaming_panel(cx)))
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
                            .id("footer-update")
                            .size(px(30.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .rounded_full()
                            .border_1()
                            .border_color(rgba(if available {
                                0x74d28aff
                            } else if theme == PreviewTheme::Light {
                                0xffffffff
                            } else {
                                0xe7eaec2e
                            }))
                            .bg(rgb(if available { 0x1f3a28 } else { 0x282c2d }))
                            .text_size(px(16.0))
                            .text_color(rgb(if available { 0x74d28a } else { 0x777d7b }))
                            .when(available && !downloading, |button| button.cursor_pointer())
                            .on_click(cx.listener(|this, _, _, cx| {
                                if this.update_available() && !this.live.update_downloading {
                                    this.start_update_download("regular", cx);
                                }
                            }))
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
                                    .child(update_title),
                            )
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .text_color(rgb(0x969b9a))
                                    .child(update_detail),
                            )
                            .children(downloading.then(|| {
                                div()
                                    .w(px(140.0))
                                    .h(px(3.0))
                                    .rounded_full()
                                    .bg(rgb(0x2a2d30))
                                    .child(
                                        div()
                                            .h(px(3.0))
                                            .rounded_full()
                                            .bg(rgb(0x74d28a))
                                            .w(relative(progress as f32 / 100.0)),
                                    )
                            })),
                    ),
            )
    }

    /// LibraryView empty hero ("YOUR LIBRARY AWAITS").
    fn render_empty_hero(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let theme = self.theme;
        let hero_height = (f32::from(viewport.height) * 0.54).clamp(300.0, 520.0);
        let searching = !self.search_query.trim().is_empty() && !self.games.is_empty();
        let starting = self.live.steam_pending == Some(SteamPending::Starting);
        div()
            .id("library-empty-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .bg(rgb(0x111416))
            .flex()
            .flex_col()
            .child(
                div()
                    .w_full()
                    .h(px(hero_height))
                    .flex_none()
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(linear_gradient(
                        180.0,
                        linear_color_stop(rgb(0x1a1d20), 0.0),
                        linear_color_stop(rgb(0x111416), 1.0),
                    ))
                    .child(if searching {
                        div().text_color(rgb(0xd4d5d4)).child("No matching games")
                    } else {
                        div()
                            .max_w(px(560.0))
                            .flex()
                            .flex_col()
                            .items_center()
                            .gap(px(12.0))
                            .text_center()
                            .child(div().text_size(px(38.0)).text_color(rgb(0x9aa09e)).child("▦"))
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(0xb9b0a1))
                                    .child("YOUR LIBRARY AWAITS"),
                            )
                            .child(
                                div()
                                    .text_size(px(39.0))
                                    .font_family("Georgia")
                                    .text_color(rgb(theme.hero_title()))
                                    .child("No installed games"),
                            )
                            .child(
                                div()
                                    .text_size(px(14.0))
                                    .line_height(px(21.0))
                                    .text_color(rgb(0xaeb3b2))
                                    .child("Start Steam or install a game to see your real library here. MetalSharp will use the games already installed on this Mac."),
                            )
                            .child(
                                div()
                                    .id("library-empty-start-steam")
                                    .mt(px(8.0))
                                    .w(px(204.0))
                                    .h(px(48.0))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .gap(px(10.0))
                                    .rounded(px(8.0))
                                    .border_1()
                                    .border_color(rgb(theme.border()))
                                    .bg(rgb(theme.control_bg()))
                                    .text_size(px(15.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(theme.control_text()))
                                    .when(starting, |d| d.opacity(0.7))
                                    .when(!starting, |d| {
                                        d.cursor_pointer()
                                            .hover(|style| style.bg(rgb(theme.control_hover())))
                                    })
                                    .on_click(cx.listener(|this, _, _, cx| this.start_steam(true, cx)))
                                    .child("◉")
                                    .child(if starting {
                                        "Starting Steam…"
                                    } else {
                                        "Start Steam"
                                    }),
                            )
                    }),
            )
    }

    fn render_library_play(
        &mut self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let matches = self.matching_games();
        if matches.is_empty() {
            return self.render_empty_hero(viewport, cx);
        }
        let width = f32::from(viewport.width);
        let hero_height = (f32::from(viewport.height) * 0.54).clamp(300.0, 520.0);
        let card_width = dock_card_width(width);
        let card_height = card_width * 1.35 + 12.0;
        let selected = if matches.contains(&self.selected_game) {
            self.selected_game
        } else {
            matches[0]
        };
        let game = self.games[selected].clone();
        let theme = self.theme;
        let accent = theme.accent();
        let control_bg = theme.control_bg();
        let control_text = theme.control_text();
        let is_running = self.is_running(game.appid);
        let launching = self.live.launching == Some(game.appid);
        let pipeline_label = library_model::game_pipeline_options(&game)
            .into_iter()
            .find(|(id, _)| *id == self.live.selected_pipeline)
            .map(|(_, label)| label)
            .unwrap_or_else(|| library_model::pipeline_label(&self.live.selected_pipeline));
        let pipeline_menu = self
            .pipeline_menu_open
            .then(|| gpui::deferred(self.render_pipeline_menu(&game, cx)).with_priority(20));
        let game_settings_menu = self
            .game_settings_open
            .then(|| gpui::deferred(self.render_game_settings(&game, cx)).with_priority(20));
        let hero_art = self.art_path(&game, true, cx);
        let play_game = game.clone();
        let badge = if game.is_ubisoft() {
            Some("Ubisoft Connect")
        } else if game.has_native_build {
            Some("Native macOS")
        } else {
            None
        };
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
            .children(hero_art.map(|art| {
                img(art)
                    .w_full()
                    .h_full()
                    .absolute()
                    .top(px(0.0))
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .object_fit(ObjectFit::Cover)
                    .opacity(0.62)
            }))
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
                            .child(game.name.clone()),
                    )
                    .children(badge.map(|badge| {
                        div()
                            .px(px(9.0))
                            .py(px(3.0))
                            .rounded(px(5.0))
                            .border_1()
                            .border_color(rgba(0xe7eaec4d))
                            .bg(rgba(0x0c0f10b8))
                            .text_size(px(11.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(0xf0f0ee))
                            .child(badge)
                    }))
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
                                    .opacity(if launching { 0.6 } else { 1.0 })
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
                                        this.launch_game(play_game.clone(), cx);
                                    }))
                                    .child(if is_running { "■" } else { "▶" })
                                    .child(if launching {
                                        "Launching"
                                    } else if is_running {
                                        "Stop"
                                    } else {
                                        "Play"
                                    }),
                            )
                            .child(
                                div()
                                    .flex_none()
                                    .flex()
                                    .items_center()
                                    .gap(px(9.0))
                                    .children((!game.is_ubisoft()).then(|| {
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
                                            .on_click(cx.listener(|this, _, _, cx| {
                                                this.open_steam_art_manager(cx)
                                            }))
                                            .child("✎")
                                    }))
                                    .children((!game.has_native_build).then(|| {
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
                                                        if this.live.pipeline_saving {
                                                            return;
                                                        }
                                                        let open = !this.pipeline_menu_open;
                                                        this.close_library_menus();
                                                        this.pipeline_menu_open = open;
                                                        cx.notify();
                                                    }))
                                                    .child("Bottle")
                                                    .child(pipeline_label.clone())
                                                    .child(if self.live.pipeline_saving {
                                                        "Saving…"
                                                    } else {
                                                        "⌄"
                                                    }),
                                            )
                                            .children(pipeline_menu)
                                    }))
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
            let card_game = self.games[index].clone();
            let selected_card = index == selected;
            let arch_slot = first_slot + slot;
            // Tilted cover variants: bundled for samples, rendered on demand for real games.
            let slot_angle = dock_card_angle(arch_slot);
            let (angle, card_art) = match card_game.preview_art {
                Some((cover, _)) if slot_angle != 0 => (
                    slot_angle,
                    Some(asset_path(&format!(
                        "dock/{}-{slot_angle}.png",
                        cover.trim_end_matches(".jpg")
                    ))),
                ),
                Some(_) => (0, self.art_path(&card_game, false, cx)),
                None => match (slot_angle != 0)
                    .then(|| self.tilted_art(&card_game, slot_angle, cx))
                    .flatten()
                {
                    Some(path) => (slot_angle, Some(path)),
                    None => (0, self.art_path(&card_game, false, cx)),
                },
            };
            let hovered = self.hovered_card == Some(index);
            let running = self.is_running(card_game.appid);
            let launching = self.live.launching == Some(card_game.appid);
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
                    this.active_tab = LibraryTab::Play;
                    this.close_library_menus();
                    this.select_game(index, cx);
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
                        .child(match card_art {
                            Some(art) => img(art)
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
                                })
                                .into_any_element(),
                            None => div()
                                .size_full()
                                .flex()
                                .flex_col()
                                .items_center()
                                .justify_center()
                                .gap(px(8.0))
                                .p(px(10.0))
                                .child(
                                    img(asset_path("metalsharp-logo.png"))
                                        .size(px(card_width * 0.42))
                                        .object_fit(ObjectFit::Contain),
                                )
                                .child(
                                    div()
                                        .text_size(px(11.0))
                                        .text_center()
                                        .text_color(rgb(0xd8dad9))
                                        .child(card_game.name.clone()),
                                )
                                .into_any_element(),
                        })
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
                        .child(if hovered || running || launching {
                            let action_game = card_game.clone();
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
                                            cx.stop_propagation();
                                            this.launch_game(action_game.clone(), cx);
                                        }))
                                        .child(if running { "■" } else { "▶" })
                                        .child(if launching {
                                            "Launching"
                                        } else if running {
                                            "Stop"
                                        } else {
                                            "Play"
                                        }),
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
                                this.scroll_dock(-1, cx);
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
                                this.scroll_dock(1, cx);
                                cx.notify();
                            }))
                            .child("›"),
                    ),
            );

        div()
            .id("library-play-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .bg(rgb(0x111416))
            .flex()
            .flex_col()
            .child(hero)
            .child(dock)
    }

    fn matching_games(&self) -> Vec<usize> {
        let query = self.search_query.trim().to_lowercase();
        self.games
            .iter()
            .enumerate()
            .filter_map(|(index, game)| {
                if query.is_empty() {
                    return Some(index);
                }
                let developer = if game.is_ubisoft() {
                    "Ubisoft"
                } else {
                    "MetalSharp library"
                };
                let tag = if game.is_ubisoft() {
                    "Ubisoft".to_owned()
                } else {
                    game.launch_method_name
                        .clone()
                        .unwrap_or_else(|| "Installed".into())
                };
                let state = if game.state.as_deref() == Some("installed") {
                    "Ready to play".to_owned()
                } else {
                    game.state.clone().unwrap_or_default()
                };
                [game.name.as_str(), developer, tag.as_str(), state.as_str()]
                    .iter()
                    .any(|value| value.to_lowercase().contains(&query))
                    .then_some(index)
            })
            .collect()
    }

    fn select_game(&mut self, index: usize, cx: &mut Context<Self>) {
        self.selected_game = index.min(self.games.len().saturating_sub(1));
        let matches = self.matching_games();
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
        self.on_featured_changed(cx);
    }

    fn scroll_dock(&mut self, direction: i32, cx: &mut Context<Self>) {
        let matches = self.matching_games();
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
        self.select_game(matches[next_position], cx);
    }

    fn open_settings_preview(&mut self, window: &mut gpui::Window, cx: &mut Context<Self>) {
        self.close_library_menus();
        self.streaming_open = false;
        if self.settings_preview.is_none() {
            let settings = cx.new(|cx| {
                let mut settings = crate::settings_preview::SettingsPreview::new();
                settings.attach_live(cx);
                settings
            });
            cx.subscribe(&settings, |this, _, event, cx| {
                use crate::settings_preview::SettingsPreviewEvent as E;
                match event {
                    E::Close => this.settings_open = false,
                    E::ReopenSetup => {
                        this.settings_open = false;
                        if this.live.enabled {
                            this.open_setup(true, cx);
                        } else {
                            this.show_setup = true;
                            this.step = 0;
                        }
                    }
                    E::LanguageChanged(code) => this.change_language(code, cx),
                    E::StartUpdate(variant) => this.start_update_download(variant, cx),
                    E::UpdateStatus(status) => this.live.update_status = Some(status.clone()),
                    E::SteamApiKeySaved(key, library) => {
                        this.live.steam_api_key = Some(key.clone());
                        match library {
                            Some(games) => {
                                this.live.library = games.clone();
                                this.live.installed_count =
                                    games.iter().filter(|g| g.installed).count();
                                this.rebuild_display_games(cx);
                            }
                            None => this.load_library(false, cx),
                        }
                    }
                    E::ReloadLibrary => this.load_library(false, cx),
                    E::RefreshLaunchers => this.refresh_steam_status(cx),
                    E::DeveloperMode(value) => {
                        this.developer_mode = *value;
                        this.save_ui_state(cx);
                    }
                    E::LowPerformance(value) => {
                        this.low_performance = *value;
                        this.save_ui_state(cx);
                    }
                    E::DeviceName(name) => this.live.device_name = name.clone(),
                    E::BackendRestarted => {
                        this.check_backend(cx);
                    }
                }
                cx.notify();
            })
            .detach();
            self.settings_preview = Some(settings);
        }
        let palette = self.theme.page_palette();
        let language = self.selected_language;
        let app_state = self.settings_app_state();
        self.settings_preview
            .as_ref()
            .unwrap()
            .update(cx, |settings, cx| {
                settings.palette = palette;
                settings.language = language;
                settings.sync_app_state(app_state);
                settings.open(window, cx);
            });
        self.settings_open = true;
        cx.notify();
    }

    fn settings_app_state(&self) -> crate::settings_preview::AppState {
        crate::settings_preview::AppState {
            live: self.live.enabled,
            steam_api_key: self.live.steam_api_key.clone(),
            device_name: self.live.device_name.clone(),
            wine_steam_installed: self.live.wine_steam_installed,
            wine_steam_running: self.live.wine_steam_running,
            mac_steam_installed: self.live.mac_steam_installed,
            mac_steam_running: self.live.mac_steam_running,
            backend_connected: self.live.backend_connected,
            backend_version: self.live.backend_version.clone(),
            update_status: self.live.update_status.clone(),
            update_downloading: self.live.update_downloading,
            update_progress: self.live.update_progress,
            update_message: self.live.update_message.clone(),
            developer_mode: self.developer_mode,
            low_performance: self.low_performance,
        }
    }

    fn close_library_menus(&mut self) {
        self.theme_menu_open = false;
        self.tab_menu_open = false;
        self.launcher_menu_open = false;
        self.steam_options_open = false;
        self.settings_open = false;
        self.game_settings_open = false;
        self.pipeline_menu_open = false;
        self.collection_menu = None;
    }

    fn render_pipeline_menu(
        &self,
        game: &LibGame,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
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
        for (index, (id, label)) in library_model::game_pipeline_options(game)
            .into_iter()
            .enumerate()
        {
            let active = self.live.selected_pipeline == id;
            let game = game.clone();
            menu = menu.child(
                div()
                    .id(("pipeline-option", index))
                    .h(px(32.0))
                    .px(px(9.0))
                    .flex()
                    .items_center()
                    .justify_between()
                    .rounded(px(5.0))
                    .bg(rgb(if active {
                        self.theme.menu_hover()
                    } else {
                        self.theme.menu_bg()
                    }))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(self.theme.menu_hover())))
                    .child(label)
                    .child(if active { "✓" } else { "" })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.pipeline_menu_open = false;
                        if this.live.enabled {
                            this.save_pipeline(game.clone(), id.clone(), false, cx);
                        } else {
                            this.live.selected_pipeline = id.clone();
                        }
                        cx.notify();
                    })),
            );
        }
        menu
    }

    fn render_game_settings(
        &self,
        game: &LibGame,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let muted = if self.theme == PreviewTheme::Light {
            0x000000
        } else {
            0x939a9b
        };
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
                                    .text_color(rgb(muted))
                                    .child("GAME SETTINGS"),
                            )
                            .child(game.name.clone()),
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
        let theme = self.theme;
        let option = move |id: (&'static str, usize), text: &'static str, active: bool| {
            div()
                .id(id)
                .px(px(8.0))
                .py(px(6.0))
                .rounded(px(5.0))
                .border_1()
                .border_color(rgb(if active {
                    theme.button_border()
                } else {
                    theme.border()
                }))
                .bg(rgb(if active {
                    theme.menu_hover()
                } else {
                    theme.menu_bg()
                }))
                .cursor_pointer()
                .child(text)
        };
        let row = |label: &'static str, options: gpui::Div| {
            div()
                .flex()
                .items_center()
                .justify_between()
                .gap(px(10.0))
                .child(label)
                .child(options)
        };
        let mut metal_fx = div().flex().gap(px(4.0));
        for (index, (mode, text)) in [("1.75", "1.75×"), ("2.0", "2×"), ("off", "Off")]
            .into_iter()
            .enumerate()
        {
            metal_fx = metal_fx.child(
                option(
                    ("game-setting-metalfx", index),
                    text,
                    self.live.metal_fx_mode == mode,
                )
                .on_click(cx.listener(move |this, _, _, cx| this.set_metal_fx(mode, cx))),
            );
        }
        let mut controller = div().flex().gap(px(4.0));
        for (index, (mode, text)) in [("off", "Off"), ("x", "XInput"), ("d", "DInput")]
            .into_iter()
            .enumerate()
        {
            controller = controller.child(
                option(
                    ("game-setting-controller", index),
                    text,
                    self.live.controller_input == mode,
                )
                .on_click(cx.listener(move |this, _, _, cx| this.set_controller(mode, cx))),
            );
        }
        let msync = self.live.msync;
        menu = menu
            .child(row("MetalFX", metal_fx))
            .child(row("Controller input", controller))
            .child(row(
                "msync",
                div().flex().child(
                    option(
                        ("game-setting-msync", 0),
                        if msync { "On" } else { "Off" },
                        msync,
                    )
                    .on_click(cx.listener(move |this, _, _, cx| this.set_msync(!msync, cx))),
                ),
            ));
        if game.installed && !game.has_native_build {
            let executable_name = game
                .executable_path
                .as_deref()
                .filter(|p| !p.is_empty())
                .and_then(|path| path.rsplit(['/', '\\']).next())
                .map(str::to_owned)
                .unwrap_or_else(|| "Automatically detected".to_string());
            menu = menu.child(
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
                                    .text_color(rgb(muted))
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
                            .on_click(
                                cx.listener(|this, _, _, cx| this.choose_featured_executable(cx)),
                            ),
                    ),
            );
        }
        if !game.is_ubisoft() {
            let active = self.live.steam_emu_active;
            let enabled = game.installed && !self.live.steam_emu_busy;
            menu = menu.child(row(
                "Steam Emu",
                div().flex().child(
                    option(
                        ("game-setting-steam-emu", 0),
                        if active { "On" } else { "Off" },
                        active,
                    )
                    .opacity(if enabled { 1.0 } else { 0.45 })
                    .on_click(cx.listener(move |this, _, _, cx| this.set_steam_emu(!active, cx))),
                ),
            ));
        }
        menu
    }

    fn render_collection_menu(
        &self,
        game: &LibGame,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let current = library_model::effective_pipeline(game);
        let mut menu = div()
            .id(("collection-pipeline-menu", game.appid as usize))
            .occlude()
            .absolute()
            .bottom(px(30.0))
            .left(px(0.0))
            .w(px(150.0))
            .flex()
            .flex_col()
            .gap(px(2.0))
            .p(px(5.0))
            .rounded(px(7.0))
            .border_1()
            .border_color(rgb(self.theme.button_border()))
            .bg(rgb(self.theme.menu_bg()))
            .shadow_lg()
            .text_size(px(11.5))
            .text_color(rgb(self.theme.control_text()));
        for (index, (id, label)) in library_model::game_pipeline_options(game)
            .into_iter()
            .enumerate()
        {
            let active = current == id;
            let game = game.clone();
            menu = menu.child(
                div()
                    .id(("collection-pipeline-option", index))
                    .h(px(28.0))
                    .px(px(8.0))
                    .flex()
                    .items_center()
                    .justify_between()
                    .rounded(px(5.0))
                    .bg(rgb(if active {
                        self.theme.menu_hover()
                    } else {
                        self.theme.menu_bg()
                    }))
                    .cursor_pointer()
                    .hover(|style| style.bg(rgb(self.theme.menu_hover())))
                    .child(label)
                    .child(if active { "✓" } else { "" })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        cx.stop_propagation();
                        this.collection_menu = None;
                        this.save_pipeline(game.clone(), id.clone(), true, cx);
                        cx.notify();
                    })),
            );
        }
        menu
    }

    fn render_collection_tabs(&self, uninstalled_view: bool, cx: &mut Context<Self>) -> gpui::Div {
        let installed = self.live.installed_count;
        let uninstalled = self
            .live
            .library
            .iter()
            .filter(|g| !g.installed && !g.is_ubisoft() && g.appid != 0)
            .count();
        let tab = |id: &'static str, label: String, selected: bool| {
            div()
                .id(id)
                .h(px(32.0))
                .px(px(14.0))
                .flex()
                .items_center()
                .rounded(px(7.0))
                .border_1()
                .border_color(rgba(if selected { 0xefe7d6ff } else { 0xe7eaec3b }))
                .bg(rgb(if selected { 0xefe7d6 } else { 0x1f2325 }))
                .text_size(px(12.5))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(if selected { 0x14161a } else { 0xe1e3e2 }))
                .cursor_pointer()
                .child(label)
        };
        div()
            .mt(px(18.0))
            .flex()
            .gap(px(8.0))
            .child(
                tab(
                    "collection-tab-installed",
                    format!("Installed  {installed}"),
                    !uninstalled_view,
                )
                .on_click(cx.listener(|this, _, _, cx| {
                    this.collection_view = CollectionView::Installed;
                    cx.notify();
                })),
            )
            .child(
                tab(
                    "collection-tab-uninstalled",
                    format!("Uninstalled  {uninstalled}"),
                    uninstalled_view,
                )
                .on_click(cx.listener(|this, _, _, cx| this.show_uninstalled(cx))),
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
        let matches = self.matching_games();
        let uninstalled_view =
            self.live.enabled && self.collection_view == CollectionView::Uninstalled;
        let uninstalled = if uninstalled_view {
            self.uninstalled_games()
        } else {
            Vec::new()
        };
        for (index, game) in uninstalled.iter().enumerate() {
            // Artwork resolves once per game; repeat requests are no-ops.
            self.request_art(game, cx);
            grid = grid.child(self.render_uninstalled_card(game, index, card_width, cx));
        }
        for index in matches.iter().copied().filter(|_| !uninstalled_view) {
            let game = self.games[index].clone();
            let running = self.is_running(game.appid);
            let launching = self.live.launching == Some(game.appid);
            let saving = self.live.collection_saving.contains(&game.appid);
            let art = self.art_path(&game, false, cx);
            let pipeline = library_model::effective_pipeline(&game);
            let pipeline_label = library_model::game_pipeline_options(&game)
                .into_iter()
                .find(|(id, _)| *id == pipeline)
                .map(|(_, label)| label)
                .unwrap_or_else(|| library_model::pipeline_label(&pipeline));
            let menu_open = self.collection_menu == Some(game.key());
            let menu = menu_open
                .then(|| gpui::deferred(self.render_collection_menu(&game, cx)).with_priority(30));
            let play_game = game.clone();
            let key = game.key();
            let badge = if game.is_ubisoft() {
                Some("Ubisoft")
            } else if game.has_native_build {
                Some("Native macOS")
            } else {
                None
            };
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
                    .child(match art {
                        Some(art) => img(art)
                            .w_full()
                            .h_full()
                            .object_fit(ObjectFit::Cover)
                            .into_any_element(),
                        None => div()
                            .size_full()
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(
                                img(asset_path("metalsharp-logo.png"))
                                    .size(px(card_width * 0.4))
                                    .object_fit(ObjectFit::Contain),
                            )
                            .into_any_element(),
                    })
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
                                    .child(game.name.clone()),
                            )
                            .children(badge.map(|badge| {
                                div()
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(rgb(0xd8dad9))
                                    .child(badge)
                            }))
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(6.0))
                                    .children((!game.has_native_build).then(|| {
                                        div()
                                            .relative()
                                            .child(
                                                div()
                                                    .id(("collection-bottle", index))
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
                                                    .opacity(if saving { 0.5 } else { 1.0 })
                                                    .cursor_pointer()
                                                    .on_click(cx.listener(move |this, _, _, cx| {
                                                        cx.stop_propagation();
                                                        if this
                                                            .live
                                                            .collection_saving
                                                            .contains(&key.1)
                                                        {
                                                            return;
                                                        }
                                                        let open =
                                                            this.collection_menu != Some(key);
                                                        this.close_library_menus();
                                                        this.collection_menu = open.then_some(key);
                                                        cx.notify();
                                                    }))
                                                    .child(format!("{pipeline_label}  ⌄")),
                                            )
                                            .children(menu)
                                    }))
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
                                            .border_color(rgb(if running {
                                                0xa52d2d
                                            } else {
                                                accent
                                            }))
                                            .bg(rgb(if running { 0xa52d2d } else { accent }))
                                            .opacity(if launching { 0.6 } else { 1.0 })
                                            .text_size(px(11.0))
                                            .font_weight(FontWeight::BOLD)
                                            .text_color(rgb(if running {
                                                0xffffff
                                            } else {
                                                control_bg
                                            }))
                                            .cursor_pointer()
                                            .on_click(cx.listener(move |this, _, _, cx| {
                                                let running = this.is_running(play_game.appid);
                                                this.select_game(index, cx);
                                                if !running {
                                                    this.active_tab = LibraryTab::Play;
                                                }
                                                this.launch_game(play_game.clone(), cx);
                                                cx.notify();
                                            }))
                                            .child(if running { "■ Stop" } else { "▶ Play" }),
                                    ),
                            ),
                    ),
            );
        }
        let count = if self.live.enabled {
            self.live.installed_count
        } else {
            matches.len()
        };

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
                            .child(if uninstalled_view {
                                "Uninstalled games"
                            } else {
                                "Installed games"
                            }),
                    )
                    .child(
                        div()
                            .mt(px(13.0))
                            .text_size(px(14.0))
                            .text_color(rgb(0xaeb3b2))
                            .child(if uninstalled_view {
                                format!(
                                    "{} games in your Steam library aren't installed yet. Install them with steamcmd.",
                                    uninstalled.len()
                                )
                            } else {
                                format!("{count} games installed and ready in your MetalSharp library.")
                            }),
                    )
                    .when(self.live.enabled, |heading| {
                        heading.child(self.render_collection_tabs(uninstalled_view, cx))
                    })
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
            .child(if uninstalled_view && uninstalled.is_empty() {
                div()
                    .w_full()
                    .py(px(60.0))
                    .flex()
                    .flex_col()
                    .items_center()
                    .gap(px(10.0))
                    .text_color(rgb(0xaeb3b2))
                    .child(div().text_size(px(36.0)).child("▦"))
                    .child(
                        div()
                            .text_size(px(20.0))
                            .text_color(rgb(0xeee9dd))
                            .child("No uninstalled games"),
                    )
                    .child(div().text_size(px(13.0)).child(
                        "Everything you own is installed, or add your Steam API key in Settings to load your full Steam library.",
                    ))
            } else if !uninstalled_view && matches.is_empty() {
                div()
                    .w_full()
                    .py(px(60.0))
                    .flex()
                    .flex_col()
                    .items_center()
                    .gap(px(10.0))
                    .text_color(rgb(0xaeb3b2))
                    .child(div().text_size(px(36.0)).child("▦"))
                    .child(
                        div()
                            .text_size(px(20.0))
                            .text_color(rgb(0xeee9dd))
                            .child("No installed games found"),
                    )
                    .child(
                        div()
                            .text_size(px(13.0))
                            .child("Install a Steam game and refresh the library to see it here."),
                    )
            } else {
                grid
            })
    }
}

impl MetalSharpApp {
    fn render_streaming_overlay(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let live = self.live.enabled;
        let stream = &self.live.streaming;
        let status = stream.status.as_ref();
        let installed = if live {
            status.is_some_and(|status| status.installed)
        } else {
            self.streaming_installed
        };
        let running = if live {
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
        let state_label = if live && status.is_none() {
            "Checking…".to_owned()
        } else if installing {
            status
                .and_then(|s| s.progress_status.clone())
                .unwrap_or_else(|| "Installing…".into())
        } else if running {
            "Connected".into()
        } else if installed {
            "Installed — Offline".into()
        } else {
            "Not installed".into()
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
                        .child(state_label),
                ),
        );
        if !installed {
            host = host.child(div().mb(px(12.0)).line_height(px(19.4)).child("Sunshine captures this Mac's screen and streams it over your local network. Install it once — about a 40 MB download from LizardByte."));
        }
        let mut actions = div().flex().flex_wrap().gap(px(9.0));
        if !installed && !installing {
            actions = actions.child(
                button(
                    "stream-install",
                    if stream.installing {
                        "Starting download…"
                    } else {
                        "↓  Install Sunshine"
                    },
                    true,
                )
                .on_click(cx.listener(|this, _, _, cx| {
                    if this.live.enabled {
                        this.streaming_install(cx);
                    } else {
                        this.streaming_installed = true;
                    }
                    cx.notify();
                })),
            );
        } else if !running && !installing {
            actions = actions.child(
                button(
                    "stream-start",
                    if stream.launching {
                        "Starting…"
                    } else {
                        "▶  Start Streaming Host"
                    },
                    true,
                )
                .on_click(cx.listener(|this, _, _, cx| {
                    if this.live.enabled {
                        this.streaming_launch(cx);
                    } else {
                        this.streaming_running = true;
                    }
                    cx.notify();
                })),
            );
        } else if running {
            actions =
                actions
                    .child(
                        button("stream-stop", "■  Stop", false).on_click(cx.listener(
                            |this, _, _, cx| {
                                if this.live.enabled {
                                    this.streaming_stop(cx);
                                } else {
                                    this.streaming_running = false;
                                }
                                cx.notify();
                            },
                        )),
                    )
                    .child(button("stream-web", "↗  Sunshine Web UI", false).on_click(
                        cx.listener(|this, _, _, cx| {
                            let url = this
                                .live
                                .streaming
                                .status
                                .as_ref()
                                .map(|s| s.web_url.clone())
                                .filter(|u| !u.is_empty())
                                .unwrap_or_else(|| "https://localhost:47990".into());
                            cx.open_url(&url);
                        }),
                    ));
        }
        host = host.child(actions);
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
                    .child(format!(
                        "Version {} · {}",
                        status
                            .map(|status| status.version.as_str())
                            .filter(|version| !version.is_empty())
                            .unwrap_or("unknown"),
                        status
                            .map(|status| status.web_url.as_str())
                            .filter(|url| !url.is_empty())
                            .unwrap_or("https://localhost:47990")
                    )),
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
        let ready_to_pair = status.is_some_and(|s| s.running && s.creds_valid);
        let pin = div()
            .w(px(110.0))
            .h(px(36.0))
            .px(px(10.0))
            .flex()
            .items_center()
            .rounded(px(8.0))
            .border_1()
            .border_color(rgba(0xffffff24))
            .bg(rgba(0xffffff0a))
            .children(self.streaming_pin.clone());
        let mut pairing = card()
            .opacity(if running { 1.0 } else { 0.55 })
            .child(div().mb(px(10.0)).child(title("Pair your device")))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(5.0))
                    .mb(px(14.0))
                    .line_height(px(20.6))
                    .child(div().child(moonlight_line))
                    .child(
                        div()
                            .flex()
                            .gap(px(9.))
                            .child(button("stream-app-store", "↗ App Store", false).on_click(cx.listener(
                                |_, _, _, cx| {
                                    cx.open_url("https://apps.apple.com/app/moonlight-game-streaming/id1000551566")
                                },
                            )))
                            .child(button("stream-play-store", "↗ Google Play", false).on_click(cx.listener(
                                |_, _, _, cx| {
                                    cx.open_url("https://play.google.com/store/apps/details?id=com.limelight")
                                },
                            ))),
                    )
                    .child("2. Start playing your game in MetalSharp on this Mac.")
                    .child("3. Open Moonlight and tap this Mac — it shows a 4-digit PIN.")
                    .child("4. Enter the PIN below to pair, then tap the game in Moonlight to start streaming."),
            )
            .child(
                div().flex().gap(px(9.0)).child(pin).child(
                    button(
                        "stream-pair",
                        if stream.pairing { "Pairing…" } else { "Pair Device" },
                        true,
                    )
                    .opacity(if ready_to_pair { 1.0 } else { 0.5 })
                    .on_click(cx.listener(|this, _, _, cx| {
                        let ready = this
                            .live
                            .streaming
                            .status
                            .as_ref()
                            .is_some_and(|s| s.running && s.creds_valid);
                        if this.live.enabled && ready {
                            this.streaming_pair(cx);
                        }
                    })),
                ),
            );
        if let Some(status) = status {
            pairing = pairing.child(div().mt(px(10.)).child(
                if status.running && !status.creds_valid {
                    "Host is starting — pairing credentials are not ready yet".into()
                } else if status.pairing_count > 0 {
                    format!(
                        "{} waiting to pair: {}",
                        status.pairing_count, status.pairings_summary
                    )
                } else {
                    String::new()
                },
            ));
        }
        let mut notes = card()
            .child(div().mb(px(10.0)).child(title("Good to know")))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(5.0))
                    .text_size(px(12.0))
                    .line_height(px(20.4))
                    .child("• Sunshine on macOS is experimental: gamepads aren't supported yet — use touch controls in Moonlight.")
                    .child("• macOS asks for Screen Recording permission the first time you stream. Approve it once.")
                    .child("• Keep both devices on the same network; ports 47984–48010 must be reachable."),
            );
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
                    if this.streaming_unpair_confirm
                        && this
                            .streaming_unpair_deadline
                            .is_some_and(|deadline| std::time::Instant::now() < deadline)
                    {
                        this.streaming_unpair_confirm = false;
                        this.streaming_unpair_all(cx);
                    } else {
                        this.streaming_unpair_confirm = true;
                        this.streaming_unpair_deadline =
                            Some(std::time::Instant::now() + std::time::Duration::from_secs(10));
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
        div()
            .id("streaming-overlay")
            .occlude()
            .absolute()
            .top(px(0.0))
            .left(px(0.0))
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .p(px(24.0))
            .bg(rgba(0x040608b8))
            .on_click(cx.listener(|this, _, _, cx| this.close_streaming_panel(cx)))
            .child(
                div()
                    .id("streaming-panel")
                    .occlude()
                    .w(px(680.0))
                    .max_w_full()
                    .h(px((f32::from(viewport.height) * 0.86).min(680.0)))
                    .flex()
                    .flex_col()
                    .rounded(px(16.0))
                    .overflow_hidden()
                    .border_1()
                    .border_color(rgba(0xffffff17))
                    .bg(rgb(0x14171a))
                    .shadow_lg()
                    .text_size(px(12.5))
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .child(
                        div()
                            .flex_none()
                            .flex()
                            .items_center()
                            .justify_between()
                            .gap(px(14.0))
                            .py(px(18.0))
                            .px(px(22.0))
                            .border_b_1()
                            .border_color(rgba(0xffffff12))
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(13.0))
                                    .child(
                                        div()
                                            .flex_none()
                                            .size(px(40.0))
                                            .flex()
                                            .items_center()
                                            .justify_center()
                                            .rounded(px(11.0))
                                            .border_1()
                                            .border_color(rgba(0x74d2c859))
                                            .bg(rgba(0x74d2c814))
                                            .child(
                                                gpui::svg()
                                                    .path("stream-tv.svg")
                                                    .size(px(20.0))
                                                    .text_color(rgb(0x74d2c8)),
                                            ),
                                    )
                                    .child(
                                        div()
                                            .min_w_0()
                                            .flex()
                                            .flex_col()
                                            .gap(px(2.0))
                                            .child(
                                                div()
                                                    .text_size(px(18.0))
                                                    .font_weight(FontWeight::SEMIBOLD)
                                                    .text_color(rgb(0xeef0ef))
                                                    .child("Game Streaming"),
                                            )
                                            .child(div().text_size(px(12.0)).text_color(rgb(0x989e9c)).child(
                                                "Stream your MetalSharp games to a phone or tablet with Sunshine + Moonlight",
                                            )),
                                    ),
                            )
                            .child(
                                div()
                                    .id("stream-close")
                                    .flex_none()
                                    .size(px(30.0))
                                    .rounded(px(8.0))
                                    .border_1()
                                    .border_color(rgba(0xffffff24))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .text_size(px(20.0))
                                    .text_color(rgba(0xffffff99))
                                    .cursor_pointer()
                                    .child("×")
                                    .on_click(cx.listener(|this, _, _, cx| this.close_streaming_panel(cx))),
                            ),
                    )
                    .child(
                        div()
                            .id("streaming-body")
                            .min_h_0()
                            .flex_1()
                            .overflow_y_scroll()
                            .flex()
                            .flex_col()
                            .gap(px(14.0))
                            .pt(px(18.0))
                            .px(px(22.0))
                            .pb(px(22.0))
                            .child(host)
                            .child(pairing)
                            .child(notes),
                    ),
            )
    }
}

impl MetalSharpApp {
    fn render_setup_page(&mut self, cx: &mut Context<Self>) -> gpui::Div {
        let selected_language = self.selected_language;
        let copy = self.copy.clone();
        let current_step = self.step.min(2);
        let language_menu_open = self.language_menu_open;
        let dismissible = self.live.setup.dismissible;
        let selected_name = LANGUAGES
            .iter()
            .find(|(code, _)| *code == selected_language)
            .map(|(_, name)| *name)
            .unwrap_or("English");
        let mut language_picker = div()
            .absolute()
            .top(px(18.0))
            .right(px(if dismissible { 60.0 } else { 18.0 }))
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
                            this.change_language(code, cx);
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
        let page_body = match current_step {
            0 => render_welcome_body(&copy),
            1 => self.render_runtime_body(cx, copy.clone()),
            _ => render_done_body(&copy, self.setup_inputs.clone(), cx),
        };
        let finishing = self.live.setup.finishing;
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
                    // Electron's Next is never disabled (SetupWizard.vue goToDoneStep).
                    div()
                        .id("setup-next")
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
                            if this.live.enabled {
                                this.setup_go_to_done(cx);
                            } else {
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
                    .opacity(if finishing { 0.5 } else { 1.0 })
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
                        if this.live.enabled {
                            this.setup_finish(cx);
                        } else {
                            this.show_setup = false;
                        }
                        cx.notify();
                    }))
                    .child(if finishing {
                        copy.preparing_steam.clone()
                    } else {
                        copy.launch.clone()
                    }),
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
            .child(page_actions)
            .child(language_picker)
            .children(dismissible.then(|| {
                div()
                    .id("setup-close")
                    .absolute()
                    .top(px(18.0))
                    .right(px(18.0))
                    .size(px(32.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(rgb(0x343638))
                    .bg(rgb(0x1a1d20))
                    .text_color(rgb(0xc5c5c1))
                    .cursor_pointer()
                    .on_click(cx.listener(|this, _, _, cx| this.setup_close(cx)))
                    .child("✕")
            }))
    }

    fn render_runtime_body(&mut self, cx: &mut Context<Self>, copy: SetupCopy) -> gpui::Div {
        let flow = &self.live.setup;
        let runtime_ready = flow.runtime_ready();
        let runtime_installing = flow.installing;
        let runtime_progress = flow.install_progress;
        let install_log_open = flow.log_open;
        let has_logs = !flow.install_logs.is_empty();
        let steam_installed = flow.steam_installed;
        let steam_installing = flow.steam_installing;
        // SetupWizard.vue installButtonLabel.
        let runtime_label = if runtime_ready {
            copy.install_complete.clone()
        } else if runtime_installing {
            // Polled /setup/install-progress: overall percent plus the current step.
            if flow.install_current.is_empty() {
                format!("Installing Runtime… {runtime_progress}%")
            } else {
                format!(
                    "Installing Runtime… {runtime_progress}% · {}",
                    flow.install_current
                )
            }
        } else if flow.install_failed {
            copy.install_failed.clone()
        } else {
            copy.install_runtime.clone()
        };
        // steamButtonLabel / steamInstallLabel.
        let steam_label = if steam_installed {
            copy.steam_installed.clone()
        } else if steam_installing {
            match flow.steam_install_stage.as_str() {
                "downloading" => copy.downloading_steam.clone(),
                "creating-steam-prefix" => copy.creating_steam_prefix.clone(),
                "installing-steam" => copy.installing_steam.clone(),
                "failed" => copy.retry_steam.clone(),
                _ => copy.preparing_steam.clone(),
            }
        } else if flow.steam_failed {
            copy.steam_failed.clone()
        } else {
            copy.install_steam.clone()
        };
        let logs: Vec<(String, LogClass)> = flow.install_logs.clone();

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
                                    !runtime_installing && !runtime_ready,
                                )
                                .on_click(cx.listener(
                                    |this, _, _, cx| {
                                        this.start_runtime_install(cx);
                                    },
                                )),
                            )
                            .child(if has_logs {
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
                                        this.live.setup.log_open = !this.live.setup.log_open;
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
                                render_install_button(
                                    "install-steam",
                                    steam_label,
                                    steam_installing,
                                    steam_installed,
                                    0,
                                    runtime_ready && !steam_installing && !steam_installed,
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
            .child(if install_log_open && has_logs {
                let mut log = div()
                    .id("setup-install-log")
                    .mt(px(14.0))
                    .max_h(px(160.0))
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
                    .flex()
                    .flex_col();
                for (text, class) in logs {
                    let color = match class {
                        LogClass::Success => 0x7cbf6a,
                        LogClass::Warn => 0xd8a84b,
                        LogClass::Error => 0xd66a6a,
                        LogClass::Active => 0xefe6d3,
                        LogClass::Info => 0x9aa09e,
                    };
                    log = log.child(div().text_color(rgb(color)).child(text));
                }
                log
            } else {
                div().id("setup-install-log-placeholder").h(px(0.0))
            })
    }

    fn start_runtime_install(&mut self, cx: &mut Context<Self>) {
        if self.live.enabled {
            self.setup_start_install(cx);
            return;
        }
        let flow = &mut self.live.setup;
        if flow.installing || flow.runtime_ready() {
            return;
        }
        // Offline preview only: installation progress is simulated.
        flow.installing = true;
        flow.install_progress = 0;
        flow.install_logs
            .push(("Starting installation...".into(), LogClass::Info));
        cx.notify();
        cx.spawn(async move |this, cx| {
            for progress in [12, 27, 46, 68, 84, 100] {
                cx.background_executor()
                    .timer(std::time::Duration::from_millis(450))
                    .await;
                let _ = this.update(cx, |this, cx| {
                    this.live.setup.install_progress = progress;
                    cx.notify();
                });
            }
            let _ = this.update(cx, |this, cx| {
                this.live.setup.installing = false;
                this.live.setup.install_status = "complete".into();
                cx.notify();
            });
        })
        .detach();
    }

    fn start_steam_install(&mut self, cx: &mut Context<Self>) {
        if self.live.enabled {
            self.setup_install_steam(cx);
            return;
        }
        let flow = &mut self.live.setup;
        if !flow.runtime_ready() || flow.steam_installing || flow.steam_installed {
            return;
        }
        flow.steam_installing = true;
        cx.notify();
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .timer(std::time::Duration::from_secs(2))
                .await;
            let _ = this.update(cx, |this, cx| {
                this.live.setup.steam_installing = false;
                this.live.setup.steam_installed = true;
                cx.notify();
            });
        })
        .detach();
    }
}

pub(crate) fn open_help(
    purpose: crate::mini_browser::BrowserPurpose,
    url: &'static str,
    cx: &mut App,
) {
    #[cfg(target_os = "macos")]
    {
        if let (Ok(request), Some(mtm)) = (
            crate::mini_browser::MiniBrowserRequest::new(purpose, url, "MetalSharp account help"),
            objc2::MainThreadMarker::new(),
        ) {
            if crate::mini_browser::open_native(mtm, request, Box::new(|_| {})).is_ok() {
                return;
            }
        }
    }
    let _ = purpose;
    cx.open_url(url);
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
    _cx: &mut Context<MetalSharpApp>,
) -> gpui::Div {
    let tip = |title: String, text: String| {
        div()
            .text_size(px(12.5))
            .line_height(px(19.0))
            .text_color(rgb(MUTED))
            .child(
                gpui::StyledText::new(format!("{title} — {text}")).with_highlights(vec![(
                    0..title.len(),
                    gpui::HighlightStyle {
                        color: Some(rgb(0xeceae3).into()),
                        font_weight: Some(FontWeight::SEMIBOLD),
                        ..Default::default()
                    },
                )]),
            )
    };
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
                    Some((
                        crate::mini_browser::BrowserPurpose::SteamApiKeyHelp,
                        "https://steamcommunity.com/dev/apikey",
                    )),
                ))
                .child(render_done_form_group(
                    copy.the_games_db_api_key.clone(),
                    copy.the_games_db_api_placeholder.clone(),
                    format!("{} TheGamesDB", copy.the_games_db_api_hint),
                    "thegamesdb-api-key",
                    fields.as_ref().map(|fields| fields[2].clone()),
                    Some((
                        crate::mini_browser::BrowserPurpose::TheGamesDbHelp,
                        "https://api.thegamesdb.net/key.php",
                    )),
                )),
        )
        .child(
            div()
                .mt(px(20.0))
                .flex()
                .flex_col()
                .gap(px(8.0))
                .child(tip(copy.start_steam.clone(), copy.start_steam_text.clone()))
                .child(tip(
                    copy.first_launch.clone(),
                    copy.first_launch_text.clone(),
                )),
        )
}

fn render_done_form_group(
    label: String,
    placeholder: String,
    hint: String,
    id: &'static str,
    input: Option<gpui::Entity<crate::search_input::SearchInput>>,
    help: Option<(crate::mini_browser::BrowserPurpose, &'static str)>,
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
                .text_color(rgb(0xeceae3))
                .child(field),
        )
        .child(
            div()
                .id(gpui::SharedString::from(format!("{id}-hint")))
                .text_size(px(11.5))
                .text_color(rgb(0x838987))
                .when(help.is_some(), |style| style.cursor_pointer().underline())
                .on_click(move |_, _, cx| {
                    if let Some((purpose, url)) = help {
                        open_help(purpose, url, cx);
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
    enabled: bool,
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
        .opacity(if enabled || complete { 1.0 } else { 0.45 })
        .text_size(px(14.0))
        .font_weight(FontWeight::BOLD)
        .text_color(rgb(if complete { 0xffffff } else { 0x14161a }));
    if enabled {
        button = button.cursor_pointer();
    }
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
        assert!(!app.live.enabled);
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
        for theme in super::PreviewTheme::ALL {
            assert!(!theme.storage_id().is_empty());
        }
    }

    #[test]
    fn search_filters_games_and_scrolls_only_matching_results() {
        let mut app = MetalSharpApp::new();
        app.search_query = "  PORTAL  ".into();
        assert_eq!(app.matching_games(), vec![5]);
        app.search_query = "no game matches this".into();
        assert!(app.matching_games().is_empty());
        app.search_query.clear();
        assert_eq!(app.matching_games().len(), super::LIBRARY_GAMES.len());
    }

    #[test]
    fn sample_library_has_twenty_distinct_games_with_bundled_artwork() {
        let assets = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("assets");
        let mut names = std::collections::HashSet::new();
        for (name, cover, hero) in super::LIBRARY_GAMES {
            assert!(names.insert(name), "duplicate game name: {name}");
            for file in [cover, hero] {
                let bytes = std::fs::read(assets.join(file)).expect("bundled image missing");
                assert!(bytes.starts_with(&[0xff, 0xd8]), "not a JPEG: {file}");
            }
        }
        assert_eq!(names.len(), 20);
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
        assert_eq!(
            (0..5).map(super::dock_card_angle).collect::<Vec<_>>(),
            [-6, -3, 0, 3, 6]
        );
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
            assert!(
                !copy.runtime_lede.is_empty(),
                "missing runtime copy for {code}"
            );
            assert!(
                !copy.steam_install_timed_out.is_empty(),
                "missing steam timeout for {code}"
            );
            assert!(
                !copy.downloading_steam.is_empty(),
                "missing steam stage for {code}"
            );
            assert!(!copy.launch.is_empty(), "missing launch label for {code}");
            assert_eq!(copy.steps.len(), 3, "invalid step labels for {code}");
        }
    }
}
