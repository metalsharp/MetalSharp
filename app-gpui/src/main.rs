#[allow(dead_code)]
mod artwork;
#[allow(dead_code)]
mod backend;
mod backend_host;
mod compat;
#[allow(dead_code)]
mod configuration;
mod diagnostics;
mod host_actions;
mod hotkeys;
mod intro_video;
mod launch_overlay;
mod library_model;
mod lifecycle;
mod live;
mod logs_preview;
mod markdown;
#[allow(dead_code)]
mod mini_browser;
mod page_palette;
mod process_manager;
mod search_input;
mod settings_preview;
mod sharp_preview;
#[allow(dead_code)]
mod streaming;
mod toast;
mod ui;
mod unix_transport;
mod updater_bridge;
mod variant;
mod window_layout;

use anyhow::Result;
use gpui::{App, AppContext, Application, Bounds, WindowBounds, WindowOptions, px, size};
use std::borrow::Cow;

struct PreviewIcons;

impl gpui::AssetSource for PreviewIcons {
    fn load(&self, path: &str) -> Result<Option<Cow<'static, [u8]>>> {
        Ok(match path {
            "icon-search.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-search.svg"))),
            "icon-chevron-down.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/icon-chevron-down.svg"
            ))),
            "icon-chevron-left.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/icon-chevron-left.svg"
            ))),
            "icon-chevron-right.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/icon-chevron-right.svg"
            ))),
            "icon-edit.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-edit.svg"))),
            "icon-close.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-close.svg"))),
            "icon-download.svg" => {
                Some(Cow::Borrowed(include_bytes!("../assets/icon-download.svg")))
            }
            "icon-play.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-play.svg"))),
            "stream-tv.svg" => Some(Cow::Borrowed(include_bytes!("../assets/stream-tv.svg"))),
            "stream-wifi.svg" => Some(Cow::Borrowed(include_bytes!("../assets/stream-wifi.svg"))),
            "theme-skeleton.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/theme-skeleton.svg"
            ))),
            "theme-forest.svg" => Some(Cow::Borrowed(include_bytes!("../assets/theme-forest.svg"))),
            "theme-orange.svg" => Some(Cow::Borrowed(include_bytes!("../assets/theme-orange.svg"))),
            "source-installers.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/source-installers.svg"
            ))),
            "source-gog.svg" => Some(Cow::Borrowed(include_bytes!("../assets/source-gog.svg"))),
            "source-epic.svg" => Some(Cow::Borrowed(include_bytes!("../assets/source-epic.svg"))),
            "source-gamejolt.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/source-gamejolt.svg"
            ))),
            "source-pcsx2.svg" => Some(Cow::Borrowed(include_bytes!("../assets/source-pcsx2.svg"))),
            "source-rpcs3.svg" => Some(Cow::Borrowed(include_bytes!("../assets/source-rpcs3.svg"))),
            "source-shadps4.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/source-shadps4.svg"
            ))),
            "source-sharpemu.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/source-sharpemu.svg"
            ))),
            "source-empty-monitor.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/source-empty-monitor.svg"
            ))),
            "launch-settings.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/launch-settings.svg"
            ))),
            "icon-steam.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-steam.svg"))),
            "icon-ubisoft.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-ubisoft.svg"))),
            "icon-gamepad.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-gamepad.svg"))),
            "icon-sharp.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-sharp.svg"))),
            "icon-collection.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/icon-collection.svg"
            ))),
            "icon-logs.svg" => Some(Cow::Borrowed(include_bytes!("../assets/icon-logs.svg"))),
            "theme-moon.svg" => Some(Cow::Borrowed(include_bytes!("../assets/theme-moon.svg"))),
            "theme-sun.svg" => Some(Cow::Borrowed(include_bytes!("../assets/theme-sun.svg"))),
            "theme-dragonfruit.svg" => Some(Cow::Borrowed(include_bytes!(
                "../assets/theme-dragonfruit.svg"
            ))),
            "theme-galaxy.svg" => Some(Cow::Borrowed(include_bytes!("../assets/theme-galaxy.svg"))),
            _ => None,
        })
    }

    fn list(&self, _path: &str) -> Result<Vec<gpui::SharedString>> {
        Ok(vec![
            "icon-search.svg".into(),
            "icon-chevron-down.svg".into(),
            "icon-chevron-left.svg".into(),
            "icon-chevron-right.svg".into(),
            "icon-edit.svg".into(),
            "icon-close.svg".into(),
            "icon-download.svg".into(),
            "icon-play.svg".into(),
            "stream-tv.svg".into(),
            "stream-wifi.svg".into(),
            "theme-skeleton.svg".into(),
            "theme-forest.svg".into(),
            "theme-orange.svg".into(),
            "launch-settings.svg".into(),
            "source-installers.svg".into(),
            "source-gog.svg".into(),
            "source-epic.svg".into(),
            "source-gamejolt.svg".into(),
            "source-pcsx2.svg".into(),
            "source-rpcs3.svg".into(),
            "source-shadps4.svg".into(),
            "source-sharpemu.svg".into(),
            "source-empty-monitor.svg".into(),
            "icon-steam.svg".into(),
            "icon-ubisoft.svg".into(),
            "icon-gamepad.svg".into(),
            "icon-sharp.svg".into(),
            "icon-collection.svg".into(),
            "icon-logs.svg".into(),
            "theme-moon.svg".into(),
            "theme-sun.svg".into(),
            "theme-dragonfruit.svg".into(),
            "theme-galaxy.svg".into(),
        ])
    }
}

