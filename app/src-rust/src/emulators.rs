//! Parity contracts for the native emulator providers.
//!
//! The packaged runtime is the C backend. These serde models keep the Rust
//! reference explicit about the supported RPCS3 provider state.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorProvider {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub supported: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Rpcs3StatusContract {
    pub ok: bool,
    pub provider: String,
    pub name: String,
    pub platform: String,
    pub supported: bool,
    pub installed: bool,
    pub state: String,
    pub architecture: String,
    pub current_tag: Option<String>,
    pub rollback_available: bool,
    pub firmware_installed: bool,
    pub environment_path: String,
    pub data_path: String,
    pub cache_path: String,
    pub executable_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorGameContract {
    pub id: String,
    pub title_id: String,
    pub title: String,
    pub version: String,
    pub category: String,
    pub path: String,
    pub installed_title: bool,
    pub has_artwork: bool,
    pub running: bool,
    pub pid: Option<i32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorUpdateContract {
    pub ok: bool,
    pub provider: String,
    pub current_tag: Option<String>,
    pub latest_tag: String,
    pub latest_version: String,
    pub available: bool,
    pub asset_name: String,
    pub download_size: u64,
    pub digest: String,
    pub published_at: String,
}

pub fn providers() -> Vec<EmulatorProvider> {
    vec![EmulatorProvider {
        id: "rpcs3".into(),
        name: "RPCS3".into(),
        platform: "PlayStation 3".into(),
        supported: true,
    }]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_contract_exposes_supported_rpcs3() {
        let values = providers();
        assert_eq!(values.len(), 1);
        assert_eq!(values[0].id, "rpcs3");
        assert!(values[0].supported);
    }

    #[test]
    fn update_contract_uses_integrity_and_archival_metadata() {
        let update = EmulatorUpdateContract {
            ok: true,
            provider: "rpcs3".into(),
            current_tag: None,
            latest_tag: "build-test".into(),
            latest_version: "0.0.42-test".into(),
            available: true,
            asset_name: "rpcs3-test_macos_aarch64.7z".into(),
            download_size: 123,
            digest: format!("sha256:{}", "a".repeat(64)),
            published_at: "2026-01-02T03:04:05Z".into(),
        };
        let value = serde_json::to_value(update).unwrap();
        assert!(value["digest"].as_str().unwrap().starts_with("sha256:"));
        assert_eq!(value["downloadSize"], 123);
    }
}
