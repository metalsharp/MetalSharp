//! Library data model and the pure rules ported from the Electron renderer
//! (`App.vue` loadLibrary merge, `LibraryView.vue` ordering/pipeline helpers).
use serde::{Deserialize, Deserializer};
use serde_json::Value;
use std::collections::{HashMap, HashSet};

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
}

impl LibGame {
    pub fn is_ubisoft(&self) -> bool {
        self.source.as_deref() == Some("ubisoft")
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
    /// Library identity used for selection: Steam and Ubisoft app ids live in
    /// different namespaces in the backend.
    pub fn key(&self) -> (bool, u64) {
        (self.is_ubisoft(), self.appid)
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

/// App.vue `loadLibrary` merge: prefer an installed Ubisoft copy over an
/// uninstalled Steam ownership entry of the same title, then append distinct
/// Ubisoft games.
pub fn merge_libraries(steam: Vec<LibGame>, ubisoft: Vec<LibGame>) -> Vec<LibGame> {
    let norm = |name: &str| name.trim().to_lowercase();
    let installed_ubisoft: HashSet<String> = ubisoft
        .iter()
        .filter(|game| game.installed)
        .map(|game| norm(&game.name))
        .collect();
    let steam_games: Vec<LibGame> = steam
        .into_iter()
        .filter(|game| !(!game.installed && installed_ubisoft.contains(&norm(&game.name))))
        .collect();
    let mut seen: HashSet<String> = steam_games.iter().map(|game| norm(&game.name)).collect();
    let mut games = steam_games;
    for mut game in ubisoft {
        let key = norm(&game.name);
        if seen.contains(&key) {
            continue;
        }
        seen.insert(key);
        if game.source.is_none() {
            game.source = Some("ubisoft".into());
        }
        games.push(game);
    }
    games
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
    if PIPELINE_OPTIONS.iter().any(|(pid, _)| *pid == id) {
        id.to_owned()
    } else {
        "auto".into()
    }
}

/// `gamePipelineOptions`: Ubisoft titles expose backend-provided options.
pub fn game_pipeline_options(game: &LibGame) -> Vec<(String, String)> {
    if !game.is_ubisoft() {
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
    let options = game_pipeline_options(game);
    [effective.to_owned(), recommended]
        .into_iter()
        .find(|id| options.iter().any(|(option, _)| option == id))
        .or_else(|| options.first().map(|(id, _)| id.clone()))
        .unwrap_or_else(|| "d3dmetal".into())
}

pub fn is_wine_steam_route(launch_method: &str) -> bool {
    [
        "d3dmetal",
        "vkd3d",
        "d3d9",
        "dxmt",
        "dxmt_32",
        "steam",
        "wine_steam",
    ]
    .contains(&launch_method.to_lowercase().as_str())
}

/// LibraryView `launchGame` endpoint + body selection.
pub fn launch_request(game: &LibGame) -> (&'static str, Value) {
    let launch_method = if game.launch_method().is_empty() {
        "auto".to_owned()
    } else {
        game.launch_method().to_owned()
    };
    if game.is_ubisoft() {
        let pipeline = if !game.preferred_pipeline().is_empty() {
            Value::String(game.preferred_pipeline().to_owned())
        } else if !game.launch_method().is_empty() {
            Value::String(game.launch_method().to_owned())
        } else {
            Value::Null
        };
        return (
            "/ubisoft/launch-game",
            serde_json::json!({"appid": game.appid, "ubisoft_id": game.ubisoft_id, "pipeline": pipeline}),
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
        endpoint,
        serde_json::json!({"appid": game.appid, "launchMethod": launch_method}),
    )
}

/// LibraryView `savePipeline` / `saveCollectionPipeline` route selection.
pub fn pipeline_save_request(game: &LibGame, pipeline: &str) -> (&'static str, Value, u64) {
    if game.is_ubisoft() {
        (
            "/ubisoft/save-pipeline",
            serde_json::json!({"ubisoft_id": game.ubisoft_id, "pipeline": pipeline}),
            30_000,
        )
    } else if pipeline == "d3dmetal" {
        (
            "/d3dmetal/bottles/save",
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
            "/bottles/edit",
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
    fn merge_prefers_installed_ubisoft_copy_and_dedupes() {
        let steam = vec![
            game(json!({"appid":1,"name":"Far Cry 5","installed":false})),
            game(json!({"appid":2,"name":"Portal","installed":true})),
        ];
        let ubisoft = vec![
            game(
                json!({"appid":900,"name":"Far Cry 5","installed":true,"source":"ubisoft","ubisoft_id":5}),
            ),
            game(json!({"appid":901,"name":"portal ","installed":true,"source":"ubisoft"})),
        ];
        let merged = merge_libraries(steam, ubisoft);
        let names: Vec<_> = merged.iter().map(|g| (g.appid, g.is_ubisoft())).collect();
        assert_eq!(names, vec![(2, false), (900, true)]);
        assert_eq!(merged[1].ubisoft_id.as_deref(), Some("5"));
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
                "/game/launch-auto",
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
                "/ubisoft/launch-game",
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
}
