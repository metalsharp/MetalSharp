<script setup lang="ts">
import { ref, inject, onMounted, onUnmounted, type Ref } from "vue";
import { useToast } from "../composables/useToast";
import { api, getAPI } from "../composables/useApi";
import type { AppConfig, UpdateStatus } from "../api-types";
import IconTrash2 from "~icons/lucide/trash-2";

interface CacheSummary {
  bytes: number;
  files: number;
  directories: number;
  apps: number;
  status: "missing" | "empty" | "active";
  path: string;
  last_modified: string | null;
}

const config = inject<Ref<AppConfig | null>>("config")!;
const wineSteamInstalled = inject<Ref<boolean>>("wineSteamInstalled")!;
const wineSteamRunning = inject<Ref<boolean>>("wineSteamRunning")!;
const macSteamInstalled = inject<Ref<boolean>>("macSteamInstalled")!;
const macSteamRunning = inject<Ref<boolean>>("macSteamRunning")!;
const backendConnected = inject<Ref<boolean>>("backendConnected")!;
const backendRestarting = ref(false);
const backendVersion = inject<Ref<string | null>>("backendVersion")!;
const updateStatus = inject<Ref<UpdateStatus | null>>("updateStatus")!;
const updateDownloading = inject<Ref<boolean>>("updateDownloading")!;
const updateProgress = inject<Ref<number>>("updateProgress")!;
const updateMessage = inject<Ref<string>>("updateMessage")!;
const startUpdateDownload = inject<() => void>("startUpdateDownload")!;
const steamApiKey = inject<Ref<string | null>>("steamApiKey")!;
const setupDeviceName = inject<Ref<string>>("setupDeviceName")!;
const reloadLibrary = inject<() => Promise<void>>("loadLibrary")!;
const library =
  inject<Ref<{ ok: boolean; total: number; installed_count: number; games: unknown[] } | null>>("library")!;
const developerMode = inject<Ref<boolean>>("developerMode")!;
const lowPerformanceMode = inject<Ref<boolean>>("lowPerformanceMode")!;

const toast = useToast();
const shaderCache = ref<CacheSummary | null>(null);
const pipelineCache = ref<CacheSummary | null>(null);
const apiKeyInput = ref("");
const graphicsRuntimeLogs = ref(false);

interface WineMonoStatus {
  latestVersion: string;
  installedVersion?: string | null;
  installed: boolean;
  upToDate: boolean;
  running: boolean;
  pid?: number | null;
  logPath?: string | null;
  targetVersion: string;
  lastError?: string | null;
  msiCached: boolean;
  downloading: boolean;
  downloadBytes: number;
  downloadTotal: number;
  downloadError?: string | null;
}
const steamMonoStatus = ref<WineMonoStatus | null>(null);
const steamMonoLoading = ref(false);
const steamMonoPollHandle = ref<ReturnType<typeof setInterval> | null>(null);

async function refreshSteamMonoStatus() {
  const result = await api<WineMonoStatus>("GET", "/wine-mono/status?prefix=steam");
  if (result?.ok) steamMonoStatus.value = result;
}

async function upgradeSteamMono() {
  steamMonoLoading.value = true;
  // Short timeout — the backend now returns immediately (kicks off download or launches installer).
  const result = await api<{ ok: boolean; pid?: number; alreadyInstalled?: boolean; downloading?: boolean; error?: string; status?: WineMonoStatus }>("POST", "/wine-mono/install", { prefix: "steam" }, 30 * 1000);
  steamMonoLoading.value = false;
  if (result?.ok) {
    if (result.alreadyInstalled) {
      await refreshSteamMonoStatus();
      toast.show("Wine Mono is already up to date", "success");
      return;
    }
    if (result.downloading) {
      // Backend kicked off async download — poll for progress.
      startSteamMonoPoll();
      return;
    }
    // Installer launched.
    toast.show("Wine Mono installer launched — complete it in the Wine window", "success");
    startSteamMonoPoll();
  } else {
    toast.show(result?.error ?? "Failed to launch Wine Mono installer", "error");
    await refreshSteamMonoStatus();
  }
}

function steamMonoButtonLabel(): string {
  const s = steamMonoStatus.value;
  if (!s) return "Upgrade Mono";
  if (s.downloading && s.downloadTotal > 0) {
    const raw = (s.downloadBytes / s.downloadTotal) * 100;
    const pct = Math.min(100, Math.floor(raw / 15) * 15);
    return `Downloading Mono ${pct}%…`;
  }
  if (s.downloading) return "Downloading Mono…";
  if (s.running) return "Running…";
  if (steamMonoLoading.value) return "Installing…";
  return "Upgrade Mono";
}

