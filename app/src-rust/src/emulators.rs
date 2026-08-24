//! Parity contracts for the native emulator providers.
//!
//! The packaged runtime is the C backend. These serde models keep the Rust
//! reference explicit about the supported RPCS3 and experimental shadPS4 provider states.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorProvider {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub supported: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub experimental: Option<bool>,
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
pub struct Shadps4StatusContract {
    pub ok: bool,
    pub provider: String,
    pub name: String,
    pub platform: String,
    pub experimental: bool,
    pub supported: bool,
    pub unsupported_reason: Option<String>,
    pub installed: bool,
    pub state: String,
    pub host_architecture: String,
    pub runtime_architecture: String,
    pub rosetta_available: bool,
    pub host_macos_major: i32,
    pub host_memory_bytes: u64,
    pub host_logical_cpu: u64,
    pub warnings: Vec<String>,
    pub runtime_minimum_macos: Option<i32>,
    pub current_tag: Option<String>,
    pub rollback_available: bool,
    pub module_count: u64,
    pub modules_ready: bool,
    pub font_file_count: u64,
    pub fonts_ready: bool,
    pub game_root_count: u64,
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
    #[serde(default)]
    pub has_update: bool,
    pub has_artwork: bool,
    pub running: bool,
    pub pid: Option<i32>,
    pub last_log_path: Option<String>,
    #[serde(default)]
    pub last_exit_code: Option<i32>,
    #[serde(default)]
    pub last_exit_signal: Option<i32>,
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
    #[serde(default)]
    pub pinned_tag: Option<String>,
    #[serde(default)]
    pub skipped_tag: Option<String>,
    #[serde(default)]
    pub suppressed: Option<String>,
    pub asset_name: String,
    pub download_size: u64,
    pub digest: String,
    pub published_at: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorUpdateProgressContract {
    pub ok: bool,
    pub status: String,
    pub running: bool,
    pub percent: i32,
    pub message: String,
    pub error: Option<String>,
    pub target_tag: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorSessionContract {
    pub pid: i32,
    pub id: String,
    pub executable: String,
    pub game_path: String,
    pub runtime_tag: Option<String>,
    pub log_path: String,
    pub started_at: i64,
}

pub fn providers() -> Vec<EmulatorProvider> {
    vec![
        EmulatorProvider {
            id: "rpcs3".into(),
            name: "RPCS3".into(),
            platform: "PlayStation 3".into(),
            supported: true,
            experimental: None,
        },
        EmulatorProvider {
            id: "shadps4".into(),
            name: "shadPS4".into(),
            platform: "PlayStation 4".into(),
            supported: true,
            experimental: Some(true),
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_contract_exposes_rpcs3_and_experimental_shadps4() {
        let values = providers();
        assert_eq!(values.len(), 2);
        assert_eq!(values[0].id, "rpcs3");
        assert!(values[0].supported);
        assert_eq!(values[1].id, "shadps4");
        assert_eq!(values[1].name, "shadPS4");
        assert_eq!(values[1].experimental, Some(true));
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
            pinned_tag: None,
            skipped_tag: None,
            suppressed: Some("none".into()),
            asset_name: "rpcs3-test_macos_aarch64.7z".into(),
            download_size: 123,
            digest: format!("sha256:{}", "a".repeat(64)),
            published_at: "2026-01-02T03:04:05Z".into(),
        };
        let value = serde_json::to_value(update).unwrap();
        assert!(value["digest"].as_str().unwrap().starts_with("sha256:"));
        assert_eq!(value["downloadSize"], 123);
    }

    #[test]
    fn shadps4_status_contract_preserves_host_readiness() {
        let status = Shadps4StatusContract {
            ok: true,
            provider: "shadps4".into(),
            name: "shadPS4".into(),
            platform: "PlayStation 4".into(),
            experimental: true,
            supported: false,
            unsupported_reason: Some("rosetta_missing".into()),
            installed: false,
            state: "unsupported_host".into(),
            host_architecture: "arm64".into(),
            runtime_architecture: "x86_64".into(),
            rosetta_available: false,
            host_macos_major: 27,
            host_memory_bytes: 16 * 1024 * 1024 * 1024,
            host_logical_cpu: 8,
            warnings: vec![],
            runtime_minimum_macos: None,
            current_tag: None,
            rollback_available: false,
            module_count: 0,
            modules_ready: false,
            font_file_count: 0,
            fonts_ready: false,
            game_root_count: 0,
            environment_path: "/tmp/shadps4".into(),
            data_path: "/tmp/shadps4/home".into(),
            cache_path: "/tmp/shadps4/home/cache".into(),
            executable_path: None,
        };
        let value = serde_json::to_value(status).unwrap();
        assert_eq!(value["provider"], "shadps4");
        assert_eq!(value["unsupportedReason"], "rosetta_missing");
        assert_eq!(value["runtimeArchitecture"], "x86_64");
    }
}
