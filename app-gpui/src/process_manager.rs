//! Electron Process Manager (`ProcessManagerOverlay.vue` plus the main-process
//! `process-manager:*` handlers): Cmd+P toggles a 720×540 live-session HUD that
//! samples `metalsharp-process-manager-helper` every 1.5 s, drives MetalFX and
//! force-kills non-Steam Wine game processes.
use crate::live::{self, Live};
use gpui::{
    App, AppContext, Bounds, Context, FocusHandle, FontWeight, Hsla, KeyDownEvent, Render,
    SharedString, Window, WindowBounds, WindowHandle, WindowOptions, div, img, linear_color_stop,
    linear_gradient, point, prelude::*, px, rgb, rgba, size,
};
use serde_json::{Value, json};
use std::cell::RefCell;
use std::path::PathBuf;
use std::time::Duration;

const HELPER: &str = "metalsharp-process-manager-helper";
const METALFX_FACTORS: [f64; 2] = [1.75, 2.0];
const MONO: &str = "Menlo";

const ACCENT: u32 = 0x5fb7e8;
const ACCENT_HOVER: u32 = 0x75c7f2;
const ACCENT_DIM: u32 = 0x347fba;
const ERROR: u32 = 0xd66a6a;
const TEXT_PRIMARY: u32 = 0xd7e0ea;
const TEXT_SECONDARY: u32 = 0x9aa8b6;
const TEXT_DIM: u32 = 0x718292;
const TEXT_BRIGHT: u32 = 0xffffff;
const BORDER: u32 = 0x8caac81f;
const BORDER_STRONG: u32 = 0x8caac833;

thread_local! {
    static WINDOW: RefCell<Option<WindowHandle<ProcessManager>>> = const { RefCell::new(None) };
}

/// Poll the Carbon hot-key flag; the handler itself cannot reach the app context.
pub fn install(cx: &mut App) {
    cx.spawn(async move |cx| {
        loop {
            cx.background_executor()
                .timer(Duration::from_millis(80))
                .await;
            if crate::hotkeys::take_process_manager_toggle() && cx.update(|cx| toggle(cx)).is_err()
            {
                break;
            }
        }
    })
    .detach();
}

/// `toggleProcessManagerWindow`: hide when visible, otherwise center and show.
pub fn toggle(cx: &mut App) {
    let existing = WINDOW.with(|slot| *slot.borrow());
    if let Some(handle) = existing {
        let shown = handle.update(cx, |pm, window, cx| {
            if native_visible(window) {
                pm.hide(window, cx);
            } else {
                pm.show(window, cx);
            }
        });
        if shown.is_ok() {
            return;
        }
    }
    open(cx);
}

fn open(cx: &mut App) {
    let bounds = Bounds::centered(None, size(px(720.0), px(540.0)), cx);
    let options = WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(bounds)),
        window_min_size: Some(size(px(480.0), px(420.0))),
        is_resizable: true,
        titlebar: Some(gpui::TitlebarOptions {
            title: Some("MetalSharp Process Manager".into()),
            appears_transparent: true,
            traffic_light_position: Some(point(px(16.0), px(16.0))),
        }),
        ..Default::default()
    };
    let Ok(handle) = cx.open_window(options, |window, cx| {
        cx.new(|cx| {
            let pm = ProcessManager::new(cx);
            window.focus(&pm.focus);
            pm
        })
    }) else {
        eprintln!("MetalSharp Process Manager window could not be opened");
        return;
    };
    WINDOW.with(|slot| *slot.borrow_mut() = Some(handle));
    let _ = handle.update(cx, |pm, window, cx| pm.show(window, cx));
    cx.activate(true);
}

#[cfg(target_os = "macos")]
fn ns_window(window: &Window) -> Option<objc2::rc::Retained<objc2_app_kit::NSWindow>> {
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    let handle = HasWindowHandle::window_handle(window).ok()?;
    let RawWindowHandle::AppKit(appkit) = handle.as_raw() else {
        return None;
    };
    let view: objc2::rc::Retained<objc2_app_kit::NSView> =
        unsafe { objc2::rc::Retained::retain(appkit.ns_view.as_ptr().cast()) }?;
    view.window()
}

fn native_visible(window: &Window) -> bool {
    #[cfg(target_os = "macos")]
    {
        ns_window(window).is_some_and(|w| w.isVisible())
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = window;
        true
    }
}

pub struct ProcessManager {
    focus: FocusHandle,
    sample: Option<Value>,
    sampling: bool,
    gpu_accel_armed: bool,
    status: String,
    metalfx: Option<Value>,
    metalfx_busy: bool,
    factor_menu_open: bool,
    visible: bool,
    polling: bool,
}

impl ProcessManager {
    fn new(cx: &mut Context<Self>) -> Self {
        Self {
            focus: cx.focus_handle(),
            sample: None,
            sampling: true,
            gpu_accel_armed: false,
            status: "Cmd+P toggles this overlay mid-session".into(),
            metalfx: None,
            metalfx_busy: false,
            factor_menu_open: false,
            visible: false,
            polling: false,
        }
    }

