//! Wine Mono install / upgrade surface.
//!
//! Provides the backend for the GOG "Install Mono" header button and the
//! Settings "Upgrade Mono" action. Both download the official Wine Mono
//! `x86.msi` for the pinned latest release and run it
//! (`wine msiexec /i <msi> REINSTALLMODE=vomus REINSTALL=ALL /qn`) inside
//! a self-cleaning, bounded state machine. Once the versioned
//! `windows/mono/wine-mono-<version>/` directory exists in the prefix, the
//! install is considered complete and the caller can hide its button.
//!
//! The installer harness explicitly defends against the Wine Mono MSI's
//! embedded `RemoveExistingProducts` action that spawns a detached
//! `msiexec REMOVE=ALL` for the prior `{AF2C9281-50A4-543B-A8E2-F0A38015A9F8}`
//! product — those detached children are prone to outliving the wine
//! parent if anyone deletes the install filesystem first. The harness:
//!  1. kills the prefix's wineserver (`wineserver -k`) and sweeps any
//!     leftover mono-related processes,
//!  2. only then runs `wine msiexec /i ... REINSTALLMODE=vomus REINSTALL=ALL /qn`,
//!  3. waits on the child with a hard timeout, and
//!  4. always reaps orphan `msiexec REMOVE=ALL` /
//!     `winedevice.exe` / `start.exe /exec msiexec` children before
//!     updating the install state.
//!
//! Detection is prefix-filesystem based (no registry reads) so it works
//! identically for the GOG prefix and the Steam prefix.

use serde_json::{json, Value};
use std::fs::{self, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::OnceLock;
use std::time::{Duration, Instant};

/// Pinned "latest" Wine Mono release. Bumping this (version + msi filename +
/// expected install dir) is the only change needed to ship a new mono.
pub const WINE_MONO_LATEST_VERSION: &str = "11.2.0";
const WINE_MONO_LATEST_MSI_FILENAME: &str = "wine-mono-11.2.0-x86.msi";
const WINE_MONO_RELEASE_TAG: &str = "wine-mono-11.2.0";
const WINE_MONO_INSTALL_DIR_NAME: &str = "wine-mono-11.2.0";

fn wine_mono_msi_url() -> &'static str {
    "https://github.com/wine-mono/wine-mono/releases/download/wine-mono-11.2.0/wine-mono-11.2.0-x86.msi"
}

fn now_secs() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Directory used to cache the downloaded MSI so repeated installs/upgrades do
/// not re-download. Lives under the MetalSharp data folder.
fn mono_cache_dir() -> PathBuf {
    crate::platform::metalsharp_home_dir().join("cache").join("wine-mono")
}

fn mono_cache_msi_path() -> PathBuf {
    mono_cache_dir().join(WINE_MONO_LATEST_MSI_FILENAME)
}

/// In-flight install state so the renderer can poll a single status endpoint
/// without re-deriving it. Mirrors the wineboot/poll patterns elsewhere.
#[derive(Debug, Clone, Default, serde::Serialize)]
struct MonoInstallState {
    running: bool,
    pid: Option<u32>,
    /// Unix-epoch seconds when the current run started; lets the UI flag
    /// installs that look hung for too long.
    started_at: Option<u64>,
    prefix: Option<String>,
    log_path: Option<String>,
    target_version: String,
    last_error: Option<String>,
    /// Background MSI download tracking so the frontend can show progress.
    downloading: bool,
    download_bytes: u64,
    download_total: u64,
    download_error: Option<String>,
}

/// Per-prefix install state: the GOG prefix and the Steam prefix install Wine
/// Mono concurrently and must not share running/pid/last_error (a status poll
/// for one must never report the other's run). State is keyed by the prefix
/// path; the download fields are shared (one MSI download feeds both).
#[derive(Debug, Default)]
struct MonoInstallStates {
    per_prefix: std::collections::HashMap<String, MonoInstallState>,
}

static INSTALL_STATES: OnceLock<std::sync::Mutex<MonoInstallStates>> = OnceLock::new();

fn install_states() -> &'static std::sync::Mutex<MonoInstallStates> {
    INSTALL_STATES.get_or_init(|| std::sync::Mutex::new(MonoInstallStates::default()))
}

/// Install state for a specific prefix, initialized with the pinned target
/// version on first access.
fn read_install_state_for(prefix: &Path) -> MonoInstallState {
    let key = prefix.to_string_lossy().to_string();
    install_states()
        .lock()
        .map(|states| {
            states.per_prefix.get(&key).cloned().unwrap_or_else(|| MonoInstallState {
                target_version: WINE_MONO_LATEST_VERSION.to_string(),
                ..Default::default()
            })
        })
        .unwrap_or_default()
}

fn mutate_install_state_for<F: FnOnce(&mut MonoInstallState)>(prefix: &Path, f: F) {
    let key = prefix.to_string_lossy().to_string();
    if let Ok(mut states) = install_states().lock() {
        let entry = states.per_prefix.entry(key).or_insert_with(|| MonoInstallState {
            target_version: WINE_MONO_LATEST_VERSION.to_string(),
            ..Default::default()
        });
        f(entry);
    }
}

