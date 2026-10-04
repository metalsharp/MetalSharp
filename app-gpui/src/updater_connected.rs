//! Source-backed typed adapter for the Electron updater flow. No polling is self-owned: host
//! chooses cadence and serializes each execute call through its backend dispatcher.
use serde::Deserialize;
use std::{
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Clone, Copy, Debug, serde::Serialize, serde::Deserialize, Eq, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum UpdateVariant {
    Regular,
    Fex,
}
impl UpdateVariant {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Regular => "regular",
            Self::Fex => "fex",
        }
    }
}
#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct DownloadProgress {
    pub status: String,
    #[serde(default)]
    pub percent: u8,
    #[serde(default)]
    pub message: String,
    pub error: Option<String>,
}
#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct DmgPath {
    pub path: Option<String>,
    pub version: Option<String>,
    pub ok: Option<bool>,
}
#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct InstallStatus {
    pub phase: String,
    #[serde(default)]
    pub percent: u8,
    #[serde(default)]
    pub message: String,
    pub error: Option<String>,
    pub new_version: Option<String>,
    pub dmg_path: Option<String>,
    #[serde(default)]
    pub timestamp: u64,
}
#[derive(Clone, Debug, PartialEq)]
pub enum UpdaterError {
    Backend(String),
    InvalidProgress,
    UnsafePath,
    Signature(String),
    VersionMismatch,
    InvalidManifestProvenance,
    Executor(String),
}
impl std::fmt::Display for UpdaterError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for UpdaterError {}

pub trait UpdateApi {
    fn start_download(&self, variant: UpdateVariant) -> Result<(), String>;
    fn download_progress(&self) -> Result<DownloadProgress, String>;
    fn dmg_path(&self, variant: UpdateVariant) -> Result<DmgPath, String>;
    fn install_status(&self) -> Result<Option<InstallStatus>, String>;
    fn clear_install_status(&self) -> Result<(), String>;
}
pub struct InstallStatusFile {
    path: PathBuf,
}
impl InstallStatusFile {
    /// Explicitly supplied connected-host home; never inferred from ambient production paths.
    pub fn new(home: &Path) -> Result<Self, UpdaterError> {
        let home = home.canonicalize().map_err(|_| UpdaterError::UnsafePath)?;
        if !home.is_dir() || home == Path::new("/") {
            return Err(UpdaterError::UnsafePath);
        }
        Ok(Self {
            path: home.join("update_install_status.json"),
        })
    }
    pub fn read(&self) -> Result<Option<InstallStatus>, UpdaterError> {
        match std::fs::symlink_metadata(&self.path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(UpdaterError::Executor(e.to_string())),
            Ok(m) if !m.is_file() || m.file_type().is_symlink() => Err(UpdaterError::UnsafePath),
            Ok(_) => {
                let bytes =
                    std::fs::read(&self.path).map_err(|e| UpdaterError::Executor(e.to_string()))?;
                let status: InstallStatus =
                    serde_json::from_slice(&bytes).map_err(|_| UpdaterError::InvalidProgress)?;
                if status.percent > 100 {
                    return Err(UpdaterError::InvalidProgress);
                }
                Ok(Some(status))
            }
        }
    }
    pub fn clear(&self) -> Result<(), UpdaterError> {
        match std::fs::symlink_metadata(&self.path) {
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(UpdaterError::Executor(e.to_string())),
            Ok(m) if !m.is_file() || m.file_type().is_symlink() => Err(UpdaterError::UnsafePath),
            Ok(_) => {
                std::fs::remove_file(&self.path).map_err(|e| UpdaterError::Executor(e.to_string()))
            }
        }
    }
}
pub struct NativeUpdateAdapter<'a> {
    pub backend: &'a crate::backend::BackendClient,
    pub status_file: &'a InstallStatusFile,
}
impl UpdateApi for NativeUpdateAdapter<'_> {
    fn start_download(&self, v: UpdateVariant) -> Result<(), String> {
        self.backend
            .post("/update/start", serde_json::json!({"variant":v.as_str()}))
            .map(|_| ())
            .map_err(|e| e.to_string())
    }
    fn download_progress(&self) -> Result<DownloadProgress, String> {
        self.backend
            .get("/update/progress")
            .map_err(|e| e.to_string())
    }
    fn dmg_path(&self, v: UpdateVariant) -> Result<DmgPath, String> {
        self.backend.update_dmg_path(v).map_err(|e| e.to_string())
    }
    fn install_status(&self) -> Result<Option<InstallStatus>, String> {
        self.status_file.read().map_err(|e| e.to_string())
    }
    fn clear_install_status(&self) -> Result<(), String> {
        self.status_file.clear().map_err(|e| e.to_string())
    }
}

