use crate::page_palette::PagePalette;
use gpui::{Context, FontWeight, ObjectFit, Render, Window, div, img, prelude::*, px, rgb};
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
};

#[path = "sharp_emu_live.rs"]
mod sharp_emu_live;
#[path = "sharp_live.rs"]
mod sharp_live;
#[path = "sharp_live_view.rs"]
mod sharp_live_view;
#[path = "sharp_tools.rs"]
mod sharp_tools;
#[path = "sharp_tools_view.rs"]
mod sharp_tools_view;

const BG: u32 = 0x111416;
const PANEL: u32 = 0x121518;
const TEXT: u32 = 0xf1eee6;
const MUTED: u32 = 0x9ba19f;
const GOLD: u32 = 0xe8d6b7;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SharpSource {
    Installers,
    Gog,
    Epic,
    GameJolt,
    Pcsx2,
    Rpcs3,
    ShadPs4,
    SharpEmu,
}
impl SharpSource {
    pub const ALL: [Self; 8] = [
        Self::Installers,
        Self::Gog,
        Self::Epic,
        Self::GameJolt,
        Self::Pcsx2,
        Self::Rpcs3,
        Self::ShadPs4,
        Self::SharpEmu,
    ];
    pub fn id(self) -> &'static str {
        match self {
            Self::Installers => "installers",
            Self::Gog => "gog",
            Self::Epic => "epic",
            Self::GameJolt => "gamejolt",
            Self::Pcsx2 => "pcsx2",
            Self::Rpcs3 => "rpcs3",
            Self::ShadPs4 => "shadps4",
            Self::SharpEmu => "sharpemu",
        }
    }
    fn icon(self, color: u32, size: f32) -> gpui::Svg {
        gpui::svg()
            .path(format!("source-{}.svg", self.id()))
            .size(px(size))
            .text_color(rgb(color))
    }
    fn index(self) -> usize {
        Self::ALL.iter().position(|s| *s == self).unwrap()
    }
    fn title(self) -> &'static str {
        match self {
            Self::Installers => "Installers",
            Self::Gog => "GOG",
            Self::Epic => "Epic",
            Self::GameJolt => "GameJolt",
            Self::Pcsx2 => "PCSX2",
            Self::Rpcs3 => "RPCS3",
            Self::ShadPs4 => "shadPS4",
            Self::SharpEmu => "SharpEmu",
        }
    }
    fn menu_detail(self) -> &'static str {
        match self {
            Self::Installers => "Windows applications",
            Self::Gog => "GOG games library",
            Self::Epic => "Epic games library",
            Self::GameJolt => "Indie games library",
            Self::Pcsx2 => "PS2 Emulation",
            Self::Rpcs3 => "PS3 Emulation",
            Self::ShadPs4 => "PS4 Emulation",
            Self::SharpEmu => "PS5 Emulation",
        }
    }
    fn header(self) -> (&'static str, &'static str, &'static str) {
        let e = "METALSHARP · INSTALLERS & EMULATORS";
        match self {
            Self::Installers => (
                e,
                "Sharp Library",
                "Install and manage Windows applications outside Steam.",
            ),
            Self::Gog => (e, "GOG Library", "GOG games library"),
            Self::Epic => (e, "Epic Library", "Epic games library"),
            Self::GameJolt => (e, "GameJolt Library", "Indie games library"),
            Self::Pcsx2 => (
                e,
                "PCSX2 Library",
                "Official stable PCSX2, isolated from your normal home folder. MetalSharp never downloads a Sony BIOS or games and preserves all mutable emulator data across updates and runtime removal.",
            ),
            Self::Rpcs3 => (
                e,
                "RPCS3 Library",
                "A verified, isolated emulator environment with atomic updates and protected user data.",
            ),
            Self::ShadPs4 => (
                e,
                "shadPS4 Library",
                "Official stable core, isolated state, atomic rollback, and protected user-owned content. Compatibility remains experimental.",
            ),
            Self::SharpEmu => (
                e,
                "SharpEmu Library",
                "SharpEmu is early-stage research software. Most games do not run, Windows is upstream’s primary target, and macOS support is experimental.",
            ),
        }
    }
}

