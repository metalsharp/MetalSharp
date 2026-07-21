use super::engine::PipelineId;
use super::pe::{D3dApi, PeInfo};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

static RULES: OnceLock<HashMap<u32, PipelineId>> = OnceLock::new();
static GAME_RECIPES: OnceLock<HashMap<u32, GameRecipe>> = OnceLock::new();

#[derive(Debug, Clone, serde::Serialize)]
pub struct GameRecipe {
    pub pipeline: PipelineId,
    pub name: String,
    pub components: Vec<String>,
    pub env: HashMap<String, String>,
    pub check_dlls: Vec<String>,
    pub offline_capable: bool,
    pub exe_names: Vec<String>,
}

impl Default for GameRecipe {
    fn default() -> Self {
        Self {
            pipeline: PipelineId::M12,
            name: String::new(),
            components: Vec::new(),
            env: HashMap::new(),
            check_dlls: Vec::new(),
            offline_capable: false,
            exe_names: Vec::new(),
        }
    }
}

fn load_rules() -> &'static HashMap<u32, PipelineId> {
    RULES.get_or_init(|| {
        let home = dirs::home_dir().unwrap_or_default();
        let current_exe = std::env::current_exe().ok();

        for path in rule_candidates(&home, current_exe.as_deref()) {
            if path.exists() {
                if let Ok(contents) = std::fs::read_to_string(&path) {
                    let (pipelines, recipes) = parse_rules_full(&contents);
                    let _ = GAME_RECIPES.set(recipes);
                    return pipelines;
                }
            }
        }

        HashMap::new()
    })
}

fn load_game_recipes() -> &'static HashMap<u32, GameRecipe> {
    let _ = load_rules();
    GAME_RECIPES.get_or_init(HashMap::new)
}

fn rule_candidates(home: &Path, current_exe: Option<&Path>) -> Vec<PathBuf> {
    let mut candidates = Vec::new();

    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.join("configs").join("mtsp-rules.toml"));
    }

    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    candidates.push(manifest_dir.join("..").join("..").join("configs").join("mtsp-rules.toml"));

    candidates.extend([
        home.join("repos").join("metalsharp").join("configs").join("mtsp-rules.toml"),
        PathBuf::from("configs/mtsp-rules.toml"),
    ]);

    if let Some(exe) = current_exe {
        if let Some(mut dir) = exe.parent() {
            for _ in 0..8 {
                candidates.push(dir.join("configs").join("mtsp-rules.toml"));
                match dir.parent() {
                    Some(p) => dir = p,
                    None => break,
                }
            }
        }
    }

    candidates.push(home.join("metalsharp").join("configs").join("mtsp-rules.toml"));
    candidates.push(crate::platform::metalsharp_home_dir_for(&home).join("configs").join("mtsp-rules.toml"));
    candidates
}

fn parse_rules_full(toml_str: &str) -> (HashMap<u32, PipelineId>, HashMap<u32, GameRecipe>) {
    let mut pipelines = HashMap::new();
    let mut recipes = HashMap::new();

    let doc: toml::Value = match toml::from_str(toml_str) {
        Ok(v) => v,
        Err(_) => return (pipelines, recipes),
    };

    let overrides = match doc.get("overrides").and_then(|v| v.as_table()) {
        Some(t) => t,
        None => return (pipelines, recipes),
    };

    for (appid_str, entry) in overrides {
        let Ok(appid) = appid_str.parse::<u32>() else {
            continue;
        };
        let Some(pipeline_str) = entry.get("pipeline").and_then(|v| v.as_str()) else {
            continue;
        };
        let Some(pipeline) = PipelineId::from_str_flexible(pipeline_str) else {
            continue;
        };

        pipelines.insert(appid, pipeline);

        let name = entry.get("name").and_then(|v| v.as_str()).unwrap_or("").to_string();
        let components = entry
            .get("dependencies")
            .and_then(|d| d.get("components"))
            .and_then(|v| v.as_array())
            .map(|arr| arr.iter().filter_map(|v| v.as_str().map(String::from)).collect())
            .unwrap_or_default();
        let env = entry
            .get("env")
            .and_then(|v| v.as_table())
            .map(|t| t.iter().filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string()))).collect())
            .unwrap_or_default();
        let check_dlls = entry
            .get("diagnostics")
            .and_then(|d| d.get("check_dlls"))
            .and_then(|v| v.as_array())
            .map(|arr| arr.iter().filter_map(|v| v.as_str().map(String::from)).collect())
            .unwrap_or_default();
        let offline_capable = entry.get("offline_capable").and_then(|v| v.as_bool()).unwrap_or(false);
        let exe_names = entry
            .get("exe_names")
            .and_then(|v| v.as_array())
            .map(|arr| arr.iter().filter_map(|v| v.as_str().map(String::from)).collect())
            .unwrap_or_default();
        recipes.insert(appid, GameRecipe { pipeline, name, components, env, check_dlls, offline_capable, exe_names });
    }

    (pipelines, recipes)
}

