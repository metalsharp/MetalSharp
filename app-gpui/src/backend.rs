use serde::Deserialize;
use serde_json::Value;
use std::{thread, time::Duration};
use ureq::Agent;

#[derive(Clone)]
pub struct BackendClient {
    base_url: String,
    agent: Agent,
}

#[derive(Clone, Debug)]
pub struct BackendStatus {
    pub ok: bool,
    pub version: Option<String>,
    pub message: String,
}

#[derive(Clone, Debug)]
pub struct LibrarySnapshot {
    pub count: usize,
    pub installed_count: usize,
    pub examples: Vec<String>,
}

#[derive(Deserialize)]
struct StatusResponse {
    #[serde(default)]
    ok: bool,
    version: Option<String>,
}

impl BackendClient {
    pub fn new(base_url: String) -> Self {
        let config = Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(20)))
            .build();
        Self {
            base_url,
            agent: config.into(),
        }
    }

    pub fn status(&self) -> BackendStatus {
        match self.get_json("/status") {
            Ok(value) => {
                let status: StatusResponse =
                    serde_json::from_value(value).unwrap_or(StatusResponse {
                        ok: false,
                        version: None,
                    });
                BackendStatus {
                    ok: status.ok,
                    version: status.version,
                    message: if status.ok {
                        "Connected to the local MetalSharp backend".into()
                    } else {
                        "Backend responded, but reports an unhealthy status".into()
                    },
                }
            }
            Err(error) => BackendStatus {
                ok: false,
                version: None,
                message: error,
            },
        }
    }

    pub fn steam_library(&self) -> Result<LibrarySnapshot, String> {
        parse_library(self.get_json("/steam/library")?)
    }

    fn get_json(&self, path: &str) -> Result<Value, String> {
        let mut last_error = None;
        for attempt in 0..3 {
            match self.agent.get(format!("{}{}", self.base_url, path)).call() {
                Ok(mut response) => {
                    return response
                        .body_mut()
                        .read_json::<Value>()
                        .map_err(|error| format!("Invalid backend JSON response: {error}"));
                }
                Err(error) => {
                    last_error = Some(error.to_string());
                    if attempt < 2 {
                        thread::sleep(Duration::from_millis(150));
                    }
                }
            }
        }
        Err(format!(
            "Backend request failed: {}",
            last_error.unwrap_or_else(|| "unknown error".into())
        ))
    }
}

fn parse_library(value: Value) -> Result<LibrarySnapshot, String> {
    let payload = value.get("data").unwrap_or(&value);
    let games = payload
        .get("games")
        .and_then(Value::as_array)
        .ok_or_else(|| "Backend response did not contain a games array".to_string())?;
    let installed_count = payload
        .get("installed_count")
        .and_then(Value::as_u64)
        .map(|count| count as usize)
        .unwrap_or_else(|| {
            games
                .iter()
                .filter(|game| game.get("installed").and_then(Value::as_bool) == Some(true))
                .count()
        });
    let examples = games
        .iter()
        .take(12)
        .map(|game| {
            game.get("name")
                .and_then(Value::as_str)
                .unwrap_or("Unnamed game")
                .to_string()
        })
        .collect();
    Ok(LibrarySnapshot {
        count: games.len(),
        installed_count,
        examples,
    })
}

#[cfg(test)]
mod tests {
    use super::parse_library;
    use serde_json::json;

    #[test]
    fn parses_direct_backend_library_payload() {
        let snapshot = parse_library(json!({
            "ok": true,
            "total": 2,
            "installed_count": 1,
            "games": [
                { "name": "Installed", "installed": true },
                { "name": "Not installed", "installed": false }
            ]
        }))
        .unwrap();

        assert_eq!(snapshot.count, 2);
        assert_eq!(snapshot.installed_count, 1);
        assert_eq!(snapshot.examples, ["Installed", "Not installed"]);
    }

    #[test]
    fn parses_electron_bridge_wrapped_library_payload() {
        let snapshot = parse_library(json!({
            "ok": true,
            "data": {
                "games": [{ "name": "Wrapped game", "installed": true }]
            }
        }))
        .unwrap();

        assert_eq!(snapshot.count, 1);
        assert_eq!(snapshot.installed_count, 1);
        assert_eq!(snapshot.examples, ["Wrapped game"]);
    }

    #[test]
    fn rejects_a_malformed_library_response() {
        let error = parse_library(json!({ "ok": true, "games": null })).unwrap_err();
        assert!(error.contains("games array"));
    }
}
