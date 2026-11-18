//! A game's settings, opened from its hero's gear: the same panel for every library (Steam, GOG, Epic,
//! Battle.net, Ubisoft, Rockstar and Windows programs), laid out like Settings with a tab per group. A game on
//! the MetalVR route gets its headset and the settings VR keeps instead.
use super::*;
use serde_json::json;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum GameSettingsTab {
    Graphics,
    Runtime,
    Advanced,
    Presentation,
    Headset,
}

impl GameSettingsTab {
    fn label(self) -> &'static str {
        match self {
            Self::Graphics => "Graphics",
            Self::Runtime => "Runtime",
            Self::Advanced => "Advanced",
            Self::Presentation => "Presentation",
            Self::Headset => "Headset",
        }
    }

    fn description(self, vr: bool) -> &'static str {
        match self {
            Self::Graphics if vr => {
                "What the game keeps on MetalVR: synchronization, MetalFX and the Metal HUD."
            }
            Self::Graphics => {
                "Synchronization, upscaling and the drivers the game's graphics run on."
            }
            Self::Runtime => {
                "The Metal HUD, which executable starts, the Steam emulator and controllers."
            }
            Self::Advanced => {
                "The game's own environment, DLL overrides, Windows version, CPU cores and priority."
            }
            Self::Presentation => {
                "How the game is shown: its name, window, resolution, frame rate and Retina."
            }
            Self::Headset => "The headset MetalVR streams this game to, and its setup.",
        }
    }
}

/// The Metal HUD's metrics (MTL_HUD_ELEMENTS), as Apple's "Customizing Metal Performance HUD" names them.
const HUD_METRICS: [(&str, &str); 20] = [
    ("fps", "FPS"),
    ("fpsgraph", "FPS graph"),
    ("frameinterval", "Frame interval"),
    ("frameintervalgraph", "Frame interval graph"),
    ("frameintervalhistogram", "Frame interval histogram"),
    ("presentdelay", "Present delay"),
    ("gputime", "GPU time"),
    ("gputimeline", "GPU timeline"),
    ("metalcpu", "Command buffers"),
    ("toplabeledcommandbuffers", "Top command buffers"),
    ("toplabeledencoders", "Top encoders"),
    ("shaders", "Shader compiler"),
    ("memory", "Memory"),
    ("thermal", "Thermal state"),
    ("disk", "Disk"),
    ("framenumber", "Frame number"),
    ("device", "Device"),
    ("rosetta", "Rosetta"),
    ("layersize", "Layer size"),
    ("layerscale", "Layer scale"),
];

/// The macOS major version (26 for macOS 26), or 0 when it can't be read.
pub(super) fn macos_major() -> u32 {
    static MAJOR: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    *MAJOR.get_or_init(|| {
        let mut buffer = [0u8; 32];
        let mut length = buffer.len();
        let name = c"kern.osproductversion";
        // SAFETY: sysctlbyname writes at most `length` bytes into `buffer` and updates `length`.
        let status = unsafe {
            libc::sysctlbyname(
                name.as_ptr(),
                buffer.as_mut_ptr().cast(),
                &mut length,
                std::ptr::null_mut(),
                0,
            )
        };
        if status != 0 {
            return 0;
        }
        std::str::from_utf8(&buffer[..length.saturating_sub(1)])
            .ok()
            .and_then(|version| version.split('.').next())
            .and_then(|major| major.parse().ok())
            .unwrap_or(0)
    })
}

