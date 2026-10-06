//! Collection › Uninstalled: owned Steam games that aren't installed yet,
//! installed with steamcmd into one of the Wine Steam client's libraries.
//! steamcmd signs in once (password + Steam Guard through this sheet); it
//! caches its own login, so MetalSharp only keeps the account name.
use super::*;
use crate::library_model::LibGame;
use crate::live::{self, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) enum CollectionView {
    #[default]
    Installed,
    Uninstalled,
}

#[derive(Clone, Debug)]
pub(super) enum SteamcmdModal {
    SignIn {
        appid: u64,
        name: String,
        state: String,
        message: String,
    },
    Library {
        appid: u64,
        name: String,
        libraries: Option<Vec<Value>>,
    },
}

const MS: fn(u64) -> std::time::Duration = std::time::Duration::from_millis;

fn active(state: &str) -> bool {
    !matches!(state, "done" | "failed" | "cancelled")
}

fn format_size(bytes: u64) -> String {
    let gb = bytes as f64 / 1_000_000_000.0;
    if gb >= 1.0 {
        format!("{gb:.1} GB")
    } else {
        format!("{:.0} MB", bytes as f64 / 1_000_000.0)
    }
}

/// Tiny top-left overlay: confirmed playable, has a launch rule, or blocked
/// by kernel-level anti-cheat (see `crate::compat`).
fn compat_badge(game: &LibGame) -> Option<gpui::Div> {
    use crate::compat::Compat;
    let (icon, label, color, background) = match crate::compat::compat(game.appid, &game.name)? {
        Compat::Playable => ("✓", "Playable", 0x7ee08a, 0x10301ae6),
        Compat::MayRun => ("–", "May Run", 0xf2d36b, 0x33290ae6),
        Compat::AntiCheat => ("✕", "Anti-Cheat", 0xff8a80, 0x3a1210e6),
    };
    Some(
        div()
            .absolute()
            .top(px(8.0))
            .left(px(8.0))
            .h(px(20.0))
            .px(px(7.0))
            .flex()
            .items_center()
            .gap(px(4.0))
            .rounded(px(5.0))
            .border_1()
            .border_color(rgba((color << 8) | 0x66))
            .bg(rgba(background))
            .text_size(px(10.0))
            .font_weight(FontWeight::BOLD)
            .text_color(rgb(color))
            .child(icon)
            .child(label),
    )
}

impl MetalSharpApp {
    pub(super) fn ensure_steamcmd_inputs(&mut self, cx: &mut Context<Self>) {
        if self.steamcmd_inputs.is_some() {
            return;
        }
        let make = |placeholder: &str, secret: bool, cx: &mut Context<Self>| {
            cx.new(|cx| {
                let mut input = crate::search_input::SearchInput::new(cx);
                input.placeholder = placeholder.to_owned().into();
                input.secret = secret;
                input.text_color = 0xeceae3;
                input.placeholder_color = 0x838987;
                input
            })
        };
        let username = make("Steam account name", false, cx);
        let password = make("Password", true, cx);
        let code = make("Steam Guard code", false, cx);
        self.steamcmd_inputs = Some([username, password, code]);
    }

    /// Owned Steam games without a local install, sorted by name.
    pub(super) fn uninstalled_games(&self) -> Vec<LibGame> {
        let query = self.search_query.trim().to_lowercase();
        let mut games: Vec<LibGame> = self
            .live
            .library
            .iter()
            .filter(|g| !g.installed && !g.is_ubisoft() && g.appid != 0)
            .filter(|g| query.is_empty() || g.name.to_lowercase().contains(&query))
            .cloned()
            .collect();
        games.sort_by_key(|g| g.name.to_lowercase());
        games
    }

    pub(super) fn show_uninstalled(&mut self, cx: &mut Context<Self>) {
        self.collection_view = CollectionView::Uninstalled;
        for game in self.uninstalled_games() {
            self.request_art(&game, cx);
        }
        self.poll_steamcmd_installs(cx);
        cx.notify();
    }

