//! Where MetalSharp keeps its data (crate::data_location), decided before the backend starts: a first launch with an
//! external drive connected asks this Mac or the drive, a data folder whose drive is not connected waits for it,
//! and Settings' Move runs here, with the backend stopped while its data is copied.
use super::*;
use crate::data_location::{self, Drive, Location, Target};
use crate::live::Live;
use crate::toast;
use std::sync::{Arc, Mutex};

#[derive(Clone, Debug)]
pub(super) enum DataPrompt {
    /// A first launch: this Mac or one of these drives.
    Choose(Vec<Drive>),
    /// The data folder's drive is not connected.
    Disconnected(String),
    /// Setting up the chosen drive failed.
    Failed(String),
}

#[derive(Clone, Debug, Default)]
pub(super) struct DataMove {
    pub destination: String,
    pub copied: u64,
    pub total: u64,
    pub stage: String,
}

const BUTTON_BG: u32 = 0xefe7d6;
const BUTTON_TEXT: u32 = 0x14161a;

impl MetalSharpApp {
    /// Start the backend once the data folder is where it should be, or ask first (a first launch with a drive to
    /// offer, or a data folder whose drive is not connected).
    pub(super) fn boot_or_ask_data_location(&mut self, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        if live.validation() {
            self.boot_backend(true, cx);
            return;
        }
        let home = live.home();
        let prompt = match data_location::location(&home) {
            Location::ThisMac | Location::Drive { .. } => None,
            Location::Missing => {
                let drives = data_location::setup_drives();
                (!drives.is_empty()).then_some(DataPrompt::Choose(drives))
            }
            Location::Disconnected { name, .. } => {
                (!data_location::reconnect(&home)).then_some(DataPrompt::Disconnected(name))
            }
        };
        match prompt {
            Some(prompt) => {
                self.live.booting = false;
                self.live.data_prompt = Some(prompt);
                cx.notify();
            }
            None => self.boot_backend(true, cx),
        }
    }

    /// Start the owned backend; `retry_once` tries a second time as a launch does (Electron index.ts:1373-1384).
    pub(super) fn boot_backend(&mut self, retry_once: bool, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        self.live.data_prompt = None;
        self.live.boot_error = None;
        self.live.booting = true;
        cx.notify();
        cx.spawn(async move |this, cx| {
            let started = cx
                .background_executor()
                .spawn(async move {
                    let first = live.start_backend();
                    if retry_once {
                        first.or_else(|_| live.start_backend())
                    } else {
                        first
                    }
                })
                .await;
            let _ = this.update(cx, |this, cx| match started {
                Ok(()) => this.after_backend_started(cx),
                Err(error) => {
                    this.live.booting = false;
                    this.live.boot_error = Some(error);
                    cx.notify();
                }
            });
        })
        .detach();
    }

    /// The first launch's choice: `None` is this Mac.
    fn choose_data_location(&mut self, drive: Option<Drive>, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        let Some(drive) = drive else {
            self.boot_backend(true, cx);
            return;
        };
        self.live.data_prompt = None;
        self.live.booting = true;
        cx.notify();
        let home = live.home();
        cx.spawn(async move |this, cx| {
            // the first touch of the drive can wait on macOS asking for access to it
            let made = cx
                .background_executor()
                .spawn(async move { data_location::use_drive(&home, &drive) })
                .await;
            let _ = this.update(cx, |this, cx| match made {
                Ok(()) => this.boot_backend(true, cx),
                Err(error) => {
                    this.live.booting = false;
                    this.live.data_prompt = Some(DataPrompt::Failed(format!("{error:#}")));
                    cx.notify();
                }
            });
        })
        .detach();
    }

    /// Settings' Move, after its confirmation: stop MetalSharp's Wine and the backend, move the data, start again.
    pub(super) fn start_data_move(&mut self, target: Target, cx: &mut Context<Self>) {
        let Some(live) = Live::get(cx) else { return };
        if self.live.data_move.is_some() {
            return;
        }
        self.settings_open = false;
        let destination = match &target {
            Target::ThisMac => "this Mac".to_owned(),
            Target::Drive(drive) => drive.name.clone(),
        };
        self.live.data_move = Some(DataMove {
            destination: destination.clone(),
            stage: "Closing Steam and games…".into(),
            ..Default::default()
        });
        cx.notify();
        let shared = Arc::new(Mutex::new((0u64, 0u64)));
        let progress = shared.clone();
        let home = live.home();
        let job = cx.background_executor().spawn(async move {
            live.stop_wine();
            live.stop_backend();
            data_location::move_data(&home, &target, &|copied, total| {
                *progress.lock().unwrap() = (copied, total);
            })
        });
        cx.spawn(async move |this, cx| {
            let mut job = std::pin::pin!(job);
            let result = loop {
                let tick = cx
                    .background_executor()
                    .timer(std::time::Duration::from_millis(500));
                match futures::future::select(job.as_mut(), std::pin::pin!(tick)).await {
                    futures::future::Either::Left((result, _)) => break result,
                    futures::future::Either::Right(_) => {
                        let (copied, total) = *shared.lock().unwrap();
                        let alive = this.update(cx, |this, cx| {
                            if let Some(moving) = this.live.data_move.as_mut() {
                                if total > 0 {
                                    moving.stage = "Copying…".into();
                                }
                                moving.copied = copied;
                                moving.total = total;
                            }
                            cx.notify();
                        });
                        if alive.is_err() {
                            return;
                        }
                    }
                }
            };
            let _ = this.update(cx, |this, cx| {
                this.live.data_move = None;
                match result {
                    Ok(()) => {
                        toast::success(cx, format!("MetalSharp's data is now on {destination}"))
                    }
                    Err(error) => {
                        toast::error(cx, format!("MetalSharp's data was not moved: {error:#}"))
                    }
                }
                this.boot_backend(true, cx);
            });
        })
        .detach();
    }

