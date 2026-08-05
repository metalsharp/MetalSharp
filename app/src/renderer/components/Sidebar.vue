<script setup lang="ts">
import { computed, nextTick, onMounted, ref, watch, type Component } from "vue";
import IconMenu from "~icons/lucide/menu";
import IconMoon from "~icons/lucide/moon";
import IconSun from "~icons/lucide/sun";
import IconSettings from "~icons/lucide/settings";
import IconBone from "~icons/lucide/bone";
import IconTreePine from "~icons/lucide/tree-pine";
import IconCitrus from "~icons/lucide/citrus";
import IconSparkles from "~icons/lucide/sparkles";
import IconBanana from "~icons/lucide/banana";
import IconFlame from "~icons/lucide/flame";
import IconTreePalm from "~icons/lucide/tree-palm";
import IconScanLine from "~icons/lucide/scan-line";
import IconGamepad from "~icons/lucide/gamepad-2";
import { themedNavIcon, type ThemeName } from "../composables/useTheme";
import { api } from "../composables/useApi";
import { useToast } from "../composables/useToast";

const props = defineProps<{
  currentView: string;
  theme: ThemeName;
}>();

const emit = defineEmits<{
  navigate: [view: string];
  selectTheme: [theme: ThemeName];
}>();

const toast = useToast();

const collapsed = ref(false);
const themePickerOpen = ref(false);
const themePickerRef = ref<HTMLElement | null>(null);

type ControllerInput = "off" | "x" | "d";
const controllerInput = ref<ControllerInput>("off");
const controllerInputBusy = ref(false);

// MetalFX Spatial upscaling (DXMT routes only: M10/M10(32)/M11/M11(32)).
// Drives the existing /metalfx/state + /metalfx/toggle overlay system
// (metalfx.overlay.json + dxmt.conf); the launcher reconciles the DXMT env
// from that state at launch.
type MetalFxMode = "1.75" | "1.50" | "off";
const metalFx = ref<MetalFxMode>("1.50");
const metalFxBusy = ref(false);

onMounted(async () => {
  const config = await api<AppConfig>("GET", "/config");
  if (config?.ok && (config.controllerInput === "x" || config.controllerInput === "d")) {
    controllerInput.value = config.controllerInput;
  }
  const state = await api<MetalFxState>("GET", "/metalfx/state");
  if (state?.ok) {
    metalFx.value = !state.enabled ? "off" : Math.abs((state.factor ?? 1.5) - 1.75) < 0.01 ? "1.75" : "1.50";
  }
});

async function setMetalFxMode(mode: MetalFxMode) {
  if (metalFxBusy.value || mode === metalFx.value) return;
  const previous = metalFx.value;
  metalFxBusy.value = true;
  metalFx.value = mode; // optimistic
  const body = mode === "off" ? { enabled: false } : { enabled: true, factor: mode === "1.75" ? 1.75 : 1.5 };
  const result = await api<MetalFxState>("POST", "/metalfx/toggle", body);
  if (result?.ok) {
    toast.show(
      mode === "off"
        ? "MetalFX disabled for DXMT routes — applies on next launch"
        : `MetalFX set to ${mode}× for DXMT routes — applies on next launch`,
      "success",
    );
  } else {
    metalFx.value = previous;
    toast.show("Failed to update MetalFX", "error");
  }
  metalFxBusy.value = false;
}

async function setControllerInput(mode: ControllerInput) {
  if (controllerInputBusy.value || mode === controllerInput.value) return;
  const previous = controllerInput.value;
  controllerInputBusy.value = true;
  controllerInput.value = mode; // optimistic
  const result = await api<AppConfig>("POST", "/config", { controllerInput: mode });
  if (result?.ok) {
    toast.show(
      mode === "off"
        ? "Controller input shims removed"
        : `Controller input shims set to ${mode === "x" ? "XInput" : "DInput"} — applied on next launch`,
      "success",
    );
  } else {
    controllerInput.value = previous;
    toast.show("Failed to update controller input", "error");
  }
  controllerInputBusy.value = false;
}

