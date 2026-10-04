//! Narrow, typed OS capabilities for the GPUI host. No renderer-provided process IDs,
//! arbitrary URLs, or unowned filesystem paths cross this boundary.
use serde::{Deserialize, Serialize};
use std::{
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, SystemTime},
};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum DialogRequest {
    Directory { title: String },
    Executable { title: String },
    Image { title: String },
    Bios { title: String },
    EmulatorPackage { title: String },
    Game { title: String },
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HostError {
    Cancelled,
    InvalidRequest,
    OutsideOwnedRoots,
    NotFound,
    Io(String),
    Unsupported,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HostResult<T> {
    Completed(T),
    Cancelled,
}

/// Canonical roots are captured once by the app host. Callers cannot widen them per request.
#[derive(Clone, Debug)]
pub struct DesktopHost {
    roots: Vec<PathBuf>,
}
impl DesktopHost {
    pub fn new(roots: impl IntoIterator<Item = PathBuf>) -> Result<Self, HostError> {
        let mut canonical = Vec::new();
        for root in roots {
            canonical.push(root.canonicalize().map_err(|_| HostError::NotFound)?);
        }
        if canonical.is_empty() {
            return Err(HostError::InvalidRequest);
        }
        Ok(Self { roots: canonical })
    }
    pub fn owns(&self, path: &Path) -> bool {
        let Ok(path) = path.canonicalize() else {
            return false;
        };
        self.roots
            .iter()
            .any(|root| path == *root || path.starts_with(root))
    }
    pub fn validate_owned(&self, path: &Path) -> Result<PathBuf, HostError> {
        let canonical = path.canonicalize().map_err(|_| HostError::NotFound)?;
        if self
            .roots
            .iter()
            .any(|root| canonical == *root || canonical.starts_with(root))
        {
            Ok(canonical)
        } else {
            Err(HostError::OutsideOwnedRoots)
        }
    }
    /// Invoke a native picker. A selected path is returned only when it belongs to an
    /// explicitly owned root; cancellation is a normal result, never an empty path.
    pub fn pick(&self, request: DialogRequest) -> Result<HostResult<PathBuf>, HostError> {
        use rfd::FileDialog;
        let title = match &request {
            DialogRequest::Directory { title }
            | DialogRequest::Executable { title }
            | DialogRequest::Image { title }
            | DialogRequest::Bios { title }
            | DialogRequest::EmulatorPackage { title }
            | DialogRequest::Game { title } => title,
        };
        if title.trim().is_empty() || title.len() > 160 {
            return Err(HostError::InvalidRequest);
        }
        let dialog = FileDialog::new().set_title(title);
        let selected = match request {
            DialogRequest::Directory { .. } => dialog.pick_folder(),
            DialogRequest::Executable { .. } => dialog
                .add_filter("Windows executable", &["exe"])
                .pick_file(),
            DialogRequest::Image { .. } => dialog
                .add_filter("Image", &["png", "jpg", "jpeg", "webp"])
                .pick_file(),
            DialogRequest::Bios { .. } => dialog
                .add_filter("Firmware", &["bin", "rom", "iso"])
                .pick_file(),
            DialogRequest::EmulatorPackage { .. } => dialog
                .add_filter("Package", &["pkg", "zip", "7z"])
                .pick_file(),
            DialogRequest::Game { .. } => dialog
                .add_filter("Game image", &["iso", "chd", "cso", "bin", "cue"])
                .pick_file(),
        };
        match selected {
            None => Ok(HostResult::Cancelled),
            Some(path) => self.validate_owned(&path).map(HostResult::Completed),
        }
    }
    pub fn reveal(&self, path: &Path) -> Result<(), HostError> {
        let path = self.validate_owned(path)?;
        let status = Command::new("/usr/bin/open")
            .arg("-R")
            .arg(path)
            .status()
            .map_err(ioerr)?;
        if status.success() {
            Ok(())
        } else {
            Err(HostError::Io("Finder reveal failed".into()))
        }
    }
    /// Fixed external destinations only. This intentionally does not permit arbitrary renderer URLs.
    pub fn open_system_link(&self, link: SystemLink) -> Result<(), HostError> {
        let url = link.url();
        let status = Command::new("/usr/bin/open")
            .arg(url)
            .status()
            .map_err(ioerr)?;
        if status.success() {
            Ok(())
        } else {
            Err(HostError::Io("system link open failed".into()))
        }
    }
    pub fn copy_text(&self, text: &str) -> Result<(), HostError> {
        if text.len() > 1_000_000 || text.contains('\0') {
            return Err(HostError::InvalidRequest);
        }
        let mut child = Command::new("/usr/bin/pbcopy")
            .stdin(Stdio::piped())
            .spawn()
            .map_err(ioerr)?;
        use std::io::Write;
        child
            .stdin
            .take()
            .ok_or_else(|| HostError::Io("clipboard pipe unavailable".into()))?
            .write_all(text.as_bytes())
            .map_err(ioerr)?;
        if child.wait().map_err(ioerr)?.success() {
            Ok(())
        } else {
            Err(HostError::Io("clipboard write failed".into()))
        }
    }
    /// Deliver a native notification without interpolating untrusted text into a command line.
    /// macOS may apply its normal Notification Center permission policy.
    pub fn notify(&self, title: &str, message: &str) -> Result<(), HostError> {
        if title.trim().is_empty()
            || title.len() > 120
            || message.len() > 1000
            || title.chars().chain(message.chars()).any(char::is_control)
        {
            return Err(HostError::InvalidRequest);
        }
        #[cfg(target_os = "macos")]
        {
            fn applescript_string(value: &str) -> String {
                value.replace('\\', "\\\\").replace('"', "\\\"")
            }
            let script = format!(
                r#"display notification "{}" with title "{}""#,
                applescript_string(message),
                applescript_string(title)
            );
            let status = Command::new("/usr/bin/osascript")
                .args(["-e", &script])
                .status()
                .map_err(ioerr)?;
            if status.success() {
                Ok(())
            } else {
                Err(HostError::Io("notification was not delivered".into()))
            }
        }
        #[cfg(not(target_os = "macos"))]
        {
            let _ = (title, message);
            Err(HostError::Unsupported)
        }
    }
    /// Poll an owned directory root. Drop/cancel stops the worker; callbacks receive paths
    /// only after canonical ownership validation. No recursive traversal or symlink following.
    pub fn watch(
        &self,
        root: &Path,
        interval: Duration,
        callback: Arc<dyn Fn(WatchEvent) + Send + Sync>,
    ) -> Result<WatchHandle, HostError> {
        let root = self.validate_owned(root)?;
        if !root.is_dir() || interval < Duration::from_millis(100) {
            return Err(HostError::InvalidRequest);
        }
        let stop = Arc::new(AtomicBool::new(false));
        let thread_stop = stop.clone();
        let host = self.clone();
        let join = thread::spawn(move || {
            let mut previous = snapshot(&root);
            while !thread_stop.load(Ordering::Acquire) {
                thread::sleep(interval);
                if thread_stop.load(Ordering::Acquire) {
                    break;
                }
                let current = snapshot(&root);
                for p in current
                    .keys()
                    .filter(|p| previous.get(*p) != current.get(*p))
                {
                    if host.owns(p) {
                        callback(WatchEvent::Changed(p.clone()));
                    }
                }
                for p in previous.keys().filter(|p| !current.contains_key(*p)) {
                    // Removed entries cannot be canonicalized anymore; read_dir yielded
                    // a direct child name, so require its parent to be the pinned root.
                    if p.parent() == Some(root.as_path()) {
                        callback(WatchEvent::Removed(p.clone()));
                    }
                }
                previous = current;
            }
        });
        Ok(WatchHandle {
            stop,
            join: Some(join),
        })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SystemLink {
    SteamStore,
    TheGamesDb,
    Rpcs3Firmware,
    Pcsx2Guide,
    SunshineWebUi,
}
impl SystemLink {
    fn url(self) -> &'static str {
        match self {
            Self::SteamStore => "https://store.steampowered.com/",
            Self::TheGamesDb => "https://www.thegamesdb.net/",
            Self::Rpcs3Firmware => "https://rpcs3.net/",
            Self::Pcsx2Guide => "https://pcsx2.net/",
            Self::SunshineWebUi => "https://localhost:47990",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum WatchEvent {
    Changed(PathBuf),
    Removed(PathBuf),
}
/// Finite native-window actions; renderer data cannot supply a window identifier or URL.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AuxiliaryWindow {
    ProcessManager,
    LaunchOverlay,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HostShortcut {
    ToggleProcessManager,
    ConfirmedForceQuitOwnedGames,
}
impl HostShortcut {
    pub fn macos_accelerator(self) -> &'static str {
        match self {
            Self::ToggleProcessManager => "Command+P",
            Self::ConfirmedForceQuitOwnedGames => "Command+Option+Q",
        }
    }
}
/// Text-only launch status for the parent-owned native overlay; no HTML or code is accepted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LaunchOverlay {
    pub game_name: String,
}
impl LaunchOverlay {
    pub fn new(game_name: &str) -> Result<Self, HostError> {
        let name = game_name.trim();
        if name.is_empty() || name.len() > 180 || name.chars().any(char::is_control) {
            return Err(HostError::InvalidRequest);
        }
        Ok(Self {
            game_name: name.to_owned(),
        })
    }
}
pub struct WatchHandle {
    stop: Arc<AtomicBool>,
    join: Option<thread::JoinHandle<()>>,
}
impl WatchHandle {
    pub fn cancel(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}
impl Drop for WatchHandle {
    fn drop(&mut self) {
        self.cancel();
    }
}
fn snapshot(root: &Path) -> std::collections::BTreeMap<PathBuf, (Option<SystemTime>, u64)> {
    let mut out = std::collections::BTreeMap::new();
    if let Ok(entries) = std::fs::read_dir(root) {
        for e in entries.flatten() {
            let p = e.path();
            if let Ok(m) = std::fs::symlink_metadata(&p) {
                if !m.file_type().is_symlink() && m.is_file() {
                    out.insert(p, (m.modified().ok(), m.len()));
                }
            }
        }
    }
    out
}
fn ioerr(e: std::io::Error) -> HostError {
    HostError::Io(e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn canonical_ownership_rejects_sibling_and_symlink_escape() {
        let dir = std::env::temp_dir().join(format!("gpui-host-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let root = dir.join("owned");
        let outside = dir.join("outside");
        std::fs::create_dir_all(&root).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(outside.join("secret"), "x").unwrap();
        std::os::unix::fs::symlink(outside.join("secret"), root.join("link")).unwrap();
        let h = DesktopHost::new([root.clone()]).unwrap();
        assert!(h.owns(&root));
        assert!(!h.owns(&outside.join("secret")));
        assert_eq!(
            h.validate_owned(&root.join("link")),
            Err(HostError::OutsideOwnedRoots)
        );
        let _ = std::fs::remove_dir_all(dir);
    }
    #[test]
    fn requests_are_typed_and_bounds_checked() {
        let request = DialogRequest::Executable {
            title: "Choose app".into(),
        };
        let encoded = serde_json::to_string(&request).unwrap();
        assert_eq!(
            serde_json::from_str::<DialogRequest>(&encoded).unwrap(),
            request
        );
        assert!(
            serde_json::from_str::<DialogRequest>(r#"{"kind":"arbitrary","path":"/"}"#).is_err()
        );
    }
    #[test]
    fn notification_input_is_bounded_and_sunshine_link_is_fixed() {
        let dir = std::env::temp_dir().join(format!("gpui-host-empty-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let host = DesktopHost::new([dir.clone()]).unwrap();
        assert_eq!(host.notify(" ", "body"), Err(HostError::InvalidRequest));
        assert_eq!(
            host.notify("Title", "line\nInjected"),
            Err(HostError::InvalidRequest)
        );
        assert_eq!(SystemLink::SunshineWebUi.url(), "https://localhost:47990");
        let _ = std::fs::remove_dir_all(dir);
    }
    #[test]
    fn watcher_snapshot_ignores_directories_and_symlinks() {
        let dir = std::env::temp_dir().join(format!("gpui-snapshot-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let root = dir.join("root");
        let outside = dir.join("outside");
        std::fs::create_dir_all(root.join("subdir")).unwrap();
        std::fs::create_dir_all(&outside).unwrap();
        std::fs::write(root.join("item"), "x").unwrap();
        std::fs::write(outside.join("secret"), "x").unwrap();
        std::os::unix::fs::symlink(outside.join("secret"), root.join("link")).unwrap();
        let files = snapshot(&root);
        assert_eq!(files.len(), 1);
        assert!(files.contains_key(&root.join("item")));
        let _ = std::fs::remove_dir_all(dir);
    }
    #[test]
    fn auxiliary_actions_are_closed_and_overlay_is_plain_bounded_text() {
        assert_eq!(
            HostShortcut::ToggleProcessManager.macos_accelerator(),
            "Command+P"
        );
        assert_eq!(
            HostShortcut::ConfirmedForceQuitOwnedGames.macos_accelerator(),
            "Command+Option+Q"
        );
        assert_eq!(LaunchOverlay::new("Cuphead").unwrap().game_name, "Cuphead");
        assert_eq!(
            LaunchOverlay::new("<script>"),
            Ok(LaunchOverlay {
                game_name: "<script>".into()
            })
        );
        assert_eq!(LaunchOverlay::new("\n"), Err(HostError::InvalidRequest));
    }
}
