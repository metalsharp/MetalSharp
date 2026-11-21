use gpui::{FontWeight, SharedString, div, prelude::*, px, rgb, rgba};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PagePalette {
    pub accent: u32,
    pub border: u32,
    pub control_border: u32,
    pub control_bg: u32,
    pub control_text: u32,
    pub hover: u32,
    pub menu_bg: u32,
    pub menu_hover: u32,
    pub light: bool,
    /// Liquid Glass (macOS 26 and later): panels are translucent over the blurred window.
    pub glass: bool,
}

impl Default for PagePalette {
    fn default() -> Self {
        Self {
            accent: 0xe8d6b7,
            border: 0xe8d6b7,
            control_border: 0xffffff38,
            control_bg: 0x080a0d,
            control_text: 0xffffff,
            hover: 0x171a1e,
            menu_bg: 0x080a0d,
            menu_hover: 0x171a1e,
            light: false,
            glass: false,
        }
    }
}

impl PagePalette {
    /// What dims the page behind a panel: nothing under Liquid Glass, where it would dim the panel's glass too.
    pub fn scrim(&self) -> gpui::Rgba {
        if self.glass {
            rgba(0x00000000)
        } else {
            rgba(0x06080a94)
        }
    }

    /// A settings panel's fill: the control background, translucent under Liquid Glass.
    pub fn panel_bg(&self) -> gpui::Rgba {
        if self.glass {
            // AppKit's glass is under the panel (liquid_glass::mark): only a veil here, for the text
            rgba((self.control_bg << 8) | 0x40)
        } else {
            rgba((self.control_bg << 8) | 0xff)
        }
    }

    /// A settings panel's rim: brighter under Liquid Glass, where light catches the glass's edge.
    pub fn panel_border(&self) -> gpui::Rgba {
        if self.glass {
            rgba(if self.light { 0xffffffb3 } else { 0xffffff47 })
        } else {
            rgba((self.control_text << 8) | 0x24)
        }
    }

    pub fn button(
        &self,
        id: impl Into<gpui::ElementId>,
        label: impl Into<SharedString>,
        primary: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let palette = *self;
        div()
            .id(id)
            .flex_none()
            .flex()
            .items_center()
            .justify_center()
            .gap(px(7.0))
            .h(px(34.0))
            .px(px(12.0))
            .rounded(px(8.0))
            .border_1()
            .border_color(gpui::rgba(self.control_border))
            .bg(rgb(if primary {
                self.accent
            } else {
                self.control_bg
            }))
            .text_color(rgb(if primary { 0x101416 } else { self.control_text }))
            .text_size(px(12.0))
            .font_weight(FontWeight::SEMIBOLD)
            .cursor_pointer()
            .hover(move |style| {
                style.bg(rgb(if primary {
                    palette.accent
                } else {
                    palette.hover
                }))
            })
            .child(label.into())
    }

    /// A setting: its title and control on one line (stacked when narrow), its description under them, and a
    /// hairline below. Settings and the game settings popup both lay settings out with it.
    pub fn setting_row(
        &self,
        title: impl Into<SharedString>,
        desc: impl IntoElement,
        control: impl IntoElement,
        narrow: bool,
    ) -> gpui::Div {
        let p = *self;
        div()
            .w_full()
            .min_w_0()
            .flex()
            .flex_col()
            .gap(px(7.0))
            .py(px(16.0))
            .border_b_1()
            .border_color(rgba(
                (p.control_border & 0xffffff00)
                    | (((p.control_border & 0xff) as f32 * 0.35) as u32),
            ))
            .child(
                div()
                    .w_full()
                    .min_w_0()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap(px(12.0))
                    .when(narrow, |d| d.flex_col().items_start())
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .text_color(rgb(p.control_text))
                            .text_size(px(14.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(title.into()),
                    )
                    .child(
                        div()
                            .flex_none()
                            .min_w_0()
                            .max_w_full()
                            .flex()
                            .flex_wrap()
                            .items_center()
                            .gap(px(8.0))
                            .child(control),
                    ),
            )
            .child(
                div()
                    .w_full()
                    .min_w_0()
                    .text_color(rgba((p.control_text << 8) | 0xad))
                    .text_size(px(13.0))
                    .line_height(px(20.0))
                    .child(desc),
            )
    }

    /// An on/off switch (the caller adds its click).
    pub fn switch(&self, id: impl Into<gpui::ElementId>, on: bool) -> gpui::Stateful<gpui::Div> {
        let p = *self;
        div()
            .id(id)
            .flex()
            .flex_none()
            .items_center()
            .w(px(40.))
            .h(px(24.))
            .px(px(2.))
            .rounded(px(999.))
            .border_1()
            .border_color(rgba(if on {
                (p.accent << 8) | 0xff
            } else {
                p.control_border
            }))
            .bg(if on {
                rgba((p.accent << 8) | 0x4d)
            } else {
                rgba((p.control_bg << 8) | 0xff)
            })
            .cursor_pointer()
            .child(
                div()
                    .size(px(18.))
                    .rounded(px(999.))
                    .bg(rgba(
                        ((if on { p.accent } else { p.control_text }) << 8)
                            | if on { 0xff } else { 0xb8 },
                    ))
                    .when(on, |d| d.ml(px(16.))),
            )
    }

    /// One choice of a few (the caller adds its click), as the segmented controls of Settings.
    pub fn choice(
        &self,
        id: impl Into<gpui::ElementId>,
        label: impl Into<SharedString>,
        active: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let p = *self;
        div()
            .id(id)
            .flex_none()
            .flex()
            .items_center()
            .h(px(30.0))
            .px(px(11.0))
            .rounded(px(999.0))
            .border_1()
            .border_color(rgba(if active {
                (p.accent << 8) | 0xc0
            } else {
                p.control_border
            }))
            .bg(rgba(if active {
                (p.accent << 8) | 0x2e
            } else {
                (p.control_bg << 8) | 0xff
            }))
            .text_size(px(12.5))
            .font_weight(if active {
                FontWeight::SEMIBOLD
            } else {
                FontWeight::NORMAL
            })
            .text_color(rgb(if active && !p.light {
                p.accent
            } else {
                p.control_text
            }))
            .cursor_pointer()
            .hover(move |style| style.bg(rgb(p.hover)))
            .child(label.into())
    }

    /// A tab of a settings panel's navigation (the caller adds its click).
    pub fn nav_item(
        &self,
        id: impl Into<gpui::ElementId>,
        label: impl Into<SharedString>,
        active: bool,
    ) -> gpui::Stateful<gpui::Div> {
        let p = *self;
        div()
            .id(id)
            .flex_none()
            .flex()
            .items_center()
            .h(px(42.0))
            .px(px(13.0))
            .rounded(px(9.0))
            .border_1()
            .border_color(rgba(if active {
                (p.accent << 8) | 0x45
            } else {
                0x00000000
            }))
            .bg(rgba(if active {
                (p.accent << 8) | 0x19
            } else {
                0x00000000
            }))
            .text_size(px(14.0))
            .font_weight(if active {
                FontWeight::SEMIBOLD
            } else {
                FontWeight::NORMAL
            })
            .text_color(rgb(if active && !p.light {
                p.accent
            } else {
                p.control_text
            }))
            .cursor_pointer()
            .hover(move |style| style.bg(rgb(p.hover)))
            .child(label.into())
    }
}

/// How the library's own fixed colors (its literals) are shown in the current theme. Dark and Light are black and
/// white UIs: their neutral colors (grays, the off-whites of text, the near-blacks of surfaces and scrims) are turned
/// to the theme's, and every colored value (accents, status colors, art) is left as it is. The other themes keep
/// every literal.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tone {
    /// the other themes: literals as they are
    Keep,
    /// Dark: neutral grays, and the dark surfaces deepened to black, under white text
    Black,
    /// Light: neutral grays inverted, so the dark surfaces are white and white text is black
    White,
}