#[derive(Clone, Debug)]
struct PreviewState {
    source: SharpSource,
    picker: bool,
    dialog: Option<&'static str>,
    notice: String,
    samples: [bool; 8],
    installed: [bool; 8],
    authenticated: [bool; 8],
    runtime_ready: [bool; 8],
    running: [bool; 8],
    network_opt_in: [bool; 8],
    bios_ready: [bool; 8],
    settings: [[usize; 3]; 8],
    sidebar_open: [[bool; 3]; 8],
    roots: [bool; 8],
    pinned: [bool; 8],
    modules: [usize; 8],
    fonts: [usize; 8],
    running_app: [Option<&'static str>; 8],
    game_installed: [HashSet<&'static str>; 8],
    game_removed: [HashSet<&'static str>; 8],
    bottle_open: [HashSet<&'static str>; 8],
    card_engines: [HashMap<&'static str, usize>; 8],
    card_picker: Option<&'static str>,
    selected_card: Option<&'static str>,
    launch_settings_open: bool,
    launch_preferences: [usize; 5],
    resolution_open: bool,
    browser_height: f32,
    browser_drag: Option<(f32, f32)>,
}
impl Default for PreviewState {
    fn default() -> Self {
        Self {
            source: SharpSource::Installers,
            picker: false,
            dialog: None,
            notice: "Preview data only · no files, accounts, or services are accessed".into(),
            samples: [false; 8],
            installed: [false; 8],
            authenticated: [false; 8],
            runtime_ready: [false; 8],
            running: [false; 8],
            network_opt_in: [false; 8],
            bios_ready: [false; 8],
            settings: [[0; 3]; 8],
            sidebar_open: [[false; 3]; 8],
            roots: [false; 8],
            pinned: [false; 8],
            modules: [0; 8],
            fonts: [0; 8],
            running_app: [None; 8],
            game_installed: std::array::from_fn(|_| HashSet::new()),
            game_removed: std::array::from_fn(|_| HashSet::new()),
            bottle_open: std::array::from_fn(|_| HashSet::new()),
            card_engines: std::array::from_fn(|_| HashMap::new()),
            card_picker: None,
            selected_card: None,
            launch_settings_open: false,
            launch_preferences: [1, 0, 1, 0, 0],
            resolution_open: false,
            browser_height: 24.0,
            browser_drag: None,
        }
    }
}
impl PreviewState {
    fn choose(&mut self, source: SharpSource) {
        self.source = source;
        self.picker = false;
        self.launch_settings_open = false;
        self.resolution_open = false;
        self.card_picker = None;
        self.selected_card = None;
        self.dialog = None;
        self.notice = format!("{} sample selected · preview only", source.title());
    }
    fn confirm_dialog(&mut self, kind: &'static str) {
        self.dialog = None;
        let action = match kind {
            "login" => Some("login"),
            "firmware" => Some("bios"),
            "install" => Some("install"),
            "installer" | "folder" | "package" => Some("add_sample"),
            "modules" | "fonts" | "remove" | "game-install" | "game-remove" => Some(kind),
            _ => None,
        };
        if let Some(action) = action {
            self.act(action);
        }
        self.selected_card = None;
    }
    fn act(&mut self, action: &'static str) {
        let i = self.source.index();
        match action {
            "install" | "initialize" => {
                self.installed[i] = true;
                self.runtime_ready[i] = true;
                self.notice = "Simulated setup complete · no installer was run".into()
            }
            "login" => {
                self.authenticated[i] = true;
                self.notice = "Sample account connected · no sign-in or network request".into()
            }
            "logout" => {
                self.authenticated[i] = false;
                self.notice = "Sample account disconnected".into()
            }
            "play" => {
                self.running[i] = true;
                self.notice = "Simulated launch · no executable was opened".into()
            }
            "stop" => {
                self.running[i] = false;
                self.notice = "Sample session stopped".into()
            }
            "bios" => {
                self.bios_ready[i] = true;
                self.notice = "Sample BIOS validated · no file was read".into()
            }
            "refresh" => {
                self.notice = "Sample library refreshed · no local library was scanned".into()
            }
            "sync" => {
                self.samples[i] = true;
                self.notice = "Preview library synchronized · no account or network was used".into()
            }
            "add_sample" => {
                self.samples[i] = true;
                self.roots[i] = true;
                self.notice = "Sample location added · no directory was accessed".into();
            }
            "game-install" => {
                if let Some(title) = self.selected_card {
                    self.game_installed[i].insert(title);
                    self.game_removed[i].remove(title);
                }
                self.notice = "Sample game installed · no downloads or disk writes".into();
            }
            "game-remove" => {
                if let Some(title) = self.selected_card {
                    self.game_removed[i].insert(title);
                    self.game_installed[i].remove(title);
                }
                self.notice = "Sample game uninstalled · no files deleted".into();
            }
            "modules" => {
                self.modules[i] = 2;
                self.notice = "Two sample modules · no files were accessed".into();
            }
            "fonts" => {
                self.fonts[i] = 3;
                self.notice = "Three sample fonts · no files were accessed".into();
            }
            "remove" => {
                self.installed[i] = false;
                self.runtime_ready[i] = false;
                self.running[i] = false;
                self.running_app[i] = None;
                self.notice = "Sample runtime removed; fixtures preserved · no disk changes".into();
            }
            "sample" => {
                self.samples[i] = !self.samples[i];
                self.notice = "Preview fixture visibility changed".into()
            }
            _ => self.notice = format!("{} preview action · no external operation", action),
        }
    }
}

pub struct SharpPreview {
    pub palette: PagePalette,
    pub asset_root: PathBuf,
    state: PreviewState,
    focus: Option<gpui::FocusHandle>,
    live: Option<sharp_live::SharpLive>,
}
impl SharpPreview {
    pub fn new() -> Self {
        Self {
            palette: PagePalette::default(),
            asset_root: PathBuf::new(),
            state: PreviewState::default(),
            focus: None,
            live: None,
        }
    }
    fn source_menu(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let source = self.state.source;
        let mut options = div()
            .id("sharp-source-list")
            .max_h(px((f32::from(viewport.height) * 0.62).min(440.0)))
            .overflow_y_scroll()
            .flex()
            .flex_wrap()
            .gap(px(8.));
        for choice in SharpSource::ALL {
            let selected = choice == source;
            options = options.child(
                div()
                    .id(choice.id())
                    .w(px(218.))
                    .min_h(px(64.))
                    .flex()
                    .items_center()
                    .gap(px(9.))
                    .p(px(8.))
                    .rounded(px(10.))
                    .border_1()
                    .border_color(rgb(if selected {
                        self.palette.accent
                    } else {
                        self.palette.border
                    }))
                    .bg(rgb(if selected {
                        if self.palette.light {
                            0xf0eee8
                        } else {
                            0x25231f
                        }
                    } else {
                        self.palette.menu_bg
                    }))
                    .cursor_pointer()
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.state.choose(choice);
                        if this.live.is_some() {
                            this.state.notice.clear();
                            if choice == SharpSource::Epic {
                                this.refresh_epic(false, cx);
                            } else if sharp_emu_live::emu_index(choice).is_some() {
                                this.emu_refresh(choice, false, cx);
                                let has_update =
                                    sharp_emu_live::emu_index(choice).is_some_and(|i| {
                                        this.live.as_ref().unwrap().emu[i].update.is_some()
                                    });
                                if !has_update {
                                    this.emu_check_update(choice, false, false, |_, _, _| {}, cx);
                                }
                            }
                        }
                        cx.notify();
                    }))
                    .child(
                        div()
                            .w(px(32.))
                            .h(px(32.))
                            .rounded(px(9.))
                            .flex()
                            .items_center()
                            .justify_center()
                            .bg(rgb(if self.palette.light {
                                0xe9e5dc
                            } else {
                                0x29261f
                            }))
                            .text_color(rgb(self.palette.accent))
                            .child(choice.icon(self.palette.accent, 16.0)),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(3.))
                            .child(
                                div()
                                    .text_size(px(12.))
                                    .text_color(rgb(self.palette.control_text))
                                    .font_weight(FontWeight::BOLD)
                                    .child(choice.title()),
                            )
                            .child(
                                div()
                                    .text_size(px(10.))
                                    .text_color(rgb(if self.palette.light {
                                        0x686d70
                                    } else {
                                        MUTED
                                    }))
                                    .child(choice.menu_detail()),
                            ),
                    )
                    .child(if selected {
                        div().text_color(rgb(self.palette.accent)).child("✓")
                    } else {
                        div()
                    }),
            );
        }
        let fixture_label = if self.state.samples[source.index()] {
            "Hide preview-only sample fixture"
        } else {
            "Show preview-only sample fixture"
        };
        let fixture = self
            .palette
            .button("source-preview-fixture", fixture_label, false)
            .on_click(cx.listener(|this, _, _, cx| {
                this.state.act("sample");
                cx.notify();
            }));
        let overlay = div()
            .id("sharp-source-picker-overlay")
            .occlude()
            .absolute()
            .top(px(52.0))
            .right(px(0.0))
            .w(px(470.))
            .max_w(px((f32::from(viewport.width) - 36.0).min(470.0)))
            .rounded(px(14.))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(rgb(self.palette.menu_bg))
            .shadow_lg()
            .p(px(12.))
            .flex()
            .flex_col()
            .gap(px(9.))
            .child(
                div()
                    .flex()
                    .justify_between()
                    .items_center()
                    .pb(px(8.))
                    .border_b_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .child(
                        div()
                            .text_size(px(11.))
                            .text_color(rgb(self.palette.control_text))
                            .font_weight(FontWeight::BOLD)
                            .child("Sharp Source"),
                    )
                    .child(
                        div()
                            .text_size(px(10.))
                            .text_color(rgb(if self.palette.light { 0x686d70 } else { MUTED }))
                            .child("Choose a collection"),
                    ),
            )
            .child(options)
            .children(self.live.is_none().then_some(fixture));
        overlay
    }
    fn launch_settings_control(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let mut control = div().relative().flex_none().child(
            div()
                .id("sharp-launch-settings-trigger")
                .size(px(42.0))
                .flex()
                .items_center()
                .justify_center()
                .rounded(px(10.0))
                .border_1()
                .border_color(gpui::rgba(self.palette.control_border))
                .bg(rgb(self.palette.control_bg))
                .cursor_pointer()
                .child(
                    gpui::svg()
                        .path("launch-settings.svg")
                        .size(px(18.0))
                        .text_color(rgb(self.palette.control_text)),
                )
                .on_click(cx.listener(|this, _, _, cx| {
                    this.state.launch_settings_open = !this.state.launch_settings_open;
                    this.state.picker = false;
                    this.state.resolution_open = false;
                    if this.state.launch_settings_open && this.live.is_some() {
                        this.load_launch_preferences(cx);
                    }
                    cx.notify();
                })),
        );
        if !self.state.launch_settings_open {
            return control;
        }
        let muted = if self.palette.light {
            0x68727b
        } else {
            0xaab3bc
        };
        let mut panel = div()
            .id("sharp-launch-settings-popover")
            .max_h(px((f32::from(viewport.height) - 350.0).max(180.0)))
            .overflow_y_scroll()
            .occlude()
            .absolute()
            .top(px(52.0))
            .right_0()
            .w(px(390.0))
            .rounded(px(14.0))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(rgb(self.palette.menu_bg))
            .p(px(16.0))
            .shadow_lg()
            .flex()
            .flex_col()
            .gap(px(12.0))
            .text_color(rgb(self.palette.control_text))
            .child(
                div()
                    .flex()
                    .justify_between()
                    .items_center()
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(muted))
                                    .child("GAME SETTINGS"),
                            )
                            .child(
                                div()
                                    .text_size(px(14.0))
                                    .font_weight(FontWeight::BOLD)
                                    .child("Shared launch preferences"),
                            ),
                    )
                    .child(
                        self.palette
                            .button("sharp-launch-settings-close", "×", false)
                            .w(px(30.0))
                            .h(px(30.0))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.state.launch_settings_open = false;
                                this.state.resolution_open = false;
                                cx.notify();
                            })),
                    ),
            );
        for (index, label, choices) in [
            (0, "METALFX", vec!["1.75×", "2×", "Off"]),
            (1, "CONTROLLER INPUT", vec!["Off", "XInput", "DInput"]),
            (2, "MSYNC", vec!["Off", "On"]),
            (
                3,
                "DISPLAY MODE",
                vec!["Game default", "Windowed", "Fullscreen"],
            ),
        ] {
            let mut options = div().flex().gap(px(4.0)).flex_wrap().justify_end();
            for (choice, text) in choices.into_iter().enumerate() {
                // msync uses the single toggle in the reference, unlike the other segmented rows.
                if index == 2 && choice != self.state.launch_preferences[2] {
                    continue;
                }
                options = options.child(
                    self.palette
                        .button(
                            ("launch-preference", index * 10 + choice),
                            text,
                            self.state.launch_preferences[index] == choice,
                        )
                        .h(px(30.0))
                        .px(px(8.0))
                        .text_size(px(11.0))
                        .rounded(px(7.0))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let value = if index == 2 {
                                1 - this.state.launch_preferences[index]
                            } else {
                                choice
                            };
                            if this.live.is_none() {
                                this.state.notice =
                                    "Shared preview launch preference changed · not saved".into();
                            }
                            this.save_launch_preference(index, value, cx);
                        })),
                );
            }
            panel = panel.child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap(px(10.0))
                    .child(
                        div()
                            .text_size(px(11.0))
                            .font_weight(FontWeight::BOLD)
                            .text_color(rgb(muted))
                            .child(label),
                    )
                    .child(options),
            );
        }
        let resolutions = [
            "Game default",
            "720p — 1280 × 720",
            "1080p — 1920 × 1080",
            "1440p — 2560 × 1440",
            "2160p / 4K — 3840 × 2160",
        ];
        let resolution = div().relative().flex_none().child(
            self.palette
                .button(
                    "launch-resolution-trigger",
                    format!("{}  ⌄", resolutions[self.state.launch_preferences[4]]),
                    false,
                )
                .h(px(32.0))
                .text_size(px(11.0))
                .on_click(cx.listener(|this, _, _, cx| {
                    this.state.resolution_open = !this.state.resolution_open;
                    cx.notify();
                })),
        );
        // The panel is already deferred; GPUI aborts on a nested defer, so the
        // choices render inline under the resolution row.
        let mut resolution_choices = None;
        if self.state.resolution_open {
            let mut choices = div()
                .id("launch-resolution-menu")
                .occlude()
                .mt(px(4.0))
                .w(px(226.0))
                .rounded(px(8.0))
                .border_1()
                .border_color(gpui::rgba(self.palette.control_border))
                .bg(rgb(self.palette.menu_bg))
                .p(px(5.0))
                .shadow_lg()
                .flex()
                .flex_col();
            for (index, label) in resolutions.into_iter().enumerate() {
                choices = choices.child(
                    self.palette
                        .button(("launch-resolution-choice", index), label, false)
                        .justify_start()
                        .border_0()
                        .text_size(px(11.0))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.state.resolution_open = false;
                            this.save_launch_preference(4, index, cx);
                        })),
                );
            }
            resolution_choices = Some(div().flex().justify_end().child(choices));
        }
        panel = panel.child(div().flex().items_center().justify_between().gap(px(10.0)).child(div().text_size(px(11.0)).font_weight(FontWeight::BOLD).text_color(rgb(muted)).child("RESOLUTION")).child(resolution))
            .children(resolution_choices)
            .child(div().text_size(px(11.0)).line_height(px(16.5)).text_color(rgb(muted)).child(if self.live.is_some() {"Windowed mode uses a Wine virtual desktop. Resolution uses it unless Fullscreen is selected; fullscreen behavior remains game-controlled. Applies on next launch."} else {"Windowed mode uses a Wine virtual desktop. Resolution uses it unless Fullscreen is selected; fullscreen behavior remains game-controlled. Applies on next launch. Preview changes are never persisted."}));
        control = control.child(gpui::deferred(panel).with_priority(50));
        control
    }

    fn emulator_overview(&self, width: f32) -> gpui::Div {
        let source = self.state.source;
        let i = source.index();
        let (eyebrow, platform) = match source {
            SharpSource::Pcsx2 => ("MANAGED PLAYSTATION 2 ENVIRONMENT", "PS2 Emulation"),
            SharpSource::Rpcs3 => ("MANAGED PLAYSTATION 3 ENVIRONMENT", "PS3 Emulation"),
            SharpSource::ShadPs4 => ("EXPERIMENTAL PLAYSTATION 4 ENVIRONMENT", "PS4 Emulation"),
            _ => (
                "EXPERIMENTAL PLAYSTATION 5 RESEARCH ENVIRONMENT",
                "PS5 Emulation",
            ),
        };
        let (_, _, desc) = source.header();
        let mut stats = vec![
            (
                "Stable runtime",
                if self.state.runtime_ready[i] {
                    "Preview build · installed"
                } else {
                    "Not installed"
                }
                .to_owned(),
            ),
            (
                "Host",
                if source == SharpSource::Pcsx2 {
                    "Apple Silicon · Rosetta"
                } else {
                    "Apple Silicon · macOS"
                }
                .to_owned(),
            ),
            (
                "Library",
                format!(
                    "{} {}",
                    if self.state.samples[i] { 2 } else { 0 },
                    if source == SharpSource::SharpEmu {
                        "layouts"
                    } else {
                        "games"
                    }
                ),
            ),
        ];
        match source {
            SharpSource::Pcsx2 => stats.insert(
                2,
                (
                    "User BIOS",
                    if self.state.bios_ready[i] {
                        "Validated · sample"
                    } else {
                        "Required"
                    }
                    .into(),
                ),
            ),
            SharpSource::Rpcs3 => stats.insert(
                2,
                (
                    "Firmware",
                    if self.state.bios_ready[i] {
                        "Installed · sample"
                    } else {
                        "Required"
                    }
                    .into(),
                ),
            ),
            SharpSource::ShadPs4 => stats.push((
                "Compatibility files",
                format!(
                    "{} modules · {} fonts",
                    self.state.modules[i], self.state.fonts[i]
                ),
            )),
            _ => stats.push((
                "Guest network",
                if self.state.network_opt_in[i] {
                    "Explicitly enabled"
                } else {
                    "Denied by default"
                }
                .into(),
            )),
        }
        let stat_width = ((width - 36.0 - 6.0) / 2.0).max(110.0);
        let mut stat_grid = div().flex().flex_wrap().gap(px(6.0));
        for (index, (label, value)) in stats.into_iter().enumerate() {
            let icon = if index == 0 {
                "source-installers.svg"
            } else if index == 1 {
                "source-empty-monitor.svg"
            } else {
                "source-pcsx2.svg"
            };
            stat_grid = stat_grid.child(
                div()
                    .w(px(stat_width))
                    .min_w_0()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .px(px(10.0))
                    .py(px(6.0))
                    .rounded(px(8.0))
                    .bg(gpui::rgba(0x00000024))
                    .border_1()
                    .border_color(gpui::rgba(0xffffff0a))
                    .child(
                        gpui::svg()
                            .path(icon)
                            .size(px(14.0))
                            .text_color(rgb(self.palette.accent)),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(2.0))
                            .child(div().text_size(px(9.0)).text_color(rgb(MUTED)).child(label))
                            .child(
                                div()
                                    .text_size(px(11.5))
                                    .text_color(rgb(TEXT))
                                    .font_weight(FontWeight::BOLD)
                                    .child(value),
                            ),
                    ),
            );
        }
        div()
            .flex_none()
            .w_full()
            .rounded(px(12.0))
            .border_1()
            .border_color(gpui::rgba((self.palette.accent << 8) | 0x40))
            .bg(rgb(0x1c2228))
            .px(px(18.0))
            .py(px(14.0))
            .flex()
            .flex_col()
            .gap(px(12.0))
            .child(
                div()
                    .flex()
                    .gap(px(12.0))
                    .items_start()
                    .child(
                        div()
                            .flex_none()
                            .size(px(34.0))
                            .rounded(px(9.0))
                            .border_1()
                            .border_color(gpui::rgba((self.palette.accent << 8) | 0x4d))
                            .bg(gpui::rgba((self.palette.accent << 8) | 0x1c))
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(source.icon(self.palette.accent, 26.0)),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .text_size(px(9.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(MUTED))
                                    .child(eyebrow),
                            )
                            .child(
                                div()
                                    .flex()
                                    .flex_wrap()
                                    .items_center()
                                    .gap(px(8.0))
                                    .child(
                                        div()
                                            .text_size(px(17.0))
                                            .font_weight(FontWeight::BOLD)
                                            .text_color(rgb(TEXT))
                                            .child(source.title()),
                                    )
                                    .child(
                                        div()
                                            .text_size(px(12.0))
                                            .text_color(rgb(0xb1b8bb))
                                            .child(format!("- {platform}")),
                                    )
                                    .child(pill(if self.state.running[i] {
                                        "● Running · sample"
                                    } else if self.state.runtime_ready[i] {
                                        "● Ready · sample"
                                    } else {
                                        "● Setup required"
                                    })),
                            )
                            .child(
                                div()
                                    .text_size(px(11.5))
                                    .line_height(px(16.675))
                                    .text_color(rgb(0xaeb3b2))
                                    .max_h(px(33.35))
                                    .overflow_hidden()
                                    .child(desc),
                            ),
                    ),
            )
            .child(stat_grid)
    }

    fn emulator_sidebar(&self, width: f32, compact: bool, cx: &mut Context<Self>) -> gpui::Div {
        let source = self.state.source;
        let i = source.index();
        let ready = self.state.runtime_ready[i];
        let commands: Vec<(&'static str, &'static str, &'static str, bool)> = match source {
            SharpSource::Pcsx2 => vec![
                ("firmware", "Import BIOS", "Accepts a .bin BIOS file", ready),
                (
                    "link",
                    "Download Firmware",
                    "Open PCSX2 firmware page",
                    true,
                ),
                ("link", "Find Games", "Open archive.org", true),
                (
                    "setup",
                    "PCSX2 Setup",
                    if ready {
                        "Controllers & renderer"
                    } else {
                        "Install PCSX2 first"
                    },
                    ready,
                ),
                ("folder", "Add Games", "Disc image or folder", true),
                ("refresh", "Scan Library", "Refresh metadata", true),
            ],
            SharpSource::Rpcs3 => vec![
                ("firmware", "Firmware", "Install PS3UPDAT.PUP", ready),
                (
                    "link",
                    "Download Firmware",
                    "Open PlayStation support",
                    true,
                ),
                ("link", "Find Games", "Open archive.org", true),
                ("package", "Install Package", "Add an owned PKG", ready),
                ("folder", "Add games", "Choose a library folder", true),
                ("refresh", "Scan library", "Refresh games and artwork", true),
            ],
            SharpSource::ShadPs4 => vec![
                ("folder", "Add games", "Choose dumped CUSA folders", true),
                (
                    "modules",
                    "Import modules",
                    "Console-dumped SPRX files",
                    true,
                ),
                ("fonts", "Import fonts", "Console-dumped font content", true),
                (
                    "refresh",
                    "Scan library",
                    "Refresh metadata and artwork",
                    true,
                ),
            ],
            _ => vec![
                (
                    "folder",
                    "Add layouts",
                    "Reference owned eboot.bin folders",
                    true,
                ),
                (
                    "refresh",
                    "Scan library",
                    "Refresh bounded local metadata",
                    true,
                ),
                ("link", "Official FAQ", "Open sharpemu.app", true),
                ("link", "Compatibility", "View upstream reports", true),
            ],
        };
        let mut side = div()
            .flex_none()
            .w(px(width))
            .min_w_0()
            .flex()
            .flex_col()
            .gap(px(8.0));
        for (index, (action, label, detail, enabled)) in commands.into_iter().enumerate() {
            let icon = match action {
                "folder" => "source-installers.svg",
                "firmware" | "modules" => "source-pcsx2.svg",
                "setup" => "source-empty-monitor.svg",
                _ => "source-rpcs3.svg",
            };
            side = side.child(
                div()
                    .id(("emulator-command", index))
                    .w_full()
                    .min_h(px(if compact { 44.0 } else { 54.0 }))
                    .rounded(px(11.0))
                    .border_1()
                    .border_color(gpui::rgba(0xffffff14))
                    .bg(gpui::rgba(0xffffff06))
                    .px(px(10.0))
                    .py(px(9.0))
                    .flex()
                    .items_center()
                    .gap(px(9.0))
                    .cursor_pointer()
                    .opacity(if enabled { 1.0 } else { 0.4 })
                    .child(
                        gpui::svg()
                            .path(icon)
                            .size(px(17.0))
                            .flex_none()
                            .text_color(rgb(self.palette.accent)),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .text_size(px(11.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(TEXT))
                                    .child(label),
                            )
                            .children((!compact).then(|| {
                                div()
                                    .text_size(px(9.0))
                                    .line_height(px(12.0))
                                    .text_color(rgb(MUTED))
                                    .child(detail)
                            })),
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if !enabled {
                            return;
                        }
                        match action {
                            "setup" => {
                                this.state.sidebar_open[i][0] = !this.state.sidebar_open[i][0]
                            }
                            "refresh" => this.state.act("refresh"),
                            "link" => {
                                this.state.notice = format!(
                                    "{label}: external pages are disabled in this offline preview"
                                )
                            }
                            _ => this.state.dialog = Some(action),
                        };
                        cx.notify();
                    })),
            );
            if action == "setup" && self.state.sidebar_open[i][0] {
                let mut settings = div()
                    .w_full()
                    .p(px(11.0))
                    .flex()
                    .flex_col()
                    .gap(px(10.0))
                    .rounded(px(8.0))
                    .bg(rgb(PANEL));
                for (setting, label, options) in [
                    (0, "CONTROLLER 1", ["DualShock 2", "Keyboard", "None"]),
                    (1, "CONTROLLER 2", ["None", "DualShock 2", "Keyboard"]),
                    (2, "RENDERER", ["Automatic", "Metal", "Software"]),
                ] {
                    settings = settings.child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(5.0))
                            .child(
                                div()
                                    .text_size(px(9.0))
                                    .text_color(rgb(MUTED))
                                    .font_weight(FontWeight::BOLD)
                                    .child(label),
                            )
                            .child(
                                self.palette
                                    .button(
                                        ("pcsx2-setting", setting),
                                        format!("{}  ⌄", options[self.state.settings[i][setting]]),
                                        false,
                                    )
                                    .w_full()
                                    .justify_start()
                                    .text_size(px(11.0))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.state.settings[i][setting] =
                                            (this.state.settings[i][setting] + 1) % 3;
                                        this.state.notice =
                                            "PCSX2 preview setting changed · not saved".into();
                                        cx.notify();
                                    })),
                            ),
                    );
                }
                side = side.child(
                    settings.child(
                        div()
                            .text_size(px(9.0))
                            .text_color(rgb(MUTED))
                            .child("Preview selections are not saved."),
                    ),
                );
            }
        }
        if source == SharpSource::SharpEmu {
            side = side.child(div().id("sharpemu-network-policy").p(px(10.0)).rounded(px(10.0)).border_1().border_color(gpui::rgba(0xffb84d3d)).bg(gpui::rgba(0xffb84d08)).flex().items_start().gap(px(8.0)).cursor_pointer()
                .child(div().size(px(14.0)).border_1().border_color(rgb(MUTED)).rounded(px(3.0)).text_size(px(11.0)).text_color(rgb(self.palette.accent)).child(if self.state.network_opt_in[i] {"✓"} else {""}))
                .child(div().min_w_0().flex_1().flex().flex_col().gap(px(4.0)).child(div().text_size(px(11.0)).font_weight(FontWeight::BOLD).text_color(rgb(TEXT)).child("Guest networking"))
                    .child(div().text_size(px(9.0)).line_height(px(13.0)).text_color(rgb(MUTED)).child("Off by default. When enabled, emulated game code may create host sockets, use DNS, and contact local or internet services. Every network-enabled launch asks again. No network is used in preview.")))
                .on_click(cx.listener(move |this, _, _, cx| {this.state.network_opt_in[i]=!this.state.network_opt_in[i]; cx.notify();})));
        }
        for (section, title) in [
            (1, "Runtime & support"),
            (
                2,
                if source == SharpSource::Pcsx2 {
                    "Game locations"
                } else {
                    "Game folders"
                },
            ),
        ] {
            let open = self.state.sidebar_open[i][section];
            let mut drawer = div()
                .w_full()
                .rounded(px(10.0))
                .border_1()
                .border_color(gpui::rgba(0xffffff14))
                .overflow_hidden()
                .bg(gpui::rgba(0xffffff04))
                .flex()
                .flex_col()
                .child(
                    div()
                        .id(("emulator-sidebar-summary", section))
                        .min_h(px(36.0))
                        .px(px(10.0))
                        .py(px(8.0))
                        .flex()
                        .items_center()
                        .gap(px(7.0))
                        .cursor_pointer()
                        .child(
                            gpui::svg()
                                .path("source-installers.svg")
                                .size(px(16.0))
                                .text_color(rgb(self.palette.accent)),
                        )
                        .child(
                            div()
                                .flex_1()
                                .text_size(px(11.0))
                                .font_weight(FontWeight::BOLD)
                                .text_color(rgb(TEXT))
                                .child(title),
                        )
                        .children((section == 2).then(|| {
                            div()
                                .text_size(px(9.0))
                                .text_color(rgb(MUTED))
                                .child(if self.state.roots[i] { "1" } else { "0" })
                        }))
                        .child(
                            div()
                                .text_size(px(10.0))
                                .text_color(rgb(MUTED))
                                .child(if open { "⌄" } else { "›" }),
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.state.sidebar_open[i][section] =
                                !this.state.sidebar_open[i][section];
                            cx.notify();
                        })),
                );
            if open {
                let mut body = div().p(px(10.0)).flex().flex_col().gap(px(8.0));
                if section == 2 {
                    body = body.child(
                        self.palette
                            .button("emulator-root-add", "+  Add", false)
                            .h(px(28.0))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.state.dialog = Some("folder");
                                cx.notify();
                            })),
                    );
                    body = body.child(div().text_size(px(10.0)).text_color(rgb(MUTED)).child(
                        if self.state.roots[i] {
                            "/preview/owned-game-fixture"
                        } else if source == SharpSource::Pcsx2 {
                            "No game locations added yet."
                        } else {
                            "No game folders added yet."
                        },
                    ));
                    if self.state.roots[i] {
                        body = body.child(
                            self.palette
                                .button("emulator-root-remove", "Remove Reference", false)
                                .h(px(28.0))
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.state.roots[i] = false;
                                    this.state.samples[i] = false;
                                    cx.notify();
                                })),
                        );
                    }
                } else {
                    body = body.child(
                        self.palette
                            .button("emulator-check", format!("Check {}", source.title()), false)
                            .h(px(28.0))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.state.act("refresh");
                                cx.notify();
                            })),
                    );
                    if ready {
                        body = body
                            .child(
                                self.palette
                                    .button(
                                        "emulator-pin",
                                        if self.state.pinned[i] {
                                            "Unpin Version"
                                        } else {
                                            "Pin Current"
                                        },
                                        false,
                                    )
                                    .h(px(28.0))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.state.pinned[i] = !this.state.pinned[i];
                                        cx.notify();
                                    })),
                            )
                            .child(
                                self.palette
                                    .button("emulator-rollback", "Rollback", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.state.act("rollback runtime");
                                        cx.notify();
                                    })),
                            )
                            .child(
                                self.palette
                                    .button("emulator-remove", "Remove Runtime", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.state.dialog = Some("remove");
                                        cx.notify();
                                    })),
                            );
                    }
                    if source == SharpSource::Pcsx2 || source == SharpSource::SharpEmu {
                        for (index, label) in if source == SharpSource::Pcsx2 {
                            vec!["BIOS Guide", "Disc Guide"]
                        } else {
                            vec!["Source & GPL License", "Official Releases"]
                        }
                        .into_iter()
                        .enumerate()
                        {
                            body = body.child(
                                self.palette
                                    .button(("emulator-guide", index), label, false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.state.notice = format!(
                                            "{label}: external pages disabled in offline preview"
                                        );
                                        cx.notify();
                                    })),
                            );
                        }
                    }
                }
                drawer = drawer.child(body);
            }
            side = side.child(drawer);
        }
        side
    }

    fn preview_dialog(
        &self,
        kind: &'static str,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let (head, desc, confirm) = dialog_copy(kind);
        let head = if kind == "install" {
            format!(
                "{} {}",
                if self.state.installed[self.state.source.index()] {
                    "Update"
                } else {
                    "Install"
                },
                self.state.source.title()
            )
        } else {
            head.to_owned()
        };
        let muted = if self.palette.light { 0x64717a } else { MUTED };
        let dialog = div().id("sharp-preview-dialog").occlude().on_click(|_,_,cx|cx.stop_propagation()).w(px(440.0)).max_w(px(f32::from(viewport.width)-48.0)).max_h(px(f32::from(viewport.height)-120.0)).overflow_y_scroll().rounded(px(16.0)).border_1().border_color(gpui::rgba(self.palette.control_border)).bg(rgb(self.palette.menu_bg)).p(px(22.0)).flex().flex_col().gap(px(14.0))
            .child(div().text_size(px(17.0)).text_color(rgb(self.palette.control_text)).font_weight(FontWeight::BOLD).child(head))
            .child(div().text_size(px(12.0)).line_height(px(18.0)).text_color(rgb(muted)).child(desc))
            .child(div().rounded(px(8.0)).bg(gpui::rgba((self.palette.accent<<8)|0x14)).p(px(10.0)).text_size(px(10.0)).line_height(px(15.0)).text_color(rgb(muted)).child("Safe preview: no credentials, executables, network, or disk changes. All values are synthetic."))
            .child(div().flex().justify_end().gap(px(8.0)).child(self.small_button("dialog-cancel","Cancel",false).on_click(cx.listener(|this,_,_,cx| {this.state.dialog=None;cx.notify();})))
                .child(self.small_button("dialog-confirm",confirm,true).on_click(cx.listener(move |this,_,_,cx| {
                    this.state.confirm_dialog(kind);
                    cx.notify();
                }))));
        div()
            .id("sharp-preview-modal")
            .occlude()
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .bg(gpui::rgba(0x000000aa))
            .on_click(cx.listener(|this, _, _, cx| {
                this.state.dialog = None;
                cx.notify();
            }))
            .child(dialog)
    }

    fn small_button(
        &self,
        id: impl Into<gpui::ElementId>,
        label: &'static str,
        primary: bool,
    ) -> gpui::Stateful<gpui::Div> {
        self.palette.button(id, label, primary)
    }
    fn card(
        &self,
        title: &'static str,
        sub: &'static str,
        badge: &'static str,
        width: f32,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let source = self.state.source;
        let i = source.index();
        let provider = matches!(source, SharpSource::Gog | SharpSource::Epic);
        let installed = !provider
            || ((sub.starts_with("Installed") || self.state.game_installed[i].contains(title))
                && !self.state.game_removed[i].contains(title));
        let running = self.state.running_app[i] == Some(title);
        let bottle = self.state.bottle_open[i].contains(title)
            || (source == SharpSource::GameJolt && !sub.starts_with("Native"));
        let cover = match title {
            "Hades" => "hades.jpg",
            "Portal 2" => "portal-2.jpg",
            "Stardew Valley" => "stardew-valley.jpg",
            "Cyberpunk 2077" => "cyberpunk.jpg",
            "Disco Elysium" => "disco-elysium.jpg",
            "Hollow Knight" => "hollow-knight.jpg",
            _ => "metalsharp-logo.png",
        };
        let mut actions = div().flex().items_center().gap(px(8.0));
        if installed && source != SharpSource::GameJolt && i < SharpSource::Pcsx2.index() {
            actions = actions.child(
                div()
                    .id(("card-bottle", title.as_ptr() as usize))
                    .size(px(32.0))
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .bg(gpui::rgba(0xffffff08))
                    .flex()
                    .items_center()
                    .justify_center()
                    .cursor_pointer()
                    .text_size(px(18.0))
                    .text_color(rgb(self.palette.accent))
                    .child("⚗")
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if !this.state.bottle_open[i].remove(title) {
                            this.state.bottle_open[i].insert(title);
                        }
                        this.state.card_picker = None;
                        cx.notify();
                    })),
            );
        }
        actions = actions.child(
            self.palette
                .button(
                    ("sample-play", title.as_ptr() as usize),
                    if running {
                        "Stop"
                    } else if installed {
                        "Play"
                    } else {
                        "Install"
                    },
                    true,
                )
                .h(px(32.0))
                .rounded(px(6.0))
                .on_click(cx.listener(move |this, _, _, cx| {
                    if !installed {
                        this.state.selected_card = Some(title);
                        this.state.dialog = Some("game-install");
                    } else {
                        this.state.running_app[i] = if running { None } else { Some(title) };
                        this.state.act(if running { "stop" } else { "play" });
                    }
                    cx.notify();
                })),
        );
        if installed && i < SharpSource::Pcsx2.index() {
            if provider || source == SharpSource::GameJolt {
                actions = actions.child(
                    self.palette
                        .button(
                            ("sample-uninstall", title.as_ptr() as usize),
                            "Uninstall",
                            false,
                        )
                        .h(px(32.0))
                        .rounded(px(6.0))
                        .text_color(rgb(0xff8585))
                        .border_color(gpui::rgba(0xff5c5c55))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if !running {
                                this.state.selected_card = Some(title);
                                this.state.dialog = Some("game-remove");
                                cx.notify();
                            }
                        })),
                );
            } else {
                actions = actions.child(
                    self.palette
                        .button(
                            ("sample-tools", title.as_ptr() as usize),
                            if bottle { "Tools  ⌃" } else { "Tools  ⌄" },
                            false,
                        )
                        .h(px(32.0))
                        .rounded(px(6.0))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if !this.state.bottle_open[i].remove(title) {
                                this.state.bottle_open[i].insert(title);
                            }
                            cx.notify();
                        })),
                );
            }
        }
        if source == SharpSource::GameJolt && sub.starts_with("Native") {
            actions = actions.child(
                div()
                    .ml_auto()
                    .text_size(px(10.0))
                    .text_color(rgb(MUTED))
                    .child("No bottle required"),
            );
        }
        let meta = if running {
            "Running · preview".to_owned()
        } else if provider && !installed {
            format!("Owned · {} · preview", source.title())
        } else if self.state.game_installed[i].contains(title) {
            format!("Installed · {} · preview", source.title())
        } else {
            sub.to_owned()
        };
        let mut body = div()
            .p(px(14.0))
            .pt(px(13.0))
            .flex()
            .flex_col()
            .gap(px(10.0))
            .child(
                div()
                    .text_size(px(14.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(0xf0f0ed))
                    .child(title),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(pill(if running {
                        "Running"
                    } else if provider && installed {
                        "INSTALLED"
                    } else if provider {
                        source.title()
                    } else {
                        badge
                    }))
                    .child(
                        div()
                            .min_w_0()
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child(meta),
                    ),
            )
            .child(actions);
        if bottle && installed {
            let engines = [
                "Auto", "D3DMetal", "DXMT", "M13", "VKD3D", "DXVK", "FNA/Mono",
            ];
            let selected = *self.state.card_engines[i].get(title).unwrap_or(&0);
            let mut field = div().relative().w_full().child(
                self.palette
                    .button(
                        ("card-engine", title.as_ptr() as usize),
                        format!("{}  ⌄", engines[selected]),
                        false,
                    )
                    .w_full()
                    .justify_start()
                    .h(px(32.0))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.state.card_picker = if this.state.card_picker == Some(title) {
                            None
                        } else {
                            Some(title)
                        };
                        cx.notify();
                    })),
            );
            if self.state.card_picker == Some(title) {
                let mut menu = div()
                    .id(("card-engine-menu", title.as_ptr() as usize))
                    .occlude()
                    .absolute()
                    .top(gpui::relative(1.0))
                    .left_0()
                    .right_0()
                    .mt(px(4.0))
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .bg(rgb(self.palette.menu_bg))
                    .p(px(5.0))
                    .flex()
                    .flex_col()
                    .shadow_lg();
                for (index, label) in engines.into_iter().enumerate() {
                    menu = menu.child(
                        self.palette
                            .button(("card-engine-option", index), label, false)
                            .w_full()
                            .justify_start()
                            .border_0()
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.state.card_engines[i].insert(title, index);
                                this.state.card_picker = None;
                                this.state.notice =
                                    "Sample bottle backend changed · not persisted".into();
                                cx.notify();
                            })),
                    );
                }
                field = field.child(gpui::deferred(menu).with_priority(60));
            }
            let mut tools = div()
                .p(px(10.0))
                .rounded(px(8.0))
                .bg(gpui::rgba(0x00000028))
                .border_1()
                .border_color(gpui::rgba(0xffffff14))
                .flex()
                .flex_col()
                .gap(px(8.0))
                .child(
                    div()
                        .text_size(px(9.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(rgb(MUTED))
                        .child("GRAPHICS BACKEND"),
                )
                .child(field);
            if source == SharpSource::Installers {
                for (index, label, kind) in [
                    (0, "Launch Doctor", "doctor"),
                    (1, "Logs and crash reports", "diagnostics"),
                    (2, "Open Bottle Folder", "bottle"),
                    (3, "Choose EXE", "bottle"),
                ] {
                    tools = tools.child(
                        self.palette
                            .button(("installer-card-tool", index as usize), label, false)
                            .w_full()
                            .h(px(28.0))
                            .text_size(px(10.0))
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.state.selected_card = Some(title);
                                this.state.dialog = Some(kind);
                                cx.notify();
                            })),
                    );
                }
                tools =
                    tools.child(div().text_size(px(9.0)).text_color(rgb(MUTED)).child(
                        "Advanced bottle and diagnostic controls use synthetic preview state.",
                    ));
            }
            body = body.child(tools);
        }
        div()
            .id(("sharp-card", title.as_ptr() as usize))
            .w(px(width))
            .min_w_0()
            .flex()
            .flex_col()
            .overflow_hidden()
            .rounded(px(12.0))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(gpui::linear_gradient(
                180.0,
                gpui::linear_color_stop(rgb(0x202427), 0.0),
                gpui::linear_color_stop(rgb(0x171a1c), 1.0),
            ))
            .child(
                img(self.asset_root.join(cover))
                    .w_full()
                    .h(px(width
                        * if source == SharpSource::GameJolt {
                            5.6 / 16.0
                        } else {
                            9.0 / 16.0
                        }))
                    .object_fit(if cover.ends_with(".png") {
                        ObjectFit::Contain
                    } else {
                        ObjectFit::Cover
                    }),
            )
            .child(body)
    }
}
impl Default for SharpPreview {
    fn default() -> Self {
        Self::new()
    }
}
impl Render for SharpPreview {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let width: f32 = window.viewport_size().width.into();
        let stacked = width <= 1280.;
        let title_size = if stacked {
            (width * 0.032).clamp(26.0, 36.0)
        } else {
            (width * 0.03).clamp(30.0, 42.0)
        };
        let source = self.state.source;
        let (eyebrow, title, subtitle) = source.header();
        let header_copy = div()
            .min_w_0()
            .flex()
            .flex_col()
            .gap(px(5.0))
            .child(
                div()
                    .mb(px(10.0))
                    .text_size(px(10.0))
                    .text_color(gpui::rgba(0xf0efe799))
                    .font_family("Georgia")
                    .child(
                        eyebrow
                            .chars()
                            .map(|ch| format!("{ch} "))
                            .collect::<String>(),
                    ),
            )
            .child(
                div()
                    .font_family("Georgia")
                    .text_size(px(title_size))
                    .line_height(px(title_size))
                    .font_weight(FontWeight::MEDIUM)
                    .text_color(rgb(0xeee9dd))
                    .child(title.to_uppercase()),
            )
            .child(
                div()
                    .mt(px(8.0))
                    .text_size(px(13.0))
                    .line_height(px(17.55))
                    .text_color(rgb(0xaeb3b2))
                    .child(subtitle),
            );
        let header_copy = if stacked {
            header_copy.flex_none()
        } else {
            header_copy.flex_1().max_w(px(560.0))
        };
        let mut header = div()
            .relative()
            .flex_none()
            .flex()
            .justify_center()
            .gap(px(if stacked {
                12.0
            } else {
                (width * 0.03).clamp(18.0, 40.0)
            }))
            .px(px(if stacked {
                (width * 0.024).clamp(18.0, 28.0)
            } else {
                (width * 0.03).clamp(24.0, 42.0)
            }))
            .py(px(if stacked { 16.0 } else { 18.0 }))
            .min_h(px(if stacked { 0.0 } else { 126.0 }))
            .border_b_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(gpui::linear_gradient(
                115.0,
                gpui::linear_color_stop(gpui::rgba(0xffffff09), 0.0),
                gpui::linear_color_stop(gpui::rgba(0xffffff00), 1.0),
            ))
            .child(header_copy);
        let source_picker = div()
            .relative()
            .flex_none()
            .child(
                div()
                    .id("sharp-source-trigger")
                    .w(px(226.))
                    .h(px(44.))
                    .flex()
                    .items_center()
                    .gap(px(10.))
                    .px(px(9.))
                    .rounded(px(10.))
                    .border_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .bg(rgb(self.palette.control_bg))
                    .cursor_pointer()
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.state.picker = !this.state.picker;
                        this.state.launch_settings_open = false;
                        this.state.resolution_open = false;
                        cx.notify();
                    }))
                    .child(
                        div()
                            .w(px(30.))
                            .h(px(30.))
                            .rounded(px(8.))
                            .flex()
                            .items_center()
                            .justify_center()
                            .bg(rgb(if self.palette.light {
                                0xece8df
                            } else {
                                0x28251f
                            }))
                            .text_color(rgb(self.palette.accent))
                            .child(source.icon(self.palette.accent, 17.0)),
                    )
                    .child(
                        div()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(1.))
                            .child(
                                div()
                                    .text_size(px(9.))
                                    .text_color(rgb(if self.palette.light {
                                        0x777b7c
                                    } else {
                                        MUTED
                                    }))
                                    .child("Sharp Source"),
                            )
                            .child(
                                div()
                                    .text_size(px(12.))
                                    .text_color(rgb(self.palette.control_text))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(source.title()),
                            ),
                    )
                    .child(div().text_color(rgb(self.palette.control_text)).child("⌄")),
            )
            .children(self.state.picker.then(|| {
                gpui::deferred(self.source_menu(window.viewport_size(), cx)).with_priority(30)
            }));
        let i = source.index();
        let installed = self.state.runtime_ready[i];
        let actions: Vec<(&str, &str, bool, bool)> = match source {
            SharpSource::Installers => vec![
                ("installer", "Install Windows Program", true, true),
                ("refresh", "↻  Refresh", false, true),
            ],
            SharpSource::Gog => vec![
                (
                    if installed { "remove" } else { "install" },
                    if installed {
                        "Uninstall"
                    } else {
                        "Install GOG"
                    },
                    false,
                    true,
                ),
                (
                    if self.state.authenticated[i] {
                        "logout"
                    } else {
                        "login"
                    },
                    if self.state.authenticated[i] {
                        "GOG Connected"
                    } else {
                        "Open GOG"
                    },
                    true,
                    installed,
                ),
                ("sync", "↻  Sync GOG", false, true),
            ],
            SharpSource::Epic => vec![
                (
                    if !installed {
                        "install"
                    } else if self.state.authenticated[i] {
                        "logout"
                    } else {
                        "login"
                    },
                    if !installed {
                        "Install Epic"
                    } else if self.state.authenticated[i] {
                        "Epic Connected"
                    } else {
                        "Open Epic"
                    },
                    installed,
                    true,
                ),
                ("sync", "↻  Sync Epic", false, true),
            ],
            SharpSource::GameJolt => vec![
                ("storage", "Open GameJolt", false, true),
                ("sync", "↻  Sync GameJolt", false, true),
            ],
            SharpSource::Pcsx2 => vec![("install", "↻  Check PCSX2", true, true)],
            SharpSource::Rpcs3 => vec![("install", "↻  Check RPCS3", true, true)],
            SharpSource::ShadPs4 => vec![("install", "↻  Check shadPS4", true, true)],
            SharpSource::SharpEmu => vec![("install", "↻  Check SharpEmu", true, true)],
        };
        let live_pc = self.live.is_some() && source.index() < SharpSource::Pcsx2.index();
        let live_emu = self.live.is_some() && source.index() >= SharpSource::Pcsx2.index();
        let mut controls = if live_pc {
            self.live_header_controls(cx)
        } else if live_emu {
            self.live_emulator_header(cx)
        } else {
            div().flex().items_center()
        };
        for (index, (action, label, primary, enabled)) in actions
            .into_iter()
            .enumerate()
            .filter(|_| !live_pc && !live_emu)
        {
            // Each string comes from a static literal above.
            let action: &'static str = match action {
                "installer" => "installer",
                "install" => "install",
                "remove" => "remove",
                "login" => "login",
                "logout" => "logout",
                "storage" => "storage",
                "sync" => "sync",
                _ => "refresh",
            };
            let button = self
                .palette
                .button(
                    ("sharp-header-action", index),
                    gpui::SharedString::from(label.to_owned()),
                    primary,
                )
                .h(px(40.0))
                .mr(px(12.0))
                .rounded(px(9.0))
                .opacity(if enabled { 1.0 } else { 0.4 })
                .on_click(cx.listener(move |this, _, _, cx| {
                    if !enabled {
                        return;
                    }
                    this.state.picker = false;
                    this.state.launch_settings_open = false;
                    if matches!(
                        action,
                        "installer" | "install" | "remove" | "login" | "storage"
                    ) {
                        this.state.dialog = Some(action);
                    } else {
                        this.state.act(action);
                    }
                    cx.notify();
                }));
            controls = controls.child(button);
        }
        if source.index() >= SharpSource::Pcsx2.index() && installed && !live_emu {
            let running = self.state.running[i];
            controls = controls.child(
                self.palette
                    .button(
                        "emulator-open",
                        format!(
                            "{} {}",
                            if running { "Stop" } else { "Open" },
                            source.title()
                        ),
                        false,
                    )
                    .h(px(40.0))
                    .mr(px(12.0))
                    .rounded(px(9.0))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.state.act(if running { "stop" } else { "play" });
                        cx.notify();
                    })),
            );
        }
        let mut source_controls = div()
            .ml_auto()
            .flex_none()
            .flex()
            .items_center()
            .gap(px(12.0));
        if source.index() < SharpSource::Pcsx2.index() {
            source_controls =
                source_controls.child(self.launch_settings_control(window.viewport_size(), cx));
        }
        controls = controls.child(source_controls.child(source_picker));
        controls = if stacked {
            controls.w_full().min_w_0()
        } else {
            controls.flex_none()
        };
        header = header.child(controls).justify_between();
        if stacked {
            header = header.flex_col().items_start();
        } else {
            header = header.flex_row().items_center();
        }
        let emulator = source.index() >= SharpSource::Pcsx2.index();
        let content_width = (width - 2.0 * (width * 0.03).clamp(22.0, 40.0)).min(
            if emulator || source == SharpSource::Installers {
                1180.0
            } else {
                width
            },
        );
        let workspace_pad = (width * 0.018).clamp(14.0, 22.0);
        let sidebar_width = if width <= 920.0 { 176.0 } else { 238.0 };
        let gap = if width <= 920.0 { 10.0 } else { 14.0 };
        // The workspace panel's 1px border also comes out of the row; without it
        // the last column overflows by 2px and wraps, leaving an empty column.
        let library_width = content_width
            - 2.0 * workspace_pad
            - 2.0
            - if emulator { sidebar_width + gap } else { 0.0 };
        let minimum = if emulator {
            240.0
        } else if source == SharpSource::Installers {
            260.0
        } else {
            224.0
        };
        let columns = ((library_width + 14.0) / (minimum + 14.0)).floor().max(1.0);
        let card_width = ((library_width - (columns - 1.0) * 14.0) / columns).floor();
        let card_width = if emulator || source == SharpSource::Installers {
            card_width.min(320.0)
        } else {
            card_width
        };
        let main = div()
            .id("sharp-library-body")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .px(px((width * 0.03).clamp(22.0, 40.0)))
            .pt(px(22.0))
            .pb(px(32.0))
            .flex()
            .flex_col()
            .gap(px(16.0));
        let mut content = div().min_w_0().flex_1().flex().flex_col().gap(px(14.0));
        let mut grid = div().flex().flex_wrap().gap(px(14.));
        if emulator {
            content = content.child(if live_emu {
                self.live_emulator_overview(library_width)
            } else {
                self.emulator_overview(library_width)
            });
        }
        let gate = if self.state.samples[source.index()] {
            None
        } else {
            match source {
                SharpSource::Installers => Some((
                    "No applications installed",
                    "Install a Windows program to add it to the Sharp Library.",
                    "Install Windows Program",
                )),
                SharpSource::Gog if !self.state.installed[source.index()] => Some((
                    "Initialize Prefix To Get Started",
                    "",
                    "Initialize GOG Prefix",
                )),
                SharpSource::Gog if !self.state.authenticated[source.index()] => Some((
                    "Login to GOG to connect your games",
                    "MetalSharp will capture the GOG login code from a controlled sign-in window.",
                    "Login to GOG",
                )),
                SharpSource::Gog => Some((
                    "No GOG games synced",
                    "Click Sync Library after adding games to your GOG account.",
                    "Sync GOG",
                )),
                SharpSource::Epic if !self.state.installed[source.index()] => Some((
                    "Install Epic Support",
                    "MetalSharp uses the pinned open-source Legendary client to access your owned library without running the Epic Games Launcher.",
                    "Install Epic Support",
                )),
                SharpSource::Epic if !self.state.authenticated[source.index()] => Some((
                    "Login to Epic Games",
                    "Sign in through Epic’s website. MetalSharp stores Legendary account state only under ~/.metalsharp.",
                    "Login to Epic",
                )),
                SharpSource::Epic => Some((
                    "No installable Epic games found",
                    "Sync your library after adding games to your Epic account. Third-party launcher-only titles are omitted.",
                    "Sync Epic",
                )),
                SharpSource::GameJolt => Some((
                    "No GameJolt games found",
                    "Place each game in its own folder inside the GameJolt directory, then sync.",
                    "Add GameJolt Folder",
                )),
                SharpSource::Pcsx2 | SharpSource::Rpcs3 | SharpSource::ShadPs4 => Some((
                    "No games found",
                    "Add a game location or folder to build your library.",
                    "Add Games",
                )),
                SharpSource::SharpEmu => Some((
                    "No layouts found",
                    "Reference an owned eboot.bin folder to add a layout.",
                    "Add Layouts",
                )),
            }
        };
        if live_pc {
            content = content.child(self.live_content(card_width, cx));
        } else if live_emu {
            content = content.child(self.live_emulator_content(card_width, cx));
        } else if !emulator || self.state.samples[i] {
            if let Some((heading, desc, _button)) = gate {
                if source == SharpSource::Installers {
                    content = content.child(
                        div()
                            .w_full()
                            .flex_1()
                            .min_h(px(320.))
                            .rounded(px(12.))
                            .border_1()
                            .border_color(gpui::rgba(self.palette.control_border))
                            .bg(gpui::rgba(0x080a0d85))
                            .p(px(28.))
                            .flex()
                            .flex_col()
                            .items_center()
                            .justify_center()
                            .gap(px(10.))
                            .child(
                                gpui::svg()
                                    .path("source-empty-monitor.svg")
                                    .size(px(48.0))
                                    .text_color(rgb(0x747c7f)),
                            )
                            .child(
                                div()
                                    .text_size(px(18.))
                                    .text_color(rgb(TEXT))
                                    .font_weight(FontWeight::BOLD)
                                    .child(heading),
                            )
                            .child(div().text_size(px(12.)).text_color(rgb(MUTED)).child(desc)),
                    );
                } else {
                    content = content.child(empty_state(self.palette, heading, desc));
                }
            } else {
                let cards: Vec<(&'static str, &'static str, &'static str)> = match source {
                    SharpSource::Installers => vec![
                        ("Hades", "Windows · 8.2 GB · Bottle healthy", "READY"),
                        ("Portal 2", "Windows · 6.4 GB · Last launch clean", "READY"),
                        (
                            "Stardew Valley",
                            "Windows · 1.1 GB · Managed bottle",
                            "READY",
                        ),
                    ],
                    SharpSource::Gog => vec![
                        (
                            "Cyberpunk 2077",
                            "Installed · Windows · 70.4 GB",
                            "INSTALLED",
                        ),
                        ("Disco Elysium", "Owned · Windows · 12.1 GB", "GOG"),
                        ("Hollow Knight", "Installed · Windows · 9.4 GB", "INSTALLED"),
                    ],
                    SharpSource::Epic => vec![
                        ("Hades", "Installed · Bottle initialized", "INSTALLED"),
                        ("Control", "Owned · 42.6 GB", "EPIC"),
                        ("Sifu", "Installed · Windows", "INSTALLED"),
                    ],
                    SharpSource::GameJolt => vec![
                        ("A Short Hike", "Native · Adventure · v1.12", "READY"),
                        ("Celeste Classic", "Native · Platformer", "READY"),
                        ("Super Crate Box", "Windows · Action", "SETUP"),
                    ],
                    SharpSource::Pcsx2 => vec![
                        ("Gran Turismo 4", "SCUS-97328 · ISO · 4.2 GB", "PS2"),
                        ("Shadow of the Colossus", "SCUS-97472 · ISO · 3.6 GB", "PS2"),
                    ],
                    SharpSource::Rpcs3 => vec![
                        ("Demon’s Souls", "BCUS981 armas · Firmware ready", "PS3"),
                        ("Journey", "NPEA00321 · Disc folder", "PS3"),
                    ],
                    SharpSource::ShadPs4 => vec![
                        ("Bloodborne", "CUSA00900 · Update dump available", "PS4"),
                        (
                            "Astro’s Playroom",
                            "CUSA20170 · Compatibility pending",
                            "PS4",
                        ),
                    ],
                    SharpSource::SharpEmu => vec![
                        (
                            "Sample eboot layout",
                            "PPSA00001 · Guest networking off",
                            "PS5",
                        ),
                        ("Demo package layout", "PPSA00002 · Runtime required", "PS5"),
                    ],
                };
                let names = cards;
                for (name, desc, badge) in names {
                    if !matches!(source, SharpSource::Gog | SharpSource::Epic)
                        && self.state.game_removed[i].contains(name)
                    {
                        continue;
                    }
                    grid = grid.child(self.card(name, desc, badge, card_width, cx));
                }
                content = content.child(grid);
            }
        }
        let notice = div()
            .flex_none()
            .mt_auto()
            .text_size(px(10.))
            .text_color(rgb(MUTED))
            .child(self.state.notice.clone());
        content = content.child(notice);
        // Electron's `.gamejolt-panel`: fixed-height panel whose games pane
        // scrolls above the resizable in-page browser.
        let live_gamejolt = source == SharpSource::GameJolt && self.live.is_some();
        let panel_height = (f32::from(window.viewport_size().height) - 310.0).max(180.0);
        let gamejolt_frame = (panel_height * self.state.browser_height / 100.0).max(32.0);
        let content: gpui::AnyElement = if live_gamejolt {
            div()
                .id("gamejolt-games-pane")
                .flex_1()
                .min_w_0()
                .h_full()
                .overflow_y_scroll()
                .pr(px(4.0))
                .pb(px(gamejolt_frame + 24.0))
                .child(content)
                .into_any_element()
        } else {
            content.into_any_element()
        };
        let workspace = div()
            .relative()
            .w_full()
            .max_w(px(content_width))
            .mx_auto()
            .flex_none()
            .min_h(gpui::relative(1.0))
            .p(px(workspace_pad))
            .rounded(px(16.0))
            .border_1()
            .border_color(gpui::rgba(
                (self.palette.control_border & 0xffffff00)
                    | (((self.palette.control_border & 0xff) as f32 * 0.74) as u32),
            ))
            .bg(gpui::linear_gradient(
                145.0,
                gpui::linear_color_stop(gpui::rgba(0xffffff07), 0.0),
                gpui::linear_color_stop(gpui::rgba(0xffffff02), 1.0),
            ))
            .flex()
            .gap(px(gap))
            .child(content)
            .children(emulator.then(|| {
                if live_emu {
                    self.live_emulator_sidebar(sidebar_width, width <= 920.0, cx)
                } else {
                    self.emulator_sidebar(sidebar_width, width <= 920.0, cx)
                }
            }));
        let mut workspace = workspace;
        if live_gamejolt {
            let embed_visible = self.gamejolt_embed_visible();
            if !embed_visible {
                crate::mini_browser::hide_embedded_gamejolt();
            }
            if let Some(live) = crate::live::Live::get(cx) {
                crate::mini_browser::set_gamejolt_download_dir(
                    crate::host_actions::gamejolt_download_dir(&live.home()),
                );
            }
            workspace = workspace.h(px(panel_height)).overflow_hidden().child(
                div()
                    .id("gamejolt-browser-frame")
                    .absolute()
                    .bottom_0()
                    .left_0()
                    .right_0()
                    .h(px(gamejolt_frame))
                    .rounded_t(px(8.0))
                    .border_1()
                    .border_color(rgb(0xffffff))
                    .bg(rgb(0xffffff))
                    .shadow(vec![gpui::BoxShadow {
                        color: gpui::rgba(0xffffff1f).into(),
                        offset: gpui::point(px(0.0), px(0.0)),
                        blur_radius: px(18.0),
                        spread_radius: px(0.0),
                    }])
                    .child(
                        div()
                            .absolute()
                            .top(px(18.0))
                            .left(px(5.0))
                            .right(px(5.0))
                            .bottom(px(5.0))
                            .rounded(px(5.0))
                            .bg(rgb(0xffffff))
                            .child(
                                gpui::canvas(
                                    |_, _, _| (),
                                    move |bounds, _, window, _| {
                                        if embed_visible {
                                            sharp_tools::place_gamejolt_embed(bounds, window);
                                        }
                                    },
                                )
                                .size_full(),
                            ),
                    )
                    .child(
                        div()
                            .id("gamejolt-browser-handle")
                            .absolute()
                            .top(px(-1.0))
                            .left_0()
                            .right_0()
                            .h(px(18.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .cursor_ns_resize()
                            .child(
                                div()
                                    .w(px(54.0))
                                    .h(px(18.0))
                                    .rounded_full()
                                    .bg(rgb(0xffffff))
                                    .flex()
                                    .items_center()
                                    .justify_center()
                                    .text_size(px(15.0))
                                    .font_weight(FontWeight::BLACK)
                                    .text_color(rgb(0x222222))
                                    .child("↕"),
                            )
                            .on_mouse_down(
                                gpui::MouseButton::Left,
                                cx.listener(|this, event: &gpui::MouseDownEvent, _, cx| {
                                    this.state.browser_drag = Some((
                                        f32::from(event.position.y),
                                        this.state.browser_height,
                                    ));
                                    cx.notify();
                                }),
                            ),
                    ),
            );
        } else if source == SharpSource::GameJolt && self.live.is_none() {
            let frame_height = ((f32::from(window.viewport_size().height) - 310.0).max(180.0)
                * self.state.browser_height
                / 100.0)
                .max(32.0);
            workspace = workspace.pb(px(frame_height + 24.0)).child(
                div()
                    .id("gamejolt-browser-frame")
                    .absolute()
                    .bottom_0()
                    .left_0()
                    .right_0()
                    .h(px(frame_height))
                    .rounded_t(px(8.0))
                    .bg(rgb(0xffffff))
                    .p(px(5.0))
                    .child(
                        div()
                            .id("gamejolt-browser-handle")
                            .absolute()
                            .top_0()
                            .left_0()
                            .right_0()
                            .h(px(18.0))
                            .text_center()
                            .text_color(rgb(0x15181a))
                            .cursor_ns_resize()
                            .child("↕")
                            .on_mouse_down(
                                gpui::MouseButton::Left,
                                cx.listener(|this, event: &gpui::MouseDownEvent, _, cx| {
                                    this.state.browser_drag = Some((
                                        f32::from(event.position.y),
                                        this.state.browser_height,
                                    ));
                                    cx.notify();
                                }),
                            ),
                    )
                    .child(
                        div()
                            .size_full()
                            .flex()
                            .items_center()
                            .justify_center()
                            .text_size(px(12.0))
                            .text_color(rgb(0x6f777a))
                            .child("GameJolt browser · disabled in the offline preview"),
                    ),
            );
        }
        let main = main.child(workspace);
        if self.focus.is_none() {
            self.focus = Some(cx.focus_handle());
        }
        let focus = self.focus.as_ref().unwrap().clone();
        let mut root = div()
            .relative()
            .size_full()
            .min_h_0()
            .flex()
            .flex_col()
            .bg(gpui::linear_gradient(
                180.0,
                gpui::linear_color_stop(rgb(0x171a1d), 0.0),
                gpui::linear_color_stop(rgb(BG), 1.0),
            ))
            .font_family("Rethink Sans")
            .child(
                div()
                    .absolute()
                    .top(px(-24.0))
                    .left(gpui::relative(0.15))
                    .w(gpui::relative(0.70))
                    .h(px(1.0))
                    .shadow(vec![gpui::BoxShadow {
                        color: gpui::rgba((self.palette.accent << 8) | 0x17).into(),
                        offset: gpui::point(px(0.0), px(0.0)),
                        blur_radius: px(72.0),
                        spread_radius: px(20.0),
                    }]),
            )
            .child(header)
            .child(main)
            .track_focus(&focus)
            .on_mouse_down(
                gpui::MouseButton::Left,
                cx.listener(|this, _, window, _| {
                    if let Some(focus) = &this.focus {
                        window.focus(focus);
                    }
                }),
            )
            .on_key_down(cx.listener(|this, event: &gpui::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" {
                    this.state.picker = false;
                    this.state.launch_settings_open = false;
                    this.state.resolution_open = false;
                    this.state.dialog = None;
                    cx.notify();
                }
            }))
            .on_mouse_move(
                cx.listener(move |this, event: &gpui::MouseMoveEvent, _, cx| {
                    if let Some((y, height)) = this.state.browser_drag {
                        // `updateGameJoltBrowserHeight`: 10–100 % of the panel.
                        let max = if this.live.is_some() { 100.0 } else { 90.0 };
                        this.state.browser_height = (height
                            + (y - f32::from(event.position.y)) / panel_height * 100.0)
                            .clamp(10.0, max);
                        cx.notify();
                    }
                    if this.live.as_ref().is_some_and(|l| l.cover_drag.is_some()) {
                        this.drag_cover_position(f32::from(event.position.x), cx);
                    }
                }),
            )
            .on_mouse_up(
                gpui::MouseButton::Left,
                cx.listener(|this, _, _, cx| {
                    this.state.browser_drag = None;
                    this.finish_cover_drag(cx);
                    cx.notify();
                }),
            )
            .on_mouse_up_out(
                gpui::MouseButton::Left,
                cx.listener(|this, _, _, cx| {
                    this.state.browser_drag = None;
                    this.finish_cover_drag(cx);
                    cx.notify();
                }),
            );
        if self.state.picker || self.state.launch_settings_open {
            let priority = if self.state.launch_settings_open {
                49
            } else {
                29
            };
            root = root.child(
                gpui::deferred(
                    div()
                        .id("sharp-menu-backdrop")
                        .absolute()
                        .inset_0()
                        .occlude()
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.state.picker = false;
                            this.state.launch_settings_open = false;
                            this.state.resolution_open = false;
                            cx.notify();
                        })),
                )
                .with_priority(priority),
            );
        }
        if let Some(kind) = self.state.dialog {
            let modal = self.preview_dialog(kind, window.viewport_size(), cx);
            root = root.child(gpui::deferred(modal).with_priority(100));
        }
        if let Some(action) = self.live.as_ref().and_then(|live| live.emu_confirm.clone()) {
            let modal = self.live_emu_confirm_overlay(action, window.viewport_size(), cx);
            root = root.child(gpui::deferred(modal).with_priority(110));
        }
        if let Some(action) = self.live.as_ref().and_then(|live| live.confirm.clone()) {
            let modal = self.live_confirm_overlay(action, window.viewport_size(), cx);
            root = root.child(gpui::deferred(modal).with_priority(110));
        }
        // Keep a persistent status note inside body, no shared application chrome is rendered.
        root
    }
}

