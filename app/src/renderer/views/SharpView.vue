<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useToast } from "../composables/useToast";
import { api, getAPI } from "../composables/useApi";
import type { SharpApp } from "../api-types";
import { themedNavIcon } from "../composables/useTheme";
import IconUpload from "~icons/lucide/upload";
import IconMonitor from "~icons/lucide/monitor";
import IconX from "~icons/lucide/x";
import IconPencil from "~icons/lucide/pencil";
import IconDownload from "~icons/lucide/download";
import IconExternalLink from "~icons/lucide/external-link";
import IconFolderPlus from "~icons/lucide/folder-plus";
import IconGamepad2 from "~icons/lucide/gamepad-2";
import IconHardDrive from "~icons/lucide/hard-drive";
import IconPackage from "~icons/lucide/package";
import IconScanLine from "~icons/lucide/scan-line";
import IconShieldCheck from "~icons/lucide/shield-check";
import sharpLogoUrl from "../icon.png";

const refreshIcon = computed(() => themedNavIcon("refresh"));

interface LaunchDoctorCheck {
  id: string;
  label: string;
  ok: boolean;
  detail: string;
}

interface LaunchDoctorReport {
  ready: boolean;
  summary: string;
  blockers: string[];
  warnings: string[];
  checks: LaunchDoctorCheck[];
  recipe: {
    pipeline: string;
    pipeline_name: string;
    backend: string;
    exe_name?: string | null;
    launch_args: string[];
  };
}

interface LogEntry {
  name: string;
  lines: string[];
}

interface CrashReport {
  file: string;
  name: string;
  source: string;
  timestamp: string;
  size_bytes: number;
}

interface BottleAction {
  id: string;
  status: string;
  detail: string;
}

interface RuntimeProfileDefinition {
  id: string;
  name: string;
  components: string[];
}

interface BottleManifest {
  id: string;
  name: string;
  bottle_type: string;
  steam_app_id?: number | null;
  arch: string;
  runtime_profile: string;
  health: string;
  prefix_path: string;
  source_installer_path?: string | null;
  game_install_path?: string | null;
  runtime_assets: { id: string; kind: string; source_path: string; present: boolean }[];
  last_launch_log?: string | null;
  last_launch_pid?: number | null;
  last_launch_status?: string | null;
  last_launch_finished_at?: string | null;
  installed_components: { id: string; state: string }[];
  installed_app_detections: { name: string; exe_path: string; source: string }[];
}

interface M12DryRun {
  ok: boolean;
  dry_run: boolean;
  missing?: Array<{ filename?: string }>;
}

interface BottleDiagnostic {
  id: string;
  ready: boolean;
  summary: string;
  actions: BottleAction[];
  checks: { id: string; ok: boolean; detail: string }[];
  component_sources?: { id: string; source: string; available: boolean; detail: string; path?: string | null }[];
}

interface ComponentRepair {
  id: string;
  status: string;
  detail: string;
  asset_path?: string | null;
  log_path?: string | null;
  pid?: number | null;
}

interface D3DMetalGptkAction {
  id: string;
  label: string;
  enabled: boolean;
  state: string;
  detail: string;
}

interface D3DMetalGptkState {
  bottle_id: string;
  appid: number;
  name: string;
  game_dir: string;
  game_exe?: string | null;
  gptk_homebrew: string;
  rosetta: string;
  gptk_payload: string;
  x64_redist: string;
  seed: string;
  play_ready: boolean;
  last_error?: string | null;
}

interface D3DMetalLaunchReport {
  pid?: number;
  appid?: number;
  bottle_id?: string;
  game_exe?: string;
  log_path?: string;
}

interface D3DMetalGptkResponse {
  ok: boolean;
  state?: D3DMetalGptkState;
  actions?: D3DMetalGptkAction[];
  launch?: D3DMetalLaunchReport;
  error?: string;
}

interface GogStatus {
  status: string;
  ready: boolean;
  authUrl: string;
  authenticated: boolean;
  gogdlAvailable: boolean;
  gogdlPath?: string | null;
  oauthHelperPath?: string | null;
  oauthHelperAvailable?: boolean;
  oauthHelperScript?: string | null;
  winePrefix: string;
  prefixInitialized: boolean;
  winePath: string;
}

interface GameJoltStorage {
  mode: "internal" | "external";
  rootPath: string;
  gamejoltDir: string;
}

interface GameJoltGame {
  id: string;
  name: string;
  install_dir: string;
  exe_path: string;
  installed: boolean;
  native: boolean;
  engine: string;
  cover_path?: string | null;
  bottle_id?: string;
  available_pipelines: { id: string; name: string; recommended?: boolean }[];
}

interface GogGame {
  productId: string;
  title: string;
  platform: string;
  slug?: string | null;
  imageUrl?: string | null;
  iconUrl?: string | null;
  installRoot?: string | null;
  gameFolder?: string | null;
  primaryExe?: string | null;
  primaryTaskName?: string | null;
  installed: boolean;
  running: boolean;
  status: string;
  downloadSizeBytes?: number | null;
  diskSizeBytes?: number | null;
  lastInstallPid?: number | null;
  lastLaunchPid?: number | null;
  lastLogPath?: string | null;
  lastError?: string | null;
}

interface Rpcs3Status {
  ok: boolean;
  installed: boolean;
  state: "ready" | "missing_firmware" | "not_installed";
  architecture: string;
  currentTag?: string | null;
  rollbackAvailable: boolean;
  firmwareInstalled: boolean;
  environmentPath: string;
  dataPath: string;
  cachePath: string;
  executablePath?: string | null;
}

interface Rpcs3Game {
  id: string;
  titleId: string;
  title: string;
  version: string;
  category: string;
  path: string;
  installedTitle: boolean;
  hasArtwork: boolean;
  running: boolean;
  pid?: number | null;
  lastLogPath?: string | null;
}

interface Rpcs3Update {
  ok: boolean;
  currentTag?: string | null;
  latestTag: string;
  latestVersion: string;
  available: boolean;
  pinnedTag?: string | null;
  skippedTag?: string | null;
  suppressed?: "pinned" | "skipped" | "none";
  assetName: string;
  downloadSize: number;
  digest: string;
  publishedAt: string;
  error?: string;
}

interface Rpcs3UpdateProgress {
  ok: boolean;
  status: string;
  running: boolean;
  percent: number;
  message: string;
  error?: string | null;
  targetTag?: string | null;
}

type SharpSource = "installers" | "gog" | "gamejolt" | "rpcs3";

const toast = useToast();
const sourceMode = ref<SharpSource>("installers");
const sourceTabs = [
  { id: "installers" as const, label: "Installers" },
  { id: "gog" as const, label: "GOG" },
  { id: "gamejolt" as const, label: "GameJolt" },
  { id: "rpcs3" as const, label: "RPCS3" },
];
const headerTitle = computed(() => {
  if (sourceMode.value === "gog") return "GOG Games Library";
  if (sourceMode.value === "gamejolt") return "GameJolt Library";
  if (sourceMode.value === "rpcs3") return "RPCS3 Library";
  return "Sharp Library";
});
const headerSubtitle = computed(() => {
  if (sourceMode.value === "gog") return "Connect, sync, install, and play GOG games through MetalSharp.";
  if (sourceMode.value === "gamejolt") return "Play GameJolt games from internal or external GameJolt storage.";
  if (sourceMode.value === "rpcs3")
    return "Install, update, configure, and launch PlayStation 3 games in an isolated environment.";
  return "Install and manage Windows applications outside Steam.";
});
const apps = ref<SharpApp[]>([]);
const cardToolsOpen = ref<Record<string, boolean>>({});
const bottles = ref<BottleManifest[]>([]);
const runtimeProfiles = ref<RuntimeProfileDefinition[]>([]);
const bottleReports = ref<Record<string, BottleDiagnostic | null>>({});
const d3dmetalStates = ref<Record<string, D3DMetalGptkState | null>>({});
const d3dmetalActions = ref<Record<string, D3DMetalGptkAction[]>>({});
const bottleLoading = ref<Record<string, boolean>>({});
const bottleAdvancedOpen = ref<Record<string, boolean>>({});
const doctorOpen = ref<Record<string, boolean>>({});
const doctorLoading = ref<Record<string, boolean>>({});
const doctorReports = ref<Record<string, LaunchDoctorReport | null>>({});
const diagnosticsOpen = ref<Record<string, boolean>>({});
const diagnosticsLoading = ref<Record<string, boolean>>({});
const launchErrors = ref<Record<string, string>>({});
const runningSharpPids = ref<Record<string, number>>({});
const recentLogLines = ref<Record<string, string[]>>({});
const recentCrashReports = ref<Record<string, CrashReport[]>>({});
const gogStatus = ref<GogStatus | null>(null);
const gogGames = ref<GogGame[]>([]);
let savedGogEngines: Record<string, string> = {};
try {
  savedGogEngines = JSON.parse(localStorage.getItem("metalsharp-gog-engines") ?? "{}");
} catch {}
const gogEngines = ref<Record<string, string>>(savedGogEngines);
const gamejoltGames = ref<GameJoltGame[]>([]);
const gamejoltStorage = ref<GameJoltStorage | null>(null);
const gamejoltLoading = ref(false);
const gamejoltRunningPids = ref<Record<string, number>>({});
const gamejoltBrowserHeight = ref(24);
const gamejoltBrowserDragging = ref(false);
const gamejoltPanel = ref<HTMLElement | null>(null);
let gamejoltDragPointerId: number | null = null;
const gamejoltDownloadToastIds = new Map<string, number>();
let gamejoltProcessPollTimer: ReturnType<typeof setInterval> | null = null;
const rpcs3Status = ref<Rpcs3Status | null>(null);
const rpcs3Games = ref<Rpcs3Game[]>([]);
const rpcs3Roots = ref<string[]>([]);
const rpcs3Update = ref<Rpcs3Update | null>(null);
const rpcs3UpdateProgress = ref<Rpcs3UpdateProgress | null>(null);
const rpcs3Loading = ref<Record<string, boolean>>({});
const rpcs3BuildLabel = computed(() => {
  const tag = rpcs3Status.value?.currentTag;
  if (!tag) return "Not installed";
  const normalized = tag.startsWith("build-") ? tag.slice(6) : tag;
  return `Build ${normalized.slice(0, 10)}`;
});
const rpcs3StateLabel = computed(() => {
  if (rpcs3Status.value?.state === "ready") return "Ready to play";
  if (rpcs3Status.value?.installed) return "Firmware required";
  return "Setup required";
});
let rpcs3ProcessPollTimer: ReturnType<typeof setInterval> | null = null;
let rpcs3UpdatePollTimer: ReturnType<typeof setInterval> | null = null;
const editingGameJoltName = ref<string | null>(null);
const gameJoltNameDraft = ref("");
const gogLoading = ref<Record<string, boolean>>({});
const gogProgress = ref<Record<string, number>>({});

interface WineMonoStatus {
  latestVersion: string;
  installedVersion?: string | null;
  installed: boolean;
  upToDate: boolean;
  running: boolean;
  stalled?: boolean;
  pid?: number | null;
  logPath?: string | null;
  targetVersion: string;
  startedAt?: number | null;
  elapsedSeconds?: number | null;
  lastError?: string | null;
  msiCached: boolean;
  downloading: boolean;
  downloadBytes: number;
  downloadTotal: number;
  downloadError?: string | null;
}
const gogMonoStatus = ref<WineMonoStatus | null>(null);
const gogMonoPollHandle = ref<ReturnType<typeof setInterval> | null>(null);

const showGogInstallMono = computed(() => {
  if (!gogStatus.value?.prefixInitialized) return false;
  const status = gogMonoStatus.value;
  if (!status) return true; // unknown — show until we confirm it is up to date
  return !status.upToDate;
});

async function refreshGogMonoStatus() {
  const result = await api<WineMonoStatus>("GET", "/wine-mono/status?prefix=gog");
  if (result?.ok) gogMonoStatus.value = result;
}

async function installGogMono() {
  if (!gogStatus.value?.prefixInitialized) {
    toast.show("Initialize the GOG prefix first", "error");
    return;
  }
  gogLoading.value.mono = true;
  // Short timeout — the backend now returns immediately (kicks off download or launches installer).
  const result = await api<{
    ok: boolean;
    pid?: number;
    alreadyInstalled?: boolean;
    downloading?: boolean;
    error?: string;
    status?: WineMonoStatus;
  }>("POST", "/wine-mono/install", { prefix: "gog" }, 30 * 1000);
  gogLoading.value.mono = false;
  if (result?.ok) {
    if (result.alreadyInstalled) {
      await refreshGogMonoStatus();
      toast.show("Wine Mono is already up to date", "success");
      return;
    }
    if (result.downloading) {
      // Backend kicked off async download — poll for progress.
      startGogMonoPoll();
      return;
    }
    // Installer launched.
    toast.show("Wine Mono installer launched — complete it in the Wine window", "success");
    startGogMonoPoll();
  } else {
    const errMsg = result?.error ?? "Failed to launch Wine Mono installer";
    toast.show(errMsg, "error", 12_000);
    await refreshGogMonoStatus();
  }
}

async function resetGogMonoInstall() {
  gogLoading.value.mono = true;
  const result = await api<{ ok: boolean; killedProcesses?: number; error?: string }>(
    "POST",
    "/wine-mono/reset",
    { prefix: "gog" },
    15 * 1000,
  );
  gogLoading.value.mono = false;
  await refreshGogMonoStatus();
  if (result?.ok) {
    const killed = result.killedProcesses ?? 0;
    toast.show(
      killed > 0
        ? `Cleared ${killed} stuck Mono processes; click Install Mono to retry`
        : "Mono install state cleared; click Install Mono to retry",
      "success",
    );
  } else {
    toast.show(result?.error ?? "Failed to reset Wine Mono install", "error");
  }
}

