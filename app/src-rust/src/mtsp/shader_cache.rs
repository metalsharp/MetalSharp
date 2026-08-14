use serde::Serialize;
use std::path::{Path, PathBuf};

pub fn deploy_preset_cache(home: &PathBuf, cache_subdir: &str, appid: u32) -> Option<u64> {
    let preset_db = find_preset(home, cache_subdir, appid)?;

    let cache_base =
        crate::platform::metalsharp_home_dir_for(&home).join("shader-cache").join(cache_subdir).join(appid.to_string());
    let _ = std::fs::create_dir_all(&cache_base);

    let user_db = cache_base.join(preset_db.file_name()?.to_string_lossy().to_string());

    if user_db.exists() {
        return merge_preset_into_user(&preset_db, &user_db);
    }

    std::fs::copy(&preset_db, &user_db).ok()
}

fn find_preset(home: &PathBuf, cache_subdir: &str, appid: u32) -> Option<PathBuf> {
    let ms_share = crate::platform::metalsharp_home_dir_for(&home)
        .join("runtime")
        .join("wine")
        .join("share")
        .join("shader-presets");
    let exe_presets = find_exe_relative_presets();

    let search_dirs: Vec<PathBuf> = {
        let mut dirs = vec![ms_share];
        dirs.extend(exe_presets);
        dirs
    };

    for subdir in preset_lookup_subdirs(cache_subdir) {
        for preset_dir in &search_dirs {
            if let Some(p) = check_preset_dir(preset_dir, subdir, appid) {
                return Some(p);
            }
        }
    }

    None
}

fn preset_lookup_subdirs(cache_subdir: &str) -> Vec<&str> {
    match cache_subdir {
        "dxmt" | "dxmt_32" => vec![cache_subdir, "dxmt-metal"],
        "vkd3d" => vec![cache_subdir, "dxmt-metal12"],
        _ => vec![cache_subdir],
    }
}

fn find_exe_relative_presets() -> Vec<PathBuf> {
    let mut results = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(mut dir) = exe.parent() {
            for _ in 0..8 {
                let candidate = dir.join("shader-presets");
                if candidate.exists() {
                    results.push(candidate);
                }
                match dir.parent() {
                    Some(p) => dir = p,
                    None => break,
                }
            }
        }
    }
    results
}

fn check_preset_dir(preset_dir: &PathBuf, cache_subdir: &str, appid: u32) -> Option<PathBuf> {
    let engine_dir = preset_dir.join(cache_subdir);
    if !engine_dir.exists() {
        return None;
    }

    let candidates =
        vec![engine_dir.join(format!("{}.db", appid)), engine_dir.join(format!("shaders_320_{}.db", appid))];

    for c in &candidates {
        if c.exists() {
            return Some(c.clone());
        }
    }

    if let Ok(entries) = std::fs::read_dir(&engine_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.starts_with(&appid.to_string()) && name.ends_with(".db") {
                return Some(entry.path());
            }
        }
    }

    None
}

fn merge_preset_into_user(preset_db: &PathBuf, user_db: &PathBuf) -> Option<u64> {
    let mut inserted: u64 = 0;

    let preset_conn = match rusqlite::Connection::open_with_flags(preset_db, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY)
    {
        Ok(c) => c,
        Err(_) => return None,
    };

    let user_conn = match rusqlite::Connection::open(user_db) {
        Ok(c) => c,
        Err(_) => return None,
    };

    let table_names: Vec<String> = preset_conn
        .prepare("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache_%'")
        .ok()
        .map(|mut stmt| {
            stmt.query_map([], |row| row.get(0))
                .ok()
                .map(|iter| iter.filter_map(|r| r.ok()).collect())
                .unwrap_or_default()
        })
        .unwrap_or_default();

    if table_names.is_empty() {
        return None;
    }

    let _ = user_conn.execute_batch("PRAGMA journal_mode=WAL;");

    for table in &table_names {
        let _ =
            user_conn.execute(&format!("CREATE TABLE IF NOT EXISTS {} (key BLOB PRIMARY KEY, value BLOB)", table), []);

        let mut sel_stmt = match preset_conn.prepare(&format!("SELECT key, value FROM {}", table)) {
            Ok(s) => s,
            Err(_) => continue,
        };

        let rows: Vec<(Vec<u8>, Vec<u8>)> = sel_stmt
            .query_map([], |row| {
                let key: Vec<u8> = row.get(0)?;
                let val: Vec<u8> = row.get(1)?;
                Ok((key, val))
            })
            .ok()
            .map(|iter| iter.filter_map(|r| r.ok()).collect())
            .unwrap_or_default();

        let _ = user_conn.execute_batch("BEGIN TRANSACTION;");

        let ins_sql = format!("INSERT OR IGNORE INTO {} (key, value) VALUES (?1, ?2)", table);
        if let Ok(mut ins_stmt) = user_conn.prepare(&ins_sql) {
            for (key, val) in &rows {
                if ins_stmt.execute(rusqlite::params![key, val]).is_ok() {
                    inserted += 1;
                }
            }
        }

        let _ = user_conn.execute_batch("COMMIT;");
    }

    if inserted > 0 {
        Some(inserted)
    } else {
        None
    }
}

