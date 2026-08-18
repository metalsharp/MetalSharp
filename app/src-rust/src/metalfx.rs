//! MetalFX Spatial Upscaling runtime toggle.
#![allow(clippy::doc_lazy_continuation)]
//!
//! DXMT gates MetalFX on the `DXMT_METALFX_SPATIAL_SWAPCHAIN` env var (read
//! live via `GetEnvironmentVariableW` at `CreateSwapChain`) and the
//! `d3d11.metalSpatialUpscaleFactor` config key (read once from `dxmt.conf` /
//! `DXMT_CONFIG` into a per-process singleton at first use).
//!
//! Because `EnableMetalFX` is a compile-time template chosen at swapchain
//! creation and `ResizeBuffers` reuses the cached type, a live on/off flip only
//! takes effect when the game creates a *fresh* swapchain (alt-enter / window
//! mode / resolution change). The factor only reloads on relaunch (Config is a
//! cached singleton). We surface that honestly.
//!
//! The overlay writes a small shared-state file (`<ms_home>/etc/metalfx.overlay.json`)
//! + updates `dxmt.conf` for the next launch. The in-process ntdll-hook polls
//! the state file and calls `SetEnvironmentVariableW` so the next swapchain
//! recreate picks up the on/off without a relaunch (x64 and i386 hooks).

use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// Shared state consumed by the ntdll-hook poller in the Wine game process.
/// The hook applies `DXMT_METALFX_SPATIAL_SWAPCHAIN` from `enabled` via
/// `SetEnvironmentVariableW`; `factor` is written to dxmt.conf for the next
/// launch (not live-reloadable). `ts` lets the hook detect changes.
#[derive(Clone, Copy)]
struct MetalFxState {
    enabled: bool,
    factor: f32,
    ts: u64,
}

fn state_path_for(home: &Path) -> PathBuf {
    home.join("etc").join("metalfx.overlay.json")
}

fn dxmt_conf_path_for(home: &Path) -> PathBuf {
    home.join("runtime").join("wine").join("etc").join("dxmt.conf")
}

fn now_ts() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn read_state(home: &Path) -> MetalFxState {
    let default = MetalFxState { enabled: true, factor: 1.5, ts: 0 };
    let Ok(txt) = fs::read_to_string(state_path_for(home)) else {
        return default;
    };
    let Ok(v): Result<Value, _> = serde_json::from_str(&txt) else {
        return default;
    };
    MetalFxState {
        enabled: v.get("enabled").and_then(|x| x.as_bool()).unwrap_or(true),
        factor: v.get("factor").and_then(|x| x.as_f64()).map(|f| f as f32).unwrap_or(1.5),
        ts: v.get("ts").and_then(|x| x.as_u64()).unwrap_or(0),
    }
}

/// The effective MetalFX state: enabled + factor, defaulting to enabled at
/// 1.50x when no state file exists. Path-based for tests. Used by the launcher
/// to reconcile the DXMT env at launch (the node's hardcoded 1.43 DXMT_CONFIG
/// must not shadow the overlay choice).
pub(crate) fn effective_state_for(home: &Path) -> (bool, f32) {
    let s = read_state(home);
    (s.enabled, s.factor)
}

fn write_state(home: &Path, state: MetalFxState) -> Result<(), String> {
    let path = state_path_for(home);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create etc dir: {e}"))?;
    }
    let body = json!({ "enabled": state.enabled, "factor": state.factor, "ts": state.ts });
    fs::write(&path, body.to_string()).map_err(|e| format!("write metalfx state: {e}"))
}

/// Read the `d3d11.metalSpatialUpscaleFactor` line from dxmt.conf (best-effort).
fn read_conf_factor(home: &Path) -> Option<f32> {
    let txt = fs::read_to_string(dxmt_conf_path_for(home)).ok()?;
    for line in txt.lines() {
        let l = line.trim();
        if let Some(rest) = l.strip_prefix("d3d11.metalSpatialUpscaleFactor") {
            let rest = rest.trim_start();
            let rest = rest.strip_prefix('=').unwrap_or(rest).trim();
            let val = rest.split_whitespace().next()?;
            return val.parse::<f32>().ok();
        }
    }
    None
}

/// Rewrite (or insert) the `d3d11.metalSpatialUpscaleFactor` line in dxmt.conf,
/// preserving every other line. Creates the file if absent.
fn write_conf_factor(home: &Path, factor: f32) -> Result<(), String> {
    let path = dxmt_conf_path_for(home);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create dxmt.conf dir: {e}"))?;
    }
    let existing = fs::read_to_string(&path).unwrap_or_default();
    let target_line = format!("d3d11.metalSpatialUpscaleFactor = {:.2}", factor);
    let mut out: Vec<String> = Vec::new();
    let mut replaced = false;
    for line in existing.lines() {
        if line.trim().starts_with("d3d11.metalSpatialUpscaleFactor") {
            out.push(target_line.clone());
            replaced = true;
        } else {
            out.push(line.to_string());
        }
    }
    if !replaced {
        out.push(target_line);
    }
    let mut content = out.join("\n");
    if !content.ends_with('\n') {
        content.push('\n');
    }
    fs::write(&path, content).map_err(|e| format!("write dxmt.conf: {e}"))?;
    crate::mtsp::launcher::ensure_dxmt_conf_shader_metal_version(home).map(|_| ())
}