function formatElapsedSeconds(seconds: number | null | undefined): string {
  if (!seconds || seconds < 0) return "0:00";
  const total = Math.floor(seconds);
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${s.toString().padStart(2, "0")}`;
}

function gogMonoButtonLabel(): string {
  const s = gogMonoStatus.value;
  if (!s) return "Install Mono";
  if (s.downloading && s.downloadTotal > 0) {
    const raw = (s.downloadBytes / s.downloadTotal) * 100;
    const pct = Math.min(100, Math.floor(raw / 5) * 5);
    return `Downloading ${pct}%…`;
  }
  if (s.downloading) return "Downloading Mono…";
  if (s.running && s.stalled) return "Stalled — tap to retry";
  if (s.running) {
    const elapsed = formatElapsedSeconds(s.elapsedSeconds);
    return `Running… ${elapsed}`;
  }
  if (gogLoading.value.mono) return "Installing Mono…";
  return "Install Mono";
}

function gogMonoButtonTitle(): string {
  const s = gogMonoStatus.value;
  if (!s) return "Install Wine Mono";
  if (s.downloadError) {
    return `Wine Mono download failed: ${s.downloadError}. Click to retry.`;
  }
  if (s.stalled) {
    return s.lastError
      ? `Wine Mono installer stalled: ${s.lastError}. Click to retry, or use Reset to clear stuck processes.`
      : "Wine Mono installer stalled. Click to retry, or use Reset to clear stuck processes.";
  }
  const base = `Wine Mono ${s.installedVersion ?? "not installed"} → ${s.latestVersion}`;
  return s.lastError ? `${base}. ${s.lastError}` : base;
}

function startGogMonoPoll() {
  if (gogMonoPollHandle.value) return;
  let stalledToastShown = false;
  gogMonoPollHandle.value = setInterval(async () => {
    await refreshGogMonoStatus();
    const status = gogMonoStatus.value;
    if (!status) return;

    // If the backend reports a hung install, surface a single recovery toast
    // and let the user decide whether to retry or reset.
    if (status.stalled === true && !stalledToastShown) {
      stalledToastShown = true;
      toast.show(
        status.lastError
          ? `Wine Mono installer stalled (${status.lastError}); tap Reset to clear or Install Mono to retry`
          : "Wine Mono installer stalled; tap Reset to clear or Install Mono to retry",
        "error",
        10_000,
      );
    }
    if (!status.stalled) {
      stalledToastShown = false;
    }

    // Download completed successfully → trigger the installer.
    if (status.msiCached && !status.downloading && !status.running && !status.upToDate && !status.stalled) {
      stopGogMonoPoll();
      gogLoading.value.mono = true;
      await installGogMono();
      return;
    }

    if (status.upToDate) {
      stopGogMonoPoll();
      toast.show(`Wine Mono ${status.latestVersion} installed`, "success");
    } else if (!status.running && !status.downloading) {
      // Installer exited without landing the latest version (user cancelled).
      stopGogMonoPoll();
      if (status.lastError) {
        toast.show(status.lastError, "error", 12_000);
      }
    }
  }, 3000);
}

function stopGogMonoPoll() {
  if (gogMonoPollHandle.value) {
    clearInterval(gogMonoPollHandle.value);
    gogMonoPollHandle.value = null;
  }
}
const engineOptions = [
  { id: "d3dmetal", name: "D3DMetal" },
  { id: "vkd3d", name: "VKD3D" },
  { id: "m11", name: "M11" },
  { id: "m11_32", name: "M11(32)" },
  { id: "m10", name: "M10" },
  { id: "m10_32", name: "M10(32)" },
  { id: "m9", name: "M9" },
  { id: "fna_arm64", name: "Mono/FNA" },
];

const componentDisplayName: Record<string, string> = {
  "mono-arm64": "Mono ARM64",
  "mono-x86": "Mono x86_64",
  fna: "FNA Runtime",
  xna: "XNA Assemblies",
  sdl2: "SDL2",
  fna3d: "FNA3D",
  faudio: "FAudio",
  fmod: "FMOD Audio",
  m12_d3d12: "M12 d3d12.dll",
  m12_d3d11: "M12 d3d11.dll",
  m12_d3d10core: "M12 d3d10core.dll",
  m12_dxgi_dxmt: "M12 dxgi_dxmt.dll",
  m12_dxgi: "M12 dxgi.dll",
  m12_winemetal: "M12 winemetal.dll / .so",
  m12_gpu_stubs: "M12 GPU Stubs",
  vkd3d_d3d12: "VKD3D d3d12.dll",
  vkd3d_d3d12core: "VKD3D d3d12core.dll",
  vkd3d_dxgi: "VKD3D dxgi.dll",
  dxvk_d3d11: "DXVK d3d11.dll",
  dxvk_d3d10core: "DXVK d3d10core.dll",
  d3d12_agility: "D3D12 Agility",
  gpu_vendor_stubs: "GPU Stubs",
  gptk_amd_stub: "GPTK AMD Stub",
  d3d10core: "D3D10core",
  winemetal: "Winemetal",
  gptk: "GPTK",
  gptk_prefix: "GPTK Prefix",
  rosetta: "Rosetta",
  corefonts: "Core Fonts",
  vcrun2019_x64: "VC++ 2015-2022 x64",
  vcrun2019_x86: "VC++ 2015-2022 x86",
  vcrun2019: "VC++ 2015-2022",
  vcrun2010: "VC++ 2010",
  vcrun2013: "VC++ 2013",
  dotnet40: ".NET 4.0",
  dotnet48: ".NET 4.8",
  webview2: "WebView2",
  directx_jun2010: "DX Jun2010",
  openal: "OpenAL",
  physx: "PhysX",
};

const fnaComponentIds = new Set(["mono-arm64", "mono-x86", "fna", "xna", "sdl2", "fna3d", "faudio", "fmod"]);

function componentLabel(id: string): string {
  return componentDisplayName[id] ?? id;
}

function componentStateClass(state: string): string {
  if (state === "installed" || state === "ready") return "pill-ok";
  if (state === "missing") return "pill-missing";
  if (state === "needs_repair" || state === "partial") return "pill-warn";
  return "pill-unknown";
}

function d3dmetalActionReady(action: D3DMetalGptkAction): boolean {
  return ["installed", "updated", "seeded"].includes(action.state);
}

function isFnaProfile(profile: string): boolean {
  return profile === "fna_arm64" || profile === "fna_x86";
}
const selectableRuntimeProfileIds = new Set(["d3dmetal", "vkd3d", "m11", "m11_32", "m10", "m10_32", "m9", "fna_arm64"]);
const visibleRuntimeProfiles = computed(() => {
  const profiles = runtimeProfiles.value.some((profile) => profile.id === "d3dmetal")
    ? runtimeProfiles.value
    : [
        ...runtimeProfiles.value,
        {
          id: "d3dmetal",
          name: "D3DMetal (GPTK)",
          components: ["gptk", "rosetta", "gptk_prefix", "vcrun2019_x64", "vcrun2019_x86"],
        },
      ];
  return profiles
    .filter((profile) => selectableRuntimeProfileIds.has(profile.id))
    .map((profile) => ({
      ...profile,
      name: profile.id === "fna_arm64" ? "Mono/FNA" : profile.name.replace(/^D3D(\d+) Metal$/, "M$1"),
    }));
});

function sharpAppNameSort(a: SharpApp, b: SharpApp) {
  return a.name.localeCompare(b.name, undefined, { sensitivity: "base", numeric: true });
}

async function loadGameJolt() {
  const [gamesResult, storageResult] = await Promise.all([
    api<{ ok: boolean; games: GameJoltGame[]; storage?: GameJoltStorage }>("GET", "/gamejolt"),
    api<{ ok: boolean; mode: "internal" | "external"; rootPath: string; gamejoltDir: string }>(
      "GET",
      "/gamejolt/storage",
    ),
  ]);
  if (gamesResult?.ok) {
    gamejoltGames.value = [...(gamesResult.games ?? [])].sort((a, b) =>
      a.name.localeCompare(b.name, undefined, { sensitivity: "base", numeric: true }),
    );
    if (gamesResult.storage) gamejoltStorage.value = gamesResult.storage;
  }
  if (storageResult?.ok) gamejoltStorage.value = storageResult;
}

async function syncGameJolt(showResult = true) {
  gamejoltLoading.value = true;
  const result = await api<{ ok: boolean; games: GameJoltGame[]; storage?: GameJoltStorage; error?: string }>(
    "POST",
    "/gamejolt/sync",
  );
  gamejoltLoading.value = false;
  if (!result?.ok) {
    if (showResult) toast.show(result?.error ?? "GameJolt scan failed", "error");
    return;
  }
  gamejoltGames.value = [...(result.games ?? [])].sort((a, b) => a.name.localeCompare(b.name));
  if (result.storage) gamejoltStorage.value = result.storage;
  if (showResult)
    toast.show(
      `Found ${gamejoltGames.value.length} GameJolt game${gamejoltGames.value.length === 1 ? "" : "s"}`,
      "success",
    );
}

async function chooseGameJoltStorage() {
  const rootPath = await getAPI().pickDirectory("Choose the parent folder for GameJolt games");
  if (!rootPath) return;
  const result = await api<{
    ok: boolean;
    mode: "internal" | "external";
    rootPath: string;
    gamejoltDir: string;
    error?: string;
  }>("POST", "/gamejolt/storage", { rootPath });
  if (result?.ok) {
    gamejoltStorage.value = result;
    await syncGameJolt();
  } else {
    toast.show(result?.error ?? "Could not change GameJolt storage", "error");
  }
}

function beginGameJoltNameEdit(game: GameJoltGame) {
  editingGameJoltName.value = game.id;
  gameJoltNameDraft.value = game.name;
}

async function saveGameJoltName(game: GameJoltGame) {
  if (editingGameJoltName.value !== game.id) return;
  const name = gameJoltNameDraft.value.trim();
  editingGameJoltName.value = null;
  if (!name || name === game.name) return;
  const result = await api<{ ok: boolean; name?: string; error?: string }>("POST", "/gamejolt/name", {
    id: game.id,
    name,
  });
  if (result?.ok && result.name) game.name = result.name;
  else toast.show(result?.error ?? "Could not save GameJolt name", "error");
}

async function launchGameJolt(game: GameJoltGame) {
  toast.show(`Launching ${game.name}...`);
  const result = await api<{ ok: boolean; pid?: number; error?: string }>("POST", "/gamejolt/launch", {
    id: game.id,
    exePath: game.exe_path,
    engine: game.native ? "native" : game.engine,
  });
  if (result?.ok && result.pid) {
    gamejoltRunningPids.value[game.id] = result.pid;
    toast.show(`Launched ${game.name}`, "success");
  } else {
    toast.show(result?.error ?? `Failed to launch ${game.name}`, "error");
  }
}

async function stopGameJolt(game: GameJoltGame) {
  const pid = gamejoltRunningPids.value[game.id];
  if (!pid) return;
  await api("POST", "/kill", { pid });
  delete gamejoltRunningPids.value[game.id];
}

async function uninstallGameJolt(game: GameJoltGame) {
  if (gamejoltRunningPids.value[game.id]) return;
  if (!window.confirm(`Uninstall ${game.name}? This removes its GameJolt folder and cannot be undone.`)) return;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/gamejolt/uninstall", {
    id: game.id,
    installDir: game.install_dir,
  });
  if (!result?.ok) {
    toast.show(result?.error ?? `Could not uninstall ${game.name}`, "error");
    return;
  }
  delete gamejoltRunningPids.value[game.id];
  await syncGameJolt(false);
  toast.show(`${game.name} uninstalled`, "success");
}

async function refreshGameJoltProcessState() {
  const entries = Object.entries(gamejoltRunningPids.value);
  await Promise.all(
    entries.map(async ([id, pid]) => {
      const result = await api<{ ok: boolean; running: boolean }>("POST", "/gamejolt/status", { pid });
      if (!result?.running) delete gamejoltRunningPids.value[id];
    }),
  );
}

async function updateGameJoltEngine(game: GameJoltGame, engine: string) {
  const previous = game.engine;
  game.engine = engine;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/gamejolt/engine", {
    id: game.id,
    engine,
  });
  if (!result?.ok) {
    game.engine = previous;
    toast.show(result?.error ?? "Could not save GameJolt launch option", "error");
  }
}

function handleGameJoltDownload(update: GameJoltDownloadUpdate) {
  let toastId = gamejoltDownloadToastIds.get(update.id);
  const total = update.totalBytes ?? 0;
  const received = update.receivedBytes ?? 0;
  const progress = total > 0 ? Math.min(0.99, received / total) : update.state === "organizing" ? 0.99 : 0;
  if (toastId === undefined) {
    toastId = toast.showDownload(`Downloading ${update.filename}...`, progress);
    gamejoltDownloadToastIds.set(update.id, toastId);
  }
  if (update.state === "downloading") {
    const percent = total > 0 ? `${Math.round((received / total) * 100)}%` : "Starting...";
    toast.updateDownload(toastId, `Downloading ${update.filename} — ${percent}`, progress);
  } else if (update.state === "organizing") {
    toast.updateDownload(toastId, `Finishing ${update.filename}...`, 0.99);
  } else if (update.state === "completed") {
    toast.finishDownload(toastId, `${update.filename} downloaded`, true);
    gamejoltDownloadToastIds.delete(update.id);
    void loadGameJolt();
  } else if (update.state === "failed") {
    toast.finishDownload(toastId, update.error ?? `Could not download ${update.filename}`, false);
    gamejoltDownloadToastIds.delete(update.id);
  }
}

function beginGameJoltBrowserDrag(event: PointerEvent) {
  if (!gamejoltPanel.value) return;
  event.preventDefault();
  gamejoltBrowserDragging.value = true;
  gamejoltDragPointerId = event.pointerId;
  updateGameJoltBrowserHeight(event.clientY);
  window.addEventListener("pointermove", moveGameJoltBrowserDrag);
  window.addEventListener("pointerup", endGameJoltBrowserDrag);
  window.addEventListener("pointercancel", endGameJoltBrowserDrag);
}

function updateGameJoltBrowserHeight(clientY: number) {
  const panel = gamejoltPanel.value;
  if (!panel) return;
  const bounds = panel.getBoundingClientRect();
  const height = ((bounds.bottom - clientY) / bounds.height) * 100;
  gamejoltBrowserHeight.value = Math.max(10, Math.min(100, height));
}

function gameJoltPanelStyle() {
  const panelHeight = gamejoltPanel.value?.getBoundingClientRect().height ?? window.innerHeight - 220;
  const browserSpace = Math.ceil((panelHeight * gamejoltBrowserHeight.value) / 100) + 24;
  return {
    "--gamejolt-browser-height": `${gamejoltBrowserHeight.value}%`,
    "--gamejolt-browser-space": `${browserSpace}px`,
  };
}

function moveGameJoltBrowserDrag(event: PointerEvent) {
  if (!gamejoltBrowserDragging.value || event.pointerId !== gamejoltDragPointerId) return;
  updateGameJoltBrowserHeight(event.clientY);
}

function endGameJoltBrowserDrag(event: PointerEvent) {
  if (event.pointerId !== gamejoltDragPointerId) return;
  gamejoltBrowserDragging.value = false;
  gamejoltDragPointerId = null;
  window.removeEventListener("pointermove", moveGameJoltBrowserDrag);
  window.removeEventListener("pointerup", endGameJoltBrowserDrag);
  window.removeEventListener("pointercancel", endGameJoltBrowserDrag);
}

async function refreshRpcs3(showResult = false) {
  const [statusResult, gamesResult] = await Promise.all([
    api<Rpcs3Status>("GET", "/sharp-library/rpcs3/status"),
    api<{ ok: boolean; games: Rpcs3Game[]; roots: string[] }>("GET", "/sharp-library/rpcs3/games"),
  ]);
  if (statusResult?.ok) rpcs3Status.value = statusResult;
  if (gamesResult?.ok) {
    rpcs3Games.value = [...(gamesResult.games ?? [])].sort((a, b) =>
      a.title.localeCompare(b.title, undefined, { sensitivity: "base", numeric: true }),
    );
    rpcs3Roots.value = gamesResult.roots ?? [];
  }
  if (showResult)
    toast.show(`Found ${rpcs3Games.value.length} RPCS3 game${rpcs3Games.value.length === 1 ? "" : "s"}`, "success");
}

async function checkRpcs3Update(showResult = true, force = showResult) {
  rpcs3Loading.value.check = true;
  const result = await api<Rpcs3Update>(
    force ? "POST" : "GET",
    force ? "/sharp-library/rpcs3/update/refresh" : "/sharp-library/rpcs3/update/check",
    force ? {} : undefined,
    30 * 1000,
  );
  rpcs3Loading.value.check = false;
  if (!result?.ok) {
    if (showResult) toast.show(result?.error ?? "Could not check RPCS3 updates", "error");
    return null;
  }
  rpcs3Update.value = result;
  if (showResult) {
    toast.show(
      result.available
        ? `${rpcs3Status.value?.installed ? "RPCS3 update" : "RPCS3"} ${result.latestVersion} is available`
        : result.suppressed === "pinned"
          ? "RPCS3 updates are pinned to the current build"
          : result.suppressed === "skipped"
            ? "The latest RPCS3 build is skipped"
            : "RPCS3 is up to date",
      result.available ? "success" : "info",
    );
  }
  return result;
}

function stopRpcs3UpdatePoll() {
  if (rpcs3UpdatePollTimer) clearInterval(rpcs3UpdatePollTimer);
  rpcs3UpdatePollTimer = null;
}

function beginRpcs3UpdatePoll() {
  stopRpcs3UpdatePoll();
  rpcs3UpdatePollTimer = setInterval(async () => {
    const progress = await api<Rpcs3UpdateProgress>("GET", "/sharp-library/rpcs3/update/progress");
    if (!progress?.ok) return;
    rpcs3UpdateProgress.value = progress;
    if (progress.status === "completed" || progress.status === "failed") {
      stopRpcs3UpdatePoll();
      rpcs3Loading.value.update = false;
      if (progress.status === "completed") {
        toast.show("RPCS3 installed successfully", "success");
        await refreshRpcs3();
        await checkRpcs3Update(false);
      } else toast.show(progress.error ?? "RPCS3 update failed", "error");
    }
  }, 1000);
}

async function installOrUpdateRpcs3() {
  const release = await checkRpcs3Update(false);
  if (!release) return;
  if (rpcs3Status.value?.installed && !release.available) {
    toast.show(
      release.suppressed === "pinned"
        ? "Unpin the current RPCS3 build before updating"
        : release.suppressed === "skipped"
          ? "Clear the skipped RPCS3 update before installing it"
          : "RPCS3 is already up to date",
      "info",
    );
    return;
  }
  const action = rpcs3Status.value?.installed ? "Update" : "Install";
  if (
    !window.confirm(
      `${action} RPCS3 ${release.latestVersion}? MetalSharp will verify and retain the previous version for rollback.`,
    )
  )
    return;
  rpcs3Loading.value.update = true;
  const result = await api<Rpcs3UpdateProgress>("POST", "/sharp-library/rpcs3/update/install", {}, 35 * 1000);
  if (!result?.ok) {
    rpcs3Loading.value.update = false;
    toast.show(result?.error ?? `Could not ${action.toLowerCase()} RPCS3`, "error");
    return;
  }
  rpcs3UpdateProgress.value = result;
  beginRpcs3UpdatePoll();
}

async function setRpcs3UpdatePolicy(action: "pin-current" | "unpin" | "skip-update" | "clear-skip") {
  const result = await api<Rpcs3Update>("POST", `/sharp-library/rpcs3/${action}`, {
    tag: rpcs3Update.value?.latestTag,
  });
  if (result?.ok) {
    rpcs3Update.value = result;
    const message =
      action === "pin-current"
        ? "RPCS3 pinned to the current build"
        : action === "unpin"
          ? "RPCS3 updates unpinned"
          : action === "skip-update"
            ? "RPCS3 update skipped"
            : "Skipped RPCS3 update cleared";
    toast.show(message, "success");
  } else toast.show(result?.error ?? "Could not save RPCS3 update preference", "error");
}

async function rollbackRpcs3() {
  if (
    !window.confirm(
      "Roll back to the previously installed RPCS3 build? Your firmware, saves, games, and settings will be preserved.",
    )
  )
    return;
  const result = await api<Rpcs3Status & { error?: string }>("POST", "/sharp-library/rpcs3/update/rollback", {});
  if (result?.ok) {
    rpcs3Status.value = result;
    toast.show("RPCS3 rolled back", "success");
  } else toast.show(result?.error ?? "RPCS3 rollback failed", "error");
}

async function addRpcs3Folder() {
  const path = await getAPI().pickDirectory("Choose a folder containing PlayStation 3 games");
  if (!path) return;
  const result = await api<{ ok: boolean; games: Rpcs3Game[]; roots: string[]; error?: string }>(
    "POST",
    "/sharp-library/rpcs3/add-root",
    { path },
  );
  if (result?.ok) {
    rpcs3Games.value = result.games ?? [];
    rpcs3Roots.value = result.roots ?? [];
    toast.show("RPCS3 game folder added", "success");
  } else toast.show(result?.error ?? "Could not add RPCS3 game folder", "error");
}

async function removeRpcs3Root(path: string) {
  const result = await api<{ ok: boolean; games: Rpcs3Game[]; roots: string[]; error?: string }>(
    "POST",
    "/sharp-library/rpcs3/remove-root",
    { path },
  );
  if (result?.ok) {
    rpcs3Games.value = result.games ?? [];
    rpcs3Roots.value = result.roots ?? [];
  } else toast.show(result?.error ?? "Could not remove RPCS3 game folder", "error");
}

async function installRpcs3Content(kind: "firmware" | "package") {
  const path = await getAPI().pickRpcs3File(kind);
  if (!path) return;
  const endpoint = kind === "firmware" ? "install-firmware" : "install-package";
  const result = await api<{ ok: boolean; pid?: number; logPath?: string; error?: string }>(
    "POST",
    `/sharp-library/rpcs3/${endpoint}`,
    { path },
  );
  if (result?.ok) {
    toast.show(
      kind === "firmware" ? "RPCS3 firmware installation started" : "RPCS3 package installation started",
      "success",
    );
    window.setTimeout(() => void refreshRpcs3(), 3000);
  } else toast.show(result?.error ?? `Could not install RPCS3 ${kind}`, "error");
}

async function openRpcs3() {
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/rpcs3/open-ui", {});
  if (!result?.ok) toast.show(result?.error ?? "Could not open RPCS3", "error");
}

async function launchRpcs3Game(game: Rpcs3Game) {
  const result = await api<{ ok: boolean; pid?: number; error?: string }>("POST", "/sharp-library/rpcs3/launch", {
    id: game.id,
    fullscreen: true,
  });
  if (result?.ok) {
    game.running = true;
    game.pid = result.pid;
  } else toast.show(result?.error ?? `Could not launch ${game.title}`, "error");
}

async function stopRpcs3Game(game: Rpcs3Game) {
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/rpcs3/stop", { id: game.id });
  if (result?.ok) {
    game.running = false;
    game.pid = null;
  } else toast.show(result?.error ?? `Could not stop ${game.title}`, "error");
}

async function removeRpcs3Runtime() {
  if (
    !window.confirm(
      "Remove managed RPCS3 runtime versions? Firmware, saves, games, settings, and caches will be preserved.",
    )
  )
    return;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/rpcs3/remove-runtime", {
    confirm: true,
  });
  if (result?.ok) {
    toast.show("RPCS3 runtime removed; user data was preserved", "success");
    rpcs3Update.value = null;
    await refreshRpcs3();
  } else toast.show(result?.error ?? "Could not remove RPCS3 runtime", "error");
}

async function load() {
  const [result, bottleResult, profileResult, gogStatusResult, gogGamesResult] = await Promise.all([
    api<{ ok: boolean; apps: SharpApp[] }>("GET", "/sharp-library"),
    api<{ ok: boolean; bottles: BottleManifest[] }>("GET", "/bottles"),
    api<{ ok: boolean; profiles: RuntimeProfileDefinition[] }>("GET", "/bottles/profiles"),
    api<{ ok: boolean; status: GogStatus }>("GET", "/sharp-library/gog/status"),
    api<{ ok: boolean; games: GogGame[]; status: GogStatus }>("GET", "/sharp-library/gog/games"),
  ]);
  if (result?.ok) {
    apps.value = [...result.apps].sort(sharpAppNameSort);
  }
  if (bottleResult?.ok) {
    bottles.value = bottleResult.bottles;
  }
  if (profileResult?.ok) runtimeProfiles.value = profileResult.profiles;
  if (gogStatusResult?.ok) gogStatus.value = gogStatusResult.status;
  if (gogGamesResult?.ok) {
    setGogGames(gogGamesResult.games ?? []);
    gogStatus.value = gogGamesResult.status;
    for (const game of gogGames.value) {
      if (game.status === "downloading") void monitorGogProgress(game.productId);
    }
  }
  if (gogStatus.value?.prefixInitialized) void refreshGogMonoStatus();
  await Promise.all([loadGameJolt(), refreshRpcs3()]);
  await syncGameJolt(false);
}

function setGogGames(games: GogGame[]) {
  const unique = new Map<string, GogGame>();
  for (const game of games) {
    const previous = unique.get(game.productId);
    unique.set(
      game.productId,
      previous
        ? {
            ...previous,
            ...game,
            title: !game.title || game.title === game.productId ? previous.title : game.title,
            imageUrl: game.imageUrl ?? previous.imageUrl,
            iconUrl: game.iconUrl ?? previous.iconUrl,
          }
        : game,
    );
  }
  gogGames.value = [...unique.values()].sort((a, b) =>
    a.title.localeCompare(b.title, undefined, { sensitivity: "base", numeric: true }),
  );
}

async function refreshGog() {
  const [statusResult, gamesResult] = await Promise.all([
    api<{ ok: boolean; status: GogStatus }>("GET", "/sharp-library/gog/status"),
    api<{ ok: boolean; games: GogGame[]; status: GogStatus }>("GET", "/sharp-library/gog/games"),
  ]);
  if (statusResult?.ok) gogStatus.value = statusResult.status;
  if (gogStatus.value?.prefixInitialized) void refreshGogMonoStatus();
  if (gamesResult?.ok) {
    setGogGames(gamesResult.games ?? []);
    gogStatus.value = gamesResult.status;
  }
}

function upsertGogGame(game: GogGame) {
  const idx = gogGames.value.findIndex((item) => item.productId === game.productId);
  const games = [...gogGames.value];
  if (idx >= 0) {
    const previous = games[idx];
    games[idx] = {
      ...previous,
      ...game,
      title: !game.title || game.title === game.productId ? previous.title : game.title,
      imageUrl: game.imageUrl ?? previous.imageUrl,
      iconUrl: game.iconUrl ?? previous.iconUrl,
    };
  } else games.push(game);
  setGogGames(games);
}

async function initializeGogPrefix() {
  gogLoading.value.setup = true;
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>(
    "POST",
    "/sharp-library/gog/initialize-prefix",
    {},
    5 * 60 * 1000,
  );
  gogLoading.value.setup = false;
  if (result?.ok) {
    if (result.status) {
      gogStatus.value = {
        ...result.status,
        prefixInitialized: true,
        status: result.status.authenticated ? "ready" : "needs_login",
        ready: result.status.authenticated,
      };
    }
    await refreshGog();
    toast.show("GOG prefix ready", "success");
    return;
  }

  await refreshGog();
  const transientHangup = !result || result.error?.toLowerCase().includes("socket hang up");
  if (transientHangup && gogStatus.value?.prefixInitialized) {
    toast.show("GOG prefix ready", "success");
    return;
  }
  toast.show(result?.error ?? "Failed to initialize GOG prefix", "error");
}

async function removeGogPrefix() {
  const message =
    "Remove the GOG Wine prefix? " +
    "This will permanently delete the isolated Wine prefix and all Wine Mono components. " +
    "Downloaded GOG games will stay on disk but cannot launch until the prefix is re-created.";
  if (!confirm(message)) return;
  gogLoading.value.removing = true;
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>(
    "POST",
    "/sharp-library/gog/remove-prefix",
    {},
    30 * 1000,
  );
  gogLoading.value.removing = false;
  if (result?.ok) {
    if (result.status) gogStatus.value = result.status;
    await refreshGog();
    toast.show("GOG prefix removed", "success");
  } else {
    toast.show(result?.error ?? "Failed to remove GOG prefix", "error");
  }
}

async function disconnectGog() {
  if (!confirm("Disconnect GOG and show Login again? Installed games will stay on disk.")) return;
  gogLoading.value.login = true;
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>(
    "POST",
    "/sharp-library/gog/logout",
    {},
    30 * 1000,
  );
  gogLoading.value.login = false;
  if (result?.ok) {
    if (result.status) gogStatus.value = result.status;
    await refreshGog();
    toast.show("GOG disconnected", "success");
  } else {
    toast.show(result?.error ?? "Failed to disconnect GOG", "error");
  }
}

async function handleGogAuthButton() {
  if (gogStatus.value?.authenticated) {
    await disconnectGog();
  } else {
    await loginGog();
  }
}

async function loginGog() {
  if (!gogStatus.value?.authUrl) return;
  // If the Rust backend hasn't staged the OAuth helper yet, refresh status and
  // try once more — this protects users who hit Login before the gogdl
  // bootstrap has finished copying tools/gog-oauth-electron into ~/.metalsharp.
  if (gogStatus.value?.oauthHelperAvailable === false) {
    await refreshGog();
  }
  gogLoading.value.login = true;
  const login = await getAPI().gogOAuthLogin(gogStatus.value.authUrl);
  if (!login.ok || !login.code) {
    gogLoading.value.login = false;
    const helperMissing = login.error?.includes("OAuth helper script not found");
    if (helperMissing) {
      toast.show("GOG login helper unavailable; reinitializing the GOG prefix", "error");
      await refreshGog();
      return;
    }
    toast.show(login.error ?? "GOG login cancelled", "error");
    return;
  }
  toast.show("GOG login code captured; finishing connection…", "success");
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>(
    "POST",
    "/sharp-library/gog/auth-code",
    { code: login.code },
    90 * 1000,
  );
  gogLoading.value.login = false;
  if (result?.ok && result.status) {
    gogStatus.value = result.status;
    toast.show("GOG connected", "success");
    await syncGogLibrary();
  } else {
    toast.show(result?.error ?? "Failed to connect GOG", "error");
  }
}

async function syncGogLibrary() {
  gogLoading.value.sync = true;
  const result = await api<{ ok: boolean; games?: GogGame[]; status?: GogStatus; error?: string }>(
    "POST",
    "/sharp-library/gog/sync",
    {},
    5 * 60 * 1000,
  );
  gogLoading.value.sync = false;
  if (result?.ok) {
    if (result.games) setGogGames(result.games);
    if (result.status) gogStatus.value = result.status;
    await refreshGog();
    toast.show("GOG library synced", "success");
  } else {
    toast.show(result?.error ?? "Failed to sync GOG library", "error");
  }
}

async function installGogGame(game: GogGame) {
  const installPath = await getAPI().pickDirectory(`Choose install folder for ${game.title}`);
  if (!installPath) return;
  gogLoading.value[`${game.productId}:install`] = true;
  const result = await api<{ ok: boolean; game?: GogGame; pid?: number; error?: string }>(
    "POST",
    "/sharp-library/gog/install",
    {
      productId: game.productId,
      title: game.title,
      platform: game.platform || "windows",
      installPath,
    },
    90 * 1000,
  );
  gogLoading.value[`${game.productId}:install`] = false;
  if (result?.ok && result.game) {
    upsertGogGame(result.game);
    toast.show(`Downloading ${game.title}`, "success");
    void monitorGogProgress(game.productId);
  } else {
    toast.show(result?.error ?? `Failed to download ${game.title}`, "error");
  }
}

async function monitorGogProgress(productId: string) {
  for (let i = 0; i < 720; i++) {
    const result = await api<{ ok: boolean; percent?: number; active?: boolean; game?: GogGame; error?: string }>(
      "POST",
      "/sharp-library/gog/progress",
      { productId },
    );
    if (result?.ok) {
      gogProgress.value[productId] = result.percent ?? gogProgress.value[productId] ?? 0;
      if (result.game) upsertGogGame(result.game);
      if (!result.active && result.game?.status !== "downloading") return;
    } else {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
}

async function playGogGame(game: GogGame) {
  gogLoading.value[`${game.productId}:play`] = true;
  const result = await api<{ ok: boolean; game?: GogGame; pid?: number; error?: string }>(
    "POST",
    "/sharp-library/gog/play",
    { productId: game.productId, engine: gogEngines.value[game.productId] ?? "auto" },
    90 * 1000,
  );
  gogLoading.value[`${game.productId}:play`] = false;
  if (result?.ok && result.game) {
    upsertGogGame(result.game);
    toast.show(`${game.title} launched`, "success");
  } else {
    toast.show(result?.error ?? `Failed to launch ${game.title}`, "error");
  }
}

function updateGogEngine(game: GogGame, engine: string) {
  gogEngines.value[game.productId] = engine;
  localStorage.setItem("metalsharp-gog-engines", JSON.stringify(gogEngines.value));
}

async function stopGogGame(game: GogGame) {
  gogLoading.value[`${game.productId}:stop`] = true;
  const result = await api<{ ok: boolean; game?: GogGame; error?: string }>("POST", "/sharp-library/gog/stop", {
    productId: game.productId,
  });
  gogLoading.value[`${game.productId}:stop`] = false;
  if (result?.ok && result.game) {
    upsertGogGame(result.game);
    toast.show(`${game.title} stopped`, "success");
  } else {
    toast.show(result?.error ?? `Failed to stop ${game.title}`, "error");
  }
}

async function uninstallGogGame(game: GogGame) {
  if (!confirm(`Delete ${game.title} from disk?`)) return;
  gogLoading.value[`${game.productId}:uninstall`] = true;
  const result = await api<{ ok: boolean; game?: GogGame; error?: string }>(
    "POST",
    "/sharp-library/gog/uninstall",
    { productId: game.productId },
    90 * 1000,
  );
  gogLoading.value[`${game.productId}:uninstall`] = false;
  if (result?.ok && result.game) {
    upsertGogGame(result.game);
    toast.show(`${game.title} uninstalled`, "success");
  } else {
    toast.show(result?.error ?? `Failed to uninstall ${game.title}`, "error");
  }
}

async function refreshSharpLibrary() {
  await load();
  toast.show("Sharp Library refreshed", "success");
}

async function refreshCurrentSource() {
  if (sourceMode.value === "rpcs3") {
    await refreshRpcs3(true);
    return;
  }
  if (sourceMode.value === "gog") {
    if (gogStatus.value?.authenticated) {
      await syncGogLibrary();
    } else {
      await refreshGog();
      toast.show("GOG status refreshed", "success");
    }
    return;
  }
  await refreshSharpLibrary();
}

async function installExe() {
  const filePath = await getAPI().pickExeFile();
  if (!filePath) return;
  toast.show("Installing application...");
  const result = await api<{ ok: boolean; app?: SharpApp; installing?: boolean; message?: string; error?: string }>(
    "POST",
    "/sharp-library/install",
    { srcPath: filePath },
  );
  if (result?.ok && result.app) {
    toast.show(`Installed ${result.app.name}`, "success");
    await load();
  } else if (result?.ok && result.installing) {
    toast.show(result.message ?? "Installer started. Finish setup, then refresh Sharp Library.", "success");
    await load();
  } else {
    toast.show(result?.error ?? "Failed to install", "error");
  }
}

async function refreshBottle(id: string) {
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; bottle?: BottleManifest; error?: string }>("POST", "/bottles/refresh", {
    id,
  });
  bottleLoading.value[id] = false;
  if (result?.ok && result.bottle) {
    upsertBottle(result.bottle);
    toast.show("Bottle scan refreshed", "success");
  } else {
    toast.show(result?.error ?? "Failed to refresh bottle", "error");
  }
}

async function doctorBottle(id: string) {
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; report?: BottleDiagnostic; error?: string }>(
    "POST",
    "/bottles/doctor",
    { id },
    2 * 60 * 1000,
  );
  bottleLoading.value[id] = false;
  if (result?.ok && result.report) {
    bottleReports.value[id] = result.report;
    const bottle = bottles.value.find((item) => item.id === id);
    if (bottle?.runtime_profile === "d3dmetal") await loadD3DMetalStatus(bottle);
    await load();
  } else {
    toast.show(result?.error ?? "Bottle Doctor failed", "error");
  }
}

async function prepareBottle(id: string) {
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; report?: BottleDiagnostic; error?: string }>(
    "POST",
    "/bottles/prepare",
    { id },
    10 * 60 * 1000,
  );
  bottleLoading.value[id] = false;
  if (result?.ok && result.report) {
    bottleReports.value[id] = result.report;
    toast.show(
      result.report.ready ? "Bottle prepared" : "Bottle needs runtime repair",
      result.report.ready ? "success" : "error",
    );
    await load();
  } else {
    toast.show(result?.error ?? "Failed to prepare bottle", "error");
  }
}

function visibleD3DMetalActionsForBottle(bottleId: string) {
  return (d3dmetalActions.value[bottleId] ?? []).filter((action) => action.id !== "play_d3dmetal");
}

function clearD3DMetalBottleState(bottleId: string) {
  d3dmetalStates.value[bottleId] = null;
  d3dmetalActions.value[bottleId] = [];
}

async function loadD3DMetalStatus(bottle: BottleManifest) {
  if (bottle.runtime_profile !== "d3dmetal") {
    clearD3DMetalBottleState(bottle.id);
    return;
  }
  if (!bottle.steam_app_id) return;
  const result = await api<D3DMetalGptkResponse>("POST", "/d3dmetal/bottles/status", {
    appid: bottle.steam_app_id,
    bottleId: bottle.id,
  });
  if (result?.ok && result.state) {
    d3dmetalStates.value[bottle.id] = result.state;
    d3dmetalActions.value[bottle.id] = result.actions ?? [];
  } else {
    clearD3DMetalBottleState(bottle.id);
  }
}

async function saveD3DMetalBottle(bottle: BottleManifest) {
  if (!bottle.steam_app_id || !bottle.game_install_path) {
    toast.show("D3DMetal save requires a Steam app id and game install path", "error");
    return;
  }
  bottleLoading.value[bottle.id] = true;
  // The first save downloads the GPTK fork via Homebrew, which can
  // take several minutes. Surface a bottom-right toast so the bottle
  // doesn't look stale while the request is in flight.
  toast.show("Saving D3DMetal bottle — downloading GPTK runtime on first save…", "success");
  const result = await api<D3DMetalGptkResponse>(
    "POST",
    "/d3dmetal/bottles/save",
    {
      appid: bottle.steam_app_id,
      bottleId: bottle.id,
      name: bottle.name,
      gameDir: bottle.game_install_path,
    },
    10 * 60 * 1000,
  );
  bottleLoading.value[bottle.id] = false;
  if (result?.ok && result.state) {
    d3dmetalStates.value[bottle.id] = result.state;
    d3dmetalActions.value[bottle.id] = result.actions ?? [];
    toast.show("D3DMetal bottle saved; seed VC runtime DLLs and seed prefix when ready", "success");
  } else {
    toast.show(result?.error ?? "D3DMetal bottle save failed", "error");
    await loadD3DMetalStatus(bottle);
  }
}

function sharpAppExeAbsolute(app: SharpApp) {
  if (app.exe_path.startsWith("/")) return app.exe_path;
  return `${app.install_dir.replace(/\/$/, "")}/${app.exe_path.replace(/^\.\//, "")}`;
}

function d3dmetalActionRoute(actionId: string) {
  switch (actionId) {
    case "install_homebrew_gptk":
      return "/d3dmetal/bottles/install-homebrew-gptk";
    case "install_rosetta":
      return "/d3dmetal/bottles/install-rosetta";
    case "repair_gptk_payload":
      return "/d3dmetal/bottles/repair-gptk-payload";
    case "install_x64_redist":
      return "/d3dmetal/bottles/install-x64-redist";
    case "seed_prefix":
      return "/d3dmetal/bottles/seed-prefix";
    default:
      return "/d3dmetal/bottles/play";
  }
}

async function runD3DMetalAction(
  bottle: BottleManifest,
  action: D3DMetalGptkAction,
  app?: SharpApp,
): Promise<number | null> {
  if (!bottle.steam_app_id) return null;
  bottleLoading.value[bottle.id] = true;
  const route = d3dmetalActionRoute(action.id);
  const result = await api<D3DMetalGptkResponse>(
    "POST",
    route,
    {
      appid: bottle.steam_app_id,
      bottleId: bottle.id,
      gameDir: bottle.game_install_path,
      gameExe: app ? sharpAppExeAbsolute(app) : undefined,
      launchArgs: app ? [...(app.launch_args ?? []), ...(app.user_launch_args ?? [])] : undefined,
    },
    10 * 60 * 1000,
  );
  bottleLoading.value[bottle.id] = false;
  if (result?.ok) {
    toast.show(action.id === "play_d3dmetal" ? "D3DMetal launch started" : `${action.label}: complete`, "success");
    if (result.state) {
      d3dmetalStates.value[bottle.id] = result.state;
      d3dmetalActions.value[bottle.id] = result.actions ?? [];
    } else {
      await loadD3DMetalStatus(bottle);
    }
    return result.launch?.pid ?? null;
  } else {
    toast.show(result?.error ?? `${action.label} failed`, "error");
    await loadD3DMetalStatus(bottle);
    return null;
  }
}

async function repairBottleComponent(id: string, component: string) {
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; repair?: ComponentRepair; error?: string }>(
    "POST",
    "/bottles/repair-component",
    {
      id,
      component,
    },
    10 * 60 * 1000,
  );
  if (result?.ok && result.repair) {
    const repair = result.repair;
    const failed = ["asset_missing", "failed", "install_failed"].includes(repair.status);
    toast.show(failed ? repair.detail : `${repair.id}: ${repair.status}`, failed ? "error" : "success");
    if (repair.status === "started" || repair.status === "seeding") {
      await pollRepairDone(id, component);
    } else {
      await doctorBottle(id);
    }
  } else {
    toast.show(result?.error ?? "Failed to repair component", "error");
  }
  bottleLoading.value[id] = false;
}

async function pollRepairDone(id: string, component: string) {
  const pollInterval = 5000;
  const maxPolls = 120;
  for (let i = 0; i < maxPolls; i++) {
    await new Promise((r) => setTimeout(r, pollInterval));
    const poll = await api<{ ok: boolean; repair?: ComponentRepair; error?: string }>(
      "POST",
      "/bottles/repair-component",
      {
        id,
        component,
        dryRun: true,
      },
      2 * 60 * 1000,
    );
    if (!poll?.ok || !poll.repair) break;
    const status = poll.repair.status;
    if (status === "already_installed") {
      toast.show(`${component}: ready`, "success");
      await doctorBottle(id);
      return;
    }
    if (["asset_missing", "failed", "install_failed"].includes(status)) {
      toast.show(poll.repair.detail || `${component}: ${status}`, "error");
      await doctorBottle(id);
      return;
    }
  }
  toast.show(`${component}: repair is taking longer than expected — check back`);
  await doctorBottle(id);
}

async function setBottleProfile(id: string, profile: string) {
  if (profile === "d3dmetal") {
    const bottle = bottles.value.find((item) => item.id === id);
    if (!bottle) return;
    await saveD3DMetalBottle(bottle);
    await load();
    await doctorBottle(id);
    return;
  }
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; bottle?: BottleManifest; error?: string }>(
    "POST",
    "/bottles/set-runtime-profile",
    {
      id,
      profile,
    },
  );
  bottleLoading.value[id] = false;
  if (result?.ok && result.bottle) {
    const isM12 = profile === "m12";
    const appid = result.bottle.steam_app_id ?? 0;
    const m12DryRun = isM12 ? await api<M12DryRun>("GET", `/diagnostics/m12/dry-run?appid=${appid}`) : null;
    upsertBottle(result.bottle);
    if (result.bottle.runtime_profile !== "d3dmetal") clearD3DMetalBottleState(id);
    if (isM12 && m12DryRun?.ok === false) {
      const missing = m12DryRun.missing
        ?.map((entry) => entry.filename)
        .filter(Boolean)
        .join(", ");
      toast.show(`M12 bottle saved, but its dry run failed${missing ? `: ${missing}` : ""}`, "error");
    } else if (isM12 && !m12DryRun) {
      toast.show("M12 bottle saved, but its dry run could not be completed", "error");
    } else {
      toast.show("Bottle profile updated", "success");
    }
    await doctorBottle(id);
  } else {
    toast.show(result?.error ?? "Failed to update bottle profile", "error");
  }
}

