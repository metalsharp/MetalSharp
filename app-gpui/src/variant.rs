//! Which MetalSharp build the backend is: `regular` (the x86_64 Wine runtime under Rosetta) or
//! `fex` (MetalSharp Wine, the native Arm64 Wine that runs x86 code through FEX). The backend
//! reports it as `variant` in `GET /status` and `GET /update/check`; an older backend leaves it
//! out, which means regular. The UI reads it wherever the two builds differ.
use serde_json::Value;
use std::sync::atomic::{AtomicBool, Ordering};

static FEX: AtomicBool = AtomicBool::new(false);

/// True when the backend is the Arm64 build with FEX.
pub fn is_fex() -> bool {
    FEX.load(Ordering::Relaxed)
}

/// The backend's variant name: `fex` or `regular`.
pub fn name() -> &'static str {
    if is_fex() { "fex" } else { "regular" }
}

/// Records the variant from a `/status` or `/update/check` reply. A reply without the field
/// (an older backend) is the regular build.
pub fn set_from_reply(reply: &Value) {
    FEX.store(reply_is_fex(reply), Ordering::Relaxed);
}

fn reply_is_fex(reply: &Value) -> bool {
    reply
        .get("variant")
        .and_then(Value::as_str)
        .is_some_and(|variant| variant.eq_ignore_ascii_case("fex"))
}

/// The pipelines a game can pick on FEX. D3DMetal, VKD3D and GPTK are not part of it.
pub const FEX_PIPELINE_OPTIONS: [(&str, &str); 5] = [
    ("dxmt", "Mullion (DXMT)"),
    ("dxvk", "DXVK"),
    ("d3d9", "D3D9"),
    ("wine_bare", "Wine"),
    ("fna_arm64", "Mono/FNA"),
];

/// On FEX the backend runs D3DMetal, VKD3D, GPTK and 32-bit DXMT on Mullion, so a saved
/// pipeline of one of those reads as Mullion (DXMT). Every other id is unchanged.
pub fn fex_pipeline(id: &str) -> &str {
    match id {
        "d3dmetal" | "vkd3d" | "m13" | "dxmt_32" | "m10" | "m11" | "m10_32" | "m11_32" => "dxmt",
        "dxvk_32" => "dxvk",
        "m9" => "d3d9",
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

/// A backend-provided pipeline list as FEX shows it: D3DMetal, VKD3D and GPTK become one
/// Mullion (DXMT) entry, and every known id gets its FEX label.
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
        assert!(!reply_is_fex(&json!({"ok": true})));
        assert!(!reply_is_fex(&json!({"ok": true, "variant": "regular"})));
        assert!(!reply_is_fex(&json!({"ok": true, "variant": null})));
        assert!(reply_is_fex(&json!({"ok": true, "variant": "fex"})));
    }

    #[test]
    fn rosetta_only_pipelines_read_as_mullion() {
        for id in ["d3dmetal", "vkd3d", "m13", "dxmt", "dxmt_32"] {
            assert_eq!(fex_pipeline(id), "dxmt");
            assert_eq!(fex_pipeline_label(id), Some("Mullion (DXMT)"));
        }
        assert_eq!(fex_pipeline_label("dxvk"), Some("DXVK"));
        assert_eq!(fex_pipeline_label("d3d9"), Some("D3D9"));
        assert_eq!(fex_pipeline_label("wine_bare"), Some("Wine"));
        assert_eq!(fex_pipeline_label("auto"), None);
    }
}
