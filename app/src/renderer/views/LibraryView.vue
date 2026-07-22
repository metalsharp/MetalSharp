<script setup lang="ts">
import { ref, inject, onMounted, onUnmounted, watch, type Ref } from "vue";
import { useToast } from "../composables/useToast";
import { api } from "../composables/useApi";
import GameCard from "../components/GameCard.vue";
import IconCrosshair from "~icons/lucide/crosshair";
import IconRefreshCcw from "~icons/lucide/refresh-ccw";
import IconBattery from "~icons/lucide/battery";

interface SteamGame {
  appid: number;
  name: string;
  installed: boolean;
  state: "installed" | "not_installed" | "downloading";
  cover_url: string;
  header_url: string;
  size_bytes?: number | null;
  launch_method?: string;
  launch_method_name?: string;
  preferred_pipeline?: string | null;
  available_pipelines?: { id: string; name: string; recommended?: boolean }[];
  has_native_build?: boolean;
  can_uninstall?: boolean;
  bottle_id?: string | null;
  bottle_health?: string | null;
  bottle_runtime_assets?: number;
}

interface SteamLibrary {
  ok: boolean;
  total: number;
  installed_count: number;
  games: SteamGame[];
}

const library = inject<Ref<SteamLibrary | null>>("library")!;
const wineSteamInstalled = inject<Ref<boolean>>("wineSteamInstalled")!;
const wineSteamRunning = inject<Ref<boolean>>("wineSteamRunning")!;
const macSteamInstalled = inject<Ref<boolean>>("macSteamInstalled")!;
const macSteamRunning = inject<Ref<boolean>>("macSteamRunning")!;
const backendConnected = inject<Ref<boolean>>("backendConnected")!;
const backendVersion = inject<Ref<string | null>>("backendVersion")!;
const developerMode = inject<Ref<boolean>>("developerMode")!;
const reloadLibrary = inject<() => Promise<void>>("loadLibrary")!;

const toast = useToast();
const search = ref("");
const filter = ref<"all" | "installed" | "not_installed">("installed");

const runningAppId = ref<number | null>(null);
const runningPid = ref<number | null>(null);
const launchingAppId = ref<number | null>(null);
let artworkRetryTimer: number | null = null;
const artworkRetryRequestedAppIds = new Set<number>();

const filteredGames = ref<SteamGame[]>([]);

function gameNameSort(a: SteamGame, b: SteamGame) {
  return a.name.localeCompare(b.name, undefined, { sensitivity: "base", numeric: true });
}

function isMacSteamLaunch(launchMethod: string) {
  return launchMethod === "mac_steam" || launchMethod === "macos_steam" || launchMethod === "native_steam";
}

function isWineSteamRouteId(launchMethod: string) {
  const method = launchMethod.toLowerCase();
  return [
    "dxmt",
    "steam",
    "wine_steam",
    "m9",
    "m10",
    "m10_32",
    "m11",
    "m11_32",
    "m12",
    "m32",
    "dx9",
    "dx10",
    "dx11",
    "dx12",
    "d3d9",
    "d3d10",
    "d3d11",
    "d3d12",
    "d3dmetal",
    "d3d9_metal",
    "dxmt_metal",
    "dxmt_metal12",
    "wined3d_32",
  ].includes(method);
}

function recommendedLaunchMethod(game: SteamGame) {
  return game.launch_method ?? game.available_pipelines?.find((pipeline) => pipeline.recommended)?.id;
}

function isWineSteamRouteLaunch(game: SteamGame, launchMethod: string) {
  const method = launchMethod.toLowerCase();
  if (method === "auto") {
    const recommended = recommendedLaunchMethod(game);
    return recommended ? isWineSteamRouteId(recommended) : false;
  }
  return isWineSteamRouteId(method);
}

const directEacAppIds = new Set([1245620, 1888160]);

function effectiveLaunchMethod(game: SteamGame, launchMethod: string) {
  const method = launchMethod.toLowerCase();
  if (directEacAppIds.has(game.appid) && ["auto", "steam", "wine_steam"].includes(method)) return "m12";
  return launchMethod;
}

function applyFilter() {
  if (!library.value) {
    filteredGames.value = [];
    return;
  }
  let games = Array.isArray(library.value.games) ? library.value.games : [];
  if (filter.value === "installed") games = games.filter((g) => g.installed);
  if (filter.value === "not_installed") games = games.filter((g) => !g.installed);
  if (search.value) {
    const q = search.value.toLowerCase();
    games = games.filter((g) => g.name.toLowerCase().includes(q));
  }
  filteredGames.value = [...games].sort(gameNameSort);
}

