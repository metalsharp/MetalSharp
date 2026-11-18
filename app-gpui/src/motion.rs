//! MetalSharp's motion: short transitions that decelerate into place (ease-out quint), so panels, menus and the
//! hero's art arrive instead of snapping in. Low Performance Mode keeps everything still.
use gpui::{Animation, AnimationExt, AnyElement, ElementId, IntoElement, Styled, px};
use std::time::Duration;

/// How long an element takes to arrive.
pub const APPEAR: Duration = Duration::from_millis(240);
/// How long a picture takes to replace another (the hero's art).
pub const CROSSFADE: Duration = Duration::from_millis(320);

/// Fades `element` in as it rises `rise` points into place. A new `id` plays it again, so key it by what the
/// element shows (a tab, a game).
pub fn appear<E: IntoElement + Styled + 'static>(
    element: E,
    id: impl Into<ElementId>,
    rise: f32,
    reduced: bool,
) -> AnyElement {
    if reduced {
        return element.into_any_element();
    }
    element
        .with_animation(
            id,
            Animation::new(APPEAR).with_easing(gpui::ease_out_quint()),
            move |element, t| element.opacity(t).mt(px(rise * (1.0 - t))),
        )
        .into_any_element()
}

/// Fades `element` in where it stands.
pub fn fade<E: IntoElement + Styled + 'static>(
    element: E,
    id: impl Into<ElementId>,
    reduced: bool,
) -> AnyElement {
    if reduced {
        return element.into_any_element();
    }
    element
        .with_animation(
            id,
            Animation::new(CROSSFADE).with_easing(gpui::ease_out_quint()),
            |element, t| element.opacity(t),
        )
        .into_any_element()
}
