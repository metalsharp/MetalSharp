<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { useI18n } from "vue-i18n";
import { api } from "../composables/useApi";
import { useToast } from "../composables/useToast";
import IconSettings from "~icons/lucide/settings";
import IconX from "~icons/lucide/x";

interface LaunchConfig {
  ok: boolean;
  error?: string;
  controllerInput?: "off" | "x" | "d";
  msync?: boolean;
  windowMode?: "default" | "windowed" | "fullscreen";
  gameResolution?: "default" | "1280x720" | "1920x1080" | "2560x1440" | "3840x2160";
}

interface MetalFxState {
  ok: boolean;
  enabled?: boolean;
  factor?: number;
}

const { t } = useI18n();
const toast = useToast();
const open = ref(false);
const loading = ref(false);
const configBusy = ref(false);
const metalFxBusy = ref(false);
const controllerInput = ref<"off" | "x" | "d">("off");
const msync = ref(true);
const windowMode = ref<"default" | "windowed" | "fullscreen">("default");
const resolution = ref<"default" | "1280x720" | "1920x1080" | "2560x1440" | "3840x2160">("default");
const metalFxMode = ref<"off" | "1.75" | "2.0">("2.0");
const disabled = computed(() => loading.value || configBusy.value || metalFxBusy.value);

async function refreshSettings() {
  loading.value = true;
  const [configResult, metalFxResult] = await Promise.all([
    api<LaunchConfig>("GET", "/config"),
    api<MetalFxState>("GET", "/metalfx/state"),
  ]);
  if (configResult?.ok) {
    controllerInput.value = configResult.controllerInput ?? "off";
    msync.value = configResult.msync !== false;
    windowMode.value = configResult.windowMode ?? "default";
    resolution.value = configResult.gameResolution ?? "default";
  }
  if (metalFxResult?.ok) {
    metalFxMode.value =
      metalFxResult.enabled === false
        ? "off"
        : Math.abs((metalFxResult.factor ?? 2) - 1.75) < 0.01
          ? "1.75"
          : "2.0";
  }
  loading.value = false;
}

async function saveConfig(patch: Partial<LaunchConfig>, apply: () => void) {
  if (disabled.value) return;
  configBusy.value = true;
  const result = await api<LaunchConfig>("POST", "/config", patch);
  if (result?.ok) apply();
  else toast.show(result?.error ?? "Could not save game launch settings", "error");
  configBusy.value = false;
}

async function setControllerInput(value: "off" | "x" | "d") {
  await saveConfig({ controllerInput: value }, () => (controllerInput.value = value));
}

async function setMsync(value: boolean) {
  await saveConfig({ msync: value }, () => (msync.value = value));
}

async function setWindowMode(value: "default" | "windowed" | "fullscreen") {
  await saveConfig({ windowMode: value }, () => (windowMode.value = value));
}

async function setResolution(value: "default" | "1280x720" | "1920x1080" | "2560x1440" | "3840x2160") {
  await saveConfig({ gameResolution: value }, () => (resolution.value = value));
}

async function setMetalFx(value: "off" | "1.75" | "2.0") {
  if (disabled.value) return;
  metalFxBusy.value = true;
  const result = await api<{ ok: boolean; error?: string }>(
    "POST",
    "/metalfx/toggle",
    value === "off" ? { enabled: false } : { enabled: true, factor: Number(value) },
  );
  if (result?.ok) metalFxMode.value = value;
  else toast.show(result?.error ?? "Could not save MetalFX setting", "error");
  metalFxBusy.value = false;
}

function onDocumentPointer(event: PointerEvent) {
  if (open.value && event.target instanceof Node && !root.value?.contains(event.target)) open.value = false;
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === "Escape") open.value = false;
}

const root = ref<HTMLElement | null>(null);
watch(open, (value) => {
  if (value) void refreshSettings();
});
onMounted(() => {
  document.addEventListener("pointerdown", onDocumentPointer);
  window.addEventListener("keydown", onKeydown);
});
onUnmounted(() => {
  document.removeEventListener("pointerdown", onDocumentPointer);
  window.removeEventListener("keydown", onKeydown);
});
</script>

