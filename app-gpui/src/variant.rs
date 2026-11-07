//! Which MetalSharp build the backend is: `regular` (the x86_64 Wine runtime under Rosetta) or
//! `fex` (MetalSharp Wine, the native Arm64 Wine that runs x86 code through FEX). The backend
//! reports it as `variant` in `GET /status` and `GET /update/check`; an older backend leaves it
//! out, which means regular. The UI reads it wherever the two builds differ.
use serde_json::Value;
use std::sync::atomic::{AtomicBool, Ordering};

static FEX: AtomicBool = AtomicBool::new(false);
static APP_FEX: AtomicBool = AtomicBool::new(false);

/// True when the active Wine runtime is MetalSharp Wine (Arm64, FEX). A MetalSharp.app of either
/// build can run either runtime (the FEX | x86_64 toggle), so this follows the user's choice.
pub fn is_fex() -> bool {
    FEX.load(Ordering::Relaxed)
}

/// The active runtime's name: `fex` or `regular`.
pub fn name() -> &'static str {
    if is_fex() { "fex" } else { "regular" }
}

/// True when this MetalSharp.app is the FEX build (what updates download), whichever runtime is active.
pub fn app_is_fex() -> bool {
    APP_FEX.load(Ordering::Relaxed)
}

/// This MetalSharp.app's build: `fex` or `regular`.
pub fn app_name() -> &'static str {
    if app_is_fex() { "fex" } else { "regular" }
}

/// Records the active runtime (`variant`) and the app's build (`app_variant`) from a `/status`
/// reply. A reply without them (an older backend) is the regular build, whose runtime is its own.
pub fn set_from_reply(reply: &Value) {
    let fex = reply_is(reply, "variant");
    FEX.store(fex, Ordering::Relaxed);
    let app = if reply.get("app_variant").is_some() {
        reply_is(reply, "app_variant")
    } else {
        fex
    };
    APP_FEX.store(app, Ordering::Relaxed);
}

fn reply_is(reply: &Value, key: &str) -> bool {
    reply
        .get(key)
        .and_then(Value::as_str)
        .is_some_and(|variant| variant.eq_ignore_ascii_case("fex"))
}

/// The x86_64 runtime's lanes: MetalSharp Wine's (Mullion, VKD3D / DXVK, OpenGL on WineMetalGL) and D3DMetal,
/// which runs only under Rosetta. VKD3D / DXVK is one lane: VKD3D-Proton's D3D12 with the full DXVK (D3D11 to D3D8).
pub const X86_64_PIPELINE_OPTIONS: [(&str, &str); 4] = [
    ("d3dmetal", "D3DMetal"),
    ("vkd3d", "VKD3D / DXVK"),
    ("dxmt", "Mullion (DXMT)"),
    // Wine's own Direct3D on OpenGL, which WineMetalGL runs on Metal
    ("wine_bare", "OpenGL"),
];

/// On the x86_64 runtime GPTK reads as D3DMetal, DXVK and the D3D9 route as VKD3D / DXVK, 32-bit
/// DXMT as Mullion and Wine's own Direct3D as OpenGL, as the backend runs them. Mono/FNA stays.
pub fn x86_64_pipeline(id: &str) -> &str {
    match id {
        "m13" | "gptk" => "d3dmetal",
        "d3d9" | "m9" | "dxvk" | "dxvk_32" => "vkd3d",
        "dxmt_32" | "m10" | "m11" | "m10_32" | "m11_32" => "dxmt",
        "m32" | "wine" => "wine_bare",
        other => other,
    }
}

/// The pipelines a game can pick on FEX. D3DMetal, GPTK and the D3D9 route are not part of it
/// (DXVK covers D3D9), nor Mono/FNA (FNA games run as Windows programs under FEX).
pub const FEX_PIPELINE_OPTIONS: [(&str, &str); 4] = [
    ("dxmt", "Mullion (DXMT)"),
    ("vkd3d", "VKD3D-Proton"),
    ("dxvk", "DXVK"),
    // Wine's own Direct3D on OpenGL (WineMetalGL), with the game asked for its OpenGL renderer.
    ("wine_bare", "OpenGL"),
];