#[derive(Clone, Debug, serde::Serialize, serde::Deserialize, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum UpdatePhase {
    Idle,
    Downloading,
    Downloaded,
    Installing,
    Complete,
    Error,
}
#[derive(Clone, Debug, serde::Serialize, serde::Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct UpdateSnapshot {
    pub phase: UpdatePhase,
    pub variant: UpdateVariant,
    pub percent: u8,
    pub message: String,
    pub error: Option<String>,
    pub dmg: Option<PathBuf>,
    pub version: Option<String>,
}
impl UpdateSnapshot {
    pub fn new(variant: UpdateVariant) -> Self {
        Self {
            phase: UpdatePhase::Idle,
            variant,
            percent: 0,
            message: "Ready to check for update".into(),
            error: None,
            dmg: None,
            version: None,
        }
    }
}
#[derive(Clone, Debug, PartialEq)]
pub enum UpdateIntent {
    Start(UpdateVariant),
    PollDownload(UpdateVariant),
    FetchDmgPath(UpdateVariant),
    PollInstall,
    ClearStatus,
}
impl UpdateIntent {
    pub fn execute(&self, api: &impl UpdateApi) -> Result<Option<UpdateSnapshot>, UpdaterError> {
        match self {
            Self::Start(v) => {
                api.clear_install_status().map_err(UpdaterError::Backend)?;
                api.start_download(*v).map_err(UpdaterError::Backend)?;
                Ok(Some(UpdateSnapshot {
                    phase: UpdatePhase::Downloading,
                    variant: *v,
                    percent: 0,
                    message: "Starting download…".into(),
                    error: None,
                    dmg: None,
                    version: None,
                }))
            }
            Self::PollDownload(variant) => {
                let p = api.download_progress().map_err(UpdaterError::Backend)?;
                if p.percent > 100 {
                    return Err(UpdaterError::InvalidProgress);
                }
                let mut s = UpdateSnapshot::new(*variant);
                s.phase = if p.status == "error" {
                    UpdatePhase::Error
                } else if ["downloaded", "complete"].contains(&p.status.as_str()) {
                    UpdatePhase::Downloaded
                } else {
                    UpdatePhase::Downloading
                };
                s.percent = p.percent;
                s.message = p.message;
                s.error = p.error;
                Ok(Some(s))
            }
            Self::FetchDmgPath(variant) => {
                let dmg = api.dmg_path(*variant).map_err(UpdaterError::Backend)?;
                let raw = dmg.path.ok_or_else(|| {
                    UpdaterError::Backend("downloaded DMG path unavailable".into())
                })?;
                let path = PathBuf::from(&raw);
                if !path.is_absolute() || path.extension().is_none_or(|e| e != "dmg") {
                    return Err(UpdaterError::UnsafePath);
                }
                let mut s = UpdateSnapshot::new(*variant);
                s.phase = UpdatePhase::Downloaded;
                s.dmg = Some(path);
                s.version = dmg.version;
                Ok(Some(s))
            }
            Self::PollInstall => {
                let p = api.install_status().map_err(UpdaterError::Backend)?;
                Ok(p.map(|p| UpdateSnapshot {
                    phase: match p.phase.as_str() {
                        "complete" => UpdatePhase::Complete,
                        "error" => UpdatePhase::Error,
                        _ => UpdatePhase::Installing,
                    },
                    variant: UpdateVariant::Regular,
                    percent: p.percent.min(100),
                    message: p.message,
                    error: p.error,
                    dmg: p.dmg_path.map(PathBuf::from),
                    version: p.new_version,
                }))
            }
            Self::ClearStatus => {
                api.clear_install_status().map_err(UpdaterError::Backend)?;
                Ok(None)
            }
        }
    }
}

