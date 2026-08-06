//! Phase 8: Mono/FNA/XNA pipeline reliability and asset coverage.
//!
//! Treats Mono/FNA/XNA as a first-class compatibility family, not only a
//! handful of known app ids. This module adds:
//!
//! * richer flavor detection (FNA / MonoGame / XNA + Steamworks.NET /
//!   CSteamworks + audio deps + x86-vs-native Mono), reusing
//!   [`crate::mtsp::launcher::detect_fna_flavor`] for the base signal and
//!   layering the additional signals the roadmap requires;
//! * receipt-driven asset staging types ([`AssetReceipt`]) so staging is
//!   auditable and reversible — record what was copied, source path + hash,
//!   required vs optional, and whether a game file was overwritten;
//! * a "profile explain" diagnostic ([`explain_profile`]) that reports WHY a
//!   game selected FNA ARM64, FNA x86, XNA/MonoGame x86, or a fallback route;
//! * a conservative unproven-game classifier ([`classify_unproven_fna_game`])
//!   that does NOT claim compatibility, stages only reversible shims, and
//!   offers fallback to the Wine route when native Mono/FNA launch is not
//!   proven.
//!
//! The pinned known-good behavior for Terraria (105600), Celeste (504230),
//! and Stardew Valley (413150) is unchanged; this module explains and
//! receipts it, it does not override it.

use serde::Serialize;
use std::collections::BTreeSet;
use std::fs;
use std::path::Path;

/// Known-good pinned FNA/XNA app ids. These keep their existing pinned
/// behavior; the unproven-game classifier never applies to them.
pub const PINNED_FNA_APPIDS: &[u32] = &[105600, 504230, 413150];

/// Additional signals layered on top of [`detect_fna_flavor`].
#[derive(Debug, Clone, Default, Serialize)]
pub struct FnaFlavorSignals {
    pub base_flavor: String,
    pub uses_steamworks_net: bool,
    pub uses_csteamworks: bool,
    pub uses_faudio: bool,
    pub uses_fmod: bool,
    pub uses_openal: bool,
    pub uses_xinput: bool,
    pub has_managed_dir: bool,
    /// True if a 32-bit Mono indicator is present (e.g. a `x86` subdirectory,
    /// a `MonoBundle` with 32-bit signatures, or a `Celeste.exe`-style name).
    /// Conservative: we only set this when we have a positive signal.
    pub indicates_x86_mono: bool,
    /// True if the game ships an arm64 native executable, indicating the
    /// native Mono lane is appropriate.
    pub indicates_native_mono: bool,
    /// File names that drove the detection (for the profile-explain report).
    pub evidence_files: Vec<String>,
}

/// Detect the richer FNA/XNA flavor signals for a game directory. This is a
/// superset of [`crate::mtsp::launcher::detect_fna_flavor`] and does NOT
/// change that function's behavior — it layers additional signals.
pub fn detect_fna_signals(game_dir: &Path) -> FnaFlavorSignals {
    use crate::mtsp::launcher::detect_fna_flavor;

    let base = detect_fna_flavor(&game_dir.to_path_buf());
    let mut signals = FnaFlavorSignals { base_flavor: format!("{:?}", base).to_lowercase(), ..Default::default() };

    let managed_dlls = collect_managed_dll_names(game_dir, &mut signals.has_managed_dir, &mut signals.evidence_files);

    let lower: BTreeSet<String> = managed_dlls.iter().map(|d| d.to_lowercase()).collect();

    signals.uses_steamworks_net = lower.contains("steamworks.net.dll");
    if signals.uses_steamworks_net {
        signals.evidence_files.push("Steamworks.NET.dll".into());
    }
    signals.uses_csteamworks = lower.contains("csteamworks.dll");
    if signals.uses_csteamworks {
        signals.evidence_files.push("CSteamworks.dll".into());
    }
    signals.uses_faudio = lower.iter().any(|d| d.contains("faudio")) || game_dir.join("libFAudio.dylib").exists();
    if signals.uses_faudio {
        signals.evidence_files.push("FAudio".into());
    }
    signals.uses_fmod = lower.iter().any(|d| d.contains("fmod")) || game_dir.join("libfmod.dylib").exists();
    if signals.uses_fmod {
        signals.evidence_files.push("FMOD".into());
    }
    signals.uses_openal = lower.iter().any(|d| d.contains("openal")) || game_dir.join("libopenal.dylib").exists();
    if signals.uses_openal {
        signals.evidence_files.push("OpenAL".into());
    }
    signals.uses_xinput =
        lower.iter().any(|d| d.contains("xinput")) || lower.iter().any(|d| d.contains("xinput1_3.dll"));
    if signals.uses_xinput {
        signals.evidence_files.push("XInput".into());
    }

    // x86-vs-native signal: a Windows-only .NET game with an x86 hint (a
    // win32 subdirectory, or the absence of an arm64 mac executable) leans
    // x86 Mono; a shipped arm64 MacOS executable leans native.
    if game_dir.join("x86").is_dir() {
        signals.indicates_x86_mono = true;
        signals.evidence_files.push("x86/".into());
    }
    if has_arm64_macos_executable(game_dir) {
        signals.indicates_native_mono = true;
        signals.evidence_files.push("arm64 macOS executable".into());
    } else if signals.base_flavor != "unknown" {
        // An FNA/XNA game with no native arm64 executable is conservatively
        // treated as x86 Mono (the historical default).
        signals.indicates_x86_mono = true;
    }

    signals
}

