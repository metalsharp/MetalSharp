//! Default rules surface.
//!
//! Exposes the "Default Rules" catalog (appid -> default pipeline + direct exe
//! fix + default launch shape) and a per-pipeline launch-shape resolver so the
//! renderer can preview what a route switch applies before launching.
//!
//! The "default" rule is the one set when a game installs (the shipped
//! `mtsp-rules.toml` override). A user can still change it per-bottle via
//! `/bottles/edit` or `/bottles/set-runtime-profile`; the chosen pipeline
//! becomes the bottle's `preferred_pipeline` and is honored at launch and in
//! the bottle card. This module only describes the shapes — it does not mutate
//! state. Deployment + cleanup on a route switch is handled by the MTSP
//! launcher (`prepare_steam_pipeline_env` -> `deploy_recipe_dlls` +
//! `quarantine_route_conflicts_for_recipe`, which now discards stale route
//! DLLs that match a runtime source instead of quarantining them).

use super::engine::{get_pipeline, PipelineId, PipelineNode};
use super::rules::{all_rules_with_recipes, get_game_recipe, GameRecipe};
use serde_json::{json, Value};

/// `true` for rules that carry a custom direct-exe fix (an `exe_names`
/// override) rather than relying on generic exe scoring.
fn has_custom_exe_fix(recipe: &GameRecipe) -> bool {
    !recipe.exe_names.is_empty()
}

/// "64-bit", "32-bit", or "mixed" derived from a deploy source subpath.
fn dll_arch_for_source_subpath(subpath: &str) -> &'static str {
    if subpath.contains("i386") {
        "32-bit"
    } else if subpath.contains("x86_64") {
        "64-bit"
    } else {
        "mixed"
    }
}

/// The deploy shape for a pipeline node: the WINEDLLOVERRIDES the route
/// applies, and the DLLs it deploys (filename, source subpath, arch). This is
/// derived purely from the pipeline definition so it is identical to what
/// `prepare_steam_pipeline_env` + `deploy_recipe_dlls` will stage at launch.
fn deploy_shape_for_node(node: &PipelineNode) -> Value {
    let dlls: Vec<Value> = node
        .deploy_dlls
        .iter()
        .map(|deploy| {
            json!({
                "filename": deploy.filename,
                "source_subpath": deploy.source_subpath,
                "arch": dll_arch_for_source_subpath(deploy.source_subpath),
                "dest_filename": deploy.dest_filename,
            })
        })
        .collect();
    json!({
        "wine_overrides": node.wine_overrides,
        "dyld_paths": node.dyld_paths,
        "winedllpath_dirs": node.winedllpath_dirs,
        "deploy_dlls": dlls,
    })
}

/// The default pipeline for an appid — the shipped rule, i.e. the one applied
/// at install time. Falls back to `resolve_pipeline` (PE/directory heuristics)
/// when no explicit override exists.
fn default_pipeline_for(appid: u32) -> PipelineId {
    super::rules::resolve_pipeline(appid)
}

/// One entry in the Default Rules catalog.
fn catalog_entry(appid: u32, recipe: &GameRecipe) -> Value {
    let node = get_pipeline(recipe.pipeline);
    let shape = deploy_shape_for_node(node);
    json!({
        "appid": appid,
        "name": recipe.name,
        "default_pipeline": recipe.pipeline.user_selectable_id().unwrap_or("auto"),
        "default_pipeline_name": node.name,
        "custom_exe_fix": has_custom_exe_fix(recipe),
        "exe_names": recipe.exe_names,
        "eac_exe_names": recipe.eac_exe_names,
        "offline_capable": recipe.offline_capable,
        "components": recipe.components,
        "check_dlls": recipe.check_dlls,
        "env": recipe.env,
        "launch_shape": shape,
    })
}

/// `GET /mtsp/default-rules` — the full catalog of custom -> direct-exe-fix
/// rules with their default launch shape. Sorted by appid.
pub fn handle_default_rules_catalog() -> Value {
    let entries: Vec<Value> =
        all_rules_with_recipes().iter().map(|(appid, recipe)| catalog_entry(*appid, recipe)).collect();
    json!({"ok": true, "rules": entries, "count": entries.len()})
}