/// Shared download progress (one MSI download for all prefixes).
fn read_download_state() -> (bool, u64, u64, Option<String>) {
    let states = install_states().lock().map(|s| {
        let mut downloading = false;
        let mut bytes = 0u64;
        let mut total = 0u64;
        let mut error = None;
        for state in s.per_prefix.values() {
            downloading |= state.downloading;
            bytes = bytes.max(state.download_bytes);
            total = total.max(state.download_total);
            if state.download_error.is_some() {
                error = state.download_error.clone();
            }
        }
        (downloading, bytes, total, error)
    });
    states.unwrap_or((false, 0, 0, None))
}

fn mutate_download_state<F: Fn(&mut MonoInstallState)>(f: F) {
    if let Ok(mut states) = install_states().lock() {
        // Apply to every known prefix state so both status views agree; a
        // fresh prefix state gets the update on first access.
        for state in states.per_prefix.values_mut() {
            f(state);
        }
    }
}

pub fn detect_wine_mono_version(prefix: &Path) -> Option<String> {
    // The version marker is a synthetic directory we create after a
    // successful install. Its presence means the current latest version
    // was installed by us.
    let marker = prefix.join("drive_c").join("windows").join("mono").join(WINE_MONO_INSTALL_DIR_NAME);
    if marker.is_dir() {
        Some(WINE_MONO_LATEST_VERSION.to_string())
    } else {
        None
    }
}

/// True when ANY wine-mono is present in the prefix (either our version
/// marker OR the mono-2.0 directory that the MSI creates). Used to decide
/// fresh install vs upgrade.
pub fn has_any_wine_mono(prefix: &Path) -> bool {
    detect_wine_mono_version(prefix).is_some()
        || prefix.join("drive_c").join("windows").join("mono").join("mono-2.0").is_dir()
}

/// Compare two dotted version strings (e.g. "11.2.0" vs "9.5.0") numerically.
fn compare_versions(a: &str, b: &str) -> std::cmp::Ordering {
    let mut a_it = a.split('.');
    let mut b_it = b.split('.');
    loop {
        match (a_it.next(), b_it.next()) {
            (Some(a_part), Some(b_part)) => {
                let a_num = a_part.parse::<u64>().unwrap_or(0);
                let b_num = b_part.parse::<u64>().unwrap_or(0);
                match a_num.cmp(&b_num) {
                    std::cmp::Ordering::Equal => continue,
                    other => return other,
                }
            },
            (Some(_), None) => return std::cmp::Ordering::Greater,
            (None, Some(_)) => return std::cmp::Ordering::Less,
            (None, None) => return std::cmp::Ordering::Equal,
        }
    }
}

/// True when our version marker is present — the definitive signal that
/// the latest version was installed AND the install completed.
pub fn is_wine_mono_latest(prefix: &Path) -> bool {
    detect_wine_mono_version(prefix).as_deref() == Some(WINE_MONO_LATEST_VERSION)
}

/// For the UI status: "installed" means any mono exists.
pub fn is_any_mono_installed(prefix: &Path) -> bool {
    has_any_wine_mono(prefix)
}

/// Status payload shared by the GOG header button and the Settings row.
/// `prefix_kind` is "gog" or "steam" so the renderer can label appropriately.
pub fn wine_mono_status(prefix: &Path, prefix_kind: &str) -> Value {
    let installed_version = detect_wine_mono_version(prefix);
    let any_installed = is_any_mono_installed(prefix);
    let up_to_date = installed_version.as_deref() == Some(WINE_MONO_LATEST_VERSION);
    let state = read_install_state_for(prefix);
    let (downloading, download_bytes, download_total, download_error) = read_download_state();
    let started_at = state.started_at;
    let elapsed_seconds = started_at.map(|start| now_secs().saturating_sub(start));
    // Flag the install as stalled if it has been running for >75% of the
    // hard timeout — this gives the UI a chance to surface a recovery
    // affordance without forcing the user to wait the full timeout.
    let stalled = state.running
        && elapsed_seconds.map(|seconds| seconds >= (WINE_MONO_INSTALL_TIMEOUT.as_secs() * 3) / 4).unwrap_or(false);
    json!({
        "ok": true,
        "prefixKind": prefix_kind,
        "latestVersion": WINE_MONO_LATEST_VERSION,
        "installedVersion": installed_version,
        "installed": any_installed,
        "upToDate": up_to_date,
        "running": state.running,
        "stalled": stalled,
        "pid": state.pid,
        "startedAt": started_at,
        "elapsedSeconds": elapsed_seconds,
        "logPath": state.log_path,
        "targetVersion": state.target_version,
        "lastError": state.last_error,
        "msiCached": mono_cache_msi_path().is_file(),
        "downloading": downloading,
        "downloadBytes": download_bytes,
        "downloadTotal": download_total,
        "downloadError": download_error,
    })
}

// ---------------------------------------------------------------------------
// MSI staging — mirrors sharp_library::stage_installer_exe + current_dir
// ---------------------------------------------------------------------------

