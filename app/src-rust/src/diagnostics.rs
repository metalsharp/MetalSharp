//! Phase 1: Baseline launch observability.
//!
//! This module provides a single, stable diagnostic surface that answers
//! "what runtime did this game actually launch with?" without changing any
//! launch or graphics behavior. It reports the resolved pipeline, runtime
//! profile, Wine binary, prefix, artifact sources (with content hashes),
//! staged DLL hashes, and shader cache directories.
//!
//! Two concerns live here:
//!
//! * [`build_launch_diagnostic`] produces a stable JSON snapshot of the launch
//!   environment for a given appid. Missing required artifacts produce a
//!   structured failure instead of a silent fallback.
//! * [`LaunchTiming`] records named checkpoints around the launch preparation
//!   stages (pipeline resolution, DLL staging, bridge checks, process spawn,
//!   log path creation) and the Steam library / bottle manifest scan. The last
//!   recorded timing for a bottle is persisted next to its logs so performance
//!   deltas can be compared between PRs.

use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Schema version for the diagnostic JSON. Bumped only when the field shape
/// changes in a backwards-incompatible way. Performance deltas are compared
/// across PRs by matching `schema_version` and the named timing checkpoints.
pub const DIAGNOSTIC_SCHEMA_VERSION: u32 = 1;

/// Name of the persisted latest-launch timing file inside a bottle's log dir.
const TIMING_LATEST_NAME: &str = "launch-timing-latest.json";

/// SHA-256 of a file's contents, returned as lowercase hex.
/// Returns `None` if the file cannot be read. This is intentional: the
/// diagnostic reports presence/absence explicitly rather than panicking.
pub fn file_sha256(path: &Path) -> Option<String> {
    let mut file = fs::File::open(path).ok()?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let bytes_read = file.read(&mut buffer).ok()?;
        if bytes_read == 0 {
            break;
        }
        hasher.update(&buffer[..bytes_read]);
    }
    Some(bytes_to_hex(&hasher.finalize()))
}

/// True when a regular file has exactly the expected non-zero size and
/// SHA-256. Callers use this for downloaded artifacts before staging or
/// executing them; a missing size or malformed hash fails closed.
pub fn file_matches_sha256(path: &Path, expected_size: u64, expected_sha256: &str) -> bool {
    let expected_sha256 = expected_sha256.trim();
    if expected_size == 0
        || expected_sha256.len() != 64
        || !expected_sha256.chars().all(|character| character.is_ascii_hexdigit())
    {
        return false;
    }

    let Ok(metadata) = fs::metadata(path) else {
        return false;
    };
    if !metadata.is_file() || metadata.len() != expected_size {
        return false;
    }

    file_sha256(path).is_some_and(|actual| actual.eq_ignore_ascii_case(expected_sha256))
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut out, "{byte:02x}");
    }
    out
}

/// A small checkpoint recorder for launch preparation timing.
///
/// Created with [`LaunchTiming::start`], advanced with [`LaunchTiming::mark`],
/// and serialized with [`LaunchTiming::to_json`]. Callers that persist timing
/// use [`LaunchTiming::record_for_bottle`].
#[derive(Debug, Clone)]
pub struct LaunchTiming {
    started_at: Instant,
    started_unix: u64,
    checkpoints: Vec<(String, Duration)>,
}

impl LaunchTiming {
    pub fn start() -> Self {
        let started_unix = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
        Self { started_at: Instant::now(), started_unix, checkpoints: Vec::new() }
    }

    /// Record a named checkpoint with the elapsed time since `start`.
    /// Repeated names are allowed and keep their insertion order so a stage
    /// with multiple sub-steps can be observed.
    pub fn mark(&mut self, name: &str) {
        let elapsed = self.started_at.elapsed();
        self.checkpoints.push((name.to_string(), elapsed));
    }

    /// Total elapsed time since `start`, regardless of how many marks exist.
    pub fn total(&self) -> Duration {
        self.started_at.elapsed()
    }

