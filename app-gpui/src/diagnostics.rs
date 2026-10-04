//! Bounded backend diagnostics DTOs. Credential-bearing lines are removed before
//! entering view state or clipboard export. This is defense-in-depth, not proof
//! that every third-party runtime log has been audited for arbitrary secrets.
use serde::{Deserialize, Deserializer, Serialize};

#[derive(Clone, Default, Deserialize, Serialize)]
pub struct DiagnosticLogs {
    #[serde(default)]
    pub logs: Vec<DiagnosticFile>,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct DiagnosticFile {
    pub name: String,
    #[serde(default, deserialize_with = "read_redacted_lines")]
    pub lines: Vec<String>,
}
fn read_redacted_lines<'de, D: Deserializer<'de>>(reader: D) -> Result<Vec<String>, D::Error> {
    let lines = Vec::<String>::deserialize(reader)?;
    let start = lines.len().saturating_sub(500);
    Ok(lines
        .into_iter()
        .skip(start)
        .map(|line| redact_line(&line))
        .collect())
}
pub(crate) fn redact_line(line: &str) -> String {
    let folded = line.to_ascii_lowercase();
    const MARKERS: &[&str] = &[
        "token",
        "password",
        "passwd",
        "api_key",
        "apikey",
        "api key",
        "authorization",
        "auth_code",
        "authcode",
        "client_secret",
        "bearer",
        "cookie",
        "--code",
        "?code=",
        "&code=",
        "?state=",
        "&state=",
    ];
    if MARKERS.iter().any(|marker| folded.contains(marker)) {
        "[redacted credential-bearing diagnostic]".into()
    } else if let Some((end, _)) = line.char_indices().nth(4096) {
        format!("{}…", &line[..end])
    } else {
        line.to_owned()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn backend_array_contract_is_decoded_and_redacted_before_view_state() {
        let logs:DiagnosticLogs=serde_json::from_str(r#"{"ok":true,"logs":[{"name":"fixture.log","lines":["runtime ready","Authorization: Bearer fixture-private","https://example.invalid/?code=fixture-private"]}]}"#).unwrap();
        assert_eq!(logs.logs[0].lines[0], "runtime ready");
        let exported = serde_json::to_string(&logs).unwrap();
        assert!(!exported.contains("fixture-private"));
        assert!(exported.contains("redacted"));
    }
    #[test]
    fn empty_logs_and_unrelated_diagnostics_are_preserved() {
        assert!(
            serde_json::from_str::<DiagnosticLogs>(r#"{"ok":true,"logs":[]}"#)
                .unwrap()
                .logs
                .is_empty()
        );
        assert_eq!(redact_line("DXMT frame complete"), "DXMT frame complete");
        assert_eq!(
            redact_line("api_key=fixture"),
            "[redacted credential-bearing diagnostic]"
        );
    }
}
