//! Typed connected-mode controller for Sharp Library and its eight source surfaces.
//!
//! This is deliberately a backend/API layer, not a second backend implementation. It
//! calls the authoritative C routes through `BackendClient`; callers own explicit-mode
//! gating, dialogs/native pickers, browser handoff and rendering.
use crate::backend::{BackendClient, BackendError};
use gpui::{InteractiveElement, prelude::*};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

#[derive(Clone, Copy, Debug, Deserialize, Serialize, Eq, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum SharpSource {
    Installers,
    Gog,
    Epic,
    GameJolt,
    Pcsx2,
    Rpcs3,
    ShadPs4,
    SharpEmu,
}
impl SharpSource {
    pub const ALL: [Self; 8] = [
        Self::Installers,
        Self::Gog,
        Self::Epic,
        Self::GameJolt,
        Self::Pcsx2,
        Self::Rpcs3,
        Self::ShadPs4,
        Self::SharpEmu,
    ];
    pub const fn id(self) -> &'static str {
        match self {
            Self::Installers => "installers",
            Self::Gog => "gog",
            Self::Epic => "epic",
            Self::GameJolt => "gamejolt",
            Self::Pcsx2 => "pcsx2",
            Self::Rpcs3 => "rpcs3",
            Self::ShadPs4 => "shadps4",
            Self::SharpEmu => "sharpemu",
        }
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SharpApp {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub install_dir: String,
    #[serde(default)]
    pub exe_path: String,
    #[serde(default)]
    pub installed: bool,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub engine: String,
    #[serde(default)]
    pub pipeline: String,
    #[serde(default)]
    pub cover_path: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct SharpApps {
    pub ok: bool,
    #[serde(default)]
    pub apps: Vec<SharpApp>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GogStatus {
    pub status: String,
    pub ready: bool,
    pub auth_url: String,
    pub authenticated: bool,
    pub gogdl_available: bool,
    #[serde(default)]
    pub gogdl_path: Option<String>,
    #[serde(default)]
    pub oauth_helper_path: Option<String>,
    #[serde(default)]
    pub oauth_helper_available: Option<bool>,
    #[serde(default)]
    pub oauth_helper_script: Option<String>,
    pub wine_prefix: String,
    pub prefix_initialized: bool,
    pub wine_path: String,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GogGame {
    pub product_id: String,
    pub title: String,
    pub platform: String,
    #[serde(default)]
    pub slug: Option<String>,
    #[serde(default)]
    pub image_url: Option<String>,
    #[serde(default)]
    pub icon_url: Option<String>,
    #[serde(default)]
    pub install_root: Option<String>,
    #[serde(default)]
    pub game_folder: Option<String>,
    #[serde(default)]
    pub executable_path: Option<String>,
    #[serde(default)]
    pub primary_exe: Option<String>,
    #[serde(default)]
    pub primary_task_name: Option<String>,
    pub installed: bool,
    pub running: bool,
    pub status: String,
    #[serde(default)]
    pub download_size_bytes: Option<u64>,
    #[serde(default)]
    pub disk_size_bytes: Option<u64>,
    #[serde(default)]
    pub last_install_pid: Option<i64>,
    #[serde(default)]
    pub last_launch_pid: Option<i64>,
    #[serde(default)]
    pub last_log_path: Option<String>,
    #[serde(default)]
    pub last_error: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EpicStatus {
    pub ok: bool,
    pub tool_available: bool,
    pub tool_version: String,
    pub tool_path: String,
    pub authenticated: bool,
    #[serde(default)]
    pub account: Option<String>,
    pub config_path: String,
    pub game_root: String,
    #[serde(default)]
    pub error: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EpicGame {
    pub app_name: String,
    pub title: String,
    #[serde(default)]
    pub version: Option<String>,
    #[serde(default)]
    pub artwork_url: Option<String>,
    #[serde(default)]
    pub artwork_source: Option<String>,
    pub installed: bool,
    #[serde(default)]
    pub install_path: Option<String>,
    #[serde(default)]
    pub executable: Option<String>,
    pub install_size: u64,
    pub bottle_initialized: bool,
    pub pipeline: String,
    pub mouse_mode: String,
    pub running: bool,
    #[serde(default)]
    pub downloading: Option<bool>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct GameJoltStorage {
    pub mode: String,
    #[serde(rename = "rootPath")]
    pub root_path: String,
    #[serde(rename = "gamejoltDir")]
    pub gamejolt_dir: String,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameJoltGame {
    pub id: String,
    pub name: String,
    #[serde(rename = "install_dir")]
    pub install_dir: String,
    #[serde(rename = "exe_path")]
    pub exe_path: String,
    pub installed: bool,
    #[serde(default, rename = "bottle_initialized")]
    pub bottle_initialized: bool,
    #[serde(default)]
    pub pipeline: String,
    #[serde(default, rename = "mouse_mode")]
    pub mouse_mode: String,
    pub native: bool,
    pub engine: String,
    #[serde(default, rename = "cover_path")]
    pub cover_path: Option<String>,
    #[serde(default, rename = "bottle_id")]
    pub bottle_id: Option<String>,
    #[serde(default, rename = "available_pipelines")]
    pub available_pipelines: Vec<PipelineOption>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct PipelineOption {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub recommended: bool,
}
#[derive(Clone, Debug, Deserialize)]
pub struct GameJoltLibrary {
    pub ok: bool,
    #[serde(default)]
    pub games: Vec<GameJoltGame>,
    #[serde(default)]
    pub storage: Option<GameJoltStorage>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorStatus {
    pub ok: bool,
    #[serde(default)]
    pub installed: bool,
    #[serde(default)]
    pub supported: bool,
    #[serde(default)]
    pub state: String,
    #[serde(default)]
    pub current_tag: Option<String>,
    #[serde(default)]
    pub rollback_available: bool,
    #[serde(default)]
    pub firmware_installed: Option<bool>,
    #[serde(default)]
    pub bios_installed: Option<bool>,
    #[serde(default)]
    pub environment_path: String,
    #[serde(default)]
    pub data_path: String,
    #[serde(default)]
    pub cache_path: String,
    #[serde(default)]
    pub executable_path: Option<String>,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(flatten)]
    pub source_fields: std::collections::BTreeMap<String, Value>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EmulatorGame {
    pub id: String,
    #[serde(default)]
    pub title_id: Option<String>,
    pub title: String,
    #[serde(default)]
    pub version: Option<String>,
    #[serde(default)]
    pub category: Option<String>,
    #[serde(default)]
    pub path: String,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub pid: Option<i64>,
    #[serde(default)]
    pub has_artwork: Option<bool>,
    #[serde(default)]
    pub last_log_path: Option<String>,
    #[serde(default)]
    pub last_exit_code: Option<i32>,
    #[serde(default)]
    pub last_exit_signal: Option<i32>,
    #[serde(flatten)]
    pub source_fields: std::collections::BTreeMap<String, Value>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct EmulatorLibrary {
    pub ok: bool,
    #[serde(default)]
    pub games: Vec<EmulatorGame>,
    #[serde(default)]
    pub roots: Vec<String>,
    #[serde(default)]
    pub error: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateInfo {
    pub ok: bool,
    #[serde(default)]
    pub current_tag: Option<String>,
    #[serde(default)]
    pub latest_tag: Option<String>,
    #[serde(default)]
    pub latest_version: Option<String>,
    #[serde(default)]
    pub available: bool,
    #[serde(default)]
    pub pinned_tag: Option<String>,
    #[serde(default)]
    pub skipped_tag: Option<String>,
    #[serde(default)]
    pub suppressed: Option<String>,
    #[serde(default)]
    pub asset_name: Option<String>,
    #[serde(default)]
    pub download_size: Option<u64>,
    #[serde(default)]
    pub digest: Option<String>,
    #[serde(default)]
    pub published_at: Option<String>,
    #[serde(default)]
    pub error: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateProgress {
    pub ok: bool,
    pub status: String,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub percent: u8,
    #[serde(default)]
    pub message: String,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(default)]
    pub target_tag: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct Pcsx2Settings {
    pub ok: bool,
    #[serde(default)]
    pub controller1: String,
    #[serde(default)]
    pub controller2: String,
    #[serde(default)]
    pub renderer: String,
    #[serde(default)]
    pub controller_options: Vec<NamedOption>,
    #[serde(default)]
    pub renderer_options: Vec<NamedOption>,
    #[serde(default)]
    pub error: Option<String>,
}
#[derive(Clone, Debug, Deserialize)]
pub struct NamedOption {
    pub id: String,
    pub label: String,
}
#[derive(Clone, Debug, Deserialize)]
pub struct OperationResult {
    pub ok: bool,
    #[serde(default)]
    pub pid: Option<i64>,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(flatten)]
    pub extra: std::collections::BTreeMap<String, Value>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Emulator {
    Pcsx2,
    Rpcs3,
    ShadPs4,
    SharpEmu,
}
impl Emulator {
    pub const fn id(self) -> &'static str {
        match self {
            Self::Pcsx2 => "pcsx2",
            Self::Rpcs3 => "rpcs3",
            Self::ShadPs4 => "shadps4",
            Self::SharpEmu => "sharpemu",
        }
    }
    fn base(self) -> String {
        format!("/sharp-library/{}/", self.id())
    }
}

/// Native browser handoff for the persistent GameJolt website. The UI/browser host
/// owns opening the window; these explicit hooks let its lifecycle owner close the
/// window and clear only the named GameJolt store after the user requests cleanup.
/// They intentionally do not touch backend game storage or other WebKit profiles.
pub const GAMEJOLT_BROWSER_URL: &str = "https://gamejolt.com/games";
pub const GAMEJOLT_BROWSER_STORE_ID: &str = "5C6F493A-2D9E-4A25-BEB0-7DC86791553A";
#[derive(Clone, Copy, Debug, Deserialize, Serialize, Eq, PartialEq)]
#[serde(rename_all = "snake_case")]
pub enum GameJoltBrowserIntent {
    OpenPersistentWebsite,
    CloseWindow,
    ClearOwnedWebsiteData,
}

/// All connected Sharp Library operations are opt-in by composition: no method is
/// called by the synthetic preview. Mutations are single-shot via BackendClient.
#[derive(Clone)]
pub struct SharpLibraryClient {
    backend: BackendClient,
}
impl SharpLibraryClient {
    pub fn new(backend: BackendClient) -> Self {
        Self { backend }
    }

    /// Execute exactly one queued intent. Host effects (native pickers, OAuth window,
    /// runtime setup UI, and the GameJolt browser) are returned explicitly and never
    /// guessed into backend calls. The caller serializes invocations and applies snapshots.
    pub fn execute_intent(&self, intent: &SharpIntent) -> Result<SharpIntentResult, BackendError> {
        use SharpIntent::*;
        let result = match intent {
            Refresh(source) => SharpIntentResult::Snapshot(self.load_snapshot(*source)?),
            Sync(source) => {
                match source {
                    SharpSource::Installers => {
                        self.installers()?;
                    }
                    SharpSource::Gog => {
                        self.gog_sync()?;
                    }
                    SharpSource::Epic => {
                        self.epic_sync()?;
                    }
                    SharpSource::GameJolt => {
                        self.gamejolt_sync()?;
                    }
                    SharpSource::Pcsx2 => {
                        self.pcsx2_scan()?;
                    }
                    SharpSource::Rpcs3 => {
                        self.emulator_scan(Emulator::Rpcs3)?;
                    }
                    SharpSource::ShadPs4 => {
                        self.emulator_scan(Emulator::ShadPs4)?;
                    }
                    SharpSource::SharpEmu => {
                        self.emulator_scan(Emulator::SharpEmu)?;
                    }
                }
                SharpIntentResult::Snapshot(self.load_snapshot(*source)?)
            }
            Install { source, game_id } => {
                SharpIntentResult::HostEffect(SharpHostEffect::ChooseInstallDetails {
                    source: *source,
                    game_id: game_id.clone(),
                })
            }
            Launch {
                source,
                game_id,
                executable_path,
                preferences,
            } => {
                let value = match source {
                    SharpSource::Installers => self.backend.post("/sharp-library/launch", json!({"id":game_id}))?,
                    SharpSource::Gog => self.gog_play(game_id, &preferences.pipeline)?,
                    SharpSource::Epic => self.epic_play(game_id)?,
                    SharpSource::GameJolt => self.backend.post("/gamejolt/launch", json!({"id":game_id,"exePath":executable_path.as_deref().ok_or(BackendError::InvalidInput)?,"engine":preferences.pipeline}))?,
                    SharpSource::Pcsx2 => self.backend.post("/sharp-library/pcsx2/launch", json!({"id":game_id,"fullscreen":preferences.fullscreen,"allowNetwork":false}))?,
                    SharpSource::Rpcs3 => self.backend.post("/sharp-library/rpcs3/launch", json!({"id":game_id,"fullscreen":preferences.fullscreen,"allowNetwork":false}))?,
                    SharpSource::ShadPs4 => self.backend.post("/sharp-library/shadps4/launch", json!({"id":game_id,"fullscreen":preferences.fullscreen,"allowNetwork":false}))?,
                    SharpSource::SharpEmu => self.backend.post("/sharp-library/sharpemu/launch", json!({"id":game_id,"fullscreen":preferences.fullscreen,"allowNetwork":preferences.allow_network}))?,
                };
                SharpIntentResult::Operation(value)
            }
            Stop { source, game_id } => SharpIntentResult::Operation(match source {
                SharpSource::Installers => self
                    .backend
                    .post("/sharp-library/stop", json!({"id":game_id}))?,
                SharpSource::Gog => self.gog_stop(game_id)?,
                SharpSource::Epic => self.epic_stop(game_id)?,
                SharpSource::GameJolt => {
                    self.backend.post("/gamejolt/stop", json!({"id":game_id}))?
                }
                SharpSource::Pcsx2 => self
                    .backend
                    .post("/sharp-library/pcsx2/stop", json!({"id":game_id}))?,
                SharpSource::Rpcs3 => self
                    .backend
                    .post("/sharp-library/rpcs3/stop", json!({"id":game_id}))?,
                SharpSource::ShadPs4 => self
                    .backend
                    .post("/sharp-library/shadps4/stop", json!({"id":game_id}))?,
                SharpSource::SharpEmu => self
                    .backend
                    .post("/sharp-library/sharpemu/stop", json!({"id":game_id}))?,
            }),
            Uninstall { source, game_id } => SharpIntentResult::Operation(match source {
                SharpSource::Installers => self
                    .backend
                    .post("/sharp-library/uninstall", json!({"id":game_id}))?,
                SharpSource::Gog => self.gog_uninstall(game_id)?,
                SharpSource::Epic => self.epic_uninstall(game_id)?,
                SharpSource::GameJolt => return Err(BackendError::InvalidInput),
                SharpSource::Pcsx2
                | SharpSource::Rpcs3
                | SharpSource::ShadPs4
                | SharpSource::SharpEmu => return Err(BackendError::InvalidInput),
            }),
            Authenticate(source) => match source {
                SharpSource::Gog | SharpSource::Epic => {
                    SharpIntentResult::HostEffect(SharpHostEffect::Authenticate(*source))
                }
                _ => return Err(BackendError::InvalidInput),
            },
            AuthCallback { source, code } => {
                SharpIntentResult::Operation(self.provider_auth_callback(*source, code)?)
            }
            Logout(source) => SharpIntentResult::Operation(match source {
                SharpSource::Gog => self.gog_logout()?,
                SharpSource::Epic => self.backend.post("/sharp-library/epic/logout", json!({}))?,
                _ => return Err(BackendError::InvalidInput),
            }),
            Import { source, kind } => SharpIntentResult::HostEffect(SharpHostEffect::PickImport {
                source: *source,
                kind: *kind,
            }),
            ImportSelected { source, kind, path } => {
                SharpIntentResult::Operation(self.import_selected(*source, *kind, path)?)
            }
            AddLibraryRoot(source) => match source {
                SharpSource::GameJolt
                | SharpSource::Pcsx2
                | SharpSource::Rpcs3
                | SharpSource::ShadPs4
                | SharpSource::SharpEmu => {
                    SharpIntentResult::HostEffect(SharpHostEffect::PickLibraryRoot(*source))
                }
                _ => return Err(BackendError::InvalidInput),
            },
            OpenRuntimeSetup(source) => match source {
                SharpSource::Pcsx2
                | SharpSource::Rpcs3
                | SharpSource::ShadPs4
                | SharpSource::SharpEmu => {
                    SharpIntentResult::HostEffect(SharpHostEffect::OpenRuntimeSetup(*source))
                }
                _ => return Err(BackendError::InvalidInput),
            },
            Scan(source) => SharpIntentResult::Snapshot(match source {
                SharpSource::Pcsx2 => {
                    self.pcsx2_scan()?;
                    self.load_snapshot(*source)?
                }
                SharpSource::Rpcs3 => {
                    self.emulator_scan(Emulator::Rpcs3)?;
                    self.load_snapshot(*source)?
                }
                SharpSource::ShadPs4 => {
                    self.emulator_scan(Emulator::ShadPs4)?;
                    self.load_snapshot(*source)?
                }
                SharpSource::SharpEmu => {
                    self.emulator_scan(Emulator::SharpEmu)?;
                    self.load_snapshot(*source)?
                }
                _ => return Err(BackendError::InvalidInput),
            }),
            Browser(intent) => {
                SharpIntentResult::HostEffect(SharpHostEffect::GameJoltBrowser(*intent))
            }
        };
        Ok(result)
    }

    fn import_selected(
        &self,
        source: SharpSource,
        kind: SharpImportKind,
        path: &str,
    ) -> Result<Value, BackendError> {
        if path.is_empty() || !std::path::Path::new(path).is_absolute() {
            return Err(BackendError::InvalidInput);
        }
        match (source, kind) {
            (SharpSource::Installers, SharpImportKind::Installer) => self
                .backend
                .post("/sharp-library/install", json!({"srcPath":path})),
            (SharpSource::Pcsx2, SharpImportKind::Bios) => self
                .backend
                .post("/sharp-library/pcsx2/import-bios", json!({"path":path})),
            (SharpSource::Rpcs3, SharpImportKind::Firmware) => self.backend.post(
                "/sharp-library/rpcs3/install-firmware",
                json!({"path":path}),
            ),
            (SharpSource::Rpcs3, SharpImportKind::Package) => self
                .backend
                .post("/sharp-library/rpcs3/install-package", json!({"path":path})),
            (SharpSource::ShadPs4, SharpImportKind::Modules) => self.backend.post(
                "/sharp-library/shadps4/import-modules",
                json!({"path":path}),
            ),
            (SharpSource::ShadPs4, SharpImportKind::Fonts) => self
                .backend
                .post("/sharp-library/shadps4/import-fonts", json!({"path":path})),
            (SharpSource::GameJolt, SharpImportKind::GameFolder) => self
                .backend
                .post("/gamejolt/storage", json!({"rootPath":path})),
            (SharpSource::Pcsx2, SharpImportKind::GameFolder) => self
                .backend
                .post("/sharp-library/pcsx2/add-root", json!({"path":path})),
            (SharpSource::Rpcs3, SharpImportKind::GameFolder) => self
                .backend
                .post("/sharp-library/rpcs3/add-root", json!({"path":path})),
            (SharpSource::ShadPs4, SharpImportKind::GameFolder) => self
                .backend
                .post("/sharp-library/shadps4/add-root", json!({"path":path})),
            (SharpSource::SharpEmu, SharpImportKind::GameFolder) => self
                .backend
                .post("/sharp-library/sharpemu/add-root", json!({"path":path})),
            _ => Err(BackendError::InvalidInput),
        }
    }

    /// Load one source catalog and its overview through the authoritative backend.
    /// This is a single request sequence owned by ConnectedApp's serialized worker.
    pub fn load_snapshot(
        &self,
        source: SharpSource,
    ) -> Result<SharpConnectedSnapshot, BackendError> {
        let mut snapshot = SharpConnectedSnapshot::default();
        match source {
            SharpSource::Installers => {
                let list = self.installers()?;
                snapshot
                    .overview
                    .push(("Applications".into(), list.apps.len().to_string()));
                snapshot.games = list
                    .apps
                    .into_iter()
                    .map(|g| SharpGameRow {
                        id: g.id,
                        title: g.name,
                        subtitle: g.engine,
                        installed: g.installed,
                        running: g.running,
                        detail: g.install_dir,
                        executable_path: Some(g.exe_path),
                    })
                    .collect();
            }
            SharpSource::Gog => {
                let (games, status) = self.gog_games()?;
                snapshot.overview.extend([
                    (
                        "Account".into(),
                        if status.authenticated {
                            "Connected"
                        } else {
                            "Not connected"
                        }
                        .into(),
                    ),
                    (
                        "Prefix".into(),
                        if status.prefix_initialized {
                            "Initialized"
                        } else {
                            "Setup required"
                        }
                        .into(),
                    ),
                ]);
                snapshot.games = games
                    .into_iter()
                    .map(|g| SharpGameRow {
                        id: g.product_id,
                        title: g.title,
                        subtitle: g.platform,
                        installed: g.installed,
                        running: g.running,
                        detail: g.status,
                        executable_path: g.executable_path,
                    })
                    .collect();
            }
            SharpSource::Epic => {
                let status = self.epic_status()?;
                let games = self.epic_games()?;
                snapshot.overview.extend([
                    (
                        "Account".into(),
                        if status.authenticated {
                            "Connected"
                        } else {
                            "Not connected"
                        }
                        .into(),
                    ),
                    (
                        "Epic tools".into(),
                        if status.tool_available {
                            status.tool_version
                        } else {
                            "Not installed".into()
                        },
                    ),
                ]);
                snapshot.games = games
                    .into_iter()
                    .map(|g| SharpGameRow {
                        id: g.app_name,
                        title: g.title,
                        subtitle: g.version.unwrap_or_default(),
                        installed: g.installed,
                        running: g.running,
                        detail: g.install_path.unwrap_or_default(),
                        executable_path: g.executable,
                    })
                    .collect();
            }
            SharpSource::GameJolt => {
                let library = self.gamejolt_library()?;
                snapshot.overview.extend([
                    ("Games".into(), library.games.len().to_string()),
                    (
                        "Storage".into(),
                        library
                            .storage
                            .as_ref()
                            .map(|s| s.root_path.clone())
                            .unwrap_or_else(|| "Default".into()),
                    ),
                ]);
                snapshot.games = library
                    .games
                    .into_iter()
                    .map(|g| SharpGameRow {
                        id: g.id,
                        title: g.name,
                        subtitle: g.engine,
                        installed: g.installed,
                        running: false,
                        detail: g.install_dir,
                        executable_path: Some(g.exe_path),
                    })
                    .collect();
            }
            _ => {
                let e = match source {
                    SharpSource::Pcsx2 => Emulator::Pcsx2,
                    SharpSource::Rpcs3 => Emulator::Rpcs3,
                    SharpSource::ShadPs4 => Emulator::ShadPs4,
                    _ => Emulator::SharpEmu,
                };
                let status = self.emulator_status(e)?;
                let library = self.emulator_games(e, false)?;
                snapshot.overview.extend([
                    (
                        "Runtime".into(),
                        if status.installed {
                            status
                                .current_tag
                                .clone()
                                .unwrap_or_else(|| "Installed".into())
                        } else {
                            "Not installed".into()
                        },
                    ),
                    ("Games".into(), library.games.len().to_string()),
                    (
                        "Firmware / BIOS".into(),
                        if status
                            .firmware_installed
                            .or(status.bios_installed)
                            .unwrap_or(false)
                        {
                            "Ready"
                        } else {
                            "Required"
                        }
                        .into(),
                    ),
                ]);
                snapshot.games = library
                    .games
                    .into_iter()
                    .map(|g| SharpGameRow {
                        id: g.id,
                        title: g.title,
                        subtitle: g.category.unwrap_or_default(),
                        installed: true,
                        running: g.running,
                        detail: g.path,
                        executable_path: None,
                    })
                    .collect();
                if let Some(error) = library.error {
                    snapshot.error = Some(error);
                }
            }
        }
        Ok(snapshot)
    }
    fn get<T: for<'de> Deserialize<'de>>(&self, path: &str) -> Result<T, BackendError> {
        self.backend.get(path)
    }
    fn post<T: for<'de> Deserialize<'de>>(
        &self,
        path: &str,
        body: Value,
    ) -> Result<T, BackendError> {
        serde_json::from_value(self.backend.post(path, body)?)
            .map_err(|_| BackendError::InvalidJson)
    }
    pub fn installers(&self) -> Result<SharpApps, BackendError> {
        self.get("/sharp-library")
    }
    pub fn bottles(&self) -> Result<Value, BackendError> {
        self.get("/bottles")
    }
    pub fn runtime_profiles(&self) -> Result<Value, BackendError> {
        self.get("/bottles/profiles")
    }
    pub fn import_installer(&self, body: Value) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/install", body)
    }
    pub fn import_bottle_app(&self, body: Value) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/import-bottle-app", body)
    }
    pub fn launch_installer(&self, id: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/launch", json!({"id":id}))
    }
    pub fn stop_installer(&self, id: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/stop", json!({"id":id}))
    }
    pub fn uninstall_installer(&self, id: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/uninstall", json!({"id":id}))
    }
    pub fn rename_installer(&self, id: &str, name: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/rename", json!({"id":id,"name":name}))
    }
    pub fn set_installer_engine(
        &self,
        id: &str,
        engine: &str,
    ) -> Result<OperationResult, BackendError> {
        self.post(
            "/sharp-library/set-engine",
            json!({"id":id,"engine":engine}),
        )
    }
    pub fn track_running(&self, id: &str, pid: u32) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/track-running", json!({"id":id,"pid":pid}))
    }
    pub fn running_installers(&self) -> Result<Value, BackendError> {
        self.get("/sharp-library/running")
    }
    pub fn uninstall_sharp_source_app(&self, id: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/uninstall", json!({"id":id}))
    }
    pub fn gog_status(&self) -> Result<GogStatus, BackendError> {
        let r: Envelope<GogStatus> = self.get("/sharp-library/gog/status")?;
        Ok(r.status)
    }
    pub fn gog_games(&self) -> Result<(Vec<GogGame>, GogStatus), BackendError> {
        let r: GamesEnvelope<GogGame, GogStatus> = self.get("/sharp-library/gog/games")?;
        Ok((r.games, r.status.ok_or(BackendError::InvalidJson)?))
    }
    pub fn gog_initialize(&self) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/gog/initialize-prefix", json!({}))
    }
    pub fn gog_remove_prefix(&self) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/gog/remove-prefix", json!({}))
    }
    pub fn gog_auth_code(&self, code: &str) -> Result<Value, BackendError> {
        if code.trim().is_empty() {
            return Err(BackendError::InvalidInput);
        }
        self.backend
            .post("/sharp-library/gog/auth-code", json!({"code":code.trim()}))
    }
    pub fn gog_sync(&self) -> Result<(Vec<GogGame>, GogStatus), BackendError> {
        let value = self.backend.post("/sharp-library/gog/sync", json!({}))?;
        let r: GamesEnvelope<GogGame, GogStatus> =
            serde_json::from_value(value).map_err(|_| BackendError::InvalidJson)?;
        Ok((r.games, r.status.ok_or(BackendError::InvalidJson)?))
    }
    pub fn gog_install(
        &self,
        product_id: &str,
        title: &str,
        platform: &str,
        install_path: &str,
    ) -> Result<Value, BackendError> {
        self.backend.post("/sharp-library/gog/install",json!({"productId":product_id,"title":title,"platform":platform,"installPath":install_path}))
    }
    pub fn gog_import(
        &self,
        product_id: &str,
        title: &str,
        platform: &str,
        install_path: &str,
    ) -> Result<Value, BackendError> {
        self.backend.post("/sharp-library/gog/import",json!({"productId":product_id,"title":title,"platform":platform,"installPath":install_path}))
    }
    pub fn gog_stop_all(&self) -> Result<Value, BackendError> {
        self.backend.post("/sharp-library/gog/stop-all", json!({}))
    }
    pub fn gog_progress(&self, product_id: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/gog/progress",
            json!({"productId":product_id}),
        )
    }
    pub fn gog_play(&self, product_id: &str, engine: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/gog/play",
            json!({"productId":product_id,"engine":engine}),
        )
    }
    pub fn gog_stop(&self, product_id: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/gog/stop", json!({"productId":product_id}))
    }
    pub fn gog_uninstall(&self, product_id: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/gog/uninstall",
            json!({"productId":product_id}),
        )
    }
    pub fn gog_save_executable(&self, product_id: &str, path: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/gog/save-executable",
            json!({"productId":product_id,"executablePath":path}),
        )
    }
    pub fn gog_logout(&self) -> Result<Value, BackendError> {
        self.backend.post("/sharp-library/gog/logout", json!({}))
    }
    pub fn epic_status(&self) -> Result<EpicStatus, BackendError> {
        self.get("/sharp-library/epic/status")
    }
    pub fn epic_games(&self) -> Result<Vec<EpicGame>, BackendError> {
        let r: GamesEnvelope<EpicGame, Value> = self.get("/sharp-library/epic/games")?;
        Ok(r.games)
    }
    pub fn epic_running(&self) -> Result<Value, BackendError> {
        self.get("/sharp-library/epic/running")
    }
    pub fn epic_install_tool(&self) -> Result<EpicStatus, BackendError> {
        self.post("/sharp-library/epic/install-tool", json!({}))
    }
    pub fn epic_auth_code(&self, code: &str) -> Result<EpicStatus, BackendError> {
        if code.trim().is_empty() {
            return Err(BackendError::InvalidInput);
        }
        self.post("/sharp-library/epic/auth", json!({"code":code.trim()}))
    }
    /// Complete only provider callbacks delivered by the native purpose-scoped browser.
    /// Authorization codes are sent once in the POST body and are never formatted here.
    pub fn provider_auth_callback(
        &self,
        source: SharpSource,
        code: &str,
    ) -> Result<Value, BackendError> {
        match source {
            SharpSource::Gog => self.gog_auth_code(code),
            SharpSource::Epic => {
                if code.trim().is_empty() {
                    return Err(BackendError::InvalidInput);
                }
                self.backend
                    .post("/sharp-library/epic/auth", json!({"code":code.trim()}))
            }
            _ => Err(BackendError::InvalidInput),
        }
    }
    pub fn epic_sync(&self) -> Result<Vec<EpicGame>, BackendError> {
        let value = self.backend.post("/sharp-library/epic/sync", json!({}))?;
        let r: GamesEnvelope<EpicGame, Value> =
            serde_json::from_value(value).map_err(|_| BackendError::InvalidJson)?;
        Ok(r.games)
    }
    pub fn epic_logout(&self) -> Result<EpicStatus, BackendError> {
        self.post("/sharp-library/epic/logout", json!({}))
    }
    pub fn epic_install(&self, app_name: &str, install_path: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/epic/install",
            json!({"appName":app_name,"installPath":install_path}),
        )
    }
    pub fn epic_thegamesdb_status(&self) -> Result<Value, BackendError> {
        self.get("/sharp-library/epic/thegamesdb-api-key")
    }
    pub fn epic_stop_all(&self) -> Result<Value, BackendError> {
        self.backend.post("/sharp-library/epic/stop-all", json!({}))
    }
    pub fn epic_initialize(
        &self,
        app_name: &str,
        pipeline: &str,
        mouse_mode: &str,
    ) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/epic/initialize",
            json!({"appName":app_name,"pipeline":pipeline,"mouseMode":mouse_mode}),
        )
    }
    pub fn epic_progress(&self, app_name: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/epic/progress", json!({"appName":app_name}))
    }
    pub fn epic_cancel(&self, app_name: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/epic/cancel", json!({"appName":app_name}))
    }
    pub fn epic_play(&self, app_name: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/epic/play", json!({"appName":app_name}))
    }
    pub fn epic_stop(&self, app_name: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/epic/stop", json!({"appName":app_name}))
    }
    pub fn epic_uninstall(&self, app_name: &str) -> Result<Value, BackendError> {
        self.backend
            .post("/sharp-library/epic/uninstall", json!({"appName":app_name}))
    }
    pub fn epic_save_executable(&self, app_name: &str, path: &str) -> Result<Value, BackendError> {
        self.backend.post(
            "/sharp-library/epic/save-executable",
            json!({"appName":app_name,"executablePath":path}),
        )
    }
    pub fn gamejolt_library(&self) -> Result<GameJoltLibrary, BackendError> {
        self.get("/gamejolt")
    }
    pub fn gamejolt_storage(&self) -> Result<GameJoltStorage, BackendError> {
        self.get("/gamejolt/storage")
    }
    pub fn gamejolt_sync(&self) -> Result<GameJoltLibrary, BackendError> {
        let value = self.backend.post("/gamejolt/sync", json!({}))?;
        serde_json::from_value(value).map_err(|_| BackendError::InvalidJson)
    }
    pub fn gamejolt_set_storage(&self, root_path: &str) -> Result<GameJoltStorage, BackendError> {
        self.post("/gamejolt/storage", json!({"rootPath":root_path}))
    }
    pub fn gamejolt_set_name(&self, id: &str, name: &str) -> Result<OperationResult, BackendError> {
        self.post("/gamejolt/name", json!({"id":id,"name":name}))
    }
    pub fn gamejolt_launch(
        &self,
        id: &str,
        exe_path: &str,
        engine: &str,
    ) -> Result<OperationResult, BackendError> {
        if id.is_empty() || exe_path.is_empty() || !std::path::Path::new(exe_path).is_absolute() {
            return Err(BackendError::InvalidInput);
        }
        self.post(
            "/gamejolt/launch",
            json!({"id":id,"exePath":exe_path,"engine":engine}),
        )
    }
    pub fn gamejolt_stop(&self, id: &str) -> Result<OperationResult, BackendError> {
        self.post("/gamejolt/stop", json!({"id":id}))
    }
    pub fn gamejolt_uninstall(&self, install_dir: &str) -> Result<OperationResult, BackendError> {
        if install_dir.is_empty() || !std::path::Path::new(install_dir).is_absolute() {
            return Err(BackendError::InvalidInput);
        }
        self.post("/gamejolt/uninstall", json!({"installDir":install_dir}))
    }
    pub fn gamejolt_running(&self) -> Result<Value, BackendError> {
        self.get("/gamejolt/running")
    }
    pub fn gamejolt_process_status(&self, id: &str, pid: i64) -> Result<Value, BackendError> {
        if id.is_empty() || pid <= 0 {
            return Err(BackendError::InvalidInput);
        }
        self.post("/gamejolt/status", json!({"id":id,"pid":pid}))
    }
    pub fn gamejolt_stop_all(&self) -> Result<Value, BackendError> {
        self.backend.post("/gamejolt/stop-all", json!({}))
    }
    pub fn gamejolt_set_game_pipeline(
        &self,
        id: &str,
        pipeline: &str,
    ) -> Result<OperationResult, BackendError> {
        self.post("/gamejolt/engine", json!({"id":id,"engine":pipeline}))
    }
    pub fn gamejolt_set_engine(
        &self,
        id: &str,
        engine: &str,
    ) -> Result<OperationResult, BackendError> {
        self.post("/gamejolt/engine", json!({"id":id,"engine":engine}))
    }
    pub fn emulator_status(&self, e: Emulator) -> Result<EmulatorStatus, BackendError> {
        self.get(&format!("{}status", e.base()))
    }
    pub fn emulator_games(&self, e: Emulator, scan: bool) -> Result<EmulatorLibrary, BackendError> {
        if e == Emulator::Pcsx2 && scan {
            self.post(&format!("{}scan", e.base()), json!({}))
        } else {
            self.get(&format!("{}games", e.base()))
        }
    }
    pub fn emulator_sessions(&self, e: Emulator) -> Result<Value, BackendError> {
        if e != Emulator::SharpEmu {
            return Err(BackendError::InvalidInput);
        }
        self.get(&format!("{}sessions", e.base()))
    }
    pub fn emulator_scan(&self, e: Emulator) -> Result<EmulatorLibrary, BackendError> {
        self.post(&format!("{}scan", e.base()), json!({}))
    }
    pub fn emulator_update_check(
        &self,
        e: Emulator,
        refresh: bool,
    ) -> Result<UpdateInfo, BackendError> {
        let verb = if refresh { "refresh" } else { "check" };
        let path = format!("{}update/{verb}", e.base());
        if refresh {
            self.post(&path, json!({}))
        } else {
            self.get(&path)
        }
    }
    pub fn emulator_update_progress(&self, e: Emulator) -> Result<UpdateProgress, BackendError> {
        self.get(&format!("{}update/progress", e.base()))
    }
    pub fn emulator_update_install(&self, e: Emulator) -> Result<UpdateProgress, BackendError> {
        self.post(&format!("{}update/install", e.base()), json!({}))
    }
    pub fn emulator_update_policy(
        &self,
        e: Emulator,
        policy: UpdatePolicy,
        tag: Option<&str>,
    ) -> Result<UpdateInfo, BackendError> {
        let route = match policy {
            UpdatePolicy::PinCurrent => "pin-current",
            UpdatePolicy::Unpin => "unpin",
            UpdatePolicy::Skip => "skip-update",
            UpdatePolicy::ClearSkip => "clear-skip",
        };
        self.post(&format!("{}{route}", e.base()), json!({"tag":tag}))
    }
    pub fn emulator_rollback(&self, e: Emulator) -> Result<EmulatorStatus, BackendError> {
        self.post(&format!("{}update/rollback", e.base()), json!({}))
    }
    pub fn emulator_add_root(
        &self,
        e: Emulator,
        path: &str,
    ) -> Result<EmulatorLibrary, BackendError> {
        self.post(&format!("{}add-root", e.base()), json!({"path":path}))
    }
    pub fn emulator_remove_root(
        &self,
        e: Emulator,
        path: &str,
    ) -> Result<EmulatorLibrary, BackendError> {
        self.post(&format!("{}remove-root", e.base()), json!({"path":path}))
    }
    pub fn emulator_install_content(
        &self,
        e: Emulator,
        kind: Rpcs3Content,
        path: &str,
    ) -> Result<OperationResult, BackendError> {
        if e != Emulator::Rpcs3 {
            return Err(BackendError::InvalidInput);
        }
        self.rpcs3_install_content(kind, path)
    }
    pub fn pcsx2_scan(&self) -> Result<EmulatorLibrary, BackendError> {
        self.emulator_scan(Emulator::Pcsx2)
    }
    pub fn pcsx2_stop_all(&self) -> Result<OperationResult, BackendError> {
        self.emulator_stop(Emulator::Pcsx2, None)
    }
    pub fn emulator_launch(
        &self,
        e: Emulator,
        id: &str,
        fullscreen: bool,
        allow_network: bool,
    ) -> Result<OperationResult, BackendError> {
        if e != Emulator::SharpEmu && allow_network {
            return Err(BackendError::InvalidInput);
        }
        self.post(
            &format!("{}launch", e.base()),
            json!({"id":id,"fullscreen":fullscreen,"allowNetwork":allow_network}),
        )
    }
    pub fn emulator_stop(
        &self,
        e: Emulator,
        id: Option<&str>,
    ) -> Result<OperationResult, BackendError> {
        let body = id.map(|id| json!({"id":id})).unwrap_or_else(|| json!({}));
        self.post(&format!("{}stop", e.base()), body)
    }
    pub fn emulator_remove_runtime(&self, e: Emulator) -> Result<OperationResult, BackendError> {
        self.post(
            &format!("{}remove-runtime", e.base()),
            json!({"confirm":true}),
        )
    }
    pub fn pcsx2_settings(&self) -> Result<Pcsx2Settings, BackendError> {
        self.get("/sharp-library/pcsx2/settings")
    }
    pub fn pcsx2_configure(&self, field: &str, value: &str) -> Result<Pcsx2Settings, BackendError> {
        if !["controller1", "controller2", "renderer"].contains(&field) {
            return Err(BackendError::InvalidInput);
        }
        self.post("/sharp-library/pcsx2/configure", json!({field:value}))
    }
    pub fn pcsx2_initialize(&self) -> Result<EmulatorStatus, BackendError> {
        self.post("/sharp-library/pcsx2/initialize", json!({}))
    }
    pub fn pcsx2_open(&self, setup: bool) -> Result<OperationResult, BackendError> {
        self.post(
            if setup {
                "/sharp-library/pcsx2/open-setup"
            } else {
                "/sharp-library/pcsx2/open-ui"
            },
            json!({}),
        )
    }
    pub fn pcsx2_import_bios(&self, path: &str) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/pcsx2/import-bios", json!({"path":path}))
    }
    pub fn rpcs3_open(&self) -> Result<OperationResult, BackendError> {
        self.post("/sharp-library/rpcs3/open-ui", json!({}))
    }
    pub fn rpcs3_install_content(
        &self,
        kind: Rpcs3Content,
        path: &str,
    ) -> Result<OperationResult, BackendError> {
        let route = match kind {
            Rpcs3Content::Firmware => "install-firmware",
            Rpcs3Content::Package => "install-package",
        };
        self.post(
            &format!("/sharp-library/rpcs3/{route}"),
            json!({"path":path}),
        )
    }
    pub fn shadps4_import(
        &self,
        kind: ShadPs4Content,
        path: &str,
    ) -> Result<OperationResult, BackendError> {
        let route = match kind {
            ShadPs4Content::Modules => "import-modules",
            ShadPs4Content::Fonts => "import-fonts",
        };
        self.post(
            &format!("/sharp-library/shadps4/{route}"),
            json!({"path":path}),
        )
    }
    pub fn epic_thegamesdb_key(&self, key: &str) -> Result<Value, BackendError> {
        self.backend.save_gamesdb_key(key)?;
        Ok(json!({"ok":true}))
    }
}
#[derive(Clone, Copy, Debug)]
pub enum UpdatePolicy {
    PinCurrent,
    Unpin,
    Skip,
    ClearSkip,
}
#[derive(Clone, Copy, Debug)]
pub enum Rpcs3Content {
    Firmware,
    Package,
}
#[derive(Clone, Copy, Debug)]
pub enum ShadPs4Content {
    Modules,
    Fonts,
}
#[derive(Deserialize)]
struct Envelope<T> {
    status: T,
}
#[derive(Deserialize)]
#[serde(bound(deserialize = "G: Deserialize<'de>, S: Deserialize<'de>"))]
struct GamesEnvelope<G, S> {
    #[serde(default)]
    games: Vec<G>,
    #[serde(default)]
    status: Option<S>,
}