<template>
  <div ref="root" class="launch-settings-control">
    <button
      class="launch-settings-button"
      type="button"
      :aria-label="t('ui.game.title')"
      :aria-expanded="open"
      :title="t('ui.game.title')"
      @click="open = !open"
    >
      <IconSettings width="18" height="18" />
    </button>
    <section v-if="open" class="launch-settings-popover" :aria-label="t('ui.game.title')" @click.stop>
      <header class="launch-settings-header">
        <div>
          <small>{{ t('ui.game.title') }}</small>
          <strong>{{ t('ui.gameLaunchSettings.subtitle') }}</strong>
        </div>
        <button class="launch-settings-close" type="button" :aria-label="t('ui.gameLaunchSettings.close')" @click="open = false">
          <IconX width="16" height="16" />
        </button>
      </header>
      <p v-if="loading" class="launch-settings-hint">{{ t('ui.sharp.checking') }}</p>
      <template v-else>
        <div class="launch-settings-row">
          <span>{{ t('ui.game.metalFx') }}</span>
          <div class="launch-settings-options">
            <button type="button" :disabled="disabled" :class="{ active: metalFxMode === '1.75' }" @click="setMetalFx('1.75')">1.75×</button>
            <button type="button" :disabled="disabled" :class="{ active: metalFxMode === '2.0' }" @click="setMetalFx('2.0')">2×</button>
            <button type="button" :disabled="disabled" :class="{ active: metalFxMode === 'off' }" @click="setMetalFx('off')">{{ t('ui.game.off') }}</button>
          </div>
        </div>
        <div class="launch-settings-row">
          <span>{{ t('ui.game.controllerInput') }}</span>
          <div class="launch-settings-options">
            <button type="button" :disabled="disabled" :class="{ active: controllerInput === 'off' }" @click="setControllerInput('off')">{{ t('ui.game.off') }}</button>
            <button type="button" :disabled="disabled" :class="{ active: controllerInput === 'x' }" @click="setControllerInput('x')">XInput</button>
            <button type="button" :disabled="disabled" :class="{ active: controllerInput === 'd' }" @click="setControllerInput('d')">DInput</button>
          </div>
        </div>
        <div class="launch-settings-row launch-settings-toggle">
          <span>{{ t('ui.game.msync') }}</span>
          <button type="button" :disabled="disabled" :class="{ active: msync }" @click="setMsync(!msync)">{{ msync ? t('ui.game.on') : t('ui.game.off') }}</button>
        </div>
        <div class="launch-settings-row">
          <span>{{ t('ui.gameLaunchSettings.displayMode') }}</span>
          <div class="launch-settings-options">
            <button type="button" :disabled="disabled" :class="{ active: windowMode === 'default' }" @click="setWindowMode('default')">{{ t('ui.gameLaunchSettings.gameDefault') }}</button>
            <button type="button" :disabled="disabled" :class="{ active: windowMode === 'windowed' }" @click="setWindowMode('windowed')">{{ t('ui.gameLaunchSettings.windowed') }}</button>
            <button type="button" :disabled="disabled" :class="{ active: windowMode === 'fullscreen' }" @click="setWindowMode('fullscreen')">{{ t('ui.gameLaunchSettings.fullscreen') }}</button>
          </div>
        </div>
        <label class="launch-settings-row launch-settings-resolution">
          <span>{{ t('ui.gameLaunchSettings.resolution') }}</span>
          <select :value="resolution" :disabled="disabled" @change="setResolution(($event.target as HTMLSelectElement).value as typeof resolution)">
            <option value="default">{{ t('ui.gameLaunchSettings.gameDefault') }}</option>
            <option value="1280x720">720p — 1280 × 720</option>
            <option value="1920x1080">1080p — 1920 × 1080</option>
            <option value="2560x1440">1440p — 2560 × 1440</option>
            <option value="3840x2160">2160p / 4K — 3840 × 2160</option>
          </select>
        </label>
      </template>
      <p class="launch-settings-hint">{{ t('ui.gameLaunchSettings.launchHint') }}</p>
    </section>
  </div>
</template>

<style scoped>
.launch-settings-control {
  position: relative;
  z-index: 90;
  flex: 0 0 auto;
}
.launch-settings-button {
  display: grid;
  width: 42px;
  height: 42px;
  place-items: center;
  border: 1px solid var(--library-control-border, rgba(255, 255, 255, 0.14));
  border-radius: 10px;
  background: var(--library-control-bg, rgba(20, 24, 28, 0.82));
  color: var(--library-control-text, #fff);
  cursor: pointer;
}
.launch-settings-button:hover,
.launch-settings-button[aria-expanded="true"] {
  border-color: var(--library-accent, #7ecbff);
  color: var(--library-accent, #7ecbff);
}
.launch-settings-popover {
  position: absolute;
  top: calc(100% + 10px);
  right: 0;
  z-index: 100;
  display: grid;
  width: min(390px, calc(100vw - 28px));
  gap: 12px;
  padding: 16px;
  border: 1px solid var(--library-control-border, rgba(255, 255, 255, 0.18));
  border-radius: 14px;
  background: var(--library-control-panel, #171b20);
  color: var(--library-control-text, #f4f6f8);
  box-shadow: 0 18px 48px rgba(0, 0, 0, 0.46);
}
.launch-settings-header,
.launch-settings-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 10px;
}
.launch-settings-header > div,
.launch-settings-row > span {
  display: grid;
  gap: 4px;
}
.launch-settings-header small,
.launch-settings-row > span {
  color: var(--library-control-muted, #aab3bc);
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.06em;
  text-transform: uppercase;
}
.launch-settings-header strong {
  font-size: 14px;
}
.launch-settings-close {
  display: grid;
  width: 30px;
  height: 30px;
  place-items: center;
  border: 0;
  border-radius: 8px;
  background: transparent;
  color: inherit;
  cursor: pointer;
}
.launch-settings-close:hover {
  background: rgba(255, 255, 255, 0.09);
}
.launch-settings-options {
  display: flex;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 4px;
}
.launch-settings-options button,
.launch-settings-toggle > button {
  min-height: 30px;
  padding: 5px 8px;
  border: 1px solid var(--library-control-border, rgba(255, 255, 255, 0.14));
  border-radius: 7px;
  background: transparent;
  color: inherit;
  cursor: pointer;
  font-size: 11px;
}
.launch-settings-options button.active,
.launch-settings-toggle > button.active {
  border-color: var(--library-accent, #7ecbff);
  background: color-mix(in srgb, var(--library-accent, #7ecbff) 16%, transparent);
  color: var(--library-accent, #7ecbff);
}
.launch-settings-options button:disabled,
.launch-settings-toggle > button:disabled,
.launch-settings-resolution select:disabled {
  cursor: wait;
  opacity: 0.55;
}
.launch-settings-resolution select {
  max-width: 190px;
  padding: 8px 10px;
  border: 1px solid var(--library-control-border, rgba(255, 255, 255, 0.14));
  border-radius: 8px;
  background: var(--library-control-bg, rgba(20, 24, 28, 0.82));
  color: inherit;
  font-size: 12px;
}
.launch-settings-hint {
  margin: 0;
  color: var(--library-control-muted, #aab3bc);
  font-size: 11px;
  line-height: 1.45;
}
</style>
