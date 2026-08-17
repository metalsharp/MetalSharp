use serde_json::json;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

const CURRENT_VERSION: &str = env!("CARGO_PKG_VERSION");
const REPO_API: &str = "https://api.github.com/repos/metalsharp/MetalSharp/releases/latest";

static UPDATING: AtomicBool = AtomicBool::new(false);
static DOWNLOAD_PERCENT: AtomicU32 = AtomicU32::new(0);

fn progress_path() -> PathBuf {
    crate::platform::metalsharp_home_dir().join("update_progress.json")
}

fn write_update_progress(status: &str, percent: u32, message: &str, error: Option<&str>) {
    let data = json!({
        "status": status,
        "percent": percent,
        "message": message,
        "error": error,
    });
    let _ = fs::write(progress_path(), serde_json::to_string(&data).unwrap_or_default());
}

pub fn is_updating() -> bool {
    UPDATING.load(Ordering::SeqCst)
}

pub fn read_update_progress() -> serde_json::Value {
    let path = progress_path();
    if path.exists() {
        if let Ok(contents) = fs::read_to_string(&path) {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(&contents) {
                return v;
            }
        }
    }
    json!({
        "status": "idle",
        "percent": 0,
        "message": "",
        "error": null,
    })
}

#[derive(serde::Deserialize)]
struct GithubRelease {
    tag_name: String,
    name: Option<String>,
    body: Option<String>,
    assets: Vec<GithubAsset>,
}

#[derive(serde::Deserialize)]
struct GithubAsset {
    name: String,
    browser_download_url: String,
    size: u64,
    digest: Option<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct DmgMetadata {
    version: String,
    source_url: String,
    size: u64,
    sha256: String,
}

#[derive(serde::Deserialize, serde::Serialize)]
struct DmgCacheManifest {
    version: String,
    source_url: String,
    expected_size: u64,
    sha256: String,
}

pub struct DownloadedDmg {
    pub path: String,
    pub version: String,
    pub size: u64,
    pub sha256: String,
}

fn find_dmg_asset(assets: &[GithubAsset]) -> Option<&GithubAsset> {
    assets.iter().find(|a| a.name.ends_with("-arm64.dmg") || a.name.ends_with(".dmg"))
}

fn normalize_sha256(value: &str) -> Option<String> {
    let value = value.trim().strip_prefix("sha256:").unwrap_or(value.trim());
    (value.len() == 64 && value.chars().all(|character| character.is_ascii_hexdigit()))
        .then(|| value.to_ascii_lowercase())
}

fn dmg_metadata(asset: &GithubAsset, version: &str) -> Result<DmgMetadata, String> {
    if asset.size == 0 {
        return Err("release DMG has no non-zero size".to_string());
    }
    let sha256 = asset
        .digest
        .as_deref()
        .and_then(normalize_sha256)
        .ok_or_else(|| "release DMG has no valid SHA-256 digest".to_string())?;
    if asset.browser_download_url.is_empty() {
        return Err("release DMG has no download URL".to_string());
    }
    Ok(DmgMetadata {
        version: version.to_string(),
        source_url: asset.browser_download_url.clone(),
        size: asset.size,
        sha256,
    })
}

pub fn check_for_update() -> serde_json::Value {
    let config = ureq::config::Config::builder().user_agent(format!("MetalSharp/{}", CURRENT_VERSION)).build();
    let agent = ureq::Agent::new_with_config(config);

    let mut resp = match agent.get(REPO_API).call() {
        Ok(r) => r,
        Err(e) => {
            return json!({
                "ok": false,
                "error": format!("failed to fetch release: {}", e),
                "current_version": CURRENT_VERSION,
            })
        },
    };

    let release: GithubRelease = match resp.body_mut().read_json() {
        Ok(r) => r,
        Err(e) => {
            return json!({
                "ok": false,
                "error": format!("failed to parse release: {}", e),
                "current_version": CURRENT_VERSION,
            })
        },
    };

    let latest_raw = release.tag_name.trim();
    let latest = clean_version(latest_raw);
    let current = CURRENT_VERSION.to_string();
    let available = semver_gt(&latest, &current);

    let dmg_asset = match find_dmg_asset(&release.assets) {
        Some(asset) => asset,
        None => {
            return json!({
                "ok": false,
                "error": "latest release has no DMG asset",
                "current_version": CURRENT_VERSION,
            })
        },
    };
    let metadata = match dmg_metadata(dmg_asset, &latest) {
        Ok(metadata) => metadata,
        Err(error) => {
            return json!({
                "ok": false,
                "error": format!("latest release DMG failed integrity metadata validation: {}", error),
                "current_version": CURRENT_VERSION,
            })
        },
    };

    let release_notes = release.body.unwrap_or_default();
    let release_name = release.name.unwrap_or_else(|| release.tag_name.clone());

    app_log(&format!(
        "Update check: current={} latest_raw='{}' latest_clean='{}' available={}",
        current, latest_raw, latest, available
    ));

    json!({
        "ok": true,
        "current_version": current,
        "latest_version": latest,
        "available": available,
        "download_url": metadata.source_url,
        "download_size": metadata.size,
        "download_sha256": metadata.sha256,
        "release_notes": release_notes,
        "release_name": release_name,
    })
}

fn clean_version(tag: &str) -> String {
    let v = tag.trim().trim_start_matches('v');
    let parts: Vec<&str> = v.split('.').collect();
    parts
        .iter()
        .map(|p| p.chars().take_while(|c| c.is_ascii_digit()).collect::<String>())
        .filter(|p| !p.is_empty())
        .collect::<Vec<_>>()
        .join(".")
}

fn semver_gt(a: &str, b: &str) -> bool {
    compare_versions(a, b).is_gt()
}

pub fn start_update() -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    if UPDATING.load(Ordering::SeqCst) {
        return Ok(json!({"ok": false, "error": "update already in progress"}));
    }

