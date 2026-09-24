<script setup lang="ts">
import { computed, ref, inject, type Ref } from "vue";
import { useToast } from "../composables/useToast";
import { api, getAPI } from "../composables/useApi";
import { useI18n } from "vue-i18n";
import LanguagePicker from "./LanguagePicker.vue";
import IconZap from "~icons/lucide/zap";
import IconBattery from "~icons/lucide/battery";
import IconLock from "~icons/lucide/lock";
import IconCheck from "~icons/lucide/check";
import IconLoader2 from "~icons/lucide/loader-2";
import IconScrollText from "~icons/lucide/scroll-text";
import IconMonitor from "~icons/lucide/monitor";
import IconGamepad2 from "~icons/lucide/gamepad-2";
import IconPlay from "~icons/lucide/play";

const emit = defineEmits<{ done: []; close: [] }>();
const props = defineProps<{ dismissible?: boolean }>();
const toast = useToast();
const { t, tm } = useI18n();
const library = inject<Ref<{ ok: boolean; total: number; installed_count: number; games: unknown[] } | null>>("library", ref(null));
const steamApiKey = inject<Ref<string | null>>("steamApiKey", ref(null));

// In mock preview mode a ?step=N query opens the wizard directly on a step.
const mockStepParam = Number(new URLSearchParams(window.location.search).get("step") ?? "0");
const step = ref(new URLSearchParams(window.location.search).has("mock") && Number.isFinite(mockStepParam) ? Math.min(Math.max(mockStepParam, 0), 2) : 0);
const deviceName = ref("");
const installProgress = ref(0);
const installStatus = ref("");
const installing = ref(false);
const installLogs = ref<{ text: string; cls: string }[]>([]);
const installCurrent = ref("");
const installFailed = ref(false);
const logOpen = ref(false);
const steamFailed = ref(false);
const finishing = ref(false);
const runtimeReady = computed(() => installStatus.value === "complete");
const steamButtonLabel = computed(() => {
  if (steamInstalled.value) return t("setup.steamInstalled");
  if (steamInstalling.value) return steamInstallLabel();
  if (steamFailed.value) return t("setup.steamFailed");
  return t("setup.installSteam");
});
const steamInstalled = ref(false);
const steamChecking = ref(false);
const steamInstalling = ref(false);
const steamInstallStage = ref("idle");
const installingSteam = ref(false);
const steps = computed(() => tm("setup.steps") as string[]);
const stepTitles = computed(() => tm("setup.titles") as string[]);
const stepTaglines = computed(() => tm("setup.taglines") as string[]);
const showcaseCovers = [
  { appid: 1091500, name: "Cyberpunk 2077", url: "https://cdn.cloudflare.steamstatic.com/steam/apps/1091500/library_600x900_2x.jpg" },
  { appid: 1245620, name: "Elden Ring", url: "https://cdn.cloudflare.steamstatic.com/steam/apps/1245620/library_600x900_2x.jpg" },
  { appid: 1145360, name: "Hades", url: "https://cdn.cloudflare.steamstatic.com/steam/apps/1145360/library_600x900_2x.jpg" },
  { appid: 1332010, name: "Stray", url: "https://cdn.cloudflare.steamstatic.com/steam/apps/1332010/library_600x900_2x.jpg" },
];

function handFocusToInstallerWindow() {
  // Let the Wine-hosted installer (Steam setup) take the foreground
  // instead of quietly opening behind the wizard.
  void getAPI().blurMainWindow?.();
}

function reclaimFocusFromInstaller() {
  void getAPI().focusMainWindow?.();
}