function startSteamMonoPoll() {
  if (steamMonoPollHandle.value) return;
  steamMonoPollHandle.value = setInterval(async () => {
    await refreshSteamMonoStatus();
    const status = steamMonoStatus.value;
    if (!status) return;

    // Download completed successfully → trigger the installer.
    if (status.msiCached && !status.downloading && !status.running && !status.upToDate) {
      stopSteamMonoPoll();
      steamMonoLoading.value = true;
      await upgradeSteamMono();
      return;
    }

    if (status.upToDate) {
      stopSteamMonoPoll();
      toast.show(`Wine Mono ${status.latestVersion} installed`, "success");
    } else if (!status.running && !status.downloading) {
      // Installer exited without landing the latest version (user cancelled).
      stopSteamMonoPoll();
    }
  }, 3000);
}

function stopSteamMonoPoll() {
  if (steamMonoPollHandle.value) {
    clearInterval(steamMonoPollHandle.value);
    steamMonoPollHandle.value = null;
  }
}

onMounted(async () => {
  apiKeyInput.value = steamApiKey.value ?? "";
  await refreshConfig();
  await refreshCacheSizes();
  void refreshSteamMonoStatus();
});

onUnmounted(() => { stopSteamMonoPoll(); });

async function refreshConfig() {
  const result = await api<AppConfig>("GET", "/config");
  if (result?.ok) {
    config.value = result;
    graphicsRuntimeLogs.value = Boolean(result.graphicsRuntimeLogs ?? result.graphics_runtime_logs);
  }
}

async function refreshCacheSizes() {
  const result = await api<{
    ok: boolean;
    shader_cache: CacheSummary;
    pipeline_cache: CacheSummary;
  }>("GET", "/cache/size");
  if (result?.ok) {
    shaderCache.value = result.shader_cache;
    pipelineCache.value = result.pipeline_cache;
  }
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
}

async function saveApiKey() {
  const key = apiKeyInput.value.trim();
  if (!key) {
    toast.show("Please enter a Steam API key", "error");
    return;
  }
  const result = await api<{
    ok: boolean;
    error?: string;
    library?: { ok: boolean; total: number; installed_count: number; games: unknown[] };
    sync?: { api_key_set: boolean; steam_id_detected: boolean };
  }>("POST", "/steam/save-api-key", { key });
  if (!result?.ok) {
    toast.show(result?.error ?? "Failed to save Steam API key", "error");
    return;
  }
  if (result.library) library.value = result.library;
  else await reloadLibrary();
  steamApiKey.value = key;
  if (result.sync && !result.sync.steam_id_detected) {
    toast.show("API key saved, but SteamID was not detected yet", "error");
  } else {
    toast.show(`API key saved — synced ${result.library?.total ?? 0} games`, "success");
  }
}

async function changeDeviceName() {
  const result = await api<{ name: string }>("GET", "/setup/device-name");
  if (result?.name) {
    setupDeviceName.value = result.name;
    await api("POST", "/setup/save", { deviceName: result.name });
    toast.show(`Device name changed to ${result.name}`);
  }
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
    const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/launch");
    if (result?.ok) {
      wineSteamRunning.value = true;
      toast.show("Steam started", "success");
    } else {
      toast.show(result?.error ?? "Failed to start Steam", "error");
    }
  }
}

async function installMacSteam() {
  const result = await api<{ ok: boolean; installed?: boolean; error?: string }>("POST", "/steam/mac-install");
  if (result?.ok) {
    if (result.installed) {
      macSteamInstalled.value = true;
      toast.show("macOS Steam is already installed", "success");
    } else {
      toast.show("Steam download page opened", "success");
    }
  } else {
    toast.show(result?.error ?? "Could not open macOS Steam installer", "error");
  }
}

async function toggleMacSteam() {
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
}

async function restartBackend() {
  if (backendRestarting.value) return;
  backendRestarting.value = true;
  try {
    toast.show("Restarting backend...", "success");
    const result = await getAPI().restartBackend();
    backendConnected.value = result.ok && (await getAPI().isBackendAlive());
    if (backendConnected.value) toast.show("Backend restarted", "success");
    else toast.show(result.error ?? "Backend did not come back online", "error");
  } catch (error) {
    backendConnected.value = false;
    toast.show(error instanceof Error ? error.message : "Failed to restart backend", "error");
  } finally {
    backendRestarting.value = false;
  }
}

