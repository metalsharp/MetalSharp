//! Connected Logs drawer model. All backend text is bounded and marker-redacted
//! before it becomes view, clipboard, or export data. This cannot detect every
//! arbitrary secret emitted by third-party software.
use crate::{
    backend::{BackendClient, BackendError},
    diagnostics::redact_line,
};
use gpui::{ClipboardItem, Context, FontWeight, Render, Window, div, prelude::*, px, rgb, rgba};
use serde::{Deserialize, Serialize};
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Component, Path, PathBuf},
};

const MAX_LIVE_LINES: usize = 2_000;
const MAX_FILE_LINES: usize = 40;
const MAX_FILE_COUNT: usize = 8;
const MAX_CRASH_COUNT: usize = 20;
const MAX_LINE_BYTES: usize = 4_096;
const MAX_EXPORT_BYTES: usize = 2 * 1024 * 1024;

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct LiveLogBatch {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub total: u64,
    #[serde(default)]
    pub lines: Vec<String>,
}
#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct CrashReports {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub reports: Vec<CrashReport>,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CrashReport {
    #[serde(default)]
    pub file: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub source: String,
    #[serde(default)]
    pub pipeline: String,
    #[serde(default)]
    pub timestamp: String,
    #[serde(default)]
    pub size_bytes: u64,
}
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct RecentLogFile {
    pub name: String,
    pub lines: Vec<String>,
}
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct LogsSnapshot {
    pub live: Vec<String>,
    pub cursor: u64,
    pub recent: Vec<RecentLogFile>,
    pub crashes: Vec<CrashReport>,
    pub source_errors: Vec<(String, String)>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum LoadState {
    Idle,
    Loading,
    Ready,
    Error(String),
}
impl Default for LoadState {
    fn default() -> Self {
        Self::Idle
    }
}

impl LogsSnapshot {
    /// Perform the three bounded backend reads. Call on a background executor.
    pub fn load(client: &BackendClient, cursor: u64) -> Result<Self, BackendError> {
        let mut source_errors = Vec::new();
        let mut stream = match client.log_stream(cursor) {
            Ok(mut stream) => {
                // The C endpoint offset is scoped to today's log file. On rollover,
                // total decreases; restart from zero to avoid a blank live drawer.
                if stream.total < cursor {
                    match client.log_stream(0) {
                        Ok(restarted) => stream = restarted,
                        Err(error) => source_errors.push(("Live logs".into(), error.to_string())),
                    }
                }
                Some(stream)
            }
            Err(error) => {
                source_errors.push(("Live logs".into(), error.to_string()));
                None
            }
        };
        let live = stream
            .as_mut()
            .map(|batch| {
                std::mem::take(&mut batch.lines)
                    .into_iter()
                    .map(|line| bounded_redacted(&line))
                    .collect()
            })
            .unwrap_or_default();
        let next_cursor = stream.map_or(cursor, |batch| batch.total);
        let files = match client.diagnostic_logs() {
            Ok(files) => Some(files),
            Err(error) => {
                source_errors.push(("Recent log files".into(), error.to_string()));
                None
            }
        };
        let recent = files
            .into_iter()
            .flat_map(|files| files.logs)
            .into_iter()
            .take(MAX_FILE_COUNT)
            .map(|file| RecentLogFile {
                name: safe_label(&file.name),
                lines: file
                    .lines
                    .into_iter()
                    .rev()
                    .take(MAX_FILE_LINES)
                    .collect::<Vec<_>>()
                    .into_iter()
                    .rev()
                    .map(|line| bounded_redacted(&line))
                    .collect(),
            })
            .collect();
        // Crash metadata is only accepted when the backend's file path resolves
        // to a regular file rooted inside its reported data home. Never open it.
        let root = client
            .backend_home()
            .ok()
            .and_then(|home| fs::canonicalize(home).ok());
        if root.is_none() {
            source_errors.push((
                "Crash reports".into(),
                "Could not resolve backend data home".into(),
            ));
        }
        let crashes = match client.crash_reports() {
            Ok(crashes) if crashes.ok => crashes.reports,
            Ok(_) => {
                source_errors.push(("Crash reports".into(), BackendError::Rejected.to_string()));
                Vec::new()
            }
            Err(error) => {
                source_errors.push(("Crash reports".into(), error.to_string()));
                Vec::new()
            }
        }
        .into_iter()
        .filter(|r| {
            root.as_ref()
                .is_some_and(|root| owned_regular_file(root, Path::new(&r.file)))
        })
        .take(MAX_CRASH_COUNT)
        .map(|mut r| {
            r.name = bounded_redacted(&safe_label(&r.name));
            r.source = bounded_redacted(&safe_label(&r.source));
            r.pipeline = bounded_redacted(&safe_label(&r.pipeline));
            r.timestamp = bounded_redacted(&safe_label(&r.timestamp));
            r.file = bounded_redacted(&safe_relative_display(
                root.as_deref().expect("root filtered"),
                Path::new(&r.file),
            ));
            r
        })
        .collect();
        Ok(Self {
            live,
            cursor: next_cursor,
            recent,
            crashes,
            source_errors,
        })
    }
}

/// Typed intent emitted by the view. The shared connected actor owns dispatch,
/// client lifetime, serialization, and host-only actions.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum LogsIntent {
    Refresh { after: u64 },
    OpenLogsFolder,
    Export { destination: PathBuf },
}
fn bounded_redacted(line: &str) -> String {
    let line = redact_line(line);
    if line.len() <= MAX_LINE_BYTES {
        line
    } else {
        let mut end = MAX_LINE_BYTES;
        while !line.is_char_boundary(end) {
            end -= 1;
        }
        format!("{}…", &line[..end])
    }
}
fn safe_label(value: &str) -> String {
    let leaf = Path::new(value)
        .file_name()
        .and_then(|v| v.to_str())
        .unwrap_or("diagnostic");
    leaf.chars().filter(|c| !c.is_control()).take(180).collect()
}
fn owned_regular_file(root: &Path, path: &Path) -> bool {
    if !path.is_absolute() {
        return false;
    }
    let Ok(canonical) = fs::canonicalize(path) else {
        return false;
    };
    canonical.starts_with(root) && fs::metadata(canonical).is_ok_and(|m| m.is_file())
}
fn safe_relative_display(root: &Path, path: &Path) -> String {
    fs::canonicalize(path)
        .ok()
        .and_then(|p| p.strip_prefix(root).ok().map(Path::to_path_buf))
        .and_then(|p| p.to_str().map(str::to_owned))
        .unwrap_or_else(|| "owned crash report".into())
}

