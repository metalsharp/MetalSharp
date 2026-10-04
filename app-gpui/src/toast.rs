//! App-wide toasts, matching the Electron `useToast` composable: bottom-right
//! stack, 4 s default lifetime, success (green) / error (red) / info styles.
use gpui::{
    App, AppContext, Context, Entity, FontWeight, Global, IntoElement, Render, Window, div,
    prelude::*, px, rgb, rgba,
};
use std::time::Duration;

const DEFAULT_TOAST: Duration = Duration::from_millis(4000);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ToastKind {
    Success,
    Error,
    Info,
}

struct Toast {
    id: u64,
    text: String,
    kind: ToastKind,
    progress: Option<f32>,
}

pub struct ToastHub {
    toasts: Vec<Toast>,
    next: u64,
}

struct ToastGlobal(Entity<ToastHub>);
impl Global for ToastGlobal {}

pub fn install(cx: &mut App) -> Entity<ToastHub> {
    let hub = cx.new(|_| ToastHub {
        toasts: Vec::new(),
        next: 0,
    });
    cx.set_global(ToastGlobal(hub.clone()));
    hub
}

pub fn hub(cx: &App) -> Option<Entity<ToastHub>> {
    cx.try_global::<ToastGlobal>()
        .map(|global| global.0.clone())
}

pub fn show(cx: &mut App, text: impl Into<String>, kind: ToastKind) {
    show_for(cx, text, kind, DEFAULT_TOAST);
}

pub fn success(cx: &mut App, text: impl Into<String>) {
    show(cx, text, ToastKind::Success);
}

pub fn error(cx: &mut App, text: impl Into<String>) {
    show(cx, text, ToastKind::Error);
}

pub fn info(cx: &mut App, text: impl Into<String>) {
    show(cx, text, ToastKind::Info);
}

pub fn show_for(cx: &mut App, text: impl Into<String>, kind: ToastKind, duration: Duration) {
    let Some(hub) = hub(cx) else { return };
    let text = text.into();
    hub.update(cx, |hub, cx| {
        let id = hub.next;
        hub.next += 1;
        hub.toasts.push(Toast {
            id,
            text,
            kind,
            progress: None,
        });
        cx.notify();
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(duration).await;
            let _ = this.update(cx, |hub, cx| {
                hub.toasts.retain(|toast| toast.id != id);
                cx.notify();
            });
        })
        .detach();
    });
}

/// `useToast().showDownload`: persistent toast with a progress bar.
pub fn show_download(cx: &mut App, text: impl Into<String>, progress: f32) -> Option<u64> {
    let hub = hub(cx)?;
    let text = text.into();
    Some(hub.update(cx, |hub, cx| {
        let id = hub.next;
        hub.next += 1;
        hub.toasts.push(Toast {
            id,
            text,
            kind: ToastKind::Info,
            progress: Some(progress.clamp(0.0, 1.0)),
        });
        cx.notify();
        id
    }))
}

pub fn update_download(cx: &mut App, id: u64, text: impl Into<String>, progress: f32) {
    let Some(hub) = hub(cx) else { return };
    let text = text.into();
    hub.update(cx, |hub, cx| {
        if let Some(toast) = hub.toasts.iter_mut().find(|t| t.id == id) {
            toast.text = text;
            toast.progress = Some(progress.clamp(0.0, 1.0));
            cx.notify();
        }
    });
}

pub fn finish_download(cx: &mut App, id: u64, text: impl Into<String>, success: bool) {
    let Some(hub) = hub(cx) else { return };
    let text = text.into();
    hub.update(cx, |hub, cx| {
        if let Some(toast) = hub.toasts.iter_mut().find(|t| t.id == id) {
            toast.text = text;
            toast.kind = if success {
                ToastKind::Success
            } else {
                ToastKind::Error
            };
            if success {
                toast.progress = Some(1.0);
            }
        }
        cx.notify();
        let lifetime = Duration::from_millis(if success { 1800 } else { 6000 });
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(lifetime).await;
            let _ = this.update(cx, |hub, cx| {
                hub.toasts.retain(|toast| toast.id != id);
                cx.notify();
            });
        })
        .detach();
    });
}

impl Render for ToastHub {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        let mut stack = div()
            .absolute()
            .bottom(px(20.0))
            .right(px(20.0))
            .flex()
            .flex_col()
            .items_end()
            .gap(px(8.0));
        for toast in &self.toasts {
            let (bg, fg) = match toast.kind {
                ToastKind::Success => (0x6bbf7a, 0xffffff),
                ToastKind::Error => (0xd66a6a, 0xffffff),
                ToastKind::Info => (0x1d2124, 0xf2efe6),
            };
            stack = stack.child(
                div()
                    .id(("toast", toast.id))
                    .max_w(px(360.0))
                    .px(px(18.0))
                    .py(px(10.0))
                    .rounded(px(6.0))
                    .bg(rgb(bg))
                    .when(toast.kind == ToastKind::Info, |toast| {
                        toast.border_1().border_color(rgba(0xffffff24))
                    })
                    .shadow_lg()
                    .text_size(px(13.0))
                    .line_height(px(17.5))
                    .font_weight(FontWeight::MEDIUM)
                    .text_color(rgb(fg))
                    .when(toast.progress.is_some(), |toast| toast.min_w(px(280.0)))
                    .child(toast.text.clone())
                    .children(toast.progress.map(|progress| {
                        div()
                            .w_full()
                            .h(px(4.0))
                            .mt(px(8.0))
                            .rounded_full()
                            .bg(rgba(0xffffff2e))
                            .child(
                                div()
                                    .h(px(4.0))
                                    .rounded_full()
                                    .bg(rgb(0xe8d6b7))
                                    .w(gpui::relative(progress)),
                            )
                    })),
            );
        }
        div().absolute().top_0().left_0().size_full().child(stack)
    }
}
