//! Liquid Glass's GPUI side (glass/MSGlass.swift is AppKit's): the surfaces that are glass mark themselves as GPUI
//! paints them, and the native glass under GPUI's view follows. A frame begins (the first thing painted), marks
//! its surfaces, and ends (the last); a surface not painted in a frame loses its glass.
use gpui::{Bounds, IntoElement, Pixels, Styled, Window, canvas, div, prelude::*};
use std::hash::{Hash, Hasher};

/// The kinds of glass, from the quietest to the brightest, so surfaces contrast.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Glass {
    /// Regular glass with a dim tint: panels, the shelf, the footer.
    Panel = 0,
    /// Clear glass: buttons, the search field, pills.
    Control = 1,
    /// Bright frosted glass: the one action that matters most (Play).
    Primary = 2,
    /// Clear glass with an accent tint: the selected card.
    Selected = 3,
    /// Deep glass: a panel over the art (settings), whose text must read over anything.
    Sheet = 4,
    /// The sidebar: regular glass tinted with the page's own color, not the accent, so the theme's color doesn't
    /// wash into it. Glass Transparency thins its tint, but it keeps its blur.
    Sidebar = 5,
}

#[cfg(target_os = "macos")]
unsafe extern "C" {
    fn ms_glass_begin(
        view: *mut core::ffi::c_void,
        dark: bool,
        page: u32,
        accent: u32,
        glow: u32,
        clarity: f64,
    );
    fn ms_glass_region(
        view: *mut core::ffi::c_void,
        id: u64,
        x: f64,
        y: f64,
        width: f64,
        height: f64,
        radius: f64,
        kind: i32,
        rotation: f64,
    );
    fn ms_glass_hero(
        view: *mut core::ffi::c_void,
        path: *const core::ffi::c_char,
        x: f64,
        y: f64,
        width: f64,
        height: f64,
    );
    fn ms_glass_end(view: *mut core::ffi::c_void);
    fn ms_glass_detach(view: *mut core::ffi::c_void);
}

#[cfg(target_os = "macos")]
fn gpui_view(window: &Window) -> Option<*mut core::ffi::c_void> {
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    let handle = HasWindowHandle::window_handle(window).ok()?;
    match handle.as_raw() {
        RawWindowHandle::AppKit(appkit) => Some(appkit.ns_view.as_ptr()),
        _ => None,
    }
}

fn rect(bounds: Bounds<Pixels>) -> (f64, f64, f64, f64) {
    (
        f32::from(bounds.origin.x) as f64,
        f32::from(bounds.origin.y) as f64,
        f32::from(bounds.size.width) as f64,
        f32::from(bounds.size.height) as f64,
    )
}

fn region_id(id: &impl Hash) -> u64 {
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    id.hash(&mut hasher);
    // 0 is the hero's
    hasher.finish().max(1)
}

/// The start of a frame's glass: paint it before anything that marks a surface.
/// `page` and `accent` are the theme's (0xRRGGBB): they tint the glass. `glow` is what a selected surface glows.
/// `clarity`, 0.0 to 1.0, is Settings' Glass Transparency: the higher, the less the panels are tinted and blurred.
pub fn begin(dark: bool, page: u32, accent: u32, glow: u32, clarity: f32) -> impl IntoElement {
    canvas(
        |_, _, _| (),
        move |_, (), window, _| {
            #[cfg(target_os = "macos")]
            COVERED.set(false);
            if let Some(view) = gpui_view(window) {
                // SAFETY: view is the window's live GPUI NSView, on the main thread.
                unsafe {
                    ms_glass_begin(
                        view,
                        dark,
                        page,
                        accent,
                        glow,
                        f64::from(clarity.clamp(0.0, 1.0)),
                    )
                }
            }
        },
    )
    .absolute()
    .size_0()
}

/// The end of a frame's glass: paint it after everything (a deferred element of the highest priority).
pub fn end() -> impl IntoElement {
    canvas(
        |_, _, _| (),
        move |_, (), window, _| {
            #[cfg(target_os = "macos")]
            if let Some(view) = gpui_view(window) {
                // SAFETY: as in begin.
                unsafe { ms_glass_end(view) }
            }
        },
    )
    .absolute()
    .size_0()
}

/// Liquid Glass turned off: the window's glass goes.
pub fn detach(window: &Window) {
    #[cfg(target_os = "macos")]
    if let Some(view) = gpui_view(window) {
        // SAFETY: as in begin.
        unsafe { ms_glass_detach(view) }
    }
}

/// Glass of `kind` under the element this is a child of (give that element `.relative()`), with its corners.
pub fn mark(id: impl Hash + 'static, kind: Glass, radius: f32) -> impl IntoElement {
    mark_turned(id, kind, radius, 0.0)
}

thread_local! {
    /// While a panel covers the library (its glass alone shows), the library's own surfaces mark nothing.
    static COVERED: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

/// From here in the frame, surfaces mark nothing (`true`) or mark again (`false`): around what a panel covers.
pub fn cover(covered: bool) -> impl IntoElement {
    canvas(|_, _, _| (), move |_, (), _, _| COVERED.set(covered))
        .absolute()
        .size_0()
}

/// As `mark`, turned `degrees` clockwise about its center (a tilted card's frame).
pub fn mark_turned(
    id: impl Hash + 'static,
    kind: Glass,
    radius: f32,
    degrees: f32,
) -> impl IntoElement {
    let id = region_id(&id);
    div().absolute().top_0().left_0().size_full().child(
        canvas(
            |_, _, _| (),
            move |bounds, (), window, _| {
                #[cfg(target_os = "macos")]
                if let Some(view) = gpui_view(window).filter(|_| !COVERED.get()) {
                    // glass stays inside what clips its element (a scrolling page), as the element's own
                    // painting does; a turned frame is left whole
                    let bounds = if degrees == 0.0 {
                        bounds.intersect(&window.content_mask().bounds)
                    } else {
                        bounds
                    };
                    if bounds.size.width <= gpui::px(0.0) || bounds.size.height <= gpui::px(0.0) {
                        return;
                    }
                    let (x, y, width, height) = rect(bounds);
                    // SAFETY: as in begin.
                    unsafe {
                        ms_glass_region(
                            view,
                            id,
                            x,
                            y,
                            width,
                            height,
                            radius as f64,
                            kind as i32,
                            degrees as f64,
                        )
                    }
                }
            },
        )
        .size_full(),
    )
}

/// The selected game's art under the glass, filling the element this is a child of; `None` shows none.
pub fn hero(path: Option<std::path::PathBuf>) -> impl IntoElement {
    let path = path.and_then(|path| std::ffi::CString::new(path.to_string_lossy().as_bytes()).ok());
    div().absolute().top_0().left_0().size_full().child(
        canvas(
            |_, _, _| (),
            move |bounds, (), window, _| {
                #[cfg(target_os = "macos")]
                if let Some(view) = gpui_view(window) {
                    let (x, y, width, height) = rect(bounds);
                    let pointer = path.as_ref().map_or(std::ptr::null(), |path| path.as_ptr());
                    // SAFETY: as in begin; pointer is NUL-terminated and lives through the call.
                    unsafe { ms_glass_hero(view, pointer, x, y, width, height) }
                }
            },
        )
        .size_full(),
    )
}
