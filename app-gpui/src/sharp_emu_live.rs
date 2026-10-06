//! Live emulator sources (PCSX2, RPCS3, shadPS4, SharpEmu) from `SharpView.vue`,
//! rendered with the approved emulator overview/sidebar/card styling.
use super::*;
use crate::live::{self, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};
use std::time::Duration;

const MS: fn(u64) -> Duration = Duration::from_millis;

#[derive(Default, Clone)]
pub(super) struct EmuState {
    pub status: Option<Value>,
    pub games: Vec<Value>,
    pub roots: Vec<String>,
    pub update: Option<Value>,
    pub progress: Option<Value>,
    pub settings: Option<Value>,
    pub updating: bool,
    pub checking: bool,
    pub network_opt_in: bool,
    pub settings_menu: Option<&'static str>,
}

#[derive(Clone, Debug, PartialEq)]
pub(super) enum EmuConfirm {
    Install(SharpSource, String),
    Rollback(SharpSource),
    RemoveRoot(SharpSource, String),
    RemoveRuntime(SharpSource),
    Pcsx2Bios(String),
    NetworkLaunch(String, String),
}

fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}
fn b(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}
fn format_bytes(bytes: u64) -> String {
    if bytes < 1024 * 1024 {
        format!("{:.1} KB", bytes as f64 / 1024.0)
    } else if bytes < 1024 * 1024 * 1024 {
        format!("{:.1} MB", bytes as f64 / (1024.0 * 1024.0))
    } else {
        format!("{:.1} GB", bytes as f64 / (1024.0 * 1024.0 * 1024.0))
    }
}

pub(super) fn emu_index(source: SharpSource) -> Option<usize> {
    match source {
        SharpSource::Pcsx2 => Some(0),
        SharpSource::Rpcs3 => Some(1),
        SharpSource::ShadPs4 => Some(2),
        SharpSource::SharpEmu => Some(3),
        _ => None,
    }
}

fn prefix(source: SharpSource) -> String {
    format!("/sharp-library/{}", source.id())
}

impl SharpPreview {
    fn emu(&self, source: SharpSource) -> EmuState {
        emu_index(source)
            .and_then(|i| self.live.as_ref().map(|l| l.emu[i].clone()))
            .unwrap_or_default()
    }

    fn emu_mut(&mut self, source: SharpSource) -> &mut EmuState {
        let i = emu_index(source).expect("emulator source");
        &mut self.live.as_mut().expect("live").emu[i]
    }

