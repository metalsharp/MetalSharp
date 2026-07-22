<script setup lang="ts">
import { computed, ref, onMounted, onUnmounted } from "vue";
import { useToast } from "../composables/useToast";
import { api, getAPI } from "../composables/useApi";
import type { SharpApp } from "../api-types";
import IconDownload from "~icons/lucide/download";
import IconUpload from "~icons/lucide/upload";
import IconRefreshCcw from "~icons/lucide/refresh-ccw";
import IconMonitor from "~icons/lucide/monitor";
import IconX from "~icons/lucide/x";
import sharpLogoUrl from "../icon.png";

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

interface RedistSourceGuide {
  id: string;
  name: string;
  source_url: string;
  local_targets: string[];
  policy: string;
  notes: string;
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

const toast = useToast();
const sourceMode = ref<"installers" | "gog">("installers");
const headerTitle = computed(() => sourceMode.value === "gog" ? "GOG Games Library" : "Sharp Library");
const headerSubtitle = computed(() =>
  sourceMode.value === "gog"
    ? "Connect, sync, install, and play GOG games through MetalSharp."
    : "Install and manage Windows applications outside Steam.",
);
const apps = ref<SharpApp[]>([]);
const dropdownOpen = ref<string | null>(null);
const dropdownStyle = ref<Record<string, string>>({});

function openDropdown(name: string, event: MouseEvent) {
  if (dropdownOpen.value === name) {
    dropdownOpen.value = null;
    return;
  }
  const btn = (event.currentTarget as HTMLElement).getBoundingClientRect();
  dropdownStyle.value = {
    top: `${btn.bottom + 6}px`,
    left: `${btn.left}px`,
    width: '420px',
  };
  dropdownOpen.value = name;
}
const bottles = ref<BottleManifest[]>([]);
const runtimeProfiles = ref<RuntimeProfileDefinition[]>([]);
const redistSources = ref<RedistSourceGuide[]>([]);
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
const launchArgDrafts = ref<Record<string, string>>({});
const gogStatus = ref<GogStatus | null>(null);
const gogGames = ref<GogGame[]>([]);
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
  const result = await api<{ ok: boolean; pid?: number; alreadyInstalled?: boolean; downloading?: boolean; error?: string; status?: WineMonoStatus }>("POST", "/wine-mono/install", { prefix: "gog" }, 30 * 1000);
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
  { id: "m12", name: "M12" },
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
  "fna": "FNA Runtime",
  "xna": "XNA Assemblies",
  "sdl2": "SDL2",
  "fna3d": "FNA3D",
  "faudio": "FAudio",
  "fmod": "FMOD Audio",
  "m12_d3d12": "M12 d3d12.dll",
  "m12_d3d11": "M12 d3d11.dll",
  "m12_d3d10core": "M12 d3d10core.dll",
  "m12_dxgi_dxmt": "M12 dxgi_dxmt.dll",
  "m12_dxgi": "M12 dxgi.dll",
  "m12_winemetal": "M12 winemetal.dll / .so",
  "m12_gpu_stubs": "M12 GPU Stubs",
  "d3d12_agility": "D3D12 Agility",
  "gpu_vendor_stubs": "GPU Stubs",
  "gptk_amd_stub": "GPTK AMD Stub",
  "d3d10core": "D3D10core",
  "winemetal": "Winemetal",
  "gptk": "GPTK",
  "gptk_prefix": "GPTK Prefix",
  "rosetta": "Rosetta",
  "corefonts": "Core Fonts",
  "vcrun2019_x64": "VC++ 2015-2022 x64",
  "vcrun2019_x86": "VC++ 2015-2022 x86",
  "vcrun2019": "VC++ 2015-2022",
  "vcrun2010": "VC++ 2010",
  "vcrun2013": "VC++ 2013",
  "dotnet40": ".NET 4.0",
  "dotnet48": ".NET 4.8",
  "webview2": "WebView2",
  "directx_jun2010": "DX Jun2010",
  "openal": "OpenAL",
  "physx": "PhysX",
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
const selectableRuntimeProfileIds = new Set(["m12", "d3dmetal", "m11", "m11_32", "m10", "m10_32", "m9", "fna_arm64"]);
const visibleRuntimeProfiles = computed(() => {
  const profiles = runtimeProfiles.value.some((profile) => profile.id === "d3dmetal")
    ? runtimeProfiles.value
    : [...runtimeProfiles.value, { id: "d3dmetal", name: "D3DMetal (GPTK)", components: ["gptk", "rosetta", "gptk_prefix", "vcrun2019_x64", "vcrun2019_x86"] }];
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

async function load() {
  const [result, bottleResult, profileResult, redistResult, gogStatusResult, gogGamesResult] = await Promise.all([
    api<{ ok: boolean; apps: SharpApp[] }>("GET", "/sharp-library"),
    api<{ ok: boolean; bottles: BottleManifest[] }>("GET", "/bottles"),
    api<{ ok: boolean; profiles: RuntimeProfileDefinition[] }>("GET", "/bottles/profiles"),
    api<{ ok: boolean; sources: RedistSourceGuide[] }>("GET", "/bottles/redist-sources"),
    api<{ ok: boolean; status: GogStatus }>("GET", "/sharp-library/gog/status"),
    api<{ ok: boolean; games: GogGame[]; status: GogStatus }>("GET", "/sharp-library/gog/games"),
  ]);
  if (result?.ok) {
    apps.value = [...result.apps].sort(sharpAppNameSort);
    for (const app of apps.value) {
      if (launchArgDrafts.value[app.id] === undefined) {
        launchArgDrafts.value[app.id] = (app.user_launch_args ?? []).join(" ");
      }
    }
  }
  if (bottleResult?.ok) {
    bottles.value = bottleResult.bottles;
  }
  if (profileResult?.ok) runtimeProfiles.value = profileResult.profiles;
  if (redistResult?.ok) redistSources.value = redistResult.sources;
  if (gogStatusResult?.ok) gogStatus.value = gogStatusResult.status;
  if (gogGamesResult?.ok) {
    setGogGames(gogGamesResult.games ?? []);
    gogStatus.value = gogGamesResult.status;
    for (const game of gogGames.value) {
      if (game.status === "downloading") void monitorGogProgress(game.productId);
    }
  }
  if (gogStatus.value?.prefixInitialized) void refreshGogMonoStatus();
}

function setGogGames(games: GogGame[]) {
  gogGames.value = [...games].sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: "base", numeric: true }));
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
  if (idx >= 0) games[idx] = game;
  else games.push(game);
  setGogGames(games);
}

