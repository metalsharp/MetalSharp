// Backend spike scaffolding is type-checked/tested, never linked into the offline preview.
#[cfg(test)]
#[allow(dead_code)]
mod backend;
#[cfg(test)]
#[allow(dead_code)]
mod backend_host;
mod logs_preview;
#[allow(dead_code)]
mod mini_browser;
mod page_palette;
mod search_input;
mod settings_preview;
mod sharp_preview;
mod ui;

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
    Application::new()
        .with_assets(PreviewIcons)
        .run(|cx: &mut App| {
            search_input::register_keys(cx);
            cx.text_system()
                .add_fonts(vec![
                    Cow::Borrowed(include_bytes!("../assets/RethinkSans-Regular.ttf")),
                    Cow::Borrowed(include_bytes!("../assets/RethinkSans-SemiBold.ttf")),
                ])
                .expect("failed to register MetalSharp setup fonts");

            let bounds = Bounds::centered(None, size(px(1360.0), px(860.0)), cx);
            cx.open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Windowed(bounds)),
                    window_min_size: Some(size(px(720.0), px(540.0))),
                    titlebar: Some(gpui::TitlebarOptions {
                        title: Some("MetalSharp".into()),
                        ..Default::default()
                    }),
                    ..Default::default()
                },
                |_, cx| cx.new(|_| ui::MetalSharpApp::new()),
            )
            .expect("failed to open MetalSharp GPUI preview");
            cx.activate(true);
        });
    Ok(())
}