async function setBottleWindowsVersion(id: string, version: string) {
  bottleLoading.value[id] = true;
  const result = await api<{ ok: boolean; repair?: ComponentRepair; error?: string }>(
    "POST",
    "/bottles/set-windows-version",
    {
      id,
      version,
    },
  );
  bottleLoading.value[id] = false;
  if (result?.ok && result.repair) {
    toast.show(`Windows mode ${version} requested`, "success");
    await doctorBottle(id);
  } else {
    toast.show(result?.error ?? "Failed to set Windows mode", "error");
  }
}

async function relaunchBottleInstaller(bottle: BottleManifest) {
  bottleLoading.value[bottle.id] = true;
  const result = await api<{ ok: boolean; installing?: boolean; message?: string; error?: string }>(
    "POST",
    "/bottles/relaunch-installer",
    { id: bottle.id },
  );
  bottleLoading.value[bottle.id] = false;
  if (result?.ok) {
    toast.show(result.message ?? "Installer relaunched", "success");
    await load();
  } else {
    toast.show(result?.error ?? "Failed to relaunch installer", "error");
  }
}

async function addBottleApp(bottle: BottleManifest, app: { name: string; exe_path: string }) {
  bottleLoading.value[bottle.id] = true;
  const result = await api<{ ok: boolean; app?: SharpApp; error?: string }>(
    "POST",
    "/sharp-library/import-bottle-app",
    {
      bottleId: bottle.id,
      exePath: app.exe_path,
      name: app.name,
    },
  );
  bottleLoading.value[bottle.id] = false;
  if (result?.ok && result.app) {
    toast.show(`Added ${result.app.name}`, "success");
    await load();
  } else {
    toast.show(result?.error ?? "Failed to add bottle app", "error");
  }
}

