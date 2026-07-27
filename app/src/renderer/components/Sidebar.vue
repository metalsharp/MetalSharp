<script setup lang="ts">
import { computed, ref, type Component } from "vue";
import IconMenu from "~icons/lucide/menu";
import IconMoon from "~icons/lucide/moon";
import IconSun from "~icons/lucide/sun";
import IconSettings from "~icons/lucide/settings";
import IconTerminal from "~icons/lucide/terminal";
import IconBone from "~icons/lucide/bone";
import IconTreePine from "~icons/lucide/tree-pine";
import IconCitrus from "~icons/lucide/citrus";
import IconSparkles from "~icons/lucide/sparkles";
import { themedNavIcon, type ThemeName } from "../composables/useTheme";

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

interface ThemeOption {
  name: ThemeName;
  label: string;
  icon: Component;
}

const themeOptions: ThemeOption[] = [
  { name: "dark", label: "Dark", icon: IconMoon },
  { name: "light", label: "Light", icon: IconSun },
  { name: "developer", label: "Dev Mode", icon: IconTerminal },
  { name: "skeleton", label: "Skeleton", icon: IconBone },
  { name: "forest", label: "Forest", icon: IconTreePine },
  { name: "orange-peel", label: "Orange Peel", icon: IconCitrus },
  { name: "dragonfruit", label: "Dragonfruit", icon: IconSparkles },
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

:global(:root[data-theme="developer"] .sidebar) {
  background-color: rgba(9, 7, 15, 0.32);
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

:global(:root[data-theme="developer"] .sidebar::before) {
  background:
    linear-gradient(180deg, rgba(255, 46, 247, 0.16) 0%, transparent 34%),
    linear-gradient(115deg, rgba(185, 255, 77, 0.1), transparent 42%, rgba(0, 245, 255, 0.08));
}

:global(:root[data-theme="developer"] .sidebar-nav-item.active) {
  border-color: rgba(185, 255, 77, 0.24);
  box-shadow:
    inset 0 1px 0 rgba(255, 255, 255, 0.09),
    inset 0 0 14px rgba(255, 46, 247, 0.06),
    0 0 0 1px rgba(0, 245, 255, 0.07);
}

:global(:root[data-theme="developer"] .sidebar-nav-item.active::before) {
  background:
    linear-gradient(90deg, transparent 0%, rgba(185, 255, 77, 0.45) 45%, rgba(0, 245, 255, 0.28) 52%, transparent 100%),
    linear-gradient(180deg, rgba(255, 46, 247, 0.16), transparent 64%);
  opacity: 0.18;
}

:global(:root[data-theme="developer"] .sidebar-nav-item.active::after) {
  border-color: rgba(0, 245, 255, 0.18);
  box-shadow:
    inset 0 0 0 1px rgba(185, 255, 77, 0.07),
    inset 0 0 12px rgba(255, 46, 247, 0.05);
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
  width: calc(var(--sidebar-width-expanded) - 16px);
  z-index: 90;
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 6px;
  background: var(--bg-card);
  border: 1px solid var(--border-strong);
  border-radius: var(--radius-md);
  box-shadow: 0 12px 32px var(--card-glow);
  -webkit-app-region: no-drag;
}

.theme-picker-item {
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
