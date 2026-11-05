//! Library data model and the pure rules ported from the Electron renderer
//! (`App.vue` loadLibrary merge, `LibraryView.vue` ordering/pipeline helpers).
use serde::{Deserialize, Deserializer};
use serde_json::Value;
use std::collections::HashMap;

fn lenient_string<'de, D: Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    Ok(match Value::deserialize(d)? {
        Value::String(s) => Some(s),
        Value::Number(n) => Some(n.to_string()),
        _ => None,
    })
}

fn lenient_bool<'de, D: Deserializer<'de>>(d: D) -> Result<bool, D::Error> {
    Ok(matches!(Value::deserialize(d)?, Value::Bool(true)))
}

fn lenient_u64<'de, D: Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    Ok(match Value::deserialize(d)? {
        Value::Number(n) => n
            .as_u64()
            .or_else(|| n.as_f64().map(|f| f.max(0.0) as u64))
            .unwrap_or(0),
        Value::String(s) => s.parse().unwrap_or(0),
        _ => 0,
    })
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct PipelineOption {
    #[serde(default, deserialize_with = "lenient_string")]
    pub id: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub name: Option<String>,
    #[serde(default, deserialize_with = "lenient_bool")]
    pub recommended: bool,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
pub struct LibGame {
    #[serde(default, deserialize_with = "lenient_u64")]
    pub appid: u64,
    #[serde(default)]
    pub name: String,
    #[serde(default, deserialize_with = "lenient_bool")]
    pub installed: bool,
    #[serde(default, deserialize_with = "lenient_string")]
    pub state: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub cover_url: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub header_url: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub launch_method: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub launch_method_name: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub preferred_pipeline: Option<String>,
    #[serde(default)]
    pub available_pipelines: Option<Vec<PipelineOption>>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub wine_game_path: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub executable_path: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub bottle_id: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub embedded_icon_path: Option<String>,
    #[serde(default, deserialize_with = "lenient_bool")]
    pub icon_pending: bool,
    #[serde(default, deserialize_with = "lenient_string")]
    pub ubisoft_artwork_url: Option<String>,
    #[serde(default, deserialize_with = "lenient_bool")]
    pub has_native_build: bool,
    #[serde(default, deserialize_with = "lenient_string")]
    pub source: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub ubisoft_id: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub rockstar_id: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub battlenet_id: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub game_dir: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub last_played_at: Option<String>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub last_played: Option<String>,
    #[serde(default, deserialize_with = "lenient_u64")]
    pub playtime_2weeks: u64,
    #[serde(default, deserialize_with = "lenient_u64")]
    pub playtime_forever: u64,
    /// Bundled preview artwork (offline preview only).
    #[serde(skip)]
    pub preview_art: Option<(&'static str, &'static str)>,
    /// Sharp Library id of a GOG, Epic, Windows program or GameJolt entry.
    #[serde(skip)]
    pub external_id: Option<String>,
    /// Artwork cache key resolved by the Sharp Library.
    #[serde(skip)]
    pub art_key: Option<String>,
}

impl LibGame {
    /// A game from a launcher MetalSharp runs itself (Ubisoft Connect, the
    /// Rockstar Games Launcher, Battle.net): the backend lists its graphics
    /// options and launches it through `/<launcher>/launch-game`.
    pub fn is_launcher_game(&self) -> bool {
        matches!(
            self.page(),
            LibraryPage::Ubisoft | LibraryPage::Rockstar | LibraryPage::BattleNet
        )
    }
    /// The launcher's route prefix and this game's id field and value.
    fn launcher_route(&self) -> Option<(&'static str, &'static str, &Option<String>)> {
        match self.page() {
            LibraryPage::Ubisoft => Some(("ubisoft", "ubisoft_id", &self.ubisoft_id)),
            LibraryPage::Rockstar => Some(("rockstar", "rockstar_id", &self.rockstar_id)),
            LibraryPage::BattleNet => Some(("battlenet", "battlenet_id", &self.battlenet_id)),
            _ => None,
        }
    }
    /// Endpoint and body that save this game's chosen executable.
    pub fn save_executable_request(&self, path: &str) -> (String, Value) {
        match self.launcher_route() {
            Some((route, key, id)) => (
                format!("/{route}/save-executable"),
                serde_json::json!({key: id, "executablePath": path}),
            ),
            None => (
                "/steam/save-executable".into(),
                serde_json::json!({"appid": self.appid, "executablePath": path}),
            ),
        }
    }
    /// A non-Steam game the user added to Wine Steam (shortcuts.vdf).
    pub fn is_shortcut(&self) -> bool {
        self.source.as_deref() == Some("steam_shortcut")
    }
    pub fn str_or<'a>(value: &'a Option<String>) -> &'a str {
        value.as_deref().unwrap_or("")
    }
    pub fn launch_method(&self) -> &str {
        Self::str_or(&self.launch_method)
    }
    pub fn preferred_pipeline(&self) -> &str {
        Self::str_or(&self.preferred_pipeline)
    }
    pub fn bottle_id(&self) -> String {
        self.bottle_id
            .clone()
            .filter(|id| !id.is_empty())
            .unwrap_or_else(|| format!("steam_{}", self.appid))
    }
    /// Library identity used for selection: each launcher's app ids live in
    /// their own namespace in the backend.
    pub fn key(&self) -> (LibraryPage, u64) {
        (self.page(), self.appid)
    }
    /// The sidebar page this game is listed on. Steam copies of games that
    /// run through Ubisoft Connect or the Rockstar launcher stay on Steam.
    pub fn page(&self) -> LibraryPage {
        match self.source.as_deref() {
            Some("ubisoft") => LibraryPage::Ubisoft,
            Some("battlenet") => LibraryPage::BattleNet,
            Some("rockstar") => LibraryPage::Rockstar,
            Some("gog") => LibraryPage::Gog,
            Some("epic") => LibraryPage::Epic,
            Some("windows" | "gamejolt") => LibraryPage::WindowsPrograms,
            _ => LibraryPage::Steam,
        }
    }
}

