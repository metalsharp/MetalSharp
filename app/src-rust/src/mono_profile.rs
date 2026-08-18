//! Mono / FNA / XNA / Unity-Mono profile discovery (Phase 1).
//!
//! Classifies a game directory into a rich, evidence-backed mono profile so
//! the FNA/Mono route can deploy the correct version-matched runtime and
//! launch the right managed entry. Grounded in real installs:
//!
//! * Unity-Mono games (DREDGE, Rain World, PlateUp) ship `UnityPlayer.dll` +
//!   `MonoBleedingEdge/` + `<Game>_Data/Managed/`; the Unity version is
//!   readable from the `globalgamemanagers` header (verified at offset 48:
//!   `2021.3.5f1\0`).
//! * Unity IL2CPP games ship `GameAssembly.dll` (native) — NOT mono-runnable;
//!   they belong on the Wine/DXMT lane.
//! * Classic FNA games (Necesse) ship a root `FNA.dll` and/or
//!   `Microsoft.Xna.Framework.*.dll` names.
//! * MonoKickstart bundles ship `<exe>.bin.osx` / `osx/libmonosgen-2.0.1.dylib`.
//!
//! The detector is deliberately cheap (file presence + a bounded header read);
//! it never spawns processes and never parses beyond 128 bytes of
//! `globalgamemanagers`.

use serde::Serialize;
use std::fs;
use std::path::Path;

/// The mono-adjacent family a game dir belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MonoProfileKind {
    /// Unity engine with the Mono scripting backend (MonoBleedingEdge).
    UnityMono,
    /// Unity engine with IL2CPP (GameAssembly.dll) — native, NOT mono-runnable.
    Il2Cpp,
    /// Classic FNA (FNA.dll).
    Fna,
    /// MonoGame (MonoGame.Framework.dll).
    MonoGame,
    /// XNA (Microsoft.Xna.Framework.dll).
    Xna,
    /// MonoKickstart bundle (`<exe>.bin.osx` / osx/libmonosgen).
    MonoKickstart,
    /// A managed .NET game with no FNA/XNA/MonoGame/Unity assemblies.
    BareDotnet,
    /// No managed-game signals at all.
    None,
}

/// Mono runtime requirement tier for the game.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MonoRequirement {
    /// Works with the baseline bundled Mono (classic XNA-era, mono 2.0-compatible).
    Baseline,
    /// Needs the modern Mono upgrade (11.2.0): Unity 2021+ or modern FNA.
    Modern,
}

/// Dependency signals the game carries.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, serde::Deserialize)]
pub struct MonoDeps {
    pub sdl2: bool,
    pub sdl3: bool,
    pub carbon: bool,
    pub faudio: bool,
    pub fmod: bool,
    pub steamworks_net: bool,
    pub galaxy: bool,
    pub bepinex: bool,
}

/// The full discovered profile for a game dir.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, serde::Deserialize)]
pub struct MonoProfile {
    pub kind: MonoProfileKind,
    /// Unity version string (e.g. "2021.3.5f1") when kind == UnityMono.
    pub unity_version: Option<String>,
    /// True when the game is 64-bit (from the game exe PE machine type).
    pub is_64_bit: bool,
    pub deps: MonoDeps,
    pub mono_requirement: MonoRequirement,
    /// True when the game is a .NET Core / .NET 5+ app (ships a
    /// `<exe>.runtimeconfig.json`). Such games CANNOT run on the bundled Mono
    /// runtime — they need a .NET runtime — so they must never take the mono
    /// route (Stardew Valley 1.6+ is net6.0 and crashed mono at init).
    pub dotnet_core: bool,
    /// File names / signals that drove the classification (for diagnostics).
    pub evidence: Vec<String>,
}

impl MonoProfile {
    pub fn none() -> Self {
        MonoProfile {
            kind: MonoProfileKind::None,
            unity_version: None,
            is_64_bit: true,
            deps: MonoDeps::default(),
            mono_requirement: MonoRequirement::Baseline,
            dotnet_core: false,
            evidence: Vec::new(),
        }
    }
}

/// Unity version pattern: 4-digit year, minor, patch, optional release letter.
fn parse_unity_version_at(data: &[u8], start: usize) -> Option<String> {
    let window = &data[start..(start + 32).min(data.len())];
    let end = window.iter().position(|&b| b == 0).unwrap_or(window.len());
    let s = std::str::from_utf8(&window[..end]).ok()?;
    if s.len() >= 7 && s.len() <= 16 {
        let mut parts = s.split('.');
        let year = parts.next()?.parse::<u32>().ok()?;
        let minor = parts.next()?.parse::<u32>().ok()?;
        let patch = parts.next()?;
        if (2005..=2030).contains(&year) && patch.chars().next().is_some_and(|c| c.is_ascii_digit()) {
            return Some(s.to_string());
        }
    }
    None
}