thread_local! {
    static TONE: std::cell::Cell<Tone> = const { std::cell::Cell::new(Tone::Keep) };
}

/// The theme's tone, for everything painted after it this frame (MetalSharp's window sets it as it renders).
pub fn set_tone(tone: Tone) {
    TONE.set(tone);
}

/// A fixed color (0xRRGGBB) in the current theme's tone.
pub fn tc(color: u32) -> u32 {
    tone_rgb(TONE.get(), color)
}

/// A fixed color with alpha (0xRRGGBBAA) in the current theme's tone: the alpha is kept.
pub fn tca(color: u32) -> u32 {
    (tone_rgb(TONE.get(), color >> 8) << 8) | (color & 0xff)
}

fn tone_rgb(tone: Tone, color: u32) -> u32 {
    if tone == Tone::Keep {
        return color;
    }
    let channel = |shift: u32| ((color >> shift) & 0xff) as f32 / 255.0;
    let (r, g, b) = (channel(16), channel(8), channel(0));
    let (max, min) = (r.max(g).max(b), r.min(g).min(b));
    // colored: an accent, a status color, a hue meant to be seen
    if max - min >= 0.2 {
        return color;
    }
    let lightness = (max + min) / 2.0;
    let gray = match tone {
        Tone::Keep => unreachable!(),
        // dark surfaces to black; text and lines stay as light as they were, without their warm or cool cast
        Tone::Black if lightness < 0.25 => lightness * 0.35,
        Tone::Black => lightness,
        // inverted: white text black, dark surfaces white (the palest of them all the way)
        Tone::White => {
            let inverted = 1.0 - lightness;
            if inverted > 0.88 {
                (inverted + 1.0) / 2.0
            } else {
                inverted
            }
        }
    };
    let level = (gray.clamp(0.0, 1.0) * 255.0).round() as u32;
    (level << 16) | (level << 8) | level
}

#[cfg(test)]
mod tone_tests {
    use super::{Tone, tone_rgb};

    #[test]
    fn dark_and_light_turn_neutrals_and_keep_colors() {
        // white text: white on Dark, black on Light
        assert_eq!(tone_rgb(Tone::Black, 0xffffff), 0xffffff);
        assert_eq!(tone_rgb(Tone::White, 0xffffff), 0x000000);
        // a near-black surface: black on Dark, white on Light
        assert!(tone_rgb(Tone::Black, 0x111416) <= 0x080808);
        assert!(tone_rgb(Tone::White, 0x111416) >= 0xf5f5f5);
        // a warm off-white text is a neutral gray, dark on Light
        assert!(tone_rgb(Tone::White, 0xefe7d6) < 0x303030);
        // colors stay
        assert_eq!(tone_rgb(Tone::White, 0x74d28a), 0x74d28a);
        assert_eq!(tone_rgb(Tone::Black, 0xff9d9d), 0xff9d9d);
        // the other themes keep every literal
        assert_eq!(tone_rgb(Tone::Keep, 0x111416), 0x111416);
    }
}