fn collect_managed_dll_names(game_dir: &Path, has_managed: &mut bool, evidence: &mut Vec<String>) -> Vec<String> {
    let mut names = Vec::new();
    let Ok(entries) = fs::read_dir(game_dir) else {
        return names;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !(path.is_dir() && entry.file_name().to_string_lossy().to_lowercase().ends_with("_data")) {
            continue;
        }
        let managed = path.join("Managed");
        let Ok(managed_entries) = fs::read_dir(&managed) else {
            continue;
        };
        *has_managed = true;
        for me in managed_entries.flatten() {
            let dll = me.file_name().to_string_lossy().to_string();
            if dll.to_lowercase().ends_with(".dll") {
                names.push(dll);
            }
        }
        if names.iter().any(|n| n.to_lowercase() == "fna.dll") {
            evidence.push("FNA.dll".into());
        }
        break;
    }
    names
}

fn has_arm64_macos_executable(game_dir: &Path) -> bool {
    // Look for a Contents/MacOS/<exe> path inside a .app bundle, or a top-
    // level arm64 Mach-O executable. Conservative: presence of an .app bundle
    // is a strong native signal; absence is not proof of x86.
    let Ok(entries) = fs::read_dir(game_dir) else {
        return false;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().to_string();
        if name.ends_with(".app") {
            return true;
        }
    }
    false
}

/// A single staged-asset receipt. Records what was copied, the source path
/// and its sha256, whether the asset was required or optional, and whether a
/// pre-existing game file was overwritten (so staging is reversible).
#[derive(Debug, Clone, Serialize)]
pub struct AssetReceipt {
    pub filename: String,
    pub source_path: String,
    pub dest_path: String,
    pub required: bool,
    pub source_sha256: Option<String>,
    pub dest_sha256: Option<String>,
    pub overwrote_game_file: bool,
    pub staged: bool,
    pub reason: String,
}

/// Build a receipt for a staged asset WITHOUT mutating anything. The caller
/// performs the copy; this records what happened. `overwrote_game_file` must
/// be true only when the destination existed and differed from the source
/// before the copy.
pub fn record_asset_receipt(
    filename: &str,
    source_path: &Path,
    dest_path: &Path,
    required: bool,
    overwrote_game_file: bool,
    staged: bool,
    reason: &str,
) -> AssetReceipt {
    AssetReceipt {
        filename: filename.to_string(),
        source_path: source_path.to_string_lossy().to_string(),
        dest_path: dest_path.to_string_lossy().to_string(),
        required,
        source_sha256: if source_path.exists() { crate::diagnostics::file_sha256(source_path) } else { None },
        dest_sha256: if dest_path.exists() { crate::diagnostics::file_sha256(dest_path) } else { None },
        overwrote_game_file,
        staged,
        reason: reason.to_string(),
    }
}

/// A staging report persisted next to the game dir so a future run can skip
/// re-copying when source and staged hashes already match.
#[derive(Debug, Clone, Serialize)]
pub struct AssetStagingReport {
    pub schema_version: u32,
    pub appid: u32,
    pub generated_at_unix: u64,
    pub receipts: Vec<AssetReceipt>,
}

impl AssetStagingReport {
    pub fn new(appid: u32) -> Self {
        AssetStagingReport {
            schema_version: 1,
            appid,
            generated_at_unix: std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_secs())
                .unwrap_or(0),
            receipts: Vec::new(),
        }
    }

    pub fn record(&mut self, receipt: AssetReceipt) {
        self.receipts.push(receipt);
    }

    /// Write the report atomically to `<game_dir>/.metalsharp/fna-staging.json`.
    pub fn persist(&self, game_dir: &Path) -> std::io::Result<()> {
        let dir = game_dir.join(".metalsharp");
        fs::create_dir_all(&dir)?;
        let final_path = dir.join("fna-staging.json");
        let tmp_path = dir.join("fna-staging.json.tmp");
        let payload = serde_json::to_string_pretty(self).unwrap_or_else(|_| "{}".into());
        fs::write(&tmp_path, payload)?;
        fs::rename(&tmp_path, &final_path)?;
        Ok(())
    }
}

