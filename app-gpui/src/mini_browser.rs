//! Purpose-scoped native WebKit mini-browser. The ordinary preview stays offline;
//! connected account/help actions call `open_native` on the AppKit/GPUI main thread.
use std::sync::Mutex;
use url::Url;

// Applied to every web resource, not just navigation. Plain HTTP/WS is denied
// (including numeric/DNS aliases of the loopback backend); HTTPS/WSS literal
// private IPv4, IPv6 literals and local DNS names are also denied. Backend
// session authentication independently protects app data against filter gaps.
fn network_rules_json() -> String {
    // WebKit's content-blocker regex subset does not support alternation.
    let mut patterns = vec![
        "^http://".to_owned(),
        "^ws://".to_owned(),
        "^file:".to_owned(),
    ];
    let hosts = [
        r"0\.",
        r"10\.",
        r"127\.",
        r"169\.254\.",
        r"172\.1[6-9]\.",
        r"172\.2[0-9]\.",
        r"172\.3[01]\.",
        r"192\.168\.",
        r"100\.6[4-9]\.",
        r"100\.[7-9][0-9]\.",
        r"100\.1[01][0-9]\.",
        r"100\.12[0-7]\.",
        r"198\.1[89]\.",
        r"\[",
    ];
    let local_hosts = [
        r"localhost",
        r"localhost\.",
        r"[^/]*\.local",
        r"[^/]*\.localhost[.:]",
        r"[^/]*\.internal",
    ];
    for scheme in ["https", "wss"] {
        for host in hosts {
            // Network prefixes intentionally match partial IPv4 host strings.
            patterns.push(format!("^{scheme}://([^/]*@)?{host}"));
        }
        // WHATWG/WebKit accepts legacy numeric IPv4 spellings (single decimal
        // integers and 0x-prefixed hex). Match them before authority delimiters;
        // this is deliberately conservative for DNS names with numeric labels.
        for numeric in [r"[0-9]+", r"0[xX][0-9a-fA-F]+"] {
            let prefix = format!("^{scheme}://([^/]*@)?{numeric}");
            patterns.push(format!("{prefix}[.:/?#]"));
            patterns.push(format!("{prefix}$"));
        }
        // Local DNS hosts require an explicit authority boundary.
        // Delimiter and end-of-string cases are separate (no alternation).
        for host in local_hosts {
            let prefix = format!("^{scheme}://([^/]*@)?{host}");
            patterns.push(format!("{prefix}[:/?#]"));
            patterns.push(format!("{prefix}$"));
        }
    }
    serde_json::Value::Array(patterns.into_iter().map(|pattern|serde_json::json!({"trigger":{"url-filter":pattern},"action":{"type":"block"}})).collect()).to_string()
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum BrowserPurpose {
    GogAuth,
    EpicAuth,
    GameJolt,
    SteamApiKeyHelp,
    SteamStore,
    TheGamesDbHelp,
}
impl BrowserPurpose {
    fn allows_host(self, host: &str) -> bool {
        let exact = |choices: &[&str]| choices.contains(&host);
        let suffix = |domains: &[&str]| {
            domains
                .iter()
                .any(|d| host == *d || host.strip_suffix(d).is_some_and(|p| p.ends_with('.')))
        };
        match self {
            Self::GogAuth => exact(&["auth.gog.com", "gog.com", "www.gog.com", "embed.gog.com"]),
            Self::EpicAuth => {
                exact(&[
                    "legendary.gl",
                    "appleid.apple.com",
                    "www.facebook.com",
                    "facebook.com",
                    "login.live.com",
                    "steamcommunity.com",
                ]) || suffix(&[
                    "epicgames.com",
                    "google.com",
                    "playstation.com",
                    "sonyentertainmentnetwork.com",
                    "nintendo.net",
                ])
            }
            // Store pages plus the GameJolt download/CDN hosts that serve builds.
            Self::GameJolt => suffix(&["gamejolt.com", "gamejolt.net", "gjcdn.net"]),
            Self::SteamApiKeyHelp => exact(&["steamcommunity.com", "www.steamcommunity.com"]),
            Self::SteamStore => exact(&[
                "steampowered.com",
                "www.steampowered.com",
                "store.steampowered.com",
            ]),
            Self::TheGamesDbHelp => {
                exact(&["thegamesdb.net", "www.thegamesdb.net", "api.thegamesdb.net"])
            }
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum UrlPolicyError {
    InvalidUrl,
    HttpsRequired,
    CredentialsForbidden,
    PortForbidden,
    HostForbidden,
}
fn has_userinfo(raw: &str) -> bool {
    raw.split_once("://")
        .and_then(|(_, r)| r.split(['/', '?', '#']).next())
        .is_some_and(|a| a.contains('@'))
}
/// Allow only HTTPS on the default port, without userinfo, on the purpose allowlist.
pub fn validate_navigation(purpose: BrowserPurpose, raw: &str) -> Result<Url, UrlPolicyError> {
    let url = Url::parse(raw).map_err(|_| UrlPolicyError::InvalidUrl)?;
    if url.scheme() != "https" {
        return Err(UrlPolicyError::HttpsRequired);
    }
    if has_userinfo(raw) {
        return Err(UrlPolicyError::CredentialsForbidden);
    }
    if url.port().is_some_and(|p| p != 443) {
        return Err(UrlPolicyError::PortForbidden);
    }
    let host = url
        .host_str()
        .ok_or(UrlPolicyError::HostForbidden)?
        .to_ascii_lowercase();
    if !purpose.allows_host(&host) {
        return Err(UrlPolicyError::HostForbidden);
    }
    Ok(url)
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum GogCallback {
    Code(String),
    Error(String),
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CallbackError {
    InvalidUrl,
    NotExactCallback,
    UnexpectedFragment,
    MissingResult,
    AmbiguousResult,
    Duplicate,
    InvalidValue,
}
pub struct GogCallbackParser(Mutex<bool>);
impl Default for GogCallbackParser {
    fn default() -> Self {
        Self(Mutex::new(false))
    }
}
impl GogCallbackParser {
    pub fn parse_once(&self, raw: &str) -> Result<GogCallback, CallbackError> {
        let url = Url::parse(raw).map_err(|_| CallbackError::InvalidUrl)?;
        if url.scheme() != "https"
            || url.host_str() != Some("embed.gog.com")
            || url.port().is_some_and(|p| p != 443)
            || has_userinfo(raw)
            || url.path() != "/on_login_success"
        {
            return Err(CallbackError::NotExactCallback);
        }
        if url.fragment().is_some() {
            return Err(CallbackError::UnexpectedFragment);
        }
        let pairs: Vec<_> = url.query_pairs().collect();
        let codes: Vec<_> = pairs.iter().filter(|(k, _)| k == "code").collect();
        // Electron's existing GOG callback accepts `error` or `error_description`.
        // Keep that provider contract, but reject repeated/competing result fields.
        let errors: Vec<_> = pairs
            .iter()
            .filter(|(k, _)| k == "error" || k == "error_description")
            .collect();
        if codes.len() + errors.len() == 0 {
            return Err(CallbackError::MissingResult);
        }
        if codes.len() + errors.len() != 1 {
            return Err(CallbackError::AmbiguousResult);
        }
        let (is_code, value) = if codes.is_empty() {
            (false, errors[0].1.as_ref())
        } else {
            (true, codes[0].1.as_ref())
        };
        let value = value.trim();
        if value.is_empty() || value.len() > 4096 || value.chars().any(char::is_control) {
            return Err(CallbackError::InvalidValue);
        }
        let mut seen = self
            .0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if *seen {
            return Err(CallbackError::Duplicate);
        }
        *seen = true;
        Ok(if is_code {
            GogCallback::Code(value.to_owned())
        } else {
            GogCallback::Error(value.to_owned())
        })
    }
}
#[derive(Clone, Debug)]
pub struct MiniBrowserRequest {
    pub purpose: BrowserPurpose,
    pub initial_url: Url,
    pub title: String,
}
impl MiniBrowserRequest {
    pub fn new(
        purpose: BrowserPurpose,
        initial_url: &str,
        title: impl Into<String>,
    ) -> Result<Self, UrlPolicyError> {
        Ok(Self {
            purpose,
            initial_url: validate_navigation(purpose, initial_url)?,
            title: title.into(),
        })
    }
}

/// Authentication completion is delivered once on native window close/result.
/// Help/GameJolt callbacks are not invoked. Invoke from GPUI's AppKit main thread.
#[derive(Clone, Eq, PartialEq)]
pub enum MiniBrowserResult {
    GogCode(String),
    EpicCode(String),
    Error(MiniBrowserError),
    Cancelled,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MiniBrowserError {
    Gog(String),
    NavigationFailed,
    InvalidCallback,
    EpicDenied,
    PrivacyPolicyUnavailable,
}
impl std::fmt::Debug for MiniBrowserResult {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::GogCode(_) => "GogCode([redacted])",
            Self::EpicCode(_) => "EpicCode([redacted])",
            Self::Error(_) => "Error([redacted])",
            Self::Cancelled => "Cancelled",
        })
    }
}

/// Only Epic's documented JSON redirect endpoint may be inspected. Federation
/// hosts and arbitrary pages on approved domains must never yield login results.
fn epic_result_endpoint(raw: &str) -> bool {
    validate_navigation(BrowserPurpose::EpicAuth, raw).is_ok_and(|url| {
        matches!(url.host_str(), Some("www.epicgames.com" | "epicgames.com"))
            && url.path() == "/id/api/redirect"
            && url.fragment().is_none()
    })
}
#[derive(serde::Deserialize)]
struct EpicDocument {
    #[serde(rename = "authorizationCode")]
    authorization_code: Option<String>,
    code: Option<String>,
    error: Option<String>,
}
fn parse_epic_document(raw_url: &str, text: &str) -> Result<MiniBrowserResult, CallbackError> {
    if !epic_result_endpoint(raw_url) {
        return Err(CallbackError::NotExactCallback);
    }
    if text.len() > 16384 {
        return Err(CallbackError::InvalidValue);
    }
    let document: EpicDocument =
        serde_json::from_str(text).map_err(|_| CallbackError::InvalidValue)?;
    let count = usize::from(document.authorization_code.is_some())
        + usize::from(document.code.is_some())
        + usize::from(document.error.is_some());
    if count == 0 {
        return Err(CallbackError::MissingResult);
    }
    if count != 1 {
        return Err(CallbackError::AmbiguousResult);
    }
    let code = document.authorization_code.or(document.code);
    if let Some(code) = code {
        if code.is_empty()
            || code.len() > 4096
            || code.chars().any(|c| c.is_control() || c.is_whitespace())
        {
            return Err(CallbackError::InvalidValue);
        }
        Ok(MiniBrowserResult::EpicCode(code))
    } else {
        Ok(MiniBrowserResult::Error(MiniBrowserError::EpicDenied))
    }
}
pub type MiniBrowserCompletion = Box<dyn FnOnce(MiniBrowserResult) + 'static>;

/// GameJolt browser downloads (Electron `persist:gamejolt` `will-download`).
#[derive(Clone, Debug)]
pub enum GameJoltDownloadEvent {
    Started {
        id: u64,
        filename: String,
        path: std::path::PathBuf,
        total: Option<u64>,
    },
    Finished {
        id: u64,
        filename: String,
        path: std::path::PathBuf,
    },
    Failed {
        id: u64,
        filename: String,
        error: String,
    },
}

static GAMEJOLT_DOWNLOAD_DIR: Mutex<Option<std::path::PathBuf>> = Mutex::new(None);
static GAMEJOLT_DOWNLOAD_EVENTS: Mutex<Vec<GameJoltDownloadEvent>> = Mutex::new(Vec::new());

/// Directory that receives `.downloads/` for the GameJolt browser.
pub fn set_gamejolt_download_dir(dir: std::path::PathBuf) {
    *GAMEJOLT_DOWNLOAD_DIR.lock().unwrap() = Some(dir);
}

pub fn take_gamejolt_download_events() -> Vec<GameJoltDownloadEvent> {
    std::mem::take(&mut *GAMEJOLT_DOWNLOAD_EVENTS.lock().unwrap())
}

fn push_gamejolt_event(event: GameJoltDownloadEvent) {
    GAMEJOLT_DOWNLOAD_EVENTS.lock().unwrap().push(event);
}

/// main `uniqueDownloadPath`.
pub fn unique_download_path(directory: &std::path::Path, filename: &str) -> std::path::PathBuf {
    let original: String = std::path::Path::new(filename)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default()
        .chars()
        .map(|c| if "\\/:*?\"<>|".contains(c) { '_' } else { c })
        .collect();
    let original = if original.is_empty() {
        "download".to_owned()
    } else {
        original
    };
    let path = std::path::Path::new(&original);
    let extension = path
        .extension()
        .map(|e| format!(".{}", e.to_string_lossy()))
        .unwrap_or_default();
    let stem = original
        .strip_suffix(&extension)
        .unwrap_or(&original)
        .to_owned();
    let mut candidate = directory.join(&original);
    let mut suffix = 2;
    while candidate.exists() {
        candidate = directory.join(format!("{stem} ({suffix}){extension}"));
        suffix += 1;
    }
    candidate
}

fn expected_navigation_cancel(domain: &str, code: i64) -> bool {
    domain == "NSURLErrorDomain" && code == -999
}

#[cfg(target_os = "macos")]
mod native {
    use super::*;
    use block2::{DynBlock, RcBlock};
    use core::cell::{Cell, OnceCell, RefCell};
    use objc2::{
        AnyThread, ClassType, DefinedClass, MainThreadMarker, MainThreadOnly, define_class,
        rc::Retained,
        runtime::{AnyObject, ProtocolObject},
        sel,
    };
    use objc2_app_kit::{
        NSBackingStoreType, NSButton, NSColor, NSLayoutAttribute, NSLayoutConstraintOrientation,
        NSStackView, NSStackViewDistribution, NSTextField, NSUserInterfaceLayoutOrientation,
        NSWindow, NSWindowDelegate, NSWindowStyleMask,
    };
    use objc2_foundation::{
        NSHTTPURLResponse, NSObject, NSObjectProtocol, NSPoint, NSRect, NSSize, NSString, NSURL,
        NSURLRequest, NSUUID,
    };
    use objc2_web_kit::{
        WKContentRuleList, WKContentRuleListStore, WKDownload, WKDownloadDelegate, WKFrameInfo,
        WKMediaCaptureType, WKNavigation, WKNavigationAction, WKNavigationActionPolicy,
        WKNavigationDelegate, WKNavigationResponse, WKNavigationResponsePolicy,
        WKPermissionDecision, WKSecurityOrigin, WKUIDelegate, WKWebView, WKWebViewConfiguration,
        WKWebsiteDataStore,
    };
    thread_local! { static DELEGATES: RefCell<Vec<Retained<Delegate>>> = const { RefCell::new(Vec::new()) }; }
    // Downloads awaiting a destination: WebKit holds delegates weakly, so keep
    // ours (and the download) alive even if the window closes meanwhile.
    thread_local! { static ARMED: RefCell<Vec<(Retained<WKDownload>, Retained<Delegate>)>> = const { RefCell::new(Vec::new()) }; }
    // Active WKDownloads: (download, id, filename, destination).
    thread_local! { static DOWNLOADS: RefCell<Vec<(Retained<WKDownload>, u64, String, std::path::PathBuf, Retained<Delegate>)>> = const { RefCell::new(Vec::new()) }; }
    thread_local! { static NEXT_DOWNLOAD: Cell<u64> = const { Cell::new(1) }; }
    #[derive(Default)]
    struct Ivars {
        purpose: OnceCell<BrowserPurpose>,
        header: RefCell<Option<Retained<NSTextField>>>,
        window: RefCell<Option<Retained<NSWindow>>>,
        web_view: RefCell<Option<Retained<WKWebView>>>,
        completion: Mutex<Option<MiniBrowserCompletion>>,
        callback: GogCallbackParser,
        navigation_epoch: Cell<u64>,
        epic_inspection_pending: Cell<bool>,
        fixture_initial_url: Option<String>,
        fixture_started: Cell<bool>,
        history_buttons: RefCell<[Option<Retained<NSButton>>; 2]>,
    }
    define_class!(
        #[unsafe(super(NSObject))]
        #[thread_kind = MainThreadOnly]
        #[ivars = Ivars]
        struct Delegate;
        unsafe impl NSObjectProtocol for Delegate {}
        unsafe impl NSWindowDelegate for Delegate {
            #[unsafe(method(windowWillClose:))]
            #[allow(non_snake_case)]
            fn windowWillClose(&self, _notification: &objc2_foundation::NSNotification) {
                self.finish(MiniBrowserResult::Cancelled);
                self.ivars().navigation_epoch.set(self.ivars().navigation_epoch.get().wrapping_add(1));
                self.ivars().epic_inspection_pending.set(false);
                if let Some(web) = self.ivars().web_view.borrow_mut().take() {
                    unsafe {
                        web.stopLoading();
                        web.setNavigationDelegate(None);
                        web.setUIDelegate(None);
                    }
                }
                self.ivars().header.borrow_mut().take();
                self.ivars().window.borrow_mut().take();
                self.ivars().history_buttons.replace([None, None]);
                // The AppKit delegate property is weak. Hold the receiver across
                // removal of our registry's last owned reference.
                let _keep_alive = unsafe {
                    Retained::retain(self as *const Delegate as *mut Delegate)
                };
                DELEGATES.with(|items| {
                    items
                        .borrow_mut()
                        .retain(|item| !std::ptr::eq::<Delegate>(&**item, self))
                });
            }
        }
        unsafe impl WKNavigationDelegate for Delegate {
            #[unsafe(method(webView:decidePolicyForNavigationAction:decisionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_decidePolicyForNavigationAction_decisionHandler(
                &self,
                _web_view: &WKWebView,
                action: &WKNavigationAction,
                handler: &DynBlock<dyn Fn(WKNavigationActionPolicy)>,
            ) {
                let is_main_frame = unsafe { action.targetFrame() }
                    .is_some_and(|frame| unsafe { frame.isMainFrame() });
                let req = unsafe { action.request() };
                let raw = req.URL()
                    .and_then(|u| u.absoluteString())
                    .map(|s| s.to_string());
                if let Some(initial)=self.ivars().fixture_initial_url.as_deref() {
                    // Only the first native loadHTMLString action is admitted. No
                    // subsequent reload/link/back action may issue a network request.
                    let allow=is_main_frame && !self.ivars().fixture_started.get()
                        && raw.as_deref().is_some_and(|url|url==initial||url=="about:blank");
                    if allow {self.ivars().fixture_started.set(true);}
                    handler.call((if allow {WKNavigationActionPolicy::Allow}else{WKNavigationActionPolicy::Cancel},));
                    return;
                }
                let mut allow = false;
                if is_main_frame {
                    if let Some(raw) = raw {
                        if *self.ivars().purpose.get().unwrap() == BrowserPurpose::GogAuth {
                            match self.ivars().callback.parse_once(&raw) {
                                Ok(GogCallback::Code(code)) => {
                                    self.finish(MiniBrowserResult::GogCode(code));
                                    self.close();
                                }
                                Ok(GogCallback::Error(error)) => {
                                    self.finish(MiniBrowserResult::Error(MiniBrowserError::Gog(error)));
                                    self.close();
                                }
                                Err(error) if error != CallbackError::NotExactCallback => {
                                    self.finish(MiniBrowserResult::Error(MiniBrowserError::InvalidCallback));
                                    self.close();
                                }
                                _ => {
                                    allow = validate_navigation(
                                        *self.ivars().purpose.get().unwrap(),
                                        &raw,
                                    )
                                    .is_ok()
                                }
                            }
                        } else {
                            allow = validate_navigation(*self.ivars().purpose.get().unwrap(), &raw)
                                .is_ok();
                        }
                    }
                }
                handler.call((if allow {
                    WKNavigationActionPolicy::Allow
                } else {
                    WKNavigationActionPolicy::Cancel
                },));
            }
            #[unsafe(method(webView:decidePolicyForNavigationResponse:decisionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_decidePolicyForNavigationResponse_decisionHandler(
                &self,
                _web_view: &WKWebView,
                response: &WKNavigationResponse,
                handler: &DynBlock<dyn Fn(WKNavigationResponsePolicy)>,
            ) {
                let native_response = unsafe { response.response() };
                let response_url = native_response.URL().and_then(|u| u.absoluteString()).map(|s| s.to_string());
                let is_attachment = if native_response.isKindOfClass(NSHTTPURLResponse::class()) {
                    Retained::downcast::<NSHTTPURLResponse>(native_response).ok()
                        .and_then(|http| http.valueForHTTPHeaderField(&NSString::from_str("Content-Disposition")))
                        .is_some_and(|value| value.to_string().split(';').next().is_some_and(|token| token.trim().eq_ignore_ascii_case("attachment")))
                } else { false };
                let gamejolt = *self.ivars().purpose.get().unwrap() == BrowserPurpose::GameJolt
                    && self.ivars().fixture_initial_url.is_none();
                if gamejolt && (is_attachment || !unsafe { response.canShowMIMEType() }) {
                    handler.call((WKNavigationResponsePolicy::Download,));
                    return;
                }
                let allow = !is_attachment && unsafe { response.canShowMIMEType() }
                    && response_url.as_deref().is_some_and(|url| match self.ivars().fixture_initial_url.as_deref() {
                        Some(initial)=>url==initial || url=="about:blank",
                        None=>validate_navigation(*self.ivars().purpose.get().unwrap(), url).is_ok(),
                    });
                handler.call((if allow {
                    WKNavigationResponsePolicy::Allow
                } else {
                    WKNavigationResponsePolicy::Cancel
                },));
            }
            #[unsafe(method(webView:navigationResponse:didBecomeDownload:))]
            #[allow(non_snake_case)]
            unsafe fn webView_navigationResponse_didBecomeDownload(&self, _web_view: &WKWebView, _response: &WKNavigationResponse, download: &WKDownload) {
                unsafe { download.setDelegate(Some(ProtocolObject::from_ref(self))) };
                self.arm_download(download);
            }
            #[unsafe(method(webView:navigationAction:didBecomeDownload:))]
            #[allow(non_snake_case)]
            unsafe fn webView_navigationAction_didBecomeDownload(&self, _web_view: &WKWebView, _action: &WKNavigationAction, download: &WKDownload) {
                unsafe { download.setDelegate(Some(ProtocolObject::from_ref(self))) };
                self.arm_download(download);
            }
            #[unsafe(method(webView:didFailProvisionalNavigation:withError:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didFailProvisionalNavigation_withError(&self, _web_view: &WKWebView, _navigation: Option<&WKNavigation>, error: &objc2_foundation::NSError) { self.navigation_failed(error); }
            #[unsafe(method(webView:didFailNavigation:withError:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didFailNavigation_withError(&self, _web_view: &WKWebView, _navigation: Option<&WKNavigation>, error: &objc2_foundation::NSError) { self.navigation_failed(error); }
            #[unsafe(method(webView:didStartProvisionalNavigation:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didStartProvisionalNavigation(
                &self,
                web_view: &WKWebView,
                _navigation: Option<&WKNavigation>,
            ) {
                self.ivars().navigation_epoch.set(self.ivars().navigation_epoch.get().wrapping_add(1));
                self.update_url(web_view);
            }
            #[unsafe(method(webView:didReceiveServerRedirectForProvisionalNavigation:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didReceiveServerRedirectForProvisionalNavigation(
                &self,
                web_view: &WKWebView,
                _navigation: Option<&WKNavigation>,
            ) {
                let raw = unsafe { web_view.URL() }
                    .and_then(|u| u.absoluteString())
                    .map(|u| u.to_string());
                let mut allowed = raw.as_deref().is_some_and(|url| {
                    validate_navigation(*self.ivars().purpose.get().unwrap(), url).is_ok()
                });
                if let Some(url) = raw.as_deref() {
                    if *self.ivars().purpose.get().unwrap() == BrowserPurpose::GogAuth {
                        match self.ivars().callback.parse_once(url) {
                            Ok(GogCallback::Code(code)) => {
                                self.finish(MiniBrowserResult::GogCode(code));
                                self.close();
                                allowed = false;
                            }
                            Ok(GogCallback::Error(error)) => {
                                self.finish(MiniBrowserResult::Error(MiniBrowserError::Gog(error)));
                                self.close();
                                allowed = false;
                            }
                            Err(error) if error != CallbackError::NotExactCallback => {
                                self.finish(MiniBrowserResult::Error(MiniBrowserError::InvalidCallback));
                                self.close();
                                allowed = false;
                            }
                            _ => {}
                        }
                    }
                }
                if !allowed {
                    unsafe {
                        web_view.stopLoading();
                    }
                }
                self.update_url(web_view);
            }
            #[unsafe(method(webView:didFinishNavigation:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didFinishNavigation(
                &self,
                web_view: &WKWebView,
                _navigation: Option<&WKNavigation>,
            ) {
                self.update_url(web_view);
                self.inspect_epic_result(web_view);
                #[cfg(feature="browser-fixture")]
                if self.ivars().fixture_initial_url.is_some() {println!("OFFLINE_BROWSER_FIXTURE_READY");} else {println!("NATIVE_BROWSER_NAVIGATION_FINISHED");}
            }
        }
        impl Delegate {
            // Optional WKUIDelegate selector added by macOS 27 WebKit. It is harmless
            // on older systems and explicitly denies geolocation when WebKit supports it.
            #[unsafe(method(webView:requestGeolocationPermissionForOrigin:initiatedByFrame:decisionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn deny_geolocation(&self, _web_view: &WKWebView, _origin: &WKSecurityOrigin, _frame: &WKFrameInfo, handler: &DynBlock<dyn Fn(WKPermissionDecision)>) { handler.call((WKPermissionDecision::Deny,)); }
        }
        unsafe impl WKDownloadDelegate for Delegate {
            #[unsafe(method(download:decideDestinationUsingResponse:suggestedFilename:completionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn download_decideDestinationUsingResponse_suggestedFilename_completionHandler(
                &self,
                download: &WKDownload,
                response: &objc2_foundation::NSURLResponse,
                suggested_filename: &NSString,
                completion_handler: &DynBlock<dyn Fn(*mut NSURL)>,
            ) {
                disarm_download(download);
                let dir = GAMEJOLT_DOWNLOAD_DIR.lock().unwrap().clone();
                let Some(dir) = dir.filter(|_| *self.ivars().purpose.get().unwrap() == BrowserPurpose::GameJolt) else {
                    completion_handler.call((core::ptr::null_mut(),));
                    return;
                };
                let incoming = dir.join(".downloads");
                let _ = std::fs::create_dir_all(&incoming);
                let filename = suggested_filename.to_string();
                let path = unique_download_path(&incoming, &filename);
                let id = NEXT_DOWNLOAD.with(|n| { let id = n.get(); n.set(id + 1); id });
                let expected = response.expectedContentLength();
                let total = (expected > 0).then_some(expected as u64);
                let retained = unsafe { Retained::retain(download as *const WKDownload as *mut WKDownload) };
                // WebKit holds download delegates weakly; keep ours alive until the transfer ends.
                let keep = unsafe { Retained::retain(self as *const Delegate as *mut Delegate) };
                if let (Some(retained), Some(keep)) = (retained, keep) {
                    DOWNLOADS.with(|d| d.borrow_mut().push((retained, id, filename.clone(), path.clone(), keep)));
                }
                push_gamejolt_event(GameJoltDownloadEvent::Started { id, filename, path: path.clone(), total });
                let url = NSURL::fileURLWithPath(&NSString::from_str(&path.to_string_lossy()));
                completion_handler.call((Retained::as_ptr(&url) as *mut NSURL,));
            }
            #[unsafe(method(downloadDidFinish:))]
            #[allow(non_snake_case)]
            unsafe fn downloadDidFinish(&self, download: &WKDownload) {
                if let Some((_, id, filename, path, _)) = take_download(download) {
                    push_gamejolt_event(GameJoltDownloadEvent::Finished { id, filename, path });
                }
            }
            #[unsafe(method(download:didFailWithError:resumeData:))]
            #[allow(non_snake_case)]
            unsafe fn download_didFailWithError_resumeData(&self, download: &WKDownload, error: &objc2_foundation::NSError, _resume: Option<&objc2_foundation::NSData>) {
                disarm_download(download);
                if let Some((_, id, filename, _, _)) = take_download(download) {
                    push_gamejolt_event(GameJoltDownloadEvent::Failed { id, filename, error: error.localizedDescription().to_string() });
                }
            }
        }
        unsafe impl WKUIDelegate for Delegate {
            #[unsafe(method(webView:runJavaScriptAlertPanelWithMessage:initiatedByFrame:completionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_runJavaScriptAlertPanelWithMessage_initiatedByFrame_completionHandler(
                &self,
                _w: &WKWebView,
                _m: &objc2_foundation::NSString,
                _f: &WKFrameInfo,
                h: &DynBlock<dyn Fn()>,
            ) {
                h.call(())
            }
            #[unsafe(method(webView:runJavaScriptConfirmPanelWithMessage:initiatedByFrame:completionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_runJavaScriptConfirmPanelWithMessage_initiatedByFrame_completionHandler(
                &self,
                _w: &WKWebView,
                _m: &objc2_foundation::NSString,
                _f: &WKFrameInfo,
                h: &DynBlock<dyn Fn(objc2::runtime::Bool)>,
            ) {
                h.call((objc2::runtime::Bool::NO,))
            }
            #[unsafe(method(webView:runJavaScriptTextInputPanelWithPrompt:defaultText:initiatedByFrame:completionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_runJavaScriptTextInputPanelWithPrompt_defaultText_initiatedByFrame_completionHandler(
                &self,
                _w: &WKWebView,
                _p: &objc2_foundation::NSString,
                _d: Option<&objc2_foundation::NSString>,
                _f: &WKFrameInfo,
                h: &DynBlock<dyn Fn(*mut objc2::runtime::AnyObject)>,
            ) {
                h.call((core::ptr::null_mut(),))
            }
            #[unsafe(method(webView:requestMediaCapturePermissionForOrigin:initiatedByFrame:type:decisionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn webView_requestMediaCapturePermissionForOrigin_initiatedByFrame_type_decisionHandler(
                &self,
                _w: &WKWebView,
                _o: &WKSecurityOrigin,
                _f: &WKFrameInfo,
                _t: WKMediaCaptureType,
                h: &DynBlock<dyn Fn(WKPermissionDecision)>,
            ) {
                h.call((WKPermissionDecision::Deny,))
            }
        }
    );
    fn disarm_download(download: &WKDownload) {
        ARMED.with(|armed| {
            armed
                .borrow_mut()
                .retain(|(item, _)| !std::ptr::eq::<WKDownload>(&**item, download))
        });
    }
    #[allow(clippy::type_complexity)]
    fn take_download(
        download: &WKDownload,
    ) -> Option<(
        Retained<WKDownload>,
        u64,
        String,
        std::path::PathBuf,
        Retained<Delegate>,
    )> {
        DOWNLOADS.with(|d| {
            let mut list = d.borrow_mut();
            let index = list
                .iter()
                .position(|(item, ..)| std::ptr::eq::<WKDownload>(&**item, download))?;
            Some(list.remove(index))
        })
    }
    impl Delegate {
        fn arm_download(&self, download: &WKDownload) {
            let download =
                unsafe { Retained::retain(download as *const WKDownload as *mut WKDownload) };
            let keep = unsafe { Retained::retain(self as *const Delegate as *mut Delegate) };
            if let (Some(download), Some(keep)) = (download, keep) {
                ARMED.with(|armed| armed.borrow_mut().push((download, keep)));
            }
        }
        fn new(
            mtm: MainThreadMarker,
            request: &MiniBrowserRequest,
            completion: MiniBrowserCompletion,
            fixture: bool,
        ) -> Retained<Self> {
            let this = Self::alloc(mtm).set_ivars(Ivars {
                purpose: OnceCell::from(request.purpose),
                header: RefCell::new(None),
                window: RefCell::new(None),
                web_view: RefCell::new(None),
                completion: Mutex::new(
                    if matches!(
                        request.purpose,
                        BrowserPurpose::GameJolt
                            | BrowserPurpose::SteamApiKeyHelp
                            | BrowserPurpose::SteamStore
                            | BrowserPurpose::TheGamesDbHelp
                    ) {
                        None
                    } else {
                        Some(completion)
                    },
                ),
                callback: GogCallbackParser::default(),
                navigation_epoch: Cell::new(0),
                epic_inspection_pending: Cell::new(false),
                fixture_initial_url: fixture.then(|| request.initial_url.to_string()),
                fixture_started: Cell::new(false),
                history_buttons: RefCell::default(),
            });
            unsafe { objc2::msg_send![super(this), init] }
        }
        fn finish(&self, result: MiniBrowserResult) {
            if matches!(
                self.ivars().purpose.get(),
                Some(
                    BrowserPurpose::GameJolt
                        | BrowserPurpose::SteamApiKeyHelp
                        | BrowserPurpose::SteamStore
                        | BrowserPurpose::TheGamesDbHelp
                )
            ) {
                return;
            }
            let callback = self
                .ivars()
                .completion
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .take();
            if let Some(cb) = callback {
                cb(result);
            }
        }
        fn navigation_failed(&self, error: &objc2_foundation::NSError) {
            // Policy denials and user back/reload cancellation are not auth failures.
            if expected_navigation_cancel(&error.domain().to_string(), error.code() as i64) {
                return;
            }
            // GameJolt is a browsing window: a response turned into a download
            // ends its navigation with WebKitErrorDomain 102, and other load
            // failures must not close the store either (Electron's webview).
            if *self.ivars().purpose.get().unwrap() == BrowserPurpose::GameJolt {
                return;
            }
            #[cfg(feature = "browser-fixture")]
            println!("NATIVE_BROWSER_NAVIGATION_FAILED ({})", error.code());
            self.finish(MiniBrowserResult::Error(MiniBrowserError::NavigationFailed));
            self.close();
        }
        fn update_url(&self, web_view: &WKWebView) {
            let history = self.ivars().history_buttons.borrow();
            if let Some(back) = &history[0] {
                back.setEnabled(unsafe { web_view.canGoBack() });
            }
            if let Some(forward) = &history[1] {
                forward.setEnabled(unsafe { web_view.canGoForward() });
            }
            if self.ivars().fixture_initial_url.is_some() {
                if let Some(header) = self.ivars().header.borrow().as_ref() {
                    header.setStringValue(&NSString::from_str(
                        "OFFLINE browser fixture — no remote page",
                    ));
                }
                return;
            }
            if let Some(url) = unsafe { web_view.URL() }.and_then(|u| u.absoluteString()) {
                if let Some(header) = self.ivars().header.borrow().as_ref() {
                    // OAuth query/fragment values can contain codes, tokens and
                    // state. Show the origin/path, not transient account secrets.
                    let display = if matches!(
                        self.ivars().purpose.get(),
                        Some(BrowserPurpose::GogAuth | BrowserPurpose::EpicAuth)
                    ) {
                        Url::parse(&url.to_string())
                            .map(|mut url| {
                                url.set_query(None);
                                url.set_fragment(None);
                                url.to_string()
                            })
                            .unwrap_or_default()
                    } else {
                        url.to_string()
                    };
                    header.setStringValue(&NSString::from_str(&display));
                }
            }
        }
        fn inspect_epic_result(&self, web_view: &WKWebView) {
            if self.ivars().purpose.get() != Some(&BrowserPurpose::EpicAuth)
                || self.ivars().epic_inspection_pending.get()
                || self.ivars().window.borrow().is_none()
            {
                return;
            }
            let Some(before) = unsafe { web_view.URL() }
                .and_then(|url| url.absoluteString())
                .map(|url| url.to_string())
            else {
                return;
            };
            if !epic_result_endpoint(&before) {
                return;
            }
            let Some(delegate) = DELEGATES.with(|items| {
                items
                    .borrow()
                    .iter()
                    .find(|item| std::ptr::eq(&***item, self))
                    .cloned()
            }) else {
                return;
            };
            // SAFETY: WebKit supplied a live main-thread object. The retained
            // reference keeps it alive until its copied completion block finishes.
            let Some(web) =
                (unsafe { Retained::retain(web_view as *const WKWebView as *mut WKWebView) })
            else {
                return;
            };
            let epoch = self.ivars().navigation_epoch.get();
            self.ivars().epic_inspection_pending.set(true);
            let completion = RcBlock::new(
                move |object: *mut AnyObject, error: *mut objc2_foundation::NSError| {
                    delegate.ivars().epic_inspection_pending.set(false);
                    if !error.is_null()
                        || delegate.ivars().window.borrow().is_none()
                        || delegate.ivars().navigation_epoch.get() != epoch
                    {
                        return;
                    }
                    let current = unsafe { web.URL() }
                        .and_then(|url| url.absoluteString())
                        .map(|url| url.to_string());
                    if current.as_deref() != Some(before.as_str()) {
                        return;
                    }
                    // SAFETY: The result pointer is valid only for this callback.
                    let Some(result) = (unsafe { object.as_ref() })
                        .and_then(|value| value.downcast_ref::<NSString>())
                    else {
                        return;
                    };
                    #[derive(serde::Deserialize)]
                    struct Envelope {
                        href: String,
                        text: Option<String>,
                    }
                    let raw = result.to_string();
                    if raw.len() > 32768 {
                        return;
                    }
                    let Ok(envelope) = serde_json::from_str::<Envelope>(&raw) else {
                        return;
                    };
                    if envelope.href != before {
                        return;
                    }
                    let Some(text) = envelope.text else {
                        return;
                    };
                    match parse_epic_document(&before, &text) {
                        Ok(result) => {
                            delegate.finish(result);
                            delegate.close();
                        }
                        Err(_) => {
                            delegate.finish(MiniBrowserResult::Error(
                                MiniBrowserError::InvalidCallback,
                            ));
                            delegate.close();
                        }
                    }
                },
            );
            // User-approved, fixed read-only extractor. No network request, DOM
            // mutation, user-supplied script, injected object or app/Node bridge.
            let script = NSString::from_str(
                "JSON.stringify({href:location.href,text:document.body && document.body.innerText.length <= 16384 ? document.body.innerText : null})",
            );
            unsafe {
                web_view.evaluateJavaScript_completionHandler(&script, Some(&completion));
            }
        }
        fn close(&self) {
            // Closing synchronously invokes windowWillClose, which mutates this
            // RefCell. Release the borrow before calling AppKit.
            let window = self.ivars().window.borrow().clone();
            if let Some(w) = window {
                w.close()
            }
        }
    }
    pub fn open(
        mtm: MainThreadMarker,
        request: MiniBrowserRequest,
        completion: MiniBrowserCompletion,
    ) -> anyhow::Result<()> {
        open_inner(mtm, request, completion, None)
    }

    /// Remove only MetalSharp's explicitly named GameJolt WebKit store. Call
    /// from the app's explicit GameJolt data-removal flow after closing its
    /// browser windows; the default WebKit store is never queried or modified.
    pub fn remove_gamejolt_store(
        mtm: MainThreadMarker,
        completion: Box<dyn FnOnce(Result<(), String>) + 'static>,
    ) -> anyhow::Result<()> {
        if !WKWebsiteDataStore::class()
            .metaclass()
            .responds_to(sel!(removeDataStoreForIdentifier:completionHandler:))
        {
            anyhow::bail!("named GameJolt store cleanup requires macOS 14 or newer");
        }
        close_embedded();
        let active = DELEGATES.with(|items| {
            items.borrow().iter().any(|delegate| {
                delegate.ivars().purpose.get() == Some(&BrowserPurpose::GameJolt)
                    && delegate.ivars().window.borrow().is_some()
            })
        });
        if active {
            anyhow::bail!("close GameJolt browser windows before removing their store");
        }
        let identifier = gamejolt_store_identifier()?;
        let completion = Mutex::new(Some(completion));
        let callback = RcBlock::new(move |error: *mut objc2_foundation::NSError| {
            let result = if error.is_null() {
                Ok(())
            } else {
                let message = unsafe { error.as_ref() }
                    .map(|error| error.localizedDescription().to_string())
                    .unwrap_or_else(|| "WebKit store removal failed".to_owned());
                Err(message)
            };
            if let Some(completion) = completion
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .take()
            {
                completion(result);
            }
        });
        unsafe {
            WKWebsiteDataStore::removeDataStoreForIdentifier_completionHandler(
                &identifier,
                &callback,
                mtm,
            );
        }
        Ok(())
    }

    fn gamejolt_store_identifier() -> anyhow::Result<Retained<NSUUID>> {
        NSUUID::initWithUUIDString(
            NSUUID::alloc(),
            &objc2_foundation::NSString::from_str("5C6F493A-2D9E-4A25-BEB0-7DC86791553A"),
        )
        .ok_or_else(|| anyhow::anyhow!("GameJolt persistent store identifier is invalid"))
    }
    // Electron's in-page `<webview partition="persist:gamejolt">`: one WKWebView
    // hosted in the main window above the GPUI view, sharing the GameJolt store
    // and download delegate with the standalone browser.
    struct Embedded {
        web: Retained<WKWebView>,
        delegate: Retained<Delegate>,
        gpui_view: Retained<objc2_app_kit::NSView>,
        monitor: Option<Retained<AnyObject>>,
    }
    thread_local! { static EMBEDDED: RefCell<Option<Embedded>> = const { RefCell::new(None) }; }
    thread_local! { static EMBEDDED_RECT: Cell<Option<[f64; 4]>> = const { Cell::new(None) }; }

    fn responder_inside(web: &WKWebView) -> bool {
        let Some(window) = web.window() else {
            return false;
        };
        let Some(responder) = window.firstResponder() else {
            return false;
        };
        if !responder.isKindOfClass(objc2_app_kit::NSView::class()) {
            return false;
        }
        let view = unsafe { &*(Retained::as_ptr(&responder) as *const objc2_app_kit::NSView) };
        view.isDescendantOf(web)
    }

    fn restore_gpui_responder(embed: &Embedded) {
        if responder_inside(&embed.web) {
            if let Some(window) = embed.gpui_view.window() {
                window.makeFirstResponder(Some(&embed.gpui_view));
            }
        }
    }

    fn create_embedded(
        mtm: MainThreadMarker,
        gpui_view: Retained<objc2_app_kit::NSView>,
    ) -> anyhow::Result<()> {
        if !WKWebsiteDataStore::class()
            .metaclass()
            .responds_to(sel!(dataStoreForIdentifier:))
        {
            anyhow::bail!("persistent GameJolt browser sessions require macOS 14 or newer");
        }
        let privacy_store = unsafe { WKContentRuleListStore::defaultStore(mtm) }
            .ok_or_else(|| anyhow::anyhow!("WebKit privacy rule store unavailable"))?;
        let request = MiniBrowserRequest::new(
            BrowserPurpose::GameJolt,
            "https://gamejolt.com/games",
            "GameJolt",
        )
        .map_err(|_| anyhow::anyhow!("GameJolt URL rejected"))?;
        let config = unsafe { WKWebViewConfiguration::new(mtm) };
        let identifier = gamejolt_store_identifier()?;
        let store = unsafe { WKWebsiteDataStore::dataStoreForIdentifier(&identifier, mtm) };
        if !unsafe { store.isPersistent() } {
            anyhow::bail!("WebKit did not create the isolated persistent GameJolt data store");
        }
        unsafe { config.setWebsiteDataStore(&store) };
        let web = unsafe {
            WKWebView::initWithFrame_configuration(WKWebView::alloc(mtm), NSRect::ZERO, &config)
        };
        let delegate = Delegate::new(mtm, &request, Box::new(|_| {}), false);
        *delegate.ivars().web_view.borrow_mut() = Some(web.clone());
        unsafe {
            web.setNavigationDelegate(Some(ProtocolObject::from_ref(&*delegate)));
            web.setUIDelegate(Some(ProtocolObject::from_ref(&*delegate)));
        }
        web.setHidden(true);
        web.setWantsLayer(true);
        unsafe {
            let layer: *mut AnyObject = objc2::msg_send![&*web, layer];
            if !layer.is_null() {
                let _: () = objc2::msg_send![layer, setCornerRadius: 5.0f64];
                let _: () = objc2::msg_send![layer, setMasksToBounds: objc2::runtime::Bool::YES];
            }
        }
        // GPUI's view never becomes first responder again by itself; hand
        // keyboard focus back when the user clicks outside the browser.
        let monitor_web = web.clone();
        let monitor_view = gpui_view.clone();
        let handler = RcBlock::new(move |event: *mut AnyObject| -> *mut AnyObject {
            if event.is_null() || monitor_web.isHidden() {
                return event;
            }
            let event_window: *mut AnyObject = unsafe { objc2::msg_send![event, window] };
            let same_window = monitor_web.window().is_some_and(|w| {
                std::ptr::eq(Retained::as_ptr(&w) as *const AnyObject, event_window)
            });
            if same_window {
                let location: NSPoint = unsafe { objc2::msg_send![event, locationInWindow] };
                let point = monitor_web.convertPoint_fromView(location, None);
                let bounds = monitor_web.bounds();
                let inside = point.x >= bounds.origin.x
                    && point.y >= bounds.origin.y
                    && point.x < bounds.origin.x + bounds.size.width
                    && point.y < bounds.origin.y + bounds.size.height;
                if !inside && responder_inside(&monitor_web) {
                    if let Some(window) = monitor_view.window() {
                        window.makeFirstResponder(Some(&monitor_view));
                    }
                }
            }
            event
        });
        // NSEventMaskLeftMouseDown | NSEventMaskRightMouseDown
        let mask: u64 = (1 << 1) | (1 << 3);
        let monitor: Option<Retained<AnyObject>> = unsafe {
            objc2::msg_send![
                objc2::class!(NSEvent),
                addLocalMonitorForEventsMatchingMask: mask,
                handler: &*handler
            ]
        };
        EMBEDDED.with(|slot| {
            *slot.borrow_mut() = Some(Embedded {
                web: web.clone(),
                delegate: delegate.clone(),
                gpui_view,
                monitor,
            })
        });
        let url = NSURL::URLWithString(&NSString::from_str(request.initial_url.as_str()))
            .ok_or_else(|| anyhow::anyhow!("initial URL could not be represented by NSURL"))?;
        let controller = unsafe { config.userContentController() };
        let pending = web.clone();
        let completion = RcBlock::new(
            move |rule: *mut WKContentRuleList, error: *mut objc2_foundation::NSError| {
                // A browser torn down while compilation was pending must never load.
                let current = EMBEDDED.with(|slot| {
                    slot.borrow()
                        .as_ref()
                        .is_some_and(|embed| std::ptr::eq::<WKWebView>(&*embed.web, &*pending))
                });
                let rule = unsafe { rule.as_ref() };
                if !current || !error.is_null() || rule.is_none() {
                    return;
                }
                unsafe {
                    controller.addContentRuleList(rule.unwrap());
                    pending.loadRequest(&NSURLRequest::requestWithURL(&url));
                }
            },
        );
        // No remote document is loaded until compilation and installation succeed.
        unsafe {
            privacy_store
                .compileContentRuleListForIdentifier_encodedContentRuleList_completionHandler(
                    Some(&NSString::from_str("MetalSharp-NoLocalNetworking-v1")),
                    Some(&NSString::from_str(&network_rules_json())),
                    Some(&completion),
                );
        }
        Ok(())
    }

    /// Show the embedded browser at `rect` (GPUI logical, top-left origin).
    pub fn show_embedded(
        mtm: MainThreadMarker,
        ns_view: *mut core::ffi::c_void,
        rect: [f64; 4],
    ) -> anyhow::Result<()> {
        let exists = EMBEDDED.with(|slot| slot.borrow().is_some());
        if !exists {
            let gpui_view = unsafe { Retained::retain(ns_view.cast::<objc2_app_kit::NSView>()) }
                .ok_or_else(|| anyhow::anyhow!("GPUI view unavailable"))?;
            create_embedded(mtm, gpui_view)?;
        }
        EMBEDDED.with(|slot| {
            let slot = slot.borrow();
            let Some(embed) = slot.as_ref() else { return };
            let Some(parent) = (unsafe { embed.gpui_view.superview() }) else {
                return;
            };
            if !unsafe { embed.web.superview() }
                .is_some_and(|view| std::ptr::eq::<objc2_app_kit::NSView>(&*view, &*parent))
            {
                parent.addSubview_positioned_relativeTo(
                    &embed.web,
                    objc2_app_kit::NSWindowOrderingMode::Above,
                    Some(&embed.gpui_view),
                );
            }
            let [x, y, w, h] = rect;
            let bounds = embed.gpui_view.bounds();
            let local_y = if embed.gpui_view.isFlipped() {
                y
            } else {
                bounds.size.height - y - h
            };
            let local = NSRect::new(NSPoint::new(x, local_y), NSSize::new(w, h));
            let frame = embed.gpui_view.convertRect_toView(local, Some(&parent));
            let current = embed.web.frame();
            if current.origin.x != frame.origin.x
                || current.origin.y != frame.origin.y
                || current.size.width != frame.size.width
                || current.size.height != frame.size.height
            {
                embed.web.setFrame(frame);
            }
            if embed.web.isHidden() {
                embed.web.setHidden(false);
            }
        });
        EMBEDDED_RECT.with(|r| r.set(Some(rect)));
        Ok(())
    }

    pub fn hide_embedded() {
        EMBEDDED.with(|slot| {
            if let Some(embed) = slot.borrow().as_ref() {
                if !embed.web.isHidden() {
                    restore_gpui_responder(embed);
                    embed.web.setHidden(true);
                }
            }
        });
        EMBEDDED_RECT.with(|r| r.set(None));
    }

    pub fn embedded_rect() -> Option<[f64; 4]> {
        EMBEDDED_RECT.with(|r| r.get())
    }

    /// Tear the embedded browser down (GameJolt data removal).
    pub fn close_embedded() {
        let embed = EMBEDDED.with(|slot| slot.borrow_mut().take());
        EMBEDDED_RECT.with(|r| r.set(None));
        let Some(embed) = embed else { return };
        restore_gpui_responder(&embed);
        unsafe {
            embed.web.stopLoading();
            embed.web.setNavigationDelegate(None);
            embed.web.setUIDelegate(None);
        }
        embed.web.removeFromSuperview();
        embed.delegate.ivars().web_view.borrow_mut().take();
        if let Some(monitor) = embed.monitor {
            let _: () =
                unsafe { objc2::msg_send![objc2::class!(NSEvent), removeMonitor: &*monitor] };
        }
    }

    fn open_inner(
        mtm: MainThreadMarker,
        request: MiniBrowserRequest,
        completion: MiniBrowserCompletion,
        fixture: Option<String>,
    ) -> anyhow::Result<()> {
        validate_navigation(request.purpose, request.initial_url.as_str())
            .map_err(|_| anyhow::anyhow!("initial browser URL rejected"))?;
        let frame = NSRect::new(NSPoint::new(0., 0.), NSSize::new(900., 650.));
        let style =
            NSWindowStyleMask::Titled | NSWindowStyleMask::Closable | NSWindowStyleMask::Resizable;
        // WKWebsiteDataStore's named persistent stores are available from macOS 14.
        // The preview app still supports macOS 13, so fail closed instead of sending
        // an unavailable Objective-C selector on older systems.
        if request.purpose == BrowserPurpose::GameJolt
            && !WKWebsiteDataStore::class()
                .metaclass()
                .responds_to(sel!(dataStoreForIdentifier:))
        {
            anyhow::bail!("persistent GameJolt browser sessions require macOS 14 or newer");
        }
        let privacy_store = unsafe { WKContentRuleListStore::defaultStore(mtm) }
            .ok_or_else(|| anyhow::anyhow!("WebKit privacy rule store unavailable"))?;
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(mtm),
                frame,
                style,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        unsafe {
            window.setReleasedWhenClosed(false);
        }
        let config = unsafe { WKWebViewConfiguration::new(mtm) };
        let store = if request.purpose == BrowserPurpose::GameJolt {
            let identifier = gamejolt_store_identifier()?;
            unsafe { WKWebsiteDataStore::dataStoreForIdentifier(&identifier, mtm) }
        } else {
            unsafe { WKWebsiteDataStore::nonPersistentDataStore(mtm) }
        };
        if request.purpose == BrowserPurpose::GameJolt && !unsafe { store.isPersistent() } {
            anyhow::bail!("WebKit did not create the isolated persistent GameJolt data store");
        }
        unsafe {
            config.setWebsiteDataStore(&store);
        }
        let web = unsafe {
            WKWebView::initWithFrame_configuration(WKWebView::alloc(mtm), NSRect::ZERO, &config)
        };
        let delegate = Delegate::new(mtm, &request, completion, fixture.is_some());
        let header = NSTextField::initWithFrame(NSTextField::alloc(mtm), NSRect::ZERO);
        header.setEditable(false);
        header.setSelectable(true);
        header.setDrawsBackground(true);
        header.setBezeled(false);
        header.setBordered(false);
        header.setBackgroundColor(Some(&NSColor::textBackgroundColor()));
        header.setTextColor(Some(&NSColor::textColor()));
        header.setMaximumNumberOfLines(1);
        header.setContentHuggingPriority_forOrientation(
            1.0,
            NSLayoutConstraintOrientation::Horizontal,
        );
        header
            .heightAnchor()
            .constraintEqualToConstant(32.0)
            .setActive(true);
        header.setContentCompressionResistancePriority_forOrientation(
            250.0,
            NSLayoutConstraintOrientation::Horizontal,
        );
        *delegate.ivars().header.borrow_mut() = Some(header.clone());
        *delegate.ivars().window.borrow_mut() = Some(window.clone());
        *delegate.ivars().web_view.borrow_mut() = Some(web.clone());
        let navigation_delegate: &ProtocolObject<dyn WKNavigationDelegate> =
            ProtocolObject::from_ref(&*delegate);
        let ui_delegate: &ProtocolObject<dyn WKUIDelegate> = ProtocolObject::from_ref(&*delegate);
        let window_delegate: &ProtocolObject<dyn NSWindowDelegate> =
            ProtocolObject::from_ref(&*delegate);
        unsafe {
            web.setNavigationDelegate(Some(navigation_delegate));
            web.setUIDelegate(Some(ui_delegate));
            window.setDelegate(Some(window_delegate));
        }
        let bar = NSStackView::initWithFrame(NSStackView::alloc(mtm), NSRect::ZERO);
        bar.setOrientation(NSUserInterfaceLayoutOrientation::Horizontal);
        bar.setAlignment(NSLayoutAttribute::CenterY);
        bar.setDistribution(NSStackViewDistribution::Fill);
        bar.setSpacing(5.);
        let buttons = [
            ("←", sel!(goBack)),
            ("→", sel!(goForward)),
            ("↻", sel!(reload)),
            ("×", sel!(close)),
        ];
        for (title, action) in buttons {
            let target: &AnyObject = if action == sel!(close) {
                &*window
            } else {
                &*web
            };
            let b = unsafe {
                NSButton::buttonWithTitle_target_action(
                    &objc2_foundation::NSString::from_str(title),
                    Some(target),
                    Some(action),
                    mtm,
                )
            };
            if action == sel!(goBack) {
                *delegate
                    .ivars()
                    .history_buttons
                    .borrow_mut()
                    .get_mut(0)
                    .unwrap() = Some(b.clone());
                b.setEnabled(false);
            }
            if action == sel!(goForward) {
                *delegate
                    .ivars()
                    .history_buttons
                    .borrow_mut()
                    .get_mut(1)
                    .unwrap() = Some(b.clone());
                b.setEnabled(false);
            }
            b.widthAnchor()
                .constraintEqualToConstant(36.0)
                .setActive(true);
            b.heightAnchor()
                .constraintEqualToConstant(28.0)
                .setActive(true);
            bar.addArrangedSubview(&b);
        }
        bar.addArrangedSubview(&header);
        let content = NSStackView::initWithFrame(NSStackView::alloc(mtm), frame);
        content.setOrientation(NSUserInterfaceLayoutOrientation::Vertical);
        content.setAlignment(NSLayoutAttribute::CenterX);
        content.setDistribution(NSStackViewDistribution::Fill);
        content.setSpacing(0.);
        content.addArrangedSubview(&bar);
        content.addArrangedSubview(&web);
        window.setContentView(Some(&content));
        // WebKit has no intrinsic content size. Explicit chrome height and full
        // widths prevent an ambiguous stack from collapsing the web surface.
        bar.heightAnchor()
            .constraintEqualToConstant(48.0)
            .setActive(true);
        bar.widthAnchor()
            .constraintEqualToAnchor(&content.widthAnchor())
            .setActive(true);
        web.widthAnchor()
            .constraintEqualToAnchor(&content.widthAnchor())
            .setActive(true);
        web.heightAnchor()
            .constraintGreaterThanOrEqualToConstant(200.0)
            .setActive(true);
        // WebKit's delegate property is weak; retain the per-window delegate.
        DELEGATES.with(|items| items.borrow_mut().push(delegate.clone()));
        window.setTitle(&objc2_foundation::NSString::from_str(&request.title));
        window.setMinSize(NSSize::new(720., 540.));
        window.center();
        window.makeKeyAndOrderFront(None);
        let req_url = NSURL::URLWithString(&objc2_foundation::NSString::from_str(
            request.initial_url.as_str(),
        ))
        .ok_or_else(|| anyhow::anyhow!("initial URL could not be represented by NSURL"))?;
        header.setStringValue(&NSString::from_str("Applying browser privacy policy…"));
        let controller = unsafe { config.userContentController() };
        let completion = block2::RcBlock::new(
            move |rule: *mut WKContentRuleList, error: *mut objc2_foundation::NSError| {
                // A window closed while compilation was pending must never load.
                if delegate.ivars().window.borrow().is_none() {
                    return;
                }
                let rule = unsafe { rule.as_ref() };
                if !error.is_null() || rule.is_none() {
                    header.setStringValue(&NSString::from_str(
                        "Privacy policy unavailable — navigation disabled",
                    ));
                    delegate.finish(MiniBrowserResult::Error(
                        MiniBrowserError::PrivacyPolicyUnavailable,
                    ));
                    return;
                }
                unsafe {
                    controller.addContentRuleList(rule.unwrap());
                    #[cfg(feature = "browser-fixture")]
                    println!("NATIVE_BROWSER_PRIVACY_POLICY_READY");
                    if let Some(html) = fixture.as_deref() {
                        web.loadHTMLString_baseURL(&NSString::from_str(html), Some(&req_url));
                    } else {
                        web.loadRequest(&NSURLRequest::requestWithURL(&req_url));
                    }
                }
            },
        );
        // No remote document is loaded until compilation and installation succeed.
        unsafe {
            privacy_store
                .compileContentRuleListForIdentifier_encodedContentRuleList_completionHandler(
                    Some(&NSString::from_str("MetalSharp-NoLocalNetworking-v1")),
                    Some(&NSString::from_str(&network_rules_json())),
                    Some(&completion),
                );
        }
        Ok(())
    }
    #[cfg(feature = "browser-fixture")]
    pub fn open_network_fixture(mtm: MainThreadMarker, port: u16) -> anyhow::Result<()> {
        if port < 1024 || matches!(port, 9274 | 9276) {
            anyhow::bail!("network fixture requires a dedicated non-backend probe port");
        }
        let mut html = String::from(
            r#"<!doctype html><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; img-src http:"><style>body{font:20px system-ui;margin:40px;background:#f2f4f8;color:#202633}</style><h1>Local-network blocking fixture</h1><p>These synthetic images must never reach the dedicated local probe server. No scripts, accounts or backend.</p>"#,
        );
        for host in ["127.0.0.1", "localhost", "2130706433", "0x7f000001"] {
            html.push_str(&format!(
                "<img alt='blocked local probe' src='http://{host}:{port}/probe'>"
            ));
        }
        open_inner(
            mtm,
            MiniBrowserRequest::new(
                BrowserPurpose::SteamApiKeyHelp,
                "https://steamcommunity.com/dev/apikey",
                "MetalSharp — NETWORK BLOCKING fixture",
            )
            .map_err(|_| anyhow::anyhow!("fixture URL rejected"))?,
            Box::new(|_| {}),
            Some(html),
        )
    }
    #[cfg(feature = "browser-fixture")]
    pub fn open_fixture(mtm: MainThreadMarker) -> anyhow::Result<()> {
        const HTML: &str = r#"<!doctype html><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'"><style>body{font:20px system-ui;margin:40px;color:#202633;background:#f2f4f8}h1{font-size:28px}</style><h1>Offline native browser fixture</h1><p>This is bundled HTML, not a remote Steam page.</p><p>No backend, accounts, scripts, links, images or network loads.</p><p>Resize the window; test the read-only header, navigation controls and close.</p>"#;
        open_inner(
            mtm,
            MiniBrowserRequest::new(
                BrowserPurpose::SteamApiKeyHelp,
                "https://steamcommunity.com/dev/apikey",
                "MetalSharp — OFFLINE browser fixture",
            )
            .map_err(|_| anyhow::anyhow!("fixture URL rejected"))?,
            Box::new(|_| {}),
            Some(HTML.to_owned()),
        )
    }
}
#[cfg(target_os = "macos")]
pub fn open_native(
    mtm: objc2::MainThreadMarker,
    request: MiniBrowserRequest,
    completion: MiniBrowserCompletion,
) -> anyhow::Result<()> {
    native::open(mtm, request, completion)
}
/// Show Electron's in-page GameJolt browser over `rect` (window logical
/// coordinates, top-left origin). Call from paint on the AppKit main thread.
#[cfg(target_os = "macos")]
pub fn show_embedded_gamejolt(window: &gpui::Window, rect: [f64; 4]) {
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    let Some(mtm) = objc2::MainThreadMarker::new() else {
        return;
    };
    let Ok(handle) = HasWindowHandle::window_handle(window) else {
        return;
    };
    let RawWindowHandle::AppKit(appkit) = handle.as_raw() else {
        return;
    };
    if let Err(error) = native::show_embedded(mtm, appkit.ns_view.as_ptr(), rect) {
        eprintln!("MetalSharp GameJolt browser unavailable: {error}");
    }
}
#[cfg(not(target_os = "macos"))]
pub fn show_embedded_gamejolt(_window: &gpui::Window, _rect: [f64; 4]) {}