function goToRuntimeStep() {
  step.value = 1;
}
const installButtonLabel = computed(() => {
  if (installStatus.value === "complete") return t("setup.installComplete");
  if (installing.value) return installCurrent.value || "Preparing…";
  if (installFailed.value) return t("setup.installFailed");
  return t("setup.installRuntime");
});
async function startInstall() {
  installing.value = true;
  installLogs.value = [];
  installProgress.value = 0;
  installFailed.value = false;
  installCurrent.value = "";

  const started = await api<{ ok: boolean; error?: string }>("POST", "/setup/install-all");
  if (!started?.ok) {
    toast.show(started?.error ?? t("setup.installFailed"), "error");
    installing.value = false;
    return;
  }

  installLogs.value.push({ text: "Starting installation...", cls: "info" });

  let lastStep = -1;
  let lastStatusText = "";

  const poll = setInterval(async () => {
    const progress = await api<{
      step: number;
      total: number;
      current: string;
      status: string;
      log: string;
      error: string | null;
    }>("GET", "/setup/install-progress");
    if (!progress) return;

    const pct = progress.total > 0 ? Math.round((progress.step / progress.total) * 100) : 0;
    installProgress.value = pct;
    if (progress.current) installCurrent.value = progress.current;

    if (progress.step !== lastStep || progress.status !== lastStatusText) {
      if (progress.status === "done" || progress.status === "skipped") {
        installLogs.value.push({ text: progress.log, cls: progress.status === "done" ? "success" : "warn" });
      } else if (progress.status === "error") {
        installLogs.value.push({ text: progress.log, cls: "error" });
        if (progress.error) installLogs.value.push({ text: `Error: ${progress.error}`, cls: "error" });
        clearInterval(poll);
        installing.value = false;
        installFailed.value = true;
        logOpen.value = true;
        return;
      } else if (progress.status === "installing" && progress.step !== lastStep) {
        installLogs.value.push({ text: progress.log, cls: "active" });
      } else if (progress.status === "complete") {
        installLogs.value.push({ text: t("setup.installComplete"), cls: "success" });
        clearInterval(poll);
        installProgress.value = 100;
        installing.value = false;
        installStatus.value = "complete";
        toast.show(t("setup.installComplete"), "success");
        checkSteam();
      }
    }

    lastStep = progress.step;
    lastStatusText = progress.status;
  }, 500);
}

async function checkSteam() {
  const s = await api<{ installed: boolean; running: boolean; installing?: boolean; install_stage?: string }>(
    "GET",
    "/steam/status",
  );
  steamInstalled.value = s?.installed === true && s?.installing !== true;
  steamInstalling.value = s?.installing === true;
  if (s?.install_stage) steamInstallStage.value = s.install_stage;
  return s;
}

function steamInstallLabel() {
  switch (steamInstallStage.value) {
    case "downloading":
      return t("setup.downloadingSteam");
    case "creating-steam-prefix":
      return t("setup.creatingSteamPrefix");
    case "installing-steam":
      return t("setup.installingSteam");
    case "failed":
      return t("setup.retrySteam");
    default:
      return steamInstalling.value ? t("setup.preparingSteam") : t("setup.installSteam");
  }
}

async function installSteam() {
  if (steamInstalling.value || steamInstalled.value) return;
  steamFailed.value = false;
  steamInstalling.value = true;
  steamInstallStage.value = "downloading";
  const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/install");
  if (!result?.ok) {
    steamInstalling.value = false;
    steamFailed.value = true;
    steamInstallStage.value = "failed";
    toast.show(result?.error ?? t("setup.steamInstallFailed"), "error");
    return;
  }
  handFocusToInstallerWindow();

  const startedAt = Date.now();
  const poll = setInterval(async () => {
    const s = await checkSteam();
    if (s?.installed && !s.installing) {
      clearInterval(poll);
      steamInstalled.value = true;
      steamInstalling.value = false;
      steamInstallStage.value = "complete";
      reclaimFocusFromInstaller();
      toast.show(t("setup.steamInstalled"), "success");
    } else if (Date.now() - startedAt > 300000) {
      clearInterval(poll);
      steamInstalling.value = false;
      steamFailed.value = true;
      steamInstallStage.value = "failed";
      reclaimFocusFromInstaller();
      toast.show(t("setup.steamInstallTimedOut"), "error");
    }
  }, 1000);
}

async function goToDoneStep() {
  step.value = steps.value.length - 1; /* Done page: last page, whatever the count */
  const gen = await api<{ name: string }>("GET", "/setup/device-name");
  if (gen?.name) deviceName.value = gen.name;
}