watch(themePickerOpen, async (open) => {
  if (!open) return;
  await nextTick();
  themePickerRef.value?.querySelector(".theme-picker-item.active")?.scrollIntoView({ block: "nearest" });
});

interface ThemeOption {
  name: ThemeName;
  label: string;
  icon: Component;
}

const themeOptions: ThemeOption[] = [
  { name: "dark", label: "Dark", icon: IconMoon },
  { name: "light", label: "Light", icon: IconSun },
  { name: "skeleton", label: "Skeleton", icon: IconBone },
  { name: "forest", label: "Forest", icon: IconTreePine },
  { name: "orange-peel", label: "Orange Peel", icon: IconCitrus },
  { name: "dragonfruit", label: "Dragonfruit", icon: IconSparkles },
  { name: "banana", label: "Banana", icon: IconBanana },
  { name: "lava", label: "Lava", icon: IconFlame },
  { name: "beach", label: "Beach", icon: IconTreePalm },
  { name: "xray", label: "Xray", icon: IconScanLine },
];

const currentThemeOption = computed(() => themeOptions.find((o) => o.name === props.theme) ?? themeOptions[0]);
const themeToggleLabel = computed(() => currentThemeOption.value.label);

function chooseTheme(name: ThemeName) {
  emit("selectTheme", name);
  themePickerOpen.value = false;
}

interface NavItem {
  view: string;
  label: string;
  icon: Component;
}

const navItems = computed<NavItem[]>(() => [
  { view: "library", label: "Library", icon: themedNavIcon("library") },
  { view: "sharp-library", label: "Sharp", icon: themedNavIcon("sharp") },
  { view: "logs", label: "Logs", icon: themedNavIcon("logs") },
]);
</script>

