use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};

const MAX_COLLECTION_LEN: usize = 4096;

static NEXT_CALLBACK_ID: AtomicU64 = AtomicU64::new(1);
static NEXT_EVENT_ID: AtomicU64 = AtomicU64::new(1);

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[allow(clippy::enum_variant_names)]
pub enum CallbackType {
    ProcessNotify,
    ThreadNotify,
    ImageLoadNotify,
}

impl CallbackType {
    pub fn nt_name(&self) -> &'static str {
        match self {
            Self::ProcessNotify => "PsSetCreateProcessNotifyRoutineEx2",
            Self::ThreadNotify => "PsSetCreateThreadNotifyRoutineEx",
            Self::ImageLoadNotify => "PsSetLoadImageNotifyRoutineEx",
        }
    }

    pub fn es_events(&self) -> &'static [&'static str] {
        match self {
            Self::ProcessNotify => &["ES_EVENT_TYPE_NOTIFY_EXEC", "ES_EVENT_TYPE_NOTIFY_EXIT"],
            Self::ThreadNotify => &["ES_EVENT_TYPE_NOTIFY_THREAD"],
            Self::ImageLoadNotify => &["ES_EVENT_TYPE_NOTIFY_MMAP"],
        }
    }

    fn from_str(s: &str) -> Option<Self> {
        match s {
            "process_notify" | "PsSetCreateProcessNotifyRoutineEx2" => Some(Self::ProcessNotify),
            "thread_notify" | "PsSetCreateThreadNotifyRoutineEx" => Some(Self::ThreadNotify),
            "image_load_notify" | "PsSetLoadImageNotifyRoutineEx" => Some(Self::ImageLoadNotify),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct CallbackRegistration {
    pub id: u64,
    pub callback_type: CallbackType,
    pub nt_routine: String,
    pub es_subscription: Vec<String>,
    pub registered_at: u64,
    pub active: bool,
    pub call_count: u64,
    pub last_fired: Option<u64>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum ProcessAction {
    Created,
    Exited,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum ThreadAction {
    Created,
    Exited,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum ImageAction {
    Loaded,
    Unloaded,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ProcessNotifyEvent {
    pub event_id: u64,
    pub parent_pid: u32,
    pub child_pid: u32,
    pub action: ProcessAction,
    pub image_name: Option<String>,
    pub command_line: Option<String>,
    pub timestamp: u64,
    pub dispatched_to: Vec<u64>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ThreadNotifyEvent {
    pub event_id: u64,
    pub process_id: u32,
    pub thread_id: u64,
    pub action: ThreadAction,
    pub start_address: Option<String>,
    pub timestamp: u64,
    pub dispatched_to: Vec<u64>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct ImageLoadNotifyEvent {
    pub event_id: u64,
    pub process_id: u32,
    pub action: ImageAction,
    pub image_base: String,
    pub image_size: usize,
    pub image_name: String,
    pub image_checksum: Option<u32>,
    pub timestamp: u64,
    pub dispatched_to: Vec<u64>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct MachIpcChannel {
    pub channel_id: u64,
    pub local_port: String,
    pub remote_port: String,
    pub direction: String,
    pub message_type: String,
    pub status: String,
    pub bytes_sent: u64,
    pub bytes_received: u64,
    pub last_activity: Option<u64>,
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct EsSubscriptionStatus {
    pub available: bool,
    pub entitlement: String,
    pub extension_installed: bool,
    pub active_subscriptions: Vec<String>,
    pub events_received: u64,
    pub events_dispatched: u64,
    pub last_event: Option<u64>,
}

static CALLBACKS: std::sync::LazyLock<std::sync::Mutex<BTreeMap<u64, CallbackRegistration>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(BTreeMap::new()));

static PROCESS_EVENTS: std::sync::LazyLock<std::sync::Mutex<Vec<ProcessNotifyEvent>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Vec::new()));

static THREAD_EVENTS: std::sync::LazyLock<std::sync::Mutex<Vec<ThreadNotifyEvent>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Vec::new()));

static IMAGE_EVENTS: std::sync::LazyLock<std::sync::Mutex<Vec<ImageLoadNotifyEvent>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Vec::new()));

static IPC_CHANNELS: std::sync::LazyLock<std::sync::Mutex<Vec<MachIpcChannel>>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(Vec::new()));

static ES_STATUS: std::sync::LazyLock<std::sync::Mutex<EsSubscriptionStatus>> = std::sync::LazyLock::new(|| {
    std::sync::Mutex::new(EsSubscriptionStatus {
        available: false,
        entitlement: "com.apple.developer.endpoint-security.client".to_string(),
        extension_installed: false,
        active_subscriptions: Vec::new(),
        events_received: 0,
        events_dispatched: 0,
        last_event: None,
    })
});

fn lock_callbacks() -> std::sync::MutexGuard<'static, BTreeMap<u64, CallbackRegistration>> {
    match CALLBACKS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_process_events() -> std::sync::MutexGuard<'static, Vec<ProcessNotifyEvent>> {
    match PROCESS_EVENTS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_thread_events() -> std::sync::MutexGuard<'static, Vec<ThreadNotifyEvent>> {
    match THREAD_EVENTS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_image_events() -> std::sync::MutexGuard<'static, Vec<ImageLoadNotifyEvent>> {
    match IMAGE_EVENTS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_ipc_channels() -> std::sync::MutexGuard<'static, Vec<MachIpcChannel>> {
    match IPC_CHANNELS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn lock_es_status() -> std::sync::MutexGuard<'static, EsSubscriptionStatus> {
    match ES_STATUS.lock() {
        Ok(g) => g,
        Err(e) => e.into_inner(),
    }
}

fn now_ms() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_millis() as u64
}