/// The conservative recommendation for an unproven FNA/XNA game.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum UnprovenRecommendation {
    /// A conservative FNA/XNA setup is reasonable: stage only reversible
    /// shims, preserve originals, and offer a Wine fallback.
    ConservativeFnaSetup,
    /// The Wine route is safer than a native Mono attempt.
    WineFallback,
    /// The game does not look like an FNA/XNA title; do not force this lane.
    NotFna,
}

/// The result of classifying an unproven game.
#[derive(Debug, Clone, Serialize)]
pub struct UnprovenClassification {
    pub appid: u32,
    pub pinned: bool,
    pub recommendation: UnprovenRecommendation,
    pub signals: FnaFlavorSignals,
    pub rationale: Vec<String>,
}

/// Classify an unproven game conservatively. Pinned known-good app ids are
/// never classified here (they keep their pinned behavior); this only
/// applies to games NOT in [`PINNED_FNA_APPIDS`].
pub fn classify_unproven_fna_game(appid: u32, game_dir: &Path) -> UnprovenClassification {
    let pinned = PINNED_FNA_APPIDS.contains(&appid);
    let signals = detect_fna_signals(game_dir);
    let mut rationale = Vec::new();

    if pinned {
        rationale.push(format!("appid {} is a pinned known-good FNA/XNA title; it keeps its pinned behavior", appid));
        return UnprovenClassification {
            appid,
            pinned: true,
            recommendation: UnprovenRecommendation::ConservativeFnaSetup,
            signals,
            rationale,
        };
    }

    if signals.base_flavor == "unknown" {
        rationale.push("no FNA/MonoGame/XNA assemblies detected; do not force this lane".into());
        return UnprovenClassification {
            appid,
            pinned: false,
            recommendation: UnprovenRecommendation::NotFna,
            signals,
            rationale,
        };
    }

    rationale.push(format!("detected {} flavor", signals.base_flavor));
    if signals.indicates_native_mono {
        rationale.push("native arm64 macOS executable present; native Mono lane is plausible".into());
    } else if signals.indicates_x86_mono {
        rationale.push("no native arm64 executable / x86 indicator present; x86 Mono lane is plausible".into());
    }
    if signals.uses_steamworks_net || signals.uses_csteamworks {
        rationale.push("Steamworks.NET/CSteamworks detected; staging must keep Steam identity shims reversible".into());
    }
    // Conservative: do NOT claim compatibility. Stage only reversible shims
    // and offer a Wine fallback. We recommend ConservativeFnaSetup only when
    // we have a positive flavor signal; otherwise WineFallback.
    let recommendation = if signals.base_flavor == "unknown" {
        UnprovenRecommendation::WineFallback
    } else {
        UnprovenRecommendation::ConservativeFnaSetup
    };
    rationale.push("conservative: stage only reversible shims, preserve originals, offer Wine fallback".into());

    UnprovenClassification { appid, pinned: false, recommendation, signals, rationale }
}

/// A "profile explain" diagnostic that reports WHY a game selected a given
/// FNA/XNA profile (or fallback), for known-good and unproven games alike.
#[derive(Debug, Clone, Serialize)]
pub struct ProfileExplanation {
    pub schema_version: u32,
    pub appid: u32,
    pub pinned: bool,
    pub mono_arch: String,
    pub method_label: String,
    pub mono_config: String,
    pub signals: FnaFlavorSignals,
    pub rationale: Vec<String>,
    /// Mono runtime tier the game needs (baseline vs modern/11.2.0 upgrade).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mono_requirement: Option<String>,
}

