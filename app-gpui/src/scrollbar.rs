//! A thin vertical scrollbar for a container scrolled with a [`ScrollHandle`].
//!
//! GPUI scrolls `overflow_y_scroll` containers but draws no scrollbar. Put the scrolled container
//! (with `.track_scroll(handle)` and [`refresh_on_scroll`]) and [`vertical`] in a `relative()`
//! parent: the bar sits over the container's right edge, shows how far it is scrolled, and
//! scrolls it when the track is clicked or dragged. It is left out when nothing scrolls.

use gpui::{
    MouseButton, Pixels, ScrollHandle, ScrollWheelEvent, Stateful, Window, div, point, prelude::*,
    px, rgba,
};

/// Space kept free above and below the thumb, so it doesn't touch rounded corners.
const INSET: f32 = 4.0;
const MIN_THUMB: f32 = 24.0;
const TRACK_WIDTH: f32 = 12.0;
const THUMB_WIDTH: f32 = 6.0;

/// The thumb's top and height in the track, for a viewport of `viewport` showing `max` more
/// pixels below when scrolled by `scrolled`; None when nothing scrolls.
fn thumb_geometry(viewport: f32, max: f32, scrolled: f32) -> Option<(f32, f32)> {
    if viewport <= 2.0 * INSET || max <= 0.5 {
        return None;
    }
    let track = viewport - 2.0 * INSET;
    let thumb = (track * viewport / (viewport + max)).clamp(MIN_THUMB.min(track), track);
    let top = INSET + (track - thumb) * (scrolled.clamp(0.0, max) / max);
    Some((top, thumb))
}

/// Scrolls `handle` so that the point at `y` (window coordinates) of the track is shown.
fn seek(handle: &ScrollHandle, y: Pixels) {
    let bounds = handle.bounds();
    let height = f32::from(bounds.size.height) - 2.0 * INSET;
    let max = f32::from(handle.max_offset().height);
    if height <= 0.0 || max <= 0.0 {
        return;
    }
    let fraction = ((f32::from(y - bounds.origin.y) - INSET) / height).clamp(0.0, 1.0);
    handle.set_offset(point(handle.offset().x, px(-max * fraction)));
}

/// The bar for the container `handle` scrolls, in `rgba` color (0xRRGGBBAA).
pub fn vertical(
    id: impl Into<gpui::ElementId>,
    handle: &ScrollHandle,
    color: u32,
) -> Option<Stateful<gpui::Div>> {
    let viewport = f32::from(handle.bounds().size.height);
    let max = f32::from(handle.max_offset().height);
    let scrolled = -f32::from(handle.offset().y);
    let (top, thumb) = thumb_geometry(viewport, max, scrolled)?;
    let down = handle.clone();
    let drag = handle.clone();
    Some(
        div()
            .id(id)
            .absolute()
            .top(px(0.0))
            .right(px(0.0))
            .w(px(TRACK_WIDTH))
            .h_full()
            .cursor_default()
            .on_mouse_down(MouseButton::Left, move |event, window, cx| {
                seek(&down, event.position.y);
                window.refresh();
                cx.stop_propagation();
            })
            .on_mouse_move(move |event, window, _| {
                if event.pressed_button == Some(MouseButton::Left) {
                    seek(&drag, event.position.y);
                    window.refresh();
                }
            })
            .on_click(|_, _, cx| cx.stop_propagation())
            .child(
                div()
                    .absolute()
                    .top(px(top))
                    .right(px((TRACK_WIDTH - THUMB_WIDTH) / 2.0))
                    .w(px(THUMB_WIDTH))
                    .h(px(thumb))
                    .rounded(px(THUMB_WIDTH / 2.0))
                    .bg(rgba(color)),
            ),
    )
}

/// For the scrolled container's `on_scroll_wheel`: redraws so the bar follows the wheel.
pub fn refresh_on_scroll(_: &ScrollWheelEvent, window: &mut Window, _: &mut gpui::App) {
    window.refresh();
}

#[cfg(test)]
mod tests {
    use super::thumb_geometry;

    #[test]
    fn no_bar_when_nothing_scrolls() {
        assert_eq!(thumb_geometry(400.0, 0.0, 0.0), None);
        assert_eq!(thumb_geometry(4.0, 100.0, 0.0), None);
    }

    #[test]
    fn thumb_spans_the_track_by_what_is_shown() {
        // half the content shown: the thumb is half the track, at the top, then at the bottom
        let (top, thumb) = thumb_geometry(408.0, 408.0, 0.0).unwrap();
        assert_eq!((top, thumb), (4.0, 200.0));
        let (top, _) = thumb_geometry(408.0, 408.0, 408.0).unwrap();
        assert_eq!(top, 4.0 + 200.0);
        // a long page keeps a thumb big enough to grab, and scrolling past the end stays put
        let (_, thumb) = thumb_geometry(200.0, 100_000.0, 0.0).unwrap();
        assert_eq!(thumb, 24.0);
        let (top, thumb) = thumb_geometry(200.0, 100_000.0, 1e9).unwrap();
        assert_eq!(top + thumb, 200.0 - 4.0);
    }
}