fn dispatch_process_event(event: &ProcessNotifyEvent) -> Vec<u64> {
    let callbacks = lock_callbacks();
    let mut dispatched = Vec::new();
    for cb in callbacks.values() {
        if cb.active && cb.callback_type == CallbackType::ProcessNotify {
            dispatched.push(cb.id);
        }
    }
    dispatched
}

fn dispatch_thread_event(event: &ThreadNotifyEvent) -> Vec<u64> {
    let callbacks = lock_callbacks();
    let mut dispatched = Vec::new();
    for cb in callbacks.values() {
        if cb.active && cb.callback_type == CallbackType::ThreadNotify {
            dispatched.push(cb.id);
        }
    }
    dispatched
}

fn dispatch_image_event(event: &ImageLoadNotifyEvent) -> Vec<u64> {
    let callbacks = lock_callbacks();
    let mut dispatched = Vec::new();
    for cb in callbacks.values() {
        if cb.active && cb.callback_type == CallbackType::ImageLoadNotify {
            dispatched.push(cb.id);
        }
    }
    dispatched
}

#[cfg(not(target_os = "macos"))]
fn detect_own_process_info() -> (u32, Option<String>) {
    let pid = std::process::id();
    let name = std::fs::read_to_string(format!("/proc/{}/comm", pid)).ok();
    (pid, name)
}

#[cfg(target_os = "macos")]
fn detect_own_process_info() -> (u32, Option<String>) {
    let pid = std::process::id();
    (pid, None)
}

pub fn handle_register_callback(body: &Map<String, Value>) -> Value {
    let type_str = match body.get("callback_type").and_then(|v| v.as_str()) {
        Some(s) => s,
        None => {
            return json!({"ok": false, "error": "callback_type required: process_notify, thread_notify, image_load_notify"})
        },
    };

    let callback_type = match CallbackType::from_str(type_str) {
        Some(ct) => ct,
        None => return json!({"ok": false, "error": format!("unknown callback_type: {}", type_str)}),
    };

    let id = NEXT_CALLBACK_ID.fetch_add(1, Ordering::Relaxed);
    let registration = CallbackRegistration {
        id,
        callback_type,
        nt_routine: callback_type.nt_name().to_string(),
        es_subscription: callback_type.es_events().iter().map(|s| s.to_string()).collect(),
        registered_at: now_ms(),
        active: true,
        call_count: 0,
        last_fired: None,
    };

    lock_callbacks().insert(id, registration.clone());

    let mut status = lock_es_status();
    for event in callback_type.es_events() {
        if !status.active_subscriptions.contains(&event.to_string()) {
            status.active_subscriptions.push(event.to_string());
        }
    }

    json!({
        "ok": true,
        "callback_id": id,
        "registration": registration,
    })
}

pub fn handle_unregister_callback(body: &Map<String, Value>) -> Value {
    let id = match body.get("callback_id").and_then(|v| v.as_u64()) {
        Some(id) => id,
        None => return json!({"ok": false, "error": "callback_id required"}),
    };

    let mut callbacks = lock_callbacks();
    match callbacks.remove(&id) {
        Some(removed) => json!({
            "ok": true,
            "removed": removed,
        }),
        None => json!({"ok": false, "error": format!("callback {} not found", id)}),
    }
}

pub fn handle_list_callbacks(_body: &Map<String, Value>) -> Value {
    let callbacks = lock_callbacks();
    let list: Vec<&CallbackRegistration> = callbacks.values().collect();
    json!({
        "ok": true,
        "count": list.len(),
        "callbacks": list,
    })
}