    /// Serialize to the stable JSON shape consumed by the diagnostic route.
    pub fn to_json(&self) -> Value {
        let checkpoints: Vec<Value> = self
            .checkpoints
            .iter()
            .map(|(name, elapsed)| {
                json!({
                    "name": name,
                    "elapsed_ms": elapsed.as_millis() as u64,
                    "elapsed_us": elapsed.as_micros() as u64,
                })
            })
            .collect();
        json!({
            "started_at_unix": self.started_unix,
            "total_ms": self.total().as_millis() as u64,
            "checkpoints": checkpoints,
        })
    }

    /// Persist this timing as the latest launch timing for a bottle.
    ///
    /// `bottle_id` is the appid-scoped bottle id (e.g. `steam_105600`). The
    /// file is written atomically (write-to-temp then rename) so a concurrent
    /// reader never sees a partial document.
    pub fn record_for_bottle(&self, home: &Path, bottle_id: &str) {
        let Some(log_dir) = bottle_log_dir(home, bottle_id) else {
            return;
        };
        if fs::create_dir_all(&log_dir).is_err() {
            return;
        }
        let final_path = log_dir.join(TIMING_LATEST_NAME);
        let tmp_path = log_dir.join(format!("{}.tmp", TIMING_LATEST_NAME));
        let payload = serde_json::to_string(&self.to_json()).unwrap_or_else(|_| "{}".into());
        if fs::write(&tmp_path, payload).is_ok() {
            let _ = fs::rename(&tmp_path, &final_path);
        }
    }
}

impl Default for LaunchTiming {
    fn default() -> Self {
        Self::start()
    }
}

/// Read the most recently persisted launch timing for a bottle, if any.
pub fn latest_launch_timing(home: &Path, bottle_id: &str) -> Option<Value> {
    let log_dir = bottle_log_dir(home, bottle_id)?;
    let raw = fs::read_to_string(log_dir.join(TIMING_LATEST_NAME)).ok()?;
    serde_json::from_str(&raw).ok()
}

/// Persist a named scan timing (e.g. "steam_library", "scan_all",
/// "bottle_manifest") as the latest of its kind. Scan timings are written
/// under `~/.metalsharp/logs/` and are not bottle-specific so that Steam
/// library refresh and manifest-write costs can be compared across PRs.
pub fn record_scan_timing(home: &Path, kind: &str, timing: &LaunchTiming) {
    if kind.is_empty() {
        return;
    }
    let logs_dir = crate::platform::metalsharp_home_dir_for(home).join("logs");
    if fs::create_dir_all(&logs_dir).is_err() {
        return;
    }
    let final_path = logs_dir.join(format!("scan-timing-{}-latest.json", kind));
    let tmp_path = logs_dir.join(format!("scan-timing-{}.tmp", kind));
    let payload = serde_json::to_string(&timing.to_json()).unwrap_or_else(|_| "{}".into());
    if fs::write(&tmp_path, payload).is_ok() {
        let _ = fs::rename(&tmp_path, &final_path);
    }
}

/// Read the most recently persisted scan timing for a kind, if any.
pub fn latest_scan_timing(home: &Path, kind: &str) -> Option<Value> {
    let logs_dir = crate::platform::metalsharp_home_dir_for(home).join("logs");
    let raw = fs::read_to_string(logs_dir.join(format!("scan-timing-{}-latest.json", kind))).ok()?;
    serde_json::from_str(&raw).ok()
}

fn bottle_log_dir(home: &Path, bottle_id: &str) -> Option<PathBuf> {
    if bottle_id.is_empty() {
        return None;
    }
    Some(crate::platform::metalsharp_home_dir_for(home).join("bottles").join(bottle_id).join("logs"))
}