// ============================================================================
// Phase 4: Shader / PSO / cache diagnostics
// ============================================================================
//
// The DXMT runtime is shipped prebuilt under lib/dxmt(-vkd3d); vendor/dxmt is
// reference source and is NOT compiled by this repo's CMake build. These Rust
// diagnostics therefore observe the runtime's on-disk products (the DXMT
// SQLite shader/pipeline caches and any JSON PSO trace sidecars DXMT emits
// under DXMT_LOG_PATH) without touching shader lowering semantics.
//
// This standardizes the trace SHAPE so a future DXMT build (or the existing
// trace flags such as DXMT_D3D12_TRACE) has a stable parsing surface, and it
// gives the cache doctor a real, testable introspection path today.

/// The shader-cache family a pipeline shares. DXMT/DXMT(32) share the
/// legacy `dxmt-metal` family; VKD3D/M13 use the isolated `dxmt-metal12` family.
pub fn shader_cache_family(pipeline: crate::mtsp::engine::PipelineId) -> &'static [&'static str] {
    use crate::mtsp::engine::PipelineId;
    match pipeline {
        PipelineId::Dxmt => &["dxmt", "dxmt-metal"],
        PipelineId::Dxmt32 => &["dxmt_32", "dxmt-metal"],
        PipelineId::Vkd3d => &["vkd3d", "dxmt-metal12"],
        PipelineId::M13 => &["m13", "dxmt-metal12"],
        _ => &[],
    }
}

/// The primary cache subdir a pipeline writes to (its isolated lane).
pub fn primary_cache_subdir(pipeline: crate::mtsp::engine::PipelineId) -> Option<&'static str> {
    use crate::mtsp::engine::PipelineId;
    match pipeline {
        PipelineId::Dxmt => Some("dxmt"),
        PipelineId::Dxmt32 => Some("dxmt_32"),
        PipelineId::Vkd3d => Some("vkd3d"),
        PipelineId::M13 => Some("m13"),
        _ => None,
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct CacheDbSummary {
    pub name: String,
    pub path: String,
    pub size_bytes: u64,
    pub mtime_unix: Option<u64>,
    pub entry_count: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct CacheDirSummary {
    pub path: String,
    pub exists: bool,
    pub db_files: Vec<CacheDbSummary>,
    pub total_entries: u64,
    pub newest_mtime_unix: Option<u64>,
    pub oldest_mtime_unix: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
pub struct CacheDoctorReport {
    pub schema_version: u32,
    pub ok: bool,
    pub appid: u32,
    pub pipeline: String,
    pub cache_family: Vec<&'static str>,
    pub shader_cache: CacheDirSummary,
    pub pipeline_cache: CacheDirSummary,
    /// sha256 of the staged runtime DLL recorded in injections.json, used to
    /// detect caches built against an older runtime build.
    pub runtime_artifact_hash: Option<String>,
    pub stale_warning: Option<String>,
}

/// Count the rows across every `cache_*` table in a DXMT SQLite cache DB.
/// Opens read-only so it never mutates a live cache.
fn count_cache_entries(db_path: &Path) -> u64 {
    let Ok(conn) = rusqlite::Connection::open_with_flags(db_path, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY) else {
        return 0;
    };
    let Ok(mut stmt) = conn.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'cache_%'") else {
        return 0;
    };
    let table_names: Vec<String> = stmt
        .query_map([], |row| row.get::<_, String>(0))
        .map(|iter| iter.filter_map(|r| r.ok()).collect())
        .unwrap_or_default();
    let mut total = 0u64;
    for table in &table_names {
        // Table names come from sqlite_master and match cache_<hex>; reject
        // anything unexpected before interpolating.
        if !table.starts_with("cache_") || table.chars().any(|c| !(c.is_ascii_alphanumeric() || c == '_')) {
            continue;
        }
        let sql = format!("SELECT COUNT(*) FROM {}", table);
        if let Ok(value) = conn.query_row(&sql, [], |row| row.get::<_, i64>(0)) {
            total += value.max(0) as u64;
        }
    }
    total
}

fn summarize_cache_dir(dir: &Path) -> CacheDirSummary {
    let mut summary = CacheDirSummary {
        path: dir.to_string_lossy().to_string(),
        exists: dir.exists(),
        db_files: Vec::new(),
        total_entries: 0,
        newest_mtime_unix: None,
        oldest_mtime_unix: None,
    };
    if !summary.exists {
        return summary;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return summary;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("db") {
            continue;
        }
        let Ok(meta) = std::fs::metadata(&path) else {
            continue;
        };
        let mtime =
            meta.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs());
        let entry_count = count_cache_entries(&path);
        summary.total_entries += entry_count;
        summary.newest_mtime_unix = match (summary.newest_mtime_unix, mtime) {
            (Some(a), Some(b)) => Some(a.max(b)),
            (None, b) => b,
            (a, None) => a,
        };
        summary.oldest_mtime_unix = match (summary.oldest_mtime_unix, mtime) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (None, b) => b,
            (a, None) => a,
        };
        summary.db_files.push(CacheDbSummary {
            name: path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default(),
            path: path.to_string_lossy().to_string(),
            size_bytes: meta.len(),
            mtime_unix: mtime,
            entry_count,
        });
    }
    summary
}