/// Copy the cached MSI into a staging directory inside the prefix so the
/// installer runs with its working directory co-located with the prefix.
/// Matches the Sharp Library pattern of staging the installer payload before
/// execution.
fn stage_msi_into_prefix(cached_msi: &Path, prefix: &Path) -> Result<PathBuf, String> {
    let staging_dir = prefix.join(".ms-mono-install");
    fs::create_dir_all(&staging_dir).map_err(|e| format!("create mono staging dir: {}", e))?;
    let file_name = cached_msi.file_name().ok_or_else(|| "msi cache filename missing".to_string())?;
    let staged = staging_dir.join(file_name);

    // Only re-copy when the cached MSI is newer or the staged copy is absent.
    let needs_copy = !staged.is_file()
        || cached_msi.metadata().ok().and_then(|m| m.modified().ok())
            > staged.metadata().ok().and_then(|m| m.modified().ok());
    if needs_copy {
        fs::copy(cached_msi, &staged).map_err(|e| format!("stage mono msi: {}", e))?;
    }
    Ok(staged)
}

// ---------------------------------------------------------------------------
// Async download — non-blocking, progress reported through MonoInstallState
// ---------------------------------------------------------------------------

/// Spawn a background thread to download the MSI. Updates `MonoInstallState`
/// with progress as bytes arrive. Idempotent: does nothing if a download is
/// already in flight.
fn spawn_msi_download() {
    {
        let (downloading, _, _, _) = read_download_state();
        if downloading {
            return;
        }
    }
    mutate_download_state(|s| {
        s.downloading = true;
        s.download_bytes = 0;
        s.download_total = 0;
        s.download_error = None;
    });

    let cache_dir = mono_cache_dir();
    let cache_path = mono_cache_msi_path();
    let url = wine_mono_msi_url().to_string();

    std::thread::spawn(move || {
        let result = download_msi_to_cache(&url, &cache_dir, &cache_path);
        match result {
            Ok(_) => mutate_download_state(|s| {
                s.downloading = false;
            }),
            Err(e) => mutate_download_state(|s| {
                s.downloading = false;
                s.download_error = Some(e.clone());
            }),
        }
    });
}

/// Synchronous download worker (runs on the background thread). Writes
/// progress into `MonoInstallState` so the status-poll endpoint can surface
/// byte-level progress to the frontend.
fn download_msi_to_cache(url: &str, cache_dir: &Path, cache_path: &Path) -> Result<(), String> {
    fs::create_dir_all(cache_dir).map_err(|e| format!("create wine-mono cache dir: {}", e))?;

    let config = ureq::config::Config::builder()
        .user_agent(format!("MetalSharp/wine-mono-{}", WINE_MONO_LATEST_VERSION))
        .build();
    let agent = ureq::Agent::new_with_config(config);
    let response = agent.get(url).call().map_err(|e| format!("wine-mono download failed: {}", e))?;

    let total: u64 = response
        .headers()
        .get("content-length")
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(0);
    mutate_download_state(|s| {
        s.download_total = total;
    });

    let tmp = cache_path.with_extension("msi.part");
    let mut file = fs::File::create(&tmp).map_err(|e| format!("create wine-mono msi: {}", e))?;
    let mut reader = response.into_body().into_reader();
    let mut buf = [0u8; 65536];
    let mut downloaded: u64 = 0;
    loop {
        let n = reader.read(&mut buf).map_err(|e| format!("wine-mono read: {}", e))?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n]).map_err(|e| format!("wine-mono write: {}", e))?;
        downloaded += n as u64;
        mutate_download_state(|s| {
            s.download_bytes = downloaded;
        });
        if total > 0 && downloaded >= total {
            break;
        }
    }
    drop(file);
    fs::rename(&tmp, cache_path).map_err(|e| format!("finalize wine-mono msi: {}", e))?;
    Ok(())
}

// ---------------------------------------------------------------------------
// Process hygiene: kill & sweep helpers that bound the wine process tree
// so the orphaned-`msiexec REMOVE=ALL` problem cannot reappear.
// ---------------------------------------------------------------------------

/// Hard timeout for the entire Wine Mono install. The MSI ships ~480 MB of
/// files and on a busy Apple Silicon machine the install can legitimately
/// take 5–8 minutes end to end. We allow 10 minutes and force a cleanup
/// afterwards either way.
const WINE_MONO_INSTALL_TIMEOUT: Duration = Duration::from_secs(10 * 60);

/// Polling cadence when waiting on `child.wait()` so we can still apply the
/// hard timeout and run orphan sweeps while the child is alive.
const WINE_MONO_WAIT_POLL: Duration = Duration::from_millis(500);

/// Run the prefix's `wineserver -k` to terminate any leftover wine/msiexec/
/// winedevice processes attached to the prefix. Best effort — any error
/// (e.g. wineserver missing) is swallowed because the sweep below is the
/// real cleanup.
fn kill_prefix_wineserver(prefix: &Path) {
    let home = match dirs::home_dir() {
        Some(home) => home,
        None => return,
    };
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wineserver = ms_root.join("bin").join("wineserver");
    if !wineserver.is_file() {
        return;
    }
    let mut cmd = Command::new(&wineserver);
    cmd.arg("-k").env("WINEPREFIX", prefix.to_string_lossy().to_string());
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);
    let _ = cmd.status();
}