/// On FEX the backend runs D3DMetal and GPTK (D3D12 games) on VKD3D-Proton, the D3D9 route on
/// DXVK, and 32-bit DXMT and Mono/FNA on Mullion, so a saved or recommended pipeline of one of
/// those reads as that lane. Every other id is unchanged.
pub fn fex_pipeline(id: &str) -> &str {
    match id {
        "d3dmetal" | "m13" => "vkd3d",
        "dxmt_32" | "m10" | "m11" | "m10_32" | "m11_32" | "fna_arm64" | "fna_x86" => "dxmt",
        "d3d9" | "m9" | "dxvk_32" => "dxvk",
        "m32" | "wine" => "wine_bare",
        other => other,
    }
}

/// The FEX label for a pipeline id, after `fex_pipeline`.
pub fn fex_pipeline_label(id: &str) -> Option<&'static str> {
    let id = fex_pipeline(id);
    FEX_PIPELINE_OPTIONS
        .iter()
        .find(|(pid, _)| *pid == id)
        .map(|(_, label)| *label)
}

/// A backend-provided pipeline list as FEX shows it: D3DMetal, GPTK and D3D9 fold into their FEX
/// lanes (each shown once), and every known id gets its FEX label.
pub fn fex_options(options: impl IntoIterator<Item = (String, String)>) -> Vec<(String, String)> {
    let mut seen = std::collections::HashSet::new();
    options
        .into_iter()
        .map(|(id, label)| match fex_pipeline_label(&id) {
            Some(fex_label) => (fex_pipeline(&id).to_owned(), fex_label.to_owned()),
            None => (id, label),
        })
        .filter(|(id, _)| seen.insert(id.clone()))
        .collect()
}

/// FEX keeps the Sharp tools' D3DMetal flow: Apple's Game Porting Toolkit in its own prefix,
/// through Rosetta. These name it so.
pub const FEX_GPTK_LABEL: &str = "GPTK (Rosetta)";
pub const FEX_GPTK_PLAY_LABEL: &str = "Play with GPTK (Rosetta)";

/// The Wine the backend runs, for the Settings backend row.
pub const FEX_WINE_LABEL: &str = "MetalSharp Wine (Arm64, FEX)";

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn missing_or_regular_variant_is_regular() {
        assert!(!reply_is(&json!({"ok": true}), "variant"));
        assert!(!reply_is(
            &json!({"ok": true, "variant": "regular"}),
            "variant"
        ));
        assert!(!reply_is(&json!({"ok": true, "variant": null}), "variant"));
        assert!(reply_is(&json!({"ok": true, "variant": "fex"}), "variant"));
    }

    #[test]
    fn rosetta_only_pipelines_read_as_fex_lanes() {
        for id in ["dxmt", "dxmt_32"] {
            assert_eq!(fex_pipeline(id), "dxmt");
            assert_eq!(fex_pipeline_label(id), Some("Mullion (DXMT)"));
        }
        for id in ["d3dmetal", "m13", "vkd3d"] {
            assert_eq!(fex_pipeline_label(id), Some("VKD3D-Proton"));
        }
        for id in ["d3d9", "m9", "dxvk"] {
            assert_eq!(fex_pipeline_label(id), Some("DXVK"));
        }
        assert_eq!(fex_pipeline_label("wine_bare"), Some("OpenGL"));
        assert_eq!(fex_pipeline_label("auto"), None);
    }

    #[test]
    fn x86_64_runs_dxvk_on_the_vkd3d_dxvk_lane() {
        assert!(X86_64_PIPELINE_OPTIONS.iter().all(|(id, _)| *id != "dxvk"));
        assert!(X86_64_PIPELINE_OPTIONS.contains(&("vkd3d", "VKD3D / DXVK")));
        for id in ["dxvk", "dxvk_32", "d3d9", "m9", "vkd3d"] {
            assert_eq!(x86_64_pipeline(id), "vkd3d", "{id}");
        }
    }
}