    fn show(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        #[cfg(target_os = "macos")]
        if let Some(ns) = ns_window(window) {
            ns.center();
            ns.makeKeyAndOrderFront(None);
        }
        window.activate_window();
        window.focus(&self.focus);
        cx.activate(true);
        if !self.visible {
            self.visible = true;
            // ProcessManagerOverlay onMounted: sample + MetalFX state, then poll.
            self.refresh(cx);
            self.refresh_metalfx(cx);
            self.start_polling(cx);
        }
    }

    /// `process-manager:close` hides the window; the renderer state survives.
    fn hide(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.visible = false;
        self.factor_menu_open = false;
        #[cfg(target_os = "macos")]
        if let Some(ns) = ns_window(window) {
            ns.orderOut(None);
        }
        #[cfg(not(target_os = "macos"))]
        window.remove_window();
        cx.notify();
    }

    fn start_polling(&mut self, cx: &mut Context<Self>) {
        if self.polling {
            return;
        }
        self.polling = true;
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(Duration::from_millis(1500))
                    .await;
                let keep = this
                    .update(cx, |pm, cx| {
                        if pm.visible {
                            pm.refresh(cx);
                        } else {
                            pm.polling = false;
                        }
                        pm.visible
                    })
                    .unwrap_or(false);
                if !keep {
                    break;
                }
            }
        })
        .detach();
    }

    fn refresh(&mut self, cx: &mut Context<Self>) {
        let live = Live::get(cx);
        cx.spawn(async move |this, cx| {
            let sample = cx
                .background_executor()
                .spawn(async move { sample(live.as_ref()) })
                .await;
            let _ = this.update(cx, |pm, cx| {
                pm.sample = Some(sample);
                pm.sampling = false;
                cx.notify();
            });
        })
        .detach();
    }

    fn refresh_metalfx(&mut self, cx: &mut Context<Self>) {
        if Live::get(cx).is_none() {
            return;
        }
        live::call(
            cx,
            "GET",
            "/metalfx/state",
            None,
            live::DEFAULT_TIMEOUT,
            |pm, r, cx| {
                if let Some(state) = r.map(live::unwrap_data) {
                    pm.metalfx = Some(state);
                    cx.notify();
                }
            },
        );
    }

    fn metalfx_enabled(&self) -> bool {
        self.metalfx
            .as_ref()
            .and_then(|m| m.get("enabled"))
            .and_then(Value::as_bool)
            .unwrap_or(false)
    }

    fn metalfx_factor(&self) -> f64 {
        self.metalfx
            .as_ref()
            .and_then(|m| m.get("factor"))
            .and_then(Value::as_f64)
            .unwrap_or(2.0)
    }

    fn post_metalfx(
        &mut self,
        enabled: bool,
        factor: f64,
        message: impl FnOnce(bool, Option<String>) -> String + 'static,
        cx: &mut Context<Self>,
    ) {
        if self.metalfx_busy || Live::get(cx).is_none() {
            return;
        }
        self.metalfx_busy = true;
        cx.notify();
        live::call(
            cx,
            "POST",
            "/metalfx/toggle",
            Some(json!({ "enabled": enabled, "factor": factor })),
            live::DEFAULT_TIMEOUT,
            move |pm, r, cx| {
                let r = r.map(live::unwrap_data);
                let ok = r.as_ref().is_some_and(live::is_ok);
                let error = live::error_text(r.as_ref());
                if let Some(state) = r {
                    pm.metalfx = Some(state);
                }
                pm.status = message(ok, error);
                pm.metalfx_busy = false;
                cx.notify();
            },
        );
    }

    fn toggle_metalfx(&mut self, enabled: bool, cx: &mut Context<Self>) {
        let factor = self.metalfx_factor();
        self.post_metalfx(
            enabled,
            factor,
            move |ok, error| {
                if ok {
                    format!(
                        "MetalFX {} — applies on next swapchain recreate (alt-enter/scene change)",
                        if enabled { "enabled" } else { "disabled" }
                    )
                } else {
                    format!(
                        "MetalFX toggle failed: {}",
                        error.unwrap_or_else(|| "unknown".into())
                    )
                }
            },
            cx,
        );
    }

    fn set_metalfx_factor(&mut self, factor: f64, cx: &mut Context<Self>) {
        self.factor_menu_open = false;
        let enabled = self.metalfx_enabled();
        self.post_metalfx(
            enabled,
            factor,
            move |ok, error| {
                if ok {
                    format!(
                        "MetalFX factor {factor:.2}× saved — applies on relaunch (DXMT Config reloads at launch)"
                    )
                } else {
                    format!(
                        "MetalFX factor failed: {}",
                        error.unwrap_or_else(|| "unknown".into())
                    )
                }
            },
            cx,
        );
    }

    fn toggle_gpu_acceleration(&mut self, cx: &mut Context<Self>) {
        self.gpu_accel_armed = !self.gpu_accel_armed;
        self.status = format!(
            "GPU acceleration visual surface {}; runtime hook pending",
            if self.gpu_accel_armed {
                "armed"
            } else {
                "parked"
            }
        );
        cx.notify();
    }

    fn quit_game(&mut self, cx: &mut Context<Self>) {
        self.status = "Force-killing non-Steam Wine game PIDs...".into();
        cx.notify();
        cx.spawn(async move |this, cx| {
            let result = cx
                .background_executor()
                .spawn(async move { quit_non_steam_wine_games() })
                .await;
            let _ = this.update(cx, |pm, cx| {
                match result {
                    Ok(()) => {
                        pm.status = "Non-Steam Wine game kill signal sent".into();
                        pm.refresh(cx);
                    }
                    Err(error) => pm.status = error,
                }
                cx.notify();
            });
        })
        .detach();
    }

    fn close(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.hide(window, cx);
    }
}

