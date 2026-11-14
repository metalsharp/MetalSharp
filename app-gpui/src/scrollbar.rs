//! A thin vertical scrollbar for a container scrolled with a [`ScrollHandle`].
//!
//! GPUI scrolls `overflow_y_scroll` containers but draws no scrollbar. Put the scrolled container
//! (with `.track_scroll(handle)` and [`refresh_on_scroll`]) and [`vertical`] in a `relative()`
//! parent: the bar sits over the container's right edge, shows how far it is scrolled, and
//! scrolls it when the track is clicked or dragged. It is left out when nothing scrolls.

use std::{cell::Cell, rc::Rc};

use gpui::{
    App, MouseButton, Pixels, ScrollHandle, ScrollWheelEvent, Stateful, Window, div, point,
    prelude::*, px, rgba,
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

/// The scroll offset that puts the thumb's top at `thumb_top` (from the track's top) in a viewport
/// of `viewport` scrolling `max` with a thumb `thumb` tall.
fn offset_for_thumb_top(viewport: f32, max: f32, thumb: f32, thumb_top: f32) -> f32 {
    let travel = viewport - 2.0 * INSET - thumb;
    if travel <= 0.0 {
        return 0.0;
    }
    ((thumb_top - INSET) / travel).clamp(0.0, 1.0) * max
}

/// Scrolls `handle` so the thumb's point `grab` below its top is at `y` (window coordinates).
fn seek(handle: &ScrollHandle, y: Pixels, grab: f32) {
    let bounds = handle.bounds();
    let viewport = f32::from(bounds.size.height);
    let max = f32::from(handle.max_offset().height);
    let Some((_, thumb)) = thumb_geometry(viewport, max, 0.0) else {
        return;
    };
    let thumb_top = f32::from(y - bounds.origin.y) - grab;
    let offset = offset_for_thumb_top(viewport, max, thumb, thumb_top);
    handle.set_offset(point(handle.offset().x, px(-offset)));
}

/// A thumb being dragged: which bar's, and where on the thumb it was taken. Held by GPUI for the
/// whole drag, so the drag follows the pointer anywhere until the button is let go.
#[derive(Clone)]
struct ThumbDrag {
    bar: &'static str,
    grab: Rc<Cell<f32>>,
}

/// GPUI draws a dragged item's preview; a thumb has none.
struct NoPreview;

impl Render for NoPreview {
    fn render(&mut self, _: &mut Window, _: &mut gpui::Context<Self>) -> impl IntoElement {
        div()
    }
}

/// The bar for the container `handle` scrolls, in `rgba` color (0xRRGGBBAA).
pub fn vertical(
    bar: &'static str,
    handle: &ScrollHandle,
    color: u32,
) -> Option<Stateful<gpui::Div>> {
    let viewport = f32::from(handle.bounds().size.height);
    let max = f32::from(handle.max_offset().height);
    let scrolled = -f32::from(handle.offset().y);
    let (top, thumb) = thumb_geometry(viewport, max, scrolled)?;
    let page = handle.clone();
    let dragged = handle.clone();
    Some(
        div()
            .id(bar)
            .absolute()
            .top(px(0.0))
            .right(px(0.0))
            .w(px(TRACK_WIDTH))
            .h_full()
            .cursor_default()
            // a click on the track (not the thumb) brings the thumb's middle there
            .on_mouse_down(MouseButton::Left, move |event, window, cx| {
                seek(&page, event.position.y, thumb / 2.0);
                window.refresh();
                cx.stop_propagation();
            })
            .on_drag_move::<ThumbDrag>(move |event, window, cx| {
                let drag = event.drag(cx);
                if drag.bar == bar {
                    seek(&dragged, event.event.position.y, drag.grab.get());
                    window.refresh();
                }
            })
            .on_click(|_, _, cx| cx.stop_propagation())
            .child(
                div()
                    .id((bar, 1usize))
                    .absolute()
                    .top(px(top))
                    .left(px(0.0))
                    .w(px(TRACK_WIDTH))
                    .h(px(thumb))
                    .flex()
                    .justify_center()
                    .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                    .on_drag(
                        ThumbDrag {
                            bar,
                            grab: Rc::new(Cell::new(thumb / 2.0)),
                        },
                        |drag: &ThumbDrag, offset, _, cx: &mut App| {
                            drag.grab.set(f32::from(offset.y));
                            cx.new(|_| NoPreview)
                        },
                    )
                    .child(
                        div()
                            .w(px(THUMB_WIDTH))
                            .h_full()
                            .rounded(px(THUMB_WIDTH / 2.0))
                            .bg(rgba(color)),
                    ),
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
    fn dragging_the_thumb_maps_its_top_to_the_offset() {
        use super::offset_for_thumb_top;
        // 408 tall, 408 more below: a 200 tall thumb travels 200
        assert_eq!(offset_for_thumb_top(408.0, 408.0, 200.0, 4.0), 0.0);
        assert_eq!(offset_for_thumb_top(408.0, 408.0, 200.0, 104.0), 204.0);
        assert_eq!(offset_for_thumb_top(408.0, 408.0, 200.0, 204.0), 408.0);
        // past either end stays at the end
        assert_eq!(offset_for_thumb_top(408.0, 408.0, 200.0, -50.0), 0.0);
        assert_eq!(offset_for_thumb_top(408.0, 408.0, 200.0, 900.0), 408.0);
    }

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
