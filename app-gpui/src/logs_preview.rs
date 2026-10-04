//! LogsView.vue parity using only explicitly synthetic, memory-only preview records.
use crate::page_palette::PagePalette;
use gpui::{
    ClipboardItem, Context, FontWeight, Render, Window, div, linear_color_stop, linear_gradient,
    prelude::*, px, rgb, rgba,
};

#[derive(Clone)]
struct PreviewLogFile {
    name: String,
    lines: Vec<String>,
}
#[derive(Clone)]
struct PreviewCrash {
    name: String,
    pipeline: String,
    source: String,
    timestamp: String,
    bytes: usize,
    file: String,
}

fn crash_pipeline(raw: &str) -> &'static str {
    match raw.trim().to_lowercase().as_str() {
        "vkd3d" => "VKD3D",
        "d3d9" | "m9" | "dxvk" | "dxvk_32" => "D3D9",
        "dxmt" | "m10" | "m11" => "DXMT",
        "dxmt_32" | "m10_32" | "m11_32" => "DXMT(32)",
        "fna_arm64" | "fna_x86" => "FNA/Mono",
        "d3dmetal" => "D3DMetal",
        "m13" => "M13",
        "system" => "System",
        _ => "Other",
    }
}

pub struct LogsPreview {
    pub palette: PagePalette,
    lines: Vec<String>,
    files: Vec<PreviewLogFile>,
    crashes: Vec<PreviewCrash>,
    open: [bool; 3],
    live_scroll: gpui::ScrollHandle,
    notice: Option<&'static str>,
    live: bool,
    active: bool,
    polling: bool,
    line_count: u64,
}

impl LogsPreview {
    pub fn attach_live(&mut self, cx: &mut gpui::Context<Self>) {
        if crate::live::Live::get(cx).is_none() {
            return;
        }
        self.live = true;
        self.lines.clear();
        self.files.clear();
        self.crashes.clear();
    }