/// Hide the embedded GameJolt browser (it keeps its page and downloads).
pub fn hide_embedded_gamejolt() {
    #[cfg(target_os = "macos")]
    native::hide_embedded();
}

/// Window-space rect of the visible embedded GameJolt browser, if shown. App
/// overlays drawn by GPUI (toasts) sit beneath it and can avoid this area.
pub fn embedded_gamejolt_rect() -> Option<[f64; 4]> {
    #[cfg(target_os = "macos")]
    return native::embedded_rect();
    #[cfg(not(target_os = "macos"))]
    None
}

/// Explicit integration seam for an app-owned GameJolt data-removal action.
/// This never touches WebKit's default store and fails if a GameJolt window is open.
#[cfg(target_os = "macos")]
pub fn remove_gamejolt_browser_data(
    mtm: objc2::MainThreadMarker,
    completion: Box<dyn FnOnce(Result<(), String>) + 'static>,
) -> anyhow::Result<()> {
    native::remove_gamejolt_store(mtm, completion)
}
#[cfg(not(target_os = "macos"))]
pub fn open_native(
    _request: MiniBrowserRequest,
    _completion: MiniBrowserCompletion,
) -> anyhow::Result<()> {
    anyhow::bail!("native MiniBrowser is available only on macOS")
}