/// Provenance is a pinned digest for the signed release-manifest bytes delivered by the
/// trusted update metadata channel. Digest alone without this pinned provenance is rejected.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TrustedManifest {
    pub bytes_sha256: String,
    pub pinned_sha256: String,
    pub source: String,
}
impl TrustedManifest {
    pub fn validate(&self) -> Result<(), UpdaterError> {
        if self.bytes_sha256.len() != 64
            || self.pinned_sha256.len() != 64
            || !self.bytes_sha256.eq_ignore_ascii_case(&self.pinned_sha256)
            || !self.source.starts_with("https://")
            || self.source.contains('@')
        {
            return Err(UpdaterError::InvalidManifestProvenance);
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CandidateIdentity {
    pub path: PathBuf,
    pub team_id: String,
    pub version: String,
}
pub trait CommandExecutor {
    fn output(&self, program: &Path, args: &[String]) -> Result<String, String>;
    fn spawn(&self, program: &Path, args: &[String]) -> Result<(), String>;
    fn trusted_updater_script(&self, path: &Path) -> bool {
        path.starts_with("/Applications")
            || path.starts_with("/usr/local")
            || path.starts_with("/opt")
    }
}
pub struct SystemCommandExecutor;
impl CommandExecutor for SystemCommandExecutor {
    fn output(&self, p: &Path, a: &[String]) -> Result<String, String> {
        let o = Command::new(p)
            .args(a)
            .output()
            .map_err(|e| e.to_string())?;
        if !o.status.success() {
            return Err("verification command failed".into());
        }
        Ok(String::from_utf8_lossy(&o.stdout).to_string() + &String::from_utf8_lossy(&o.stderr))
    }
    fn spawn(&self, p: &Path, a: &[String]) -> Result<(), String> {
        Command::new(p)
            .args(a)
            .spawn()
            .map(|_| ())
            .map_err(|e| e.to_string())
    }
}
fn safe_candidate(path: &Path) -> Result<PathBuf, UpdaterError> {
    let canonical = path.canonicalize().map_err(|_| UpdaterError::UnsafePath)?;
    if canonical.starts_with("/Applications/MetalSharp.app")
        || canonical.starts_with("/Users")
        || !canonical.is_dir()
        || !canonical.extension().is_some_and(|e| e == "app")
    {
        return Err(UpdaterError::UnsafePath);
    }
    Ok(canonical)
}
/// Verify the candidate's actual macOS signature/team and bundle version before an explicit
/// restart/installer capability can run. Executor injection keeps unit fixtures side-effect-free.
pub fn verify_candidate(
    executor: &impl CommandExecutor,
    path: &Path,
    expected_team: &str,
    expected_version: &str,
) -> Result<CandidateIdentity, UpdaterError> {
    let path = safe_candidate(path)?;
    let codesign = Path::new("/usr/bin/codesign");
    let info = executor
        .output(
            codesign,
            &[
                "-dv".into(),
                "--verbose=4".into(),
                path.to_string_lossy().into_owned(),
            ],
        )
        .map_err(UpdaterError::Signature)?;
    let team = info
        .lines()
        .find_map(|l| l.strip_prefix("TeamIdentifier="))
        .ok_or_else(|| UpdaterError::Signature("missing team identifier".into()))?;
    let version = executor
        .output(
            Path::new("/usr/libexec/PlistBuddy"),
            &[
                "-c".into(),
                "Print :CFBundleShortVersionString".into(),
                path.join("Contents/Info.plist")
                    .to_string_lossy()
                    .into_owned(),
            ],
        )
        .map_err(UpdaterError::Signature)?
        .trim()
        .to_owned();
    if team != expected_team {
        return Err(UpdaterError::Signature("unexpected signing team".into()));
    }
    if version != expected_version {
        return Err(UpdaterError::VersionMismatch);
    }
    Ok(CandidateIdentity {
        path,
        team_id: team.into(),
        version,
    })
}
/// Capability represents explicit user authorization, accepted only after candidate and
/// manifest checks. Never silently kills the current app or launches on status receipt.
pub fn execute_checked_handoff(
    executor: &impl CommandExecutor,
    script: &Path,
    dmg: &Path,
    home: &Path,
    candidate: &CandidateIdentity,
    manifest: &TrustedManifest,
    backend_pid: u32,
    app_pid: u32,
) -> Result<(), UpdaterError> {
    manifest.validate()?;
    let dmg = dmg.canonicalize().map_err(|_| UpdaterError::UnsafePath)?;
    let home = home.canonicalize().map_err(|_| UpdaterError::UnsafePath)?;
    let status_file = home.join("update_install_status.json");
    let observed_digest = executor
        .output(
            Path::new("/usr/bin/shasum"),
            &[
                "-a".into(),
                "256".into(),
                dmg.to_string_lossy().into_owned(),
            ],
        )
        .map_err(UpdaterError::Executor)?;
    let observed_digest = observed_digest.split_whitespace().next().unwrap_or("");
    if !observed_digest.eq_ignore_ascii_case(&manifest.bytes_sha256) {
        return Err(UpdaterError::InvalidManifestProvenance);
    }
    if backend_pid == 0
        || app_pid == 0
        || !dmg.is_file()
        || dmg.extension().is_none_or(|e| e != "dmg")
        || !script.is_absolute()
        || !script.is_file()
    {
        return Err(UpdaterError::UnsafePath);
    }
    let script = script
        .canonicalize()
        .map_err(|_| UpdaterError::UnsafePath)?;
    if !executor.trusted_updater_script(&script) {
        return Err(UpdaterError::UnsafePath);
    }
    executor
        .spawn(
            Path::new("/bin/bash"),
            &[
                script.to_string_lossy().into_owned(),
                "--dmg".into(),
                dmg.to_string_lossy().into_owned(),
                "--backend-pid".into(),
                backend_pid.to_string(),
                "--target-version".into(),
                candidate.version.clone(),
                "--status-file".into(),
                status_file.to_string_lossy().into_owned(),
                "--metalsharp-home".into(),
                home.to_string_lossy().into_owned(),
                "--app-pid".into(),
                app_pid.to_string(),
            ],
        )
        .map_err(UpdaterError::Executor)
}

/// Explicit user-triggered relaunch after migration completion; signature and version are
/// rechecked immediately before execution. Only the app path is opened, never arbitrary text.
pub fn restart_verified_app(
    executor: &impl CommandExecutor,
    path: &Path,
    expected_team: &str,
    expected_version: &str,
) -> Result<(), UpdaterError> {
    let app = verify_candidate(executor, path, expected_team, expected_version)?;
    executor
        .spawn(
            Path::new("/usr/bin/open"),
            &["-a".into(), app.path.to_string_lossy().into_owned()],
        )
        .map_err(UpdaterError::Executor)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{cell::RefCell, fs};
    struct Api {
        requests: RefCell<Vec<String>>,
        progress: DownloadProgress,
    }
    impl UpdateApi for Api {
        fn start_download(&self, v: UpdateVariant) -> Result<(), String> {
            self.requests
                .borrow_mut()
                .push(format!("start:{}", v.as_str()));
            Ok(())
        }
        fn download_progress(&self) -> Result<DownloadProgress, String> {
            Ok(self.progress.clone())
        }
        fn dmg_path(&self, v: UpdateVariant) -> Result<DmgPath, String> {
            self.requests
                .borrow_mut()
                .push(format!("dmg:{}", v.as_str()));
            Ok(DmgPath {
                path: Some("/tmp/x.dmg".into()),
                version: Some("1.2".into()),
                ok: Some(true),
            })
        }
        fn install_status(&self) -> Result<Option<InstallStatus>, String> {
            Ok(Some(InstallStatus {
                phase: "complete".into(),
                percent: 100,
                message: "done".into(),
                error: None,
                new_version: Some("1.2".into()),
                dmg_path: None,
                timestamp: 1,
            }))
        }
        fn clear_install_status(&self) -> Result<(), String> {
            self.requests.borrow_mut().push("clear".into());
            Ok(())
        }
    }
    struct Exec {
        outputs: RefCell<Vec<String>>,
        spawned: RefCell<Vec<(PathBuf, Vec<String>)>>,
        allow_fixture_script: bool,
    }
    impl CommandExecutor for Exec {
        fn output(&self, _: &Path, _args: &[String]) -> Result<String, String> {
            Ok(self.outputs.borrow_mut().remove(0))
        }
        fn spawn(&self, p: &Path, a: &[String]) -> Result<(), String> {
            self.spawned.borrow_mut().push((p.into(), a.into()));
            Ok(())
        }
        fn trusted_updater_script(&self, path: &Path) -> bool {
            self.allow_fixture_script
                || path.starts_with("/Applications")
                || path.starts_with("/usr/local")
                || path.starts_with("/opt")
        }
    }
    #[test]
    fn backend_adapter_lifecycle_is_typed_and_variant_specific() {
        let a = Api {
            requests: RefCell::default(),
            progress: DownloadProgress {
                status: "downloaded".into(),
                percent: 100,
                message: "done".into(),
                error: None,
            },
        };
        let s = UpdateIntent::Start(UpdateVariant::Fex)
            .execute(&a)
            .unwrap()
            .unwrap();
        assert_eq!(s.phase, UpdatePhase::Downloading);
        assert_eq!(*a.requests.borrow(), vec!["clear", "start:fex"]);
        assert_eq!(
            UpdateIntent::PollDownload(UpdateVariant::Fex)
                .execute(&a)
                .unwrap()
                .unwrap()
                .phase,
            UpdatePhase::Downloaded
        );
        assert_eq!(
            UpdateIntent::FetchDmgPath(UpdateVariant::Fex)
                .execute(&a)
                .unwrap()
                .unwrap()
                .variant,
            UpdateVariant::Fex
        );
        assert_eq!(
            UpdateIntent::PollInstall
                .execute(&a)
                .unwrap()
                .unwrap()
                .phase,
            UpdatePhase::Complete
        );
    }
    #[test]
    fn trusted_manifest_and_handoff_reject_untrusted_inputs_without_process() {
        let exec = Exec {
            allow_fixture_script: true,
            outputs: RefCell::new(vec![
                "Executable=fixture\nTeamIdentifier=TEAM42\n".into(),
                "1.2".into(),
                format!("{}  fixture.dmg", "a".repeat(64)),
            ]),
            spawned: RefCell::default(),
        };
        let manifest = TrustedManifest {
            bytes_sha256: "a".repeat(64),
            pinned_sha256: "a".repeat(64),
            source: "https://updates.example/manifest".into(),
        };
        assert!(manifest.validate().is_ok());
        assert_eq!(
            TrustedManifest {
                pinned_sha256: "b".repeat(64),
                ..manifest.clone()
            }
            .validate(),
            Err(UpdaterError::InvalidManifestProvenance)
        );
        let dir =
            std::env::temp_dir().join(format!("gpui-update-candidate-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(dir.join("Fixture.app/Contents")).unwrap();
        fs::write(dir.join("Fixture.app/Contents/Info.plist"), "fixture").unwrap();
        let id = verify_candidate(&exec, &dir.join("Fixture.app"), "TEAM42", "1.2").unwrap();
        assert_eq!(id.team_id, "TEAM42");
        assert!(
            execute_checked_handoff(
                &exec,
                Path::new("/tmp/not-a-script"),
                Path::new("/tmp/missing.dmg"),
                &dir,
                &id,
                &manifest,
                1,
                2
            )
            .is_err()
        );
        assert!(exec.spawned.borrow().is_empty());
        let dmg = dir.join("candidate.dmg");
        let script = dir.join("update.sh");
        fs::write(&dmg, "fixture").unwrap();
        fs::write(&script, "fixture").unwrap();
        execute_checked_handoff(&exec, &script, &dmg, &dir, &id, &manifest, 22, 33).unwrap();
        let (program, args) = exec.spawned.borrow().last().unwrap().clone();
        assert_eq!(program, PathBuf::from("/bin/bash"));
        assert!(args.windows(2).any(|a| a == ["--backend-pid", "22"]));
        assert!(args.windows(2).any(|a| a == ["--target-version", "1.2"]));
        assert!(args.iter().any(|a| a == "--status-file"));
        assert!(args.iter().any(|a| a == "--metalsharp-home"));
        let _ = fs::remove_dir_all(dir);
    }
    #[test]
    fn install_status_file_reads_and_clears_only_its_fixture_home() {
        let home = std::env::temp_dir().join(format!("gpui-update-status-{}", std::process::id()));
        let _ = fs::remove_dir_all(&home);
        fs::create_dir_all(&home).unwrap();
        let store = InstallStatusFile::new(&home).unwrap();
        assert!(store.read().unwrap().is_none());
        fs::write(home.join("update_install_status.json"),r#"{"phase":"complete","percent":100,"message":"done","error":null,"new_version":"1.2","timestamp":5}"#).unwrap();
        assert_eq!(store.read().unwrap().unwrap().phase, "complete");
        store.clear().unwrap();
        assert!(!home.join("update_install_status.json").exists());
        let _ = fs::remove_dir_all(home);
    }
}