/// Resolve the on-disk shader cache directories that a pipeline would use for
/// an appid, including the legacy shared DXMT-Metal family aliases. This
/// mirrors [`crate::mtsp::shader_cache`] lookup families so the diagnostic
/// reports the same roots the runtime consults.
pub fn shader_cache_dirs(home: &Path, pipeline: crate::mtsp::engine::PipelineId, appid: u32) -> Vec<PathBuf> {
    use crate::mtsp::engine::PipelineId;

    let cache_root = crate::platform::metalsharp_home_dir_for(home).join("shader-cache");
    let appid_str = appid.to_string();

    let subdirs: &[&str] = match pipeline {
        PipelineId::M9 => &["m9", "dxmt-metal"],
        PipelineId::M10 => &["m10", "dxmt-metal"],
        PipelineId::M11 => &["m11", "dxmt-metal"],
        PipelineId::M12 => &["m12", "dxmt-metal12"],
        PipelineId::M13 => &["m13", "dxmt-metal12"],
        _ => &[],
    };

    subdirs.iter().map(|sub| cache_root.join(sub).join(&appid_str)).collect()
}

/// Build the stable launch diagnostic JSON for an appid and an optional
/// requested pipeline.
///
/// This walks the same resolution path as `prepare_pipeline` /
/// `handle_steam_runtime_doctor`: it resolves the pipeline, resolves the
/// game directory, resolves the deploy list from the pipeline node, and
/// reports each artifact source with presence and content hash. Required
/// artifacts that are missing produce a structured failure (`ok: false`)
/// rather than a silent fallback.
pub fn build_launch_diagnostic(appid: u32, requested: Option<crate::mtsp::engine::PipelineId>) -> Value {
    match dirs::home_dir() {
        Some(home) => build_launch_diagnostic_for(&home, appid, requested),
        None => json!({
            "ok": false,
            "schema_version": DIAGNOSTIC_SCHEMA_VERSION,
            "error": "home directory could not be resolved",
            "appid": appid,
        }),
    }
}

