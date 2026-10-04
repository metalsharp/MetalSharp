//! Production connection to the authoritative C backend, shared by every page.
//!
//! Mirrors the Electron renderer's `api()` helper: every request runs off the
//! UI thread, independent requests may overlap (the backend serialises them),
//! and the callback receives `data ?? body` or `None` when the transport fails.
use crate::backend_host::{BackendHost, HostConfig};
use gpui::{App, Context, Global};
use serde_json::Value;
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(30);

struct Inner {
    base_url: String,
    agent: ureq::Agent,
    token: Mutex<Option<String>>,
    host: Mutex<Option<BackendHost>>,
    config: HostConfig,
}

#[derive(Clone)]
pub struct Live(Arc<Inner>);
impl Global for Live {}

impl Live {
    pub fn new(config: HostConfig) -> Self {
        let agent = ureq::Agent::config_builder()
            .timeout_global(Some(DEFAULT_TIMEOUT))
            .proxy(None)
            .max_redirects(0)
            .http_status_as_error(false)
            .build()
            .into();
        Self(Arc::new(Inner {
            base_url: format!("http://127.0.0.1:{}", config.port),
            agent,
            token: Mutex::new(None),
            host: Mutex::new(None),
            config,
        }))
    }

    pub fn get(cx: &App) -> Option<Live> {
        cx.try_global::<Live>().cloned()
    }

    pub fn home(&self) -> PathBuf {
        self.0.config.home.clone()
    }

    pub fn resources(&self) -> PathBuf {
        self.0.config.resources.clone()
    }

    /// Spawn (or respawn) the owned backend. Blocking: call off the UI thread.
    pub fn start_backend(&self) -> Result<(), String> {
        // Never hold the host lock across stop/start: UI-thread readers
        // (health check, quit, updater pid) must not stall behind a restart.
        let previous = self.0.host.lock().unwrap().take();
        if let Some(mut previous) = previous {
            previous.stop();
        }
        let host = BackendHost::start(&self.0.config).map_err(|error| format!("{error:#}"))?;
        *self.0.token.lock().unwrap() = host.client().token().map(str::to_owned);
        *self.0.host.lock().unwrap() = Some(host);
        Ok(())
    }

    pub fn stop_backend(&self) {
        if let Some(mut host) = self.0.host.lock().unwrap().take() {
            host.stop();
        }
    }

    /// Non-blocking stop for app quit (see `BackendHost::terminate_detached`).
    pub fn stop_backend_detached(&self) {
        if let Some(host) = self.0.host.lock().unwrap().take() {
            host.terminate_detached();
        }
    }

    pub fn backend_pid(&self) -> Option<u32> {
        self.0.host.lock().unwrap().as_ref().map(BackendHost::pid)
    }

    pub fn backend_alive(&self) -> bool {
        self.0
            .host
            .lock()
            .unwrap()
            .as_mut()
            .is_some_and(BackendHost::is_running)
    }

    /// Blocking request. `Err` only for transport/parse failures; a response
    /// with `ok:false` is returned as `Ok` so callers can surface its `error`.
    pub fn request(
        &self,
        method: &str,
        path: &str,
        body: Option<&Value>,
        timeout: Duration,
    ) -> Result<Value, String> {
        if !path.starts_with('/') || path.starts_with("//") || path.chars().any(char::is_control) {
            return Err("Invalid backend route".into());
        }
        let url = format!("{}{path}", self.0.base_url);
        let token = self.0.token.lock().unwrap().clone();
        let response = match method {
            "GET" => {
                let mut request = self.0.agent.get(&url);
                if let Some(token) = &token {
                    request = request.header("X-MetalSharp-Client-Token", token);
                }
                request
                    .config()
                    .timeout_global(Some(timeout))
                    .build()
                    .call()
            }
            "POST" => {
                let payload =
                    serde_json::to_vec(body.unwrap_or(&Value::Object(Default::default())))
                        .map_err(|error| error.to_string())?;
                let mut request = self.0.agent.post(&url);
                if let Some(token) = &token {
                    request = request.header("X-MetalSharp-Client-Token", token);
                }
                request
                    .header("Content-Type", "application/json")
                    .config()
                    .timeout_global(Some(timeout))
                    .build()
                    .send(payload.as_slice())
            }
            _ => return Err("Unsupported method".into()),
        };
        let mut response = response.map_err(|error| match error {
            ureq::Error::Timeout(_) => "request timeout".to_owned(),
            other => other.to_string(),
        })?;
        let status = response.status().as_u16();
        let value: Value = response
            .body_mut()
            .with_config()
            .limit(64 * 1024 * 1024)
            .read_json()
            .unwrap_or_else(|_| Value::Object(Default::default()));
        if !(200..300).contains(&status) && value.get("ok").and_then(Value::as_bool) != Some(false)
        {
            return Ok(
                serde_json::json!({"ok": false, "error": format!("Backend returned HTTP {status}")}),
            );
        }
        Ok(value)
    }

    /// Raw bytes (artwork routes such as `/art/grid/<appid>/<kind>`).
    pub fn get_bytes(&self, path: &str, timeout: Duration) -> Option<Vec<u8>> {
        let url = format!("{}{path}", self.0.base_url);
        let token = self.0.token.lock().unwrap().clone();
        let mut request = self.0.agent.get(&url);
        if let Some(token) = &token {
            request = request.header("X-MetalSharp-Client-Token", token);
        }
        let mut response = request
            .config()
            .timeout_global(Some(timeout))
            .build()
            .call()
            .ok()?;
        if response.status().as_u16() != 200 {
            return None;
        }
        response
            .body_mut()
            .with_config()
            .limit(32 * 1024 * 1024)
            .read_to_vec()
            .ok()
            .filter(|bytes| !bytes.is_empty())
    }
}

/// Electron `api()` result unwrapping: `res.data ?? res`.
pub fn unwrap_data(value: Value) -> Value {
    match value.get("data") {
        Some(data) if !data.is_null() => data.clone(),
        _ => value,
    }
}

pub fn is_ok(value: &Value) -> bool {
    value.get("ok").and_then(Value::as_bool) == Some(true)
}

pub fn error_text(value: Option<&Value>) -> Option<String> {
    value
        .and_then(|value| value.get("error"))
        .and_then(Value::as_str)
        .filter(|error| !error.is_empty())
        .map(str::to_owned)
}

/// Fire a backend request from an entity and deliver the result on the UI
/// thread. `None` mirrors Electron's `api()` returning `null` on failure.
pub fn call<V: 'static>(
    cx: &mut Context<V>,
    method: &'static str,
    path: impl Into<String>,
    body: Option<Value>,
    timeout: Duration,
    done: impl FnOnce(&mut V, Option<Value>, &mut Context<V>) + 'static,
) {
    let path = path.into();
    let Some(live) = Live::get(cx) else {
        return;
    };
    cx.spawn(async move |this, cx| {
        let result = cx
            .background_executor()
            .spawn(async move { live.request(method, &path, body.as_ref(), timeout) })
            .await;
        let value = result.ok().map(unwrap_data);
        let _ = this.update(cx, |view, cx| done(view, value, cx));
    })
    .detach();
}