/// Read the staged runtime DLL sha256 recorded in the game's injections.json,
/// if any. Used to detect caches built against an older runtime build.
fn staged_runtime_hash(home: &Path, appid: u32) -> Option<String> {
    let dual = crate::scan::resolve_dual_game_dir(appid);
    let game_dir = dual.wine_dir?;
    let manifest_path = game_dir.join(".metalsharp").join("injections.json");
    let raw = std::fs::read_to_string(&manifest_path).ok()?;
    let manifest: serde_json::Value = serde_json::from_str(&raw).ok()?;
    manifest.get("dlls").and_then(|v| v.as_array())?.iter().find_map(|dll| {
        if dll.get("filename").and_then(|v| v.as_str()) == Some("d3d12.dll") {
            dll.get("sha256").and_then(|v| v.as_str()).map(|s| s.to_string())
        } else {
            None
        }
    })
}

/// Build the cache doctor report for an appid, resolving the pipeline the same
/// way the diagnostic route does.
pub fn cache_doctor(appid: u32) -> serde_json::Value {
    let Some(home) = dirs::home_dir() else {
        return serde_json::json!({"ok": false, "appid": appid, "error": "home directory could not be resolved"});
    };
    let pipeline = crate::bottles::resolve_steam_pipeline_for_request(appid, None);
    cache_doctor_for(&home, pipeline, appid)
}

/// Cache doctor with an explicit home (used by tests; never mutates global env).
pub fn cache_doctor_for(home: &Path, pipeline: crate::mtsp::engine::PipelineId, appid: u32) -> serde_json::Value {
    let ms_home = crate::platform::metalsharp_home_dir_for(home);
    let appid_str = appid.to_string();
    let family = shader_cache_family(pipeline);
    let primary = primary_cache_subdir(pipeline).unwrap_or("");

    // Summarize every family member dir; report the primary lane entry first.
    let shader_cache = summarize_cache_dir(&ms_home.join("shader-cache").join(primary).join(&appid_str));
    let pipeline_cache = summarize_cache_dir(&ms_home.join("pipeline-cache").join(primary).join(&appid_str));

    let runtime_artifact_hash = staged_runtime_hash(home, appid);
    let stale_warning = if shader_cache.total_entries > 0 && runtime_artifact_hash.is_none() {
        Some("shader cache has entries but no runtime artifact hash is recorded for this game; staleness cannot be verified".to_string())
    } else {
        None
    };

    let report = CacheDoctorReport {
        schema_version: 1,
        ok: true,
        appid,
        pipeline: pipeline_preference_id_str(pipeline).to_string(),
        cache_family: family.to_vec(),
        shader_cache,
        pipeline_cache,
        runtime_artifact_hash,
        stale_warning,
    };
    serde_json::to_value(report)
        .unwrap_or_else(|_| serde_json::json!({"ok": false, "error": "failed to serialize report"}))
}

