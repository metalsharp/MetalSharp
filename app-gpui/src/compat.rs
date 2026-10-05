//! Compatibility hints for Steam games, built into the app from the repo's
//! compatibility sources: confirmed games in games-supported.md, launch rules
//! in mtsp-rules.toml, and titles whose kernel-level anti-cheat can't run.
use std::collections::HashSet;
use std::sync::OnceLock;

const SUPPORTED_GAMES: &str = include_str!("../../docs/games-supported.md");
const LAUNCH_RULES: &str = include_str!("../../configs/mtsp-rules.toml");

/// Kernel-level anti-cheat (EA AntiCheat, Easy Anti-Cheat kernel mode, ...).
const KERNEL_ANTI_CHEAT_APPIDS: &[u64] = &[
    1097150, // Fall Guys
    2073850, // THE FINALS
    2290180, // Riders Republic
    2943650, // FragPunk
];
/// Titles outside Steam (no appid) matched by name.
const KERNEL_ANTI_CHEAT_NAMES: &[&str] = &["fortnite"];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Compat {
    /// Listed in games-supported.md.
    Playable,
    /// Has a launch rule, but isn't confirmed playable yet.
    MayRun,
    /// Kernel-level anti-cheat; won't run under Wine.
    AntiCheat,
}

/// Appids from the supported-games tables (`| Game | AppID | Notes |`).
fn supported_appids(markdown: &str) -> HashSet<u64> {
    markdown
        .lines()
        .filter(|line| line.trim_start().starts_with('|'))
        .flat_map(|line| line.split('|').map(str::trim))
        .filter(|cell| cell.len() >= 2 && cell.bytes().all(|b| b.is_ascii_digit()))
        .filter_map(|cell| cell.parse().ok())
        .collect()
}

/// Appids with an `[overrides.<appid>]` launch rule.
fn rule_appids(toml: &str) -> HashSet<u64> {
    toml.lines()
        .filter_map(|line| line.trim().strip_prefix("[overrides."))
        .filter_map(|rest| {
            let digits: String = rest.chars().take_while(char::is_ascii_digit).collect();
            digits.parse().ok()
        })
        .collect()
}

pub fn compat(appid: u64, name: &str) -> Option<Compat> {
    static SUPPORTED: OnceLock<HashSet<u64>> = OnceLock::new();
    static RULES: OnceLock<HashSet<u64>> = OnceLock::new();
    let lower = name.to_lowercase();
    if KERNEL_ANTI_CHEAT_APPIDS.contains(&appid)
        || KERNEL_ANTI_CHEAT_NAMES.iter().any(|n| lower.contains(n))
    {
        return Some(Compat::AntiCheat);
    }
    if SUPPORTED
        .get_or_init(|| supported_appids(SUPPORTED_GAMES))
        .contains(&appid)
    {
        return Some(Compat::Playable);
    }
    if RULES
        .get_or_init(|| rule_appids(LAUNCH_RULES))
        .contains(&appid)
    {
        return Some(Compat::MayRun);
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn supported_table_appids_are_parsed() {
        let doc = "| Game | AppID | Notes |\n|---|---:|---|\n| Elden Ring | 1245620 | Offline Play |\n| Far Cry 5 | Ubisoft | |\n";
        assert_eq!(supported_appids(doc), HashSet::from([1245620]));
    }

    #[test]
    fn override_headers_are_parsed() {
        let toml =
            "[overrides.105600]\npipeline = \"fna_arm64\"\n[overrides.312520.dependencies]\n";
        assert_eq!(rule_appids(toml), HashSet::from([105600, 312520]));
    }

    #[test]
    fn shipped_sources_classify_known_games() {
        assert_eq!(compat(1245620, "ELDEN RING"), Some(Compat::Playable));
        assert_eq!(compat(1097150, "Fall Guys"), Some(Compat::AntiCheat));
        assert_eq!(compat(0, "Fortnite"), Some(Compat::AntiCheat));
        assert_eq!(compat(105600, "Terraria").is_some(), true);
        assert_eq!(compat(1, "Not a game"), None);
    }
}