/// Read the Unity version from `globalgamemanagers` (verified at offset 48 on
/// a real DREDGE 2021.3.5f1 install; scanned over the first 128 bytes to be
/// robust across Unity releases).
pub fn parse_unity_version(data: &[u8]) -> Option<String> {
    let scan = &data[..data.len().min(128)];
    for i in 0..scan.len() {
        if scan[i].is_ascii_digit() {
            if let Some(v) = parse_unity_version_at(scan, i) {
                return Some(v);
            }
        }
    }
    None
}

fn has_file_ci(dir: &Path, name: &str) -> bool {
    let lower = name.to_lowercase();
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let Some(fname) = entry.file_name().to_str().map(str::to_lowercase) else {
            continue;
        };
        if fname == lower {
            return true;
        }
    }
    false
}

fn has_dir_ci(dir: &Path, name: &str) -> bool {
    let lower = name.to_lowercase();
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        if entry.path().is_dir() && entry.file_name().to_string_lossy().to_lowercase() == lower {
            return true;
        }
    }
    false
}

/// Any file whose name contains `needle` (case-insensitive) in `dir`.
fn has_file_containing(dir: &Path, needle: &str) -> bool {
    let lower = needle.to_lowercase();
    let Ok(entries) = fs::read_dir(dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let fname = entry.file_name().to_string_lossy().to_lowercase();
        if fname.contains(&lower) {
            return true;
        }
    }
    false
}

/// Read the managed assembly names under `<game>_data/Managed` (first dir that
/// has a Managed subdir; also checks a root `Managed/`).
fn collect_managed_assembly_names(game_dir: &Path) -> Vec<String> {
    let mut names = Vec::new();
    let Ok(entries) = fs::read_dir(game_dir) else {
        return names;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let fname = entry.file_name().to_string_lossy().to_lowercase();
        if fname.ends_with("_data") && path.is_dir() {
            let managed = path.join("Managed");
            if let Ok(me) = fs::read_dir(&managed) {
                for m in me.flatten() {
                    let name = m.file_name().to_string_lossy().to_lowercase();
                    if name.ends_with(".dll") {
                        names.push(name);
                    }
                }
                if !names.is_empty() {
                    break;
                }
            }
        }
    }
    if names.is_empty() {
        let managed = game_dir.join("Managed");
        if let Ok(me) = fs::read_dir(&managed) {
            for m in me.flatten() {
                let name = m.file_name().to_string_lossy().to_lowercase();
                if name.ends_with(".dll") {
                    names.push(name);
                }
            }
        }
    }
    names
}

/// Unity backend detection: `MonoBleedingEdge/` dir = Mono backend;
/// `GameAssembly.dll` = IL2CPP.
fn detect_unity_backend(game_dir: &Path) -> Option<MonoProfileKind> {
    if has_dir_ci(game_dir, "MonoBleedingEdge") {
        return Some(MonoProfileKind::UnityMono);
    }
    if has_file_ci(game_dir, "GameAssembly.dll") {
        return Some(MonoProfileKind::Il2Cpp);
    }
    None
}

fn detect_fna_family(managed: &[String], game_dir: &Path) -> Option<MonoProfileKind> {
    if managed.iter().any(|n| n == "fna.dll") || has_file_ci(game_dir, "FNA.dll") {
        return Some(MonoProfileKind::Fna);
    }
    if managed.iter().any(|n| n.starts_with("monogame") || n.contains("mg.framework"))
        || has_root_assembly_containing(game_dir, "monogame.framework")
    {
        return Some(MonoProfileKind::MonoGame);
    }
    if managed.iter().any(|n| n.starts_with("microsoft.xna.framework"))
        || has_root_assembly_containing(game_dir, "microsoft.xna.framework")
    {
        return Some(MonoProfileKind::Xna);
    }
    None
}

/// True when the game ROOT contains a .dll whose lowercased name contains
/// `needle` — covers the classic XNA/FNA layout (Terraria, Stardew Valley,
/// etc.) where managed assemblies sit next to the exe instead of a
/// `<Game>_Data/Managed` dir.
fn has_root_assembly_containing(game_dir: &Path, needle: &str) -> bool {
    let Ok(entries) = fs::read_dir(game_dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_lowercase();
        if name.ends_with(".dll") && name.contains(needle) {
            return true;
        }
    }
    false
}

