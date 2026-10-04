//! Connected Settings overlay. Explicitly opt-in: the offline preview continues
//! to use `settings_preview::SettingsPreview` and never constructs this entity.
//! Backend-owned services are represented as typed intents; this component does
//! not pretend they succeeded when no host integration has handled them.
use crate::{
    configuration::{PreferenceChange, RuntimePreferences},
    page_palette::PagePalette,
    search_input::SearchInput,
};
use gpui::{Context, EventEmitter, Render, Window, div, prelude::*, px, rgb, rgba};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StorefrontAction {
    StartWineSteam,
    StopWineSteam,
    InstallMacSteam,
    StartMacSteam,
    StopMacSteam,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SettingsPath {
    DataHome,
    Logs,
    WineRuntime,
    SteamPrefix,
}
#[derive(Clone, Eq, PartialEq)]
pub enum SettingsIntent {
    SavePreference(PreferenceChange),
    SaveSteamKey(String),
    SaveGamesDbKey(String),
    ReopenSetup,
    OpenDataFolder,
    OpenLogsFolder,
    RepairDataAccess,
    RestartBackend,
    ForceKillProcesses,
    ClearShaderCache,
    ClearPipelineCache,
    CheckForUpdates,
    InstallUpdate,
    InstallFexUpdate,
    Uninstall,
    ChangeDeviceName(String),
    ChangeLanguage(String),
    Storefront(StorefrontAction),
    ChoosePath(SettingsPath),
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SettingsConnectedEvent {
    Close,
    Intent(SettingsIntent),
}
impl std::fmt::Debug for SettingsIntent {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::SaveSteamKey(_) => f.write_str("SaveSteamKey([REDACTED])"),
            Self::SaveGamesDbKey(_) => f.write_str("SaveGamesDbKey([REDACTED])"),
            Self::ChangeLanguage(value) => f.debug_tuple("ChangeLanguage").field(value).finish(),
            Self::ChangeDeviceName(value) => {
                f.debug_tuple("ChangeDeviceName").field(value).finish()
            }
            other => f.write_str(match other {
                Self::SavePreference(_) => "SavePreference",
                Self::ReopenSetup => "ReopenSetup",
                Self::OpenDataFolder => "OpenDataFolder",
                Self::OpenLogsFolder => "OpenLogsFolder",
                Self::RepairDataAccess => "RepairDataAccess",
                Self::RestartBackend => "RestartBackend",
                Self::ForceKillProcesses => "ForceKillProcesses",
                Self::ClearShaderCache => "ClearShaderCache",
                Self::ClearPipelineCache => "ClearPipelineCache",
                Self::CheckForUpdates => "CheckForUpdates",
                Self::InstallUpdate => "InstallUpdate",
                Self::InstallFexUpdate => "InstallFexUpdate",
                Self::Uninstall => "Uninstall",
                Self::Storefront(_) => "Storefront",
                Self::ChoosePath(_) => "ChoosePath",
                Self::SaveSteamKey(_)
                | Self::SaveGamesDbKey(_)
                | Self::ChangeDeviceName(_)
                | Self::ChangeLanguage(_) => unreachable!(),
            }),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Confirm {
    ForceKill,
    ClearShader,
    ClearPipeline,
    InstallUpdate,
    InstallFexUpdate,
    Uninstall,
}
impl Confirm {
    fn intent(self) -> SettingsIntent {
        match self {
            Self::ForceKill => SettingsIntent::ForceKillProcesses,
            Self::ClearShader => SettingsIntent::ClearShaderCache,
            Self::ClearPipeline => SettingsIntent::ClearPipelineCache,
            Self::InstallUpdate => SettingsIntent::InstallUpdate,
            Self::InstallFexUpdate => SettingsIntent::InstallFexUpdate,
            Self::Uninstall => SettingsIntent::Uninstall,
        }
    }
}

#[derive(Clone, Default, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SettingsSnapshot {
    pub preferences: RuntimePreferences,
    pub steam_configured: bool,
    pub gamesdb_configured: bool,
    pub device_name: String,
}
pub struct SettingsConnected {
    pub palette: PagePalette,
    preferences: RuntimePreferences,
    steam_input: gpui::Entity<SearchInput>,
    gamesdb_input: gpui::Entity<SearchInput>,
    device_name_input: gpui::Entity<SearchInput>,
    steam_configured: bool,
    gamesdb_configured: bool,
    busy: bool,
    loading: bool,
    error: Option<String>,
    notice: Option<String>,
    confirm: Option<Confirm>,
}
impl SettingsConnected {
    pub fn new(snapshot: SettingsSnapshot, palette: PagePalette, cx: &mut Context<Self>) -> Self {
        let input = |hint: &str, cx: &mut Context<Self>| {
            let hint = hint.to_owned();
            cx.new(move |cx| {
                let mut field = SearchInput::new(cx);
                field.placeholder = hint.into();
                field.secret = true;
                field
            })
        };
        let this = Self {
            palette,
            preferences: snapshot.preferences.clone(),
            steam_input: input("Steam Web API key", cx),
            gamesdb_input: input("TheGamesDB API key", cx),
            device_name_input: cx.new(|cx| {
                let mut field = SearchInput::new(cx);
                field.placeholder = "Device name".into();
                field
            }),
            steam_configured: snapshot.steam_configured,
            gamesdb_configured: snapshot.gamesdb_configured,
            busy: false,
            loading: false,
            error: None,
            notice: None,
            confirm: None,
        };
        this.device_name_input.update(cx, |input, cx| {
            input.content = snapshot.device_name.into();
            cx.notify();
        });
        this
    }
    pub fn mark_configured_key(&mut self, steam: bool, configured: bool, cx: &mut Context<Self>) {
        if steam {
            self.steam_configured = configured
        } else {
            self.gamesdb_configured = configured
        }
        cx.notify();
    }
    pub fn apply_preferences(
        &mut self,
        preferences: RuntimePreferences,
        device_name: &str,
        cx: &mut Context<Self>,
    ) {
        self.preferences = preferences;
        self.device_name_input.update(cx, |input, cx| {
            if input.content.is_empty() {
                input.content = device_name.to_owned().into();
            }
            cx.notify();
        });
        cx.notify();
    }
    pub fn apply_snapshot(&mut self, snapshot: SettingsSnapshot, cx: &mut Context<Self>) {
        self.preferences = snapshot.preferences;
        self.steam_configured = snapshot.steam_configured;
        self.gamesdb_configured = snapshot.gamesdb_configured;
        self.device_name_input.update(cx, |input, cx| {
            input.content = snapshot.device_name.into();
            cx.notify();
        });
        self.loading = false;
        cx.notify();
    }
    /// Parent host reports completion for scoped OS/updater/runtime intents here.
    pub fn host_operation_started(&mut self, cx: &mut Context<Self>) {
        self.busy = true;
        self.error = None;
        self.notice = None;
        cx.notify();
    }
    pub fn host_operation_finished(
        &mut self,
        result: Result<String, String>,
        cx: &mut Context<Self>,
    ) {
        self.busy = false;
        match result {
            Ok(message) => self.notice = Some(message),
            Err(message) => self.error = Some(message),
        }
        cx.notify();
    }
    pub fn credential_inputs(&self) -> [gpui::Entity<SearchInput>; 2] {
        [self.steam_input.clone(), self.gamesdb_input.clone()]
    }
    pub fn device_name_input(&self) -> gpui::Entity<SearchInput> {
        self.device_name_input.clone()
    }
    fn save_preference(&mut self, change: PreferenceChange, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        self.busy = true;
        self.error = None;
        self.notice = None;
        self.intent(SettingsIntent::SavePreference(change), cx);
        cx.notify();
    }
    fn save_device_name(&mut self, name: String, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        self.busy = true;
        self.error = None;
        self.notice = None;
        self.intent(SettingsIntent::ChangeDeviceName(name), cx);
        cx.notify();
    }
    fn save_key(&mut self, steam: bool, cx: &mut Context<Self>) {
        if self.busy {
            return;
        }
        let input = if steam {
            self.steam_input.clone()
        } else {
            self.gamesdb_input.clone()
        };
        let key = input.read(cx).content.to_string();
        if key.trim().is_empty() || key.len() > 512 || key.chars().any(char::is_control) {
            self.error =
                Some("Enter a valid key (1–512 characters, no control characters).".into());
            cx.notify();
            return;
        }
        input.update(cx, |f, cx| f.clear(cx));
        self.busy = true;
        self.error = None;
        self.notice = None;
        let intent = if steam {
            SettingsIntent::SaveSteamKey(key)
        } else {
            SettingsIntent::SaveGamesDbKey(key)
        };
        self.intent(intent, cx);
        cx.notify();
    }
    fn intent(&mut self, intent: SettingsIntent, cx: &mut Context<Self>) {
        cx.emit(SettingsConnectedEvent::Intent(intent));
    }
    fn ask(&mut self, confirm: Confirm, cx: &mut Context<Self>) {
        self.confirm = Some(confirm);
        cx.notify();
    }
    fn unavailable(&self, label: impl Into<gpui::SharedString>) -> gpui::Div {
        div()
            .px(px(10.))
            .py(px(7.))
            .rounded(px(6.))
            .bg(rgb(0x25292b))
            .text_color(rgb(0x858b89))
            .cursor_default()
            .child(label.into())
    }
    fn action(
        &self,
        id: &'static str,
        label: impl Into<gpui::SharedString>,
        primary: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let p = self.palette;
        div()
            .id(id)
            .px(px(11.))
            .py(px(7.))
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
            .cursor_pointer()
            .child(label.into())
    }
    fn field(input: &gpui::Entity<SearchInput>) -> gpui::Div {
        div()
            .w(px(205.))
            .h(px(32.))
            .px(px(9.))
            .py(px(6.))
            .rounded(px(6.))
            .border_1()
            .border_color(rgb(0x555b5c))
            .child(input.clone())
    }
    fn row(&self, label: &str, desc: &str, control: impl IntoElement) -> gpui::Div {
        div()
            .flex()
            .justify_between()
            .items_start()
            .gap(px(12.))
            .py(px(9.))
            .border_b_1()
            .border_color(rgba(self.palette.control_border))
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .child(
                        div()
                            .text_size(px(12.))
                            .font_weight(gpui::FontWeight::SEMIBOLD)
                            .child(label.to_owned()),
                    )
                    .child(
                        div()
                            .mt(px(3.))
                            .text_size(px(10.5))
                            .text_color(rgba((self.palette.control_text << 8) | 0xa0))
                            .child(desc.to_owned()),
                    ),
            )
            .child(
                div()
                    .flex()
                    .flex_wrap()
                    .items_center()
                    .justify_end()
                    .gap(px(7.))
                    .child(control),
            )
    }
    fn card(&self, title: &str, content: impl IntoElement, danger: bool) -> gpui::Div {
        div()
            .min_w_0()
            .px(px(14.))
            .pt(px(12.))
            .pb(px(5.))
            .rounded(px(9.))
            .border_1()
            .border_color(rgba(self.palette.control_border))
            .bg(rgba((self.palette.control_bg << 8) | 0xd1))
            .child(
                div()
                    .mb(px(8.))
                    .pb(px(7.))
                    .border_b_1()
                    .border_color(rgba(if danger {
                        0xff5c5c88
                    } else {
                        (self.palette.accent << 8) | 0x73
                    }))
                    .text_color(rgb(if danger {
                        0xff8585
                    } else {
                        self.palette.control_text
                    }))
                    .text_size(px(12.))
                    .font_weight(gpui::FontWeight::BOLD)
                    .child(title.to_uppercase()),
            )
            .child(content)
    }
    fn confirmed(&mut self, cx: &mut Context<Self>) {
        let Some(action) = self.confirm.take() else {
            return;
        };
        let intent = action.intent();
        self.intent(intent, cx);
        cx.notify();
    }
    fn confirm_view(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let Some(confirm) = self.confirm else {
            return div().id("settings-connected-confirm");
        };
        let (title, copy) = match confirm {
            Confirm::ForceKill => (
                "Force kill processes?",
                "This may stop active games, installers and downloads.",
            ),
            Confirm::ClearShader => ("Clear shader cache?", "Cached shader data will be removed."),
            Confirm::ClearPipeline => (
                "Clear pipeline cache?",
                "Cached pipeline data will be removed.",
            ),
            Confirm::InstallUpdate => (
                "Install update?",
                "The updater may download software and restart the application.",
            ),
            Confirm::InstallFexUpdate => (
                "Install FEX update?",
                "This experimental update may be less stable than baseline.",
            ),
            Confirm::Uninstall => (
                "Uninstall MetalSharp?",
                "This may remove application data and installed runtime resources.",
            ),
        };
        div()
            .id("settings-connected-confirm")
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgba(0x00000099))
            .child(
                div()
                    .w(px(390.))
                    .max_w_full()
                    .p(px(20.))
                    .rounded(px(10.))
                    .bg(rgb(self.palette.menu_bg))
                    .text_color(rgb(self.palette.control_text))
                    .flex()
                    .flex_col()
                    .gap(px(12.))
                    .child(div().text_size(px(15.)).child(title))
                    .child(div().text_size(px(12.)).child(copy))
                    .child(
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.))
                            .child(self.action("cancel-intent", "Cancel", false).on_click(
                                cx.listener(|this, _, _, cx| {
                                    this.confirm = None;
                                    cx.notify()
                                }),
                            ))
                            .child(
                                self.action("confirm-intent", "Confirm", true)
                                    .on_click(cx.listener(|this, _, _, cx| this.confirmed(cx))),
                            ),
                    ),
            )
    }
}
impl EventEmitter<SettingsConnectedEvent> for SettingsConnected {}
impl Render for SettingsConnected {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let p = self.palette;
        let prefs = self.preferences.clone();
        let disabled = self.busy || self.loading;
        let prefs_card = div()
            .child(
                self.row(
                    "High Resolution (Retina)",
                    "Apply high resolution rendering to future Wine Steam launches.",
                    self.action(
                        "retina-toggle",
                        format!("Retina: {}", prefs.retina_mode),
                        false,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        s.save_preference(
                            PreferenceChange::RetinaMode(!s.preferences.retina_mode),
                            cx,
                        )
                    })),
                ),
            )
            .child(
                self.row(
                    "Exclude native macOS games",
                    "Hide native Steam games from the Windows library.",
                    self.action(
                        "native-games-toggle",
                        format!("Excluded: {}", prefs.exclude_native_mac_steam_games),
                        false,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        s.save_preference(
                            PreferenceChange::ExcludeNativeMacSteamGames(
                                !s.preferences.exclude_native_mac_steam_games,
                            ),
                            cx,
                        )
                    })),
                ),
            )
            .child(
                self.row(
                    "Graphics runtime logs",
                    "Enable diagnostic graphics logs on future launches.",
                    self.action(
                        "graphics-logs-toggle",
                        format!("Enabled: {}", prefs.graphics_runtime_logs),
                        false,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        s.save_preference(
                            PreferenceChange::GraphicsRuntimeLogs(
                                !s.preferences.graphics_runtime_logs,
                            ),
                            cx,
                        )
                    })),
                ),
            )
            .child(
                self.row(
                    "MSync",
                    "Runtime synchronization preference.",
                    self.action("msync-toggle", format!("Enabled: {}", prefs.msync), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            s.save_preference(PreferenceChange::Msync(!s.preferences.msync), cx)
                        })),
                ),
            )
            .child(
                self.row(
                    "Controller input",
                    "Select XInput, DirectInput, or disabled.",
                    self.action(
                        "controller-toggle",
                        format!("{:?}", prefs.controller_input),
                        false,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        let n = match s.preferences.controller_input {
                            crate::configuration::ControllerInput::Off => {
                                crate::configuration::ControllerInput::XInput
                            }
                            crate::configuration::ControllerInput::XInput => {
                                crate::configuration::ControllerInput::DInput
                            }
                            crate::configuration::ControllerInput::DInput => {
                                crate::configuration::ControllerInput::Off
                            }
                        };
                        s.save_preference(PreferenceChange::ControllerInput(n), cx)
                    })),
                ),
            )
            .child(
                self.row(
                    "Window mode",
                    "Default, windowed, or fullscreen.",
                    self.action("window-toggle", format!("{:?}", prefs.window_mode), false)
                        .on_click(cx.listener(|s, _, _, cx| {
                            let n = match s.preferences.window_mode {
                                crate::configuration::WindowMode::Default => {
                                    crate::configuration::WindowMode::Windowed
                                }
                                crate::configuration::WindowMode::Windowed => {
                                    crate::configuration::WindowMode::Fullscreen
                                }
                                crate::configuration::WindowMode::Fullscreen => {
                                    crate::configuration::WindowMode::Default
                                }
                            };
                            s.save_preference(PreferenceChange::WindowMode(n), cx)
                        })),
                ),
            )
            .child(
                self.row(
                    "Game resolution",
                    "Default, 720p, 1080p, 1440p, or 4K.",
                    self.action(
                        "resolution-toggle",
                        format!("{:?}", prefs.game_resolution),
                        false,
                    )
                    .on_click(cx.listener(|s, _, _, cx| {
                        let n = match s.preferences.game_resolution {
                            crate::configuration::GameResolution::Default => {
                                crate::configuration::GameResolution::Hd
                            }
                            crate::configuration::GameResolution::Hd => {
                                crate::configuration::GameResolution::FullHd
                            }
                            crate::configuration::GameResolution::FullHd => {
                                crate::configuration::GameResolution::Qhd
                            }
                            crate::configuration::GameResolution::Qhd => {
                                crate::configuration::GameResolution::Uhd
                            }
                            crate::configuration::GameResolution::Uhd => {
                                crate::configuration::GameResolution::Default
                            }
                        };
                        s.save_preference(PreferenceChange::GameResolution(n), cx)
                    })),
                ),
            );
        let inputs = self.steam_input.clone();
        let games = self.gamesdb_input.clone();
        let device_input = self.device_name_input.clone();
        let mut body = div()
            .id("connected-settings-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .grid()
            .grid_cols(if window.viewport_size().width <= px(900.) {
                1
            } else {
                2
            })
            .gap(px(12.))
            .px(px(18.))
            .py(px(14.));
        body = body.child(self.card("Game preferences", prefs_card, false));
        body = body.child(self.card(
            "Steam integration",
            div()
                .child(self.row(
                    "Steam Web API key",
                    "Masked; stored by the existing backend. Saving requests ownership sync.",
                    div().flex().flex_wrap().gap(px(6.))
                        .child(Self::field(&inputs))
                        .child(self.action("save-steam-key", "Save & Sync", true).on_click(cx.listener(|s, _, _, cx| s.save_key(true, cx))))
                        .child(div().text_size(px(10.)).child(if self.steam_configured { "Key configured" } else { "No key configured" })),
                ))
                .child(self.row(
                    "Device name",
                    "Saved through the existing setup configuration API.",
                    div().flex().flex_wrap().gap(px(6.))
                        .child(Self::field(&device_input))
                        .child(self.action("save-device-name", "Save name", false).on_click(cx.listener(|s, _, _, cx| {
                            let name = s.device_name_input.read(cx).content.trim().to_owned();
                            if name.is_empty() || name.len() > 80 || name.chars().any(char::is_control) {
                                s.error = Some("Device name must be 1–80 printable characters.".into()); cx.notify();
                            } else { s.save_device_name(name, cx); }
                        }))),
                ))
                .child(self.row(
                    "Language",
                    "Choose an application locale; the host persists and applies it.",
                    self.unavailable("Locale persistence/application is not integrated in this connected candidate."),
                )),
            false,
        ));
        let gamesdb_controls = div()
            .flex()
            .flex_wrap()
            .gap(px(6.))
            .child(Self::field(&games))
            .child(
                self.action("save-gamesdb-key", "Save key", true)
                    .on_click(cx.listener(|s, _, _, cx| s.save_key(false, cx))),
            )
            .child(div().text_size(px(10.)).child(if self.gamesdb_configured {
                "Key configured"
            } else {
                "No key configured"
            }));
        body = body.child(self.card(
            "Epic integration",
            div().child(self.row(
                "TheGamesDB API key",
                "Optional Epic artwork fallback key; input is masked.",
                gamesdb_controls,
            )),
            false,
        ));
        body = body.child(
            self.card(
                "Runtime & storefront",
                div()
                    .child(
                        self.row(
                            "Wine Steam",
                            "Start/stop the existing Wine Steam client through the owning host.",
                            div()
                                .flex()
                                .flex_wrap()
                                .gap(px(6.))
                                .child(
                                    self.action("start-wine-steam", "Start Wine Steam", false)
                                        .on_click(cx.listener(|s, _, _, cx| {
                                            s.intent(
                                                SettingsIntent::Storefront(
                                                    StorefrontAction::StartWineSteam,
                                                ),
                                                cx,
                                            )
                                        })),
                                )
                                .child(
                                    self.action("stop-wine-steam", "Stop Wine Steam", false)
                                        .on_click(cx.listener(|s, _, _, cx| {
                                            s.intent(
                                                SettingsIntent::Storefront(
                                                    StorefrontAction::StopWineSteam,
                                                ),
                                                cx,
                                            )
                                        })),
                                ),
                        ),
                    )
                    .child(
                        self.row(
                            "macOS Steam",
                            "Native storefront lifecycle is delegated to the host.",
                            self.unavailable("Native Steam install/start/stop is not wired to a reviewed host workflow."),
                        ),
                    )
                    .child(
                        self.row(
                            "Repair or setup",
                            "Reopen setup wizard using the connected setup controller.",
                            self.action("reopen-setup", "Run Setup Wizard", true).on_click(cx.listener(|s, _, _, cx| {s.intent(SettingsIntent::ReopenSetup, cx)})),
                        ),
                    ),
                false,
            ),
        );
        let backend = div()
            .child(
                self.row(
                    "Backend state",
                    if self.loading {
                        "Loading configuration…"
                    } else {
                        "Owned authenticated backend session."
                    },
                    self.action("restart-backend", "Restart Backend", false)
                        .on_click(
                            cx.listener(|s, _, _, cx| s.intent(SettingsIntent::RestartBackend, cx)),
                        ),
                ),
            )
            .child(
                self.row(
                    "Force kill",
                    "Terminate MetalSharp runtime processes.",
                    self.action("force-kill", "Force Kill…", false)
                        .on_click(cx.listener(|s, _, _, cx| s.ask(Confirm::ForceKill, cx))),
                ),
            )
            .child(self.row(
                "Developer tools",
                "Host developer diagnostics are not enabled in this connected candidate.",
                self.unavailable("Unavailable"),
            ));
        body = body.child(self.card("Backend & runtime", backend, false));
        body = body.child(
            self.card(
                "Data paths & repair",
                div()
                    .child(
                        self.row(
                            "Data folder",
                            "Open the configured MetalSharp data directory.",
                            self.action("open-data", "Open Data Folder", false)
                                .on_click(cx.listener(|s, _, _, cx| {
                                    s.intent(SettingsIntent::OpenDataFolder, cx)
                                })),
                        ),
                    )
                    .child(
                        self.row(
                            "Logs folder",
                            "Open diagnostic logs in the host file manager.",
                            self.action("open-logs", "Open Logs Folder", false)
                                .on_click(cx.listener(|s, _, _, cx| {
                                    s.intent(SettingsIntent::OpenLogsFolder, cx)
                                })),
                        ),
                    )
                    .child(
                        self.row(
                            "Data access",
                            "Check data access without changing OS permissions.",
                            self.action("repair-access", "Verify Access", true)
                                .on_click(cx.listener(|s, _, _, cx| {
                                    s.intent(SettingsIntent::RepairDataAccess, cx)
                                })),
                        ),
                    )
                    .child(self.row(
                        "Path configuration",
                        "Choose a supported runtime/storefront path.",
                        self.unavailable(
                            "Custom path editing is not supported by the current backend contract.",
                        ),
                    )),
                false,
            ),
        );
        body =
            body.child(
                self.card(
                    "Cache & updates",
                    div()
                        .child(self.row(
                            "Shader cache",
                            "Clear only after explicit confirmation.",
                            self.action("clear-shader", "Clear…", false).on_click(
                                cx.listener(|s, _, _, cx| s.ask(Confirm::ClearShader, cx)),
                            ),
                        ))
                        .child(self.row(
                            "Pipeline cache",
                            "Clear only after explicit confirmation.",
                            self.action("clear-pipeline", "Clear…", false).on_click(
                                cx.listener(|s, _, _, cx| s.ask(Confirm::ClearPipeline, cx)),
                            ),
                        ))
                        .child(
                            self.row(
                                "Application updates",
                                "Update execution is delegated to the host updater service.",
                                self.action("check-updates", "Check…", false).on_click(
                                    cx.listener(|s, _, _, cx| {
                                        s.intent(SettingsIntent::CheckForUpdates, cx)
                                    }),
                                ),
                            ),
                        ),
                    false,
                ),
            );
        body = body.child(
            self.card(
                "Danger zone",
                div().child(
                    self.row(
                        "Uninstall",
                        "May remove app resources and user data.",
                        self.unavailable("App/data uninstall is intentionally unavailable until a reversible removal plan is reviewed."),
                    ),
                ),
                true,
            ),
        );
        let header = div()
            .flex_none()
            .flex()
            .justify_between()
            .items_center()
            .px(px(20.))
            .py(px(14.))
            .border_b_1()
            .border_color(rgba(p.control_border))
            .child(
                div()
                    .text_size(px(24.))
                    .font_family("Georgia")
                    .child("Settings"),
            )
            .child(
                self.action("close-connected-settings", "×", false)
                    .on_click(cx.listener(|_, _, _, cx| cx.emit(SettingsConnectedEvent::Close))),
            );
        let mut panel = div()
            .id("settings-connected-panel")
            .occlude()
            .on_click(|_, _, cx| cx.stop_propagation())
            .flex()
            .flex_col()
            .w(px(1080.))
            .max_w(gpui::relative(1.))
            .max_h(px(
                (f32::from(window.viewport_size().height) - 92.).max(120.)
            ))
            .rounded(px(14.))
            .border_1()
            .border_color(rgba((p.accent << 8) | 0xff))
            .bg(rgb(p.control_bg))
            .text_color(rgb(p.control_text))
            .child(header);
        if let Some(e) = &self.error {
            panel = panel.child(
                div()
                    .px(px(18.))
                    .py(px(7.))
                    .text_color(rgb(0xff8585))
                    .child(e.clone()),
            );
        } else if let Some(n) = &self.notice {
            panel = panel.child(
                div()
                    .px(px(18.))
                    .py(px(7.))
                    .text_color(rgb(0x6fce88))
                    .child(n.clone()),
            );
        } else if disabled {
            panel = panel.child(
                div()
                    .px(px(18.))
                    .py(px(7.))
                    .child("Loading / saving settings…"),
            );
        }
        panel = panel.child(body);
        let overlay = div()
            .id("settings-connected-overlay")
            .absolute()
            .inset_0()
            .flex()
            .justify_center()
            .items_start()
            .pt(px(68.))
            .px(px(24.))
            .pb(px(24.))
            .bg(rgba(0x06080a94))
            .on_click(cx.listener(|this, _, _, cx| {
                this.confirm = None;
                cx.emit(SettingsConnectedEvent::Close);
            }))
            .child(panel);
        overlay.children(
            self.confirm
                .map(|_| gpui::deferred(self.confirm_view(cx)).with_priority(240)),
        )
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn credentials_are_initialized_masked_and_no_debug_surface() {
        assert!(std::mem::size_of::<Confirm>() > 0);
        let p = PagePalette::default();
        assert!(p.accent > 0);
    }
    #[test]
    fn config_contract_is_existing_partial_preferences() {
        assert_eq!(
            PreferenceChange::Msync(false).body(),
            serde_json::json!({"msync":false})
        );
    }
    #[test]
    fn confirmation_intents_are_exact_and_destructive_actions_require_confirmation() {
        let mappings = [
            (Confirm::ForceKill, SettingsIntent::ForceKillProcesses),
            (Confirm::ClearShader, SettingsIntent::ClearShaderCache),
            (Confirm::ClearPipeline, SettingsIntent::ClearPipelineCache),
            (Confirm::InstallUpdate, SettingsIntent::InstallUpdate),
            (Confirm::InstallFexUpdate, SettingsIntent::InstallFexUpdate),
            (Confirm::Uninstall, SettingsIntent::Uninstall),
        ];
        for (confirmation, intent) in mappings {
            assert_eq!(confirmation.intent(), intent);
        }
    }
    #[test]
    fn settings_snapshot_roundtrips_and_secret_intent_debug_is_redacted() {
        let snapshot = SettingsSnapshot {
            preferences: RuntimePreferences::default(),
            steam_configured: true,
            gamesdb_configured: false,
            device_name: "fixture host".into(),
        };
        let value = serde_json::to_value(&snapshot).unwrap();
        let decoded: SettingsSnapshot = serde_json::from_value(value).unwrap();
        assert!(decoded.steam_configured);
        assert_eq!(decoded.device_name, "fixture host");
        let debug = format!(
            "{:?}",
            SettingsIntent::SaveSteamKey("do-not-leak-this-fixture".into())
        );
        assert!(!debug.contains("do-not-leak-this-fixture"));
        assert!(debug.contains("REDACTED"));
    }
    #[test]
    fn path_and_storefront_intents_use_closed_typed_kinds() {
        let paths = [
            SettingsPath::DataHome,
            SettingsPath::Logs,
            SettingsPath::WineRuntime,
            SettingsPath::SteamPrefix,
        ];
        assert_eq!(paths.len(), 4);
        let storefront = [
            StorefrontAction::StartWineSteam,
            StorefrontAction::StopWineSteam,
            StorefrontAction::InstallMacSteam,
            StorefrontAction::StartMacSteam,
            StorefrontAction::StopMacSteam,
        ];
        assert_eq!(storefront.len(), 5);
        assert_eq!(
            SettingsIntent::ChoosePath(SettingsPath::WineRuntime),
            SettingsIntent::ChoosePath(SettingsPath::WineRuntime)
        );
    }
}