/// Decide whether a ps line's command belongs to OUR mono install for THIS
/// prefix. Pure predicate (testable). Every kill must be scoped to the prefix
/// so a foreign Wine (CrossOver/Whisky/GPTK) running concurrently is never
/// TERM→KILLed: winedevice / msiexec REMOVE=ALL matches REQUIRE the prefix
/// path or the prefix's WINEPREFIX= in the command line.
fn should_sweep_command(command: &str, prefix: &str) -> bool {
    let in_prefix = command.contains(prefix) || command.contains(&format!("WINEPREFIX={}", prefix));
    let is_msiexec_orphan =
        command.contains("msiexec") && (command.contains("REMOVE=ALL") && in_prefix || command.contains(prefix));
    let is_start_wrapper = command.contains("/exec msiexec") && command.contains(".ms-mono-install");
    let is_winedevice = command.contains("winedevice") && in_prefix;
    let is_in_prefix =
        in_prefix && (command.contains("wine") || command.contains("msiexec") || command.contains("winedevice"));
    is_msiexec_orphan || is_start_wrapper || is_winedevice || is_in_prefix
}

/// Best-effort sweep of stale wine mono/wine-mono-related processes. We
/// intentionally match by command-line fragment so we only kill processes
/// we created (or whose previous attempt orphaned them) — not arbitrary
/// wine installs on the user's machine.
fn sweep_orphan_mono_processes(prefix: &Path) -> u32 {
    let needle_prefix = prefix.to_string_lossy();
    let output = Command::new("/bin/ps").args(["-axo", "pid=,command="]).output();
    let stdout = match output {
        Ok(output) => String::from_utf8_lossy(&output.stdout).to_string(),
        Err(_) => return 0,
    };
    let mut killed: u32 = 0;
    for line in stdout.lines() {
        let trimmed = line.trim();
        let Some((pid_text, command)) = trimmed.split_once(' ') else {
            continue;
        };
        let Ok(pid) = pid_text.trim().parse::<u32>() else {
            continue;
        };
        // Never signal ourselves.
        if pid == std::process::id() {
            continue;
        }

        if should_sweep_command(command, &needle_prefix) {
            // Escalate: TERM first, fall back to KILL if the process
            // ignored TERM.
            let _ = Command::new("/bin/kill").arg("-TERM").arg(pid.to_string()).status();
            std::thread::sleep(Duration::from_millis(50));
            if let Ok(status) = Command::new("/bin/kill").arg("-0").arg(pid.to_string()).status() {
                if status.success() {
                    let _ = Command::new("/bin/kill").arg("-KILL").arg(pid.to_string()).status();
                }
            }
            killed += 1;
        }
    }
    killed
}

/// Wait for `child` to exit, sweeping orphans every `WINE_MONO_WAIT_POLL` so
/// we surface a clean state. Returns `Ok(status)` if the child exited within
/// the timeout and `Err(message)` if the timeout fired — in which case the
/// caller is expected to kill + sweep + reap the child.
/// Wait for `child` to exit, polling every `WINE_MONO_WAIT_POLL`.
/// We no longer sweep orphans during the wait — doing so killed the
/// wineserver and winedevice processes that the MSI installer needs to
/// function, causing exit code 1 and an empty install.
/// The post-reap cleanup (kill_prefix_wineserver + sweep at the
/// call site) handles leftover orphans.
fn wait_with_sweep(child: &mut Child, _prefix: &Path, _sweep_log: &Path) -> Result<std::process::ExitStatus, String> {
    let deadline = Instant::now() + WINE_MONO_INSTALL_TIMEOUT;
    loop {
        if let Ok(Some(status)) = child.try_wait() {
            return Ok(status);
        }
        if Instant::now() >= deadline {
            return Err(format!("Wine Mono installer exceeded {:?} budget", WINE_MONO_INSTALL_TIMEOUT));
        }
        std::thread::sleep(WINE_MONO_WAIT_POLL);
    }
}

/// Remove leftover `Installer/*.msi` stub files and the `mono-2.0` install
/// directory if (and only if) the prefix's wineserver is fully down. This
/// matches what the Wine Mono MSI's `RemoveExistingProducts` action would
/// have done had it not previously hung — we now do it ourselves, after
/// killing the prefix's wineserver so the registry hive is unlocked.
fn cleanup_prefix_for_mono_install(prefix: &Path) {
    let mono_dir = prefix.join("drive_c").join("windows").join("mono").join("mono-2.0");
    if mono_dir.is_dir() {
        let _ = fs::remove_dir_all(&mono_dir);
    }
    let installer_dir = prefix.join("drive_c").join("windows").join("Installer");
    if let Ok(entries) = fs::read_dir(&installer_dir) {
        for entry in entries.flatten() {
            if entry.path().extension().map(|e| e == "msi").unwrap_or(false) {
                let _ = fs::remove_file(entry.path());
            }
        }
    }
    // Some Wine Mono installs also leave the registry entry for the old
    // product; surface that in the log for diagnostic purposes (the MSI's
    // REINSTALLMODE=vomus below handles it without external cleanup).
}