pub fn handle_fire_process_event(body: &Map<String, Value>) -> Value {
    let parent_pid = match body.get("parent_pid").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "parent_pid (u32) required"}),
    };
    let child_pid = match body.get("child_pid").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "child_pid (u32) required"}),
    };
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some("created") => ProcessAction::Created,
        Some("exited") => ProcessAction::Exited,
        _ => return json!({"ok": false, "error": "action required: created or exited"}),
    };

    let event_id = NEXT_EVENT_ID.fetch_add(1, Ordering::Relaxed);
    let mut event = ProcessNotifyEvent {
        event_id,
        parent_pid,
        child_pid,
        action,
        image_name: body.get("image_name").and_then(|v| v.as_str()).map(|s| s.to_string()),
        command_line: body.get("command_line").and_then(|v| v.as_str()).map(|s| s.to_string()),
        timestamp: now_ms(),
        dispatched_to: Vec::new(),
    };

    event.dispatched_to = dispatch_process_event(&event);

    {
        let mut callbacks = lock_callbacks();
        for &cb_id in &event.dispatched_to {
            if let Some(cb) = callbacks.get_mut(&cb_id) {
                cb.call_count += 1;
                cb.last_fired = Some(event.timestamp);
            }
        }
    }
    {
        let mut status = lock_es_status();
        status.events_received += 1;
        status.events_dispatched += event.dispatched_to.len() as u64;
        status.last_event = Some(event.timestamp);
    }

    {
        let mut evts = lock_process_events();
        evts.push(event.clone());
        if evts.len() > MAX_COLLECTION_LEN {
            let excess = evts.len() - MAX_COLLECTION_LEN;
            evts.drain(0..excess);
        }
    }

    json!({
        "ok": true,
        "event_id": event.event_id,
        "dispatched_to_count": event.dispatched_to.len(),
        "dispatched_to": event.dispatched_to,
        "event": event,
    })
}

pub fn handle_fire_thread_event(body: &Map<String, Value>) -> Value {
    let process_id = match body.get("process_id").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "process_id (u32) required"}),
    };
    let thread_id = match body.get("thread_id").and_then(|v| v.as_u64()) {
        Some(t) => t,
        None => return json!({"ok": false, "error": "thread_id (u64) required"}),
    };
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some("created") => ThreadAction::Created,
        Some("exited") => ThreadAction::Exited,
        _ => return json!({"ok": false, "error": "action required: created or exited"}),
    };

    let event_id = NEXT_EVENT_ID.fetch_add(1, Ordering::Relaxed);
    let mut event = ThreadNotifyEvent {
        event_id,
        process_id,
        thread_id,
        action,
        start_address: body.get("start_address").and_then(|v| v.as_str()).map(|s| s.to_string()),
        timestamp: now_ms(),
        dispatched_to: Vec::new(),
    };

    event.dispatched_to = dispatch_thread_event(&event);

    {
        let mut callbacks = lock_callbacks();
        for &cb_id in &event.dispatched_to {
            if let Some(cb) = callbacks.get_mut(&cb_id) {
                cb.call_count += 1;
                cb.last_fired = Some(event.timestamp);
            }
        }
    }
    {
        let mut status = lock_es_status();
        status.events_received += 1;
        status.events_dispatched += event.dispatched_to.len() as u64;
        status.last_event = Some(event.timestamp);
    }

    {
        let mut evts = lock_thread_events();
        evts.push(event.clone());
        if evts.len() > MAX_COLLECTION_LEN {
            let excess = evts.len() - MAX_COLLECTION_LEN;
            evts.drain(0..excess);
        }
    }

    json!({
        "ok": true,
        "event_id": event.event_id,
        "dispatched_to_count": event.dispatched_to.len(),
        "dispatched_to": event.dispatched_to,
        "event": event,
    })
}

pub fn handle_fire_image_event(body: &Map<String, Value>) -> Value {
    let process_id = match body.get("process_id").and_then(|v| v.as_u64()) {
        Some(p) if p <= u32::MAX as u64 => p as u32,
        _ => return json!({"ok": false, "error": "process_id (u32) required"}),
    };
    let action = match body.get("action").and_then(|v| v.as_str()) {
        Some("loaded") => ImageAction::Loaded,
        Some("unloaded") => ImageAction::Unloaded,
        _ => return json!({"ok": false, "error": "action required: loaded or unloaded"}),
    };
    let image_name = body.get("image_name").and_then(|v| v.as_str()).unwrap_or("unknown").to_string();
    let image_base = body.get("image_base").and_then(|v| v.as_str()).unwrap_or("0x00000000").to_string();
    let image_size = body.get("image_size").and_then(|v| v.as_u64()).unwrap_or(0) as usize;

    let event_id = NEXT_EVENT_ID.fetch_add(1, Ordering::Relaxed);
    let mut event = ImageLoadNotifyEvent {
        event_id,
        process_id,
        action,
        image_base,
        image_size,
        image_name,
        image_checksum: body.get("image_checksum").and_then(|v| v.as_u64()).map(|v| v as u32),
        timestamp: now_ms(),
        dispatched_to: Vec::new(),
    };

    event.dispatched_to = dispatch_image_event(&event);

    {
        let mut callbacks = lock_callbacks();
        for &cb_id in &event.dispatched_to {
            if let Some(cb) = callbacks.get_mut(&cb_id) {
                cb.call_count += 1;
                cb.last_fired = Some(event.timestamp);
            }
        }
    }
    {
        let mut status = lock_es_status();
        status.events_received += 1;
        status.events_dispatched += event.dispatched_to.len() as u64;
        status.last_event = Some(event.timestamp);
    }

    {
        let mut evts = lock_image_events();
        evts.push(event.clone());
        if evts.len() > MAX_COLLECTION_LEN {
            let excess = evts.len() - MAX_COLLECTION_LEN;
            evts.drain(0..excess);
        }
    }

    json!({
        "ok": true,
        "event_id": event.event_id,
        "dispatched_to_count": event.dispatched_to.len(),
        "dispatched_to": event.dispatched_to,
        "event": event,
    })
}

