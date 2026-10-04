//! Window-scoped install completion intent, not an installer or permission grant.
//! A closed/replaced window cannot act on an old HTTP completion.
use crate::streaming::StreamingStatus;
#[derive(Default)]
pub(crate) struct StreamingWatch {
    generation: u64,
    open: bool,
    awaiting_install: bool,
}
impl StreamingWatch {
    pub fn open(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.open = true;
        self.awaiting_install = false;
    }
    pub fn close(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.open = false;
        self.awaiting_install = false;
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    pub fn generation(&self) -> u64 {
        self.generation
    }
    pub fn accepts(&self, generation: u64) -> bool {
        self.open && self.generation == generation
    }
    pub fn accepted_install(&mut self, generation: u64) {
        if self.accepts(generation) {
            self.awaiting_install = true;
        }
    }
    pub fn failed(&mut self, generation: u64) {
        if self.accepts(generation) {
            self.awaiting_install = false;
        }
    }
    pub fn observe(&mut self, generation: u64, status: &StreamingStatus) -> bool {
        if !self.accepts(generation) || !self.awaiting_install || status.installing {
            return false;
        }
        match status.progress_status.as_deref() {
            Some("complete") => {
                self.awaiting_install = false;
                status.installed && !status.running
            }
            Some("error") => {
                self.awaiting_install = false;
                false
            }
            _ => false,
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn complete() -> StreamingStatus {
        StreamingStatus {
            installed: true,
            progress_status: Some("complete".into()),
            ..Default::default()
        }
    }
    #[test]
    fn completion_only_launches_once_after_explicit_accepted_install() {
        let mut watch = StreamingWatch::default();
        watch.open();
        let generation = watch.generation();
        assert!(!watch.observe(generation, &complete()));
        watch.accepted_install(generation);
        assert!(!watch.observe(
            generation,
            &StreamingStatus {
                installing: true,
                ..complete()
            }
        ));
        assert!(watch.observe(generation, &complete()));
        assert!(!watch.observe(generation, &complete()));
    }
    #[test]
    fn closing_or_reopening_cancels_inflight_intent_and_stale_responses() {
        let mut watch = StreamingWatch::default();
        watch.open();
        let old = watch.generation();
        watch.accepted_install(old);
        watch.close();
        assert!(!watch.observe(old, &complete()));
        watch.open();
        watch.accepted_install(old);
        assert!(!watch.accepts(old));
        assert!(!watch.observe(watch.generation(), &complete()));
    }
    #[test]
    fn failed_unknown_or_already_running_state_never_retries_mutation() {
        let mut watch = StreamingWatch::default();
        watch.open();
        let generation = watch.generation();
        watch.accepted_install(generation);
        watch.failed(generation);
        assert!(!watch.observe(generation, &complete()));
        watch.accepted_install(generation);
        assert!(!watch.observe(
            generation,
            &StreamingStatus {
                running: true,
                ..complete()
            }
        ));
        watch.accepted_install(generation);
        assert!(!watch.observe(
            generation,
            &StreamingStatus {
                progress_status: Some("error".into()),
                ..complete()
            }
        ));
        assert!(!watch.observe(generation, &complete()));
    }
}
