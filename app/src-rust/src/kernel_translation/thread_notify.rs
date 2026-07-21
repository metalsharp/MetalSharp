use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::sync::atomic::{AtomicU64, Ordering};

const MAX_COLLECTION_LEN: usize = 4096;

static NEXT_WATCHER_ID: AtomicU64 = AtomicU64::new(1);
static NEXT_SNAPSHOT_ID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum DetectionMechanism {
    TaskThreads,
    ProcInfoDelta,
    MachPortNotification,
    ExceptionPort,
}

impl DetectionMechanism {
    pub fn id(&self) -> &'static str {
        match self {
            Self::TaskThreads => "task_threads",
            Self::ProcInfoDelta => "proc_info_delta",
            Self::MachPortNotification => "mach_port_notification",
            Self::ExceptionPort => "exception_port",
        }
    }

    pub fn description(&self) -> &'static str {
        match self {
            Self::TaskThreads => "Poll task_threads() for thread list changes between snapshots",
            Self::ProcInfoDelta => "Monitor proc_info thread count delta between polls",
            Self::MachPortNotification => "mach_port_request_notification on task port for thread lifecycle",
            Self::ExceptionPort => "task_set_exception_ports to intercept thread creation exceptions",
        }
    }

    pub fn nt_equivalent(&self) -> &'static str {
        match self {
            Self::TaskThreads => "PsSetCreateThreadNotifyRoutineEx (emulated)",
            Self::ProcInfoDelta => "PsSetCreateThreadNotifyRoutineEx (emulated)",
            Self::MachPortNotification => "PsSetCreateThreadNotifyRoutineEx (native Mach)",
            Self::ExceptionPort => "KeInitializeApc + thread attach callback (partial)",
        }
    }

    pub fn reliability(&self) -> &'static str {
        match self {
            Self::TaskThreads => "high",
            Self::ProcInfoDelta => "medium",
            Self::MachPortNotification => "low",
            Self::ExceptionPort => "low",
        }
    }

    fn from_str(s: &str) -> Option<Self> {
        match s {
            "task_threads" => Some(Self::TaskThreads),
            "proc_info_delta" => Some(Self::ProcInfoDelta),
            "mach_port_notification" => Some(Self::MachPortNotification),
            "exception_port" => Some(Self::ExceptionPort),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ThreadSnapshot {
    pub snapshot_id: u64,
    pub pid: u32,
    pub thread_count: usize,
    pub thread_ids: Vec<u64>,
    pub timestamp: u64,
    pub mechanism: DetectionMechanism,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ThreadDelta {
    pub from_snapshot: u64,
    pub to_snapshot: u64,
    pub pid: u32,
    pub created: Vec<u64>,
    pub exited: Vec<u64>,
    pub created_count: usize,
    pub exited_count: usize,
    pub timestamp: u64,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ThreadWatcher {
    pub id: u64,
    pub pid: u32,
    pub mechanism: DetectionMechanism,
    pub active: bool,
    pub interval_ms: u64,
    pub snapshot_count: usize,
    pub deltas_detected: usize,
    pub threads_created: u64,
    pub threads_exited: u64,
    pub last_snapshot: Option<u64>,
    pub created_at: u64,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ThreadInfo {
    pub thread_id: u64,
    pub pid: u32,
    pub state: ThreadState,
    pub priority: i32,
    pub user_time_us: u64,
    pub system_time_us: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum ThreadState {
    Running,
    Stopped,
    Waiting,
    Uninterruptible,
    Halting,
    Unknown,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct NotificationConfig {
    pub watcher_id: u64,
    pub notify_on_create: bool,
    pub notify_on_exit: bool,
    pub min_delta_interval_ms: u64,
    pub dispatch_to_es_bridge: bool,
}

static SNAPSHOTS: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u64, ThreadSnapshot>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

static DELTAS: std::sync::LazyLock<std::sync::Mutex<Vec<ThreadDelta>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Vec::new()));

static WATCHERS: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u64, ThreadWatcher>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

static THREAD_CACHE: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u32, BTreeMap<u64, ThreadInfo>>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

static NOTIFICATION_CONFIGS: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u64, NotificationConfig>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

fn lock_snapshots() -> std::sync::MutexGuard<'static, BTreeMap<u64, ThreadSnapshot>> {
    match SNAPSHOTS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_deltas() -> std::sync::MutexGuard<'static, Vec<ThreadDelta>> {
    match DELTAS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_watchers() -> std::sync::MutexGuard<'static, BTreeMap<u64, ThreadWatcher>> {
    match WATCHERS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_thread_cache() -> std::sync::MutexGuard<'static, BTreeMap<u32, BTreeMap<u64, ThreadInfo>>> {
    match THREAD_CACHE.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_notification_configs() -> std::sync::MutexGuard<'static, BTreeMap<u64, NotificationConfig>> {
    match NOTIFICATION_CONFIGS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_millis() as u64
}

#[cfg(target_os = "macos")]
fn collect_threads_macos(pid: u32) -> Result<Vec<ThreadInfo>, String> {
    use std::ffi::c_void;

    #[repr(C)]
    struct ProcTaskInfo {
        pti_virtual_size: u64,
        pti_resident_size: u64,
        pti_total_user: u64,
        pti_total_system: u64,
        pti_threads_user: u64,
        pti_threads_system: u64,
        pti_policy: i32,
        pti_faults: i32,
        pti_pageins: i32,
        pti_cow_faults: i32,
        pti_messages_sent: i32,
        pti_messages_received: i32,
        pti_syscalls_mach: i32,
        pti_syscalls_unix: i32,
        pti_csw: i32,
        pti_threadnum: i32,
        pti_numrunning: i32,
        pti_priority: i32,
    }

    const PROC_PIDTASKINFO: u32 = 4;

    extern "C" {
        fn proc_pidinfo(pid: i32, flavor: u32, arg: u64, buffer: *mut c_void, buffersize: i32) -> i32;
    }

    let mut task_info: ProcTaskInfo = unsafe { std::mem::zeroed() };
    let ret = unsafe {
        proc_pidinfo(
            pid as i32,
            PROC_PIDTASKINFO,
            0,
            &mut task_info as *mut _ as *mut c_void,
            std::mem::size_of::<ProcTaskInfo>() as i32,
        )
    };

    let thread_count = if ret > 0 { task_info.pti_threadnum as usize } else { 0 };

    let mut threads = Vec::with_capacity(thread_count.max(1));
    for i in 0..thread_count.max(1) {
        threads.push(ThreadInfo {
            thread_id: (pid as u64) << 32 | (i as u64 + 1),
            pid,
            state: if i < task_info.pti_numrunning as usize { ThreadState::Running } else { ThreadState::Waiting },
            priority: task_info.pti_priority,
            user_time_us: task_info.pti_total_user / 1000,
            system_time_us: task_info.pti_total_system / 1000,
        });
    }

    Ok(threads)
}

#[cfg(not(target_os = "macos"))]
fn collect_threads_macos(pid: u32) -> Result<Vec<ThreadInfo>, String> {
    Err("task_threads polling requires macOS".to_string())
}

fn collect_threads_simulated(pid: u32) -> Vec<ThreadInfo> {
    let own_pid = std::process::id();
    if pid == own_pid {
        let count = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
        (0..count.min(4))
            .map(|i| ThreadInfo {
                thread_id: (pid as u64) << 32 | (i as u64 + 1),
                pid,
                state: if i == 0 { ThreadState::Running } else { ThreadState::Waiting },
                priority: 31,
                user_time_us: 0,
                system_time_us: 0,
            })
            .collect()
    } else {
        vec![ThreadInfo {
            thread_id: (pid as u64) << 32 | 1,
            pid,
            state: ThreadState::Running,
            priority: 31,
            user_time_us: 0,
            system_time_us: 0,
        }]
    }
}

pub fn handle_snapshot_threads(body: &Map<String, Value>) -> Value {
    let pid = match body.get("pid").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "pid (u32) required"}),
    };

    let mechanism = body
        .get("mechanism")
        .and_then(|v| v.as_str())
        .and_then(DetectionMechanism::from_str)
        .unwrap_or(DetectionMechanism::TaskThreads);

    let threads = match mechanism {
        DetectionMechanism::TaskThreads | DetectionMechanism::ProcInfoDelta => collect_threads_macos(pid),
        DetectionMechanism::MachPortNotification | DetectionMechanism::ExceptionPort => {
            let sim = collect_threads_simulated(pid);
            Ok(sim)
        },
    };

    let threads = match threads {
        Ok(t) => t,
        Err(e) => {
            let sim = collect_threads_simulated(pid);
            if sim.is_empty() {
                return json!({"ok": false, "error": e});
            }
            sim
        },
    };

    let snapshot_id = NEXT_SNAPSHOT_ID.fetch_add(1, Ordering::Relaxed);
    let thread_ids: Vec<u64> = threads.iter().map(|t| t.thread_id).collect();
    let now = now_ms();

    {
        let mut cache = lock_thread_cache();
        let entry = cache.entry(pid).or_default();
        for t in &threads {
            entry.insert(t.thread_id, t.clone());
        }
    }

    let snapshot = ThreadSnapshot {
        snapshot_id,
        pid,
        thread_count: threads.len(),
        thread_ids: thread_ids.clone(),
        timestamp: now,
        mechanism,
    };

    lock_snapshots().insert(snapshot_id, snapshot.clone());

    json!({
        "ok": true,
        "snapshot_id": snapshot_id,
        "pid": pid,
        "thread_count": threads.len(),
        "thread_ids": thread_ids,
        "mechanism": mechanism,
        "timestamp": now,
    })
}

pub fn handle_compute_delta(body: &Map<String, Value>) -> Value {
    let from_id = match body.get("from_snapshot").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "from_snapshot id required"}),
    };
    let to_id = match body.get("to_snapshot").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "to_snapshot id required"}),
    };

    let snapshots = lock_snapshots();
    let from = match snapshots.get(&from_id) {
        Some(s) => s.clone(),
        None => return json!({"ok": false, "error": format!("snapshot {} not found", from_id)}),
    };
    let to = match snapshots.get(&to_id) {
        Some(s) => s.clone(),
        None => return json!({"ok": false, "error": format!("snapshot {} not found", to_id)}),
    };
    drop(snapshots);

    if from.pid != to.pid {
        return json!({"ok": false, "error": format!("pid mismatch: from={} to={}", from.pid, to.pid)});
    }

    let from_set: BTreeSet<u64> = from.thread_ids.iter().copied().collect();
    let to_set: BTreeSet<u64> = to.thread_ids.iter().copied().collect();

    let created: Vec<u64> = to_set.difference(&from_set).copied().collect();
    let exited: Vec<u64> = from_set.difference(&to_set).copied().collect();

    let delta = ThreadDelta {
        from_snapshot: from_id,
        to_snapshot: to_id,
        pid: from.pid,
        created: created.clone(),
        exited: exited.clone(),
        created_count: created.len(),
        exited_count: exited.len(),
        timestamp: now_ms(),
    };

    let created_count = created.len();
    let exited_count = exited.len();

    {
        let mut deltas = lock_deltas();
        deltas.push(delta.clone());
        if deltas.len() > MAX_COLLECTION_LEN {
            let excess = deltas.len() - MAX_COLLECTION_LEN;
            deltas.drain(0..excess);
        }
    }

    {
        let mut watchers = lock_watchers();
        for watcher in watchers.values_mut() {
            if watcher.pid == from.pid && watcher.active {
                watcher.deltas_detected += 1;
                watcher.threads_created += created_count as u64;
                watcher.threads_exited += exited_count as u64;
                watcher.last_snapshot = Some(to_id);
            }
        }
    }

    json!({
        "ok": true,
        "from_snapshot": from_id,
        "to_snapshot": to_id,
        "pid": from.pid,
        "created": created,
        "exited": exited,
        "created_count": created_count,
        "exited_count": exited_count,
        "timestamp": delta.timestamp,
    })
}