    pub(super) fn render_data_prompt(
        &self,
        prompt: DataPrompt,
        cx: &mut Context<Self>,
    ) -> gpui::Div {
        let (title, detail, buttons): (String, String, Vec<gpui::Stateful<gpui::Div>>) =
            match prompt {
                DataPrompt::Choose(drives) => {
                    let mut buttons = vec![
                        data_button("data-this-mac", "Keep it on this Mac".into(), true).on_click(
                            cx.listener(|this, _, _, cx| this.choose_data_location(None, cx)),
                        ),
                    ];
                    for (index, drive) in drives.into_iter().enumerate() {
                        let label = if drive.has_data {
                            format!("Use the data on {}", drive.name)
                        } else {
                            format!(
                                "Use {} ({} free)",
                                drive.name,
                                data_location::format_size(drive.free)
                            )
                        };
                        buttons.push(data_button(("data-drive", index), label, false).on_click(
                            cx.listener(move |this, _, _, cx| {
                                this.choose_data_location(Some(drive.clone()), cx)
                            }),
                        ));
                    }
                    (
                        "Where should MetalSharp keep its data?".into(),
                        format!(
                            "MetalSharp's Wine runtime, the Steam client and game prefixes, and shader caches need {} or more, and grow with the games you play. Keep them on this Mac, or in a {} folder on an external drive, which needs to be connected whenever you use MetalSharp. You can move them later in Settings.",
                            data_location::format_size(data_location::SETUP_MIN_FREE),
                            data_location::DRIVE_FOLDER
                        ),
                        buttons,
                    )
                }
                DataPrompt::Disconnected(name) => (
                    format!("Connect {name}"),
                    format!(
                        "MetalSharp keeps its data on {name}, which isn't connected. Connect the drive, then try again."
                    ),
                    vec![
                        data_button("data-retry", "Try Again".into(), true).on_click(
                            cx.listener(|this, _, _, cx| this.boot_or_ask_data_location(cx)),
                        ),
                    ],
                ),
                DataPrompt::Failed(error) => (
                    "MetalSharp couldn't use that drive".into(),
                    error,
                    vec![
                        data_button("data-back", "Choose Again".into(), true).on_click(
                            cx.listener(|this, _, _, cx| this.boot_or_ask_data_location(cx)),
                        ),
                        data_button("data-this-mac", "Keep it on this Mac".into(), false).on_click(
                            cx.listener(|this, _, _, cx| this.choose_data_location(None, cx)),
                        ),
                    ],
                ),
            };
        status_page(title, detail).child(
            div()
                .mt(px(6.0))
                .flex()
                .flex_wrap()
                .justify_center()
                .gap(px(10.0))
                .children(buttons),
        )
    }

    pub(super) fn render_data_move(&self, moving: &DataMove) -> gpui::Div {
        let fraction = if moving.total > 0 {
            (moving.copied as f32 / moving.total as f32).clamp(0.0, 1.0)
        } else {
            0.0
        };
        let detail = if moving.total > 0 {
            format!(
                "{} — {} of {}. Keep MetalSharp open and the drive connected until it's done.",
                moving.stage,
                data_location::format_size(moving.copied),
                data_location::format_size(moving.total)
            )
        } else {
            moving.stage.clone()
        };
        status_page(
            format!("Moving MetalSharp's data to {}", moving.destination),
            detail,
        )
        .child(
            div()
                .mt(px(6.0))
                .w(px(360.0))
                .h(px(8.0))
                .rounded(px(999.0))
                .bg(rgb(0x292b2d))
                .child(
                    div()
                        .h(px(8.0))
                        .w(relative(fraction))
                        .rounded(px(999.0))
                        .bg(rgb(BUTTON_BG)),
                ),
        )
    }
}

fn status_page(title: String, detail: String) -> gpui::Div {
    div()
        .size_full()
        .flex()
        .flex_col()
        .items_center()
        .justify_center()
        .gap(px(14.0))
        .bg(rgb(PAGE_BG))
        .font_family("Rethink Sans")
        .text_color(rgb(TEXT))
        .child(
            img(asset_path("metalsharp-logo.png"))
                .size(px(72.0))
                .object_fit(ObjectFit::Contain),
        )
        .child(
            div()
                .text_size(px(20.0))
                .font_weight(FontWeight::SEMIBOLD)
                .child(title),
        )
        .child(
            div()
                .max_w(px(560.0))
                .text_center()
                .text_size(px(13.0))
                .line_height(px(19.0))
                .text_color(rgb(MUTED))
                .child(detail),
        )
}

fn data_button(
    id: impl Into<gpui::ElementId>,
    label: String,
    primary: bool,
) -> gpui::Stateful<gpui::Div> {
    div()
        .id(id)
        .h(px(40.0))
        .px(px(22.0))
        .flex()
        .items_center()
        .rounded(px(8.0))
        .border_1()
        .border_color(rgb(if primary { BUTTON_BG } else { 0x3a3d40 }))
        .bg(rgb(if primary { BUTTON_BG } else { 0x1b1d20 }))
        .text_color(rgb(if primary { BUTTON_TEXT } else { TEXT }))
        .font_weight(FontWeight::BOLD)
        .cursor_pointer()
        .child(label)
}
