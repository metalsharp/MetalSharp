//! Game Settings > Advanced > Report a problem: a zip on the Desktop with what a bug report about a game needs,
//! which the user attaches to an issue themselves. Nothing is sent anywhere. Every text line goes through
//! `diagnostics::redact_line` first, so a credential-bearing line is replaced, as it is on the Logs page.
//!
//! The zip holds MetalSharp's logs of today and yesterday, the game's five newest launch logs (`logs/launches`: the
//! Mac, versions, Wine settings, Wine's errors and how each launch ended), the game's Mullion logs (`logs/dxmt/<appid>`
//! and `logs/fex-dxmt/<appid>`, where the backend points `MULLION_LOG_PATH`), the three newest crash reports, and
//! `system.txt`: the Mac, macOS, MetalSharp's version and build, and the game's route and settings.
use crate::diagnostics::redact_line;
use std::fs;
use std::io::{self, BufRead, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, SystemTime};

/// What the report says of the game.
pub struct GameFacts {
    pub name: String,
    pub appid: u64,
    pub source: String,
    pub pipeline: String,
    pub env: String,
    pub dll_overrides: String,
}

/// The most of one file that goes in: its end, where the problem is.
const MOST_BYTES: u64 = 4 << 20;

/// A name for files and folders: letters, digits and dashes.
pub fn slug(name: &str) -> String {
    let mut out = String::new();
    for c in name.chars() {
        if c.is_ascii_alphanumeric() {
            out.push(c);
        } else if !out.ends_with('-') && !out.is_empty() {
            out.push('-');
        }
    }
    let out = out.trim_end_matches('-');
    if out.is_empty() {
        "game".to_owned()
    } else {
        out.chars().take(48).collect()
    }
}

/// The lines of `from`'s last MOST_BYTES, redacted, into `to`.
fn copy_redacted(from: &Path, to: &Path) -> io::Result<()> {
    let mut file = fs::File::open(from)?;
    let length = file.metadata()?.len();
    if length > MOST_BYTES {
        use std::io::{Seek, SeekFrom};
        file.seek(SeekFrom::Start(length - MOST_BYTES))?;
    }
    let mut out = io::BufWriter::new(fs::File::create(to)?);
    if length > MOST_BYTES {
        writeln!(
            out,
            "[the first {} bytes are left out]",
            length - MOST_BYTES
        )?;
    }
    for line in io::BufReader::new(file).split(b'\n') {
        let line = String::from_utf8_lossy(&line?).into_owned();
        writeln!(out, "{}", redact_line(&line))?;
    }
    out.flush()
}

/// The newest `count` files of a folder (not its subfolders), newest first.
fn newest_files(folder: &Path, count: usize) -> Vec<PathBuf> {
    let mut files: Vec<(SystemTime, PathBuf)> = fs::read_dir(folder)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| {
            let meta = entry.metadata().ok()?;
            meta.is_file().then(|| {
                (
                    meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                    entry.path(),
                )
            })
        })
        .collect();
    files.sort_by(|a, b| b.0.cmp(&a.0));
    files
        .into_iter()
        .take(count)
        .map(|(_, path)| path)
        .collect()
}

