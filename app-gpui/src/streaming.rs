//! Typed Sunshine contracts. This module never installs, launches or pairs by itself.
use crate::backend::{BackendClient, BackendError};
use serde::Deserialize;
use serde_json::{Value, json};

#[derive(Clone, Default, Deserialize)]
pub struct StreamingStatus {
    pub installed: bool,
    pub running: bool,
    pub version: String,
    pub creds_configured: bool,
    pub creds_valid: bool,
    pub creds_username: String,
    pub web_url: String,
    pub installing: bool,
    pub pairing_count: usize,
    pub pairings_summary: String,
    pub progress_status: Option<String>,
    pub progress_detail: Option<String>,
}
impl StreamingStatus {
    pub fn sanitize(mut self) -> Self {
        self.progress_status = self
            .progress_status
            .map(|line| crate::diagnostics::redact_line(&line));
        self.progress_detail = self
            .progress_detail
            .map(|line| crate::diagnostics::redact_line(&line));
        self.pairings_summary = crate::diagnostics::redact_line(&self.pairings_summary)
            .chars()
            .take(1024)
            .collect();
        self.creds_username = self.creds_username.chars().take(128).collect();
        self.version = self.version.chars().take(128).collect();
        self
    }
    pub fn can_pair(&self) -> bool {
        self.running && self.creds_valid && !self.installing
    }
    pub(crate) fn allows(&self, action: &StreamingAction) -> bool {
        match action {
            StreamingAction::Install => !self.installed && !self.installing,
            StreamingAction::Start => self.installed && !self.running && !self.installing,
            StreamingAction::Stop => self.running,
            StreamingAction::Pair(_) | StreamingAction::UnpairAll => self.can_pair(),
        }
    }
}
// PINs are transient secrets; intentionally no Debug/Serialize implementation.
pub enum StreamingAction {
    Install,
    Start,
    Stop,
    Pair(String),
    UnpairAll,
}
impl StreamingAction {
    fn request(&self) -> Result<(&'static str, Value, std::time::Duration), BackendError> {
        let (path, body, seconds) = match self {
            Self::Install => ("/streaming/install", json!({}), 30),
            Self::Start => ("/streaming/launch", json!({}), 75),
            Self::Stop => ("/streaming/stop", json!({}), 30),
            Self::Pair(pin) => {
                if pin.len() != 4 || !pin.bytes().all(|byte| byte.is_ascii_digit()) {
                    return Err(BackendError::InvalidInput);
                }
                ("/streaming/pin", json!({"pin":pin}), 95)
            }
            Self::UnpairAll => ("/streaming/unpair-all", json!({}), 30),
        };
        Ok((path, body, std::time::Duration::from_secs(seconds)))
    }
}
impl BackendClient {
    pub fn streaming_status(&self) -> Result<StreamingStatus, BackendError> {
        self.get::<StreamingStatus>("/streaming/status")
            .map(StreamingStatus::sanitize)
    }
    pub fn streaming_action(&self, action: StreamingAction) -> Result<Value, BackendError> {
        let (path, body, timeout) = action.request()?;
        self.request("POST", path, Some(&body), timeout)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn fixture(reply: serde_json::Value) -> (BackendClient, std::thread::JoinHandle<String>) {
        use std::io::{Read, Write};
        let socket = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let client = BackendClient::for_port(socket.local_addr().unwrap().port())
            .unwrap()
            .with_client_token("a".repeat(64))
            .unwrap();
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = socket.accept().unwrap();
            stream
                .set_read_timeout(Some(std::time::Duration::from_secs(3)))
                .unwrap();
            let mut bytes = Vec::new();
            loop {
                let mut buffer = [0; 2048];
                let n = stream.read(&mut buffer).unwrap();
                assert!(n > 0);
                bytes.extend_from_slice(&buffer[..n]);
                if let Some(end) = bytes.windows(4).position(|w| w == b"\r\n\r\n") {
                    let headers = String::from_utf8_lossy(&bytes[..end]);
                    let length = headers
                        .lines()
                        .find_map(|line| {
                            line.to_lowercase()
                                .strip_prefix("content-length:")
                                .and_then(|value| value.trim().parse::<usize>().ok())
                        })
                        .unwrap_or(0);
                    if bytes.len() >= end + 4 + length {
                        break;
                    }
                }
                assert!(bytes.len() < 16384);
            }
            let body = reply.to_string();
            write!(stream,"HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n{}",body.len(),body).unwrap();
            String::from_utf8(bytes).unwrap()
        });
        (client, handle)
    }
    #[test]
    fn real_http_pair_transport_has_owned_session_header_and_exact_pin_body() {
        let (client, request) = fixture(json!({"ok":true}));
        client
            .streaming_action(StreamingAction::Pair("0042".into()))
            .unwrap();
        let request = request.join().unwrap();
        assert!(request.starts_with("POST /streaming/pin HTTP/1.1"));
        assert!(
            request
                .to_lowercase()
                .contains(&format!("x-metalsharp-client-token: {}", "a".repeat(64)))
        );
        assert_eq!(
            request.split("\r\n\r\n").nth(1).unwrap(),
            "{\"pin\":\"0042\"}"
        );
    }
    #[test]
    fn status_decodes_nullable_c_contract_and_redacts_before_storage() {
        let (client, request) = fixture(
            json!({"ok":true,"installed":true,"running":true,"version":"v1","creds_configured":true,"creds_valid":false,"creds_username":"fixture","web_url":"https://localhost:47990","installing":false,"pairing_count":2,"pairings_summary":"fixture device","progress_status":null,"progress_detail":"Authorization: private-fixture"}),
        );
        let status = client.streaming_status().unwrap();
        assert!(status.installed && status.running);
        assert!(!status.can_pair());
        assert_eq!(status.pairing_count, 2);
        assert!(status.progress_status.is_none());
        assert!(!status.progress_detail.unwrap().contains("private-fixture"));
        assert!(
            request
                .join()
                .unwrap()
                .starts_with("GET /streaming/status HTTP/1.1")
        );
    }
    #[test]
    fn invalid_pin_is_rejected_before_transport() {
        let socket = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        socket.set_nonblocking(true).unwrap();
        let client = BackendClient::for_port(socket.local_addr().unwrap().port()).unwrap();
        assert!(matches!(
            client.streaming_action(StreamingAction::Pair("123".into())),
            Err(BackendError::InvalidInput)
        ));
        assert!(
            matches!(socket.accept(),Err(error) if error.kind()==std::io::ErrorKind::WouldBlock)
        );
    }
    #[test]
    fn pin_contract_rejects_unicode_whitespace_and_non_four_digit_values() {
        for pin in ["", "123", "12345", "１２３４", "12 4", "123\n"] {
            assert!(StreamingAction::Pair(pin.into()).request().is_err());
        }
        let (path, body, timeout) = StreamingAction::Pair("0042".into()).request().unwrap();
        assert_eq!(path, "/streaming/pin");
        assert_eq!(body, json!({"pin":"0042"}));
        assert_eq!(timeout.as_secs(), 95);
    }
    #[test]
    fn command_routes_and_timeouts_match_renderer() {
        for (action, path, seconds) in [
            (StreamingAction::Install, "/streaming/install", 30),
            (StreamingAction::Start, "/streaming/launch", 75),
            (StreamingAction::Stop, "/streaming/stop", 30),
            (StreamingAction::UnpairAll, "/streaming/unpair-all", 30),
        ] {
            let (actual, body, timeout) = action.request().unwrap();
            assert_eq!(actual, path);
            assert_eq!(body, json!({}));
            assert_eq!(timeout.as_secs(), seconds);
        }
    }
    #[test]
    fn pending_pin_requests_are_not_registered_clients_or_unpair_authority() {
        let status = StreamingStatus {
            installed: true,
            running: true,
            creds_valid: true,
            pairing_count: 0,
            ..Default::default()
        };
        assert!(status.allows(&StreamingAction::UnpairAll));
        assert!(status.allows(&StreamingAction::Pair("0042".into())));
        assert!(!status.allows(&StreamingAction::Install));
        assert!(!status.allows(&StreamingAction::Start));
        assert!(
            !StreamingStatus {
                creds_valid: false,
                ..status
            }
            .allows(&StreamingAction::UnpairAll)
        );
    }
    #[test]
    fn pairing_requires_verified_credentials_and_running_host() {
        let mut status = StreamingStatus::default();
        assert!(!status.can_pair());
        status.running = true;
        assert!(!status.can_pair());
        status.creds_valid = true;
        assert!(status.can_pair());
        status.installing = true;
        assert!(!status.can_pair());
        status.progress_detail = Some("Authorization: fixture-private".into());
        assert!(
            !status
                .sanitize()
                .progress_detail
                .unwrap()
                .contains("fixture-private")
        );
    }
}