function requestArtworkRetry(appid: number) {
  if (artworkRetryRequestedAppIds.has(appid)) return;
  artworkRetryRequestedAppIds.add(appid);
  if (artworkRetryTimer !== null) return;

  artworkRetryTimer = window.setTimeout(async () => {
    artworkRetryTimer = null;
    await reloadLibrary();
  }, 15_000);
}

async function toggleSteam() {
  if (wineSteamRunning.value) {
    const result = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/stop");
    if (result?.ok && result.running === false) {
      wineSteamRunning.value = false;
      toast.show("Wine Steam stopped");
    } else {
      wineSteamRunning.value = result?.running ?? true;
      toast.show(result?.error ?? "Wine Steam is still running", "error");
    }
  } else {
    toast.show("Starting Steam...", "success");
    const result = await api<{ ok: boolean }>("POST", "/steam/launch");
    if (result?.ok) {
      wineSteamRunning.value = true;
      toast.show("Steam started — log in through the Steam window", "success");
    }
  }
  reloadLibrary();
}

async function toggleMacSteam() {
  if (!macSteamInstalled.value) {
    return;
  }
  if (macSteamRunning.value) {
    const result = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/mac-stop");
    if (result?.ok && result.running === false) {
      macSteamRunning.value = false;
      toast.show("Mac Steam stopped");
    } else {
      macSteamRunning.value = result?.running ?? true;
      toast.show(result?.error ?? "Mac Steam is still running", "error");
    }
  } else {
    if (wineSteamRunning.value) {
      if (!confirm("Stop Wine Steam and start Mac Steam?")) return;
      const stopResult = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/stop");
      if (!stopResult?.ok || stopResult.running !== false) {
        wineSteamRunning.value = stopResult?.running ?? true;
        toast.show(stopResult?.error ?? "Wine Steam is still running", "error");
        return;
      }
      wineSteamRunning.value = false;
    }
    toast.show("Starting Mac Steam...", "success");
    const result = await api<{ ok: boolean }>("POST", "/steam/mac-launch");
    if (result?.ok) {
      macSteamRunning.value = true;
      toast.show("Mac Steam started", "success");
    }
  }
  reloadLibrary();
}

async function launchGame(game: SteamGame, launchMethod = "auto") {
  const selectedLaunchMethod = effectiveLaunchMethod(game, launchMethod);
  if (isMacSteamLaunch(selectedLaunchMethod) && wineSteamRunning.value) {
    if (!confirm(`Stop Wine Steam and launch ${game.name} through MacOS Steam?`)) return;
    const stopResult = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/stop");
    if (!stopResult?.ok || stopResult.running !== false) {
      wineSteamRunning.value = stopResult?.running ?? true;
      toast.show(stopResult?.error ?? "Wine Steam is still running", "error");
      return;
    }
    wineSteamRunning.value = false;
  }

  launchingAppId.value = game.appid;
  const useWineSteamRoute = isWineSteamRouteLaunch(game, selectedLaunchMethod);
  const launchEndpoint = useWineSteamRoute ? "/steam/launch-game" : "/game/launch-auto";
  const launchResult = await api<{
    ok: boolean;
    pid?: number;
    error?: string;
    engine?: string;
    gameType?: string;
    offline_mode?: boolean;
    steam_runtime?: string;
  }>(
    "POST",
    launchEndpoint,
    {
      appid: game.appid,
      launchMethod: selectedLaunchMethod,
    },
    10 * 60 * 1000,
  );

  launchingAppId.value = null;

  if (launchResult?.ok && launchResult.pid) {
    runningPid.value = launchResult.pid;
    runningAppId.value = game.appid;
    if (useWineSteamRoute && !launchResult.offline_mode && launchResult.steam_runtime !== "offline")
      wineSteamRunning.value = true;
    if (isMacSteamLaunch(selectedLaunchMethod) || launchResult.gameType === "macos_steam") macSteamRunning.value = true;
    toast.show(`Launched ${game.name}`, "success");
  } else {
    toast.show(launchResult?.error ?? `Failed to launch ${game.name}`, "error");
  }
}