async function openMetalsharpFolder() {
  const result = await getAPI().openMetalsharpFolder();
  toast.show(
    result?.ok ? "MetalSharp data folder opened" : (result?.error ?? "Failed to open data folder"),
    result?.ok ? "success" : "error",
  );
}

async function openLogsFolder() {
  const result = await getAPI().openLogsFolder();
  toast.show(
    result?.ok ? "Logs folder opened" : (result?.error ?? "Failed to open logs"),
    result?.ok ? "success" : "error",
  );
}

async function repairDataAccess() {
  const result = await getAPI().repairDataAccess();
  const failed = result?.checks?.filter((check) => !check.ok) ?? [];
  if (result?.ok) {
    toast.show("MetalSharp data access verified", "success");
  } else {
    toast.show(failed[0]?.error ?? result?.error ?? "MetalSharp data access needs attention", "error");
  }
}

async function clearShaderCache() {
  const result = await api<{ ok: boolean; bytes_freed: number; files_removed: number }>("POST", "/cache/clear", {
    type: "shader",
  });
  if (result?.ok) toast.show(`Shader cache cleared — ${formatBytes(result.bytes_freed)} freed`);
  await refreshCacheSizes();
}

async function clearPipelineCache() {
  const result = await api<{ ok: boolean; bytes_freed: number; files_removed: number }>("POST", "/cache/clear", {
    type: "pipeline",
  });
  if (result?.ok) toast.show(`Pipeline cache cleared — ${formatBytes(result.bytes_freed)} freed`);
  await refreshCacheSizes();
}

async function checkForUpdates() {
  toast.show("Checking for updates...", "success");
  const result = await api<UpdateStatus>("GET", "/update/check");
  if (result) updateStatus.value = result;
  if (result?.ok && result.available) toast.show(`Update available: v${result.latest_version}`, "success");
  else if (result?.ok) toast.show("You're up to date!", "success");
  else toast.show("Could not check for updates", "error");
}

function cacheBadgeClass(cache: CacheSummary | null): string {
  if (!cache || cache.status === "missing" || cache.status === "empty") return "badge-warn";
  return "badge-ok";
}

function cacheStatusText(cache: CacheSummary | null): string {
  if (!cache) return "...";
  if (cache.status === "missing") return "Missing";
  if (cache.status === "empty") return "Empty";
  return `${formatBytes(cache.bytes)} · ${cache.files} files`;
}

function toggleDeveloperMode(enabled: boolean) {
  developerMode.value = enabled;
  localStorage.setItem("metalsharp-developer-mode", String(enabled));
}

function toggleLowPerformanceMode(enabled: boolean) {
  lowPerformanceMode.value = enabled;
  toast.show(enabled ? "Low Performance Mode enabled" : "Low Performance Mode disabled", "success");
}

async function forceKillProcesses() {
  if (!confirm("Force kill MetalSharp Wine/runtime processes? This can stop active games, installers, and downloads.")) return;
  const result = await api<{
    ok: boolean;
    terminated?: unknown[];
    killed?: unknown[];
    errors?: unknown[];
    error?: string;
  }>("POST", "/processes/force-kill", {}, 15000);
  if (!result) {
    toast.show("Force kill request failed", "error");
    return;
  }
  const count = (result.terminated?.length ?? 0) + (result.killed?.length ?? 0);
  if (result.ok) {
    toast.show(count > 0 ? `Force killed ${count} process${count === 1 ? "" : "es"}` : "No MetalSharp runtime processes found", "success");
  } else {
    toast.show(result.error ?? `Force kill completed with ${result.errors?.length ?? 0} error(s)`, "error");
  }
}

async function toggleGraphicsRuntimeLogs(enabled: boolean) {
  const previous = graphicsRuntimeLogs.value;
  graphicsRuntimeLogs.value = enabled;
  const result = await api<AppConfig>("POST", "/config", { graphicsRuntimeLogs: enabled, logs: enabled });
  if (result?.ok) {
    config.value = result;
    graphicsRuntimeLogs.value = Boolean(result.graphicsRuntimeLogs ?? result.graphics_runtime_logs);
    toast.show(
      graphicsRuntimeLogs.value ? "Graphics runtime logs enabled for future launches" : "Graphics runtime logs disabled",
      "success",
    );
  } else {
    graphicsRuntimeLogs.value = previous;
    toast.show("Failed to save graphics logging setting", "error");
  }
}

function uninstallMetalsharp() {
  getAPI().uninstallApp();
}
</script>

