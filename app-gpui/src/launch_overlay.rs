//! Electron `showLaunchOverlay` (main/index.ts): a click-through "Now Playing"
//! pill at the right edge of the primary display, shown 5 s after a launch and
//! removed after 9.1 s.
use gpui::{
    App, AppContext, Bounds, Context, FontWeight, Render, Window, WindowBackgroundAppearance,
    WindowBounds, WindowKind, WindowOptions, div, point, prelude::*, px, rgb, rgba, size,
};
use std::cell::RefCell;
use std::time::Duration;

thread_local! {
    static CURRENT: RefCell<Option<gpui::AnyWindowHandle>> = const { RefCell::new(None) };
}

pub struct LaunchOverlayPill {
    game: String,
}

impl Render for LaunchOverlayPill {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        let kbd = |label: &'static str| {
            div()
                .min_w(px(22.0))
                .px(px(7.0))
                .py(px(2.0))
                .rounded(px(6.0))
                .border_1()
                .border_color(rgba(0xefcf9d66))
                .bg(rgba(0xefcf9d1f))
                .text_color(rgb(0xefcf9d))
                .text_size(px(12.0))
                .font_weight(FontWeight::BOLD)
                .flex()
                .justify_center()
                .child(label)
        };
        div().size_full().pl(px(14.0)).py(px(10.0)).child(
            div()
                .size_full()
                .px(px(22.0))
                .py(px(16.0))
                .rounded(px(16.0))
                .border_1()
                .border_color(rgba(0xefcf9d47))
                .bg(rgba(0x0a0c0ed1))
                .flex()
                .flex_col()
                .child(
                    div()
                        .mb(px(7.0))
                        .text_size(px(10.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(rgba(0xaeb3b2e6))
                        .child("NOW PLAYING"),
                )
                .child(
                    div()
                        .mb(px(9.0))
                        .text_size(px(14.0))
                        .font_weight(FontWeight::BOLD)
                        .text_color(rgb(0xf1efe9))
                        .overflow_hidden()
                        .whitespace_nowrap()
                        .text_ellipsis()
                        .child(self.game.clone()),
                )
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(7.0))
                        .text_size(px(13.0))
                        .text_color(rgb(0xe8e2d4))
                        .child("Press")
                        .child(kbd("⌘"))
                        .child("+")
                        .child(kbd("⌥"))
                        .child("+")
                        .child(kbd("Q"))
                        .child("To Quit Playing Anytime"),
                ),
        )
    }
}

#[cfg(target_os = "macos")]
fn make_click_through(window: &Window) {
    use objc2::rc::Retained;
    use objc2_app_kit::{NSView, NSWindowCollectionBehavior};
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    let Ok(handle) = HasWindowHandle::window_handle(window) else {
        return;
    };
    let RawWindowHandle::AppKit(appkit) = handle.as_raw() else {
        return;
    };
    let view: Option<Retained<NSView>> =
        unsafe { Retained::retain(appkit.ns_view.as_ptr().cast()) };
    let Some(view) = view else { return };
    if let Some(ns_window) = view.window() {
        ns_window.setIgnoresMouseEvents(true);
        ns_window.setCollectionBehavior(
            NSWindowCollectionBehavior::CanJoinAllSpaces
                | NSWindowCollectionBehavior::FullScreenAuxiliary
                | NSWindowCollectionBehavior::Stationary,
        );
        ns_window.setHasShadow(false);
    }
}

pub fn show(cx: &mut App, game: &str) {
    close(cx);
    let (width, height) = (460.0, 108.0);
    let display = cx.primary_display();
    let Some(display) = display else { return };
    let area = display.bounds();
    let origin = point(
        area.origin.x + area.size.width - px(width + 28.0),
        area.origin.y + (area.size.height - px(height)) / 2.0,
    );
    let options = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds::new(
            origin,
            size(px(width), px(height)),
        ))),
        titlebar: None,
        focus: false,
        show: true,
        kind: WindowKind::PopUp,
        is_movable: false,
        is_resizable: false,
        is_minimizable: false,
        display_id: Some(display.id()),
        window_background: WindowBackgroundAppearance::Transparent,
        ..Default::default()
    };
    let game = game.to_owned();
    let Ok(handle) = cx.open_window(options, move |window, cx| {
        #[cfg(target_os = "macos")]
        make_click_through(window);
        cx.new(|_| LaunchOverlayPill { game })
    }) else {
        return;
    };
    let any: gpui::AnyWindowHandle = handle.into();
    CURRENT.with(|current| *current.borrow_mut() = Some(any));
    cx.spawn(async move |cx| {
        cx.background_executor()
            .timer(Duration::from_millis(9100))
            .await;
        let _ = cx.update(|cx| {
            let is_current = CURRENT.with(|current| *current.borrow() == Some(any));
            if is_current {
                close(cx);
            }
        });
    })
    .detach();
}

pub fn close(cx: &mut App) {
    if let Some(handle) = CURRENT.with(|current| current.borrow_mut().take()) {
        let _ = handle.update(cx, |_, window, _| window.remove_window());
    }
}

/// `showLaunchQuitHint`: show the overlay 5 s after a successful launch.
pub fn hint_later<V: 'static>(game: String, cx: &mut gpui::Context<V>) {
    cx.spawn(async move |_, cx| {
        cx.background_executor()
            .timer(Duration::from_millis(5000))
            .await;
        let _ = cx.update(|cx| show(cx, &game));
    })
    .detach();
}