/// Sidebar pages, in sidebar order. Logs is pinned to the sidebar's bottom.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum LibraryPage {
    #[default]
    Steam,
    Ubisoft,
    BattleNet,
    Rockstar,
    Gog,
    Epic,
    WindowsPrograms,
    Logs,
}

impl LibraryPage {
    pub const LAUNCHERS: [Self; 7] = [
        Self::Steam,
        Self::Ubisoft,
        Self::BattleNet,
        Self::Rockstar,
        Self::Gog,
        Self::Epic,
        Self::WindowsPrograms,
    ];
    pub fn label(self) -> &'static str {
        match self {
            Self::Steam => "Steam",
            Self::Ubisoft => "Ubisoft",
            Self::BattleNet => "Battle.net",
            Self::Rockstar => "Rockstar",
            Self::Gog => "GOG",
            Self::Epic => "Epic",
            Self::WindowsPrograms => "Windows Programs",
            Self::Logs => "Logs",
        }
    }
    pub fn icon(self) -> &'static str {
        match self {
            Self::Steam => "icon-steam.svg",
            Self::Ubisoft => "icon-ubisoft.svg",
            Self::BattleNet => "icon-battlenet.svg",
            Self::Rockstar => "icon-rockstar.svg",
            Self::Gog => "source-gog.svg",
            Self::Epic => "source-epic.svg",
            Self::WindowsPrograms => "icon-windows.svg",
            Self::Logs => "icon-logs.svg",
        }
    }
    pub fn storage_id(self) -> &'static str {
        match self {
            Self::Steam => "steam",
            Self::Ubisoft => "ubisoft",
            Self::BattleNet => "battlenet",
            Self::Rockstar => "rockstar",
            Self::Gog => "gog",
            Self::Epic => "epic",
            Self::WindowsPrograms => "windows",
            Self::Logs => "logs",
        }
    }
    pub fn from_storage_id(id: &str) -> Option<Self> {
        Self::LAUNCHERS
            .into_iter()
            .chain([Self::Logs])
            .find(|page| page.storage_id() == id)
    }
    /// Pages whose Collection view is the Sharp Library source view.
    pub fn sharp_source(self) -> Option<&'static str> {
        match self {
            Self::Gog => Some("gog"),
            Self::Epic => Some("epic"),
            Self::WindowsPrograms => Some("installers"),
            _ => None,
        }
    }
    /// Pages with a Library/Collection toggle.
    pub fn has_collection(self) -> bool {
        self == Self::Steam || self.sharp_source().is_some()
    }
}