/// UI-owned state. `clear_view` never mutates files and advances polling to the
/// observed backend cursor so old events do not immediately reappear.
#[derive(Clone, Default)]
pub struct LogsController {
    pub snapshot: LogsSnapshot,
    pub state: LoadState,
    pub filter: String,
    pub hidden_before_cursor: u64,
}
impl LogsController {
    pub fn begin_refresh(&mut self) {
        self.state = LoadState::Loading;
    }
    pub fn accept_refresh(&mut self, result: Result<LogsSnapshot, BackendError>) {
        match result {
            Ok(mut next) => {
                let mut combined = std::mem::take(&mut self.snapshot.live);
                combined.extend(next.live);
                if combined.len() > MAX_LIVE_LINES {
                    combined.drain(..combined.len() - MAX_LIVE_LINES);
                }
                next.live = combined;
                self.state = if next.source_errors.is_empty() {
                    LoadState::Ready
                } else {
                    LoadState::Error(
                        next.source_errors
                            .iter()
                            .map(|(source, error)| format!("{source}: {error}"))
                            .collect::<Vec<_>>()
                            .join(" · "),
                    )
                };
                self.snapshot = next;
            }
            Err(error) => self.state = LoadState::Error(error.to_string()),
        }
    }
    pub fn filtered_live(&self) -> impl Iterator<Item = &String> {
        let needle = self.filter.to_lowercase();
        self.snapshot
            .live
            .iter()
            .filter(move |line| needle.is_empty() || line.to_lowercase().contains(&needle))
    }
    pub fn filtered_recent<'a>(
        &self,
        file: &'a RecentLogFile,
    ) -> impl Iterator<Item = &'a String> + 'a {
        let needle = self.filter.to_lowercase();
        file.lines
            .iter()
            .filter(move |line| needle.is_empty() || line.to_lowercase().contains(&needle))
    }
    pub fn filtered_crashes(&self) -> impl Iterator<Item = &CrashReport> {
        let needle = self.filter.to_lowercase();
        self.snapshot.crashes.iter().filter(move |r| {
            needle.is_empty()
                || [
                    r.name.as_str(),
                    r.pipeline.as_str(),
                    r.source.as_str(),
                    r.timestamp.as_str(),
                ]
                .iter()
                .any(|s| s.to_lowercase().contains(&needle))
        })
    }
    pub fn clear_view(&mut self) {
        self.hidden_before_cursor = self.snapshot.cursor;
        self.snapshot.live.clear();
        self.snapshot.recent.clear();
        self.snapshot.crashes.clear();
        self.state = LoadState::Ready;
    }
    pub fn copy_text(&self) -> Option<String> {
        let text = self.filtered_live().cloned().collect::<Vec<_>>().join("\n");
        (!text.is_empty()).then_some(text)
    }
    pub fn export_text(&self) -> Result<String, ExportError> {
        let mut out = String::from(
            "MetalSharp diagnostics export (marker-redacted; arbitrary third-party secrets may remain)\n\nLive logs\n",
        );
        for line in self.filtered_live() {
            out.push_str(&bounded_redacted(line));
            out.push('\n');
        }
        out.push_str("\nRecent log files\n");
        for file in &self.snapshot.recent {
            out.push_str(&safe_label(&file.name));
            out.push('\n');
            for line in file.lines.iter().rev().take(MAX_FILE_LINES).rev() {
                out.push_str(&bounded_redacted(line));
                out.push('\n');
            }
        }
        out.push_str("\nCrash report metadata (report contents are not exported)\n");
        for report in &self.snapshot.crashes {
            out.push_str(&format!(
                "{} | {} | {} | {} | {} bytes | {}\n",
                safe_label(&report.pipeline),
                safe_label(&report.name),
                safe_label(&report.source),
                safe_label(&report.timestamp),
                report.size_bytes,
                safe_label(&report.file)
            ));
        }
        if out.len() > MAX_EXPORT_BYTES {
            return Err(ExportError::TooLarge);
        }
        Ok(out)
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ExportError {
    UnsafeDestination,
    AlreadyExists,
    Io,
    TooLarge,
}

/// Write only redacted view data; create-new semantics avoid silently replacing
/// an existing user file. Existing symlinks and paths inside the MetalSharp home
/// are rejected, preventing accidental overwrite of logs/runtime-owned files.
pub fn export_redacted(
    controller: &LogsController,
    destination: &Path,
    metalsharp_home: &Path,
) -> Result<(), ExportError> {
    let name = destination
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or(ExportError::UnsafeDestination)?;
    if name.is_empty()
        || name.starts_with('.')
        || name.chars().any(char::is_control)
        || destination
            .components()
            .any(|c| matches!(c, Component::ParentDir))
        || destination.exists()
        || fs::symlink_metadata(destination).is_ok()
    {
        return Err(if destination.exists() {
            ExportError::AlreadyExists
        } else {
            ExportError::UnsafeDestination
        });
    }
    let parent = destination.parent().ok_or(ExportError::UnsafeDestination)?;
    let parent = fs::canonicalize(parent).map_err(|_| ExportError::UnsafeDestination)?;
    let home = fs::canonicalize(metalsharp_home).map_err(|_| ExportError::UnsafeDestination)?;
    if parent.starts_with(&home) {
        return Err(ExportError::UnsafeDestination);
    }
    let path: PathBuf = parent.join(name);
    let text = controller.export_text()?;
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|_| ExportError::Io)?;
    file.write_all(text.as_bytes())
        .map_err(|_| ExportError::Io)?;
    file.sync_all().map_err(|_| ExportError::Io)?;
    Ok(())
}