async function finish() {
  if (finishing.value) return;
  finishing.value = true;
  try {
    const keyInput = document.getElementById("setup-api-key") as HTMLInputElement;
    const nameInput = document.getElementById("setup-device-name") as HTMLInputElement;
    const name = nameInput?.value?.trim() || deviceName.value;
    const key = keyInput?.value?.trim();

    const wrappers = await api<{ ok: boolean; error?: string }>("POST", "/steam/ensure-launch-ready");
    if (!wrappers?.ok) {
      toast.show(
        wrappers?.error ?? t("setup.wrapperWarning"),
        "error",
      );
    }

    await api("POST", "/setup/save", { step: 2, deviceName: name, completed: true });
    if (key) {
      const result = await api<{
        ok: boolean;
        error?: string;
        library?: { ok: boolean; total: number; installed_count: number; games: unknown[] };
        sync?: { steam_id_detected: boolean };
      }>("POST", "/steam/save-api-key", { key });
      if (!result?.ok) {
        toast.show(result?.error ?? t("setup.apiKeySaveFailed"), "error");
        return;
      }
      steamApiKey.value = key;
      if (result.library) library.value = result.library;
      if (result.sync && !result.sync.steam_id_detected) {
        toast.show(t("setup.apiKeySteamIdMissing"), "error");
      }
    }
    emit("done");
  } finally {
    finishing.value = false;
  }
}



</script>