    if UPDATING.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
        return Ok(json!({"ok": false, "error": "update already in progress"}));
    }

    DOWNLOAD_PERCENT.store(0, Ordering::SeqCst);
    write_update_progress("starting", 0, "Checking for updates...", None);

    std::thread::spawn(|| {
        run_download();
        UPDATING.store(false, Ordering::SeqCst);
    });

    Ok(json!({"ok": true}))
}

fn run_download() {
    write_update_progress("checking", 5, "Fetching latest release info...", None);

    let update_info = check_for_update();
    let download_url = match update_info.get("download_url").and_then(|u| u.as_str()) {
        Some(url) if !url.is_empty() => url.to_string(),
        _ => {
            write_update_progress("error", 0, "No DMG download URL found", Some("no_download_url"));
            return;
        },
    };

    let latest_version = update_info.get("latest_version").and_then(|v| v.as_str()).unwrap_or("unknown").to_string();
    let expected_size = match update_info.get("download_size").and_then(|v| v.as_u64()) {
        Some(size) if size > 0 => size,
        _ => {
            write_update_progress("error", 0, "Release DMG has no expected size", Some("dmg_size_missing"));
            return;
        },
    };
    let expected_sha256 = match update_info.get("download_sha256").and_then(|v| v.as_str()).and_then(normalize_sha256) {
        Some(sha256) => sha256,
        None => {
            write_update_progress("error", 0, "Release DMG has no valid SHA-256", Some("dmg_hash_missing"));
            return;
        },
    };
    let metadata = DmgMetadata {
        version: latest_version.clone(),
        source_url: download_url.clone(),
        size: expected_size,
        sha256: expected_sha256,
    };

    let home = match dirs::home_dir() {
        Some(h) => h,
        None => {
            write_update_progress("error", 0, "no home directory", Some("no_home"));
            return;
        },
    };

    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("updates");
    let _ = fs::create_dir_all(&cache_dir);
    let dmg_path = cache_dir.join(format!("MetalSharp-{}.dmg", latest_version));

    if cached_dmg_ready(&dmg_path, &metadata) {
        app_log(&format!("Using cached DMG: {}", dmg_path.display()));
    } else {
        remove_cached_dmg(&dmg_path);
        write_update_progress("downloading", 10, &format!("Downloading v{}...", latest_version), None);
        if let Err(e) = download_with_progress(&download_url, &dmg_path) {
            write_update_progress("error", 0, &format!("Download failed: {}", e), Some(&e.to_string()));
            remove_cached_dmg(&dmg_path);
            return;
        }
        if !crate::diagnostics::file_matches_sha256(&dmg_path, metadata.size, &metadata.sha256) {
            write_update_progress(
                "error",
                0,
                "Downloaded DMG did not match the latest release size and SHA-256",
                Some("dmg_integrity_mismatch"),
            );
            remove_cached_dmg(&dmg_path);
            return;
        }
        if let Err(error) = write_dmg_cache_manifest(&dmg_path, &metadata) {
            write_update_progress("error", 0, "Could not record DMG provenance", Some(&error));
            remove_cached_dmg(&dmg_path);
            return;
        }
    }

    write_update_progress("downloaded", 80, &format!("Download complete — ready to install v{}", latest_version), None);
}

