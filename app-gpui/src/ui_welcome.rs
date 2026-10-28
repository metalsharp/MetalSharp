//! The FEX build's welcome: an overlay over the library, shown once after an x86_64 install (0.80.0
//! or any regular build) moved to the Arm64 FEX build. The backend decides that it is due
//! (`GET /welcome/fex`, set by the regular-to-FEX migration or setup's Steam prefix conversion) and
//! records it as shown (`POST /welcome/fex/dismiss`), so it never comes back: not for a fresh FEX
//! install, a FEX update or the regular build.
use super::*;
use crate::live::{self, is_ok};

pub(super) const FEX_WELCOME_TITLE: &str = "Welcome To MetalSharp 1.0.0: An Arm64 Wine with FEX";
const ISSUES_URL: &str = "https://github.com/metalsharp/MetalSharp/issues";

/// The welcome's pages: heading and paragraphs. The graphics page follows the FEX build's routing
/// in steam_actions.c and wine_runtime.c (ms_wine_route_pipeline).
pub(super) const FEX_WELCOME_PAGES: [(&str, &[&str]); 3] = [
    (
        "Early access",
        &[
            "This MetalSharp runs your games on MetalSharp Wine, a native Arm64 Wine that translates Windows x86 \
             code with FEX instead of Rosetta. It is early access: expect bugs, games that don't start yet, and \
             rough edges.",
            "When something goes wrong, please report it. The Logs tab has each game's live log and its crash \
             reports (Settings > Open Logs opens the folder); attach them to an issue on MetalSharp's GitHub.",
        ],
    ),
    (
        "The new graphics layout",
        &[
            "VKD3D-Proton: most Direct3D 12 games run here, on Vulkan through MetalSharp's MoltenVK to Metal. \
             Games that used D3DMetal or GPTK on the x86_64 build move to this lane.",
            "Mullion (DXMT): Direct3D 12, 11 and 10 straight to Metal, for 64-bit and 32-bit x86 games. It is \
             the default lane, and the one for D3D11 and D3D10 games.",
            "DXVK: Direct3D 9, 10 and 11 on Vulkan through MoltenVK, for 64-bit games. The x86_64 build's D3D9 \
             route runs here, and it is worth a try when a game misbehaves on Mullion.",
            "OpenGL: Wine's own Direct3D on WineMetalGL; games with an OpenGL renderer (Unity, Unreal, Source) \
             are started with it.",
            "Each game's lane is in the pipeline menu on its card in the library.",
        ],
    ),
    (
        "Get Into It",
        &[
            "Your library, Steam and settings came across from the x86_64 build, and your Wine prefixes were moved \
             to MetalSharp Wine with a backup. That's it: pick a game and play.",
        ],
    ),
];

/// The page after a Back (-1) or Next (+1), kept within the pages.
pub(super) fn fex_welcome_turn(page: usize, step: isize) -> usize {
    page.saturating_add_signed(step)
        .min(FEX_WELCOME_PAGES.len() - 1)
}

