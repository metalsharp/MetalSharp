//! Where MetalSharp keeps its data: `~/.metalsharp` on this Mac, or a `MetalSharp` folder at the top of an external
//! drive that `~/.metalsharp` links to. Everything else (the backend, Wine, launch.env, the Wine wrappers) keeps using
//! `~/.metalsharp`, so the drive is a choice of where that folder lives, made before the backend starts.
//!
//! The drive's folder holds a marker with an id (`.metalsharp-data.json`) and this Mac remembers it
//! (`~/Library/Application Support/MetalSharp/data-location.json`), so the folder is found again when the drive
//! comes back under another name.
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

use anyhow::{Context, Result, bail};

/// The folder MetalSharp's data goes in at the top of a drive.
pub const DRIVE_FOLDER: &str = "MetalSharp";
const MARKER: &str = ".metalsharp-data.json";
/// What a drive needs free to be offered for a new install: the runtime, the Steam client's prefix and some caches.
pub const SETUP_MIN_FREE: u64 = 15 * 1024 * 1024 * 1024;
/// Room kept free on a drive data is moved to, over the data's own size.
const MOVE_MARGIN: u64 = 2 * 1024 * 1024 * 1024;

/// A drive that can hold MetalSharp's data: a local, writable APFS or Mac OS Extended volume under /Volumes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Drive {
    pub name: String,
    pub mount: PathBuf,
    pub free: u64,
    /// The drive already has a MetalSharp data folder (with its marker) from an earlier install.
    pub has_data: bool,
}

impl Drive {
    pub fn folder(&self) -> PathBuf {
        self.mount.join(DRIVE_FOLDER)
    }
}

/// Where the data folder is now.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Location {
    /// No data folder yet: a first launch.
    Missing,
    /// `~/.metalsharp` is a folder on this Mac.
    ThisMac,
    /// `~/.metalsharp` links to a folder on a drive that is connected.
    Drive { folder: PathBuf, name: String },
    /// `~/.metalsharp` links to a folder that is not there: the drive is not connected.
    Disconnected { folder: PathBuf, name: String },
}

impl Location {
    pub fn drive_name(&self) -> Option<&str> {
        match self {
            Location::Drive { name, .. } | Location::Disconnected { name, .. } => Some(name),
            _ => None,
        }
    }
}

pub fn location(home: &Path) -> Location {
    let Ok(meta) = std::fs::symlink_metadata(home) else {
        return Location::Missing;
    };
    if !meta.file_type().is_symlink() {
        return Location::ThisMac;
    }
    let Ok(target) = std::fs::read_link(home) else {
        return Location::Missing;
    };
    let folder = if target.is_absolute() {
        target
    } else {
        home.parent().unwrap_or(Path::new("/")).join(target)
    };
    let name = drive_name_of(&folder);
    if folder.is_dir() {
        Location::Drive { folder, name }
    } else {
        Location::Disconnected { folder, name }
    }
}

/// "AverySSD" for /Volumes/AverySSD/MetalSharp; the folder's own name otherwise.
fn drive_name_of(folder: &Path) -> String {
    let mut parts = folder
        .components()
        .skip_while(|c| c.as_os_str() != "Volumes");
    match (parts.next(), parts.next()) {
        (Some(_), Some(name)) => name.as_os_str().to_string_lossy().into_owned(),
        _ => folder
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
    }
}