pub fn handle_process_events(_body: &Map<String, Value>) -> Value {
    let events = lock_process_events();
    json!({
        "ok": true,
        "count": events.len(),
        "events": events.iter().collect::<Vec<_>>(),
    })
}

pub fn handle_thread_events(_body: &Map<String, Value>) -> Value {
    let events = lock_thread_events();
    json!({
        "ok": true,
        "count": events.len(),
        "events": events.iter().collect::<Vec<_>>(),
    })
}

pub fn handle_image_events(_body: &Map<String, Value>) -> Value {
    let events = lock_image_events();
    json!({
        "ok": true,
        "count": events.len(),
        "events": events.iter().collect::<Vec<_>>(),
    })
}

pub fn handle_create_ipc_channel(body: &Map<String, Value>) -> Value {
    let direction = body.get("direction").and_then(|v| v.as_str()).unwrap_or("bidirectional");
    let message_type = body.get("message_type").and_then(|v| v.as_str()).unwrap_or("es_event");

    let channel_id = NEXT_CALLBACK_ID.fetch_add(1, Ordering::Relaxed);
    let channel = MachIpcChannel {
        channel_id,
        local_port: format!("0x{:08x}", 0x4100 + channel_id as u32),
        remote_port: format!("0x{:08x}", 0x8200 + channel_id as u32),
        direction: direction.to_string(),
        message_type: message_type.to_string(),
        status: "active".to_string(),
        bytes_sent: 0,
        bytes_received: 0,
        last_activity: Some(now_ms()),
    };

    lock_ipc_channels().push(channel.clone());

    json!({
        "ok": true,
        "channel": channel,
    })
}

pub fn handle_ipc_channels(_body: &Map<String, Value>) -> Value {
    let channels = lock_ipc_channels();
    json!({
        "ok": true,
        "count": channels.len(),
        "channels": channels.iter().collect::<Vec<_>>(),
    })
}

pub fn handle_es_status(_body: &Map<String, Value>) -> Value {
    let status = lock_es_status();
    let callbacks = lock_callbacks();
    let process_events = lock_process_events();
    let thread_events = lock_thread_events();
    let image_events = lock_image_events();
    let channels = lock_ipc_channels();

    let callback_counts: BTreeMap<String, usize> = callbacks.values().fold(BTreeMap::new(), |mut acc, cb| {
        let key = cb.callback_type.nt_name().to_string();
        *acc.entry(key).or_insert(0) += 1;
        acc
    });

    json!({
        "ok": true,
        "es": *status,
        "callbacks": {
            "total": callbacks.len(),
            "active": callbacks.values().filter(|c| c.active).count(),
            "by_type": callback_counts,
        },
        "events": {
            "process": process_events.len(),
            "thread": thread_events.len(),
            "image": image_events.len(),
            "total": process_events.len() + thread_events.len() + image_events.len(),
        },
        "ipc_channels": channels.len(),
        "translation_map": {
            "process_notify": {
                "nt_callback": "PsSetCreateProcessNotifyRoutineEx2",
                "es_events": ["ES_EVENT_TYPE_NOTIFY_EXEC", "ES_EVENT_TYPE_NOTIFY_EXIT"],
                "xnu_mechanism": "fork/exec → proc_info → EndpointSecurity",
            },
            "thread_notify": {
                "nt_callback": "PsSetCreateThreadNotifyRoutineEx",
                "es_events": ["ES_EVENT_TYPE_NOTIFY_THREAD"],
                "xnu_mechanism": "bsdthread_create → task_threads polling",
            },
            "image_load_notify": {
                "nt_callback": "PsSetLoadImageNotifyRoutineEx",
                "es_events": ["ES_EVENT_TYPE_NOTIFY_EXEC", "ES_EVENT_TYPE_NOTIFY_MMAP"],
                "xnu_mechanism": "mmap(PROT_EXEC) → EndpointSecurity NOTIFY_MMAP",
            },
        },
    })
}