<template>
  <div class="setup-overlay">
    <div class="setup-wizard">
      <div class="setup-visual">
        <div class="setup-visual-art" aria-hidden="true"></div>
        <div class="setup-visual-vignette" aria-hidden="true"></div>
        <img class="setup-visual-logo" src="../assets/setup-hero-logo.png" alt="MetalSharp" />
        <div class="setup-covers" aria-hidden="true">
          <img v-for="(cover, i) in showcaseCovers" :key="cover.appid" :src="cover.url" :alt="cover.name" loading="lazy" />
        </div>
        <div class="setup-steps">
          <template v-for="(s, i) in steps" :key="i">
            <div class="setup-step-item" :class="{ done: i < step, current: i === step }">
              <div class="setup-step-dot" :class="{ done: i < step, current: i === step }"></div>
              <span class="setup-step-label" :class="{ current: i === step }">{{ s }}</span>
            </div>
            <div v-if="i < steps.length - 1" class="setup-step-line" :class="{ done: i < step }"></div>
          </template>
        </div>
      </div>

      <div class="setup-pane">
        <LanguagePicker class="setup-language-picker" />
        <button v-if="props.dismissible" class="setup-wizard-close" type="button" aria-label="Exit setup" title="Exit setup" @click="emit('close')">
          ✕
        </button>
        <div class="setup-pane-scroll">
          <div class="setup-eyebrow">{{ t("setup.stepOf", { step: step + 1, total: steps.length }) }}</div>
          <h1 class="setup-title">{{ stepTitles[step] }}</h1>
          <p class="setup-tagline">{{ stepTaglines[step] }}</p>

          <div v-if="step === 0" class="setup-body">
            <p class="setup-lede">{{ t("setup.lede") }}</p>
            <div class="setup-features">
              <div class="setup-feature">
                <div class="setup-feature-icon"><IconZap width="20" height="20" /></div>
                <div>
                  <div class="setup-feature-title">{{ t("setup.features.directx") }}</div>
                  <div class="setup-feature-desc">{{ t("setup.features.directxDesc") }}</div>
                </div>
              </div>
              <div class="setup-feature">
                <div class="setup-feature-icon"><IconMonitor width="20" height="20" /></div>
                <div>
                  <div class="setup-feature-title">{{ t("setup.features.fna") }}</div>
                  <div class="setup-feature-desc">{{ t("setup.features.fnaDesc") }}</div>
                </div>
              </div>
              <div class="setup-feature">
                <div class="setup-feature-icon"><IconGamepad2 width="20" height="20" /></div>
                <div>
                  <div class="setup-feature-title">{{ t("setup.features.steam") }}</div>
                  <div class="setup-feature-desc">{{ t("setup.features.steamDesc") }}</div>
                </div>
              </div>
            </div>
          </div>

          <div v-if="step === 1" class="setup-body">
            <p class="setup-lede">{{ t("setup.runtimeLede") }}</p>

            <div class="setup-tools-label">{{ t("setup.bundledTools") }}</div>
            <div class="setup-tool-list">
              <div class="setup-tool-row"><strong>zstd / unzstd</strong><span>{{ t("setup.toolExtraction") }}</span></div>
              <div class="setup-tool-row"><strong>unrar</strong><span>{{ t("setup.toolRar") }}</span></div>
              <div class="setup-tool-row"><strong>wrestool / icotool</strong><span>{{ t("setup.toolIcons") }}</span></div>
              <div class="setup-tool-row"><strong>lsar / unar</strong><span>{{ t("setup.toolArchives") }}</span></div>
            </div>

            <div class="setup-install-grid">
              <div class="setup-install-col">
                <button
                  class="setup-btn primary install-btn"
                  :class="{ working: installing, done: installStatus === 'complete' }"
                  :disabled="installing || installStatus === 'complete'"
                  @click="startInstall"
                >
                  <span v-if="installing" class="install-btn-progress" :style="{ width: installProgress + '%' }"></span>
                  <span class="install-btn-label">
                    <IconLoader2 v-if="installing" class="install-spinner" width="16" height="16" />
                    <IconCheck v-else-if="installStatus === 'complete'" width="17" height="17" />
                    {{ installButtonLabel }}
                  </span>
                </button>
                <button
                  v-if="installLogs.length"
                  class="setup-btn ghost install-log-btn"
                  :class="{ active: logOpen }"
                  title="View install log"
                  @click="logOpen = !logOpen"
                >
                  <IconScrollText width="14" height="14" />
                  {{ t("setup.installLog") }}
                </button>
              </div>
              <div class="setup-install-col">
                <button
                  class="setup-btn primary install-btn"
                  :class="{ working: steamInstalling, done: steamInstalled }"
                  :disabled="!runtimeReady || steamInstalling || steamInstalled"
                  @click="installSteam"
                >
                  <span v-if="steamInstalling" class="install-btn-progress indeterminate"></span>
                  <span class="install-btn-label">
                    <IconLoader2 v-if="steamInstalling" class="install-spinner" width="16" height="16" />
                    <IconCheck v-else-if="steamInstalled" width="17" height="17" />
                    {{ steamButtonLabel }}
                  </span>
                </button>
                <div v-if="!steamInstalled" class="setup-steam-hint">{{ t("setup.startSteamHint") }}</div>
              </div>
            </div>
            <div v-if="logOpen && installLogs.length" class="setup-log">
              <div v-for="(log, i) in installLogs" :key="i" class="setup-log-line" :class="log.cls">
                {{ log.text }}
              </div>
            </div>
          </div>

          <div v-if="step === 2" class="setup-body">
            <div class="setup-complete-icon"><IconCheck width="30" height="30" /></div>
            <div class="setup-form">
              <div class="setup-form-group">
                <label class="setup-label">{{ t("setup.deviceName") }}</label>
                <input id="setup-device-name" type="text" :value="deviceName" :placeholder="t('setup.devicePlaceholder')" class="setup-input" />
                <div class="setup-hint">{{ t("setup.deviceHint") }}</div>
              </div>
              <div class="setup-form-group">
                <label class="setup-label">{{ t("setup.apiKey") }}</label>
                <input id="setup-api-key" type="password" :placeholder="t('setup.apiPlaceholder')" class="setup-input" />
                <div class="setup-hint">{{ t("setup.apiHint") }}
                  <a href="https://steamcommunity.com/dev/apikey" target="_blank">steamcommunity.com/dev/apikey</a>
                </div>
              </div>
            </div>
            <div class="setup-tips">
              <div class="setup-tip"><strong>{{ t("setup.startSteam") }}</strong> — {{ t("setup.startSteamText") }}</div>
              <div class="setup-tip"><strong>{{ t("setup.firstLaunch") }}</strong> — {{ t("setup.firstLaunchText") }}</div>
            </div>
          </div>
        </div>

        <div class="setup-actions">
          <button v-if="step > 0 && step < 2" class="setup-btn ghost" @click="step = step - 1">{{ t("actions.back") }}</button>
          <button v-if="step === 0" class="setup-btn primary" @click="step = 1">
            <IconPlay width="16" height="16" fill="currentColor" /> {{ t("setup.getStarted") }}
          </button>
          <template v-else-if="step === 1">
            <button class="setup-btn primary" type="button" @click="goToDoneStep">{{ t("actions.next") }}</button>
          </template>
          <button v-else class="setup-btn primary" :disabled="finishing" @click="finish">
            {{ finishing ? t("setup.preparingSteam") : t("setup.launch") }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.setup-overlay {
  position: fixed;
  inset: 0;
  z-index: 1000;
  display: flex;
  align-items: stretch;
  justify-content: center;
  padding: clamp(10px, 2.5vh, 24px) clamp(10px, 2.5vw, 36px);
  background: #08090c;
}
.setup-wizard {
  flex: 1;
  display: flex;
  overflow: hidden;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 18px;
  background: #101316;
  box-shadow: 0 40px 120px rgba(0, 0, 0, 0.55);
  font-family: var(--font-rethink);
  isolation: isolate;
}

/* ---- left visual ---- */
.setup-visual {
  position: relative;
  flex: 1 1 52%;
  min-width: 0;
  display: flex;
  flex-direction: column;
  justify-content: flex-end;
  overflow: hidden;
}
.setup-visual-art {
  position: absolute;
  inset: 0;
  background:
    radial-gradient(95% 58% at 52% 76%, rgba(255, 166, 77, 0.5), rgba(255, 120, 40, 0.16) 46%, transparent 72%),
    radial-gradient(70% 45% at 22% 60%, rgba(96, 142, 92, 0.14), transparent 65%),
    radial-gradient(60% 40% at 80% 30%, rgba(64, 90, 120, 0.18), transparent 70%),
    linear-gradient(180deg, #131b25 0%, #0f151c 55%, #0b0e12 100%);
}
.setup-visual-art::after {
  content: "";
  position: absolute;
  inset: 0;
  background:
    radial-gradient(38% 26% at 52% 66%, rgba(255, 214, 150, 0.2), transparent 70%),
    linear-gradient(180deg, transparent 60%, rgba(8, 10, 13, 0.55) 100%);
}
.setup-visual-vignette {
  position: absolute;
  inset: 0;
  background: linear-gradient(90deg, rgba(9, 11, 14, 0.35), transparent 30%, transparent 70%, rgba(9, 11, 14, 0.5));
  pointer-events: none;
}
.setup-visual-logo {
  position: absolute;
  z-index: 1;
  left: 50%;
  top: 34%;
  width: clamp(280px, 36vw, 470px);
  transform: translate(-50%, -50%);
  filter: drop-shadow(0 24px 60px rgba(0, 0, 0, 0.55)) drop-shadow(0 0 80px rgba(255, 176, 92, 0.18));
  pointer-events: none;
  user-select: none;
}
.setup-covers {
  position: relative;
  z-index: 2;
  display: flex;
  align-items: flex-end;
  justify-content: center;
  gap: clamp(10px, 1.4vw, 18px);
  padding: 0 40px 84px;
  perspective: 900px;
}
.setup-covers img {
  width: clamp(80px, 8.5vw, 124px);
  aspect-ratio: 0.7;
  object-fit: cover;
  border-radius: 6px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  box-shadow: 0 26px 55px rgba(0, 0, 0, 0.55);
  transition: transform 0.3s ease;
}
.setup-covers img:nth-child(1) { transform: perspective(900px) rotateY(9deg) translateY(5px); }
.setup-covers img:nth-child(2) { transform: perspective(900px) translateY(-16px) scale(1.13); z-index: 2; }
.setup-covers img:nth-child(3) { transform: perspective(900px) rotateY(-6deg) translateY(-5px); }
.setup-covers img:nth-child(4) { transform: perspective(900px) rotateY(-9deg) translateY(7px); }

.setup-steps {
  position: relative;
  z-index: 2;
  display: flex;
  align-items: flex-start;
  padding: 0 40px 26px;
}
.setup-step-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  flex: 0 0 auto;
}
.setup-step-dot {
  width: 13px;
  height: 13px;
  border-radius: 50%;
  border: 1.5px solid rgba(255, 255, 255, 0.35);
  background: transparent;
  transition: all var(--transition);
}
.setup-step-dot.done {
  border-color: transparent;
  background: rgba(239, 230, 211, 0.55);
}
.setup-step-dot.current {
  border-color: #efe6d3;
  background: #efe6d3;
  box-shadow: 0 0 12px rgba(239, 230, 211, 0.45);
}
.setup-step-label {
  color: rgba(255, 255, 255, 0.55);
  font-size: 11px;
}
.setup-step-label.current {
  color: #fff;
  font-weight: 650;
}
.setup-step-line {
  flex: 1;
  height: 1px;
  margin-top: 6px;
  background: rgba(255, 255, 255, 0.18);
}
.setup-step-line.done {
  background: rgba(239, 230, 211, 0.5);
}

/* ---- right pane ---- */
.setup-language-picker {
  position: absolute;
  top: 18px;
  left: clamp(26px, 4vw, 68px);
  z-index: 2;
}
.setup-pane {
  position: relative;
  flex: 1 1 48%;
  min-width: 0;
  display: flex;
  flex-direction: column;
  padding: clamp(26px, 4.5vw, 60px) clamp(26px, 4vw, 68px) clamp(20px, 3vw, 34px);
  background: linear-gradient(180deg, #171a1e 0%, #131518 100%);
  border-left: 1px solid rgba(255, 255, 255, 0.06);
}
.setup-pane-scroll {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  scrollbar-width: thin;
  scrollbar-color: rgba(255, 255, 255, 0.16) transparent;
}
.setup-wizard-close {
  position: absolute;
  top: 18px;
  right: 18px;
  z-index: 2;
  display: grid;
  place-items: center;
  width: 30px;
  height: 30px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 8px;
  color: rgba(255, 255, 255, 0.6);
  background: transparent;
  font-size: 13px;
  cursor: pointer;
}
.setup-wizard-close:hover {
  border-color: rgba(255, 255, 255, 0.32);
  color: #fff;
}
.setup-eyebrow {
  color: rgba(240, 239, 231, 0.5);
  font-size: 11px;
  font-weight: 650;
  letter-spacing: 0.42em;
}
.setup-title {
  margin: 18px 0 12px;
  color: #f2efe6;
  font-family: Georgia, "Times New Roman", serif;
  font-size: clamp(32px, 3.4vw, 54px);
  font-weight: 500;
  line-height: 1.02;
}
.setup-tagline {
  margin: 0;
  color: #e9e7e0;
  font-size: clamp(16px, 1.4vw, 21px);
}
.setup-lede {
  margin: 18px 0 22px;
  color: #9aa09e;
  font-size: 13.5px;
  line-height: 1.5;
}
.setup-body {
  margin-top: 8px;
}

/* features (welcome) */
.setup-features {
  display: flex;
  flex-direction: column;
  margin-top: 14px;
}
.setup-feature {
  display: flex;
  align-items: center;
  gap: 18px;
  padding: 19px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.07);
}
.setup-feature:first-child {
  border-top: 0;
}
.setup-feature-icon {
  display: grid;
  flex: 0 0 auto;
  place-items: center;
  width: 48px;
  height: 48px;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 11px;
  color: #d8d5cc;
  background: rgba(255, 255, 255, 0.03);
}
.setup-feature-title {
  color: #eceae3;
  font-size: 15.5px;
  font-weight: 650;
}
.setup-feature-desc {
  margin-top: 3px;
  color: #9aa09e;
  font-size: 13px;
}

/* bundled tools */
.setup-tools-label {
  margin: 6px 0 10px;
  color: rgba(240, 239, 231, 0.5);
  font-size: 10.5px;
  font-weight: 700;
  letter-spacing: 0.28em;
  text-transform: uppercase;
}
.setup-tool-list {
  display: flex;
  flex-direction: column;
  margin-bottom: 26px;
}
.setup-tool-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 15px 0;
  border-top: 1px solid rgba(255, 255, 255, 0.07);
  color: #eceae3;
  font-size: 14px;
}
.setup-tool-row:first-child {
  border-top: 0;
}
.setup-tool-row span {
  color: #9aa09e;
  font-size: 12.5px;
  text-align: right;
}