#[cfg(all(target_os = "macos", feature = "browser-fixture"))]
pub fn open_offline_fixture(mtm: objc2::MainThreadMarker) -> anyhow::Result<()> {
    native::open_fixture(mtm)
}
#[cfg(all(target_os = "macos", feature = "browser-fixture"))]
pub fn open_network_fixture(mtm: objc2::MainThreadMarker, port: u16) -> anyhow::Result<()> {
    native::open_network_fixture(mtm, port)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn network_rules_only_block_and_use_webkit_supported_regex_subset() {
        let rules: serde_json::Value = serde_json::from_str(&network_rules_json()).unwrap();
        let rules = rules.as_array().unwrap();
        assert_eq!(rules.len(), 59);
        for rule in rules {
            assert_eq!(rule["action"]["type"], "block");
            let pattern = rule["trigger"]["url-filter"].as_str().unwrap();
            assert!(!pattern.contains('|'));
            regex::Regex::new(pattern).unwrap_or_else(|error| panic!("{pattern}: {error}"));
        }
        assert_eq!(rules[0]["trigger"]["url-filter"], "^http://");
    }

    #[test]
    fn private_local_url_patterns_cover_authority_end_query_and_scheme_variants() {
        let rules: serde_json::Value = serde_json::from_str(&network_rules_json()).unwrap();
        let regexes: Vec<_> = rules
            .as_array()
            .unwrap()
            .iter()
            .map(|rule| regex::Regex::new(rule["trigger"]["url-filter"].as_str().unwrap()).unwrap())
            .collect();
        let blocked = |raw: &str| {
            let canonical = Url::parse(raw)
                .map(|url| url.to_string())
                .unwrap_or_else(|_| raw.to_owned());
            regexes.iter().any(|rule| rule.is_match(&canonical))
        };
        for raw in [
            "https://localhost",
            "https://localhost?x=1",
            "https://localhost/",
            "https://printer.local",
            "https://printer.local?x=1",
            "https://service.internal",
            "https://service.internal?x=1",
            "https://127.0.0.1",
            "https://10.0.0.1?x=1",
            "https://[::1]/",
            "https://[fd00::1]?x=1",
            "wss://localhost",
            "wss://printer.local?x=1",
            "wss://192.168.1.2/",
            "http://2130706433/",
            "http://0x7f000001/",
            "https://2130706433/",
            "https://2130706433?x=1",
            "https://0x7f000001/",
            "https://0x7f000001?x=1",
            "https://127.1/",
            "wss://2130706433/",
            "wss://0x7f000001?x=1",
        ] {
            assert!(blocked(raw), "must be blocked: {raw}");
        }
        for raw in [
            "https://public.example/",
            "wss://public.example/socket",
            "https://printer.local.example/",
            "https://notinternal.example/",
        ] {
            assert!(!blocked(raw), "must not match private-host rules: {raw}");
        }
    }
    #[test]
    fn steam_store_navigation_stays_separate_from_key_help() {
        for url in [
            "https://steampowered.com",
            "https://www.steampowered.com",
            "https://store.steampowered.com/",
        ] {
            assert!(validate_navigation(BrowserPurpose::SteamStore, url).is_ok());
        }
        for url in [
            "http://steampowered.com",
            "https://steampowered.com.attacker.invalid",
            "https://attacker.steampowered.com",
            "https://steamcommunity.com/dev/apikey",
        ] {
            assert!(validate_navigation(BrowserPurpose::SteamStore, url).is_err());
        }
        assert!(
            validate_navigation(
                BrowserPurpose::SteamApiKeyHelp,
                "https://store.steampowered.com"
            )
            .is_err()
        );
    }
    #[test]
    fn policy_or_user_cancellation_does_not_abort_auth() {
        assert!(expected_navigation_cancel("NSURLErrorDomain", -999));
        assert!(!expected_navigation_cancel("NSURLErrorDomain", -1001));
        assert!(!expected_navigation_cancel("UnrelatedError", -999));
    }
    #[test]
    fn epic_extraction_is_scoped_to_exact_result_endpoint() {
        let callback = "https://www.epicgames.com/id/api/redirect?clientId=fixture";
        assert_eq!(
            parse_epic_document(callback, r#"{"authorizationCode":"fixture-code"}"#),
            Ok(MiniBrowserResult::EpicCode("fixture-code".into()))
        );
        for url in [
            "http://www.epicgames.com/id/api/redirect",
            "https://www.epicgames.com.evil.test/id/api/redirect",
            "https://evil.epicgames.com/id/api/redirect",
            "https://www.epicgames.com/id/login",
            "https://accounts.google.com/id/api/redirect",
            "https://user@www.epicgames.com/id/api/redirect",
            "https://www.epicgames.com:444/id/api/redirect",
            "https://www.epicgames.com/id/api/redirect#spoof",
        ] {
            assert_eq!(
                parse_epic_document(url, r#"{"authorizationCode":"fixture-code"}"#),
                Err(CallbackError::NotExactCallback),
                "{url}"
            );
        }
    }
    #[test]
    fn epic_document_rejects_ambiguous_invalid_and_oversized_results() {
        let callback = "https://www.epicgames.com/id/api/redirect";
        for body in [
            r#"{}"#,
            r#"{"authorizationCode":""}"#,
            r#"{"authorizationCode":"bad code"}"#,
            r#"{"authorizationCode":"first","code":"second"}"#,
            r#"{"authorizationCode":"first","error":"denied"}"#,
            r#"{"authorizationCode":"first","authorizationCode":"second"}"#,
            r#"{"authorizationCode":123}"#,
            "<html>not a result</html>",
        ] {
            assert!(parse_epic_document(callback, body).is_err());
        }
        assert!(parse_epic_document(callback, &"x".repeat(16385)).is_err());
        assert_eq!(
            parse_epic_document(callback, r#"{"error":"denied"}"#),
            Ok(MiniBrowserResult::Error(MiniBrowserError::EpicDenied))
        );
    }
    #[test]
    fn typed_auth_results_redact_debug_output() {
        for result in [
            MiniBrowserResult::GogCode("fixture-secret".into()),
            MiniBrowserResult::EpicCode("fixture-secret".into()),
            MiniBrowserResult::Error(MiniBrowserError::Gog("fixture-secret".into())),
        ] {
            assert!(!format!("{result:?}").contains("fixture-secret"));
        }
    }
    #[test]
    fn purpose_allowlist_is_exact_secure_and_scoped() {
        assert!(validate_navigation(BrowserPurpose::GogAuth, "https://auth.gog.com/").is_ok());
        assert!(
            validate_navigation(
                BrowserPurpose::GogAuth,
                "https://embed.gog.com/on_login_success"
            )
            .is_ok()
        );
        assert!(
            validate_navigation(BrowserPurpose::EpicAuth, "https://accounts.google.com/").is_ok()
        );
        assert!(
            validate_navigation(BrowserPurpose::EpicAuth, "https://appleid.apple.com/").is_ok()
        );
        assert!(validate_navigation(BrowserPurpose::EpicAuth, "https://www.facebook.com/").is_ok());
        assert!(
            validate_navigation(BrowserPurpose::EpicAuth, "https://id.playstation.com/").is_ok()
        );
        assert!(
            validate_navigation(BrowserPurpose::EpicAuth, "https://accounts.nintendo.net/").is_ok()
        );
        assert!(
            validate_navigation(
                BrowserPurpose::SteamApiKeyHelp,
                "https://steamcommunity.com/dev/apikey"
            )
            .is_ok()
        );
        assert!(
            validate_navigation(
                BrowserPurpose::TheGamesDbHelp,
                "https://www.thegamesdb.net/"
            )
            .is_ok()
        );
        assert!(validate_navigation(BrowserPurpose::EpicAuth, "https://login.live.com/").is_ok());
        assert!(
            validate_navigation(
                BrowserPurpose::EpicAuth,
                "https://id.sonyentertainmentnetwork.com/"
            )
            .is_ok()
        );
        assert!(
            validate_navigation(BrowserPurpose::GameJolt, "https://gamejolt.com/games").is_ok()
        );
        assert!(
            validate_navigation(
                BrowserPurpose::TheGamesDbHelp,
                "https://api.thegamesdb.net/"
            )
            .is_ok()
        );
        for raw in [
            "http://embed.gog.com/",
            "https://embed.gog.com.evil.test/",
            "https://evil.test/",
            "javascript:alert(1)",
            "https://user@embed.gog.com/",
            "https://@embed.gog.com/",
            "https://embed.gog.com:444/",
        ] {
            assert!(
                validate_navigation(BrowserPurpose::GogAuth, raw).is_err(),
                "{raw}"
            );
        }
        assert!(
            validate_navigation(BrowserPurpose::GameJolt, "https://steamcommunity.com/").is_err()
        );
    }
    #[test]
    fn gog_callback_is_exact_and_one_shot() {
        let p = GogCallbackParser::default();
        let cb = "https://embed.gog.com/on_login_success?code=secret";
        assert_eq!(p.parse_once(cb), Ok(GogCallback::Code("secret".into())));
        assert_eq!(p.parse_once(cb), Err(CallbackError::Duplicate));
        assert_eq!(
            GogCallbackParser::default()
                .parse_once("https://embed.gog.com/on_login_success?error=denied"),
            Ok(GogCallback::Error("denied".into()))
        );
        assert_eq!(
            GogCallbackParser::default()
                .parse_once("https://embed.gog.com/on_login_success?error_description=denied"),
            Ok(GogCallback::Error("denied".into()))
        );
        // Callback consumption belongs to a single window/session parser.
        assert_eq!(
            GogCallbackParser::default().parse_once(cb),
            Ok(GogCallback::Code("secret".into()))
        );
        for raw in [
            "http://embed.gog.com/on_login_success?code=x",
            "https://embed.gog.com.evil.test/on_login_success?code=x",
            "https://user@embed.gog.com/on_login_success?code=x",
            "https://embed.gog.com:444/on_login_success?code=x",
            "https://embed.gog.com/on_login_success/?code=x",
            "https://embed.gog.com/on_login_success?code=x&error=y",
            "https://embed.gog.com/on_login_success?error=x&error_description=y",
            "https://embed.gog.com/on_login_success?error_description=x&error_description=y",
        ] {
            assert!(
                GogCallbackParser::default().parse_once(raw).is_err(),
                "{raw}"
            );
        }
    }
}
