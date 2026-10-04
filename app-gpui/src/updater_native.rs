//! Candidate-only, reversible app-bundle transaction primitives. This module never mounts
//! DMGs, requests privilege, kills processes, opens applications, or chooses production paths.
use crate::resource_home::{
    FileDigest, ResourceError, validate_candidate_path, validate_candidate_root, verify_resources,
};
use std::{
    fs, io,
    path::{Component, Path, PathBuf},
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum UpdatePhase {
    Validating,
    Staging,
    ReadyToCommit,
    Committing,
    Complete,
    Cancelled,
    RolledBack,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UpdateProgress {
    pub phase: UpdatePhase,
    pub files_done: usize,
    pub files_total: usize,
    pub message: String,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum UpdateError {
    UnsafePath,
    InvalidManifest,
    Cancelled,
    Io(String),
    Integrity(ResourceError),
    Commit(String),
}
impl std::fmt::Display for UpdateError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for UpdateError {}
impl From<ResourceError> for UpdateError {
    fn from(e: ResourceError) -> Self {
        Self::Integrity(e)
    }
}

fn walk_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), UpdateError> {
    for entry in fs::read_dir(dir).map_err(ioerr)? {
        let entry = entry.map_err(ioerr)?;
        let p = entry.path();
        let kind = fs::symlink_metadata(&p).map_err(ioerr)?.file_type();
        if kind.is_symlink() {
            return Err(UpdateError::UnsafePath);
        } else if kind.is_dir() {
            walk_files(&p, out)?
        } else if kind.is_file() {
            out.push(p)
        } else {
            return Err(UpdateError::UnsafePath);
        }
    }
    Ok(())
}
fn ioerr(e: io::Error) -> UpdateError {
    UpdateError::Io(e.to_string())
}
fn copy_tree(
    src: &Path,
    dst: &Path,
    cancel: &dyn Fn() -> bool,
    progress: &mut dyn FnMut(UpdateProgress),
    done: &mut usize,
    total: usize,
) -> Result<(), UpdateError> {
    fs::create_dir_all(dst).map_err(ioerr)?;
    for ent in fs::read_dir(src).map_err(ioerr)? {
        if cancel() {
            return Err(UpdateError::Cancelled);
        }
        let ent = ent.map_err(ioerr)?;
        let s = ent.path();
        let d = dst.join(ent.file_name());
        let ty = fs::symlink_metadata(&s).map_err(ioerr)?.file_type();
        if ty.is_symlink() {
            return Err(UpdateError::UnsafePath);
        } else if ty.is_dir() {
            copy_tree(&s, &d, cancel, progress, done, total)?
        } else if ty.is_file() {
            fs::copy(&s, &d).map_err(ioerr)?;
            *done += 1;
            progress(UpdateProgress {
                phase: UpdatePhase::Staging,
                files_done: *done,
                files_total: total,
                message: "Staging verified application resources".into(),
            });
        } else {
            return Err(UpdateError::UnsafePath);
        }
    }
    Ok(())
}
fn validate_names(paths: &[PathBuf]) -> Result<(), UpdateError> {
    for p in paths {
        if p.components().any(|c| !matches!(c, Component::Normal(_))) {
            return Err(UpdateError::UnsafePath);
        }
    }
    Ok(())
}

/// Copy into a sibling staging directory, verify hashes, and leave the current app untouched.
/// A caller-supplied cancellation signal is polled between files; partial staging is removed.
pub fn stage_candidate(
    current: &Path,
    source: &Path,
    staging: &Path,
    manifest: &[FileDigest],
    cancel: &dyn Fn() -> bool,
    mut progress: impl FnMut(UpdateProgress),
) -> Result<(), UpdateError> {
    if manifest.is_empty() {
        return Err(UpdateError::InvalidManifest);
    }
    let parent = current.parent().ok_or(UpdateError::UnsafePath)?;
    let canonical = validate_candidate_root(parent)?;
    let current = validate_candidate_path(current).map_err(UpdateError::Integrity)?;
    if current.parent() != Some(canonical.as_path()) {
        return Err(UpdateError::UnsafePath);
    }
    let source = fs::canonicalize(source).map_err(ioerr)?;
    if source.starts_with(&current) {
        return Err(UpdateError::UnsafePath);
    }
    if staging.parent() != Some(parent) {
        return Err(UpdateError::UnsafePath);
    }
    let stage = canonical.join(staging.file_name().ok_or(UpdateError::UnsafePath)?);
    if stage == current || fs::symlink_metadata(&stage).is_ok() {
        return Err(UpdateError::UnsafePath);
    }
    let mut source_files = Vec::new();
    walk_files(&source, &mut source_files)?;
    let mut relative: Vec<PathBuf> = source_files
        .iter()
        .map(|p| p.strip_prefix(&source).unwrap().to_path_buf())
        .collect();
    validate_names(&relative)?;
    relative.sort();
    let mut listed = Vec::with_capacity(manifest.len());
    for item in manifest {
        let rel = PathBuf::from(&item.path);
        validate_names(std::slice::from_ref(&rel))?;
        listed.push(rel);
    }
    listed.sort();
    if listed.windows(2).any(|w| w[0] == w[1])
        || listed != relative.iter().cloned().collect::<Vec<_>>()
    {
        return Err(UpdateError::InvalidManifest);
    }
    let total = relative.len();
    progress(UpdateProgress {
        phase: UpdatePhase::Validating,
        files_done: 0,
        files_total: total,
        message: "Checking update resource manifest".into(),
    });
    // Verify the downloaded candidate before copying and the staged copy after.
    verify_resources(&source, manifest)?;
    let mut done = 0;
    if let Err(e) = copy_tree(&source, &stage, cancel, &mut progress, &mut done, total) {
        let _ = fs::remove_dir_all(&stage);
        return Err(e);
    }
    if cancel() {
        let _ = fs::remove_dir_all(&stage);
        return Err(UpdateError::Cancelled);
    }
    verify_resources(&stage, manifest)?;
    progress(UpdateProgress {
        phase: UpdatePhase::ReadyToCommit,
        files_done: done,
        files_total: total,
        message: "Candidate staged; existing installation unchanged".into(),
    });
    Ok(())
}

/// Atomic same-volume swap with backup restoration on failure. Data home is never touched.
pub fn commit_candidate(current: &Path, staging: &Path, backup: &Path) -> Result<(), UpdateError> {
    let parent = current.parent().ok_or(UpdateError::UnsafePath)?;
    if staging.parent() != Some(parent) || backup.parent() != Some(parent) {
        return Err(UpdateError::UnsafePath);
    }
    let canonical = validate_candidate_root(parent)?;
    let current_path = validate_candidate_path(current).map_err(UpdateError::Integrity)?;
    let staging_path = validate_candidate_path(staging).map_err(UpdateError::Integrity)?;
    let backup_path = validate_candidate_path(backup).map_err(UpdateError::Integrity)?;
    if [
        current_path.parent(),
        staging_path.parent(),
        backup_path.parent(),
    ]
    .iter()
    .any(|p| *p != Some(canonical.as_path()))
    {
        return Err(UpdateError::UnsafePath);
    }
    if current_path == staging_path || current_path == backup_path || staging_path == backup_path {
        return Err(UpdateError::UnsafePath);
    }
    let staging_meta = fs::symlink_metadata(&staging_path).map_err(ioerr)?;
    if !staging_meta.is_dir() || staging_meta.file_type().is_symlink() {
        return Err(UpdateError::UnsafePath);
    }
    if fs::symlink_metadata(&backup_path).is_ok() {
        return Err(UpdateError::Commit("backup path already exists".into()));
    }
    let had_current = current_path.exists();
    if had_current {
        fs::rename(&current_path, &backup_path).map_err(ioerr)?;
    }
    if let Err(e) = fs::rename(&staging_path, &current_path) {
        if had_current {
            let _ = fs::rename(&backup_path, &current_path);
        }
        return Err(UpdateError::Commit(e.to_string()));
    }
    Ok(())
}

pub fn rollback_candidate(current: &Path, backup: &Path) -> Result<(), UpdateError> {
    if current.parent() != backup.parent() {
        return Err(UpdateError::UnsafePath);
    }
    let parent = current.parent().ok_or(UpdateError::UnsafePath)?;
    let canonical = validate_candidate_root(parent)?;
    let current = validate_candidate_path(current).map_err(UpdateError::Integrity)?;
    let backup = validate_candidate_path(backup).map_err(UpdateError::Integrity)?;
    if current.parent() != Some(canonical.as_path()) || backup.parent() != Some(canonical.as_path())
    {
        return Err(UpdateError::UnsafePath);
    }
    if current == backup {
        return Err(UpdateError::UnsafePath);
    }
    let backup_meta = fs::symlink_metadata(&backup).map_err(ioerr)?;
    if !backup_meta.is_dir() || backup_meta.file_type().is_symlink() {
        return Err(UpdateError::UnsafePath);
    }
    let failed = current.with_extension("failed-update");
    if failed.exists() {
        return Err(UpdateError::Commit(
            "failed-update path already exists".into(),
        ));
    }
    if current.exists() {
        fs::rename(&current, &failed).map_err(ioerr)?;
    }
    if let Err(e) = fs::rename(&backup, &current) {
        if failed.exists() {
            let _ = fs::rename(&failed, &current);
        }
        return Err(UpdateError::Commit(e.to_string()));
    }
    let _ = fs::remove_dir_all(failed);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};
    fn temp() -> PathBuf {
        std::env::temp_dir().join(format!(
            "gpui-updater-fixture-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ))
    }
    fn hash(p: &Path) -> String {
        let o = std::process::Command::new("shasum")
            .args(["-a", "256", p.to_str().unwrap()])
            .output()
            .or_else(|_| std::process::Command::new("sha256sum").arg(p).output())
            .unwrap();
        String::from_utf8_lossy(&o.stdout)
            .split_whitespace()
            .next()
            .unwrap()
            .to_string()
    }
    #[test]
    fn stages_verifies_cancels_and_rolls_back_without_touching_data() {
        let root = temp();
        let current = root.join("Candidate.app");
        let source = root.join("source");
        let stage = root.join(".Candidate.stage");
        let backup = root.join(".Candidate.backup");
        fs::create_dir_all(&current).unwrap();
        fs::create_dir_all(source.join("Contents")).unwrap();
        fs::write(current.join("old"), b"old").unwrap();
        fs::write(source.join("Contents/new"), b"new app").unwrap();
        let data = root.join("metalsharp-home");
        fs::create_dir_all(&data).unwrap();
        fs::write(data.join("save"), b"save").unwrap();
        let m = [FileDigest {
            path: "Contents/new".into(),
            sha256: hash(&source.join("Contents/new")),
        }];
        let mut progress = Vec::new();
        stage_candidate(&current, &source, &stage, &m, &|| false, |p| {
            progress.push(p)
        })
        .unwrap();
        assert!(current.join("old").exists());
        assert!(
            progress
                .iter()
                .any(|p| p.phase == UpdatePhase::ReadyToCommit)
        );
        commit_candidate(&current, &stage, &backup).unwrap();
        assert!(current.join("Contents/new").exists());
        assert!(backup.join("old").exists());
        rollback_candidate(&current, &backup).unwrap();
        assert!(current.join("old").exists());
        assert_eq!(fs::read(data.join("save")).unwrap(), b"save");
        let canceled =
            stage_candidate(&current, &source, &stage, &m, &|| true, |_| {}).unwrap_err();
        assert_eq!(canceled, UpdateError::Cancelled);
        let _ = fs::remove_dir_all(root);
    }
    #[test]
    fn refuses_bad_integrity_manifest_and_non_sibling_transactions() {
        let root = temp();
        fs::create_dir_all(&root).unwrap();
        let current = root.join("app");
        let source = root.join("src");
        let stage = root.join("stage");
        fs::create_dir_all(&current).unwrap();
        fs::create_dir_all(&source).unwrap();
        fs::write(source.join("x"), b"x").unwrap();
        let good = [FileDigest {
            path: "x".into(),
            sha256: hash(&source.join("x")),
        }];
        let bad = [FileDigest {
            path: "x".into(),
            sha256: "0".repeat(64),
        }];
        assert!(matches!(
            stage_candidate(&current, &source, &stage, &bad, &|| false, |_| {}),
            Err(UpdateError::Integrity(ResourceError::HashMismatch(_)))
        ));
        fs::write(source.join("unlisted"), b"not authenticated").unwrap();
        assert_eq!(
            stage_candidate(&current, &source, &stage, &good, &|| false, |_| {}).unwrap_err(),
            UpdateError::InvalidManifest
        );
        fs::remove_file(source.join("unlisted")).unwrap();
        fs::create_dir(&stage).unwrap();
        fs::write(stage.join("keep"), b"owned elsewhere").unwrap();
        assert_eq!(
            stage_candidate(&current, &source, &stage, &good, &|| false, |_| {}).unwrap_err(),
            UpdateError::UnsafePath
        );
        assert_eq!(fs::read(stage.join("keep")).unwrap(), b"owned elsewhere");
        assert_eq!(
            commit_candidate(&current, &root.join("outside/stage"), &root.join("backup"))
                .unwrap_err(),
            UpdateError::UnsafePath
        );
        let _ = fs::remove_dir_all(root);
    }
}