fn dmg_cache_manifest_path(path: &Path) -> PathBuf {
    PathBuf::from(format!("{}.json", path.display()))
}

fn remove_cached_dmg(path: &Path) {
    let _ = fs::remove_file(path);
    let _ = fs::remove_file(dmg_cache_manifest_path(path));
    let _ = fs::remove_file(path.with_extension("dmg.tmp"));
    let _ = fs::remove_file(path.with_extension("dmg.json.tmp"));
}

fn cached_dmg_ready(path: &Path, expected: &DmgMetadata) -> bool {
    let Ok(manifest_contents) = fs::read_to_string(dmg_cache_manifest_path(path)) else {
        return false;
    };
    let Ok(manifest) = serde_json::from_str::<DmgCacheManifest>(&manifest_contents) else {
        return false;
    };
    if manifest.version != expected.version
        || manifest.source_url != expected.source_url
        || manifest.expected_size != expected.size
        || manifest.sha256 != expected.sha256
    {
        return false;
    }
    crate::diagnostics::file_matches_sha256(path, expected.size, &expected.sha256)
}

fn write_dmg_cache_manifest(dmg_path: &Path, metadata: &DmgMetadata) -> Result<(), String> {
    let manifest = DmgCacheManifest {
        version: metadata.version.clone(),
        source_url: metadata.source_url.clone(),
        expected_size: metadata.size,
        sha256: metadata.sha256.clone(),
    };
    let manifest_path = dmg_cache_manifest_path(dmg_path);
    let tmp = dmg_path.with_extension("dmg.json.tmp");
    let contents =
        serde_json::to_vec_pretty(&manifest).map_err(|error| format!("serialize DMG manifest: {}", error))?;
    fs::write(&tmp, contents).map_err(|error| format!("write DMG manifest: {}", error))?;
    fs::rename(&tmp, manifest_path).map_err(|error| {
        let _ = fs::remove_file(&tmp);
        format!("finalize DMG manifest: {}", error)
    })?;
    Ok(())
}

