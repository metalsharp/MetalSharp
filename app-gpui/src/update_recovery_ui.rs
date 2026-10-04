//! Renderable connected updater + migration recovery surface. Parent owns Root composition
//! and translates these typed intents into its single-threaded serial backend dispatcher.
use crate::{
    migration_connected::{MigrationProgress, MigrationStatus},
    updater_connected::{UpdatePhase, UpdateSnapshot},
};
use gpui::{Context, Render, Window, div, prelude::*, px, rgb};

#[derive(Clone, Debug, PartialEq)]
pub enum RecoveryIntent {
    StartUpdate(crate::updater_connected::UpdateVariant),
    PollUpdateDownload,
    PollUpdateInstall,
    ClearUpdateStatus,
    CheckMigration,
    StartMigration,
    PollMigration,
    DetachMigrationPolling,
    RestartAfterMigration,
}
#[derive(Clone, Debug, Default)]
pub struct RecoverySnapshot {
    pub update: Option<UpdateSnapshot>,
    pub migration_status: MigrationStatus,
    pub migration: MigrationProgress,
    pub notice: String,
    pub busy: bool,
}
pub struct RecoveryPanel {
    pub snapshot: RecoverySnapshot,
    pub emit: Box<dyn FnMut(RecoveryIntent)>,
    pending_migration_confirmation: bool,
}
impl RecoveryPanel {
    pub fn set_busy(&mut self, busy: bool, cx: &mut Context<Self>) {
        self.snapshot.busy = busy;
        cx.notify();
    }
    pub fn apply_snapshot(&mut self, snapshot: RecoverySnapshot, cx: &mut Context<Self>) {
        self.snapshot = snapshot;
        if self.snapshot.migration_status != MigrationStatus::Idle {
            self.pending_migration_confirmation = false;
        }
        cx.notify();
    }
    pub fn new(snapshot: RecoverySnapshot, emit: impl FnMut(RecoveryIntent) + 'static) -> Self {
        Self {
            snapshot,
            emit: Box::new(emit),
            pending_migration_confirmation: false,
        }
    }
    fn action(&mut self, intent: RecoveryIntent) {
        if self.snapshot.busy {
            return;
        }
        if intent == RecoveryIntent::StartMigration && !self.pending_migration_confirmation {
            self.pending_migration_confirmation = true;
            return;
        }
        if intent == RecoveryIntent::StartMigration {
            self.pending_migration_confirmation = false;
        }
        (self.emit)(intent);
    }
}
impl Render for RecoveryPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let snap = self.snapshot.clone();
        let migration_confirmation = self.pending_migration_confirmation;
        let update = snap.update.clone();
        let mut card = div()
            .w(px(560.))
            .flex()
            .flex_col()
            .gap(px(15.))
            .rounded(px(16.))
            .border_1()
            .border_color(rgb(0x353b40))
            .bg(rgb(0x151a1f))
            .p(px(28.))
            .child(div().text_size(px(22.)).child("MetalSharp recovery"));
        if let Some(u) = update.clone() {
            let phase = match u.phase {
                UpdatePhase::Idle => "Ready",
                UpdatePhase::Downloading => "Downloading update",
                UpdatePhase::Downloaded => "Download complete",
                UpdatePhase::Installing => "Installing update",
                UpdatePhase::Complete => "Update installed",
                UpdatePhase::Error => "Update failed",
            };
            card = card
                .child(div().child(format!("{} · {}%", phase, u.percent)))
                .child(
                    div()
                        .h(px(8.))
                        .w_full()
                        .rounded_full()
                        .bg(rgb(0x30373c))
                        .child(
                            div()
                                .h(px(8.))
                                .w(px(5.6 * u.percent as f32))
                                .rounded_full()
                                .bg(rgb(if u.phase == UpdatePhase::Error {
                                    0xd87567
                                } else {
                                    0x74d2c8
                                })),
                        ),
                )
                .child(div().child(u.message));
            if let Some(error) = u.error {
                card = card.child(div().text_color(rgb(0xe88b7e)).child(error));
            }
        }
        card = card
            .child(div().text_size(px(18.)).child("Migration status"))
            .child(div().child(format!(
                "{:?} · {}%",
                snap.migration_status,
                snap.migration.percent()
            )))
            .child(
                div()
                    .h(px(8.))
                    .w_full()
                    .rounded_full()
                    .bg(rgb(0x30373c))
                    .child(
                        div()
                            .h(px(8.))
                            .w(px(5.6 * snap.migration.percent() as f32))
                            .rounded_full()
                            .bg(rgb(0x74d2c8)),
                    ),
            )
            .child(div().child(snap.migration.message.clone()));
        if let Some(error) = snap.migration.error.clone() {
            card = card.child(div().text_color(rgb(0xe88b7e)).child(error));
        }
        card = card.child(div().text_color(rgb(0x9aa09e)).child(snap.notice.clone()));
        let button = |id, label, enabled: bool| {
            div()
                .id(id)
                .px(px(14.))
                .py(px(9.))
                .rounded(px(7.))
                .bg(rgb(if enabled { 0x293338 } else { 0x1d2225 }))
                .text_color(rgb(if enabled { 0xf2efe6 } else { 0x777d7b }))
                .cursor_pointer()
                .child(label)
        };
        let mut actions = div().flex().flex_wrap().gap(px(9.));
        if let Some(u) = update {
            let v = u.variant;
            actions = actions
                .child(
                    button("update-start", "Download update", !snap.busy).on_click(cx.listener(
                        move |this, _, _, _| this.action(RecoveryIntent::StartUpdate(v)),
                    )),
                )
                .child(
                    button("update-download-poll", "Refresh download", !snap.busy).on_click(
                        cx.listener(|this, _, _, _| {
                            this.action(RecoveryIntent::PollUpdateDownload)
                        }),
                    ),
                )
                .child(
                    button("update-install-poll", "Refresh installer", !snap.busy).on_click(
                        cx.listener(|this, _, _, _| this.action(RecoveryIntent::PollUpdateInstall)),
                    ),
                )
                .child(
                    button("update-clear", "Clear update status", !snap.busy).on_click(
                        cx.listener(|this, _, _, _| this.action(RecoveryIntent::ClearUpdateStatus)),
                    ),
                );
        }
        if migration_confirmation {
            card=card.child(div().text_color(rgb(0xe8d6b7)).child("Migration changes the existing user data home. Click Confirm migration only after reviewing the backup and rollback plan."));
        }
        actions = actions
            .child(
                button("migration-check", "Check migration", !snap.busy).on_click(
                    cx.listener(|this, _, _, _| this.action(RecoveryIntent::CheckMigration)),
                ),
            )
            .child(
                button(
                    "migration-start",
                    if migration_confirmation {
                        "Confirm migration"
                    } else {
                        "Review migration"
                    },
                    !snap.busy,
                )
                .on_click(cx.listener(|this, _, _, _| this.action(RecoveryIntent::StartMigration))),
            )
            .child(
                button("migration-poll", "Refresh migration", !snap.busy).on_click(
                    cx.listener(|this, _, _, _| this.action(RecoveryIntent::PollMigration)),
                ),
            )
            .child(
                button("migration-detach", "Stop status polling", !snap.busy).on_click(
                    cx.listener(|this, _, _, _| {
                        this.action(RecoveryIntent::DetachMigrationPolling)
                    }),
                ),
            )
            .child(
                button(
                    "migration-restart",
                    "Restart after confirmed migration",
                    !snap.busy && snap.migration_status == MigrationStatus::Complete,
                )
                .on_click(
                    cx.listener(|this, _, _, _| this.action(RecoveryIntent::RestartAfterMigration)),
                ),
            );
        card = card.child(actions);
        div()
            .id("update-migration-recovery")
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .overflow_y_scroll()
            .bg(rgb(0x0e1218))
            .text_color(rgb(0xf2efe6))
            .font_family("Rethink Sans")
            .child(card)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    #[test]
    fn migration_start_requires_two_explicit_activations() {
        let intents = Arc::new(Mutex::new(Vec::new()));
        let captured = intents.clone();
        let mut panel = RecoveryPanel::new(RecoverySnapshot::default(), move |i| {
            captured.lock().unwrap().push(i)
        });
        panel.action(RecoveryIntent::StartMigration);
        assert!(intents.lock().unwrap().is_empty());
        panel.action(RecoveryIntent::StartMigration);
        assert_eq!(
            *intents.lock().unwrap(),
            vec![RecoveryIntent::StartMigration]
        );
    }
    #[test]
    fn connected_panel_intent_snapshot_lifecycle_fixture() {
        let intents = Arc::new(Mutex::new(Vec::new()));
        let captured = intents.clone();
        let mut panel = RecoveryPanel::new(RecoverySnapshot::default(), move |i| {
            captured.lock().unwrap().push(i)
        });
        panel.action(RecoveryIntent::CheckMigration);
        panel.snapshot.migration_status = MigrationStatus::Complete;
        panel.snapshot.migration = MigrationProgress {
            status: "complete".into(),
            step: 4,
            total: 4,
            message: "migration finished".into(),
            error: None,
        };
        panel.action(RecoveryIntent::RestartAfterMigration);
        assert_eq!(panel.snapshot.migration.percent(), 100);
        assert_eq!(
            *intents.lock().unwrap(),
            vec![
                RecoveryIntent::CheckMigration,
                RecoveryIntent::RestartAfterMigration
            ]
        );
    }
}