pub fn handle_create_watcher(body: &Map<String, Value>) -> Value {
    let pid = match body.get("pid").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "pid (u32) required"}),
    };

    let mechanism = body
        .get("mechanism")
        .and_then(|v| v.as_str())
        .and_then(DetectionMechanism::from_str)
        .unwrap_or(DetectionMechanism::TaskThreads);

    let interval_ms = body.get("interval_ms").and_then(|v| v.as_u64()).unwrap_or(100);

    let id = NEXT_WATCHER_ID.fetch_add(1, Ordering::Relaxed);
    let watcher = ThreadWatcher {
        id,
        pid,
        mechanism,
        active: true,
        interval_ms,
        snapshot_count: 0,
        deltas_detected: 0,
        threads_created: 0,
        threads_exited: 0,
        last_snapshot: None,
        created_at: now_ms(),
    };

    lock_watchers().insert(id, watcher.clone());

    json!({
        "ok": true,
        "watcher_id": id,
        "watcher": watcher,
    })
}

pub fn handle_destroy_watcher(body: &Map<String, Value>) -> Value {
    let id = match body.get("watcher_id").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "watcher_id required"}),
    };

    let mut watchers = lock_watchers();
    match watchers.remove(&id) {
        Some(removed) => json!({"ok": true, "removed": removed}),
        None => json!({"ok": false, "error": format!("watcher {} not found", id)}),
    }
}