function upsertBottle(bottle: BottleManifest) {
  const idx = bottles.value.findIndex((item) => item.id === bottle.id);
  if (idx >= 0) bottles.value[idx] = bottle;
  else bottles.value.push(bottle);
}

function bottleBadgeClass(health: string) {
  return health === "ready" ? "badge-ok" : "badge-warn";
}

function bottleBadgeLabel(bottle: BottleManifest) {
  switch (bottle.health) {
    case "ready":
      return "Installed";
    case "needs_repair":
      return "Bottle needs repair";
    case "partial":
      return "Partial install";
    case "new":
    default:
      return "Not installed";
  }
}

function bottleForApp(app: SharpApp) {
  if (!app.bottle_id) return null;
  return bottles.value.find((b) => b.id === app.bottle_id) ?? null;
}

async function openBottleLaunchLog(bottle: BottleManifest) {
  if (!bottle.last_launch_log) return;
  const result = await api<{ ok: boolean; path?: string; error?: string }>("POST", "/diagnostics/open", {
    path: bottle.last_launch_log,
  });
  if (!result?.ok) {
    toast.show(result?.error ?? "Failed to open launch log", "error");
  }
}

async function launchApp(id: string, engine: string) {
  const app = apps.value.find((a) => a.id === id);
  if (!app) return;
  if (engine === "d3dmetal" && app.bottle_id) {
    const bottle = bottles.value.find((item) => item.id === app.bottle_id);
    if (bottle?.steam_app_id) {
      if (!d3dmetalStates.value[bottle.id]) await loadD3DMetalStatus(bottle);
      const playAction = d3dmetalActions.value[bottle.id]?.find((action) => action.id === "play_d3dmetal") ?? {
        id: "play_d3dmetal",
        label: "Play D3DMetal",
        enabled: d3dmetalStates.value[bottle.id]?.play_ready === true,
        state: d3dmetalStates.value[bottle.id]?.play_ready ? "seeded" : "missing",
        detail: "Launch game exe directly through GPTK Wine",
      };
      if (!d3dmetalStates.value[bottle.id]?.play_ready || !playAction.enabled) {
        toast.show("D3DMetal bottle is not ready; seed VC runtime DLLs and seed prefix first", "error");
        return;
      }
      const pid = await runD3DMetalAction(bottle, playAction, app);
      if (pid) {
        runningSharpPids.value[id] = pid;
        launchErrors.value[id] = "";
        diagnosticsOpen.value[id] = false;
      }
      return;
    }
  }
  toast.show(`Launching ${app.name}...`);
  const result = await api<{ ok: boolean; pid?: number; pipeline?: string; warnings?: string[]; error?: string }>(
    "POST",
    "/sharp-library/launch",
    { id, engine },
  );
  if (result?.ok && result.pid) {
    const warning = result.warnings?.[0];
    runningSharpPids.value[id] = result.pid;
    launchErrors.value[id] = "";
    diagnosticsOpen.value[id] = false;
    toast.show(warning ? `Launched ${app.name}: ${warning}` : `Launched ${app.name}`, "success");
  } else {
    const error = result?.error ?? `Failed to launch ${app.name}`;
    launchErrors.value[id] = error;
    toast.show(error, "error");
    await openDiagnostics(app);
  }
}

async function stopSharpApp(app: SharpApp) {
  const pid = runningSharpPids.value[app.id];
  if (!pid) return;
  await api("POST", "/kill", { pid });
  delete runningSharpPids.value[app.id];
  toast.show(`Closed ${app.name}`);
}

async function updateEngine(id: string, engine: string) {
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/set-engine", { id, engine });
  if (result?.ok) {
    const app = apps.value.find((a) => a.id === id);
    if (app) app.engine = engine;
  } else {
    toast.show(result?.error ?? "Failed to set engine", "error");
  }
}

async function uninstallApp(id: string) {
  const app = apps.value.find((a) => a.id === id);
  if (!app) return;
  if (!confirm(`Uninstall ${app.name}?`)) return;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/uninstall", { id });
  if (result?.ok) {
    toast.show(`Uninstalled ${app.name}`, "success");
    await load();
  } else toast.show(result?.error ?? "Failed to uninstall", "error");
}

async function setCover(id: string) {
  const filePath = await getAPI().pickImageFile();
  if (!filePath) return;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/set-cover", {
    id,
    coverPath: filePath,
  });
  if (result?.ok) {
    toast.show("Cover updated", "success");
    await load();
  } else toast.show(result?.error ?? "Failed to set cover", "error");
}

async function addAsset(app: SharpApp) {
  if (!app.bottle_id) {
    toast.show(`${app.name} is not associated with an app bottle`, "error");
    return;
  }
  const filePath = await getAPI().pickAssetFile();
  if (!filePath) return;
  const result = await api<{ ok: boolean; path?: string; error?: string }>("POST", "/sharp-library/add-asset", {
    id: app.id,
    assetPath: filePath,
  });
  if (result?.ok) {
    toast.show(`Added asset to ${app.name}`, "success");
  } else {
    toast.show(result?.error ?? "Failed to add asset", "error");
  }
}

async function updateCoverPosition(app: SharpApp) {
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/set-cover-position", {
    id: app.id,
    x: app.cover_position_x,
    y: app.cover_position_y,
  });
  if (!result?.ok) toast.show(result?.error ?? "Failed to save cover position", "error");
}

function coverPosition(app: SharpApp): string {
  return `${app.cover_position_x ?? 50}% ${app.cover_position_y ?? 50}%`;
}

async function runDoctor(app: SharpApp) {
  doctorOpen.value[app.id] = true;
  doctorLoading.value[app.id] = true;
  doctorReports.value[app.id] = null;
  const result = await api<{ ok: boolean; report?: LaunchDoctorReport; error?: string }>(
    "POST",
    "/sharp-library/doctor",
    {
      id: app.id,
      engine: app.engine,
    },
  );
  doctorLoading.value[app.id] = false;

  if (result?.ok && result.report) {
    doctorReports.value[app.id] = result.report;
  } else {
    toast.show(result?.error ?? "Launch Doctor failed", "error");
  }
}

async function openDiagnostics(app: SharpApp) {
  diagnosticsOpen.value[app.id] = true;
  await Promise.all([runDoctor(app), loadRecentDiagnostics(app)]);
}

async function loadRecentDiagnostics(app: SharpApp) {
  diagnosticsLoading.value[app.id] = true;
  const [logsResult, crashResult] = await Promise.all([
    api<{ ok: boolean; logs: LogEntry[] }>("GET", "/logs"),
    api<{ ok: boolean; reports: CrashReport[] }>("GET", "/logs/crash-reports"),
  ]);
  diagnosticsLoading.value[app.id] = false;

  if (logsResult?.ok) {
    const allLines = logsResult.logs.flatMap((entry) => entry.lines.map((line) => `[${entry.name}] ${line}`));
    const appNeedles = [app.name, app.exe_path, app.install_dir].map((value) => value.toLowerCase());
    const matching = allLines.filter((line) =>
      appNeedles.some((needle) => needle && line.toLowerCase().includes(needle)),
    );
    recentLogLines.value[app.id] = (matching.length ? matching : allLines).slice(-40);
  }

  if (crashResult?.ok) {
    const appNeedles = [app.name, app.exe_path, app.install_dir].map((value) => value.toLowerCase());
    recentCrashReports.value[app.id] = crashResult.reports
      .filter((report) => {
        const haystack = `${report.name} ${report.file} ${report.source}`.toLowerCase();
        return appNeedles.some((needle) => needle && haystack.includes(needle));
      })
      .slice(0, 5);
  }
}