fn helper_candidates(live: Option<&Live>) -> Vec<PathBuf> {
    let native = |base: PathBuf| base.join("scripts/tools/native").join(HELPER);
    let mut candidates = Vec::new();
    if let Some(live) = live {
        candidates.push(native(live.resources()));
    }
    if let Some(exe_dir) = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(PathBuf::from))
    {
        candidates.push(native(exe_dir.join("../Resources")));
    }
    if let Some(live) = live {
        candidates.push(native(live.home()));
    }
    candidates.push(
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../app/native")
            .join(HELPER),
    );
    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.join("native").join(HELPER));
    }
    candidates
}

fn shell_path() -> String {
    let mut parts: Vec<String> = [
        "/opt/homebrew/bin",
        "/usr/local/bin",
        "/usr/bin",
        "/bin",
        "/usr/sbin",
        "/sbin",
    ]
    .map(str::to_owned)
    .to_vec();
    if let Ok(existing) = std::env::var("PATH") {
        for part in existing.split(':') {
            if !part.is_empty() && !parts.iter().any(|p| p == part) {
                parts.push(part.to_owned());
            }
        }
    }
    parts.join(":")
}

/// Run a helper with Electron's 2.5 s `execFile` timeout.
fn run_helper(path: &std::path::Path) -> Option<Value> {
    use std::io::Read;
    let mut child = std::process::Command::new(path)
        .env("PATH", shell_path())
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .ok()?;
    let mut stdout = child.stdout.take()?;
    let reader = std::thread::spawn(move || {
        let mut text = String::new();
        let _ = stdout.read_to_string(&mut text);
        text
    });
    let deadline = std::time::Instant::now() + Duration::from_millis(2500);
    loop {
        match child.try_wait() {
            Ok(Some(status)) if status.success() => break,
            Ok(Some(_)) | Err(_) => return None,
            Ok(None) if std::time::Instant::now() >= deadline => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
            Ok(None) => std::thread::sleep(Duration::from_millis(20)),
        }
    }
    let parsed: Value = serde_json::from_str(&reader.join().ok()?).ok()?;
    parsed.is_object().then_some(parsed)
}

/// `processManagerSample`: first working helper, else the JS fallback numbers.
fn sample(live: Option<&Live>) -> Value {
    for candidate in helper_candidates(live) {
        if !candidate.exists() {
            continue;
        }
        if let Some(mut result) = run_helper(&candidate) {
            result["helper_path"] = json!(candidate.to_string_lossy());
            return result;
        }
    }
    fallback_sample()
}

#[cfg(target_os = "macos")]
fn sysctl_u64(name: &str) -> Option<u64> {
    let name = std::ffi::CString::new(name).ok()?;
    let mut value: u64 = 0;
    let mut len = std::mem::size_of::<u64>();
    let status = unsafe {
        libc::sysctlbyname(
            name.as_ptr(),
            (&mut value as *mut u64).cast(),
            &mut len,
            std::ptr::null_mut(),
            0,
        )
    };
    match (status, len) {
        (0, 8) => Some(value),
        (0, 4) => Some(value & 0xffff_ffff),
        _ => None,
    }
}

#[cfg(target_os = "macos")]
fn sysctl_string(name: &str) -> Option<String> {
    let name = std::ffi::CString::new(name).ok()?;
    let mut len = 0usize;
    unsafe {
        if libc::sysctlbyname(
            name.as_ptr(),
            std::ptr::null_mut(),
            &mut len,
            std::ptr::null_mut(),
            0,
        ) != 0
        {
            return None;
        }
        let mut buffer = vec![0u8; len];
        if libc::sysctlbyname(
            name.as_ptr(),
            buffer.as_mut_ptr().cast(),
            &mut len,
            std::ptr::null_mut(),
            0,
        ) != 0
        {
            return None;
        }
        buffer.truncate(len);
        while buffer.last() == Some(&0) {
            buffer.pop();
        }
        String::from_utf8(buffer).ok()
    }
}