impl MetalSharpApp {
    /// Asks the backend whether the welcome is due; the FEX build only.
    pub(super) fn check_fex_welcome(&mut self, cx: &mut Context<Self>) {
        if !crate::variant::is_fex() || self.live.fex_welcome_page.is_some() {
            return;
        }
        live::call(
            cx,
            "GET",
            "/welcome/fex",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                if result.as_ref().is_some_and(|r| {
                    is_ok(r) && r.get("show").and_then(serde_json::Value::as_bool) == Some(true)
                }) {
                    this.live.fex_welcome_page = Some(0);
                    cx.notify();
                }
            },
        );
    }

    /// Closes the welcome into the library and records it as shown.
    fn dismiss_fex_welcome(&mut self, cx: &mut Context<Self>) {
        self.live.fex_welcome_page = None;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/welcome/fex/dismiss",
            None,
            live::DEFAULT_TIMEOUT,
            |_, _, _| {},
        );
    }

    fn turn_fex_welcome(&mut self, step: isize, cx: &mut Context<Self>) {
        if let Some(page) = self.live.fex_welcome_page {
            self.live.fex_welcome_page = Some(fex_welcome_turn(page, step));
            cx.notify();
        }
    }

    /// The overlay: the library behind it frosted (GPUI has no backdrop blur), a click outside
    /// dismisses it, Back and Next move between the pages.
    pub(super) fn render_fex_welcome(
        &self,
        page: usize,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let page = page.min(FEX_WELCOME_PAGES.len() - 1);
        let last = page + 1 == FEX_WELCOME_PAGES.len();
        let (heading, paragraphs) = FEX_WELCOME_PAGES[page];
        let button = |id: &'static str, label: &'static str, primary: bool| {
            div()
                .id(id)
                .h(px(34.0))
                .px(px(16.0))
                .flex()
                .items_center()
                .rounded(px(7.0))
                .border_1()
                .border_color(rgba(if primary { 0xefe7d6ff } else { 0xffffff2e }))
                .bg(rgb(if primary { 0xefe7d6 } else { 0x1d2124 }))
                .text_color(rgb(if primary { 0x14161a } else { 0xe6e8e7 }))
                .text_size(px(13.0))
                .font_weight(FontWeight::SEMIBOLD)
                .cursor_pointer()
                .child(label)
        };
        let dots = div()
            .flex()
            .gap(px(6.0))
            .children((0..FEX_WELCOME_PAGES.len()).map(|index| {
                div()
                    .w(px(if index == page { 18.0 } else { 6.0 }))
                    .h(px(6.0))
                    .rounded(px(3.0))
                    .bg(rgba(if index == page {
                        0xefe7d6ff
                    } else {
                        0xffffff3a
                    }))
            }));
        let mut buttons = div().flex().items_center().gap(px(8.0));
        if page == 0 {
            buttons = buttons.child(
                button("fex-welcome-report", "Report an issue", false)
                    .on_click(|_, _, cx| cx.open_url(ISSUES_URL)),
            );
        } else {
            buttons = buttons.child(
                button("fex-welcome-back", "Back", false)
                    .on_click(cx.listener(|this, _, _, cx| this.turn_fex_welcome(-1, cx))),
            );
        }
        buttons = buttons.child(if last {
            button("fex-welcome-done", "Get Into It", true)
                .on_click(cx.listener(|this, _, _, cx| this.dismiss_fex_welcome(cx)))
        } else {
            button("fex-welcome-next", "Next", true)
                .on_click(cx.listener(|this, _, _, cx| this.turn_fex_welcome(1, cx)))
        });
        div()
            .id("fex-welcome-backdrop")
            .occlude()
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgba(0x080a0de6))
            .on_click(cx.listener(|this, _, _, cx| this.dismiss_fex_welcome(cx)))
            .child(
                div()
                    .id("fex-welcome")
                    .occlude()
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .w(px(560.0))
                    .p(px(26.0))
                    .rounded(px(14.0))
                    .border_1()
                    .border_color(rgba(0xffffff1f))
                    .bg(rgb(0x14171a))
                    .shadow_lg()
                    .text_color(rgb(0xe6e8e7))
                    .font_family("Rethink Sans")
                    .flex()
                    .flex_col()
                    .gap(px(16.0))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(6.0))
                            .child(
                                div()
                                    .text_size(px(12.0))
                                    .text_color(rgb(MUTED))
                                    .child(format!("{} of {}", page + 1, FEX_WELCOME_PAGES.len())),
                            )
                            .child(
                                div()
                                    .text_size(px(19.0))
                                    .line_height(px(25.0))
                                    .font_weight(FontWeight::BOLD)
                                    .child(FEX_WELCOME_TITLE),
                            ),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(10.0))
                            .child(
                                div()
                                    .text_size(px(15.0))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(heading),
                            )
                            .children(paragraphs.iter().map(|text| {
                                div()
                                    .text_size(px(13.0))
                                    .line_height(px(19.0))
                                    .text_color(rgb(0xc9cdcb))
                                    .child(*text)
                            })),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .justify_between()
                            .child(dots)
                            .child(buttons),
                    ),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn welcome_has_three_pages_ending_in_get_into_it() {
        assert_eq!(FEX_WELCOME_PAGES.len(), 3);
        assert_eq!(FEX_WELCOME_PAGES[0].0, "Early access");
        assert_eq!(FEX_WELCOME_PAGES[2].0, "Get Into It");
        assert_eq!(
            FEX_WELCOME_TITLE,
            "Welcome To MetalSharp 1.0.0: An Arm64 Wine with FEX"
        );
        let graphics = FEX_WELCOME_PAGES[1].1.join(" ");
        for lane in ["VKD3D-Proton", "Mullion", "DXVK", "OpenGL"] {
            assert!(graphics.contains(lane), "{lane}");
        }
    }

    #[test]
    fn back_and_next_stay_within_the_pages() {
        assert_eq!(fex_welcome_turn(0, -1), 0);
        assert_eq!(fex_welcome_turn(0, 1), 1);
        assert_eq!(fex_welcome_turn(1, 1), 2);
        assert_eq!(fex_welcome_turn(2, 1), 2);
        assert_eq!(fex_welcome_turn(2, -1), 1);
    }
}
