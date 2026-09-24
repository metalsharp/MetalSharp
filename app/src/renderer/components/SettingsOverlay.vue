<script setup lang="ts">
import { ref, inject, onMounted, onUnmounted, type Ref } from "vue";
import { useI18n } from "vue-i18n";
import { useToast } from "../composables/useToast";
import { api, getAPI } from "../composables/useApi";
import type { AppConfig, UpdateStatus } from "../api-types";
import IconX from "~icons/lucide/x";
import IconTrash2 from "~icons/lucide/trash-2";
import LanguagePicker from "./LanguagePicker.vue";

interface CacheSummary {
  bytes: number;
  files: number;
  directories: number;
  apps: number;
  status: "missing" | "empty" | "active";
  path: string;
  last_modified: string | null;
}

const emit = defineEmits<{ close: [] }>();

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
const startFexUpdateDownload = inject<() => void>("startFexUpdateDownload")!;
const steamApiKey = inject<Ref<string | null>>("steamApiKey")!;
const setupDeviceName = inject<Ref<string>>("setupDeviceName")!;
const reloadLibrary = inject<() => Promise<void>>("loadLibrary")!;
const library =
  inject<Ref<{ ok: boolean; total: number; installed_count: number; games: unknown[] } | null>>("library")!;
const developerMode = inject<Ref<boolean>>("developerMode")!;
const reopenSetup = inject<(() => void) | null>("reopenSetup", null);;
const lowPerformanceMode = inject<Ref<boolean>>("lowPerformanceMode")!;

const toast = useToast();
const { t } = useI18n();
const shaderCache = ref<CacheSummary | null>(null);
const pipelineCache = ref<CacheSummary | null>(null);
const apiKeyInput = ref("");
const graphicsRuntimeLogs = ref(false);
const retinaMode = ref(false);
const retinaModeBusy = ref(false);

onMounted(async () => {
  apiKeyInput.value = steamApiKey.value ?? "";
  await refreshConfig();
  await refreshCacheSizes();
  window.addEventListener("keydown", handleKeydown);
});

onUnmounted(() => {
  window.removeEventListener("keydown", handleKeydown);
});

function handleKeydown(event: KeyboardEvent) {
  if (event.key === "Escape") emit("close");
}

async function refreshConfig() {
  const result = await api<AppConfig>("GET", "/config");
  if (result?.ok) {
    config.value = result;
    graphicsRuntimeLogs.value = Boolean(result.graphicsRuntimeLogs ?? result.graphics_runtime_logs);
    retinaMode.value = result.retinaMode === true;
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
  if (!confirm("Force kill MetalSharp Wine/runtime processes? This can stop active games, installers, and downloads."))
    return;
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
    toast.show(
      count > 0 ? `Force killed ${count} process${count === 1 ? "" : "es"}` : "No MetalSharp runtime processes found",
      "success",
    );
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
      graphicsRuntimeLogs.value
        ? "Graphics runtime logs enabled for future launches"
        : "Graphics runtime logs disabled",
      "success",
    );
  } else {
    graphicsRuntimeLogs.value = previous;
    toast.show("Failed to save graphics logging setting", "error");
  }
}

async function toggleRetinaMode(enabled: boolean) {
  if (retinaModeBusy.value || enabled === retinaMode.value) return;
  const previous = retinaMode.value;
  retinaModeBusy.value = true;
  retinaMode.value = enabled;
  const result = await api<AppConfig>("POST", "/config", { retinaMode: enabled });
  if (result?.ok) {
    config.value = result;
    retinaMode.value = result.retinaMode === true;
    toast.show(`Retina rendering ${enabled ? "enabled" : "disabled"} — restart Wine Steam to apply`, "success");
  } else {
    retinaMode.value = previous;
    toast.show("Failed to update Retina rendering", "error");
  }
  retinaModeBusy.value = false;
}

function uninstallMetalsharp() {
  getAPI().uninstallApp();
}
</script>