/// Stable row projection for the connected provider surface. Provider wire models remain
/// available above; this presentation projection deliberately contains no credentials.
#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SharpGameRow {
    pub id: String,
    pub title: String,
    pub subtitle: String,
    pub installed: bool,
    pub running: bool,
    pub detail: String,
    pub executable_path: Option<String>,
}
#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SharpConnectedSnapshot {
    pub loading: bool,
    pub error: Option<String>,
    pub overview: Vec<(String, String)>,
    pub games: Vec<SharpGameRow>,
    pub progress: Option<(u8, String)>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SharpIntent {
    Refresh(SharpSource),
    Sync(SharpSource),
    Install {
        source: SharpSource,
        game_id: String,
    },
    Launch {
        source: SharpSource,
        game_id: String,
        executable_path: Option<String>,
        preferences: SharpLaunchPreferences,
    },
    Stop {
        source: SharpSource,
        game_id: String,
    },
    Uninstall {
        source: SharpSource,
        game_id: String,
    },
    Authenticate(SharpSource),
    /// Transient callback from the purpose-scoped native auth browser. Never store/log code.
    AuthCallback {
        source: SharpSource,
        code: String,
    },
    Logout(SharpSource),
    /// Request a native picker; the host returns ImportSelected with the chosen path.
    Import {
        source: SharpSource,
        kind: SharpImportKind,
    },
    ImportSelected {
        source: SharpSource,
        kind: SharpImportKind,
        path: String,
    },
    AddLibraryRoot(SharpSource),
    OpenRuntimeSetup(SharpSource),
    Scan(SharpSource),
    Browser(GameJoltBrowserIntent),
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum SharpImportKind {
    Installer,
    Bios,
    Firmware,
    Package,
    Modules,
    Fonts,
    GameFolder,
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase", tag = "effect")]
pub enum SharpHostEffect {
    Authenticate(SharpSource),
    PickImport {
        source: SharpSource,
        kind: SharpImportKind,
    },
    PickLibraryRoot(SharpSource),
    OpenRuntimeSetup(SharpSource),
    ChooseInstallDetails {
        source: SharpSource,
        game_id: String,
    },
    GameJoltBrowser(GameJoltBrowserIntent),
}
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "camelCase", tag = "kind", content = "value")]
pub enum SharpIntentResult {
    Snapshot(SharpConnectedSnapshot),
    Operation(Value),
    HostEffect(SharpHostEffect),
}

