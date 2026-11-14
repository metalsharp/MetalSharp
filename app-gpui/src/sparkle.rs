//! MetalSharp's updates through Sparkle (sparkle/MSSparkleBridge.m): Sparkle finds the running build's newer version
//! in its appcast, downloads it (a `.delta` when the release has one for this version), checks its EdDSA and code
//! signatures, and installs it once MetalSharp quits. The GPUI update sheet draws `state()`.
//!
//! Without the `sparkle` feature (development builds, tests) nothing is linked and Sparkle is never available, so
//! updates fall back to the DMG path.

use serde::Deserialize;

/// The bridge's state (`ms_sparkle_state_json`).
#[derive(Clone, Debug, Default, Deserialize, PartialEq)]
#[serde(default)]
pub struct State {
    pub available: bool,
    /// idle, checking, downloading, extracting, ready, installing, none (no newer version in the feed) or error
    pub phase: String,
    /// The version Sparkle found.
    pub version: String,
    pub expected: u64,
    pub received: u64,
    /// Extraction, 0 to 1.
    pub extracted: f64,
    /// MetalSharp has quit for the installer.
    pub terminated: bool,
    pub error: String,
}

impl State {
    pub fn parse(json: &str) -> State {
        serde_json::from_str(json).unwrap_or_default()
    }

    /// The update sheet's progress, in percent: the download from 5 to 75, extraction to 90, then the install.
    pub fn percent(&self) -> f32 {
        match self.phase.as_str() {
            "checking" => 2.0,
            "downloading" if self.expected > 0 => {
                5.0 + 70.0 * (self.received.min(self.expected) as f32 / self.expected as f32)
            }
            "downloading" => 5.0,
            "extracting" => 75.0 + 15.0 * self.extracted.clamp(0.0, 1.0) as f32,
            "ready" => 90.0,
            "installing" => 95.0,
            _ => 0.0,
        }
    }

    /// The update sheet's line under its progress bar.
    pub fn message(&self) -> String {
        let mb = |bytes: u64| bytes as f64 / 1_000_000.0;
        match self.phase.as_str() {
            "checking" => "Checking for the update...".into(),
            "downloading" if self.expected > 0 => format!(
                "Downloading MetalSharp {}: {:.0} of {:.0} MB",
                self.version,
                mb(self.received.min(self.expected)),
                mb(self.expected)
            ),
            "downloading" => format!("Downloading MetalSharp {}...", self.version),
            "extracting" => "Preparing the update...".into(),
            "ready" => "Closing MetalSharp to install the update...".into(),
            "installing" => "Installing the update...".into(),
            "error" => self.error.clone(),
            _ => String::new(),
        }
    }
}

#[cfg(all(target_os = "macos", feature = "sparkle"))]
mod bridge {
    use std::ffi::{CStr, c_char, c_int};

    unsafe extern "C" {
        fn ms_sparkle_available() -> c_int;
        fn ms_sparkle_update() -> c_int;
        fn ms_sparkle_install() -> c_int;
        fn ms_sparkle_cancel();
        fn ms_sparkle_state_json() -> *mut c_char;
        fn ms_sparkle_free(text: *mut c_char);
    }

    pub fn available() -> bool {
        unsafe { ms_sparkle_available() == 1 }
    }

    pub fn update() -> bool {
        unsafe { ms_sparkle_update() == 0 }
    }

    pub fn install() -> bool {
        unsafe { ms_sparkle_install() == 0 }
    }

    pub fn cancel() {
        unsafe { ms_sparkle_cancel() }
    }

    pub fn state_json() -> String {
        unsafe {
            let text = ms_sparkle_state_json();
            if text.is_null() {
                return String::new();
            }
            let json = CStr::from_ptr(text).to_string_lossy().into_owned();
            ms_sparkle_free(text);
            json
        }
    }
}

#[cfg(not(all(target_os = "macos", feature = "sparkle")))]
mod bridge {
    pub fn available() -> bool {
        false
    }
    pub fn update() -> bool {
        false
    }
    pub fn install() -> bool {
        false
    }
    pub fn cancel() {}
    pub fn state_json() -> String {
        String::new()
    }
}

/// This MetalSharp can update itself through Sparkle: it was built with Sparkle and a feed. Main thread only.
pub fn available() -> bool {
    bridge::available()
}

/// Starts a check that downloads and prepares whatever newer version the feed has. Main thread only.
pub fn update() -> bool {
    bridge::update()
}

/// Lets Sparkle install the prepared update; it does so, and relaunches MetalSharp, once MetalSharp quits.
pub fn install() -> bool {
    bridge::install()
}

/// Cancels a check or download, or sets aside a prepared update.
pub fn cancel() {
    bridge::cancel()
}

pub fn state() -> State {
    State::parse(&bridge::state_json())
}

/// The migration marker MetalSharp's next launch reads (`.post-update-migration`), as update.sh writes it.
pub fn migration_marker(version: &str, variant: &str, now: u64) -> String {
    serde_json::json!({
        "needed": true,
        "target_version": version,
        "target_variant": variant,
        "previous_variant": variant,
        "timestamp": now,
    })
    .to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_bridge_state() {
        let state = State::parse(
            r#"{"available":true,"phase":"downloading","version":"1.3.0","expected":200000000,"received":50000000,"extracted":0,"terminated":false,"error":""}"#,
        );
        assert!(state.available);
        assert_eq!(state.phase, "downloading");
        assert_eq!(state.percent(), 22.5);
        assert_eq!(state.message(), "Downloading MetalSharp 1.3.0: 50 of 200 MB");
    }

    #[test]
    fn progress_only_moves_forward_through_the_phases() {
        let at = |phase: &str, extracted: f64| {
            State {
                phase: phase.into(),
                expected: 10,
                received: 10,
                extracted,
                ..State::default()
            }
            .percent()
        };
        let steps = [
            at("checking", 0.0),
            at("downloading", 0.0),
            at("extracting", 0.0),
            at("extracting", 1.0),
            at("ready", 0.0),
            at("installing", 0.0),
        ];
        assert!(steps.windows(2).all(|pair| pair[0] <= pair[1]), "{steps:?}");
    }

    #[test]
    fn garbage_is_an_unavailable_idle_state() {
        let state = State::parse("not json");
        assert!(!state.available);
        assert_eq!(state.percent(), 0.0);
    }

    #[test]
    fn writes_the_marker_update_sh_writes() {
        let marker: serde_json::Value = serde_json::from_str(&migration_marker("1.3.0", "fex", 42)).unwrap();
        assert_eq!(marker["needed"], true);
        assert_eq!(marker["target_version"], "1.3.0");
        assert_eq!(marker["target_variant"], "fex");
        assert_eq!(marker["previous_variant"], "fex");
        assert_eq!(marker["timestamp"], 42);
    }

    #[test]
    fn unavailable_without_the_feature() {
        if cfg!(not(feature = "sparkle")) {
            assert!(!available());
            assert!(!update());
            assert!(!install());
        }
    }
}
