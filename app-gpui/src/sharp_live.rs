//! Live Sharp Library sources (Installers, GOG, Epic, GameJolt) ported from
//! `SharpView.vue`. Rendering reuses the approved Sharp card/page styling.
use super::*;
use crate::live::{self, error_text, is_ok};
use crate::toast;
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};
use std::time::Duration;

const MS: fn(u64) -> Duration = Duration::from_millis;

pub(super) const ENGINE_OPTIONS: [(&str, &str); 6] = [
    ("d3dmetal", "D3DMetal"),
    ("vkd3d", "VKD3D"),
    ("d3d9", "D3D9"),
    ("dxmt", "DXMT"),
    ("dxmt_32", "DXMT(32)"),
    ("fna_arm64", "Mono/FNA"),
];

#[derive(Clone, Debug, PartialEq)]
pub(super) enum SharpConfirm {
    UninstallApp(String, String),
    RemoveGogPrefix,
    DisconnectGog,
    UninstallGog(String, String),
    DisconnectEpic,
    UninstallEpic(String, String),
    UninstallGameJolt(String, String, String),
}

#[derive(Default)]
pub(super) struct SharpLive {
    pub apps: Vec<Value>,
    pub sharp_running: HashMap<String, u64>,
    pub gog_status: Option<Value>,
    pub gog_games: Vec<Value>,
    pub gog_progress: HashMap<String, f64>,
    pub gog_engines: HashMap<String, String>,
    pub gog_loading: HashSet<String>,
    pub epic_status: Option<Value>,
    pub epic_games: Vec<Value>,
    pub epic_progress: HashMap<String, f64>,
    pub epic_loading: HashSet<String>,
    pub epic_cancelled: HashSet<String>,
    pub gj_games: Vec<Value>,
    pub gj_storage: Option<Value>,
    pub gj_running: HashMap<String, u64>,
    pub gj_loading: bool,
    pub open_bottle: HashSet<String>,
    pub engine_menu: Option<String>,
    pub confirm: Option<SharpConfirm>,
    pub oauth: Option<std::sync::mpsc::Receiver<crate::mini_browser::MiniBrowserResult>>,
    pub emu: [super::sharp_emu_live::EmuState; 4],
    pub emu_confirm: Option<super::sharp_emu_live::EmuConfirm>,
    pub active: bool,
    pub loaded: bool,
    polling: bool,
    tick: u64,
    in_flight: HashSet<&'static str>,
}

fn s<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn b(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}

fn sort_by_title(list: &mut [Value], key: &str) {
    list.sort_by(|a, b| s(a, key).to_lowercase().cmp(&s(b, key).to_lowercase()));
}

impl SharpPreview {
    pub fn attach_live(&mut self, cx: &mut Context<Self>) {
        if crate::live::Live::get(cx).is_none() {
            return;
        }
        let mut live = SharpLive::default();
        let engines_path =
            crate::live::Live::get(cx).map(|l| l.home().join("gpui-gog-engines.json"));
        if let Some(path) = engines_path {
            if let Ok(text) = std::fs::read_to_string(path) {
                if let Ok(map) = serde_json::from_str::<HashMap<String, String>>(&text) {
                    live.gog_engines = map;
                }
            }
        }
        self.live = Some(live);
        self.state.notice.clear();
        if let Some(cache) = crate::artwork::cache(cx) {
            cx.observe(&cache, |_, _, cx| cx.notify()).detach();
        }
    }

    pub fn set_active(&mut self, active: bool, cx: &mut Context<Self>) {
        let Some(live) = self.live.as_mut() else {
            return;
        };
        if live.active == active {
            return;
        }
        live.active = active;
        if active {
            self.load(cx);
            if !self.live.as_ref().is_some_and(|l| l.polling) {
                self.live.as_mut().unwrap().polling = true;
                self.start_polling(cx);
            }
        }
    }

    fn live_mut(&mut self) -> &mut SharpLive {
        self.live.as_mut().expect("live sharp state")
    }