/// The drives that can hold MetalSharp's data. Not the system's own volume, a network or read-only one, one Finder
/// hides, a Time Machine backup, nor a format without symlinks and Unix permissions (FAT, exFAT, NTFS), which Wine's
/// prefixes and the backend's socket need.
pub fn drives() -> Vec<Drive> {
    let mut list = Vec::new();
    let mut mounts: *mut libc::statfs = std::ptr::null_mut();
    let count = unsafe { libc::getmntinfo(&mut mounts, libc::MNT_NOWAIT) };
    if count <= 0 || mounts.is_null() {
        return list;
    }
    let mounts = unsafe { std::slice::from_raw_parts(mounts, count as usize) };
    for fs in mounts {
        let text = |chars: &[libc::c_char]| unsafe {
            std::ffi::CStr::from_ptr(chars.as_ptr())
                .to_string_lossy()
                .into_owned()
        };
        let kind = text(&fs.f_fstypename);
        let mount = PathBuf::from(text(&fs.f_mntonname));
        let flags = fs.f_flags as i32;
        if !matches!(kind.as_str(), "apfs" | "hfs")
            || flags & libc::MNT_LOCAL == 0
            || flags & (libc::MNT_RDONLY | libc::MNT_DONTBROWSE | libc::MNT_ROOTFS) != 0
            || mount.parent() != Some(Path::new("/Volumes"))
            || mount.join("Backups.backupdb").exists()
        {
            continue;
        }
        let Some(name) = mount.file_name().map(|n| n.to_string_lossy().into_owned()) else {
            continue;
        };
        if name.starts_with('.') {
            continue;
        }
        let free = fs.f_bavail as u64 * fs.f_bsize as u64;
        let has_data = read_marker(&mount.join(DRIVE_FOLDER)).is_some();
        list.push(Drive {
            name,
            mount,
            free,
            has_data,
        });
    }
    list.sort_by(|a, b| a.name.cmp(&b.name));
    list
}

/// The drives a first launch offers: those with room for an install, or MetalSharp data already on them.
pub fn setup_drives() -> Vec<Drive> {
    drives()
        .into_iter()
        .filter(|d| d.has_data || d.free >= SETUP_MIN_FREE)
        .collect()
}

fn read_marker(folder: &Path) -> Option<String> {
    let text = std::fs::read_to_string(folder.join(MARKER)).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    value.get("id")?.as_str().map(str::to_owned)
}

fn write_marker(folder: &Path, id: &str) -> Result<()> {
    let text = serde_json::to_string_pretty(&serde_json::json!({
        "id": id,
        "note": "MetalSharp's data folder. Its ~/.metalsharp links here.",
    }))?;
    std::fs::write(folder.join(MARKER), text).context("Write the data folder's marker")
}