/// `GET /metalfx/state` — current toggle + factor + how it applies.
pub fn get_state() -> Value {
    let home = crate::platform::metalsharp_home_dir();
    get_state_for(&home)
}

/// Path-parameterized core (testable without mutating process-global env).
pub fn get_state_for(home: &Path) -> Value {
    let s = read_state(home);
    let conf_factor = read_conf_factor(home).unwrap_or(1.5);
    json!({
        "ok": true,
        "enabled": s.enabled,
        "factor": s.factor,
        "conf_factor": conf_factor,
        "source": "metalfx.overlay.json",
        "applies": "on/off takes effect on the game's next swapchain recreate (alt-enter / resolution change); factor applies on relaunch",
    })
}

/// `POST /metalfx/toggle` — body `{ "enabled": bool, "factor"?: number }`.
/// Writes the shared state file (hook picks up on/off live) and updates
/// dxmt.conf (factor, next launch). Returns the new state.
pub fn set_state(body: &serde_json::Map<String, Value>) -> Value {
    let home = crate::platform::metalsharp_home_dir();
    set_state_for(&home, body)
}

pub fn set_state_for(home: &Path, body: &serde_json::Map<String, Value>) -> Value {
    let enabled = body.get("enabled").and_then(|v| v.as_bool());
    let factor = body.get("factor").and_then(|v| v.as_f64()).map(|f| f as f32);

    let mut current = read_state(home);
    if let Some(e) = enabled {
        current.enabled = e;
    }
    if let Some(f) = factor {
        if f.is_finite() && (1.0..=3.0).contains(&f) {
            current.factor = f;
        }
    }
    current.ts = now_ts();

    let mut errors: Vec<String> = Vec::new();
    if let Err(e) = write_state(home, current) {
        errors.push(e);
    }
    // Factor goes to dxmt.conf for the next launch (Config singleton can't
    // reload mid-process). On/off is delivered live via the hook + env.
    if let Err(e) = write_conf_factor(home, current.factor) {
        errors.push(e);
    }

    if errors.is_empty() {
        get_state_for(home)
    } else {
        json!({ "ok": false, "error": errors.join("; "), "enabled": current.enabled, "factor": current.factor })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static SEQ: AtomicU64 = AtomicU64::new(0);

    fn isolated_home() -> PathBuf {
        let n = SEQ.fetch_add(1, Ordering::SeqCst);
        let d = std::env::temp_dir().join(format!(
            "ms-metalfx-{}-{n}-{}",
            std::process::id(),
            SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()
        ));
        let _ = fs::create_dir_all(&d);
        d
    }

    #[test]
    fn toggle_roundtrips_state_and_dxmt_conf_factor() {
        let home = isolated_home();
        // initial: no state, no conf -> defaults (enabled at 1.50)
        let s0 = get_state_for(&home);
        assert_eq!(s0["enabled"], true);
        assert_eq!(s0["factor"], 1.5);
        assert_eq!(effective_state_for(&home), (true, 1.5));

        // change factor to 1.75 (stays enabled)
        let mut body = serde_json::Map::new();
        body.insert("enabled".into(), true.into());
        body.insert("factor".into(), 1.75.into());
        let r = set_state_for(&home, &body);
        assert_eq!(r["ok"], true);
        assert_eq!(r["enabled"], true);
        assert_eq!(r["factor"], 1.75);
        assert_eq!(effective_state_for(&home), (true, 1.75));

        // state file + dxmt.conf both written
        assert!(state_path_for(&home).exists());
        let conf = fs::read_to_string(dxmt_conf_path_for(&home)).unwrap();
        assert!(conf.contains("d3d11.metalSpatialUpscaleFactor = 1.75"));
        assert_eq!(read_conf_factor(&home), Some(1.75));

        // flip off without changing factor
        let mut body2 = serde_json::Map::new();
        body2.insert("enabled".into(), false.into());
        let r2 = set_state_for(&home, &body2);
        assert_eq!(r2["enabled"], false);
        assert_eq!(r2["factor"], 1.75);
        assert_eq!(effective_state_for(&home), (false, 1.75));

        // reject out-of-range factor
        let mut body3 = serde_json::Map::new();
        body3.insert("factor".into(), 0.5.into());
        let r3 = set_state_for(&home, &body3);
        assert_eq!(r3["factor"], 1.75, "out-of-range factor must be ignored");

        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn write_conf_factor_preserves_other_lines() {
        let home = isolated_home();
        let conf = dxmt_conf_path_for(&home);
        fs::create_dir_all(conf.parent().unwrap()).unwrap();
        fs::write(
            &conf,
            "d3d11.preferredMaxFrameRate = 60\n# comment\nd3d11.metalSpatialUpscaleFactor = 2.0\nd3d11.maxFeatureLevel = 12_1\n",
        )
        .unwrap();
        write_conf_factor(&home, 1.33).unwrap();
        let out = fs::read_to_string(&conf).unwrap();
        assert!(out.contains("d3d11.metalSpatialUpscaleFactor = 1.33"));
        assert!(out.contains("d3d11.preferredMaxFrameRate = 60"));
        assert!(out.contains("# comment"));
        assert!(out.contains("d3d11.maxFeatureLevel = 12_1"));
        assert!(!out.contains("= 2.0"));
        let _ = fs::remove_dir_all(&home);
    }
}
