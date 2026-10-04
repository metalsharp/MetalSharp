use crate::page_palette::PagePalette;
use gpui::{
    Context, EventEmitter, FocusHandle, FontWeight, Render, Window, div, prelude::*, px, rgb, rgba,
};

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

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SettingsPreviewEvent {
    Close,
    ReopenSetup,
    LanguageChanged(&'static str),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConfirmAction {
    Update,
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
    steam_key_sample: bool,
    epic_key_sample: bool,
    device_name: &'static str,
    wine_installed: bool,
    wine_running: bool,
    mac_installed: bool,
    mac_running: bool,
    exclude_native: bool,
    retina: bool,
    low_performance: bool,
    developer: bool,
    graphics_logs: bool,
    backend_restarting: bool,
    shader_bytes: u32,
    pipeline_bytes: u32,
    update_available: bool,
    fex_available: bool,
    update_downloading: bool,
    update_progress: u8,
    update_fex: bool,
    confirm: Option<ConfirmAction>,
    language_menu: bool,
    notice: Option<&'static str>,
}

impl Default for SettingsPreview {
    fn default() -> Self {
        Self::new()
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
            steam_key_sample: false,
            epic_key_sample: false,
            device_name: "Preview Mac",
            wine_installed: false,
            wine_running: false,
            mac_installed: false,
            mac_running: false,
            exclude_native: false,
            retina: false,
            low_performance: false,
            developer: false,
            graphics_logs: false,
            backend_restarting: false,
            shader_bytes: 428_000_000,
            pipeline_bytes: 76_000_000,
            update_available: false,
            fex_available: true,
            update_downloading: false,
            update_progress: 0,
            update_fex: false,
            confirm: None,
            language_menu: false,
            notice: Some(
                "SAFE PREVIEW · Settings are simulated in memory; no backend, files, processes, or credentials are accessed.",
            ),
        }
    }
    pub fn open(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.focus.is_none() {
            self.focus = Some(cx.focus_handle());
        }
        if let Some(focus) = &self.focus {
            window.focus(focus);
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
    fn credential_control(&self, epic: bool, cx: &mut Context<Self>) -> gpui::Div {
        let configured = if epic {
            self.epic_key_sample
        } else {
            self.steam_key_sample
        };
        let placeholder = if configured {
            gpui::SharedString::from("••••••••••••••••")
        } else {
            self.tr(if epic {
                "setup.theGamesDbApiPlaceholder"
            } else {
                "ui.settings.apiKey"
            })
        };
        div().min_w_0().flex().items_center().flex_wrap().gap(px(8.0))
            .child(div().min_w_0().w(px(310.0)).max_w_full().flex().items_center().gap(px(8.0))
                .child(div().min_w_0().flex_1().w(px(210.0)).h(px(30.0)).px(px(10.0)).flex().items_center().rounded(px(6.0)).border_1().border_color(rgba(self.palette.control_border)).bg(rgba((self.palette.control_bg<<8)|0xb3)).text_size(px(12.0)).text_color(rgba((self.palette.control_text<<8)|0x99)).overflow_hidden().child(placeholder))
                .child(self.action_button(if epic {"epic-save"} else {"steam-save"}, if epic {self.tr("actions.save")} else {format!("{} & Sync",self.tr("actions.save")).into()},true).on_click(cx.listener(move |this,_,_,cx| {
                    if epic {this.epic_key_sample=true;} else {this.steam_key_sample=true;}
                    this.say("A fixed synthetic key state was selected. Credential entry is disabled; no key was captured or sent.",cx);
                }))))
            .child(self.badge(self.tr(if epic {if configured {"ui.settings.theGamesDbKeySaved"} else {"ui.settings.theGamesDbNoKey"}} else if configured {"ui.settings.keySaved"} else {"ui.settings.noKey"}),configured))
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
    fn confirm_state(&mut self, action: ConfirmAction) {
        self.confirm = None;
        self.notice = Some(match action {
            ConfirmAction::ForceKill => {
                self.wine_running = false;
                self.mac_running = false;
                "Force kill simulated. Only synthetic running flags changed; no real process was touched."
            }
            ConfirmAction::Uninstall => "Uninstall simulated; no app or data was removed.",
            ConfirmAction::SwitchToMac => {
                self.wine_running = false;
                self.mac_running = true;
                "Simulated Wine Steam stopped and Mac Steam started. No executable was launched."
            }
            ConfirmAction::Update | ConfirmAction::FexUpdate => {
                self.update_fex = action == ConfirmAction::FexUpdate;
                self.update_downloading = true;
                self.update_progress = 0;
                "Simulated update started; no download, installation, or app restart occurs."
            }
        });
    }
    fn start_update_clock(&self, cx: &mut Context<Self>) {
        cx.spawn(async move |this,cx| {
            for progress in [15,35,60,85,100] {
                cx.background_executor().timer(std::time::Duration::from_millis(650)).await;
                let _=this.update(cx,|this,cx| {
                    if this.update_downloading {this.update_progress=progress;
                        if progress==100 {this.update_downloading=false;this.update_available=false;this.notice=Some("Preview update complete. No software was installed or app restarted.");}
                        cx.notify();
                    }
                });
            }
        }).detach();
    }
    fn confirmation_overlay(&self, cx: &mut Context<Self>) -> gpui::Stateful<gpui::Div> {
        let action = self.confirm.unwrap();
        let (title, copy) = match action {
            ConfirmAction::ForceKill => (
                "Force Kill Processes",
                "Force kill MetalSharp Wine/runtime processes? This can stop active games, installers, and downloads.",
            ),
            ConfirmAction::Uninstall => (
                "Uninstall MetalSharp?",
                "Permanently delete all Wine prefixes, bottles, Steam installation, Wine runtime, shader caches, and settings?",
            ),
            ConfirmAction::SwitchToMac => (
                "Switch Steam Client",
                "Stop Wine Steam and start Mac Steam?",
            ),
            _ => (
                "Confirm Update",
                "Run the in-memory update progress simulation? No software will be downloaded or installed, and MetalSharp will not close or restart.",
            ),
        };
        div().id("settings-confirm-backdrop").occlude().absolute().inset_0().flex().items_center().justify_center().p(px(24.0)).bg(rgba(0x00000088)).on_click(cx.listener(|this,_,_,cx| {this.confirm=None;cx.stop_propagation();cx.notify();}))
            .child(div().id("settings-confirm-dialog").occlude().on_click(|_,_,cx|cx.stop_propagation()).w(px(400.0)).max_w_full().p(px(20.0)).rounded(px(10.0)).border_1().border_color(rgba(self.palette.control_border)).bg(rgb(self.palette.menu_bg)).text_color(rgb(self.palette.control_text)).flex().flex_col().gap(px(14.0))
                .child(div().text_size(px(15.0)).font_weight(FontWeight::BOLD).child(title))
                .child(div().text_size(px(12.0)).line_height(px(18.0)).child(copy))
                .child(div().text_size(px(11.0)).line_height(px(16.0)).text_color(rgba((self.palette.control_text<<8)|0x9e)).child("Safe preview: confirmation changes synthetic memory only. No real processes, software, files, or app restart are involved."))
                .child(div().flex().justify_end().gap(px(8.0)).child(self.action_button("confirm-cancel","Cancel",false).on_click(cx.listener(|this,_,_,cx| {this.confirm=None;cx.notify();})))
                    .child(self.action_button("confirm-safe","Confirm (simulate)",true).on_click(cx.listener(move |this,_,_,cx| {
                        this.confirm_state(action); if matches!(action,ConfirmAction::Update|ConfirmAction::FexUpdate) {this.start_update_clock(cx);}cx.notify();
                    })))))
    }
    fn dismiss(&mut self, cx: &mut Context<Self>) {
        cx.emit(SettingsPreviewEvent::Close);
    }
    fn say(&mut self, text: &'static str, cx: &mut Context<Self>) {
        self.notice = Some(text);
        cx.notify();
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
        let steam_row = self.row(
            self.tr("ui.settings.apiKey"),
            format!(
                "{} steamcommunity.com/dev/apikey.",
                self.tr("ui.settingsDesc.apiKey")
            ),
            self.credential_control(false, cx),
        );
        let device_row = self
            .row(
                self.tr("ui.settings.deviceName"),
                self.tr("ui.settingsDesc.device"),
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .child(self.text_control(self.device_name))
                    .child(
                        self.action_button("device-change", self.tr("ui.settings.change"), false)
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.device_name = if this.device_name == "Preview Mac" {
                                    "Preview Device"
                                } else {
                                    "Preview Mac"
                                };
                                this.say("Preview device name changed in memory.", cx)
                            })),
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
                .child(if self.wine_installed {
                    self.badge(self.tr("ui.settings.installed"), true)
                } else {
                    self.badge(self.tr("ui.settings.notInstalled"), false)
                })
                .when(self.wine_installed, |d| {
                    d.child(
                        self.action_button(
                            "wine-toggle",
                            if self.wine_running {
                                self.tr("ui.settings.stopSteam")
                            } else {
                                self.tr("ui.settings.startSteam")
                            },
                            false,
                        )
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.wine_running = !this.wine_running;
                            if this.wine_running {
                                this.mac_running = false;
                            }
                            this.say(
                                if this.wine_running {
                                    "Simulated Wine Steam started."
                                } else {
                                    "Simulated Wine Steam stopped."
                                },
                                cx,
                            )
                        })),
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
                .child(if self.mac_installed {
                    self.badge(self.tr("ui.settings.installed"), true)
                } else {
                    self.badge(self.tr("ui.settings.notInstalled"), false)
                })
                .child(
                    self.action_button(
                        "mac-action",
                        if self.mac_installed {
                            if self.mac_running {
                                self.tr("ui.settings.stopSteamMac")
                            } else {
                                self.tr("ui.settings.startSteamMac")
                            }
                        } else {
                            self.tr("ui.settings.installMacSteam")
                        },
                        !self.mac_installed,
                    )
                    .on_click(cx.listener(|this, _, _, cx| {
                        if !this.mac_installed {
                            this.mac_installed = true;
                            this.say("Simulated macOS Steam installation complete.", cx)
                        } else if this.mac_running {
                            this.mac_running = false;
                            this.say("Simulated Mac Steam stopped.", cx)
                        } else if this.wine_running {
                            this.confirm = Some(ConfirmAction::SwitchToMac);
                            cx.notify();
                        } else {
                            this.mac_running = true;
                            this.say("Simulated Mac Steam started.", cx)
                        }
                    })),
                )
                .when(self.mac_installed, |d| {
                    d.child(self.text_control("Exclude native"))
                        .child(
                            self.toggle("exclude-native", self.exclude_native, cx, |s, cx| {
                                s.exclude_native = !s.exclude_native;
                                s.say("Native Steam game visibility changed in memory.", cx)
                            }),
                        )
                }),
        );
        let retina = self.row(
            "High Resolution (Retina)",
            self.tr("ui.settingsDesc.retina"),
            self.toggle("retina-toggle", self.retina, cx, |s, cx| {
                s.retina = !s.retina;
                s.say(
                    if s.retina {
                        "Retina rendering enabled (preview); restart Wine Steam to apply."
                    } else {
                        "Retina rendering disabled (preview)."
                    },
                    cx,
                )
            }),
        );
        let backend = self.row(
            self.tr("ui.settings.backendRuntime"),
            self.tr("ui.settingsDesc.backend"),
            self.badge(self.tr("ui.settings.offline"), false),
        );
        let restart = self.row(
            self.tr("ui.settings.restartBackend"),
            self.tr("ui.settingsDesc.restart"),
            self.action_button(
                "restart-backend",
                if self.backend_restarting {
                    "Restarting…"
                } else {
                    "Restart Backend"
                },
                false,
            )
            .on_click(cx.listener(|s, _, _, cx| {
                s.backend_restarting = true;
                s.say("Backend restart simulated; no process was started.", cx);
                s.backend_restarting = false;
                cx.notify()
            })),
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
                    if self.low_performance {
                        "Reduced Effects"
                    } else {
                        "Full Effects"
                    },
                    !self.low_performance,
                ))
                .child(
                    self.toggle("low-performance", self.low_performance, cx, |s, cx| {
                        s.low_performance = !s.low_performance;
                        s.say("Low Performance Mode changed in memory.", cx)
                    }),
                ),
        );
        let dev = self.row(
            self.tr("ui.settings.developerTools"),
            self.tr("ui.settingsDesc.developer"),
            self.toggle("developer", self.developer, cx, |s, cx| {
                s.developer = !s.developer;
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
                .child(
                    self.toggle("graphics-logs", self.graphics_logs, cx, |s, cx| {
                        s.graphics_logs = !s.graphics_logs;
                        s.say("Graphics log preference changed in memory.", cx)
                    }),
                ),
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
                        .on_click(
                            cx.listener(|s, _, _, cx| s.say("Data folder opening simulated.", cx)),
                        ),
                )
                .child(
                    self.action_button("open-logs", self.tr("ui.settings.openLogs"), false)
                        .on_click(
                            cx.listener(|s, _, _, cx| s.say("Logs folder opening simulated.", cx)),
                        ),
                ),
        );
        let repair = self.row(
            self.tr("ui.settings.dataAccess"),
            self.tr("ui.settingsDesc.dataAccess"),
            self.action_button("repair-data", self.tr("ui.settings.repairVerify"), true)
                .on_click(cx.listener(|s, _, _, cx| {
                    s.say(
                        "Data access verified in preview; no permissions changed.",
                        cx,
                    )
                })),
        );
        let shader = self.row(
            self.tr("ui.settings.shaderCache"),
            self.tr("ui.settingsDesc.shader"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge(
                    if self.shader_bytes == 0 {
                        "Empty".to_owned()
                    } else {
                        format_bytes(self.shader_bytes)
                    },
                    self.shader_bytes > 0,
                ))
                .child(self.text_control(if self.shader_bytes == 0 {
                    ""
                } else {
                    "12 apps · Today"
                }))
                .child(
                    self.action_button("clear-shader", self.tr("ui.settings.clear"), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            s.shader_bytes = 0;
                            s.say("Shader cache cleared in simulation — 408 MB freed.", cx)
                        })),
                ),
        );
        let pipeline = self.row(
            self.tr("ui.settings.pipelineCache"),
            self.tr("ui.settingsDesc.pipeline"),
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge(
                    if self.pipeline_bytes == 0 {
                        "Empty".to_owned()
                    } else {
                        format_bytes(self.pipeline_bytes)
                    },
                    self.pipeline_bytes > 0,
                ))
                .child(
                    self.action_button("clear-pipeline", self.tr("ui.settings.clear"), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            s.pipeline_bytes = 0;
                            s.say("Pipeline cache cleared in simulation — 72 MB freed.", cx)
                        })),
                ),
        );
        let version = self.row(
            self.tr("ui.settings.version"),
            if self.update_available {
                gpui::SharedString::from("v1.9.0 available (current: v1.8.2)")
            } else {
                self.tr("ui.settings.upToDate")
            },
            div()
                .flex()
                .items_center()
                .gap(px(8.))
                .child(self.badge("v1.8.2", true))
                .child(
                    self.action_button("check-updates", self.tr("ui.settings.checkNow"), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            s.update_available = true;
                            s.say("Preview fixture: v1.9.0 available.", cx)
                        })),
                ),
        );
        let update = if self.update_available && !self.update_downloading {
            Some(
                self.row(
                    self.tr("ui.settings.downloadUpdate"),
                    "v1.9.0 is ready to download",
                    self.action_button(
                        "download-update",
                        self.tr("ui.settings.downloadInstall"),
                        true,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        s.confirm = Some(ConfirmAction::Update);
                        cx.notify();
                    })),
                ),
            )
        } else {
            None
        };
        let fex = if self.update_available && self.fex_available && !self.update_downloading {
            Some(
                self.row(
                    self.tr("ui.settings.fexUpdate"),
                    "macOS 27+ only · experimental and potentially less stable than baseline",
                    self.action_button("update-fex", self.tr("ui.settings.updateFex"), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            s.confirm = Some(ConfirmAction::FexUpdate);
                            cx.notify();
                        })),
                ),
            )
        } else {
            None
        };
        let progress = if self.update_downloading {
            let label = if self.update_fex {
                "Downloading FEX…"
            } else {
                "Installing update…"
            };
            Some(
                self.row(
                    label,
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
                                        .w(gpui::relative(self.update_progress as f32 / 100.))
                                        .rounded(px(999.))
                                        .bg(rgb(p.accent)),
                                ),
                        )
                        .child(self.text_control(format!("{}%", self.update_progress))),
                ),
            )
        } else {
            None
        };
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
                        .when(!self.wine_installed, |d| d.child(setup_row))
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
                        .when(self.developer, |d| d.child(logs)),
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
fn format_bytes(bytes: u32) -> String {
    if bytes < 1024 {
        format!("{bytes} B")
    } else if bytes < 1024 * 1024 {
        format!("{:.1} KB", bytes as f32 / 1024.0)
    } else if bytes < 1024 * 1024 * 1024 {
        format!("{:.1} MB", bytes as f32 / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", bytes as f32 / (1024.0 * 1024.0 * 1024.0))
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
        assert!(!s.wine_installed);
        assert!(!s.mac_installed);
        assert!(!s.mac_running && !s.wine_running);
        assert_eq!(LANGUAGES.len(), 20);
        assert!(!s.steam_key_sample && !s.epic_key_sample);
    }
    #[test]
    fn cache_simulation_state_is_local_and_danger_actions_are_confirmed() {
        let mut s = SettingsPreview::new();
        s.shader_bytes = 0;
        assert_eq!(s.shader_bytes, 0);
        s.confirm = Some(ConfirmAction::ForceKill);
        assert_eq!(s.confirm, Some(ConfirmAction::ForceKill));
        s.confirm = Some(ConfirmAction::Uninstall);
        assert_eq!(s.confirm, Some(ConfirmAction::Uninstall));
    }
    #[test]
    fn confirmed_actions_only_mutate_synthetic_state() {
        let mut preview = SettingsPreview::new();
        preview.wine_running = true;
        preview.confirm_state(ConfirmAction::SwitchToMac);
        assert!(!preview.wine_running && preview.mac_running);
        preview.confirm_state(ConfirmAction::ForceKill);
        assert!(!preview.wine_running && !preview.mac_running);
        preview.confirm_state(ConfirmAction::FexUpdate);
        assert!(preview.update_downloading && preview.update_fex);
        assert_eq!(preview.update_progress, 0);
        let bytes = preview.shader_bytes;
        preview.confirm_state(ConfirmAction::Uninstall);
        assert_eq!(preview.shader_bytes, bytes);
    }
    #[test]
    fn all_settings_locales_have_source_keys_and_english_fallback() {
        let mut preview = SettingsPreview::new();
        for (language, _) in LANGUAGES {
            preview.language = language;
            assert_eq!(preview.locales[language].len(), 77);
            assert!(!preview.tr("settings.title").is_empty());
            assert!(!preview.tr("language.label").is_empty());
        }
        preview.language = "unsupported";
        assert_eq!(preview.tr("settings.title").as_ref(), "Settings");
        assert_eq!(format_bytes(1024), "1.0 KB");
    }
    #[test]
    fn credential_fixture_never_contains_real_key() {
        let s = SettingsPreview::new();
        assert!(!s.steam_key_sample && !s.epic_key_sample);
        assert_eq!("•••••••• (preview only)", "•••••••• (preview only)");
    }
}
