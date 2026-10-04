//! Resource location and integrity primitives for the explicit connected app.
//! No implicit production-home fallback is provided by these APIs.
use serde::{Deserialize, Serialize};
use std::path::{Component, Path, PathBuf};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResourceError {
    InvalidRoot,
    Missing(String),
    UnsafePath(String),
    HashToolUnavailable,
    HashMismatch(String),
    Io(String),
}
impl std::fmt::Display for ResourceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for ResourceError {}

/// Resolve only an explicit app bundle's Resources directory or a supplied development root.
pub fn resource_root(
    bundle_resources: Option<&Path>,
    development_root: Option<&Path>,
) -> Result<PathBuf, ResourceError> {
    for candidate in [bundle_resources, development_root].into_iter().flatten() {
        if candidate.is_absolute() && candidate.is_dir() {
            return Ok(candidate.to_path_buf());
        }
    }
    Err(ResourceError::InvalidRoot)
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct FileDigest {
    pub path: String,
    pub sha256: String,
}

/// Check an explicit allow-list of packaged files. Relative paths cannot escape the root,
/// symlinks are rejected, and sha256 is obtained from the OS tool (not a weak ad-hoc hash).
pub fn verify_resources(root: &Path, manifest: &[FileDigest]) -> Result<(), ResourceError> {
    if !root.is_absolute() || !root.is_dir() {
        return Err(ResourceError::InvalidRoot);
    }
    for item in manifest {
        let rel = Path::new(&item.path);
        if rel.as_os_str().is_empty()
            || rel.components().any(|c| !matches!(c, Component::Normal(_)))
        {
            return Err(ResourceError::UnsafePath(item.path.clone()));
        }
        if item.sha256.len() != 64 || !item.sha256.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ResourceError::UnsafePath(item.path.clone()));
        }
        let full = root.join(rel);
        let mut ancestor = root.to_path_buf();
        for component in rel.components() {
            let Component::Normal(part) = component else {
                return Err(ResourceError::UnsafePath(item.path.clone()));
            };
            ancestor.push(part);
            let meta = std::fs::symlink_metadata(&ancestor)
                .map_err(|_| ResourceError::Missing(item.path.clone()))?;
            if meta.file_type().is_symlink() {
                return Err(ResourceError::UnsafePath(item.path.clone()));
            }
        }
        let meta = std::fs::symlink_metadata(&full)
            .map_err(|_| ResourceError::Missing(item.path.clone()))?;
        if !meta.is_file() {
            return Err(ResourceError::UnsafePath(item.path.clone()));
        }
        let output = std::process::Command::new("shasum")
            .arg("-a")
            .arg("256")
            .arg("--")
            .arg(&full)
            .output()
            .or_else(|_| {
                std::process::Command::new("sha256sum")
                    .arg("--")
                    .arg(&full)
                    .output()
            })
            .map_err(|_| ResourceError::HashToolUnavailable)?;
        if !output.status.success() {
            return Err(ResourceError::HashToolUnavailable);
        }
        let actual = String::from_utf8_lossy(&output.stdout)
            .split_whitespace()
            .next()
            .unwrap_or("")
            .to_ascii_lowercase();
        if actual != item.sha256.to_ascii_lowercase() {
            return Err(ResourceError::HashMismatch(item.path.clone()));
        }
    }
    Ok(())
}

/// Require a disposable, caller-owned root. Production data and installed app destinations
/// are always refused, even if the caller accidentally supplies them.
pub fn validate_candidate_root(root: &Path) -> Result<PathBuf, ResourceError> {
    if !root.is_absolute() {
        return Err(ResourceError::InvalidRoot);
    }
    let canonical = std::fs::canonicalize(root).map_err(|e| ResourceError::Io(e.to_string()))?;
    if canonical == Path::new("/Applications/MetalSharp.app")
        || canonical.starts_with(Path::new("/Applications/MetalSharp.app"))
        || canonical == Path::new("/Users")
        || canonical.starts_with(Path::new("/Users"))
    {
        return Err(ResourceError::InvalidRoot);
    }
    Ok(canonical)
}

/// Validate a not-yet-created candidate while also preventing the named installed app path.
pub fn validate_candidate_path(path: &Path) -> Result<PathBuf, ResourceError> {
    let parent = path.parent().ok_or(ResourceError::InvalidRoot)?;
    let canonical_parent = validate_candidate_root(parent)?;
    let name = path.file_name().ok_or(ResourceError::InvalidRoot)?;
    let canonical = canonical_parent.join(name);
    let installed = Path::new("/Applications/MetalSharp.app");
    if canonical == installed || canonical.starts_with(installed) {
        return Err(ResourceError::InvalidRoot);
    }
    Ok(canonical)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn resource_manifest_checks_hash_and_safe_relative_paths() {
        let root =
            std::env::temp_dir().join(format!("gpui-resource-fixture-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join("runtime.bin"), b"fixture").unwrap();
        let digest = std::process::Command::new("shasum")
            .args(["-a", "256", root.join("runtime.bin").to_str().unwrap()])
            .output()
            .or_else(|_| {
                std::process::Command::new("sha256sum")
                    .arg(root.join("runtime.bin"))
                    .output()
            })
            .unwrap();
        let hash = String::from_utf8_lossy(&digest.stdout)
            .split_whitespace()
            .next()
            .unwrap()
            .to_string();
        assert!(
            verify_resources(
                &root,
                &[FileDigest {
                    path: "runtime.bin".into(),
                    sha256: hash.clone()
                }]
            )
            .is_ok()
        );
        assert!(matches!(
            verify_resources(
                &root,
                &[FileDigest {
                    path: "../runtime.bin".into(),
                    sha256: hash.clone()
                }]
            ),
            Err(ResourceError::UnsafePath(_))
        ));
        assert!(matches!(
            verify_resources(
                &root,
                &[FileDigest {
                    path: "runtime.bin".into(),
                    sha256: "0".repeat(64)
                }]
            ),
            Err(ResourceError::HashMismatch(_))
        ));
        #[cfg(unix)]
        {
            use std::os::unix::fs::symlink;
            std::fs::create_dir_all(root.join("linked")).unwrap();
            std::fs::remove_dir(root.join("linked")).unwrap();
            symlink(std::env::temp_dir(), root.join("linked")).unwrap();
            assert!(matches!(
                verify_resources(
                    &root,
                    &[FileDigest {
                        path: "linked/outside".into(),
                        sha256: hash.clone()
                    }]
                ),
                Err(ResourceError::UnsafePath(_))
            ));
        }
        if Path::new("/Applications").is_dir() {
            assert!(matches!(
                validate_candidate_path(Path::new("/Applications/MetalSharp.app")),
                Err(ResourceError::InvalidRoot)
            ));
        }
        let _ = std::fs::remove_dir_all(root);
    }
}