fn pipeline_preference_id_str(pipeline: crate::mtsp::engine::PipelineId) -> &'static str {
    use crate::mtsp::engine::PipelineId;
    match pipeline {
        PipelineId::Dxmt => "dxmt",
        PipelineId::Dxmt32 => "dxmt_32",
        PipelineId::Vkd3d => "vkd3d",
        PipelineId::M13 => "m13",
        PipelineId::D3DMetal => "d3dmetal",
        PipelineId::FnaArm64 => "fna_arm64",
        PipelineId::WineBare => "wine_bare",
        _ => "auto",
    }
}

// ----------------------------------------------------------------------------
// PSO diagnostic manifest schema
// ----------------------------------------------------------------------------
//
// Standardizes the trace fields a PSO creation should record so that "Failed
// to create PSO" is never an end-state diagnosis. DXMT emits traces when its
// trace flags are set (DXMT_D3D12_TRACE, etc.); this struct defines the stable
// JSON shape those traces should produce and that the Rust backend parses.

#[derive(Debug, Clone, Serialize, serde::Deserialize)]
pub struct PsoDiagnosticManifest {
    pub schema_version: u32,
    pub kind: PsoKind,
    pub dxil_input_hash: Option<String>,
    pub msl_output_hash: Option<String>,
    pub root_signature_hash: Option<String>,
    #[serde(default)]
    pub vertex_input_layout_hash: Option<String>,
    #[serde(default)]
    pub render_target_formats: Vec<String>,
    #[serde(default)]
    pub depth_stencil_format: Option<String>,
    #[serde(default)]
    pub sample_count: Option<u32>,
    #[serde(default)]
    pub uses_stage_in: Option<bool>,
    #[serde(default)]
    pub async_compile: Option<bool>,
    #[serde(default)]
    pub compile_status: Option<String>,
    #[serde(default)]
    pub metal_error: Option<String>,
    #[serde(default)]
    pub objc_exception: Option<String>,
    #[serde(default)]
    pub recorded_at_unix: Option<u64>,
}

#[derive(Debug, Clone, Copy, Serialize, serde::Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum PsoKind {
    Graphics,
    Compute,
}

impl PsoDiagnosticManifest {
    pub fn failed(&self) -> bool {
        self.compile_status
            .as_deref()
            .map(|s| s.eq_ignore_ascii_case("failed") || s.eq_ignore_ascii_case("error"))
            .unwrap_or(false)
    }
}

/// Parse a DXMT PSO trace JSON document into the standardized manifest.
pub fn parse_pso_manifest(raw: &str) -> Result<PsoDiagnosticManifest, serde_json::Error> {
    serde_json::from_str::<PsoDiagnosticManifest>(raw)
}

