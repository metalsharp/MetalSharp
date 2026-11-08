//! FEX | x86_64: the Wine runtime toggle at the top of every library page. A MetalSharp.app of either
//! build runs either runtime: MetalSharp Wine (Arm64, x86 code through FEX) or the x86_64 Wine under
//! Rosetta. Switching to one that isn't set up asks first, downloads it when this MetalSharp.app does
//! not carry it, and runs its setup; prefixes (Steam's, launchers', games') are shared, so a Steam set up
//! under one starts and plays under the other. The backend restarts on the runtime switched to.
use super::*;
use crate::live::{self, Live, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum RuntimeStage {
    Confirm,
    Downloading,
    Switching,
}

#[derive(Clone)]
pub(super) struct RuntimeSwitch {
    pub target: &'static str,
    pub installed: bool,
    pub source: bool,
    pub stage: RuntimeStage,
    pub received: u64,
    pub total: u64,
    pub error: Option<String>,
}

pub(super) fn runtime_label(variant: &str) -> &'static str {
    if variant == "fex" { "FEX" } else { "x86_64" }
}

fn runtime_long_name(variant: &str) -> &'static str {
    if variant == "fex" {
        "MetalSharp Wine (Arm64, FEX)"
    } else {
        "x86_64 Wine (Rosetta)"
    }
}

impl MetalSharpApp {
    /// A click on the toggle: the runtimes' state, then the prompt.
    pub(super) fn request_runtime_switch(&mut self, target: &'static str, cx: &mut Context<Self>) {
        if target == crate::variant::name() || self.runtime_switch.is_some() {
            return;
        }
        live::call(
            cx,
            "GET",
            "/runtime/state",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, state, cx| {
                let Some(state) = state.filter(|s| is_ok(s)) else {
                    toast::error(cx, "Could not read the Wine runtimes");
                    return;
                };
                let entry = state.get(target);
                let flag = |key: &str| {
                    entry
                        .and_then(|e| e.get(key))
                        .and_then(Value::as_bool)
                        .unwrap_or(false)
                };
                this.runtime_switch = Some(RuntimeSwitch {
                    target,
                    installed: flag("installed"),
                    source: flag("source"),
                    stage: RuntimeStage::Confirm,
                    received: 0,
                    total: 0,
                    error: None,
                });
                cx.notify();
            },
        );
    }

    /// After Settings uninstalled the active runtime: the backend switched to the other one, so it starts again
    /// on that one, as after a switch from the toggle.
    pub(super) fn restart_backend_after_runtime_change(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        cx.spawn(async move |this, cx| {
            let started = cx
                .background_executor()
                .spawn(async move { live.start_backend() })
                .await;
            let _ = this.update(cx, |this, cx| {
                match started {
                    Ok(()) => this.after_backend_started(cx),
                    Err(error) => toast::error(cx, error),
                }
                cx.notify();
            });
        })
        .detach();
    }