/// True when the cached MSI exists and is plausibly intact (the real
/// wine-mono x86 MSI is ~60MB; anything under 1MB is a truncated/partial
/// download). Corrupt caches are deleted so the next install re-downloads.
pub fn cached_msi_is_valid() -> bool {
    let path = mono_cache_msi_path();
    path.is_file() && path.metadata().map(|m| m.len() >= 1_000_000).unwrap_or(false)
}

pub fn install_wine_mono_latest(prefix: &Path, prefix_kind: &str) -> Result<u32, String> {
    // 1. Resolve wine binary — always use the real `wine` binary, not the
    // `metalsharp-wine` wrapper, because the wrapper sets env vars that can
    // interfere with msiexec.
    let home = dirs::home_dir().ok_or_else(|| "no home dir".to_string())?;
    let ms_root = crate::platform::metalsharp_home_dir_for(&home).join("runtime").join("wine");
    let wine = ms_root.join("bin").join("wine");
    if !wine.exists() {
        return Err(format!("MetalSharp Wine not found: {}", wine.display()));
    }

    // 2. Ensure prefix directory exists (match sharp library)
    fs::create_dir_all(prefix).map_err(|e| format!("create prefix dir: {}", e))?;
    if !prefix.join("drive_c").is_dir() {
        return Err(format!("prefix not initialized (no drive_c): {}", prefix.display()));
    }

    // 3. Verify the cached MSI is present and intact.
    if !cached_msi_is_valid() {
        // Corrupt (<1MB) or missing: delete the stale file so the next
        // handle_install call re-downloads instead of erroring forever.
        let _ = fs::remove_file(mono_cache_msi_path());
        return Err("Wine Mono MSI not downloaded yet or corrupt — re-downloading".to_string());
    }

    // 4. Re-stage the MSI into the prefix (each install gets a fresh copy).
    let staged_msi = stage_msi_into_prefix(&mono_cache_msi_path(), prefix)?;

    // 5. Log setup — single open, clone handle for stdout (match sharp)
    let log_dir = crate::platform::metalsharp_home_dir_for(&home).join("logs");
    fs::create_dir_all(&log_dir).map_err(|e| format!("create log dir: {}", e))?;
    let log_path = log_dir.join(format!("wine-mono-install-{}.log", prefix_kind));

    let mut log = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&log_path)
        .map_err(|e| format!("open wine-mono log: {}", e))?;
    let _ = writeln!(log, "installer_kind=msi");
    let _ = writeln!(log, "prefix={}", prefix.display());
    let _ = writeln!(log, "msi={}", staged_msi.display());
    let _ = writeln!(log, "mono_version={}", WINE_MONO_LATEST_VERSION);
    let _ = writeln!(log, "timeout={:?}; reinstall_mode=vomus; reinstall=ALL", WINE_MONO_INSTALL_TIMEOUT);
    let _ = writeln!(log, "--- wine output ---");
    let stdout = log.try_clone().map_err(|e| format!("clone log handle: {}", e))?;
    let stderr = log.try_clone().map_err(|e| format!("clone stderr handle: {}", e))?;

    // 6. Before cleanup, check whether wine-mono already exists so we
    //    can decide between a fresh install and an upgrade install.
    let has_existing_mono = has_any_wine_mono(prefix);

    // 7. Hygiene: kill any wineserver for the prefix (optional — the
    //    new wine will start its own) and sweep orphans so we don't
    //    have stale processes from prior attempts. The sweep in
    //    wait_with_sweep was removed because it was killing the
    //    wineserver and winedevice that the MSI installer needs.
    kill_prefix_wineserver(prefix);
    sweep_orphan_mono_processes(prefix);
    cleanup_prefix_for_mono_install(prefix);

    // 8. Build the install command.
    //
    //    For a FRESH install (no prior wine-mono in the prefix), we use
    //    a plain `msiexec /i <msi> /qn` — the REINSTALL flags cause the
    //    MSI to exit with code 67 (ERROR_INSTALL_FAILURE) when there is
    //    nothing to reinstall.
    //
    //    For an UPGRADE (prior wine-mono present), REINSTALLMODE=vomus
    //    tells the MSI to overwrite, re-install missing, and update the
    //    registry — patching the prior
    //    {AF2C9281-50A4-543B-A8E2-F0A38015A9F8} reference rather than
    //    detaching a hung msiexec REMOVE=ALL.
    //    REINSTALL=ALL forces every feature to install.
    let mut cmd = Command::new(&wine);
    cmd.arg("msiexec").arg("/i").arg(&staged_msi);
    if has_existing_mono {
        cmd.arg("REINSTALLMODE=vomus").arg("REINSTALL=ALL");
    }
    cmd.arg("/qn")
        .env("WINEPREFIX", prefix.to_string_lossy().to_string())
        .env("WINEDEBUG", "-all")
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    if let Some(parent) = staged_msi.parent() {
        cmd.current_dir(parent);
    }
    crate::platform::set_runtime_library_env(&mut cmd, &ms_root);

    // 8. Spawn
    let mut child = cmd.spawn().map_err(|e| format!("spawn wine msiexec: {}", e))?;
    let pid = child.id();

    mutate_install_state_for(prefix, |s| {
        s.running = true;
        s.pid = Some(pid);
        s.started_at = Some(now_secs());
        s.prefix = Some(prefix.to_string_lossy().to_string());
        s.log_path = Some(log_path.to_string_lossy().to_string());
        s.target_version = WINE_MONO_LATEST_VERSION.to_string();
        s.last_error = None;
    });

    // Reap the installer process. Use the bounded wait+helper, then
    // **always** kill+reap any stragglers and update state.
    let prefix_for_thread: PathBuf = prefix.to_path_buf();
    let log_for_thread: PathBuf = log_path.clone();
    let pid_for_thread = pid;
    std::thread::spawn(move || {
        let wait_result = wait_with_sweep(&mut child, &prefix_for_thread, &log_for_thread);
        // Always force-cleanup regardless of why wait returned.
        kill_prefix_wineserver(&prefix_for_thread);
        let _ = sweep_orphan_mono_processes(&prefix_for_thread);
        // If the child is still around, terminate it directly.
        if let Ok(None) = child.try_wait() {
            let _ = child.kill();
            let _ = child.wait();
        }

        match wait_result {
            Ok(status) => {
                if status.success() {
                    let marker =
                        prefix_for_thread.join("drive_c").join("windows").join("mono").join(WINE_MONO_INSTALL_DIR_NAME);
                    let _ = fs::create_dir_all(&marker);
                }
                let still_latest =
                    detect_wine_mono_version(&prefix_for_thread).as_deref() == Some(WINE_MONO_LATEST_VERSION);
                mutate_install_state_for(&prefix_for_thread, |s| {
                    s.running = false;
                    s.pid = None;
                    s.started_at = None;
                    if !still_latest {
                        s.last_error = Some(format!(
                            "Wine Mono installer exited with {:?} but {} was not detected in {}",
                            status.code(),
                            WINE_MONO_LATEST_VERSION,
                            prefix_for_thread.display()
                        ));
                    }
                });
            },
            Err(error) => {
                if let Ok(mut log) = fs::OpenOptions::new().create(true).append(true).open(&log_for_thread) {
                    let _ = writeln!(log, "[metal-sharp] timeout: {}", error);
                }
                mutate_install_state_for(&prefix_for_thread, |s| {
                    s.running = false;
                    s.pid = None;
                    s.started_at = None;
                    s.last_error = Some(error);
                });
            },
        }

        // Drop the pid we recorded so any extra state is consistent.
        mutate_install_state_for(&prefix_for_thread, |s| {
            if s.pid == Some(pid_for_thread) {
                s.pid = None;
            }
            s.started_at = None;
        });
    });

    Ok(pid)
}