/// MonoKickstart detection: `<exe>.bin.osx` present, `osx/libmonosgen*`
/// present, or a top-level `kick.bin.osx`.
fn detect_kickstart(game_dir: &Path) -> bool {
    if has_file_ci(game_dir, "kick.bin.osx") {
        return true;
    }
    let osx_dir = game_dir.join("osx");
    if osx_dir.is_dir() && has_file_containing(&osx_dir, "libmonosgen") {
        return true;
    }
    let Ok(entries) = fs::read_dir(game_dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let fname = entry.file_name().to_string_lossy().to_lowercase();
        if fname.ends_with(".bin.osx") {
            return true;
        }
    }
    false
}

fn detect_deps(game_dir: &Path, managed: &[String]) -> MonoDeps {
    let lower: Vec<String> = managed.to_vec();
    let contains = |needle: &str| lower.iter().any(|n| n.contains(needle));
    MonoDeps {
        sdl2: has_file_containing(game_dir, "libsdl2") || has_file_containing(game_dir, "sdl2.dll") || contains("sdl2"),
        sdl3: has_file_containing(game_dir, "libsdl3") || has_file_containing(game_dir, "sdl3.dll") || contains("sdl3"),
        carbon: has_file_containing(game_dir, "libcarbon") || contains("carbon"),
        faudio: has_file_containing(game_dir, "libfaudio") || contains("faudio"),
        fmod: has_file_containing(game_dir, "libfmod") || contains("fmod"),
        steamworks_net: contains("steamworks.net") || has_root_assembly_containing(game_dir, "steamworks.net"),
        galaxy: contains("galaxy") || has_root_assembly_containing(game_dir, "galaxy"),
        bepinex: has_dir_ci(game_dir, "BepInEx"),
    }
}

/// Discover the mono profile for a game directory. Pure filesystem reads;
/// never spawns, never writes.
pub fn discover_mono_profile(game_dir: &Path) -> MonoProfile {
    if !game_dir.is_dir() {
        return MonoProfile::none();
    }

    let mut evidence = Vec::new();
    let is_unity = has_file_ci(game_dir, "UnityPlayer.dll");
    let unity_backend = if is_unity { detect_unity_backend(game_dir) } else { None };
    let managed = collect_managed_assembly_names(game_dir);
    let fna_family = detect_fna_family(&managed, game_dir);
    let kickstart = detect_kickstart(game_dir);
    let deps = detect_deps(game_dir, &managed);

    // 64-bit from the game exe PE machine type (first exe found).
    let is_64_bit = exe_is_64_bit(game_dir);

    // .NET Core / .NET 5+ detection: a `<exe>.runtimeconfig.json` at the game
    // root marks a self-contained or framework-dependent .NET app that the
    // bundled Mono runtime cannot execute (mono asserts at init on the
    // net6.0 mscorlib surface, e.g. Stardew Valley 1.6+).
    let dotnet_core = is_dotnet_core_game(game_dir);
    if dotnet_core {
        evidence.push("runtimeconfig.json (.NET Core app)".into());
    }

    let kind = if is_unity {
        evidence.push("UnityPlayer.dll".into());
        match unity_backend {
            Some(MonoProfileKind::UnityMono) => {
                evidence.push("MonoBleedingEdge/".into());
                MonoProfileKind::UnityMono
            },
            Some(MonoProfileKind::Il2Cpp) => {
                evidence.push("GameAssembly.dll (IL2CPP)".into());
                MonoProfileKind::Il2Cpp
            },
            _ => {
                evidence.push("Unity engine, backend unknown".into());
                MonoProfileKind::UnityMono
            },
        }
    } else if kickstart {
        evidence.push("MonoKickstart bundle".into());
        MonoProfileKind::MonoKickstart
    } else if let Some(family) = fna_family {
        match family {
            MonoProfileKind::Fna => evidence.push("FNA.dll".into()),
            MonoProfileKind::MonoGame => evidence.push("MonoGame.Framework.dll".into()),
            MonoProfileKind::Xna => evidence.push("Microsoft.Xna.Framework.dll".into()),
            _ => {},
        }
        family
    } else if !managed.is_empty() {
        evidence.push(format!("{} managed assemblies", managed.len()));
        MonoProfileKind::BareDotnet
    } else {
        return MonoProfile::none();
    };

    // Unity version from globalgamemanagers header.
    let unity_version = if kind == MonoProfileKind::UnityMono {
        let ggm = game_dir.join("globalgamemanagers");
        if !ggm.exists() {
            // Unity 5+ layouts keep it inside `<Game>_Data/`.
            let mut found = None;
            if let Ok(entries) = fs::read_dir(game_dir) {
                for entry in entries.flatten() {
                    if entry.file_name().to_string_lossy().to_lowercase().ends_with("_data") {
                        let candidate = entry.path().join("globalgamemanagers");
                        if candidate.exists() {
                            found = Some(candidate);
                            break;
                        }
                    }
                }
            }
            found
        } else {
            Some(ggm)
        }
        .and_then(|p| fs::read(p).ok())
        .and_then(|data| parse_unity_version(&data))
    } else {
        None
    };
    if let Some(v) = &unity_version {
        evidence.push(format!("unity {v}"));
    }

    // Mono requirement: Unity 2021+ needs modern Mono; classic XNA/FNA with
    // no SDL3/Unity stays baseline.
    let mono_requirement = if kind == MonoProfileKind::UnityMono {
        let modern = unity_version
            .as_deref()
            .and_then(|v| v.split('.').next())
            .and_then(|y| y.parse::<u32>().ok())
            .map(|y| y >= 2021)
            .unwrap_or(true); // Unity without a version -> assume modern
        if modern {
            MonoRequirement::Modern
        } else {
            MonoRequirement::Baseline
        }
    } else if deps.sdl3 || kind == MonoProfileKind::MonoGame {
        MonoRequirement::Modern
    } else {
        MonoRequirement::Baseline
    };

    MonoProfile { kind, unity_version, is_64_bit, deps, mono_requirement, dotnet_core, evidence }
}