impl MetalSharpApp {
    fn game_settings_tabs(&self) -> &'static [GameSettingsTab] {
        if self.live.selected_pipeline == "metalvr" {
            &[GameSettingsTab::Headset, GameSettingsTab::Graphics]
        } else {
            &[
                GameSettingsTab::Graphics,
                GameSettingsTab::Runtime,
                GameSettingsTab::Advanced,
                GameSettingsTab::Presentation,
            ]
        }
    }

    /// The selected game's settings, centered over the window: the close button, Escape or a click around the
    /// panel closes it.
    pub(super) fn render_game_settings_overlay(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> Option<gpui::Stateful<gpui::Div>> {
        if !self.game_settings_open {
            return None;
        }
        let game = self.games.get(self.selected_game)?.clone();
        let p = self.theme.page_palette();
        let width = (f32::from(viewport.width) - 48.0).clamp(300.0, 900.0);
        let height = (f32::from(viewport.height) - 48.0).clamp(260.0, 660.0);
        let sidebar = width >= 660.0;
        let narrow = width < 540.0;
        let vr = self.live.selected_pipeline == "metalvr";
        let tabs = self.game_settings_tabs();
        let tab = if tabs.contains(&self.game_settings_tab) {
            self.game_settings_tab
        } else {
            tabs[0]
        };
        let rows = match tab {
            GameSettingsTab::Graphics => self.graphics_rows(&game, vr, narrow, cx),
            GameSettingsTab::Runtime => self.runtime_rows(&game, narrow, cx),
            GameSettingsTab::Advanced => self.advanced_rows(&game, narrow, cx),
            GameSettingsTab::Presentation => self.presentation_rows(&game, narrow, cx),
            GameSettingsTab::Headset => self.headset_rows(narrow, cx),
        };

        let mut navigation = div()
            .flex_none()
            .flex()
            .gap(px(5.0))
            .p(px(12.0))
            .bg(rgba((p.hover << 8) | 0x38))
            .when(sidebar, |d| d.w(px(184.0)).h_full().flex_col().border_r_1())
            .when(!sidebar, |d| d.w_full().flex_wrap().border_b_1())
            .border_color(rgba((p.control_text << 8) | 0x12));
        for (index, item) in tabs.iter().copied().enumerate() {
            navigation = navigation.child(
                p.nav_item(("game-settings-tab", index), item.label(), item == tab)
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.game_settings_tab = item;
                        this.hud_metrics_open = false;
                        this.game_settings_scroll
                            .set_offset(gpui::point(px(0.0), px(0.0)));
                        cx.notify();
                    })),
            );
        }

        let content = div()
            .w_full()
            .max_w(px(680.0))
            .mx_auto()
            .flex()
            .flex_col()
            .child(
                div()
                    .mb(px(6.0))
                    .child(
                        div()
                            .text_size(px(22.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(tab.label()),
                    )
                    .child(
                        div()
                            .mt(px(6.0))
                            .text_size(px(13.0))
                            .line_height(px(20.0))
                            .text_color(rgba((p.control_text << 8) | 0xa8))
                            .child(tab.description(vr)),
                    ),
            )
            .children(rows)
            .child(
                div()
                    .pt(px(14.0))
                    .text_size(px(12.0))
                    .text_color(rgba((p.control_text << 8) | 0x80))
                    .child("Changes apply when the game next starts."),
            );
        let scroller = div()
            .relative()
            .flex_1()
            .min_w_0()
            .min_h_0()
            .flex()
            .child(
                div()
                    .id("game-settings-scroll")
                    .flex_1()
                    .min_w_0()
                    .min_h_0()
                    .overflow_y_scroll()
                    .track_scroll(&self.game_settings_scroll)
                    .on_scroll_wheel(crate::scrollbar::refresh_on_scroll)
                    .px(px(if narrow { 16.0 } else { 26.0 }))
                    .py(px(20.0))
                    .child(crate::motion::appear(
                        content,
                        ("game-settings-tab-in", tab as usize),
                        6.0,
                        self.low_performance,
                    )),
            )
            .children(crate::scrollbar::vertical(
                "game-settings-scrollbar",
                &self.game_settings_scroll,
                (p.control_text << 8) | 0x66,
            ));
        let header = div()
            .flex_none()
            .flex()
            .items_center()
            .justify_between()
            .gap(px(16.))
            .px(px(24.0))
            .pt(px(20.0))
            .pb(px(18.0))
            .border_b_1()
            .border_color(rgba(p.control_border))
            .bg(rgba((p.hover << 8) | 0x66))
            .child(
                div()
                    .min_w_0()
                    .flex()
                    .flex_col()
                    .gap(px(4.))
                    .child(
                        div()
                            .text_color(rgba((p.control_text << 8) | 0x99))
                            .text_size(px(10.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(if vr {
                                "GAME SETTINGS · METALVR"
                            } else {
                                "GAME SETTINGS"
                            }),
                    )
                    .child(
                        div()
                            .font_family("Rethink Sans")
                            .text_size(px(24.))
                            .font_weight(FontWeight::SEMIBOLD)
                            .line_height(px(28.0))
                            .truncate()
                            .child(game.name.clone()),
                    ),
            )
            .child(
                p.button("close-game-settings", "", false)
                    .size(px(36.0))
                    .px(px(0.0))
                    .child(crate::ui::control_icon(
                        "icon-close.svg",
                        18.0,
                        p.control_text,
                    ))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.game_settings_open = false;
                        cx.notify();
                    })),
            );
        let panel = div()
            .id("game-settings-popover")
            .occlude()
            .on_click(|_, _, cx| cx.stop_propagation())
            .flex()
            .flex_col()
            .w(px(width))
            .h(px(height))
            .rounded(px(18.0))
            .border_1()
            .border_color(p.panel_border())
            .bg(p.panel_bg())
            .when(p.glass, |panel| {
                panel.relative().child(crate::liquid_glass::mark(
                    "game-settings-glass",
                    crate::liquid_glass::Glass::Sheet,
                    18.0,
                ))
            })
            .text_color(rgb(p.control_text))
            .overflow_hidden()
            .shadow(vec![gpui::BoxShadow {
                color: rgba(0x0000008c).into(),
                offset: gpui::point(px(0.), px(22.)),
                blur_radius: px(55.),
                spread_radius: px(0.),
            }])
            .child(header)
            .child(
                div()
                    .flex_1()
                    .min_h_0()
                    .flex()
                    .when(!sidebar, |d| d.flex_col())
                    .child(navigation)
                    .child(scroller),
            );
        Some(
            div()
                .id("game-settings-overlay")
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .occlude()
                .bg(p.scrim())
                .on_click(cx.listener(|this, _, _, cx| {
                    this.game_settings_open = false;
                    cx.notify();
                }))
                .child(crate::motion::appear(
                    panel,
                    "game-settings-in",
                    12.0,
                    self.low_performance,
                )),
        )
    }

    /// Choices for one setting: (label, value) pairs, the current value, and what a click on one does.
    fn choices(
        &self,
        id: &'static str,
        options: &[(&'static str, &'static str)],
        current: &str,
        enabled: bool,
        cx: &mut Context<Self>,
        on_pick: impl Fn(&mut Self, &'static str, &mut Context<Self>) + Clone + 'static,
    ) -> gpui::Div {
        let p = self.theme.page_palette();
        let mut row = div()
            .flex()
            .flex_wrap()
            .justify_end()
            .gap(px(6.0))
            .opacity(if enabled { 1.0 } else { 0.5 });
        for (index, (label, value)) in options.iter().copied().enumerate() {
            let active = current == value;
            let pick = on_pick.clone();
            row = row.child(p.choice((id, index), label, active).on_click(cx.listener(
                move |this, _, _, cx| {
                    if enabled && !active {
                        pick(this, value, cx);
                    }
                },
            )));
        }
        row
    }

    /// A text field (an input entity) with a button that saves it.
    fn field_with_button(
        &self,
        id: &'static str,
        input: &gpui::Entity<crate::search_input::SearchInput>,
        button: gpui::Stateful<gpui::Div>,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let p = self.theme.page_palette();
        let theme = self.theme;
        input.update(cx, |input, _| {
            input.text_color = theme.control_text();
            input.placeholder_color = if theme == PreviewTheme::Light {
                0x7a7a7a
            } else {
                0x6f7778
            };
        });
        let focus_input = input.clone();
        div()
            .w_full()
            .flex()
            .items_center()
            .gap(px(8.0))
            .child(
                div()
                    .id(id)
                    .flex_1()
                    .min_w_0()
                    .h(px(34.0))
                    .px(px(10.0))
                    .overflow_hidden()
                    .flex()
                    .items_center()
                    .rounded(px(8.0))
                    .border_1()
                    .border_color(rgba(p.control_border))
                    .bg(rgb(theme.page_bg_raised()))
                    .cursor(gpui::CursorStyle::IBeam)
                    .on_click(move |_, window, cx| {
                        window.focus(&focus_input.read(cx).focus_handle(cx));
                    })
                    .child(div().flex_1().min_w_0().child(input.clone())),
            )
            .child(button)
    }

    fn graphics_rows(
        &self,
        game: &LibGame,
        vr: bool,
        narrow: bool,
        cx: &mut Context<Self>,
    ) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let mut rows = Vec::new();
        let msync = self.live.msync;
        rows.push(p.setting_row(
            "msync",
            "Shared-memory synchronization, faster than Wine's own in most games. For every game.",
            p.switch("game-setting-msync", msync)
                .on_click(cx.listener(move |this, _, _, cx| this.set_msync(!msync, cx))),
            narrow,
        ));
        rows.push(p.setting_row(
            "MetalFX",
            "Renders below the display and upscales with MetalFX: the factor divides the resolution the game \
             is shown. For every game.",
            self.choices(
                "game-setting-metalfx",
                &[("1.5×", "1.5"), ("1.75×", "1.75"), ("2×", "2.0"), ("Off", "off")],
                self.live.metal_fx_mode,
                true,
                cx,
                |this, mode, cx| this.set_metal_fx(mode, cx),
            ),
            narrow,
        ));
        if vr {
            rows.extend(self.metal_hud_rows(narrow, cx));
            return rows;
        }
        // Rosetta's x87 sidecar; the FEX build runs x87 code with FEX.
        if !crate::variant::is_fex() {
            let x87 = self.live.x87;
            rows.push(p.setting_row(
                "x87 acceleration",
                "Rosetta's x87 sidecar, which speeds up old x87 floating point. For every game.",
                p.switch("game-setting-x87", x87)
                    .on_click(cx.listener(move |this, _, _, cx| this.set_x87(!x87, cx))),
                narrow,
            ));
        }
        if crate::variant::is_fex() {
            let ray_tracing = self.live.ray_tracing;
            rows.push(p.setting_row(
                "MoltenVK ray tracing",
                "MoltenVK's ray tracing pipelines on the VKD3D-Proton lane: DXR 1.1 and feature level 12_2. \
                 MoltenVK calls them experimental. For every game.",
                p.switch("game-setting-ray-tracing", ray_tracing).on_click(cx.listener(
                    move |this, _, _, cx| this.set_ray_tracing(!ray_tracing, cx),
                )),
                narrow,
            ));
        }
        let tuning = &self.live.tuning;
        let tunable = self.tunable(game);
        let enabled = tunable && tuning.loaded && !tuning.busy;
        rows.push(p.setting_row(
            "OpenGL renderer",
            if tunable {
                "WineMetalGL runs OpenGL on Metal directly. Zink is Mesa's OpenGL on Vulkan, for games \
                 WineMetalGL doesn't draw right. This game only."
            } else {
                "Install the game to choose its renderers."
            },
            self.choices(
                "game-setting-opengl",
                &[("WineMetalGL", "winemetalgl"), ("Zink", "zink")],
                &tuning.opengl_renderer,
                enabled,
                cx,
                |this, value, cx| {
                    this.save_game_tuning(json!({"openglRenderer": value}), None, cx)
                },
            ),
            narrow,
        ));
        let kosmickrisp = macos_major() >= 26;
        rows.push(p.setting_row(
            "Vulkan renderer",
            if !kosmickrisp {
                "MoltenVK. KosmicKrisp, Mesa's Vulkan on Metal, needs macOS 26 or later."
            } else if tunable {
                "MoltenVK, or KosmicKrisp: Mesa's Vulkan driver on Metal, for Vulkan games and Zink. This \
                 game only."
            } else {
                "Install the game to choose its renderers."
            },
            self.choices(
                "game-setting-vulkan",
                &[("MoltenVK", "moltenvk"), ("KosmicKrisp", "kosmickrisp")],
                &tuning.vulkan_renderer,
                enabled && kosmickrisp,
                cx,
                |this, value, cx| {
                    this.save_game_tuning(json!({"vulkanRenderer": value}), None, cx)
                },
            ),
            narrow,
        ));
        rows
    }

    /// Whether the game has its own settings (`/game-tuning`): an installed Windows game.
    fn tunable(&self, game: &LibGame) -> bool {
        game.installed
            && !game.needs_install
            && !game.has_native_build
            && game.tuning_key().is_some()
    }

    /// The Metal HUD switch, with the metrics it shows in a list under it.
    fn metal_hud_rows(&self, narrow: bool, cx: &mut Context<Self>) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let metal_hud = self.live.metal_hud;
        let open = self.hud_metrics_open;
        let chosen = &self.live.metal_hud_elements;
        let summary = if chosen.is_empty() {
            "Metrics: HUD default".to_string()
        } else {
            format!("Metrics: {}", chosen.len())
        };
        let control =
            div()
                .flex()
                .items_center()
                .gap(px(8.0))
                .child(
                    p.choice(
                        "game-setting-hud-metrics",
                        format!("{summary} {}", if open { "▴" } else { "▾" }),
                        open,
                    )
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.hud_metrics_open = !this.hud_metrics_open;
                        cx.notify();
                    })),
                )
                .child(p.switch("game-setting-metal-hud", metal_hud).on_click(
                    cx.listener(move |this, _, _, cx| this.set_metal_hud(!metal_hud, cx)),
                ));
        let mut rows = vec![p.setting_row(
            "Metal HUD",
            "Apple's Metal performance HUD over the game, with the metrics chosen under Metrics. For every game.",
            control,
            narrow,
        )];
        if open {
            let mut list = div()
                .w_full()
                .flex()
                .flex_wrap()
                .gap(px(6.0))
                .py(px(12.0))
                .border_b_1()
                .border_color(rgba((p.control_border & 0xffffff00) | 0x22));
            for (index, (element, label)) in HUD_METRICS.into_iter().enumerate() {
                let on = chosen.iter().any(|name| name == element);
                list =
                    list.child(
                        p.choice(
                            ("game-setting-hud-metric", index),
                            format!("{} {label}", if on { "✓" } else { "+" }),
                            on,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.toggle_metal_hud_element(element, cx)
                        })),
                    );
            }
            rows.push(
                div()
                    .w_full()
                    .flex()
                    .flex_col()
                    .child(
                        div()
                            .pt(px(10.0))
                            .text_size(px(12.0))
                            .text_color(rgba((p.control_text << 8) | 0x99))
                            .child("None chosen shows the HUD's own selection."),
                    )
                    .child(list),
            );
        }
        rows
    }

    fn runtime_rows(&self, game: &LibGame, narrow: bool, cx: &mut Context<Self>) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let mut rows = self.metal_hud_rows(narrow, cx);
        // Windows programs and Game Jolt games keep their executable in the Sharp Library.
        let sharp_app = matches!(game.source.as_deref(), Some("windows" | "gamejolt"));
        if game.installed && !game.needs_install && !game.has_native_build && !sharp_app {
            let executable_name = game
                .executable_path
                .as_deref()
                .filter(|path| !path.is_empty())
                .and_then(|path| path.rsplit(['/', '\\']).next())
                .map(str::to_owned)
                .unwrap_or_else(|| "Automatically detected".to_string());
            rows.push(
                p.setting_row(
                    "Launch executable",
                    format!("The program MetalSharp starts for this game: {executable_name}."),
                    p.button("choose-game-executable", "Choose EXE", false)
                        .on_click(
                            cx.listener(|this, _, _, cx| this.choose_featured_executable(cx)),
                        ),
                    narrow,
                ),
            );
        }
        // Goldberg stands in for a Steam app; a non-Steam shortcut has none.
        if game.page() == LibraryPage::Steam && !game.is_shortcut() {
            let active = self.live.steam_emu_active;
            let enabled = game.installed && !self.live.steam_emu_busy;
            rows.push(p.setting_row(
                "Steam emulator",
                "Goldberg's Steam API in the game's folder, so it starts without the Steam client. This game only.",
                p.switch("game-setting-steam-emu", active)
                    .opacity(if enabled { 1.0 } else { 0.5 })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if enabled {
                            this.set_steam_emu(!active, cx)
                        }
                    })),
                narrow,
            ));
        }
        // MetalSharp Wine's own xinput/dinput see controllers through SDL; the FEX build stages no input shims.
        if !crate::variant::is_fex() {
            if let Some(settings) = game.sharp_settings.as_ref() {
                let current = settings.controller_input.clone().unwrap_or_default();
                rows.push(p.setting_row(
                    "Controller input",
                    "Shared uses the Windows Programs collection's choice.",
                    self.choices(
                        "game-setting-sharp-controller",
                        &[
                            ("Shared", ""),
                            ("Off", "off"),
                            ("XInput", "x"),
                            ("DInput", "d"),
                        ],
                        &current,
                        true,
                        cx,
                        |this, value, cx| {
                            this.set_sharp_preference(
                                "controller_input",
                                (!value.is_empty()).then_some(value),
                                cx,
                            )
                        },
                    ),
                    narrow,
                ));
            } else {
                rows.push(p.setting_row(
                    "Controller input",
                    "The controller API the game is given. For every game.",
                    self.choices(
                        "game-setting-controller",
                        &[("Off", "off"), ("XInput", "x"), ("DInput", "d")],
                        self.live.controller_input,
                        true,
                        cx,
                        |this, mode, cx| this.set_controller(mode, cx),
                    ),
                    narrow,
                ));
            }
        }
        rows
    }

    fn advanced_rows(
        &self,
        game: &LibGame,
        narrow: bool,
        cx: &mut Context<Self>,
    ) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let mut rows = Vec::new();
        if let Some(row) = self.packed_app_row(game, narrow, cx) {
            rows.push(row);
        }
        // A Windows program's own launch arguments.
        if let Some(inputs) = self
            .sharp_setting_inputs
            .as_ref()
            .filter(|inputs| inputs.key == game.key() && game.sharp_settings.is_some())
        {
            let field = self
                .field_with_button(
                    "sharp-setting-args",
                    &inputs.args,
                    p.button("sharp-setting-args-save", "Save", false)
                        .on_click(cx.listener(|this, _, _, cx| this.save_sharp_launch_args(cx))),
                    cx,
                )
                .on_key_down(cx.listener(|this, event: &gpui::KeyDownEvent, _, cx| {
                    if event.keystroke.key == "enter" {
                        this.save_sharp_launch_args(cx);
                        cx.stop_propagation();
                    }
                }));
            rows.push(p.setting_row(
                "Launch arguments",
                "Passed to the program at each start; quotes keep spaces in an argument.",
                div(),
                narrow,
            ));
            rows.push(div().w_full().pb(px(12.0)).child(field));
        }
        if !self.tunable(game) {
            rows.push(p.setting_row(
                "Game settings",
                "Environment variables, DLL overrides, the Windows version, CPU cores and priority are for an \
                 installed Windows game.",
                div(),
                narrow,
            ));
            return rows;
        }
        let Some(inputs) = self.tuning_inputs.clone() else {
            return rows;
        };
        let tuning = &self.live.tuning;
        let enabled = tuning.loaded && !tuning.busy;
        for (field, (title, hint)) in [
            (
                "Environment variables",
                "NAME=value, separated by spaces; quotes keep spaces in a value, -NAME removes one.",
            ),
            (
                "DLL overrides",
                "For this game's executable: name=n (native), b (builtin), n,b, b,n or d (disabled).",
            ),
        ]
        .into_iter()
        .enumerate()
        {
            rows.push(p.setting_row(title, hint, div(), narrow));
            rows.push(
                div().w_full().pb(px(12.0)).child(
                    self.field_with_button(
                        if field == 0 { "game-tuning-env" } else { "game-tuning-dlls" },
                        &inputs[field],
                        p.button(("game-tuning-save", field), "Save", false)
                            .on_click(cx.listener(move |this, _, _, cx| this.save_tuning_text(field, cx))),
                        cx,
                    ),
                ),
            );
        }
        rows.push(p.setting_row(
            "Windows version",
            "The version Wine reports to this game's executable.",
            self.choices(
                "game-tuning-windows",
                &[
                    ("Default", "default"),
                    ("XP", "winxp"),
                    ("7", "win7"),
                    ("8.1", "win81"),
                    ("10", "win10"),
                    ("11", "win11"),
                ],
                &tuning.windows_version,
                enabled,
                cx,
                |this, version, cx| {
                    this.save_game_tuning(json!({"windowsVersion": version}), None, cx)
                },
            ),
            narrow,
        ));
        let cores = tuning.cpu_cores.to_string();
        rows.push(p.setting_row(
            "CPU cores",
            "The processor count the game sees, where MetalSharp starts the game itself.",
            self.choices(
                "game-tuning-cores",
                &[
                    ("Auto", "0"),
                    ("1", "1"),
                    ("2", "2"),
                    ("4", "4"),
                    ("6", "6"),
                    ("8", "8"),
                ],
                &cores,
                enabled,
                cx,
                |this, count, cx| {
                    let count: u32 = count.parse().unwrap_or(0);
                    this.save_game_tuning(json!({"cpuCores": count}), None, cx)
                },
            ),
            narrow,
        ));
        let low = tuning.low_priority;
        rows.push(
            p.setting_row(
                "Low priority",
                "Runs the game at a lower scheduling priority, so the Mac stays responsive.",
                p.switch("game-tuning-priority", low)
                    .opacity(if enabled { 1.0 } else { 0.5 })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.save_game_tuning(json!({"lowPriority": !low}), None, cx)
                    })),
                narrow,
            ),
        );
        rows
    }

    /// The game as its own app in Applications, which starts it (and its launcher) and quits with it.
    fn packed_app_row(
        &self,
        game: &LibGame,
        narrow: bool,
        cx: &mut Context<Self>,
    ) -> Option<gpui::Div> {
        let p = self.theme.page_palette();
        let (_, packed) = self
            .live
            .packed_app
            .as_ref()
            .filter(|(target, _)| game.packed_app_ref().as_ref() == Some(target))?;
        let busy = self.live.pack_app_busy;
        let detail = match packed {
            Some(path) => path
                .rsplit('/')
                .next()
                .map(|name| format!("In Applications as {name}."))
                .unwrap_or_else(|| "In Applications.".to_string()),
            None => {
                "The game as its own app, which opens from Applications or the Dock.".to_string()
            }
        };
        let mut buttons = div()
            .flex()
            .gap(px(6.0))
            .opacity(if busy { 0.5 } else { 1.0 })
            .child(
                p.button(
                    "pack-game-app",
                    if packed.is_some() {
                        "Update .app"
                    } else {
                        "Pack as .app"
                    },
                    false,
                )
                .on_click(cx.listener(|this, _, _, cx| this.pack_featured_app(cx))),
            );
        if packed.is_some() {
            buttons = buttons.child(
                p.button("remove-game-app", "Remove .app", false)
                    .on_click(cx.listener(|this, _, _, cx| this.remove_featured_app(cx))),
            );
        }
        Some(p.setting_row("Standalone app", detail, buttons, narrow))
    }

    fn presentation_rows(
        &self,
        game: &LibGame,
        narrow: bool,
        cx: &mut Context<Self>,
    ) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let mut rows = Vec::new();
        // A Windows program's or Game Jolt game's name.
        if let Some(inputs) = self
            .sharp_setting_inputs
            .as_ref()
            .filter(|inputs| inputs.key == game.key())
        {
            rows.push(p.setting_row(
                "Name",
                "How the game is listed in your library.",
                div(),
                narrow,
            ));
            rows.push(
                div().w_full().pb(px(12.0)).child(
                    self.field_with_button(
                        "sharp-setting-name",
                        &inputs.name,
                        p.button("sharp-setting-name-save", "Rename", false)
                            .on_click(cx.listener(|this, _, _, cx| this.save_sharp_name(cx))),
                        cx,
                    )
                    .on_key_down(cx.listener(
                        |this, event: &gpui::KeyDownEvent, _, cx| {
                            if event.keystroke.key == "enter" {
                                this.save_sharp_name(cx);
                                cx.stop_propagation();
                            }
                        },
                    )),
                ),
            );
        }
        const MODES: [(&str, &str); 3] = [
            ("Game default", "default"),
            ("Windowed", "windowed"),
            ("Fullscreen", "fullscreen"),
        ];
        const RESOLUTIONS: [(&str, &str); 5] = [
            ("Game default", "default"),
            ("720p", "1280x720"),
            ("1080p", "1920x1080"),
            ("1440p", "2560x1440"),
            ("4K", "3840x2160"),
        ];
        if let Some(settings) = game.sharp_settings.as_ref() {
            // A Windows program's own display mode and resolution, over the collection's.
            let with_shared = |options: &[(&'static str, &'static str)]| {
                let mut all = vec![("Shared", "")];
                all.extend_from_slice(options);
                all
            };
            let mode = settings.window_mode.clone().unwrap_or_default();
            rows.push(p.setting_row(
                "Display mode",
                "Windowed and a resolution use a Wine virtual desktop. Shared uses the Windows Programs \
                 collection's choice.",
                self.choices(
                    "sharp-setting-window-mode",
                    &with_shared(&MODES),
                    &mode,
                    true,
                    cx,
                    |this, value, cx| {
                        this.set_sharp_preference("window_mode", (!value.is_empty()).then_some(value), cx)
                    },
                ),
                narrow,
            ));
            let resolution = settings.game_resolution.clone().unwrap_or_default();
            rows.push(p.setting_row(
                "Resolution",
                "The size of the program's virtual desktop.",
                self.choices(
                    "sharp-setting-resolution",
                    &with_shared(&RESOLUTIONS),
                    &resolution,
                    true,
                    cx,
                    |this, value, cx| {
                        this.set_sharp_preference(
                            "game_resolution",
                            (!value.is_empty()).then_some(value),
                            cx,
                        )
                    },
                ),
                narrow,
            ));
        } else {
            rows.push(p.setting_row(
                "Display mode",
                "Windowed and a resolution use a Wine virtual desktop. For every game.",
                self.choices(
                    "game-setting-window-mode",
                    &MODES,
                    &self.live.window_mode,
                    true,
                    cx,
                    |this, value, cx| this.set_shared_display("windowMode", value, cx),
                ),
                narrow,
            ));
            rows.push(p.setting_row(
                "Resolution",
                "The size of the virtual desktop games start in. For every game.",
                self.choices(
                    "game-setting-resolution",
                    &RESOLUTIONS,
                    &self.live.game_resolution,
                    true,
                    cx,
                    |this, value, cx| this.set_shared_display("gameResolution", value, cx),
                ),
                narrow,
            ));
        }
        // The frame rate limit of the lane the game runs on (Mullion's mullion.conf, VKD3D-Proton's
        // VKD3D_FRAME_RATE).
        if let Some(limit) = self.frame_rate_limit() {
            let current = limit.to_string();
            rows.push(p.setting_row(
                "Limit framerate",
                "A ceiling on the game's frame rate, for the graphics lane it runs on.",
                self.choices(
                    "game-setting-frame-rate",
                    &[("30", "30"), ("60", "60"), ("Off", "0")],
                    &current,
                    true,
                    cx,
                    |this, fps, cx| this.set_frame_rate(fps.parse().unwrap_or(0), cx),
                ),
                narrow,
            ));
        }
        // Retina is a prefix's: a game in a bottle of its own can have its own (metalsharp/MetalSharp#797).
        if self.tunable(game) && game.page() != LibraryPage::Steam {
            let tuning = &self.live.tuning;
            rows.push(p.setting_row(
                "High resolution (Retina)",
                "The game's bottle at the display's full resolution. Default follows Settings > Graphics.",
                self.choices(
                    "game-setting-retina",
                    &[("Default", "default"), ("On", "on"), ("Off", "off")],
                    &tuning.retina,
                    tuning.loaded && !tuning.busy,
                    cx,
                    |this, value, cx| this.save_game_tuning(json!({"retina": value}), None, cx),
                ),
                narrow,
            ));
        }
        rows
    }

    /// The Headset tab: the headset MetalVR streams to (one list for every headset it supports, saved for the
    /// VR server's next start), and the guided setup of the chosen one.
    fn headset_rows(&self, narrow: bool, cx: &mut Context<Self>) -> Vec<gpui::Div> {
        let p = self.theme.page_palette();
        let vr = &self.live.vr_headset;
        let mut rows = Vec::new();
        if !vr.loaded {
            rows.push(p.setting_row("Headset", "Asking MetalVR…", div(), narrow));
            return rows;
        }
        let current = vr.model(&vr.headset);
        let open = self.headset_menu_open;
        rows.push(
            p.setting_row(
                "Headset",
                match current {
                    Some(model) if model.streamed => {
                        "Streamed over Wi-Fi or USB with ALVR's client on the headset."
                    }
                    Some(_) => "Plugged into this Mac's video output and USB.",
                    None => "Choose the headset you play with. It is saved for every MetalVR game.",
                },
                p.choice(
                    "vr-headset-menu",
                    format!(
                        "{} {}",
                        current
                            .map(|model| model.name.as_str())
                            .unwrap_or("Choose a headset"),
                        if open { "▴" } else { "▾" }
                    ),
                    open,
                )
                .opacity(if vr.busy { 0.5 } else { 1.0 })
                .on_click(cx.listener(|this, _, _, cx| {
                    this.headset_menu_open = !this.headset_menu_open;
                    cx.notify();
                })),
                narrow,
            ),
        );
        if open {
            let mut list = div()
                .w_full()
                .flex()
                .flex_wrap()
                .gap(px(6.0))
                .py(px(12.0))
                .border_b_1()
                .border_color(rgba((p.control_border & 0xffffff00) | 0x22));
            for (index, model) in vr.models.iter().enumerate() {
                let key = model.key.clone();
                list = list.child(
                    p.choice(
                        ("vr-headset", index),
                        model.name.clone(),
                        model.key == vr.headset,
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.headset_menu_open = false;
                        if this.live.vr_headset.headset != key {
                            this.save_vr_headset(key.clone(), None, cx);
                        }
                        cx.notify();
                    })),
                );
            }
            rows.push(list);
        }
        rows.push(
            p.setting_row(
                "Setup",
                match current {
                    Some(model) => format!(
                        "A guided setup for the {}: what to connect, and each step.",
                        model.name
                    ),
                    None => "Choose a headset first.".to_string(),
                },
                p.button("vr-headset-setup", "Setup", true)
                    .opacity(if current.is_some() { 1.0 } else { 0.5 })
                    .on_click(cx.listener(|this, _, _, cx| this.open_vr_setup(cx))),
                narrow,
            ),
        );
        rows.push(p.setting_row(
            "MetalVR",
            if vr.installed {
                "Installed. Start the VR server from the game's hero before you play."
            } else {
                "Not installed yet: the setup installs it."
            },
            div(),
            narrow,
        ));
        rows
    }

    fn open_vr_setup(&mut self, cx: &mut Context<Self>) {
        let vr = &self.live.vr_headset;
        if vr.model(&vr.headset).is_none() {
            return;
        }
        let trust = vr.trust.clone();
        let text_color = self.theme.control_text();
        self.vr_trust_input = Some(cx.new(|cx| {
            let mut input = crate::search_input::SearchInput::new(cx);
            input.content = trust.into();
            input.placeholder = "1234.client".into();
            input.text_color = text_color;
            input.font_size = 13.0;
            input
        }));
        self.vr_setup_open = true;
        self.headset_menu_open = false;
        cx.notify();
    }

    fn save_vr_trust(&mut self, cx: &mut Context<Self>) {
        let Some(input) = self.vr_trust_input.clone() else {
            return;
        };
        let trust = input.read(cx).content.trim().to_owned();
        let headset = self.live.vr_headset.headset.clone();
        self.save_vr_headset(headset, Some(trust), cx);
    }

    /// The chosen headset's guided setup (MetalVR's docs/headsets): each step, with the button that does it where
    /// MetalSharp can.
    pub(super) fn render_vr_setup_overlay(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> Option<gpui::Stateful<gpui::Div>> {
        if !self.vr_setup_open || !self.game_settings_open {
            return None;
        }
        let vr = &self.live.vr_headset;
        let model = vr.model(&vr.headset)?.clone();
        let p = self.theme.page_palette();
        let width = (f32::from(viewport.width) - 64.0).clamp(300.0, 620.0);
        let height = (f32::from(viewport.height) - 64.0).clamp(260.0, 640.0);
        let installed = vr.installed;

        // (title, what to do, action button: (label, step) or a special action)
        enum Action {
            Step(&'static str, &'static str),
            Trust,
            None,
        }
        let connect = match (model.driver.as_str(), model.key.as_str()) {
            ("rift", "rift-cv1") => {
                "The headset's HDMI into this Mac's own HDMI port (not a hub or adapter: macOS may never turn its \
                 display on through one), its USB into a USB 3 port. Rift Sensors are optional: with them the \
                 headset and Touch controllers are tracked in position."
            }
            ("rift", _) => {
                "The Rift S's DisplayPort through a USB-C to DisplayPort cable, its USB into a USB 3 port. The \
                 first time the server starts, allow the camera (\"Rift S Sensor\") when macOS asks: without it the \
                 headset tracks rotation only."
            }
            ("lighthouse", _) => {
                "DisplayPort through a USB-C or Thunderbolt port (Vive and Vive Pro: through the link box), USB \
                 into a USB 3 port, and power. Base stations set up, and the controllers already paired with the \
                 headset (pairing is done once in SteamVR on Windows or Linux)."
            }
            ("psvr2", _) => {
                "Sony's PlayStation VR2 PC adapter with its power supply: a USB-C to DisplayPort cable into the \
                 adapter's DisplayPort input, and the adapter's USB into a USB 3 port."
            }
            _ if model.key == "steam-frame" => {
                "The Frame and this Mac on the same network (Wi-Fi 6E or 7 on the Frame; the Mac preferably \
                 wired). Install MetalVR's patched ALVR client on the Frame (MetalVR's scripts/build-alvr-client.sh, \
                 then adb install with the Frame in developer mode)."
            }
            _ => {
                "The headset and this Mac on the same network (5 or 6 GHz Wi-Fi, the Mac preferably wired), or the \
                 headset on USB with developer mode and USB debugging on. Install ALVR's client v20 on the headset \
                 from the Meta store or ALVR's releases."
            }
        };
        let mut steps: Vec<(&'static str, String, Action)> = vec![
            ("Connect", connect.to_string(), Action::None),
            (
                "Install MetalVR",
                if installed {
                    "Installed in MetalSharp's folder (metalvr-runtime).".to_string()
                } else {
                    "Downloads MetalVR 0.2.1, checks it, and installs it in Terminal.".to_string()
                },
                Action::Step(if installed { "Reinstall" } else { "Install" }, "install"),
            ),
        ];
        if model.driver == "psvr2" {
            steps.push((
                "Pair the Sense controllers",
                "In Bluetooth settings: hold a controller's Create or Options button and the PS button until its \
                 light blinks, then connect it. Do this for both."
                    .to_string(),
                Action::Step("Open Bluetooth", "bluetooth"),
            ));
        }
        if model.display {
            steps.push((
                "Let macOS show the headset",
                "macOS keeps a headset's display off the desktop. This writes the override that lifts that, in \
                 Terminal (it asks for your password once per Mac). Then unplug the headset's video cable and plug \
                 it in again; if macOS makes it the main display, move the menu bar back in Displays."
                    .to_string(),
                Action::Step("Run in Terminal", "display"),
            ));
            steps.push((
                "Displays",
                "Only if the headset became the main display.".to_string(),
                Action::Step("Open Displays", "displays"),
            ));
        }
        steps.push((
            "SteamVR",
            "Gives SteamVR in MetalSharp's Steam MetalVR's driver, for SteamVR games (OpenXR games need nothing). \
             Install SteamVR in MetalSharp's Steam first."
                .to_string(),
            Action::Step("Set up SteamVR", "steamvr"),
        ));
        if model.streamed {
            steps.push((
                "Trust the headset",
                "Start ALVR's client on the headset: it shows its name, such as 1234.client. Only a trusted headset \
                 may connect; on USB, the name is client.wired."
                    .to_string(),
                Action::Trust,
            ));
        }
        if model.driver == "lighthouse" {
            steps.push((
                "First start",
                "Keep the headset still in view of the base stations for a few seconds while MetalVR finds them; \
                 it remembers where they stand."
                    .to_string(),
                Action::None,
            ));
        }
        steps.push((
            "Play",
            "Start the VR server with the VR Server button beside the game's settings, then press Play.".to_string(),
            Action::None,
        ));

        let mut list = div().flex().flex_col();
        for (index, (title, text, action)) in steps.into_iter().enumerate() {
            let control: gpui::AnyElement = match action {
                Action::Step(label, step) => p
                    .button(("vr-setup-step", index), label, false)
                    .on_click(cx.listener(move |this, _, _, cx| this.run_vr_setup_step(step, cx)))
                    .into_any_element(),
                Action::Trust => match self.vr_trust_input.as_ref() {
                    Some(input) => self
                        .field_with_button(
                            "vr-setup-trust",
                            input,
                            p.button("vr-setup-trust-save", "Trust", false)
                                .on_click(cx.listener(|this, _, _, cx| this.save_vr_trust(cx))),
                            cx,
                        )
                        .w(px(300.0))
                        .into_any_element(),
                    None => div().into_any_element(),
                },
                Action::None => div().into_any_element(),
            };
            let badge = div()
                .flex_none()
                .size(px(26.0))
                .rounded(px(999.0))
                .flex()
                .items_center()
                .justify_center()
                .bg(rgba((p.accent << 8) | 0x2e))
                .text_color(rgb(if p.light { p.control_text } else { p.accent }))
                .text_size(px(12.5))
                .font_weight(FontWeight::SEMIBOLD)
                .child(format!("{}", index + 1));
            list = list.child(
                div()
                    .flex()
                    .gap(px(14.0))
                    .py(px(14.0))
                    .border_b_1()
                    .border_color(rgba((p.control_border & 0xffffff00) | 0x22))
                    .child(badge)
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(8.0))
                            .child(
                                div()
                                    .text_size(px(14.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(title),
                            )
                            .child(
                                div()
                                    .text_size(px(13.0))
                                    .line_height(px(20.0))
                                    .text_color(rgba((p.control_text << 8) | 0xad))
                                    .child(text),
                            )
                            .child(control),
                    ),
            );
        }
        let panel = div()
            .id("vr-setup-panel")
            .occlude()
            .on_click(|_, _, cx| cx.stop_propagation())
            .flex()
            .flex_col()
            .w(px(width))
            .h(px(height))
            .rounded(px(18.0))
            .border_1()
            .border_color(p.panel_border())
            .bg(p.panel_bg())
            .when(p.glass, |panel| {
                panel.relative().child(crate::liquid_glass::mark(
                    "vr-setup-glass",
                    crate::liquid_glass::Glass::Sheet,
                    18.0,
                ))
            })
            .text_color(rgb(p.control_text))
            .overflow_hidden()
            .shadow(vec![gpui::BoxShadow {
                color: rgba(0x0000008c).into(),
                offset: gpui::point(px(0.), px(22.)),
                blur_radius: px(55.),
                spread_radius: px(0.),
            }])
            .child(
                div()
                    .flex_none()
                    .flex()
                    .items_center()
                    .justify_between()
                    .px(px(24.0))
                    .pt(px(20.0))
                    .pb(px(18.0))
                    .border_b_1()
                    .border_color(rgba(p.control_border))
                    .bg(rgba((p.hover << 8) | 0x66))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .child(
                                div()
                                    .text_color(rgba((p.control_text << 8) | 0x99))
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child("METALVR SETUP"),
                            )
                            .child(
                                div()
                                    .font_family("Rethink Sans")
                                    .text_size(px(22.))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(model.name.clone()),
                            ),
                    )
                    .child(
                        p.button("vr-setup-done", "Done", true)
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.vr_setup_open = false;
                                this.load_vr_headset(cx);
                                cx.notify();
                            })),
                    ),
            )
            .child(
                div()
                    .id("vr-setup-scroll")
                    .flex_1()
                    .min_h_0()
                    .overflow_y_scroll()
                    .px(px(24.0))
                    .pb(px(16.0))
                    .child(list),
            );
        Some(
            div()
                .id("vr-setup-overlay")
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .occlude()
                .bg(p.scrim())
                .on_click(cx.listener(|this, _, _, cx| {
                    this.vr_setup_open = false;
                    cx.notify();
                }))
                .child(crate::motion::appear(
                    panel,
                    "vr-setup-in",
                    12.0,
                    self.low_performance,
                )),
        )
    }
}
