//! Closed, source-backed theme IDs and per-surface color tokens. This service is inert by
//! default: offline preview starts in the approved dark palette and performs no I/O.
use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};

#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, PartialEq, Eq, Hash)]
#[serde(rename_all = "kebab-case")]
pub enum ThemeId {
    #[default]
    Dark,
    Light,
    Skeleton,
    Forest,
    OrangePeel,
    Dragonfruit,
    Lava,
}
impl ThemeId {
    pub const ALL: [Self; 7] = [
        Self::Dark,
        Self::Light,
        Self::Skeleton,
        Self::Forest,
        Self::OrangePeel,
        Self::Dragonfruit,
        Self::Lava,
    ];
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Dark => "dark",
            Self::Light => "light",
            Self::Skeleton => "skeleton",
            Self::Forest => "forest",
            Self::OrangePeel => "orange-peel",
            Self::Dragonfruit => "dragonfruit",
            Self::Lava => "lava",
        }
    }
    pub fn parse(value: &str) -> Option<Self> {
        Self::ALL.into_iter().find(|theme| theme.as_str() == value)
    }
    pub fn palette(self) -> ThemePalette {
        match self {
            Self::Dark => palette(
                0xe8d6b7, 0xe8d6b7, 0xffffff38, 0x080a0d, 0xffffff, 0x171a1e, 0x080a0d, 0x171a1e,
                false,
            ),
            Self::Light => palette(
                0x4db8ff, 0x303b48, 0x1e27323d, 0xfdfbf7, 0x1e2732, 0xffffff, 0xfdfbf7, 0xffffff,
                true,
            ),
            Self::Skeleton => palette(
                0xd6d0c4, 0x77736b, 0xeeeeee3d, 0x242424, 0xeeeeee, 0x303030, 0x242424, 0x303030,
                false,
            ),
            Self::Forest => palette(
                0x6fce88, 0x6fce88, 0x8cbe963d, 0x182219, 0xdce8dc, 0x1e2b20, 0x182219, 0x1e2b20,
                false,
            ),
            Self::OrangePeel => palette(
                0xff9a45, 0xff9a45, 0xffaa783d, 0x231610, 0xf2e4d8, 0x2b1b12, 0x231610, 0x2b1b12,
                false,
            ),
            Self::Dragonfruit => palette(
                0xff66aa, 0xff66aa, 0xffaad23d, 0x2c182a, 0xf8e4f0, 0x361e33, 0x2c182a, 0x361e33,
                false,
            ),
            Self::Lava => palette(
                0xff6b52, 0xff6b52, 0xff6e5a4d, 0x2b0d12, 0xfff2ee, 0x3b1117, 0x2b0d12, 0x3b1117,
                false,
            ),
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ThemePalette {
    pub accent: u32,
    pub border: u32,
    pub control_border: u32,
    pub control_bg: u32,
    pub control_text: u32,
    pub hover: u32,
    pub menu_bg: u32,
    pub menu_hover: u32,
    pub light: bool,
    pub library_accent: u32,
}
fn palette(
    accent: u32,
    border: u32,
    control_border: u32,
    control_bg: u32,
    control_text: u32,
    hover: u32,
    menu_bg: u32,
    menu_hover: u32,
    light: bool,
) -> ThemePalette {
    ThemePalette {
        accent,
        border,
        control_border,
        control_bg,
        control_text,
        hover,
        menu_bg,
        menu_hover,
        light,
        library_accent: accent,
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ThemeSnapshot {
    pub app: ThemeId,
    pub library: ThemeId,
    pub revision: u64,
}
#[derive(Clone, Debug)]
pub struct ThemePreferences {
    app: ThemeId,
    library: ThemeId,
    revision: u64,
}
impl Default for ThemePreferences {
    fn default() -> Self {
        Self {
            app: ThemeId::Dark,
            library: ThemeId::Dark,
            revision: 0,
        }
    }
}
impl ThemePreferences {
    pub fn from_config(value: &Value) -> Self {
        let prefs = &value["appearance"];
        Self {
            app: prefs["theme"]
                .as_str()
                .and_then(ThemeId::parse)
                .unwrap_or_default(),
            library: prefs["libraryTheme"]
                .as_str()
                .and_then(ThemeId::parse)
                .unwrap_or_else(|| {
                    prefs["theme"]
                        .as_str()
                        .and_then(ThemeId::parse)
                        .unwrap_or_default()
                }),
            revision: 0,
        }
    }
    pub fn snapshot(&self) -> ThemeSnapshot {
        ThemeSnapshot {
            app: self.app,
            library: self.library,
            revision: self.revision,
        }
    }
    pub fn set_app(&mut self, id: ThemeId) -> bool {
        if self.app == id {
            return false;
        }
        self.app = id;
        self.revision = self.revision.wrapping_add(1);
        true
    }
    pub fn set_library(&mut self, id: ThemeId) -> bool {
        if self.library == id {
            return false;
        }
        self.library = id;
        self.revision = self.revision.wrapping_add(1);
        true
    }
    /// Partial typed patch. A caller merges this object into the existing config document;
    /// every unrelated/future key, credential, profile, and backend-owned value is retained.
    pub fn patch(&self) -> Value {
        serde_json::json!({"appearance":{"theme":self.app.as_str(),"libraryTheme":self.library.as_str()}})
    }
    pub fn palette(&self) -> ThemePalette {
        self.app.palette()
    }
    pub fn library_palette(&self) -> ThemePalette {
        self.library.palette()
    }
}
/// Recursively merge a partial preference patch into the existing JSON document.
pub fn merge_patch(existing: &mut Value, patch: &Value) {
    match (existing, patch) {
        (Value::Object(target), Value::Object(source)) => {
            for (key, value) in source {
                if value.is_object() {
                    merge_patch(
                        target
                            .entry(key)
                            .or_insert_with(|| Value::Object(Map::new())),
                        value,
                    );
                } else {
                    target.insert(key.clone(), value.clone());
                }
            }
        }
        (target, value) => *target = value.clone(),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn all_closed_theme_ids_have_the_expected_library_and_contrast_tokens() {
        assert_eq!(
            ThemeId::ALL.iter().map(|x| x.as_str()).collect::<Vec<_>>(),
            [
                "dark",
                "light",
                "skeleton",
                "forest",
                "orange-peel",
                "dragonfruit",
                "lava"
            ]
        );
        let expected = [
            0xe8d6b7, 0x4db8ff, 0xd6d0c4, 0x6fce88, 0xff9a45, 0xff66aa, 0xff6b52,
        ];
        for (theme, accent) in ThemeId::ALL.into_iter().zip(expected) {
            let p = theme.palette();
            assert_eq!(p.accent, accent);
            assert_eq!(p.library_accent, accent);
            assert_ne!(p.control_bg, p.control_text);
            assert_eq!(ThemeId::parse(theme.as_str()), Some(theme));
        }
        assert_eq!(ThemeId::parse("unknown"), None);
        assert!(ThemeId::Light.palette().light);
        assert!(!ThemeId::Forest.palette().light);
    }
    #[test]
    fn typed_partial_persistence_retains_unknown_keys_and_maps_separate_library_theme() {
        let mut cfg = serde_json::json!({"appearance":{"theme":"forest","future":"keep"},"privateFutureKey":"do-not-drop","credentials":{"token":"synthetic"},"profile":[1,2]});
        let mut prefs = ThemePreferences::from_config(&cfg);
        assert_eq!(prefs.snapshot().app, ThemeId::Forest);
        assert_eq!(prefs.snapshot().library, ThemeId::Forest);
        prefs.set_app(ThemeId::Light);
        prefs.set_library(ThemeId::Lava);
        merge_patch(&mut cfg, &prefs.patch());
        assert_eq!(cfg["appearance"]["theme"], "light");
        assert_eq!(cfg["appearance"]["libraryTheme"], "lava");
        assert_eq!(cfg["appearance"]["future"], "keep");
        assert_eq!(cfg["privateFutureKey"], "do-not-drop");
        assert_eq!(cfg["credentials"]["token"], "synthetic");
        assert_eq!(cfg["profile"], serde_json::json!([1, 2]));
        assert_eq!(
            ThemePreferences::from_config(&cfg).snapshot().library,
            ThemeId::Lava
        );
    }
    #[test]
    fn defaults_are_offline_dark_and_updates_are_snapshotted() {
        let mut prefs = ThemePreferences::default();
        let old = prefs.snapshot();
        assert_eq!(old.app, ThemeId::Dark);
        assert_eq!(old.library, ThemeId::Dark);
        assert!(prefs.set_app(ThemeId::Forest));
        assert_eq!(prefs.snapshot().revision, old.revision + 1);
    }
}