/// True when the game root ships a `<something>.runtimeconfig.json` — the
/// signature of a .NET Core / .NET 5+ application. The bundled Mono runtime
/// cannot run these (a mono 4.5 profile asserts on net6.0 assemblies during
/// runtime init), so they must route to a Wine pipeline instead.
pub fn is_dotnet_core_game(game_dir: &Path) -> bool {
    let Ok(entries) = fs::read_dir(game_dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_lowercase();
        if name.ends_with(".runtimeconfig.json") && entry.path().is_file() {
            // Confirm the payload actually targets .NET Core/5+ (a stray file
            // named *.runtimeconfig.json without a tfm is not a .NET app).
            if let Ok(data) = fs::read(entry.path()) {
                let text = String::from_utf8_lossy(&data);
                if text.contains("\"tfm\"") {
                    return true;
                }
            }
        }
    }
    false
}

/// 64-bit determination from the first `.exe` in the game dir (PE machine
/// type). Falls back to true (arm64 host default) when no exe is readable.
fn exe_is_64_bit(game_dir: &Path) -> bool {
    let Ok(entries) = fs::read_dir(game_dir) else {
        return true;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().map(|e| e == "exe").unwrap_or(false) {
            if let Ok(data) = fs::read(&path) {
                if let Some(info) = crate::mtsp::pe::parse_pe_imports(&data) {
                    return info.is_64_bit;
                }
            }
        }
    }
    true
}

/// Shorthand: is this game eligible for the FNA/Mono route?
pub fn is_mono_route_eligible(profile: &MonoProfile) -> bool {
    // .NET Core / .NET 5+ apps (runtimeconfig.json) cannot run on the
    // bundled Mono runtime — never offer the mono route for them.
    if profile.dotnet_core {
        return false;
    }
    matches!(
        profile.kind,
        MonoProfileKind::UnityMono
            | MonoProfileKind::Fna
            | MonoProfileKind::MonoGame
            | MonoProfileKind::Xna
            | MonoProfileKind::MonoKickstart
            | MonoProfileKind::BareDotnet
    )
}

/// Human-readable label for the mono requirement tier (for UI/status).
pub fn mono_requirement_label(requirement: MonoRequirement) -> &'static str {
    match requirement {
        MonoRequirement::Baseline => "baseline",
        MonoRequirement::Modern => "modern",
    }
}