fn preview_size(argument: &str) -> Result<Option<(f32, f32)>> {
    if argument == "--preview" {
        return Ok(Some((1360.0, 860.0)));
    }
    let Some(dimensions) = argument.strip_prefix("--preview=") else {
        return Ok(None);
    };
    let (width, height) = dimensions
        .split_once('x')
        .and_then(|(width, height)| Some((width.parse::<f32>().ok()?, height.parse::<f32>().ok()?)))
        .filter(|(width, height)| {
            width.is_finite()
                && height.is_finite()
                && (720.0..=3840.0).contains(width)
                && (540.0..=2160.0).contains(height)
        })
        .ok_or_else(|| {
            anyhow::anyhow!("Use --preview=WIDTHxHEIGHT with width 720..3840 and height 540..2160")
        })?;
    Ok(Some((width, height)))
}

/**
 * Sizes the initial window to leave desktop space, preserving explicit preview dimensions.
 *
 * @param display_size - Primary display size in logical points, when available.
 * @param preview_override - Validated dimensions from an explicit --preview=WIDTHxHEIGHT argument.
 * @returns Initial window content size in logical points.
 */
fn initial_window_size(
    display_size: Option<(f32, f32)>,
    preview_override: Option<(f32, f32)>,
) -> (f32, f32) {
    if let Some(size) = preview_override {
        return size;
    }
    match display_size {
        Some((width, height)) => window_layout::WindowSize::Automatic.for_display(width, height),
        _ => (1360.0, 860.0),
    }
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
    let browser_fixture = matches!(args.as_slice(),[mode] if mode=="--browser-fixture");
    #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
    let browser_steam_test = matches!(args.as_slice(),[mode] if mode=="--browser-steam-test");
    #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
    if matches!(args.as_slice(),[mode] if mode=="--browser-network-fixture") {
        let port: u16 = std::env::var("METALSHARP_BROWSER_PROBE_PORT")?.parse()?;
        gpui::Application::new().run(move |cx| {
            let mtm = objc2::MainThreadMarker::new().expect("GPUI runs on main thread");
            if let Err(error) = mini_browser::open_network_fixture(mtm, port) {
                eprintln!("Network fixture failed: {error}");
                cx.quit();
                return;
            }
            println!("NETWORK_BROWSER_FIXTURE_OPENED");
            cx.activate(true);
        });
        return Ok(());
    }
    // Electron `isProcessManagerOnlyRuntime`: only the Cmd+P HUD, talking to
    // whatever backend is already listening; no main window, no owned backend.
    if std::env::var("METALSHARP_PROCESS_MANAGER_ONLY").as_deref() == Ok("1")
        || args.iter().any(|arg| arg == "--process-manager-overlay")
    {
        let config = backend_host::HostConfig::from_environment(false)?;
        Application::new()
            .with_assets(PreviewIcons)
            .run(move |cx: &mut App| {
                cx.set_global(live::Live::new(config));
                hotkeys::register();
                process_manager::install(cx);
                process_manager::toggle(cx);
                cx.on_window_closed(|cx| {
                    if cx.windows().is_empty() {
                        cx.quit();
                    }
                })
                .detach();
            });
        return Ok(());
    }
    // Default launch is the production app (~/.metalsharp/run/backend.sock), like Electron.
    let preview_bounds = match args.as_slice() {
        [mode] => preview_size(mode)?,
        _ => None,
    };
    let preview_override = match args.as_slice() {
        [mode] if mode.starts_with("--preview=") => preview_bounds,
        _ => None,
    };
    let connected = match args.as_slice() {
        [] => Some(backend_host::HostConfig::from_environment(false)?),
        [_] if preview_bounds.is_some() => None,
        #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
        [mode] if mode == "--browser-fixture" || mode == "--browser-steam-test" => None,
        [mode] if mode == "--connected-validation" || mode == "--connected-setup-validation" => {
            Some(backend_host::HostConfig::from_environment(true)?)
        }
        [mode] if mode == "--connected-production" => {
            Some(backend_host::HostConfig::from_environment(false)?)
        }
        // macOS may pass -psn_*/-NS* launch arguments; treat as a normal launch.
        _ if args.iter().all(|arg| {
            arg.starts_with("-psn") || arg.starts_with("-NS") || arg.starts_with("-Apple")
        }) =>
        {
            Some(backend_host::HostConfig::from_environment(false)?)
        }
        _ => anyhow::bail!(
            "Use no arguments for MetalSharp, --preview[=WIDTHxHEIGHT] for the offline sample UI, or --connected-validation for an isolated data home"
        ),
    };
    if connected.is_some() {
        lifecycle::install_connected_signal_handlers()?;
    }
    Application::new()
        .with_assets(PreviewIcons)
        .run(move |cx: &mut App| {
            search_input::register_keys(cx);
            cx.text_system()
                .add_fonts(vec![
                    Cow::Borrowed(include_bytes!("../assets/RethinkSans-Regular.ttf")),
                    Cow::Borrowed(include_bytes!("../assets/RethinkSans-SemiBold.ttf")),
                ])
                .expect("failed to register MetalSharp setup fonts");

            #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
            if browser_fixture {
                mini_browser::open_offline_fixture(
                    objc2::MainThreadMarker::new().expect("AppKit main thread required"),
                )
                .expect("failed to open offline browser fixture");
                println!("OFFLINE_NATIVE_BROWSER_OPENED");
                cx.activate(true);
                return;
            }
            #[cfg(all(target_os = "macos", feature = "browser-fixture"))]
            if browser_steam_test {
                mini_browser::open_native(
                    objc2::MainThreadMarker::new().expect("AppKit main thread required"),
                    mini_browser::MiniBrowserRequest::new(
                        mini_browser::BrowserPurpose::SteamStore,
                        "https://steampowered.com",
                        "MetalSharp — Steam browser test",
                    )
                    .expect("Steam URL policy"),
                    Box::new(|_| {}),
                )
                .expect("failed to open native Steam browser");
                cx.activate(true);
                return;
            }
            cx.on_window_closed(|cx| {
                if cx
                    .windows()
                    .iter()
                    .all(|window| window.downcast::<ui::MetalSharpApp>().is_none())
                {
                    cx.quit();
                }
            })
            .detach();
            let display_size = cx.primary_display().map(|display| {
                let size = display.bounds().size;
                (f32::from(size.width), f32::from(size.height))
            });
            let (width, height) = initial_window_size(display_size, preview_override);
            let bounds = Bounds::centered(None, size(px(width), px(height)), cx);
            let options = WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                window_min_size: Some(size(px(720.0), px(540.0))),
                titlebar: Some(gpui::TitlebarOptions {
                    title: Some("MetalSharp".into()),
                    ..Default::default()
                }),
                ..Default::default()
            };
            crate::toast::install(cx);
            crate::artwork::install(cx);
            if let Some(config) = connected {
                crate::hotkeys::register();
                crate::process_manager::install(cx);
                cx.open_window(options, move |window, cx| {
                    cx.new(|cx| ui::MetalSharpApp::new_live(config, window, cx))
                })
                .expect("failed to open MetalSharp");
            } else {
                cx.open_window(options, move |window, cx| {
                    if preview_bounds.is_some() {
                        let (requested_width, requested_height) = (width, height);
                        window.on_next_frame(move |window, _| {
                            let viewport = window.viewport_size();
                            let width = f32::from(viewport.width);
                            let height = f32::from(viewport.height);
                            let scale = window.scale_factor();
                            println!(
                                "METALSHARP_PREVIEW_VIEWPORT {}",
                                serde_json::json!({
                                    "requested_logical_width": requested_width,
                                    "requested_logical_height": requested_height,
                                    "logical_width": width,
                                    "logical_height": height,
                                    "scale_factor": scale,
                                    "device_width": width * scale,
                                    "device_height": height * scale,
                                })
                            );
                        });
                    }
                    cx.new(|_| ui::MetalSharpApp::new())
                })
                .expect("failed to open GPUI offline preview");
            }
            cx.activate(true);
        });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{initial_window_size, preview_size};

    #[test]
    fn initial_window_leaves_desktop_space_at_logical_display_sizes() {
        for (display, expected) in [
            ((1920.0, 1080.0), (1440.0, 864.0)),
            ((2560.0, 1440.0), (1920.0, 1152.0)),
            ((3840.0, 2160.0), (2800.0, 1700.0)),
            ((7680.0, 4320.0), (2800.0, 1700.0)),
            ((800.0, 600.0), (720.0, 540.0)),
        ] {
            let size = initial_window_size(Some(display), None);
            assert_eq!(size, expected);
            assert!(size.0 < display.0 && size.1 < display.1);
        }
        // A 4K display at 2× exposes the same logical bounds as Full HD at 1×.
        assert_eq!(
            initial_window_size(Some((3840.0 / 2.0, 2160.0 / 2.0)), None),
            (1440.0, 864.0)
        );
        assert_eq!(initial_window_size(None, None), (1360.0, 860.0));
        assert_eq!(
            initial_window_size(Some((f32::NAN, 1080.0)), None),
            (1360.0, 860.0)
        );
    }

    #[test]
    fn explicit_preview_size_bypasses_adaptive_window_caps() {
        let requested = preview_size("--preview=3840x2160").unwrap();
        assert_eq!(
            initial_window_size(Some((1920.0, 1080.0)), requested),
            (3840.0, 2160.0)
        );
    }

    #[test]
    fn preview_dimensions_accept_defaults_and_supported_viewports() {
        for (argument, expected) in [
            ("--preview", (1360.0, 860.0)),
            ("--preview=720x540", (720.0, 540.0)),
            ("--preview=1920x1080", (1920.0, 1080.0)),
            ("--preview=2560x1440", (2560.0, 1440.0)),
            ("--preview=3840x2160", (3840.0, 2160.0)),
        ] {
            assert_eq!(preview_size(argument).unwrap(), Some(expected));
        }
        for argument in [
            "--connected-validation",
            "--connected-production",
            "-psn_0_1",
        ] {
            assert_eq!(preview_size(argument).unwrap(), None);
        }
    }

    #[test]
    fn preview_dimensions_reject_invalid_or_unbounded_sizes() {
        for argument in [
            "--preview=",
            "--preview=1920",
            "--preview=x1080",
            "--preview=1920x",
            "--preview=1920x1080x2",
            "--preview=719x1080",
            "--preview=3841x1080",
            "--preview=1920x539",
            "--preview=1920x2161",
            "--preview=NaNx1080",
            "--preview=1920xNaN",
            "--preview=infx1080",
            "--preview=1920xinf",
            "--preview=1e40x1080",
        ] {
            assert!(preview_size(argument).is_err(), "accepted {argument}");
        }
    }
}