/// Connected counterpart to the synthetic preview. The parent owns navigation and
/// decides when to construct this entity; ordinary preview never creates it.
pub struct LogsConnected {
    pub controller: LogsController,
    pending_intent: Option<LogsIntent>,
    filter_input: gpui::Entity<crate::search_input::SearchInput>,
    live_open: bool,
    crashes_open: bool,
    recent_open: bool,
}
impl LogsConnected {
    pub fn new(cx: &mut Context<Self>) -> Self {
        let filter_input = cx.new(|cx| {
            let mut input = crate::search_input::SearchInput::new(cx);
            input.placeholder = "Filter logs".into();
            input.text_color = 0xcfd1d0;
            input.placeholder_color = 0x8f958f;
            input
        });
        Self {
            controller: LogsController::default(),
            pending_intent: None,
            filter_input,
            live_open: true,
            crashes_open: false,
            recent_open: false,
        }
    }
    pub fn request_initial_refresh(&mut self, cx: &mut Context<Self>) {
        self.refresh(cx);
    }
    pub fn take_intent(&mut self) -> Option<LogsIntent> {
        self.pending_intent.take()
    }
    /// Called by the parent shared actor on its background executor.
    pub fn execute(
        client: &BackendClient,
        intent: LogsIntent,
    ) -> Result<LogsSnapshot, BackendError> {
        match intent {
            LogsIntent::Refresh { after } => LogsSnapshot::load(client, after),
            LogsIntent::OpenLogsFolder => Ok(LogsSnapshot::default()),
            LogsIntent::Export { .. } => Ok(LogsSnapshot::default()),
        }
    }
    pub fn apply_refresh(
        &mut self,
        result: Result<LogsSnapshot, BackendError>,
        cx: &mut Context<Self>,
    ) {
        self.controller.accept_refresh(result);
        cx.notify();
    }
    /// Parent actor executes an export intent off the UI thread using its current
    /// owned client; home discovery is always backend `/status`, never env/OS data.
    pub fn execute_export(
        client: &BackendClient,
        controller: &LogsController,
        destination: &Path,
    ) -> Result<(), ExportError> {
        let home = client
            .backend_home()
            .map_err(|_| ExportError::UnsafeDestination)?;
        export_redacted(controller, destination, &home)
    }
    pub fn request_open_logs_folder(&mut self, cx: &mut Context<Self>) {
        self.pending_intent = Some(LogsIntent::OpenLogsFolder);
        cx.notify();
    }
    pub fn export(&mut self, cx: &mut Context<Self>) {
        cx.spawn(async move |this, cx| {
            let destination = rfd::AsyncFileDialog::new()
                .set_file_name("metalsharp-diagnostics.txt")
                .save_file()
                .await;
            let Some(destination) = destination else {
                return;
            };
            let _ = this.update(cx, |this, cx| {
                this.pending_intent = Some(LogsIntent::Export {
                    destination: destination.path().to_path_buf(),
                });
                cx.notify();
            });
        })
        .detach();
    }
    pub fn refresh(&mut self, cx: &mut Context<Self>) {
        self.controller.begin_refresh();
        let cursor = self.controller.snapshot.cursor;
        self.pending_intent = Some(LogsIntent::Refresh { after: cursor });
        cx.notify();
    }
    fn section_button(
        &self,
        label: &'static str,
        open: bool,
        id: &'static str,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(id)
            .px(px(14.0))
            .py(px(9.0))
            .rounded(px(7.0))
            .border_1()
            .border_color(rgba(0xffffff24))
            .text_color(rgb(0xcfd1d0))
            .cursor_pointer()
            .child(label)
            .on_click(cx.listener(move |this, _, _, cx| {
                match id {
                    "logs-live" => this.live_open = !this.live_open,
                    "logs-crashes" => this.crashes_open = !this.crashes_open,
                    _ => this.recent_open = !this.recent_open,
                }
                cx.notify();
            }))
            .opacity(if open { 1.0 } else { 0.8 })
    }
    fn action(
        label: &'static str,
        id: &'static str,
        cx: &mut Context<Self>,
        callback: impl Fn(&mut Self, &mut Context<Self>) + 'static,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(id)
            .px(px(12.0))
            .py(px(8.0))
            .rounded(px(6.0))
            .border_1()
            .border_color(rgba(0xffffff24))
            .text_color(rgb(0xcfd1d0))
            .cursor_pointer()
            .child(label)
            .on_click(cx.listener(move |this, _, _, cx| callback(this, cx)))
    }
}
impl Render for LogsConnected {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.controller.filter = self.filter_input.read(cx).content.to_string();
        let filtered: Vec<String> = self.controller.filtered_live().cloned().collect();
        let filtered_count = filtered.len();
        let crashes: Vec<CrashReport> = self.controller.filtered_crashes().cloned().collect();
        let mut recent = div().flex().flex_col().gap(px(8.0));
        for file in &self.controller.snapshot.recent {
            recent = recent.child(
                div()
                    .p(px(10.0))
                    .border_1()
                    .border_color(rgba(0xffffff12))
                    .child(safe_label(&file.name))
                    .child(
                        div()
                            .mt(px(6.0))
                            .text_size(px(11.0))
                            .text_color(rgb(0xb9bfb9))
                            .children(
                                self.controller
                                    .filtered_recent(file)
                                    .map(|line| div().child(line.clone())),
                            ),
                    ),
            );
        }
        let has_crashes = !crashes.is_empty();
        let mut crash_rows = div().flex().flex_col();
        for report in crashes {
            crash_rows = crash_rows.child(
                div()
                    .p(px(10.0))
                    .border_b_1()
                    .border_color(rgba(0xffffff12))
                    .child(format!(
                        "{} · {} · {} · {} bytes",
                        report.pipeline, report.name, report.timestamp, report.size_bytes
                    ))
                    .child(
                        div()
                            .text_size(px(10.0))
                            .text_color(rgb(0x8f958f))
                            .child(report.file),
                    ),
            );
        }
        let state = match &self.controller.state {
            LoadState::Idle => "Not loaded".to_owned(),
            LoadState::Loading => "Loading diagnostics…".into(),
            LoadState::Ready => String::new(),
            LoadState::Error(e) => format!("Could not load diagnostics: {e}"),
        };
        div()
            .id("connected-logs")
            .size_full()
            .min_h_0()
            .flex()
            .flex_col()
            .gap(px(0.0))
            .bg(rgb(0x111416))
            .text_color(rgb(0xeeeeed))
            .child(
                div()
                    .flex_none()
                    .flex()
                    .flex_col()
                    .gap(px(14.0))
                    .pt(px(26.0))
                    .px(px(28.0))
                    .pb(px(16.0))
                    .border_b_1()
                    .border_color(rgba(0xffffff12))
                    .child(
                        div()
                            .font_family("Georgia")
                            .text_size(px(10.0))
                            .text_color(rgba(0xf0efe799))
                            .child("M E T A L S H A R P  ·  D I A G N O S T I C S"),
                    )
                    .child(
                        div()
                            .font_family("Georgia")
                            .font_weight(FontWeight::MEDIUM)
                            .text_size(px(44.0))
                            .line_height(px(48.0))
                            .text_color(rgb(0xeee9dd))
                            .child("LOGS"),
                    )
                    .child(
                        div()
                            .text_size(px(13.5))
                            .text_color(rgb(0xaeb3b2))
                            .child("Live MetalSharp runtime logs"),
                    ),
            )
            .child(
                div()
                    .flex()
                    .gap(px(8.0))
                    .child(self.section_button("Live", self.live_open, "logs-live", cx))
                    .child(self.section_button(
                        "Crash Reports",
                        self.crashes_open,
                        "logs-crashes",
                        cx,
                    ))
                    .child(self.section_button("Recent Files", self.recent_open, "logs-recent", cx))
                    .child(Self::action(
                        "Open Logs",
                        "logs-open-folder",
                        cx,
                        |this, cx| this.request_open_logs_folder(cx),
                    ))
                    .child(Self::action("Refresh", "logs-refresh", cx, |this, cx| {
                        this.refresh(cx)
                    }))
                    .child(Self::action("Copy", "logs-copy", cx, |this, cx| {
                        if let Some(text) = this.controller.copy_text() {
                            cx.write_to_clipboard(ClipboardItem::new_string(text));
                        }
                    }))
                    .child(Self::action("Export", "logs-export", cx, |this, cx| {
                        this.export(cx)
                    }))
                    .child(Self::action("Clear View", "logs-clear", cx, |this, cx| {
                        this.controller.clear_view();
                        cx.notify();
                    })),
            )
            .child(
                div()
                    .w(px(320.0))
                    .rounded(px(6.0))
                    .border_1()
                    .border_color(rgba(0xffffff24))
                    .child(self.filter_input.clone()),
            )
            .child(
                div()
                    .text_size(px(12.0))
                    .text_color(rgb(0xaeb3b2))
                    .child(state),
            )
            .child(
                div()
                    .id("logs-connected-scroll")
                    .flex_1()
                    .min_h_0()
                    .overflow_y_scroll()
                    .flex()
                    .flex_col()
                    .gap(px(16.0))
                    .pt(px(16.0))
                    .px(px(28.0))
                    .pb(px(24.0))
                    .child(if self.live_open {
                        div()
                            .p(px(12.0))
                            .rounded(px(10.0))
                            .border_1()
                            .border_color(rgba(0xffffff18))
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .children(if filtered.is_empty() {
                                vec![
                                    div()
                                        .text_color(rgb(0x8f958f))
                                        .child("No live log lines match this filter."),
                                ]
                            } else {
                                filtered
                                    .into_iter()
                                    .map(|line| {
                                        div()
                                            .text_size(px(11.0))
                                            .text_color(rgb(0xc9ceca))
                                            .child(line)
                                    })
                                    .collect()
                            })
                    } else {
                        div()
                    })
                    .child(if self.crashes_open {
                        if !has_crashes {
                            div().child("No crash reports match this filter.")
                        } else {
                            crash_rows
                        }
                    } else {
                        div()
                    })
                    .child(if self.recent_open {
                        if self.controller.snapshot.recent.is_empty() {
                            div().child("No recent log files.")
                        } else {
                            recent
                        }
                    } else {
                        div()
                    }),
            )
            .child(
                div()
                    .flex_none()
                    .flex()
                    .justify_between()
                    .px(px(28.0))
                    .py(px(10.0))
                    .border_t_1()
                    .border_color(rgba(0xffffff12))
                    .text_size(px(11.0))
                    .text_color(rgb(0x8f958f))
                    .child(format!(
                        "{} live · {} crash reports · {} recent files",
                        filtered_count,
                        self.controller.snapshot.crashes.len(),
                        self.controller.snapshot.recent.len()
                    ))
                    .child("Clear View only clears this session's display"),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> LogsController {
        let mut c = LogsController::default();
        c.snapshot = LogsSnapshot {
            live: vec![
                "one".into(),
                "Authorization: Bearer fixture-secret".into(),
                "Two".into(),
            ],
            cursor: 42,
            recent: vec![RecentLogFile {
                name: "../../fixture.log".into(),
                lines: vec!["api_key=fixture-secret".into(), "safe".into()],
            }],
            crashes: vec![CrashReport {
                file: "/private/home/logs/crash.log".into(),
                name: "crash.log".into(),
                source: "fixture".into(),
                pipeline: "DXMT".into(),
                timestamp: "2026-01-01".into(),
                size_bytes: 12,
            }],
            source_errors: Vec::new(),
        };
        c.state = LoadState::Ready;
        c
    }
    #[test]
    fn filter_copy_export_are_bounded_and_redacted() {
        let mut c = fixture();
        assert_eq!(c.copy_text().unwrap().lines().count(), 3);
        c.filter = "two".into();
        assert_eq!(c.copy_text().unwrap(), "Two");
        c.filter.clear();
        let export = c.export_text().unwrap();
        assert!(export.contains("redacted credential-bearing diagnostic"));
        assert!(!export.contains("fixture-secret"));
        assert!(export.contains("report contents are not exported"));
        assert!(!export.contains("/private/home"));
    }
    #[gpui::test]
    fn virtual_window_emits_parent_owned_actions_and_renders_drawers(
        cx: &mut gpui::TestAppContext,
    ) {
        let window = cx.add_window(|_, cx| LogsConnected::new(cx));
        window
            .update(cx, |view, window, cx| {
                view.request_initial_refresh(cx);
                assert_eq!(view.take_intent(), Some(LogsIntent::Refresh { after: 0 }));
                view.request_open_logs_folder(cx);
                assert_eq!(view.take_intent(), Some(LogsIntent::OpenLogsFolder));
                let _rendered = view.render(window, cx);
            })
            .unwrap();
    }
    #[test]
    fn offset_rollover_and_bounded_window_are_explicit() {
        let mut controller = LogsController::default();
        controller.snapshot.cursor = 90;
        controller.accept_refresh(Ok(LogsSnapshot {
            cursor: 2,
            live: vec!["new-day-a".into(), "new-day-b".into()],
            ..Default::default()
        }));
        assert_eq!(controller.snapshot.cursor, 2);
        assert_eq!(controller.snapshot.live, ["new-day-a", "new-day-b"]);
        let bounded = (0..MAX_LIVE_LINES + 10)
            .map(|i| i.to_string())
            .collect::<Vec<_>>();
        controller.accept_refresh(Ok(LogsSnapshot {
            live: bounded,
            cursor: 3,
            ..Default::default()
        }));
        assert_eq!(controller.snapshot.live.len(), MAX_LIVE_LINES);
        assert_eq!(controller.snapshot.live.first().unwrap(), "10");
    }
    #[test]
    fn refresh_accumulates_bounded_live_lines_and_errors_are_visible() {
        let mut c = fixture();
        c.accept_refresh(Ok(LogsSnapshot {
            live: vec!["new".into()],
            cursor: 43,
            ..Default::default()
        }));
        assert_eq!(c.snapshot.live.len(), 4);
        c.accept_refresh(Err(BackendError::Transport));
        assert!(matches!(c.state, LoadState::Error(_)));
    }
    #[test]
    fn clear_view_only_clears_memory_and_retains_cursor() {
        let mut c = fixture();
        c.clear_view();
        assert!(
            c.snapshot.live.is_empty()
                && c.snapshot.recent.is_empty()
                && c.snapshot.crashes.is_empty()
        );
        assert_eq!(c.hidden_before_cursor, 42);
        assert_eq!(c.snapshot.cursor, 42);
    }
    #[test]
    fn export_destination_is_create_new_and_outside_owned_home() {
        let root = std::env::temp_dir().join(format!("gpui-log-fixture-{}", std::process::id()));
        let home = root.join("home");
        let output = root.join("output");
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&home).unwrap();
        fs::create_dir_all(&output).unwrap();
        let dest = output.join("diagnostics.txt");
        export_redacted(&fixture(), &dest, &home).unwrap();
        assert!(
            !fs::read_to_string(&dest)
                .unwrap()
                .contains("fixture-secret")
        );
        assert_eq!(
            export_redacted(&fixture(), &dest, &home),
            Err(ExportError::AlreadyExists)
        );
        assert_eq!(
            export_redacted(&fixture(), &home.join("logs.txt"), &home),
            Err(ExportError::UnsafeDestination)
        );
        fs::remove_dir_all(root).unwrap();
    }
}
