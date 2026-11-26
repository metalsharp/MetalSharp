//! Rendering for the live Sharp sources, reusing the approved card styling.
use super::sharp_live::{SharpConfirm, engine_options};
use super::*;
use serde_json::Value;

fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn b(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}

fn format_bytes(bytes: u64) -> String {
    if bytes < 1024 {
        format!("{bytes} B")
    } else if bytes < 1024 * 1024 {
        format!("{:.1} KB", bytes as f64 / 1024.0)
    } else if bytes < 1024 * 1024 * 1024 {
        format!("{:.1} MB", bytes as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}

pub(super) fn live_pill(text: String) -> gpui::Div {
    div()
        .px(px(8.))
        .py(px(4.))
        .rounded(px(20.))
        .bg(rgb(crate::page_palette::tc(0x28251f)))
        .text_size(px(9.))
        .text_color(rgb(GOLD))
        .font_weight(FontWeight::BOLD)
        .child(text.to_uppercase())
}

impl SharpPreview {
    /// Header buttons for the live PC sources.
    pub(super) fn live_header_controls(&self, cx: &mut Context<Self>) -> gpui::Div {
        let live = self.live.as_ref().unwrap();
        let mut controls = div().flex().items_center();
        let button =
            |this: &Self, id: &'static str, label: String, primary: bool, enabled: bool| {
                this.palette
                    .button(id, label, primary)
                    .h(px(40.0))
                    .mr(px(12.0))
                    .rounded(px(9.0))
                    .opacity(if enabled { 1.0 } else { 0.4 })
            };
        match self.state.source {
            SharpSource::Installers => {
                controls = controls.child(
                    button(
                        self,
                        "sharp-install-exe",
                        "Install Windows Program".into(),
                        true,
                        true,
                    )
                    .on_click(cx.listener(|this, _, _, cx| this.install_exe(cx))),
                );
            }
            SharpSource::Gog => {
                let status = live.gog_status.clone().unwrap_or(Value::Null);
                // GOG's prefix is set up and removed from the GOG button's settings in the header
                let initialized = b(&status, "prefixInitialized");
                let authenticated = b(&status, "authenticated");
                let login_busy = live.gog_loading.contains("login");
                let label = if authenticated {
                    "GOG Connected".to_owned()
                } else if login_busy {
                    "Checking…".to_owned()
                } else {
                    "Open GOG".to_owned()
                };
                let enabled = !login_busy && initialized;
                controls = controls.child(
                    button(self, "sharp-gog-auth", label, true, enabled).on_click(cx.listener(
                        move |this, _, _, cx| {
                            if enabled {
                                this.gog_auth_button(cx);
                            }
                        },
                    )),
                );
            }
            SharpSource::Epic => {
                let status = live.epic_status.clone().unwrap_or(Value::Null);
                if !b(&status, "toolAvailable") {
                    let busy = live.epic_loading.contains("tool");
                    controls = controls.child(
                        button(
                            self,
                            "sharp-epic-tool",
                            if busy {
                                "Installing…".into()
                            } else {
                                "Install Epic".into()
                            },
                            false,
                            !busy,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if !busy {
                                this.install_epic_support(cx, false);
                            }
                        })),
                    );
                } else {
                    let busy = live.epic_loading.contains("login");
                    let label = if b(&status, "authenticated") {
                        let account = s(&status, "account");
                        if account.is_empty() {
                            "Epic Connected".to_owned()
                        } else {
                            account.to_owned()
                        }
                    } else if busy {
                        "Checking…".to_owned()
                    } else {
                        "Open Epic".to_owned()
                    };
                    controls = controls.child(
                        button(self, "sharp-epic-auth", label, true, !busy).on_click(cx.listener(
                            move |this, _, _, cx| {
                                if !busy {
                                    this.epic_auth_button(cx);
                                }
                            },
                        )),
                    );
                }
            }
            SharpSource::GameJolt => {
                controls = controls.child(
                    button(
                        self,
                        "sharp-gj-storage",
                        "Game Jolt Folder".into(),
                        false,
                        true,
                    )
                    .on_click(cx.listener(|this, _, _, cx| this.choose_gamejolt_storage(cx))),
                );
            }
        }
        let (label, busy) = match self.state.source {
            SharpSource::Gog => (
                if live.gog_loading.contains("sync") {
                    "↻  Sync"
                } else {
                    "↻  Sync GOG"
                },
                live.gog_loading.contains("sync"),
            ),
            SharpSource::Epic => (
                if live.epic_loading.contains("sync") {
                    "↻  Sync"
                } else {
                    "↻  Sync Epic"
                },
                live.epic_loading.contains("sync"),
            ),
            SharpSource::GameJolt => (
                if live.gj_loading {
                    "↻  Sync"
                } else {
                    "↻  Sync Game Jolt"
                },
                false,
            ),
            _ => ("↻  Refresh", false),
        };
        controls.child(
            button(self, "sharp-refresh", label.into(), false, !busy).on_click(cx.listener(
                move |this, _, _, cx| {
                    if !busy {
                        this.refresh_current_source(cx);
                    }
                },
            )),
        )
    }

    fn engine_picker(
        &self,
        key: String,
        current: String,
        options: Vec<(String, String)>,
        on_select: impl Fn(&mut Self, String, &mut Context<Self>) + Clone + 'static,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let open = self.live.as_ref().unwrap().engine_menu.as_deref() == Some(key.as_str());
        // On FEX a saved D3DMetal engine is the GPTK (Rosetta) tool flow; a saved VKD3D or M13
        // engine runs on Mullion, so it reads as Mullion.
        let gptk = crate::variant::is_fex()
            && current == "d3dmetal"
            && !options.iter().any(|(id, _)| *id == current);
        let current = if crate::variant::is_fex()
            && !gptk
            && !options.iter().any(|(id, _)| *id == current)
            && options
                .iter()
                .any(|(id, _)| id.as_str() == crate::variant::fex_pipeline(&current))
        {
            crate::variant::fex_pipeline(&current).to_owned()
        } else {
            current
        };
        let label = options
            .iter()
            .find(|(id, _)| *id == current)
            .map(|(_, label)| label.clone())
            .or_else(|| gptk.then(|| crate::variant::FEX_GPTK_LABEL.to_owned()))
            .unwrap_or_else(|| {
                if current.is_empty() || current == "auto" {
                    "Auto".into()
                } else {
                    current.clone()
                }
            });
        let toggle_key = key.clone();
        // The deck's graphics dropdown (library-pipeline-select): the lane's name and a chevron, on the hero's
        // control glass under Liquid Glass.
        let glass = self.palette.glass;
        let mut field = div().relative().w_full().child(
            div()
                .id(gpui::SharedString::from(format!("engine-{key}")))
                .relative()
                .w_full()
                .h(px(34.0))
                .flex()
                .items_center()
                .gap(px(8.0))
                .px(px(12.0))
                .rounded(px(if glass { 999.0 } else { 8.0 }))
                .border_1()
                .border_color(gpui::rgba(crate::page_palette::tca(0xffffff4d)))
                .bg(gpui::rgba(if glass {
                    (self.palette.accent << 8) | 0x30
                } else {
                    crate::page_palette::tca(0x0c0f10b8)
                }))
                .when(glass, |control| {
                    control.child(crate::liquid_glass::mark(
                        format!("engine-{key}"),
                        crate::liquid_glass::Glass::Control,
                        999.0,
                    ))
                })
                .text_size(px(12.0))
                .text_color(rgb(crate::page_palette::tc(0xf0f0ee)))
                .cursor_pointer()
                .child(div().flex_1().min_w_0().truncate().child(label))
                .child(
                    gpui::svg()
                        .path("icon-chevron-down.svg")
                        .size(px(16.0))
                        .flex_none()
                        .text_color(rgb(crate::page_palette::tc(0xf0f0ee))),
                )
                .on_click(cx.listener(move |this, _, _, cx| {
                    let live = this.live.as_mut().unwrap();
                    live.engine_menu = if live.engine_menu.as_deref() == Some(toggle_key.as_str()) {
                        None
                    } else {
                        Some(toggle_key.clone())
                    };
                    cx.notify();
                })),
        );
        if open {
            let mut menu = div()
                .id(gpui::SharedString::from(format!("engine-menu-{key}")))
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
            for (index, (id, label)) in options.into_iter().enumerate() {
                let on_select = on_select.clone();
                menu = menu.child(
                    self.palette
                        .button(
                            gpui::SharedString::from(format!("engine-{key}-{index}")),
                            label,
                            id == current,
                        )
                        .w_full()
                        .justify_start()
                        .border_0()
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.live.as_mut().unwrap().engine_menu = None;
                            on_select(this, id.clone(), cx);
                            cx.notify();
                        })),
                );
            }
            field = field.child(gpui::deferred(menu).with_priority(60));
        }
        field
    }

    pub(super) fn bottle_panel(&self, title: &'static str, content: gpui::Div) -> gpui::Div {
        div()
            .p(px(10.0))
            .rounded(px(8.0))
            .bg(gpui::rgba(crate::page_palette::tca(0x00000028)))
            .border_1()
            .border_color(gpui::rgba(crate::page_palette::tca(0xffffff14)))
            .flex()
            .flex_col()
            .gap(px(8.0))
            .child(
                div()
                    .text_size(px(9.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(MUTED))
                    .child(title),
            )
            .child(content)
    }

    fn card_shell(
        &self,
        key: &str,
        width: f32,
        art: Option<std::path::PathBuf>,
        _ratio: f32,
        body: gpui::Div,
    ) -> gpui::Stateful<gpui::Div> {
        let banner = match art {
            Some(art) => crate::artwork::art_img(art)
                .size_full()
                .object_fit(ObjectFit::Cover)
                .into_any_element(),
            None => div()
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .bg(rgb(crate::page_palette::tc(0x15181a)))
                .child(
                    img(self.asset_root.join("metalsharp-logo.png"))
                        .size(px(width * 0.4))
                        .object_fit(ObjectFit::Contain),
                )
                .into_any_element(),
        };
        self.art_card_frame(key, width, banner, body)
    }

    /// A store's Collection card (GOG, Epic, Game Jolt): its art fills the whole card, and its title and actions sit over the art's foot on a dark
    /// fade. The card is at least 5:4 portrait, and taller when its actions need it.
    fn art_card_frame(
        &self,
        key: &str,
        width: f32,
        banner: gpui::AnyElement,
        body: gpui::Div,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(gpui::SharedString::from(format!("sharp-live-card-{key}")))
            .relative()
            .w(px(width))
            .min_w_0()
            .min_h(px(width * 1.25))
            .flex()
            .flex_col()
            .justify_end()
            .overflow_hidden()
            .rounded(px(12.0))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(rgb(crate::page_palette::tc(0x15181a)))
            .child(div().absolute().inset_0().child(banner))
            .child(div().absolute().inset_0().bg(gpui::linear_gradient(
                180.0,
                gpui::linear_color_stop(gpui::rgba(0x00000000), 0.35),
                gpui::linear_color_stop(gpui::rgba(0x000000e0), 1.0),
            )))
            .child(div().relative().child(body))
    }

    fn card_frame(
        &self,
        key: &str,
        width: f32,
        banner: gpui::AnyElement,
        body: gpui::Div,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(gpui::SharedString::from(format!("sharp-live-card-{key}")))
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
                gpui::linear_color_stop(rgb(crate::page_palette::tc(0x202427)), 0.0),
                gpui::linear_color_stop(rgb(crate::page_palette::tc(0x171a1c)), 1.0),
            ))
            .child(banner)
            .child(body)
    }

    /// The game's settings (Graphics, Runtime, Advanced, Presentation) from its Collection card, as the Steam
    /// Collection grid's gear opens them (#833): a gear in the banner's corner. `source` and `id` are the deck's,
    /// so only cards the deck lists (`deck_games`) get one. `beside_close` moves it left of a running program's
    /// close button, which has the corner.
    fn with_settings_gear(
        &self,
        card: gpui::Stateful<gpui::Div>,
        source: &'static str,
        id: String,
        beside_close: bool,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        card.relative().child(
            div()
                .id(gpui::SharedString::from(format!(
                    "sharp-card-settings-{source}-{id}"
                )))
                .absolute()
                .top(px(8.0))
                .right(px(if beside_close { 40.0 } else { 8.0 }))
                .size(px(28.0))
                .flex()
                .items_center()
                .justify_center()
                .rounded(px(999.0))
                .border_1()
                .border_color(gpui::rgba(self.palette.control_border))
                .bg(gpui::rgba((self.palette.control_bg << 8) | 0xd8))
                .cursor_pointer()
                .hover(|style| style.opacity(0.85))
                .on_click(cx.listener(move |_, _, _, cx| {
                    cx.stop_propagation();
                    cx.emit(SharpPreviewEvent::OpenGameSettings {
                        source,
                        id: id.clone(),
                    });
                }))
                .child(
                    gpui::svg()
                        .path("launch-settings.svg")
                        .size(px(15.0))
                        .flex_none()
                        .text_color(rgb(self.palette.control_text)),
                ),
        )
    }

    fn card_body(
        &self,
        title: impl IntoElement,
        pill: String,
        meta: String,
        progress: Option<f64>,
    ) -> gpui::Div {
        div()
            .p(px(14.0))
            .pt(px(13.0))
            .flex()
            .flex_col()
            .gap(px(10.0))
            .child(
                div()
                    .text_size(px(14.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(crate::page_palette::tc(0xf0f0ed)))
                    .child(title),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(live_pill(pill))
                    .child(
                        div()
                            .min_w_0()
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child(meta),
                    ),
            )
            .children(progress.map(|percent| {
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .flex_1()
                            .h(px(5.0))
                            .rounded_full()
                            .bg(rgb(crate::page_palette::tc(0x2a2d30)))
                            .child(
                                div()
                                    .h(px(5.0))
                                    .rounded_full()
                                    .bg(rgb(self.palette.accent))
                                    .w(gpui::relative((percent / 100.0).clamp(0.0, 1.0) as f32)),
                            ),
                    )
                    .child(
                        div()
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child(format!("{}%", percent.floor() as u64)),
                    )
            }))
    }

    fn action(
        &self,
        id: String,
        label: &'static str,
        primary: bool,
        danger: bool,
        enabled: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let button = self
            .palette
            .button(gpui::SharedString::from(id), label, primary)
            .h(px(32.0))
            .rounded(px(6.0))
            .opacity(if enabled { 1.0 } else { 0.45 });
        if danger {
            button
                .text_color(rgb(crate::page_palette::tc(0xff8585)))
                .border_color(gpui::rgba(crate::page_palette::tca(0xff5c5c55)))
        } else {
            button
        }
    }

    /// A store card's actions: the game's graphics lane (the deck's dropdown) with its main action (Play, Stop)
    /// beside it, or the main action (Install) across the card for a game that isn't installed. Uninstalling is in
    /// the game's settings (its gear's Uninstall tab).
    fn card_actions(
        &self,
        lane: Option<gpui::Div>,
        primary: gpui::Stateful<gpui::Div>,
    ) -> gpui::Div {
        match lane {
            Some(lane) => div()
                .flex()
                .items_center()
                .gap(px(8.0))
                .child(div().flex_1().min_w_0().child(lane))
                .child(
                    primary
                        .flex_none()
                        .h(px(34.0))
                        .px(px(20.0))
                        .rounded(px(8.0))
                        .text_size(px(14.0)),
                ),
            None => div().child(
                primary
                    .w_full()
                    .h(px(38.0))
                    .rounded(px(8.0))
                    .text_size(px(14.0)),
            ),
        }
    }

    fn gate(&self, heading: &str, desc: &str) -> gpui::Div {
        div()
            .w_full()
            .flex_1()
            .min_h(px(220.0))
            .rounded(px(12.0))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(gpui::rgba((self.palette.control_bg << 8) | 0x85))
            .px(px(20.0))
            .py(px(28.0))
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap(px(8.0))
            .child(
                div()
                    .text_size(px(17.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(rgb(TEXT))
                    .child(heading.to_owned()),
            )
            .children((!desc.is_empty()).then(|| {
                div()
                    .max_w(px(520.0))
                    .text_center()
                    .text_size(px(12.0))
                    .text_color(rgb(MUTED))
                    .child(desc.to_owned())
            }))
    }

    /// Library content for the live PC sources.
    pub(super) fn live_content(&self, card_width: f32, cx: &mut Context<Self>) -> gpui::Div {
        let live = self.live.as_ref().unwrap();
        let mut grid = div().flex().flex_wrap().gap(px(14.));
        match self.state.source {
            SharpSource::Installers => {
                if live.apps.is_empty() {
                    return div().w_full().child(
                        div()
                            .w_full()
                            .min_h(px(320.))
                            .rounded(px(12.))
                            .border_1()
                            .border_color(gpui::rgba(self.palette.control_border))
                            .bg(gpui::rgba(crate::page_palette::tca(0x080a0d85)))
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
                                    .text_color(rgb(crate::page_palette::tc(0x747c7f))),
                            )
                            .child(
                                div()
                                    .text_size(px(18.))
                                    .text_color(rgb(TEXT))
                                    .font_weight(FontWeight::BOLD)
                                    .child("No applications installed"),
                            )
                            .child(div().text_size(px(12.)).text_color(rgb(MUTED)).child(
                                "Install a Windows program to add it to the Sharp Library.",
                            )),
                    );
                }
                for app in live.apps.clone() {
                    let id = s(&app, "id").to_owned();
                    let running = live.sharp_running.contains_key(&id);
                    let engine = super::sharp_live::app_engine(&app);
                    let size = app
                        .get("size_bytes")
                        .and_then(Value::as_u64)
                        .filter(|b| *b > 0);
                    let meta = format!(
                        "Windows{}",
                        size.map(|b| format!(" · {}", format_bytes(b)))
                            .unwrap_or_default()
                    );
                    let bottle = self.bottle_for_app(&app);
                    let open = live.open_bottle.contains(&id);
                    let play_app = app.clone();
                    let mut actions = div().flex().items_center().gap(px(8.0)).child(
                        self.action(
                            format!("app-play-{id}"),
                            if running { "Stop" } else { "Play" },
                            true,
                            false,
                            true,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let running = this
                                .live
                                .as_ref()
                                .is_some_and(|l| l.sharp_running.contains_key(s(&play_app, "id")));
                            if running {
                                this.stop_app(play_app.clone(), cx);
                            } else {
                                this.launch_app(play_app.clone(), cx);
                            }
                        })),
                    );
                    let tools_id = id.clone();
                    actions = actions.child(
                        self.action(
                            format!("app-tools-{id}"),
                            if open { "Tools  ⌃" } else { "Tools  ⌄" },
                            false,
                            false,
                            true,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let live = this.live.as_mut().unwrap();
                            if !live.open_bottle.remove(&tools_id) {
                                live.open_bottle.insert(tools_id.clone());
                            }
                            cx.notify();
                        })),
                    );
                    let title = self.name_title(false, &id, s(&app, "name"), cx);
                    let mut body = self.card_body(
                        title,
                        if running {
                            "Running".into()
                        } else {
                            "Ready".into()
                        },
                        meta,
                        None,
                    );
                    if let Some(bottle) = &bottle {
                        body = body.child(div().flex().child(live_pill(
                            super::sharp_tools::bottle_badge_label(bottle).into(),
                        )));
                        let log = s(bottle, "last_launch_log").to_owned();
                        if s(bottle, "last_launch_status") == "exited" && !log.is_empty() {
                            body = body.child(
                                div()
                                    .id(gpui::SharedString::from(format!("app-launch-log-{id}")))
                                    .text_size(px(10.0))
                                    .text_color(rgb(self.palette.accent))
                                    .cursor_pointer()
                                    .hover(|style| style.underline())
                                    .child("Open launch log")
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.open_bottle_launch_log(log.clone(), cx)
                                    })),
                            );
                        }
                    }
                    body = body.child(actions);
                    if open {
                        let options = super::sharp_live::app_engine_options();
                        let engine_id = id.clone();
                        let cover_id = id.clone();
                        let asset_app = app.clone();
                        let uninstall = (id.clone(), s(&app, "name").to_owned());
                        let mut content = div()
                            .flex()
                            .flex_col()
                            .gap(px(8.0))
                            .child(self.engine_picker(
                                format!("app-{id}"),
                                engine,
                                options,
                                move |this, value, cx| {
                                    this.set_app_engine(engine_id.clone(), value, cx)
                                },
                                cx,
                            ))
                            .child(
                                div()
                                    .flex()
                                    .gap(px(8.0))
                                    .child(
                                        self.action(
                                            format!("app-cover-{id}"),
                                            "Change Image",
                                            false,
                                            false,
                                            true,
                                        )
                                        .flex_1()
                                        .on_click(
                                            cx.listener(move |this, _, _, cx| {
                                                this.set_app_cover(cover_id.clone(), cx)
                                            }),
                                        ),
                                    )
                                    .child(
                                        self.action(
                                            format!("app-asset-{id}"),
                                            "Add Asset",
                                            false,
                                            false,
                                            !running,
                                        )
                                        .flex_1()
                                        .on_click(
                                            cx.listener(move |this, _, _, cx| {
                                                this.add_app_asset(&asset_app, cx)
                                            }),
                                        ),
                                    ),
                            );
                        let has_cover = app
                            .get("cover")
                            .is_some_and(|c| !c.is_null() && c != &Value::Bool(false) && c != "");
                        if has_cover {
                            content = content.child(self.cover_sliders(&app, cx));
                        }
                        content = content.child(
                            self.action(
                                format!("app-uninstall-{id}"),
                                "Uninstall",
                                false,
                                true,
                                !running,
                            )
                            .w_full()
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    this.live.as_mut().unwrap().confirm =
                                        Some(SharpConfirm::UninstallApp(
                                            uninstall.0.clone(),
                                            uninstall.1.clone(),
                                        ));
                                    cx.notify();
                                },
                            )),
                        );
                        body = body.child(self.bottle_panel("GRAPHICS BACKEND", content));
                    }
                    body = body.child(self.app_diagnostics(&app, cx));
                    let banner =
                        self.app_banner(&app, card_width, card_width * 9.0 / 16.0, running, cx);
                    let mut card = self.card_frame(&format!("app-{id}"), card_width, banner, body);
                    // Setup and installer executables aren't on the deck, so they have no game settings.
                    let adopted = !s(&app, "adopted_from").is_empty();
                    if adopted || !super::sharp_deck::installer_like(s(&app, "exe_path")) {
                        card = self.with_settings_gear(card, "windows", id.clone(), running, cx);
                    }
                    grid = grid.child(card);
                }
                div().w_full().child(grid)
            }
            SharpSource::Gog => {
                let status = live.gog_status.clone().unwrap_or(Value::Null);
                if !b(&status, "gogdlAvailable")
                    && live.gog_status.is_some()
                    && !b(&status, "prefixInitialized")
                {
                    return self.gate("Initialize Prefix To Get Started", "");
                }
                if !b(&status, "prefixInitialized") {
                    return self.gate(
                        "Initialize GOG Prefix",
                        "Create the isolated Wine prefix before connecting games.",
                    );
                }
                if !b(&status, "authenticated") {
                    return self.gate(
                        "Login to GOG to connect your games",
                        "MetalSharp will capture the GOG login code from a controlled sign-in window.",
                    );
                }
                if live.gog_games.is_empty() {
                    return self.gate(
                        "No GOG games synced",
                        "Click Sync Library after adding games to your GOG account.",
                    );
                }
                for game in live.gog_games.clone() {
                    let id = s(&game, "productId").to_owned();
                    let running = b(&game, "running");
                    let installed = b(&game, "installed");
                    let downloading = s(&game, "status") == "downloading";
                    let pill = if running {
                        "Running"
                    } else if installed {
                        "Installed"
                    } else if downloading {
                        "Downloading"
                    } else {
                        "GOG"
                    };
                    let meta = game
                        .get("downloadSizeBytes")
                        .and_then(Value::as_u64)
                        .filter(|b| *b > 0)
                        .map(format_bytes)
                        .unwrap_or_default();
                    let progress =
                        downloading.then(|| live.gog_progress.get(&id).copied().unwrap_or(0.0));
                    let mut body =
                        self.card_body(s(&game, "title").to_owned(), pill.into(), meta, progress);
                    let g = game.clone();
                    let primary = if running {
                        let busy = live.gog_loading.contains(&format!("{id}:stop"));
                        self.action(format!("gog-stop-{id}"), "Stop", true, false, !busy)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if !this
                                    .live
                                    .as_ref()
                                    .unwrap()
                                    .gog_loading
                                    .contains(&format!("{}:stop", s(&g, "productId")))
                                {
                                    this.stop_gog_game(g.clone(), cx)
                                }
                            }))
                    } else if installed {
                        let busy = live.gog_loading.contains(&format!("{id}:play"));
                        self.action(format!("gog-play-{id}"), "Play", true, false, !busy)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if !this
                                    .live
                                    .as_ref()
                                    .unwrap()
                                    .gog_loading
                                    .contains(&format!("{}:play", s(&g, "productId")))
                                {
                                    this.play_gog_game(g.clone(), cx);
                                }
                            }))
                    } else {
                        let busy =
                            downloading || live.gog_loading.contains(&format!("{id}:install"));
                        self.action(
                            format!("gog-install-{id}"),
                            if downloading {
                                "Downloading…"
                            } else {
                                "Install"
                            },
                            true,
                            false,
                            !busy,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if !busy {
                                this.install_gog_game(g.clone(), cx);
                            }
                        }))
                    };
                    let lane = installed.then(|| {
                        let engine = super::sharp_live::engine_lane(
                            live.gog_engines
                                .get(&id)
                                .map(String::as_str)
                                .unwrap_or("auto"),
                            super::sharp_live::STORE_AUTO_LANE,
                        );
                        let engine_id = id.clone();
                        self.engine_picker(
                            format!("gog-{id}"),
                            engine,
                            engine_options(),
                            move |this, value, cx| {
                                this.set_gog_engine(engine_id.clone(), value, cx)
                            },
                            cx,
                        )
                    });
                    body = body.child(self.card_actions(lane, primary));
                    let card = self.card_shell(
                        &format!("gog-{id}"),
                        card_width,
                        self.cover_path(&format!("sharp-gog-{id}"), cx),
                        9.0 / 16.0,
                        body,
                    );
                    grid = grid.child(self.with_settings_gear(card, "gog", id.clone(), false, cx));
                }
                div().w_full().child(grid)
            }
            SharpSource::Epic => {
                let status = live.epic_status.clone().unwrap_or(Value::Null);
                if !b(&status, "toolAvailable") {
                    return self.gate(
                        "Install Epic Support",
                        "MetalSharp uses the pinned open-source Legendary client to access your owned library without running the Epic Games Launcher.",
                    );
                }
                if !b(&status, "authenticated") {
                    return self.gate(
                        "Login to Epic Games",
                        "Sign in through Epic’s website. MetalSharp stores Legendary account state only under ~/.metalsharp.",
                    );
                }
                if live.epic_games.is_empty() {
                    return self.gate(
                        "No installable Epic games found",
                        "Sync your library after adding games to your Epic account. Third-party launcher-only titles are omitted.",
                    );
                }
                for game in live.epic_games.clone() {
                    let app = s(&game, "appName").to_owned();
                    let running = b(&game, "running");
                    let installed = b(&game, "installed");
                    let initialized = b(&game, "bottleInitialized");
                    let tracked = live.epic_progress.get(&app).copied();
                    let pill = if running {
                        "Running"
                    } else if installed {
                        "Installed"
                    } else if tracked.is_some() {
                        "Downloading"
                    } else {
                        "Epic"
                    };
                    let meta = game
                        .get("installSize")
                        .and_then(Value::as_u64)
                        .filter(|b| *b > 0)
                        .map(format_bytes)
                        .unwrap_or_default();
                    let mut body =
                        self.card_body(s(&game, "title").to_owned(), pill.into(), meta, tracked);
                    let g = game.clone();
                    let primary = if running {
                        self.action(format!("epic-stop-{app}"), "Stop", true, false, true)
                            .on_click(
                                cx.listener(move |this, _, _, cx| {
                                    this.stop_epic_game(g.clone(), cx)
                                }),
                            )
                    } else if installed && initialized {
                        let busy = live.epic_loading.contains(&format!("{app}:play"));
                        self.action(format!("epic-play-{app}"), "Play", true, false, !busy)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if !busy {
                                    this.play_epic_game(g.clone(), cx);
                                }
                            }))
                    } else if installed {
                        let busy = live.epic_loading.contains(&format!("{app}:initialize"));
                        self.action(
                            format!("epic-init-{app}"),
                            "Initialize Bottle",
                            true,
                            false,
                            !busy,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if !busy {
                                this.initialize_epic_bottle(g.clone(), cx);
                            }
                        }))
                    } else if tracked.is_some() {
                        self.action(
                            format!("epic-cancel-{app}"),
                            "Stop Download",
                            false,
                            false,
                            true,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.cancel_epic_install(g.clone(), cx)
                        }))
                    } else {
                        let busy = live.epic_loading.contains(&format!("{app}:install"));
                        self.action(format!("epic-install-{app}"), "Install", true, false, !busy)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if !busy {
                                    this.install_epic_game(g.clone(), cx);
                                }
                            }))
                    };
                    let lane = installed.then(|| {
                        let pipeline_app = app.clone();
                        self.engine_picker(
                            format!("epic-{app}"),
                            super::sharp_live::engine_lane(
                                s(&game, "pipeline"),
                                super::sharp_live::STORE_AUTO_LANE,
                            ),
                            engine_options(),
                            move |this, value, cx| {
                                this.update_epic_bottle(pipeline_app.clone(), "pipeline", value, cx)
                            },
                            cx,
                        )
                    });
                    body = body.child(self.card_actions(lane, primary));
                    // Epic games' mouse mode, which only this card sets
                    if installed {
                        let mouse_app = app.clone();
                        body = body.child(self.engine_picker(
                            format!("epic-mouse-{app}"),
                            match s(&game, "mouseMode") {
                                "" => "no-recenter".into(),
                                other => other.into(),
                            },
                            vec![
                                ("no-recenter".into(), "Mouse: No recenter".into()),
                                ("auto".into(), "Mouse: Auto".into()),
                            ],
                            move |this, value, cx| {
                                this.update_epic_bottle(mouse_app.clone(), "mouseMode", value, cx)
                            },
                            cx,
                        ));
                    }
                    let card = self.card_shell(
                        &format!("epic-{app}"),
                        card_width,
                        self.cover_path(&format!("sharp-epic-{app}"), cx),
                        9.0 / 16.0,
                        body,
                    );
                    grid =
                        grid.child(self.with_settings_gear(card, "epic", app.clone(), false, cx));
                }
                div().w_full().child(grid)
            }
            SharpSource::GameJolt => {
                let mut content = div().w_full().flex().flex_col().gap(px(12.0));
                if let Some(storage) = &live.gj_storage {
                    let dir = s(storage, "gamejoltDir");
                    if !dir.is_empty() {
                        content = content.child(
                            div()
                                .text_size(px(11.0))
                                .text_color(rgb(MUTED))
                                .child(format!("Library folder: {dir}")),
                        );
                    }
                }
                if live.gj_games.is_empty() {
                    return content.child(self.gate(
                        "No Game Jolt games yet",
                        "Download a game in the Game Jolt browser below, or put each game in its own folder in the Game Jolt folder and sync.",
                    ));
                }
                for game in live.gj_games.clone() {
                    let id = s(&game, "id").to_owned();
                    let running = live.gj_running.contains_key(&id);
                    let native = b(&game, "native");
                    let title = self.name_title(true, &id, s(&game, "name"), cx);
                    let mut body = self.card_body(
                        title,
                        if running {
                            "Running".into()
                        } else {
                            "Ready".into()
                        },
                        if native {
                            "Native".into()
                        } else {
                            "Windows".into()
                        },
                        None,
                    );
                    let g = game.clone();
                    let mut actions = div().flex().items_center().gap(px(8.0)).child(
                        self.action(
                            format!("gj-play-{id}"),
                            if running { "Stop" } else { "Play" },
                            true,
                            false,
                            true,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let running = this
                                .live
                                .as_ref()
                                .is_some_and(|l| l.gj_running.contains_key(s(&g, "id")));
                            if running {
                                this.stop_gamejolt(g.clone(), cx);
                            } else {
                                this.launch_gamejolt(g.clone(), cx);
                            }
                        })),
                    );
                    let confirm = (
                        id.clone(),
                        s(&game, "name").to_owned(),
                        s(&game, "install_dir").to_owned(),
                    );
                    actions = actions.child(
                        self.action(
                            format!("gj-uninstall-{id}"),
                            "Uninstall",
                            false,
                            true,
                            !running,
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let running = this
                                .live
                                .as_ref()
                                .is_some_and(|l| l.gj_running.contains_key(&confirm.0));
                            if !running {
                                this.live.as_mut().unwrap().confirm =
                                    Some(SharpConfirm::UninstallGameJolt(
                                        confirm.0.clone(),
                                        confirm.1.clone(),
                                        confirm.2.clone(),
                                    ));
                                cx.notify();
                            }
                        })),
                    );
                    if native {
                        actions = actions.child(
                            div()
                                .ml_auto()
                                .text_size(px(10.0))
                                .text_color(rgb(MUTED))
                                .child("No bottle required"),
                        );
                    }
                    body = body.child(actions);
                    if !native {
                        let options = super::sharp_live::gamejolt_engine_options(&game);
                        let engine_id = id.clone();
                        body = body.child(self.bottle_panel(
                            "GRAPHICS BACKEND",
                            div().child(self.engine_picker(
                                format!("gj-{id}"),
                                super::sharp_live::gamejolt_engine(&game),
                                options,
                                move |this, value, cx| {
                                    this.set_gamejolt_engine(engine_id.clone(), value, cx)
                                },
                                cx,
                            )),
                        ));
                    }
                    let mut card = self.card_shell(
                        &format!("gj-{id}"),
                        card_width,
                        self.cover_path(&format!("sharp-gamejolt-{id}"), cx),
                        9.0 / 16.0,
                        body,
                    );
                    // The deck lists a Game Jolt game once it has an executable.
                    if !s(&game, "exe_path").is_empty() {
                        card = self.with_settings_gear(card, "gamejolt", id.clone(), false, cx);
                    }
                    grid = grid.child(card);
                }
                content.child(grid)
            }
        }
    }

    pub(super) fn live_confirm_overlay(
        &self,
        action: SharpConfirm,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let (head, desc, confirm) = Self::confirm_copy(&action);
        let muted = if self.palette.light { 0x64717a } else { MUTED };
        let dialog = div()
            .id("sharp-live-dialog")
            .occlude()
            .on_click(|_, _, cx| cx.stop_propagation())
            .w(px(440.0))
            .max_w(px(f32::from(viewport.width) - 48.0))
            .rounded(px(16.0))
            .border_1()
            .border_color(gpui::rgba(self.palette.control_border))
            .bg(rgb(self.palette.menu_bg))
            .p(px(22.0))
            .flex()
            .flex_col()
            .gap(px(14.0))
            .child(
                div()
                    .text_size(px(17.0))
                    .text_color(rgb(self.palette.control_text))
                    .font_weight(FontWeight::BOLD)
                    .child(head),
            )
            .children((!desc.is_empty()).then(|| {
                div()
                    .text_size(px(12.0))
                    .line_height(px(18.0))
                    .text_color(rgb(muted))
                    .child(desc)
            }))
            .child(
                div()
                    .flex()
                    .justify_end()
                    .gap(px(8.0))
                    .child(
                        self.palette
                            .button("sharp-live-cancel", "Cancel", false)
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.live.as_mut().unwrap().confirm = None;
                                cx.notify();
                            })),
                    )
                    .child(
                        self.palette
                            .button("sharp-live-confirm", confirm, true)
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.run_confirm(action.clone(), cx);
                            })),
                    ),
            );
        div()
            .id("sharp-live-modal")
            .occlude()
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .bg(gpui::rgba(crate::page_palette::tca(0x000000aa)))
            .on_click(cx.listener(|this, _, _, cx| {
                this.live.as_mut().unwrap().confirm = None;
                cx.notify();
            }))
            .child(dialog)
    }
}