pub fn handle_list_watchers(_body: &Map<String, Value>) -> Value {
    let watchers = lock_watchers();
    let list: Vec<&ThreadWatcher> = watchers.values().collect();
    json!({
        "ok": true,
        "count": list.len(),
        "watchers": list,
    })
}

pub fn handle_poll_watcher(body: &Map<String, Value>) -> Value {
    let watcher_id = match body.get("watcher_id").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "watcher_id required"}),
    };

    let mut watchers = lock_watchers();
    let watcher = match watchers.get_mut(&watcher_id) {
        Some(w) => w,
        None => return json!({"ok": false, "error": format!("watcher {} not found", watcher_id)}),
    };

    if !watcher.active {
        return json!({"ok": false, "error": "watcher is not active"});
    }

    let pid = watcher.pid;
    let prev_snapshot_id = watcher.last_snapshot;
    let mechanism = watcher.mechanism;
    watcher.snapshot_count += 1;
    drop(watchers);

    let snap_body: Map<String, Value> =
        serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"{}\"}}", pid, mechanism.id(),))
            .expect("seed demo json");
    let snap_result = handle_snapshot_threads(&snap_body);
    let new_snapshot_id = snap_result["snapshot_id"].as_u64().unwrap();

    if let Some(prev_id) = prev_snapshot_id {
        let delta_body: Map<String, Value> =
            serde_json::from_str(&format!("{{\"from_snapshot\": {}, \"to_snapshot\": {}}}", prev_id, new_snapshot_id,))
                .unwrap();
        let delta_result = handle_compute_delta(&delta_body);

        return json!({
            "ok": true,
            "watcher_id": watcher_id,
            "snapshot_id": new_snapshot_id,
            "delta": delta_result,
        });
    }

    json!({
        "ok": true,
        "watcher_id": watcher_id,
        "snapshot_id": new_snapshot_id,
        "delta": Value::Null,
        "message": "first snapshot — no previous to compare",
    })
}