async function initializeGogPrefix() {
  gogLoading.value.setup = true;
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>("POST", "/sharp-library/gog/initialize-prefix", {}, 5 * 60 * 1000);
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
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>("POST", "/sharp-library/gog/logout", {}, 30 * 1000);
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
  const result = await api<{ ok: boolean; status?: GogStatus; error?: string }>("POST", "/sharp-library/gog/auth-code", { code: login.code }, 90 * 1000);
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
  const result = await api<{ ok: boolean; games?: GogGame[]; status?: GogStatus; error?: string }>("POST", "/sharp-library/gog/sync", {}, 5 * 60 * 1000);
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
  const result = await api<{ ok: boolean; game?: GogGame; pid?: number; error?: string }>("POST", "/sharp-library/gog/install", {
    productId: game.productId,
    platform: game.platform || "windows",
    installPath,
  }, 90 * 1000);
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
    const result = await api<{ ok: boolean; percent?: number; active?: boolean; game?: GogGame; error?: string }>("POST", "/sharp-library/gog/progress", { productId });
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
  const result = await api<{ ok: boolean; game?: GogGame; pid?: number; error?: string }>("POST", "/sharp-library/gog/play", { productId: game.productId }, 90 * 1000);
  gogLoading.value[`${game.productId}:play`] = false;
  if (result?.ok && result.game) {
    upsertGogGame(result.game);
    toast.show(`${game.title} launched`, "success");
  } else {
    toast.show(result?.error ?? `Failed to launch ${game.title}`, "error");
  }
}

