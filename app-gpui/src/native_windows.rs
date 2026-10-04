//! Concrete GPUI auxiliary-window presentations and application-scoped shortcut bindings.
//! Registration is intentionally a host call, not an OS-global shortcut/permission request.
use crate::{
    desktop_host::{HostShortcut, LaunchOverlay},
    process_manager_connected::{ProcessManagerView, ProcessSnapshot},
};
use gpui::{
    App, AppContext, Bounds, Context, KeyBinding, Render, Window, WindowBounds, WindowHandle,
    WindowOptions, div, prelude::*, px, rgb, size,
};

gpui::actions!(
    metalsharp_native_windows,
    [ToggleProcessManager, ConfirmForceQuit]
);

/// Registers bindings in the GPUI application while it is active. These are app keyboard
/// accelerators, not OS-wide hotkeys and do not request Accessibility/Input Monitoring rights.
pub fn register_app_shortcuts(cx: &mut App) {
    cx.bind_keys([
        KeyBinding::new("cmd-p", ToggleProcessManager, None),
        KeyBinding::new("cmd-alt-q", ConfirmForceQuit, None),
    ]);
    let _ = HostShortcut::ToggleProcessManager.macos_accelerator();
}
fn options(title: &'static str, width: f32, height: f32, cx: &App) -> WindowOptions {
    let bounds = Bounds::centered(None, size(px(width), px(height)), cx);
    WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(bounds)),
        window_min_size: Some(size(px(width.min(420.)), px(height.min(260.)))),
        titlebar: Some(gpui::TitlebarOptions {
            title: Some(title.into()),
            ..Default::default()
        }),
        ..Default::default()
    }
}
pub fn open_process_manager(
    cx: &mut App,
    snapshot: ProcessSnapshot,
) -> anyhow::Result<WindowHandle<ProcessManagerView>> {
    let window_options = options("MetalSharp — Running Games", 520., 420., cx);
    cx.open_window(window_options, move |_, app| {
        app.new(|_| ProcessManagerView::new(snapshot))
    })
}
pub struct LaunchOverlayView {
    pub overlay: LaunchOverlay,
}
impl Render for LaunchOverlayView {
    fn render(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("native-launch-overlay")
            .size_full()
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap(px(8.))
            .p(px(20.))
            .bg(rgb(0xe91b1b1b))
            .text_color(rgb(0xf2efe6))
            .font_family("Rethink Sans")
            .child(
                div()
                    .text_size(px(12.))
                    .text_color(rgb(0xc2bda9))
                    .child("NOW PLAYING"),
            )
            .child(
                div()
                    .text_size(px(19.))
                    .child(self.overlay.game_name.clone()),
            )
            .child(
                div()
                    .text_size(px(12.))
                    .text_color(rgb(0xc2bda9))
                    .child("Press ⌘⌥Q to open the managed process controls"),
            )
    }
}
pub fn open_launch_overlay(
    cx: &mut App,
    overlay: LaunchOverlay,
) -> anyhow::Result<WindowHandle<LaunchOverlayView>> {
    let window_options = options("MetalSharp — Launching", 360., 150., cx);
    cx.open_window(window_options, move |_, app| {
        app.new(|_| LaunchOverlayView { overlay })
    })
}
pub fn close_window<V: Render + 'static>(
    handle: &WindowHandle<V>,
    cx: &mut App,
) -> anyhow::Result<()> {
    handle.update(cx, |_, window, _| window.remove_window())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shortcut_policy_is_app_scoped_and_matches_product_keys() {
        assert_eq!(
            HostShortcut::ToggleProcessManager.macos_accelerator(),
            "Command+P"
        );
        assert_eq!(
            HostShortcut::ConfirmedForceQuitOwnedGames.macos_accelerator(),
            "Command+Option+Q"
        );
    }
    #[test]
    fn overlay_requires_bounded_plain_text() {
        assert!(LaunchOverlay::new("EVE Online").is_ok());
        assert!(LaunchOverlay::new("\u{7f}").is_err());
    }
}