fn download_with_progress(url: &str, dest: &PathBuf) -> Result<(), String> {
    let config = ureq::config::Config::builder().user_agent(format!("MetalSharp/{}", CURRENT_VERSION)).build();
    let agent = ureq::Agent::new_with_config(config);

    let resp = agent.get(url).call().map_err(|e| format!("HTTP request failed: {}", e))?;

    let total_size: u64 = resp
        .headers()
        .get("content-length")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(0);

    let tmp_path = dest.with_extension("dmg.tmp");
    let result = (|| {
        let mut file = fs::File::create(&tmp_path).map_err(|e| format!("create file: {}", e))?;

        let mut reader = resp.into_body().into_reader();
        let mut buf = [0u8; 65536];
        let mut downloaded: u64 = 0;
        let mut last_percent: u32 = 10;

        loop {
            let n = reader.read(&mut buf).map_err(|e| format!("read error: {}", e))?;
            if n == 0 {
                break;
            }
            use std::io::Write;
            file.write_all(&buf[..n]).map_err(|e| format!("write error: {}", e))?;
            downloaded += n as u64;

            if total_size > 0 {
                let pct = 10 + ((downloaded as f64 / total_size as f64) * 70.0) as u32;
                if pct > last_percent {
                    last_percent = pct;
                    DOWNLOAD_PERCENT.store(pct, Ordering::SeqCst);
                    write_update_progress("downloading", pct, &format!("Downloading... {}%", pct), None);
                }
            }
        }

        drop(file);
        fs::rename(&tmp_path, dest).map_err(|e| format!("rename: {}", e))?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp_path);
    }
    result
}

pub fn cleanup_downloaded_dmgs() -> serde_json::Value {
    let home = match dirs::home_dir() {
        Some(h) => h,
        None => return json!({"ok": false, "error": "no home dir"}),
    };

    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("updates");
    if !cache_dir.exists() {
        return json!({"ok": true, "removed": 0, "bytes_freed": 0});
    }

    let mut removed = 0u32;
    let mut bytes_freed: u64 = 0;

    if let Ok(entries) = fs::read_dir(&cache_dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            let is_dmg = path.extension().map(|e| e == "dmg").unwrap_or(false);
            let is_manifest = path
                .file_name()
                .and_then(|name| name.to_str())
                .map(|name| name.ends_with(".dmg.json"))
                .unwrap_or(false);
            let is_partial = path
                .file_name()
                .and_then(|name| name.to_str())
                .map(|name| name.ends_with(".dmg.tmp") || name.ends_with(".dmg.json.tmp"))
                .unwrap_or(false);
            if is_dmg || is_manifest || is_partial {
                let size = fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
                if fs::remove_file(&path).is_ok() {
                    bytes_freed += size;
                    if is_dmg {
                        removed += 1;
                    }
                }
            }
        }
    }

    if removed > 0 {
        app_log(&format!("Cleaned up {} downloaded DMG(s), freed {} bytes", removed, bytes_freed));
    }

    json!({"ok": true, "removed": removed, "bytes_freed": bytes_freed})
}

pub fn get_downloaded_dmg() -> Option<DownloadedDmg> {
    let update_info = check_for_update();
    let latest_version = update_info.get("latest_version").and_then(|v| v.as_str())?;
    let source_url = update_info.get("download_url").and_then(|v| v.as_str())?;
    let size = update_info.get("download_size").and_then(|v| v.as_u64()).filter(|size| *size > 0)?;
    let sha256 = update_info.get("download_sha256").and_then(|v| v.as_str()).and_then(normalize_sha256)?;
    let metadata = DmgMetadata {
        version: latest_version.to_string(),
        source_url: source_url.to_string(),
        size,
        sha256: sha256.clone(),
    };
    let home = dirs::home_dir()?;
    let cache_dir = crate::platform::metalsharp_home_dir_for(&home).join("cache").join("updates");
    let dmg_path = cache_dir.join(format!("MetalSharp-{}.dmg", latest_version));

    if cached_dmg_ready(&dmg_path, &metadata) {
        Some(DownloadedDmg {
            path: dmg_path.to_string_lossy().to_string(),
            version: latest_version.to_string(),
            size,
            sha256,
        })
    } else {
        None
    }
}

pub fn get_downloaded_dmg_path() -> Option<String> {
    get_downloaded_dmg().map(|download| download.path)
}

