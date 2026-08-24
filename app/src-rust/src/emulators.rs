//! Parity contracts for the native emulator providers.
//!
//! The packaged runtime is the C backend. These serde models keep the Rust
//! reference explicit about the same provider states without pretending that
//! dormant RPCS4 code is installable.

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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Rpcs4StatusContract {
    pub ok: bool,
    pub provider: String,
    pub name: String,
    pub platform: String,
    pub supported: bool,
    pub install_available: bool,
    pub state: String,
    pub repository: String,
    pub last_upstream_commit: String,
    pub reason: String,
    pub readiness_gate: Vec<String>,
}

pub fn providers() -> Vec<EmulatorProvider> {
    vec![
        EmulatorProvider {
            id: "rpcs3".into(),
            name: "RPCS3".into(),
            platform: "PlayStation 3".into(),
            supported: true,
        },
        EmulatorProvider {
            id: "rpcs4".into(),
            name: "RPCS4".into(),
            platform: "PlayStation 4".into(),
            supported: false,
        },
    ]
}

pub fn rpcs4_status() -> Rpcs4StatusContract {
    Rpcs4StatusContract {
        ok: true,
        provider: "rpcs4".into(),
        name: "RPCS4".into(),
        platform: "PlayStation 4".into(),
        supported: false,
        install_available: false,
        state: "unsupported_upstream".into(),
        repository: "https://github.com/xYaroslavGTx/rpcs4".into(),
        last_upstream_commit: "2016-05-18".into(),
        reason:
            "The candidate repository has one Windows-only skeleton commit, no releases, no CI, and no macOS runtime."
                .into(),
        readiness_gate: vec![
            "Maintained upstream".into(),
            "Native macOS build".into(),
            "Versioned releases".into(),
            "Stable launch CLI".into(),
            "Documented game layout".into(),
            "Integrity-verifiable artifacts".into(),
            "Boot evidence".into(),
        ],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_contract_keeps_rpcs3_and_rpcs4_separate() {
        let values = providers();
        assert_eq!(values.len(), 2);
        assert_eq!(values[0].id, "rpcs3");
        assert!(values[0].supported);
        assert_eq!(values[1].id, "rpcs4");
        assert!(!values[1].supported);
    }

    #[test]
    fn rpcs4_fails_closed_until_every_gate_is_met() {
        let status = rpcs4_status();
        assert!(!status.supported);
        assert!(!status.install_available);
        assert_eq!(status.state, "unsupported_upstream");
        assert_eq!(status.readiness_gate.len(), 7);
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
