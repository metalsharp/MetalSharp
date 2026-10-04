//! Local C-backend transport. Requests run on the background executor, never the UI thread.
//! Mutating requests are sent exactly once: a timeout must not repeat an install/launch.
use serde::{Deserialize, de::DeserializeOwned};
use serde_json::{Value, json};
use std::time::Duration;
use ureq::Agent;

#[derive(Clone)]
pub struct BackendClient {
    base_url: String,
    agent: Agent,
}

#[derive(Clone, Debug, Deserialize)]
pub struct BackendStatus {
    pub ok: bool,
    pub version: Option<String>,
    pub pid: Option<u32>,
    pub metalsharp_home: Option<String>,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupState {
    #[serde(default)]
    pub completed: bool,
    #[serde(default)]
    pub step: usize,
    #[serde(default)]
    pub device_name: String,
    #[serde(default)]
    pub runtime_migration_required: bool,
}

#[derive(Clone, Debug, Default, Deserialize)]
pub struct InstallProgress {
    #[serde(default)]
    pub step: usize,
    #[serde(default)]
    pub total: usize,
    #[serde(default)]
    pub current: String,
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub log: String,
    pub error: Option<String>,
}
impl InstallProgress {
    pub fn percent(&self) -> usize {
        if self.total == 0 {
            0
        } else {
            self.step
                .saturating_mul(100)
                .checked_div(self.total)
                .unwrap_or(0)
                .min(100)
        }
    }
}

#[derive(Clone, Debug, Default, Deserialize)]
pub struct LauncherStatus {
    #[serde(default)]
    pub installed: bool,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub installing: bool,
    #[serde(default)]
    pub install_stage: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Launcher {
    Steam,
    Ubisoft,
}
impl Launcher {
    fn prefix(self) -> &'static str {
        match self {
            Self::Steam => "/steam",
            Self::Ubisoft => "/ubisoft",
        }
    }
}

// These errors deliberately exclude response bodies, request bodies, URLs and
// provider messages: any of those may contain API keys or authorization codes.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum BackendError {
    Transport,
    Http(u16),
    InvalidJson,
    Rejected,
    InvalidRoute,
    InvalidInput,
}
impl std::fmt::Display for BackendError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Transport => f.write_str(
                "Local backend unavailable or request timed out; refresh status before retrying",
            ),
            Self::Http(code) => write!(f, "Backend returned HTTP {code}"),
            Self::InvalidJson => f.write_str("Backend returned invalid response data"),
            Self::Rejected => {
                f.write_str("Backend rejected the operation; inspect redacted backend diagnostics")
            }
            Self::InvalidRoute => f.write_str("Invalid backend route"),
            Self::InvalidInput => f.write_str("Invalid operation input"),
        }
    }
}
impl std::error::Error for BackendError {}

#[derive(Clone, Debug, Deserialize)]
pub struct Game {
    pub appid: u64,
    pub name: String,
    #[serde(default)]
    pub installed: bool,
    #[serde(default)]
    pub has_native_build: bool,
    #[serde(default, deserialize_with = "nullable_string")]
    pub launch_method: String,
    #[serde(default, deserialize_with = "nullable_string")]
    pub preferred_pipeline: String,
    pub ubisoft_id: Option<String>,
    #[serde(default)]
    pub source: String,
}
fn nullable_string<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    Ok(Option::<String>::deserialize(deserializer)?.unwrap_or_default())
}
#[derive(Clone, Debug, Deserialize)]
pub struct Library {
    pub games: Vec<Game>,
    #[serde(default)]
    pub installed_count: usize,
}

