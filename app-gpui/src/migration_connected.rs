//! Typed connected migration orchestration matching the existing C backend routes.
//! Migration itself is backend-owned and cannot be canceled; cancellation only detaches UI polling.
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Deserialize, Default, Eq, PartialEq)]
pub struct MigrationCheck {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub needed: bool,
    #[serde(default)]
    pub reason: String,
    pub current_version: Option<String>,
    pub target_version: Option<String>,
    pub current_schema: Option<u64>,
    pub target_schema: Option<u64>,
    pub post_update_target_version: Option<String>,
    pub running_version: Option<String>,
    pub update_target_satisfied: Option<bool>,
}
#[derive(Clone, Copy, Debug, Default, Deserialize, Serialize, Eq, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum MigrationStatus {
    #[default]
    Idle,
    Starting,
    Running,
    Complete,
    Error,
    Detached,
}
#[derive(Clone, Debug, Deserialize, Serialize, Default, Eq, PartialEq)]
pub struct MigrationProgress {
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub step: usize,
    #[serde(default)]
    pub total: usize,
    #[serde(default)]
    pub message: String,
    pub error: Option<String>,
}
impl MigrationProgress {
    pub fn percent(&self) -> u8 {
        if self.total == 0 {
            if self.status == "complete" { 100 } else { 0 }
        } else {
            self.step
                .saturating_mul(100)
                .checked_div(self.total)
                .unwrap_or(0)
                .min(100) as u8
        }
    }
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MigrationError {
    Backend(String),
    UpdateTargetMismatch {
        target: Option<String>,
        running: Option<String>,
    },
    InvalidProgress,
}
impl std::fmt::Display for MigrationError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}
impl std::error::Error for MigrationError {}