function doctorActionLabel(check: LaunchDoctorCheck, app: SharpApp): string {
  if (check.id === "runtime_assets" || check.id === "dll_sources") return "Install runtime";
  if (check.id === "exe_route") return app.engine === "auto" ? "Switch to Wine" : "Switch to Auto";
  if (check.detail.toLowerCase().includes("steam")) return "Restart Steam";
  if (check.id === "launcher_exe") return "Open logs";
  return "Open logs";
}

async function runDoctorAction(app: SharpApp, check: LaunchDoctorCheck) {
  const label = doctorActionLabel(check, app);
  if (label === "Install runtime") {
    const result = await api<{ ok: boolean; error?: string }>("POST", "/setup/install-all");
    toast.show(
      result?.ok ? "Runtime install started" : (result?.error ?? "Failed to start runtime install"),
      result?.ok ? "success" : "error",
    );
  } else if (label === "Restart Steam") {
    await api("POST", "/steam/stop");
    const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/launch");
    toast.show(
      result?.ok ? "Steam restart requested" : (result?.error ?? "Failed to restart Steam"),
      result?.ok ? "success" : "error",
    );
  } else if (label === "Switch to Auto") {
    await updateEngine(app.id, "auto");
    await runDoctor({ ...app, engine: "auto" });
  } else if (label === "Switch to Wine") {
    await updateEngine(app.id, "wine_bare");
    await runDoctor({ ...app, engine: "wine_bare" });
  } else {
    await openLogFolder();
  }
}

async function clearShaderCache(app: SharpApp) {
  const result = await api<{ ok: boolean; bytes_freed?: number; files_removed?: number; error?: string }>(
    "POST",
    "/cache/clear",
    { type: "shader" },
  );
  if (result?.ok) {
    toast.show(`All shader caches cleared before next ${app.name} launch`, "success");
  } else {
    toast.show(result?.error ?? "Failed to clear shader cache", "error");
  }
}

async function openLogFolder() {
  const result = await getAPI().openLogsFolder();
  if (result && result.ok === false) {
    toast.show(result.error ?? "Failed to open logs", "error");
  }
}

async function openBottleLog(bottle: BottleManifest) {
  if (!bottle.last_launch_log) {
    toast.show("No bottle launch log recorded yet", "warning");
    return;
  }
  await getAPI().openInFinder(bottle.last_launch_log);
}

async function openBottleFolder(bottle: BottleManifest) {
  await getAPI().openInFinder(bottle.prefix_path);
}

async function copyDiagnosticBundle(app: SharpApp) {
  const report = doctorReports.value[app.id];
  const payload = [
    `MetalSharp Sharp Library Diagnostic Bundle`,
    `App: ${app.name}`,
    `ID: ${app.id}`,
    `Engine: ${app.engine}`,
    `EXE: ${app.install_dir}/${app.exe_path}`,
    `Last launch error: ${launchErrors.value[app.id] || "none"}`,
    "",
    "Doctor:",
    report ? JSON.stringify(report, null, 2) : "No doctor report loaded",
    "",
    "Recent crash reports:",
    JSON.stringify(recentCrashReports.value[app.id] ?? [], null, 2),
    "",
    "Recent launch log:",
    (recentLogLines.value[app.id] ?? []).join("\n"),
  ].join("\n");
  const result = await getAPI().copyText(payload);
  toast.show(
    result?.ok ? "Diagnostic bundle copied" : (result?.error ?? "Failed to copy diagnostics"),
    result?.ok ? "success" : "error",
  );
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
}

let removeGameJoltDownloadListener: (() => void) | null = null;
onMounted(() => {
  void load();
  void refreshGameJoltProcessState();
  gamejoltProcessPollTimer = setInterval(() => void refreshGameJoltProcessState(), 1500);
  rpcs3ProcessPollTimer = setInterval(() => {
    if (sourceMode.value === "rpcs3" && rpcs3Status.value?.installed) void refreshRpcs3();
  }, 3000);
  removeGameJoltDownloadListener = getAPI().onGameJoltDownload(handleGameJoltDownload);
});
onUnmounted(() => {
  stopGogMonoPoll();
  if (gamejoltProcessPollTimer) clearInterval(gamejoltProcessPollTimer);
  gamejoltProcessPollTimer = null;
  if (rpcs3ProcessPollTimer) clearInterval(rpcs3ProcessPollTimer);
  rpcs3ProcessPollTimer = null;
  stopRpcs3UpdatePoll();
  removeGameJoltDownloadListener?.();
  removeGameJoltDownloadListener = null;
  if (gamejoltDragPointerId !== null) {
    gamejoltDragPointerId = null;
    gamejoltBrowserDragging.value = false;
  }
  window.removeEventListener("pointermove", moveGameJoltBrowserDrag);
  window.removeEventListener("pointerup", endGameJoltBrowserDrag);
  window.removeEventListener("pointercancel", endGameJoltBrowserDrag);
});
</script>