pub fn parse_library(value: Option<&Value>) -> Option<Vec<LibGame>> {
    let value = value?;
    let games = value.get("games")?.as_array()?;
    Some(
        games
            .iter()
            .filter_map(|game| serde_json::from_value::<LibGame>(game.clone()).ok())
            .collect(),
    )
}

/// Steam and Ubisoft Connect games, each listed on its own page. A game
/// without a source is a Steam game.
pub fn merge_libraries(steam: Vec<LibGame>, ubisoft: Vec<LibGame>) -> Vec<LibGame> {
    let mut games = steam;
    games.extend(ubisoft.into_iter().map(|mut game| {
        if game.source.is_none() {
            game.source = Some("ubisoft".into());
        }
        game
    }));
    games
}

/// App id for a Sharp Library entry: the source in the top four bits keeps it
/// clear of Steam and Ubisoft ids and of the other sources.
pub fn external_appid(source: &str, id: &str) -> u64 {
    let tag: u64 = match source {
        "windows" => 0x9,
        "gamejolt" => 0xa,
        "gog" => 0xb,
        _ => 0xc,
    };
    let hash = id.bytes().fold(0xcbf2_9ce4_8422_2325u64, |hash, byte| {
        (hash ^ u64::from(byte)).wrapping_mul(0x0100_0000_01b3)
    });
    (tag << 60) | (hash & ((1 << 60) - 1))
}

fn parse_time(value: &Option<String>) -> i64 {
    // RFC3339-ish or epoch seconds; anything unparseable sorts as 0 like Date.parse NaN.
    let Some(value) = value.as_deref().filter(|v| !v.is_empty()) else {
        return 0;
    };
    if let Ok(n) = value.parse::<i64>() {
        return n * 1000;
    }
    0
}

/// LibraryView `liveGames`: installed only, ordered by local play history,
/// backend last-played, recent/total playtime, then name.
pub fn live_games(library: &[LibGame], history: &HashMap<String, i64>) -> Vec<LibGame> {
    let mut games: Vec<LibGame> = library.iter().filter(|g| g.installed).cloned().collect();
    games.sort_by(|a, b| {
        let ah = history.get(&a.appid.to_string()).copied().unwrap_or(0);
        let bh = history.get(&b.appid.to_string()).copied().unwrap_or(0);
        if ah != bh {
            return bh.cmp(&ah);
        }
        let al = parse_time(&a.last_played_at).max(parse_time(&a.last_played));
        let bl = parse_time(&b.last_played_at).max(parse_time(&b.last_played));
        if al != bl {
            return bl.cmp(&al);
        }
        let ap = a.playtime_2weeks * 100 + a.playtime_forever;
        let bp = b.playtime_2weeks * 100 + b.playtime_forever;
        bp.cmp(&ap)
            .then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });
    games
}

pub const PIPELINE_OPTIONS: [(&str, &str); 6] = [
    ("d3dmetal", "D3DMetal"),
    ("vkd3d", "VKD3D"),
    ("d3d9", "D3D9"),
    ("dxmt", "DXMT"),
    ("dxmt_32", "DXMT(32)"),
    ("fna_arm64", "Mono/FNA"),
];

pub fn pipeline_label(id: &str) -> String {
    if crate::variant::is_fex() {
        return crate::variant::fex_pipeline_label(id)
            .unwrap_or("Auto")
            .to_owned();
    }
    match id {
        "m9" | "dxvk" | "dxvk_32" => "D3D9".into(),
        _ => PIPELINE_OPTIONS
            .iter()
            .find(|(pid, _)| *pid == id)
            .map(|(_, label)| (*label).to_owned())
            .unwrap_or_else(|| "Auto".into()),
    }
}