fn compare_versions(left: &str, right: &str) -> std::cmp::Ordering {
    let left = parse_version_parts(left);
    let right = parse_version_parts(right);
    for i in 0..std::cmp::max(left.len(), right.len()) {
        let l = left.get(i).unwrap_or(&0);
        let r = right.get(i).unwrap_or(&0);
        match l.cmp(r) {
            std::cmp::Ordering::Equal => {},
            ordering => return ordering,
        }
    }
    std::cmp::Ordering::Equal
}

fn parse_version_parts(value: &str) -> Vec<u32> {
    value
        .split('.')
        .filter_map(|p| {
            let clean: String = p.chars().take_while(|c| c.is_ascii_digit()).collect();
            clean.parse::<u32>().ok()
        })
        .collect()
}

fn app_log(msg: &str) {
    let home = dirs::home_dir().unwrap_or_default();
    let log_dir = crate::platform::metalsharp_home_dir_for(&home).join("logs");
    let _ = fs::create_dir_all(&log_dir);
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
    let secs = now.as_secs();
    let h = (secs / 3600) % 24;
    let m = (secs / 60) % 60;
    let s = secs % 60;
    let line = format!("[{:02}:{:02}:{:02}] {}\n", h, m, s, msg);
    let (year, month, day) = unix_days_to_ymd(secs / 86400);
    let log_path = log_dir.join(format!("{:04}-{:02}-{:02}.log", year, month, day));
    let _ = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
        .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
}

fn unix_days_to_ymd(days_since_epoch: u64) -> (i64, u32, u32) {
    let z = days_since_epoch as i64 + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let mut year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    if month <= 2 {
        year += 1;
    }
    (year, month as u32, day as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unix_days_to_ymd_handles_epoch() {
        assert_eq!(unix_days_to_ymd(0), (1970, 1, 1));
    }

    #[test]
    fn unix_days_to_ymd_handles_current_dates_without_underflow() {
        assert_eq!(unix_days_to_ymd(20_592), (2026, 5, 19));
    }

    #[test]
    fn cached_dmg_requires_manifest_size_and_sha256_match() {
        let home = test_home("cached-dmg-size");
        fs::create_dir_all(&home).expect("create test dir");
        let dmg = home.join("MetalSharp-0.1.0.dmg");
        let metadata = DmgMetadata {
            version: "0.1.0".to_string(),
            source_url: "https://github.com/metalsharp/MetalSharp/releases/download/v0.1.0/MetalSharp-0.1.0-arm64.dmg"
                .to_string(),
            size: 4,
            sha256: "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08".to_string(),
        };

        assert!(!cached_dmg_ready(&dmg, &metadata));

        fs::write(&dmg, b"test").expect("write dmg");
        assert!(!cached_dmg_ready(&dmg, &metadata), "a DMG without provenance is not installable");
        write_dmg_cache_manifest(&dmg, &metadata).expect("write manifest");
        assert!(cached_dmg_ready(&dmg, &metadata));

        let mut wrong_size = metadata.clone();
        wrong_size.size = 0;
        assert!(!cached_dmg_ready(&dmg, &wrong_size));

        let mut wrong_hash = metadata.clone();
        wrong_hash.sha256 = "0000000000000000000000000000000000000000000000000000000000000000".to_string();
        assert!(!cached_dmg_ready(&dmg, &wrong_hash));

        fs::write(&dmg, b"evil").expect("replace cached dmg");
        assert!(!cached_dmg_ready(&dmg, &metadata), "tampered cached DMG must be rejected");

        let _ = fs::remove_dir_all(home);
    }

    #[test]
    fn release_dmg_metadata_requires_a_digest() {
        let asset = GithubAsset {
            name: "MetalSharp-0.1.0-arm64.dmg".to_string(),
            browser_download_url:
                "https://github.com/metalsharp/MetalSharp/releases/download/v0.1.0/MetalSharp-0.1.0-arm64.dmg"
                    .to_string(),
            size: 4,
            digest: None,
        };
        assert!(dmg_metadata(&asset, "0.1.0").is_err());
    }

    fn test_home(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "metalsharp-updater-{}-{}-{}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).expect("system time").as_nanos()
        ))
    }
}