/// `jsProcessManagerFallback` (Node `os.totalmem/freemem/loadavg/cpus`).
fn fallback_sample() -> Value {
    #[cfg(target_os = "macos")]
    let (total, free, cores, chip) = {
        let page = unsafe { libc::sysconf(libc::_SC_PAGESIZE) }.max(0) as u64;
        let free_pages = sysctl_u64("vm.page_free_count").unwrap_or(0);
        (
            sysctl_u64("hw.memsize").unwrap_or(0),
            free_pages * page,
            sysctl_u64("hw.ncpu").unwrap_or(1).max(1),
            sysctl_string("machdep.cpu.brand_string").unwrap_or_else(|| "arm64".into()),
        )
    };
    #[cfg(not(target_os = "macos"))]
    let (total, free, cores, chip) = (0u64, 0u64, 1u64, std::env::consts::ARCH.to_owned());
    let mut loads = [0f64; 3];
    let load = if unsafe { libc::getloadavg(loads.as_mut_ptr(), 3) } > 0 {
        loads[0]
    } else {
        0.0
    };
    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    json!({
        "ok": true,
        "source": "metalsharp-process-helper-js-fallback",
        "timestamp": timestamp,
        "fps": null,
        "cpu_percent": (((load / cores as f64) * 1000.0).round() / 10.0).min(100.0),
        "cpu_temp_c": null,
        "cores_used": (load * 10.0).round() / 10.0,
        "cores_total": cores,
        "ram_used_bytes": total.saturating_sub(free),
        "ram_total_bytes": total,
        "gpu_percent": null,
        "gpu_label": "Metal session telemetry hook pending",
        "chip": chip,
        "processes": [],
    })
}

struct ProcessRow {
    pid: i32,
    comm: String,
    command: String,
}

fn parse_process_rows(output: &str) -> Vec<ProcessRow> {
    output
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            let (pid, rest) = line.split_once(char::is_whitespace)?;
            let rest = rest.trim_start();
            let (comm, command) = rest
                .split_once(char::is_whitespace)
                .map_or((rest, ""), |(c, r)| (c, r.trim_start()));
            Some(ProcessRow {
                pid: pid.parse().ok()?,
                comm: comm.to_owned(),
                command: command.to_owned(),
            })
        })
        .collect()
}

fn basename_lower(path: &str) -> String {
    path.rsplit('/').next().unwrap_or(path).to_lowercase()
}

fn is_non_steam_wine_process(row: &ProcessRow) -> bool {
    let command = row.command.to_lowercase();
    let comm = basename_lower(&row.comm);
    let haystack = format!("{comm} {command}");
    let is_steam = haystack.contains("steam");
    let is_wine = comm == "wine"
        || comm == "wineserver"
        || comm.starts_with("wine")
        || command.contains("/wine")
        || command.contains("wineserver")
        || command.contains("wine-preloader")
        || command.contains("wine64-preloader")
        || command.contains("wineboot")
        || command.contains("drive_c/");
    row.pid > 0 && row.pid as u32 != std::process::id() && is_wine && !is_steam
}

/// `processManagerQuitGame`: SIGKILL every non-Steam Wine process.
fn quit_non_steam_wine_games() -> Result<(), String> {
    let output = std::process::Command::new("/bin/ps")
        .args(["-axo", "pid=,comm=,command="])
        .output()
        .map_err(|error| error.to_string())?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_owned());
    }
    let rows = parse_process_rows(&String::from_utf8_lossy(&output.stdout));
    let mut errors = Vec::new();
    for row in rows.iter().filter(|row| is_non_steam_wine_process(row)) {
        if unsafe { libc::kill(row.pid, libc::SIGKILL) } != 0 {
            let error = std::io::Error::last_os_error();
            if error.raw_os_error() != Some(libc::ESRCH) {
                errors.push(format!("{}: {error}", row.pid));
            }
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err("Action failed".into())
    }
}

fn fmt_bytes(value: f64) -> String {
    if !value.is_finite() || value <= 0.0 {
        return "--".into();
    }
    let units = ["B", "KB", "MB", "GB", "TB"];
    let mut n = value;
    let mut i = 0;
    while n >= 1024.0 && i < units.len() - 1 {
        n /= 1024.0;
        i += 1;
    }
    if n >= 10.0 {
        format!("{n:.1} {}", units[i])
    } else {
        format!("{n:.2} {}", units[i])
    }
}

fn num(value: Option<&Value>, key: &str) -> Option<f64> {
    value.and_then(|v| v.get(key)).and_then(Value::as_f64)
}

fn text(value: Option<&Value>, key: &str) -> Option<String> {
    value
        .and_then(|v| v.get(key))
        .and_then(Value::as_str)
        .map(str::to_owned)
}

fn color(hex: u32) -> Hsla {
    rgb(hex).into()
}