<template>
  <nav class="sidebar" :class="{ collapsed }">
    <div class="sidebar-top">
      <button class="sidebar-hamburger" @click="collapsed = !collapsed" title="Toggle sidebar">
        <IconMenu width="18" height="18" />
      </button>
      <div class="sidebar-logo">
        <img src="../icon.png" alt="M" class="sidebar-logo-icon" />
        <span v-if="!collapsed" class="sidebar-logo-text">MetalSharp</span>
      </div>
    </div>

    <div class="sidebar-nav">
      <button
        v-for="item in navItems"
        :key="item.view"
        class="sidebar-nav-item"
        :class="{ active: currentView === item.view }"
        @click="emit('navigate', item.view)"
        :title="collapsed ? item.label : undefined"
      >
        <component :is="item.icon" class="sidebar-nav-icon" width="18" height="18" />
        <span v-if="!collapsed" class="sidebar-nav-label">{{ item.label }}</span>
      </button>
    </div>

    <div class="sidebar-bottom">
      <div class="sidebar-input-selector" :title="collapsed ? 'MetalFX' : undefined">
        <div v-if="!collapsed" class="sidebar-input-label">
          <IconScanLine class="sidebar-input-icon" width="14" height="14" />
          <span>MetalFX</span>
        </div>
        <div class="sidebar-input-options" role="group" aria-label="MetalFX upscaling">
          <button
            class="sidebar-input-option"
            :class="{ active: metalFx === '1.75' }"
            :disabled="metalFxBusy"
            :aria-pressed="metalFx === '1.75'"
            :title="collapsed ? '1.75' : undefined"
            @click="setMetalFxMode('1.75')"
          >
            1.75
          </button>
          <button
            class="sidebar-input-option"
            :class="{ active: metalFx === '1.50' }"
            :disabled="metalFxBusy"
            :aria-pressed="metalFx === '1.50'"
            :title="collapsed ? '1.50' : undefined"
            @click="setMetalFxMode('1.50')"
          >
            1.50
          </button>
          <button
            class="sidebar-input-option"
            :class="{ active: metalFx === 'off' }"
            :disabled="metalFxBusy"
            :aria-pressed="metalFx === 'off'"
            :title="collapsed ? 'Off' : undefined"
            @click="setMetalFxMode('off')"
          >
            Off
          </button>
        </div>
      </div>
      <div class="sidebar-input-selector" :title="collapsed ? 'Controller input' : undefined">
        <div v-if="!collapsed" class="sidebar-input-label">
          <IconGamepad class="sidebar-input-icon" width="14" height="14" />
          <span>Controller</span>
        </div>
        <div class="sidebar-input-options" role="group" aria-label="Controller input shims">
          <button
            class="sidebar-input-option"
            :class="{ active: controllerInput === 'off' }"
            :disabled="controllerInputBusy"
            :aria-pressed="controllerInput === 'off'"
            :title="collapsed ? 'Off' : undefined"
            @click="setControllerInput('off')"
          >
            Off
          </button>
          <button
            class="sidebar-input-option"
            :class="{ active: controllerInput === 'x' }"
            :disabled="controllerInputBusy"
            :aria-pressed="controllerInput === 'x'"
            :title="collapsed ? 'XInput' : undefined"
            @click="setControllerInput('x')"
          >
            X
          </button>
          <button
            class="sidebar-input-option"
            :class="{ active: controllerInput === 'd' }"
            :disabled="controllerInputBusy"
            :aria-pressed="controllerInput === 'd'"
            :title="collapsed ? 'DInput' : undefined"
            @click="setControllerInput('d')"
          >
            D
          </button>
        </div>
      </div>
      <button
        class="sidebar-nav-item sidebar-theme-toggle"
        @click="themePickerOpen = !themePickerOpen"
        :title="collapsed ? themeToggleLabel : undefined"
      >
        <component :is="currentThemeOption.icon" class="sidebar-nav-icon" width="18" height="18" />
        <span v-if="!collapsed" class="sidebar-nav-label">{{ themeToggleLabel }}</span>
      </button>
      <Teleport to="body">
        <div v-if="themePickerOpen" class="theme-picker-backdrop" @click="themePickerOpen = false"></div>
        <div v-if="themePickerOpen" class="theme-picker-popover">
          <div class="theme-picker-header">Theme</div>
          <div ref="themePickerRef" class="theme-picker-list">
            <button
              v-for="option in themeOptions"
              :key="option.name"
              class="theme-picker-item"
              :class="{ active: option.name === theme }"
              @click="chooseTheme(option.name)"
            >
              <component :is="option.icon" class="theme-picker-icon" width="16" height="16" />
              <span class="theme-picker-label">{{ option.label }}</span>
            </button>
          </div>
        </div>
      </Teleport>
      <button
        class="sidebar-nav-item"
        :class="{ active: currentView === 'settings' }"
        @click="emit('navigate', 'settings')"
        :title="collapsed ? 'Settings' : undefined"
      >
        <IconSettings class="sidebar-nav-icon" width="18" height="18" />
        <span v-if="!collapsed" class="sidebar-nav-label">Settings</span>
      </button>
    </div>
  </nav>
</template>

<style scoped>
.sidebar {
  width: var(--sidebar-width-expanded);
  height: 100vh;
  min-height: 0;
  background-color: color-mix(in srgb, var(--bg-surface) 32%, transparent);
  backdrop-filter: blur(42px) saturate(210%) brightness(1.12);
  -webkit-backdrop-filter: blur(42px) saturate(210%) brightness(1.12);
  border-right: 1px solid color-mix(in srgb, white 18%, var(--border));
  box-shadow:
    inset 1px 0 rgba(255, 255, 255, 0.09),
    inset -1px 0 rgba(255, 255, 255, 0.08),
    16px 0 42px rgba(0, 0, 0, 0.18);
  display: flex;
  flex-direction: column;
  transition: width 0.2s ease;
  overflow: hidden;
  flex-shrink: 0;
  -webkit-app-region: drag;
  position: relative;
  isolation: isolate;
}
.sidebar::before {
  content: "";
  position: absolute;
  inset: 0;
  background:
    linear-gradient(180deg, rgba(255, 255, 255, 0.19), transparent 26%),
    linear-gradient(
      112deg,
      color-mix(in srgb, var(--accent) 22%, transparent),
      transparent 46%,
      rgba(255, 255, 255, 0.085)
    );
  pointer-events: none;
  z-index: 0;
}