fn parse_rules(toml_str: &str) -> HashMap<u32, PipelineId> {
    parse_rules_full(toml_str).0
}

pub fn resolve_pipeline(appid: u32) -> PipelineId {
    let rules = load_rules();

    if let Some(&pipeline) = rules.get(&appid) {
        return resolve_dxmt_alias(appid, pipeline);
    }

    let game_dir = crate::setup::resolve_windows_game_dir(appid).or_else(|| crate::setup::resolve_game_dir(appid));
    if let Some(ref dir) = game_dir {
        if dir.exists() {
            if crate::setup::detect_dotnet_game(dir) {
                return PipelineId::FnaArm64;
            }

            if let Some(pe_info) = super::pe::analyze_game_exe(dir) {
                if let Some(pipeline) = pe_info_to_pipeline(&pe_info) {
                    return pipeline;
                }
            }

            if let Some(detected) = detect_from_directory(dir) {
                return detected;
            }
        }
    }

    default_pipeline()
}

pub fn resolve_requested_pipeline(appid: u32, requested: Option<PipelineId>) -> PipelineId {
    match requested {
        Some(PipelineId::Dxmt) | None => resolve_pipeline(appid),
        Some(pipeline) => pipeline,
    }
}

fn resolve_dxmt_alias(appid: u32, pipeline: PipelineId) -> PipelineId {
    if pipeline == PipelineId::Dxmt {
        return detect_dxmt_pipeline(appid).unwrap_or_else(default_pipeline);
    }
    pipeline
}

fn detect_dxmt_pipeline(appid: u32) -> Option<PipelineId> {
    let game_dir = crate::setup::resolve_windows_game_dir(appid).or_else(|| crate::setup::resolve_game_dir(appid))?;
    if crate::setup::detect_dotnet_game(&game_dir) {
        return Some(PipelineId::FnaArm64);
    }
    if let Some(pe_info) = super::pe::analyze_game_exe(&game_dir) {
        if let Some(pipeline) = pe_info_to_pipeline(&pe_info) {
            return Some(pipeline);
        }
    }
    detect_from_directory(&game_dir)
}

pub fn get_game_recipe(appid: u32) -> Option<GameRecipe> {
    let recipes = load_game_recipes();
    recipes.get(&appid).cloned()
}

pub fn all_rules_with_recipes() -> Vec<(u32, GameRecipe)> {
    let _ = load_rules();
    let recipes = load_game_recipes();
    let mut result: Vec<(u32, GameRecipe)> = recipes.iter().map(|(&appid, recipe)| (appid, recipe.clone())).collect();
    result.sort_by_key(|(appid, _)| *appid);
    result
}

pub fn game_missing_dependencies(appid: u32, prefix: &Path) -> Vec<String> {
    let Some(recipe) = get_game_recipe(appid) else {
        return Vec::new();
    };
    if recipe.components.is_empty() {
        return Vec::new();
    }
    let mut missing = Vec::new();
    for component_id in &recipe.components {
        if !recipe_component_satisfied(component_id, prefix) {
            missing.push(component_id.clone());
        }
    }
    missing
}

