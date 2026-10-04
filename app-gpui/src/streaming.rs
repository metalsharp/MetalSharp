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
        self.progress_detail = self
            .progress_detail
            .map(|line| crate::diagnostics::redact_line(&line));
        self.pairings_summary = self.pairings_summary.chars().take(1024).collect();
        self.creds_username = self.creds_username.chars().take(128).collect();
        self.version = self.version.chars().take(128).collect();
        self
    }
    pub fn can_pair(&self) -> bool {
        self.running && self.creds_valid && !self.installing
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