:global(:root[data-theme="dark"] .sidebar) {
  background-color: rgba(18, 28, 40, 0.32);
}

:global(:root[data-theme="light"] .sidebar) {
  background-color: rgba(255, 255, 255, 0.32);
}

:global(:root[data-theme="skeleton"] .sidebar) {
  background-color: rgba(19, 19, 19, 0.32);
}

:global(:root[data-theme="forest"] .sidebar) {
  background-color: rgba(13, 21, 16, 0.32);
}

:global(:root[data-theme="orange-peel"] .sidebar) {
  background-color: rgba(18, 11, 8, 0.32);
}

:global(:root[data-theme="dragonfruit"] .sidebar) {
  background-color: rgba(26, 14, 24, 0.32);
}

:global(:root[data-theme="banana"] .sidebar) {
  background-color: rgba(74, 61, 22, 0.32);
}

:global(:root[data-theme="lava"] .sidebar) {
  background-color: rgba(26, 6, 6, 0.32);
}

:global(:root[data-theme="beach"] .sidebar) {
  background-color: rgba(138, 116, 78, 0.32);
}

:global(:root[data-theme="xray"] .sidebar) {
  background-color: rgba(4, 8, 6, 0.32);
}

:global(:root[data-low-performance="true"] .sidebar) {
  background-color: var(--bg-surface);
  backdrop-filter: none;
  -webkit-backdrop-filter: none;
  border-right-color: var(--border);
  box-shadow: none;
}

:global(:root[data-low-performance="true"] .sidebar::before) {
  display: none;
}
.sidebar > * {
  position: relative;
  z-index: 1;
}
.sidebar.collapsed {
  width: var(--sidebar-width-collapsed);
}

.sidebar-top {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 108px;
  padding: 38px 12px 12px;
  border-bottom: 1px solid var(--border);
  flex: 0 0 auto;
}

.sidebar-hamburger {
  background: transparent;
  border: 1px solid transparent;
  color: var(--sidebar-text);
  cursor: pointer;
  padding: 4px;
  border-radius: var(--radius-md);
  display: flex;
  align-items: center;
  justify-content: center;
  -webkit-app-region: no-drag;
  transition: color var(--transition);
  flex-shrink: 0;
}
.sidebar-hamburger:hover {
  color: var(--sidebar-text-hover);
  border-color: var(--border);
  background: var(--sidebar-hover);
}

.sidebar-logo {
  display: flex;
  align-items: center;
  gap: 8px;
  overflow: hidden;
  min-width: 0;
}
.sidebar-logo-icon {
  width: 26px;
  height: 26px;
  flex-shrink: 0;
}
.sidebar-logo-text {
  font-family: var(--font-logo);
  font-size: 10px;
  color: transparent;
  background: linear-gradient(
    90deg,
    var(--sidebar-logo-color),
    var(--sidebar-logo-accent),
    var(--sidebar-logo-color),
    var(--sidebar-logo-accent)
  );
  background-size: 300% 100%;
  -webkit-background-clip: text;
  background-clip: text;
  line-height: 1.4;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  animation: logo-shift 6s linear infinite;
}
@keyframes logo-shift {
  0% {
    background-position: 0% 50%;
  }
  100% {
    background-position: 100% 50%;
  }
}

.sidebar-nav {
  flex: 1;
  min-height: 0;
  padding: 10px 8px;
  display: flex;
  flex-direction: column;
  gap: 2px;
  overflow-y: auto;
}