<template>
  <div class="settings-view">
    <div class="settings-header"><h1>Settings</h1></div>

    <div class="settings-section">
      <h2>Steam Integration</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Steam Web API Key</div>
          <div class="settings-desc">
            Required to load your full game library. Get a free key at
            <a href="https://steamcommunity.com/dev/apikey" target="_blank">steamcommunity.com/dev/apikey</a>.
          </div>
        </div>
        <div class="settings-value">
          <div class="settings-input-row">
            <input
              v-model="apiKeyInput"
              type="password"
              class="control-input"
              placeholder="Enter your Steam Web API key..."
            />
            <button class="btn btn-primary btn-sm" @click="saveApiKey">Save &amp; Sync</button>
          </div>
          <span v-if="steamApiKey" class="badge badge-ok">Key saved</span>
          <span v-else class="badge badge-warn">No key — only installed games shown</span>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Device Name</div>
          <div class="settings-desc">Identifies this machine to Steam for persistent login</div>
        </div>
        <div class="settings-value">
          <span>{{ setupDeviceName || "Not set" }}</span>
          <button class="btn btn-secondary btn-sm" @click="changeDeviceName">Change</button>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Steam</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Wine Steam (MetalSharp)</div>
          <div class="settings-desc">Windows Steam running in MetalSharp Wine</div>
        </div>
        <div class="settings-value">
          <span v-if="wineSteamInstalled" class="badge badge-ok">Installed</span>
          <span v-else class="badge badge-warn">Not Installed</span>
          <button v-if="wineSteamInstalled" class="btn btn-secondary btn-sm" @click="toggleSteam">
            {{ wineSteamRunning ? "Stop Steam" : "Start Steam" }}
          </button>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Steam Mac</div>
          <div class="settings-desc">Native macOS Steam used for games with Mac builds</div>
        </div>
        <div class="settings-value">
          <span v-if="macSteamInstalled" class="badge badge-ok">Installed</span>
          <span v-else class="badge badge-warn">Not Installed</span>
          <button v-if="macSteamInstalled" class="btn btn-secondary btn-sm" @click="toggleMacSteam">
            {{ macSteamRunning ? "Stop Steam Mac" : "Start Steam Mac" }}
          </button>
          <button v-else class="btn btn-primary btn-sm" @click="installMacSteam">Install macOS Steam</button>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Backend</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Game Runtime Backend</div>
          <div class="settings-desc">
            The Rust backend handles game launches, Steam integration, and shader management
          </div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="backendConnected ? 'badge-ok' : 'badge-warn'">
            {{ backendConnected ? "Connected" : "Offline" }}
          </span>
          <span v-if="backendVersion" class="settings-version">v{{ backendVersion }}</span>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Restart Backend</div>
          <div class="settings-desc">Kill and restart the backend process</div>
        </div>
        <div class="settings-value">
          <button class="btn btn-secondary btn-sm" :disabled="backendRestarting" @click="restartBackend">
            {{ backendRestarting ? "Restarting..." : "Restart Backend" }}
          </button>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Force Kill Processes</div>
          <div class="settings-desc">Destructively stops MetalSharp Wine/runtime helper processes while keeping this app and backend alive.</div>
        </div>
        <div class="settings-value">
          <button class="btn btn-danger btn-sm" @click="forceKillProcesses">Force Kill Processes</button>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Low Performance Mode</div>
          <div class="settings-desc">Disables blur, glass, glow, and heavy motion while preserving layout and essential progress updates.</div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="lowPerformanceMode ? 'badge-warn' : 'badge-ok'">
            {{ lowPerformanceMode ? "Reduced Effects" : "Full Effects" }}
          </span>
          <label class="settings-toggle toggle-label" aria-label="Low Performance Mode">
            <input
              type="checkbox"
              :checked="lowPerformanceMode"
              @change="toggleLowPerformanceMode(($event.target as HTMLInputElement).checked)"
            />
            <span class="toggle-switch"></span>
          </label>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Developer Tools</div>
          <div class="settings-desc">Show launch routing, doctor controls, and advanced card tools</div>
        </div>
        <div class="settings-value">
          <label class="settings-toggle toggle-label" aria-label="Developer Tools">
            <input
              type="checkbox"
              :checked="developerMode"
              @change="toggleDeveloperMode(($event.target as HTMLInputElement).checked)"
            />
            <span class="toggle-switch"></span>
          </label>
        </div>
      </div>
      <div v-if="developerMode" class="settings-row">
        <div>
          <div class="settings-label">Graphics Runtime Logs</div>
          <div class="settings-desc">
            Opt in to DXMT graphics logs for future launches. Off by default so M12 games do not emit runtime logs unless requested.
          </div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="graphicsRuntimeLogs ? 'badge-warn' : 'badge-ok'">
            {{ graphicsRuntimeLogs ? "Logs On" : "Default Off" }}
          </span>
          <label class="settings-toggle toggle-label" aria-label="Graphics Runtime Logs">
            <input
              type="checkbox"
              :checked="graphicsRuntimeLogs"
              @change="toggleGraphicsRuntimeLogs(($event.target as HTMLInputElement).checked)"
            />
            <span class="toggle-switch"></span>
          </label>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Runtime</h2>
      <div v-if="steamMonoStatus && !steamMonoStatus.upToDate" class="settings-row">
        <div>
          <div class="settings-label">Wine Mono</div>
          <div class="settings-desc">
            Download and install Wine Mono {{ steamMonoStatus.latestVersion }} into the Steam prefix.
            <span v-if="steamMonoStatus.installed">Installed: v{{ steamMonoStatus.installedVersion }}.</span>
            <span v-else>No Wine Mono installed.</span>
            <span v-if="steamMonoStatus.downloadError" class="download-error">Download failed: {{ steamMonoStatus.downloadError }}.</span>
            The installer runs interactively in a Wine window.
          </div>
        </div>
        <div class="settings-value">
          <button
            class="btn btn-primary btn-sm"
            :disabled="steamMonoLoading || steamMonoStatus.running || steamMonoStatus.downloading"
            @click="upgradeSteamMono"
          >
            {{ steamMonoButtonLabel() }}
          </button>
          <div v-if="steamMonoStatus.downloading && steamMonoStatus.downloadTotal > 0" class="mono-progress-bar">
            <div class="mono-progress-fill" :style="{ width: Math.round((steamMonoStatus.downloadBytes / steamMonoStatus.downloadTotal) * 100) + '%' }"></div>
          </div>
        </div>
      </div>
      <div v-else-if="steamMonoStatus && steamMonoStatus.upToDate" class="settings-row">
        <div>
          <div class="settings-label">Wine Mono</div>
          <div class="settings-desc">Wine Mono v{{ steamMonoStatus.installedVersion }} is up to date.</div>
        </div>
        <div class="settings-value">
          <span class="badge badge-ok">Up to date</span>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Data &amp; Permissions</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">MetalSharp Data Folder</div>
          <div class="settings-desc">
            Preserves logs, Sharp Library apps, covers, launch options, caches, and runtime state across updates.
          </div>
        </div>
        <div class="settings-value">
          <button class="btn btn-secondary btn-sm" @click="openMetalsharpFolder">Open Data</button>
          <button class="btn btn-secondary btn-sm" @click="openLogsFolder">Open Logs</button>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Data Access Check</div>
          <div class="settings-desc">
            Recreates required folders and verifies MetalSharp can write logs and library metadata after an app update.
          </div>
        </div>
        <div class="settings-value">
          <button class="btn btn-primary btn-sm" @click="repairDataAccess">Repair &amp; Verify</button>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Cache</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Shader Cache</div>
          <div class="settings-desc">Persist compiled shaders to disk for faster loading</div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="cacheBadgeClass(shaderCache)">{{ cacheStatusText(shaderCache) }}</span>
          <span v-if="shaderCache?.apps" class="settings-version">{{ shaderCache.apps }} apps</span>
          <span v-if="shaderCache?.last_modified" class="settings-version">{{ shaderCache.last_modified }}</span>
          <button class="btn btn-secondary btn-sm" @click="clearShaderCache">Clear</button>
        </div>
      </div>
      <div class="settings-row">
        <div>
          <div class="settings-label">Pipeline Cache</div>
          <div class="settings-desc">Persist compiled pipeline state objects</div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="cacheBadgeClass(pipelineCache)">{{ cacheStatusText(pipelineCache) }}</span>
          <span v-if="pipelineCache?.last_modified" class="settings-version">{{ pipelineCache.last_modified }}</span>
          <button class="btn btn-secondary btn-sm" @click="clearPipelineCache">Clear</button>
        </div>
      </div>
    </div>

    <div class="settings-section">
      <h2>Updates</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Version</div>
          <div class="settings-desc">
            {{
              updateStatus?.ok && updateStatus?.available
                ? `v${updateStatus.latest_version} available (current: v${updateStatus.current_version})`
                : updateStatus?.ok
                  ? "You're up to date"
                  : "Could not check for updates"
            }}
          </div>
        </div>
        <div class="settings-value">
          <span class="badge" :class="updateStatus?.ok ? 'badge-ok' : 'badge-warn'">
            v{{ updateStatus?.current_version ?? "unknown" }}
          </span>
          <button v-if="!updateDownloading" class="btn btn-secondary btn-sm" @click="checkForUpdates">Check Now</button>
        </div>
      </div>
      <div v-if="updateStatus?.ok && updateStatus?.available && !updateDownloading" class="settings-row">
        <div>
          <div class="settings-label">Download Update</div>
          <div class="settings-desc">v{{ updateStatus.latest_version }} is ready to download</div>
        </div>
        <div class="settings-value">
          <button class="btn btn-primary btn-sm" @click="startUpdateDownload">Download &amp; Install</button>
        </div>
      </div>
      <div v-if="updateDownloading" class="settings-row">
        <div>
          <div class="settings-label">{{ updateMessage || "Updating..." }}</div>
          <div class="settings-desc">Do not close MetalSharp during the update</div>
        </div>
        <div class="settings-value">
          <div class="update-progress-bar">
            <div class="update-progress-fill" :style="{ width: updateProgress + '%' }"></div>
          </div>
          <span class="settings-version">{{ updateProgress }}%</span>
        </div>
      </div>
    </div>

    <div class="settings-section danger-zone">
      <h2><IconTrash2 width="14" height="14" /> Danger Zone</h2>
      <div class="settings-row">
        <div>
          <div class="settings-label">Uninstall MetalSharp</div>
          <div class="settings-desc">
            Permanently deletes all Wine prefixes, bottles, Steam installation, Wine runtime, shader caches, and settings. The app will close after cleanup.
          </div>
        </div>
        <div class="settings-value">
          <button class="btn btn-danger btn-sm" @click="uninstallMetalsharp">Uninstall MetalSharp</button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.settings-view {
  padding: 24px 28px;
  height: 100%;
  overflow-y: auto;
}
.settings-header {
  margin: -24px -28px 24px;
  padding: 24px 28px 18px;
  background: var(--page-header-bg);
  border-bottom: 1px solid var(--border);
}
.settings-header h1 {
  font-size: 22px;
  font-weight: 600;
}