pub fn normalize_pipeline(id: &str) -> String {
    if crate::variant::is_fex() {
        return match crate::variant::fex_pipeline_label(id) {
            Some(_) => crate::variant::fex_pipeline(id).to_owned(),
            None => "auto".into(),
        };
    }
    if PIPELINE_OPTIONS.iter().any(|(pid, _)| *pid == id) {
        id.to_owned()
    } else {
        "auto".into()
    }
}

/// `gamePipelineOptions`: Ubisoft titles expose backend-provided options.
/// On FEX the list is Mullion (DXMT), VKD3D-Proton, DXVK and OpenGL (`variant::FEX_PIPELINE_OPTIONS`);
/// DXVK is for x86_64 games only, which the backend checks at launch.
pub fn game_pipeline_options(game: &LibGame) -> Vec<(String, String)> {
    game_pipeline_options_for(game, crate::variant::is_fex())
}

fn game_pipeline_options_for(game: &LibGame, fex: bool) -> Vec<(String, String)> {
    if fex {
        let options: Vec<(String, String)> = if game.is_launcher_game() {
            game.available_pipelines
                .clone()
                .unwrap_or_default()
                .into_iter()
                .filter_map(|option| Some((option.id?, option.name.unwrap_or_default())))
                .collect()
        } else {
            crate::variant::FEX_PIPELINE_OPTIONS
                .iter()
                .map(|(id, label)| ((*id).to_owned(), (*label).to_owned()))
                .collect()
        };
        // The backend runs D3DMetal and GPTK on VKD3D-Proton and D3D9 on DXVK here: show each lane once.
        return crate::variant::fex_options(options);
    }
    if !game.is_launcher_game() {
        return PIPELINE_OPTIONS
            .iter()
            .map(|(id, label)| ((*id).to_owned(), (*label).to_owned()))
            .collect();
    }
    game.available_pipelines
        .clone()
        .unwrap_or_default()
        .into_iter()
        .filter_map(|option| Some((option.id?, option.name.unwrap_or_default())))
        .collect()
}

/// `collectionPipelineValue` / featured watcher selection.
pub fn effective_pipeline(game: &LibGame) -> String {
    effective_pipeline_for(game, crate::variant::is_fex())
}

fn effective_pipeline_for(game: &LibGame, fex: bool) -> String {
    let effective = if !game.preferred_pipeline().is_empty() {
        game.preferred_pipeline()
    } else {
        game.launch_method()
    };
    let recommended = game
        .available_pipelines
        .as_ref()
        .and_then(|options| options.iter().find(|option| option.recommended))
        .and_then(|option| option.id.clone())
        .unwrap_or_default();
    let options = game_pipeline_options_for(game, fex);
    let fex_id = |id: String| {
        if fex {
            crate::variant::fex_pipeline(&id).to_owned()
        } else {
            id
        }
    };
    [fex_id(effective.to_owned()), fex_id(recommended)]
        .into_iter()
        .find(|id| options.iter().any(|(option, _)| option == id))
        .or_else(|| options.first().map(|(id, _)| id.clone()))
        .unwrap_or_else(|| if fex { "dxmt" } else { "d3dmetal" }.into())
}

pub fn is_wine_steam_route(launch_method: &str) -> bool {
    let launch_method = launch_method.to_lowercase();
    // DXVK and Wine are FEX-only pipeline choices.
    if crate::variant::is_fex() && ["dxvk", "wine_bare"].contains(&launch_method.as_str()) {
        return true;
    }
    [
        "d3dmetal",
        "vkd3d",
        "d3d9",
        "dxmt",
        "dxmt_32",
        "steam",
        "wine_steam",
    ]
    .contains(&launch_method.as_str())
}