.sidebar-nav-item {
  position: relative;
  display: flex;
  align-items: center;
  justify-content: flex-start;
  gap: 10px;
  min-height: 36px;
  padding: 8px 10px;
  border: 1px solid transparent;
  background: none;
  color: var(--sidebar-text);
  font-weight: 700;
  border-radius: var(--radius-sm);
  cursor: pointer;
  transition: all var(--transition);
  width: 100%;
  text-align: left;
  font-size: 13px;
  -webkit-app-region: no-drag;
  white-space: nowrap;
  overflow: hidden;
}
.sidebar-nav-item:hover {
  background: var(--sidebar-hover);
  color: var(--sidebar-text-hover);
  border-color: var(--border);
}
.sidebar-nav-item.active {
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.045), transparent 60%), var(--sidebar-active);
  color: var(--sidebar-text-active);
  border-color: rgba(95, 183, 232, 0.18);
  box-shadow:
    inset 0 1px 0 rgba(255, 255, 255, 0.08),
    inset 0 0 12px rgba(95, 183, 232, 0.04);
}
.sidebar-nav-item.active::before,
.sidebar-nav-item.active::after {
  content: "";
  position: absolute;
  pointer-events: none;
  border-radius: inherit;
}
.sidebar-nav-item.active::before {
  inset: -1px;
  background:
    linear-gradient(90deg, transparent 0%, rgba(130, 219, 255, 0.34) 48%, transparent 100%),
    linear-gradient(180deg, rgba(255, 255, 255, 0.1), transparent 64%);
  opacity: 0.12;
  filter: blur(12px);
  transform: translateX(-64%);
  animation: sidebar-active-sheen 7.5s ease-in-out infinite;
}
.sidebar-nav-item.active::after {
  inset: 0;
  border: 1px solid rgba(122, 210, 255, 0.1);
  box-shadow:
    inset 0 0 0 1px rgba(255, 255, 255, 0.025),
    inset 0 0 10px rgba(95, 183, 232, 0.04);
  opacity: 0.48;
}

.sidebar-nav-icon {
  position: relative;
  z-index: 1;
  flex-shrink: 0;
  width: 18px;
  height: 18px;
}
.sidebar-nav-label {
  position: relative;
  z-index: 1;
  overflow: hidden;
  text-overflow: ellipsis;
}

@keyframes sidebar-active-sheen {
  0%,
  18% {
    transform: translateX(-70%);
    opacity: 0;
  }
  42% {
    opacity: 0.14;
  }
  68%,
  100% {
    transform: translateX(70%);
    opacity: 0;
  }
}

:global(:root[data-theme="light"] .sidebar-nav-item.active) {
  border-color: rgba(52, 127, 186, 0.16);
  box-shadow:
    inset 0 1px 0 rgba(255, 255, 255, 0.3),
    inset 0 0 12px rgba(52, 127, 186, 0.035);
}
:global(:root[data-theme="light"] .sidebar-nav-item.active::before) {
  background:
    linear-gradient(90deg, transparent 0%, rgba(52, 127, 186, 0.2) 48%, transparent 100%),
    linear-gradient(180deg, rgba(255, 255, 255, 0.28), transparent 64%);
  opacity: 0.12;
}
:global(:root[data-theme="light"] .sidebar-nav-item.active::after) {
  border-color: rgba(52, 127, 186, 0.1);
  box-shadow:
    inset 0 0 0 1px rgba(255, 255, 255, 0.14),
    inset 0 0 10px rgba(52, 127, 186, 0.03);
}

@media (prefers-reduced-motion: reduce) {
  .sidebar-nav-item.active::before {
    animation: none;
    opacity: 0.12;
    transform: none;
  }
}

.sidebar-theme-toggle {
  margin-bottom: 4px;
}

.sidebar-input-selector {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 8px 10px;
  margin-bottom: 4px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 38%, transparent);
}