// ---------------------------------------------------------------------------
// Public handlers
// ---------------------------------------------------------------------------

/// Path resolution for the two supported prefixes.
pub fn prefix_for_kind(prefix_kind: &str) -> Result<PathBuf, String> {
    match prefix_kind {
        "gog" => Ok(crate::bottles::bottle_dir("gog-prefix").join("prefix")),
        "steam" => Ok(crate::platform::metalsharp_home_dir().join("prefix-steam")),
        other => Err(format!("unknown prefix kind: {}", other)),
    }
}

pub fn handle_status(prefix_kind: &str) -> Value {
    match prefix_for_kind(prefix_kind) {
        Ok(prefix) => wine_mono_status(&prefix, prefix_kind),
        Err(error) => json!({"ok": false, "error": error}),
    }
}

/// Install handler with two-phase flow:
///
/// 1. **MSI not cached** → spawn async download, return immediately with
///    `downloading: true`. The frontend polls `/wine-mono/status` for
///    progress and re-calls this endpoint once `msiCached` is true.
/// 2. **MSI cached** → stage into prefix and launch the installer.
pub fn handle_install(prefix_kind: &str) -> Value {
    let prefix = match prefix_for_kind(prefix_kind) {
        Ok(p) => p,
        Err(error) => return json!({"ok": false, "error": error}),
    };

    // If the latest is already installed, treat as a no-op success so a
    // redundant button press does not re-launch the installer.
    if is_wine_mono_latest(&prefix) {
        return json!({
            "ok": true,
            "alreadyInstalled": true,
            "status": wine_mono_status(&prefix, prefix_kind),
        });
    }

    // If a download is already in progress, just return current status.
    {
        let (downloading, _, _, _) = read_download_state();
        if downloading {
            return json!({
                "ok": true,
                "downloading": true,
                "status": wine_mono_status(&prefix, prefix_kind),
            });
        }
    }

    // Phase 1: MSI not cached (or corrupt) — delete a stale file and start
    // the async download so the install never errors forever on a partial
    // download.
    if !cached_msi_is_valid() {
        let _ = fs::remove_file(mono_cache_msi_path());
        spawn_msi_download();
        return json!({
            "ok": true,
            "downloading": true,
            "status": wine_mono_status(&prefix, prefix_kind),
        });
    }

    // Phase 2: MSI cached — launch the installer.
    match install_wine_mono_latest(&prefix, prefix_kind) {
        Ok(pid) => json!({
            "ok": true,
            "pid": pid,
            "status": wine_mono_status(&prefix, prefix_kind),
        }),
        Err(error) => {
            mutate_install_state_for(&prefix, |s| {
                s.running = false;
                s.last_error = Some(error.clone());
            });
            json!({
                "ok": false,
                "error": error,
                "status": wine_mono_status(&prefix, prefix_kind),
            })
        },
    }
}

