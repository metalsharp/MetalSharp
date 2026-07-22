<script setup lang="ts">
import { ref, computed, onMounted, provide, watch, type Component } from "vue";
import Sidebar from "./components/Sidebar.vue";
import Toast from "./components/Toast.vue";
import SetupWizard from "./components/SetupWizard.vue";
import MigrationView from "./components/MigrationView.vue";
import ProcessManagerOverlay from "./components/ProcessManagerOverlay.vue";
import LibraryView from "./views/LibraryView.vue";
import SharpView from "./views/SharpView.vue";
import LogsView from "./views/LogsView.vue";
import SettingsView from "./views/SettingsView.vue";
import { useTheme } from "./composables/useTheme";
import { useToast } from "./composables/useToast";
import { getAPI, api } from "./composables/useApi";
import type { AppConfig, UpdateStatus, SteamStatus } from "./api-types";

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
}

interface SteamLibrary {
  ok: boolean;
  total: number;
  installed_count: number;
  games: SteamGame[];
}

const currentView = ref("library");
const isProcessManagerOverlay = new URLSearchParams(window.location.search).get("overlay") === "process-manager";
const showSetup = ref(false);
const showMigration = ref(false);
const backendConnected = ref(false);
const backendVersion = ref<string | null>(null);
const wineSteamInstalled = ref(false);
const wineSteamRunning = ref(false);
const macSteamInstalled = ref(false);
const macSteamRunning = ref(false);
const library = ref<SteamLibrary | null>(null);
const config = ref<AppConfig | null>(null);
const updateStatus = ref<UpdateStatus | null>(null);
const steamApiKey = ref<string | null>(null);
const setupDeviceName = ref("");
const developerMode = ref(localStorage.getItem("metalsharp-developer-mode") === "true");
const lowPerformanceMode = ref(localStorage.getItem("metalsharp-low-performance-mode") === "true");

const updateDownloading = ref(false);
const updateProgress = ref(0);
const updateMessage = ref("");
const showUpdateChangelog = ref(false);
const updateDismissed = ref(false);
let updatePollTimer: ReturnType<typeof setInterval> | null = null;
let installPollTimer: ReturnType<typeof setInterval> | null = null;

const { theme, toggle: toggleTheme } = useTheme();
const toast = useToast();

const viewMap: Record<string, Component> = {
  library: LibraryView,
  "sharp-library": SharpView,
  logs: LogsView,
  settings: SettingsView,
};

const activeView = computed(() => viewMap[currentView.value] ?? LibraryView);