    pub(super) fn begin_steamcmd_install(
        &mut self,
        appid: u64,
        name: String,
        cx: &mut Context<Self>,
    ) {
        live::call(
            cx,
            "GET",
            "/steamcmd/status",
            None,
            MS(10_000),
            move |this, r, cx| {
                let Some(status) = r.filter(is_ok) else {
                    toast::error(cx, "MetalSharp backend is not responding");
                    return;
                };
                if status.get("signedIn").and_then(Value::as_bool) == Some(true) {
                    this.open_library_picker(appid, name, cx);
                } else {
                    this.open_steamcmd_sign_in(appid, name, &status, cx);
                }
            },
        );
    }

    fn open_steamcmd_sign_in(
        &mut self,
        appid: u64,
        name: String,
        status: &Value,
        cx: &mut Context<Self>,
    ) {
        self.ensure_steamcmd_inputs(cx);
        if let Some([username, password, code]) = &self.steamcmd_inputs {
            let saved = status
                .get("username")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned();
            username.update(cx, |input, cx| {
                if input.content.is_empty() && !saved.is_empty() {
                    input.content = saved.into();
                }
                cx.notify();
            });
            password.update(cx, |input, cx| input.clear(cx));
            code.update(cx, |input, cx| input.clear(cx));
        }
        self.steamcmd_modal = Some(SteamcmdModal::SignIn {
            appid,
            name,
            state: "idle".into(),
            message: String::new(),
        });
        cx.notify();
    }