<template>
  <div class="sharp-view">
    <div class="sharp-header glass-header">
      <div class="sharp-header-title">
        <h1>{{ headerTitle }}</h1>
        <p>{{ headerSubtitle }}</p>
      </div>
      <div class="sharp-header-controls">
        <nav class="source-tabs" role="tablist" aria-label="Sharp Library sources">
          <button
            v-for="tab in sourceTabs"
            :key="tab.id"
            class="source-tab"
            :class="{ active: sourceMode === tab.id }"
            type="button"
            role="tab"
            :aria-selected="sourceMode === tab.id"
            @click="sourceMode = tab.id"
          >
            {{ tab.label }}
          </button>
        </nav>
        <button v-if="sourceMode === 'installers'" class="btn btn-primary" @click="installExe">
          <IconUpload class="btn-icon" width="14" height="14" />
          <span class="btn-label-long">Install Windows Program</span><span class="btn-label-short">Install</span>
        </button>
        <button
          v-if="sourceMode === 'gog'"
          class="btn btn-secondary"
          :disabled="gogLoading.setup || gogLoading.removing"
          :title="
            gogStatus?.prefixInitialized
              ? 'Remove the GOG Wine prefix. Downloaded GOG games stay on disk but cannot launch until the prefix is re-created.'
              : 'Create the isolated Wine prefix required to install and launch GOG games.'
          "
          @click="gogStatus?.prefixInitialized ? removeGogPrefix() : initializeGogPrefix()"
        >
          <span class="btn-label-long">{{
            gogLoading.removing
              ? "Removing…"
              : gogLoading.setup
                ? "Initializing…"
                : gogStatus?.prefixInitialized
                  ? "Remove Prefix"
                  : "Initialize GOG Prefix"
          }}</span
          ><span class="btn-label-short">Prefix</span>
        </button>
        <button
          v-if="sourceMode === 'gog' && showGogInstallMono"
          class="btn btn-primary"
          :disabled="
            gogLoading.mono ||
            (gogMonoStatus?.running && !gogMonoStatus?.stalled) ||
            (gogMonoStatus?.downloading && !gogMonoStatus?.stalled)
          "
          :title="gogMonoButtonTitle()"
          @click="installGogMono"
        >
          <span class="btn-label-long">{{ gogMonoButtonLabel() }}</span
          ><span class="btn-label-short">Mono</span>
        </button>
        <button
          v-if="sourceMode === 'gog' && showGogInstallMono && (gogMonoStatus?.stalled || gogMonoStatus?.running)"
          class="btn btn-secondary btn-sm"
          :disabled="gogLoading.mono"
          title="Kill any stuck Wine Mono processes and reset the install state"
          @click="resetGogMonoInstall"
        >
          <span class="btn-label-long">Reset</span><span class="btn-label-short">Reset</span>
        </button>
        <button v-if="sourceMode === 'gamejolt'" class="btn btn-secondary" @click="chooseGameJoltStorage">
          <span class="btn-label-long">{{ gamejoltStorage ? "Change Folder" : "Choose GameJolt Folder" }}</span>
          <span class="btn-label-short">{{ gamejoltStorage ? "Change" : "Folder" }}</span>
        </button>
        <button
          v-if="sourceMode === 'gog'"
          class="btn btn-primary"
          :disabled="gogLoading.login || !gogStatus?.prefixInitialized"
          :title="
            gogStatus?.oauthHelperAvailable === false
              ? 'GOG OAuth helper is missing; click “Prefix” or refresh to stage the new bundled helper.'
              : 'Sign in to GOG to enable library sync and downloads'
          "
          @click="handleGogAuthButton"
        >
          <span class="btn-label-long">{{
            gogStatus?.authenticated ? "GOG Connected" : gogLoading.login ? "Connecting…" : "Login to GOG"
          }}</span
          ><span class="btn-label-short">{{ gogStatus?.authenticated ? "Connected" : "Login" }}</span>
        </button>
        <button
          v-if="sourceMode !== 'rpcs3'"
          class="btn btn-secondary"
          :disabled="sourceMode === 'gog' && gogLoading.sync"
          @click="sourceMode === 'gamejolt' ? syncGameJolt() : refreshCurrentSource()"
        >
          <component :is="refreshIcon" class="btn-icon" width="14" height="14" />
          <span class="btn-label-long">{{
            sourceMode === "gog"
              ? gogLoading.sync
                ? "Syncing…"
                : "Sync GOG"
              : sourceMode === "gamejolt"
                ? gamejoltLoading
                  ? "Scanning…"
                  : "Sync GameJolt"
                : "Refresh"
          }}</span
          ><span class="btn-label-short">{{
            sourceMode === "gog" || sourceMode === "gamejolt" ? "Sync" : "Refresh"
          }}</span>
        </button>
      </div>
    </div>

    <div class="sharp-body view-body-surface">
      <template v-if="sourceMode === 'installers'">
        <div v-if="apps.length === 0" class="empty-state">
          <div class="empty-icon">
            <IconMonitor width="48" height="48" />
          </div>
          <h2>No applications installed</h2>
          <p>Click "Install Windows Program" to add a Windows application</p>
        </div>

        <div v-else class="sharp-grid">
          <div v-for="app in apps" :key="app.id" class="sharp-card" :class="{ running: runningSharpPids[app.id] }">
            <div class="sharp-card-banner">
              <img
                v-if="app.cover"
                :src="`http://127.0.0.1:9274/sharp-library/cover?id=${app.id}`"
                :alt="app.name"
                :style="{ objectPosition: coverPosition(app) }"
              />
              <img v-else :src="sharpLogoUrl" :alt="`${app.name} default artwork`" class="sharp-cover-fallback" />
              <button
                v-if="runningSharpPids[app.id]"
                class="running-close-button"
                title="Close application"
                @click="stopSharpApp(app)"
              >
                <IconX width="14" height="14" />
              </button>
            </div>
            <div class="sharp-card-body">
              <div class="sharp-card-title">{{ app.name }}</div>
              <div class="sharp-card-meta">
                <span class="badge badge-ok">Sharp App</span>
                <span v-if="bottleForApp(app)" class="badge" :class="bottleBadgeClass(bottleForApp(app)!.health)">
                  {{ bottleBadgeLabel(bottleForApp(app)!) }}
                </span>
                <span class="sharp-card-size">{{ formatBytes(app.size_bytes) }}</span>
              </div>
              <div
                v-if="bottleForApp(app)?.last_launch_status === 'exited' && bottleForApp(app)?.last_launch_log"
                class="sharp-card-bottle"
              >
                <span class="sharp-card-launch-log">
                  <a
                    href="#"
                    @click.prevent="openBottleLaunchLog(bottleForApp(app)!)"
                    :title="bottleForApp(app)!.last_launch_log ?? ''"
                  >
                    Open launch log
                  </a>
                </span>
              </div>
              <div class="sharp-card-actions">
                <div class="sharp-card-actions-row">
                  <button v-if="runningSharpPids[app.id]" class="btn btn-stop" @click="stopSharpApp(app)">Stop</button>
                  <button v-else class="btn btn-play" @click="launchApp(app.id, app.engine)">Play</button>
                  <select
                    class="control-input"
                    :value="app.engine"
                    @change="updateEngine(app.id, ($event.target as HTMLSelectElement).value)"
                  >
                    <option v-if="app.engine === 'm12'" value="m12" hidden>M12</option>
                    <option v-for="option in engineOptions" :key="option.id" :value="option.id">
                      {{ option.name }}
                    </option>
                  </select>
                  <button
                    class="btn btn-secondary sharp-tools-button"
                    type="button"
                    :aria-expanded="cardToolsOpen[app.id] === true"
                    @click="cardToolsOpen[app.id] = !cardToolsOpen[app.id]"
                  >
                    Tools
                  </button>
                </div>
                <div v-if="cardToolsOpen[app.id]" class="sharp-card-tools">
                  <div class="sharp-tool-actions">
                    <button class="btn btn-secondary btn-sm" @click="setCover(app.id)">Set Cover</button>
                    <button
                      class="btn btn-secondary btn-sm"
                      :disabled="!app.bottle_id"
                      :title="
                        app.bottle_id
                          ? 'Copy a file into this app bottle prefix'
                          : 'This app is not associated with a bottle'
                      "
                      @click="addAsset(app)"
                    >
                      Add Asset
                    </button>
                  </div>
                  <div v-if="app.cover" class="cover-position-controls">
                    <label>
                      <span>X</span>
                      <input
                        v-model.number="app.cover_position_x"
                        type="range"
                        min="0"
                        max="100"
                        @change="updateCoverPosition(app)"
                      />
                    </label>
                    <label>
                      <span>Y</span>
                      <input
                        v-model.number="app.cover_position_y"
                        type="range"
                        min="0"
                        max="100"
                        @change="updateCoverPosition(app)"
                      />
                    </label>
                  </div>
                  <button class="btn btn-danger btn-sm sharp-uninstall-button" @click="uninstallApp(app.id)">
                    Uninstall
                  </button>
                </div>
                <div v-if="launchErrors[app.id]" class="launch-failure">
                  <span>Last launch failed</span>
                  <strong>{{ launchErrors[app.id] }}</strong>
                </div>
                <details v-if="doctorOpen[app.id]" class="doctor-panel" open>
                  <summary class="drawer-summary">
                    <span>Launch Doctor</span>
                    <small>{{ doctorReports[app.id]?.summary ?? "Checking launch prerequisites" }}</small>
                  </summary>
                  <div v-if="doctorLoading[app.id]" class="doctor-loading">Checking launch prerequisites...</div>
                  <template v-else-if="doctorReports[app.id]">
                    <div class="doctor-summary">
                      <span class="badge" :class="doctorReports[app.id]?.ready ? 'badge-ok' : 'badge-warn'">
                        {{ doctorReports[app.id]?.ready ? "Ready" : "Blocked" }}
                      </span>
                      <span>{{ doctorReports[app.id]?.summary }}</span>
                    </div>
                    <div class="doctor-checks">
                      <div
                        v-for="check in doctorReports[app.id]?.checks ?? []"
                        :key="check.id"
                        class="doctor-check"
                        :class="{ failed: !check.ok }"
                      >
                        <span class="doctor-check-state">{{ check.ok ? "OK" : "!" }}</span>
                        <span class="doctor-check-label">{{ check.label }}</span>
                        <span class="doctor-check-detail">
                          {{ check.detail }}
                          <button
                            v-if="!check.ok || check.id === 'launcher_exe'"
                            class="doctor-action"
                            @click="runDoctorAction(app, check)"
                          >
                            {{ doctorActionLabel(check, app) }}
                          </button>
                        </span>
                      </div>
                    </div>
                    <div v-if="doctorReports[app.id]?.recipe.launch_args.length" class="doctor-notes">
                      <div>Args: {{ doctorReports[app.id]?.recipe.launch_args.join(" ") }}</div>
                    </div>
                    <div v-if="doctorReports[app.id]?.blockers.length" class="doctor-notes blocked">
                      <div v-for="blocker in doctorReports[app.id]?.blockers" :key="blocker">{{ blocker }}</div>
                    </div>
                    <div v-if="doctorReports[app.id]?.warnings.length" class="doctor-notes">
                      <div v-for="warning in doctorReports[app.id]?.warnings" :key="warning">{{ warning }}</div>
                    </div>
                  </template>
                </details>
                <details v-if="diagnosticsOpen[app.id]" class="diagnostics-panel" open>
                  <summary class="drawer-summary">
                    <span>Logs and crash reports</span>
                    <small
                      >{{ recentCrashReports[app.id]?.length ?? 0 }} crash reports ·
                      {{ recentLogLines[app.id]?.length ?? 0 }} log lines</small
                    >
                  </summary>
                  <div class="diagnostics-toolbar">
                    <button class="btn btn-secondary btn-sm" @click="clearShaderCache(app)">
                      Clear All Shader Caches
                    </button>
                    <button class="btn btn-secondary btn-sm" @click="openLogFolder">Open Logs</button>
                    <button class="btn btn-secondary btn-sm" @click="copyDiagnosticBundle(app)">Copy Bundle</button>
                  </div>
                  <div v-if="recentCrashReports[app.id]?.length" class="diagnostics-section">
                    <div class="diagnostics-title">Recent crash reports</div>
                    <div v-for="report in recentCrashReports[app.id]" :key="report.file" class="crash-row">
                      <span>{{ report.name }}</span>
                      <small>{{ report.timestamp }} · {{ report.source }}</small>
                    </div>
                  </div>
                  <div class="diagnostics-section">
                    <div class="diagnostics-title">Recent launch log</div>
                    <pre class="log-tail">{{
                      (recentLogLines[app.id] ?? ["No recent log lines loaded."]).join("\n")
                    }}</pre>
                  </div>
                </details>
              </div>
            </div>
          </div>
        </div>
      </template>

      <template v-else-if="sourceMode === 'gog'">
        <section class="gog-panel">
          <div v-if="!gogStatus?.gogdlAvailable" class="empty-state compact">
            <h2>gogdl is not installed</h2>
            <p>Install gogdl under ~/.metalsharp/tools/gogdl or set METALSHARP_GOGDL_BIN.</p>
          </div>
          <div v-else-if="!gogStatus?.prefixInitialized" class="empty-state compact">
            <h2>Initialize GOG prefix</h2>
            <p>Create the isolated Wine prefix before connecting games.</p>
          </div>
          <div v-else-if="!gogStatus?.authenticated" class="empty-state compact">
            <h2>Login to GOG to connect your games</h2>
            <p>MetalSharp will capture the GOG login code from a controlled sign-in window.</p>
          </div>
          <div v-else-if="gogGames.length === 0" class="empty-state compact">
            <h2>No GOG games synced</h2>
            <p>Click Sync Library after adding games to your GOG account.</p>
          </div>

          <div v-else class="sharp-grid">
            <div
              v-for="game in gogGames"
              :key="game.productId"
              class="sharp-card gog-card"
              :class="{ running: game.running }"
            >
              <div class="sharp-card-banner">
                <img v-if="game.imageUrl" :src="game.imageUrl" :alt="game.title" />
                <img v-else :src="sharpLogoUrl" :alt="`${game.title} default artwork`" class="sharp-cover-fallback" />
                <button v-if="game.running" class="running-close-button" title="Stop game" @click="stopGogGame(game)">
                  <IconX width="14" height="14" />
                </button>
              </div>
              <div class="sharp-card-body">
                <div class="sharp-card-title">{{ game.title }}</div>
                <div class="sharp-card-meta">
                  <span
                    class="badge"
                    :class="
                      game.running
                        ? 'badge-ok'
                        : game.installed
                          ? 'badge-ok'
                          : game.status === 'downloading'
                            ? 'badge-warn'
                            : 'badge-muted'
                    "
                  >
                    {{
                      game.running
                        ? "Running"
                        : game.installed
                          ? "Installed"
                          : game.status === "downloading"
                            ? "Downloading"
                            : "GOG"
                    }}
                  </span>
                  <span v-if="game.downloadSizeBytes" class="sharp-card-size">{{
                    formatBytes(game.downloadSizeBytes)
                  }}</span>
                </div>
                <div v-if="game.status === 'downloading'" class="gog-progress">
                  <div class="gog-progress-bar">
                    <span :style="{ width: `${gogProgress[game.productId] ?? 0}%` }"></span>
                  </div>
                  <small>{{ Math.floor(gogProgress[game.productId] ?? 0) }}%</small>
                </div>
                <div class="sharp-card-actions">
                  <div class="sharp-card-actions-row">
                    <button
                      v-if="game.running"
                      class="btn btn-stop"
                      :disabled="gogLoading[`${game.productId}:stop`]"
                      @click="stopGogGame(game)"
                    >
                      Stop
                    </button>
                    <button
                      v-else-if="game.installed"
                      class="btn btn-play"
                      :disabled="gogLoading[`${game.productId}:play`]"
                      @click="playGogGame(game)"
                    >
                      Play
                    </button>
                    <button
                      v-else
                      class="btn btn-primary"
                      :disabled="game.status === 'downloading' || gogLoading[`${game.productId}:install`]"
                      @click="installGogGame(game)"
                    >
                      {{ game.status === "downloading" ? "Downloading…" : "Install" }}
                    </button>
                    <button
                      v-if="game.installed"
                      class="btn btn-danger"
                      :disabled="gogLoading[`${game.productId}:uninstall`]"
                      @click="uninstallGogGame(game)"
                    >
                      Uninstall
                    </button>
                    <select
                      v-if="game.installed"
                      class="control-input gog-pipeline-select"
                      :value="gogEngines[game.productId] ?? 'auto'"
                      aria-label="GOG bottle pipeline"
                      @change="updateGogEngine(game, ($event.target as HTMLSelectElement).value)"
                    >
                      <option value="auto">Auto</option>
                      <option v-for="option in engineOptions" :key="option.id" :value="option.id">
                        {{ option.name }}
                      </option>
                    </select>
                  </div>
                  <div v-if="game.status === 'install_failed' && game.lastError" class="gog-card-meta">
                    <strong class="launch-failure">{{ game.lastError }}</strong>
                  </div>
                </div>
              </div>
            </div>
          </div>
        </section>
      </template>

      <template v-else-if="sourceMode === 'gamejolt'">
        <section
          ref="gamejoltPanel"
          class="gamejolt-panel"
          :class="{ dragging: gamejoltBrowserDragging }"
          :style="gameJoltPanelStyle()"
          @pointermove="moveGameJoltBrowserDrag"
          @pointerup="endGameJoltBrowserDrag"
          @pointercancel="endGameJoltBrowserDrag"
        >
          <div class="gamejolt-games-pane">
            <div v-if="gamejoltGames.length === 0" class="empty-state compact">
              <h2>No GameJolt games found</h2>
              <p>Place each game in its own folder inside the GameJolt directory, then sync.</p>
            </div>
            <div v-else class="gamejolt-grid">
              <article
                v-for="game in gamejoltGames"
                :key="game.id"
                class="sharp-card gamejolt-card"
                :class="{ running: gamejoltRunningPids[game.id] }"
              >
                <div class="sharp-card-banner">
                  <img
                    v-if="game.cover_path"
                    :src="`http://127.0.0.1:9274/gamejolt/cover?id=${encodeURIComponent(game.id)}`"
                    :alt="game.name"
                  />
                  <img v-else :src="sharpLogoUrl" :alt="`${game.name} default artwork`" class="sharp-cover-fallback" />
                </div>
                <div class="sharp-card-body">
                  <div class="sharp-card-title gamejolt-card-title">
                    <input
                      v-if="editingGameJoltName === game.id"
                      v-model="gameJoltNameDraft"
                      class="gamejolt-name-input"
                      maxlength="160"
                      autofocus
                      @keydown.enter.prevent="saveGameJoltName(game)"
                      @keydown.esc="editingGameJoltName = null"
                      @blur="saveGameJoltName(game)"
                    />
                    <span v-else>{{ game.name }}</span>
                    <button
                      v-if="editingGameJoltName !== game.id"
                      class="gamejolt-edit-name"
                      type="button"
                      title="Rename game"
                      aria-label="Rename game"
                      @click.stop="beginGameJoltNameEdit(game)"
                    >
                      <IconPencil width="13" height="13" />
                    </button>
                  </div>
                  <div class="sharp-card-meta">
                    <span class="badge" :class="game.native ? 'badge-ok' : 'badge-muted'">
                      {{ game.native ? "Native macOS" : "Windows" }}
                    </span>
                  </div>
                  <div class="gamejolt-actions-main">
                    <button v-if="gamejoltRunningPids[game.id]" class="btn btn-stop" @click="stopGameJolt(game)">
                      Stop
                    </button>
                    <button v-else class="btn btn-play" @click="launchGameJolt(game)">Play</button>
                    <button
                      class="btn btn-uninstall"
                      type="button"
                      :disabled="!!gamejoltRunningPids[game.id]"
                      @click="uninstallGameJolt(game)"
                    >
                      Uninstall
                    </button>
                    <select
                      v-if="!game.native"
                      class="control-input gamejolt-pipeline-select"
                      v-model="game.engine"
                      aria-label="GameJolt bottle pipeline"
                      @change="updateGameJoltEngine(game, game.engine)"
                    >
                      <option v-for="option in game.available_pipelines" :key="option.id" :value="option.id">
                        {{ option.name }}
                      </option>
                    </select>
                    <span v-else class="gamejolt-native-note">No bottle required</span>
                  </div>
                </div>
              </article>
            </div>
          </div>
          <div class="gamejolt-browser-frame">
            <div
              class="gamejolt-browser-handle"
              role="separator"
              aria-label="Resize GameJolt browser"
              aria-orientation="horizontal"
              @pointerdown="beginGameJoltBrowserDrag"
            >
              <span class="gamejolt-browser-arrow" aria-hidden="true">↕</span>
            </div>
            <webview class="gamejolt-browser" src="https://gamejolt.com/games" partition="persist:gamejolt"></webview>
          </div>
        </section>
      </template>

      <template v-else-if="sourceMode === 'rpcs3'">
        <section class="emulator-panel rpcs3-dashboard">
          <div class="rpcs3-overview">
            <div class="rpcs3-overview-main">
              <div class="rpcs3-brand-mark" aria-hidden="true"><IconGamepad2 width="26" height="26" /></div>
              <div class="rpcs3-overview-copy">
                <div class="rpcs3-eyebrow">Managed PlayStation 3 environment</div>
                <div class="rpcs3-title-row">
                  <h2>RPCS3</h2>
                  <span
                    class="rpcs3-state-pill"
                    :class="rpcs3Status?.state === 'ready' ? 'ready' : rpcs3Status?.installed ? 'attention' : 'muted'"
                  >
                    <span class="rpcs3-state-dot" aria-hidden="true"></span>{{ rpcs3StateLabel }}
                  </span>
                </div>
                <p>A verified, isolated emulator environment with atomic updates and protected user data.</p>
              </div>
            </div>
            <div class="rpcs3-primary-actions">
              <button
                class="btn btn-primary rpcs3-primary-button"
                :disabled="rpcs3Loading.update || rpcs3Loading.check"
                @click="installOrUpdateRpcs3"
              >
                <IconDownload width="15" height="15" />
                {{ rpcs3Loading.update ? "Installing…" : rpcs3Status?.installed ? "Update RPCS3" : "Install RPCS3" }}
              </button>
              <button v-if="rpcs3Status?.installed" class="btn btn-secondary" @click="openRpcs3">
                <IconExternalLink width="14" height="14" /> Open RPCS3
              </button>
            </div>
            <div class="rpcs3-stats">
              <div class="rpcs3-stat">
                <IconHardDrive width="16" height="16" />
                <span
                  ><small>Runtime</small
                  ><strong :title="rpcs3Status?.currentTag ?? ''">{{ rpcs3BuildLabel }}</strong></span
                >
              </div>
              <div class="rpcs3-stat">
                <IconShieldCheck width="16" height="16" />
                <span
                  ><small>Firmware</small
                  ><strong>{{ rpcs3Status?.firmwareInstalled ? "Installed" : "Required" }}</strong></span
                >
              </div>
              <div class="rpcs3-stat">
                <IconGamepad2 width="16" height="16" />
                <span
                  ><small>Library</small
                  ><strong>{{ rpcs3Games.length }} game{{ rpcs3Games.length === 1 ? "" : "s" }}</strong></span
                >
              </div>
              <div v-if="rpcs3Update" class="rpcs3-stat rpcs3-stat-wide">
                <IconShieldCheck width="16" height="16" />
                <span
                  ><small>Latest verified release</small
                  ><strong>{{ rpcs3Update.latestVersion }} · {{ formatBytes(rpcs3Update.downloadSize) }}</strong></span
                >
              </div>
            </div>
          </div>

          <div class="rpcs3-command-bar" aria-label="RPCS3 library actions">
            <button class="rpcs3-command" :disabled="!rpcs3Status?.installed" @click="installRpcs3Content('firmware')">
              <IconShieldCheck width="17" height="17" /><span
                ><strong>Firmware</strong><small>Install PS3UPDAT.PUP</small></span
              >
            </button>
            <button class="rpcs3-command" :disabled="!rpcs3Status?.installed" @click="installRpcs3Content('package')">
              <IconPackage width="17" height="17" /><span
                ><strong>Install package</strong><small>Add an owned PKG</small></span
              >
            </button>
            <button class="rpcs3-command" @click="addRpcs3Folder">
              <IconFolderPlus width="17" height="17" /><span
                ><strong>Add games</strong><small>Choose a library folder</small></span
              >
            </button>
            <button class="rpcs3-command" :disabled="rpcs3Loading.check" @click="refreshRpcs3(true)">
              <IconScanLine width="17" height="17" /><span
                ><strong>Scan library</strong><small>Refresh games and artwork</small></span
              >
            </button>
          </div>

          <div
            v-if="rpcs3UpdateProgress?.running || rpcs3UpdateProgress?.status === 'failed'"
            class="emulator-update-card"
          >
            <div class="emulator-progress-row">
              <strong>{{ rpcs3UpdateProgress.message }}</strong>
              <span>{{ rpcs3UpdateProgress.percent }}%</span>
            </div>
            <div class="gog-progress-bar"><span :style="{ width: `${rpcs3UpdateProgress.percent}%` }"></span></div>
            <small v-if="rpcs3UpdateProgress.error" class="launch-failure">{{ rpcs3UpdateProgress.error }}</small>
          </div>

          <details class="rpcs3-management">
            <summary>
              <span>Environment management</span>
              <small>Updates, rollback, storage, and advanced controls</small>
            </summary>
            <div class="rpcs3-management-actions">
              <button class="btn btn-secondary btn-sm" :disabled="rpcs3Loading.check" @click="checkRpcs3Update()">
                {{ rpcs3Loading.check ? "Checking…" : "Check Updates" }}
              </button>
              <button
                v-if="rpcs3Status?.installed && rpcs3Update"
                class="btn btn-secondary btn-sm"
                @click="setRpcs3UpdatePolicy(rpcs3Update.pinnedTag ? 'unpin' : 'pin-current')"
              >
                {{ rpcs3Update.pinnedTag ? "Unpin Build" : "Pin Current" }}
              </button>
              <button
                v-if="rpcs3Update?.available"
                class="btn btn-secondary btn-sm"
                @click="setRpcs3UpdatePolicy('skip-update')"
              >
                Skip Update
              </button>
              <button
                v-if="rpcs3Update?.skippedTag"
                class="btn btn-secondary btn-sm"
                @click="setRpcs3UpdatePolicy('clear-skip')"
              >
                Clear Skip
              </button>
              <button v-if="rpcs3Status?.rollbackAvailable" class="btn btn-secondary btn-sm" @click="rollbackRpcs3">
                Rollback
              </button>
              <button
                v-if="rpcs3Status?.environmentPath"
                class="btn btn-secondary btn-sm"
                @click="getAPI().openRpcs3Path(rpcs3Status.environmentPath)"
              >
                Open Environment
              </button>
              <button v-if="rpcs3Status?.installed" class="btn btn-danger btn-sm" @click="removeRpcs3Runtime">
                Remove Runtime
              </button>
            </div>
          </details>

          <details v-if="rpcs3Roots.length" class="emulator-roots">
            <summary>
              Game folders <span>{{ rpcs3Roots.length }}</span>
            </summary>
            <div v-for="root in rpcs3Roots" :key="root" class="emulator-root-row">
              <button class="emulator-root-path" type="button" @click="getAPI().openRpcs3Path(root)">{{ root }}</button>
              <button class="btn btn-secondary btn-sm" type="button" @click="removeRpcs3Root(root)">Remove</button>
            </div>
          </details>

          <div v-if="!rpcs3Status?.installed" class="rpcs3-onboarding">
            <div class="rpcs3-onboarding-icon"><IconDownload width="24" height="24" /></div>
            <div>
              <span class="rpcs3-step">Step 1 of 3</span>
              <h2>Install the verified RPCS3 runtime</h2>
              <p>
                MetalSharp selects the official build for this Mac, verifies its digest and signature, then activates it
                atomically.
              </p>
            </div>
            <button class="btn btn-primary" :disabled="rpcs3Loading.update" @click="installOrUpdateRpcs3">
              Install RPCS3
            </button>
          </div>
          <div v-else-if="!rpcs3Status.firmwareInstalled" class="rpcs3-onboarding">
            <div class="rpcs3-onboarding-icon"><IconShieldCheck width="24" height="24" /></div>
            <div>
              <span class="rpcs3-step">Step 2 of 3</span>
              <h2>Add PlayStation 3 firmware</h2>
              <p>Select your legally acquired PS3UPDAT.PUP. MetalSharp never downloads or bundles Sony firmware.</p>
            </div>
            <div class="rpcs3-onboarding-actions">
              <button
                class="btn btn-secondary"
                type="button"
                title="Open the official PlayStation 3 system software page"
                @click="getAPI().openRpcs3FirmwarePage()"
              >
                <IconExternalLink width="13" height="13" /> Firmware Link
              </button>
              <button class="btn btn-primary" @click="installRpcs3Content('firmware')">Select Firmware</button>
            </div>
          </div>
          <div v-else-if="rpcs3Roots.length === 0" class="rpcs3-onboarding">
            <div class="rpcs3-onboarding-icon"><IconFolderPlus width="24" height="24" /></div>
            <div>
              <span class="rpcs3-step">Step 3 of 3</span>
              <h2>Build your PlayStation 3 library</h2>
              <p>Add a folder containing disc layouts or install a legally acquired package.</p>
            </div>
            <button class="btn btn-primary" @click="addRpcs3Folder">Add Games Folder</button>
          </div>

          <div v-else-if="rpcs3Games.length === 0" class="rpcs3-onboarding">
            <div class="rpcs3-onboarding-icon"><IconScanLine width="24" height="24" /></div>
            <div>
              <span class="rpcs3-step">Library folder added</span>
              <h2>No PlayStation 3 games found yet</h2>
              <p>MetalSharp saved your game folder. Add a supported disc layout to it, then scan the library again.</p>
            </div>
            <button class="btn btn-primary" @click="refreshRpcs3(true)">Scan Library</button>
          </div>

          <div v-else class="sharp-grid">
            <article
              v-for="game in rpcs3Games"
              :key="game.id"
              class="sharp-card emulator-game-card"
              :class="{ running: game.running }"
            >
              <div class="sharp-card-banner">
                <img
                  v-if="game.hasArtwork"
                  :src="`http://127.0.0.1:9274/sharp-library/rpcs3/cover?id=${encodeURIComponent(game.id)}`"
                  :alt="game.title"
                />
                <img v-else :src="sharpLogoUrl" :alt="`${game.title} default artwork`" class="sharp-cover-fallback" />
                <button v-if="game.running" class="running-close-button" title="Stop game" @click="stopRpcs3Game(game)">
                  <IconX width="14" height="14" />
                </button>
              </div>
              <div class="sharp-card-body">
                <div class="sharp-card-title">{{ game.title }}</div>
                <div class="sharp-card-meta">
                  <span class="badge" :class="game.running ? 'badge-ok' : 'badge-muted'">{{
                    game.running ? "Running" : game.titleId || "PS3"
                  }}</span>
                  <span v-if="game.version" class="sharp-card-size">v{{ game.version }}</span>
                </div>
                <div class="sharp-card-actions-row">
                  <button v-if="game.running" class="btn btn-stop" @click="stopRpcs3Game(game)">Stop</button>
                  <button
                    v-else
                    class="btn btn-play"
                    :disabled="!rpcs3Status?.firmwareInstalled"
                    @click="launchRpcs3Game(game)"
                  >
                    Play
                  </button>
                  <button class="btn btn-secondary" @click="getAPI().openRpcs3Path(game.path)">Open Folder</button>
                  <button
                    v-if="game.lastLogPath"
                    class="btn btn-secondary"
                    @click="getAPI().openRpcs3Path(game.lastLogPath)"
                  >
                    Log
                  </button>
                </div>
              </div>
            </article>
          </div>
        </section>
      </template>
    </div>
  </div>