function markD3DMetalLaunched(game: SteamGame, pid: number) {
  runningPid.value = pid;
  runningAppId.value = game.appid;
  launchingAppId.value = null;
}

async function stopGame(game: SteamGame) {
  await api("POST", "/kill", { pid: runningPid.value, appid: game.appid });
  runningPid.value = null;
  runningAppId.value = null;
  toast.show(`Stopped ${game.name}`);
}

async function installGame(game: SteamGame) {
  if (!wineSteamInstalled.value) {
    toast.show("Install Windows Steam first (Settings)", "error");
    return;
  }
  if (!wineSteamRunning.value) {
    toast.show("Starting Steam...", "success");
    await api("POST", "/steam/launch");
    wineSteamRunning.value = true;
  }
  toast.show(`Open Steam and install ${game.name}. It will appear here when ready.`, "success");
}

async function uninstallGame(game: SteamGame) {
  if (game.can_uninstall === false) {
    toast.show(`${game.name} is only installed in macOS Steam. Uninstall it from macOS Steam.`, "error");
    return;
  }
  if (!confirm(`Uninstall ${game.name}? Game files will be deleted.`)) return;
  toast.show(`Uninstalling ${game.name}...`);
  const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/uninstall-game", { appid: game.appid });
  if (result?.ok) {
    toast.show(`Uninstalled ${game.name}`);
    reloadLibrary();
  } else {
    toast.show(result?.error ?? `Failed to uninstall ${game.name}`, "error");
  }
}

onMounted(() => {
  applyFilter();
});

onUnmounted(() => {
  if (artworkRetryTimer !== null) {
    window.clearTimeout(artworkRetryTimer);
    artworkRetryTimer = null;
  }
});

watch([library, search, filter], () => {
  applyFilter();
});
</script>

<template>
  <div class="library-view">
    <div class="library-header glass-header">
      <div class="library-title-row">
        <div>
          <h1>Library</h1>
          <p class="library-counts">
            {{ library?.total ?? 0 }} games &middot;
            {{ library?.games.filter((g) => g.installed).length ?? 0 }} installed
          </p>
        </div>
        <div class="library-status-strip">
          <span v-if="wineSteamRunning" class="badge badge-ok">Steam Running</span>
          <span v-else-if="wineSteamInstalled" class="badge badge-warn">Steam Offline</span>
          <span class="badge" :class="backendConnected ? 'badge-ok' : 'badge-error'">
            {{ backendConnected ? `Backend${backendVersion ? " v" + backendVersion : ""}` : "Backend Offline" }}
          </span>
        </div>
      </div>
      <div class="library-controls">
        <div class="library-launch-actions">
          <button class="btn btn-secondary library-control-button" title="Wine Steam" @click="toggleSteam">
            <IconCrosshair class="control-icon" width="15" height="15" />
            <span class="control-label">{{ wineSteamRunning ? "Stop Wine Steam" : "Start Wine Steam" }}</span>
          </button>
          <button
            class="btn btn-secondary library-control-button refresh-button"
            title="Refresh"
            @click="reloadLibrary()"
          >
            <IconRefreshCcw class="control-icon" width="15" height="15" />
            <span class="control-label">Refresh</span>
          </button>
        </div>
        <div class="library-controls-center">
          <input v-model="search" class="control-input" type="text" placeholder="Search games..." />
          <select v-model="filter" class="control-input">
            <option value="all">All Games</option>
            <option value="installed">Installed</option>
            <option value="not_installed">Not Installed</option>
          </select>
        </div>
      </div>
    </div>

    <div class="library-body view-body-surface">
      <div v-if="!library || library.games.length === 0" class="empty-state">
        <div class="empty-icon">
          <IconBattery width="48" height="48" />
        </div>
        <h2>No games found</h2>
        <p>Add your Steam API key in Settings to load your library, or download a game manually.</p>
      </div>

      <div v-else class="game-grid">
        <div v-for="game in filteredGames" :key="game.appid" class="game-grid-item">
          <GameCard
            :game="game"
            :running="runningAppId === game.appid"
            :launching="launchingAppId === game.appid"
            :steam-installed="wineSteamInstalled"
            :developer-mode="developerMode"
            @play="launchGame(game, $event)"
            @d3dmetal-launched="markD3DMetalLaunched(game, $event)"
            @stop="stopGame(game)"
            @install="installGame(game)"
            @uninstall="uninstallGame(game)"
            @artwork-missing="requestArtworkRetry"
          />
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.library-view {
  padding: 0 28px;
  height: 100%;
  width: 100%;
  min-width: 0;
  overflow-y: auto;
  overflow-x: hidden;
  display: flex;
  flex-direction: column;
  background: transparent;
}
:global(:root[data-theme="developer"] .library-view) {
  background: transparent;
}

