//! Window size preferences use logical points and always leave room for the desktop.

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum WindowSize {
    #[default]
    Automatic,
    Compact,
    FullHd,
    Qhd,
    Large,
}

impl WindowSize {
    pub const ALL: [Self; 5] = [
        Self::Automatic,
        Self::Compact,
        Self::FullHd,
        Self::Qhd,
        Self::Large,
    ];

    pub fn label_key(self) -> &'static str {
        match self {
            Self::Automatic => "settings.window_size_automatic",
            Self::Compact => "settings.window_size_compact",
            Self::FullHd => "settings.window_size_full_hd",
            Self::Qhd => "settings.window_size_qhd",
            Self::Large => "settings.window_size_large",
        }
    }

    /**
     * Fits the selected preset inside the logical display while respecting the app's minimum size.
     *
     * @param width - Display width in logical points.
     * @param height - Display height in logical points.
     * @returns Window content size, capped at 85% of the display unless the minimum size requires more.
     */
    pub fn for_display(self, width: f32, height: f32) -> (f32, f32) {
        if !width.is_finite() || !height.is_finite() || width <= 0.0 || height <= 0.0 {
            return (1360.0, 860.0);
        }
        let requested = match self {
            Self::Automatic => (width * 0.75, height * 0.8),
            Self::Compact => (1360.0, 860.0),
            Self::FullHd => (1920.0, 1080.0),
            Self::Qhd => (2560.0, 1440.0),
            Self::Large => (2800.0, 1700.0),
        };
        (
            requested
                .0
                .clamp(720.0, (width * 0.85).clamp(720.0, 2800.0)),
            requested
                .1
                .clamp(540.0, (height * 0.85).clamp(540.0, 1700.0)),
        )
    }

    /**
     * Applies the preset to the window's current display and centers the resized macOS window.
     *
     * @param window - Application window to resize without entering fullscreen.
     * @param cx - Application context used to schedule native changes outside the current update.
     */
    pub fn apply(self, window: &mut gpui::Window, cx: &mut gpui::App) {
        let (width, height) = window
            .display(cx)
            .or_else(|| cx.primary_display())
            .map(|display| {
                let size = display.bounds().size;
                self.for_display(f32::from(size.width), f32::from(size.height))
            })
            .unwrap_or((1360.0, 860.0));

        #[cfg(target_os = "macos")]
        {
            use objc2::rc::Retained;
            use objc2_app_kit::{NSView, NSWindowStyleMask};
            use objc2_foundation::NSSize;
            use raw_window_handle::{HasWindowHandle, RawWindowHandle};

            if let Ok(handle) = HasWindowHandle::window_handle(window)
                && let RawWindowHandle::AppKit(appkit) = handle.as_raw()
                && let Some(view) =
                    unsafe { Retained::<NSView>::retain(appkit.ns_view.as_ptr().cast()) }
                && let Some(native_window) = view.window()
            {
                cx.spawn(async move |cx| {
                    if native_window
                        .styleMask()
                        .contains(NSWindowStyleMask::FullScreen)
                    {
                        native_window.toggleFullScreen(None);
                        for _ in 0..60 {
                            cx.background_executor()
                                .timer(std::time::Duration::from_millis(50))
                                .await;
                            if !native_window
                                .styleMask()
                                .contains(NSWindowStyleMask::FullScreen)
                            {
                                break;
                            }
                        }
                        if native_window
                            .styleMask()
                            .contains(NSWindowStyleMask::FullScreen)
                        {
                            return;
                        }
                    }
                    native_window.setContentSize(NSSize::new(width as f64, height as f64));
                    native_window.center();
                })
                .detach();
                return;
            }
        }
        window.resize(gpui::size(gpui::px(width), gpui::px(height)));
    }
}

#[cfg(test)]
mod tests {
    use super::WindowSize;

    #[test]
    fn presets_keep_their_requested_sizes_on_a_large_logical_display() {
        for (preset, expected) in [
            (WindowSize::Automatic, (2800.0, 1700.0)),
            (WindowSize::Compact, (1360.0, 860.0)),
            (WindowSize::FullHd, (1920.0, 1080.0)),
            (WindowSize::Qhd, (2560.0, 1440.0)),
            (WindowSize::Large, (2800.0, 1700.0)),
        ] {
            assert_eq!(preset.for_display(3840.0, 2160.0), expected);
        }
    }

    #[test]
    fn presets_leave_desktop_space_and_respect_minimum_bounds() {
        for (width, height) in [
            (800.0_f32, 600.0_f32),
            (1920.0, 1080.0),
            (2560.0, 1440.0),
            (3840.0, 2160.0),
            (7680.0, 4320.0),
        ] {
            for preset in WindowSize::ALL {
                let size = preset.for_display(width, height);
                assert!((720.0..=2800.0).contains(&size.0));
                assert!((540.0..=1700.0).contains(&size.1));
                assert!(size.0 <= (width * 0.85).max(720.0));
                assert!(size.1 <= (height * 0.85).max(540.0));
                assert!(size.0 < width && size.1 < height);
            }
        }
        assert_eq!(WindowSize::Large.for_display(800.0, 600.0), (720.0, 540.0));
    }

    #[test]
    fn automatic_sizing_uses_logical_points_including_hidpi_displays() {
        assert_eq!(
            WindowSize::Automatic.for_display(1920.0, 1080.0),
            (1440.0, 864.0)
        );
        assert_eq!(
            WindowSize::Automatic.for_display(2560.0, 1440.0),
            (1920.0, 1152.0)
        );
        assert_eq!(
            WindowSize::Automatic.for_display(3840.0 / 2.0, 2160.0 / 2.0),
            WindowSize::Automatic.for_display(1920.0, 1080.0)
        );
        assert_eq!(
            WindowSize::Automatic.for_display(f32::NAN, 1080.0),
            (1360.0, 860.0)
        );
    }

    #[test]
    fn preference_round_trips_with_stable_snake_case_values() {
        assert_eq!(WindowSize::default(), WindowSize::Automatic);
        for (preset, stored) in
            WindowSize::ALL
                .into_iter()
                .zip(["automatic", "compact", "full_hd", "qhd", "large"])
        {
            let value = serde_json::to_value(preset).unwrap();
            assert_eq!(value, stored);
            assert_eq!(serde_json::from_value::<WindowSize>(value).unwrap(), preset);
            assert!(preset.label_key().starts_with("settings.window_size_"));
        }
        assert!(serde_json::from_str::<WindowSize>("\"unknown\"").is_err());
    }
}