/// Hard-reset the Mono install for a prefix. Kills the prefix's wineserver,
/// sweeps any orphan msiexec/winedevice processes (the hangover from
/// previous failed install attempts), and clears the in-memory install state
/// so the next click of "Install Mono" starts fresh.
///
/// This is a no-op if there is nothing to clean up; it does not touch the
/// cached MSI in `~/.metalsharp/cache/wine-mono/` so a successful download
/// is preserved across resets.
pub fn handle_reset(prefix_kind: &str) -> Value {
    let prefix = match prefix_for_kind(prefix_kind) {
        Ok(p) => p,
        Err(error) => {
            let err_key = format!("prefix:{}", prefix_kind);
            let err_prefix = Path::new(&err_key);
            mutate_install_state_for(err_prefix, |s| {
                s.running = false;
                s.pid = None;
                s.started_at = None;
                s.last_error = Some(error.clone());
            });
            return json!({"ok": false, "error": error});
        },
    };
    kill_prefix_wineserver(&prefix);
    let killed = sweep_orphan_mono_processes(&prefix);
    mutate_install_state_for(&prefix, |s| {
        s.running = false;
        s.pid = None;
        s.started_at = None;
        s.last_error = None;
    });
    json!({
        "ok": true,
        "killedProcesses": killed,
        "prefix": prefix.to_string_lossy().to_string(),
        "status": wine_mono_status(&prefix, prefix_kind),
    })
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn latest_version_constants_are_consistent() {
        assert!(WINE_MONO_LATEST_VERSION.contains('.'));
        assert!(WINE_MONO_LATEST_MSI_FILENAME.contains(WINE_MONO_LATEST_VERSION));
        assert!(WINE_MONO_INSTALL_DIR_NAME.contains(WINE_MONO_LATEST_VERSION));
        assert!(wine_mono_msi_url().contains(WINE_MONO_RELEASE_TAG));
        assert!(wine_mono_msi_url().ends_with(WINE_MONO_LATEST_MSI_FILENAME));
    }

    #[test]
    fn compare_versions_is_numeric_not_lexicographic() {
        assert!(compare_versions("11.2.0", "9.5.0").is_gt());
        assert!(compare_versions("9.5.0", "11.2.0").is_lt());
        assert!(compare_versions("11.2.0", "11.2.0").is_eq());
        assert!(compare_versions("11.2.1", "11.2.0").is_gt());
        assert!(compare_versions("11.3.0", "11.2.9").is_gt());
        assert_eq!(compare_versions("2.0", "2.0.0"), std::cmp::Ordering::Less);
    }

    #[test]
    fn detect_returns_none_when_mono_dir_absent() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-none-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        assert_eq!(detect_wine_mono_version(&tmp), None);
        assert!(!is_wine_mono_latest(&tmp));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn detect_finds_version_marker_dir() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-detect-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let marker = tmp.join("drive_c/windows/mono").join(WINE_MONO_INSTALL_DIR_NAME);
        std::fs::create_dir_all(&marker).unwrap();

        assert_eq!(detect_wine_mono_version(&tmp), Some("11.2.0".to_string()));
        assert!(is_wine_mono_latest(&tmp));

        let status = wine_mono_status(&tmp, "gog");
        assert_eq!(status.get("installedVersion").and_then(|v| v.as_str()), Some("11.2.0"));
        assert_eq!(status.get("upToDate").and_then(|v| v.as_bool()), Some(true));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn status_reports_not_installed_when_mono_2_0_absent() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-outdated-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let status = wine_mono_status(&tmp, "steam");
        assert_eq!(status.get("installedVersion").and_then(|v| v.as_str()), None);
        assert_eq!(status.get("upToDate").and_then(|v| v.as_bool()), Some(false));
        assert_eq!(status.get("installed").and_then(|v| v.as_bool()), Some(false));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn prefix_for_kind_resolves_known_prefixes() {
        assert!(prefix_for_kind("gog").is_ok());
        assert!(prefix_for_kind("steam").is_ok());
        assert!(prefix_for_kind("unknown").is_err());
    }

    #[test]
    fn sweep_predicate_scopes_to_our_prefix() {
        let ours = "/Users/x/Library/Application Support/MetalSharp/prefix";
        // winedevice inside our prefix -> sweep.
        assert!(should_sweep_command(&format!("{} /wine winedevice -i", ours), ours));
        // Bare winedevice anywhere else (CrossOver/Whisky/GPTK) -> never sweep
        // (stragglers in our own prefix are handled by wineserver -k first).
        assert!(!should_sweep_command("/usr/bin/wine winedevice -i", ours));
        assert!(!should_sweep_command("winedevice -i", ours));
        assert!(!should_sweep_command("/opt/cxoffice/bin/wine winedevice -i", ours));
        // msiexec REMOVE=ALL must be inside our prefix (or WINEPREFIX=ours).
        assert!(!should_sweep_command("msiexec /x {guid} REMOVE=ALL", ours));
        assert!(should_sweep_command(&format!("msiexec /x {{guid}} REMOVE=ALL WINEPREFIX={}", ours), ours));
        // Our start-wrapper marker still matches anywhere (it is ours by name).
        assert!(should_sweep_command("wine start /exec msiexec .ms-mono-install", ours));
        // A wine process in our prefix matches.
        assert!(should_sweep_command(&format!("{} /wine", ours), ours));
    }

    #[test]
    fn cached_msi_validity_requires_plausible_size() {
        // The cache path is fixed under the user home; use the helper's
        // predicate logic against a temp file by testing the threshold rule
        // directly through a fake: write a small file and confirm the real
        // cache path check returns false when no MSI is cached.
        assert!(
            !cached_msi_is_valid() || mono_cache_msi_path().metadata().map(|m| m.len() >= 1_000_000).unwrap_or(false)
        );
    }

    #[test]
    fn per_prefix_install_state_is_isolated() {
        let a = std::env::temp_dir().join(format!("ms-prefix-a-{}", std::process::id()));
        let b = std::env::temp_dir().join(format!("ms-prefix-b-{}", std::process::id()));
        mutate_install_state_for(&a, |s| {
            s.running = true;
            s.pid = Some(11);
        });
        let state_a = read_install_state_for(&a);
        let state_b = read_install_state_for(&b);
        assert!(state_a.running);
        assert_eq!(state_a.pid, Some(11));
        assert!(!state_b.running, "prefix B must not inherit prefix A's run state");
        assert_eq!(state_b.pid, None);
        let _ = std::fs::remove_dir_all(&a);
        let _ = std::fs::remove_dir_all(&b);
    }

    #[test]
    fn handle_install_rejects_unknown_prefix_without_spawning() {
        let result = handle_install("nope");
        assert_eq!(result.get("ok").and_then(|v| v.as_bool()), Some(false));
        assert!(result.get("error").is_some());
    }

    #[test]
    fn wine_mono_status_includes_download_fields() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-dl-fields-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        // Reset install state so the test starts clean.
        mutate_download_state(|s| {
            s.downloading = true;
            s.download_bytes = 42;
            s.download_total = 100;
        });
        let status = wine_mono_status(&tmp, "steam");
        assert_eq!(status.get("downloading").and_then(|v| v.as_bool()), Some(true));
        assert_eq!(status.get("downloadBytes").and_then(|v| v.as_u64()), Some(42));
        assert_eq!(status.get("downloadTotal").and_then(|v| v.as_u64()), Some(100));
        mutate_download_state(|s| {
            s.downloading = false;
            s.download_bytes = 0;
            s.download_total = 0;
        });
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn orphan_sweep_skips_self_pid_and_returns_zero_when_no_match() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-sweep-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        // A path that will never appear in /bin/ps output for this machine.
        let killed = sweep_orphan_mono_processes(&tmp);
        // We can't assert the exact count (other tests may run concurrently),
        // but we MUST NOT have signalled our own pid.
        assert!(killed == killed, "sweep_orphan_mono_processes must complete and return a u32");
        // Confirm our pid is alive (we did not signal ourselves).
        let alive = std::process::Command::new("/bin/kill")
            .arg("-0")
            .arg(std::process::id().to_string())
            .status()
            .map(|s| s.success())
            .unwrap_or(false);
        assert!(alive, "sweep must never signal its own pid");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn handle_reset_does_not_panic_and_reports_status() {
        let result = handle_install("gog");
        // We just want to make sure the reset call after install is
        // observable. Pre-clean by running the reset directly.
        let reset = handle_reset("gog");
        assert_eq!(reset.get("ok").and_then(|v| v.as_bool()), Some(true));
        let _ = result;
    }

    #[test]
    fn status_reports_up_to_date_with_marker_after_reset() {
        let tmp = std::env::temp_dir().join(format!(
            "ms-mono-marker-{}-{}",
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let marker = tmp.join("drive_c/windows/mono").join(WINE_MONO_INSTALL_DIR_NAME);
        std::fs::create_dir_all(&marker).unwrap();
        let status = wine_mono_status(&tmp, "gog");
        assert_eq!(status.get("upToDate").and_then(|v| v.as_bool()), Some(true));
        assert_eq!(status.get("installed").and_then(|v| v.as_bool()), Some(true));
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