fn recipe_component_satisfied(component_id: &str, prefix: &Path) -> bool {
    let drive_c = prefix.join("drive_c");
    let windows = drive_c.join("windows");
    let system32 = windows.join("system32");
    let syswow64 = windows.join("syswow64");
    let has_system_dll = |dll: &str| -> bool { system32.join(dll).exists() || syswow64.join(dll).exists() };
    let has_system32_dll = |dll: &str| -> bool { system32.join(dll).exists() };
    let has_syswow64_dll = |dll: &str| -> bool { syswow64.join(dll).exists() };

    match component_id {
        "vcrun2019" => ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"].iter().all(|dll| has_system_dll(dll)),
        "vcrun2019_x64" => {
            ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"].iter().all(|dll| has_system32_dll(dll))
        },
        "vcrun2019_x86" => ["vcruntime140.dll", "msvcp140.dll"].iter().all(|dll| has_syswow64_dll(dll)),
        "vcrun2010" => ["msvcr100.dll", "msvcp100.dll"].iter().all(|dll| has_system_dll(dll)),
        "vcrun2013" => ["msvcr120.dll", "msvcp120.dll"].iter().all(|dll| has_system_dll(dll)),
        "dotnet40" | "dotnet48" => {
            windows.join("Microsoft.NET").join("Framework").join("v4.0.30319").join("clr.dll").exists()
                || windows.join("Microsoft.NET").join("Framework64").join("v4.0.30319").join("clr.dll").exists()
        },
        "gecko" => windows.join("gecko").exists() || system32.join("gecko").exists() || syswow64.join("gecko").exists(),
        "webview2" => {
            drive_c.join("Program Files (x86)").join("Microsoft").join("EdgeWebView").exists()
                || drive_c.join("Program Files").join("Microsoft").join("EdgeWebView").exists()
        },
        "directx_jun2010" => {
            ["d3dx9_43.dll", "d3dx10_43.dll", "d3dx11_43.dll", "xinput1_3.dll"].iter().all(|dll| has_system_dll(dll))
        },
        "corefonts" => ["arial.ttf", "times.ttf"].iter().all(|font| windows.join("Fonts").join(font).exists()),
        "gpu_vendor_stubs" => ["nvapi64.dll", "nvngx.dll"].iter().all(|dll| has_system32_dll(dll)),
        "gptk_amd_stub" => has_system32_dll("atidxx64.dll"),
        _ => true,
    }
}

fn default_pipeline() -> PipelineId {
    PipelineId::M12
}

fn detect_from_directory(dir: &PathBuf) -> Option<PipelineId> {
    let has_file_ci = |name: &str| -> bool {
        let name_lower = name.to_lowercase();
        if let Ok(entries) = std::fs::read_dir(dir) {
            for entry in entries.flatten() {
                if entry.file_name().to_string_lossy().to_lowercase() == name_lower {
                    return true;
                }
            }
        }
        false
    };
    let has_dir_ci = |name: &str| -> bool {
        let name_lower = name.to_lowercase();
        if let Ok(entries) = std::fs::read_dir(dir) {
            for entry in entries.flatten() {
                if entry.path().is_dir() && entry.file_name().to_string_lossy().to_lowercase() == name_lower {
                    return true;
                }
            }
        }
        false
    };
    let has_glob = |pattern: &str| -> bool {
        if let Ok(entries) = std::fs::read_dir(dir) {
            for entry in entries.flatten() {
                let name = entry.file_name().to_string_lossy().to_lowercase();
                if name.ends_with(&pattern.to_lowercase()) {
                    return true;
                }
            }
        }
        false
    };

    if has_file_ci("unityplayer.dll") || has_file_ci("gameassembly.dll") {
        return Some(PipelineId::M11);
    }

    if has_dir_ci("engine") && has_dir_ci("binaries") {
        return Some(PipelineId::M11);
    }

    if has_glob(".pak") {
        return Some(PipelineId::M11);
    }

    if has_dir_ci("engine") && has_dir_ci("content") {
        return Some(PipelineId::M11);
    }

    if has_glob(".bdt") || has_glob(".bhd") {
        return Some(PipelineId::M11);
    }

    if has_glob("re_chunk_") || has_file_ci("re2_config.ini") || has_file_ci("re8_config.ini") {
        return Some(PipelineId::M11);
    }

    if has_file_ci("d3dx9_43.dll") {
        return Some(PipelineId::WineBare);
    }

    if has_file_ci("steam_api64.dll") || has_file_ci("steam_api.dll") {
        return Some(PipelineId::M11);
    }

    None
}

