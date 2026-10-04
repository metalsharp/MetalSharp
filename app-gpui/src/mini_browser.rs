//! Purpose-scoped native WebKit mini-browser. This module is deliberately not wired
//! into preview settings/runtime; call `open_native` from the AppKit/GPUI main thread.
use std::sync::Mutex;
use url::Url;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum BrowserPurpose {
    GogAuth,
    EpicAuth,
    GameJolt,
    SteamApiKeyHelp,
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
            Self::GameJolt => suffix(&["gamejolt.com"]),
            Self::SteamApiKeyHelp => exact(&["steamcommunity.com", "www.steamcommunity.com"]),
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
        let errors: Vec<_> = pairs.iter().filter(|(k, _)| k == "error").collect();
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

/// Completion is delivered once on native window close/callback. Only GOG can yield
/// a code; help/browser closes yield Cancelled. Invoke from GPUI's AppKit main thread.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MiniBrowserResult {
    GogCode(String),
    Error(MiniBrowserError),
    Cancelled,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MiniBrowserError {
    Gog(String),
    NavigationFailed,
}
pub type MiniBrowserCompletion = Box<dyn FnOnce(MiniBrowserResult) + 'static>;

#[cfg(target_os = "macos")]
mod native {
    use super::*;
    use block2::DynBlock;
    use core::cell::{OnceCell, RefCell};
    use objc2::{
        AnyThread, ClassType, DefinedClass, MainThreadMarker, MainThreadOnly, define_class,
        rc::Retained,
        runtime::{AnyObject, ProtocolObject},
        sel,
    };
    use objc2_app_kit::{
        NSBackingStoreType, NSButton, NSColor, NSLayoutAttribute, NSStackView,
        NSStackViewDistribution, NSTextField, NSUserInterfaceLayoutOrientation, NSWindow,
        NSWindowDelegate, NSWindowStyleMask,
    };
    use objc2_foundation::{
        NSHTTPURLResponse, NSObject, NSObjectProtocol, NSPoint, NSRect, NSSize, NSString, NSURL,
        NSURLRequest, NSUUID,
    };
    use objc2_web_kit::{
        WKFrameInfo, WKMediaCaptureType, WKNavigation, WKNavigationAction,
        WKNavigationActionPolicy, WKNavigationDelegate, WKNavigationResponse,
        WKNavigationResponsePolicy, WKPermissionDecision, WKSecurityOrigin, WKUIDelegate,
        WKWebView, WKWebViewConfiguration, WKWebsiteDataStore,
    };
    thread_local! { static DELEGATES: RefCell<Vec<Retained<Delegate>>> = const { RefCell::new(Vec::new()) }; }
    #[derive(Default)]
    struct Ivars {
        purpose: OnceCell<BrowserPurpose>,
        header: RefCell<Option<Retained<NSTextField>>>,
        window: RefCell<Option<Retained<NSWindow>>>,
        completion: Mutex<Option<MiniBrowserCompletion>>,
        callback: GogCallbackParser,
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
                self.ivars().header.borrow_mut().take();
                self.ivars().window.borrow_mut().take();
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
                let raw = unsafe { req.URL() }
                    .and_then(|u| unsafe { u.absoluteString() })
                    .map(|s| s.to_string());
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
                unsafe {
                    handler.call((if allow {
                        WKNavigationActionPolicy::Allow
                    } else {
                        WKNavigationActionPolicy::Cancel
                    },));
                }
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
                let allow = !is_attachment && unsafe { response.canShowMIMEType() }
                    && response_url.as_deref().is_some_and(|url| validate_navigation(*self.ivars().purpose.get().unwrap(), url).is_ok());
                unsafe {
                    handler.call((if allow {
                        WKNavigationResponsePolicy::Allow
                    } else {
                        WKNavigationResponsePolicy::Cancel
                    },));
                }
            }
            #[unsafe(method(webView:didFailProvisionalNavigation:withError:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didFailProvisionalNavigation_withError(&self, _web_view: &WKWebView, _navigation: Option<&WKNavigation>, _error: &objc2_foundation::NSError) { self.finish(MiniBrowserResult::Error(MiniBrowserError::NavigationFailed)); self.close(); }
            #[unsafe(method(webView:didFailNavigation:withError:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didFailNavigation_withError(&self, _web_view: &WKWebView, _navigation: Option<&WKNavigation>, _error: &objc2_foundation::NSError) { self.finish(MiniBrowserResult::Error(MiniBrowserError::NavigationFailed)); self.close(); }
            #[unsafe(method(webView:didStartProvisionalNavigation:))]
            #[allow(non_snake_case)]
            unsafe fn webView_didStartProvisionalNavigation(
                &self,
                web_view: &WKWebView,
                _navigation: Option<&WKNavigation>,
            ) {
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
            }
        }
        impl Delegate {
            // Optional WKUIDelegate selector added by macOS 27 WebKit. It is harmless
            // on older systems and explicitly denies geolocation when WebKit supports it.
            #[unsafe(method(webView:requestGeolocationPermissionForOrigin:initiatedByFrame:decisionHandler:))]
            #[allow(non_snake_case)]
            unsafe fn deny_geolocation(&self, _web_view: &WKWebView, _origin: &WKSecurityOrigin, _frame: &WKFrameInfo, handler: &DynBlock<dyn Fn(WKPermissionDecision)>) { handler.call((WKPermissionDecision::Deny,)); }
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
                unsafe { h.call(()) }
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
                unsafe { h.call((objc2::runtime::Bool::NO,)) }
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
                unsafe { h.call((core::ptr::null_mut(),)) }
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
                unsafe { h.call((WKPermissionDecision::Deny,)) }
            }
        }
    );
    impl Delegate {
        fn new(
            mtm: MainThreadMarker,
            request: &MiniBrowserRequest,
            completion: MiniBrowserCompletion,
        ) -> Retained<Self> {
            let this = Self::alloc(mtm).set_ivars(Ivars {
                purpose: OnceCell::from(request.purpose),
                header: RefCell::new(None),
                window: RefCell::new(None),
                completion: Mutex::new(
                    if matches!(
                        request.purpose,
                        BrowserPurpose::GameJolt
                            | BrowserPurpose::SteamApiKeyHelp
                            | BrowserPurpose::TheGamesDbHelp
                    ) {
                        None
                    } else {
                        Some(completion)
                    },
                ),
                callback: GogCallbackParser::default(),
            });
            unsafe { objc2::msg_send![super(this), init] }
        }
        fn finish(&self, result: MiniBrowserResult) {
            if matches!(
                self.ivars().purpose.get(),
                Some(
                    BrowserPurpose::GameJolt
                        | BrowserPurpose::SteamApiKeyHelp
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
        fn update_url(&self, web_view: &WKWebView) {
            if let Some(url) = unsafe { web_view.URL() }.and_then(|u| u.absoluteString()) {
                if let Some(header) = self.ivars().header.borrow().as_ref() {
                    header.setStringValue(&url);
                }
            }
        }
        fn close(&self) {
            if let Some(w) = self.ivars().window.borrow().as_ref() {
                w.close()
            }
        }
    }
    pub fn open(
        mtm: MainThreadMarker,
        request: MiniBrowserRequest,
        completion: MiniBrowserCompletion,
    ) -> anyhow::Result<()> {
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
        let window = unsafe {
            NSWindow::initWithContentRect_styleMask_backing_defer(
                NSWindow::alloc(mtm),
                frame,
                style,
                NSBackingStoreType::Buffered,
                false,
            )
        };
        let config = unsafe { WKWebViewConfiguration::new(mtm) };
        let store = if request.purpose == BrowserPurpose::GameJolt {
            let identifier = unsafe {
                NSUUID::initWithUUIDString(
                    NSUUID::alloc(),
                    &objc2_foundation::NSString::from_str("5C6F493A-2D9E-4A25-BEB0-7DC86791553A"),
                )
            }
            .ok_or_else(|| anyhow::anyhow!("GameJolt persistent store identifier is invalid"))?;
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
        let delegate = Delegate::new(mtm, &request, completion);
        let header = unsafe { NSTextField::initWithFrame(NSTextField::alloc(mtm), NSRect::ZERO) };
        unsafe {
            header.setEditable(false);
            header.setSelectable(true);
            header.setDrawsBackground(true);
            header.setBackgroundColor(Some(&NSColor::whiteColor()));
        }
        *delegate.ivars().header.borrow_mut() = Some(header.clone());
        *delegate.ivars().window.borrow_mut() = Some(window.clone());
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
        let bar = unsafe { NSStackView::initWithFrame(NSStackView::alloc(mtm), NSRect::ZERO) };
        unsafe {
            bar.setOrientation(NSUserInterfaceLayoutOrientation::Horizontal);
            bar.setAlignment(NSLayoutAttribute::Height);
            bar.setDistribution(NSStackViewDistribution::Fill);
            bar.setSpacing(5.);
        }
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
            unsafe {
                bar.addArrangedSubview(&b);
            }
        }
        unsafe {
            bar.addArrangedSubview(&header);
        }
        let content = unsafe { NSStackView::initWithFrame(NSStackView::alloc(mtm), frame) };
        unsafe {
            content.setOrientation(NSUserInterfaceLayoutOrientation::Vertical);
            content.setAlignment(NSLayoutAttribute::Width);
            content.setDistribution(NSStackViewDistribution::Fill);
            content.setSpacing(0.);
            content.addArrangedSubview(&bar);
            content.addArrangedSubview(&web);
            window.setContentView(Some(&content));
        }
        // The native delegate is weakly held by WebKit; retain it on the main thread
        // for the lifetime of the process (small per-window object; close cancels callback).
        DELEGATES.with(|items| items.borrow_mut().push(delegate));
        unsafe {
            window.setTitle(&objc2_foundation::NSString::from_str(&request.title));
        }
        window.setMinSize(NSSize::new(720., 540.));
        window.center();
        window.makeKeyAndOrderFront(None);
        let req_url = NSURL::URLWithString(&objc2_foundation::NSString::from_str(
            request.initial_url.as_str(),
        ))
        .ok_or_else(|| anyhow::anyhow!("initial URL could not be represented by NSURL"))?;
        unsafe {
            web.loadRequest(&NSURLRequest::requestWithURL(&req_url));
        }
        Ok(())
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
#[cfg(not(target_os = "macos"))]
pub fn open_native(
    _request: MiniBrowserRequest,
    _completion: MiniBrowserCompletion,
) -> anyhow::Result<()> {
    anyhow::bail!("native MiniBrowser is available only on macOS")
}

#[cfg(test)]
mod tests {
    use super::*;
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
        for raw in [
            "http://embed.gog.com/on_login_success?code=x",
            "https://embed.gog.com.evil.test/on_login_success?code=x",
            "https://user@embed.gog.com/on_login_success?code=x",
            "https://embed.gog.com:444/on_login_success?code=x",
            "https://embed.gog.com/on_login_success/?code=x",
            "https://embed.gog.com/on_login_success?code=x&error=y",
        ] {
            assert!(
                GogCallbackParser::default().parse_once(raw).is_err(),
                "{raw}"
            );
        }
    }
}
