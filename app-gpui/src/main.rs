#[allow(dead_code)]
mod artwork;
#[allow(dead_code)]
mod backend;
mod backend_host;
#[allow(dead_code)]
mod configuration;
mod diagnostics;
mod host_actions;
mod hotkeys;
mod launch_overlay;
mod library_model;
mod lifecycle;
mod live;
mod logs_preview;
#[allow(dead_code)]
mod mini_browser;
mod page_palette;
mod search_input;
mod settings_preview;
mod sharp_preview;
#[allow(dead_code)]
mod streaming;
mod toast;
mod ui;
mod updater_bridge;

use anyhow::Result;
use gpui::{App, AppContext, Application, Bounds, WindowBounds, WindowOptions, px, size};
use std::borrow::Cow;

struct PreviewIcons;

impl gpui::AssetSource for PreviewIcons {
    fn load(&self, path: &str) -> Result<Option<Cow<'static, [u8]>>> {
        Ok(match path {
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
            _ => None,
        })
    }

    fn list(&self, _path: &str) -> Result<Vec<gpui::SharedString>> {
        Ok(vec![
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
        ])
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
    // Default launch is the production app (port 9274, ~/.metalsharp), like Electron.
    let connected = match args.as_slice() {
        [] => Some(backend_host::HostConfig::from_environment(false)?),
        [mode] if mode == "--preview" => None,
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
            "Use no arguments for MetalSharp, --preview for the offline sample UI, or --connected-validation for an isolated data home"
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
            let bounds = Bounds::centered(None, size(px(1360.0), px(860.0)), cx);
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
                cx.open_window(options, move |window, cx| {
                    cx.new(|cx| ui::MetalSharpApp::new_live(config, window, cx))
                })
                .expect("failed to open MetalSharp");
            } else {
                cx.open_window(options, |_, cx| cx.new(|_| ui::MetalSharpApp::new()))
                    .expect("failed to open GPUI offline preview");
            }
            cx.activate(true);
        });
    Ok(())
}
