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
import { themedNavIcon, type ThemeName } from "../composables/useTheme";
import { api } from "../composables/useApi";

const props = defineProps<{
  currentView: string;
  theme: ThemeName;
}>();

const emit = defineEmits<{
  navigate: [view: string];
  selectTheme: [theme: ThemeName];
}>();

const collapsed = ref(false);
const themePickerOpen = ref(false);
const themePickerRef = ref<HTMLElement | null>(null);
const controllerMode = ref<"dinput" | "xinput">("xinput");
const msyncEnabled = ref(true);
const runtimePreferencesSaving = ref(false);

async function loadRuntimePreferences() {
  const config = await api<AppConfig>("GET", "/config");
  if (!config?.ok) return;
  controllerMode.value = config.controllerMode ?? config.controller_mode ?? "xinput";
  msyncEnabled.value = Boolean(config.msyncEnabled ?? config.msync_enabled ?? true);
}

async function saveRuntimePreferences(body: Record<string, unknown>) {
  if (runtimePreferencesSaving.value) return false;
  runtimePreferencesSaving.value = true;
  const config = await api<AppConfig>("POST", "/config", body);
  if (config?.ok) {
    controllerMode.value = config.controllerMode ?? config.controller_mode ?? controllerMode.value;
    msyncEnabled.value = Boolean(config.msyncEnabled ?? config.msync_enabled ?? msyncEnabled.value);
  }
  runtimePreferencesSaving.value = false;
  return Boolean(config?.ok);
}

function selectControllerMode(mode: "dinput" | "xinput") {
  if (mode === controllerMode.value) return;
  const previous = controllerMode.value;
  controllerMode.value = mode;
  void saveRuntimePreferences({ controllerMode: mode }).then((saved) => {
    if (!saved) controllerMode.value = previous;
  });
}

function toggleMsync() {
  const previous = msyncEnabled.value;
  const enabled = !msyncEnabled.value;
  msyncEnabled.value = enabled;
  void saveRuntimePreferences({ msyncEnabled: enabled }).then((saved) => {
    if (!saved) msyncEnabled.value = previous;
  });
}

onMounted(() => void loadRuntimePreferences());

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

    <div class="sidebar-runtime-controls">
      <div class="runtime-control controller-control" :title="collapsed ? `Controller: ${controllerMode}` : undefined">
        <span v-if="!collapsed" class="runtime-control-label">Controller</span>
        <div class="controller-selector" role="group" aria-label="Controller input mode">
          <button
            class="controller-option"
            :class="{ active: controllerMode === 'dinput' }"
            :aria-pressed="controllerMode === 'dinput'"
            :disabled="runtimePreferencesSaving"
            title="DirectInput"
            @click="selectControllerMode('dinput')"
          >
            D
          </button>
          <span v-if="!collapsed" class="controller-divider">|</span>
          <button
            class="controller-option"
            :class="{ active: controllerMode === 'xinput' }"
            :aria-pressed="controllerMode === 'xinput'"
            :disabled="runtimePreferencesSaving"
            title="XInput"
            @click="selectControllerMode('xinput')"
          >
            X
          </button>
        </div>
      </div>
      <button
        class="msync-toggle"
        :class="{ active: msyncEnabled }"
        :aria-pressed="msyncEnabled"
        :disabled="runtimePreferencesSaving"
        :title="collapsed ? `Msync ${msyncEnabled ? 'on' : 'off'}` : undefined"
        @click="toggleMsync"
      >
        <span class="msync-label-full">Msync</span>
        <span class="msync-label-compact">M</span>
      </button>
    </div>

    <div class="sidebar-bottom">
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

.sidebar-runtime-controls {
  padding: 8px;
  border-top: 1px solid var(--border);
  display: flex;
  flex-direction: column;
  gap: 6px;
  -webkit-app-region: no-drag;
}

.runtime-control {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  color: var(--sidebar-text);
  font-size: 12px;
  font-weight: 700;
}

.runtime-control-label {
  overflow: hidden;
  text-overflow: ellipsis;
}

.controller-selector {
  display: inline-flex;
  align-items: center;
  padding: 2px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 62%, transparent);
}

.controller-option {
  min-width: 25px;
  min-height: 24px;
  padding: 2px 7px;
  border: 0;
  border-radius: calc(var(--radius-sm) - 2px);
  background: transparent;
  color: var(--text-dim);
  font: inherit;
  cursor: pointer;
}

.controller-option.active {
  background: var(--accent-glow);
  color: var(--accent);
  box-shadow: inset 0 0 0 1px var(--accent-dim);
}

.controller-divider {
  color: var(--border-strong);
  font-weight: 400;
}

.msync-toggle {
  width: 100%;
  min-height: 32px;
  border: 1px solid var(--border-strong);
  border-radius: var(--radius-sm);
  background: color-mix(in srgb, var(--bg-surface) 72%, transparent);
  color: var(--text-dim);
  font-size: 12px;
  font-weight: 800;
  cursor: pointer;
  transition: all var(--transition);
}

.msync-toggle.active {
  border-color: rgba(63, 211, 132, 0.66);
  background: rgba(32, 178, 105, 0.18);
  color: rgb(83, 231, 151);
  box-shadow: inset 0 0 12px rgba(32, 178, 105, 0.12);
}

.msync-label-compact,
.sidebar.collapsed .msync-label-full {
  display: none;
}

.sidebar.collapsed .msync-label-compact {
  display: inline;
}

.controller-option:disabled,
.msync-toggle:disabled {
  cursor: wait;
  opacity: 0.62;
}

.sidebar.collapsed .sidebar-runtime-controls {
  align-items: center;
  padding-inline: 6px;
}

.sidebar.collapsed .runtime-control {
  justify-content: center;
}

.sidebar.collapsed .controller-selector {
  flex-direction: column;
  padding: 1px;
}

.sidebar.collapsed .controller-option {
  min-width: 27px;
  padding-inline: 4px;
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
  .sidebar-runtime-controls {
    align-items: center;
    padding-inline: 6px;
  }
  .runtime-control {
    justify-content: center;
  }
  .runtime-control-label,
  .controller-divider,
  .msync-label-full {
    display: none;
  }
  .msync-label-compact {
    display: inline;
  }
  .controller-selector {
    flex-direction: column;
    padding: 1px;
  }
  .controller-option {
    min-width: 27px;
    padding-inline: 4px;
  }
}
</style>