#[derive(Clone, Debug, Deserialize, Serialize, Default, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct SharpLaunchPreferences {
    pub pipeline: String,
    pub fullscreen: bool,
    /// SharpEmu networking is false unless the user explicitly enables it per launch.
    pub allow_network: bool,
}

/// GPUI presentation for connected Sharp providers. The owner retains the backend client,
/// executes queued typed intents on its serial backend lane, then calls apply_snapshot;
/// this component never clones a client or polls independently.
pub struct SharpConnectedView {
    source: SharpSource,
    snapshot: SharpConnectedSnapshot,
    intents: Vec<SharpIntent>,
    launch_dialog: Option<(String, Option<String>)>,
    preferences: SharpLaunchPreferences,
    notice: Option<String>,
    pending_uninstall: Option<(SharpSource, String)>,
    busy: bool,
}
impl SharpConnectedView {
    pub fn new(source: SharpSource) -> Self {
        Self {
            source,
            snapshot: SharpConnectedSnapshot {
                loading: true,
                ..Default::default()
            },
            intents: Vec::new(),
            launch_dialog: None,
            preferences: SharpLaunchPreferences::default(),
            notice: None,
            pending_uninstall: None,
            busy: false,
        }
    }
    pub fn source(&self) -> SharpSource {
        self.source
    }
    pub fn set_source(&mut self, source: SharpSource, cx: &mut gpui::Context<Self>) {
        self.source = source;
        self.snapshot = SharpConnectedSnapshot {
            loading: true,
            ..Default::default()
        };
        self.intents.push(SharpIntent::Refresh(source));
        cx.notify();
    }
    pub fn apply_snapshot(
        &mut self,
        snapshot: SharpConnectedSnapshot,
        cx: &mut gpui::Context<Self>,
    ) {
        self.snapshot = snapshot;
        self.busy = false;
        cx.notify();
    }
    pub fn take_intents(&mut self) -> Vec<SharpIntent> {
        std::mem::take(&mut self.intents)
    }
    pub fn submit_intent(&mut self, intent: SharpIntent, cx: &mut gpui::Context<Self>) {
        self.queue(intent, cx);
    }
    pub fn set_busy(&mut self, busy: bool, cx: &mut gpui::Context<Self>) {
        self.busy = busy;
        cx.notify();
    }
    pub fn apply_error(&mut self, error: String, cx: &mut gpui::Context<Self>) {
        self.snapshot.loading = false;
        self.snapshot.error = Some(error);
        self.busy = false;
        cx.notify();
    }
    fn queue(&mut self, intent: SharpIntent, cx: &mut gpui::Context<Self>) {
        if self.busy {
            return;
        }
        self.intents.push(intent);
        self.notice = None;
        cx.notify();
    }
    fn source_title(source: SharpSource) -> &'static str {
        match source {
            SharpSource::Installers => "Installers",
            SharpSource::Gog => "GOG",
            SharpSource::Epic => "Epic",
            SharpSource::GameJolt => "GameJolt",
            SharpSource::Pcsx2 => "PCSX2",
            SharpSource::Rpcs3 => "RPCS3",
            SharpSource::ShadPs4 => "shadPS4",
            SharpSource::SharpEmu => "SharpEmu",
        }
    }
    fn action_button(
        &self,
        id: &'static str,
        label: &'static str,
        intent: SharpIntent,
        cx: &mut gpui::Context<Self>,
    ) -> impl gpui::IntoElement {
        gpui::div()
            .id(id)
            .px(gpui::px(12.))
            .py(gpui::px(8.))
            .rounded(gpui::px(8.))
            .bg(gpui::rgb(0x29251f))
            .border_1()
            .border_color(gpui::rgb(0x675b48))
            .text_color(gpui::rgb(0xe8d6b7))
            .text_size(gpui::px(11.))
            .cursor_pointer()
            .child(label)
            .on_click(cx.listener(move |this, _, _, cx| this.queue(intent.clone(), cx)))
    }
}
impl gpui::Render for SharpConnectedView {
    fn render(
        &mut self,
        _window: &mut gpui::Window,
        cx: &mut gpui::Context<Self>,
    ) -> impl gpui::IntoElement {
        use gpui::prelude::*;
        let source = self.source;
        let mut nav = gpui::div()
            .w(gpui::px(196.))
            .flex_none()
            .flex()
            .flex_col()
            .gap(gpui::px(5.))
            .p(gpui::px(14.))
            .bg(gpui::rgb(0x121518))
            .border_r_1()
            .border_color(gpui::rgb(0x303437));
        for item in SharpSource::ALL {
            let selected = source == item;
            nav = nav.child(
                gpui::div()
                    .id(item.id())
                    .px(gpui::px(10.))
                    .py(gpui::px(10.))
                    .rounded(gpui::px(8.))
                    .bg(gpui::rgb(if selected { 0x28251f } else { 0x121518 }))
                    .text_color(gpui::rgb(if selected { 0xe8d6b7 } else { 0xb6b9b7 }))
                    .text_size(gpui::px(12.))
                    .cursor_pointer()
                    .child(Self::source_title(item))
                    .on_click(cx.listener(move |this, _, _, cx| this.set_source(item, cx))),
            );
        }
        nav = nav.child(
            gpui::div()
                .mt(gpui::px(14.))
                .text_size(gpui::px(10.))
                .text_color(gpui::rgb(0x858d8d))
                .child("SOURCE COLLECTIONS"),
        );
        let mut main = gpui::div()
            .flex_1()
            .min_w(gpui::px(0.))
            .flex()
            .flex_col()
            .gap(gpui::px(14.))
            .p(gpui::px(24.))
            .overflow_hidden();
        main = main.child(
            gpui::div()
                .flex()
                .justify_between()
                .items_start()
                .child(
                    gpui::div()
                        .flex()
                        .flex_col()
                        .gap(gpui::px(5.))
                        .child(
                            gpui::div()
                                .text_size(gpui::px(10.))
                                .text_color(gpui::rgb(0xa59b86))
                                .child("METALSHARP · CONNECTED LIBRARY"),
                        )
                        .child(
                            gpui::div()
                                .text_size(gpui::px(25.))
                                .text_color(gpui::rgb(0xf1eee6))
                                .font_weight(gpui::FontWeight::BOLD)
                                .child(format!("{} Library", Self::source_title(source))),
                        ),
                )
                .child(
                    gpui::div()
                        .flex()
                        .gap(gpui::px(7.))
                        .child(self.action_button(
                            "sharp-refresh",
                            "Refresh",
                            SharpIntent::Refresh(source),
                            cx,
                        ))
                        .child(self.action_button(
                            "sharp-sync",
                            "Sync library",
                            SharpIntent::Sync(source),
                            cx,
                        )),
                ),
        );
        let mut overview = gpui::div()
            .w_full()
            .flex()
            .flex_wrap()
            .gap(gpui::px(8.))
            .rounded(gpui::px(12.))
            .bg(gpui::rgb(0x1c2228))
            .border_1()
            .border_color(gpui::rgb(0x393b37))
            .p(gpui::px(14.));
        for (label, value) in &self.snapshot.overview {
            overview = overview.child(
                gpui::div()
                    .min_w(gpui::px(150.))
                    .flex_1()
                    .flex()
                    .flex_col()
                    .gap(gpui::px(4.))
                    .child(
                        gpui::div()
                            .text_size(gpui::px(10.))
                            .text_color(gpui::rgb(0x9ba19f))
                            .child(label.clone()),
                    )
                    .child(
                        gpui::div()
                            .text_size(gpui::px(12.))
                            .text_color(gpui::rgb(0xf1eee6))
                            .child(value.clone()),
                    ),
            );
        }
        if !self.snapshot.overview.is_empty() {
            main = main.child(overview);
        }
        if let Some((percent, label)) = &self.snapshot.progress {
            main = main.child(
                gpui::div()
                    .p(gpui::px(12.))
                    .rounded(gpui::px(9.))
                    .bg(gpui::rgb(0x262a2b))
                    .text_color(gpui::rgb(0xe8d6b7))
                    .text_size(gpui::px(11.))
                    .child(format!("{label} · {percent}%")),
            );
        }
        if self.snapshot.loading {
            main = main.child(
                gpui::div()
                    .p(gpui::px(20.))
                    .text_color(gpui::rgb(0xb0b5b3))
                    .child("Loading provider catalog…"),
            );
        }
        if let Some(error) = &self.snapshot.error {
            main = main.child(
                gpui::div()
                    .p(gpui::px(14.))
                    .rounded(gpui::px(9.))
                    .border_1()
                    .border_color(gpui::rgb(0x79463f))
                    .bg(gpui::rgb(0x2c201f))
                    .text_color(gpui::rgb(0xf0b7aa))
                    .child(error.clone()),
            );
        }
        if self.snapshot.games.is_empty() && !self.snapshot.loading && self.snapshot.error.is_none()
        {
            main = main.child(gpui::div().w_full().p(gpui::px(22.)).rounded(gpui::px(12.)).bg(gpui::rgb(0x171b1d)).text_color(gpui::rgb(0xb4b9b7)).child("No games in this collection yet. Sync your provider library or add an owned game location."));
        }
        for (index, game) in self.snapshot.games.iter().enumerate() {
            let id = game.id.clone();
            let executable_path = game.executable_path.clone();
            let installed = game.installed;
            let running = game.running;
            let card = gpui::div()
                .id(("sharp-game-card", index))
                .w_full()
                .min_h(gpui::px(92.))
                .p(gpui::px(14.))
                .rounded(gpui::px(11.))
                .border_1()
                .border_color(gpui::rgb(0x34383a))
                .bg(gpui::rgb(0x171b1d))
                .flex()
                .items_center()
                .justify_between()
                .gap(gpui::px(12.));
            let detail = if game.detail.is_empty() {
                game.subtitle.clone()
            } else {
                format!("{} · {}", game.subtitle, game.detail)
            };
            let mut controls = gpui::div()
                .flex()
                .flex_wrap()
                .justify_end()
                .gap(gpui::px(6.));
            if running {
                controls = controls.child(self.action_button(
                    "sharp-game-stop",
                    "Stop",
                    SharpIntent::Stop {
                        source,
                        game_id: id.clone(),
                    },
                    cx,
                ));
            } else if installed {
                let launch_id = id.clone();
                let launch_executable = executable_path.clone();
                controls = controls.child(
                    gpui::div()
                        .id("sharp-game-play")
                        .px(gpui::px(12.))
                        .py(gpui::px(8.))
                        .rounded(gpui::px(8.))
                        .bg(gpui::rgb(0x29251f))
                        .border_1()
                        .border_color(gpui::rgb(0x675b48))
                        .text_color(gpui::rgb(0xe8d6b7))
                        .text_size(gpui::px(11.))
                        .cursor_pointer()
                        .child("Play")
                        .on_click(cx.listener(move |this, _, _, cx| {
                            this.launch_dialog =
                                Some((launch_id.clone(), launch_executable.clone()));
                            cx.notify();
                        })),
                );
            } else {
                controls = controls.child(self.action_button(
                    "sharp-game-install",
                    "Install",
                    SharpIntent::Install {
                        source,
                        game_id: id.clone(),
                    },
                    cx,
                ));
            }
            if installed
                && !running
                && matches!(
                    source,
                    SharpSource::Installers | SharpSource::Gog | SharpSource::Epic
                )
            {
                let confirming = self.pending_uninstall.as_ref() == Some(&(source, id.clone()));
                let remove_id = id.clone();
                controls = controls.child(
                    gpui::div()
                        .id("sharp-game-remove")
                        .px(gpui::px(12.))
                        .py(gpui::px(8.))
                        .rounded(gpui::px(8.))
                        .bg(gpui::rgb(if confirming { 0x74352d } else { 0x29251f }))
                        .border_1()
                        .border_color(gpui::rgb(0x675b48))
                        .text_color(gpui::rgb(0xe8d6b7))
                        .text_size(gpui::px(11.))
                        .cursor_pointer()
                        .child(if confirming {
                            "Confirm uninstall"
                        } else {
                            "Uninstall"
                        })
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if this.pending_uninstall.as_ref() == Some(&(source, remove_id.clone()))
                            {
                                this.queue(
                                    SharpIntent::Uninstall {
                                        source,
                                        game_id: remove_id.clone(),
                                    },
                                    cx,
                                );
                                this.pending_uninstall = None;
                            } else {
                                this.pending_uninstall = Some((source, remove_id.clone()));
                                cx.notify();
                            }
                        })),
                );
            }
            main = main.child(
                card.child(
                    gpui::div()
                        .min_w(gpui::px(0.))
                        .flex_1()
                        .flex()
                        .flex_col()
                        .gap(gpui::px(5.))
                        .child(
                            gpui::div()
                                .text_size(gpui::px(14.))
                                .font_weight(gpui::FontWeight::BOLD)
                                .text_color(gpui::rgb(0xf1eee6))
                                .child(game.title.clone()),
                        )
                        .child(
                            gpui::div()
                                .text_size(gpui::px(10.))
                                .text_color(gpui::rgb(0x9ba19f))
                                .child(detail),
                        )
                        .child(
                            gpui::div()
                                .text_size(gpui::px(10.))
                                .text_color(gpui::rgb(0xb49e7a))
                                .child(if running {
                                    "Running"
                                } else if installed {
                                    "Installed"
                                } else {
                                    "Not installed"
                                }),
                        ),
                )
                .child(controls),
            );
        }
        if matches!(source, SharpSource::Gog | SharpSource::Epic) {
            main = main.child(
                gpui::div()
                    .flex()
                    .gap(gpui::px(7.))
                    .child(self.action_button(
                        "sharp-auth",
                        "Sign in / authorize",
                        SharpIntent::Authenticate(source),
                        cx,
                    ))
                    .child(self.action_button(
                        "sharp-logout",
                        "Sign out",
                        SharpIntent::Logout(source),
                        cx,
                    )),
            );
        }
        if source == SharpSource::GameJolt {
            main = main.child(
                gpui::div()
                    .flex()
                    .gap(gpui::px(7.))
                    .child(self.action_button(
                        "gamejolt-browser",
                        "Open GameJolt browser",
                        SharpIntent::Browser(GameJoltBrowserIntent::OpenPersistentWebsite),
                        cx,
                    ))
                    .child(self.action_button(
                        "gamejolt-add-root",
                        "Choose game folder",
                        SharpIntent::AddLibraryRoot(source),
                        cx,
                    )),
            );
        }
        if matches!(
            source,
            SharpSource::Pcsx2 | SharpSource::Rpcs3 | SharpSource::ShadPs4 | SharpSource::SharpEmu
        ) {
            let kind = match source {
                SharpSource::Pcsx2 => SharpImportKind::Bios,
                SharpSource::Rpcs3 => SharpImportKind::Firmware,
                SharpSource::ShadPs4 => SharpImportKind::Modules,
                _ => SharpImportKind::GameFolder,
            };
            let label = match source {
                SharpSource::Pcsx2 => "Import BIOS…",
                SharpSource::Rpcs3 => "Install firmware…",
                SharpSource::ShadPs4 => "Import compatibility files…",
                _ => "Add game layout…",
            };
            main = main.child(
                gpui::div()
                    .flex()
                    .gap(gpui::px(7.))
                    .child(self.action_button(
                        "sharp-emulator-import",
                        label,
                        SharpIntent::Import { source, kind },
                        cx,
                    ))
                    .child(self.action_button(
                        "sharp-emulator-setup",
                        "Runtime setup",
                        SharpIntent::OpenRuntimeSetup(source),
                        cx,
                    ))
                    .child(self.action_button(
                        "sharp-emulator-scan",
                        "Scan games",
                        SharpIntent::Scan(source),
                        cx,
                    )),
            );
        }
        if let Some(error) = &self.notice {
            main = main.child(
                gpui::div()
                    .text_color(gpui::rgb(0xe8d6b7))
                    .child(error.clone()),
            );
        }
        if let Some((game, executable_path)) = self.launch_dialog.clone() {
            let source = self.source;
            let dialog = gpui::div()
                .id("sharp-launch-dialog")
                .absolute()
                .inset_0()
                .flex()
                .items_center()
                .justify_center()
                .bg(gpui::rgba(0x000000aa))
                .child(
                    gpui::div()
                        .w(gpui::px(390.))
                        .p(gpui::px(20.))
                        .rounded(gpui::px(13.))
                        .bg(gpui::rgb(0x202426))
                        .border_1()
                        .border_color(gpui::rgb(0x514a3e))
                        .flex()
                        .flex_col()
                        .gap(gpui::px(12.))
                        .child(
                            gpui::div()
                                .text_size(gpui::px(16.))
                                .text_color(gpui::rgb(0xf1eee6))
                                .child(format!("Launch {game}")),
                        )
                        .child(
                            gpui::div()
                                .flex()
                                .gap(gpui::px(8.))
                                .child(
                                    gpui::div()
                                        .id("sharp-launch-fullscreen-toggle")
                                        .px(gpui::px(10.))
                                        .py(gpui::px(8.))
                                        .bg(gpui::rgb(0x29251f))
                                        .text_color(gpui::rgb(0xe8d6b7))
                                        .cursor_pointer()
                                        .child(if self.preferences.fullscreen {
                                            "✓ Fullscreen"
                                        } else {
                                            "Fullscreen"
                                        })
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            this.preferences.fullscreen =
                                                !this.preferences.fullscreen;
                                            cx.notify();
                                        })),
                                )
                                .child(
                                    gpui::div()
                                        .id("sharp-launch-network-toggle")
                                        .px(gpui::px(10.))
                                        .py(gpui::px(8.))
                                        .rounded(gpui::px(7.))
                                        .text_color(gpui::rgb(0xe8d6b7))
                                        .cursor_pointer()
                                        .child(if self.preferences.allow_network {
                                            "✓ Guest network"
                                        } else {
                                            "Guest network off"
                                        })
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            if this.source == SharpSource::SharpEmu {
                                                this.preferences.allow_network =
                                                    !this.preferences.allow_network;
                                            }
                                            cx.notify();
                                        })),
                                )
                                .child(
                                    gpui::div()
                                        .flex()
                                        .justify_end()
                                        .gap(gpui::px(8.))
                                        .child(
                                            gpui::div()
                                                .id("sharp-launch-cancel")
                                                .px(gpui::px(12.))
                                                .py(gpui::px(8.))
                                                .text_color(gpui::rgb(0xb6b9b7))
                                                .cursor_pointer()
                                                .child("Cancel")
                                                .on_click(cx.listener(|this, _, _, cx| {
                                                    this.launch_dialog = None;
                                                    cx.notify();
                                                })),
                                        )
                                        .child(
                                            gpui::div()
                                                .id("sharp-launch-confirm")
                                                .px(gpui::px(12.))
                                                .py(gpui::px(8.))
                                                .rounded(gpui::px(7.))
                                                .bg(gpui::rgb(0x5b4b34))
                                                .text_color(gpui::rgb(0xf1eee6))
                                                .cursor_pointer()
                                                .child("Launch")
                                                .on_click(cx.listener(move |this, _, _, cx| {
                                                    this.queue(
                                                        SharpIntent::Launch {
                                                            source,
                                                            game_id: game.clone(),
                                                            executable_path: executable_path
                                                                .clone(),
                                                            preferences: this.preferences.clone(),
                                                        },
                                                        cx,
                                                    );
                                                    this.launch_dialog = None;
                                                })),
                                        ),
                                ),
                        ),
                );
            main = main.child(dialog);
        }
        gpui::div()
            .size_full()
            .flex()
            .bg(gpui::rgb(0x111416))
            .child(nav)
            .child(main)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Read, Write},
        net::TcpListener,
        thread,
        time::Duration,
    };
    fn fixture(reply: &'static str) -> (SharpLibraryClient, thread::JoinHandle<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let backend = BackendClient::for_port(port).unwrap();
        let join = thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut b = Vec::new();
            let mut tmp = [0; 2048];
            loop {
                let n = s.read(&mut tmp).unwrap();
                b.extend_from_slice(&tmp[..n]);
                if let Some(h) = b.windows(4).position(|w| w == b"\r\n\r\n") {
                    let text = String::from_utf8_lossy(&b[..h]).to_ascii_lowercase();
                    let len = text
                        .lines()
                        .find_map(|l| {
                            l.strip_prefix("content-length:")
                                .and_then(|x| x.trim().parse::<usize>().ok())
                        })
                        .unwrap_or(0);
                    if b.len() >= h + 4 + len {
                        break;
                    }
                }
            }
            write!(
                s,
                "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
                reply.len(),
                reply
            )
            .unwrap();
            String::from_utf8(b).unwrap()
        });
        (SharpLibraryClient::new(backend), join)
    }
    #[test]
    fn exposes_exact_eight_source_ids() {
        assert_eq!(
            SharpSource::ALL.map(SharpSource::id),
            [
                "installers",
                "gog",
                "epic",
                "gamejolt",
                "pcsx2",
                "rpcs3",
                "shadps4",
                "sharpemu"
            ]
        )
    }
    #[test]
    fn gamejolt_storage_contract_preserves_camel_case() {
        let (c, t) = fixture(
            r#"{"ok":true,"mode":"external","rootPath":"/fixtures/jolt","gamejoltDir":"/fixtures/jolt/.metalsharp"}"#,
        );
        let x = c.gamejolt_storage().unwrap();
        assert_eq!(x.root_path, "/fixtures/jolt");
        assert_eq!(
            t.join().unwrap().split_whitespace().nth(1),
            Some("/gamejolt/storage")
        )
    }
    #[test]
    fn epic_catalog_accepts_source_contract_without_gog_status_field() {
        let (client, server) = fixture(
            r#"{"ok":true,"games":[{"appName":"Fixture","title":"Game","installed":false,"installSize":0,"bottleInitialized":false,"pipeline":"dxmt","mouseMode":"auto","running":false}]}"#,
        );
        let games = client.epic_games().unwrap();
        assert_eq!(games[0].app_name, "Fixture");
        assert_eq!(games[0].install_size, 0);
        assert!(
            server
                .join()
                .unwrap()
                .starts_with("GET /sharp-library/epic/games ")
        );
    }
    #[test]
    fn epic_oauth_code_is_posted_only_to_auth_body() {
        let (c, t) = fixture(
            r#"{"ok":true,"toolAvailable":true,"toolVersion":"1","toolPath":"/tmp/tool","authenticated":true,"configPath":"/tmp/config","gameRoot":"/tmp/games"}"#,
        );
        c.epic_auth_code("fixture-code").unwrap();
        let request = t.join().unwrap();
        assert!(request.starts_with("POST /sharp-library/epic/auth "));
        assert!(request.contains(r#"{"code":"fixture-code"}"#));
        assert!(
            !request
                .split("\r\n\r\n")
                .next()
                .unwrap()
                .contains("fixture-code")
        )
    }
    #[test]
    fn sharpemu_network_requires_explicit_lane_opt_in_and_only_sharpemu_accepts() {
        let (c, t) = fixture(r#"{"ok":true,"pid":20}"#);
        assert_eq!(
            c.emulator_launch(Emulator::Rpcs3, "fixture", true, true)
                .unwrap_err(),
            BackendError::InvalidInput
        );
        c.emulator_launch(Emulator::SharpEmu, "fixture", false, true)
            .unwrap();
        let req = t.join().unwrap();
        assert!(req.contains(r#""allowNetwork":true"#))
    }
    #[test]
    fn pcsx2_configure_whitelists_renderer_fields() {
        let (c, _) = fixture(
            r#"{"ok":true,"controller1":"none","controller2":"none","renderer":"metal","controllerOptions":[],"rendererOptions":[]}"#,
        );
        assert_eq!(
            c.pcsx2_configure("other", "x").unwrap_err(),
            BackendError::InvalidInput
        )
    }
    #[test]
    fn gamejolt_game_models_keep_snake_case_wire_contract() {
        let game:GameJoltGame=serde_json::from_str(r#"{"id":"j1","name":"Fixture","install_dir":"/tmp/game","exe_path":"/tmp/game/a.exe","installed":true,"bottleInitialized":false,"pipeline":"dxmt","mouseMode":"auto","native":false,"engine":"wine","available_pipelines":[{"id":"dxmt","name":"DXMT","recommended":true}]}"#).unwrap();
        assert_eq!(game.install_dir, "/tmp/game");
        assert_eq!(game.available_pipelines[0].id, "dxmt")
    }
    #[test]
    fn gog_nullable_catalog_fields_and_absent_optional_fields_are_safe() {
        let json = r#"{"productId":"42","title":"Fixture","platform":"windows","slug":null,"imageUrl":null,"iconUrl":null,"installRoot":null,"gameFolder":null,"executablePath":null,"primaryExe":null,"primaryTaskName":null,"installed":false,"running":false,"status":"available","downloadSizeBytes":null,"diskSizeBytes":null,"lastInstallPid":null,"lastLaunchPid":null,"lastLogPath":null,"lastError":null}"#;
        let game: GogGame = serde_json::from_str(json).unwrap();
        assert_eq!(game.product_id, "42");
        assert_eq!(game.executable_path, None);
        assert_eq!(game.last_error, None);
    }
    #[test]
    fn install_progress_deserializes_fraction_and_optional_error() {
        let progress: UpdateProgress = serde_json::from_str(r#"{"ok":true,"status":"downloading","running":true,"percent":42,"message":"Downloading runtime","targetTag":"v1.2"}"#).unwrap();
        assert_eq!(progress.percent, 42);
        assert_eq!(progress.target_tag.as_deref(), Some("v1.2"));
        assert_eq!(progress.error, None);
    }
    #[test]
    fn malformed_provider_response_is_a_safe_typed_failure() {
        let (client, server) = fixture("not-json");
        assert_eq!(client.gog_status().unwrap_err(), BackendError::InvalidJson);
        server.join().unwrap();
    }
    #[test]
    fn gog_sync_is_a_post_and_returns_catalog_status() {
        let (client, server) = fixture(
            r#"{"games":[],"status":{"status":"ready","ready":true,"authUrl":"https://auth.invalid","authenticated":true,"gogdlAvailable":true,"winePrefix":"/tmp/prefix","prefixInitialized":true,"winePath":"/tmp/wine"}}"#,
        );
        let (games, status) = client.gog_sync().unwrap();
        assert!(games.is_empty());
        assert!(status.authenticated);
        assert!(
            server
                .join()
                .unwrap()
                .starts_with("POST /sharp-library/gog/sync ")
        );
    }
    #[test]
    fn per_game_epic_action_uses_app_name_body() {
        let (client, server) = fixture(r#"{"ok":true}"#);
        client.epic_play("owned-app").unwrap();
        let req = server.join().unwrap();
        assert!(req.starts_with("POST /sharp-library/epic/play "));
        assert_eq!(
            serde_json::from_str::<Value>(req.split_once("\r\n\r\n").unwrap().1).unwrap(),
            json!({"appName":"owned-app"})
        );
    }
    #[test]
    fn gog_auth_callback_uses_its_auth_code_route() {
        let (client, server) = fixture(r#"{"ok":true}"#);
        client
            .provider_auth_callback(SharpSource::Gog, "gog-code")
            .unwrap();
        let req = server.join().unwrap();
        assert!(req.starts_with("POST /sharp-library/gog/auth-code "));
        assert!(req.contains(r#"{"code":"gog-code"}"#));
    }
    #[test]
    fn gamejolt_launch_preserves_exe_path_camelcase_contract() {
        let (c, t) = fixture(r#"{"ok":true,"pid":22}"#);
        c.gamejolt_launch("gamejolt_abc", "/owned/GameJolt/a.exe", "dxmt")
            .unwrap();
        let req = t.join().unwrap();
        assert!(req.starts_with("POST /gamejolt/launch "));
        let body: Value = serde_json::from_str(req.split_once("\r\n\r\n").unwrap().1).unwrap();
        assert_eq!(
            body,
            json!({"id":"gamejolt_abc","exePath":"/owned/GameJolt/a.exe","engine":"dxmt"})
        );
    }
    #[test]
    fn pcsx2_explicit_scan_uses_post_action_route() {
        let (c, t) = fixture(r#"{"ok":true,"games":[],"roots":[]}"#);
        c.pcsx2_scan().unwrap();
        assert!(
            t.join()
                .unwrap()
                .starts_with("POST /sharp-library/pcsx2/scan ")
        );
    }
    #[test]
    fn central_dispatch_result_wire_roundtrips_all_shapes() {
        let values = [
            SharpIntentResult::Snapshot(SharpConnectedSnapshot::default()),
            SharpIntentResult::Operation(json!({"ok":true})),
            SharpIntentResult::HostEffect(SharpHostEffect::PickImport {
                source: SharpSource::Pcsx2,
                kind: SharpImportKind::Bios,
            }),
        ];
        for value in values {
            let bytes = serde_json::to_vec(&value).unwrap();
            let decoded: SharpIntentResult = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(decoded, value);
        }
    }
    #[test]
    fn dispatcher_browser_and_picker_are_host_only_effects() {
        let client = SharpLibraryClient::new(BackendClient::for_port(1).unwrap());
        assert_eq!(
            client
                .execute_intent(&SharpIntent::Browser(
                    GameJoltBrowserIntent::OpenPersistentWebsite
                ))
                .unwrap(),
            SharpIntentResult::HostEffect(SharpHostEffect::GameJoltBrowser(
                GameJoltBrowserIntent::OpenPersistentWebsite
            ))
        );
        assert_eq!(
            client
                .execute_intent(&SharpIntent::Import {
                    source: SharpSource::Pcsx2,
                    kind: SharpImportKind::Bios
                })
                .unwrap(),
            SharpIntentResult::HostEffect(SharpHostEffect::PickImport {
                source: SharpSource::Pcsx2,
                kind: SharpImportKind::Bios
            })
        );
        assert_eq!(
            client
                .execute_intent(&SharpIntent::AddLibraryRoot(SharpSource::GameJolt))
                .unwrap(),
            SharpIntentResult::HostEffect(SharpHostEffect::PickLibraryRoot(SharpSource::GameJolt))
        );
    }
    #[test]
    fn dispatcher_installer_import_uses_c_src_path_contract() {
        let (client, server) = fixture(r#"{"ok":true}"#);
        client
            .execute_intent(&SharpIntent::ImportSelected {
                source: SharpSource::Installers,
                kind: SharpImportKind::Installer,
                path: "/fixtures/game.exe".into(),
            })
            .unwrap();
        let request = server.join().unwrap();
        assert!(request.starts_with("POST /sharp-library/install "));
        assert_eq!(
            serde_json::from_str::<Value>(request.split_once("\r\n\r\n").unwrap().1).unwrap(),
            json!({"srcPath":"/fixtures/game.exe"})
        );
    }
    #[test]
    fn dispatcher_epic_callback_returns_backend_result_without_code_in_headers() {
        let (client, server) = fixture(r#"{"ok":true,"authenticated":true}"#);
        let response = client
            .execute_intent(&SharpIntent::AuthCallback {
                source: SharpSource::Epic,
                code: "callback-secret".into(),
            })
            .unwrap();
        assert_eq!(
            response,
            SharpIntentResult::Operation(json!({"ok":true,"authenticated":true}))
        );
        let request = server.join().unwrap();
        assert!(request.starts_with("POST /sharp-library/epic/auth "));
        assert!(
            !request
                .split("\r\n\r\n")
                .next()
                .unwrap()
                .contains("callback-secret")
        );
    }
    #[test]
    fn gamejolt_browser_intent_is_explicit_and_scoped() {
        assert_eq!(
            SharpIntent::Browser(GameJoltBrowserIntent::OpenPersistentWebsite),
            SharpIntent::Browser(GameJoltBrowserIntent::OpenPersistentWebsite)
        );
        assert_eq!(GAMEJOLT_BROWSER_URL, "https://gamejolt.com/games");
        assert_eq!(
            GAMEJOLT_BROWSER_STORE_ID,
            "5C6F493A-2D9E-4A25-BEB0-7DC86791553A"
        );
        assert_eq!(
            GameJoltBrowserIntent::ClearOwnedWebsiteData,
            GameJoltBrowserIntent::ClearOwnedWebsiteData
        )
    }
}