/// Collect the most recent PSO manifests recorded under an appid's pipeline
/// cache dir (DXMT_LOG_PATH). Returns up to `limit` newest entries.
pub fn recent_pso_manifests(
    home: &Path,
    pipeline: crate::mtsp::engine::PipelineId,
    appid: u32,
    limit: usize,
) -> Vec<PsoDiagnosticManifest> {
    let Some(primary) = primary_cache_subdir(pipeline) else {
        return Vec::new();
    };
    let dir =
        crate::platform::metalsharp_home_dir_for(home).join("pipeline-cache").join(primary).join(appid.to_string());
    let Ok(entries) = std::fs::read_dir(&dir) else {
        return Vec::new();
    };
    let mut found: Vec<(Option<u64>, PsoDiagnosticManifest)> = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if !name.starts_with("pso-") || path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        if let Ok(raw) = std::fs::read_to_string(&path) {
            if let Ok(manifest) = parse_pso_manifest(&raw) {
                found.push((manifest.recorded_at_unix, manifest));
            }
        }
    }
    found.sort_by_key(|b| std::cmp::Reverse(b.0));
    found.into_iter().take(limit).map(|(_, m)| m).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dxmt_reuses_shared_dxmt_metal_preset_family() {
        assert_eq!(preset_lookup_subdirs("dxmt"), vec!["dxmt", "dxmt-metal"]);
    }

    #[test]
    fn dxmt_reuses_dxmt_preset_family() {
        assert_eq!(preset_lookup_subdirs("dxmt"), vec!["dxmt", "dxmt-metal"]);
    }

    // ---- Phase 4: cache doctor + PSO manifest ----

    #[test]
    fn shader_cache_family_keeps_legacy_and_vkd3d_isolated() {
        use crate::mtsp::engine::PipelineId;
        // DXMT/DXMT(32) share the legacy dxmt-metal family.
        assert_eq!(shader_cache_family(PipelineId::Dxmt), &["dxmt", "dxmt-metal"]);
        // VKD3D/M13 use the isolated dxmt-metal12 family and must not mix.
        assert_eq!(shader_cache_family(PipelineId::Vkd3d), &["vkd3d", "dxmt-metal12"]);
        assert_eq!(shader_cache_family(PipelineId::M13), &["m13", "dxmt-metal12"]);
    }

    #[test]
    fn primary_cache_subdir_maps_each_pipeline_to_its_isolated_lane() {
        use crate::mtsp::engine::PipelineId;
        assert_eq!(primary_cache_subdir(PipelineId::Dxmt), Some("dxmt"));
        assert_eq!(primary_cache_subdir(PipelineId::Dxmt32), Some("dxmt_32"));
        assert_eq!(primary_cache_subdir(PipelineId::Vkd3d), Some("vkd3d"));
        assert_eq!(primary_cache_subdir(PipelineId::M13), Some("m13"));
    }

    fn make_dxmt_cache_db(path: &Path, rows: &[(&str, Vec<&str>)]) {
        let conn = rusqlite::Connection::open(path).unwrap();
        for (table, keys) in rows {
            conn.execute(&format!("CREATE TABLE {} (key BLOB PRIMARY KEY, value BLOB)", table), []).unwrap();
            for k in keys {
                conn.execute(
                    &format!("INSERT INTO {} (key, value) VALUES (?1, ?2)", table),
                    rusqlite::params![k.as_bytes(), b"v"],
                )
                .unwrap();
            }
        }
    }

    #[test]
    fn cache_doctor_counts_entries_and_reports_isolated_vkd3d_lane() {
        use crate::mtsp::engine::PipelineId;
        let home = std::env::temp_dir().join("ms-cache-doctor-vkd3d");
        let _ = std::fs::remove_dir_all(&home);
        let ms_home = crate::platform::metalsharp_home_dir_for(&home);
        let shader_dir = ms_home.join("shader-cache").join("vkd3d").join("42");
        let pipeline_dir = ms_home.join("pipeline-cache").join("vkd3d").join("42");
        std::fs::create_dir_all(&shader_dir).unwrap();
        std::fs::create_dir_all(&pipeline_dir).unwrap();

        make_dxmt_cache_db(&shader_dir.join("shaders_42.db"), &[("cache_1", vec!["a", "b", "c"])]);
        make_dxmt_cache_db(&pipeline_dir.join("pipelines_42.db"), &[("cache_2", vec!["x", "y"])]);

        let report = cache_doctor_for(&home, PipelineId::Vkd3d, 42);
        assert_eq!(report.get("schema_version").and_then(|v| v.as_u64()), Some(1));
        assert_eq!(report.get("pipeline").and_then(|v| v.as_str()), Some("vkd3d"));
        let shader = report.get("shader_cache").unwrap();
        assert_eq!(shader.get("total_entries").and_then(|v| v.as_u64()), Some(3));
        assert_eq!(shader.get("exists").and_then(|v| v.as_bool()), Some(true));
        let pipeline = report.get("pipeline_cache").unwrap();
        assert_eq!(pipeline.get("total_entries").and_then(|v| v.as_u64()), Some(2));

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn cache_doctor_reports_empty_state_without_panicking() {
        use crate::mtsp::engine::PipelineId;
        let home = std::env::temp_dir().join("ms-cache-doctor-empty");
        let _ = std::fs::remove_dir_all(&home);
        std::fs::create_dir_all(&home).unwrap();

        let report = cache_doctor_for(&home, PipelineId::Dxmt, 999);
        assert_eq!(report.get("pipeline").and_then(|v| v.as_str()), Some("dxmt"));
        let shader = report.get("shader_cache").unwrap();
        assert_eq!(shader.get("exists").and_then(|v| v.as_bool()), Some(false));
        assert_eq!(shader.get("total_entries").and_then(|v| v.as_u64()), Some(0));

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn cache_doctor_warns_when_entries_exist_without_runtime_hash() {
        use crate::mtsp::engine::PipelineId;
        let home = std::env::temp_dir().join("ms-cache-doctor-stale");
        let _ = std::fs::remove_dir_all(&home);
        let ms_home = crate::platform::metalsharp_home_dir_for(&home);
        let shader_dir = ms_home.join("shader-cache").join("dxmt").join("7");
        std::fs::create_dir_all(&shader_dir).unwrap();
        make_dxmt_cache_db(&shader_dir.join("shaders_7.db"), &[("cache_1", vec!["a"])]);

        let report = cache_doctor_for(&home, PipelineId::Dxmt, 7);
        // No injections.json staged => runtime_artifact_hash is null and a
        // stale warning must be present.
        assert_eq!(report.get("runtime_artifact_hash").and_then(|v| v.as_str()), None);
        assert!(report.get("stale_warning").and_then(|v| v.as_str()).is_some(), "must warn when entries lack a hash");

        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn parse_pso_manifest_decodes_graphics_pso_failure() {
        let raw = r#"{
            "schema_version": 1,
            "kind": "graphics",
            "dxil_input_hash": "abc123",
            "msl_output_hash": "def456",
            "root_signature_hash": "rs1",
            "vertex_input_layout_hash": "vil1",
            "render_target_formats": ["R8G8B8A8_UNORM"],
            "depth_stencil_format": "D32_FLOAT",
            "sample_count": 1,
            "uses_stage_in": true,
            "async_compile": true,
            "compile_status": "failed",
            "metal_error": "unsupported pixel format",
            "objc_exception": null,
            "recorded_at_unix": 1700000000
        }"#;
        let manifest = parse_pso_manifest(raw).expect("graphics manifest must parse");
        assert_eq!(manifest.kind, PsoKind::Graphics);
        assert_eq!(manifest.dxil_input_hash.as_deref(), Some("abc123"));
        assert_eq!(manifest.root_signature_hash.as_deref(), Some("rs1"));
        assert_eq!(manifest.render_target_formats, vec!["R8G8B8A8_UNORM"]);
        assert_eq!(manifest.depth_stencil_format.as_deref(), Some("D32_FLOAT"));
        assert_eq!(manifest.sample_count, Some(1));
        assert_eq!(manifest.uses_stage_in, Some(true));
        assert_eq!(manifest.async_compile, Some(true));
        assert_eq!(manifest.compile_status.as_deref(), Some("failed"));
        assert_eq!(manifest.metal_error.as_deref(), Some("unsupported pixel format"));
        assert!(manifest.failed(), "failed status must be detected");
    }

    #[test]
    fn parse_pso_manifest_decodes_compute_pso_success() {
        let raw = r#"{
            "schema_version": 1,
            "kind": "compute",
            "dxil_input_hash": "cmp1",
            "msl_output_hash": "cmp2",
            "root_signature_hash": "crs",
            "compile_status": "ok",
            "recorded_at_unix": 1700000001
        }"#;
        let manifest = parse_pso_manifest(raw).expect("compute manifest must parse");
        assert_eq!(manifest.kind, PsoKind::Compute);
        assert_eq!(manifest.vertex_input_layout_hash, None);
        assert_eq!(manifest.render_target_formats, Vec::<String>::new());
        assert!(!manifest.failed(), "ok status must not be reported as failed");
    }

    #[test]
    fn recent_pso_manifests_collects_newest_first() {
        use crate::mtsp::engine::PipelineId;
        let home = std::env::temp_dir().join("ms-pso-recent");
        let _ = std::fs::remove_dir_all(&home);
        let dir = crate::platform::metalsharp_home_dir_for(&home).join("pipeline-cache").join("vkd3d").join("100");
        std::fs::create_dir_all(&dir).unwrap();

        for (idx, ts) in [(3u32, 1700000000u64), (1, 1700000002), (2, 1700000001)] {
            let raw = format!(
                r#"{{"schema_version":1,"kind":"graphics","dxil_input_hash":"h{}","compile_status":"ok","recorded_at_unix":{}}}"#,
                idx, ts
            );
            std::fs::write(dir.join(format!("pso-{}.json", idx)), raw).unwrap();
        }
        // A non-pso json must be ignored.
        std::fs::write(dir.join("other.json"), "{}").unwrap();

        let manifests = recent_pso_manifests(&home, PipelineId::Vkd3d, 100, 2);
        assert_eq!(manifests.len(), 2, "must respect the limit and return newest first");
        assert_eq!(manifests[0].dxil_input_hash.as_deref(), Some("h1"), "newest first");
        assert_eq!(manifests[1].dxil_input_hash.as_deref(), Some("h2"));

        let _ = std::fs::remove_dir_all(&home);
    }
}