    /// `refreshPcsx2` / `refreshRpcs3` / `refreshShadps4` / `refreshSharpemu`.
    pub(super) fn emu_refresh(&mut self, source: SharpSource, show: bool, cx: &mut Context<Self>) {
        let p = prefix(source);
        live::call(
            cx,
            "GET",
            format!("{p}/status"),
            None,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.emu_mut(source).status = Some(r);
                    cx.notify();
                }
            },
        );
        let (method, path, body) = if show && source == SharpSource::Pcsx2 {
            ("POST", format!("{p}/scan"), Some(json!({})))
        } else {
            ("GET", format!("{p}/games"), None)
        };
        live::call(
            cx,
            method,
            path,
            body,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.apply_emu_games(source, &r);
                    this.emu_request_covers(source, cx);
                    if show {
                        let count = this.emu(source).games.len();
                        toast::success(
                            cx,
                            format!(
                                "Found {count} {} game{}",
                                source.title(),
                                if count == 1 { "" } else { "s" }
                            ),
                        );
                    }
                    cx.notify();
                }
            },
        );
        if source == SharpSource::Pcsx2 {
            live::call(
                cx,
                "GET",
                format!("{p}/settings"),
                None,
                live::DEFAULT_TIMEOUT,
                move |this, r, cx| {
                    if let Some(r) = r.filter(is_ok) {
                        this.emu_mut(source).settings = Some(r);
                        cx.notify();
                    }
                },
            );
        }
    }

    fn apply_emu_games(&mut self, source: SharpSource, r: &Value) {
        let mut games = r
            .get("games")
            .and_then(Value::as_array)
            .cloned()
            .unwrap_or_default();
        games.sort_by(|a, b| {
            s(a, "title")
                .to_lowercase()
                .cmp(&s(b, "title").to_lowercase())
        });
        let roots = r
            .get("roots")
            .and_then(Value::as_array)
            .map(|roots| {
                roots
                    .iter()
                    .filter_map(|v| v.as_str().map(str::to_owned))
                    .collect()
            })
            .unwrap_or_default();
        let emu = self.emu_mut(source);
        emu.games = games;
        emu.roots = roots;
    }

    pub(super) fn emu_request_covers(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        let Some(cache) = crate::artwork::cache(cx) else {
            return;
        };
        let p = prefix(source);
        let requests: Vec<(String, Vec<crate::artwork::Candidate>)> = self
            .emu(source)
            .games
            .iter()
            .filter(|g| b(g, "hasArtwork"))
            .map(|g| {
                let id = s(g, "id");
                let encoded: String = url::form_urlencoded::byte_serialize(id.as_bytes()).collect();
                (
                    format!("sharp-{}-{id}", source.id()),
                    vec![crate::artwork::Candidate::Backend(format!(
                        "{p}/cover?id={encoded}"
                    ))],
                )
            })
            .collect();
        cache.update(cx, |cache, cx| {
            for (key, candidates) in requests {
                cache.resolve(key, candidates, cx);
            }
        });
    }

    /// `check*Update`.
    pub(super) fn emu_check_update(
        &mut self,
        source: SharpSource,
        show: bool,
        force: bool,
        then: impl FnOnce(&mut Self, Option<Value>, &mut Context<Self>) + 'static,
        cx: &mut Context<Self>,
    ) {
        self.emu_mut(source).checking = true;
        cx.notify();
        let p = prefix(source);
        let (method, path, body) = if force {
            ("POST", format!("{p}/update/refresh"), Some(json!({})))
        } else {
            ("GET", format!("{p}/update/check"), None)
        };
        live::call(
            cx,
            method,
            path,
            body,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                this.emu_mut(source).checking = false;
                let Some(r) = r.filter(is_ok) else {
                    if show {
                        toast::error(cx, format!("Could not check {} updates", source.title()));
                    }
                    cx.notify();
                    then(this, None, cx);
                    return;
                };
                this.emu_mut(source).update = Some(r.clone());
                if show {
                    let installed = this
                        .emu(source)
                        .status
                        .as_ref()
                        .is_some_and(|s| b(s, "installed"));
                    let latest = s(&r, "latestVersion").to_owned();
                    if b(&r, "available") {
                        toast::success(
                            cx,
                            format!(
                                "{} {latest} is available",
                                if installed {
                                    format!("{} update", source.title())
                                } else {
                                    source.title().to_owned()
                                }
                            ),
                        );
                    } else {
                        toast::info(
                            cx,
                            match s(&r, "suppressed") {
                                "pinned" => {
                                    format!("The current {} version is pinned", source.title())
                                }
                                "skipped" => {
                                    format!("The latest {} version is skipped", source.title())
                                }
                                _ => format!("{} is up to date", source.title()),
                            },
                        );
                    }
                }
                cx.notify();
                then(this, Some(r), cx);
            },
        );
    }

    /// `installOrUpdate*` (header "Check X").
    pub(super) fn emu_install_or_update(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        let emu = self.emu(source);
        if emu.updating || emu.checking {
            return;
        }
        if emu
            .status
            .as_ref()
            .is_some_and(|s| s.get("supported").and_then(Value::as_bool) == Some(false))
        {
            toast::error(
                cx,
                format!("{} is not supported on this Mac", source.title()),
            );
            return;
        }
        let proceed = move |this: &mut Self, release: Option<Value>, cx: &mut Context<Self>| {
            let Some(release) = release else { return };
            let installed = this
                .emu(source)
                .status
                .as_ref()
                .is_some_and(|s| b(s, "installed"));
            if installed && !b(&release, "available") {
                toast::info(cx, format!("{} is already up to date", source.title()));
                return;
            }
            let action = if installed { "Update" } else { "Install" };
            let size = release
                .get("downloadSize")
                .and_then(Value::as_u64)
                .unwrap_or(0);
            let text = format!(
                "{action} official {} {} ({})?\n\nMetalSharp verifies the official release before activation. BIOS, firmware, saves, states, settings, profiles, caches, and games are preserved.",
                source.title(),
                s(&release, "latestVersion"),
                format_bytes(size)
            );
            this.live.as_mut().unwrap().emu_confirm = Some(EmuConfirm::Install(source, text));
            cx.notify();
        };
        match emu.update {
            Some(update) => proceed(self, Some(update), cx),
            None => self.emu_check_update(source, false, false, proceed, cx),
        }
    }

    fn emu_start_install(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        self.emu_mut(source).updating = true;
        cx.notify();
        let p = prefix(source);
        live::call(
            cx,
            "POST",
            format!("{p}/update/install"),
            Some(json!({})),
            MS(35_000),
            move |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    this.emu_mut(source).updating = false;
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| {
                            format!("Could not start the {} install", source.title())
                        }),
                    );
                    cx.notify();
                    return;
                }
                this.emu_mut(source).progress = r;
                this.emu_poll_progress(source, cx);
            },
        );
    }

    fn emu_poll_progress(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            cx.background_executor().timer(MS(650)).await;
            let _ = this.update(cx, move |_, cx| {
                let p = prefix(source);
                live::call(
                    cx,
                    "GET",
                    format!("{p}/update/progress"),
                    None,
                    live::DEFAULT_TIMEOUT,
                    move |this, r, cx| {
                        let Some(progress) = r.filter(is_ok) else {
                            this.emu_poll_progress(source, cx);
                            return;
                        };
                        this.emu_mut(source).progress = Some(progress.clone());
                        cx.notify();
                        let status = s(&progress, "status");
                        if !b(&progress, "running") && (status == "completed" || status == "failed")
                        {
                            this.emu_mut(source).updating = false;
                            if status == "completed" {
                                toast::success(cx, format!("{} is ready", source.title()));
                            } else {
                                toast::error(
                                    cx,
                                    progress
                                        .get("error")
                                        .and_then(Value::as_str)
                                        .filter(|e| !e.is_empty())
                                        .map(str::to_owned)
                                        .unwrap_or_else(|| {
                                            format!("{} update failed", source.title())
                                        }),
                                );
                            }
                            this.emu_refresh(source, false, cx);
                            this.emu_check_update(source, false, false, |_, _, _| {}, cx);
                        } else {
                            this.emu_poll_progress(source, cx);
                        }
                    },
                );
            });
        })
        .detach();
    }

    pub(super) fn emu_policy(
        &mut self,
        source: SharpSource,
        action: &'static str,
        cx: &mut Context<Self>,
    ) {
        let tag = self
            .emu(source)
            .update
            .as_ref()
            .map(|u| s(u, "latestTag").to_owned());
        let p = prefix(source);
        live::call(
            cx,
            "POST",
            format!("{p}/{action}"),
            Some(json!({"tag": tag})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.emu_mut(source).update = r;
                    toast::success(
                        cx,
                        match action {
                            "pin-current" => format!("Current {} version pinned", source.title()),
                            "unpin" => format!("{} version unpinned", source.title()),
                            "skip-update" => format!("{} update skipped", source.title()),
                            _ => format!("Skipped {} update cleared", source.title()),
                        },
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| {
                            format!("Could not save {} update preference", source.title())
                        }),
                    );
                }
                cx.notify();
            },
        );
    }

    fn emu_rollback(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        let p = prefix(source);
        live::call(
            cx,
            "POST",
            format!("{p}/update/rollback"),
            Some(json!({})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.emu_mut(source).status = r;
                    toast::success(
                        cx,
                        format!(
                            "{} runtime rolled back; user data was preserved",
                            source.title()
                        ),
                    );
                    this.emu_refresh(source, false, cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("{} rollback failed", source.title())),
                    );
                }
            },
        );
    }

    pub(super) fn emu_add_root(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        // PCSX2 accepts a disc image or a folder; the others pick folders.
        let pick_file = source == SharpSource::Pcsx2;
        cx.spawn(async move |this, cx| {
            let picked = if pick_file {
                rfd::AsyncFileDialog::new()
                    .set_title("Choose a PlayStation 2 disc image or game folder")
                    .add_filter("PS2 disc images", &["iso", "chd", "cso", "zso", "gz", "bin", "img", "mdf", "nrg", "dump", "elf"])
                    .pick_file()
                    .await
            } else {
                rfd::AsyncFileDialog::new().set_title(format!("Choose a folder containing {} games", source.title())).pick_folder().await
            };
            let Some(picked) = picked else { return };
            let path = picked.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                let p = prefix(source);
                live::call(cx, "POST", format!("{p}/add-root"), Some(json!({"path": path})), live::DEFAULT_TIMEOUT, move |this, r, cx| {
                    if let Some(r) = r.filter(is_ok) {
                        this.apply_emu_games(source, &r);
                        this.emu_request_covers(source, cx);
                        toast::success(
                            cx,
                            if source == SharpSource::Pcsx2 {
                                "PCSX2 game location added. External game files remain in place.".to_owned()
                            } else {
                                format!("{} game folder added", source.title())
                            },
                        );
                        this.emu_refresh(source, false, cx);
                    } else {
                        toast::error(cx, format!("Could not add the {} game folder", source.title()));
                    }
                    cx.notify();
                });
            });
        })
        .detach();
    }

    fn emu_remove_root(&mut self, source: SharpSource, path: String, cx: &mut Context<Self>) {
        let p = prefix(source);
        live::call(
            cx,
            "POST",
            format!("{p}/remove-root"),
            Some(json!({"path": path})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                    this.apply_emu_games(source, r);
                    toast::success(
                        cx,
                        format!(
                            "{} folder reference removed; game files were preserved",
                            source.title()
                        ),
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| {
                            format!("Could not remove the {} folder", source.title())
                        }),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn emu_launch(&mut self, source: SharpSource, game: Value, cx: &mut Context<Self>) {
        let id = s(&game, "id").to_owned();
        let title = s(&game, "title").to_owned();
        if source == SharpSource::SharpEmu && self.emu(source).network_opt_in {
            self.live.as_mut().unwrap().emu_confirm = Some(EmuConfirm::NetworkLaunch(id, title));
            cx.notify();
            return;
        }
        self.emu_launch_now(source, id, title, false, cx);
    }

    fn emu_launch_now(
        &mut self,
        source: SharpSource,
        id: String,
        title: String,
        network: bool,
        cx: &mut Context<Self>,
    ) {
        let p = prefix(source);
        let body = if source == SharpSource::SharpEmu {
            json!({"id": id, "fullscreen": false, "allowNetwork": network})
        } else {
            json!({"id": id, "fullscreen": true})
        };
        live::call(
            cx,
            "POST",
            format!("{p}/launch"),
            Some(body),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(game) = this
                        .emu_mut(source)
                        .games
                        .iter_mut()
                        .find(|g| s(g, "id") == id)
                    {
                        game["running"] = json!(true);
                    }
                    if source == SharpSource::Pcsx2 {
                        toast::success(cx, format!("{title} started in PCSX2"));
                        this.emu_refresh(source, false, cx);
                    }
                    crate::launch_overlay::hint_later(title.clone(), cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not launch {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn emu_stop(
        &mut self,
        source: SharpSource,
        game: Option<Value>,
        cx: &mut Context<Self>,
    ) {
        let p = prefix(source);
        let id = game.as_ref().map(|g| s(g, "id").to_owned());
        let title = game
            .as_ref()
            .map(|g| s(g, "title").to_owned())
            .unwrap_or_else(|| source.title().to_owned());
        let body = match &id {
            Some(id) => json!({"id": id}),
            None => json!({}),
        };
        live::call(
            cx,
            "POST",
            format!("{p}/stop"),
            Some(body),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(id) = &id {
                        if let Some(game) = this
                            .emu_mut(source)
                            .games
                            .iter_mut()
                            .find(|g| s(g, "id") == id)
                        {
                            game["running"] = json!(false);
                        }
                    }
                    if source == SharpSource::Pcsx2 {
                        toast::success(cx, format!("{title} stopped"));
                        this.emu_refresh(source, false, cx);
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("Could not stop {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    fn emu_remove_runtime(&mut self, source: SharpSource, cx: &mut Context<Self>) {
        let p = prefix(source);
        live::call(
            cx,
            "POST",
            format!("{p}/remove-runtime"),
            Some(json!({"confirm": true})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.emu_mut(source).update = None;
                    toast::success(
                        cx,
                        format!(
                            "{} runtime removed; user data was preserved",
                            source.title()
                        ),
                    );
                    this.emu_refresh(source, false, cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| {
                            format!("Could not remove {} runtime", source.title())
                        }),
                    );
                }
            },
        );
    }

    pub(super) fn emu_open(&mut self, source: SharpSource, setup: bool, cx: &mut Context<Self>) {
        let p = prefix(source);
        let path = if setup {
            format!("{p}/open-setup")
        } else {
            format!("{p}/open-ui")
        };
        live::call(
            cx,
            "POST",
            path,
            Some(json!({})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not open {}", source.title())),
                    );
                } else if source == SharpSource::Pcsx2 {
                    this.emu_refresh(source, false, cx);
                }
            },
        );
    }

    /// PCSX2 BIOS / RPCS3 firmware + package / shadPS4 modules + fonts imports.
    pub(super) fn emu_import(
        &mut self,
        source: SharpSource,
        kind: &'static str,
        cx: &mut Context<Self>,
    ) {
        cx.spawn(async move |this, cx| {
            let picked = match (source, kind) {
                (SharpSource::Pcsx2, _) => {
                    rfd::AsyncFileDialog::new()
                        .set_title("Choose a PlayStation 2 BIOS")
                        .add_filter("BIOS", &["bin"])
                        .pick_file()
                        .await
                }
                (SharpSource::Rpcs3, "firmware") => {
                    rfd::AsyncFileDialog::new()
                        .set_title("Choose PS3UPDAT.PUP")
                        .add_filter("PS3 firmware", &["pup", "PUP"])
                        .pick_file()
                        .await
                }
                (SharpSource::Rpcs3, _) => {
                    rfd::AsyncFileDialog::new()
                        .set_title("Choose an owned PS3 package")
                        .add_filter("PS3 package", &["pkg", "PKG"])
                        .pick_file()
                        .await
                }
                (_, "modules") => {
                    rfd::AsyncFileDialog::new()
                        .set_title("Choose a folder of console-dumped PS4 modules")
                        .pick_folder()
                        .await
                }
                _ => {
                    rfd::AsyncFileDialog::new()
                        .set_title("Choose dumped PS4 font content")
                        .pick_folder()
                        .await
                }
            };
            let Some(picked) = picked else { return };
            let path = picked.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |this, cx| {
                if source == SharpSource::Pcsx2 {
                    this.live.as_mut().unwrap().emu_confirm = Some(EmuConfirm::Pcsx2Bios(path));
                    cx.notify();
                    return;
                }
                let p = prefix(source);
                let route = match (source, kind) {
                    (SharpSource::Rpcs3, "firmware") => format!("{p}/install-firmware"),
                    (SharpSource::Rpcs3, _) => format!("{p}/install-package"),
                    (_, "modules") => format!("{p}/import-modules"),
                    _ => format!("{p}/import-fonts"),
                };
                live::call(
                    cx,
                    "POST",
                    route,
                    Some(json!({"path": path})),
                    live::DEFAULT_TIMEOUT,
                    move |this, r, cx| {
                        if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                            let message = match (source, kind) {
                                (SharpSource::Rpcs3, "firmware") => {
                                    "RPCS3 firmware installation started".to_owned()
                                }
                                (SharpSource::Rpcs3, _) => {
                                    "RPCS3 package installation started".to_owned()
                                }
                                (_, "modules") => format!(
                                    "Imported {} supported modules",
                                    r.get("imported").and_then(Value::as_u64).unwrap_or(0)
                                ),
                                _ => format!(
                                    "Imported {} font files",
                                    r.get("files").and_then(Value::as_u64).unwrap_or(0)
                                ),
                            };
                            toast::success(cx, message);
                            if source == SharpSource::Rpcs3 {
                                cx.spawn(async move |this, cx| {
                                    cx.background_executor().timer(MS(3000)).await;
                                    let _ = this
                                        .update(cx, |this, cx| this.emu_refresh(source, false, cx));
                                })
                                .detach();
                            } else {
                                this.emu_refresh(source, false, cx);
                            }
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref()).unwrap_or_else(|| {
                                    format!("Could not install {} {kind}", source.title())
                                }),
                            );
                        }
                    },
                );
            });
        })
        .detach();
    }

    fn emu_import_bios(&mut self, path: String, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/sharp-library/pcsx2/import-bios",
            Some(json!({"path": path})),
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                    let label = [s(r, "description"), s(r, "region")]
                        .into_iter()
                        .find(|v| !v.is_empty())
                        .unwrap_or("PlayStation 2 BIOS")
                        .to_owned();
                    toast::success(cx, format!("Validated {label}"));
                    this.emu_refresh(SharpSource::Pcsx2, false, cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "PCSX2 rejected this BIOS dump".into()),
                    );
                }
            },
        );
    }

    pub(super) fn pcsx2_configure(
        &mut self,
        field: &'static str,
        value: String,
        cx: &mut Context<Self>,
    ) {
        live::call(
            cx,
            "POST",
            "/sharp-library/pcsx2/configure",
            Some(json!({field: value})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let source = SharpSource::Pcsx2;
                let Some(result) = r.as_ref().filter(|r| is_ok(r)).cloned() else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not save the PCSX2 setting".into()),
                    );
                    this.emu_refresh(source, false, cx);
                    return;
                };
                let options_key = if field == "renderer" {
                    "rendererOptions"
                } else {
                    "controllerOptions"
                };
                let current = s(&result, field).to_owned();
                let label = result
                    .get(options_key)
                    .and_then(Value::as_array)
                    .and_then(|options| options.iter().find(|o| s(o, "id") == current))
                    .map(|o| s(o, "label").to_owned())
                    .unwrap_or(value);
                let name = match field {
                    "controller1" => "Controller 1",
                    "controller2" => "Controller 2",
                    _ => "Renderer",
                };
                this.emu_mut(source).settings = Some(result);
                toast::success(cx, format!("{name} saved as {label}"));
                cx.notify();
            },
        );
    }

    pub(super) fn run_emu_confirm(&mut self, action: EmuConfirm, cx: &mut Context<Self>) {
        self.live.as_mut().unwrap().emu_confirm = None;
        match action {
            EmuConfirm::Install(source, _) => self.emu_start_install(source, cx),
            EmuConfirm::Rollback(source) => self.emu_rollback(source, cx),
            EmuConfirm::RemoveRoot(source, path) => self.emu_remove_root(source, path, cx),
            EmuConfirm::RemoveRuntime(source) => self.emu_remove_runtime(source, cx),
            EmuConfirm::Pcsx2Bios(path) => self.emu_import_bios(path, cx),
            EmuConfirm::NetworkLaunch(id, title) => {
                self.emu_launch_now(SharpSource::SharpEmu, id, title, true, cx)
            }
        }
        cx.notify();
    }

    pub(super) fn emu_confirm_copy(action: &EmuConfirm) -> (String, String, &'static str) {
        match action {
            EmuConfirm::Install(source, text) => (format!("{} runtime", source.title()), text.clone(), "Continue"),
            EmuConfirm::Rollback(source) => (
                format!("Roll back {}?", source.title()),
                "Switch to the previous runtime? Mutable user data is preserved, but savestates may not be compatible across versions.".into(),
                "Roll back",
            ),
            EmuConfirm::RemoveRoot(source, path) => (
                format!("Remove this {} library reference?", source.title()),
                format!("{path}\n\nNo game files will be deleted."),
                "Remove",
            ),
            EmuConfirm::RemoveRuntime(source) => (
                format!("Remove the managed {} runtime?", source.title()),
                "BIOS, firmware, memory cards, saves, savestates, settings, controller profiles, covers, caches, logs, and external games will be preserved.".into(),
                "Remove",
            ),
            EmuConfirm::Pcsx2Bios(_) => (
                "Import PlayStation 2 BIOS?".into(),
                "Import this BIOS dumped from a PlayStation 2 console you own? MetalSharp copies it only into the isolated PCSX2 environment and never uploads it.".into(),
                "Import",
            ),
            EmuConfirm::NetworkLaunch(..) => (
                "Enable guest networking?".into(),
                "Enable unrestricted guest networking for this launch? Emulated game code may open host sockets, use DNS, and contact local or internet services.".into(),
                "Launch",
            ),
        }
    }

    // ───────────────────────────── rendering ─────────────────────────────

    pub(super) fn live_emulator_header(&self, cx: &mut Context<Self>) -> gpui::Div {
        let source = self.state.source;
        let emu = self.emu(source);
        let status = emu.status.clone().unwrap_or(Value::Null);
        let supported = status.get("supported").and_then(Value::as_bool) != Some(false);
        let label = if emu.updating {
            format!("↻  Installing… {}", source.title())
        } else if emu.checking {
            format!("↻  Checking… {}", source.title())
        } else {
            format!("↻  Check {}", source.title())
        };
        let enabled = supported && !emu.updating && !emu.checking;
        let mut controls = div().flex().items_center().child(
            self.palette
                .button("emu-check", label, true)
                .h(px(40.0))
                .mr(px(12.0))
                .rounded(px(9.0))
                .opacity(if enabled { 1.0 } else { 0.4 })
                .on_click(cx.listener(move |this, _, _, cx| {
                    if enabled {
                        this.emu_install_or_update(source, cx);
                    }
                })),
        );
        let running = s(&status, "state") == "running";
        if source == SharpSource::Pcsx2 && running {
            controls = controls.child(
                self.palette
                    .button("emu-stop-managed", "Stop PCSX2", false)
                    .h(px(40.0))
                    .mr(px(12.0))
                    .rounded(px(9.0))
                    .text_color(rgb(0xff8585))
                    .on_click(cx.listener(move |this, _, _, cx| this.emu_stop(source, None, cx))),
            );
        } else if matches!(source, SharpSource::Pcsx2 | SharpSource::Rpcs3)
            && b(&status, "installed")
        {
            controls = controls.child(
                self.palette
                    .button("emu-open", format!("Open {}", source.title()), false)
                    .h(px(40.0))
                    .mr(px(12.0))
                    .rounded(px(9.0))
                    .on_click(cx.listener(move |this, _, _, cx| this.emu_open(source, false, cx))),
            );
        }
        controls
    }

    pub(super) fn live_emulator_overview(&self, width: f32) -> gpui::Div {
        let source = self.state.source;
        let emu = self.emu(source);
        let status = emu.status.clone().unwrap_or(Value::Null);
        let (eyebrow, platform) = match source {
            SharpSource::Pcsx2 => ("MANAGED PLAYSTATION 2 ENVIRONMENT", "PS2 Emulation"),
            SharpSource::Rpcs3 => ("MANAGED PLAYSTATION 3 ENVIRONMENT", "PS3 Emulation"),
            SharpSource::ShadPs4 => ("EXPERIMENTAL PLAYSTATION 4 ENVIRONMENT", "PS4 Emulation"),
            _ => (
                "EXPERIMENTAL PLAYSTATION 5 RESEARCH ENVIRONMENT",
                "PS5 Emulation",
            ),
        };
        let (_, _, desc) = source.header();
        let installed = b(&status, "installed");
        let runtime = match status.get("currentTag").and_then(Value::as_str) {
            Some(tag) if installed => tag.to_owned(),
            _ if emu.status.is_none() => "Checking host…".into(),
            _ => "Not installed".into(),
        };
        let host = format!(
            "{} · macOS {}",
            match s(&status, "hostArchitecture") {
                "" => "Apple Silicon",
                arch => arch,
            },
            status
                .get("hostMacosMajor")
                .and_then(Value::as_u64)
                .map(|v| v.to_string())
                .unwrap_or_else(|| "—".into())
        );
        let count = emu.games.len();
        let mut stats = vec![
            ("Stable runtime", runtime),
            ("Host", host),
            (
                "Library",
                format!(
                    "{count} {}",
                    if source == SharpSource::SharpEmu {
                        "layouts"
                    } else {
                        "games"
                    }
                ),
            ),
        ];
        match source {
            SharpSource::Pcsx2 => stats.insert(
                2,
                (
                    "User BIOS",
                    if b(&status, "biosInstalled") {
                        [s(&status, "biosDescription"), s(&status, "biosRegion")]
                            .into_iter()
                            .find(|v| !v.is_empty())
                            .unwrap_or("Validated")
                            .to_owned()
                    } else {
                        "Required".into()
                    },
                ),
            ),
            SharpSource::Rpcs3 => stats.insert(
                2,
                (
                    "Firmware",
                    if b(&status, "firmwareInstalled") {
                        "Installed".into()
                    } else {
                        "Required".into()
                    },
                ),
            ),
            SharpSource::ShadPs4 => stats.push((
                "Compatibility files",
                format!(
                    "{} modules · {} fonts",
                    status
                        .get("moduleCount")
                        .and_then(Value::as_u64)
                        .unwrap_or(0),
                    status
                        .get("fontFileCount")
                        .and_then(Value::as_u64)
                        .unwrap_or(0)
                ),
            )),
            _ => stats.push((
                "Guest network",
                if emu.network_opt_in {
                    "Explicitly enabled".into()
                } else {
                    "Denied by default".into()
                },
            )),
        }
        let state_label = match s(&status, "state") {
            "running" => "● Running",
            "ready" => "● Ready",
            "unsupported_host" => "● Unsupported Mac",
            "missing_bios" => "● BIOS required",
            "missing_firmware" => "● Firmware required",
            "runtime_probe_failed" => "● Runtime repair required",
            "no_game_folders" | "no_game_roots" => "● Add games",
            "no_games" => "● No layouts found",
            _ if emu.status.is_none() => "● Checking host…",
            _ => "● Setup required",
        };
        let progress = emu.updating.then(|| {
            let p = emu.progress.clone().unwrap_or(Value::Null);
            (
                p.get("percent").and_then(Value::as_f64).unwrap_or(0.0),
                s(&p, "message").to_owned(),
            )
        });
        let stat_width = ((width - 36.0 - 6.0) / 2.0).max(110.0);
        let mut stat_grid = div().flex().flex_wrap().gap(px(6.0));
        for (index, (label, value)) in stats.into_iter().enumerate() {
            let icon = match index {
                0 => "source-installers.svg",
                1 => "source-empty-monitor.svg",
                _ => "source-pcsx2.svg",
            };
            stat_grid = stat_grid.child(
                div()
                    .w(px(stat_width))
                    .min_w_0()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .px(px(10.0))
                    .py(px(6.0))
                    .rounded(px(8.0))
                    .bg(gpui::rgba(0x00000024))
                    .border_1()
                    .border_color(gpui::rgba(0xffffff0a))
                    .child(
                        gpui::svg()
                            .path(icon)
                            .size(px(14.0))
                            .text_color(rgb(self.palette.accent)),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(2.0))
                            .child(div().text_size(px(9.0)).text_color(rgb(MUTED)).child(label))
                            .child(
                                div()
                                    .text_size(px(11.5))
                                    .text_color(rgb(TEXT))
                                    .font_weight(FontWeight::BOLD)
                                    .child(value),
                            ),
                    ),
            );
        }
        div()
            .flex_none()
            .w_full()
            .rounded(px(12.0))
            .border_1()
            .border_color(gpui::rgba((self.palette.accent << 8) | 0x40))
            .bg(rgb(0x1c2228))
            .px(px(18.0))
            .py(px(14.0))
            .flex()
            .flex_col()
            .gap(px(12.0))
            .child(
                div()
                    .flex()
                    .gap(px(12.0))
                    .items_start()
                    .child(
                        div()
                            .flex_none()
                            .size(px(34.0))
                            .rounded(px(9.0))
                            .border_1()
                            .border_color(gpui::rgba((self.palette.accent << 8) | 0x4d))
                            .bg(gpui::rgba((self.palette.accent << 8) | 0x1c))
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(source.icon(self.palette.accent, 26.0)),
                    )
                    .child(
                        div()
                            .min_w_0()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(
                                div()
                                    .text_size(px(9.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(MUTED))
                                    .child(eyebrow),
                            )
                            .child(
                                div()
                                    .flex()
                                    .flex_wrap()
                                    .items_center()
                                    .gap(px(8.0))
                                    .child(
                                        div()
                                            .text_size(px(17.0))
                                            .font_weight(FontWeight::BOLD)
                                            .text_color(rgb(TEXT))
                                            .child(source.title()),
                                    )
                                    .child(
                                        div()
                                            .text_size(px(12.0))
                                            .text_color(rgb(0xb1b8bb))
                                            .child(format!("- {platform}")),
                                    )
                                    .child(pill(state_label)),
                            )
                            .child(
                                div()
                                    .text_size(px(11.5))
                                    .line_height(px(16.675))
                                    .text_color(rgb(0xaeb3b2))
                                    .max_h(px(33.35))
                                    .overflow_hidden()
                                    .child(desc),
                            ),
                    ),
            )
            .child(stat_grid)
            .children(progress.map(|(percent, message)| {
                div()
                    .flex()
                    .flex_col()
                    .gap(px(6.0))
                    .child(div().text_size(px(10.5)).text_color(rgb(MUTED)).child(
                        if message.is_empty() {
                            "Installing…".to_owned()
                        } else {
                            message
                        },
                    ))
                    .child(
                        div()
                            .h(px(6.0))
                            .w_full()
                            .rounded_full()
                            .bg(rgb(0x2a2d30))
                            .child(
                                div()
                                    .h(px(6.0))
                                    .rounded_full()
                                    .bg(rgb(self.palette.accent))
                                    .w(gpui::relative((percent / 100.0).clamp(0.0, 1.0) as f32)),
                            ),
                    )
            }))
    }

    pub(super) fn live_emulator_sidebar(
        &self,
        width: f32,
        compact: bool,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let source = self.state.source;
        let emu = self.emu(source);
        let status = emu.status.clone().unwrap_or(Value::Null);
        let installed = b(&status, "installed");
        // (action, label, detail, enabled)
        let commands: Vec<(&'static str, &'static str, &'static str, bool)> = match source {
            SharpSource::Pcsx2 => vec![
                ("bios", "Import BIOS", "Accepts a .bin BIOS file", installed),
                (
                    "link-pcsx2-firmware",
                    "Download Firmware",
                    "Open PCSX2 firmware page",
                    true,
                ),
                ("link-archive", "Find Games", "Open archive.org", true),
                (
                    "setup",
                    "PCSX2 Setup",
                    if installed {
                        "Controllers & renderer"
                    } else {
                        "Install PCSX2 first"
                    },
                    installed,
                ),
                ("folder", "Add Games", "Disc image or folder", true),
                ("scan", "Scan Library", "Refresh metadata", true),
            ],
            SharpSource::Rpcs3 => vec![
                ("firmware", "Firmware", "Install PS3UPDAT.PUP", installed),
                (
                    "link-rpcs3-firmware",
                    "Download Firmware",
                    "Open PlayStation support",
                    true,
                ),
                ("link-archive", "Find Games", "Open archive.org", true),
                ("package", "Install Package", "Add an owned PKG", installed),
                ("folder", "Add games", "Choose a library folder", true),
                ("scan", "Scan library", "Refresh games and artwork", true),
            ],
            SharpSource::ShadPs4 => vec![
                ("folder", "Add games", "Choose dumped CUSA folders", true),
                (
                    "modules",
                    "Import modules",
                    "Console-dumped SPRX files",
                    true,
                ),
                ("fonts", "Import fonts", "Console-dumped font content", true),
                ("scan", "Scan library", "Refresh metadata and artwork", true),
            ],
            _ => vec![
                (
                    "folder",
                    "Add layouts",
                    "Reference owned eboot.bin folders",
                    true,
                ),
                (
                    "scan",
                    "Scan library",
                    "Refresh bounded local metadata",
                    true,
                ),
                (
                    "link-sharpemu-faq",
                    "Official FAQ",
                    "Open sharpemu.app",
                    true,
                ),
                (
                    "link-sharpemu-compat",
                    "Compatibility",
                    "View upstream reports",
                    true,
                ),
            ],
        };
        let mut side = div()
            .flex_none()
            .w(px(width))
            .min_w_0()
            .flex()
            .flex_col()
            .gap(px(8.0));
        let setup_open = emu.settings_menu.is_some() || self.state.sidebar_open[source.index()][0];
        for (index, (action, label, detail, enabled)) in commands.into_iter().enumerate() {
            let icon = match action {
                "folder" => "source-installers.svg",
                "bios" | "firmware" | "modules" => "source-pcsx2.svg",
                "setup" => "source-empty-monitor.svg",
                _ => "source-rpcs3.svg",
            };
            side = side.child(
                div()
                    .id(("emulator-live-command", index))
                    .w_full()
                    .min_h(px(if compact { 44.0 } else { 54.0 }))
                    .rounded(px(11.0))
                    .border_1()
                    .border_color(gpui::rgba(0xffffff14))
                    .bg(gpui::rgba(0xffffff06))
                    .px(px(10.0))
                    .py(px(9.0))
                    .flex()
                    .items_center()
                    .gap(px(9.0))
                    .cursor_pointer()
                    .opacity(if enabled { 1.0 } else { 0.4 })
                    .child(gpui::svg().path(icon).size(px(17.0)).flex_none().text_color(rgb(self.palette.accent)))
                    .child(
                        div()
                            .min_w_0()
                            .flex()
                            .flex_col()
                            .gap(px(3.0))
                            .child(div().text_size(px(11.0)).font_weight(FontWeight::BOLD).text_color(rgb(TEXT)).child(label))
                            .children((!compact).then(|| div().text_size(px(9.0)).line_height(px(12.0)).text_color(rgb(MUTED)).child(detail))),
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        if !enabled {
                            return;
                        }
                        match action {
                            "setup" => {
                                let i = source.index();
                                this.state.sidebar_open[i][0] = !this.state.sidebar_open[i][0];
                            }
                            "scan" => this.emu_refresh(source, true, cx),
                            "folder" => this.emu_add_root(source, cx),
                            "bios" => this.emu_import(source, "bios", cx),
                            "firmware" => this.emu_import(source, "firmware", cx),
                            "package" => this.emu_import(source, "package", cx),
                            "modules" => this.emu_import(source, "modules", cx),
                            "fonts" => this.emu_import(source, "fonts", cx),
                            "link-archive" => cx.open_url("https://archive.org/"),
                            "link-pcsx2-firmware" => cx.open_url("https://www.retrostic.com/bios/pcsx2-playstation-2"),
                            "link-rpcs3-firmware" => cx.open_url("https://www.playstation.com/en-us/support/hardware/ps3/system-software/"),
                            "link-sharpemu-faq" => cx.open_url("https://sharpemu.app/faq/"),
                            "link-sharpemu-compat" => cx.open_url("https://sharpemu.app/compatibility/"),
                            _ => {}
                        }
                        cx.notify();
                    })),
            );
            if action == "setup" && setup_open {
                let settings = emu.settings.clone().unwrap_or(Value::Null);
                let mut panel = div()
                    .w_full()
                    .p(px(11.0))
                    .flex()
                    .flex_col()
                    .gap(px(10.0))
                    .rounded(px(8.0))
                    .bg(rgb(PANEL));
                for (field, label, options_key) in [
                    ("controller1", "CONTROLLER 1", "controllerOptions"),
                    ("controller2", "CONTROLLER 2", "controllerOptions"),
                    ("renderer", "RENDERER", "rendererOptions"),
                ] {
                    let options: Vec<(String, String)> = settings
                        .get(options_key)
                        .and_then(Value::as_array)
                        .map(|list| {
                            list.iter()
                                .map(|o| (s(o, "id").to_owned(), s(o, "label").to_owned()))
                                .collect()
                        })
                        .unwrap_or_default();
                    let current = s(&settings, field).to_owned();
                    let current_label = options
                        .iter()
                        .find(|(id, _)| *id == current)
                        .map(|(_, l)| l.clone())
                        .unwrap_or(current.clone());
                    let open = emu.settings_menu == Some(field);
                    let mut control = div().relative().w_full().child(
                        self.palette
                            .button(
                                gpui::SharedString::from(format!("pcsx2-live-{field}")),
                                format!("{current_label}  ⌄"),
                                false,
                            )
                            .w_full()
                            .justify_start()
                            .text_size(px(11.0))
                            .on_click(cx.listener(move |this, _, _, cx| {
                                let emu = this.emu_mut(source);
                                emu.settings_menu = if emu.settings_menu == Some(field) {
                                    None
                                } else {
                                    Some(field)
                                };
                                cx.notify();
                            })),
                    );
                    if open {
                        let mut menu = div()
                            .id(gpui::SharedString::from(format!("pcsx2-live-menu-{field}")))
                            .occlude()
                            .absolute()
                            .top(gpui::relative(1.0))
                            .left_0()
                            .right_0()
                            .mt(px(4.0))
                            .rounded(px(8.0))
                            .border_1()
                            .border_color(gpui::rgba(self.palette.control_border))
                            .bg(rgb(self.palette.menu_bg))
                            .p(px(5.0))
                            .flex()
                            .flex_col()
                            .shadow_lg();
                        for (index, (id, label)) in options.into_iter().enumerate() {
                            menu = menu.child(
                                self.palette
                                    .button(
                                        gpui::SharedString::from(format!(
                                            "pcsx2-live-{field}-{index}"
                                        )),
                                        label,
                                        id == current,
                                    )
                                    .w_full()
                                    .justify_start()
                                    .border_0()
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.emu_mut(source).settings_menu = None;
                                        this.pcsx2_configure(field, id.clone(), cx);
                                    })),
                            );
                        }
                        control = control.child(gpui::deferred(menu).with_priority(60));
                    }
                    panel = panel.child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(5.0))
                            .child(
                                div()
                                    .text_size(px(9.0))
                                    .text_color(rgb(MUTED))
                                    .font_weight(FontWeight::BOLD)
                                    .child(label),
                            )
                            .child(control),
                    );
                }
                panel = panel.child(
                    self.palette
                        .button("pcsx2-open-setup", "Open PCSX2 Setup", false)
                        .w_full()
                        .h(px(28.0))
                        .on_click(
                            cx.listener(move |this, _, _, cx| this.emu_open(source, true, cx)),
                        ),
                );
                side = side.child(panel);
            }
        }
        if source == SharpSource::SharpEmu {
            let enabled = emu.network_opt_in;
            side = side.child(
                div()
                    .id("sharpemu-live-network")
                    .p(px(10.0))
                    .rounded(px(10.0))
                    .border_1()
                    .border_color(gpui::rgba(0xffb84d3d))
                    .bg(gpui::rgba(0xffb84d08))
                    .flex()
                    .items_start()
                    .gap(px(8.0))
                    .cursor_pointer()
                    .child(div().size(px(14.0)).border_1().border_color(rgb(MUTED)).rounded(px(3.0)).text_size(px(11.0)).text_color(rgb(self.palette.accent)).child(if enabled { "✓" } else { "" }))
                    .child(
                        div()
                            .min_w_0()
                            .flex_1()
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .child(div().text_size(px(11.0)).font_weight(FontWeight::BOLD).text_color(rgb(TEXT)).child("Guest networking"))
                            .child(div().text_size(px(9.0)).line_height(px(13.0)).text_color(rgb(MUTED)).child(
                                "Off by default. When enabled, emulated game code may create host sockets, use DNS, and contact local or internet services. Every network-enabled launch asks again.",
                            )),
                    )
                    .on_click(cx.listener(move |this, _, _, cx| {
                        let emu = this.emu_mut(source);
                        emu.network_opt_in = !emu.network_opt_in;
                        cx.notify();
                    })),
            );
        }
        // Runtime & support / Game folders drawers.
        let update = emu.update.clone();
        let rollback = b(&status, "rollbackAvailable");
        for (section, title) in [
            (1usize, "Runtime & support"),
            (
                2usize,
                if source == SharpSource::Pcsx2 {
                    "Game locations"
                } else {
                    "Game folders"
                },
            ),
        ] {
            let i = source.index();
            let open = self.state.sidebar_open[i][section];
            let mut drawer = div()
                .w_full()
                .rounded(px(10.0))
                .border_1()
                .border_color(gpui::rgba(0xffffff14))
                .overflow_hidden()
                .child(
                    div()
                        .id(("emulator-live-drawer", section))
                        .px(px(10.0))
                        .py(px(9.0))
                        .flex()
                        .items_center()
                        .gap(px(7.0))
                        .cursor_pointer()
                        .child(
                            gpui::svg()
                                .path("source-installers.svg")
                                .size(px(16.0))
                                .text_color(rgb(self.palette.accent)),
                        )
                        .child(
                            div()
                                .flex_1()
                                .text_size(px(11.0))
                                .font_weight(FontWeight::BOLD)
                                .text_color(rgb(TEXT))
                                .child(title),
                        )
                        .children((section == 2).then(|| {
                            div()
                                .text_size(px(9.0))
                                .text_color(rgb(MUTED))
                                .child(emu.roots.len().to_string())
                        }))
                        .child(
                            div()
                                .text_size(px(10.0))
                                .text_color(rgb(MUTED))
                                .child(if open { "⌄" } else { "›" }),
                        )
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.state.sidebar_open[i][section] =
                                !this.state.sidebar_open[i][section];
                            cx.notify();
                        })),
                );
            if open {
                let mut body = div().p(px(10.0)).flex().flex_col().gap(px(8.0));
                if section == 2 {
                    body = body.child(
                        self.palette
                            .button("emulator-live-root-add", "+  Add", false)
                            .h(px(28.0))
                            .on_click(
                                cx.listener(move |this, _, _, cx| this.emu_add_root(source, cx)),
                            ),
                    );
                    if emu.roots.is_empty() {
                        body = body.child(div().text_size(px(10.0)).text_color(rgb(MUTED)).child(
                            if source == SharpSource::Pcsx2 {
                                "No game locations added yet."
                            } else {
                                "No game folders added yet."
                            },
                        ));
                    }
                    for (index, root) in emu.roots.iter().enumerate() {
                        let path = root.clone();
                        body = body.child(
                            div()
                                .flex()
                                .items_center()
                                .gap(px(6.0))
                                .child(
                                    div()
                                        .flex_1()
                                        .min_w_0()
                                        .overflow_hidden()
                                        .text_size(px(10.0))
                                        .text_color(rgb(MUTED))
                                        .child(root.clone()),
                                )
                                .child(
                                    self.palette
                                        .button(
                                            ("emulator-live-root-remove", index),
                                            "Remove",
                                            false,
                                        )
                                        .h(px(24.0))
                                        .text_size(px(10.0))
                                        .on_click(cx.listener(move |this, _, _, cx| {
                                            this.live.as_mut().unwrap().emu_confirm =
                                                Some(EmuConfirm::RemoveRoot(source, path.clone()));
                                            cx.notify();
                                        })),
                                ),
                        );
                    }
                } else {
                    let pinned = update
                        .as_ref()
                        .is_some_and(|u| !s(u, "pinnedTag").is_empty());
                    let skipped = update
                        .as_ref()
                        .is_some_and(|u| !s(u, "skippedTag").is_empty());
                    body = body.child(
                        self.palette
                            .button(
                                "emulator-live-check",
                                format!("Check {}", source.title()),
                                false,
                            )
                            .h(px(28.0))
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.emu_check_update(source, true, true, |_, _, _| {}, cx)
                            })),
                    );
                    if installed {
                        body = body
                            .child(
                                self.palette
                                    .button(
                                        "emulator-live-pin",
                                        if pinned {
                                            "Unpin Version"
                                        } else {
                                            "Pin Current"
                                        },
                                        false,
                                    )
                                    .h(px(28.0))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.emu_policy(
                                            source,
                                            if pinned { "unpin" } else { "pin-current" },
                                            cx,
                                        )
                                    })),
                            )
                            .children(
                                update
                                    .as_ref()
                                    .filter(|u| b(u, "available") || skipped)
                                    .map(|_| {
                                        self.palette
                                            .button(
                                                "emulator-live-skip",
                                                if skipped {
                                                    "Clear Skipped Update"
                                                } else {
                                                    "Skip This Update"
                                                },
                                                false,
                                            )
                                            .h(px(28.0))
                                            .on_click(cx.listener(move |this, _, _, cx| {
                                                this.emu_policy(
                                                    source,
                                                    if skipped {
                                                        "clear-skip"
                                                    } else {
                                                        "skip-update"
                                                    },
                                                    cx,
                                                )
                                            }))
                                    }),
                            )
                            .children(rollback.then(|| {
                                self.palette
                                    .button("emulator-live-rollback", "Rollback", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.live.as_mut().unwrap().emu_confirm =
                                            Some(EmuConfirm::Rollback(source));
                                        cx.notify();
                                    }))
                            }))
                            .child(
                                self.palette
                                    .button("emulator-live-remove", "Remove Runtime", false)
                                    .h(px(28.0))
                                    .text_color(rgb(0xff8585))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.live.as_mut().unwrap().emu_confirm =
                                            Some(EmuConfirm::RemoveRuntime(source));
                                        cx.notify();
                                    })),
                            );
                    }
                    if source == SharpSource::Pcsx2 {
                        body = body
                            .child(
                                self.palette
                                    .button("pcsx2-bios-guide", "BIOS Guide", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|_, _, _, cx| {
                                        cx.open_url("https://pcsx2.net/docs/setup/bios/")
                                    })),
                            )
                            .child(
                                self.palette
                                    .button("pcsx2-disc-guide", "Disc Guide", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|_, _, _, cx| {
                                        cx.open_url("https://pcsx2.net/docs/setup/discs/")
                                    })),
                            );
                    } else if source == SharpSource::SharpEmu {
                        body = body
                            .child(
                                self.palette
                                    .button("sharpemu-repo", "Source & GPL License", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|_, _, _, cx| {
                                        cx.open_url("https://github.com/sharpemu/sharpemu")
                                    })),
                            )
                            .child(
                                self.palette
                                    .button("sharpemu-releases", "Official Releases", false)
                                    .h(px(28.0))
                                    .on_click(cx.listener(|_, _, _, cx| {
                                        cx.open_url("https://github.com/sharpemu/sharpemu/releases")
                                    })),
                            );
                    }
                }
                drawer = drawer.child(body);
            }
            side = side.child(drawer);
        }
        side
    }

    pub(super) fn live_emulator_content(
        &self,
        card_width: f32,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let source = self.state.source;
        let emu = self.emu(source);
        if emu.games.is_empty() {
            let (heading, desc) = if source == SharpSource::SharpEmu {
                (
                    "No layouts found",
                    "Reference an owned eboot.bin folder to add a layout.",
                )
            } else {
                (
                    "No games found",
                    "Add a game location or folder to build your library.",
                )
            };
            return div()
                .w_full()
                .child(empty_state(self.palette, heading, desc));
        }
        let mut grid = div().flex().flex_wrap().gap(px(14.));
        for game in emu.games.iter().cloned() {
            let id = s(&game, "id").to_owned();
            let running = b(&game, "running");
            let serial = [s(&game, "serial"), s(&game, "titleId")]
                .into_iter()
                .find(|v| !v.is_empty())
                .unwrap_or("")
                .to_owned();
            let detail = match source {
                SharpSource::Pcsx2 => format!(
                    "{}{}{}",
                    serial,
                    if s(&game, "format").is_empty() {
                        String::new()
                    } else {
                        format!(" · {}", s(&game, "format").to_uppercase())
                    },
                    game.get("size")
                        .and_then(Value::as_u64)
                        .filter(|b| *b > 0)
                        .map(|b| format!(" · {}", format_bytes(b)))
                        .unwrap_or_default()
                ),
                SharpSource::SharpEmu => format!("{serial} · {}", s(&game, "contentVersion")),
                _ => format!("{serial} · {}", s(&game, "version")),
            };
            let art = self.cover_path(&format!("sharp-{}-{id}", source.id()), cx);
            let badge = match source {
                SharpSource::Pcsx2 => "PS2",
                SharpSource::Rpcs3 => "PS3",
                SharpSource::ShadPs4 => "PS4",
                _ => "PS5",
            };
            let action_game = game.clone();
            let body = div()
                .p(px(14.0))
                .pt(px(13.0))
                .flex()
                .flex_col()
                .gap(px(10.0))
                .child(
                    div()
                        .text_size(px(14.0))
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(rgb(0xf0f0ed))
                        .child(s(&game, "title").to_owned()),
                )
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(pill(if running { "Running" } else { badge }))
                        .child(
                            div()
                                .min_w_0()
                                .text_size(px(10.0))
                                .text_color(rgb(MUTED))
                                .child(detail),
                        ),
                )
                .child(
                    self.palette
                        .button(
                            gpui::SharedString::from(format!("emu-play-{id}")),
                            if running { "Stop" } else { "Play" },
                            true,
                        )
                        .h(px(32.0))
                        .rounded(px(6.0))
                        .on_click(cx.listener(move |this, _, _, cx| {
                            let running =
                                this.emu(source).games.iter().any(|g| {
                                    s(g, "id") == s(&action_game, "id") && b(g, "running")
                                });
                            if running {
                                this.emu_stop(source, Some(action_game.clone()), cx);
                            } else {
                                this.emu_launch(source, action_game.clone(), cx);
                            }
                        })),
                );
            grid = grid.child(
                div()
                    .id(gpui::SharedString::from(format!("emu-card-{id}")))
                    .w(px(card_width))
                    .min_w_0()
                    .flex()
                    .flex_col()
                    .overflow_hidden()
                    .rounded(px(12.0))
                    .border_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .bg(gpui::linear_gradient(
                        180.0,
                        gpui::linear_color_stop(rgb(0x202427), 0.0),
                        gpui::linear_color_stop(rgb(0x171a1c), 1.0),
                    ))
                    .child(match art {
                        Some(art) => crate::artwork::art_img(art)
                            .w_full()
                            .h(px(card_width * 9.0 / 16.0))
                            .object_fit(ObjectFit::Cover)
                            .into_any_element(),
                        None => div()
                            .w_full()
                            .h(px(card_width * 9.0 / 16.0))
                            .flex()
                            .items_center()
                            .justify_center()
                            .bg(rgb(0x15181a))
                            .child(source.icon(self.palette.accent, 40.0))
                            .into_any_element(),
                    })
                    .child(body),
            );
        }
        div().w_full().child(grid)
    }

    pub(super) fn live_emu_confirm_overlay(
        &self,
        action: EmuConfirm,
        viewport: gpui::Size<gpui::Pixels>,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        let (head, desc, confirm) = Self::emu_confirm_copy(&action);
        let muted = if self.palette.light { 0x64717a } else { MUTED };
        div()
            .id("sharp-emu-modal")
            .occlude()
            .absolute()
            .inset_0()
            .flex()
            .items_center()
            .justify_center()
            .bg(gpui::rgba(0x000000aa))
            .on_click(cx.listener(|this, _, _, cx| {
                this.live.as_mut().unwrap().emu_confirm = None;
                cx.notify();
            }))
            .child(
                div()
                    .id("sharp-emu-dialog")
                    .occlude()
                    .on_click(|_, _, cx| cx.stop_propagation())
                    .w(px(460.0))
                    .max_w(px(f32::from(viewport.width) - 48.0))
                    .rounded(px(16.0))
                    .border_1()
                    .border_color(gpui::rgba(self.palette.control_border))
                    .bg(rgb(self.palette.menu_bg))
                    .p(px(22.0))
                    .flex()
                    .flex_col()
                    .gap(px(14.0))
                    .child(
                        div()
                            .text_size(px(17.0))
                            .text_color(rgb(self.palette.control_text))
                            .font_weight(FontWeight::BOLD)
                            .child(head),
                    )
                    .child(
                        div()
                            .text_size(px(12.0))
                            .line_height(px(18.0))
                            .text_color(rgb(muted))
                            .child(desc),
                    )
                    .child(
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.0))
                            .child(
                                self.palette
                                    .button("sharp-emu-cancel", "Cancel", false)
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.live.as_mut().unwrap().emu_confirm = None;
                                        cx.notify();
                                    })),
                            )
                            .child(
                                self.palette
                                    .button("sharp-emu-confirm", confirm, true)
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.run_emu_confirm(action.clone(), cx);
                                    })),
                            ),
                    ),
            )
    }
}
