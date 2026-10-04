//! Native localization service backed by the source-extracted production catalogues.
//! `snapshot()` is an owned render snapshot; callers should replace it after `set_language`.
use std::collections::HashMap;

pub const LANGUAGES: [(&str, &str); 20] = [
    ("en", "English"),
    ("zh-CN", "简体中文"),
    ("es", "Español"),
    ("hi", "हिन्दी"),
    ("ar", "العربية"),
    ("pt-BR", "Português (Brasil)"),
    ("bn", "বাংলা"),
    ("ru", "Русский"),
    ("ja", "日本語"),
    ("pa", "ਪੰਜਾਬੀ"),
    ("de", "Deutsch"),
    ("jv", "Basa Jawa"),
    ("ko", "한국어"),
    ("fr", "Français"),
    ("te", "తెలుగు"),
    ("vi", "Tiếng Việt"),
    ("tr", "Türkçe"),
    ("ur", "اردو"),
    ("it", "Italiano"),
    ("mr", "मराठी"),
];

#[derive(Clone, Debug)]
pub struct Catalogues(HashMap<String, HashMap<String, String>>);
fn flatten(prefix: &str, value: &serde_json::Value, output: &mut HashMap<String, String>) {
    match value {
        serde_json::Value::String(text) => {
            output.insert(prefix.to_owned(), text.clone());
        }
        serde_json::Value::Object(children) => {
            for (key, child) in children {
                flatten(
                    if prefix.is_empty() {
                        key.clone()
                    } else {
                        format!("{prefix}.{key}")
                    }
                    .as_str(),
                    child,
                    output,
                );
            }
        }
        serde_json::Value::Array(children) => {
            for (index, child) in children.iter().enumerate() {
                flatten(&format!("{prefix}.{index}"), child, output);
            }
        }
        _ => {}
    }
}
fn read_catalogue(raw: &str) -> HashMap<String, HashMap<String, String>> {
    let parsed: serde_json::Value = serde_json::from_str(raw).expect("locale catalogue JSON");
    parsed
        .as_object()
        .expect("locale object")
        .iter()
        .map(|(locale, value)| {
            let mut strings = HashMap::new();
            flatten("", value, &mut strings);
            (locale.clone(), strings)
        })
        .collect()
}
impl Default for Catalogues {
    fn default() -> Self {
        let mut data = read_catalogue(include_str!("../assets/settings-locales.json"));
        for (locale, entries) in read_catalogue(include_str!("../assets/setup-locales.json")) {
            data.entry(locale).or_default().extend(entries);
        }
        Self(data)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LocaleSnapshot {
    pub language: String,
    pub revision: u64,
}
#[derive(Clone, Debug)]
pub struct Localization {
    catalogues: Catalogues,
    language: String,
    revision: u64,
}
impl Default for Localization {
    fn default() -> Self {
        Self::new("en")
    }
}
impl Localization {
    pub fn new(language: &str) -> Self {
        Self::with_catalogues(language, Catalogues::default())
    }
    pub fn with_catalogues(language: &str, catalogues: Catalogues) -> Self {
        let language = normalize_language(language);
        Self {
            catalogues,
            language,
            revision: 0,
        }
    }
    pub fn language(&self) -> &str {
        &self.language
    }
    pub fn snapshot(&self) -> LocaleSnapshot {
        LocaleSnapshot {
            language: self.language.clone(),
            revision: self.revision,
        }
    }
    /// Returns true and advances the revision only when effective locale changes.
    pub fn set_language(&mut self, language: &str) -> bool {
        let normalized = normalize_language(language);
        if normalized == self.language {
            return false;
        }
        self.language = normalized;
        self.revision = self.revision.wrapping_add(1);
        true
    }
    /// Vue-i18n-style key lookup with English then key fallback; replacements are plain text,
    /// never interpreted as markup. `{count}` also supports the ICU-independent count form.
    pub fn text(&self, key: &str, params: &[(&str, &str)]) -> String {
        let value = self
            .catalogues
            .0
            .get(&self.language)
            .and_then(|m| m.get(key))
            .or_else(|| self.catalogues.0.get("en").and_then(|m| m.get(key)));
        let mut out = value.cloned().unwrap_or_else(|| key.to_owned());
        for (name, value) in params {
            out = out.replace(&format!("{{{name}}}"), value);
        }
        out
    }
    /// Select a singular/plural message explicitly for callers whose source has two branches.
    pub fn plural(&self, key: &str, count: i64, one: &str, other: &str) -> String {
        self.text(key, &[("count", if count == 1 { one } else { other })])
    }
}
pub fn normalize_language(input: &str) -> String {
    let normalized = input.trim().replace('_', "-");
    if normalized.eq_ignore_ascii_case("zh")
        || normalized.eq_ignore_ascii_case("zh-hans")
        || normalized.eq_ignore_ascii_case("zh-cn")
    {
        return "zh-CN".into();
    }
    if normalized.eq_ignore_ascii_case("pt") || normalized.eq_ignore_ascii_case("pt-br") {
        return "pt-BR".into();
    }
    LANGUAGES
        .iter()
        .find(|(code, _)| code.eq_ignore_ascii_case(&normalized))
        .map(|(code, _)| (*code).to_owned())
        .unwrap_or_else(|| "en".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn catalogue_has_all_source_locales_and_native_sources() {
        let catalogues = Catalogues::default();
        for (locale, _) in LANGUAGES {
            let entries = &catalogues.0[locale];
            assert!(entries.len() >= 77, "{locale}");
            assert!(!entries["language.label"].is_empty());
        }
        assert_eq!(catalogues.0.len(), 20);
    }
    #[test]
    fn normalization_fallback_and_live_snapshots() {
        let mut service = Localization::new(" PT_br ");
        assert_eq!(service.language(), "pt-BR");
        let before = service.snapshot();
        assert!(service.set_language("zh_hans"));
        let after = service.snapshot();
        assert_eq!(after.language, "zh-CN");
        assert_eq!(after.revision, before.revision + 1);
        assert!(!service.set_language("ZH-cn"));
        assert_eq!(service.text("no.such.key", &[]), "no.such.key");
        assert_eq!(normalize_language("xx-YY"), "en");
    }
    #[test]
    fn interpolation_is_literal_and_plural_branch_receives_count() {
        let service = Localization::default();
        assert_eq!(
            service.text("stepOf", &[("step", "<1&>"), ("total", "3")]),
            "Step <1&> of 3"
        );
        let plural_catalogue = Catalogues(HashMap::from([(
            "en".to_owned(),
            HashMap::from([("items".to_owned(), "{count} item(s)".to_owned())]),
        )]));
        let plural_service = Localization::with_catalogues("en", plural_catalogue);
        assert_eq!(
            plural_service.plural("items", 1, "one", "many"),
            "one item(s)"
        );
        assert_eq!(
            plural_service.plural("items", 2, "one", "many"),
            "many item(s)"
        );
        assert_eq!(service.text("ui.settings.apiKey", &[]), "Steam Web API Key");
    }
}
