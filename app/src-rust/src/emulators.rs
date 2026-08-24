//! Parity contracts for the native emulator providers.
//!
//! The packaged runtime is the C backend. These serde models keep the Rust
//! reference explicit about the supported PCSX2, RPCS3, and experimental shadPS4 and SharpEmu provider states.

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
pub struct Pcsx2StatusContract {
    pub ok: bool,
    pub provider: String,
    pub name: String,
    pub platform: String,
    pub supported: bool,
    pub unsupported_reason: Option<String>,
    pub installed: bool,
    pub runtime_valid: bool,
    pub state: String,
    pub host_architecture: String,
    pub runtime_architecture: String,
    pub rosetta_available: bool,
    pub sse41_available: bool,
    pub host_macos_major: i32,
    pub host_memory_bytes: u64,
    pub host_logical_cpu: u64,
    pub warnings: Vec<String>,
    pub runtime_minimum_macos: Option<i32>,
    pub current_tag: Option<String>,
    pub rollback_available: bool,
    pub setup_complete: bool,
    pub bios_installed: bool,
    pub bios_count: u64,
    pub bios_region: Option<String>,
    pub bios_description: Option<String>,
    pub game_root_count: u64,
    pub active_session_count: u64,
    pub data_path_flag: bool,
    pub upstream_updater_disabled: bool,
    pub environment_path: String,
    pub data_path: String,
    pub cache_path: String,
    pub executable_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Pcsx2GameContract {
    pub id: String,
    pub serial: Option<String>,
    pub title: String,
    pub region: Option<String>,
    pub format: String,
    pub size: u64,
    pub path: String,
    pub has_artwork: bool,
    pub running: bool,
    pub pid: Option<i32>,
    pub last_log_path: Option<String>,
    pub last_exit_code: Option<i32>,
    pub last_exit_signal: Option<i32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Pcsx2GamesContract {
    pub ok: bool,
    pub provider: String,
    pub roots: Vec<String>,
    pub scanned_entries: u64,
    pub truncated: bool,
    pub games: Vec<Pcsx2GameContract>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Pcsx2SettingOptionContract {
    pub id: String,
    pub label: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Pcsx2SettingsContract {
    pub ok: bool,
    pub controller1: String,
    pub controller2: String,
    pub renderer: String,
    pub controller_options: Vec<Pcsx2SettingOptionContract>,
    pub renderer_options: Vec<Pcsx2SettingOptionContract>,
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
pub struct SharpemuStatusContract {
    pub ok: bool,
    pub provider: String,
    pub name: String,
    pub platform: String,
    pub experimental: bool,
    pub supported: bool,
    pub unsupported_reason: Option<String>,
    pub installed: bool,
    pub runtime_valid: bool,
    pub state: String,
    pub host_architecture: String,
    pub runtime_architecture: String,
    pub rosetta_available: bool,
    pub host_macos_major: i32,
    pub runtime_minimum_macos: i32,
    pub host_memory_bytes: u64,
    pub host_logical_cpu: u64,
    pub available_disk_bytes: u64,
    pub archive_tools_available: bool,
    pub gpu_probe_ready: bool,
    pub network_isolation_available: bool,
    pub network_default: String,
    pub network_opt_in_available: bool,
    pub upstream_notarized: bool,
    pub locally_ad_hoc_signed: bool,
    pub cli_only: bool,
    pub graphics_backend: String,
    pub update_running: bool,
    pub warnings: Vec<String>,
    pub current_tag: Option<String>,
    pub rollback_available: bool,
    pub game_root_count: u64,
    pub game_count: u64,
    pub environment_path: String,
    pub data_path: String,
    pub cache_path: String,
    pub logs_path: String,
    pub executable_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SharpemuGameContract {
    pub id: String,
    pub title_id: String,
    pub title: String,
    pub content_version: String,
    pub master_version: String,
    pub path: String,
    pub executable_size: u64,
    pub has_artwork: bool,
    pub running: bool,
    pub pid: Option<i32>,
    pub last_log_path: Option<String>,
    pub last_exit_code: Option<i32>,
    pub last_exit_signal: Option<i32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SharpemuGamesContract {
    pub ok: bool,
    pub provider: String,
    pub roots: Vec<String>,
    pub scanned_entries: u64,
    pub truncated: bool,
    pub games: Vec<SharpemuGameContract>,
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
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pcsx2_log_path: Option<String>,
    pub started_at: i64,
}

pub fn providers() -> Vec<EmulatorProvider> {
    vec![
        EmulatorProvider {
            id: "pcsx2".into(),
            name: "PCSX2".into(),
            platform: "PlayStation 2".into(),
            supported: true,
            experimental: None,
        },
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
        EmulatorProvider {
            id: "sharpemu".into(),
            name: "SharpEmu".into(),
            platform: "PlayStation 5".into(),
            supported: true,
            experimental: Some(true),
        },
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn provider_contract_exposes_managed_console_environments() {
        let values = providers();
        assert_eq!(values.len(), 4);
        assert_eq!(values[0].id, "pcsx2");
        assert_eq!(values[0].platform, "PlayStation 2");
        assert!(values[0].supported);
        assert_eq!(values[1].id, "rpcs3");
        assert!(values[1].supported);
        assert_eq!(values[2].id, "shadps4");
        assert_eq!(values[2].name, "shadPS4");
        assert_eq!(values[2].experimental, Some(true));
        assert_eq!(values[3].id, "sharpemu");
        assert_eq!(values[3].platform, "PlayStation 5");
        assert_eq!(values[3].experimental, Some(true));
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
    fn pcsx2_contract_preserves_bios_isolation_and_host_readiness() {
        let status = Pcsx2StatusContract {
            ok: true,
            provider: "pcsx2".into(),
            name: "PCSX2".into(),
            platform: "PlayStation 2".into(),
            supported: true,
            unsupported_reason: None,
            installed: true,
            runtime_valid: true,
            state: "ready".into(),
            host_architecture: "arm64".into(),
            runtime_architecture: "x86_64".into(),
            rosetta_available: true,
            sse41_available: false,
            host_macos_major: 26,
            host_memory_bytes: 16 * 1024 * 1024 * 1024,
            host_logical_cpu: 8,
            warnings: vec![],
            runtime_minimum_macos: Some(11),
            current_tag: Some("v2.6.3".into()),
            rollback_available: true,
            setup_complete: true,
            bios_installed: true,
            bios_count: 1,
            bios_region: Some("USA".into()),
            bios_description: Some("USA v02.30".into()),
            game_root_count: 1,
            active_session_count: 0,
            data_path_flag: false,
            upstream_updater_disabled: true,
            environment_path: "/tmp/pcsx2".into(),
            data_path: "/tmp/pcsx2/home/Library/Application Support/PCSX2".into(),
            cache_path: "/tmp/pcsx2/home/Library/Application Support/PCSX2/cache".into(),
            executable_path: Some("/tmp/pcsx2/current/PCSX2.app/Contents/MacOS/PCSX2".into()),
        };
        let value = serde_json::to_value(status).unwrap();
        assert_eq!(value["provider"], "pcsx2");
        assert_eq!(value["biosInstalled"], true);
        assert_eq!(value["dataPathFlag"], false);
        assert_eq!(value["runtimeArchitecture"], "x86_64");
    }

    #[test]
    fn pcsx2_settings_contract_preserves_upstream_values() {
        let settings = Pcsx2SettingsContract {
            ok: true,
            controller1: "DualShock2".into(),
            controller2: "Guitar".into(),
            renderer: "metal".into(),
            controller_options: vec![Pcsx2SettingOptionContract {
                id: "DualShock2".into(),
                label: "DualShock 2".into(),
            }],
            renderer_options: vec![Pcsx2SettingOptionContract { id: "metal".into(), label: "Metal".into() }],
        };
        let value = serde_json::to_value(settings).unwrap();
        assert_eq!(value["controller1"], "DualShock2");
        assert_eq!(value["controller2"], "Guitar");
        assert_eq!(value["renderer"], "metal");
        assert_eq!(value["controllerOptions"][0]["label"], "DualShock 2");
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

    #[test]
    fn sharpemu_contract_preserves_network_and_runtime_boundaries() {
        let status = SharpemuStatusContract {
            ok: true,
            provider: "sharpemu".into(),
            name: "SharpEmu".into(),
            platform: "PlayStation 5".into(),
            experimental: true,
            supported: true,
            unsupported_reason: None,
            installed: true,
            runtime_valid: true,
            state: "ready".into(),
            host_architecture: "arm64".into(),
            runtime_architecture: "x86_64".into(),
            rosetta_available: true,
            host_macos_major: 27,
            runtime_minimum_macos: 26,
            host_memory_bytes: 16 * 1024 * 1024 * 1024,
            host_logical_cpu: 10,
            available_disk_bytes: 20 * 1024 * 1024 * 1024,
            archive_tools_available: true,
            gpu_probe_ready: true,
            network_isolation_available: true,
            network_default: "denied".into(),
            network_opt_in_available: true,
            upstream_notarized: false,
            locally_ad_hoc_signed: true,
            cli_only: true,
            graphics_backend: "Vulkan · MoltenVK".into(),
            update_running: false,
            warnings: vec!["experimental_emulator".into()],
            current_tag: Some("v0.0.3-release.3".into()),
            rollback_available: false,
            game_root_count: 1,
            game_count: 1,
            environment_path: "/tmp/sharpemu".into(),
            data_path: "/tmp/sharpemu/state".into(),
            cache_path: "/tmp/sharpemu/cache".into(),
            logs_path: "/tmp/sharpemu/logs".into(),
            executable_path: Some("/tmp/sharpemu/current/SharpEmu".into()),
        };
        let value = serde_json::to_value(status).unwrap();
        assert_eq!(value["provider"], "sharpemu");
        assert_eq!(value["networkDefault"], "denied");
        assert_eq!(value["upstreamNotarized"], false);
        assert_eq!(value["runtimeArchitecture"], "x86_64");
    }
}
