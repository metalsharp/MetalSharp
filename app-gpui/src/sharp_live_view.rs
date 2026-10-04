//! Rendering for the live Sharp sources, reusing the approved card styling.
use super::sharp_live::{ENGINE_OPTIONS, SharpConfirm};
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

fn live_pill(text: String) -> gpui::Div {
    div()
        .px(px(8.))
        .py(px(4.))
        .rounded(px(20.))
        .bg(rgb(0x28251f))
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
                let initialized = b(&status, "prefixInitialized");
                let busy =
                    live.gog_loading.contains("setup") || live.gog_loading.contains("removing");
                let label = if live.gog_loading.contains("removing") {
                    "Uninstall".to_owned()
                } else if live.gog_loading.contains("setup") {
                    "Installing…".to_owned()
                } else if initialized {
                    "Uninstall".to_owned()
                } else {
                    "Install GOG".to_owned()
                };
                controls = controls.child(
                    button(self, "sharp-gog-prefix", label, false, !busy).on_click(cx.listener(
                        move |this, _, _, cx| {
                            let live = this.live.as_ref().unwrap();
                            if live.gog_loading.contains("setup")
                                || live.gog_loading.contains("removing")
                            {
                                return;
                            }
                            if initialized {
                                this.live.as_mut().unwrap().confirm =
                                    Some(SharpConfirm::RemoveGogPrefix);
                                cx.notify();
                            } else {
                                this.initialize_gog_prefix(cx);
                            }
                        },
                    )),
                );
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
                controls = controls
                    .child(
                        button(
                            self,
                            "sharp-gj-storage",
                            "Open GameJolt".into(),
                            false,
                            true,
                        )
                        .on_click(cx.listener(|this, _, _, cx| this.choose_gamejolt_storage(cx))),
                    )
                    .child(
                        button(
                            self,
                            "sharp-gj-browser",
                            "Browse GameJolt".into(),
                            false,
                            true,
                        )
                        .on_click(cx.listener(|this, _, _, cx| this.open_gamejolt_browser(cx))),
                    );
            }
            _ => {}
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
                    "↻  Sync GameJolt"
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
        let label = options
            .iter()
            .find(|(id, _)| *id == current)
            .map(|(_, label)| label.clone())
            .unwrap_or_else(|| {
                if current.is_empty() || current == "auto" {
                    "Auto".into()
                } else {
                    current.clone()
                }
            });
        let toggle_key = key.clone();
        let mut field = div().relative().w_full().child(
            self.palette
                .button(
                    gpui::SharedString::from(format!("engine-{key}")),
                    format!("{label}  ⌄"),
                    false,
                )
                .w_full()
                .justify_start()
                .h(px(32.0))
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

    fn bottle_panel(&self, title: &'static str, content: gpui::Div) -> gpui::Div {
        div()
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
                    .child(title),
            )
            .child(content)
    }

    fn card_shell(
        &self,
        key: &str,
        width: f32,
        art: Option<std::path::PathBuf>,
        ratio: f32,
        body: gpui::Div,
    ) -> gpui::Stateful<gpui::Div> {
        let banner_height = width * ratio;
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
                gpui::linear_color_stop(rgb(0x202427), 0.0),
                gpui::linear_color_stop(rgb(0x171a1c), 1.0),
            ))
            .child(match art {
                Some(art) => img(art)
                    .w_full()
                    .h(px(banner_height))
                    .object_fit(ObjectFit::Cover)
                    .into_any_element(),
                None => div()
                    .w_full()
                    .h(px(banner_height))
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(rgb(0x15181a))
                    .child(
                        img(self.asset_root.join("metalsharp-logo.png"))
                            .size(px(banner_height * 0.55))
                            .object_fit(ObjectFit::Contain),
                    )
                    .into_any_element(),
            })
            .child(body)
    }

    fn card_body(
        &self,
        title: String,
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
                    .text_color(rgb(0xf0f0ed))
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
                            .bg(rgb(0x2a2d30))
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
                .text_color(rgb(0xff8585))
                .border_color(gpui::rgba(0xff5c5c55))
        } else {
            button
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
                    let engine = match s(&app, "engine") {
                        "" => "auto".to_owned(),
                        other => other.to_owned(),
                    };
                    let size = app
                        .get("size_bytes")
                        .and_then(Value::as_u64)
                        .filter(|b| *b > 0);
                    let meta = format!(
                        "Windows{}",
                        size.map(|b| format!(" · {}", format_bytes(b)))
                            .unwrap_or_default()
                    );
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
                    let mut body = self.card_body(
                        s(&app, "name").to_owned(),
                        if running {
                            "Running".into()
                        } else {
                            "Ready".into()
                        },
                        meta,
                        None,
                    );
                    body = body.child(actions);
                    if open {
                        let mut options = vec![("auto".to_owned(), "Auto".to_owned())];
                        options.extend(
                            ENGINE_OPTIONS
                                .iter()
                                .map(|(i, l)| ((*i).to_owned(), (*l).to_owned())),
                        );
                        let engine_id = id.clone();
                        let cover_id = id.clone();
                        let uninstall = (id.clone(), s(&app, "name").to_owned());
                        let content = div()
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
                                self.action(
                                    format!("app-cover-{id}"),
                                    "Change Image",
                                    false,
                                    false,
                                    true,
                                )
                                .w_full()
                                .on_click(cx.listener(
                                    move |this, _, _, cx| this.set_app_cover(cover_id.clone(), cx),
                                )),
                            )
                            .child(
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
                    grid = grid.child(self.card_shell(
                        &format!("app-{id}"),
                        card_width,
                        self.cover_path(&format!("sharp-app-{id}"), cx),
                        9.0 / 16.0,
                        body,
                    ));
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
                    let mut actions = div().flex().items_center().gap(px(8.0));
                    if installed {
                        let toggle = id.clone();
                        actions = actions.child(
                            div()
                                .id(gpui::SharedString::from(format!("gog-bottle-{id}")))
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
                                    let live = this.live.as_mut().unwrap();
                                    if !live.open_bottle.remove(&toggle) {
                                        live.open_bottle.insert(toggle.clone());
                                    }
                                    cx.notify();
                                })),
                        );
                    }
                    let g = game.clone();
                    if running {
                        let busy = live.gog_loading.contains(&format!("{id}:stop"));
                        actions = actions.child(
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
                                })),
                        );
                    } else if installed {
                        let busy = live.gog_loading.contains(&format!("{id}:play"));
                        actions = actions.child(
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
                                })),
                        );
                    } else {
                        let busy =
                            downloading || live.gog_loading.contains(&format!("{id}:install"));
                        actions = actions.child(
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
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    if !busy {
                                        this.install_gog_game(g.clone(), cx);
                                    }
                                },
                            )),
                        );
                    }
                    if installed {
                        let busy = live.gog_loading.contains(&format!("{id}:uninstall"));
                        let confirm = (id.clone(), s(&game, "title").to_owned());
                        actions = actions.child(
                            self.action(
                                format!("gog-uninstall-{id}"),
                                "Uninstall",
                                false,
                                true,
                                !busy,
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    if !busy {
                                        this.live.as_mut().unwrap().confirm =
                                            Some(SharpConfirm::UninstallGog(
                                                confirm.0.clone(),
                                                confirm.1.clone(),
                                            ));
                                        cx.notify();
                                    }
                                },
                            )),
                        );
                    }
                    body = body.child(actions);
                    if installed && live.open_bottle.contains(&id) {
                        let mut options = vec![("auto".to_owned(), "Auto".to_owned())];
                        options.extend(
                            ENGINE_OPTIONS
                                .iter()
                                .map(|(i, l)| ((*i).to_owned(), (*l).to_owned())),
                        );
                        let engine = live
                            .gog_engines
                            .get(&id)
                            .cloned()
                            .unwrap_or_else(|| "auto".into());
                        let engine_id = id.clone();
                        let exe_game = game.clone();
                        let mut content =
                            div()
                                .flex()
                                .flex_col()
                                .gap(px(8.0))
                                .child(self.engine_picker(
                                    format!("gog-{id}"),
                                    engine,
                                    options,
                                    move |this, value, cx| {
                                        this.set_gog_engine(engine_id.clone(), value, cx)
                                    },
                                    cx,
                                ));
                        if s(&game, "platform") == "windows" || s(&game, "platform").is_empty() {
                            content = content.child(
                                self.action(
                                    format!("gog-exe-{id}"),
                                    "Choose EXE",
                                    false,
                                    false,
                                    true,
                                )
                                .w_full()
                                .on_click(cx.listener(
                                    move |this, _, _, cx| {
                                        this.choose_provider_executable(false, exe_game.clone(), cx)
                                    },
                                )),
                            );
                        }
                        body = body.child(self.bottle_panel("GRAPHICS BACKEND", content));
                    }
                    grid = grid.child(self.card_shell(
                        &format!("gog-{id}"),
                        card_width,
                        self.cover_path(&format!("sharp-gog-{id}"), cx),
                        9.0 / 16.0,
                        body,
                    ));
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
                    let mut actions = div().flex().items_center().gap(px(8.0));
                    if installed {
                        let toggle = app.clone();
                        actions = actions.child(
                            div()
                                .id(gpui::SharedString::from(format!("epic-bottle-{app}")))
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
                                    let live = this.live.as_mut().unwrap();
                                    if !live.open_bottle.remove(&toggle) {
                                        live.open_bottle.insert(toggle.clone());
                                    }
                                    cx.notify();
                                })),
                        );
                    }
                    let g = game.clone();
                    if running {
                        actions = actions.child(
                            self.action(format!("epic-stop-{app}"), "Stop", true, false, true)
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.stop_epic_game(g.clone(), cx)
                                })),
                        );
                    } else if installed && initialized {
                        let busy = live.epic_loading.contains(&format!("{app}:play"));
                        actions = actions.child(
                            self.action(format!("epic-play-{app}"), "Play", true, false, !busy)
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    if !busy {
                                        this.play_epic_game(g.clone(), cx);
                                    }
                                })),
                        );
                    } else if installed {
                        let busy = live.epic_loading.contains(&format!("{app}:initialize"));
                        actions = actions.child(
                            self.action(
                                format!("epic-init-{app}"),
                                "Initialize Bottle",
                                true,
                                false,
                                !busy,
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    if !busy {
                                        this.initialize_epic_bottle(g.clone(), cx);
                                    }
                                },
                            )),
                        );
                    } else if tracked.is_some() {
                        actions = actions.child(
                            self.action(
                                format!("epic-cancel-{app}"),
                                "Stop Download",
                                false,
                                false,
                                true,
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| this.cancel_epic_install(g.clone(), cx),
                            )),
                        );
                    } else {
                        let busy = live.epic_loading.contains(&format!("{app}:install"));
                        actions = actions.child(
                            self.action(
                                format!("epic-install-{app}"),
                                "Install",
                                true,
                                false,
                                !busy,
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    if !busy {
                                        this.install_epic_game(g.clone(), cx);
                                    }
                                },
                            )),
                        );
                    }
                    if installed {
                        let busy =
                            running || live.epic_loading.contains(&format!("{app}:uninstall"));
                        let confirm = (app.clone(), s(&game, "title").to_owned());
                        actions = actions.child(
                            self.action(
                                format!("epic-uninstall-{app}"),
                                "Uninstall",
                                false,
                                true,
                                !busy,
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| {
                                    if !busy {
                                        this.live.as_mut().unwrap().confirm =
                                            Some(SharpConfirm::UninstallEpic(
                                                confirm.0.clone(),
                                                confirm.1.clone(),
                                            ));
                                        cx.notify();
                                    }
                                },
                            )),
                        );
                    }
                    body = body.child(actions);
                    if installed && live.open_bottle.contains(&app) {
                        let mut options = vec![("auto".to_owned(), "Auto".to_owned())];
                        options.extend(
                            ENGINE_OPTIONS
                                .iter()
                                .map(|(i, l)| ((*i).to_owned(), (*l).to_owned())),
                        );
                        let pipeline_app = app.clone();
                        let mouse_app = app.clone();
                        let exe_game = game.clone();
                        let content = div()
                            .flex()
                            .flex_col()
                            .gap(px(8.0))
                            .child(self.engine_picker(
                                format!("epic-{app}"),
                                match s(&game, "pipeline") {
                                    "" => "auto".into(),
                                    other => other.into(),
                                },
                                options,
                                move |this, value, cx| {
                                    this.update_epic_bottle(
                                        pipeline_app.clone(),
                                        "pipeline",
                                        value,
                                        cx,
                                    )
                                },
                                cx,
                            ))
                            .child(self.engine_picker(
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
                                    this.update_epic_bottle(
                                        mouse_app.clone(),
                                        "mouseMode",
                                        value,
                                        cx,
                                    )
                                },
                                cx,
                            ))
                            .child(
                                self.action(
                                    format!("epic-exe-{app}"),
                                    "Choose EXE",
                                    false,
                                    false,
                                    true,
                                )
                                .w_full()
                                .on_click(cx.listener(
                                    move |this, _, _, cx| {
                                        this.choose_provider_executable(true, exe_game.clone(), cx)
                                    },
                                )),
                            );
                        body = body.child(self.bottle_panel("GRAPHICS BACKEND", content));
                    }
                    grid = grid.child(self.card_shell(
                        &format!("epic-{app}"),
                        card_width,
                        self.cover_path(&format!("sharp-epic-{app}"), cx),
                        9.0 / 16.0,
                        body,
                    ));
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
                        "No GameJolt games found",
                        "Place each game in its own folder inside the GameJolt directory, then sync.",
                    ));
                }
                for game in live.gj_games.clone() {
                    let id = s(&game, "id").to_owned();
                    let running = live.gj_running.contains_key(&id);
                    let native = b(&game, "native");
                    let mut body = self.card_body(
                        s(&game, "name").to_owned(),
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
                        let options: Vec<(String, String)> = game
                            .get("available_pipelines")
                            .and_then(Value::as_array)
                            .map(|list| {
                                list.iter()
                                    .map(|p| (s(p, "id").to_owned(), s(p, "name").to_owned()))
                                    .filter(|(id, _)| !id.is_empty())
                                    .collect()
                            })
                            .filter(|list: &Vec<(String, String)>| !list.is_empty())
                            .unwrap_or_else(|| {
                                ["d3dmetal", "vkd3d", "dxmt", "dxmt_32", "d3d9"]
                                    .iter()
                                    .filter_map(|id| ENGINE_OPTIONS.iter().find(|(i, _)| i == id))
                                    .map(|(i, l)| ((*i).to_owned(), (*l).to_owned()))
                                    .collect()
                            });
                        let engine_id = id.clone();
                        body = body.child(self.bottle_panel(
                            "GRAPHICS BACKEND",
                            div().child(self.engine_picker(
                                format!("gj-{id}"),
                                s(&game, "engine").to_owned(),
                                options,
                                move |this, value, cx| {
                                    this.set_gamejolt_engine(engine_id.clone(), value, cx)
                                },
                                cx,
                            )),
                        ));
                    }
                    grid = grid.child(self.card_shell(
                        &format!("gj-{id}"),
                        card_width,
                        self.cover_path(&format!("sharp-gamejolt-{id}"), cx),
                        5.6 / 16.0,
                        body,
                    ));
                }
                content.child(grid)
            }
            _ => div(),
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
            .bg(gpui::rgba(0x000000aa))
            .on_click(cx.listener(|this, _, _, cx| {
                this.live.as_mut().unwrap().confirm = None;
                cx.notify();
            }))
            .child(dialog)
    }
}
