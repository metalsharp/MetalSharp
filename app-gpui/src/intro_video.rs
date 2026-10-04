//! App.vue first-launch startup video: `MetalSharp-Startup.mp4` plays muted over
//! the whole window with a "Skip intro" button. Ending, failing or skipping
//! dismisses it; the caller then records it as seen (Electron's localStorage
//! `metalsharp-startup-video-seen`).
use gpui::Window;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

/// The 8 s clip plus load slack; a page that never reports back is dismissed.
const MAX_SHOW: Duration = Duration::from_secs(30);

fn intro_page() -> PathBuf {
    let page = crate::ui::asset_path("intro/intro.html");
    page.canonicalize().unwrap_or(page)
}

fn video_path() -> Option<PathBuf> {
    // Canonical: WebKit's read-access root must really contain the file.
    let video = crate::ui::asset_path("intro/MetalSharp-Startup.mp4");
    if video.is_file() {
        video.canonicalize().ok()
    } else {
        None
    }
}

fn file_url(path: &Path) -> String {
    let mut url = String::from("file://");
    for byte in path.to_string_lossy().bytes() {
        if byte.is_ascii_alphanumeric() || b"/-_.~".contains(&byte) {
            url.push(byte as char);
        } else {
            url.push_str(&format!("%{byte:02X}"));
        }
    }
    url
}

fn common_ancestor(a: &Path, b: &Path) -> PathBuf {
    let mut shared = PathBuf::new();
    for (x, y) in a.components().zip(b.components()) {
        if x != y {
            break;
        }
        shared.push(x);
    }
    shared
}

#[cfg(target_os = "macos")]
mod native {
    use super::*;
    use objc2::rc::Retained;
    use objc2::{MainThreadMarker, MainThreadOnly};
    use objc2_app_kit::{NSAutoresizingMaskOptions, NSView};
    use objc2_foundation::{NSString, NSURL};
    use objc2_web_kit::{WKAudiovisualMediaTypes, WKWebView, WKWebViewConfiguration};
    use std::cell::RefCell;

    struct Shown {
        web: Retained<WKWebView>,
        host: Retained<NSView>,
        started: Instant,
    }

    thread_local! {
        static SHOWN: RefCell<Option<Shown>> = const { RefCell::new(None) };
    }

    fn host_view(window: &Window) -> Option<Retained<NSView>> {
        use raw_window_handle::{HasWindowHandle, RawWindowHandle};
        let handle = HasWindowHandle::window_handle(window).ok()?;
        let RawWindowHandle::AppKit(appkit) = handle.as_raw() else {
            return None;
        };
        unsafe { Retained::retain(appkit.ns_view.as_ptr().cast()) }
    }

    pub fn show(window: &Window) -> bool {
        if SHOWN.with(|s| s.borrow().is_some()) {
            return true;
        }
        let (Some(mtm), Some(host), Some(video)) =
            (MainThreadMarker::new(), host_view(window), video_path())
        else {
            return false;
        };
        let page = intro_page();
        if !page.is_file() {
            return false;
        }
        let page_url = format!("{}?src={}", file_url(&page), {
            // The query value itself must be URL-encoded once more.
            file_url(&video).replace('%', "%25").replace(':', "%3A")
        });
        let Some(url) = NSURL::URLWithString(&NSString::from_str(&page_url)) else {
            return false;
        };
        let read_access = NSURL::fileURLWithPath(&NSString::from_str(
            &common_ancestor(&page, &video).to_string_lossy(),
        ));
        let web = unsafe {
            let config = WKWebViewConfiguration::new(mtm);
            config.setMediaTypesRequiringUserActionForPlayback(WKAudiovisualMediaTypes::None);
            let web = WKWebView::initWithFrame_configuration(
                WKWebView::alloc(mtm),
                host.bounds(),
                &config,
            );
            // No white flash before the page paints its #050607 background.
            let no = objc2_foundation::NSNumber::new_bool(false);
            let _: () = objc2::msg_send![&*web, setValue: &*no, forKey: &*NSString::from_str("drawsBackground")];
            web
        };
        web.setAutoresizingMask(
            NSAutoresizingMaskOptions::ViewWidthSizable
                | NSAutoresizingMaskOptions::ViewHeightSizable,
        );
        host.addSubview(&web);
        unsafe {
            web.loadFileURL_allowingReadAccessToURL(&url, &read_access);
        }
        SHOWN.with(|s| {
            *s.borrow_mut() = Some(Shown {
                web,
                host,
                started: Instant::now(),
            })
        });
        true
    }

    /// True once the overlay has finished (and is removed), or none is shown.
    pub fn poll_finished() -> bool {
        let done = SHOWN.with(|s| {
            let s = s.borrow();
            let Some(shown) = s.as_ref() else {
                return true;
            };
            let ended = unsafe { shown.web.URL() }
                .and_then(|url| url.fragment())
                .is_some_and(|fragment| fragment.to_string() == "done");
            ended || shown.started.elapsed() > MAX_SHOW
        });
        if done {
            dismiss();
        }
        done
    }

    fn dismiss() {
        if let Some(shown) = SHOWN.with(|s| s.borrow_mut().take()) {
            shown.web.removeFromSuperview();
            if let Some(window) = shown.host.window() {
                window.makeFirstResponder(Some(&shown.host));
            }
        }
    }
}

#[cfg(target_os = "macos")]
pub use native::{poll_finished, show};

#[cfg(not(target_os = "macos"))]
pub fn show(_window: &Window) -> bool {
    false
}
#[cfg(not(target_os = "macos"))]
pub fn poll_finished() -> bool {
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn file_urls_are_percent_encoded() {
        assert_eq!(
            file_url(Path::new("/Applications/Metal Sharp.app/a.mp4")),
            "file:///Applications/Metal%20Sharp.app/a.mp4"
        );
    }

    #[test]
    fn read_access_covers_page_and_video() {
        assert_eq!(
            common_ancestor(
                Path::new("/r/app-gpui/assets/intro/intro.html"),
                Path::new("/r/app-gpui/assets/intro/v.mp4")
            ),
            PathBuf::from("/r/app-gpui/assets/intro")
        );
    }
}