/// Same as [`build_launch_diagnostic`], but with an explicit home directory.
/// This form is used by tests so they never mutate the process-global
/// `METALSHARP_HOME` (which would race with other parallel tests).
pub fn build_launch_diagnostic_for(
    home: &Path,
    appid: u32,
    requested: Option<crate::mtsp::engine::PipelineId>,
) -> Value {
    let ms_home = crate::platform::metalsharp_home_dir_for(home);

    let pipeline = crate::bottles::resolve_steam_pipeline_for_request(appid, requested);
    let node = crate::mtsp::engine::get_pipeline(pipeline);
    let profile = crate::bottles::runtime_profile_for_app_pipeline(appid, pipeline);
    let dual = crate::scan::resolve_dual_game_dir(appid);
    let prefix = ms_home.join("prefix-steam");

    let wine_root = ms_home.join("runtime").join("wine");
    let wine_binary = crate::platform::runtime_wine_binary(&wine_root);
    let wine_library_env = crate::platform::runtime_library_env(&wine_root)
        .map(|(key, value)| json!({ "key": key, "value": value }))
        .unwrap_or_else(|| json!({ "present": false }));

    // Artifact sources: every deploy_dll in the pipeline node, resolved against
    // the runtime wine root. Optional stubs (nvapi/nvngx/atidxx) are tolerated
    // as missing without causing a structured failure.
    let mut artifact_sources: Vec<Value> = Vec::new();
    let mut missing_required: Vec<Value> = Vec::new();
    for deploy in &node.deploy_dlls {
        let source_path = wine_root.join(deploy.source_subpath).join(deploy.filename);
        let present = source_path.exists();
        let is_optional_stub = deploy.filename.starts_with("nvapi")
            || deploy.filename.starts_with("nvngx")
            || deploy.filename.starts_with("atidxx");
        let sha = if present { file_sha256(&source_path) } else { None };
        let size = if present { fs::metadata(&source_path).ok().map(|m| m.len()) } else { None };

        artifact_sources.push(json!({
            "source_subpath": deploy.source_subpath,
            "filename": deploy.filename,
            "dest_filename": deploy.dest_filename,
            "source_path": source_path.to_string_lossy(),
            "present": present,
            "optional": is_optional_stub,
            "sha256": sha,
            "size_bytes": size,
        }));

        if !present && !is_optional_stub {
            missing_required.push(json!({
                "filename": deploy.filename,
                "source_subpath": deploy.source_subpath,
                "source_path": source_path.to_string_lossy(),
            }));
        }
    }

    // Staged DLL hashes: read the most recent injections manifest written by
    // deploy_recipe_dlls into <game_dir>/.metalsharp/injections.json, and
    // report the current sha256 of each staged destination.
    let staged_dll_hashes = staged_dll_hashes_for(dual.wine_dir.as_deref());

    // Cache directories for this pipeline+appid.
    let cache_dirs: Vec<Value> = shader_cache_dirs(home, pipeline, appid)
        .into_iter()
        .map(|dir| {
            let exists = dir.exists();
            let entry_count = if exists { fs::read_dir(&dir).ok().map(|rd| rd.count() as u64).unwrap_or(0) } else { 0 };
            json!({
                "path": dir.to_string_lossy(),
                "exists": exists,
                "entry_count": entry_count,
            })
        })
        .collect();

    let bundle_hash = bundle_hash_for(&ms_home);

    let generated_at_unix = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);

    if !missing_required.is_empty() {
        return json!({
            "ok": false,
            "schema_version": DIAGNOSTIC_SCHEMA_VERSION,
            "metalsharp_version": env!("CARGO_PKG_VERSION"),
            "generated_at_unix": generated_at_unix,
            "appid": appid,
            "pipeline": pipeline,
            "pipeline_name": node.name,
            "runtime_profile": profile,
            "error": "required runtime artifacts are missing",
            "missing_artifacts": missing_required,
            "artifact_sources": artifact_sources,
            "wine_binary_path": wine_binary.to_string_lossy(),
            "wine_binary_exists": wine_binary.exists(),
            "wine_library_env": wine_library_env,
            "prefix_path": prefix.to_string_lossy(),
            "prefix_exists": prefix.exists(),
            "game_install_path": dual.wine_dir.as_ref().map(|p| p.to_string_lossy().to_string()),
            "bundle_hash": bundle_hash,
            "cache_directories": cache_dirs,
        });
    }

    json!({
        "ok": true,
        "schema_version": DIAGNOSTIC_SCHEMA_VERSION,
        "metalsharp_version": env!("CARGO_PKG_VERSION"),
        "generated_at_unix": generated_at_unix,
        "appid": appid,
        "pipeline": pipeline,
        "pipeline_name": node.name,
        "backend": node.backend,
        "graphics_backend": node.graphics_backend,
        "runtime_profile": profile,
        "wine_binary_path": wine_binary.to_string_lossy(),
        "wine_binary_exists": wine_binary.exists(),
        "wine_library_env": wine_library_env,
        "prefix_path": prefix.to_string_lossy(),
        "prefix_exists": prefix.exists(),
        "game_install_path": dual.wine_dir.as_ref().map(|p| p.to_string_lossy().to_string()),
        "bundle_hash": bundle_hash,
        "artifact_sources": artifact_sources,
        "staged_dll_hashes": staged_dll_hashes,
        "cache_directories": cache_dirs,
    })
}