impl ProcessManager {
    fn stat_card(
        label: &'static str,
        value: String,
        note: String,
        meter: Option<(f64, u32, u32)>,
        value_color: u32,
    ) -> gpui::Div {
        div()
            .flex_1()
            .min_w_0()
            .min_h(px(84.0))
            .px(px(12.0))
            .py(px(10.0))
            .rounded(px(16.0))
            .border_1()
            .border_color(rgba(BORDER))
            .bg(rgba(0x0d0b179e))
            .child(
                div()
                    .text_size(px(10.0))
                    .text_color(color(TEXT_SECONDARY))
                    .child(label),
            )
            .child(
                div()
                    .mt(px(5.0))
                    .mb(px(3.0))
                    .font_family(MONO)
                    .text_size(px(20.0))
                    .font_weight(FontWeight::BOLD)
                    .text_color(color(value_color))
                    .child(value),
            )
            .child(
                div()
                    .text_size(px(10.0))
                    .text_color(color(TEXT_SECONDARY))
                    .overflow_hidden()
                    .whitespace_nowrap()
                    .text_ellipsis()
                    .child(note),
            )
            .children(meter.map(|(pct, from, to)| {
                div()
                    .mt(px(7.0))
                    .h(px(5.0))
                    .w_full()
                    .rounded_full()
                    .bg(rgba(0x00f5ff1c))
                    .overflow_hidden()
                    .child(
                        div()
                            .h_full()
                            .w(gpui::relative((pct / 100.0) as f32))
                            .rounded_full()
                            .bg(linear_gradient(
                                90.0,
                                linear_color_stop(color(from), 0.0),
                                linear_color_stop(color(to), 1.0),
                            )),
                    )
            }))
    }

    fn action_card(id: &'static str, armed: bool, danger: bool) -> gpui::Stateful<gpui::Div> {
        let border = if danger {
            rgba(0xff4f7761)
        } else if armed {
            rgba(ACCENT << 8 | 0xff)
        } else {
            rgba(0x00f5ff3d)
        };
        div()
            .id(id)
            .flex_1()
            .min_w_0()
            .min_h(px(74.0))
            .px(px(12.0))
            .py(px(10.0))
            .rounded(px(16.0))
            .border_1()
            .border_color(border)
            .bg(linear_gradient(
                135.0,
                linear_color_stop(rgba(0x0f1a2bbd), 0.0),
                linear_color_stop(rgba(0x150f22bd), 1.0),
            ))
            .when(armed, |d| d.shadow_md())
    }

    fn action_text(
        span: &'static str,
        strong: String,
        small: &'static str,
        strong_color: u32,
    ) -> [gpui::Div; 3] {
        [
            div()
                .text_size(px(10.0))
                .text_color(color(TEXT_SECONDARY))
                .child(span),
            div()
                .my(px(5.0))
                .text_size(px(14.0))
                .font_weight(FontWeight::BOLD)
                .text_color(color(strong_color))
                .child(strong),
            div()
                .text_size(px(10.0))
                .text_color(color(TEXT_SECONDARY))
                .child(small),
        ]
    }
}

impl Render for ProcessManager {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let s = self.sample.as_ref();
        let ram_total = num(s, "ram_total_bytes").unwrap_or(0.0);
        let ram_used = num(s, "ram_used_bytes").unwrap_or(0.0);
        let ram_pct = if ram_total > 0.0 {
            (ram_used / ram_total * 100.0).clamp(0.0, 100.0)
        } else {
            0.0
        };
        let cpu_pct = num(s, "cpu_percent").unwrap_or(0.0).clamp(0.0, 100.0);
        let gpu = num(s, "gpu_percent");
        let gpu_pct = gpu.unwrap_or(0.0).clamp(0.0, 100.0);
        let fps = num(s, "fps");
        let fps_label = fps.map_or("WAIT".to_owned(), |f| f.round().to_string());
        let gpu_mem = num(s, "gpu_mem_used_bytes").filter(|b| *b > 0.0);
        let cpu_temp = num(s, "cpu_temp_c");
        let temp_label = match (gpu_mem, cpu_temp) {
            (Some(bytes), _) => {
                let gb = bytes / (1024.0 * 1024.0 * 1024.0);
                if gb >= 1.0 {
                    format!("{gb:.1}GB")
                } else {
                    format!("{}MB", (bytes / (1024.0 * 1024.0)).round())
                }
            }
            (None, Some(temp)) => format!("{}°C", temp.round()),
            (None, None) => "VRAM".into(),
        };
        let temp_note = if gpu_mem.is_some() {
            "allocated VRAM".to_owned()
        } else if cpu_temp.is_none() {
            "unavailable".to_owned()
        } else {
            text(s, "cpu_temp_source").unwrap_or_else(|| "PMU sensor".into())
        };
        let ram_label = if ram_total > 0.0 {
            format!("{} / {}", fmt_bytes(ram_used), fmt_bytes(ram_total))
        } else {
            "--".into()
        };
        let cores_label = match s {
            Some(_) => format!(
                "{:.1} / {}",
                num(s, "cores_used").unwrap_or(0.0),
                num(s, "cores_total").unwrap_or(0.0)
            ),
            None => "--".into(),
        };
        let fps_note = if fps.is_none() {
            "waiting for non-Steam Wine FPS".to_owned()
        } else {
            text(s, "fps_source").unwrap_or_else(|| "frames/sec".into())
        };
        let processes: Vec<Value> = s
            .and_then(|s| s.get("processes"))
            .and_then(Value::as_array)
            .map(|rows| rows.iter().take(10).cloned().collect())
            .unwrap_or_default();
        let metalfx_on = self.metalfx_enabled();
        let factor = self.metalfx_factor();
        let busy = self.metalfx_busy;