fn new_id() -> String {
    let mut bytes = [0u8; 16];
    if std::fs::File::open("/dev/urandom")
        .and_then(|mut f| std::io::Read::read_exact(&mut f, &mut bytes))
        .is_err()
    {
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        bytes = (now ^ std::process::id() as u128).to_le_bytes();
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// This Mac's record of the drive folder's id: `~/Library/Application Support/MetalSharp/data-location.json`.
fn record_path(home: &Path) -> PathBuf {
    home.parent()
        .unwrap_or(Path::new("/"))
        .join("Library/Application Support/MetalSharp/data-location.json")
}

fn write_record(home: &Path, folder: &Path, id: &str) -> Result<()> {
    let path = record_path(home);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let text = serde_json::to_string_pretty(&serde_json::json!({
        "id": id,
        "folder": folder.to_string_lossy(),
    }))?;
    std::fs::write(path, text).context("Record the data folder's drive")
}

fn recorded_id(home: &Path) -> Option<String> {
    let text = std::fs::read_to_string(record_path(home)).ok()?;
    let value: serde_json::Value = serde_json::from_str(&text).ok()?;
    value.get("id")?.as_str().map(str::to_owned)
}

/// Point `~/.metalsharp` at `folder`, replacing the link that is there (never a real folder).
fn link(home: &Path, folder: &Path) -> Result<()> {
    if let Ok(meta) = std::fs::symlink_metadata(home) {
        if !meta.file_type().is_symlink() {
            bail!("{} is a folder, not a link", home.display());
        }
        std::fs::remove_file(home).context("Replace the data folder link")?;
    }
    std::os::unix::fs::symlink(folder, home).context("Link the data folder")
}

/// A first launch on `drive`: its MetalSharp folder (made now, or the one an earlier install left there) becomes
/// the data folder.
pub fn use_drive(home: &Path, drive: &Drive) -> Result<()> {
    let folder = drive.folder();
    let id = match read_marker(&folder) {
        Some(id) => id,
        None => {
            if folder.exists()
                && std::fs::read_dir(&folder)
                    .map(|mut e| e.next().is_some())
                    .unwrap_or(true)
            {
                bail!(
                    "{} already has a folder named {DRIVE_FOLDER} that is not MetalSharp's data",
                    drive.name
                );
            }
            std::fs::create_dir_all(&folder).with_context(|| {
                format!(
                    "Could not create {} — allow MetalSharp to use files on removable volumes in System Settings > Privacy & Security > Files and Folders",
                    folder.display()
                )
            })?;
            let id = new_id();
            write_marker(&folder, &id)?;
            id
        }
    };
    if location(home) == Location::ThisMac {
        bail!(
            "{} already exists on this Mac; move it from Settings instead",
            home.display()
        );
    }
    link(home, &folder)?;
    write_record(home, &folder, &id)
}

/// The data folder's drive, back under another name (or mount point): relinks `~/.metalsharp` to the connected
/// folder whose marker has the recorded id. Whether the data folder is there now.
pub fn reconnect(home: &Path) -> bool {
    match location(home) {
        Location::Drive { .. } => return true,
        Location::Disconnected { .. } => {}
        _ => return false,
    }
    let Some(id) = recorded_id(home) else {
        return false;
    };
    for drive in drives() {
        let folder = drive.folder();
        if read_marker(&folder).as_deref() == Some(id.as_str()) {
            return link(home, &folder).is_ok() && write_record(home, &folder, &id).is_ok();
        }
    }
    false
}

/// Where data is moved to.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Target {
    ThisMac,
    Drive(Drive),
}

/// Bytes on disk and entries (files, folders, links) of a tree, not following links. Sockets and other special
/// files are left out: a copy does not carry them.
fn tree_size(root: &Path) -> (u64, u64) {
    let mut bytes = 0;
    let mut entries = 0;
    let mut stack = vec![root.to_path_buf()];
    while let Some(path) = stack.pop() {
        let Ok(meta) = std::fs::symlink_metadata(&path) else {
            continue;
        };
        let kind = meta.file_type();
        if !(kind.is_file() || kind.is_dir() || kind.is_symlink()) {
            continue;
        }
        entries += 1;
        if kind.is_file() {
            bytes += meta.len();
        }
        if kind.is_dir() {
            if let Ok(children) = std::fs::read_dir(&path) {
                stack.extend(children.flatten().map(|c| c.path()));
            }
        }
    }
    (bytes, entries)
}

/// "12.3 GB"
pub fn format_size(bytes: u64) -> String {
    let gb = bytes as f64 / (1024.0 * 1024.0 * 1024.0);
    if gb >= 1.0 {
        format!("{gb:.1} GB")
    } else {
        format!("{:.0} MB", bytes as f64 / (1024.0 * 1024.0))
    }
}

fn free_bytes(path: &Path) -> Option<u64> {
    let c = std::ffi::CString::new(path.as_os_str().as_encoded_bytes()).ok()?;
    let mut fs: libc::statfs = unsafe { std::mem::zeroed() };
    (unsafe { libc::statfs(c.as_ptr(), &mut fs) } == 0)
        .then(|| fs.f_bavail as u64 * fs.f_bsize as u64)
}

fn same_volume(a: &Path, b: &Path) -> bool {
    use std::os::unix::fs::MetadataExt;
    matches!((std::fs::metadata(a), std::fs::metadata(b)), (Ok(a), Ok(b)) if a.dev() == b.dev())
}

/// The data folder's size in bytes, for the move's confirmation.
pub fn data_size(home: &Path) -> u64 {
    std::fs::canonicalize(home)
        .map(|folder| tree_size(&folder).0)
        .unwrap_or(0)
}

/// Move the data folder to `target`. MetalSharp's Wine processes and the backend must be stopped first. Copies with
/// ditto (which keeps the prefixes' symlinks, permissions and the read-only MetalSharp Wine copy as they are), checks
/// the copy has every file and byte, points `~/.metalsharp` at it, and only then deletes the original. `progress`
/// gets (bytes copied, bytes in all) while it copies.
pub fn move_data(home: &Path, target: &Target, progress: &dyn Fn(u64, u64)) -> Result<()> {
    let user = home.parent().context("The data folder has no parent")?;
    let source = std::fs::canonicalize(home).context("Find the data folder")?;
    let destination = match target {
        Target::ThisMac => {
            if location(home) == Location::ThisMac {
                bail!("MetalSharp's data is already on this Mac");
            }
            user.join(".metalsharp.moving")
        }
        Target::Drive(drive) => {
            if same_volume(&source, &drive.mount) {
                bail!("MetalSharp's data is already on {}", drive.name);
            }
            let folder = drive.folder();
            if folder.exists() {
                bail!(
                    "{} already has a folder named {DRIVE_FOLDER}; move or rename it first",
                    drive.name
                );
            }
            folder
        }
    };
    if destination.exists() {
        // a move that stopped part way left its copy (only ever .metalsharp.moving here)
        crate::host_actions::remove_path(&destination);
    }
    let (total, entries) = tree_size(&source);
    let volume = destination
        .parent()
        .context("The destination has no parent")?;
    let free = free_bytes(volume).context("Read the destination's free space")?;
    if free < total + MOVE_MARGIN {
        bail!(
            "Not enough space: the data needs {}, and {} is free",
            format_size(total + MOVE_MARGIN),
            format_size(free)
        );
    }
    progress(0, total);
    let mut ditto = Command::new("/usr/bin/ditto")
        .arg(&source)
        .arg(&destination)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .context("Start ditto")?;
    let status = loop {
        if let Some(status) = ditto.try_wait()? {
            break status;
        }
        let copied = free_bytes(volume).map_or(0, |now| free.saturating_sub(now));
        progress(copied.min(total), total);
        std::thread::sleep(Duration::from_millis(500));
    };
    if !status.success() {
        let mut detail = String::new();
        if let Some(mut stderr) = ditto.stderr.take() {
            let _ = std::io::Read::read_to_string(&mut stderr, &mut detail);
        }
        crate::host_actions::remove_path(&destination);
        bail!("The copy failed: {}", detail.trim());
    }
    progress(total, total);
    let (copied, copied_entries) = tree_size(&destination);
    if copied != total || copied_entries != entries {
        crate::host_actions::remove_path(&destination);
        bail!(
            "The copy is incomplete ({copied_entries} of {entries} items, {copied} of {total} bytes); nothing was changed"
        );
    }
    match target {
        Target::Drive(_) => {
            let id = read_marker(&destination).unwrap_or_else(new_id);
            write_marker(&destination, &id)?;
            // ~/.metalsharp, a folder or a link, makes way for the link; a folder is kept until the link is in
            let kept = user.join(format!(".metalsharp.moved-{}", std::process::id()));
            let was_folder = location(home) == Location::ThisMac;
            if was_folder {
                std::fs::rename(home, &kept).context("Set the old data folder aside")?;
            }
            if let Err(error) = link(home, &destination) {
                if was_folder {
                    let _ = std::fs::rename(&kept, home);
                }
                crate::host_actions::remove_path(&destination);
                return Err(error);
            }
            write_record(home, &destination, &id)?;
            crate::host_actions::remove_path(if was_folder { &kept } else { &source });
        }
        Target::ThisMac => {
            let _ = std::fs::remove_file(destination.join(MARKER));
            std::fs::remove_file(home).context("Remove the data folder link")?;
            if let Err(error) = std::fs::rename(&destination, home) {
                let _ = std::os::unix::fs::symlink(&source, home);
                return Err(error).context("Put the data folder in place");
            }
            let _ = std::fs::remove_file(record_path(home));
            crate::host_actions::remove_path(&source);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> PathBuf {
        let base = std::env::temp_dir().join(format!(
            "metalsharp-data-location-{name}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&base);
        std::fs::create_dir_all(base.join("Users/fixture")).unwrap();
        std::fs::canonicalize(&base).unwrap()
    }

    #[test]
    fn locations() {
        let base = scratch("locations");
        let home = base.join("Users/fixture/.metalsharp");
        assert_eq!(location(&home), Location::Missing);
        std::fs::create_dir(&home).unwrap();
        assert_eq!(location(&home), Location::ThisMac);
        std::fs::remove_dir(&home).unwrap();
        let folder = base.join("Volumes/Games/MetalSharp");
        std::os::unix::fs::symlink(&folder, &home).unwrap();
        assert_eq!(
            location(&home),
            Location::Disconnected {
                folder: folder.clone(),
                name: "Games".into()
            }
        );
        std::fs::create_dir_all(&folder).unwrap();
        assert_eq!(
            location(&home),
            Location::Drive {
                folder,
                name: "Games".into()
            }
        );
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn first_launch_on_a_drive_and_its_return_under_another_name() {
        let base = scratch("drive");
        let home = base.join("Users/fixture/.metalsharp");
        let mount = base.join("Volumes/Games");
        std::fs::create_dir_all(&mount).unwrap();
        let drive = Drive {
            name: "Games".into(),
            mount: mount.clone(),
            free: SETUP_MIN_FREE,
            has_data: false,
        };
        use_drive(&home, &drive).unwrap();
        assert!(matches!(location(&home), Location::Drive { .. }));
        let id = read_marker(&drive.folder()).unwrap();
        assert_eq!(recorded_id(&home).as_deref(), Some(id.as_str()));
        std::fs::write(home.join("setup.json"), "{}").unwrap();
        assert!(drive.folder().join("setup.json").is_file());
        // a second first launch reuses the folder and its id
        std::fs::remove_file(&home).unwrap();
        use_drive(&home, &drive).unwrap();
        assert_eq!(read_marker(&drive.folder()).as_deref(), Some(id.as_str()));
        // the drive's folder going away leaves the link disconnected
        std::fs::rename(&mount, base.join("Volumes/Games 1")).unwrap();
        assert!(matches!(location(&home), Location::Disconnected { .. }));
        // a folder named MetalSharp that is not MetalSharp's data is not taken
        let other = base.join("Volumes/Other");
        std::fs::create_dir_all(other.join(DRIVE_FOLDER)).unwrap();
        std::fs::write(other.join(DRIVE_FOLDER).join("notes.txt"), "mine").unwrap();
        std::fs::remove_file(&home).unwrap();
        assert!(
            use_drive(
                &home,
                &Drive {
                    name: "Other".into(),
                    mount: other,
                    free: SETUP_MIN_FREE,
                    has_data: false
                }
            )
            .is_err()
        );
        // nor is a data folder that is a real folder on this Mac replaced by a link
        std::fs::create_dir(&home).unwrap();
        assert!(use_drive(&home, &drive).is_err());
        assert_eq!(location(&home), Location::ThisMac);
        let _ = std::fs::remove_dir_all(&base);
    }

    // a data folder on a drive comes back to this Mac with its links and read-only parts, and the drive's copy goes
    #[test]
    fn move_back_to_this_mac() {
        use std::os::unix::fs::PermissionsExt;
        let base = scratch("move");
        let home = base.join("Users/fixture/.metalsharp");
        let mount = base.join("Volumes/Games");
        std::fs::create_dir_all(&mount).unwrap();
        let drive = Drive {
            name: "Games".into(),
            mount,
            free: SETUP_MIN_FREE,
            has_data: false,
        };
        use_drive(&home, &drive).unwrap();
        std::fs::create_dir_all(home.join("prefix-steam/dosdevices")).unwrap();
        std::fs::create_dir_all(home.join("prefix-steam/drive_c/users")).unwrap();
        std::os::unix::fs::symlink("../drive_c", home.join("prefix-steam/dosdevices/c:")).unwrap();
        std::fs::write(home.join("setup.json"), "{\"completed\":true}").unwrap();
        let sealed = home.join("runtime/MetalSharp-Wine.app/Contents");
        std::fs::create_dir_all(&sealed).unwrap();
        std::fs::write(sealed.join("Info.plist"), "plist").unwrap();
        std::fs::set_permissions(&sealed, std::fs::Permissions::from_mode(0o555)).unwrap();
        let seen = std::cell::Cell::new(0u64);
        move_data(&home, &Target::ThisMac, &|copied, _| seen.set(copied)).unwrap();
        assert_eq!(location(&home), Location::ThisMac);
        assert_eq!(
            std::fs::read_to_string(home.join("setup.json")).unwrap(),
            "{\"completed\":true}"
        );
        assert_eq!(
            std::fs::read_link(home.join("prefix-steam/dosdevices/c:")).unwrap(),
            Path::new("../drive_c")
        );
        assert!(
            home.join("runtime/MetalSharp-Wine.app/Contents/Info.plist")
                .is_file()
        );
        assert!(!home.join(MARKER).exists());
        assert!(!drive.folder().exists(), "the drive's copy is deleted");
        assert!(recorded_id(&home).is_none());
        assert!(seen.get() > 0);
        assert!(move_data(&home, &Target::ThisMac, &|_, _| {}).is_err());
        crate::host_actions::remove_path(&base);
    }

    /// The move across drives, on a real one: METALSHARP_TEST_DRIVE=/Volumes/<Drive> cargo test -- --ignored
    /// data_location. A data folder on this Mac goes to the drive's MetalSharp folder and comes back.
    #[test]
    #[ignore]
    fn move_to_a_real_drive_and_back() {
        use std::os::unix::fs::PermissionsExt;
        let Some(mount) = std::env::var_os("METALSHARP_TEST_DRIVE").map(PathBuf::from) else {
            return;
        };
        let drive = drives()
            .into_iter()
            .find(|d| d.mount == mount)
            .expect("METALSHARP_TEST_DRIVE is not a drive MetalSharp can use");
        assert!(
            !drive.folder().exists(),
            "the drive already has a MetalSharp folder"
        );
        let base = scratch("real-drive");
        let home = base.join("Users/fixture/.metalsharp");
        std::fs::create_dir_all(home.join("prefix-steam/dosdevices")).unwrap();
        std::fs::create_dir_all(home.join("prefix-steam/drive_c/windows")).unwrap();
        std::os::unix::fs::symlink("../drive_c", home.join("prefix-steam/dosdevices/c:")).unwrap();
        std::os::unix::fs::symlink("/", home.join("prefix-steam/dosdevices/z:")).unwrap();
        std::fs::write(
            home.join("prefix-steam/drive_c/windows/win.ini"),
            vec![b'x'; 3 << 20],
        )
        .unwrap();
        let sealed = home.join("runtime/MetalSharp-Wine.app/Contents");
        std::fs::create_dir_all(&sealed).unwrap();
        std::fs::write(sealed.join("Info.plist"), "plist").unwrap();
        std::fs::set_permissions(&sealed, std::fs::Permissions::from_mode(0o555)).unwrap();
        let (bytes, entries) = tree_size(&home);

        move_data(&home, &Target::Drive(drive.clone()), &|_, _| {}).unwrap();
        assert_eq!(
            location(&home),
            Location::Drive {
                folder: drive.folder(),
                name: drive.name.clone()
            }
        );
        assert_eq!(read_marker(&drive.folder()), recorded_id(&home));
        assert!(!base.join("Users/fixture/.metalsharp.moved").exists());
        assert_eq!(
            std::fs::read_link(home.join("prefix-steam/dosdevices/z:")).unwrap(),
            Path::new("/")
        );
        let (moved_bytes, moved_entries) = tree_size(&drive.folder());
        assert_eq!(
            (moved_bytes, moved_entries),
            (
                bytes
                    + std::fs::metadata(drive.folder().join(MARKER))
                        .unwrap()
                        .len(),
                entries + 1
            )
        );
        // the data folder reached through the link, as the backend does
        assert!(home.join("prefix-steam/drive_c/windows/win.ini").is_file());

        move_data(&home, &Target::ThisMac, &|_, _| {}).unwrap();
        assert_eq!(location(&home), Location::ThisMac);
        assert!(!drive.folder().exists(), "the drive's copy is deleted");
        assert_eq!(tree_size(&home), (bytes, entries));
        crate::host_actions::remove_path(&base);
    }

    #[test]
    fn drive_names() {
        assert_eq!(
            drive_name_of(Path::new("/Volumes/AverySSD/MetalSharp")),
            "AverySSD"
        );
        assert_eq!(
            drive_name_of(Path::new("/Volumes/My Games/MetalSharp")),
            "My Games"
        );
    }

    #[test]
    fn drives_are_local_writable_mac_volumes() {
        // whatever is connected: none is the system volume, and each has its folder at its top
        for drive in drives() {
            assert!(drive.mount.starts_with("/Volumes/"), "{:?}", drive.mount);
            assert_eq!(drive.folder(), drive.mount.join("MetalSharp"));
        }
    }
}
