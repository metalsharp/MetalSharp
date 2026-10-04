//! Safe owned-child primitives plus the connected Wine/game process-manager presentation.
//! Wine telemetry and stop requests are delegated to the authoritative C backend; this view
//! never accepts caller-provided PIDs or process names.
use crate::backend::{BackendClient, BackendError};
use gpui::{Context, EventEmitter, Render, Window, div, prelude::*, px, rgb};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    process::{Child, ExitStatus},
    sync::atomic::{AtomicU64, Ordering},
    time::Instant,
};

static NEXT_OWNER: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OwnedProcessId {
    owner: u64,
    sequence: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProcessRow {
    pub id: OwnedProcessId,
    pub label: String,
    pub pid: u32,
    pub running_for_ms: u128,
}
#[derive(Debug, PartialEq, Eq)]
pub enum ProcessError {
    InvalidLabel,
    NotOwned,
    ConfirmationRequired,
    AlreadyExited,
    Io(String),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StopConfirmation(OwnedProcessId);
struct Entry {
    label: String,
    child: Child,
    started: Instant,
}
pub struct OwnedProcessManager {
    owner: u64,
    next: u64,
    entries: BTreeMap<OwnedProcessId, Entry>,
}
impl Default for OwnedProcessManager {
    fn default() -> Self {
        Self::new()
    }
}
impl OwnedProcessManager {
    pub fn new() -> Self {
        Self {
            owner: NEXT_OWNER.fetch_add(1, Ordering::Relaxed),
            next: 1,
            entries: BTreeMap::new(),
        }
    }
    pub fn register(
        &mut self,
        label: &str,
        mut child: Child,
    ) -> Result<OwnedProcessId, ProcessError> {
        if label.trim().is_empty() || label.len() > 96 || label.chars().any(char::is_control) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(ProcessError::InvalidLabel);
        }
        let sequence = self.next;
        self.next = self.next.checked_add(1).ok_or(ProcessError::InvalidLabel)?;
        let id = OwnedProcessId {
            owner: self.owner,
            sequence,
        };
        self.entries.insert(
            id,
            Entry {
                label: label.to_owned(),
                child,
                started: Instant::now(),
            },
        );
        Ok(id)
    }
    pub fn sample(&mut self) -> Vec<ProcessRow> {
        let mut out = Vec::new();
        let mut dead = Vec::new();
        for (id, e) in &mut self.entries {
            match e.child.try_wait() {
                Ok(Some(_)) | Err(_) => dead.push(*id),
                Ok(None) => out.push(ProcessRow {
                    id: *id,
                    label: e.label.clone(),
                    pid: e.child.id(),
                    running_for_ms: e.started.elapsed().as_millis(),
                }),
            }
        }
        for id in dead {
            self.entries.remove(&id);
        }
        out
    }
    pub fn confirm_stop(&self, id: OwnedProcessId) -> Result<StopConfirmation, ProcessError> {
        self.entries
            .contains_key(&id)
            .then_some(StopConfirmation(id))
            .ok_or(ProcessError::NotOwned)
    }
    pub fn stop(
        &mut self,
        id: OwnedProcessId,
        confirmation: Option<StopConfirmation>,
    ) -> Result<ExitStatus, ProcessError> {
        if confirmation != Some(StopConfirmation(id)) {
            return Err(ProcessError::ConfirmationRequired);
        }
        if id.owner != self.owner {
            return Err(ProcessError::NotOwned);
        }
        let mut e = self.entries.remove(&id).ok_or(ProcessError::NotOwned)?;
        if let Some(status) = e.child.try_wait().map_err(ioerr)? {
            return Ok(status);
        }
        e.child.kill().map_err(ioerr)?;
        e.child.wait().map_err(ioerr)
    }
    pub fn stop_all(&mut self, confirmed: bool) {
        if confirmed {
            for id in self.entries.keys().copied().collect::<Vec<_>>() {
                let _ = self.stop(id, Some(StopConfirmation(id)));
            }
        }
    }
}
impl Drop for OwnedProcessManager {
    fn drop(&mut self) {
        self.stop_all(true)
    }
}
fn ioerr(e: std::io::Error) -> ProcessError {
    ProcessError::Io(e.to_string())
}

/// Backend telemetry contract from GET /game/running and appid-scoped POST /kill.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct WineProcess {
    pub appid: u64,
    pub pid: u32,
    pub name: String,
}
#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq, Eq)]
pub struct ProcessSnapshot {
    pub processes: Vec<WineProcess>,
}
#[derive(Deserialize)]
struct RunningResponse {
    #[serde(default)]
    running: Vec<RunningProcess>,
}
#[derive(Deserialize)]
struct RunningProcess {
    appid: u64,
    pid: u32,
}
impl ProcessSnapshot {
    pub fn load(client: &BackendClient, games: &[(u64, String)]) -> Result<Self, BackendError> {
        let payload: Value = client.get("/game/running")?;
        let parsed: RunningResponse =
            serde_json::from_value(payload).map_err(|_| BackendError::InvalidJson)?;
        let names: BTreeMap<_, _> = games.iter().cloned().collect();
        let mut processes = parsed
            .running
            .into_iter()
            .filter(|p| p.appid > 0 && p.pid > 0)
            .map(|p| WineProcess {
                appid: p.appid,
                pid: p.pid,
                name: names
                    .get(&p.appid)
                    .cloned()
                    .unwrap_or_else(|| format!("Game {}", p.appid)),
            })
            .collect::<Vec<_>>();
        processes.sort_by_key(|p| p.appid);
        processes.dedup_by_key(|p| p.appid);
        Ok(Self { processes })
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProcessIntent {
    StopGame { appid: u64 },
    ForceQuitGames,
}
impl ProcessIntent {
    /// Executes only the C backend's appid-scoped game stop contract. Bulk force quit is
    /// explicit and kept separate for a confirmation flow in the host.
    pub fn execute(self, client: &BackendClient) -> Result<Value, BackendError> {
        match self {
            Self::StopGame { appid } if appid > 0 => client.post("/kill", json!({"appid":appid})),
            Self::StopGame { .. } => Err(BackendError::InvalidInput),
            Self::ForceQuitGames => client.post("/games/force-quit", json!({})),
        }
    }
}
#[derive(Clone, Debug)]
pub struct ProcessIntentEvent(pub ProcessIntent);
impl EventEmitter<ProcessIntentEvent> for ProcessManagerView {}
pub struct ProcessManagerView {
    snapshot: ProcessSnapshot,
    busy: bool,
}
impl ProcessManagerView {
    pub fn new(snapshot: ProcessSnapshot) -> Self {
        Self {
            snapshot,
            busy: false,
        }
    }
    pub fn apply_snapshot(&mut self, snapshot: ProcessSnapshot, cx: &mut Context<Self>) {
        self.snapshot = snapshot;
        self.busy = false;
        cx.notify()
    }
    pub fn set_busy(&mut self, busy: bool, cx: &mut Context<Self>) {
        self.busy = busy;
        cx.notify()
    }
    fn emit(&self, intent: ProcessIntent, cx: &mut Context<Self>) {
        cx.emit(ProcessIntentEvent(intent));
    }
}
impl Render for ProcessManagerView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut root=div().id("native-process-manager").size_full().flex().flex_col().gap(px(12.)).p(px(22.)).bg(rgb(0x101316)).text_color(rgb(0xf2efe6)).font_family("Rethink Sans")
            .child(div().text_size(px(20.)).child("Running games"))
            .child(div().text_color(rgb(0xc2bda9)).child("Live telemetry from MetalSharp’s managed Wine backend. Stop targets one registered game; no process-name or PID kill is available."));
        if self.snapshot.processes.is_empty() {
            root = root.child(
                div()
                    .py(px(18.))
                    .child("No managed game processes are currently running."),
            );
        }
        for process in &self.snapshot.processes {
            let appid = process.appid;
            let label = process.name.clone();
            let pid = process.pid;
            root = root.child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .py(px(9.))
                    .border_b_1()
                    .border_color(rgb(0x303638))
                    .child(
                        div().flex_1().child(div().child(label)).child(
                            div()
                                .text_size(px(12.))
                                .text_color(rgb(0xa6aaa5))
                                .child(format!("App {} · PID {} (backend-reported)", appid, pid)),
                        ),
                    )
                    .child(
                        div()
                            .id(("stop-game", appid))
                            .px(px(12.))
                            .py(px(7.))
                            .rounded(px(6.))
                            .bg(rgb(if self.busy { 0x292d2f } else { 0x653d39 }))
                            .cursor_pointer()
                            .child("Stop game")
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if !this.busy {
                                    this.emit(ProcessIntent::StopGame { appid }, cx)
                                }
                            })),
                    ),
            );
        }
        root
    }
}