pub trait MigrationApi {
    fn migration_check(&self) -> Result<MigrationCheck, String>;
    fn migration_start(&self) -> Result<(), String>;
    fn migration_progress(&self) -> Result<MigrationProgress, String>;
}
impl MigrationApi for crate::backend::BackendClient {
    fn migration_check(&self) -> Result<MigrationCheck, String> {
        self.get("/update/migrate/check").map_err(|e| e.to_string())
    }
    fn migration_start(&self) -> Result<(), String> {
        self.post("/update/migrate/start", serde_json::json!({}))
            .map(|_| ())
            .map_err(|e| e.to_string())
    }
    fn migration_progress(&self) -> Result<MigrationProgress, String> {
        self.get("/update/migrate/progress")
            .map_err(|e| e.to_string())
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct MigrationSession {
    pub status: MigrationStatus,
    pub progress: MigrationProgress,
    pub error: Option<String>,
    polls: u32,
}
impl Default for MigrationSession {
    fn default() -> Self {
        Self {
            status: MigrationStatus::Idle,
            progress: MigrationProgress::default(),
            error: None,
            polls: 0,
        }
    }
}
impl MigrationSession {
    pub fn start(api: &impl MigrationApi) -> Result<Self, MigrationError> {
        let check = api.migration_check().map_err(MigrationError::Backend)?;
        if check.update_target_satisfied == Some(false) {
            return Err(MigrationError::UpdateTargetMismatch {
                target: check.post_update_target_version,
                running: check.running_version,
            });
        }
        let mut s = Self::default();
        if !check.needed {
            s.status = MigrationStatus::Complete;
            s.progress = MigrationProgress {
                status: "complete".into(),
                step: 1,
                total: 1,
                message: "No migration required".into(),
                error: None,
            };
            return Ok(s);
        }
        s.status = MigrationStatus::Starting;
        match api.migration_start() {
            Ok(()) => s.status = MigrationStatus::Running,
            Err(e) if e.to_ascii_lowercase().contains("already in progress") => {
                s.status = MigrationStatus::Running
            }
            Err(e) => return Err(MigrationError::Backend(e)),
        };
        Ok(s)
    }
    /// One bounded poll. Terminal error text comes from the backend contract; callers should
    /// show it as status text, not treat it as a filesystem path or execute it.
    pub fn poll(&mut self, api: &impl MigrationApi) -> Result<&MigrationProgress, MigrationError> {
        if self.status != MigrationStatus::Running {
            return Err(MigrationError::InvalidProgress);
        }
        let p = api.migration_progress().map_err(MigrationError::Backend)?;
        if p.step > p.total && p.total > 0 {
            return Err(MigrationError::InvalidProgress);
        }
        self.polls = self.polls.saturating_add(1);
        match p.status.as_str() {
            "complete" => self.status = MigrationStatus::Complete,
            "error" => {
                self.status = MigrationStatus::Error;
                self.error = p.error.clone().or_else(|| Some(p.message.clone()));
            }
            "running" | "idle" | "" => {}
            _ => return Err(MigrationError::InvalidProgress),
        }
        self.progress = p;
        Ok(&self.progress)
    }
    /// The C backend provides no cancellation route: this explicitly stops further UI polling,
    /// but never claims the migration worker has stopped.
    pub fn detach_polling(&mut self) {
        if self.status == MigrationStatus::Running {
            self.status = MigrationStatus::Detached
        }
    }
    pub fn poll_count(&self) -> u32 {
        self.polls
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    struct Api {
        check: MigrationCheck,
        start: Result<(), String>,
        progress: RefCell<Vec<Result<MigrationProgress, String>>>,
    }
    impl MigrationApi for Api {
        fn migration_check(&self) -> Result<MigrationCheck, String> {
            Ok(self.check.clone())
        }
        fn migration_start(&self) -> Result<(), String> {
            self.start.clone()
        }
        fn migration_progress(&self) -> Result<MigrationProgress, String> {
            self.progress.borrow_mut().remove(0)
        }
    }
    fn api() -> Api {
        Api {
            check: MigrationCheck {
                ok: true,
                needed: true,
                reason: "post_update_marker".into(),
                current_version: Some("0.75.0".into()),
                target_version: Some("0.76.0".into()),
                current_schema: Some(0),
                target_schema: Some(1),
                post_update_target_version: None,
                running_version: Some("0.76.0".into()),
                update_target_satisfied: Some(true),
            },
            start: Ok(()),
            progress: RefCell::new(vec![
                Ok(MigrationProgress {
                    status: "running".into(),
                    step: 2,
                    total: 8,
                    message: "Preserving settings".into(),
                    error: None,
                }),
                Ok(MigrationProgress {
                    status: "error".into(),
                    step: 4,
                    total: 8,
                    message: "failed".into(),
                    error: Some("fixture failure".into()),
                }),
            ]),
        }
    }
    #[test]
    fn migration_start_progress_error_and_detach_are_explicit() {
        let a = api();
        let mut s = MigrationSession::start(&a).unwrap();
        assert_eq!(s.status, MigrationStatus::Running);
        assert_eq!(s.poll(&a).unwrap().percent(), 25);
        assert_eq!(
            s.poll(&a).unwrap().error.as_deref(),
            Some("fixture failure")
        );
        assert_eq!(s.status, MigrationStatus::Error);
        let mut s = MigrationSession {
            status: MigrationStatus::Running,
            ..Default::default()
        };
        s.detach_polling();
        assert_eq!(s.status, MigrationStatus::Detached);
        assert_eq!(s.poll(&api()).unwrap_err(), MigrationError::InvalidProgress)
    }
    #[test]
    fn already_running_and_not_needed_contracts() {
        let mut a = api();
        a.start = Err("migration already in progress".into());
        assert_eq!(
            MigrationSession::start(&a).unwrap().status,
            MigrationStatus::Running
        );
        a.check.needed = false;
        assert_eq!(
            MigrationSession::start(&a).unwrap().status,
            MigrationStatus::Complete
        )
    }
    #[test]
    fn mismatched_post_update_target_never_starts_migration() {
        let mut a = api();
        a.check.update_target_satisfied = Some(false);
        a.check.post_update_target_version = Some("0.77.0".into());
        a.check.running_version = Some("0.76.0".into());
        assert_eq!(
            MigrationSession::start(&a).unwrap_err(),
            MigrationError::UpdateTargetMismatch {
                target: Some("0.77.0".into()),
                running: Some("0.76.0".into())
            }
        );
    }
}
