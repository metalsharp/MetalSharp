use gpui::{FontWeight, SharedString, div, prelude::*, px, rgb};

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
        }
    }
}

impl PagePalette {
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
}