fn pe_info_to_pipeline(pe: &PeInfo) -> Option<PipelineId> {
    if !pe.is_64_bit {
        return Some(PipelineId::M9);
    }
    match pe.detected_api {
        D3dApi::D3D12 => Some(PipelineId::M12),
        D3dApi::D3D11 => Some(PipelineId::M11),
        D3dApi::D3D10 => Some(PipelineId::M10),
        D3dApi::D3D9 => Some(PipelineId::M9),
        D3dApi::Unknown => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn d3d12_pe_maps_to_m12() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d12.dll".into()],
            detected_api: D3dApi::D3D12,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M12));
    }

    #[test]
    fn broad_directory_heuristics_do_not_override_d3d12_pe_mapping() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d12.dll".into(), "steam_api64.dll".into()],
            detected_api: D3dApi::D3D12,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M12));
    }

    #[test]
    fn unresolved_games_default_to_main_m12_engine() {
        assert_eq!(default_pipeline(), PipelineId::M12);
    }

    #[test]
    fn d3d10_pe_maps_to_m10() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d10core.dll".into()],
            detected_api: D3dApi::D3D10,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M10));
    }

    #[test]
    fn d3d10_pe_mapping_is_not_demoted_to_m11_by_heuristics() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d10core.dll".into(), "steam_api64.dll".into()],
            detected_api: D3dApi::D3D10,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M10));
    }

    #[test]
    fn d3d10_32_bit_pe_routes_to_m9() {
        let pe = PeInfo {
            machine_type: 0x014c,
            is_64_bit: false,
            imports: vec!["d3d10.dll".into()],
            detected_api: D3dApi::D3D10,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M9));
    }

    #[test]
    fn d3d12_32_bit_pe_routes_to_m9() {
        let pe = PeInfo {
            machine_type: 0x014c,
            is_64_bit: false,
            imports: vec!["d3d12.dll".into()],
            detected_api: D3dApi::D3D12,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M9));
    }

    #[test]
    fn d3d11_32_bit_pe_routes_to_m9() {
        let pe = PeInfo {
            machine_type: 0x014c,
            is_64_bit: false,
            imports: vec!["d3d11.dll".into()],
            detected_api: D3dApi::D3D11,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M9));
    }

    #[test]
    fn d3d9_pe_maps_to_m9() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d9.dll".into()],
            detected_api: D3dApi::D3D9,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M9));
    }

    #[test]
    fn d3d9_pe_mapping_is_not_demoted_to_m11_by_heuristics() {
        let pe = PeInfo {
            machine_type: 0x8664,
            is_64_bit: true,
            imports: vec!["d3d9.dll".into(), "steam_api.dll".into()],
            detected_api: D3dApi::D3D9,
        };

        assert_eq!(pe_info_to_pipeline(&pe), Some(PipelineId::M9));
    }

    #[test]
    fn shipped_rules_cover_researched_installed_titles() {
        let rules = parse_rules(include_str!("../../../../configs/mtsp-rules.toml"));

        for (appid, pipeline) in [
            (17410, PipelineId::M9),
            (312520, PipelineId::M11),
            (387290, PipelineId::M11),
            (475150, PipelineId::M11_32),
            (504230, PipelineId::FnaArm64),
            (49520, PipelineId::M9),
            (508440, PipelineId::M11),
            (535520, PipelineId::M9),
            (774361, PipelineId::M9),
            (1169040, PipelineId::WineBare),
            (1237320, PipelineId::M11),
            (1245620, PipelineId::M12),
            (1562430, PipelineId::FnaArm64),
            (1623730, PipelineId::M12),
            (1868140, PipelineId::M11),
            (1928870, PipelineId::M12),
            (1962700, PipelineId::M12),
            (2358720, PipelineId::M11),
            (2456740, PipelineId::M12),
            (275850, PipelineId::WineBare),
            (284160, PipelineId::M11),
            (1326470, PipelineId::M11),
            (1583230, PipelineId::M12),
            (3164500, PipelineId::M11),
            (3527290, PipelineId::M12),
            (22380, PipelineId::M9),
            (1030300, PipelineId::M12),
            (222880, PipelineId::M11),
            (305620, PipelineId::M11),
            (1260320, PipelineId::M11),
            (1782210, PipelineId::M11),
            (1966720, PipelineId::M11),
            (2302640, PipelineId::M11),
            (291550, PipelineId::M11),
            (599140, PipelineId::M11),
            (4704690, PipelineId::M11),
        ] {
            assert_eq!(rules.get(&appid), Some(&pipeline), "appid {appid}");
        }
    }

    #[test]
    fn shipped_rules_precede_stale_user_copies() {
        let home = Path::new("/Users/alex");
        let current_exe = Path::new("/Applications/MetalSharp.app/Contents/MacOS/metalsharp-backend");
        let candidates = rule_candidates(home, Some(current_exe));

        let repo_rules = home.join("repos").join("metalsharp").join("configs").join("mtsp-rules.toml");
        let stale_user_rules = crate::platform::metalsharp_home_dir_for(&home).join("configs").join("mtsp-rules.toml");
        let repo_pos = candidates.iter().position(|path| path == &repo_rules).unwrap();
        let stale_user_pos = candidates.iter().position(|path| path == &stale_user_rules).unwrap();

        assert!(repo_pos < stale_user_pos);
    }

    #[test]
    fn game_recipes_parse_dependencies() {
        let (pipelines, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        assert!(!pipelines.is_empty());
        assert!(!recipes.is_empty());

        let elden = recipes.get(&1245620).expect("elden ring recipe");
        assert_eq!(elden.pipeline, PipelineId::M12);
        assert_eq!(elden.name, "ELDEN RING");
        assert!(elden.components.contains(&"vcrun2019".to_string()));
        assert!(elden.components.contains(&"directx_jun2010".to_string()));
        assert!(elden.check_dlls.contains(&"d3d12.dll".to_string()));
    }

    #[test]
    fn shipped_m11_m12_rules_have_no_anticheat_and_include_route_diagnostics() {
        let shipped_rules = include_str!("../../../../configs/mtsp-rules.toml");
        assert!(!shipped_rules.contains("anticheat"), "shipped rules must not contain anti-cheat metadata");
        let (_, recipes) = parse_rules_full(shipped_rules);

        let m12_required = ["d3d12.dll", "d3d11.dll", "dxgi_dxmt.dll", "dxgi.dll", "winemetal.dll"];
        let m11_required = ["d3d11.dll", "dxgi.dll", "winemetal.dll"];
        let required_by_pipeline =
            [(PipelineId::M12, m12_required.as_slice()), (PipelineId::M11, m11_required.as_slice())];

        for (pipeline, required) in required_by_pipeline {
            let matching_recipes = recipes.iter().filter(|(_, recipe)| recipe.pipeline == pipeline).collect::<Vec<_>>();
            assert!(!matching_recipes.is_empty(), "expected shipped {:?} rules", pipeline);
            for (appid, recipe) in matching_recipes {
                for dll in required {
                    assert!(
                        recipe.check_dlls.iter().any(|value| value == dll),
                        "appid {} {:?} diagnostics must include {} (got {:?})",
                        appid,
                        pipeline,
                        dll,
                        recipe.check_dlls
                    );
                }
            }
        }
    }

    #[test]
    fn game_recipes_parse_goat_simulator_m9_runtime() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let goat = recipes.get(&265930).expect("goat simulator recipe");
        assert_eq!(goat.pipeline, PipelineId::M9);
        assert!(goat.components.contains(&"dotnet40".to_string()));
        assert!(!goat.components.contains(&"dotnet48".to_string()));
        assert!(goat.components.contains(&"vcrun2010".to_string()));
        assert!(goat.components.contains(&"directx_jun2010".to_string()));
        assert!(goat.env.is_empty());
        assert!(goat.check_dlls.contains(&"d3d9.dll".to_string()));
        assert!(goat.check_dlls.contains(&"mscoree.dll".to_string()));
        assert!(goat.check_dlls.contains(&"msvcr100.dll".to_string()));
        assert!(goat.check_dlls.contains(&"msvcp100.dll".to_string()));
    }

    #[test]
    fn game_recipes_parse_titan_quest_m11_32_bit_route() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let titan_quest = recipes.get(&475150).expect("titan quest recipe");
        assert_eq!(titan_quest.pipeline, PipelineId::M11_32);
        assert_eq!(titan_quest.name, "Titan Quest Anniversary Edition");
        // M11(32) ships its own WINEDLLOVERRIDES via the pipeline node; the
        // recipe must not carry a stale d3d9 override that would clobber it.
        assert!(
            !titan_quest.env.contains_key("WINEDLLOVERRIDES"),
            "titan quest M11(32) recipe must not override the route's WINEDLLOVERRIDES"
        );
        assert!(titan_quest.check_dlls.contains(&"d3d11.dll".to_string()));
        assert!(titan_quest.check_dlls.contains(&"dxgi.dll".to_string()));
        assert!(titan_quest.check_dlls.contains(&"winemetal.dll".to_string()));
        assert!(titan_quest.components.contains(&"vcrun2019_x86".to_string()));
        assert!(!titan_quest.components.contains(&"vcrun2019".to_string()));
    }

    #[test]
    fn game_recipes_parse_hades_m11_32_exe_override() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let hades = recipes.get(&1145360).expect("hades recipe");
        assert_eq!(hades.pipeline, PipelineId::M11_32);
        assert_eq!(hades.name, "Hades");
        assert_eq!(hades.exe_names, vec!["x86/Hades.exe".to_string()]);
        assert!(hades.check_dlls.contains(&"d3d11.dll".to_string()));
        assert!(hades.check_dlls.contains(&"dxgi.dll".to_string()));
        assert!(hades.check_dlls.contains(&"winemetal.dll".to_string()));
    }

    #[test]
    fn game_recipes_parse_ori_m11_exe_override() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let ori = recipes.get(&387290).expect("ori recipe");
        assert_eq!(ori.pipeline, PipelineId::M11);
        assert_eq!(ori.name, "Ori and the Blind Forest: Definitive Edition");
        assert_eq!(ori.exe_names, vec!["oriDE.exe".to_string()]);
        assert!(ori.check_dlls.contains(&"d3d11.dll".to_string()));
        assert!(ori.check_dlls.contains(&"dxgi.dll".to_string()));
        assert!(ori.check_dlls.contains(&"winemetal.dll".to_string()));
    }

    #[test]
    fn game_recipes_parse_resident_evil_4_exe_override() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let re4 = recipes.get(&2050650).expect("resident evil 4 recipe");
        assert_eq!(re4.pipeline, PipelineId::M12);
        assert_eq!(re4.name, "Resident Evil 4");
        assert_eq!(re4.exe_names, vec!["re4.exe".to_string()]);
        assert!(re4.offline_capable);
    }

    #[test]
    fn game_recipes_parse_gta_v_rockstar_runtime() {
        let (_, recipes) = parse_rules_full(include_str!("../../../../configs/mtsp-rules.toml"));
        let gta = recipes.get(&271590).expect("gta v recipe");
        assert_eq!(gta.pipeline, PipelineId::M11);
        assert_eq!(gta.name, "Grand Theft Auto V Legacy");
        assert!(gta.components.contains(&"gecko".to_string()));
        assert!(gta.components.contains(&"webview2".to_string()));
        assert!(gta.components.contains(&"dotnet48".to_string()));
        assert!(gta.components.contains(&"vcrun2019_x64".to_string()));
        assert!(gta.components.contains(&"vcrun2019_x86".to_string()));
        assert!(gta.components.contains(&"vcrun2013".to_string()));
        assert!(gta.components.contains(&"directx_jun2010".to_string()));
        assert!(gta.components.contains(&"corefonts".to_string()));
        assert!(gta.check_dlls.contains(&"d3d11.dll".to_string()));
        assert!(gta.check_dlls.contains(&"dxgi.dll".to_string()));
    }

    #[test]
    fn recipe_component_detection_requires_complete_runtime_sets() {
        let root = test_prefix("recipe-component-completeness");
        let system32 = root.join("drive_c/windows/system32");
        let syswow64 = root.join("drive_c/windows/syswow64");
        std::fs::create_dir_all(&system32).expect("create system32");
        std::fs::create_dir_all(&syswow64).expect("create syswow64");

        std::fs::write(system32.join("vcruntime140.dll"), b"dll").expect("write partial vcrun");
        assert!(!recipe_component_satisfied("vcrun2019", &root));
        assert!(!recipe_component_satisfied("vcrun2019_x64", &root));
        std::fs::write(system32.join("vcruntime140_1.dll"), b"dll").expect("write vcrun dll");
        std::fs::write(system32.join("msvcp140.dll"), b"dll").expect("write vcrun dll");
        assert!(recipe_component_satisfied("vcrun2019", &root));
        assert!(recipe_component_satisfied("vcrun2019_x64", &root));
        assert!(!recipe_component_satisfied("vcrun2019_x86", &root));
        std::fs::write(syswow64.join("vcruntime140.dll"), b"dll").expect("write x86 vcrun dll");
        std::fs::write(syswow64.join("msvcp140.dll"), b"dll").expect("write x86 vcrun dll");
        assert!(recipe_component_satisfied("vcrun2019_x86", &root));

        std::fs::write(system32.join("msvcr100.dll"), b"dll").expect("write partial vcrun2010");
        assert!(!recipe_component_satisfied("vcrun2010", &root));
        std::fs::write(system32.join("msvcp100.dll"), b"dll").expect("write vcrun2010 dll");
        assert!(recipe_component_satisfied("vcrun2010", &root));

        let framework = root.join("drive_c/windows/Microsoft.NET/Framework/v4.0.30319");
        std::fs::create_dir_all(&framework).expect("create dotnet framework dir");
        std::fs::write(framework.join("mscorlib.dll"), b"dll").expect("write dotnet facade");
        assert!(!recipe_component_satisfied("dotnet48", &root));
        assert!(!recipe_component_satisfied("dotnet40", &root));
        std::fs::write(framework.join("clr.dll"), b"dll").expect("write native clr");
        assert!(recipe_component_satisfied("dotnet48", &root));
        assert!(recipe_component_satisfied("dotnet40", &root));

        assert!(!recipe_component_satisfied("gecko", &root));
        std::fs::create_dir_all(system32.join("gecko")).expect("create gecko dir");
        assert!(recipe_component_satisfied("gecko", &root));

        assert!(!recipe_component_satisfied("webview2", &root));
        std::fs::create_dir_all(root.join("drive_c/Program Files (x86)/Microsoft/EdgeWebView"))
            .expect("create webview dir");
        assert!(recipe_component_satisfied("webview2", &root));

        std::fs::write(system32.join("d3dx9_43.dll"), b"dll").expect("write partial directx");
        assert!(!recipe_component_satisfied("directx_jun2010", &root));
        for dll in ["d3dx10_43.dll", "d3dx11_43.dll", "xinput1_3.dll"] {
            std::fs::write(system32.join(dll), b"dll").expect("write directx dll");
        }
        assert!(recipe_component_satisfied("directx_jun2010", &root));

        std::fs::write(system32.join("nvapi64.dll"), b"dll").expect("write partial gpu vendor stubs");
        assert!(!recipe_component_satisfied("gpu_vendor_stubs", &root));
        std::fs::write(system32.join("nvngx.dll"), b"dll").expect("write gpu vendor stubs");
        assert!(recipe_component_satisfied("gpu_vendor_stubs", &root));
        assert!(!recipe_component_satisfied("gptk_amd_stub", &root));
        std::fs::write(system32.join("atidxx64.dll"), b"dll").expect("write amd vendor stub");
        assert!(recipe_component_satisfied("gptk_amd_stub", &root));

        let _ = std::fs::remove_dir_all(root);
    }

    fn test_prefix(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "metalsharp-rules-{}-{}-{}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
        ))
    }

    /// Parse the shipped TOML once, and validate:
    ///   1. No duplicate `[overrides.APPID]` headers
    ///   2. Every override has both a `name` and a `pipeline`
    ///   3. Every `pipeline` value is one of the known PipelineId strings
    ///   4. No empty `name` strings
    #[test]
    fn shipped_rules_toml_is_well_formed() {
        const SOURCE: &str = include_str!("../../../../configs/mtsp-rules.toml");
        let mut seen_sections: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
        let mut errors: Vec<String> = Vec::new();

        for line in SOURCE.lines() {
            let section = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')).unwrap_or("");
            if let Some(appid_str) = section.strip_prefix("overrides.") {
                // Only the `[overrides.NNNN]` top-level table counts;
                // sub-tables like `[overrides.NNNN.diagnostics]` are skipped.
                if !appid_str.contains('.') {
                    if let Ok(appid) = appid_str.parse::<u32>() {
                        *seen_sections.entry(appid).or_insert(0) += 1;
                    }
                }
            }
        }
        for (appid, count) in &seen_sections {
            if *count > 1 {
                errors.push(format!("[overrides.{}] is defined {count} times", appid));
            }
        }

        let (_, recipes) = parse_rules_full(SOURCE);
        for (appid, recipe) in &recipes {
            if recipe.name.trim().is_empty() {
                errors.push(format!("[overrides.{}] has empty name", appid));
            }
        }

        for line in SOURCE.lines() {
            if let Some(rest) = line.strip_prefix("pipeline = ") {
                let pipeline_str = rest.trim().trim_matches('"');
                if PipelineId::from_str_flexible(pipeline_str).is_none() {
                    errors.push(format!("unknown pipeline: {pipeline_str:?}"));
                }
            }
        }

        if !errors.is_empty() {
            let summary = format!("{} shipped-rules TOML validation error(s):", errors.len());
            panic!("{summary}\n  {}", errors.join("\n  "));
        }
    }
}