        let header = div()
            .flex()
            .items_center()
            .justify_between()
            .gap(px(20.0))
            .pl(px(18.0))
            .pr(px(18.0))
            .pt(px(36.0))
            .pb(px(9.0))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(11.0))
                    .child(
                        div()
                            .size(px(44.0))
                            .rounded(px(14.0))
                            .border_1()
                            .border_color(rgba(0xb9ff4d61))
                            .bg(rgba(0x0d0b17d1))
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(
                                img(crate::ui::asset_path("metalsharp-logo.png")).size(px(32.0)),
                            ),
                    )
                    .child(
                        div()
                            .child(
                                div()
                                    .mb(px(4.0))
                                    .font_family(MONO)
                                    .text_size(px(9.0))
                                    .text_color(color(ACCENT_HOVER))
                                    .child("LIVE SESSION HUD"),
                            )
                            .child(
                                div()
                                    .text_size(px(16.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(color(TEXT_PRIMARY))
                                    .child("MetalSharp Process Manager"),
                            ),
                    ),
            )
            .child(
                div()
                    .id("pm-close")
                    .size(px(32.0))
                    .rounded_full()
                    .border_1()
                    .border_color(rgba(0xff4f7770))
                    .bg(rgba(0xff4f771a))
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_size(px(20.0))
                    .text_color(rgb(0xffd7df))
                    .cursor_pointer()
                    .child("×")
                    .on_click(cx.listener(|pm, _, window, cx| pm.close(window, cx))),
            );

        let stats = div()
            .flex()
            .flex_col()
            .gap(px(9.0))
            .px(px(18.0))
            .pt(px(6.0))
            .pb(px(10.0))
            .child(
                div()
                    .flex()
                    .gap(px(9.0))
                    .child(Self::stat_card(
                        "Active FPS",
                        fps_label,
                        fps_note,
                        None,
                        ACCENT,
                    ))
                    .child(Self::stat_card(
                        "GPU Memory",
                        temp_label,
                        temp_note,
                        None,
                        TEXT_BRIGHT,
                    ))
                    .child(Self::stat_card(
                        "Cores Used",
                        cores_label,
                        format!("{cpu_pct:.1}% CPU"),
                        Some((cpu_pct, ACCENT, ACCENT_HOVER)),
                        TEXT_BRIGHT,
                    ))
                    .child(Self::stat_card(
                        "RAM Usage",
                        format!("{ram_pct:.0}%"),
                        ram_label,
                        Some((ram_pct, ACCENT, ACCENT_HOVER)),
                        TEXT_BRIGHT,
                    )),
            )
            .child(
                Self::stat_card(
                    "GPU Usage",
                    gpu.map_or("SYS".to_owned(), |_| format!("{gpu_pct:.0}%")),
                    text(s, "gpu_label")
                        .unwrap_or_else(|| "system GPU telemetry unavailable".into()),
                    Some((gpu_pct, ACCENT_DIM, ACCENT_HOVER)),
                    TEXT_BRIGHT,
                )
                .min_h(px(64.0)),
            );

        let [mfx_span, mfx_strong, mfx_small] = Self::action_text(
            "MetalFX Spatial",
            if metalfx_on { "ON" } else { "OFF" }.into(),
            if metalfx_on {
                "upscaling armed"
            } else {
                "native res"
            },
            ACCENT,
        );
        let factor_label = |f: f64| if f == 2.0 { "2×" } else { "1.75×" };
        let mono_button = |id: &'static str, label: String| {
            div()
                .id(id)
                .px(px(10.0))
                .py(px(4.0))
                .rounded(px(10.0))
                .border_1()
                .font_family(MONO)
                .text_size(px(11.0))
                .text_color(color(TEXT_BRIGHT))
                .bg(rgba(0x0d0b17bd))
                .when(busy, |d| d.opacity(0.5))
                .when(!busy, |d| d.cursor_pointer())
                .child(label)
        };
        let metalfx_card = Self::action_card("pm-metalfx", metalfx_on, false)
            .child(mfx_span)
            .child(mfx_strong)
            .child(mfx_small)
            .child(
                div()
                    .mt(px(6.0))
                    .flex()
                    .items_center()
                    .gap(px(6.0))
                    .child(
                        mono_button(
                            "pm-metalfx-toggle",
                            if metalfx_on { "Disable" } else { "Enable" }.into(),
                        )
                        .border_color(color(ACCENT))
                        .on_click(
                            cx.listener(move |pm, _, _, cx| pm.toggle_metalfx(!metalfx_on, cx)),
                        ),
                    )
                    .child(
                        div()
                            .relative()
                            .child(
                                mono_button(
                                    "pm-metalfx-factor",
                                    format!("{} ▾", factor_label(factor)),
                                )
                                .border_color(rgba(BORDER_STRONG))
                                .on_click(cx.listener(
                                    move |pm, _, _, cx| {
                                        if !pm.metalfx_busy {
                                            pm.factor_menu_open = !pm.factor_menu_open;
                                            cx.notify();
                                        }
                                    },
                                )),
                            )
                            .when(self.factor_menu_open, |d| {
                                d.child(gpui::deferred(
                                    div()
                                        .absolute()
                                        .top(px(26.0))
                                        .left_0()
                                        .min_w(px(64.0))
                                        .py(px(4.0))
                                        .rounded(px(8.0))
                                        .border_1()
                                        .border_color(rgba(BORDER_STRONG))
                                        .bg(rgb(0x0d0b17))
                                        .shadow_lg()
                                        .children(METALFX_FACTORS.iter().map(|&f| {
                                            div()
                                                .id(SharedString::from(format!("pm-factor-{f}")))
                                                .px(px(10.0))
                                                .py(px(4.0))
                                                .font_family(MONO)
                                                .text_size(px(11.0))
                                                .text_color(if f == factor {
                                                    color(ACCENT)
                                                } else {
                                                    color(TEXT_BRIGHT)
                                                })
                                                .hover(|d| d.bg(rgba(0x5fb7e81f)))
                                                .cursor_pointer()
                                                .child(factor_label(f))
                                                .on_click(cx.listener(move |pm, _, _, cx| {
                                                    pm.set_metalfx_factor(f, cx)
                                                }))
                                        })),
                                ))
                            }),
                    ),
            )
            .child(
                div()
                    .mt(px(6.0))
                    .text_size(px(10.0))
                    .text_color(color(TEXT_DIM))
                    .child("on/off: next swapchain recreate · factor: relaunch"),
            );
        let [q_span, q_strong, q_small] = Self::action_text(
            "Quit Game",
            "Force Kill".into(),
            "non-Steam Wine PIDs",
            ERROR,
        );
        let quit_card = Self::action_card("pm-quit", false, true)
            .cursor_pointer()
            .child(q_span)
            .child(q_strong)
            .child(q_small)
            .on_click(cx.listener(|pm, _, _, cx| pm.quit_game(cx)));
        let [g_span, g_strong, g_small] = Self::action_text(
            "GPU Acceleration",
            "Planned".into(),
            "runtime control coming soon",
            ACCENT,
        );
        let gpu_card = Self::action_card("pm-gpu", self.gpu_accel_armed, false)
            .cursor_pointer()
            .child(g_span)
            .child(g_strong)
            .child(g_small)
            .on_click(cx.listener(|pm, _, _, cx| pm.toggle_gpu_acceleration(cx)));
        let actions = div()
            .flex()
            .gap(px(9.0))
            .px(px(18.0))
            .pb(px(10.0))
            .child(metalfx_card)
            .child(quit_card)
            .child(gpu_card)
            // `.pm-action-grid` has four columns for three actions.
            .child(div().flex_1().min_w_0());

        let process_rows = if processes.is_empty() {
            vec![
                div()
                    .px(px(10.0))
                    .py(px(7.0))
                    .font_family(MONO)
                    .text_size(px(10.0))
                    .text_color(color(TEXT_DIM))
                    .child("No Wine/Steam/MetalSharp session processes detected yet.")
                    .into_any_element(),
            ]
        } else {
            processes
                .iter()
                .map(|p| {
                    let name = p.get("name").and_then(Value::as_str).unwrap_or("");
                    let name = name.rsplit('/').next().unwrap_or(name).to_owned();
                    let pid = p.get("pid").and_then(Value::as_i64).unwrap_or(0);
                    let command = p
                        .get("command")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_owned();
                    let fresh = p.get("fps_fresh").and_then(Value::as_bool) == Some(true);
                    let fps = p
                        .get("fps")
                        .and_then(Value::as_f64)
                        .filter(|_| fresh)
                        .map_or("-- FPS".to_owned(), |f| format!("{} FPS", f.round()));
                    let cpu = p.get("cpu_percent").and_then(Value::as_f64).unwrap_or(0.0);
                    let mem = p.get("mem_percent").and_then(Value::as_f64).unwrap_or(0.0);
                    let cell = |label: String| div().w(px(64.0)).flex_none().child(label);
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .px(px(10.0))
                        .py(px(7.0))
                        .border_b_1()
                        .border_color(rgba(0x00f5ff14))
                        .font_family(MONO)
                        .text_size(px(10.0))
                        .text_color(color(TEXT_PRIMARY))
                        .child(
                            div()
                                .flex_1()
                                .min_w_0()
                                .overflow_hidden()
                                .child(
                                    div()
                                        .font_weight(FontWeight::BOLD)
                                        .whitespace_nowrap()
                                        .text_ellipsis()
                                        .overflow_hidden()
                                        .child(name),
                                )
                                .child(
                                    div()
                                        .text_color(color(TEXT_DIM))
                                        .whitespace_nowrap()
                                        .text_ellipsis()
                                        .overflow_hidden()
                                        .child(format!("pid {pid} · {command}")),
                                ),
                        )
                        .child(cell(fps))
                        .child(cell(format!("{cpu:.1}% CPU")))
                        .child(cell(format!("{mem:.1}% MEM")))
                        .into_any_element()
                })
                .collect()
        };
        let process_section = div()
            .mx(px(18.0))
            .mb(px(10.0))
            .rounded(px(16.0))
            .border_1()
            .border_color(rgba(BORDER))
            .bg(rgba(0x09070f75))
            .overflow_hidden()
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .px(px(12.0))
                    .py(px(10.0))
                    .border_b_1()
                    .border_color(rgba(BORDER))
                    .child(
                        div()
                            .child(
                                div()
                                    .mb(px(4.0))
                                    .font_family(MONO)
                                    .text_size(px(9.0))
                                    .text_color(color(ACCENT_HOVER))
                                    .child("PROCESS VIEW"),
                            )
                            .child(
                                div()
                                    .text_size(px(14.0))
                                    .font_weight(FontWeight::BOLD)
                                    .text_color(color(TEXT_PRIMARY))
                                    .child("Wine / Steam Session"),
                            ),
                    )
                    .child(
                        div()
                            .font_family(MONO)
                            .text_size(px(10.0))
                            .text_color(color(TEXT_DIM))
                            .child(if self.sampling {
                                "sampling".to_owned()
                            } else {
                                format!("{} rows", processes.len())
                            }),
                    ),
            )
            .child(
                div()
                    .id("pm-process-list")
                    .max_h(px(96.0))
                    .overflow_y_scroll()
                    .children(process_rows),
            );

        let footer = div()
            .flex()
            .justify_between()
            .gap(px(12.0))
            .px(px(18.0))
            .pb(px(12.0))
            .font_family(MONO)
            .text_size(px(10.0))
            .text_color(color(TEXT_DIM))
            .child(div().min_w_0().child(self.status.clone()))
            .child(
                div()
                    .flex_none()
                    .child(text(s, "source").unwrap_or_else(|| "waiting".into())),
            );

        div()
            .id("pm-shell")
            .track_focus(&self.focus)
            .on_key_down(cx.listener(|pm, event: &KeyDownEvent, window, cx| {
                if event.keystroke.key == "escape" {
                    if pm.factor_menu_open {
                        pm.factor_menu_open = false;
                        cx.notify();
                    } else {
                        pm.close(window, cx);
                    }
                }
            }))
            .size_full()
            .font_family(".SystemUIFont")
            .text_color(color(TEXT_PRIMARY))
            .bg(linear_gradient(
                160.0,
                linear_color_stop(rgb(0x1a1122), 0.0),
                linear_color_stop(rgb(0x0b1418), 1.0),
            ))
            .child(
                div()
                    .id("pm-panel")
                    .size_full()
                    .overflow_y_scroll()
                    .bg(linear_gradient(
                        140.0,
                        linear_color_stop(rgba(0x09070fd1), 0.0),
                        linear_color_stop(rgba(0x05232ca3), 1.0),
                    ))
                    .child(header)
                    .child(stats)
                    .child(actions)
                    .child(process_section)
                    .child(footer),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quit_game_targets_only_non_steam_wine() {
        let rows = parse_process_rows(
            "  101 /opt/wine/bin/wine64-preloader C:\\\\Game\\\\game.exe\n\
             102 wineserver wineserver -p\n\
             103 /opt/wine/bin/wine C:\\\\Program Files\\\\Steam\\\\steam.exe\n\
             104 /usr/bin/zsh zsh\n\
             105 game.exe /prefix/drive_c/Games/game.exe\n",
        );
        let targets: Vec<i32> = rows
            .iter()
            .filter(|r| is_non_steam_wine_process(r))
            .map(|r| r.pid)
            .collect();
        assert_eq!(targets, vec![101, 102, 105]);
    }

    #[test]
    fn bytes_format_like_overlay() {
        assert_eq!(fmt_bytes(0.0), "--");
        assert_eq!(fmt_bytes(8_641_740_800.0), "8.05 GB");
        assert_eq!(fmt_bytes(17_179_869_184.0), "16.0 GB");
    }
}