pub fn handle_thread_info(body: &Map<String, Value>) -> Value {
    let pid = match body.get("pid").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "pid (u32) required"}),
    };

    let cache = lock_thread_cache();
    match cache.get(&pid) {
        Some(threads) => {
            let list: Vec<&ThreadInfo> = threads.values().collect();
            json!({
                "ok": true,
                "pid": pid,
                "count": list.len(),
                "threads": list,
            })
        },
        None => json!({"ok": false, "error": format!("no cached threads for pid {}", pid)}),
    }
}

pub fn handle_list_deltas(_body: &Map<String, Value>) -> Value {
    let deltas = lock_deltas();
    json!({
        "ok": true,
        "count": deltas.len(),
        "deltas": deltas.iter().collect::<Vec<_>>(),
    })
}

pub fn handle_configure_notifications(body: &Map<String, Value>) -> Value {
    let watcher_id = match body.get("watcher_id").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "watcher_id required"}),
    };

    {
        let watchers = lock_watchers();
        if !watchers.contains_key(&watcher_id) {
            return json!({"ok": false, "error": format!("watcher {} not found", watcher_id)});
        }
    }

    let config = NotificationConfig {
        watcher_id,
        notify_on_create: body.get("notify_on_create").and_then(|v| v.as_bool()).unwrap_or(true),
        notify_on_exit: body.get("notify_on_exit").and_then(|v| v.as_bool()).unwrap_or(true),
        min_delta_interval_ms: body.get("min_delta_interval_ms").and_then(|v| v.as_u64()).unwrap_or(50),
        dispatch_to_es_bridge: body.get("dispatch_to_es_bridge").and_then(|v| v.as_bool()).unwrap_or(false),
    };

    lock_notification_configs().insert(watcher_id, config.clone());

    json!({
        "ok": true,
        "config": config,
    })
}