const updateChangelog = computed(() => {
  if (!updateStatus.value?.release_notes) return "";
  const firstLine = updateStatus.value.release_notes.split("\n").find((l) => l.trim().length > 0) ?? "";
  const cleaned = firstLine
    .replace(/^#+\s*/, "")
    .replace(/\*\*/g, "")
    .replace(/`/g, "")
    .trim();
  if (cleaned.length <= 20) return cleaned;
  return cleaned.slice(0, 19) + "\u2026";
});

const fullUpdateChangelog = computed(() => updateStatus.value?.release_notes?.trim() ?? "");

provide("library", library);
provide("config", config);
provide("wineSteamInstalled", wineSteamInstalled);
provide("wineSteamRunning", wineSteamRunning);
provide("macSteamInstalled", macSteamInstalled);
provide("macSteamRunning", macSteamRunning);
provide("backendConnected", backendConnected);
provide("backendVersion", backendVersion);
provide("updateStatus", updateStatus);
provide("updateDownloading", updateDownloading);
provide("updateProgress", updateProgress);
provide("updateMessage", updateMessage);
provide("startUpdateDownload", startUpdateDownload);
provide("steamApiKey", steamApiKey);
provide("setupDeviceName", setupDeviceName);
provide("developerMode", developerMode);
provide("lowPerformanceMode", lowPerformanceMode);
provide("toast", toast);
provide("loadLibrary", loadLibrary);
provide("api", api);

async function refreshSteamStatus() {
  const steamStatus = await api<{
    installed: boolean;
    running: boolean;
    mac_installed: boolean;
    mac_running: boolean;
    metalsharp_wine_available: boolean;
  }>("GET", "/steam/status");
  if (steamStatus) {
    wineSteamInstalled.value = steamStatus.installed;
    wineSteamRunning.value = steamStatus.running;
    macSteamInstalled.value = steamStatus.mac_installed;
    macSteamRunning.value = steamStatus.mac_running;
  }
}

async function loadLibrary() {
  const lib = await api<SteamLibrary>("GET", "/steam/library");
  if (lib && Array.isArray(lib.games)) {
    library.value = lib;
  }

  await api<{ steam: SteamStatus }>("GET", "/scan");
  await refreshSteamStatus();
}

async function checkBackend() {
  try {
    const res = await getAPI().request("GET", "/status");
    backendConnected.value = res.ok;
    if (res.ok) {
      const status = res.data as { version?: string } | undefined;
      if (status?.version) backendVersion.value = status.version;
    }
  } catch {
    backendConnected.value = false;
  }
}

async function checkForUpdates() {
  const result = await api<UpdateStatus>("GET", "/update/check");
  if (result) updateStatus.value = result;
  if (result?.ok && result.available) {
    toast.show(`Update available: v${result.latest_version}`, "success");
  }
}

async function startUpdateDownload() {
  if (updateDownloading.value) return;
  const backend = getAPI();
  const ready = await backend.updaterEnsureReady();
  if (!ready?.ok) {
    toast.show(ready?.error ?? "Updater not available", "error");
    return;
  }
  const pid = await backend.backendGetPid();
  if (!pid) {
    toast.show("Cannot get backend PID", "error");
    return;
  }
  const targetVersion = updateStatus.value?.latest_version ?? "";
  if (!targetVersion) {
    toast.show("Update version is unavailable", "error");
    return;
  }
  if (!updateStatus.value?.download_url) {
    toast.show("Update DMG asset is unavailable", "error");
    return;
  }
  updateDownloading.value = true;
  updateProgress.value = 0;
  updateMessage.value = "Starting download...";
  await backend.updaterClearStatus();

  const startResult = await api<{ ok: boolean; error?: string }>("POST", "/update/start");
  if (!startResult?.ok) {
    toast.show(startResult?.error ?? "Failed to start download", "error");
    updateDownloading.value = false;
    return;
  }

  if (updatePollTimer) clearInterval(updatePollTimer);
  let downloadPolls = 0;
  updatePollTimer = setInterval(async () => {
    downloadPolls += 1;
    const progress = await api<{ status: string; percent: number; message: string; error: string | null }>(
      "GET",
      "/update/progress",
    );
    if (!progress) return;
    updateProgress.value = progress.percent ?? 0;
    updateMessage.value = progress.message ?? "";
    if (progress.status === "downloaded" || progress.status === "complete") {
      if (updatePollTimer) clearInterval(updatePollTimer);
      updatePollTimer = null;
      const dmgResult = await api<{ ok: boolean; path?: string; version?: string }>("GET", "/update/dmg-path");
      if (!dmgResult?.path) {
        toast.show("Download complete but DMG not found", "error");
        updateDownloading.value = false;
        return;
      }
      const installVersion = dmgResult.version ?? targetVersion;
      const spawnResult = await backend.updaterSpawnInstall(dmgResult.path, pid, installVersion);
      if (!spawnResult?.ok) {
        toast.show(spawnResult?.error ?? "Failed to start installer", "error");
        updateDownloading.value = false;
        return;
      }
      updateMessage.value = "Installing update...";
      updateProgress.value = 90;
      if (installPollTimer) clearInterval(installPollTimer);
      let installPolls = 0;
      installPollTimer = setInterval(async () => {
        installPolls += 1;
        const installStatus = await backend.updaterInstallStatus();
        if (!installStatus) {
          if (installPolls > 180) {
            if (installPollTimer) clearInterval(installPollTimer);
            installPollTimer = null;
            updateDownloading.value = false;
            toast.show("Installer did not report status", "error");
          }
          return;
        }
        updateProgress.value = installStatus.percent ?? 90;
        updateMessage.value = installStatus.message ?? "Installing...";
        if (installStatus.phase === "complete") {
          if (installPollTimer) clearInterval(installPollTimer);
          installPollTimer = null;
          updateDownloading.value = false;
          updateProgress.value = 100;
          updateMessage.value = "";
          toast.show("Update installed — restarting...", "success");
          await new Promise((r) => setTimeout(r, 2000));
          backend.quitApp();
        } else if (installStatus.phase === "error") {
          if (installPollTimer) clearInterval(installPollTimer);
          installPollTimer = null;
          updateDownloading.value = false;
          toast.show(installStatus.error ?? installStatus.message ?? "Install failed", "error");
        }
      }, 1000);
    } else if (progress.status === "error") {
      if (updatePollTimer) clearInterval(updatePollTimer);
      updatePollTimer = null;
      updateDownloading.value = false;
      toast.show(progress.error ?? "Download failed", "error");
    } else if (downloadPolls > 3600) {
      if (updatePollTimer) clearInterval(updatePollTimer);
      updatePollTimer = null;
      updateDownloading.value = false;
      toast.show("Update download timed out", "error");
    }
  }, 500);
}

async function getSteamApiKey() {
  const result = await api<{ key: string }>("GET", "/steam/api-key");
  steamApiKey.value = result?.key ?? null;
}

function startHealthPolling() {
  setInterval(refreshSteamStatus, 5000);

  setInterval(async () => {
    const result = await api<{ ok?: boolean; new_appids?: number[] }>("GET", "/steam/watch-steamapps");
    if (result?.new_appids && result.new_appids.length > 0) {
      await loadLibrary();
    }
  }, 30000);

  setInterval(async () => {
    const prev = backendConnected.value;
    backendConnected.value = await getAPI().isBackendAlive();
    if (backendConnected.value) {
      try {
        const status = await api<{ ok?: boolean; version?: string }>("GET", "/status");
        if (status?.version) backendVersion.value = status.version;
      } catch {}
    } else {
      backendVersion.value = null;
    }
    if (prev && !backendConnected.value) toast.show("Backend connection lost", "error");
    else if (!prev && backendConnected.value) toast.show("Backend connected", "success");
  }, 120000);
}

function onSetupDone() {
  showSetup.value = false;
  initApp();
}

async function initApp() {
  await getSteamApiKey();
  await loadLibrary();
  const state = await api<{ deviceName?: string }>("GET", "/setup/state");
  if (state?.deviceName) setupDeviceName.value = state.deviceName;
  startHealthPolling();
}

function applyLowPerformanceMode(enabled: boolean) {
  document.documentElement.dataset.lowPerformance = String(enabled);
}

watch(lowPerformanceMode, (enabled) => {
  localStorage.setItem("metalsharp-low-performance-mode", String(enabled));
  applyLowPerformanceMode(enabled);
});

onMounted(async () => {
  applyLowPerformanceMode(lowPerformanceMode.value);
  await checkBackend();
  const migrationMode = await getAPI().isMigrationMode?.();
  if (migrationMode) {
    showMigration.value = true;
    return;
  }
  const firstLaunch = await getAPI().isFirstLaunch();
  const setupState = await api<{ deviceName?: string; runtimeMigrationRequired?: boolean }>("GET", "/setup/state");
  if (setupState?.deviceName) setupDeviceName.value = setupState.deviceName;
  if (firstLaunch || setupState?.runtimeMigrationRequired) {
    showSetup.value = true;
    return;
  }
  await initApp();
  checkForUpdates();
});
</script>

<template>
  <ProcessManagerOverlay v-if="isProcessManagerOverlay" />
  <MigrationView v-else-if="showMigration" />
  <SetupWizard v-else-if="showSetup" @done="onSetupDone()" />
  <template v-else>
    <Sidebar
      :current-view="currentView"
      :theme="theme"
      @navigate="currentView = $event"
      @toggle-theme="toggleTheme()"
    />
    <main
      class="content"
      :class="{ 'content-glass-header': ['library', 'sharp-library', 'logs'].includes(currentView) }"
    >
      <div class="drag-strip"></div>
      <div v-if="updateStatus?.ok && updateStatus?.available && !updateDismissed" class="update-banner">
        <span class="update-banner-text" v-if="!updateDownloading"
          >MetalSharp v{{ updateStatus.latest_version }} is available<span
            v-if="updateChangelog"
            class="update-changelog"
            >{{ updateChangelog }}</span
          ></span
        >
        <span class="update-banner-text" v-else>{{ updateMessage }}</span>
        <div v-if="updateDownloading" class="update-banner-progress">
          <div class="update-banner-progress-fill" :style="{ width: updateProgress + '%' }"></div>
        </div>
        <button v-if="!updateDownloading" class="update-banner-btn" @click="startUpdateDownload">
          Download &amp; Install
        </button>
        <button
          v-if="!updateDownloading && fullUpdateChangelog"
          class="update-banner-btn update-banner-secondary"
          @click.stop="showUpdateChangelog = true"
        >
          What's New
        </button>
        <button v-if="!updateDownloading" class="update-banner-close" @click="updateDismissed = true" title="Dismiss">
          &times;
        </button>
      </div>
      <component :is="activeView" :key="currentView" />
    </main>
  </template>
  <Teleport to="body">
    <div v-if="showUpdateChangelog" class="modal-backdrop" @click="showUpdateChangelog = false">
      <section class="update-changelog-modal" @click.stop>
        <header class="update-changelog-modal-header">
          <h2>MetalSharp v{{ updateStatus?.latest_version }}</h2>
          <button
            class="modal-close-btn"
            type="button"
            aria-label="Close"
            title="Close"
            @click="showUpdateChangelog = false"
          >
            x
          </button>
        </header>
        <pre class="update-changelog-body">{{ fullUpdateChangelog }}</pre>
      </section>
    </div>
  </Teleport>
  <Toast />
</template>

<style>
.drag-strip {
  height: 0;
  -webkit-app-region: drag;
  flex-shrink: 0;
}
.update-banner {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  padding: 8px 16px;
  background: var(--sidebar-bg);
  backdrop-filter: blur(24px) saturate(180%);
  -webkit-backdrop-filter: blur(24px) saturate(180%);
  border-bottom: 1px solid rgba(140, 170, 200, 0.08);
  color: var(--text-primary);
  font-size: 12px;
  font-weight: 600;
  flex-shrink: 0;
  position: relative;
}
.update-banner::before {
  content: "";
  position: absolute;
  inset: 0;
  background: linear-gradient(
    90deg,
    rgba(95, 183, 232, 0.06) 0%,
    transparent 30%,
    transparent 70%,
    rgba(95, 183, 232, 0.04) 100%
  );
  pointer-events: none;
}
.update-banner:hover {
  opacity: 0.9;
}
.update-banner-text {
  white-space: nowrap;
}
.update-changelog {
  margin-left: 8px;
  opacity: 0.7;
  font-weight: 400;
}
.update-banner-btn {
  background: rgba(95, 183, 232, 0.18);
  border: 1px solid rgba(95, 183, 232, 0.25);
  color: var(--accent);
  padding: 3px 14px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 700;
  cursor: pointer;
  position: relative;
  z-index: 1;
}
.update-banner-btn:hover {
  background: rgba(95, 183, 232, 0.28);
}
.update-banner-secondary {
  background: rgba(140, 170, 200, 0.1);
  border-color: var(--border);
  color: var(--text-secondary);
}
.update-banner-close {
  background: none;
  border: none;
  color: var(--text-dim);
  font-size: 18px;
  line-height: 1;
  cursor: pointer;
  padding: 0 4px;
  position: relative;
  z-index: 1;
}
.update-banner-close:hover {
  color: var(--text-primary);
}
.update-banner-progress {
  width: 120px;
  height: 4px;
  background: rgba(140, 170, 200, 0.15);
  border-radius: 2px;
  overflow: hidden;
}
.update-banner-progress-fill {
  height: 100%;
  background: var(--accent);
  border-radius: 2px;
  transition: width 0.3s ease;
}
.modal-backdrop {
  position: fixed;
  inset: 0;
  z-index: 80;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 32px;
  background: rgba(6, 10, 16, 0.68);
}
.update-changelog-modal {
  width: 640px;
  max-width: calc(100vw - 64px);
  max-height: min(620px, 82vh);
  display: flex;
  flex-direction: column;
  background: var(--bg-card);
  color: var(--text-primary);
  border: 1px solid var(--border);
  border-radius: 8px;
  box-shadow: 0 24px 60px rgba(0, 0, 0, 0.42);
  overflow: hidden;
  transform: translateZ(0);
}
.update-changelog-modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 16px 18px;
  border-bottom: 1px solid var(--border);
  flex-shrink: 0;
}
.update-changelog-modal-header h2 {
  margin: 0;
  font-size: 16px;
  line-height: 1.25;
}
.modal-close-btn {
  border: 1px solid var(--border);
  background: var(--bg-surface);
  color: var(--text-primary);
  border-radius: 4px;
  width: 32px;
  height: 32px;
  padding: 0;
  font-size: 18px;
  line-height: 1;
  font-weight: 700;
  cursor: pointer;
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.update-changelog-modal pre,
.update-changelog-body {
  margin: 0;
  padding: 18px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-word;
  font: inherit;
  line-height: 1.5;
  flex: 1;
  min-height: 0;
}
.content {
  flex: 1;
  overflow-y: auto;
  padding: 0;
  min-width: 0;
  display: flex;
  flex-direction: column;
  background: var(--bg-deep);
}
:root[data-theme="developer"] .content {
  background:
    linear-gradient(118deg, rgba(185, 255, 77, 0.025), transparent 36%, rgba(0, 245, 255, 0.03) 72%, transparent),
    radial-gradient(circle at 24% 16%, rgba(255, 46, 247, 0.14), transparent 34%),
    radial-gradient(circle at 84% 10%, rgba(0, 245, 255, 0.11), transparent 30%), var(--bg-deep);
}
.content.content-glass-header {
  background: transparent;
}
:root[data-theme="developer"] .update-banner {
  background:
    linear-gradient(90deg, rgba(185, 255, 77, 0.16), rgba(0, 245, 255, 0.1), rgba(255, 46, 247, 0.14)),
    rgba(9, 7, 15, 0.84);
  border-bottom-color: rgba(185, 255, 77, 0.28);
  color: var(--text-primary);
}
:root[data-theme="developer"] .update-banner-btn {
  border-radius: 999px;
  border-color: rgba(185, 255, 77, 0.42);
  background: rgba(9, 7, 15, 0.62);
  color: var(--accent);
}
</style>