.settings-section {
  margin-bottom: 28px;
  padding-bottom: 20px;
  border-bottom: 1px solid var(--border);
}
.settings-section:last-child {
  border-bottom: none;
}
.settings-section h2 {
  font-size: 14px;
  font-weight: 600;
  color: var(--accent);
  margin-bottom: 14px;
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.settings-row {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 24px;
  padding: 10px 0;
}
.settings-label {
  font-size: 13px;
  font-weight: 600;
  margin-bottom: 2px;
}
.settings-desc {
  font-size: 12px;
  color: var(--text-dim);
  line-height: 1.4;
}
.settings-value {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
  flex-wrap: wrap;
  justify-content: flex-end;
}
.settings-input-row {
  display: flex;
  gap: 8px;
  align-items: center;
}
.settings-input-row input {
  width: 280px;
}
.settings-version {
  font-size: 12px;
  color: var(--text-dim);
}
.update-progress-bar {
  width: 140px;
  height: 6px;
  background: var(--border);
  border-radius: 3px;
  overflow: hidden;
}
.update-progress-fill {
  height: 100%;
  background: var(--accent);
  border-radius: 3px;
  transition: width 0.3s ease;
}
.settings-toggle {
  display: inline-flex;
  align-items: center;
  min-height: 30px;
}
.danger-zone {
  border-color: rgba(220, 60, 60, 0.25);
}
.danger-zone h2 {
  color: #dc3c3c;
  display: flex;
  align-items: center;
  gap: 6px;
}
.btn-danger {
  background: rgba(220, 60, 60, 0.15);
  border: 1px solid rgba(220, 60, 60, 0.3);
  color: #dc3c3c;
}
.btn-danger:hover {
  background: rgba(220, 60, 60, 0.25);
}
.download-error {
  color: #dc3c3c;
  font-weight: 500;
}
.mono-progress-bar {
  width: 120px;
  height: 6px;
  background: var(--border);
  border-radius: 3px;
  overflow: hidden;
  margin-top: 4px;
}
.mono-progress-fill {
  height: 100%;
  background: var(--accent);
  border-radius: 3px;
  transition: width 0.3s ease;
}
</style>