/// `GET /mtsp/launch-shape?appid=<>&pipeline=<>` — the launch shape a specific
/// pipeline would apply to a given game, regardless of the default rule. Used
/// by the bottle dropdown to preview a route switch (e.g. Hades DXMT(32) ->
/// DXMT). `pipeline=auto` resolves to the default rule.
pub fn handle_launch_shape(appid: u32, pipeline: Option<PipelineId>) -> Value {
    let recipe = get_game_recipe(appid);
    let resolved = pipeline.unwrap_or_else(|| default_pipeline_for(appid));
    let node = get_pipeline(resolved);
    let shape = deploy_shape_for_node(node);
    json!({
        "ok": true,
        "appid": appid,
        "pipeline": resolved.user_selectable_id().unwrap_or("auto"),
        "pipeline_name": node.name,
        "backend": node.backend,
        "graphics_backend": node.graphics_backend,
        "requires_wine": node.requires_wine,
        "is_default_rule": recipe.as_ref().map(|r| r.pipeline == resolved).unwrap_or(false),
        "custom_exe_fix": recipe.as_ref().map(has_custom_exe_fix).unwrap_or(false),
        "exe_names": recipe.as_ref().map(|r| r.exe_names.clone()).unwrap_or_default(),
        "launch_shape": shape,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catalog_includes_hades_and_titan_quest_with_dxmt32_default() {
        let catalog = handle_default_rules_catalog();
        let rules = catalog.get("rules").and_then(|v| v.as_array()).expect("rules array");
        let hades = rules
            .iter()
            .find(|entry| entry.get("appid").and_then(|v| v.as_u64()) == Some(1145360))
            .expect("hades rule present");
        assert_eq!(hades.get("default_pipeline").and_then(|v| v.as_str()), Some("dxmt_32"));
        assert_eq!(hades.get("custom_exe_fix").and_then(|v| v.as_bool()), Some(true));
        let exe_names = hades.get("exe_names").and_then(|v| v.as_array()).expect("exe_names");
        assert_eq!(exe_names.len(), 1);
        assert_eq!(exe_names[0].as_str(), Some("x86/Hades.exe"));

        let tq = rules
            .iter()
            .find(|entry| entry.get("appid").and_then(|v| v.as_u64()) == Some(475150))
            .expect("titan quest rule present");
        assert_eq!(tq.get("default_pipeline").and_then(|v| v.as_str()), Some("dxmt_32"));
        assert_eq!(tq.get("custom_exe_fix").and_then(|v| v.as_bool()), Some(false));
    }

    #[test]
    fn launch_shape_for_hades_dxmt_reports_64_bit_dxmt_deploy_set() {
        // Switching Hades from the DXMT(32) default to plain DXMT must report the
        // 64-bit DXMT deploy set (Wine d3d10/d3d10_1 + DXMT d3d11/dxgi/d3d10core/
        // winemetal from x86_64 lanes) and the DXMT wine overrides, so the UI
        // can show the user exactly what will be applied.
        let shape = handle_launch_shape(1145360, Some(PipelineId::Dxmt));
        assert_eq!(shape.get("pipeline").and_then(|v| v.as_str()), Some("dxmt"));
        assert_eq!(shape.get("pipeline_name").and_then(|v| v.as_str()), Some("DXMT"));
        assert_eq!(shape.get("is_default_rule").and_then(|v| v.as_bool()), Some(false));
        assert_eq!(shape.get("custom_exe_fix").and_then(|v| v.as_bool()), Some(true));

        let launch_shape = shape.get("launch_shape").expect("launch_shape");
        let overrides = launch_shape.get("wine_overrides").and_then(|v| v.as_str()).unwrap_or_default();
        assert!(
            overrides.contains("d3d10")
                && overrides.contains("d3d11")
                && overrides.contains("dxgi")
                && overrides.contains("winemetal")
                && overrides.contains("=n,b"),
            "DXMT route overrides must be present, got {}",
            overrides
        );

        let dlls = launch_shape.get("deploy_dlls").and_then(|v| v.as_array()).expect("deploy_dlls");
        let filenames: Vec<&str> = dlls.iter().filter_map(|d| d.get("filename").and_then(|v| v.as_str())).collect();
        for required in ["d3d10.dll", "d3d10_1.dll", "d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll"] {
            assert!(filenames.contains(&required), "DXMT deploy set missing {}", required);
        }
        assert!(!filenames.contains(&"dxgi_dxmt.dll"), "DXMT deploy set must not include M12-owned dxgi_dxmt.dll");
        for dll in dlls {
            assert_eq!(dll.get("arch").and_then(|v| v.as_str()), Some("64-bit"));
            assert!(dll.get("source_subpath").and_then(|v| v.as_str()).unwrap_or("").contains("x86_64-windows"));
        }
    }

    #[test]
    fn launch_shape_for_hades_dxmt32_reports_32_bit_dxmt_deploy_set() {
        let shape = handle_launch_shape(1145360, Some(PipelineId::Dxmt32));
        assert_eq!(shape.get("pipeline").and_then(|v| v.as_str()), Some("dxmt_32"));
        assert_eq!(shape.get("is_default_rule").and_then(|v| v.as_bool()), Some(true));
        let dlls = shape
            .get("launch_shape")
            .and_then(|v| v.get("deploy_dlls"))
            .and_then(|v| v.as_array())
            .expect("deploy_dlls");
        let filenames: Vec<&str> = dlls.iter().filter_map(|d| d.get("filename").and_then(|v| v.as_str())).collect();
        for required in ["d3d10.dll", "d3d10_1.dll", "d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll"] {
            assert!(filenames.contains(&required), "DXMT(32) deploy set missing {}", required);
        }
        assert!(!filenames.contains(&"dxgi_dxmt.dll"), "DXMT(32) deploy set must not include M12-owned dxgi_dxmt.dll");
        for dll in dlls {
            assert_eq!(dll.get("arch").and_then(|v| v.as_str()), Some("32-bit"));
            assert!(dll.get("source_subpath").and_then(|v| v.as_str()).unwrap_or("").contains("i386-windows"));
        }
    }

    #[test]
    fn launch_shape_auto_resolves_to_default_rule() {
        let auto = handle_launch_shape(1145360, None);
        assert_eq!(auto.get("pipeline").and_then(|v| v.as_str()), Some("dxmt_32"));
        assert_eq!(auto.get("is_default_rule").and_then(|v| v.as_bool()), Some(true));
    }

    #[test]
    fn dll_arch_classification_is_subpath_based() {
        assert_eq!(dll_arch_for_source_subpath("lib/dxmt/i386-windows"), "32-bit");
        assert_eq!(dll_arch_for_source_subpath("lib/dxmt/x86_64-windows"), "64-bit");
        assert_eq!(dll_arch_for_source_subpath("lib/wine/i386-windows"), "32-bit");
        assert_eq!(dll_arch_for_source_subpath("lib/vkd3d-proton/x86_64-windows"), "64-bit");
        assert_eq!(dll_arch_for_source_subpath("lib/other"), "mixed");
    }
}