/// LibraryView `launchGame` endpoint + body selection.
pub fn launch_request(game: &LibGame) -> (String, Value) {
    let launch_method = if game.launch_method().is_empty() {
        "auto".to_owned()
    } else {
        game.launch_method().to_owned()
    };
    if let Some((route, key, id)) = game.launcher_route() {
        let pipeline = if !game.preferred_pipeline().is_empty() {
            Value::String(game.preferred_pipeline().to_owned())
        } else if !game.launch_method().is_empty() {
            Value::String(game.launch_method().to_owned())
        } else {
            Value::Null
        };
        return (
            format!("/{route}/launch-game"),
            serde_json::json!({"appid": game.appid, key: id, "pipeline": pipeline}),
        );
    }
    let endpoint = if game.has_native_build {
        "/steam/mac-launch-game"
    } else if is_wine_steam_route(&launch_method) {
        "/steam/launch-game"
    } else {
        "/game/launch-auto"
    };
    (
        endpoint.to_owned(),
        serde_json::json!({"appid": game.appid, "launchMethod": launch_method}),
    )
}

/// LibraryView `savePipeline` / `saveCollectionPipeline` route selection.
pub fn pipeline_save_request(game: &LibGame, pipeline: &str) -> (String, Value, u64) {
    if let Some((route, key, id)) = game.launcher_route() {
        (
            format!("/{route}/save-pipeline"),
            serde_json::json!({key: id, "pipeline": pipeline}),
            30_000,
        )
    } else if pipeline == "d3dmetal" {
        (
            "/d3dmetal/bottles/save".into(),
            serde_json::json!({
                "appid": game.appid,
                "bottleId": game.bottle_id(),
                "name": game.name,
                "gameDir": LibGame::str_or(&game.wine_game_path),
            }),
            10 * 60 * 1000,
        )
    } else {
        (
            "/bottles/edit".into(),
            serde_json::json!({"id": game.bottle_id(), "name": game.name, "preferredPipeline": pipeline}),
            30_000,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn game(value: Value) -> LibGame {
        serde_json::from_value(value).unwrap()
    }

    #[test]
    fn steam_shortcuts_launch_through_wine_steam() {
        let shortcut = game(json!({
            "appid": 3039101758u64, "name": "Soma", "installed": true,
            "source": "steam_shortcut", "gameid": "13052842659859660800",
            "launch_method": "steam", "cover_url": null
        }));
        assert!(shortcut.is_shortcut() && shortcut.page() == LibraryPage::Steam);
        assert_eq!(shortcut.key(), (LibraryPage::Steam, 3039101758));
        let (endpoint, body) = launch_request(&shortcut);
        assert_eq!(endpoint, "/steam/launch-game");
        assert_eq!(body["launchMethod"], "steam");
        assert_eq!(body["appid"], 3039101758u64);
        let routed = game(
            json!({"appid": 3039101758u64, "source": "steam_shortcut", "launch_method": "d3dmetal"}),
        );
        assert_eq!(launch_request(&routed).0, "/steam/launch-game");
        assert!(!game(json!({"appid": 400, "name": "Portal"})).is_shortcut());
    }

    #[test]
    fn steam_and_ubisoft_games_stay_on_their_own_pages() {
        let steam = vec![
            game(json!({"appid":1,"name":"Far Cry 5","installed":false})),
            game(json!({"appid":2,"name":"Portal","installed":true})),
            game(json!({"appid":3,"name":"Soma","installed":true,"source":"steam_shortcut"})),
        ];
        let ubisoft = vec![
            game(json!({"appid":900,"name":"Far Cry 5","installed":true,"ubisoft_id":5})),
            game(json!({"appid":2,"name":"portal ","installed":true,"source":"ubisoft"})),
        ];
        let merged = merge_libraries(steam, ubisoft);
        let pages: Vec<_> = merged.iter().map(|g| (g.appid, g.page())).collect();
        assert_eq!(
            pages,
            vec![
                (1, LibraryPage::Steam),
                (2, LibraryPage::Steam),
                (3, LibraryPage::Steam),
                (900, LibraryPage::Ubisoft),
                (2, LibraryPage::Ubisoft),
            ]
        );
        assert_ne!(merged[1].key(), merged[4].key());
        assert_eq!(merged[3].ubisoft_id.as_deref(), Some("5"));
    }

    #[test]
    fn rockstar_and_battlenet_games_use_their_launcher_routes() {
        let rockstar = game(json!({"appid": 1073741825u64, "source": "rockstar",
            "rockstar_id": "gta5", "preferred_pipeline": "d3dmetal"}));
        assert!(rockstar.is_launcher_game());
        assert_eq!(
            launch_request(&rockstar),
            (
                "/rockstar/launch-game".to_owned(),
                json!({"appid": 1073741825u64, "rockstar_id": "gta5", "pipeline": "d3dmetal"})
            )
        );
        let battlenet = game(json!({"appid": 536870913u64, "source": "battlenet",
            "battlenet_id": "prometheus"}));
        let (path, body, _) = pipeline_save_request(&battlenet, "dxmt");
        assert_eq!(path, "/battlenet/save-pipeline");
        assert_eq!(
            body,
            json!({"battlenet_id": "prometheus", "pipeline": "dxmt"})
        );
        assert_eq!(
            battlenet.save_executable_request("/x/Overwatch.exe").0,
            "/battlenet/save-executable"
        );
        let steam = game(json!({"appid": 400}));
        assert!(!steam.is_launcher_game());
        assert_eq!(
            steam.save_executable_request("/x").0,
            "/steam/save-executable"
        );
    }

    #[test]
    fn external_ids_stay_apart_by_source() {
        let gog = external_appid("gog", "1207658924");
        assert_eq!(gog, external_appid("gog", "1207658924"));
        assert_ne!(gog, external_appid("epic", "1207658924"));
        assert_eq!(gog >> 60, 0xb);
        assert!(external_appid("windows", "sharp_1") > u64::from(u32::MAX));
    }

    #[test]
    fn sources_map_to_sidebar_pages() {
        for (source, page) in [
            ("battlenet", LibraryPage::BattleNet),
            ("rockstar", LibraryPage::Rockstar),
            ("gog", LibraryPage::Gog),
            ("epic", LibraryPage::Epic),
            ("windows", LibraryPage::WindowsPrograms),
            ("gamejolt", LibraryPage::WindowsPrograms),
        ] {
            assert_eq!(game(json!({"appid": 1, "source": source})).page(), page);
        }
        for page in LibraryPage::LAUNCHERS
            .into_iter()
            .chain([LibraryPage::Logs])
        {
            assert_eq!(LibraryPage::from_storage_id(page.storage_id()), Some(page));
        }
        assert!(LibraryPage::Steam.has_collection() && LibraryPage::Gog.has_collection());
        assert!(!LibraryPage::Ubisoft.has_collection() && !LibraryPage::Logs.has_collection());
    }

    #[test]
    fn synced_owned_games_remain_available_outside_the_installed_library() {
        let response = json!({"ok": true, "library": {"games": [
            {"appid": 400, "name": "Portal", "installed": true, "state": "installed"},
            {
                "appid": 620, "name": "Portal 2", "installed": false, "state": "not_installed",
                "cover_url": "https://steamcdn-a.akamaihd.net/steam/apps/620/library_600x900.jpg",
                "header_url": "https://steamcdn-a.akamaihd.net/steam/apps/620/header.jpg",
                "wine_game_path": null, "executable_path": null, "bottle_id": null
            }
        ]}});
        let library = merge_libraries(parse_library(response.get("library")).unwrap(), Vec::new());
        assert_eq!(library.len(), 2);
        let owned = &library[1];
        assert_eq!(owned.appid, 620);
        assert!(!owned.installed && owned.page() == LibraryPage::Steam);
        assert_eq!(owned.state.as_deref(), Some("not_installed"));
        assert_eq!(
            owned.cover_url,
            response["library"]["games"][1]["cover_url"]
                .as_str()
                .map(str::to_owned)
        );
        assert_eq!(
            owned.header_url,
            response["library"]["games"][1]["header_url"]
                .as_str()
                .map(str::to_owned)
        );
        assert!(owned.wine_game_path.is_none() && owned.executable_path.is_none());
        let installed = live_games(&library, &HashMap::new());
        assert_eq!(
            installed.iter().map(|game| game.appid).collect::<Vec<_>>(),
            [400]
        );
    }

    #[test]
    fn launch_routes_match_renderer() {
        let g = game(json!({"appid":10,"name":"x","launch_method":"dxmt"}));
        assert_eq!(launch_request(&g).0, "/steam/launch-game");
        let g = game(json!({"appid":10,"name":"x","launch_method":"mono"}));
        assert_eq!(launch_request(&g).0, "/game/launch-auto");
        let g = game(json!({"appid":10,"name":"x"}));
        assert_eq!(
            launch_request(&g),
            (
                "/game/launch-auto".to_owned(),
                json!({"appid":10,"launchMethod":"auto"})
            )
        );
        let g = game(json!({"appid":10,"name":"x","has_native_build":true}));
        assert_eq!(launch_request(&g).0, "/steam/mac-launch-game");
        let g = game(
            json!({"appid":10,"name":"x","source":"ubisoft","ubisoft_id":"77","preferred_pipeline":"dxmt"}),
        );
        assert_eq!(
            launch_request(&g),
            (
                "/ubisoft/launch-game".to_owned(),
                json!({"appid":10,"ubisoft_id":"77","pipeline":"dxmt"})
            )
        );
    }

    #[test]
    fn pipeline_saves_use_d3dmetal_route_or_bottle_edit() {
        let g = game(json!({"appid":10,"name":"G","wine_game_path":"C:/G"}));
        let (path, body, _) = pipeline_save_request(&g, "d3dmetal");
        assert_eq!(path, "/d3dmetal/bottles/save");
        assert_eq!(
            body,
            json!({"appid":10,"bottleId":"steam_10","name":"G","gameDir":"C:/G"})
        );
        let (path, body, _) = pipeline_save_request(&g, "dxmt");
        assert_eq!(path, "/bottles/edit");
        assert_eq!(
            body,
            json!({"id":"steam_10","name":"G","preferredPipeline":"dxmt"})
        );
        assert_eq!(
            effective_pipeline(&game(json!({"appid":1,"name":"a","launch_method":"vkd3d"}))),
            "vkd3d"
        );
        assert_eq!(
            effective_pipeline(&game(json!({"appid":1,"name":"a"}))),
            "d3dmetal"
        );
    }

    #[test]
    fn fex_lists_mullion_vkd3d_dxvk_opengl_and_maps_saved_rosetta_pipelines() {
        let g = game(json!({"appid":1,"name":"a","preferred_pipeline":"d3dmetal"}));
        let ids: Vec<String> = game_pipeline_options_for(&g, true)
            .into_iter()
            .map(|(id, _)| id)
            .collect();
        assert_eq!(ids, ["dxmt", "vkd3d", "dxvk", "wine_bare"]);
        assert_eq!(effective_pipeline_for(&g, true), "vkd3d");
        assert_eq!(effective_pipeline_for(&g, false), "d3dmetal");
        for (saved, lane) in [("m13", "vkd3d"), ("dxmt_32", "dxmt"), ("d3d9", "dxvk")] {
            let g = game(json!({"appid":1,"name":"a","launch_method":saved}));
            assert_eq!(effective_pipeline_for(&g, true), lane);
        }
        let g = game(json!({"appid":1,"name":"a","launch_method":"vkd3d"}));
        assert_eq!(effective_pipeline_for(&g, true), "vkd3d");
        let ubisoft = game(
            json!({"appid":2,"name":"u","source":"ubisoft","ubisoft_id":"7",
            "available_pipelines":[{"id":"d3dmetal","name":"D3DMetal"},{"id":"dxmt","name":"DXMT"},{"id":"d3d9","name":"D3D9"}]}),
        );
        assert_eq!(
            game_pipeline_options_for(&ubisoft, true),
            [
                ("vkd3d".to_owned(), "VKD3D-Proton".to_owned()),
                ("dxmt".to_owned(), "Mullion (DXMT)".to_owned()),
                ("dxvk".to_owned(), "DXVK".to_owned())
            ]
        );
        assert_eq!(
            game_pipeline_options_for(&g, false).len(),
            PIPELINE_OPTIONS.len()
        );
    }
}