</template>

<style scoped>
.sharp-view {
  padding: 0 28px;
  height: 100%;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  background: transparent;
}
.sharp-header {
  flex-shrink: 0;
  height: 160px;
  display: flex;
  flex-direction: column;
  margin: 0 -28px;
  padding: 44px 28px 14px;
  border-bottom: 1px solid var(--border);
  -webkit-app-region: drag;
  position: relative;
}
.sharp-body {
  flex: 1;
  margin: 0 -28px;
  padding: 20px 28px 32px;
}
.sharp-header::after {
  content: "";
  position: absolute;
  inset: 0;
  background:
    radial-gradient(ellipse 60% 80% at 20% 50%, rgba(95, 183, 232, 0.08) 0%, transparent 70%),
    radial-gradient(ellipse 40% 60% at 80% 50%, rgba(95, 183, 232, 0.05) 0%, transparent 60%);
  pointer-events: none;
}
.sharp-header-controls {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  -webkit-app-region: no-drag;
  margin-top: auto;
  min-height: 0;
  container-type: inline-size;
}
@media (max-width: 1040px) {
  .sharp-header {
    height: 202px;
  }
}
.btn-label-short {
  display: none;
}
@container (max-width: 700px) {
  .btn-label-long {
    display: none;
  }
  .btn-label-short {
    display: inline;
  }
}
.sharp-header-controls .btn {
  min-width: 0;
  flex-shrink: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.btn-icon {
  flex-shrink: 0;
}
.sharp-header-title {
  display: flex;
  flex-direction: column;
  gap: 6px;
  min-width: 0;
}
.sharp-header h1 {
  font-size: 24px;
  font-weight: 750;
  line-height: 1.1;
}
.sharp-header-title p {
  max-width: 720px;
  margin: 0;
  color: var(--text-secondary);
  font-size: 13px;
  line-height: 1.35;
}
.sharp-header-controls {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  overflow: visible;
  -webkit-app-region: no-drag;
}
.source-tabs {
  display: inline-flex;
  align-items: stretch;
  gap: 2px;
  min-height: 34px;
  max-width: 100%;
  overflow-x: auto;
  flex-shrink: 1;
  border-bottom: 1px solid var(--border-strong);
  scrollbar-width: none;
}
.source-tabs::-webkit-scrollbar {
  display: none;
}
.source-tab {
  flex: 0 0 auto;
  position: relative;
  padding: 0 12px;
  color: var(--text-secondary);
  border: 0;
  background: transparent;
  font: inherit;
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
}
.source-tab::after {
  position: absolute;
  right: 8px;
  bottom: -1px;
  left: 8px;
  height: 2px;
  content: "";
  background: transparent;
}
.source-tab:hover {
  color: var(--text-primary);
}
.source-tab.active {
  color: var(--text-primary);
}
.source-tab.active::after {
  background: var(--accent);
}

.support-drawer {
  margin-bottom: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: color-mix(in srgb, var(--bg-card) 82%, transparent);
  overflow: hidden;
}
.dropdown-wrap {
  position: relative;
  min-width: 0;
  flex-shrink: 1;
  overflow: hidden;
}
.dropdown-count {
  opacity: 0.5;
  font-size: 11px;
  margin-left: 2px;
}
.dropdown-panel {
  position: fixed;
  max-height: min(60vh, 520px);
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.3);
  z-index: 100;
  overflow: hidden;
}
.dropdown-scroll {
  overflow-y: auto;
  max-height: min(60vh, 520px);
  padding: 8px;
}
.bottle-card-compact {
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  margin-bottom: 6px;
  background: var(--bg-surface);
}
.bottle-card-compact:last-child {
  margin-bottom: 0;
}
.redist-source-compact {
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  margin-bottom: 6px;
  background: var(--bg-surface);
  font-size: 12px;
}
.redist-source-compact:last-child {
  margin-bottom: 0;
}
.bottle-list {
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.bottle-card-main {
  display: grid;
  grid-template-columns: minmax(170px, 1.2fr) minmax(300px, 1.8fr) auto;
  align-items: center;
  gap: 14px;
}
.bottle-identity {
  min-width: 0;
}
.bottle-title {
  max-width: 100%;
  overflow: hidden;
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 700;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.bottle-meta,
.bottle-components {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 7px;
  color: var(--text-dim);
  font-size: 10px;
}
.bottle-facts {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
  min-width: 0;
}
.bottle-facts span {
  min-width: 0;
  color: var(--text-secondary);
  font-size: 11px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.bottle-facts strong {
  display: block;
  margin-bottom: 2px;
  color: var(--text-dim);
  font-size: 9px;
  font-weight: 700;
  text-transform: uppercase;
}
.bottle-actions {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 6px;
}
.bottle-control-surface {
  margin-top: 8px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 82%, transparent);
}
.bottle-control-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, max-content)) minmax(190px, 1fr) auto;
  gap: 8px;
  padding: 9px;
}
.bottle-control-grid .control-input {
  min-width: 0;
}
.windows-version-controls {
  display: flex;
  gap: 6px;
}
.bottle-detections {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: 8px;
}
.component-pill {
  padding: 3px 6px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  color: var(--text-secondary);
}
.component-pill.pill-ok {
  color: var(--color-green, #4ade80);
  border-color: var(--color-green, #4ade80);
}
.component-pill.pill-missing {
  color: var(--color-red, #f87171);
  border-color: var(--color-red, #f87171);
}
.component-pill.pill-warn {
  color: var(--color-yellow, #facc15);
  border-color: var(--color-yellow, #facc15);
}
.fna-component-header {
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  color: var(--text-secondary);
  margin-bottom: 4px;
  grid-column: 1 / -1;
}
.component-source-row {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  align-items: baseline;
  margin-top: 4px;
}
.source-label {
  font-weight: 600;
  min-width: 110px;
}
.source-ok {
  color: var(--color-green, #4ade80);
}
.source-missing {
  color: var(--color-red, #f87171);
}
.source-detail {
  color: var(--text-secondary);
  font-size: 11px;
  flex-basis: 100%;
}
.bottle-report {
  margin-top: 10px;
  padding-top: 10px;
  border-top: 1px solid var(--border);
}
.bottle-action-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  margin-top: 6px;
}
.bottle-action-row span {
  overflow-wrap: anywhere;
}
@media (max-width: 980px) {
  .bottle-card-main {
    grid-template-columns: minmax(0, 1fr);
    align-items: start;
  }
  .bottle-facts {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
  .bottle-actions {
    justify-content: flex-start;
  }
  .bottle-control-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
.compatibility-table {
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  overflow: hidden;
  background: var(--bg-card);
}
.compatibility-row {
  display: grid;
  grid-template-columns: minmax(170px, 1.3fr) 100px 92px 92px 92px minmax(150px, 1fr) minmax(130px, 0.9fr);
  gap: 10px;
  align-items: center;
  padding: 8px 10px;
  border-top: 1px solid var(--border);
  color: var(--text-secondary);
  font-size: 11px;
}
.compatibility-row:first-child {
  border-top: 0;
}
.compatibility-header {
  color: var(--text-dim);
  font-weight: 700;
  text-transform: uppercase;
}
.compatibility-row strong,
.compatibility-row small {
  display: block;
}
.compatibility-row small {
  margin-top: 2px;
  color: var(--text-dim);
}
.compatibility-select,
.compatibility-input {
  min-width: 0;
  width: 100%;
  height: 28px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--bg-input);
  color: var(--text-secondary);
  font-size: 11px;
}
.compatibility-input {
  padding: 4px 7px;
}
.redist-source-list {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 10px;
}
.redist-source-card {
  padding: 12px;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-card);
}
.redist-source-card strong,
.redist-source-card small {
  display: block;
}
.redist-source-card small,
.redist-source-card p {
  margin-top: 4px;
  color: var(--text-dim);
  font-size: 11px;
  line-height: 1.4;
}
.redist-targets {
  display: flex;
  flex-direction: column;
  gap: 4px;
  margin: 8px 0;
}
.redist-targets span {
  padding: 4px 6px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  color: var(--text-secondary);
  font-size: 10px;
  overflow-wrap: anywhere;
}

.sharp-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 12px;
  align-items: start;
}
.gog-panel {
  display: flex;
  flex-direction: column;
  gap: 16px;
}
.gog-setup-card {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  padding: 16px;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-card);
}
.gog-setup-card h2 {
  margin: 0 0 6px;
  color: var(--text-primary);
}
.gog-setup-card p {
  margin: 0 0 8px;
  color: var(--text-secondary);
}
.gog-setup-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  flex-wrap: wrap;
  gap: 8px;
}
.gog-progress {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
}
.gog-progress-bar {
  flex: 1;
  height: 8px;
  overflow: hidden;
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg-deep) 70%, transparent);
}
.gog-progress-bar span {
  display: block;
  height: 100%;
  border-radius: inherit;
  background: linear-gradient(90deg, var(--accent), #7ee787);
}
.gog-card-meta {
  display: flex;
  flex-direction: column;
  gap: 4px;
  color: var(--text-dim);
  font-size: 11px;
  overflow-wrap: anywhere;
}

.sharp-card {
  align-self: start;
  height: fit-content;
  background: var(--bg-card);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  overflow: hidden;
  box-shadow:
    0 0 0 1px color-mix(in srgb, var(--accent) 22%, transparent),
    0 0 26px color-mix(in srgb, var(--accent) 20%, transparent),
    0 16px 34px color-mix(in srgb, var(--bg-deep) 34%, transparent);
  transition:
    transform var(--transition),
    border-color var(--transition),
    box-shadow var(--transition);
}
.sharp-card:hover {
  border-color: var(--border-strong);
  transform: translateY(-1px);
  box-shadow:
    0 0 0 1px color-mix(in srgb, var(--accent) 36%, transparent),
    0 0 34px color-mix(in srgb, var(--accent) 28%, transparent),
    0 20px 42px color-mix(in srgb, var(--bg-deep) 42%, transparent);
}
.gog-card .sharp-card-banner {
  isolation: isolate;
  background: color-mix(in srgb, var(--bg-deep) 94%, black);
}
.gog-card .sharp-card-banner::after {
  content: "";
  position: absolute;
  inset: 0;
  z-index: 1;
  pointer-events: none;
  background:
    linear-gradient(to bottom, rgba(5, 10, 14, 0.06) 0%, rgba(5, 10, 14, 0.18) 45%, rgba(5, 10, 14, 0.62) 100%),
    linear-gradient(to right, rgba(5, 10, 14, 0.2), transparent 28%, transparent 72%, rgba(5, 10, 14, 0.2));
  mix-blend-mode: multiply;
}
.gog-card .sharp-card-banner img {
  position: relative;
  z-index: 0;
  object-position: center top;
  filter: brightness(0.68) contrast(1.55) saturate(1.22);
}
.sharp-card.running {
  border-color: var(--success);
  box-shadow:
    0 0 0 1px color-mix(in srgb, var(--success) 48%, transparent),
    0 0 34px color-mix(in srgb, var(--success) 28%, transparent),
    0 20px 42px color-mix(in srgb, var(--bg-deep) 42%, transparent);
}
.sharp-card-banner {
  width: 100%;
  aspect-ratio: 16 / 5.6;
  height: auto;
  background: var(--bg-surface);
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  position: relative;
}
.sharp-card-banner img {
  width: 100%;
  height: 100%;
  object-fit: cover;
}
.sharp-card-banner img.sharp-cover-fallback {
  object-fit: contain;
  padding: 26px;
  background:
    radial-gradient(circle at 50% 45%, color-mix(in srgb, var(--accent) 18%, transparent), transparent 48%),
    var(--bg-surface);
}
.running-close-button {
  position: absolute;
  top: 8px;
  right: 8px;
  width: 30px;
  height: 30px;
  border: 1px solid color-mix(in srgb, var(--error) 44%, var(--border));
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-deep) 82%, transparent);
  color: var(--error);
  display: inline-flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  box-shadow: 0 8px 22px var(--card-glow);
}
.running-close-button:hover {
  border-color: var(--error);
  background: var(--error-bg);
}
.sharp-card-body {
  padding: 11px 12px 12px;
}
.sharp-card-title {
  font-size: 14px;
  font-weight: 600;
  margin-bottom: 5px;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}
.gamejolt-card-title {
  display: flex;
  align-items: center;
  gap: 6px;
}
.gamejolt-card-title > span {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
}
.gamejolt-edit-name {
  display: inline-grid;
  flex: 0 0 auto;
  place-items: center;
  width: 22px;
  height: 22px;
  padding: 0;
  color: var(--text-dim);
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: transparent;
  cursor: pointer;
}
.gamejolt-edit-name:hover {
  color: var(--accent);
  border-color: var(--border);
  background: var(--sidebar-hover);
}
.gamejolt-name-input {
  min-width: 0;
  width: 100%;
  padding: 3px 6px;
  color: var(--text-primary);
  border: 1px solid var(--accent);
  border-radius: var(--radius-sm);
  background: var(--bg-input);
  font: inherit;
}
.sharp-card-meta {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-bottom: 10px;
}
.sharp-card-bottle {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  font-size: 11px;
}
.sharp-card-launch-log a {
  color: var(--accent, #4ea8de);
  text-decoration: none;
}
.sharp-card-launch-log a:hover {
  text-decoration: underline;
}
.sharp-card-size {
  font-size: 11px;
  color: var(--text-dim);
}
.sharp-card-actions {
  display: flex;
  flex-direction: column;
  gap: 7px;
}
.sharp-card-actions-row {
  display: flex;
  align-items: center;
  gap: 8px;
}
.sharp-card-actions-row .btn-play,
.sharp-card-actions-row .btn-stop {
  min-width: 58px;
}
.sharp-card-actions-row .control-input {
  min-width: 0;
  flex: 1 1 auto;
}
.sharp-tools-button {
  flex: 0 0 auto;
}
.sharp-card-tools {
  padding: 8px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 72%, transparent);
}
.sharp-tool-actions {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 6px;
}
.sharp-tool-actions .btn {
  min-width: 0;
  padding-inline: 6px;
}
.cover-position-controls {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 7px;
  padding: 8px 0 0;
}
.cover-position-controls label {
  display: grid;
  grid-template-columns: 14px minmax(0, 1fr);
  align-items: center;
  gap: 6px;
  color: var(--text-dim);
  font-size: 10px;
  font-weight: 700;
}
.cover-position-controls input {
  min-width: 0;
}
.launch-options-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 6px;
  padding: 0 8px 8px;
}
.launch-options-input {
  width: 100%;
}
.sharp-uninstall-button {
  margin-top: 8px;
  width: 100%;
}