pub fn handle_detect_events(body: &Map<String, Value>) -> Value {
    let (own_pid, _own_name) = detect_own_process_info();
    let watch_pid = body.get("watch_pid").and_then(|v| v.as_u64()).map(|p| p as u32);
    let watch_tid = body.get("watch_tid").and_then(|v| v.as_u64());

    let mut detected = Vec::new();
    let now = now_ms();

    if let Some(pid) = watch_pid {
        let process_events = lock_process_events();
        for evt in process_events.iter() {
            if evt.child_pid == pid || evt.parent_pid == pid {
                detected.push(json!({
                    "type": "process",
                    "event_id": evt.event_id,
                    "action": evt.action,
                    "parent_pid": evt.parent_pid,
                    "child_pid": evt.child_pid,
                    "image_name": evt.image_name,
                    "age_ms": now.saturating_sub(evt.timestamp),
                }));
            }
        }

        let thread_events = lock_thread_events();
        for evt in thread_events.iter() {
            if evt.process_id == pid {
                detected.push(json!({
                    "type": "thread",
                    "event_id": evt.event_id,
                    "action": evt.action,
                    "process_id": evt.process_id,
                    "thread_id": evt.thread_id,
                    "age_ms": now.saturating_sub(evt.timestamp),
                }));
            }
        }

        let image_events = lock_image_events();
        for evt in image_events.iter() {
            if evt.process_id == pid {
                detected.push(json!({
                    "type": "image",
                    "event_id": evt.event_id,
                    "action": evt.action,
                    "image_name": evt.image_name,
                    "image_base": evt.image_base,
                    "age_ms": now.saturating_sub(evt.timestamp),
                }));
            }
        }
    }

    if let Some(tid) = watch_tid {
        let thread_events = lock_thread_events();
        for evt in thread_events.iter() {
            if evt.thread_id == tid {
                detected.push(json!({
                    "type": "thread",
                    "event_id": evt.event_id,
                    "action": evt.action,
                    "process_id": evt.process_id,
                    "thread_id": evt.thread_id,
                    "age_ms": now.saturating_sub(evt.timestamp),
                }));
            }
        }
    }

    json!({
        "ok": true,
        "own_pid": own_pid,
        "watch_pid": watch_pid,
        "watch_tid": watch_tid,
        "detected_count": detected.len(),
        "events": detected,
    })
}

pub fn handle_nt_callback_bridge(body: &Map<String, Value>) -> Value {
    let nt_routine = match body.get("nt_routine").and_then(|v| v.as_str()) {
        Some(s) => s,
        None => return json!({"ok": false, "error": "nt_routine required"}),
    };

    let callback_type = match CallbackType::from_str(nt_routine) {
        Some(ct) => ct,
        None => return json!({"ok": false, "error": format!("unknown NT routine: {}", nt_routine)}),
    };

    let callbacks = lock_callbacks();
    let matching: Vec<&CallbackRegistration> =
        callbacks.values().filter(|c| c.callback_type == callback_type && c.active).collect();

    let (process_events, thread_events, image_events) = {
        let pe = lock_process_events();
        let te = lock_thread_events();
        let ie = lock_image_events();
        (pe.len(), te.len(), ie.len())
    };

    let total_calls: u64 = matching.iter().map(|c| c.call_count).sum();

    json!({
        "ok": true,
        "nt_routine": nt_routine,
        "es_events": callback_type.es_events(),
        "registered_callbacks": matching.len(),
        "total_callback_invocations": total_calls,
        "queued_events": {
            "process": process_events,
            "thread": thread_events,
            "image": image_events,
        },
        "bridge_status": if matching.is_empty() { "no_callbacks_registered" } else { "active" },
        "translation": format!(
            "NT {} → ES [{}] → {} registered callbacks ({} total invocations)",
            nt_routine,
            callback_type.es_events().join(", "),
            matching.len(),
            total_calls,
        ),
    })
}