    fn confirm_runtime_switch(&mut self, cx: &mut Context<Self>) {
        let Some(switch) = self.runtime_switch.as_mut() else {
            return;
        };
        switch.error = None;
        if switch.installed || switch.source {
            self.perform_runtime_switch(cx);
            return;
        }
        switch.stage = RuntimeStage::Downloading;
        let target = switch.target;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/runtime/download",
            Some(json!({ "variant": target })),
            live::DEFAULT_TIMEOUT,
            move |this, started, cx| {
                if !started.as_ref().is_some_and(is_ok) {
                    this.runtime_switch_failed(
                        error_text(started.as_ref())
                            .unwrap_or_else(|| "Could not start the download".into()),
                        cx,
                    );
                    return;
                }
                this.poll_runtime_download(cx);
            },
        );
    }

    fn poll_runtime_download(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/runtime/download/progress",
            None,
            live::DEFAULT_TIMEOUT,
            |this, progress, cx| {
                let Some(progress) = progress else {
                    this.runtime_switch_failed("The download stopped".into(), cx);
                    return;
                };
                let number = |key: &str| progress.get(key).and_then(Value::as_u64).unwrap_or(0);
                let flag = |key: &str| progress.get(key).and_then(Value::as_bool).unwrap_or(false);
                if let Some(switch) = this.runtime_switch.as_mut() {
                    switch.received = number("received");
                    switch.total = number("total");
                }
                cx.notify();
                if flag("done") {
                    if flag("ok") {
                        this.perform_runtime_switch(cx);
                    } else {
                        let error = progress
                            .get("error")
                            .and_then(Value::as_str)
                            .unwrap_or("The download failed");
                        this.runtime_switch_failed(error.to_owned(), cx);
                    }
                    return;
                }
                cx.spawn(async move |this, cx| {
                    cx.background_executor()
                        .timer(std::time::Duration::from_millis(500))
                        .await;
                    let _ = this.update(cx, |this, cx| {
                        if this.runtime_switch.is_some() {
                            this.poll_runtime_download(cx);
                        }
                    });
                })
                .detach();
            },
        );
    }

    /// The switch itself, then the backend's restart on the new runtime, then its setup when it isn't
    /// set up yet.
    fn perform_runtime_switch(&mut self, cx: &mut Context<Self>) {
        let Some(switch) = self.runtime_switch.as_mut() else {
            return;
        };
        switch.stage = RuntimeStage::Switching;
        let target = switch.target;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/runtime/switch",
            Some(json!({ "variant": target })),
            std::time::Duration::from_secs(120),
            move |this, reply, cx| {
                if !reply.as_ref().is_some_and(is_ok) {
                    this.runtime_switch_failed(
                        error_text(reply.as_ref())
                            .unwrap_or_else(|| "Could not switch the Wine runtime".into()),
                        cx,
                    );
                    return;
                }
                let installed = reply
                    .as_ref()
                    .and_then(|r| r.get("installed"))
                    .and_then(Value::as_bool)
                    .unwrap_or(false);
                let Some(live) = Live::get(cx) else { return };
                cx.spawn(async move |this, cx| {
                    let started = cx
                        .background_executor()
                        .spawn(async move { live.start_backend() })
                        .await;
                    let _ = this.update(cx, |this, cx| {
                        this.runtime_switch = None;
                        match started {
                            Ok(()) if installed => {
                                toast::success(cx, format!("Now on {}", runtime_long_name(target)));
                                this.after_backend_started(cx);
                                this.repair_vcrun(cx);
                            }
                            Ok(()) => {
                                // the runtime's setup, which installs it (and converts the Steam prefix where it must)
                                this.check_backend(cx);
                                this.open_setup(true, cx);
                                this.step = 1;
                                this.setup_start_install(cx);
                            }
                            Err(error) => toast::error(cx, error),
                        }
                        cx.notify();
                    });
                })
                .detach();
            },
        );
    }

    /// The shared VC++ repair after a switch: each prefix gets the VC++ 2015-2022 runtime the new runtime needs
    /// (Microsoft's ARM64 one under FEX, downloaded on the first switch; the x64 one under x86_64). Quick when the
    /// prefixes already have it.
    fn repair_vcrun(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/runtime/vcrun/repair",
            None,
            std::time::Duration::from_secs(1800),
            move |_this, reply, cx| {
                if !reply.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(reply.as_ref())
                            .unwrap_or_else(|| "Could not set up the VC++ 2022 runtime".into()),
                    );
                }
            },
        );
    }

    fn runtime_switch_failed(&mut self, error: String, cx: &mut Context<Self>) {
        if let Some(switch) = self.runtime_switch.as_mut() {
            switch.error = Some(error);
            switch.stage = RuntimeStage::Confirm;
        }
        cx.notify();
    }

    /// FEX | x86_64, right of the search field in the header (where Library | Collection was).
    pub(super) fn render_runtime_toggle(
        &self,
        layout: LibraryLayout,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let border = rgba(0xe7eaec4d);
        let fill = rgba(0x0c0f10b8);
        let active = crate::variant::name();
        let segment = |id: &'static str, variant: &'static str| {
            let on = active == variant;
            div()
                .id(id)
                .h_full()
                .flex()
                .items_center()
                .px(px(layout.scaled(14.0)))
                .rounded(px(layout.scaled(6.0)))
                .text_size(px(layout.scaled(12.5)))
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(rgb(if on { 0xffffff } else { 0xaab4c4 }))
                .when(on, |d| d.bg(rgba(0xffffff26)))
                .cursor_pointer()
                .on_click(
                    cx.listener(move |this, _, _, cx| this.request_runtime_switch(variant, cx)),
                )
                .child(runtime_label(variant))
        };
        div()
            .flex_none()
            .h(px(layout.control_height))
            .flex()
            .items_center()
            .gap(px(2.0))
            .p(px(3.0))
            .rounded(px(layout.scaled(8.0)))
            .border_1()
            .border_color(border)
            .bg(fill)
            .child(segment("runtime-toggle-fex", "fex"))
            .child(segment("runtime-toggle-x86_64", "regular"))
    }

    pub(super) fn render_runtime_prompt(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> Option<gpui::Stateful<gpui::Div>> {
        let switch = self.runtime_switch.clone()?;
        let layout = LibraryLayout::for_viewport(viewport.width.into(), viewport.height.into());
        let theme = self.theme;
        let text_color = theme.control_text();
        let name = runtime_long_name(switch.target);
        let (title, body, confirm) = if switch.installed {
            (
                format!("Switch to {name}?"),
                "Steam and any running games close first. Your Steam login, games, saves and prefixes carry over; the first start of each prefix after a switch takes a few seconds longer while Wine updates it.".to_owned(),
                "Switch",
            )
        } else if switch.source {
            (
                format!("Set up {name}?"),
                format!(
                    "{name} isn't set up yet. MetalSharp sets it up next to the one you have, so you can switch between them here. Your Steam login, games and prefixes carry over."
                ),
                "Set up",
            )
        } else {
            (
                format!("Download and set up {name}?"),
                format!(
                    "{name} isn't installed. MetalSharp downloads it (about 1.5 GB) and sets it up next to the one you have, so you can switch between them here. Your Steam login, games and prefixes carry over."
                ),
                "Download and set up",
            )
        };
        let status = match switch.stage {
            RuntimeStage::Downloading if switch.total > 0 => Some(format!(
                "Downloading… {:.1} of {:.1} GB",
                switch.received as f64 / 1e9,
                switch.total as f64 / 1e9
            )),
            RuntimeStage::Downloading => Some(format!(
                "Downloading… {:.1} GB",
                switch.received as f64 / 1e9
            )),
            RuntimeStage::Switching => Some("Switching…".to_owned()),
            RuntimeStage::Confirm => None,
        };
        let busy = switch.stage != RuntimeStage::Confirm;
        let button = |id: &'static str, label: &'static str, primary: bool| {
            div()
                .id(id)
                .px(px(layout.scaled(14.0)))
                .py(px(layout.scaled(7.0)))
                .rounded(px(layout.scaled(7.0)))
                .text_size(px(layout.scaled(13.0)))
                .when(primary, |b| b.bg(rgb(0x2f6fdf)).text_color(rgb(0xffffff)))
                .when(!primary, |b| b.text_color(rgb(text_color)))
                .cursor_pointer()
                .hover(move |style| {
                    if primary {
                        style.bg(rgb(0x3d7ef0))
                    } else {
                        style.bg(rgb(theme.menu_hover()))
                    }
                })
                .child(label)
        };
        let mut actions = div().flex().justify_end().gap(px(layout.scaled(8.0)));
        if !busy {
            actions = actions
                .child(
                    button("runtime-switch-cancel", "Cancel", false).on_click(cx.listener(
                        |this, _, _, cx| {
                            this.runtime_switch = None;
                            cx.notify();
                        },
                    )),
                )
                .child(
                    button("runtime-switch-confirm", confirm, true)
                        .on_click(cx.listener(|this, _, _, cx| this.confirm_runtime_switch(cx))),
                );
        }
        let panel = div()
            .id("runtime-switch-prompt")
            .occlude()
            .w(px(layout.scaled(460.0)))
            .flex()
            .flex_col()
            .gap(px(layout.scaled(14.0)))
            .p(px(layout.scaled(22.0)))
            .rounded(px(layout.scaled(12.0)))
            .border_1()
            .border_color(rgba(theme.page_palette().control_border))
            .bg(rgb(theme.menu_bg()))
            .shadow_lg()
            .child(
                div()
                    .text_size(px(layout.scaled(18.0)))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(text_color))
                    .child(title),
            )
            .child(
                div()
                    .text_size(px(layout.scaled(12.5)))
                    .line_height(px(layout.scaled(18.0)))
                    .text_color(rgb(0xaeb3b2))
                    .child(body),
            )
            .children(status.map(|status| {
                div()
                    .text_size(px(layout.scaled(12.5)))
                    .text_color(rgb(text_color))
                    .child(status)
            }))
            .children(switch.error.map(|error| {
                div()
                    .text_size(px(layout.scaled(12.5)))
                    .text_color(rgb(0xff7a6e))
                    .child(error)
            }))
            .child(actions);
        Some(
            div()
                .id("runtime-switch-scrim")
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .occlude()
                .child(
                    div()
                        .id("runtime-switch-backdrop")
                        .absolute()
                        .top(px(0.0))
                        .left(px(0.0))
                        .size_full()
                        .bg(rgba(0x000000a0))
                        .on_click(cx.listener(|this, _, _, cx| {
                            if this
                                .runtime_switch
                                .as_ref()
                                .is_some_and(|s| s.stage == RuntimeStage::Confirm)
                            {
                                this.runtime_switch = None;
                                cx.notify();
                            }
                        })),
                )
                .child(panel),
        )
    }
}