/* runtime install */
.setup-install-grid {
  display: flex;
  align-items: stretch;
  gap: 12px;
}
.setup-install-col {
  flex: 1 1 0;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 10px;
}
.install-btn {
  position: relative;
  width: 100%;
  min-width: 0;
  overflow: hidden;
}
.install-btn.working {
  cursor: progress;
}
.install-btn.done {
  background: #3d9a58;
  color: #fff;
}
.install-btn:disabled:not(.done) {
  opacity: 0.4;
}
.install-btn-progress.indeterminate {
  width: 100%;
  animation: setup-indeterminate 1.2s ease-in-out infinite;
  background: linear-gradient(90deg, transparent, rgba(20, 22, 26, 0.22), transparent);
}
@keyframes setup-indeterminate {
  0% {
    transform: translateX(-100%);
  }
  100% {
    transform: translateX(100%);
  }
}

.install-btn-progress {
  position: absolute;
  top: 0;
  bottom: 0;
  left: 0;
  width: 0%;
  background: rgba(20, 22, 26, 0.18);
  transition: width 0.3s ease;
}
.install-btn-label {
  position: relative;
  z-index: 1;
  display: inline-flex;
  align-items: center;
  gap: 10px;
  white-space: nowrap;
}
.install-spinner {
  animation: setup-spin 0.9s linear infinite;
}
@keyframes setup-spin {
  to {
    transform: rotate(360deg);
  }
}
.install-log-btn {
  min-height: 38px;
  padding: 0 14px;
  border-radius: 8px;
  font-size: 12px;
  justify-content: center;
}
.install-log-btn.active {
  border-color: #efe6d3;
  color: #efe6d3;
}
.setup-log {
  max-height: 220px;
  margin-top: 14px;
  padding: 12px 14px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 10px;
  background: #0c0f11;
  overflow-y: auto;
  font-family: "SF Mono", ui-monospace, Menlo, monospace;
  font-size: 11.5px;
  line-height: 1.7;
}
.setup-log-line.info { color: #c9ceca; }
.setup-log-line.active { color: #efe6d3; }
.setup-log-line.success { color: #7cbf6a; }
.setup-log-line.warn { color: #ffb84d; }
.setup-log-line.error { color: #ff5c5c; }
.setup-steam-section {
  margin-top: 26px;
  padding-top: 20px;
  border-top: 1px solid rgba(255, 255, 255, 0.07);
}
.setup-steam-section h2 {
  margin: 0 0 6px;
  color: #eceae3;
  font-size: 17px;
}
.setup-steam-section p {
  margin: 0 0 14px;
  color: #9aa09e;
  font-size: 13px;
}
.setup-steam-install-status {
  margin-top: 10px;
  color: #ffb84d;
  font-size: 12.5px;
}

/* done */
.setup-complete-icon {
  display: grid;
  place-items: center;
  width: 54px;
  height: 54px;
  margin-bottom: 18px;
  border: 1px solid rgba(124, 191, 106, 0.4);
  border-radius: 50%;
  color: #7cbf6a;
  background: rgba(124, 191, 106, 0.08);
}
.setup-form {
  display: flex;
  flex-direction: column;
  gap: 16px;
  margin-top: 20px;
}
.setup-form-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.setup-label {
  color: #cfd2cf;
  font-size: 12px;
  font-weight: 650;
}
.setup-input {
  min-height: 40px;
  padding: 0 12px;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 8px;
  color: #eceae3;
  background: rgba(255, 255, 255, 0.04);
  font: inherit;
  font-size: 13px;
  outline: none;
}
.setup-input:focus {
  border-color: #efe6d3;
}
.setup-hint {
  color: #838987;
  font-size: 11.5px;
}
.setup-hint a {
  color: #efe6d3;
}
.setup-tips {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-top: 24px;
}
.setup-tip {
  padding: 12px 14px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 10px;
  color: #9aa09e;
  font-size: 12.5px;
  line-height: 1.5;
}
.setup-tip strong {
  color: #d8d5cc;
}

/* actions */
.setup-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 10px;
  padding-top: 22px;
}
.setup-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
  border: 0;
  cursor: pointer;
  font: inherit;
  transition: transform 0.16s ease, filter 0.16s ease, border-color 0.16s ease;
}
.setup-btn:disabled {
  opacity: 0.45;
  cursor: default;
  transform: none;
}
.setup-btn.primary {
  min-height: 52px;
  padding: 0 32px;
  border-radius: 10px;
  color: #14161a;
  background: #efe7d6;
  font-size: 16px;
  font-weight: 700;
}
.setup-btn.primary.sm {
  min-height: 36px;
  padding: 0 16px;
  border-radius: 7px;
  font-size: 12.5px;
}
.setup-btn.primary:hover:not(:disabled) {
  transform: translateY(-1px);
  filter: brightness(1.05);
}
.setup-btn.ghost {
  margin-right: auto;
  min-height: 44px;
  padding: 0 18px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 9px;
  color: #cfd2cf;
  background: transparent;
  font-size: 13px;
}
.setup-btn.ghost:hover:not(:disabled) {
  border-color: rgba(255, 255, 255, 0.32);
}

@media (max-width: 900px) {
  .setup-visual {
    display: none;
  }
}
</style>