pub fn handle_seed_demo(_body: &Map<String, Value>) -> Value {
    let now = now_ms();
    let own_pid = std::process::id();

    let r1 = handle_register_callback(&serde_json::from_str("{\"callback_type\": \"process_notify\"}").unwrap());
    let r2 = handle_register_callback(&serde_json::from_str("{\"callback_type\": \"image_load_notify\"}").unwrap());
    let r3 = handle_register_callback(&serde_json::from_str("{\"callback_type\": \"thread_notify\"}").unwrap());

    let cb1 = r1["callback_id"].as_u64().unwrap();
    let cb2 = r2["callback_id"].as_u64().unwrap();
    let cb3 = r3["callback_id"].as_u64().unwrap();

    let e1 = handle_fire_process_event(&serde_json::from_str(&format!(
        "{{\"parent_pid\": {}, \"child_pid\": 9999, \"action\": \"created\", \"image_name\": \"game.exe\", \"command_line\": \"game.exe --anti-cheat\"}}",
        own_pid
    )).unwrap());

    let e2 = handle_fire_image_event(&serde_json::from_str(
        "{\"process_id\": 9999, \"action\": \"loaded\", \"image_name\": \"ntdll.dll\", \"image_base\": \"0x7ffe00000000\", \"image_size\": 2097152}",
    ).unwrap());

    let e3 = handle_fire_thread_event(
        &serde_json::from_str(
        "{\"process_id\": 9999, \"thread_id\": 7777, \"action\": \"created\", \"start_address\": \"0x7ffe00001000\"}",
    )
        .unwrap(),
    );

    let e4 = handle_fire_process_event(
        &serde_json::from_str(&format!("{{\"parent_pid\": {}, \"child_pid\": 9999, \"action\": \"exited\"}}", own_pid))
            .unwrap(),
    );

    handle_create_ipc_channel(
        &serde_json::from_str("{\"direction\": \"bidirectional\", \"message_type\": \"es_event\"}").unwrap(),
    );

    json!({
        "ok": true,
        "seeded": {
            "callbacks_registered": 3,
            "callback_ids": [cb1, cb2, cb3],
            "events_fired": 4,
            "event_ids": [
                e1["event_id"].as_u64().unwrap(),
                e2["event_id"].as_u64().unwrap(),
                e3["event_id"].as_u64().unwrap(),
                e4["event_id"].as_u64().unwrap(),
            ],
        },
        "scenario": "Anti-cheat game launch: process created → ntdll loaded → worker thread created → process exited",
        "own_pid": own_pid,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_body() -> Map<String, Value> {
        Map::new()
    }

    #[test]
    fn test_register_process_callback() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let result = handle_register_callback(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["callback_id"].as_u64().unwrap() > 0);
        assert_eq!(result["registration"]["nt_routine"], "PsSetCreateProcessNotifyRoutineEx2");
        assert_eq!(result["registration"]["active"], true);

        let id = result["callback_id"].as_u64().unwrap();
        let callbacks = lock_callbacks();
        assert!(callbacks.contains_key(&id));
        let cb = &callbacks[&id];
        assert_eq!(cb.callback_type, CallbackType::ProcessNotify);
        assert_eq!(cb.es_subscription, vec!["ES_EVENT_TYPE_NOTIFY_EXEC", "ES_EVENT_TYPE_NOTIFY_EXIT"]);
    }

    #[test]
    fn test_register_image_load_callback() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"image_load_notify\"}").expect("seed demo json");
        let result = handle_register_callback(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["registration"]["nt_routine"], "PsSetLoadImageNotifyRoutineEx");
        let es = result["registration"]["es_subscription"].as_array().unwrap();
        assert!(es.iter().any(|e| e == "ES_EVENT_TYPE_NOTIFY_MMAP"));
        assert!(!es.iter().any(|e| e == "ES_EVENT_TYPE_NOTIFY_EXEC"));
    }

    #[test]
    fn test_register_thread_callback() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"thread_notify\"}").expect("seed demo json");
        let result = handle_register_callback(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["registration"]["nt_routine"], "PsSetCreateThreadNotifyRoutineEx");
    }

    #[test]
    fn test_register_unknown_callback_fails() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"unknown\"}").expect("seed demo json");
        let result = handle_register_callback(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_unregister_callback() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let result = handle_register_callback(&body);
        let id = result["callback_id"].as_u64().unwrap();

        let unreg: Map<String, Value> =
            serde_json::from_str(&format!("{{\"callback_id\": {}}}", id)).expect("seed demo json");
        let unreg_result = handle_unregister_callback(&unreg);
        assert!(unreg_result["ok"].as_bool().unwrap());
        assert!(unreg_result["removed"].is_object());

        let callbacks = lock_callbacks();
        assert!(!callbacks.contains_key(&id));
    }

    #[test]
    fn test_unregister_unknown_fails() {
        let body: Map<String, Value> = serde_json::from_str("{\"callback_id\": 99999999}").expect("seed demo json");
        let result = handle_unregister_callback(&body);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_fire_process_created_event() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        handle_register_callback(&reg);

        let fire: Map<String, Value> = serde_json::from_str(
            "{\"parent_pid\": 100, \"child_pid\": 200, \"action\": \"created\", \"image_name\": \"game.exe\"}",
        )
        .unwrap();
        let result = handle_fire_process_event(&fire);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["dispatched_to_count"].as_u64().unwrap() >= 1);
        assert_eq!(result["event"]["action"], "Created");
        assert_eq!(result["event"]["parent_pid"], 100);
        assert_eq!(result["event"]["child_pid"], 200);
    }

    #[test]
    fn test_fire_process_exit_event() {
        let fire: Map<String, Value> =
            serde_json::from_str("{\"parent_pid\": 100, \"child_pid\": 200, \"action\": \"exited\"}")
                .expect("seed demo json");
        let result = handle_fire_process_event(&fire);
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["event"]["action"], "Exited");
    }

    #[test]
    fn test_fire_thread_event() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"thread_notify\"}").expect("seed demo json");
        handle_register_callback(&reg);

        let fire: Map<String, Value> = serde_json::from_str(
            "{\"process_id\": 100, \"thread_id\": 500, \"action\": \"created\", \"start_address\": \"0x1000\"}",
        )
        .unwrap();
        let result = handle_fire_thread_event(&fire);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["dispatched_to_count"].as_u64().unwrap() >= 1);
        assert_eq!(result["event"]["thread_id"], 500);
    }

    #[test]
    fn test_fire_image_event() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"image_load_notify\"}").expect("seed demo json");
        handle_register_callback(&reg);

        let fire: Map<String, Value> = serde_json::from_str(
            "{\"process_id\": 100, \"action\": \"loaded\", \"image_name\": \"kernel32.dll\", \"image_base\": \"0x7ffe00100000\", \"image_size\": 1048576}"
        ).unwrap();
        let result = handle_fire_image_event(&fire);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["dispatched_to_count"].as_u64().unwrap() >= 1);
        assert_eq!(result["event"]["image_name"], "kernel32.dll");
    }

    #[test]
    fn test_dispatch_to_multiple_callbacks() {
        let reg1: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let reg2: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let r1 = handle_register_callback(&reg1);
        let r2 = handle_register_callback(&reg2);
        let id1 = r1["callback_id"].as_u64().unwrap();
        let id2 = r2["callback_id"].as_u64().unwrap();

        let fire: Map<String, Value> =
            serde_json::from_str("{\"parent_pid\": 1, \"child_pid\": 2, \"action\": \"created\"}")
                .expect("seed demo json");
        let result = handle_fire_process_event(&fire);
        let dispatched = result["dispatched_to"].as_array().unwrap();
        assert!(dispatched.iter().any(|d| d.as_u64() == Some(id1)));
        assert!(dispatched.iter().any(|d| d.as_u64() == Some(id2)));
    }

    #[test]
    fn test_no_dispatch_to_wrong_callback_type() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"image_load_notify\"}").expect("seed demo json");
        let r = handle_register_callback(&reg);
        let id = r["callback_id"].as_u64().unwrap();

        let fire: Map<String, Value> =
            serde_json::from_str("{\"parent_pid\": 1, \"child_pid\": 2, \"action\": \"created\"}")
                .expect("seed demo json");
        let result = handle_fire_process_event(&fire);
        let dispatched = result["dispatched_to"].as_array().unwrap();
        assert!(!dispatched.iter().any(|d| d.as_u64() == Some(id)));
    }

    #[test]
    fn test_callback_call_count_tracking() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let reg_result = handle_register_callback(&reg);
        let cb_id = reg_result["callback_id"].as_u64().unwrap();

        let before = lock_callbacks().get(&cb_id).map(|c| c.call_count).unwrap_or(0);

        for i in 0..3 {
            let fire: Map<String, Value> = serde_json::from_str(&format!(
                "{{\"parent_pid\": 1, \"child_pid\": {}, \"action\": \"created\"}}",
                i + 100
            ))
            .unwrap();
            handle_fire_process_event(&fire);
        }

        let callbacks = lock_callbacks();
        let cb = &callbacks[&cb_id];
        assert!(cb.call_count >= before + 3, "expected call_count >= {} but got {}", before + 3, cb.call_count);
        assert!(cb.last_fired.is_some());
    }

    #[test]
    fn test_es_status() {
        let result = handle_es_status(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["translation_map"].is_object());
        assert_eq!(result["es"]["entitlement"], "com.apple.developer.endpoint-security.client");
    }

    #[test]
    fn test_nt_callback_bridge() {
        let reg: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let r = handle_register_callback(&reg);
        let id = r["callback_id"].as_u64().unwrap();

        let bridge: Map<String, Value> =
            serde_json::from_str("{\"nt_routine\": \"PsSetCreateProcessNotifyRoutineEx2\"}").expect("seed demo json");
        let result = handle_nt_callback_bridge(&bridge);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["registered_callbacks"].as_u64().unwrap() >= 1);
        assert_eq!(result["bridge_status"], "active");

        let dispatched = result["registered_callbacks"].as_u64().unwrap();
        assert!(dispatched >= 1);
    }

    #[test]
    fn test_detect_events_by_pid() {
        let unique_child: u32 = 50000 + (std::process::id() % 10000) as u32;
        let fire: Map<String, Value> = serde_json::from_str(&format!(
            "{{\"parent_pid\": 1, \"child_pid\": {}, \"action\": \"created\", \"image_name\": \"test.exe\"}}",
            unique_child
        ))
        .unwrap();
        handle_fire_process_event(&fire);

        let detect: Map<String, Value> =
            serde_json::from_str(&format!("{{\"watch_pid\": {}}}", unique_child)).expect("seed demo json");
        let result = handle_detect_events(&detect);
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["detected_count"].as_u64().unwrap() >= 1);
    }

    #[test]
    fn test_detect_events_no_match() {
        let detect: Map<String, Value> = serde_json::from_str("{\"watch_pid\": 99999999}").expect("seed demo json");
        let result = handle_detect_events(&detect);
        assert_eq!(result["detected_count"], 0);
    }

    #[test]
    fn test_ipc_channel_create() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"direction\": \"bidirectional\", \"message_type\": \"es_event\"}")
                .expect("seed demo json");
        let result = handle_create_ipc_channel(&body);
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["channel"]["direction"], "bidirectional");
        assert_eq!(result["channel"]["status"], "active");
    }

    #[test]
    fn test_list_ipc_channels() {
        let body: Map<String, Value> =
            serde_json::from_str("{\"direction\": \"bidirectional\", \"message_type\": \"es_event\"}")
                .expect("seed demo json");
        handle_create_ipc_channel(&body);

        let result = handle_ipc_channels(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 1);
    }

    #[test]
    fn test_seed_demo() {
        let result = handle_seed_demo(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert_eq!(result["seeded"]["callbacks_registered"], 3);
        assert_eq!(result["seeded"]["events_fired"], 4);

        let cb_ids = result["seeded"]["callback_ids"].as_array().unwrap();
        assert_eq!(cb_ids.len(), 3);
        for id in cb_ids {
            let id = id.as_u64().unwrap();
            let callbacks = lock_callbacks();
            assert!(callbacks.contains_key(&id));
        }

        let event_ids = result["seeded"]["event_ids"].as_array().unwrap();
        assert_eq!(event_ids.len(), 4);
    }

    #[test]
    fn test_list_callbacks() {
        let reg1: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"process_notify\"}").expect("seed demo json");
        let reg2: Map<String, Value> =
            serde_json::from_str("{\"callback_type\": \"thread_notify\"}").expect("seed demo json");
        let r1 = handle_register_callback(&reg1);
        let r2 = handle_register_callback(&reg2);
        let id1 = r1["callback_id"].as_u64().unwrap();
        let id2 = r2["callback_id"].as_u64().unwrap();

        let result = handle_list_callbacks(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 2);

        let ids: Vec<u64> = result["callbacks"].as_array().unwrap().iter().filter_map(|c| c["id"].as_u64()).collect();
        assert!(ids.contains(&id1));
        assert!(ids.contains(&id2));
    }

    #[test]
    fn test_process_events_list() {
        let fire: Map<String, Value> =
            serde_json::from_str("{\"parent_pid\": 1, \"child_pid\": 2, \"action\": \"created\"}")
                .expect("seed demo json");
        let r = handle_fire_process_event(&fire);
        let eid = r["event_id"].as_u64().unwrap();

        let result = handle_process_events(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 1);

        let ids: Vec<u64> =
            result["events"].as_array().unwrap().iter().filter_map(|e| e["event_id"].as_u64()).collect();
        assert!(ids.contains(&eid));
    }

    #[test]
    fn test_thread_events_list() {
        let fire: Map<String, Value> =
            serde_json::from_str("{\"process_id\": 1, \"thread_id\": 2, \"action\": \"created\"}")
                .expect("seed demo json");
        let r = handle_fire_thread_event(&fire);
        let eid = r["event_id"].as_u64().unwrap();

        let result = handle_thread_events(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 1);

        let ids: Vec<u64> =
            result["events"].as_array().unwrap().iter().filter_map(|e| e["event_id"].as_u64()).collect();
        assert!(ids.contains(&eid));
    }

    #[test]
    fn test_image_events_list() {
        let fire: Map<String, Value> =
            serde_json::from_str("{\"process_id\": 1, \"action\": \"loaded\", \"image_name\": \"test.dll\"}")
                .expect("seed demo json");
        let r = handle_fire_image_event(&fire);
        let eid = r["event_id"].as_u64().unwrap();

        let result = handle_image_events(&empty_body());
        assert!(result["ok"].as_bool().unwrap());
        assert!(result["count"].as_u64().unwrap() >= 1);

        let ids: Vec<u64> =
            result["events"].as_array().unwrap().iter().filter_map(|e| e["event_id"].as_u64()).collect();
        assert!(ids.contains(&eid));
    }

    #[test]
    fn test_fire_event_invalid_action() {
        let fire: Map<String, Value> =
            serde_json::from_str("{\"parent_pid\": 1, \"child_pid\": 2, \"action\": \"invalid\"}")
                .expect("seed demo json");
        let result = handle_fire_process_event(&fire);
        assert!(!result["ok"].as_bool().unwrap());
    }

    #[test]
    fn test_fire_event_missing_pid() {
        let fire: Map<String, Value> = serde_json::from_str("{\"action\": \"created\"}").expect("seed demo json");
        let result = handle_fire_process_event(&fire);
        assert!(!result["ok"].as_bool().unwrap());
    }
}