<template>
  <div class="settings-overlay" @click.self="emit('close')">
    <div class="settings-panel">
      <header class="so-header">
        <div class="so-header-title">
          <span class="so-eyebrow">METALSHARP</span>
          <h2>{{ t("settings.title") }}</h2>
        </div>
        <div class="so-header-actions">
          <button class="so-close" type="button" aria-label="Close settings" @click="emit('close')">
            <IconX width="18" height="18" />
          </button>
        </div>
      </header>

      <div class="so-body">
        <section class="so-card">
          <h3>{{ t("settings.steamIntegration") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.apiKey") }}</div>
              <div class="so-desc">
                Required to load your full game library. Get a free key at
                <a href="https://steamcommunity.com/dev/apikey" target="_blank" rel="noreferrer">steamcommunity.com/dev/apikey</a>.
              </div>
            </div>
            <div class="so-row-control">
              <div class="so-input-row">
                <input v-model="apiKeyInput" type="password" class="so-input" :placeholder="t('ui.settings.apiKey')" />
                <button class="so-btn primary" type="button" @click="saveApiKey">{{ t("actions.save") }} &amp; Sync</button>
              </div>
              <span v-if="steamApiKey" class="badge badge-ok">{{ t("ui.settings.keySaved") }}</span>
              <span v-else class="badge badge-warn">{{ t("ui.settings.noKey") }}</span>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.deviceName") }}</div>
              <div class="so-desc">Identifies this machine to Steam for persistent login</div>
            </div>
            <div class="so-row-control">
              <span class="so-value-text">{{ setupDeviceName || t("ui.settings.notSet") }}</span>
              <button class="so-btn" type="button" @click="changeDeviceName">{{ t("ui.settings.change") }}</button>
              <span class="so-row-divider" aria-hidden="true"></span>
              <LanguagePicker compact />
            </div>
          </div>
        </section>

        <section class="so-card">
          <h3>{{ t("settings.steam") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.wineSteam") }}</div>
              <div class="so-desc">Windows Steam running in MetalSharp Wine</div>
            </div>
            <div class="so-row-control">
              <span v-if="wineSteamInstalled" class="badge badge-ok">{{ t("ui.settings.installed") }}</span>
              <span v-else class="badge badge-warn">{{ t("ui.settings.notInstalled") }}</span>
              <button v-if="wineSteamInstalled" class="so-btn" type="button" @click="toggleSteam">
                {{ wineSteamRunning ? t("ui.settings.stopSteam") : t("ui.settings.startSteam") }}
              </button>
            </div>
          </div>
          <div v-if="!wineSteamInstalled && reopenSetup" class="so-row">
            <div class="so-row-info">
              <div class="so-label">Missing Windows Steam?</div>
              <div class="so-desc">Re-run the setup wizard to install or repair the Steam runtime</div>
            </div>
            <div class="so-row-control">
              <button class="so-btn primary" type="button" @click="reopenSetup">Run Setup Wizard</button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.steamMac") }}</div>
              <div class="so-desc">Native macOS Steam used for games with Mac builds</div>
            </div>
            <div class="so-row-control">
              <span v-if="macSteamInstalled" class="badge badge-ok">Installed</span>
              <span v-else class="badge badge-warn">Not Installed</span>
              <button v-if="macSteamInstalled" class="so-btn" type="button" @click="toggleMacSteam">
                {{ macSteamRunning ? t("ui.settings.stopSteamMac") : t("ui.settings.startSteamMac") }}
              </button>
              <button v-else class="so-btn primary" type="button" @click="installMacSteam">{{ t("ui.settings.installMacSteam") }}</button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">High Resolution (Retina)</div>
              <div class="so-desc">Sharp Wine windows at native display resolution; restart Wine Steam to apply</div>
            </div>
            <div class="so-row-control">
              <label class="so-toggle" aria-label="High Resolution Retina">
                <input
                  type="checkbox"
                  :checked="retinaMode"
                  :disabled="retinaModeBusy"
                  @change="toggleRetinaMode(($event.target as HTMLInputElement).checked)"
                />
                <span class="so-switch"></span>
              </label>
            </div>
          </div>
        </section>

        <section class="so-card">
          <h3>{{ t("settings.backend") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.backendRuntime") }}</div>
              <div class="so-desc">The C backend handles game launches, Steam integration, and shader management</div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="backendConnected ? 'badge-ok' : 'badge-warn'">
                {{ backendConnected ? t("ui.settings.connected") : t("ui.settings.offline") }}
              </span>
              <span v-if="backendVersion" class="so-value-dim">v{{ backendVersion }}</span>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.restartBackend") }}</div>
              <div class="so-desc">Kill and restart the backend process</div>
            </div>
            <div class="so-row-control">
              <button class="so-btn" type="button" :disabled="backendRestarting" @click="restartBackend">
                {{ backendRestarting ? t("ui.settings.restarting") : t("ui.settings.restartBackend") }}
              </button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.forceKill") }}</div>
              <div class="so-desc">
                Destructively stops MetalSharp Wine/runtime helper processes while keeping this app and backend alive.
              </div>
            </div>
            <div class="so-row-control">
              <button class="so-btn danger" type="button" @click="forceKillProcesses">{{ t("ui.settings.forceKill") }}</button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.lowPerformance") }}</div>
              <div class="so-desc">
                Disables blur, glass, glow, and heavy motion while preserving layout and essential progress updates.
              </div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="lowPerformanceMode ? 'badge-warn' : 'badge-ok'">
                {{ lowPerformanceMode ? "Reduced Effects" : "Full Effects" }}
              </span>
              <label class="so-toggle" aria-label="Low Performance Mode">
                <input
                  type="checkbox"
                  :checked="lowPerformanceMode"
                  @change="toggleLowPerformanceMode(($event.target as HTMLInputElement).checked)"
                />
                <span class="so-switch"></span>
              </label>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.developerTools") }}</div>
              <div class="so-desc">Show launch routing, doctor controls, and advanced card tools</div>
            </div>
            <div class="so-row-control">
              <label class="so-toggle" aria-label="Developer Tools">
                <input
                  type="checkbox"
                  :checked="developerMode"
                  @change="toggleDeveloperMode(($event.target as HTMLInputElement).checked)"
                />
                <span class="so-switch"></span>
              </label>
            </div>
          </div>
          <div v-if="developerMode" class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.graphicsLogs") }}</div>
              <div class="so-desc">
                Opt in to DXMT graphics logs for future launches. Off by default to keep routine launches quiet
                unless requested.
              </div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="graphicsRuntimeLogs ? 'badge-warn' : 'badge-ok'">
                {{ graphicsRuntimeLogs ? "Logs On" : "Default Off" }}
              </span>
              <label class="so-toggle" aria-label="Graphics Runtime Logs">
                <input
                  type="checkbox"
                  :checked="graphicsRuntimeLogs"
                  @change="toggleGraphicsRuntimeLogs(($event.target as HTMLInputElement).checked)"
                />
                <span class="so-switch"></span>
              </label>
            </div>
          </div>
        </section>

        <section class="so-card">
          <h3>{{ t("settings.dataPermissions") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.dataFolder") }}</div>
              <div class="so-desc">
                Preserves logs, Sharp Library apps, covers, launch options, caches, and runtime state across updates.
              </div>
            </div>
            <div class="so-row-control">
              <button class="so-btn" type="button" @click="openMetalsharpFolder">{{ t("ui.settings.openData") }}</button>
              <button class="so-btn" type="button" @click="openLogsFolder">{{ t("ui.settings.openLogs") }}</button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.dataAccess") }}</div>
              <div class="so-desc">
                Recreates required folders and verifies MetalSharp can write logs and library metadata after an app update.
              </div>
            </div>
            <div class="so-row-control">
              <button class="so-btn primary" type="button" @click="repairDataAccess">{{ t("ui.settings.repairVerify") }}</button>
            </div>
          </div>
        </section>

        <section class="so-card">
          <h3>{{ t("settings.cache") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.shaderCache") }}</div>
              <div class="so-desc">Persist compiled shaders to disk for faster loading</div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="cacheBadgeClass(shaderCache)">{{ cacheStatusText(shaderCache) }}</span>
              <span v-if="shaderCache?.apps" class="so-value-dim">{{ shaderCache.apps }} apps</span>
              <span v-if="shaderCache?.last_modified" class="so-value-dim">{{ shaderCache.last_modified }}</span>
              <button class="so-btn" type="button" @click="clearShaderCache">{{ t("ui.settings.clear") }}</button>
            </div>
          </div>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.pipelineCache") }}</div>
              <div class="so-desc">Persist compiled pipeline state objects</div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="cacheBadgeClass(pipelineCache)">{{ cacheStatusText(pipelineCache) }}</span>
              <span v-if="pipelineCache?.last_modified" class="so-value-dim">{{ pipelineCache.last_modified }}</span>
              <button class="so-btn" type="button" @click="clearPipelineCache">{{ t("ui.settings.clear") }}</button>
            </div>
          </div>
        </section>

        <section class="so-card">
          <h3>{{ t("settings.updates") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.version") }}</div>
              <div class="so-desc">
                {{
                  updateStatus?.ok && updateStatus?.available
                    ? `v${updateStatus.latest_version} available (current: v${updateStatus.current_version})`
                    : updateStatus?.ok
                      ? t("ui.settings.upToDate")
                      : t("ui.settings.couldNotCheck")
                }}
              </div>
            </div>
            <div class="so-row-control">
              <span class="badge" :class="updateStatus?.ok ? 'badge-ok' : 'badge-warn'">
                v{{ updateStatus?.current_version ?? "unknown" }}
              </span>
              <button v-if="!updateDownloading" class="so-btn" type="button" @click="checkForUpdates">{{ t("ui.settings.checkNow") }}</button>
            </div>
          </div>
          <div v-if="updateStatus?.ok && updateStatus?.available && !updateDownloading" class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.downloadUpdate") }}</div>
              <div class="so-desc">v{{ updateStatus.latest_version }} is ready to download</div>
            </div>
            <div class="so-row-control">
              <button class="so-btn primary" type="button" @click="startUpdateDownload()">{{ t("ui.settings.downloadInstall") }}</button>
            </div>
          </div>
          <div
            v-if="updateStatus?.ok && updateStatus?.available && updateStatus?.fex_available && !updateDownloading"
            class="so-row"
          >
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.fexUpdate") }}</div>
              <div class="so-desc">macOS 27+ only · experimental and potentially less stable than baseline</div>
            </div>
            <div class="so-row-control">
              <button class="so-btn" type="button" @click="startFexUpdateDownload">{{ t("ui.settings.updateFex") }}</button>
            </div>
          </div>
          <div v-if="updateDownloading" class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ updateMessage || t("ui.settings.updating") }}</div>
              <div class="so-desc">Do not close MetalSharp during the update</div>
            </div>
            <div class="so-row-control">
              <div class="so-progress">
                <div class="so-progress-fill" :style="{ width: updateProgress + '%' }"></div>
              </div>
              <span class="so-value-dim">{{ updateProgress }}%</span>
            </div>
          </div>
        </section>

        <section class="so-card danger">
          <h3><IconTrash2 width="14" height="14" /> {{ t("settings.dangerZone") }}</h3>
          <div class="so-row">
            <div class="so-row-info">
              <div class="so-label">{{ t("ui.settings.uninstall") }}</div>
              <div class="so-desc">
                Permanently deletes all Wine prefixes, bottles, Steam installation, Wine runtime, shader caches, and
                settings. The app will close after cleanup.
              </div>
            </div>
            <div class="so-row-control">
              <button class="so-btn danger" type="button" @click="uninstallMetalsharp">{{ t("ui.settings.uninstall") }}</button>
            </div>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* Themed via the --library-* custom properties inherited from .library-view. */
.settings-overlay {
  position: fixed;
  z-index: 120;
  inset: 0;
  display: flex;
  justify-content: center;
  align-items: flex-start;
  padding: 26px 24px 24px;
  background: rgba(6, 8, 10, 0.58);
  backdrop-filter: blur(10px);
  animation: so-fade 0.22s ease;
}
.settings-panel {
  display: flex;
  flex-direction: column;
  width: min(1080px, 100%);
  max-height: calc(100vh - 52px);
  border: 1px solid var(--library-accent);
  border-radius: 14px;
  background: color-mix(in srgb, var(--library-control-bg) 94%, #000);
  box-shadow: 0 30px 90px rgba(0, 0, 0, 0.55), 0 0 40px color-mix(in srgb, var(--library-accent) 14%, transparent);
  color: var(--library-control-text);
  overflow: hidden;
  -webkit-app-region: no-drag;
  animation: so-drop 0.28s cubic-bezier(0.22, 1, 0.36, 1);
}
@keyframes so-fade {
  from {
    opacity: 0;
  }
}
@keyframes so-drop {
  from {
    opacity: 0;
    transform: translateY(-26px);
  }
}
.so-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 18px 22px 14px;
  border-bottom: 1px solid var(--library-control-border);
  background: color-mix(in srgb, var(--library-control-hover) 40%, transparent);
}
.so-header-title {
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.so-eyebrow {
  color: color-mix(in srgb, var(--library-control-text) 60%, transparent);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 10px;
  letter-spacing: 0.5em;
}
.so-header h2 {
  margin: 0;
  color: var(--library-control-text);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 26px;
  font-weight: 500;
  line-height: 1;
}
.so-header-actions {
  display: inline-flex;
  align-items: center;
  gap: 12px;
}
.so-row-divider {
  align-self: stretch;
  width: 1px;
  min-height: 28px;
  margin: 0 6px;
  background: var(--library-control-border);
}
.so-row-control :deep(.language-picker) {
  color: var(--library-control-text);
}
.so-row-control :deep(.language-picker select) {
  border-color: var(--library-control-border);
  background: var(--library-control-bg);
  color: var(--library-control-text);
}
.so-close {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  border: 1px solid var(--library-accent);
  border-radius: 7px;
  color: #fff;
  background: transparent;
  cursor: pointer;
  transition: background 0.18s ease, transform 0.18s ease;
}
.so-close:hover {
  background: color-mix(in srgb, var(--library-accent) 18%, transparent);
  transform: translateY(-1px);
}
.so-body {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 14px;
  align-content: start;
  padding: 16px 20px 22px;
  overflow-y: auto;
}
.so-card {
  padding: 14px 16px 6px;
  border: 1px solid var(--library-control-border);
  border-radius: 10px;
  background: color-mix(in srgb, var(--library-control-bg) 82%, transparent);
}
.so-card h3 {
  display: flex;
  align-items: center;
  gap: 7px;
  margin: 0 0 10px;
  padding-bottom: 9px;
  border-bottom: 1px solid color-mix(in srgb, var(--library-accent) 45%, transparent);
  color: var(--library-control-text);
  font: inherit;
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
}
.so-card.danger h3 {
  border-bottom-color: color-mix(in srgb, #ff5c5c 55%, transparent);
  color: #ff8585;
}
.so-row {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 18px;
  padding: 11px 0;
  border-bottom: 1px solid color-mix(in srgb, var(--library-control-border) 55%, transparent);
}
.so-row:last-child {
  border-bottom: 0;
}
.so-row-info {
  flex: 1 1 auto;
  min-width: 0;
}
.so-label {
  color: var(--library-control-text);
  font-size: 13px;
  font-weight: 650;
}
.so-desc {
  margin-top: 3px;
  color: color-mix(in srgb, var(--library-control-text) 62%, transparent);
  font-size: 11.5px;
  line-height: 1.45;
}
.so-desc a {
  color: var(--library-accent);
  text-decoration: none;
}
.so-desc a:hover {
  text-decoration: underline;
}
.so-row-control {
  display: flex;
  flex: 0 0 auto;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
  max-width: 55%;
}
.so-value-text {
  color: var(--library-control-text);
  font-size: 12.5px;
}
.so-value-dim {
  color: color-mix(in srgb, var(--library-control-text) 62%, transparent);
  font-size: 11px;
}
.so-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 30px;
  padding: 0 12px;
  border: 1px solid var(--library-control-border);
  border-radius: 6px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  cursor: pointer;
  font: inherit;
  font-size: 11.5px;
  font-weight: 650;
  white-space: nowrap;
  transition: background 0.16s ease, border-color 0.16s ease;
}
.so-btn:hover:not(:disabled) {
  background: var(--library-control-hover);
  border-color: var(--library-accent);
}
.so-btn:disabled {
  opacity: 0.55;
  cursor: default;
}
.so-btn.primary {
  border-color: var(--library-accent);
  color: var(--library-control-bg);
  background: var(--library-accent);
}
.so-btn.primary:hover:not(:disabled) {
  background: color-mix(in srgb, var(--library-accent) 84%, #fff);
}
.so-btn.danger {
  border-color: rgba(255, 92, 92, 0.55);
  color: #ff8585;
  background: color-mix(in srgb, #ff5c5c 12%, transparent);
}
.so-btn.danger:hover:not(:disabled) {
  border-color: #ff5c5c;
  background: color-mix(in srgb, #ff5c5c 22%, transparent);
}
.so-input-row {
  display: flex;
  align-items: center;
  gap: 8px;
}
.so-input {
  width: 210px;
  min-height: 30px;
  padding: 0 10px;
  border: 1px solid var(--library-control-border);
  border-radius: 6px;
  color: var(--library-control-text);
  background: color-mix(in srgb, var(--library-control-bg) 70%, transparent);
  font: inherit;
  font-size: 12px;
  outline: none;
}
.so-input:focus {
  border-color: var(--library-accent);
}
.so-toggle {
  display: inline-flex;
  cursor: pointer;
}
.so-toggle input {
  display: none;
}
.so-switch {
  position: relative;
  display: inline-block;
  width: 34px;
  height: 19px;
  border: 1px solid var(--library-control-border);
  border-radius: 999px;
  background: var(--library-control-bg);
  transition: background 0.18s ease, border-color 0.18s ease;
}
.so-switch::after {
  content: "";
  position: absolute;
  top: 2px;
  left: 2px;
  width: 13px;
  height: 13px;
  border-radius: 50%;
  background: color-mix(in srgb, var(--library-control-text) 72%, transparent);
  transition: transform 0.18s ease, background 0.18s ease;
}
.so-toggle input:checked + .so-switch {
  border-color: var(--library-accent);
  background: color-mix(in srgb, var(--library-accent) 30%, var(--library-control-bg));
}
.so-toggle input:checked + .so-switch::after {
  transform: translateX(15px);
  background: var(--library-accent);
}
.so-progress {
  flex: 0 1 160px;
  height: 7px;
  border-radius: 999px;
  background: color-mix(in srgb, var(--library-control-border) 60%, transparent);
  overflow: hidden;
}
.so-progress-fill {
  height: 100%;
  border-radius: 999px;
  background: var(--library-accent);
  transition: width 0.3s ease;
}
@media (max-width: 900px) {
  .so-body {
    grid-template-columns: 1fr;
  }
  .so-row {
    flex-direction: column;
    gap: 8px;
  }
  .so-row-control {
    max-width: none;
    justify-content: flex-start;
  }
}
</style>
