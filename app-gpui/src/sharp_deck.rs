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
    /// Artwork cache key the Sharp Library already resolves for its cards.
    pub art_key: String,
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
    base.contains("setup") || base.contains("install")
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
                art_key: format!("sharp-gamejolt-{id}"),
            });
        }
        for game in live.gog_games.iter().filter(|g| flag(g, "installed")) {
            let id = text(game, "productId");
            games.push(DeckGame {
                source: "gog",
                id: id.to_owned(),
                name: text(game, "title").to_owned(),
                running: flag(game, "running"),
                busy: live.gog_loading.contains(&format!("{id}:play")),
                art_key: format!("sharp-gog-{id}"),
            });
        }
        for game in live.epic_games.iter().filter(|g| flag(g, "installed")) {
            let id = text(game, "appName");
            games.push(DeckGame {
                source: "epic",
                id: id.to_owned(),
                name: text(game, "title").to_owned(),
                running: flag(game, "running"),
                busy: live.epic_loading.contains(&format!("{id}:play")),
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
        assert!(!installer_like("/prefix/drive_c/Games/Celeste/Celeste.exe"));
    }
}