impl BackendClient {
    pub fn for_port(port: u16) -> Result<Self, BackendError> {
        if port == 0 {
            return Err(BackendError::InvalidInput);
        }
        let config = Agent::config_builder()
            .timeout_global(Some(Duration::from_secs(30)))
            .proxy(None)
            .max_redirects(0)
            .http_status_as_error(false)
            .build();
        Ok(Self {
            base_url: format!("http://127.0.0.1:{port}"),
            agent: config.into(),
        })
    }
    pub fn base_url(&self) -> &str {
        &self.base_url
    }
    pub fn request(
        &self,
        method: &str,
        path: &str,
        body: Option<&Value>,
        timeout: Duration,
    ) -> Result<Value, BackendError> {
        if !path.starts_with('/')
            || path.starts_with("//")
            || path.contains(['?', '#', '\\'])
            || path.chars().any(char::is_control)
            || !matches!(method, "GET" | "POST")
        {
            return Err(BackendError::InvalidRoute);
        }
        let url = format!("{}{path}", self.base_url);
        let response = match method {
            "GET" => self
                .agent
                .get(&url)
                .config()
                .timeout_global(Some(timeout))
                .build()
                .call(),
            "POST" => {
                // C HTTP server expects Content-Length, not chunked transfer.
                let payload = serde_json::to_vec(body.unwrap_or(&json!({})))
                    .map_err(|_| BackendError::InvalidInput)?;
                self.agent
                    .post(&url)
                    .header("Content-Type", "application/json")
                    .config()
                    .timeout_global(Some(timeout))
                    .build()
                    .send(payload.as_slice())
            }
            _ => unreachable!(),
        };
        let mut response = response.map_err(|_| BackendError::Transport)?;
        let status = response.status().as_u16();
        if !(200..300).contains(&status) {
            return Err(BackendError::Http(status));
        }
        let value: Value = response
            .body_mut()
            .with_config()
            .limit(16 * 1024 * 1024)
            .read_json()
            .map_err(|_| BackendError::InvalidJson)?;
        if value.get("ok").and_then(Value::as_bool) == Some(false) {
            return Err(BackendError::Rejected);
        }
        Ok(value.get("data").cloned().unwrap_or(value))
    }
    pub fn get<T: DeserializeOwned>(&self, path: &str) -> Result<T, BackendError> {
        serde_json::from_value(self.request("GET", path, None, Duration::from_secs(30))?)
            .map_err(|_| BackendError::InvalidJson)
    }
    pub fn post(&self, path: &str, body: Value) -> Result<Value, BackendError> {
        self.request("POST", path, Some(&body), Duration::from_secs(600))
    }
    pub fn status(&self) -> Result<BackendStatus, BackendError> {
        self.get("/status")
    }
    pub fn setup_state(&self) -> Result<SetupState, BackendError> {
        self.get("/setup/state")
    }
    pub fn install_runtime(&self) -> Result<(), BackendError> {
        self.post("/setup/install-all", json!({})).map(|_| ())
    }
    pub fn install_progress(&self) -> Result<InstallProgress, BackendError> {
        self.get("/setup/install-progress")
    }
    pub fn install_steam(&self) -> Result<(), BackendError> {
        self.post("/steam/install", json!({})).map(|_| ())
    }
    pub fn launcher_status(&self, launcher: Launcher) -> Result<LauncherStatus, BackendError> {
        self.get(&format!("{}/status", launcher.prefix()))
    }
    pub fn set_launcher_running(
        &self,
        launcher: Launcher,
        running: bool,
    ) -> Result<(), BackendError> {
        self.post(
            &format!(
                "{}/{}",
                launcher.prefix(),
                if running { "launch" } else { "stop" }
            ),
            json!({}),
        )
        .map(|_| ())
    }
    pub fn library(&self, launcher: Launcher) -> Result<Library, BackendError> {
        self.get(&format!("{}/library", launcher.prefix()))
    }
    pub fn save_steam_key(&self, key: &str) -> Result<Value, BackendError> {
        self.post("/steam/save-api-key", json!({"key": key.trim()}))
    }
    pub fn save_gamesdb_key(&self, key: &str) -> Result<(), BackendError> {
        self.post(
            "/sharp-library/epic/thegamesdb-api-key",
            json!({"key": key.trim()}),
        )
        .map(|_| ())
    }
    pub fn complete_setup(
        &self,
        device: &str,
        steam_key: &str,
        gamesdb_key: &str,
    ) -> Result<(), BackendError> {
        // Credentials and wrapper checks must succeed BEFORE marking setup done.
        self.post("/steam/ensure-launch-ready", json!({}))?;
        if !steam_key.trim().is_empty() {
            self.save_steam_key(steam_key)?;
        }
        if !gamesdb_key.trim().is_empty() {
            self.save_gamesdb_key(gamesdb_key)?;
        }
        self.post(
            "/setup/save",
            json!({"step":2,"deviceName":device.trim(),"completed":true}),
        )
        .map(|_| ())
    }
    pub fn launch_game(&self, game: &Game) -> Result<Value, BackendError> {
        if game.appid == 0 {
            return Err(BackendError::InvalidInput);
        }
        let method = if game.launch_method.is_empty() {
            "auto"
        } else {
            &game.launch_method
        };
        let (path, body) = if game.source == "ubisoft" {
            let id = game
                .ubisoft_id
                .as_deref()
                .filter(|id| !id.is_empty() && id.bytes().all(|ch| ch.is_ascii_digit()))
                .ok_or(BackendError::InvalidInput)?;
            (
                "/ubisoft/launch-game",
                json!({"appid":game.appid,"ubisoft_id":id,"pipeline":if game.preferred_pipeline.is_empty() { method } else { &game.preferred_pipeline }}),
            )
        } else {
            let path = if game.has_native_build {
                "/steam/mac-launch-game"
            } else if [
                "d3dmetal",
                "vkd3d",
                "d3d9",
                "dxmt",
                "dxmt_32",
                "steam",
                "wine_steam",
            ]
            .contains(&method.to_ascii_lowercase().as_str())
            {
                "/steam/launch-game"
            } else {
                "/game/launch-auto"
            };
            (path, json!({"appid":game.appid,"launchMethod":method}))
        };
        self.post(path, body)
    }
    pub fn stop_game(&self, appid: u64) -> Result<(), BackendError> {
        if appid == 0 {
            return Err(BackendError::InvalidInput);
        }
        self.post("/kill", json!({"appid":appid})).map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Read, Write},
        net::TcpListener,
        thread,
    };
    fn server(reply: &'static str) -> (BackendClient, thread::JoinHandle<String>) {
        let socket = TcpListener::bind("127.0.0.1:0").unwrap();
        let client = BackendClient::for_port(socket.local_addr().unwrap().port()).unwrap();
        let thread = thread::spawn(move || {
            let (mut stream, _) = socket.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            let mut bytes = Vec::new();
            loop {
                let mut buf = [0; 2048];
                let n = stream.read(&mut buf).unwrap();
                bytes.extend_from_slice(&buf[..n]);
                if let Some(end) = bytes.windows(4).position(|w| w == b"\r\n\r\n") {
                    let header = String::from_utf8_lossy(&bytes[..end]).to_ascii_lowercase();
                    let len: usize = header
                        .lines()
                        .find_map(|line| {
                            line.strip_prefix("content-length:")
                                .and_then(|v| v.trim().parse().ok())
                        })
                        .unwrap_or(0);
                    if bytes.len() >= end + 4 + len {
                        break;
                    }
                }
                if n == 0 {
                    break;
                }
            }
            write!(
                stream,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                reply.len(),
                reply
            )
            .unwrap();
            String::from_utf8(bytes).unwrap()
        });
        (client, thread)
    }
    #[test]
    fn launcher_post_preserves_route_and_payload() {
        let (c, t) = server(r#"{"ok":true}"#);
        c.set_launcher_running(Launcher::Ubisoft, true).unwrap();
        let request = t.join().unwrap();
        assert!(request.starts_with("POST /ubisoft/launch HTTP/1.1"));
        assert!(request.ends_with("{}"));
    }
    #[test]
    fn steam_key_is_sent_only_in_json_body() {
        let (c, t) = server(r#"{"ok":true,"sync":{"steam_id_detected":false}}"#);
        c.save_steam_key("fixture-key").unwrap();
        let request = t.join().unwrap();
        assert!(request.starts_with("POST /steam/save-api-key HTTP/1.1"));
        assert!(
            !request
                .split("\r\n\r\n")
                .next()
                .unwrap()
                .contains("fixture-key")
        );
        assert!(request.contains(r#""key":"fixture-key""#));
    }
    #[test]
    fn backend_errors_do_not_expose_secrets() {
        let (c, t) = server(r#"{"ok":false,"error":"secret-fixture-key"}"#);
        let e = c.save_steam_key("secret-fixture-key").unwrap_err();
        assert_eq!(e, BackendError::Rejected);
        assert!(!e.to_string().contains("secret"));
        t.join().unwrap();
    }
    #[test]
    fn direct_and_wrapped_libraries_parse() {
        for body in [
            r#"{"ok":true,"games":[{"appid":10,"name":"Fixture"}]}"#,
            r#"{"data":{"games":[{"appid":10,"name":"Fixture"}]}}"#,
        ] {
            let (c, t) = server(body);
            let l = c.library(Launcher::Steam).unwrap();
            assert_eq!(l.games[0].appid, 10);
            t.join().unwrap();
        }
    }
    #[test]
    fn rejects_unrestricted_routes_and_bad_identifiers() {
        let c = BackendClient::for_port(9276).unwrap();
        for p in [
            "https://example.com",
            "//example.com",
            "/status?code=secret",
            "/x\r\nHost: evil",
        ] {
            assert_eq!(c.get::<Value>(p).unwrap_err(), BackendError::InvalidRoute);
        }
        assert_eq!(c.stop_game(0).unwrap_err(), BackendError::InvalidInput);
        assert!(BackendClient::for_port(0).is_err());
    }
    #[test]
    fn ubisoft_identifier_is_a_string_and_pipeline_is_preserved() {
        let game: Game = serde_json::from_value(json!({"appid":123,"name":"Ubisoft fixture","source":"ubisoft","ubisoft_id":"0042","installed":true,"preferred_pipeline":"dxmt_32","launch_method":"ubisoft"})).unwrap();
        let (c, t) = server(r#"{"ok":true,"pid":1234}"#);
        c.launch_game(&game).unwrap();
        let request = t.join().unwrap();
        assert!(request.starts_with("POST /ubisoft/launch-game HTTP/1.1"));
        let payload: Value =
            serde_json::from_str(request.split_once("\r\n\r\n").unwrap().1).unwrap();
        assert_eq!(
            payload,
            json!({"appid":123,"ubisoft_id":"0042","pipeline":"dxmt_32"})
        );
    }
    #[test]
    fn steam_launch_dispatch_matches_renderer_route_contract() {
        for (method, native, path) in [
            ("d3dmetal", false, "/steam/launch-game"),
            ("dxmt_32", false, "/steam/launch-game"),
            ("auto", false, "/game/launch-auto"),
            ("mono", false, "/game/launch-auto"),
            ("auto", true, "/steam/mac-launch-game"),
        ] {
            let game:Game=serde_json::from_value(json!({"appid":10,"name":"Fixture","launch_method":method,"has_native_build":native})).unwrap();
            let (c, t) = server(r#"{"ok":true}"#);
            c.launch_game(&game).unwrap();
            let request = t.join().unwrap();
            assert!(request.starts_with(&format!("POST {path} HTTP/1.1")));
            let payload: Value =
                serde_json::from_str(request.split_once("\r\n\r\n").unwrap().1).unwrap();
            assert_eq!(payload, json!({"appid":10,"launchMethod":method}));
        }
    }
    #[test]
    fn runtime_and_steam_installs_use_length_delimited_json() {
        for runtime in [true, false] {
            let (c, t) = server(r#"{"ok":true}"#);
            if runtime {
                c.install_runtime().unwrap();
            } else {
                c.install_steam().unwrap();
            }
            let request = t.join().unwrap();
            assert!(request.starts_with(if runtime {
                "POST /setup/install-all "
            } else {
                "POST /steam/install "
            }));
            assert!(request.to_ascii_lowercase().contains("content-length: 2"));
            assert!(!request.to_ascii_lowercase().contains("transfer-encoding"));
        }
    }
    #[test]
    fn nullable_preferences_are_not_a_library_failure() {
        let game: Game = serde_json::from_value(
            json!({"appid":1,"name":"Fixture","launch_method":null,"preferred_pipeline":null}),
        )
        .unwrap();
        assert!(game.launch_method.is_empty());
    }
    #[test]
    fn failed_wrapper_check_does_not_mark_setup_complete_or_send_keys() {
        let (c, t) = server(r#"{"ok":false,"error":"fixture"}"#);
        assert_eq!(
            c.complete_setup("Fixture", "secret-fixture-key", "secret-fixture-gamesdb")
                .unwrap_err(),
            BackendError::Rejected
        );
        let request = t.join().unwrap();
        assert!(request.starts_with("POST /steam/ensure-launch-ready "));
        assert!(!request.contains("secret-fixture"));
    }
    #[test]
    fn install_progress_bounds() {
        let p = InstallProgress {
            step: usize::MAX,
            total: 1,
            ..Default::default()
        };
        assert_eq!(p.percent(), 100);
        assert_eq!(InstallProgress::default().percent(), 0);
    }
}