    /// LogsView mount/unmount: poll `/logs/stream` every 2 s while visible.
    pub fn set_active(&mut self, active: bool, cx: &mut gpui::Context<Self>) {
        if !self.live || self.active == active {
            return;
        }
        self.active = active;
        if !active {
            return;
        }
        self.poll_logs(cx);
        self.load_crash_reports(cx);
        self.load_log_files(cx);
        if self.polling {
            return;
        }
        self.polling = true;
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(std::time::Duration::from_secs(2))
                    .await;
                let keep = this.update(cx, |this, cx| {
                    if !this.active {
                        this.polling = false;
                        return false;
                    }
                    this.poll_logs(cx);
                    true
                });
                if !matches!(keep, Ok(true)) {
                    break;
                }
            }
        })
        .detach();
    }

    fn poll_logs(&mut self, cx: &mut Context<Self>) {
        let after = self.line_count;
        crate::live::call(
            cx,
            "GET",
            format!("/logs/stream?after={after}"),
            None,
            crate::live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                let Some(result) = result.filter(crate::live::is_ok) else {
                    return;
                };
                if let Some(lines) = result.get("lines").and_then(serde_json::Value::as_array) {
                    if !lines.is_empty() {
                        this.lines
                            .extend(lines.iter().filter_map(|l| l.as_str().map(str::to_owned)));
                        if this.open[0] {
                            this.live_scroll.scroll_to_bottom();
                        }
                    }
                }
                if let Some(total) = result.get("total").and_then(serde_json::Value::as_u64) {
                    this.line_count = total;
                }
                cx.notify();
            },
        );
    }

    fn load_crash_reports(&mut self, cx: &mut Context<Self>) {
        crate::live::call(
            cx,
            "GET",
            "/logs/crash-reports",
            None,
            crate::live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                let Some(result) = result.filter(crate::live::is_ok) else {
                    return;
                };
                let Some(reports) = result.get("reports").and_then(serde_json::Value::as_array)
                else {
                    return;
                };
                if reports.is_empty() {
                    return;
                }
                let text = |v: &serde_json::Value, k: &str| {
                    v.get(k)
                        .and_then(serde_json::Value::as_str)
                        .unwrap_or("")
                        .to_owned()
                };
                this.crashes = reports
                    .iter()
                    .take(20)
                    .map(|r| PreviewCrash {
                        name: text(r, "name"),
                        pipeline: crash_pipeline(&text(r, "pipeline")).to_owned(),
                        source: text(r, "source"),
                        timestamp: text(r, "timestamp"),
                        bytes: r
                            .get("size_bytes")
                            .and_then(serde_json::Value::as_u64)
                            .unwrap_or(0) as usize,
                        file: text(r, "file"),
                    })
                    .collect();
                cx.notify();
            },
        );
    }

    fn load_log_files(&mut self, cx: &mut Context<Self>) {
        crate::live::call(
            cx,
            "GET",
            "/logs",
            None,
            crate::live::DEFAULT_TIMEOUT,
            |this, result, cx| {
                let Some(result) = result.filter(crate::live::is_ok) else {
                    return;
                };
                this.files = result
                    .get("logs")
                    .and_then(serde_json::Value::as_array)
                    .into_iter()
                    .flatten()
                    .map(|file| PreviewLogFile {
                        name: file
                            .get("name")
                            .and_then(serde_json::Value::as_str)
                            .unwrap_or("")
                            .to_owned(),
                        lines: file
                            .get("lines")
                            .and_then(serde_json::Value::as_array)
                            .into_iter()
                            .flatten()
                            .filter_map(|l| l.as_str().map(str::to_owned))
                            .collect(),
                    })
                    .collect();
                cx.notify();
            },
        );
    }

    pub fn new() -> Self {
        let mut lines = vec![
            "[PREVIEW] Synthetic diagnostics only — no production logs are read.".into(),
            "[12:00:00] engine: D3DMetal · bundled preview runtime".into(),
            "[12:00:01] [LAUNCH] Portal 2 · simulated launch request".into(),
            "[12:00:02] [LAUNCHED] Portal 2 · preview session active".into(),
            "[12:00:05] warning: preview executable selection is memory-only".into(),
            "[12:00:09] [STOP] Portal 2 · simulated stop request".into(),
            "[12:00:10] [STOPPED] Portal 2 · preview session ended".into(),
            "[12:00:12] [LAUNCH FAILED] Synthetic error example; no executable was started".into(),
        ];
        for index in 0..64 {
            lines.push(format!("[12:01:{:02}] [PREVIEW] Sample diagnostic record {:02} · library artwork loaded locally", index % 60, index + 1));
        }
        let files = vec![
            PreviewLogFile {
                name: "preview-runtime.log".into(),
                lines: lines.iter().take(8).cloned().collect(),
            },
            PreviewLogFile {
                name: "preview-launch.log".into(),
                lines: vec![
                    "[PREVIEW] engine: DXMT · synthetic launch diagnostics".into(),
                    "[PREVIEW] [LAUNCHED] Hades · simulated session".into(),
                    "[PREVIEW] [STOPPED] Hades · no real process was started".into(),
                ],
            },
        ];
        Self {
            palette: PagePalette::default(),
            lines,
            files,
            open: [false; 3],
            live_scroll: gpui::ScrollHandle::new(),
            notice: None,
            live: false,
            active: false,
            polling: false,
            line_count: 0,
            crashes: vec![
                PreviewCrash {
                    name: "preview-vkd3d.ips".into(),
                    pipeline: "VKD3D".into(),
                    source: "Synthetic macOS report".into(),
                    timestamp: "2026-10-03 12:00:12".into(),
                    bytes: 18432,
                    file: "preview://crash-reports/preview-vkd3d.ips".into(),
                },
                PreviewCrash {
                    name: "preview-dxmt.ips".into(),
                    pipeline: "DXMT".into(),
                    source: "Synthetic Wine report".into(),
                    timestamp: "2026-10-03 11:59:45".into(),
                    bytes: 32768,
                    file: "preview://crash-reports/preview-dxmt.ips".into(),
                },
                PreviewCrash {
                    name: "preview-d3dmetal.ips".into(),
                    pipeline: "D3DMetal".into(),
                    source: "Synthetic macOS report".into(),
                    timestamp: "2026-10-03 11:58:00".into(),
                    bytes: 8192,
                    file: "preview://crash-reports/preview-d3dmetal.ips".into(),
                },
            ],
        }
    }

    #[cfg_attr(not(test), allow(dead_code))]
    pub fn append_preview_event(&mut self, message: impl Into<String>) {
        self.lines.push(message.into());
        // Cap synthetic history so a long UI-review session does not accumulate unbounded memory.
        if self.lines.len() > 1000 {
            self.lines.drain(..self.lines.len() - 1000);
        }
        if self.open[0] {
            self.live_scroll.scroll_to_bottom();
        }
    }

    fn clear_view(&mut self) {
        self.line_count = 0;
        self.lines.clear();
        self.files.clear();
        self.crashes.clear();
        self.notice = None;
    }

    fn copy_live(&self, cx: &mut Context<Self>) {
        if !self.lines.is_empty() {
            cx.write_to_clipboard(ClipboardItem::new_string(self.lines.join("\n")));
        }
    }

    fn section_button(
        &self,
        index: usize,
        label: &'static str,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        self.palette
            .button(("logs-section", index), label, false)
            .mr(px(12.0))
            .h(px(30.0))
            .rounded(px(7.0))
            .bg(rgba(0x00000000))
            .on_click(cx.listener(move |this, _, _, cx| {
                this.open[index] = !this.open[index];
                if index == 0 && this.open[index] {
                    this.live_scroll.scroll_to_bottom();
                }
                cx.notify();
            }))
    }

    fn summary(
        &self,
        index: usize,
        label: &'static str,
        count: String,
        cx: &mut Context<Self>,
    ) -> gpui::Stateful<gpui::Div> {
        div()
            .id(("logs-drawer-summary", index))
            .flex_none()
            .flex()
            .items_center()
            .gap(px(12.0))
            .px(px(16.0))
            .py(px(13.0))
            .text_size(px(13.0))
            .font_weight(FontWeight::BOLD)
            .text_color(rgb(0xeeeeed))
            .cursor_pointer()
            .hover(|style| style.bg(rgba(0xffffff08)))
            .on_click(cx.listener(move |this, _, _, cx| {
                this.open[index] = !this.open[index];
                if index == 0 && this.open[index] {
                    this.live_scroll.scroll_to_bottom();
                }
                cx.notify();
            }))
            .child(label)
            .child(
                div()
                    .ml_auto()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(self.palette.accent))
                    .child(count),
            )
            .child(
                div()
                    .text_color(rgba(0xffffff66))
                    .child(if self.open[index] { "⌄" } else { "›" }),
            )
    }

    fn drawer(&self, live: bool) -> gpui::Div {
        div()
            .flex_none()
            .w_full()
            .flex()
            .flex_col()
            .rounded(px(12.0))
            .border_1()
            .border_color(if live {
                rgba((self.palette.accent << 8) | 0x66)
            } else {
                rgba(0xffffff14)
            })
            .bg(rgba(0xffffff06))
            .overflow_hidden()
            .text_color(rgb(0xcfd1d0))
    }
}