/// What a command prints, trimmed; empty when it does not run.
fn output(program: &str, args: &[&str]) -> String {
    Command::new(program)
        .args(args)
        .output()
        .ok()
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn system_text(game: &GameFacts) -> String {
    let memory = output("/usr/sbin/sysctl", &["-n", "hw.memsize"])
        .parse::<u64>()
        .unwrap_or(0)
        >> 30;
    format!(
        "MetalSharp {} ({} build)\nmacOS {} ({})\nMac: {} ({}), {} GB\n\nGame: {}\nApp ID: {}\nSource: {}\nRoute: {}\n\
         Environment variables: {}\nDLL overrides: {}\n",
        env!("CARGO_PKG_VERSION"),
        if crate::variant::is_fex() {
            "Arm64 FEX"
        } else {
            "regular"
        },
        output("/usr/bin/sw_vers", &["-productVersion"]),
        output("/usr/bin/sw_vers", &["-buildVersion"]),
        output("/usr/sbin/sysctl", &["-n", "hw.model"]),
        output("/usr/sbin/sysctl", &["-n", "machdep.cpu.brand_string"]),
        memory,
        game.name,
        game.appid,
        if game.source.is_empty() {
            "steam"
        } else {
            &game.source
        },
        if game.pipeline.is_empty() {
            "default"
        } else {
            &game.pipeline
        },
        redact_line(if game.env.is_empty() {
            "(none)"
        } else {
            &game.env
        }),
        if game.dll_overrides.is_empty() {
            "(none)"
        } else {
            &game.dll_overrides
        },
    )
}

fn day_name(time: SystemTime) -> String {
    let seconds = time
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    output("/bin/date", &["-r", &seconds.to_string(), "+%Y-%m-%d"])
}

/// Makes the report and returns the zip's path: `~/Desktop/MetalSharp report - <game> - <time>.zip`. The folder it
/// is made from is `<home>/reports/<game>-<time>`, kept beside the zip's copy until the next report replaces it.
pub fn build(home: &Path, desktop: &Path, game: &GameFacts) -> io::Result<PathBuf> {
    let stamp = output("/bin/date", &["+%Y-%m-%d-%H%M%S"]);
    let name = format!("{}-{}", slug(&game.name), stamp);
    let reports = home.join("reports");
    // only the newest report's folder stays: older ones are in their zips
    if let Ok(entries) = fs::read_dir(&reports) {
        for entry in entries.flatten() {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
    let folder = reports.join(&name);
    fs::create_dir_all(folder.join("logs"))?;
    fs::write(folder.join("system.txt"), system_text(game))?;
    let logs = home.join("logs");
    let now = SystemTime::now();
    for day in [now, now - Duration::from_secs(24 * 3600)] {
        let file = logs.join(format!("{}.log", day_name(day)));
        if file.is_file() {
            copy_redacted(&file, &folder.join("logs").join(file.file_name().unwrap()))?;
        }
    }
    if game.appid != 0 {
        // the game's own launch logs (#834): `<date>_<time>-<appid>-<route>.log`, newest first
        let marker = format!("-{}-", game.appid);
        let launches: Vec<PathBuf> = newest_files(&logs.join("launches"), usize::MAX)
            .into_iter()
            .filter(|file| {
                file.file_name()
                    .and_then(|name| name.to_str())
                    .is_some_and(|name| name.contains(&marker) && name.ends_with(".log"))
            })
            .take(5)
            .collect();
        if !launches.is_empty() {
            fs::create_dir_all(folder.join("launches"))?;
            for file in launches {
                copy_redacted(
                    &file,
                    &folder.join("launches").join(file.file_name().unwrap()),
                )?;
            }
        }
        for lane in ["dxmt", "fex-dxmt"] {
            let source = logs.join(lane).join(game.appid.to_string());
            let files = newest_files(&source, 8);
            if files.is_empty() {
                continue;
            }
            let target = folder.join("mullion").join(lane);
            fs::create_dir_all(&target)?;
            for file in files {
                copy_redacted(&file, &target.join(file.file_name().unwrap()))?;
            }
        }
    }
    let crashes = newest_files(&logs.join("crash-reports"), 3);
    if !crashes.is_empty() {
        fs::create_dir_all(folder.join("crash-reports"))?;
        for file in crashes {
            copy_redacted(
                &file,
                &folder.join("crash-reports").join(file.file_name().unwrap()),
            )?;
        }
    }
    let zip = desktop.join(format!(
        "MetalSharp report - {} - {}.zip",
        slug(&game.name),
        stamp
    ));
    let status = Command::new("/usr/bin/ditto")
        .args(["-c", "-k", "--keepParent"])
        .arg(&folder)
        .arg(&zip)
        .status()?;
    if !status.success() {
        return Err(io::Error::other("ditto could not make the zip"));
    }
    Ok(zip)
}

/// Shows the zip in the Finder.
pub fn reveal(zip: &Path) {
    let _ = Command::new("/usr/bin/open").arg("-R").arg(zip).status();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn slug_keeps_letters_and_digits() {
        assert_eq!(slug("Counter-Strike 2"), "Counter-Strike-2");
        assert_eq!(slug("  ???  "), "game");
        assert_eq!(
            slug("Tom Clancy's The Division® 2"),
            "Tom-Clancy-s-The-Division-2"
        );
    }

    #[test]
    fn a_report_holds_redacted_logs_and_the_game() {
        let root =
            std::env::temp_dir().join(format!("metalsharp-report-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let home = root.join("home");
        let desktop = root.join("desktop");
        fs::create_dir_all(home.join("logs/fex-dxmt/730")).unwrap();
        fs::create_dir_all(&desktop).unwrap();
        fs::write(
            home.join(format!("logs/{}.log", day_name(SystemTime::now()))),
            "launch ok\nAuthorization: Bearer fixture-private\n",
        )
        .unwrap();
        fs::write(
            home.join("logs/fex-dxmt/730/cs2_d3d11.log"),
            "info: Mullion\n",
        )
        .unwrap();
        // the game's launch logs go in (#834); another game's (appid 7300) don't
        fs::create_dir_all(home.join("logs/launches")).unwrap();
        fs::write(
            home.join("logs/launches/2026-11-27_21-30-00-730-dxmt.log"),
            "Launch ended: exit code 0 after 5s\nAuthorization: Bearer launch-private\n",
        )
        .unwrap();
        fs::write(
            home.join("logs/launches/2026-11-27_21-31-00-7300-dxmt.log"),
            "another game\n",
        )
        .unwrap();
        let game = GameFacts {
            name: "Counter-Strike 2".into(),
            appid: 730,
            source: String::new(),
            pipeline: "dxmt".into(),
            env: "MULLION_LOSSY_TEXTURES=1".into(),
            dll_overrides: String::new(),
        };
        let zip = build(&home, &desktop, &game).unwrap();
        assert!(zip.is_file());
        let folder = fs::read_dir(home.join("reports"))
            .unwrap()
            .next()
            .unwrap()
            .unwrap()
            .path();
        let log =
            fs::read_to_string(folder.join(format!("logs/{}.log", day_name(SystemTime::now()))))
                .unwrap();
        assert!(log.contains("launch ok") && !log.contains("fixture-private"));
        assert!(folder.join("mullion/fex-dxmt/cs2_d3d11.log").is_file());
        let launch =
            fs::read_to_string(folder.join("launches/2026-11-27_21-30-00-730-dxmt.log")).unwrap();
        assert!(launch.contains("Launch ended") && !launch.contains("launch-private"));
        assert!(
            !folder
                .join("launches/2026-11-27_21-31-00-7300-dxmt.log")
                .exists()
        );
        let system = fs::read_to_string(folder.join("system.txt")).unwrap();
        assert!(
            system.contains("Counter-Strike 2")
                && system.contains("App ID: 730")
                && system.contains("MULLION_LOSSY")
        );
        let _ = fs::remove_dir_all(&root);
    }
}