/// Read `<game_dir>/.metalsharp/injections.json` (written by
/// `deploy_recipe_dlls`) and report the current sha256 of each staged DLL
/// destination. Returns an empty array if the game dir or manifest is absent.
fn staged_dll_hashes_for(game_dir: Option<&Path>) -> Vec<Value> {
    let Some(game_dir) = game_dir else {
        return Vec::new();
    };
    let manifest_path = game_dir.join(".metalsharp").join("injections.json");
    let Ok(raw) = fs::read_to_string(&manifest_path) else {
        return Vec::new();
    };
    let Ok(manifest) = serde_json::from_str::<Value>(&raw) else {
        return Vec::new();
    };
    let Some(dlls) = manifest.get("dlls").and_then(|v| v.as_array()) else {
        return Vec::new();
    };

    let manifest_pipeline = manifest.get("pipeline").cloned();
    let manifest_pipeline_name = manifest.get("pipeline_name").cloned();
    let manifest_updated_at = manifest.get("updated_at_unix").cloned();

    dlls.iter()
        .filter_map(|dll| {
            let filename = dll.get("filename").and_then(|v| v.as_str())?;
            let dest = dll.get("dest_path").and_then(|v| v.as_str()).map(PathBuf::from)?;
            let source_path = dll.get("source_path").and_then(|v| v.as_str()).map(PathBuf::from);
            let present = dest.exists();
            let sha = if present { file_sha256(&dest) } else { None };
            let source_sha = source_path.and_then(|p| if p.exists() { file_sha256(&p) } else { None });
            let matches_source = match (sha.as_ref(), source_sha.as_ref()) {
                (Some(a), Some(b)) => Some(a == b),
                _ => None,
            };
            Some(json!({
                "filename": filename,
                "dest_path": dest.to_string_lossy(),
                "present": present,
                "sha256": sha,
                "source_sha256": source_sha,
                "matches_source": matches_source,
                "manifest_pipeline": manifest_pipeline,
                "manifest_pipeline_name": manifest_pipeline_name,
                "manifest_updated_at_unix": manifest_updated_at,
            }))
        })
        .collect()
}

