//! Game Settings > Advanced > Mullion: switches for Mullion's features a player may want per game, kept as variables
//! in the game's own environment variables (`/game-tuning`'s `env`), so the field shows what a switch did and a
//! variable typed there turns its switch on. Mullion reads them where it starts (Direct3D 10 to 12 on the DXMT and
//! Mullion routes); other routes ignore them.

/// One switch: its variable, the value that turns it on, and whether it is on without the variable.
pub struct Switch {
    pub id: &'static str,
    pub title: &'static str,
    pub hint: &'static str,
    pub variable: &'static str,
    /// the value the switch sets when it is on, or (default_on) off
    pub value: &'static str,
    pub default_on: bool,
}

pub const SWITCHES: &[Switch] = &[
    Switch {
        id: "mullion-memory-saver",
        title: "Memory saver",
        hint: "A Direct3D 12 game's large HDR textures take half the memory with lossy compression, at a small cost \
               in precision (M2 and later). For a game that runs out of memory on this Mac.",
        variable: "MULLION_LOSSY_TEXTURES",
        value: "1",
        default_on: false,
    },
    Switch {
        id: "mullion-keep-wired",
        title: "Keep memory ready while idle",
        hint: "Keeps the game's GPU memory in place through pauses, loading screens and time in the background, so its \
               first frames after are not slow.",
        variable: "MULLION_RESIDENCY_HEARTBEAT",
        value: "0",
        default_on: true,
    },
    Switch {
        id: "mullion-detailed-log",
        title: "Detailed log",
        hint: "Mullion's log also notes GPU errors, memory, slow shaders and late frames, for a problem report.",
        variable: "MULLION_TRACE_GPU",
        value: "1",
        default_on: false,
    },
];

/// The variables the detailed log sets beside its own.
const DETAILED_LOG_ALSO: &[&str] = &[
    "MULLION_TRACE_MEMORY",
    "MULLION_TRACE_SHADER",
    "MULLION_TRACE_PRESENT",
];

/// The text's variables, split as the backend splits them: on spaces, but not inside double quotes.
fn tokens(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    for c in text.chars() {
        match c {
            '"' => {
                quoted = !quoted;
                current.push(c);
            }
            c if c.is_whitespace() && !quoted => {
                if !current.is_empty() {
                    out.push(std::mem::take(&mut current));
                }
            }
            c => current.push(c),
        }
    }
    if !current.is_empty() {
        out.push(current);
    }
    out
}

fn names(token: &str, name: &str) -> bool {
    token == format!("-{name}") || token.split_once('=').is_some_and(|(key, _)| key == name)
}

/// The value the text gives a variable, if it gives one.
pub fn value_of(text: &str, name: &str) -> Option<String> {
    tokens(text).into_iter().rev().find_map(|token| {
        token
            .split_once('=')
            .filter(|(key, _)| *key == name)
            .map(|(_, v)| v.trim_matches('"').to_owned())
    })
}

/// The text with a variable set to a value, or taken out (None), keeping every other variable as it was written.
pub fn with_variable(text: &str, name: &str, value: Option<&str>) -> String {
    let mut kept: Vec<String> = tokens(text)
        .into_iter()
        .filter(|token| !names(token, name))
        .collect();
    if let Some(value) = value {
        kept.push(format!("{name}={value}"));
    }
    kept.join(" ")
}

impl Switch {
    pub fn is_on(&self, text: &str) -> bool {
        match value_of(text, self.variable) {
            Some(value) => (value == self.value) != self.default_on,
            None => self.default_on,
        }
    }

    /// The text with the switch turned to `on`.
    pub fn turned(&self, text: &str, on: bool) -> String {
        let set = (on != self.default_on).then_some(self.value);
        let mut text = with_variable(text, self.variable, set);
        if self.variable == "MULLION_TRACE_GPU" {
            for also in DETAILED_LOG_ALSO {
                text = with_variable(&text, also, set);
            }
        }
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_switch_adds_and_removes_only_its_variable() {
        let saver = &SWITCHES[0];
        let text = r#"DXVK_HUD=fps NAME="a value""#;
        assert!(!saver.is_on(text));
        let on = saver.turned(text, true);
        assert_eq!(
            on,
            r#"DXVK_HUD=fps NAME="a value" MULLION_LOSSY_TEXTURES=1"#
        );
        assert!(saver.is_on(&on));
        assert_eq!(saver.turned(&on, false), r#"DXVK_HUD=fps NAME="a value""#);
    }

    #[test]
    fn a_switch_on_by_default_writes_its_off_value() {
        let wired = &SWITCHES[1];
        assert!(wired.is_on(""));
        let off = wired.turned("", false);
        assert_eq!(off, "MULLION_RESIDENCY_HEARTBEAT=0");
        assert!(!wired.is_on(&off));
        assert_eq!(wired.turned(&off, true), "");
    }

    #[test]
    fn the_detailed_log_sets_its_categories_together() {
        let log = &SWITCHES[2];
        let on = log.turned("-WINEDEBUG", true);
        assert!(
            on.starts_with("-WINEDEBUG MULLION_TRACE_GPU=1")
                && on.contains("MULLION_TRACE_PRESENT=1")
        );
        assert_eq!(log.turned(&on, false), "-WINEDEBUG");
    }

    #[test]
    fn a_removed_variable_turns_its_switch_back() {
        assert!(!SWITCHES[0].is_on("-MULLION_LOSSY_TEXTURES"));
        assert_eq!(
            with_variable(
                "-MULLION_LOSSY_TEXTURES A=1",
                "MULLION_LOSSY_TEXTURES",
                None
            ),
            "A=1"
        );
        assert_eq!(value_of(r#"X="1 2""#, "X").as_deref(), Some("1 2"));
    }
}