.sidebar-input-label {
  display: flex;
  align-items: center;
  gap: 7px;
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.05em;
  text-transform: uppercase;
  color: var(--text-dim);
}

.sidebar-input-icon {
  flex-shrink: 0;
}

.sidebar-input-options {
  display: flex;
  gap: 4px;
}

.sidebar-input-option {
  flex: 1;
  min-height: 26px;
  padding: 3px 6px;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: none;
  color: var(--sidebar-text);
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
  transition: all var(--transition);
  -webkit-app-region: no-drag;
}

.sidebar-input-option:hover:not(:disabled) {
  background: var(--sidebar-hover);
  color: var(--sidebar-text-hover);
  border-color: var(--border);
}

.sidebar-input-option.active {
  background: linear-gradient(180deg, rgba(255, 255, 255, 0.045), transparent 60%), var(--sidebar-active);
  color: var(--sidebar-text-active);
  border-color: rgba(95, 183, 232, 0.18);
}

.sidebar-input-option:disabled {
  opacity: 0.6;
  cursor: default;
}

.theme-picker-backdrop {
  position: fixed;
  inset: 0;
  z-index: 89;
}

.theme-picker-popover {
  position: fixed;
  left: 8px;
  bottom: 96px;
  width: calc(var(--sidebar-width-expanded) - 8px);
  z-index: 90;
  display: flex;
  flex-direction: column;
  padding: 6px;
  background: var(--bg-card);
  border: 1px solid var(--border-strong);
  border-radius: var(--radius-md);
  box-shadow: 0 12px 32px var(--card-glow);
  -webkit-app-region: no-drag;
}

.theme-picker-header {
  padding: 4px 10px 6px;
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--text-dim);
  border-bottom: 1px solid var(--border);
  margin-bottom: 4px;
}

.theme-picker-list {
  display: flex;
  flex-direction: column;
  gap: 2px;
  max-height: 288px;
  overflow-y: auto;
  overscroll-behavior: contain;
  scroll-snap-type: y proximity;
  scrollbar-width: thin;
  scrollbar-color: var(--accent-dim) transparent;
}
.theme-picker-list::-webkit-scrollbar {
  width: 4px;
}
.theme-picker-list::-webkit-scrollbar-thumb {
  background: var(--accent-dim);
  border-radius: 2px;
}

.theme-picker-item {
  scroll-snap-align: start;
  display: flex;
  align-items: center;
  gap: 10px;
  min-height: 32px;
  padding: 6px 10px;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  background: none;
  color: var(--text-primary);
  font-size: 13px;
  font-weight: 600;
  cursor: pointer;
  text-align: left;
  transition: all var(--transition);
}
.theme-picker-item:hover {
  background: var(--sidebar-hover);
  border-color: var(--border);
}
.theme-picker-item.active {
  background: var(--accent-glow);
  border-color: var(--accent-dim);
  color: var(--accent);
}

.theme-picker-icon {
  flex-shrink: 0;
  width: 16px;
  height: 16px;
}

.sidebar-bottom {
  padding: 8px;
  border-top: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  gap: 2px;
  flex: 0 0 auto;
}

.sidebar.collapsed .sidebar-top {
  justify-content: center;
  padding-left: 6px;
  padding-right: 6px;
}
.sidebar.collapsed .sidebar-logo {
  display: none;
}
.sidebar.collapsed .sidebar-nav-item {
  justify-content: center;
  padding: 8px;
  gap: 0;
}
.sidebar.collapsed .sidebar-nav-icon {
  display: block;
}

@media (max-width: 720px) {
  .sidebar {
    width: var(--sidebar-width-collapsed);
  }
  .sidebar-top {
    justify-content: center;
    padding-left: 6px;
    padding-right: 6px;
  }
  .sidebar-logo,
  .sidebar-nav-label {
    display: none;
  }
  .sidebar-nav-item {
    justify-content: center;
    padding: 8px;
    gap: 0;
  }
}
</style>