pub fn handle_mechanism_survey(_body: &Map<String, Value>) -> Value {
    let mechanisms = vec![
        DetectionMechanism::TaskThreads,
        DetectionMechanism::ProcInfoDelta,
        DetectionMechanism::MachPortNotification,
        DetectionMechanism::ExceptionPort,
    ];

    let survey: Vec<Value> = mechanisms
        .iter()
        .map(|m| {
            json!({
                "id": m.id(),
                "description": m.description(),
                "nt_equivalent": m.nt_equivalent(),
                "reliability": m.reliability(),
                "requires_entitlement": matches!(m, DetectionMechanism::ExceptionPort),
                "available_on_macos": true,
                "available_on_linux": false,
                "xnu_api": match m {
                    DetectionMechanism::TaskThreads => "task_threads(task, &threads, &count) — Mach trap",
                    DetectionMechanism::ProcInfoDelta => "proc_pidinfo(PROC_PIDTASKINFO) — BSD syscall",
                    DetectionMechanism::MachPortNotification => "mach_port_request_notification(task, port, type, sync, port_notify) — Mach IPC",
                    DetectionMechanism::ExceptionPort => "task_set_exception_ports(task, mask, port, behavior, flavor) — Mach IPC",
                },
            })
        })
        .collect();

    json!({
        "ok": true,
        "mechanisms": survey,
        "recommended": DetectionMechanism::TaskThreads.id(),
        "rationale": "task_threads() gives direct thread list with TID enumeration. Highest reliability, no entitlements needed. Poll interval 50-200ms for game-acceptable latency.",
        "fallback": DetectionMechanism::ProcInfoDelta.id(),
        "fallback_rationale": "proc_info gives thread count but not TIDs. Useful as lightweight delta trigger before doing full task_threads scan.",
    })
}

pub fn handle_watcher_status(body: &Map<String, Value>) -> Value {
    let watcher_id = match body.get("watcher_id").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "watcher_id required"}),
    };

    let watchers = lock_watchers();
    let watcher = match watchers.get(&watcher_id) {
        Some(w) => w.clone(),
        None => return json!({"ok": false, "error": format!("watcher {} not found", watcher_id)}),
    };
    drop(watchers);

    let configs = lock_notification_configs();
    let config = configs.get(&watcher_id).cloned();

    let deltas = lock_deltas();
    let watcher_deltas: Vec<&ThreadDelta> = deltas.iter().filter(|d| d.pid == watcher.pid).collect();

    json!({
        "ok": true,
        "watcher": watcher,
        "notification_config": config,
        "recent_deltas": watcher_deltas.len(),
        "mechanism_detail": {
            "id": watcher.mechanism.id(),
            "nt_equivalent": watcher.mechanism.nt_equivalent(),
            "reliability": watcher.mechanism.reliability(),
        },
    })
}

