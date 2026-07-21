interface Game {
  id: string;
  name: string;
  exe_path: string;
  platform: string;
  steam_app_id?: number;
  size_bytes?: number;
  metalsharp_compatible: boolean;
  cover_art?: string;
}

interface SteamStatus {
  installed: boolean;
  path?: string;
  login_state?: LoginState;
  mac_installed?: boolean;
  mac_running?: boolean;
  mac_path?: string;
  mac_install_url?: string;
  running: boolean;
}

interface LoginState {
  state: "logged_in" | "logged_out" | "unknown";
  account?: { name: string; remembered: boolean }[] | null;
}

interface AppConfig {
  ok: boolean;
  launch_mode?: "native" | "wine";
  wine_available?: boolean;
  native_available: boolean;
  mono_available?: boolean;
  graphicsRuntimeLogs?: boolean;
  graphics_runtime_logs?: boolean;
}

interface UpdateStatus {
  ok: boolean;
  available: boolean;
  current_version: string;
  latest_version: string;
  download_url: string;
  download_size: number;
  release_notes: string;
  release_name: string;
  error?: string;
}

interface UpdateProgress {
  status: string;
  percent: number;
  message: string;
  error: string | null;
}

interface InstallStatus {
  phase: string;
  percent: number;
  message: string;
  error: string | null;
  new_version: string | null;
  dmg_path?: string | null;
  timestamp: number;
}

interface UpdaterReadyResult {
  ok: boolean;
  error?: string;
  scriptPath?: string;
  candidates?: string[];
}

interface CrashReportSummary {
  id: string;
  timestamp: string;
  game: string;
  exit_code: number;
}

interface SharpApp {
  id: string;
  name: string;
  exe_path: string;
  install_dir: string;
  cover: string | null;
  cover_position_x: number;
  cover_position_y: number;
  engine: string;
  launch_args: string[];
  user_launch_args: string[];
  bottle_id?: string | null;
  installed_at: string;
  size_bytes: number;
}

interface BackendResponse {
  ok: boolean;
  data?: unknown;
  error?: string;
  pid?: number;
  games?: unknown[];
  drives?: unknown[];
  prefixes?: unknown[];
  mapped?: number;
}

interface SetupState {
  ok: boolean;
  completed: boolean;
  savedCompleted?: boolean;
  step: number;
  deviceName: string;
  steamApiKeySet: boolean;
  runtimeMigrationRequired?: boolean;
  dxmtRuntime?: {
    current: boolean;
    filesReady: boolean;
    installedVersion?: string | null;
    requiredVersion: string;
    manifestPath: string;
  };
}

interface Dependency {
  id: string;
  name: string;
  desc: string;
  installed: boolean;
  required: boolean;
  installCmd: string;
}

interface DependenciesResponse {
  ok: boolean;
  allInstalled: boolean;
  dependencies: Dependency[];
}

interface ProcessManagerProcess {
  pid: number;
  name: string;
  command: string;
  cpu_percent: number;
  mem_percent: number;
  fps: number | null;
  fps_fresh: boolean;
}

interface ProcessManagerSample {
  ok: boolean;
  source: string;
  timestamp?: number;
  fps: number | null;
  fps_source?: string;
  fps_fresh?: boolean;
  cpu_percent: number;
  cpu_temp_c: number | null;
  cpu_temp_source?: string;
  cores_used: number;
  cores_total: number;
  ram_used_bytes: number;
  ram_total_bytes: number;
  gpu_percent: number | null;
  gpu_label?: string;
  chip?: string;
  processes: ProcessManagerProcess[];
  helper_path?: string;
}

type ProcessManagerAction = "metalfx" | "gpu-acceleration" | "quit-game" | "view-steam";

interface MetalFxState {
  ok: boolean;
  enabled: boolean;
  factor: number;
  conf_factor?: number;
  source?: string;
  applies?: string;
  error?: string;
}

interface ProcessManagerActionResult {
  ok: boolean;
  error?: string;
  visualOnly?: boolean;
  action?: ProcessManagerAction;
  killed?: number[];
  errors?: string[];
  skippedSteamWinePids?: number;
  mode?: string;
}

type MetalsharpAPI = {
  request: (
    method: string,
    url: string,
    body?: Record<string, unknown>,
    timeoutMs?: number,
  ) => Promise<BackendResponse>;
  isFirstLaunch: () => Promise<boolean>;
  isMigrationMode: () => Promise<boolean>;
  restartAfterMigration: () => Promise<{ ok: boolean; error?: string; deletedDmg?: string | null; launched?: string }>;
  ejectDmg: () => Promise<void>;
  installDeps: (command: string) => Promise<{ ok: boolean; error?: string }>;
  installHomebrew: () => Promise<{ ok: boolean; installed?: boolean; path?: string; message?: string; error?: string }>;
  homebrewStatus: () => Promise<{ installed: boolean; path?: string }>;
  openInFinder: (path: string) => Promise<void>;
  openLogsFolder: () => Promise<{ ok: boolean; path?: string; error?: string }>;
  openMetalsharpFolder: () => Promise<{ ok: boolean; path?: string; error?: string }>;
  repairDataAccess: () => Promise<{
    ok: boolean;
    path?: string;
    checks?: { dir: string; ok: boolean; error?: string }[];
    error?: string;
  }>;
  copyText: (text: string) => Promise<{ ok: boolean; error?: string }>;
  restartBackend: () => Promise<{ ok: boolean; error?: string }>;
  isBackendAlive: () => Promise<boolean>;
  updaterEnsureReady: () => Promise<UpdaterReadyResult>;
  updaterSpawnInstall: (
    dmgPath: string,
    backendPid: number,
    targetVersion: string,
  ) => Promise<{ ok: boolean; error?: string }>;
  updaterInstallStatus: () => Promise<InstallStatus | null>;
  updaterClearStatus: () => Promise<void>;
  backendGetPid: () => Promise<number | null>;
  migrateCheck: () => Promise<BackendResponse>;
  migrateStart: () => Promise<BackendResponse>;
  migrateProgress: () => Promise<BackendResponse>;
  quitApp: () => void;
  uninstallApp: () => void;
  pickExeFile: () => Promise<string | null>;
  pickImageFile: () => Promise<string | null>;
  pickDirectory: (title?: string) => Promise<string | null>;
  gogOAuthLogin: (authUrl: string) => Promise<{ ok: boolean; code?: string; redirectUrl?: string; error?: string }>;
  processManagerToggle: () => Promise<{ ok: boolean }>;
  processManagerClose: () => Promise<{ ok: boolean }>;
  processManagerSample: () => Promise<ProcessManagerSample>;
  processManagerAction: (action: ProcessManagerAction) => Promise<ProcessManagerActionResult>;
};
