//! Typed, partial configuration updates. Never serialize a full snapshot over
//! unknown backend-owned keys, credentials, profiles or future settings.
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, Eq, PartialEq)]
pub enum ControllerInput {
    #[default]
    #[serde(rename = "off")]
    Off,
    #[serde(rename = "x")]
    XInput,
    #[serde(rename = "d")]
    DInput,
}
#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, Eq, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum WindowMode {
    #[default]
    Default,
    Windowed,
    Fullscreen,
}
#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, Eq, PartialEq)]
pub enum GameResolution {
    #[default]
    #[serde(rename = "default")]
    Default,
    #[serde(rename = "1280x720")]
    Hd,
    #[serde(rename = "1920x1080")]
    FullHd,
    #[serde(rename = "2560x1440")]
    Qhd,
    #[serde(rename = "3840x2160")]
    Uhd,
}
#[derive(Clone, Debug, Deserialize, Serialize, Eq, PartialEq)]
#[serde(default, rename_all = "camelCase")]
pub struct RuntimePreferences {
    pub graphics_runtime_logs: bool,
    pub controller_input: ControllerInput,
    pub window_mode: WindowMode,
    pub game_resolution: GameResolution,
    pub msync: bool,
    pub retina_mode: bool,
    pub exclude_native_mac_steam_games: bool,
}
impl Default for RuntimePreferences {
    fn default() -> Self {
        Self {
            graphics_runtime_logs: false,
            controller_input: ControllerInput::Off,
            window_mode: WindowMode::Default,
            game_resolution: GameResolution::Default,
            msync: true,
            retina_mode: false,
            exclude_native_mac_steam_games: false,
        }
    }
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PreferenceChange {
    GraphicsRuntimeLogs(bool),
    ControllerInput(ControllerInput),
    WindowMode(WindowMode),
    GameResolution(GameResolution),
    Msync(bool),
    RetinaMode(bool),
    ExcludeNativeMacSteamGames(bool),
}
impl PreferenceChange {
    pub fn body(self) -> Value {
        match self {
            Self::GraphicsRuntimeLogs(enabled) => {
                json!({"graphicsRuntimeLogs":enabled,"logs":enabled})
            }
            Self::ControllerInput(value) => json!({"controllerInput":value}),
            Self::WindowMode(value) => json!({"windowMode":value}),
            Self::GameResolution(value) => json!({"gameResolution":value}),
            Self::Msync(enabled) => json!({"msync":enabled}),
            Self::RetinaMode(enabled) => json!({"retinaMode":enabled}),
            Self::ExcludeNativeMacSteamGames(enabled) => {
                json!({"excludeNativeMacSteamGames":enabled})
            }
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn configuration_defaults_match_c_backend_and_ignore_unrelated_secrets() {
        let configuration: RuntimePreferences = serde_json::from_value(
            json!({"ok":true,"privateFutureKey":"secret-fixture","msync":false}),
        )
        .unwrap();
        assert_eq!(configuration.controller_input, ControllerInput::Off);
        assert!(!configuration.msync);
        assert!(!format!("{configuration:?}").contains("secret-fixture"));
        assert!(RuntimePreferences::default().msync);
    }
    #[test]
    fn changes_are_partial_with_exact_backend_enum_values() {
        assert_eq!(
            PreferenceChange::ControllerInput(ControllerInput::XInput).body(),
            json!({"controllerInput":"x"})
        );
        assert_eq!(
            PreferenceChange::GameResolution(GameResolution::FullHd).body(),
            json!({"gameResolution":"1920x1080"})
        );
        assert_eq!(
            PreferenceChange::WindowMode(WindowMode::Fullscreen).body(),
            json!({"windowMode":"fullscreen"})
        );
        assert_eq!(
            PreferenceChange::Msync(false).body(),
            json!({"msync":false})
        );
    }
}
