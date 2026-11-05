//! "Where should these games go?": before a launcher is installed or the GOG
//! or Epic prefix is created, the user picks this Mac or an external drive.
//! The backend then keeps that launcher's games in a "MetalSharp <Name>
//! Games" folder on the drive.
use super::*;
use crate::library_model::LibGame;
use crate::live::{self, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};

/// What to do once a drive is chosen.
#[derive(Clone)]
pub(super) enum DriveNext {
    Launcher(LauncherAction),
    Gate(&'static str, crate::sharp_preview::sharp_deck::GateStep),
    DeckInstall(LibGame),
}

#[derive(Clone)]
pub(super) struct DrivePrompt {
    pub page: LibraryPage,
    pub volumes: Vec<Value>,
    pub next: DriveNext,
    pub saving: bool,
}

fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

pub(super) fn folder_name(page: LibraryPage) -> String {
    format!("MetalSharp {} Games", page.label())
}

fn free_space(bytes: u64) -> String {
    let gb = bytes as f64 / 1_000_000_000.0;
    if gb >= 1000.0 {
        format!("{:.1} TB free", gb / 1000.0)
    } else {
        format!("{gb:.0} GB free")
    }
}

impl MetalSharpApp {
    /// Ask for a drive, then carry on with `next`.
    pub(super) fn ask_drive(&mut self, page: LibraryPage, next: DriveNext, cx: &mut Context<Self>) {
        self.close_library_menus();
        live::call(
            cx,
            "GET",
            "/library-locations",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                let volumes = result
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("volumes"))
                    .and_then(Value::as_array)
                    .cloned()
                    .unwrap_or_default();
                if volumes.is_empty() {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Could not list this Mac's drives".into()),
                    );
                    return;
                }
                this.drive_prompt = Some(DrivePrompt {
                    page,
                    volumes,
                    next,
                    saving: false,
                });
                cx.notify();
            },
        );
    }

    /// Whether this launcher already has a games folder on a mounted drive.
    pub(super) fn refresh_library_locations(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/library-locations",
            None,
            live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                if let Some(locations) = result
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("locations"))
                    .and_then(Value::as_object)
                {
                    this.live.library_locations = locations
                        .iter()
                        .filter(|(_, location)| {
                            location.get("available").and_then(Value::as_bool) == Some(true)
                        })
                        .map(|(launcher, _)| launcher.clone())
                        .collect();
                    cx.notify();
                }
            },
        );
    }

    fn choose_drive(&mut self, volume: String, cx: &mut Context<Self>) {
        let Some(prompt) = self.drive_prompt.as_mut() else {
            return;
        };
        if prompt.saving {
            return;
        }
        prompt.saving = true;
        let launcher = prompt.page.storage_id();
        live::call(
            cx,
            "POST",
            "/library-locations/set",
            Some(json!({"launcher": launcher, "volume": volume})),
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                let Some(prompt) = this.drive_prompt.take() else {
                    return;
                };
                if !result.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| "Could not create the games folder".into()),
                    );
                    cx.notify();
                    return;
                }
                if let Some(path) = result
                    .as_ref()
                    .and_then(|r| r.get("path"))
                    .and_then(Value::as_str)
                {
                    toast::success(cx, format!("{} games go to {path}", prompt.page.label()));
                }
                this.live.library_locations.insert(launcher.to_owned());
                match prompt.next {
                    DriveNext::Launcher(action) => this.run_launcher_action_now(action, cx),
                    DriveNext::Gate(source, step) => {
                        if let Some(sharp) = &this.sharp_preview {
                            sharp.update(cx, |sharp, cx| sharp.run_gate(source, step, cx));
                        }
                    }
                    DriveNext::DeckInstall(game) => this.install_deck_game(game, cx),
                }
                cx.notify();
            },
        );
        cx.notify();
    }

    pub(super) fn render_drive_prompt(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> Option<impl IntoElement> {
        let prompt = self.drive_prompt.as_ref()?;
        let layout = LibraryLayout::for_viewport(viewport.width.into(), viewport.height.into());
        let theme = self.theme;
        let text_color = theme.control_text();
        let mut rows = div().flex().flex_col().gap(px(layout.scaled(8.0)));
        for (index, volume) in prompt.volumes.iter().enumerate() {
            let id = text(volume, "id").to_owned();
            let external = volume.get("external").and_then(Value::as_bool) == Some(true);
            let free = volume
                .get("free_bytes")
                .and_then(Value::as_u64)
                .map(free_space)
                .unwrap_or_default();
            let root = text(volume, "root");
            rows = rows.child(
                div()
                    .id(("drive-option", index))
                    .flex()
                    .items_center()
                    .gap(px(layout.scaled(12.0)))
                    .p(px(layout.scaled(12.0)))
                    .rounded(px(layout.scaled(8.0)))
                    .border_1()
                    .border_color(rgba(theme.page_palette().control_border))
                    .bg(rgb(theme.control_bg()))
                    .cursor_pointer()
                    .hover(move |style| style.bg(rgb(theme.control_hover())))
                    .when(prompt.saving, |row| row.opacity(0.6))
                    .on_click(cx.listener(move |this, _, _, cx| this.choose_drive(id.clone(), cx)))
                    .child(control_icon(
                        if external {
                            "icon-download.svg"
                        } else {
                            "icon-windows.svg"
                        },
                        layout.scaled(22.0),
                        text_color,
                    ))
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(2.0))
                            .child(
                                div()
                                    .text_size(px(layout.scaled(14.0)))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(rgb(text_color))
                                    .child(format!(
                                        "{}{}",
                                        text(volume, "name"),
                                        if external { " · External drive" } else { "" }
                                    )),
                            )
                            .child(
                                div()
                                    .text_size(px(layout.scaled(11.5)))
                                    .text_color(rgb(0x96a1b2))
                                    .truncate()
                                    .child(format!("{root}/{} · {free}", folder_name(prompt.page))),
                            ),
                    ),
            );
        }
        let panel = div()
            .id("drive-prompt")
            .occlude()
            .w(px(layout.scaled(460.0)))
            .flex()
            .flex_col()
            .gap(px(layout.scaled(14.0)))
            .p(px(layout.scaled(22.0)))
            .rounded(px(layout.scaled(12.0)))
            .border_1()
            .border_color(rgba(theme.page_palette().control_border))
            .bg(rgb(theme.menu_bg()))
            .shadow_lg()
            .child(
                div()
                    .text_size(px(layout.scaled(18.0)))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(text_color))
                    .child(format!("Where should {} games go?", prompt.page.label())),
            )
            .child(
                div()
                    .text_size(px(layout.scaled(12.5)))
                    .line_height(px(layout.scaled(18.0)))
                    .text_color(rgb(0xaeb3b2))
                    .child(format!(
                        "MetalSharp creates a \"{}\" folder on the drive you pick, installs games there and launches them from it.",
                        folder_name(prompt.page)
                    )),
            )
            .child(rows)
            .child(
                div().flex().justify_end().child(
                    div()
                        .id("drive-prompt-cancel")
                        .px(px(layout.scaled(14.0)))
                        .py(px(layout.scaled(7.0)))
                        .rounded(px(layout.scaled(7.0)))
                        .text_size(px(layout.scaled(13.0)))
                        .text_color(rgb(text_color))
                        .cursor_pointer()
                        .hover(move |style| style.bg(rgb(theme.menu_hover())))
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.drive_prompt = None;
                            cx.notify();
                        }))
                        .child("Cancel"),
                ),
            );
        Some(
            div()
                .id("drive-prompt-scrim")
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .bg(rgba(0x000000a0))
                .occlude()
                .child(panel),
        )
    }
}