.launch-failure {
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 8px 10px;
  border: 1px solid color-mix(in srgb, var(--danger) 50%, var(--border));
  border-radius: var(--radius-md);
  background: color-mix(in srgb, var(--danger) 12%, var(--bg-surface));
  color: var(--text-secondary);
  font-size: 11px;
  line-height: 1.35;
}
.launch-failure span {
  color: var(--danger);
  font-weight: 700;
}
.launch-failure strong {
  font-weight: 500;
  overflow-wrap: anywhere;
}

.doctor-panel {
  margin-top: 2px;
  padding: 10px;
  background: var(--bg-surface);
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  font-size: 11px;
}
.drawer-summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
  cursor: pointer;
  color: var(--text-secondary);
  font-size: 11px;
  font-weight: 700;
  list-style: none;
}
.drawer-summary::-webkit-details-marker {
  display: none;
}
.drawer-summary::after {
  content: "v";
  color: var(--text-dim);
  transition: transform 120ms ease;
}
details:not([open]) > .drawer-summary::after {
  transform: rotate(-90deg);
}
.drawer-summary small {
  min-width: 0;
  flex: 1;
  color: var(--text-dim);
  font-size: 10px;
  font-weight: 500;
  overflow: hidden;
  text-align: right;
  text-overflow: ellipsis;
  white-space: nowrap;
}
details[open] > .drawer-summary {
  margin-bottom: 10px;
}
.doctor-loading {
  color: var(--text-dim);
}
.doctor-summary {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
  color: var(--text-secondary);
}
.doctor-checks {
  display: flex;
  flex-direction: column;
  gap: 5px;
}
.doctor-check {
  display: grid;
  grid-template-columns: 28px minmax(68px, 82px) 1fr;
  gap: 6px;
  align-items: start;
  color: var(--text-dim);
}
.doctor-check.failed {
  color: var(--text-primary);
}
.doctor-check-state {
  font-size: 9px;
  color: var(--text-dim);
}
.doctor-check-label {
  color: var(--text-secondary);
}
.doctor-check-detail {
  overflow-wrap: anywhere;
}
.doctor-action {
  display: inline-flex;
  align-items: center;
  min-height: 22px;
  margin-top: 5px;
  padding: 3px 8px;
  border: 1px solid var(--border-strong);
  border-radius: var(--radius-sm);
  background: var(--bg-elevated);
  color: var(--text-secondary);
  font-size: 10px;
  font-weight: 600;
  cursor: pointer;
}
.doctor-action:hover {
  border-color: var(--accent);
  color: var(--text-primary);
}
.doctor-notes {
  margin-top: 8px;
  color: var(--text-dim);
  line-height: 1.4;
}
.doctor-notes.blocked {
  color: var(--danger);
}

.diagnostics-panel {
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
}
.diagnostics-toolbar {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 10px;
}
.diagnostics-section + .diagnostics-section {
  margin-top: 10px;
}
.diagnostics-title {
  margin-bottom: 5px;
  color: var(--text-secondary);
  font-size: 11px;
  font-weight: 700;
}
.crash-row {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 6px 0;
  border-top: 1px solid var(--border);
  font-size: 11px;
  color: var(--text-secondary);
}
.crash-row small {
  color: var(--text-dim);
}
.log-tail {
  max-height: 180px;
  overflow: auto;
  margin: 0;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--bg-deep);
  color: var(--text-secondary);
  font-family: var(--font-mono);
  font-size: 10px;
  line-height: 1.55;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.empty-state {
  text-align: center;
  padding: 80px 20px;
  color: var(--text-dim);
}
.empty-state.compact {
  padding: 36px 20px;
}
.empty-icon {
  margin-bottom: 16px;
  opacity: 0.4;
}
.empty-state h2 {
  font-size: 16px;
  margin-bottom: 8px;
  color: var(--text-secondary);
}
.empty-state p {
  font-size: 13px;
}
.emulator-panel {
  display: flex;
  flex-direction: column;
  gap: 14px;
}
.rpcs3-dashboard {
  width: 100%;
  max-width: 1180px;
  margin: 0 auto;
}
.rpcs3-overview {
  position: relative;
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 20px 28px;
  overflow: hidden;
  padding: 24px;
  border: 1px solid color-mix(in srgb, var(--accent) 24%, var(--border));
  border-radius: 16px;
  background:
    linear-gradient(
      135deg,
      color-mix(in srgb, var(--bg-card) 94%, var(--accent) 6%),
      color-mix(in srgb, var(--bg-card) 98%, transparent)
    ),
    var(--bg-card);
  box-shadow:
    0 18px 48px rgba(0, 0, 0, 0.16),
    inset 0 1px rgba(255, 255, 255, 0.035);
}
.rpcs3-overview::after {
  content: "";
  position: absolute;
  top: -110px;
  right: -70px;
  width: 300px;
  height: 300px;
  border-radius: 50%;
  background: color-mix(in srgb, var(--accent) 11%, transparent);
  filter: blur(10px);
  pointer-events: none;
}
.rpcs3-overview-main,
.rpcs3-primary-actions,
.rpcs3-stats {
  position: relative;
  z-index: 1;
}
.rpcs3-overview-main {
  display: flex;
  gap: 14px;
  align-items: flex-start;
  min-width: 0;
}
.rpcs3-brand-mark,
.rpcs3-onboarding-icon {
  display: grid;
  flex: 0 0 auto;
  place-items: center;
  color: var(--accent);
  border: 1px solid color-mix(in srgb, var(--accent) 30%, transparent);
  background: color-mix(in srgb, var(--accent) 11%, var(--bg-card));
  box-shadow: inset 0 1px rgba(255, 255, 255, 0.05);
}
.rpcs3-brand-mark {
  width: 50px;
  height: 50px;
  border-radius: 14px;
}
.rpcs3-overview-copy {
  min-width: 0;
}
.rpcs3-eyebrow,
.rpcs3-step {
  color: var(--text-dim);
  font-size: 10px;
  font-weight: 750;
  letter-spacing: 0.09em;
  text-transform: uppercase;
}
.rpcs3-title-row {
  display: flex;
  flex-wrap: wrap;
  gap: 10px;
  align-items: center;
  margin: 3px 0 5px;
}
.rpcs3-title-row h2 {
  margin: 0;
  color: var(--text-primary);
  font-size: 24px;
  line-height: 1.1;
}
.rpcs3-state-pill {
  display: inline-flex;
  gap: 6px;
  align-items: center;
  padding: 4px 8px;
  color: var(--text-secondary);
  font-size: 10px;
  font-weight: 700;
  border: 1px solid var(--border);
  border-radius: 999px;
  background: color-mix(in srgb, var(--bg-input) 80%, transparent);
}
.rpcs3-state-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: var(--text-dim);
}
.rpcs3-state-pill.ready {
  color: var(--success);
  border-color: color-mix(in srgb, var(--success) 35%, transparent);
}
.rpcs3-state-pill.ready .rpcs3-state-dot {
  background: var(--success);
  box-shadow: 0 0 8px color-mix(in srgb, var(--success) 70%, transparent);
}
.rpcs3-state-pill.attention {
  color: var(--warn);
  border-color: color-mix(in srgb, var(--warn) 35%, transparent);
}
.rpcs3-state-pill.attention .rpcs3-state-dot {
  background: var(--warn);
}
.rpcs3-overview-copy p {
  max-width: 650px;
  margin: 0;
  color: var(--text-secondary);
  font-size: 12px;
  line-height: 1.55;
}
.rpcs3-primary-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  align-content: flex-start;
  justify-content: flex-end;
}
.rpcs3-primary-actions .btn {
  display: inline-flex;
  gap: 7px;
  align-items: center;
}
.rpcs3-primary-button {
  min-width: 132px;
}
.rpcs3-stats {
  display: grid;
  grid-column: 1 / -1;
  grid-template-columns: repeat(3, minmax(130px, 0.72fr)) minmax(220px, 1.35fr);
  gap: 8px;
}
.rpcs3-stat {
  display: flex;
  gap: 10px;
  align-items: center;
  min-width: 0;
  padding: 10px 12px;
  color: var(--text-dim);
  border: 1px solid color-mix(in srgb, var(--border) 82%, transparent);
  border-radius: 10px;
  background: color-mix(in srgb, var(--bg-input) 58%, transparent);
}
.rpcs3-stat > svg {
  flex: 0 0 auto;
  color: color-mix(in srgb, var(--accent) 80%, var(--text-secondary));
}
.rpcs3-stat span {
  min-width: 0;
}
.rpcs3-stat small,
.rpcs3-stat strong {
  display: block;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.rpcs3-stat small {
  margin-bottom: 2px;
  color: var(--text-dim);
  font-size: 9px;
  letter-spacing: 0.04em;
  text-transform: uppercase;
}
.rpcs3-stat strong {
  color: var(--text-primary);
  font-size: 11px;
  font-weight: 700;
}
.rpcs3-command-bar {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 8px;
}
.rpcs3-command {
  display: flex;
  gap: 10px;
  align-items: center;
  min-width: 0;
  padding: 11px 12px;
  color: var(--text-secondary);
  text-align: left;
  border: 1px solid var(--border);
  border-radius: 11px;
  background: color-mix(in srgb, var(--bg-card) 88%, transparent);
  cursor: pointer;
  transition:
    transform 140ms ease,
    border-color 140ms ease,
    background 140ms ease;
}
.rpcs3-command:hover:not(:disabled) {
  border-color: color-mix(in srgb, var(--accent) 42%, var(--border));
  background: color-mix(in srgb, var(--bg-card) 88%, var(--accent) 12%);
  transform: translateY(-1px);
}
.rpcs3-command:disabled {
  cursor: not-allowed;
  opacity: 0.42;
}
.rpcs3-command > svg {
  flex: 0 0 auto;
  color: var(--accent);
}
.rpcs3-command span,
.rpcs3-command strong,
.rpcs3-command small {
  display: block;
  min-width: 0;
}
.rpcs3-command strong {
  overflow: hidden;
  color: var(--text-primary);
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.rpcs3-command small {
  margin-top: 2px;
  overflow: hidden;
  color: var(--text-dim);
  font-size: 9px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.emulator-update-card,
.emulator-roots,
.rpcs3-management {
  padding: 13px 15px;
  border: 1px solid var(--border);
  border-radius: 11px;
  background: color-mix(in srgb, var(--bg-card) 88%, transparent);
}
.emulator-progress-row,
.emulator-root-row {
  display: flex;
  gap: 8px;
  align-items: center;
}
.emulator-progress-row {
  justify-content: space-between;
  margin-bottom: 8px;
}
.emulator-update-card .launch-failure {
  display: block;
  margin-top: 8px;
}
.rpcs3-management summary,
.emulator-roots summary {
  display: flex;
  gap: 8px;
  align-items: baseline;
  color: var(--text-secondary);
  font-size: 11px;
  font-weight: 700;
  cursor: pointer;
}
.rpcs3-management summary small {
  color: var(--text-dim);
  font-size: 9px;
  font-weight: 500;
}
.emulator-roots summary span {
  display: inline-grid;
  min-width: 18px;
  height: 18px;
  place-items: center;
  color: var(--text-dim);
  font-size: 9px;
  border-radius: 999px;
  background: var(--bg-input);
}
.rpcs3-management-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 7px;
  padding-top: 12px;
}
.emulator-root-row {
  margin-top: 8px;
}
.emulator-root-path {
  min-width: 0;
  flex: 1;
  padding: 7px 9px;
  overflow: hidden;
  color: var(--text-secondary);
  text-align: left;
  text-overflow: ellipsis;
  white-space: nowrap;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--bg-input);
  cursor: pointer;
}
.rpcs3-onboarding {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto;
  gap: 16px;
  align-items: center;
  padding: 22px 24px;
  border: 1px solid color-mix(in srgb, var(--accent) 18%, var(--border));
  border-radius: 14px;
  background: linear-gradient(
    110deg,
    color-mix(in srgb, var(--bg-card) 93%, var(--accent) 7%),
    color-mix(in srgb, var(--bg-card) 96%, transparent)
  );
}
.rpcs3-onboarding-icon {
  width: 46px;
  height: 46px;
  border-radius: 13px;
}
.rpcs3-onboarding-actions {
  display: flex;
  gap: 8px;
  align-items: center;
}
.rpcs3-onboarding h2 {
  margin: 3px 0 4px;
  color: var(--text-primary);
  font-size: 15px;
}
.rpcs3-onboarding p {
  max-width: 670px;
  margin: 0;
  color: var(--text-secondary);
  font-size: 11px;
  line-height: 1.5;
}
.emulator-game-card .sharp-card-actions-row .btn {
  flex: 1;
}
@media (max-width: 920px) {
  .rpcs3-overview {
    grid-template-columns: 1fr;
  }
  .rpcs3-primary-actions {
    justify-content: flex-start;
  }
  .rpcs3-stats {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
  .rpcs3-command-bar {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
@media (max-width: 620px) {
  .rpcs3-overview {
    padding: 18px;
  }
  .rpcs3-stats,
  .rpcs3-command-bar {
    grid-template-columns: 1fr;
  }
  .rpcs3-onboarding {
    grid-template-columns: auto minmax(0, 1fr);
  }
  .rpcs3-onboarding > .btn {
    grid-column: 1 / -1;
    justify-self: stretch;
  }
  .rpcs3-onboarding-actions {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    grid-column: 1 / -1;
  }
  .rpcs3-onboarding-actions .btn {
    justify-content: center;
  }
}
.gamejolt-panel {
  position: relative;
  min-height: calc(100vh - 220px);
  height: calc(100vh - 220px);
  overflow: hidden;
}
.gamejolt-games-pane {
  width: 100%;
  height: 100%;
  overflow-y: auto;
  padding-right: 4px;
  padding-bottom: var(--gamejolt-browser-space);
}
.gamejolt-storage-line {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 12px;
  color: var(--text-secondary);
  font-size: 11px;
}
.gamejolt-storage-line span {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.gamejolt-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 12px;
  align-items: start;
}
.gamejolt-card .sharp-card-banner {
  aspect-ratio: 16 / 5.6;
}
.gamejolt-native-note {
  color: var(--text-dim);
  font-size: 11px;
  white-space: nowrap;
}
.gamejolt-actions-main {
  display: flex;
  align-items: center;
  gap: 8px;
}
.gamejolt-actions-main .btn {
  flex: 0 1 auto;
}
.gamejolt-actions-main .gamejolt-pipeline-select {
  flex: 1 1 auto;
  min-width: 0;
}
.btn-uninstall {
  color: var(--error);
  border-color: var(--error-bg);
}
.btn-uninstall:hover:not(:disabled) {
  background: var(--error-bg);
  border-color: var(--error);
}
.gamejolt-native-note {
  flex: 1 1 0;
  min-width: 0;
  text-align: right;
}
.gamejolt-browser-frame {
  position: absolute;
  z-index: 20;
  right: 0;
  bottom: 0;
  left: 0;
  height: var(--gamejolt-browser-height);
  overflow: hidden;
  padding: 5px;
  border: 1px solid #fff;
  border-radius: var(--radius-md) var(--radius-md) 0 0;
  background: #fff;
  box-shadow: 0 0 18px rgba(255, 255, 255, 0.12);
  transition: height 120ms ease;
}
.gamejolt-panel.dragging .gamejolt-browser-frame {
  transition: none;
}
.gamejolt-browser-handle {
  position: absolute;
  z-index: 2;
  top: -1px;
  right: 0;
  left: 0;
  height: 18px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: ns-resize;
  touch-action: none;
}
.gamejolt-browser-handle span {
  width: 54px;
  height: 18px;
  display: grid;
  place-items: center;
  color: #222;
  font-size: 15px;
  font-weight: 900;
  line-height: 1;
  border-radius: 99px;
  background: #fff;
  box-shadow: 0 0 8px rgba(0, 0, 0, 0.35);
}
.gamejolt-browser {
  display: flex;
  width: 100%;
  height: 100%;
  border: 0;
  border-radius: calc(var(--radius-md) - 3px);
  background: #fff;
}
@media (max-width: 800px) {
  .gamejolt-panel {
    min-height: calc(100vh - 260px);
    height: calc(100vh - 260px);
  }
}
</style>