fn log_color(line: &str, accent: u32) -> u32 {
    if line.contains("[LAUNCH]") || line.contains("[LAUNCHED]") {
        0x7cbf6a
    } else if line.contains("[STOP]") || line.contains("[STOPPED]") {
        0xffb84d
    } else if line.contains("[STOP FAILED]") || line.contains("[LAUNCH FAILED]") {
        0xff5c5c
    } else if line.contains("engine:") {
        accent
    } else if ["crash", "error", "failed"]
        .iter()
        .any(|word| line.to_lowercase().contains(word))
    {
        0xffb84d
    } else {
        0xc9ceca
    }
}

fn format_bytes(bytes: usize) -> String {
    if bytes < 1024 {
        format!("{bytes} B")
    } else if bytes < 1024 * 1024 {
        format!("{:.1} KB", bytes as f32 / 1024.0)
    } else {
        format!("{:.1} MB", bytes as f32 / (1024.0 * 1024.0))
    }
}

impl Render for LogsPreview {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let viewport = window.viewport_size();
        let palette = self.palette;
        let action = |id: &'static str, label: &'static str| {
            palette
                .button(id, label, false)
                .mr(px(12.0))
                .rounded(px(6.0))
                .h(px(30.0))
                .bg(rgba(0x00000000))
        };
        let mut controls = div().flex().items_center().justify_between().flex_wrap().gap(px(16.0))
            .child(div().flex().items_center()
                .child(self.section_button(0, "Live", cx)).child(self.section_button(1, "Crash Reports", cx)).child(self.section_button(2, "Log Files", cx)))
            .child(div().flex().items_center()
                .child(action("logs-open-folder", "Open Logs").on_click(cx.listener(|this, _, _, cx| {
                    if let Some(live) = crate::live::Live::get(cx) {
                        let _ = crate::host_actions::open_folder(&live.home().join("logs"));
                        return;
                    }
                    this.notice = Some("Isolated preview: Open Logs is simulated; no production log folder is accessed."); cx.notify();
                })))
                .child(action("logs-copy", "Copy").opacity(if self.lines.is_empty() { 0.4 } else { 1.0 })
                    .on_click(cx.listener(|this, _, _, cx| { this.copy_live(cx); })))
                .child(action("logs-clear", "Clear View").on_click(cx.listener(|this, _, _, cx| { this.clear_view(); cx.notify(); }))));
        if let Some(notice) = self.notice {
            controls = controls.child(
                div()
                    .w_full()
                    .text_size(px(11.0))
                    .text_color(rgb(0xaeb3b2))
                    .child(notice),
            );
        }
        let header = div()
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
                    .flex()
                    .flex_col()
                    .child(
                        div()
                            .mb(px(10.0))
                            .font_family("Georgia")
                            .text_size(px(10.0))
                            .text_color(rgba(0xf0efe799))
                            .child("M E T A L S H A R P  ·  D I A G N O S T I C S"),
                    )
                    .child(
                        div()
                            .font_family("Georgia")
                            .font_weight(FontWeight::MEDIUM)
                            .text_size(px((f32::from(viewport.width) * 0.036).clamp(34.0, 54.0)))
                            .line_height(px((f32::from(viewport.width) * 0.036).clamp(34.0, 54.0)))
                            .text_color(rgb(0xeee9dd))
                            .child("LOGS"),
                    )
                    .child(
                        div()
                            .mt(px(8.0))
                            .text_size(px(13.5))
                            .text_color(rgb(0xaeb3b2))
                            .child("Live MetalSharp runtime logs"),
                    ),
            )
            .child(controls);
        let mut live = self.drawer(true).shadow_lg().child(self.summary(
            0,
            "Live log stream",
            format!("{} lines", self.lines.len()),
            cx,
        ));
        if self.open[0] {
            let mut stream = div()
                .id("logs-live-lines")
                .flex_none()
                .h(px((f32::from(viewport.height) * 0.40).clamp(180.0, 380.0)))
                .overflow_y_scroll()
                .track_scroll(&self.live_scroll)
                .bg(rgb(0x0c0f11))
                .border_t_1()
                .border_color(rgba(0xffffff12))
                .py(px(14.0))
                .px(px(16.0))
                .font_family("Menlo")
                .text_size(px(12.0))
                .line_height(px(21.0));
            for (index, line) in self.lines.iter().enumerate() {
                stream = stream.child(
                    div()
                        .id(("logs-line", index))
                        .text_color(rgb(log_color(line, palette.accent)))
                        .child(line.clone()),
                );
            }
            live = live
                .child(
                    div()
                        .flex_none()
                        .flex()
                        .gap(px(8.0))
                        .py(px(8.0))
                        .px(px(12.0))
                        .border_t_1()
                        .border_color(rgba(0xffffff12))
                        .child(
                            action("logs-live-copy", "Copy")
                                .opacity(if self.lines.is_empty() { 0.4 } else { 1.0 })
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.copy_live(cx);
                                })),
                        ),
                )
                .child(stream);
        }
        let mut crashes = self.drawer(false).child(self.summary(
            1,
            "Crash Reports",
            self.crashes.len().to_string(),
            cx,
        ));
        if self.open[1] {
            if self.crashes.is_empty() {
                crashes = crashes.child(
                    div()
                        .py(px(18.0))
                        .px(px(16.0))
                        .text_size(px(12.0))
                        .text_color(rgb(0x8f958f))
                        .text_center()
                        .child("No crash reports found."),
                );
            } else {
                for pipeline in [
                    "VKD3D", "D3D9", "DXMT", "DXMT(32)", "FNA/Mono", "D3DMetal", "M13", "System",
                    "Other",
                ] {
                    let reports: Vec<_> = self
                        .crashes
                        .iter()
                        .filter(|report| report.pipeline.as_str() == pipeline)
                        .collect();
                    if reports.is_empty() {
                        continue;
                    }
                    crashes = crashes.child(
                        div()
                            .pt(px(10.0))
                            .px(px(16.0))
                            .pb(px(4.0))
                            .text_size(px(11.0))
                            .font_weight(FontWeight::BOLD)
                            .text_color(rgb(palette.accent))
                            .child(pipeline),
                    );
                    for report in reports {
                        crashes = crashes.child(
                            div()
                                .flex_none()
                                .flex()
                                .flex_col()
                                .gap(px(2.0))
                                .py(px(10.0))
                                .px(px(16.0))
                                .border_t_1()
                                .border_color(rgba(0xffffff0f))
                                .text_size(px(12.0))
                                .child(
                                    div()
                                        .font_weight(FontWeight::BOLD)
                                        .text_color(rgb(0xf0f0ed))
                                        .child(report.name.clone()),
                                )
                                .child(div().text_color(rgb(0x8f958f)).child(format!(
                                    "{} - {} - {}",
                                    report.source,
                                    report.timestamp,
                                    format_bytes(report.bytes)
                                )))
                                .child(
                                    div()
                                        .text_size(px(10.0))
                                        .text_color(rgb(0x8f958f))
                                        .child(report.file.clone()),
                                ),
                        );
                    }
                }
            }
        }
        let mut drawers = div()
            .flex_none()
            .w_full()
            .flex()
            .flex_col()
            .gap(px(12.0))
            .child(crashes);
        if !self.files.is_empty() {
            let mut files = self.drawer(false).child(self.summary(
                2,
                "Recent log files",
                self.files.len().to_string(),
                cx,
            ));
            if self.open[2] {
                for (index, file) in self.files.iter().enumerate() {
                    files = files.child(
                        div()
                            .flex_none()
                            .py(px(12.0))
                            .px(px(16.0))
                            .border_t_1()
                            .border_color(rgba(0xffffff0f))
                            .child(
                                div()
                                    .mb(px(8.0))
                                    .text_size(px(12.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(rgb(0xe6e8e6))
                                    .child(file.name.clone()),
                            )
                            .child(
                                div()
                                    .id(("logs-file-content", index))
                                    .max_h(px(180.0))
                                    .overflow_y_scroll()
                                    .p(px(12.0))
                                    .border_1()
                                    .border_color(rgba(0xffffff12))
                                    .rounded(px(8.0))
                                    .bg(rgb(0x0c0f11))
                                    .font_family("Menlo")
                                    .text_size(px(10.5))
                                    .line_height(px(16.8))
                                    .text_color(rgb(0xb9bfb9))
                                    .children(
                                        file.lines
                                            .iter()
                                            .rev()
                                            .take(40)
                                            .collect::<Vec<_>>()
                                            .into_iter()
                                            .rev()
                                            .map(|line| div().child(line.clone())),
                                    ),
                            ),
                    );
                }
            }
            drawers = drawers.child(files);
        }
        div()
            .id("logs-page")
            .relative()
            .overflow_hidden()
            .size_full()
            .min_h_0()
            .flex()
            .flex_col()
            .bg(linear_gradient(
                180.0,
                linear_color_stop(rgb(0x171a1d), 0.0),
                linear_color_stop(rgb(0x111416), 1.0),
            ))
            .child(
                div()
                    .absolute()
                    .top(px(-24.0))
                    .left(gpui::relative(0.15))
                    .w(gpui::relative(0.70))
                    .h(px(1.0))
                    .shadow(vec![gpui::BoxShadow {
                        color: rgba((palette.accent << 8) | 0x17).into(),
                        offset: gpui::point(px(0.0), px(0.0)),
                        blur_radius: px(72.0),
                        spread_radius: px(20.0),
                    }]),
            )
            .child(header)
            .child(
                div()
                    .id("logs-page-scroll")
                    .flex_1()
                    .min_h_0()
                    .overflow_y_scroll()
                    .flex()
                    .flex_col()
                    .gap(px(16.0))
                    .pt(px(16.0))
                    .px(px(28.0))
                    .pb(px(24.0))
                    .child(live)
                    .child(drawers),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn log_event_colors_follow_original_priority() {
        assert_eq!(log_color("[LAUNCHED] preview", 0x123456), 0x7cbf6a);
        assert_eq!(log_color("[STOPPED] preview", 0x123456), 0xffb84d);
        assert_eq!(log_color("[LAUNCH FAILED] error", 0x123456), 0xff5c5c);
        assert_eq!(log_color("engine: error", 0x123456), 0x123456);
        assert_eq!(log_color("warning: crash", 0x123456), 0xffb84d);
    }
    #[test]
    fn clearing_only_removes_preview_memory_and_event_history_is_bounded() {
        let mut logs = LogsPreview::new();
        assert_eq!(logs.open, [false; 3]);
        assert!(!logs.lines.is_empty() && !logs.crashes.is_empty() && !logs.files.is_empty());
        logs.clear_view();
        assert!(logs.lines.is_empty() && logs.crashes.is_empty() && logs.files.is_empty());
        for index in 0..1100 {
            logs.append_preview_event(format!("[PREVIEW] {index}"));
        }
        assert_eq!(logs.lines.len(), 1000);
        assert_eq!(format_bytes(18432), "18.0 KB");
    }
}