.library-header {
  flex-shrink: 0;
  margin: 0 -28px;
  padding: 44px 28px 14px;
  min-width: 0;
  border-bottom: 1px solid var(--border);
  -webkit-app-region: drag;
  position: relative;
  overflow: hidden;
}
.library-body {
  flex: 1;
  margin: 0 -28px;
  padding: 20px 28px 32px;
}
:global(:root[data-theme="developer"] .library-header) {
  border-bottom-color: rgba(185, 255, 77, 0.28);
  box-shadow:
    inset 0 -1px 0 rgba(0, 245, 255, 0.22),
    0 20px 56px rgba(0, 0, 0, 0.22);
}
.library-header::after {
  content: "";
  position: absolute;
  inset: 0;
  background:
    radial-gradient(ellipse 60% 80% at 20% 50%, rgba(95, 183, 232, 0.08) 0%, transparent 70%),
    radial-gradient(ellipse 40% 60% at 80% 50%, rgba(95, 183, 232, 0.05) 0%, transparent 60%);
  pointer-events: none;
}
:global(:root[data-theme="developer"] .library-header::after) {
  background: linear-gradient(
    90deg,
    rgba(255, 46, 247, 0.1),
    transparent 34%,
    rgba(0, 245, 255, 0.09) 78%,
    transparent
  );
}
.library-title-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, auto);
  align-items: flex-start;
  justify-content: space-between;
  gap: 18px;
  margin-bottom: 18px;
  min-width: 0;
}
.library-title-row > div:first-child {
  min-width: 0;
}
.library-header h1 {
  font-size: 24px;
  font-weight: 750;
  line-height: 1.1;
}
:global(:root[data-theme="developer"] .library-header h1) {
  color: var(--accent);
  font-family: var(--font-mono);
  font-weight: 800;
}
.library-counts {
  margin-top: 6px;
  color: var(--text-dim);
  font-size: 12px;
}
.library-status-strip {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
  flex-wrap: wrap;
  justify-self: end;
  min-width: 0;
  max-width: 100%;
}
.library-status-strip .badge {
  min-width: 0;
  max-width: min(260px, 100%);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.library-controls {
  display: grid;
  grid-template-columns: minmax(0, max-content) minmax(240px, 1fr);
  align-items: center;
  gap: 12px;
  min-width: 0;
  width: 100%;
  -webkit-app-region: no-drag;
}
.library-launch-actions {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
  min-width: 0;
}
.library-controls-center {
  display: grid;
  grid-template-columns: minmax(120px, 1fr) minmax(120px, 148px);
  gap: 10px;
  min-width: 0;
}
.library-controls-center input {
  min-width: 0;
}
.library-controls-center select {
  min-width: 0;
}
.library-control-button {
  flex: 0 1 auto;
  min-width: 0;
  max-width: 100%;
}
.control-icon {
  flex: 0 0 15px;
}
.control-label {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
}

@media (max-width: 1040px) {
  .library-controls {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 880px) {
  .library-controls {
    gap: 6px;
  }
  .library-control-button {
    min-width: 34px;
  }
  .library-launch-actions {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  }
  .library-controls-center {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 620px) {
  .library-view {
    padding-left: 16px;
    padding-right: 16px;
  }
  .library-header {
    margin-left: -16px;
    margin-right: -16px;
    padding-left: 16px;
    padding-right: 16px;
  }
  .library-launch-actions {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 720px) {
  .library-header {
    padding-top: 36px;
  }
  .library-controls-center {
    grid-template-columns: 1fr;
  }
  .library-status-strip .badge {
    max-width: min(180px, calc(50vw - 32px));
    padding-inline: 6px;
    font-size: 9px;
  }
}

.game-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(min(100%, 300px), 1fr));
  gap: 18px;
  align-items: start;
  min-width: 0;
  width: 100%;
  max-width: 100%;
}

.game-grid-item {
  min-width: 0;
}

.empty-state {
  text-align: center;
  padding: 80px 20px;
  color: var(--text-dim);
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
  max-width: 360px;
  margin: 0 auto;
  line-height: 1.5;
}
</style>
