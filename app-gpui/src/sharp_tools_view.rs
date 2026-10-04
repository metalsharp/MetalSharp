//! Rendering for the Sharp installer tooling (`SharpView.vue` installer card:
//! inline rename, positioned cover, cover sliders, Launch Doctor, diagnostics).
use super::*;
use serde_json::Value;

fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn b(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}

fn cover_axis(app: &Value, key: &str) -> f32 {
    app.get(key)
        .and_then(Value::as_f64)
        .unwrap_or(50.0)
        .clamp(0.0, 100.0) as f32
}

const ERROR: u32 = 0xff8585;

impl SharpPreview {
    fn tool_button(
        &self,
        id: String,
        label: impl Into<gpui::SharedString>,
    ) -> gpui::Stateful<gpui::Div> {
        self.palette
            .button(gpui::SharedString::from(id), label, false)
            .h(px(28.0))
            .px(px(10.0))
            .rounded(px(6.0))
            .text_size(px(11.0))
    }

    /// Card title with the pencil rename affordance.
    pub(super) fn name_title(
        &self,
        gamejolt: bool,
        id: &str,
        name: &str,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let live = self.live.as_ref().unwrap();
        let prefix = if gamejolt { "gj" } else { "app" };
        if let Some(edit) = live
            .name_edit
            .as_ref()
            .filter(|edit| edit.gamejolt == gamejolt && edit.id == id)
        {
            let saving = edit.saving;
            let key_id = id.to_owned();
            let mut row = div().flex().items_center().gap(px(6.0)).child(
                div()
                    .id(gpui::SharedString::from(format!(
                        "{prefix}-name-input-{id}"
                    )))
                    .flex_1()
                    .min_w_0()
                    .h(px(28.0))
                    .px(px(8.0))
                    .rounded(px(6.0))
                    .border_1()
                    .border_color(rgb(self.palette.accent))
                    .bg(rgb(0x111416))
                    .flex()
                    .items_center()
                    .text_size(px(13.0))
                    .font_weight(FontWeight::NORMAL)
                    .child(edit.input.clone())
                    // Keep focus in the input (the page root refocuses itself).
                    .on_mouse_down(gpui::MouseButton::Left, |_, _, cx| cx.stop_propagation())
                    .on_key_down(cx.listener(move |this, event: &gpui::KeyDownEvent, _, cx| {
                        match event.keystroke.key.as_str() {
                            "enter" if gamejolt => this.save_gamejolt_name(key_id.clone(), cx),
                            "enter" => this.save_app_name(key_id.clone(), cx),
                            "escape" => this.cancel_name_edit(cx),
                            _ => return,
                        }
                        cx.stop_propagation();
                    })),
            );
            if !gamejolt {
                let save_id = id.to_owned();
                row = row
                    .child(
                        self.tool_button(format!("app-name-save-{id}"), "✓")
                            .opacity(if saving { 0.45 } else { 1.0 })
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.save_app_name(save_id.clone(), cx)
                            })),
                    )
                    .child(
                        self.tool_button(format!("app-name-cancel-{id}"), "✕")
                            .opacity(if saving { 0.45 } else { 1.0 })
                            .on_click(cx.listener(|this, _, _, cx| this.cancel_name_edit(cx))),
                    );
            }
            return row.into_any_element();
        }
        let edit_id = id.to_owned();
        let edit_name = name.to_owned();
        div()
            .flex()
            .items_center()
            .gap(px(6.0))
            .child(div().min_w_0().child(name.to_owned()))
            .child(
                div()
                    .id(gpui::SharedString::from(format!("{prefix}-name-edit-{id}")))
                    .flex_none()
                    .px(px(4.0))
                    .rounded(px(4.0))
                    .text_size(px(12.0))
                    .text_color(rgb(MUTED))
                    .cursor_pointer()
                    .hover(|style| style.text_color(rgb(TEXT)))
                    .child("✎")
                    .on_mouse_down(gpui::MouseButton::Left, |_, _, cx| cx.stop_propagation())
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.begin_name_edit(
                            gamejolt,
                            edit_id.clone(),
                            edit_name.clone(),
                            window,
                            cx,
                        )
                    })),
            )
            .into_any_element()
    }

    /// Installer banner: `object-fit: cover` with `object-position` and the
    /// horizontal-pan zoom from `sharpCoverImageStyle`, plus the running close button.
    pub(super) fn app_banner(
        &self,
        app: &Value,
        width: f32,
        height: f32,
        running: bool,
        cx: &mut Context<Self>,
    ) -> gpui::AnyElement {
        let id = s(app, "id").to_owned();
        let mut banner = div()
            .relative()
            .w(px(width))
            .h(px(height))
            .overflow_hidden()
            .bg(rgb(0x15181a));
        let path = self.cover_path(&format!("sharp-app-{id}"), cx);
        match path {
            Some(path) => {
                let x = cover_axis(app, "cover_position_x");
                let y = cover_axis(app, "cover_position_y");
                match self
                    .cover_dimensions(&path)
                    .filter(|(w, h)| *w > 0 && *h > 0)
                {
                    Some((iw, ih)) => {
                        let (iw, ih) = (iw as f32, ih as f32);
                        let scale = (width / iw).max(height / ih);
                        let (mut dw, mut dh) = (iw * scale, ih * scale);
                        let mut left = (width - dw) * x / 100.0;
                        let mut top = (height - dh) * y / 100.0;
                        let pan = if iw / ih <= width / height {
                            1.0 + (x - 50.0).abs() / 250.0
                        } else {
                            1.0
                        };
                        if pan > 1.0 {
                            let (ox, oy) = (width * x / 100.0, height * y / 100.0);
                            left = ox + (left - ox) * pan;
                            top = oy + (top - oy) * pan;
                            dw *= pan;
                            dh *= pan;
                        }
                        banner = banner.child(
                            img(path)
                                .absolute()
                                .left(px(left))
                                .top(px(top))
                                .w(px(dw))
                                .h(px(dh))
                                .object_fit(ObjectFit::Fill),
                        );
                    }
                    None => {
                        banner = banner.child(img(path).size_full().object_fit(ObjectFit::Cover));
                    }
                }
            }
            None => {
                banner = banner.flex().items_center().justify_center().child(
                    img(self.asset_root.join("metalsharp-logo.png"))
                        .size(px(height * 0.55))
                        .object_fit(ObjectFit::Contain),
                );
            }
        }
        if running {
            let app = app.clone();
            banner = banner.child(
                div()
                    .id(gpui::SharedString::from(format!("app-close-{id}")))
                    .absolute()
                    .top(px(8.0))
                    .right(px(8.0))
                    .size(px(26.0))
                    .rounded_full()
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(gpui::rgba(0x000000a0))
                    .border_1()
                    .border_color(gpui::rgba(0xffffff40))
                    .text_size(px(12.0))
                    .text_color(rgb(0xffffff))
                    .cursor_pointer()
                    .child("✕")
                    .on_click(cx.listener(move |this, _, _, cx| this.stop_app(app.clone(), cx))),
            );
        }
        banner.into_any_element()
    }

    /// `cover-position-controls`: X / Y range inputs saved on release.
    pub(super) fn cover_sliders(&self, app: &Value, cx: &mut Context<Self>) -> gpui::Div {
        let id = s(app, "id").to_owned();
        let mut sliders = div().flex().flex_col().gap(px(6.0));
        for (horizontal, axis) in [(true, "x"), (false, "y")] {
            let value = cover_axis(
                app,
                if horizontal {
                    "cover_position_x"
                } else {
                    "cover_position_y"
                },
            );
            let key = format!("{id}:{axis}");
            let store = self.live.as_ref().unwrap().slider_bounds.clone();
            let drag_id = id.clone();
            let track = div()
                .id(gpui::SharedString::from(format!("cover-{key}")))
                .relative()
                .flex_1()
                .h(px(16.0))
                .cursor_pointer()
                .child(
                    gpui::canvas(
                        move |bounds, _, _| {
                            store.borrow_mut().insert(key, bounds);
                        },
                        |_, _, _, _| {},
                    )
                    .absolute()
                    .size_full(),
                )
                .child(
                    div()
                        .absolute()
                        .left_0()
                        .right_0()
                        .top(px(6.0))
                        .h(px(4.0))
                        .rounded_full()
                        .bg(rgb(0x2a2d30)),
                )
                .child(
                    div()
                        .absolute()
                        .left_0()
                        .top(px(6.0))
                        .h(px(4.0))
                        .rounded_full()
                        .w(gpui::relative(value / 100.0))
                        .bg(rgb(self.palette.accent)),
                )
                .child(
                    div()
                        .absolute()
                        .top(px(1.0))
                        .left(gpui::relative(value / 100.0))
                        .ml(px(-7.0))
                        .size(px(14.0))
                        .rounded_full()
                        .bg(rgb(0xf0f0ed)),
                )
                .on_mouse_down(
                    gpui::MouseButton::Left,
                    cx.listener(move |this, event: &gpui::MouseDownEvent, _, cx| {
                        this.live.as_mut().unwrap().cover_drag =
                            Some((drag_id.clone(), horizontal));
                        this.drag_cover_position(f32::from(event.position.x), cx);
                    }),
                );
            sliders = sliders.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .w(px(12.0))
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child(axis.to_uppercase()),
                    )
                    .child(track),
            );
        }
        sliders
    }

    /// `launch-failure`, Launch Doctor and diagnostics drawers for one app.
    pub(super) fn app_diagnostics(&self, app: &Value, cx: &mut Context<Self>) -> gpui::Div {
        let live = self.live.as_ref().unwrap();
        let id = s(app, "id").to_owned();
        let mut column = div().flex().flex_col().gap(px(8.0));
        if let Some(error) = live.launch_errors.get(&id).filter(|e| !e.is_empty()) {
            column = column.child(
                div()
                    .p(px(8.0))
                    .rounded(px(6.0))
                    .border_1()
                    .border_color(gpui::rgba(0xff5c5c55))
                    .bg(gpui::rgba(0xff5c5c14))
                    .flex()
                    .flex_col()
                    .gap(px(2.0))
                    .child(
                        div()
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child("Last launch failed"),
                    )
                    .child(
                        div()
                            .text_size(px(11.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(ERROR))
                            .child(error.clone()),
                    ),
            );
        }
        if live.doctor_open.contains(&id) {
            let report = live.doctor_reports.get(&id);
            let summary = report
                .map(|r| s(r, "summary").to_owned())
                .unwrap_or_else(|| "Checking launch prerequisites".into());
            let mut content = div().flex().flex_col().gap(px(6.0)).child(
                div()
                    .text_size(px(10.0))
                    .text_color(rgb(MUTED))
                    .child(summary),
            );
            if live.doctor_loading.contains(&id) {
                content = content.child(
                    div()
                        .text_size(px(11.0))
                        .text_color(rgb(MUTED))
                        .child("Checking launch prerequisites..."),
                );
            } else if let Some(report) = report {
                let ready = b(report, "ready");
                content = content.child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(
                            div()
                                .px(px(8.))
                                .py(px(3.))
                                .rounded(px(20.))
                                .bg(rgb(if ready { 0x1d3326 } else { 0x3a2a1a }))
                                .text_size(px(9.))
                                .font_weight(FontWeight::BOLD)
                                .text_color(rgb(if ready { 0x7fdc9a } else { 0xf0b46a }))
                                .child(if ready { "READY" } else { "BLOCKED" }),
                        )
                        .child(
                            div()
                                .min_w_0()
                                .text_size(px(11.0))
                                .text_color(rgb(TEXT))
                                .child(s(report, "summary").to_owned()),
                        ),
                );
                for check in report
                    .get("checks")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                {
                    let ok = b(check, "ok");
                    let mut detail = div()
                        .min_w_0()
                        .flex_1()
                        .flex()
                        .flex_col()
                        .gap(px(4.0))
                        .child(
                            div()
                                .text_size(px(10.0))
                                .text_color(rgb(MUTED))
                                .child(s(check, "detail").to_owned()),
                        );
                    if !ok || s(check, "id") == "launcher_exe" {
                        let label = super::sharp_tools::doctor_action_label(check, app);
                        let (app, check) = (app.clone(), check.clone());
                        detail = detail.child(
                            self.tool_button(format!("doctor-{id}-{}", s(&check, "id")), label)
                                .flex_none()
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.run_doctor_action(app.clone(), check.clone(), cx)
                                })),
                        );
                    }
                    content = content.child(
                        div()
                            .flex()
                            .items_start()
                            .gap(px(8.0))
                            .child(
                                div()
                                    .w(px(18.0))
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(if ok { 0x7fdc9a } else { ERROR }))
                                    .child(if ok { "OK" } else { "!" }),
                            )
                            .child(
                                div()
                                    .w(px(90.0))
                                    .flex_none()
                                    .text_size(px(10.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(rgb(TEXT))
                                    .child(s(check, "label").to_owned()),
                            )
                            .child(detail),
                    );
                }
                let args: Vec<&str> = report
                    .get("recipe")
                    .and_then(|r| r.get("launch_args"))
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(Value::as_str)
                    .collect();
                if !args.is_empty() {
                    content = content.child(
                        div()
                            .text_size(px(10.0))
                            .text_color(rgb(MUTED))
                            .child(format!("Args: {}", args.join(" "))),
                    );
                }
                for (key, color) in [("blockers", ERROR), ("warnings", MUTED)] {
                    for note in report
                        .get(key)
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .filter_map(Value::as_str)
                    {
                        content = content.child(
                            div()
                                .text_size(px(10.0))
                                .text_color(rgb(color))
                                .child(note.to_owned()),
                        );
                    }
                }
            }
            column = column.child(self.bottle_panel("LAUNCH DOCTOR", content));
        }
        if live.diagnostics_open.contains(&id) {
            let crashes = live.recent_crashes.get(&id).cloned().unwrap_or_default();
            let lines = live.recent_log_lines.get(&id).cloned();
            let name = s(app, "name").to_owned();
            let bundle_app = app.clone();
            let mut content = div()
                .flex()
                .flex_col()
                .gap(px(8.0))
                .child(
                    div()
                        .text_size(px(10.0))
                        .text_color(rgb(MUTED))
                        .child(format!(
                            "{} crash reports · {} log lines",
                            crashes.len(),
                            lines.as_ref().map_or(0, Vec::len)
                        )),
                )
                .child(
                    div()
                        .flex()
                        .flex_wrap()
                        .gap(px(6.0))
                        .child(
                            self.tool_button(
                                format!("diag-shader-{id}"),
                                "Clear All Shader Caches",
                            )
                            .on_click(cx.listener(
                                move |this, _, _, cx| this.clear_shader_cache(name.clone(), cx),
                            )),
                        )
                        .child(
                            self.tool_button(format!("diag-logs-{id}"), "Open Logs")
                                .on_click(cx.listener(|this, _, _, cx| this.open_log_folder(cx))),
                        )
                        .child(
                            self.tool_button(format!("diag-bundle-{id}"), "Copy Bundle")
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.copy_diagnostic_bundle(&bundle_app, cx)
                                })),
                        ),
                );
            if !crashes.is_empty() {
                let mut list = div().flex().flex_col().gap(px(4.0)).child(
                    div()
                        .text_size(px(10.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(rgb(TEXT))
                        .child("Recent crash reports"),
                );
                for report in &crashes {
                    list =
                        list.child(
                            div()
                                .flex()
                                .flex_col()
                                .child(
                                    div()
                                        .text_size(px(10.0))
                                        .text_color(rgb(TEXT))
                                        .child(s(report, "name").to_owned()),
                                )
                                .child(div().text_size(px(9.0)).text_color(rgb(MUTED)).child(
                                    format!("{} · {}", s(report, "timestamp"), s(report, "source")),
                                )),
                        );
                }
                content = content.child(list);
            }
            content = content.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(4.0))
                    .child(
                        div()
                            .text_size(px(10.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(TEXT))
                            .child("Recent launch log"),
                    )
                    .child(
                        div()
                            .id(gpui::SharedString::from(format!("diag-log-{id}")))
                            .max_h(px(160.0))
                            .overflow_y_scroll()
                            .p(px(6.0))
                            .rounded(px(4.0))
                            .bg(rgb(0x0c0e10))
                            .font_family("Menlo")
                            .text_size(px(9.0))
                            .text_color(rgb(0xc8ccc9))
                            .children(
                                lines
                                    .unwrap_or_else(|| vec!["No recent log lines loaded.".into()])
                                    .into_iter()
                                    .map(|line| div().child(line)),
                            ),
                    ),
            );
            column = column.child(self.bottle_panel("LOGS AND CRASH REPORTS", content));
        }
        column
    }
}