/// True when the profile needs the modern Mono upgrade (11.2.0) rather than
/// the baseline runtime that ships with the app.
pub fn requires_mono_upgrade(profile: &MonoProfile) -> bool {
    profile.mono_requirement == MonoRequirement::Modern
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_dir(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "ms-mono-profile-{}-{}-{:x}",
            name,
            std::process::id(),
            std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write_unity_layout(dir: &Path, version: &str, il2cpp: bool, x86: bool) {
        fs::write(dir.join("UnityPlayer.dll"), b"unity").unwrap();
        if il2cpp {
            fs::write(dir.join("GameAssembly.dll"), b"il2cpp").unwrap();
        } else {
            fs::create_dir_all(dir.join("MonoBleedingEdge").join("EmbedRuntime")).unwrap();
            fs::write(dir.join("MonoBleedingEdge").join("EmbedRuntime").join("mono-2.0-bdwgc.dll"), b"mono").unwrap();
        }
        let data_dir = dir.join("DREDGE_Data");
        fs::create_dir_all(data_dir.join("Managed")).unwrap();
        for dll in ["UnityEngine.CoreModule.dll", "Assembly-CSharp.dll", "Newtonsoft.Json.dll"] {
            fs::write(data_dir.join("Managed").join(dll), b"m").unwrap();
        }
        // globalgamemanagers: version at offset 48 (real layout), plus a
        // leading length field.
        let mut ggm = vec![0u8; 48];
        ggm.extend_from_slice(version.as_bytes());
        ggm.push(0);
        fs::write(data_dir.join("globalgamemanagers"), &ggm).unwrap();
        // x86 exe signal.
        let exe_bytes = if x86 {
            // Minimal PE32 header with machine type 0x014c.
            pe_bytes(0x014c)
        } else {
            pe_bytes(0x8664)
        };
        fs::write(dir.join("DREDGE.exe"), exe_bytes).unwrap();
    }

    /// Minimal PE header with a given machine type (enough for parse_pe_imports).
    fn pe_bytes(machine: u16) -> Vec<u8> {
        let mut data = vec![0u8; 0x200];
        data[0..2].copy_from_slice(b"MZ");
        data[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes()); // e_lfanew
        data[0x80..0x84].copy_from_slice(b"PE\0\0");
        data[0x84..0x86].copy_from_slice(&machine.to_le_bytes());
        // Optional header magic PE32 (0x10b) at 0x98.
        data[0x98..0x9a].copy_from_slice(&0x10bu16.to_le_bytes());
        data
    }

    #[test]
    fn parse_unity_version_reads_real_header_layout() {
        let mut data = vec![0u8; 48];
        data.extend_from_slice(b"2021.3.5f1\0");
        assert_eq!(parse_unity_version(&data), Some("2021.3.5f1".to_string()));
    }

    #[test]
    fn parse_unity_version_rejects_garbage() {
        assert_eq!(parse_unity_version(b"not a version here"), None);
        assert_eq!(parse_unity_version(&[0u8; 64]), None);
    }

    #[test]
    fn dredge_shaped_dir_is_unity_mono_with_version_and_x86() {
        let dir = make_dir("dredge");
        write_unity_layout(&dir, "2021.3.5f1", false, true);
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::UnityMono);
        assert_eq!(profile.unity_version.as_deref(), Some("2021.3.5f1"));
        assert!(!profile.is_64_bit, "DREDGE is x86 PE32");
        assert_eq!(profile.mono_requirement, MonoRequirement::Modern);
        assert!(is_mono_route_eligible(&profile));
        assert!(profile.evidence.iter().any(|e| e.contains("2021.3.5f1")), "{:?}", profile.evidence);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn il2cpp_shaped_dir_is_not_mono_eligible() {
        let dir = make_dir("il2cpp");
        write_unity_layout(&dir, "2022.3.10f1", true, false);
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::Il2Cpp);
        assert!(!is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn necesse_shaped_dir_is_classic_fna() {
        let dir = make_dir("necesse");
        for name in [
            "FNA.dll",
            "Microsoft.Xna.Framework.dll",
            "Microsoft.Xna.Framework.Game.dll",
            "Microsoft.Xna.Framework.Graphics.dll",
        ] {
            fs::write(dir.join(name), b"dll").unwrap();
        }
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::Fna);
        assert_eq!(profile.mono_requirement, MonoRequirement::Baseline);
        assert!(is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn kickstart_bundle_is_detected() {
        let dir = make_dir("kickstart");
        fs::write(dir.join("Game.bin.osx"), b"kick").unwrap();
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::MonoKickstart);
        assert!(is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn terraria_shaped_dir_is_xna_via_root_assembly() {
        // Real Terraria layout (verified 2026-08-06 on the user's install):
        // root-level XNA assemblies, NO *_Data/Managed dir, NO FNA.dll.
        // The XNA family must be detected from the root Microsoft.Xna.*.dll
        // even though there is no Managed/ directory.
        let dir = make_dir("terraria");
        fs::write(dir.join("Terraria.exe"), b"mz").unwrap();
        fs::write(dir.join("Microsoft.Xna.Framework.Content.Pipeline.dll"), b"xna-content-pipeline").unwrap();
        fs::write(dir.join("ReLogic.Native.dll"), b"native").unwrap();
        fs::write(dir.join("steam_api.dll"), b"steam").unwrap();
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::Xna);
        assert_eq!(profile.mono_requirement, MonoRequirement::Baseline);
        assert!(is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn bepinex_modded_unity_still_detected() {
        let dir = make_dir("bepinex");
        write_unity_layout(&dir, "2022.3.5f1", false, false);
        fs::create_dir_all(dir.join("BepInEx")).unwrap();
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::UnityMono);
        assert!(profile.deps.bepinex);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn stardew_shaped_dir_is_monogame_via_root_assembly() {
        // Real Stardew Valley layout (verified 2026-08-06 on the user's
        // install): root-level MonoGame.Framework.dll + net6.0
        // runtimeconfig, NO *_Data/Managed dir. The MonoGame family must be
        // detected from the root assembly (classic root-layout coverage).
        let dir = make_dir("stardew");
        fs::write(dir.join("Stardew Valley.exe"), b"mz").unwrap();
        fs::write(dir.join("Stardew Valley.dll"), b"managed").unwrap();
        fs::write(dir.join("MonoGame.Framework.dll"), b"monogame").unwrap();
        fs::write(dir.join("Galaxy64.dll"), b"galaxy").unwrap();
        fs::write(dir.join("GalaxyCSharp.dll"), b"galaxy-cs").unwrap();
        fs::write(dir.join("SDL2.dll"), b"sdl2").unwrap();
        fs::write(dir.join("Stardew Valley.runtimeconfig.json"), b"{\"runtimeOptions\":{\"tfm\":\"net6.0\"}}").unwrap();
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::MonoGame);
        assert!(profile.deps.galaxy, "GalaxyCSharp must be a detected dep");
        assert!(profile.deps.sdl2, "SDL2.dll must be a detected dep");
        // net6.0 MonoGame -> modern mono requirement, BUT the runtimeconfig
        // marks it a .NET Core app that the bundled Mono cannot run — it must
        // NOT be mono-route-eligible (Stardew 1.6+ regression guard).
        assert_eq!(profile.mono_requirement, MonoRequirement::Modern);
        assert!(profile.dotnet_core, "net6.0 runtimeconfig must be detected");
        assert!(!is_mono_route_eligible(&profile), "a .NET Core app must never take the mono route");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn dotnet_core_detection_ignores_netframework_app_config() {
        // .NET Framework games ship app.config (or nothing), never a
        // *.runtimeconfig.json — they must stay mono-route-eligible.
        let dir = make_dir("netfx");
        fs::write(dir.join("Terraria.exe"), b"mz").unwrap();
        fs::write(dir.join("Microsoft.Xna.Framework.dll"), b"xna").unwrap();
        fs::write(dir.join("Terraria.exe.config"), b"<configuration/>").unwrap();
        let profile = discover_mono_profile(&dir);
        assert!(!profile.dotnet_core);
        assert!(is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn empty_dir_is_none() {
        let dir = make_dir("empty");
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::None);
        assert!(!is_mono_route_eligible(&profile));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn sdl3_dep_is_detected() {
        let dir = make_dir("sdl3");
        fs::write(dir.join("FNA.dll"), b"dll").unwrap();
        fs::write(dir.join("libSDL3.dylib"), b"sdl3").unwrap();
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.kind, MonoProfileKind::Fna);
        assert!(profile.deps.sdl3);
        assert_eq!(profile.mono_requirement, MonoRequirement::Modern);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn mono_requirement_labels_and_upgrade_helpers() {
        let dir = make_dir("req");
        write_unity_layout(&dir, "2021.3.5f1", false, false);
        let profile = discover_mono_profile(&dir);
        assert_eq!(profile.mono_requirement, MonoRequirement::Modern);
        assert!(requires_mono_upgrade(&profile));
        assert_eq!(mono_requirement_label(MonoRequirement::Modern), "modern");
        assert_eq!(mono_requirement_label(MonoRequirement::Baseline), "baseline");
        let _ = fs::remove_dir_all(&dir);
    }
}