fn pill(text: &'static str) -> gpui::Div {
    div()
        .px(px(8.))
        .py(px(4.))
        .rounded(px(20.))
        .bg(rgb(0x28251f))
        .text_size(px(9.))
        .text_color(rgb(GOLD))
        .font_weight(FontWeight::BOLD)
        .child(text)
}
fn empty_state(palette: PagePalette, heading: &'static str, desc: &'static str) -> gpui::Div {
    div()
        .w_full()
        .flex_1()
        .min_h(px(220.0))
        .rounded(px(12.0))
        .border_1()
        .border_color(gpui::rgba(palette.control_border))
        .bg(gpui::rgba((palette.control_bg << 8) | 0x85))
        .px(px(20.0))
        .py(px(36.0))
        .flex()
        .flex_col()
        .items_center()
        .justify_center()
        .gap(px(8.0))
        .text_center()
        .child(
            div()
                .text_size(px(16.0))
                .text_color(rgb(0xb1b8bb))
                .font_weight(FontWeight::BOLD)
                .child(heading),
        )
        .children((!desc.is_empty()).then(|| {
            div()
                .text_size(px(13.0))
                .line_height(px(19.5))
                .text_color(rgb(MUTED))
                .child(desc)
        }))
}
fn dialog_copy(k: &str) -> (&'static str, &'static str, &'static str) {
    match k {
        "game-install" => (
            "Install Game",
            "Install only the selected synthetic game fixture. No account, download, installer, or disk changes are involved.",
            "Simulate installation",
        ),
        "game-remove" => (
            "Uninstall Game",
            "Remove only the selected game from preview memory. Nothing is deleted from disk.",
            "Simulate uninstall",
        ),
        "installer" => (
            "Install Windows Program",
            "The production app asks for a Windows installer. This offline preview uses an explicit synthetic application fixture instead; no installer is selected or run.",
            "Use sample application",
        ),
        "install" => (
            "Install Runtime",
            "Simulate the source's isolated runtime setup. No software is downloaded, installed, or executed. Firmware, games, and settings remain synthetic.",
            "Simulate setup",
        ),
        "remove" => (
            "Remove Runtime",
            "Remove only this source's simulated runtime. Sample games and settings are preserved. Nothing on disk is changed.",
            "Simulate removal",
        ),
        "login" => (
            "Connect account",
            "Preview-only sign-in simulation; no credentials are requested and no provider page is opened.",
            "Simulate connection",
        ),
        "firmware" => (
            "Import firmware",
            "Select a firmware or BIOS file. Only dump content you own.",
            "Use sample firmware",
        ),
        "folder" => (
            "Add game location",
            "Choose a folder or disc image containing games you own.",
            "Use sample location",
        ),
        "doctor" => (
            "Launch Doctor",
            "Checks bottle health, runtime, executable selection, and launch recipe.",
            "Run sample checks",
        ),
        "diagnostics" => (
            "Logs & crash reports",
            "Review recent launch logs and crash reports for the selected app.",
            "Load sample report",
        ),
        "details" => (
            "Application details",
            "Preview metadata, installed state, version, and source information.",
            "Close details",
        ),
        "package" => (
            "Install owned PKG",
            "A local package picker would select a PKG. Preview mode never reads a file.",
            "Simulate package selection",
        ),
        "modules" => (
            "Import modules",
            "Select console-dumped SPRX files that you own. No file is read in preview mode.",
            "Use sample module state",
        ),
        "fonts" => (
            "Import fonts",
            "Select console-dumped font content. No file is read in preview mode.",
            "Use sample font state",
        ),
        "bottle" => (
            "Bottle tools",
            "Manage graphics pipeline, components, cover art, and isolated app files.",
            "Open sample tools",
        ),
        _ => (
            "Game Jolt storage",
            "Choose internal or external storage for local Game Jolt installs.",
            "Use sample storage",
        ),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_eight_sources_are_selectable_and_have_distinct_ids() {
        let ids: std::collections::HashSet<_> = SharpSource::ALL.iter().map(|s| s.id()).collect();
        assert_eq!(ids.len(), 8);
        for s in SharpSource::ALL {
            let mut state = PreviewState::default();
            state.choose(s);
            assert_eq!(state.source, s);
            assert_eq!(state.source.title().is_empty(), false);
        }
    }
    #[test]
    fn safe_actions_only_change_preview_state() {
        let mut s = PreviewState::default();
        s.act("login");
        assert!(s.authenticated[SharpSource::Installers.index()]);
        s.act("install");
        assert!(s.installed[0] && s.runtime_ready[0]);
        s.act("play");
        assert!(s.running[0]);
        s.act("stop");
        assert!(!s.running[0]);
        s.act("bios");
        assert!(s.bios_ready[0]);
        assert!(!s.network_opt_in[0]);
    }
    #[test]
    fn source_switch_keeps_provider_state_independent() {
        let mut s = PreviewState::default();
        s.choose(SharpSource::Gog);
        s.act("install");
        s.act("login");
        s.act("play");
        assert!(s.installed[1] && s.authenticated[1] && s.running[1]);
        s.choose(SharpSource::Epic);
        assert!(!s.installed[2] && !s.authenticated[2] && !s.running[2]);
        s.act("login");
        assert!(s.authenticated[2] && s.authenticated[1]);
        s.choose(SharpSource::Gog);
        assert!(s.installed[1] && s.authenticated[1] && s.running[1]);
    }
    #[test]
    fn game_confirmations_only_change_the_selected_preview_game_and_source() {
        let mut state = PreviewState::default();
        state.choose(SharpSource::Epic);
        state.selected_card = Some("Control");
        state.confirm_dialog("game-install");
        assert!(state.game_installed[2].contains("Control"));
        assert!(state.game_installed[1].is_empty());
        assert!(!state.game_installed[2].contains("Hades"));
        assert!(state.selected_card.is_none());
        state.selected_card = Some("Control");
        state.confirm_dialog("game-remove");
        assert!(!state.game_installed[2].contains("Control"));
        assert!(state.game_removed[2].contains("Control"));
    }
    #[test]
    fn runtime_removal_preserves_synthetic_user_data_and_other_sources() {
        let mut state = PreviewState::default();
        state.choose(SharpSource::Pcsx2);
        state.confirm_dialog("install");
        state.confirm_dialog("firmware");
        state.confirm_dialog("folder");
        state.settings[4][0] = 2;
        state.running_app[4] = Some("Gran Turismo 4");
        state.act("play");
        state.confirm_dialog("remove");
        assert!(!state.installed[4] && !state.runtime_ready[4] && !state.running[4]);
        assert!(state.running_app[4].is_none());
        assert!(state.bios_ready[4] && state.roots[4] && state.samples[4]);
        assert_eq!(state.settings[4][0], 2);
        assert!(!state.installed[5] && !state.bios_ready[5] && !state.roots[5]);
    }
    #[test]
    fn samples_settings_and_guest_network_are_source_scoped() {
        let mut s = PreviewState::default();
        s.choose(SharpSource::Pcsx2);
        s.act("sample");
        s.settings[4][0] = 2;
        s.choose(SharpSource::SharpEmu);
        s.network_opt_in[7] = true;
        assert!(s.samples[4] && s.network_opt_in[7]);
        assert_eq!(s.settings[4][0], 2);
        assert_eq!(s.settings[7][0], 0);
    }
}