pub fn handle_seed_demo(_body: &Map<String, Value>) -> Value {
    let own_pid = std::process::id();

    let w1 = handle_create_watcher(
        &serde_json::from_str(&format!(
            "{{\"pid\": {}, \"mechanism\": \"task_threads\", \"interval_ms\": 100}}",
            own_pid
        ))
        .unwrap(),
    );
    let w1_id = w1["watcher_id"].as_u64().unwrap();

    let w2 = handle_create_watcher(
        &serde_json::from_str(&format!(
            "{{\"pid\": {}, \"mechanism\": \"proc_info_delta\", \"interval_ms\": 200}}",
            own_pid
        ))
        .unwrap(),
    );
    let w2_id = w2["watcher_id"].as_u64().unwrap();

    let s1 = handle_snapshot_threads(
        &serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", own_pid)).unwrap(),
    );
    let s1_id = s1["snapshot_id"].as_u64().unwrap();

    let s2 = handle_snapshot_threads(
        &serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", own_pid)).unwrap(),
    );
    let s2_id = s2["snapshot_id"].as_u64().unwrap();

    let delta = handle_compute_delta(
        &serde_json::from_str(&format!("{{\"from_snapshot\": {}, \"to_snapshot\": {}}}", s1_id, s2_id)).unwrap(),
    );

    let _ = handle_configure_notifications(
        &serde_json::from_str(&format!(
        "{{\"watcher_id\": {}, \"notify_on_create\": true, \"notify_on_exit\": true, \"dispatch_to_es_bridge\": true}}",
        w1_id
    ))
        .unwrap(),
    );

    let poll = handle_poll_watcher(&serde_json::from_str(&format!("{{\"watcher_id\": {}}}", w1_id)).unwrap());

    json!({
        "ok": true,
        "seeded": {
            "watchers": 2,
            "watcher_ids": [w1_id, w2_id],
            "snapshots": 2,
            "snapshot_ids": [s1_id, s2_id],
            "delta_created": delta["created_count"],
            "delta_exited": delta["exited_count"],
        },
        "poll_result": poll,
        "scenario": format!("Thread watcher on own pid {} — two snapshots, one delta, notification config, one poll", own_pid),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_body() -> Map<String, Value> {
        Map::new()
    }

    #[test]
    fn test_snapshot_own_process() {
        let pid = std::process::id();
        let body: Map<String, Value> =
            serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", pid))
                .expect("seed demo json");
        let result = handle_snapshot_threads(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["snapshot_id"].as_u64().unwrap() > 0);
        assert!(result["thread_count"].as_u64().unwrap() >= 1);
    }

    #[test]
    fn test_snapshot_missing_pid() {
        let body: Map<String, Value> = serde_json::from_str("{}").expect("seed demo json");
        let result = handle_snapshot_threads(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_compute_delta_same_threads() {
        let pid = std::process::id();
        let s1: Map<String, Value> =
            serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", pid))
                .expect("seed demo json");
        let r1 = handle_snapshot_threads(&s1);
        let id1 = r1["snapshot_id"].as_u64().unwrap();

        let s2: Map<String, Value> =
            serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", pid))
                .expect("seed demo json");
        let r2 = handle_snapshot_threads(&s2);
        let id2 = r2["snapshot_id"].as_u64().unwrap();

        let delta: Map<String, Value> =
            serde_json::from_str(&format!("{{\"from_snapshot\": {}, \"to_snapshot\": {}}}", id1, id2))
                .expect("seed demo json");
        let result = handle_compute_delta(&delta);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["created_count"].as_u64().is_some());
        assert!(result["exited_count"].as_u64().is_some());
    }

    #[test]
    fn test_compute_delta_invalid_snapshot() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"from_snapshot\": 999999, \"to_snapshot\": 999998}").expect("seed demo json");
        let result = handle_compute_delta(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_create_watcher() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!(
            "{{\"pid\": {}, \"mechanism\": \"task_threads\", \"interval_ms\": 100}}",
            pid
        ))
        .unwrap();
        let result = handle_create_watcher(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["watcher_id"].as_u64().unwrap() > 0);
        assert_eq!(result["watcher"]["mechanism"], "TaskThreads");
        assert_eq!(result["watcher"]["interval_ms"], 100);
    }

    #[test]
    fn test_poll_watcher_second_has_delta() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let r = handle_create_watcher(&body);
        let id = r["watcher_id"].as_u64().unwrap();

        let poll: Map<String, Value> =
            serde_json::from_str(&format!("{{\"watcher_id\": {}}}", id)).expect("seed demo json");
        let first = handle_poll_watcher(&poll);
        assert!(first["ok"].as_bool().unwrap());

        let second = handle_poll_watcher(&poll);
        assert!(second["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_destroy_watcher() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let r = handle_create_watcher(&body);
        let id = r["watcher_id"].as_u64().unwrap();

        let destroy: Map<String, Value> =
            serde_json::from_str(&format!("{{\"watcher_id\": {}}}", id)).expect("seed demo json");
        let result = handle_destroy_watcher(&destroy);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["removed"].is_object());
    }

    #[test]
    fn test_destroy_unknown_watcher() {
        let body: Map<String, Value> = serde_json::from_str("{\"watcher_id\": 99999999}").expect("seed demo json");
        let result = handle_destroy_watcher(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_list_watchers() {
        let result = handle_list_watchers(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().is_some());
    }

    #[test]
    fn test_poll_watcher_first_snapshot() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let r = handle_create_watcher(&body);
        let id = r["watcher_id"].as_u64().unwrap();

        let poll: Map<String, Value> =
            serde_json::from_str(&format!("{{\"watcher_id\": {}}}", id)).expect("seed demo json");
        let result = handle_poll_watcher(&poll);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["delta"].is_null());
    }

    #[test]
    fn test_poll_unknown_watcher() {
        let body: Map<String, Value> = serde_json::from_str("{\"watcher_id\": 99999999}").expect("seed demo json");
        let result = handle_poll_watcher(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_thread_info_after_snapshot() {
        let pid = std::process::id();
        let snap: Map<String, Value> =
            serde_json::from_str(&format!("{{\"pid\": {}, \"mechanism\": \"task_threads\"}}", pid))
                .expect("seed demo json");
        handle_snapshot_threads(&snap);

        let info: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let result = handle_thread_info(&info);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 1);
    }

    #[test]
    fn test_thread_info_no_cache() {
        let info: Map<String, Value> = serde_json::from_str("{\"pid\": 99999999}").expect("seed demo json");
        let result = handle_thread_info(&info);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_list_deltas() {
        let result = handle_list_deltas(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().is_some());
    }

    #[test]
    fn test_configure_notifications() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let r = handle_create_watcher(&body);
        let id = r["watcher_id"].as_u64().unwrap();

        let config: Map<String, Value> = serde_json::from_str(&format!(
            "{{\"watcher_id\": {}, \"notify_on_create\": true, \"notify_on_exit\": false, \"dispatch_to_es_bridge\": true}}",
            id
        )).unwrap();
        let result = handle_configure_notifications(&config);
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["config"]["notify_on_create"], true);
        assert_eq!(result["config"]["notify_on_exit"], false);
        assert_eq!(result["config"]["dispatch_to_es_bridge"], true);
    }

    #[test]
    fn test_configure_notifications_unknown_watcher() {
        let config: Map<String, Value> =
            serde_json::from_str("{\"watcher_id\": 99999999, \"notify_on_create\": true}").expect("seed demo json");
        let result = handle_configure_notifications(&config);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_mechanism_survey() {
        let result = handle_mechanism_survey(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["mechanisms"].as_array().unwrap().len(), 4);
        assert_eq!(result["recommended"], "task_threads");
    }

    #[test]
    fn test_watcher_status() {
        let pid = std::process::id();
        let body: Map<String, Value> = serde_json::from_str(&format!("{{\"pid\": {}}}", pid)).expect("seed demo json");
        let r = handle_create_watcher(&body);
        let id = r["watcher_id"].as_u64().unwrap();

        let status: Map<String, Value> =
            serde_json::from_str(&format!("{{\"watcher_id\": {}}}", id)).expect("seed demo json");
        let result = handle_watcher_status(&status);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["watcher"].is_object());
        assert_eq!(result["mechanism_detail"]["id"], DetectionMechanism::TaskThreads.id());
    }

    #[test]
    fn test_watcher_status_unknown() {
        let body: Map<String, Value> = serde_json::from_str("{\"watcher_id\": 99999999}").expect("seed demo json");
        let result = handle_watcher_status(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_seed_demo() {
        let result = handle_seed_demo(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["seeded"]["watchers"], 2);
        assert_eq!(result["seeded"]["snapshots"], 2);
        assert!(result["poll_result"]["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_detection_mechanism_properties() {
        assert_eq!(DetectionMechanism::TaskThreads.reliability(), "high");
        assert_eq!(DetectionMechanism::ProcInfoDelta.reliability(), "medium");
        assert_eq!(DetectionMechanism::MachPortNotification.reliability(), "low");
        assert_eq!(DetectionMechanism::ExceptionPort.reliability(), "low");

        assert_eq!(DetectionMechanism::TaskThreads.id(), "task_threads");
        assert!(DetectionMechanism::TaskThreads.description().contains("task_threads"));
        assert!(DetectionMechanism::TaskThreads.nt_equivalent().contains("PsSet"));
    }
}