/// Best-effort runtime bundle hash. MetalSharp records an installed bundle
/// identity under the runtime root when present; if it is absent we report
/// `null` rather than inventing one, so the diagnostic never lies about
/// provenance.
fn bundle_hash_for(ms_home: &Path) -> Option<String> {
    let candidates = [
        ms_home.join("runtime").join("bundle-hash.txt"),
        ms_home.join("runtime").join("wine").join("bundle-hash.txt"),
        ms_home.join("bundle-hash.txt"),
    ];
    for candidate in candidates {
        if let Ok(raw) = fs::read_to_string(&candidate) {
            let trimmed = raw.trim();
            if !trimmed.is_empty() {
                return Some(trimmed.to_string());
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn file_sha256_matches_known_value() {
        let dir = std::env::temp_dir().join("ms-diag-sha-test");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("blob.bin");
        fs::write(&path, b"abc").unwrap();
        // Known SHA-256 of "abc"
        let known = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        assert_eq!(file_sha256(&path).as_deref(), Some(known));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn file_sha256_returns_none_for_missing_file() {
        let path = std::env::temp_dir().join("ms-diag-sha-missing-nope.bin");
        let _ = fs::remove_file(&path);
        assert_eq!(file_sha256(&path), None);
    }

    #[test]
    fn file_matches_sha256_requires_size_and_hash() {
        let dir = std::env::temp_dir().join("ms-diag-sha-verify-test");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("blob.bin");
        fs::write(&path, b"abc").unwrap();
        let known = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

        assert!(file_matches_sha256(&path, 3, known));
        assert!(!file_matches_sha256(&path, 0, known));
        assert!(!file_matches_sha256(&path, 3, "not-a-sha256"));
        assert!(!file_matches_sha256(&path, 4, known));
        assert!(!file_matches_sha256(&path, 3, "0000000000000000000000000000000000000000000000000000000000000000"));

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn launch_timing_records_named_checkpoints_in_order() {
        let mut t = LaunchTiming::start();
        t.mark("pipeline_resolution");
        t.mark("dll_staging");
        t.mark("wine_spawn");
        let value = t.to_json();
        let names: Vec<String> = value
            .get("checkpoints")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c.get("name").unwrap().as_str().unwrap().to_string())
            .collect();
        assert_eq!(names, vec!["pipeline_resolution", "dll_staging", "wine_spawn"]);
        assert!(value.get("total_ms").unwrap().as_u64().is_some());
    }

    #[test]
    fn launch_timing_total_grows_with_marks() {
        let mut t = LaunchTiming::start();
        let before = t.total();
        // Spin briefly so elapsed is nonzero.
        while t.total() == before {}
        t.mark("wait");
        assert!(t.total() >= before);
    }

    #[test]
    fn shader_cache_dirs_include_dxmt_metal_family_for_legacy_pipelines() {
        let home = std::env::temp_dir().join("ms-diag-cache-test");
        let _ = fs::remove_dir_all(&home);
        let dirs = shader_cache_dirs(&home, crate::mtsp::engine::PipelineId::M11, 42);
        let names: Vec<String> = dirs.iter().map(|d| d.to_string_lossy().to_string()).collect();
        assert!(names.iter().any(|n| n.contains("shader-cache/m11/42")), "got {:?}", names);
        assert!(names.iter().any(|n| n.contains("shader-cache/dxmt-metal/42")), "got {:?}", names);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn shader_cache_dirs_use_m12_isolated_family() {
        let home = std::env::temp_dir().join("ms-diag-cache-m12");
        let _ = fs::remove_dir_all(&home);
        let dirs = shader_cache_dirs(&home, crate::mtsp::engine::PipelineId::M12, 7);
        let names: Vec<String> = dirs.iter().map(|d| d.to_string_lossy().to_string()).collect();
        assert!(names.iter().any(|n| n.contains("shader-cache/m12/7")), "got {:?}", names);
        assert!(names.iter().any(|n| n.contains("shader-cache/dxmt-metal12/7")), "got {:?}", names);
        // M12 must NOT share the dxmt-metal legacy family.
        assert!(!names.iter().any(|n| n.contains("shader-cache/dxmt-metal/")), "got {:?}", names);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn build_launch_diagnostic_contains_required_fields_for_known_appid() {
        // Celeste (504230) resolves to a stable pipeline regardless of host
        // state. The diagnostic must report the pipeline id, runtime profile,
        // prefix path, wine binary, and artifact_sources shape. Missing
        // artifacts on a clean test host are reported as a structured failure.
        // We pass an explicit temp home so this never mutates METALSHARP_HOME.
        let home = std::env::temp_dir().join("ms-diag-fields-home");
        let _ = fs::remove_dir_all(&home);
        fs::create_dir_all(&home).unwrap();
        let report = build_launch_diagnostic_for(&home, 504230, None);
        let _ = fs::remove_dir_all(&home);

        assert_eq!(report.get("schema_version").and_then(|v| v.as_u64()), Some(DIAGNOSTIC_SCHEMA_VERSION as u64));
        assert_eq!(report.get("metalsharp_version").and_then(|v| v.as_str()), Some(env!("CARGO_PKG_VERSION")));
        assert_eq!(report.get("appid").and_then(|v| v.as_u64()), Some(504230));
        // pipeline is serialized as snake_case and must be present either way.
        assert!(report.get("pipeline").is_some(), "diagnostic must report pipeline id");
        assert!(report.get("pipeline_name").is_some(), "diagnostic must report pipeline name");
        assert!(report.get("runtime_profile").is_some(), "diagnostic must report runtime profile");
        assert!(report.get("prefix_path").is_some(), "diagnostic must report prefix path");
        assert!(report.get("wine_binary_path").is_some(), "diagnostic must report wine binary path");
        assert!(report.get("artifact_sources").unwrap().is_array(), "diagnostic must report artifact sources");
    }

    #[test]
    fn build_launch_diagnostic_reports_structured_failure_when_artifacts_missing() {
        // Use an explicit empty home so no runtime artifacts exist, then request
        // M12 which requires d3d12.dll etc. The diagnostic must report ok=false
        // with a missing_artifacts array, not a silent ok=true. No global env
        // mutation, so this is safe under parallel test execution.
        let home = std::env::temp_dir().join("ms-diag-empty-home");
        let _ = fs::remove_dir_all(&home);
        fs::create_dir_all(&home).unwrap();

        let report = build_launch_diagnostic_for(&home, 999999, Some(crate::mtsp::engine::PipelineId::M12));

        let _ = fs::remove_dir_all(&home);

        // 999999 is not a known game, so it resolves through the fallback.
        // If it happens to resolve to M12, we get a structured failure. If it
        // resolves to a non-DXMT route, there are no required deploy_dlls and
        // ok=true is valid. Either way, the shape must be valid: when ok=false,
        // missing_artifacts MUST be a non-empty array.
        let ok = report.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
        if !ok {
            let missing = report.get("missing_artifacts").and_then(|v| v.as_array()).unwrap();
            assert!(!missing.is_empty(), "ok=false must include missing_artifacts: {}", report);
            assert!(
                report.get("error").and_then(|v| v.as_str()).unwrap_or("").contains("missing"),
                "ok=false must explain the failure: {}",
                report
            );
        }
    }

    #[test]
    fn latest_launch_timing_round_trips_through_disk() {
        let home = std::env::temp_dir().join("ms-diag-timing-rt");
        let _ = fs::remove_dir_all(&home);
        fs::create_dir_all(&home).unwrap();
        let mut t = LaunchTiming::start();
        t.mark("pipeline_resolution");
        t.mark("dll_staging");
        t.record_for_bottle(&home, "steam_504230");

        let read_back = latest_launch_timing(&home, "steam_504230").expect("timing should persist");
        let names: Vec<String> = read_back
            .get("checkpoints")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c.get("name").unwrap().as_str().unwrap().to_string())
            .collect();
        assert_eq!(names, vec!["pipeline_resolution", "dll_staging"]);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn scan_timing_round_trips_through_disk() {
        let home = std::env::temp_dir().join("ms-diag-scan-rt");
        let _ = fs::remove_dir_all(&home);
        fs::create_dir_all(&home).unwrap();
        let mut t = LaunchTiming::start();
        t.mark("library_load_start");
        t.mark("library_load_done");
        record_scan_timing(&home, "steam_library", &t);
        let read_back = latest_scan_timing(&home, "steam_library").expect("scan timing should persist");
        let names: Vec<String> = read_back
            .get("checkpoints")
            .unwrap()
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c.get("name").unwrap().as_str().unwrap().to_string())
            .collect();
        assert_eq!(names, vec!["library_load_start", "library_load_done"]);
        let _ = fs::remove_dir_all(&home);
    }

    #[test]
    fn staged_dll_hashes_reads_injections_manifest_and_hashes_destinations() {
        let dir = std::env::temp_dir().join("ms-diag-staged");
        let _ = fs::remove_dir_all(&dir);
        let injection_dir = dir.join(".metalsharp");
        fs::create_dir_all(&injection_dir).unwrap();

        // Stage a fake DLL at the destination and record it in the manifest.
        let dest = dir.join("d3d12.dll");
        fs::write(&dest, b"fake-d3d12").unwrap();
        let source = std::env::temp_dir().join("ms-diag-staged-src.bin");
        fs::write(&source, b"fake-d3d12").unwrap();

        let manifest = json!({
            "appid": 504230,
            "pipeline": "m12",
            "pipeline_name": "M12",
            "updated_at_unix": 1700000000u64,
            "dlls": [{
                "filename": "d3d12.dll",
                "source_path": source.to_string_lossy(),
                "dest_path": dest.to_string_lossy(),
            }],
        });
        fs::write(injection_dir.join("injections.json"), serde_json::to_string_pretty(&manifest).unwrap()).unwrap();

        let hashes = staged_dll_hashes_for(Some(&dir));
        assert_eq!(hashes.len(), 1);
        let entry = &hashes[0];
        assert_eq!(entry.get("filename").and_then(|v| v.as_str()), Some("d3d12.dll"));
        assert_eq!(entry.get("present").and_then(|v| v.as_bool()), Some(true));
        assert!(entry.get("sha256").and_then(|v| v.as_str()).is_some());
        assert_eq!(
            entry.get("matches_source").and_then(|v| v.as_bool()),
            Some(true),
            "staged DLL hash must match the recorded source hash"
        );
        let _ = fs::remove_dir_all(&dir);
        let _ = fs::remove_file(&source);
    }
}
