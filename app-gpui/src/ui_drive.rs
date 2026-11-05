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

/// GOG and Epic downloads go straight into the folder; the other launchers
/// install games themselves, so the folder appears to them as a drive.
fn drive_description(page: LibraryPage) -> String {
    let folder = folder_name(page);
    let drive = match page {
        LibraryPage::Ubisoft => Some(("Ubisoft Connect", "Y:")),
        LibraryPage::BattleNet => Some(("Battle.net", "Y:")),
        LibraryPage::Rockstar => Some(("the Rockstar Games Launcher", "R:")),
        _ => None,
    };
    match drive {
        Some((launcher, letter)) => format!(
            "MetalSharp creates a \"{folder}\" folder on the drive you pick. In {launcher} it is drive {letter}: choose it when installing a game."
        ),
        None => format!(
            "MetalSharp creates a \"{folder}\" folder on the drive you pick, installs games there and launches them from it."
        ),
    }
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
                    .child(drive_description(prompt.page)),
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

/// "Uninstall <launcher>?" with an opt-in to delete its games folder too.
#[derive(Clone)]
pub(super) struct UninstallPrompt {
    pub page: LibraryPage,
    /// The launcher's "MetalSharp <Name> Games" folder, when one is set.
    pub folder: Option<String>,
    pub delete_games: bool,
    pub busy: bool,
}

impl MetalSharpApp {
    pub(super) fn ask_uninstall(&mut self, page: LibraryPage, cx: &mut Context<Self>) {
        self.close_library_menus();
        self.uninstall_prompt = Some(UninstallPrompt {
            page,
            folder: None,
            delete_games: false,
            busy: false,
        });
        cx.notify();
        live::call(
            cx,
            "GET",
            "/library-locations",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, result, cx| {
                let folder = result
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("locations"))
                    .and_then(|locations| locations.get(page.storage_id()))
                    .and_then(|location| location.get("path"))
                    .and_then(Value::as_str)
                    .map(str::to_owned);
                if let Some(prompt) = this.uninstall_prompt.as_mut().filter(|p| p.page == page) {
                    prompt.folder = folder;
                    cx.notify();
                }
            },
        );
    }

    fn run_uninstall(&mut self, cx: &mut Context<Self>) {
        let Some(prompt) = self.uninstall_prompt.as_mut() else {
            return;
        };
        if prompt.busy {
            return;
        }
        prompt.busy = true;
        let page = prompt.page;
        let delete_games = prompt.delete_games && prompt.folder.is_some();
        let name = self.page_launcher_name();
        live::call(
            cx,
            "POST",
            "/launchers/uninstall",
            Some(json!({"launcher": page.storage_id(), "delete_games": delete_games})),
            std::time::Duration::from_secs(600),
            move |this, result, cx| {
                this.uninstall_prompt = None;
                if result.as_ref().is_some_and(is_ok) {
                    toast::success(
                        cx,
                        if delete_games {
                            format!("Uninstalled {name} and deleted its games")
                        } else {
                            format!("Uninstalled {name}")
                        },
                    );
                    if delete_games {
                        this.live.library_locations.remove(page.storage_id());
                    }
                    this.load_library(true, cx);
                    this.refresh_steam_status(cx);
                    if let Some(sharp) = &this.sharp_preview {
                        sharp.update(cx, |sharp, cx| sharp.reload(cx));
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(result.as_ref())
                            .unwrap_or_else(|| format!("Could not uninstall {name}")),
                    );
                }
                cx.notify();
            },
        );
        cx.notify();
    }

    pub(super) fn render_uninstall_prompt(
        &self,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> Option<impl IntoElement> {
        let prompt = self.uninstall_prompt.as_ref()?;
        let layout = LibraryLayout::for_viewport(viewport.width.into(), viewport.height.into());
        let theme = self.theme;
        let text_color = theme.control_text();
        let name = self.page_launcher_name();
        let what = match prompt.page {
            LibraryPage::Rockstar => {
                "This removes the Rockstar Games Launcher from the Steam prefix. Steam, and Rockstar games you own on Steam, stay as they are.".to_owned()
            }
            LibraryPage::Gog => "This removes the GOG prefix. Your GOG sign-in stays, so setting it up again skips Login.".to_owned(),
            LibraryPage::Epic => {
                "This removes the Epic prefixes and Legendary's sign-in, so the page starts over at Create Prefix.".to_owned()
            }
            _ => format!("This removes {name} and its Wine prefix."),
        };
        let checkbox = prompt.folder.clone().map(|folder| {
            let on = prompt.delete_games;
            div()
                .id("uninstall-delete-games")
                .flex()
                .items_start()
                .gap(px(layout.scaled(10.0)))
                .p(px(layout.scaled(10.0)))
                .rounded(px(layout.scaled(8.0)))
                .border_1()
                .border_color(rgba(if on { 0xd9534fb0 } else { 0xe7eaec33 }))
                .cursor_pointer()
                .on_click(cx.listener(|this, _, _, cx| {
                    if let Some(prompt) = this.uninstall_prompt.as_mut() {
                        prompt.delete_games = !prompt.delete_games;
                        cx.notify();
                    }
                }))
                .child(
                    div()
                        .flex_none()
                        .mt(px(2.0))
                        .size(px(layout.scaled(16.0)))
                        .flex()
                        .items_center()
                        .justify_center()
                        .rounded(px(4.0))
                        .border_1()
                        .border_color(rgb(if on { 0xd9534f } else { 0x96a1b2 }))
                        .when(on, |d| d.bg(rgb(0xd9534f)))
                        .text_size(px(layout.scaled(11.0)))
                        .text_color(rgb(0xffffff))
                        .children(on.then_some("✓")),
                )
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .flex()
                        .flex_col()
                        .gap(px(2.0))
                        .child(
                            div()
                                .text_size(px(layout.scaled(13.0)))
                                .text_color(rgb(text_color))
                                .child("Also delete the games folder and every game in it"),
                        )
                        .child(
                            div()
                                .text_size(px(layout.scaled(11.5)))
                                .text_color(rgb(0x96a1b2))
                                .child(folder),
                        ),
                )
        });
        let danger = prompt.delete_games;
        let panel = div()
            .id("uninstall-prompt")
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
                    .child(format!("Uninstall {name}?")),
            )
            .child(
                div()
                    .text_size(px(layout.scaled(12.5)))
                    .line_height(px(layout.scaled(18.0)))
                    .text_color(rgb(0xaeb3b2))
                    .child(what),
            )
            .children(checkbox)
            .child(
                div()
                    .flex()
                    .justify_end()
                    .gap(px(layout.scaled(8.0)))
                    .child(
                        div()
                            .id("uninstall-cancel")
                            .px(px(layout.scaled(14.0)))
                            .py(px(layout.scaled(7.0)))
                            .rounded(px(layout.scaled(7.0)))
                            .text_size(px(layout.scaled(13.0)))
                            .text_color(rgb(text_color))
                            .cursor_pointer()
                            .hover(move |style| style.bg(rgb(theme.menu_hover())))
                            .on_click(cx.listener(|this, _, _, cx| {
                                if !this.uninstall_prompt.as_ref().is_some_and(|p| p.busy) {
                                    this.uninstall_prompt = None;
                                    cx.notify();
                                }
                            }))
                            .child("Cancel"),
                    )
                    .child(
                        div()
                            .id("uninstall-confirm")
                            .px(px(layout.scaled(14.0)))
                            .py(px(layout.scaled(7.0)))
                            .rounded(px(layout.scaled(7.0)))
                            .bg(rgb(0xa52d2d))
                            .text_size(px(layout.scaled(13.0)))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(0xffffff))
                            .when(prompt.busy, |d| d.opacity(0.6))
                            .cursor_pointer()
                            .hover(|style| style.bg(rgb(0xbb3c3c)))
                            .on_click(cx.listener(|this, _, _, cx| this.run_uninstall(cx)))
                            .child(if prompt.busy {
                                "Uninstalling…"
                            } else if danger {
                                "Uninstall and delete games"
                            } else {
                                "Uninstall"
                            }),
                    ),
            );
        Some(
            div()
                .id("uninstall-prompt-scrim")
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