    fn start_polling(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(MS(500)).await;
                let keep = this.update(cx, |this, cx| {
                    let Some(live) = this.live.as_mut() else {
                        return false;
                    };
                    if !live.active {
                        live.polling = false;
                        return false;
                    }
                    live.tick += 1;
                    let tick = live.tick;
                    // OAuth completion from the native browser.
                    if let Some(result) = live.oauth.as_ref().and_then(|rx| rx.try_recv().ok()) {
                        live.oauth = None;
                        this.finish_oauth(result, cx);
                    }
                    if tick % 3 == 0 {
                        this.refresh_gamejolt_running(cx);
                        this.refresh_sharp_running(cx);
                    }
                    if tick % 6 == 0 {
                        let source = this.state.source;
                        let installed = super::sharp_emu_live::emu_index(source).is_some_and(|i| {
                            this.live.as_ref().unwrap().emu[i]
                                .status
                                .as_ref()
                                .is_some_and(|s| b(s, "installed"))
                        });
                        if installed {
                            this.emu_refresh(source, false, cx);
                        }
                    }
                    if tick % 4 == 0 {
                        let live = this.live.as_ref().unwrap();
                        let gog = this.state.source == SharpSource::Gog
                            && live.gog_games.iter().any(|g| b(g, "running"));
                        let epic = this.state.source == SharpSource::Epic
                            && live.epic_games.iter().any(|g| b(g, "running"));
                        if gog {
                            this.refresh_gog_running(cx);
                        }
                        if epic {
                            this.refresh_epic_running(cx);
                        }
                    }
                    true
                });
                if !matches!(keep, Ok(true)) {
                    break;
                }
            }
        })
        .detach();
    }

    fn guarded(&mut self, key: &'static str) -> bool {
        let live = self.live_mut();
        if live.in_flight.contains(key) {
            return false;
        }
        live.in_flight.insert(key);
        true
    }

    fn release(&mut self, key: &'static str) {
        if let Some(live) = self.live.as_mut() {
            live.in_flight.remove(key);
        }
    }

    /// SharpView `load()`.
    pub(super) fn load(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/sharp-library",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let mut apps = r
                        .get("apps")
                        .and_then(Value::as_array)
                        .cloned()
                        .unwrap_or_default();
                    sort_by_title(&mut apps, "name");
                    this.live_mut().apps = apps;
                    this.request_covers(cx);
                    cx.notify();
                }
            },
        );
        self.refresh_gog(cx);
        self.load_gamejolt(cx, true);
        self.refresh_epic(false, cx);
        self.emu_refresh(SharpSource::Rpcs3, false, cx);
        self.emu_refresh(SharpSource::ShadPs4, false, cx);
        if matches!(
            self.state.source,
            SharpSource::Pcsx2 | SharpSource::SharpEmu
        ) {
            self.emu_refresh(self.state.source, false, cx);
        }
        self.live_mut().loaded = true;
    }

    fn request_covers(&mut self, cx: &mut Context<Self>) {
        let Some(cache) = crate::artwork::cache(cx) else {
            return;
        };
        use crate::artwork::Candidate as C;
        let live = self.live.as_ref().unwrap();
        let mut requests: Vec<(String, Vec<C>)> = Vec::new();
        for app in &live.apps {
            let id = s(app, "id");
            let encoded: String = url::form_urlencoded::byte_serialize(id.as_bytes()).collect();
            requests.push((
                format!("sharp-app-{id}"),
                vec![C::Backend(format!("/sharp-library/cover?id={encoded}"))],
            ));
        }
        for game in &live.gog_games {
            let url = s(game, "imageUrl");
            if !url.is_empty() {
                requests.push((
                    format!("sharp-gog-{}", s(game, "productId")),
                    vec![C::Url(url.to_owned())],
                ));
            }
        }
        for game in &live.epic_games {
            let url = s(game, "artworkUrl");
            if !url.is_empty() {
                let candidate = if s(game, "artworkSource") == "TheGamesDB" {
                    C::LocalFile(url.to_owned())
                } else {
                    C::Url(url.to_owned())
                };
                requests.push((
                    format!("sharp-epic-{}", s(game, "appName")),
                    vec![candidate],
                ));
            }
        }
        for game in &live.gj_games {
            let id = s(game, "id");
            let encoded: String = url::form_urlencoded::byte_serialize(id.as_bytes()).collect();
            let mut candidates = vec![C::Backend(format!("/gamejolt/cover?id={encoded}"))];
            let cover = s(game, "cover_path");
            if !cover.is_empty() {
                candidates.insert(0, C::LocalFile(cover.to_owned()));
            }
            requests.push((format!("sharp-gamejolt-{id}"), candidates));
        }
        cache.update(cx, |cache, cx| {
            for (key, candidates) in requests {
                cache.resolve(key, candidates, cx);
            }
        });
    }

    pub(super) fn cover_path(&self, key: &str, cx: &gpui::App) -> Option<std::path::PathBuf> {
        crate::artwork::cache(cx)?.read(cx).path(key)
    }

    // ───────────────────────────── installers ─────────────────────────────

    fn refresh_sharp_running(&mut self, cx: &mut Context<Self>) {
        if !self.guarded("sharp-running") {
            return;
        }
        live::call(
            cx,
            "GET",
            "/sharp-library/running",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.release("sharp-running");
                if let Some(r) = r.filter(is_ok) {
                    let next: HashMap<String, u64> = r
                        .get("running")
                        .and_then(Value::as_array)
                        .into_iter()
                        .flatten()
                        .filter_map(|e| {
                            Some((e.get("id")?.as_str()?.to_owned(), e.get("pid")?.as_u64()?))
                        })
                        .collect();
                    if next != this.live_mut().sharp_running {
                        this.live_mut().sharp_running = next;
                        cx.notify();
                    }
                }
            },
        );
    }

    pub(super) fn install_exe(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            let Some(file) = rfd::AsyncFileDialog::new()
                .add_filter("Windows installer", &["exe", "msi"])
                .pick_file()
                .await
            else {
                return;
            };
            let path = file.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                toast::info(cx, "Installing application...");
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/install",
                    Some(json!({"srcPath": path})),
                    live::DEFAULT_TIMEOUT,
                    |this, r, cx| {
                        if let Some(app) = r.as_ref().filter(|r| is_ok(r)).and_then(|r| r.get("app")) {
                            toast::success(cx, format!("Installed {}", s(app, "name")));
                            this.load(cx);
                        } else if r.as_ref().is_some_and(|r| is_ok(r) && b(r, "installing")) {
                            toast::success(
                                cx,
                                r.as_ref()
                                    .and_then(|r| r.get("message"))
                                    .and_then(Value::as_str)
                                    .unwrap_or("Installer started. Finish setup, then refresh Sharp Library.")
                                    .to_owned(),
                            );
                            this.load(cx);
                        } else {
                            toast::error(cx, error_text(r.as_ref()).unwrap_or_else(|| "Failed to install".into()));
                        }
                    },
                );
            });
        })
        .detach();
    }

    pub(super) fn launch_app(&mut self, app: Value, cx: &mut Context<Self>) {
        let id = s(&app, "id").to_owned();
        let name = s(&app, "name").to_owned();
        let engine = app
            .get("engine")
            .and_then(Value::as_str)
            .unwrap_or("auto")
            .to_owned();
        toast::info(cx, format!("Launching {name}..."));
        live::call(
            cx,
            "POST",
            "/sharp-library/launch",
            Some(json!({"id": id, "engine": engine})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let pid = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("pid"))
                    .and_then(Value::as_u64);
                if let Some(pid) = pid {
                    this.live_mut().sharp_running.insert(id.clone(), pid);
                    let warning = r
                        .as_ref()
                        .and_then(|r| r.get("warnings"))
                        .and_then(Value::as_array)
                        .and_then(|w| w.first())
                        .and_then(Value::as_str)
                        .map(str::to_owned);
                    crate::launch_overlay::hint_later(name.clone(), cx);
                    toast::success(
                        cx,
                        match warning {
                            Some(w) => format!("Launched {name}: {w}"),
                            None => format!("Launched {name}"),
                        },
                    );
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Failed to launch {name}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn stop_app(&mut self, app: Value, cx: &mut Context<Self>) {
        let id = s(&app, "id").to_owned();
        let name = s(&app, "name").to_owned();
        live::call(
            cx,
            "POST",
            "/sharp-library/stop",
            Some(json!({"id": id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.live_mut().sharp_running.remove(&id);
                    toast::info(cx, format!("Closed {name}"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("Could not close {name}")),
                    );
                    this.refresh_sharp_running(cx);
                }
                cx.notify();
            },
        );
    }

    pub(super) fn set_app_engine(&mut self, id: String, engine: String, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/sharp-library/set-engine",
            Some(json!({"id": id, "engine": engine})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(app) = this.live_mut().apps.iter_mut().find(|a| s(a, "id") == id) {
                        app["engine"] = json!(engine);
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Failed to set engine".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn set_app_cover(&mut self, id: String, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            let Some(file) = rfd::AsyncFileDialog::new()
                .add_filter("Image", &["jpg", "jpeg", "png", "webp"])
                .pick_file()
                .await
            else {
                return;
            };
            let path = file.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/set-cover",
                    Some(json!({"id": id, "coverPath": path})),
                    live::DEFAULT_TIMEOUT,
                    move |this, r, cx| {
                        if r.as_ref().is_some_and(is_ok) {
                            if let Some(cache) = crate::artwork::cache(cx) {
                                cache.update(cx, |cache, cx| {
                                    cache.forget(&format!("sharp-app-{id}"), cx)
                                });
                            }
                            toast::success(cx, "Cover updated");
                            this.load(cx);
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Failed to set cover".into()),
                            );
                        }
                    },
                );
            });
        })
        .detach();
    }

    fn uninstall_app(&mut self, id: String, name: String, cx: &mut Context<Self>) {
        live::call(
            cx,
            "POST",
            "/sharp-library/uninstall",
            Some(json!({"id": id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    toast::success(cx, format!("Uninstalled {name}"));
                    this.load(cx);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Failed to uninstall".into()),
                    );
                }
            },
        );
    }

    // ───────────────────────────────── GOG ─────────────────────────────────

    fn set_gog_games(&mut self, games: Vec<Value>) {
        let mut unique: Vec<Value> = Vec::new();
        for game in games {
            let id = s(&game, "productId").to_owned();
            if let Some(previous) = unique.iter_mut().find(|g| s(g, "productId") == id) {
                let mut merged = previous.clone();
                if let (Some(target), Some(source)) = (merged.as_object_mut(), game.as_object()) {
                    for (k, v) in source {
                        if (k == "title"
                            && (v.as_str().unwrap_or("").is_empty() || v.as_str() == Some(&id)))
                            || ((k == "imageUrl" || k == "iconUrl") && v.is_null())
                        {
                            continue;
                        }
                        target.insert(k.clone(), v.clone());
                    }
                }
                *previous = merged;
            } else {
                unique.push(game);
            }
        }
        sort_by_title(&mut unique, "title");
        self.live_mut().gog_games = unique;
    }

    fn upsert_gog_game(&mut self, game: Value) {
        let mut games = self.live_mut().gog_games.clone();
        games.push(game);
        self.set_gog_games(games);
    }

    pub(super) fn refresh_gog(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/sharp-library/gog/status",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(status) = r.filter(is_ok).and_then(|r| r.get("status").cloned()) {
                    this.live_mut().gog_status = Some(status);
                    cx.notify();
                }
            },
        );
        live::call(
            cx,
            "GET",
            "/sharp-library/gog/games",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let games = r
                        .get("games")
                        .and_then(Value::as_array)
                        .cloned()
                        .unwrap_or_default();
                    this.set_gog_games(games);
                    if let Some(status) = r.get("status") {
                        this.live_mut().gog_status = Some(status.clone());
                    }
                    let downloading: Vec<String> = this
                        .live_mut()
                        .gog_games
                        .iter()
                        .filter(|g| s(g, "status") == "downloading")
                        .map(|g| s(g, "productId").to_owned())
                        .collect();
                    for id in downloading {
                        if !this.live_mut().gog_progress.contains_key(&id) {
                            this.live_mut().gog_progress.insert(id.clone(), 0.0);
                            this.monitor_gog_progress(id, 0, cx);
                        }
                    }
                    this.request_covers(cx);
                    cx.notify();
                }
            },
        );
    }

    fn refresh_gog_running(&mut self, cx: &mut Context<Self>) {
        if !self.guarded("gog-running") {
            return;
        }
        live::call(
            cx,
            "GET",
            "/sharp-library/gog/games",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.release("gog-running");
                if let Some(r) = r.filter(is_ok) {
                    this.set_gog_games(
                        r.get("games")
                            .and_then(Value::as_array)
                            .cloned()
                            .unwrap_or_default(),
                    );
                    if let Some(status) = r.get("status") {
                        this.live_mut().gog_status = Some(status.clone());
                    }
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn initialize_gog_prefix(&mut self, cx: &mut Context<Self>) {
        self.live_mut().gog_loading.insert("setup".into());
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/initialize-prefix",
            Some(json!({})),
            MS(5 * 60 * 1000),
            |this, r, cx| {
                this.live_mut().gog_loading.remove("setup");
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(mut status) = r.as_ref().and_then(|r| r.get("status")).cloned() {
                        let authenticated = b(&status, "authenticated");
                        status["prefixInitialized"] = json!(true);
                        status["status"] = json!(if authenticated {
                            "ready"
                        } else {
                            "needs_login"
                        });
                        status["ready"] = json!(authenticated);
                        this.live_mut().gog_status = Some(status);
                    }
                    this.refresh_gog(cx);
                    toast::success(cx, "GOG prefix ready");
                } else {
                    let hangup = r.is_none()
                        || error_text(r.as_ref())
                            .is_some_and(|e| e.to_lowercase().contains("socket hang up"));
                    let error = error_text(r.as_ref())
                        .unwrap_or_else(|| "Failed to initialize GOG prefix".into());
                    // Decide after the refreshed status arrives (Electron awaits refreshGog).
                    live::call(
                        cx,
                        "GET",
                        "/sharp-library/gog/status",
                        None,
                        live::DEFAULT_TIMEOUT,
                        move |this, status, cx| {
                            if let Some(status) =
                                status.filter(is_ok).and_then(|r| r.get("status").cloned())
                            {
                                this.live_mut().gog_status = Some(status);
                            }
                            let initialized = this
                                .live_mut()
                                .gog_status
                                .as_ref()
                                .is_some_and(|s| b(s, "prefixInitialized"));
                            if hangup && initialized {
                                toast::success(cx, "GOG prefix ready");
                            } else {
                                toast::error(cx, error);
                            }
                            this.refresh_gog(cx);
                        },
                    );
                }
                cx.notify();
            },
        );
    }

    fn remove_gog_prefix(&mut self, cx: &mut Context<Self>) {
        self.live_mut().gog_loading.insert("removing".into());
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/remove-prefix",
            Some(json!({})),
            MS(30_000),
            |this, r, cx| {
                this.live_mut().gog_loading.remove("removing");
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(status) = r.as_ref().and_then(|r| r.get("status")).cloned() {
                        this.live_mut().gog_status = Some(status);
                    }
                    this.refresh_gog(cx);
                    toast::success(cx, "GOG prefix removed");
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to remove GOG prefix".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    fn disconnect_gog(&mut self, cx: &mut Context<Self>) {
        self.live_mut().gog_loading.insert("login".into());
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/logout",
            Some(json!({})),
            MS(30_000),
            |this, r, cx| {
                this.live_mut().gog_loading.remove("login");
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(status) = r.as_ref().and_then(|r| r.get("status")).cloned() {
                        this.live_mut().gog_status = Some(status);
                    }
                    this.refresh_gog(cx);
                    toast::success(cx, "GOG disconnected");
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Failed to disconnect GOG".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn gog_auth_button(&mut self, cx: &mut Context<Self>) {
        let status = self.live_mut().gog_status.clone().unwrap_or(Value::Null);
        if b(&status, "authenticated") {
            self.live_mut().confirm = Some(SharpConfirm::DisconnectGog);
            cx.notify();
            return;
        }
        let url = s(&status, "authUrl").to_owned();
        if url.is_empty() {
            return;
        }
        self.live_mut().gog_loading.insert("login".into());
        self.open_oauth(crate::mini_browser::BrowserPurpose::GogAuth, &url, cx);
    }

    fn open_oauth(
        &mut self,
        purpose: crate::mini_browser::BrowserPurpose,
        url: &str,
        cx: &mut Context<Self>,
    ) {
        if self.live_mut().oauth.is_some() {
            // One sign-in window at a time; a second would orphan the first result.
            self.live_mut().gog_loading.remove("login");
            self.live_mut().epic_loading.remove("login");
            toast::error(cx, "Finish the open sign-in window first");
            cx.notify();
            return;
        }
        #[cfg(target_os = "macos")]
        {
            let request = crate::mini_browser::MiniBrowserRequest::new(
                purpose,
                url,
                "MetalSharp account sign-in",
            );
            if let (Ok(request), Some(mtm)) = (request, objc2::MainThreadMarker::new()) {
                let (tx, rx) = std::sync::mpsc::channel();
                if crate::mini_browser::open_native(
                    mtm,
                    request,
                    Box::new(move |result| {
                        let _ = tx.send(result);
                    }),
                )
                .is_ok()
                {
                    self.live_mut().oauth = Some(rx);
                    cx.notify();
                    return;
                }
            }
        }
        self.live_mut().gog_loading.remove("login");
        self.live_mut().epic_loading.remove("login");
        toast::error(cx, "Could not open the sign-in window");
        cx.notify();
    }

    fn finish_oauth(
        &mut self,
        result: crate::mini_browser::MiniBrowserResult,
        cx: &mut Context<Self>,
    ) {
        use crate::mini_browser::MiniBrowserResult as R;
        match result {
            R::GogCode(code) => {
                toast::success(cx, "GOG login code captured; finishing connection…");
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/gog/auth-code",
                    Some(json!({"code": code})),
                    MS(90_000),
                    |this, r, cx| {
                        this.live_mut().gog_loading.remove("login");
                        if let Some(status) = r
                            .as_ref()
                            .filter(|r| is_ok(r))
                            .and_then(|r| r.get("status"))
                            .cloned()
                        {
                            this.live_mut().gog_status = Some(status);
                            toast::success(cx, "GOG connected");
                            this.sync_gog(cx);
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Failed to connect GOG".into()),
                            );
                        }
                        cx.notify();
                    },
                );
            }
            R::EpicCode(code) => {
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/epic/auth",
                    Some(json!({"code": code})),
                    MS(90_000),
                    |this, r, cx| {
                        this.live_mut().epic_loading.remove("login");
                        if r.as_ref()
                            .is_some_and(|r| is_ok(r) && b(r, "authenticated"))
                        {
                            let r = r.unwrap();
                            let account = s(&r, "account").to_owned();
                            this.live_mut().epic_status = Some(r);
                            toast::success(
                                cx,
                                if account.is_empty() {
                                    "Connected to Epic".into()
                                } else {
                                    format!("Connected to Epic as {account}")
                                },
                            );
                            this.sync_epic(cx);
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Epic authentication failed".into()),
                            );
                        }
                        cx.notify();
                    },
                );
            }
            R::Cancelled => {
                let gog = self.live_mut().gog_loading.remove("login");
                self.live_mut().epic_loading.remove("login");
                toast::error(
                    cx,
                    if gog {
                        "GOG login cancelled"
                    } else {
                        "Epic sign-in cancelled"
                    },
                );
            }
            _ => {
                let gog = self.live_mut().gog_loading.remove("login");
                self.live_mut().epic_loading.remove("login");
                toast::error(
                    cx,
                    if gog {
                        "Failed to connect GOG"
                    } else {
                        "Epic authentication failed"
                    },
                );
            }
        }
        cx.notify();
    }

    pub(super) fn sync_gog(&mut self, cx: &mut Context<Self>) {
        self.live_mut().gog_loading.insert("sync".into());
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/sync",
            Some(json!({})),
            MS(5 * 60 * 1000),
            |this, r, cx| {
                this.live_mut().gog_loading.remove("sync");
                if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                    if let Some(games) = r.get("games").and_then(Value::as_array) {
                        this.set_gog_games(games.clone());
                    }
                    if let Some(status) = r.get("status") {
                        this.live_mut().gog_status = Some(status.clone());
                    }
                    this.refresh_gog(cx);
                    toast::success(cx, "GOG library synced");
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Failed to sync GOG library".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn install_gog_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let title = s(&game, "title").to_owned();
        cx.spawn(async move |this, cx| {
            let Some(folder) = rfd::AsyncFileDialog::new()
                .set_title(format!("Choose install folder for {title}"))
                .pick_folder()
                .await
            else {
                return;
            };
            let path = folder.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |this, cx| {
                let id = s(&game, "productId").to_owned();
                this.live_mut().gog_loading.insert(format!("{id}:install"));
                cx.notify();
                let platform = match s(&game, "platform") {
                    "" => "windows",
                    other => other,
                }
                .to_owned();
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/gog/install",
                    Some(json!({"productId": id, "title": title, "platform": platform, "installPath": path})),
                    MS(90_000),
                    move |this, r, cx| {
                        this.live_mut().gog_loading.remove(&format!("{id}:install"));
                        if let Some(game) = r.as_ref().filter(|r| is_ok(r)).and_then(|r| r.get("game")).cloned() {
                            this.upsert_gog_game(game);
                            toast::success(cx, format!("Downloading {title}"));
                            this.monitor_gog_progress(id, 0, cx);
                        } else {
                            toast::error(cx, error_text(r.as_ref()).unwrap_or_else(|| format!("Failed to download {title}")));
                        }
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    fn monitor_gog_progress(&mut self, id: String, attempt: u32, cx: &mut Context<Self>) {
        if attempt >= 720 {
            // Let the next refresh restart monitoring (Electron re-monitors on load).
            self.live_mut().gog_progress.remove(&id);
            return;
        }
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/progress",
            Some(json!({"productId": id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let Some(r) = r.filter(is_ok) else {
                    this.live_mut().gog_progress.remove(&id);
                    cx.notify();
                    return;
                };
                let previous = this
                    .live_mut()
                    .gog_progress
                    .get(&id)
                    .copied()
                    .unwrap_or(0.0);
                let percent = r.get("percent").and_then(Value::as_f64).unwrap_or(previous);
                this.live_mut().gog_progress.insert(id.clone(), percent);
                if let Some(game) = r.get("game").cloned() {
                    this.upsert_gog_game(game);
                }
                cx.notify();
                let still = b(&r, "active")
                    || r.get("game")
                        .is_some_and(|g| s(g, "status") == "downloading");
                if !still {
                    this.live_mut().gog_progress.remove(&id);
                    this.request_covers(cx);
                    return;
                }
                cx.spawn(async move |this, cx| {
                    cx.background_executor().timer(MS(2000)).await;
                    let _ = this.update(cx, |this, cx| {
                        this.monitor_gog_progress(id, attempt + 1, cx)
                    });
                })
                .detach();
            },
        );
    }

    pub(super) fn play_gog_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let id = s(&game, "productId").to_owned();
        let title = s(&game, "title").to_owned();
        let engine = self
            .live_mut()
            .gog_engines
            .get(&id)
            .cloned()
            .unwrap_or_else(|| "auto".into());
        self.live_mut().gog_loading.insert(format!("{id}:play"));
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/play",
            Some(json!({"productId": id, "engine": engine})),
            MS(90_000),
            move |this, r, cx| {
                this.live_mut().gog_loading.remove(&format!("{id}:play"));
                if let Some(game) = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("game"))
                    .cloned()
                {
                    this.upsert_gog_game(game);
                    crate::launch_overlay::hint_later(title.clone(), cx);
                    toast::success(cx, format!("{title} launched"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Failed to launch {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn stop_gog_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let id = s(&game, "productId").to_owned();
        let title = s(&game, "title").to_owned();
        self.live_mut().gog_loading.insert(format!("{id}:stop"));
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/stop",
            Some(json!({"productId": id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                this.live_mut().gog_loading.remove(&format!("{id}:stop"));
                if let Some(game) = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("game"))
                    .cloned()
                {
                    this.upsert_gog_game(game);
                    toast::success(cx, format!("{title} stopped"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("Failed to stop {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    fn uninstall_gog_game(&mut self, id: String, title: String, cx: &mut Context<Self>) {
        self.live_mut()
            .gog_loading
            .insert(format!("{id}:uninstall"));
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/gog/uninstall",
            Some(json!({"productId": id})),
            MS(90_000),
            move |this, r, cx| {
                this.live_mut()
                    .gog_loading
                    .remove(&format!("{id}:uninstall"));
                if let Some(game) = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("game"))
                    .cloned()
                {
                    this.upsert_gog_game(game);
                    toast::success(cx, format!("{title} uninstalled"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Failed to uninstall {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn set_gog_engine(&mut self, id: String, engine: String, cx: &mut Context<Self>) {
        self.live_mut().gog_engines.insert(id, engine);
        if let Some(live) = crate::live::Live::get(cx) {
            let _ = std::fs::write(
                live.home().join("gpui-gog-engines.json"),
                serde_json::to_vec(&self.live_mut().gog_engines).unwrap_or_default(),
            );
        }
        cx.notify();
    }

    // ───────────────────────────────── Epic ─────────────────────────────────

    pub(super) fn refresh_epic(&mut self, force_sync: bool, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/sharp-library/epic/status",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let Some(status) = r.filter(is_ok) else {
                    return;
                };
                let authenticated = b(&status, "authenticated");
                this.live_mut().epic_status = Some(status);
                if !authenticated {
                    this.live_mut().epic_games.clear();
                    cx.notify();
                    return;
                }
                let (method, path) = if force_sync {
                    ("POST", "/sharp-library/epic/sync")
                } else {
                    ("GET", "/sharp-library/epic/games")
                };
                live::call(
                    cx,
                    method,
                    path,
                    Some(json!({})),
                    MS(10 * 60 * 1000),
                    move |this, r, cx| {
                        if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                            let mut games = r
                                .get("games")
                                .and_then(Value::as_array)
                                .cloned()
                                .unwrap_or_default();
                            sort_by_title(&mut games, "title");
                            let downloading: Vec<String> = games
                                .iter()
                                .filter(|g| b(g, "downloading"))
                                .map(|g| s(g, "appName").to_owned())
                                .collect();
                            this.live_mut().epic_games = games;
                            for app in downloading {
                                if !this.live_mut().epic_progress.contains_key(&app) {
                                    this.live_mut().epic_progress.insert(app.clone(), 0.0);
                                    this.monitor_epic_progress(app, 0, cx);
                                }
                            }
                            this.request_covers(cx);
                        } else if force_sync {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Epic library sync failed".into()),
                            );
                        }
                        cx.notify();
                    },
                );
            },
        );
    }

    fn refresh_epic_running(&mut self, cx: &mut Context<Self>) {
        if !self.guarded("epic-running") {
            return;
        }
        live::call(
            cx,
            "GET",
            "/sharp-library/epic/running",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.release("epic-running");
                let Some(r) = r.filter(is_ok) else { return };
                let running: HashSet<String> = r
                    .get("running")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(|v| v.as_str().map(str::to_owned))
                    .collect();
                for game in this.live_mut().epic_games.iter_mut() {
                    let app = s(game, "appName").to_owned();
                    game["running"] = json!(running.contains(&app));
                }
                cx.notify();
            },
        );
    }

    pub(super) fn install_epic_support(&mut self, cx: &mut Context<Self>, then_login: bool) {
        self.live_mut().epic_loading.insert("tool".into());
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/install-tool",
            Some(json!({})),
            MS(30 * 60 * 1000),
            move |this, r, cx| {
                this.live_mut().epic_loading.remove("tool");
                if r.as_ref()
                    .is_some_and(|r| is_ok(r) && b(r, "toolAvailable"))
                {
                    this.live_mut().epic_status = r;
                    toast::success(cx, "Epic support installed");
                    if then_login {
                        this.epic_auth_button(cx);
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not install Epic support".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn epic_auth_button(&mut self, cx: &mut Context<Self>) {
        let status = self.live_mut().epic_status.clone().unwrap_or(Value::Null);
        if b(&status, "authenticated") {
            self.live_mut().confirm = Some(SharpConfirm::DisconnectEpic);
            cx.notify();
            return;
        }
        if !b(&status, "toolAvailable") {
            self.install_epic_support(cx, true);
            return;
        }
        self.live_mut().epic_loading.insert("login".into());
        self.open_oauth(
            crate::mini_browser::BrowserPurpose::EpicAuth,
            "https://legendary.gl/epiclogin",
            cx,
        );
    }

    fn logout_epic(&mut self, cx: &mut Context<Self>) {
        self.live_mut().epic_loading.insert("login".into());
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/logout",
            Some(json!({})),
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.live_mut().epic_loading.remove("login");
                if r.as_ref().is_some_and(is_ok) {
                    this.live_mut().epic_status = r;
                    this.live_mut().epic_games.clear();
                    toast::success(cx, "Epic Games disconnected");
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| "Epic logout failed".into()),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn sync_epic(&mut self, cx: &mut Context<Self>) {
        self.live_mut().epic_loading.insert("sync".into());
        cx.notify();
        live::call(
            cx,
            "GET",
            "/sharp-library/epic/status",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                let Some(status) = r.filter(is_ok) else {
                    this.live_mut().epic_loading.remove("sync");
                    cx.notify();
                    return;
                };
                let authenticated = b(&status, "authenticated");
                this.live_mut().epic_status = Some(status);
                if !authenticated {
                    this.live_mut().epic_loading.remove("sync");
                    this.live_mut().epic_games.clear();
                    cx.notify();
                    return;
                }
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/epic/sync",
                    Some(json!({})),
                    MS(10 * 60 * 1000),
                    |this, r, cx| {
                        this.live_mut().epic_loading.remove("sync");
                        if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                            let mut games = r
                                .get("games")
                                .and_then(Value::as_array)
                                .cloned()
                                .unwrap_or_default();
                            sort_by_title(&mut games, "title");
                            this.live_mut().epic_games = games;
                            this.request_covers(cx);
                            toast::success(cx, "Epic library synced");
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Epic library sync failed".into()),
                            );
                        }
                        cx.notify();
                    },
                );
            },
        );
    }

    pub(super) fn install_epic_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let title = s(&game, "title").to_owned();
        let app = s(&game, "appName").to_owned();
        cx.spawn(async move |this, cx| {
            let Some(folder) = rfd::AsyncFileDialog::new()
                .set_title(format!("Choose install folder for {title}"))
                .pick_folder()
                .await
            else {
                return;
            };
            let path = folder.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |this, cx| {
                this.live_mut()
                    .epic_loading
                    .insert(format!("{app}:install"));
                cx.notify();
                live::call(
                    cx,
                    "POST",
                    "/sharp-library/epic/install",
                    Some(json!({"appName": app, "installPath": path})),
                    MS(30_000),
                    move |this, r, cx| {
                        this.live_mut()
                            .epic_loading
                            .remove(&format!("{app}:install"));
                        if r.as_ref().is_some_and(is_ok) {
                            toast::success(cx, format!("Downloading {title} to {path}"));
                            this.live_mut().epic_progress.insert(app.clone(), 0.0);
                            this.monitor_epic_progress(app, 0, cx);
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| format!("Could not download {title}")),
                            );
                        }
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    fn monitor_epic_progress(&mut self, app: String, attempt: u32, cx: &mut Context<Self>) {
        if attempt >= 21600 {
            return;
        }
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/progress",
            Some(json!({"appName": app})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                let Some(r) = r.filter(is_ok) else { return };
                this.live_mut().epic_progress.insert(
                    app.clone(),
                    r.get("percent").and_then(Value::as_f64).unwrap_or(0.0),
                );
                cx.notify();
                if b(&r, "active") {
                    cx.spawn(async move |this, cx| {
                        cx.background_executor().timer(MS(2000)).await;
                        let _ = this.update(cx, |this, cx| {
                            this.monitor_epic_progress(app, attempt + 1, cx)
                        });
                    })
                    .detach();
                    return;
                }
                this.live_mut().epic_progress.remove(&app);
                if this.live_mut().epic_cancelled.remove(&app) {
                    this.refresh_epic(false, cx);
                    return;
                }
                live::call(
                    cx,
                    "GET",
                    "/sharp-library/epic/games",
                    None,
                    MS(10 * 60 * 1000),
                    move |this, r, cx| {
                        if let Some(r) = r.as_ref().filter(|r| is_ok(r)) {
                            let mut games = r
                                .get("games")
                                .and_then(Value::as_array)
                                .cloned()
                                .unwrap_or_default();
                            sort_by_title(&mut games, "title");
                            this.live_mut().epic_games = games;
                        }
                        let game = this
                            .live_mut()
                            .epic_games
                            .iter()
                            .find(|g| s(g, "appName") == app)
                            .cloned();
                        let title = game
                            .as_ref()
                            .map(|g| s(g, "title").to_owned())
                            .unwrap_or_else(|| app.clone());
                        if game.as_ref().is_some_and(|g| b(g, "installed")) {
                            toast::success(cx, format!("{title} installed"));
                        } else {
                            toast::error(
                                cx,
                                format!("{title} download stopped; inspect its install log"),
                            );
                        }
                        this.request_covers(cx);
                        cx.notify();
                    },
                );
            },
        );
    }

    pub(super) fn cancel_epic_install(&mut self, game: Value, cx: &mut Context<Self>) {
        let app = s(&game, "appName").to_owned();
        let title = s(&game, "title").to_owned();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/cancel",
            Some(json!({"appName": app})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.live_mut().epic_cancelled.insert(app.clone());
                    this.live_mut().epic_progress.remove(&app);
                    toast::success(cx, format!("{title} download stopped"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not stop {title} download")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn initialize_epic_bottle(&mut self, game: Value, cx: &mut Context<Self>) {
        let app = s(&game, "appName").to_owned();
        let title = s(&game, "title").to_owned();
        let pipeline = match s(&game, "pipeline") {
            "" => "auto",
            other => other,
        }
        .to_owned();
        let mouse = match s(&game, "mouseMode") {
            "" => "no-recenter",
            other => other,
        }
        .to_owned();
        self.live_mut()
            .epic_loading
            .insert(format!("{app}:initialize"));
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/initialize",
            Some(json!({"appName": app, "pipeline": pipeline, "mouseMode": mouse})),
            MS(5 * 60 * 1000),
            move |this, r, cx| {
                this.live_mut()
                    .epic_loading
                    .remove(&format!("{app}:initialize"));
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(game) = this
                        .live_mut()
                        .epic_games
                        .iter_mut()
                        .find(|g| s(g, "appName") == app)
                    {
                        game["bottleInitialized"] = json!(true);
                    }
                    toast::success(cx, format!("{title} bottle initialized"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not initialize {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn update_epic_bottle(
        &mut self,
        app: String,
        field: &'static str,
        value: String,
        cx: &mut Context<Self>,
    ) {
        let game = {
            let live = self.live_mut();
            let Some(game) = live.epic_games.iter_mut().find(|g| s(g, "appName") == app) else {
                return;
            };
            game[field] = json!(value);
            game.clone()
        };
        if b(&game, "bottleInitialized") {
            self.initialize_epic_bottle(game, cx);
        }
        cx.notify();
    }

    pub(super) fn play_epic_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let app = s(&game, "appName").to_owned();
        let title = s(&game, "title").to_owned();
        self.live_mut().epic_loading.insert(format!("{app}:play"));
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/play",
            Some(json!({"appName": app})),
            MS(5 * 60 * 1000),
            move |this, r, cx| {
                this.live_mut().epic_loading.remove(&format!("{app}:play"));
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(game) = this
                        .live_mut()
                        .epic_games
                        .iter_mut()
                        .find(|g| s(g, "appName") == app)
                    {
                        game["running"] = json!(true);
                    }
                    crate::launch_overlay::hint_later(title.clone(), cx);
                    toast::success(cx, format!("{title} launched"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not launch {title}")),
                    );
                }
                this.refresh_epic_running(cx);
                cx.notify();
            },
        );
    }

    pub(super) fn stop_epic_game(&mut self, game: Value, cx: &mut Context<Self>) {
        let app = s(&game, "appName").to_owned();
        let title = s(&game, "title").to_owned();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/stop",
            Some(json!({"appName": app})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    if let Some(game) = this
                        .live_mut()
                        .epic_games
                        .iter_mut()
                        .find(|g| s(g, "appName") == app)
                    {
                        game["running"] = json!(false);
                    }
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("Could not stop {title}")),
                    );
                }
                this.refresh_epic_running(cx);
                cx.notify();
            },
        );
    }

    fn uninstall_epic_game(&mut self, app: String, title: String, cx: &mut Context<Self>) {
        self.live_mut()
            .epic_loading
            .insert(format!("{app}:uninstall"));
        cx.notify();
        live::call(
            cx,
            "POST",
            "/sharp-library/epic/uninstall",
            Some(json!({"appName": app})),
            MS(10 * 60 * 1000),
            move |this, r, cx| {
                this.live_mut()
                    .epic_loading
                    .remove(&format!("{app}:uninstall"));
                if r.as_ref().is_some_and(is_ok) {
                    this.refresh_epic(false, cx);
                    toast::success(cx, format!("{title} uninstalled"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not uninstall {title}")),
                    );
                }
                cx.notify();
            },
        );
    }

    /// Shared "Choose EXE" for GOG / Epic cards.
    pub(super) fn choose_provider_executable(
        &mut self,
        epic: bool,
        game: Value,
        cx: &mut Context<Self>,
    ) {
        let (key_field, key, route, default_dir, title) = if epic {
            (
                "appName",
                s(&game, "appName").to_owned(),
                "/sharp-library/epic/save-executable",
                [s(&game, "executable"), s(&game, "installPath")]
                    .into_iter()
                    .find(|p| !p.is_empty())
                    .map(str::to_owned),
                s(&game, "title").to_owned(),
            )
        } else {
            (
                "productId",
                s(&game, "productId").to_owned(),
                "/sharp-library/gog/save-executable",
                [
                    s(&game, "executablePath"),
                    s(&game, "gameFolder"),
                    s(&game, "installRoot"),
                ]
                .into_iter()
                .find(|p| !p.is_empty())
                .map(str::to_owned),
                s(&game, "title").to_owned(),
            )
        };
        cx.spawn(async move |this, cx| {
            let mut dialog = rfd::AsyncFileDialog::new().add_filter("Windows executable", &["exe"]);
            if let Some(dir) = default_dir.map(std::path::PathBuf::from) {
                let dir = if dir.is_file() {
                    dir.parent().map(|p| p.to_path_buf()).unwrap_or(dir)
                } else {
                    dir
                };
                if dir.is_dir() {
                    dialog = dialog.set_directory(dir);
                }
            }
            let Some(file) = dialog.pick_file().await else {
                return;
            };
            let selected = file.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                live::call(
                    cx,
                    "POST",
                    route,
                    Some(json!({key_field: key, "executablePath": selected})),
                    live::DEFAULT_TIMEOUT,
                    move |this, r, cx| {
                        if !r.as_ref().is_some_and(is_ok) {
                            toast::error(
                                cx,
                                error_text(r.as_ref()).unwrap_or_else(|| {
                                    format!("Could not save {title}'s executable")
                                }),
                            );
                            return;
                        }
                        let saved = r
                            .as_ref()
                            .and_then(|r| r.get("executablePath"))
                            .and_then(Value::as_str)
                            .unwrap_or(&selected)
                            .to_owned();
                        let live = this.live_mut();
                        let list = if epic {
                            &mut live.epic_games
                        } else {
                            &mut live.gog_games
                        };
                        if let Some(game) = list.iter_mut().find(|g| s(g, key_field) == key) {
                            game[if epic { "executable" } else { "executablePath" }] = json!(saved);
                        }
                        toast::success(cx, format!("{title}: launch executable saved"));
                        cx.notify();
                    },
                );
            });
        })
        .detach();
    }

    // ─────────────────────────────── GameJolt ───────────────────────────────

    pub(super) fn load_gamejolt(&mut self, cx: &mut Context<Self>, then_sync: bool) {
        live::call(
            cx,
            "GET",
            "/gamejolt",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let mut games = r
                        .get("games")
                        .and_then(Value::as_array)
                        .cloned()
                        .unwrap_or_default();
                    sort_by_title(&mut games, "name");
                    this.live_mut().gj_games = games;
                    if let Some(storage) = r.get("storage") {
                        this.live_mut().gj_storage = Some(storage.clone());
                    }
                    this.request_covers(cx);
                    cx.notify();
                }
                if then_sync {
                    this.sync_gamejolt(false, cx);
                }
            },
        );
        live::call(
            cx,
            "GET",
            "/gamejolt/storage",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    this.live_mut().gj_storage = Some(r);
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn sync_gamejolt(&mut self, show: bool, cx: &mut Context<Self>) {
        self.live_mut().gj_loading = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/gamejolt/sync",
            None,
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                this.live_mut().gj_loading = false;
                let Some(r) = r.filter(is_ok) else {
                    if show {
                        toast::error(cx, "GameJolt scan failed");
                    }
                    cx.notify();
                    return;
                };
                let mut games = r
                    .get("games")
                    .and_then(Value::as_array)
                    .cloned()
                    .unwrap_or_default();
                sort_by_title(&mut games, "name");
                let count = games.len();
                this.live_mut().gj_games = games;
                if let Some(storage) = r.get("storage") {
                    this.live_mut().gj_storage = Some(storage.clone());
                }
                this.request_covers(cx);
                if show {
                    toast::success(
                        cx,
                        format!(
                            "Found {count} GameJolt game{}",
                            if count == 1 { "" } else { "s" }
                        ),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn choose_gamejolt_storage(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            let Some(folder) = rfd::AsyncFileDialog::new()
                .set_title("Choose the parent folder for GameJolt games")
                .pick_folder()
                .await
            else {
                return;
            };
            let root = folder.path().to_string_lossy().into_owned();
            let _ = this.update(cx, move |_, cx| {
                live::call(
                    cx,
                    "POST",
                    "/gamejolt/storage",
                    Some(json!({"rootPath": root})),
                    live::DEFAULT_TIMEOUT,
                    |this, r, cx| {
                        if r.as_ref().is_some_and(is_ok) {
                            this.live_mut().gj_storage = r;
                            this.sync_gamejolt(true, cx);
                        } else {
                            toast::error(
                                cx,
                                error_text(r.as_ref())
                                    .unwrap_or_else(|| "Could not change GameJolt storage".into()),
                            );
                        }
                    },
                );
            });
        })
        .detach();
    }

    pub fn reload_gamejolt(&mut self, cx: &mut Context<Self>) {
        if self.live.is_some() {
            self.load_gamejolt(cx, false);
        }
    }

    /// Open the GameJolt store in the native browser (Electron embeds it below the grid).
    pub(super) fn open_gamejolt_browser(&mut self, cx: &mut Context<Self>) {
        if let Some(live) = crate::live::Live::get(cx) {
            crate::mini_browser::set_gamejolt_download_dir(
                crate::host_actions::gamejolt_download_dir(&live.home()),
            );
        }
        #[cfg(target_os = "macos")]
        {
            if let (Ok(request), Some(mtm)) = (
                crate::mini_browser::MiniBrowserRequest::new(
                    crate::mini_browser::BrowserPurpose::GameJolt,
                    "https://gamejolt.com/games",
                    "MetalSharp — GameJolt",
                ),
                objc2::MainThreadMarker::new(),
            ) {
                if crate::mini_browser::open_native(mtm, request, Box::new(|_| {})).is_ok() {
                    return;
                }
            }
        }
        cx.open_url("https://gamejolt.com/games");
    }

    fn refresh_gamejolt_running(&mut self, cx: &mut Context<Self>) {
        if !self.guarded("gj-running") {
            return;
        }
        live::call(
            cx,
            "GET",
            "/gamejolt/running",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                this.release("gj-running");
                let Some(r) = r.filter(is_ok) else { return };
                let next: HashMap<String, u64> = r
                    .get("running")
                    .and_then(Value::as_array)
                    .into_iter()
                    .flatten()
                    .filter_map(|e| {
                        Some((e.get("id")?.as_str()?.to_owned(), e.get("pid")?.as_u64()?))
                    })
                    .collect();
                if next != this.live_mut().gj_running {
                    this.live_mut().gj_running = next;
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn launch_gamejolt(&mut self, game: Value, cx: &mut Context<Self>) {
        let id = s(&game, "id").to_owned();
        let name = s(&game, "name").to_owned();
        let engine = if b(&game, "native") {
            "native".to_owned()
        } else {
            s(&game, "engine").to_owned()
        };
        toast::info(cx, format!("Launching {name}..."));
        live::call(
            cx,
            "POST",
            "/gamejolt/launch",
            Some(json!({"id": id, "exePath": s(&game, "exe_path"), "engine": engine})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if let Some(pid) = r
                    .as_ref()
                    .filter(|r| is_ok(r))
                    .and_then(|r| r.get("pid"))
                    .and_then(Value::as_u64)
                {
                    this.live_mut().gj_running.insert(id.clone(), pid);
                    crate::launch_overlay::hint_later(name.clone(), cx);
                    toast::success(cx, format!("Launched {name}"));
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Failed to launch {name}")),
                    );
                }
                cx.notify();
            },
        );
    }

    pub(super) fn stop_gamejolt(&mut self, game: Value, cx: &mut Context<Self>) {
        let id = s(&game, "id").to_owned();
        let name = s(&game, "name").to_owned();
        live::call(
            cx,
            "POST",
            "/gamejolt/stop",
            Some(json!({"id": id})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.live_mut().gj_running.remove(&id);
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref()).unwrap_or_else(|| format!("Could not stop {name}")),
                    );
                    this.refresh_gamejolt_running(cx);
                }
                cx.notify();
            },
        );
    }

    fn uninstall_gamejolt(
        &mut self,
        id: String,
        name: String,
        install_dir: String,
        cx: &mut Context<Self>,
    ) {
        live::call(
            cx,
            "POST",
            "/gamejolt/uninstall",
            Some(json!({"id": id, "installDir": install_dir})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| format!("Could not uninstall {name}")),
                    );
                    return;
                }
                this.live_mut().gj_running.remove(&id);
                this.sync_gamejolt(false, cx);
                toast::success(cx, format!("{name} uninstalled"));
            },
        );
    }

    pub(super) fn set_gamejolt_engine(
        &mut self,
        id: String,
        engine: String,
        cx: &mut Context<Self>,
    ) {
        let previous = {
            let live = self.live_mut();
            let Some(game) = live.gj_games.iter_mut().find(|g| s(g, "id") == id) else {
                return;
            };
            let previous = s(game, "engine").to_owned();
            game["engine"] = json!(engine);
            previous
        };
        cx.notify();
        live::call(
            cx,
            "POST",
            "/gamejolt/engine",
            Some(json!({"id": id, "engine": engine})),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if !r.as_ref().is_some_and(is_ok) {
                    if let Some(game) = this
                        .live_mut()
                        .gj_games
                        .iter_mut()
                        .find(|g| s(g, "id") == id)
                    {
                        game["engine"] = json!(previous);
                    }
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not save GameJolt launch option".into()),
                    );
                    cx.notify();
                }
            },
        );
    }

    // ─────────────────────────── confirmations ───────────────────────────

    pub(super) fn run_confirm(&mut self, action: SharpConfirm, cx: &mut Context<Self>) {
        self.live_mut().confirm = None;
        match action {
            SharpConfirm::UninstallApp(id, name) => self.uninstall_app(id, name, cx),
            SharpConfirm::RemoveGogPrefix => self.remove_gog_prefix(cx),
            SharpConfirm::DisconnectGog => self.disconnect_gog(cx),
            SharpConfirm::UninstallGog(id, title) => self.uninstall_gog_game(id, title, cx),
            SharpConfirm::DisconnectEpic => self.logout_epic(cx),
            SharpConfirm::UninstallEpic(app, title) => self.uninstall_epic_game(app, title, cx),
            SharpConfirm::UninstallGameJolt(id, name, dir) => {
                self.uninstall_gamejolt(id, name, dir, cx)
            }
        }
        cx.notify();
    }

    pub(super) fn confirm_copy(action: &SharpConfirm) -> (String, String, &'static str) {
        match action {
            SharpConfirm::UninstallApp(_, name) => (format!("Uninstall {name}?"), String::new(), "Uninstall"),
            SharpConfirm::RemoveGogPrefix => (
                "Remove the GOG Wine prefix?".into(),
                "This will permanently delete the isolated Wine prefix and all Wine Mono components. Downloaded GOG games will stay on disk but cannot launch until the prefix is re-created.".into(),
                "Remove",
            ),
            SharpConfirm::DisconnectGog => (
                "Disconnect GOG?".into(),
                "Disconnect GOG and show Login again? Installed games will stay on disk.".into(),
                "Disconnect",
            ),
            SharpConfirm::UninstallGog(_, title) => (format!("Delete {title} from disk?"), String::new(), "Delete"),
            SharpConfirm::DisconnectEpic => (
                "Disconnect Epic Games?".into(),
                "Installed games and isolated game bottles will stay on disk.".into(),
                "Disconnect",
            ),
            SharpConfirm::UninstallEpic(_, title) => (
                format!("Uninstall {title}?"),
                "This deletes its downloaded Epic game files and isolated Wine bottle.".into(),
                "Uninstall",
            ),
            SharpConfirm::UninstallGameJolt(_, name, _) => (
                format!("Uninstall {name}?"),
                "This removes its GameJolt folder and cannot be undone.".into(),
                "Uninstall",
            ),
        }
    }

    /// Header Refresh/Sync (`refreshCurrentSource` / `syncGameJolt`).
    pub(super) fn refresh_current_source(&mut self, cx: &mut Context<Self>) {
        match self.state.source {
            SharpSource::GameJolt => self.sync_gamejolt(true, cx),
            SharpSource::Epic => {
                if self
                    .live_mut()
                    .epic_status
                    .as_ref()
                    .is_some_and(|s| b(s, "authenticated"))
                {
                    self.sync_epic(cx);
                } else {
                    self.refresh_epic(false, cx);
                    toast::success(cx, "Epic status refreshed");
                }
            }
            SharpSource::Gog => {
                if self
                    .live_mut()
                    .gog_status
                    .as_ref()
                    .is_some_and(|s| b(s, "authenticated"))
                {
                    self.sync_gog(cx);
                } else {
                    self.refresh_gog(cx);
                    toast::success(cx, "GOG status refreshed");
                }
            }
            _ => {
                self.load(cx);
                toast::success(cx, "Sharp Library refreshed");
            }
        }
    }

    // ─────────────────────── shared launch preferences ───────────────────────

    /// GameLaunchSettingsPopover `refreshSettings`.
    pub(super) fn load_launch_preferences(&mut self, cx: &mut Context<Self>) {
        live::call(
            cx,
            "GET",
            "/config",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let p = &mut this.state.launch_preferences;
                    p[1] = match s(&r, "controllerInput") {
                        "x" => 1,
                        "d" => 2,
                        _ => 0,
                    };
                    p[2] = usize::from(r.get("msync").and_then(Value::as_bool) != Some(false));
                    p[3] = match s(&r, "windowMode") {
                        "windowed" => 1,
                        "fullscreen" => 2,
                        _ => 0,
                    };
                    p[4] = match s(&r, "gameResolution") {
                        "1280x720" => 1,
                        "1920x1080" => 2,
                        "2560x1440" => 3,
                        "3840x2160" => 4,
                        _ => 0,
                    };
                    cx.notify();
                }
            },
        );
        live::call(
            cx,
            "GET",
            "/metalfx/state",
            None,
            live::DEFAULT_TIMEOUT,
            |this, r, cx| {
                if let Some(r) = r.filter(is_ok) {
                    let factor = r.get("factor").and_then(Value::as_f64).unwrap_or(2.0);
                    this.state.launch_preferences[0] =
                        if r.get("enabled").and_then(Value::as_bool) == Some(false) {
                            2
                        } else if (factor - 1.75).abs() < 0.01 {
                            0
                        } else {
                            1
                        };
                    cx.notify();
                }
            },
        );
    }

    pub(super) fn save_launch_preference(
        &mut self,
        index: usize,
        choice: usize,
        cx: &mut Context<Self>,
    ) {
        if self.live.is_none() {
            self.state.launch_preferences[index] = choice;
            cx.notify();
            return;
        }
        if index == 0 {
            let body = match choice {
                0 => json!({"enabled": true, "factor": 1.75}),
                1 => json!({"enabled": true, "factor": 2.0}),
                _ => json!({"enabled": false}),
            };
            live::call(
                cx,
                "POST",
                "/metalfx/toggle",
                Some(body),
                live::DEFAULT_TIMEOUT,
                move |this, r, cx| {
                    if r.as_ref().is_some_and(is_ok) {
                        this.state.launch_preferences[0] = choice;
                    } else {
                        toast::error(
                            cx,
                            error_text(r.as_ref())
                                .unwrap_or_else(|| "Could not save MetalFX setting".into()),
                        );
                    }
                    cx.notify();
                },
            );
            return;
        }
        let patch = match index {
            1 => {
                let value = ["off", "x", "d"][choice.min(2)];
                json!({"controllerInput": value})
            }
            2 => json!({"msync": choice == 1}),
            3 => {
                let value = ["default", "windowed", "fullscreen"][choice.min(2)];
                json!({"windowMode": value})
            }
            _ => {
                let value =
                    ["default", "1280x720", "1920x1080", "2560x1440", "3840x2160"][choice.min(4)];
                json!({"gameResolution": value})
            }
        };
        live::call(
            cx,
            "POST",
            "/config",
            Some(patch),
            live::DEFAULT_TIMEOUT,
            move |this, r, cx| {
                if r.as_ref().is_some_and(is_ok) {
                    this.state.launch_preferences[index] = choice;
                } else {
                    toast::error(
                        cx,
                        error_text(r.as_ref())
                            .unwrap_or_else(|| "Could not save game launch settings".into()),
                    );
                }
                cx.notify();
            },
        );
    }
}