#[cfg(test)]
mod connected_tests {
    use super::*;
    use crate::backend::BackendClient;
    use std::{
        io::{Read, Write},
        net::TcpListener,
        thread,
        time::Duration,
    };
    fn fixture(reply: &'static str) -> (BackendClient, thread::JoinHandle<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let c = BackendClient::for_port(listener.local_addr().unwrap().port()).unwrap();
        let h = thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut b = Vec::new();
            loop {
                let mut chunk = [0; 2048];
                let n = s.read(&mut chunk).unwrap();
                if n == 0 {
                    break;
                }
                b.extend_from_slice(&chunk[..n]);
                if let Some(end) = b.windows(4).position(|w| w == b"\r\n\r\n") {
                    let header = String::from_utf8_lossy(&b[..end]).to_ascii_lowercase();
                    let len = header
                        .lines()
                        .find_map(|line| {
                            line.strip_prefix("content-length:")
                                .and_then(|v| v.trim().parse::<usize>().ok())
                        })
                        .unwrap_or(0);
                    if b.len() >= end + 4 + len {
                        break;
                    }
                }
            }
            let req = String::from_utf8_lossy(&b).to_string();
            write!(
                s,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                reply.len(),
                reply
            )
            .unwrap();
            req
        });
        (c, h)
    }
    #[test]
    fn telemetry_uses_authoritative_endpoint_and_maps_names_without_trusting_extra_process_fields()
    {
        let (c, h) = fixture(
            r#"{"ok":true,"running":[{"appid":22,"pid":1002},{"appid":0,"pid":5},{"appid":22,"pid":1010},{"appid":33,"pid":0}]}"#,
        );
        let s = ProcessSnapshot::load(&c, &[(22, "A Game".into())]).unwrap();
        let req = h.join().unwrap();
        assert!(req.starts_with("GET /game/running "));
        assert_eq!(
            s.processes,
            vec![WineProcess {
                appid: 22,
                pid: 1002,
                name: "A Game".into()
            }]
        );
    }
    #[test]
    fn stop_intent_is_appid_scoped_and_force_quit_is_distinct() {
        let (c, h) = fixture(r#"{"ok":true}"#);
        ProcessIntent::StopGame { appid: 22 }.execute(&c).unwrap();
        let req = h.join().unwrap();
        assert!(req.starts_with("POST /kill "));
        assert!(req.ends_with(r#"{"appid":22}"#));
        let c = BackendClient::for_port(12345).unwrap();
        assert_eq!(
            ProcessIntent::StopGame { appid: 0 }.execute(&c),
            Err(BackendError::InvalidInput)
        );
    }
    #[test]
    fn owned_process_manager_still_requires_owned_confirmation() {
        let mut m = OwnedProcessManager::new();
        let id = m
            .register(
                "fixture",
                std::process::Command::new("/bin/sleep")
                    .arg("3")
                    .spawn()
                    .unwrap(),
            )
            .unwrap();
        assert_eq!(m.stop(id, None), Err(ProcessError::ConfirmationRequired));
        let token = m.confirm_stop(id).unwrap();
        assert!(!m.stop(id, Some(token)).unwrap().success());
    }
}
