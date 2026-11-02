//! Quitting MetalSharp ends its Wine processes too: the Wine Steam client, games, wineserver and their helpers
//! (x86_64 Wine and the FEX build's MetalSharp Wine alike), and the "MetalSharp Steam" helper app. Left running,
//! they keep MetalSharp's processes alive after the app has gone, so a closed MetalSharp had to be force quit.
//!
//! Processes are told apart by their executable, not their command line: Wine shows a game's Windows command line
//! (`C:\...\steam.exe`) in place of its own.
use std::path::{Path, PathBuf};

/// The places whose processes are MetalSharp's Wine: the data folder's Wine runtimes (only their Wine
/// executables, as the data folder also holds other tools), and the app bundle's MetalSharp Wine and Steam helper.
pub struct WineRoots {
    data: Option<PathBuf>,
    bundled: Vec<PathBuf>,
}

impl WineRoots {
    pub fn new(home: &Path, resources: &Path) -> Self {
        let bundled = ["MetalSharp-Wine.app", "MetalSharp Steam.app"]
            .iter()
            .filter_map(|name| resources.join(name).canonicalize().ok())
            .collect();
        Self {
            data: home.canonicalize().ok(),
            bundled,
        }
    }

    /// Whether a process running `executable` (a resolved path, as the kernel reports it) is one of them.
    pub fn owns(&self, executable: &Path) -> bool {
        if self.bundled.iter().any(|root| executable.starts_with(root)) {
            return true;
        }
        let Some(data) = &self.data else { return false };
        let Ok(inside) = executable.strip_prefix(data) else {
            return false;
        };
        is_wine_executable(inside)
    }
}

/// A Wine executable within the data folder: one in a MetalSharp Wine app copy, under a Wine's `lib/wine` or
/// `bin`, or named as Wine's are (wine, wine64, wine-preloader, wineserver, MetalSharp's wine launchers).
fn is_wine_executable(relative: &Path) -> bool {
    let text = relative.to_string_lossy();
    let name = relative
        .file_name()
        .map(|name| name.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    text.contains("MetalSharp-Wine.app/")
        || text.contains("/lib/wine/")
        || text.contains("/wine/bin/")
        || name.starts_with("wine")
        || name.contains("-wine")
}

/// SIGKILL every running process the roots own, other than this one; returns how many were signalled.
pub fn kill_all(roots: &WineRoots) -> usize {
    let own = std::process::id() as libc::pid_t;
    pids()
        .into_iter()
        .filter(|&pid| pid > 1 && pid != own)
        .filter(|&pid| executable(pid).is_some_and(|path| roots.owns(&path)))
        .filter(|&pid| unsafe { libc::kill(pid, libc::SIGKILL) } == 0)
        .count()
}

fn pids() -> Vec<libc::pid_t> {
    let count = unsafe { libc::proc_listallpids(std::ptr::null_mut(), 0) };
    if count <= 0 {
        return Vec::new();
    }
    // room for processes started since the count
    let mut pids = vec![0 as libc::pid_t; count as usize + 64];
    let bytes = (pids.len() * std::mem::size_of::<libc::pid_t>()) as libc::c_int;
    let found = unsafe { libc::proc_listallpids(pids.as_mut_ptr().cast(), bytes) };
    pids.truncate(found.max(0) as usize);
    pids
}

fn executable(pid: libc::pid_t) -> Option<PathBuf> {
    let mut buffer = vec![0u8; libc::PROC_PIDPATHINFO_MAXSIZE as usize];
    let length =
        unsafe { libc::proc_pidpath(pid, buffer.as_mut_ptr().cast(), buffer.len() as u32) };
    if length <= 0 {
        return None;
    }
    buffer.truncate(length as usize);
    Some(PathBuf::from(String::from_utf8_lossy(&buffer).into_owned()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roots() -> WineRoots {
        WineRoots {
            data: Some(PathBuf::from("/Users/fixture/.metalsharp")),
            bundled: vec![
                PathBuf::from(
                    "/Applications/MetalSharp.app/Contents/Resources/MetalSharp-Wine.app",
                ),
                PathBuf::from(
                    "/Applications/MetalSharp.app/Contents/Resources/MetalSharp Steam.app",
                ),
            ],
        }
    }

    #[test]
    fn owns_metalsharp_wine_processes() {
        let roots = roots();
        for path in [
            // x86_64 build
            "/Users/fixture/.metalsharp/runtime/wine/bin/wine64-preloader",
            "/Users/fixture/.metalsharp/runtime/wine/bin/wineserver",
            "/Users/fixture/.metalsharp/runtime/wine/lib/wine/x86_64-unix/wine",
            // FEX build: the runtime copy of MetalSharp Wine, its wrappers and game launcher
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/MacOS/wine",
            "/Users/fixture/.metalsharp/runtime/MetalSharp-Wine.app/Contents/lib/wine/aarch64-unix/wineserver",
            "/Users/fixture/.metalsharp/cache/metalsharp-wine-game-launcher",
            // the app bundle's own copies
            "/Applications/MetalSharp.app/Contents/Resources/MetalSharp-Wine.app/Contents/MacOS/wine",
            "/Applications/MetalSharp.app/Contents/Resources/MetalSharp Steam.app/Contents/MacOS/MetalSharp Steam",
        ] {
            assert!(roots.owns(Path::new(path)), "{path}");
        }
    }

    // a process running a Wine executable of a data folder ends, and one running the same program elsewhere does not
    #[test]
    fn kills_running_wine_of_the_data_folder() {
        let base =
            std::env::temp_dir().join(format!("metalsharp-wine-shutdown-{}", std::process::id()));
        let home = base.join("home");
        let wine = home.join("runtime/wine/bin/wineserver");
        let other = base.join("elsewhere/wineserver");
        for path in [&wine, &other] {
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::copy("/bin/sleep", path).unwrap();
        }
        let mut owned = std::process::Command::new(&wine).arg("30").spawn().unwrap();
        let mut unowned = std::process::Command::new(&other)
            .arg("30")
            .spawn()
            .unwrap();
        std::thread::sleep(std::time::Duration::from_millis(200));
        let roots = WineRoots::new(&home, &base.join("no-resources"));
        assert!(kill_all(&roots) >= 1);
        let status = owned.wait().unwrap();
        assert!(
            !status.success(),
            "the data folder's wineserver was not killed"
        );
        assert!(
            matches!(unowned.try_wait(), Ok(None)),
            "a wineserver elsewhere was killed"
        );
        let _ = unowned.kill();
        let _ = unowned.wait();
        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn leaves_other_processes() {
        let roots = roots();
        for path in [
            // MetalSharp itself and its backend
            "/Applications/MetalSharp.app/Contents/MacOS/MetalSharp",
            "/Applications/MetalSharp.app/Contents/Resources/runtime/metalsharp-backend",
            // tools the data folder holds that are not Wine
            "/Users/fixture/.metalsharp/tools/rpcs3/RPCS3.app/Contents/MacOS/rpcs3",
            "/Users/fixture/.metalsharp/tools/steamcmd/steamcmd",
            // Wine that is not MetalSharp's, and the Mac Steam client
            "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine64-preloader",
            "/Applications/Steam.app/Contents/MacOS/steam_osx",
            "/Users/fixture/.metalsharp-other/runtime/wine/bin/wine64-preloader",
        ] {
            assert!(!roots.owns(Path::new(path)), "{path}");
        }
    }
}
