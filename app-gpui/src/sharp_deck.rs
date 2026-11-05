//! Installed Sharp Library games for the app's GOG, Epic and Windows Programs
//! pages. Play and Stop go through the same Sharp Library actions as the
//! Collection view, so launches, engines and running state stay in one place.
use super::*;
use serde_json::Value;

/// One launchable entry on a deck page.
#[derive(Clone, Debug, PartialEq)]
pub struct DeckGame {
    /// "gog", "epic", "windows" or "gamejolt" (`LibGame::source`).
    pub source: &'static str,
    pub id: String,
    pub name: String,
    pub running: bool,
    pub busy: bool,
    /// Owned GOG and Epic games are listed before they are installed.
    pub installed: bool,
    /// Download progress, 0–100, while installing.
    pub progress: Option<f64>,
    /// Artwork cache key the Sharp Library already resolves for its cards.
    pub art_key: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GateStep {
    Prefix,
    Login,
    Sync,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeckGate {
    pub heading: &'static str,
    pub description: &'static str,
    pub button: &'static str,
    pub busy: bool,
    pub step: GateStep,
}

fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn flag(value: &Value, key: &str) -> bool {
    value.get(key).and_then(Value::as_bool) == Some(true)
}

/// Setup and installer executables stay in Collection; the deck lists what
/// they installed (the backend's `installer_like`).
pub fn installer_like(exe: &str) -> bool {
    let base = exe.rsplit(['/', '\\']).next().unwrap_or(exe).to_lowercase();
    base.contains("setup") || base.contains("install") || base.ends_with(".msi")
}

impl SharpPreview {
    pub fn deck_games(&self) -> Vec<DeckGame> {
        let Some(live) = self.live.as_ref() else {
            return Vec::new();
        };
        let mut games = Vec::new();
        for app in &live.apps {
            let id = text(app, "id");
            let adopted = !text(app, "adopted_from").is_empty();
            if id.is_empty() || (!adopted && installer_like(text(app, "exe_path"))) {
                continue;
            }
            games.push(DeckGame {
                source: "windows",
                id: id.to_owned(),
                name: text(app, "name").to_owned(),
                running: live.sharp_running.contains_key(id),
                busy: false,
                installed: true,
                progress: None,
                art_key: format!("sharp-app-{id}"),
            });
        }
        for game in live
            .gj_games
            .iter()
            .filter(|g| !text(g, "exe_path").is_empty())
        {
            let id = text(game, "id");
            games.push(DeckGame {
                source: "gamejolt",
                id: id.to_owned(),
                name: text(game, "name").to_owned(),
                running: live.gj_running.contains_key(id),
                busy: false,
                installed: true,
                progress: None,
                art_key: format!("sharp-gamejolt-{id}"),
            });
        }
        // Every owned GOG and Epic game, installed or not, like the
        // Sharp Library view; Play installs the ones that aren't.
        for game in &live.gog_games {
            let id = text(game, "productId");
            let busy = [":play", ":install"]
                .iter()
                .any(|step| live.gog_loading.contains(&format!("{id}{step}")));
            games.push(DeckGame {
                source: "gog",
                id: id.to_owned(),
                name: text(game, "title").to_owned(),
                running: flag(game, "running"),
                busy,
                installed: flag(game, "installed"),
                progress: live.gog_progress.get(id).copied(),
                art_key: format!("sharp-gog-{id}"),
            });
        }
        for game in &live.epic_games {
            let id = text(game, "appName");
            let busy = [":play", ":install"]
                .iter()
                .any(|step| live.epic_loading.contains(&format!("{id}{step}")));
            games.push(DeckGame {
                source: "epic",
                id: id.to_owned(),
                name: text(game, "title").to_owned(),
                running: flag(game, "running"),
                busy,
                installed: flag(game, "installed"),
                progress: live.epic_progress.get(id).copied(),
                art_key: format!("sharp-epic-{id}"),
            });
        }
        games.retain(|game| !game.id.is_empty());
        games
    }

    fn deck_entry(&self, source: &str, id: &str) -> Option<Value> {
        let live = self.live.as_ref()?;
        let (list, key) = match source {
            "windows" => (&live.apps, "id"),
            "gamejolt" => (&live.gj_games, "id"),
            "gog" => (&live.gog_games, "productId"),
            "epic" => (&live.epic_games, "appName"),
            _ => return None,
        };
        list.iter().find(|entry| text(entry, key) == id).cloned()
    }

    /// The setup step a GOG or Epic page shows in its middle until the
    /// account's games can be listed: Create Prefix, then Login, then Sync.
    pub fn deck_gate(&self, source: &str) -> Option<DeckGate> {
        let live = self.live.as_ref()?;
        match source {
            "gog" => {
                let status = live.gog_status.as_ref()?;
                if !flag(status, "prefixInitialized") {
                    Some(DeckGate {
                        heading: "Create the GOG prefix",
                        description: "MetalSharp keeps GOG games in their own Wine prefix.",
                        button: "Create Prefix",
                        busy: live.gog_loading.contains("setup"),
                        step: GateStep::Prefix,
                    })
                } else if !flag(status, "gogdlAvailable") {
                    // A prefix without gogdl (its install failed): setup installs it, keeping the prefix.
                    Some(DeckGate {
                        heading: "Finish GOG setup",
                        description: "GOG support didn't finish installing. MetalSharp will try again.",
                        button: "Finish Setup",
                        busy: live.gog_loading.contains("setup"),
                        step: GateStep::Prefix,
                    })
                } else if !flag(status, "authenticated") {
                    Some(DeckGate {
                        heading: "Log in to GOG",
                        description: "MetalSharp captures the GOG login code from a sign-in window, then lists your games.",
                        button: "Login",
                        busy: live.gog_loading.contains("login"),
                        step: GateStep::Login,
                    })
                } else if live.gog_games.is_empty() {
                    Some(DeckGate {
                        heading: "No GOG games yet",
                        description: "Sync after adding games to your GOG account.",
                        button: "Sync Library",
                        busy: live.gog_loading.contains("sync"),
                        step: GateStep::Sync,
                    })
                } else {
                    None
                }
            }
            "epic" => {
                let status = live.epic_status.as_ref()?;
                if !flag(status, "toolAvailable") {
                    Some(DeckGate {
                        heading: "Create the Epic prefix",
                        description: "Sets up the open-source Legendary client MetalSharp uses for your Epic library; each game gets its own prefix when installed.",
                        button: "Create Prefix",
                        busy: live.epic_loading.contains("tool"),
                        step: GateStep::Prefix,
                    })
                } else if !flag(status, "authenticated") {
                    Some(DeckGate {
                        heading: "Log in to Epic Games",
                        description: "Sign in through Epic's website. MetalSharp stores the account state only under ~/.metalsharp.",
                        button: "Login",
                        busy: live.epic_loading.contains("login"),
                        step: GateStep::Login,
                    })
                } else if live.epic_games.is_empty() {
                    Some(DeckGate {
                        heading: "No Epic games yet",
                        description: "Sync after adding games to your Epic account. Third-party launcher-only titles are left out.",
                        button: "Sync Library",
                        busy: live.epic_loading.contains("sync"),
                        step: GateStep::Sync,
                    })
                } else {
                    None
                }
            }
            _ => None,
        }
    }

    pub fn run_gate(&mut self, source: &str, step: GateStep, cx: &mut Context<Self>) {
        match (source, step) {
            ("gog", GateStep::Prefix) => self.initialize_gog_prefix(cx),
            ("gog", GateStep::Login) => self.gog_auth_button(cx),
            ("gog", GateStep::Sync) => self.sync_gog(cx),
            ("epic", GateStep::Prefix) => self.install_epic_support(cx, false),
            ("epic", GateStep::Login) => self.epic_auth_button(cx),
            ("epic", GateStep::Sync) => self.sync_epic(cx),
            _ => {}
        }
        cx.notify();
    }

    /// Reload every source after a launcher was uninstalled.
    pub fn reload(&mut self, cx: &mut Context<Self>) {
        if self.live.is_some() {
            self.load(cx);
        }
    }

    /// Install a Program: pick a Windows installer or program to run.
    pub fn deck_install_program(&mut self, cx: &mut Context<Self>) {
        self.install_exe(cx);
    }

    /// Download an owned GOG or Epic game (asks for its folder).
    pub fn deck_install(&mut self, source: &str, id: &str, cx: &mut Context<Self>) {
        let Some(entry) = self.deck_entry(source, id) else {
            return;
        };
        match source {
            "gog" => self.install_gog_game(entry, cx),
            "epic" => self.install_epic_game(entry, cx),
            _ => {}
        }
    }

    /// Play, or Stop when it is running.
    pub fn deck_toggle(&mut self, source: &str, id: &str, running: bool, cx: &mut Context<Self>) {
        let Some(entry) = self.deck_entry(source, id) else {
            return;
        };
        match (source, running) {
            ("windows", false) => self.launch_app(entry, cx),
            ("windows", true) => self.stop_app(entry, cx),
            ("gamejolt", false) => self.launch_gamejolt(entry, cx),
            ("gamejolt", true) => self.stop_gamejolt(entry, cx),
            ("gog", false) => self.play_gog_game(entry, cx),
            ("gog", true) => self.stop_gog_game(entry, cx),
            ("epic", false) => self.play_epic_game(entry, cx),
            ("epic", true) => self.stop_epic_game(entry, cx),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::installer_like;

    #[test]
    fn setup_executables_stay_out_of_the_deck() {
        assert!(installer_like("/Users/a/Downloads/StellaSoraSetup.exe"));
        assert!(installer_like("C:\\Temp\\install_game.exe"));
        assert!(installer_like("/Users/a/Downloads/Tool-x64.MSI"));
        assert!(!installer_like("/prefix/drive_c/Games/Celeste/Celeste.exe"));
    }
}