async function stopGogGame(game: GogGame) {
  gogLoading.value[`${game.productId}:stop`] = true;
  const result = await api<{ ok: boolean; game?: GogGame; error?: string }>("POST", "/sharp-library/gog/stop", { productId: game.productId });
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
  const result = await api<{ ok: boolean; game?: GogGame; error?: string }>("POST", "/sharp-library/gog/uninstall", { productId: game.productId }, 90 * 1000);
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
  const result = await api<{ ok: boolean; bottle?: BottleManifest; error?: string }>("POST", "/bottles/refresh", { id });
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
    toast.show(result.report.ready ? "Bottle prepared" : "Bottle needs runtime repair", result.report.ready ? "success" : "error");
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
  const result = await api<D3DMetalGptkResponse>("POST", "/d3dmetal/bottles/save", {
    appid: bottle.steam_app_id,
    bottleId: bottle.id,
    name: bottle.name,
    gameDir: bottle.game_install_path,
  }, 10 * 60 * 1000);
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

async function runD3DMetalAction(bottle: BottleManifest, action: D3DMetalGptkAction, app?: SharpApp): Promise<number | null> {
  if (!bottle.steam_app_id) return null;
  bottleLoading.value[bottle.id] = true;
  const route = d3dmetalActionRoute(action.id);
  const result = await api<D3DMetalGptkResponse>("POST", route, {
    appid: bottle.steam_app_id,
    bottleId: bottle.id,
    gameDir: bottle.game_install_path,
    gameExe: app ? sharpAppExeAbsolute(app) : undefined,
    launchArgs: app ? [...(app.launch_args ?? []), ...(app.user_launch_args ?? [])] : undefined,
  }, 10 * 60 * 1000);
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
  const result = await api<{ ok: boolean; bottle?: BottleManifest; error?: string }>("POST", "/bottles/set-runtime-profile", {
    id,
    profile,
  });
  bottleLoading.value[id] = false;
  if (result?.ok && result.bottle) {
    const isM12 = profile === "m12";
    const appid = result.bottle.steam_app_id ?? 0;
    const m12DryRun = isM12
      ? await api<M12DryRun>("GET", `/diagnostics/m12/dry-run?appid=${appid}`)
      : null;
    upsertBottle(result.bottle);
    if (result.bottle.runtime_profile !== "d3dmetal") clearD3DMetalBottleState(id);
    if (isM12 && m12DryRun?.ok === false) {
      const missing = m12DryRun.missing?.map((entry) => entry.filename).filter(Boolean).join(", ");
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
  const result = await api<{ ok: boolean; repair?: ComponentRepair; error?: string }>("POST", "/bottles/set-windows-version", {
    id,
    version,
  });
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

async function openRedistSource(source: RedistSourceGuide) {
  await getAPI().copyText(source.source_url);
  toast.show(`${source.name} source URL copied`, "success");
}

async function addBottleApp(bottle: BottleManifest, app: { name: string; exe_path: string }) {
  bottleLoading.value[bottle.id] = true;
  const result = await api<{ ok: boolean; app?: SharpApp; error?: string }>("POST", "/sharp-library/import-bottle-app", {
    bottleId: bottle.id,
    exePath: app.exe_path,
    name: app.name,
  });
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
  const result = await api<{ ok: boolean; path?: string; error?: string }>(
    "POST",
    "/diagnostics/open",
    { path: bottle.last_launch_log },
  );
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

function splitLaunchArgs(value: string): string[] {
  const matches = value.match(/(?:[^\s"]+|"[^"]*")+/g) ?? [];
  return matches.map((arg) => arg.replace(/^"|"$/g, "").trim()).filter(Boolean);
}

async function saveLaunchArgs(app: SharpApp) {
  const args = splitLaunchArgs(launchArgDrafts.value[app.id] ?? "");
  const result = await api<{ ok: boolean; error?: string }>("POST", "/sharp-library/set-launch-args", {
    id: app.id,
    args,
  });
  if (result?.ok) {
    app.user_launch_args = args;
    toast.show("Launch options saved", "success");
  } else {
    toast.show(result?.error ?? "Failed to save launch options", "error");
  }
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

function closeDropdowns(e: MouseEvent) {
  if (!(e.target as HTMLElement).closest('.dropdown-wrap')) dropdownOpen.value = null;
}
onMounted(() => { document.addEventListener('click', closeDropdowns); load(); });
onUnmounted(() => { document.removeEventListener('click', closeDropdowns); stopGogMonoPoll(); });
</script>

<template>
  <div class="sharp-view">
    <div class="sharp-header">
      <div class="sharp-header-title">
        <h1>{{ headerTitle }}</h1>
        <p>{{ headerSubtitle }}</p>
      </div>
      <div class="sharp-header-controls">
        <div class="source-switch">
          <button class="btn btn-secondary btn-sm" :class="{ active: sourceMode === 'installers' }" @click="sourceMode = 'installers'">Installers</button>
          <button class="btn btn-secondary btn-sm" :class="{ active: sourceMode === 'gog' }" @click="sourceMode = 'gog'">GOG</button>
        </div>
        <div v-if="sourceMode === 'installers' && redistSources.length" class="dropdown-wrap">
          <button class="btn btn-secondary" @click="openDropdown('redist', $event)">
            <IconDownload class="btn-icon" width="14" height="14" />
            <span class="btn-label-long">Redist Sources</span><span class="btn-label-short">Redist</span> <span class="dropdown-count">{{ redistSources.length }}</span>
          </button>
          <div v-if="dropdownOpen === 'redist'" class="dropdown-panel" :style="dropdownStyle" @click.stop>
            <div class="dropdown-scroll">
              <article v-for="source in redistSources" :key="source.id" class="redist-source-compact">
                <div><strong>{{ source.name }}</strong><small>{{ source.policy }}</small></div>
                <p>{{ source.notes }}</p>
                <div class="redist-targets"><span v-for="target in source.local_targets" :key="target">{{ target }}</span></div>
                <button class="btn btn-secondary btn-sm" @click="openRedistSource(source)">Copy Source URL</button>
              </article>
            </div>
          </div>
        </div>
        <button v-if="sourceMode === 'installers'" class="btn btn-primary" @click="installExe">
          <IconUpload class="btn-icon" width="14" height="14" />
          <span class="btn-label-long">Install Windows Program</span><span class="btn-label-short">Install</span>
        </button>
        <button
          v-if="sourceMode === 'gog'"
          class="btn btn-secondary"
          :disabled="gogLoading.setup || gogLoading.removing"
          :title="gogStatus?.prefixInitialized
            ? 'Remove the GOG Wine prefix. Downloaded GOG games stay on disk but cannot launch until the prefix is re-created.'
            : 'Create the isolated Wine prefix required to install and launch GOG games.'"
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
          }}</span><span class="btn-label-short">Prefix</span>
        </button>
        <button
          v-if="sourceMode === 'gog' && showGogInstallMono"
          class="btn btn-primary"
          :disabled="gogLoading.mono || (gogMonoStatus?.running && !gogMonoStatus?.stalled) || (gogMonoStatus?.downloading && !gogMonoStatus?.stalled)"
          :title="gogMonoButtonTitle()"
          @click="installGogMono"
        >
          <span class="btn-label-long">{{ gogMonoButtonLabel() }}</span><span class="btn-label-short">Mono</span>
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
        <button
          v-if="sourceMode === 'gog'"
          class="btn btn-primary"
          :disabled="gogLoading.login || !gogStatus?.prefixInitialized"
          :title="gogStatus?.oauthHelperAvailable === false
            ? 'GOG OAuth helper is missing; click “Prefix” or refresh to stage the new bundled helper.'
            : 'Sign in to GOG to enable library sync and downloads'"
          @click="handleGogAuthButton"
        >
          <span class="btn-label-long">{{ gogStatus?.authenticated ? "GOG Connected" : gogLoading.login ? "Connecting…" : "Login to GOG" }}</span><span class="btn-label-short">{{ gogStatus?.authenticated ? "Connected" : "Login" }}</span>
        </button>
        <button class="btn btn-secondary" :disabled="sourceMode === 'gog' && gogLoading.sync" @click="refreshCurrentSource">
          <IconRefreshCcw class="btn-icon" width="14" height="14" />
          <span class="btn-label-long">{{ sourceMode === 'gog' ? gogLoading.sync ? "Syncing…" : "Sync GOG" : "Refresh" }}</span><span class="btn-label-short">{{ sourceMode === 'gog' ? "Sync" : "Refresh" }}</span>
        </button>
      </div>
    </div>

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
            <span class="sharp-card-size">{{ formatBytes(app.size_bytes) }}</span>
          </div>
          <div v-if="bottleForApp(app)" class="sharp-card-bottle">
            <span class="badge" :class="bottleBadgeClass(bottleForApp(app)!.health)">
              {{ bottleBadgeLabel(bottleForApp(app)!) }}
            </span>
            <span
              v-if="bottleForApp(app)!.last_launch_status === 'exited' && bottleForApp(app)!.last_launch_log"
              class="sharp-card-launch-log"
            >
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
                <option v-for="option in engineOptions" :key="option.id" :value="option.id">
                  {{ option.name }}
                </option>
              </select>
            </div>
            <details class="sharp-card-tools">
              <summary class="drawer-summary">
                <span>Tools</span>
                <small>cover, launch args, diagnostics</small>
              </summary>
              <div class="sharp-tool-actions">
                <button class="btn btn-secondary btn-sm" @click="setCover(app.id)">Set Cover</button>
                <button class="btn btn-secondary btn-sm" :disabled="doctorLoading[app.id]" @click="runDoctor(app)">
                  {{ doctorLoading[app.id] ? "Checking" : "Doctor" }}
                </button>
                <button
                  class="btn btn-secondary btn-sm"
                  :disabled="diagnosticsLoading[app.id]"
                  @click="openDiagnostics(app)"
                >
                  {{ diagnosticsLoading[app.id] ? "Loading" : "Diagnostics" }}
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
              <div class="launch-options-row">
                <input
                  v-model="launchArgDrafts[app.id]"
                  class="control-input launch-options-input"
                  type="text"
                  placeholder="Launch options..."
                  @keydown.enter="saveLaunchArgs(app)"
                />
                <button class="btn btn-secondary btn-sm" @click="saveLaunchArgs(app)">Save</button>
              </div>
              <button class="btn btn-danger btn-sm sharp-uninstall-button" @click="uninstallApp(app.id)">
                Uninstall
              </button>
            </details>
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
                <button class="btn btn-secondary btn-sm" @click="clearShaderCache(app)">Clear All Shader Caches</button>
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
                <pre class="log-tail">{{ (recentLogLines[app.id] ?? ["No recent log lines loaded."]).join("\n") }}</pre>
              </div>
            </details>
          </div>
        </div>
      </div>
    </div>
    </template>

    <template v-else>
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
          <div v-for="game in gogGames" :key="game.productId" class="sharp-card gog-card" :class="{ running: game.running }">
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
                <span class="badge" :class="game.running ? 'badge-ok' : game.installed ? 'badge-ok' : game.status === 'downloading' ? 'badge-warn' : 'badge-muted'">
                  {{ game.running ? "Running" : game.installed ? "Installed" : game.status === "downloading" ? "Downloading" : "GOG" }}
                </span>
                <span v-if="game.downloadSizeBytes" class="sharp-card-size">{{ formatBytes(game.downloadSizeBytes) }}</span>
              </div>
              <div v-if="game.status === 'downloading'" class="gog-progress">
                <div class="gog-progress-bar"><span :style="{ width: `${gogProgress[game.productId] ?? 0}%` }"></span></div>
                <small>{{ Math.floor(gogProgress[game.productId] ?? 0) }}%</small>
              </div>
              <div class="sharp-card-actions">
                <div class="sharp-card-actions-row">
                  <button v-if="game.running" class="btn btn-stop" :disabled="gogLoading[`${game.productId}:stop`]" @click="stopGogGame(game)">Stop</button>
                  <button v-else-if="game.installed" class="btn btn-play" :disabled="gogLoading[`${game.productId}:play`]" @click="playGogGame(game)">Play</button>
                  <button v-else class="btn btn-primary" :disabled="game.status === 'downloading' || gogLoading[`${game.productId}:install`]" @click="installGogGame(game)">
                    {{ game.status === 'downloading' ? "Downloading…" : "Install" }}
                  </button>
                  <button v-if="game.installed" class="btn btn-danger" :disabled="gogLoading[`${game.productId}:uninstall`]" @click="uninstallGogGame(game)">Uninstall</button>
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
  </div>
</template>

<style scoped>
.sharp-view {
  padding: 0 28px 32px;
  height: 100%;
  overflow-y: auto;
}
.sharp-header {
  display: flex;
  flex-direction: column;
  gap: 18px;
  margin: 0 -28px 20px;
  padding: 44px 28px 13px;
  background: var(--page-header-bg);
  border-bottom: 1px solid var(--border);
  -webkit-app-region: drag;
  position: relative;
}
.sharp-header::after {
  content: "";
  position: absolute;
  inset: 0;
  background: radial-gradient(ellipse 60% 80% at 20% 50%, rgba(95, 183, 232, 0.08) 0%, transparent 70%),
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
  padding-top: 25px;
  min-height: 0;
  container-type: inline-size;
}
.btn-label-short { display: none; }
@container (max-width: 700px) {
  .btn-label-long { display: none; }
  .btn-label-short { display: inline; }
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
.source-switch {
  display: inline-flex;
  gap: 4px;
  padding: 3px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-card) 80%, transparent);
}
.source-switch .active {
  border-color: color-mix(in srgb, var(--accent) 70%, transparent);
  background: color-mix(in srgb, var(--accent) 18%, transparent);
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
.gog-card {
  border-color: color-mix(in srgb, var(--border) 84%, transparent);
  box-shadow:
    0 0 0 1px color-mix(in srgb, var(--border) 58%, transparent),
    0 10px 26px color-mix(in srgb, var(--bg-deep) 34%, transparent);
}
.gog-card:hover {
  border-color: color-mix(in srgb, var(--accent) 36%, var(--border));
  box-shadow:
    0 0 0 1px color-mix(in srgb, var(--accent) 16%, transparent),
    0 14px 30px color-mix(in srgb, var(--bg-deep) 38%, transparent);
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
.sharp-card-tools {
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 72%, transparent);
}
.sharp-card-tools .drawer-summary {
  padding: 8px;
}
.sharp-card-tools[open] {
  padding-bottom: 8px;
}
.sharp-tool-actions {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 6px;
  padding: 0 8px 8px;
}
.sharp-tool-actions .btn {
  min-width: 0;
  padding-inline: 6px;
}
.cover-position-controls {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 7px;
  padding: 0 8px 8px;
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
  margin: 0 8px;
  width: calc(100% - 16px);
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
</style>