    fn submit_steamcmd_sign_in(&mut self, cx: &mut Context<Self>) {
        let Some([username, password, _]) = self.steamcmd_inputs.clone() else {
            return;
        };
        let user = username.read(cx).content.trim().to_owned();
        let secret = password.read(cx).content.to_string();
        if user.is_empty() || secret.is_empty() {
            toast::error(cx, "Enter your Steam account name and password");
            return;
        }
        password.update(cx, |input, cx| input.clear(cx));
        if let Some(SteamcmdModal::SignIn { state, message, .. }) = &mut self.steamcmd_modal {
            *state = "preparing".into();
            *message = "Preparing steamcmd...".into();
        }
        cx.notify();
        live::call(
            cx,
            "POST",
            "/steamcmd/login",
            Some(json!({"username": user, "password": secret})),
            MS(30_000),
            |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.poll_steamcmd_login(cx);
                } else {
                    let error =
                        error_text(r.as_ref()).unwrap_or_else(|| "Could not start sign-in".into());
                    if let Some(SteamcmdModal::SignIn { state, message, .. }) =
                        &mut this.steamcmd_modal
                    {
                        *state = "failed".into();
                        *message = error;
                    }
                    cx.notify();
                }
            },
        );
    }

    fn submit_steamcmd_code(&mut self, cx: &mut Context<Self>) {
        let Some([_, _, code]) = self.steamcmd_inputs.clone() else {
            return;
        };
        let value = code.read(cx).content.trim().to_owned();
        if value.is_empty() {
            return;
        }
        code.update(cx, |input, cx| input.clear(cx));
        live::call(
            cx,
            "POST",
            "/steamcmd/login-code",
            Some(json!({"code": value})),
            MS(10_000),
            |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Could not send the code".into()),
                    );
                } else if let Some(SteamcmdModal::SignIn { state, message, .. }) =
                    &mut this.steamcmd_modal
                {
                    *state = "signing_in".into();
                    *message = "Checking the code...".into();
                }
                cx.notify();
            },
        );
    }

    fn poll_steamcmd_login(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(1000)).await;
            let _ = this.update(cx, |this, cx| {
                if !matches!(this.steamcmd_modal, Some(SteamcmdModal::SignIn { .. })) {
                    return;
                }
                live::call(
                    cx,
                    "GET",
                    "/steamcmd/status",
                    None,
                    MS(10_000),
                    |this, r, cx| {
                        let Some(status) = r.filter(is_ok) else {
                            this.poll_steamcmd_login(cx);
                            return;
                        };
                        let login = status.get("login").cloned().unwrap_or(Value::Null);
                        let next = login
                            .get("state")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        let text = login
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        let Some(SteamcmdModal::SignIn {
                            appid,
                            name,
                            state,
                            message,
                        }) = &mut this.steamcmd_modal
                        else {
                            return;
                        };
                        *state = next.clone();
                        *message = text;
                        let (appid, name) = (*appid, name.clone());
                        match next.as_str() {
                            "signed_in" => {
                                toast::success(cx, "Signed in to Steam");
                                this.open_library_picker(appid, name, cx);
                            }
                            "failed" | "cancelled" | "idle" => {}
                            _ => this.poll_steamcmd_login(cx),
                        }
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    fn close_steamcmd_modal(&mut self, cx: &mut Context<Self>) {
        if let Some(SteamcmdModal::SignIn { state, .. }) = &self.steamcmd_modal {
            if matches!(
                state.as_str(),
                "preparing" | "signing_in" | "needs_code" | "needs_confirmation"
            ) {
                live::call(
                    cx,
                    "POST",
                    "/steamcmd/login-cancel",
                    None,
                    MS(5_000),
                    |_, _, _| {},
                );
            }
        }
        if let Some([_, password, code]) = &self.steamcmd_inputs {
            password.update(cx, |input, cx| input.clear(cx));
            code.update(cx, |input, cx| input.clear(cx));
        }
        self.steamcmd_modal = None;
        cx.notify();
    }

    fn open_library_picker(&mut self, appid: u64, name: String, cx: &mut Context<Self>) {
        self.steamcmd_modal = Some(SteamcmdModal::Library {
            appid,
            name,
            libraries: None,
        });
        cx.notify();
        live::call(
            cx,
            "GET",
            "/steamcmd/libraries",
            None,
            MS(10_000),
            |this, r, cx| {
                let libraries = r
                    .filter(is_ok)
                    .and_then(|r| r.get("libraries").and_then(Value::as_array).cloned())
                    .unwrap_or_default();
                if let Some(SteamcmdModal::Library {
                    libraries: slot, ..
                }) = &mut this.steamcmd_modal
                {
                    *slot = Some(libraries);
                }
                cx.notify();
            },
        );
    }

    fn start_steamcmd_install(
        &mut self,
        appid: u64,
        name: String,
        library: String,
        cx: &mut Context<Self>,
    ) {
        self.steamcmd_modal = None;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/steamcmd/install",
            Some(json!({"appid": appid, "name": name, "library": library})),
            MS(10_000),
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    toast::info(cx, format!("Installing {name} with steamcmd..."));
                    this.poll_steamcmd_installs(cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not install {name}")),
                    );
                }
            },
        );
    }

    fn cancel_steamcmd_install(&mut self, appid: u64, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/steamcmd/cancel",
            Some(json!({"appid": appid})),
            MS(10_000),
            |this, _, cx| this.poll_steamcmd_installs(cx),
        );
    }

    /// Polls once a second while any install is queued or running.
    pub(super) fn poll_steamcmd_installs(&mut self, cx: &mut Context<Self>) {
        if self.live.steamcmd_polling {
            return;
        }
        self.live.steamcmd_polling = true;
        self.fetch_steamcmd_installs(cx);
    }

    fn fetch_steamcmd_installs(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/steamcmd/installs",
            None,
            MS(10_000),
            |this, r, cx| {
                let installs = r
                    .filter(is_ok)
                    .and_then(|r| r.get("installs").and_then(Value::as_array).cloned())
                    .unwrap_or_default();
                let mut finished = false;
                let mut any_active = false;
                for install in installs {
                    let Some(appid) = install.get("appid").and_then(Value::as_u64) else {
                        continue;
                    };
                    let state = install
                        .get("state")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_owned();
                    let previous = this
                        .live
                        .steamcmd_installs
                        .get(&appid)
                        .and_then(|p| p.get("state").and_then(Value::as_str))
                        .map(str::to_owned);
                    let name = install
                        .get("name")
                        .and_then(Value::as_str)
                        .unwrap_or("Game")
                        .to_owned();
                    let was_active = previous.as_deref().is_some_and(active);
                    if was_active && state == "done" {
                        toast::success(cx, format!("{name} installed"));
                        finished = true;
                    } else if was_active && state == "failed" {
                        let message = install
                            .get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_owned();
                        toast::error(cx, format!("{name}: {message}"));
                        if install.get("needsLogin").and_then(Value::as_bool) == Some(true) {
                            this.open_steamcmd_sign_in(appid, name.clone(), &Value::Null, cx);
                        }
                    }
                    any_active |= active(&state);
                    this.live.steamcmd_installs.insert(appid, install);
                }
                if finished {
                    this.load_library(true, cx);
                }
                cx.notify();
                if !any_active {
                    this.live.steamcmd_polling = false;
                    return;
                }
                cx.spawn(async move |this, cx| {
                    cx.background_executor().timer(MS(1000)).await;
                    let _ = this.update(cx, |this, cx| this.fetch_steamcmd_installs(cx));
                })
                .detach();
            },
        );
    }

    pub(super) fn render_uninstalled_card(
        &mut self,
        game: &LibGame,
        index: usize,
        card_width: f32,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let accent = self.theme.accent();
        let control_bg = self.theme.control_bg();
        let art = self.art_path(game, false, cx);
        let appid = game.appid;
        let name = game.name.clone();
        let job = self.live.steamcmd_installs.get(&appid).cloned();
        let state = job
            .as_ref()
            .and_then(|j| j.get("state").and_then(Value::as_str))
            .unwrap_or("")
            .to_owned();
        let running = active(&state) && !state.is_empty();
        let progress = job
            .as_ref()
            .and_then(|j| j.get("progress").and_then(Value::as_f64))
            .unwrap_or(0.0)
            .clamp(0.0, 100.0);
        let detail = job.as_ref().map(|j| {
            let message = j.get("message").and_then(Value::as_str).unwrap_or("");
            let done = j
                .get("downloadedBytes")
                .and_then(Value::as_u64)
                .unwrap_or(0);
            let total = j.get("totalBytes").and_then(Value::as_u64).unwrap_or(0);
            match state.as_str() {
                "downloading" | "verifying" if total > 0 => format!(
                    "{} {:.0}% · {} of {}",
                    if state == "verifying" {
                        "Verifying"
                    } else {
                        "Downloading"
                    },
                    progress,
                    format_size(done),
                    format_size(total)
                ),
                "queued" => "Queued".to_owned(),
                _ => message.to_owned(),
            }
        });
        let button = |id: (&'static str, usize), label: &'static str, primary: bool| {
            div()
                .id(id)
                .h(px(28.0))
                .px(px(11.0))
                .flex()
                .items_center()
                .rounded(px(6.0))
                .border_1()
                .border_color(rgb(if primary { accent } else { 0x4a4e50 }))
                .bg(rgb(if primary { accent } else { 0x23272a }))
                .text_size(px(11.0))
                .font_weight(FontWeight::BOLD)
                .text_color(rgb(if primary { control_bg } else { 0xe1e3e2 }))
                .cursor_pointer()
                .child(label)
        };
        let action = if running {
            button(("uninstalled-cancel", index), "Cancel", false).on_click(
                cx.listener(move |this, _, _, cx| this.cancel_steamcmd_install(appid, cx)),
            )
        } else {
            let install_name = name.clone();
            button(("uninstalled-install", index), "⤓ Install", true).on_click(cx.listener(
                move |this, _, _, cx| this.begin_steamcmd_install(appid, install_name.clone(), cx),
            ))
        };
        div()
            .id(("uninstalled-card", index))
            .relative()
            .w(px(card_width))
            .h(px(card_width * 1.36))
            .flex_none()
            .overflow_hidden()
            .rounded(px(8.0))
            .border_1()
            .border_color(rgba(0xe0e2e03b))
            .bg(rgb(0x242729))
            .shadow_lg()
            .child(match art {
                Some(art) => crate::artwork::art_img(art)
                    .w_full()
                    .h_full()
                    .object_fit(ObjectFit::Cover)
                    .opacity(if running { 1.0 } else { 0.72 })
                    .into_any_element(),
                None => div()
                    .size_full()
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(
                        img(asset_path("metalsharp-logo.png"))
                            .size(px(card_width * 0.4))
                            .object_fit(ObjectFit::Contain),
                    )
                    .into_any_element(),
            })
            .child(
                div()
                    .absolute()
                    .top(px(0.0))
                    .right(px(0.0))
                    .bottom(px(0.0))
                    .left(px(0.0))
                    .bg(linear_gradient(
                        180.0,
                        linear_color_stop(rgba(0x00000000), 0.34),
                        linear_color_stop(rgba(0x050708e6), 1.0),
                    )),
            )
            .children(compat_badge(game))
            .child(
                div()
                    .absolute()
                    .left(px(12.0))
                    .right(px(12.0))
                    .bottom(px(12.0))
                    .flex()
                    .flex_col()
                    .gap(px(7.0))
                    .child(
                        div()
                            .text_size(px(14.0))
                            .font_weight(FontWeight::BOLD)
                            .text_color(rgb(0xf0f0ed))
                            .child(name),
                    )
                    .children(detail.filter(|d| !d.is_empty()).map(|detail| {
                        div()
                            .text_size(px(10.5))
                            .text_color(rgb(if state == "failed" {
                                0xf0a0a0
                            } else {
                                0xd8dad9
                            }))
                            .child(detail)
                    }))
                    .children(running.then(|| {
                        div()
                            .h(px(4.0))
                            .w_full()
                            .rounded(px(2.0))
                            .bg(rgba(0xffffff26))
                            .child(
                                div()
                                    .h_full()
                                    .w(gpui::relative((progress / 100.0) as f32))
                                    .rounded(px(2.0))
                                    .bg(rgb(accent)),
                            )
                    }))
                    .child(div().flex().child(action)),
            )
    }

    pub(super) fn render_steamcmd_modal(
        &mut self,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let modal = self.steamcmd_modal.clone();
        let button = |id: &'static str, label: String, primary: bool| {
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
        let field = |label: &'static str, input: gpui::Entity<crate::search_input::SearchInput>| {
            div()
                .flex()
                .flex_col()
                .gap(px(6.0))
                .child(
                    div()
                        .text_size(px(12.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(rgb(0xcfd2cf))
                        .child(label),
                )
                .child(
                    div()
                        .h(px(36.0))
                        .flex()
                        .items_center()
                        .rounded(px(8.0))
                        .border_1()
                        .border_color(rgb(0x3b3e40))
                        .bg(rgb(0x191c1f))
                        .px(px(12.0))
                        .text_size(px(13.0))
                        .child(div().w_full().child(input)),
                )
        };
        let mut body = div().flex().flex_col().gap(px(14.0));
        match modal {
            Some(SteamcmdModal::SignIn {
                name,
                state,
                message,
                ..
            }) => {
                let inputs = self.steamcmd_inputs.clone();
                let busy = matches!(state.as_str(), "preparing" | "signing_in");
                let wants_code = matches!(state.as_str(), "needs_code" | "needs_confirmation");
                body = body
                    .child(
                        div()
                            .text_size(px(17.0))
                            .font_weight(FontWeight::BOLD)
                            .child("Sign in to Steam"),
                    )
                    .child(
                        div()
                            .text_size(px(12.5))
                            .line_height(px(18.0))
                            .text_color(rgb(0xaeb3b2))
                            .child(format!(
                                "steamcmd needs your Steam account once to download {name}. MetalSharp doesn't store your password; steamcmd keeps its own sign-in token."
                            )),
                    );
                if let Some([username, password, code]) = inputs {
                    if wants_code {
                        body = body.child(field("Steam Guard code", code));
                    } else {
                        body = body
                            .child(field("Account name", username))
                            .child(field("Password", password));
                    }
                }
                if !message.is_empty() {
                    body = body.child(
                        div()
                            .text_size(px(12.0))
                            .line_height(px(17.0))
                            .text_color(rgb(if state == "failed" {
                                0xf0a0a0
                            } else {
                                0xffd47f
                            }))
                            .child(message),
                    );
                }
                let primary = if wants_code {
                    button("steamcmd-code-submit", "Submit code".into(), true)
                        .on_click(cx.listener(|this, _, _, cx| this.submit_steamcmd_code(cx)))
                } else {
                    button(
                        "steamcmd-sign-in",
                        if busy { "Signing in..." } else { "Sign in" }.into(),
                        true,
                    )
                    .opacity(if busy { 0.6 } else { 1.0 })
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if !busy {
                            this.submit_steamcmd_sign_in(cx);
                        }
                    }))
                };
                body =
                    body.child(
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.0))
                            .child(button("steamcmd-cancel", "Cancel".into(), false).on_click(
                                cx.listener(|this, _, _, cx| this.close_steamcmd_modal(cx)),
                            ))
                            .child(primary),
                    );
            }
            Some(SteamcmdModal::Library {
                appid,
                name,
                libraries,
            }) => {
                body = body
                    .child(
                        div()
                            .text_size(px(17.0))
                            .font_weight(FontWeight::BOLD)
                            .child(format!("Install {name}")),
                    )
                    .child(
                        div()
                            .text_size(px(12.5))
                            .text_color(rgb(0xaeb3b2))
                            .child("Choose the Steam library to install into."),
                    );
                match libraries {
                    None => {
                        body = body.child(
                            div()
                                .text_size(px(12.5))
                                .child("Loading Steam libraries..."),
                        );
                    }
                    Some(libraries) if libraries.is_empty() => {
                        body = body
                            .child(div().text_size(px(12.5)).text_color(rgb(0xf0a0a0)).child(
                            "No Steam libraries found. Start Steam once so it creates its library.",
                        ));
                    }
                    Some(libraries) => {
                        for (index, library) in libraries.into_iter().enumerate() {
                            let path = library
                                .get("path")
                                .and_then(Value::as_str)
                                .unwrap_or("")
                                .to_owned();
                            let internal =
                                library.get("internal").and_then(Value::as_bool) == Some(true);
                            let available =
                                library.get("available").and_then(Value::as_bool) != Some(false);
                            let free = library.get("freeBytes").and_then(Value::as_u64);
                            let title = if internal {
                                "Internal Steam library".to_owned()
                            } else {
                                format!(
                                    "External Steam library — {}",
                                    path.trim_start_matches("/Volumes/")
                                )
                            };
                            let subtitle = match (available, free) {
                                (false, _) => "Not connected".to_owned(),
                                (true, Some(free)) => {
                                    format!("{} free · {path}", format_size(free))
                                }
                                (true, None) => path.clone(),
                            };
                            let install_name = name.clone();
                            body = body.child(
                                div()
                                    .id(("steamcmd-library", index))
                                    .p(px(12.0))
                                    .rounded(px(8.0))
                                    .border_1()
                                    .border_color(rgba(0xffffff26))
                                    .bg(rgb(0x1d2124))
                                    .flex()
                                    .flex_col()
                                    .gap(px(4.0))
                                    .opacity(if available { 1.0 } else { 0.5 })
                                    .when(available, |row| {
                                        row.cursor_pointer()
                                            .hover(|s| s.bg(rgb(0x262b2e)))
                                            .on_click(cx.listener(move |this, _, _, cx| {
                                                this.start_steamcmd_install(
                                                    appid,
                                                    install_name.clone(),
                                                    path.clone(),
                                                    cx,
                                                )
                                            }))
                                    })
                                    .child(
                                        div()
                                            .text_size(px(13.5))
                                            .font_weight(FontWeight::SEMIBOLD)
                                            .child(title),
                                    )
                                    .child(
                                        div()
                                            .text_size(px(11.5))
                                            .text_color(rgb(0x9ea3a2))
                                            .child(subtitle),
                                    ),
                            );
                        }
                    }
                }
                body = body.child(
                    div().flex().justify_end().child(
                        button("steamcmd-library-cancel", "Cancel".into(), false)
                            .on_click(cx.listener(|this, _, _, cx| this.close_steamcmd_modal(cx))),
                    ),
                );
            }
            None => {}
        }
        div()
            .id("steamcmd-backdrop")
            .occlude()
            .absolute()
            .top_0()
            .left_0()
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .bg(rgba(0x000000a0))
            .child(
                div()
                    .id("steamcmd-modal")
                    .occlude()
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .w(px(460.0))
                    .p(px(22.0))
                    .rounded(px(12.0))
                    .border_1()
                    .border_color(rgba(0xffffff1f))
                    .bg(rgb(0x14171a))
                    .text_color(rgb(0xe6e8e7))
                    .child(body),
            )
    }
}