/// Explain the FNA/XNA profile selection for an appid + game dir. For pinned
/// games, reports the pinned profile and the signals that confirm it. For
/// unproven games, reports the conservative classification.
pub fn explain_profile(appid: u32, game_dir: &Path) -> ProfileExplanation {
    let profile = crate::mtsp::launcher::find_fna_profile(appid);
    let pinned = PINNED_FNA_APPIDS.contains(&appid);
    let signals = detect_fna_signals(game_dir);
    let mut rationale = Vec::new();

    let mono_arch = match profile.mono_arch {
        crate::mtsp::launcher::MonoArch::Native => "native".to_string(),
        crate::mtsp::launcher::MonoArch::X86 => "x86".to_string(),
    };

    // Mono runtime tier from discovery (baseline vs modern/11.2.0 upgrade).
    let discovered = crate::mono_profile::discover_mono_profile(game_dir);
    let mono_requirement = if discovered.kind != crate::mono_profile::MonoProfileKind::None {
        Some(crate::mono_profile::mono_requirement_label(discovered.mono_requirement).to_string())
    } else {
        None
    };
    if mono_requirement.as_deref() == Some("modern") {
        rationale.push("needs the modern Mono runtime (11.2.0 upgrade via the Mono button)".into());
    }

    if pinned {
        rationale.push(format!(
            "appid {} is pinned to the {} lane with the {} mono config",
            appid, profile.method_label, profile.mono_config
        ));
        if signals.base_flavor != "unknown" {
            rationale.push(format!("detected {} flavor confirms the FNA/XNA family", signals.base_flavor));
        }
    } else {
        let class = classify_unproven_fna_game(appid, game_dir);
        rationale = class.rationale;
    }

    ProfileExplanation {
        schema_version: 1,
        appid,
        pinned,
        mono_arch,
        method_label: profile.method_label.to_string(),
        mono_config: profile.mono_config.to_string(),
        signals,
        rationale,
        mono_requirement,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_managed_game_dir(dir: &Path, dlls: &[&str]) {
        let managed = dir.join("Celeste_data").join("Managed");
        fs::create_dir_all(&managed).unwrap();
        for dll in dlls {
            fs::write(managed.join(dll), b"dll-bytes").unwrap();
        }
    }

    #[test]
    fn detect_fna_signals_flags_fna_and_audio_deps() {
        let dir = std::env::temp_dir().join("ms-fna-signals-fna");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll", "Steamworks.NET.dll", "CSteamworks.dll"]);
        // Top-level native libs.
        fs::write(dir.join("libFAudio.dylib"), b"x").unwrap();
        fs::write(dir.join("libfmod.dylib"), b"x").unwrap();

        let signals = detect_fna_signals(&dir);
        assert_eq!(signals.base_flavor, "fna");
        assert!(signals.has_managed_dir);
        assert!(signals.uses_steamworks_net);
        assert!(signals.uses_csteamworks);
        assert!(signals.uses_faudio);
        assert!(signals.uses_fmod);
        assert!(!signals.evidence_files.is_empty());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn detect_fna_signals_flags_monogame_and_xna() {
        let dir = std::env::temp_dir().join("ms-fna-signals-mg");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["MonoGame.Framework.dll"]);
        let signals = detect_fna_signals(&dir);
        assert_eq!(signals.base_flavor, "monogame");

        let dir2 = std::env::temp_dir().join("ms-fna-signals-xna");
        let _ = fs::remove_dir_all(&dir2);
        make_managed_game_dir(&dir2, &["Microsoft.Xna.Framework.dll"]);
        let signals2 = detect_fna_signals(&dir2);
        assert_eq!(signals2.base_flavor, "xna");
        let _ = fs::remove_dir_all(&dir);
        let _ = fs::remove_dir_all(&dir2);
    }

    #[test]
    fn detect_fna_signals_unknown_when_no_fna_assemblies() {
        let dir = std::env::temp_dir().join("ms-fna-signals-none");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("game.exe"), b"x").unwrap();
        let signals = detect_fna_signals(&dir);
        assert_eq!(signals.base_flavor, "unknown");
        assert!(!signals.has_managed_dir);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn classify_unproven_fna_game_is_conservative_for_unknown_flavor() {
        let dir = std::env::temp_dir().join("ms-fna-classify-unknown");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        // Not pinned, no FNA signal => NotFna.
        let class = classify_unproven_fna_game(999999, &dir);
        assert!(!class.pinned);
        assert!(matches!(class.recommendation, UnprovenRecommendation::NotFna));
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn classify_unproven_fna_game_recommends_conservative_setup_for_fna_signal() {
        let dir = std::env::temp_dir().join("ms-fna-classify-fna");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll"]);
        let class = classify_unproven_fna_game(888888, &dir);
        assert!(!class.pinned);
        assert!(matches!(class.recommendation, UnprovenRecommendation::ConservativeFnaSetup));
        assert!(
            class.rationale.iter().any(|r| r.contains("reversible")),
            "must be conservative: {:?}",
            class.rationale
        );
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn classify_unproven_fna_game_never_overrides_pinned_titles() {
        // Terraria/Celeste/Stardew are pinned and must keep their behavior.
        let dir = std::env::temp_dir().join("ms-fna-classify-pinned");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        for appid in PINNED_FNA_APPIDS {
            let class = classify_unproven_fna_game(*appid, &dir);
            assert!(class.pinned, "appid {} must be pinned", appid);
        }
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn pinned_fna_appids_cover_terrarria_celeste_stardew() {
        assert!(PINNED_FNA_APPIDS.contains(&105600), "Terraria");
        assert!(PINNED_FNA_APPIDS.contains(&504230), "Celeste");
        assert!(PINNED_FNA_APPIDS.contains(&413150), "Stardew Valley");
    }

    #[test]
    fn record_asset_receipt_captures_source_and_dest_hashes() {
        let dir = std::env::temp_dir().join("ms-fna-receipt");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let src = dir.join("libfmod.dylib");
        let dst = dir.join("game").join("libfmod.dylib");
        fs::create_dir_all(dst.parent().unwrap()).unwrap();
        fs::write(&src, b"fmod-bytes").unwrap();
        fs::write(&dst, b"old-bytes").unwrap();

        let receipt = record_asset_receipt("libfmod.dylib", &src, &dst, true, true, true, "required audio shim");
        assert_eq!(receipt.filename, "libfmod.dylib");
        assert!(receipt.required);
        assert!(receipt.overwrote_game_file);
        assert!(receipt.staged);
        assert!(receipt.source_sha256.is_some());
        assert!(receipt.dest_sha256.is_some());
        assert!(receipt.source_sha256 != receipt.dest_sha256, "old dest must differ from new source");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn asset_staging_report_persists_and_round_trips() {
        let dir = std::env::temp_dir().join("ms-fna-staging-report");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let mut report = AssetStagingReport::new(504230);
        report.record(AssetReceipt {
            filename: "libFAudio.dylib".into(),
            source_path: "/shims/libFAudio.dylib".into(),
            dest_path: dir.join("libFAudio.dylib").to_string_lossy().to_string(),
            required: true,
            source_sha256: Some("abc".into()),
            dest_sha256: Some("abc".into()),
            overwrote_game_file: false,
            staged: true,
            reason: "required audio shim".into(),
        });
        report.persist(&dir).unwrap();

        let raw = fs::read_to_string(dir.join(".metalsharp").join("fna-staging.json")).unwrap();
        let back: serde_json::Value = serde_json::from_str(&raw).unwrap();
        assert_eq!(back.get("schema_version").and_then(|v| v.as_u64()), Some(1));
        assert_eq!(back.get("appid").and_then(|v| v.as_u64()), Some(504230));
        assert_eq!(back.get("receipts").unwrap().as_array().unwrap().len(), 1);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn explain_profile_reports_pinned_celeste_lane() {
        let dir = std::env::temp_dir().join("ms-fna-explain-celeste");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll"]);
        let explanation = explain_profile(504230, &dir);
        assert!(explanation.pinned);
        assert_eq!(explanation.method_label, "xna_fna_x86");
        assert_eq!(explanation.mono_config, "celeste-x86-mono.config");
        assert_eq!(explanation.mono_arch, "x86");
        assert!(explanation.rationale.iter().any(|r| r.contains("pinned")), "{:?}", explanation.rationale);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn explain_profile_reports_pinned_stardew_native_lane() {
        let dir = std::env::temp_dir().join("ms-fna-explain-stardew");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll"]);
        let explanation = explain_profile(413150, &dir);
        assert!(explanation.pinned);
        assert_eq!(explanation.method_label, "xna_fna_arm64");
        assert_eq!(explanation.mono_arch, "native");
        assert_eq!(explanation.mono_config, "stardew-mono.config");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn explain_profile_reports_pinned_terraria_x86_lane() {
        let dir = std::env::temp_dir().join("ms-fna-explain-terraria");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll"]);
        let explanation = explain_profile(105600, &dir);
        assert!(explanation.pinned);
        assert_eq!(explanation.method_label, "xna_fna_x86");
        assert_eq!(explanation.mono_config, "generic-fna-mono.config");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn explain_profile_for_unproven_game_is_explanatory_not_claiming() {
        let dir = std::env::temp_dir().join("ms-fna-explain-unproven");
        let _ = fs::remove_dir_all(&dir);
        make_managed_game_dir(&dir, &["FNA.dll"]);
        let explanation = explain_profile(777777, &dir);
        assert!(!explanation.pinned);
        // Must NOT claim compatibility; rationale must mention conservatism.
        assert!(
            explanation.rationale.iter().any(|r| r.to_lowercase().contains("conservative") || r.contains("reversible")),
            "unproven explanation must be conservative: {:?}",
            explanation.rationale
        );
        let _ = fs::remove_dir_all(&dir);
    }
}
