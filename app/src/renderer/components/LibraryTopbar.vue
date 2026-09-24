<script setup lang="ts">
import { computed, inject, ref, type Component, type Ref } from "vue";
import { useI18n } from "vue-i18n";
import { api, steamFix } from "../composables/useApi";
import { useToast } from "../composables/useToast";
import { themes, useTheme, type ThemeName } from "../composables/useTheme";
import { useLibraryThemeStyle } from "../composables/useLibraryTheme";
import SettingsOverlay from "./SettingsOverlay.vue";
import IconSearch from "~icons/lucide/search";
import IconSettings from "~icons/lucide/settings";
import IconChevronDown from "~icons/lucide/chevron-down";
import IconMoon from "~icons/lucide/moon";
import IconSun from "~icons/lucide/sun";
import IconBone from "~icons/lucide/bone";
import IconTreePine from "~icons/lucide/tree-pine";
import IconCitrus from "~icons/lucide/citrus";
import IconGrape from "~icons/lucide/grape";
import IconFlame from "~icons/lucide/flame";
import IconGamepad from "~icons/lucide/gamepad-2";
import IconLibrary from "~icons/lucide/library";
import IconDownload from "~icons/lucide/download";
import IconScrollText from "~icons/lucide/scroll-text";

type TopbarTab = "play" | "collection" | "sharp-library" | "logs";

const props = withDefaults(
  defineProps<{
    activeTab: TopbarTab;
    search?: string;
    showSearch?: boolean;
  }>(),
  { search: "", showSearch: true },
);

const emit = defineEmits<{
  navigate: [tab: TopbarTab];
  "update:search": [value: string];
}>();

const { theme, setTheme } = useTheme();
const libraryThemeStyle = useLibraryThemeStyle();
const themeMenuOpen = ref(false);
const tabMenuOpen = ref(false);
const themeButtonEl = ref<HTMLElement | null>(null);
const tabButtonEl = ref<HTMLElement | null>(null);
const settingsOpen = ref(false);
const steamFixMenuOpen = ref(false);
const steamFixBusy = ref(false);
const wineSteamRunning = inject<Ref<boolean>>("wineSteamRunning")!;
const toast = useToast();
const { t } = useI18n();

const themeIcons: Record<ThemeName, Component> = {
  dark: IconMoon,
  light: IconSun,
  skeleton: IconBone,
  forest: IconTreePine,
  "orange-peel": IconCitrus,
  dragonfruit: IconGrape,
  lava: IconFlame,
};
const themeLabels: Record<ThemeName, string> = {
  dark: "Dark",
  light: "Light",
  skeleton: "Skeleton",
  forest: "Forest",
  "orange-peel": "Orange",
  dragonfruit: "Dragonfruit",
  lava: "Lava",
};

const tabOptions = computed<{ id: TopbarTab; label: string; icon: Component }[]>(() => [
  { id: "play", label: t("library.play"), icon: IconGamepad },
  { id: "collection", label: t("library.collection"), icon: IconLibrary },
  { id: "sharp-library", label: t("nav.sharp"), icon: IconDownload },
  { id: "logs", label: t("nav.logs"), icon: IconScrollText },
]);
const activeTabOption = computed(
  () => tabOptions.value.find((option) => option.id === props.activeTab) || tabOptions.value[0],
);
const themeMenuStyle = computed(() => {
  if (!themeMenuOpen.value || !themeButtonEl.value) return {};
  const rect = themeButtonEl.value.getBoundingClientRect();
  return { top: `${rect.bottom + 8}px`, left: `${rect.left}px` };
});
const tabMenuStyle = computed(() => {
  if (!tabMenuOpen.value || !tabButtonEl.value) return {};
  const rect = tabButtonEl.value.getBoundingClientRect();
  return { top: `${rect.bottom + 8}px`, left: `${rect.right - 174}px` };
});

function chooseTab(tab: TopbarTab) {
  tabMenuOpen.value = false;
  emit("navigate", tab);
}

function openSettings() {
  settingsOpen.value = true;
}

function onSearchInput(event: Event) {
  emit("update:search", (event.target as HTMLInputElement).value);
}

async function runSteamFix() {
  if (steamFixBusy.value) return;
  steamFixMenuOpen.value = false;
  steamFixBusy.value = true;
  toast.show(t("library.fixingSteam"));
  const result = await steamFix();
  steamFixBusy.value = false;
  if (result?.ok) toast.show(t("library.fixSteam"), "success");
  else toast.show(result?.error ?? t("library.fixSteam"), "error");
}

async function toggleSteam() {
  if (wineSteamRunning.value) {
    const result = await api<{ ok: boolean; running?: boolean; error?: string }>("POST", "/steam/stop");
    if (result?.ok) wineSteamRunning.value = false;
    toast.show(result?.error || "Wine Steam stopped", result?.ok ? "success" : "error");
  } else {
    const result = await api<{ ok: boolean; error?: string }>("POST", "/steam/launch");
    if (result?.ok) wineSteamRunning.value = true;
    toast.show(result?.error || "Starting Wine Steam...", result?.ok ? "success" : "error");
  }
}
</script>

<template>
  <header class="library-topbar" :class="{ 'no-search': !showSearch }">
    <div class="library-window-space" aria-hidden="true"></div>
    <div class="library-header-right">
      <div class="library-brand" aria-label="MetalSharp">
        <img src="../assets/metalsharp-logo.png" alt="" class="library-brand-mark" />
        <span>MetalSharp</span>
      </div>
      <div class="library-steam-split">
        <button class="library-steam-button" type="button" @click="toggleSteam">
          <svg class="steam-mark" viewBox="0 0 24 24" aria-hidden="true">
            <circle cx="16.4" cy="6.8" r="3.25"></circle>
            <circle cx="6.1" cy="17.1" r="2.35"></circle>
            <path d="M8.15 15.85 13.45 9.8M3.85 17.8l-2.3-.9"></path>
          </svg>
          <span>{{ wineSteamRunning ? t("library.stopSteam") : t("library.startSteam") }}</span>
        </button>
        <button
          class="library-steam-gear"
          type="button"
          :aria-label="t('library.steamOptions')"
          :disabled="steamFixBusy"
          @click.stop="steamFixMenuOpen = !steamFixMenuOpen"
        >
          <IconSettings width="13" height="13" />
        </button>
        <div v-if="steamFixMenuOpen" class="library-steam-fix-backdrop" @click="steamFixMenuOpen = false"></div>
        <div v-if="steamFixMenuOpen" class="library-steam-fix-menu">
          <button type="button" :disabled="steamFixBusy" @click="runSteamFix">
            {{ steamFixBusy ? t("library.fixingSteam") : t("library.fixSteam") }}
          </button>
        </div>
      </div>
      <label v-if="showSearch" class="library-search">
        <IconSearch width="18" height="18" aria-hidden="true" />
        <input
          :value="search"
          type="search"
          :placeholder="t('library.searchPlaceholder')"
          @input="onSearchInput($event)"
        />
      </label>
      <div class="library-theme-control">
        <button
          ref="themeButtonEl"
          class="library-theme-button"
          type="button"
          :aria-label="t('library.theme')"
          @click="themeMenuOpen = !themeMenuOpen"
        >
          <component :is="themeIcons[theme] || IconMoon" width="17" height="17" />
        </button>
      </div>
      <nav class="library-nav" :aria-label="t('library.navigation')">
        <div class="library-tab-control">
          <button ref="tabButtonEl" class="library-tab-button" type="button" @click="tabMenuOpen = !tabMenuOpen">
            <component :is="activeTabOption.icon" width="16" height="16" />
            <span>{{ activeTabOption.label }}</span>
            <IconChevronDown width="14" height="14" />
          </button>
        </div>
        <button
          class="library-settings-button"
          type="button"
          :aria-label="t('nav.settings')"
          :title="t('nav.settings')"
          @click="openSettings"
        >
          <IconSettings width="19" height="19" />
        </button>
      </nav>
    </div>
  </header>

  <Teleport to="body">
    <div
      v-if="themeMenuOpen"
      class="library-theme-menu library-topbar-overlay"
      :style="[libraryThemeStyle, themeMenuStyle]"
    >
      <button
        v-for="themeName in themes"
        :key="themeName"
        type="button"
        :class="{ active: themeName === theme }"
        @click="
          setTheme(themeName);
          themeMenuOpen = false;
        "
      >
        <span class="theme-menu-swatch" :data-theme="themeName"></span>
        <span>{{ themeLabels[themeName] }}</span>
      </button>
    </div>
    <div v-if="tabMenuOpen" class="library-tab-menu library-topbar-overlay" :style="[libraryThemeStyle, tabMenuStyle]">
      <button
        v-for="option in tabOptions"
        :key="option.id"
        type="button"
        :class="{ active: option.id === activeTab }"
        @click="chooseTab(option.id)"
      >
        <component :is="option.icon" width="16" height="16" />
        <span>{{ option.label }}</span>
      </button>
    </div>
  </Teleport>

  <SettingsOverlay v-if="settingsOpen" @close="settingsOpen = false" />
</template>

<style scoped>
.library-topbar {
  position: relative;
  z-index: 1000;
  display: flex;
  align-items: center;
  gap: 24px;
  height: 56px;
  min-height: 56px;
  padding: 9px 16px 8px 46px;
  background: rgba(25, 28, 31, 0.97);
  border-bottom: 1px solid var(--line);
  box-shadow: 0 4px 20px rgba(0, 0, 0, 0.2);
  -webkit-app-region: drag;
}
.library-search,
.library-steam-fix-backdrop {
  -webkit-app-region: no-drag;
}
.library-topbar button,
.library-topbar input,
.library-topbar select,
.library-topbar textarea,
.library-topbar a {
  -webkit-app-region: no-drag;
}
.library-window-space {
  flex: 0 0 30px;
  align-self: stretch;
}
.library-header-right {
  display: flex;
  flex: 1 1 auto;
  min-width: 0;
  align-items: center;
  gap: 10px;
}
.library-theme-control {
  position: relative;
  z-index: 1001;
  flex: 0 0 auto;
  margin-right: 4px;
}
.library-theme-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  min-width: 28px;
  flex: 0 0 28px;
  height: 30px;
  padding: 0;
  border: 1px solid var(--library-accent);
  border-radius: 5px;
  color: #fff !important;
  background: transparent;
  cursor: pointer;
  font: inherit;
}
.library-theme-button svg {
  display: block;
  flex: 0 0 17px;
  color: #fff !important;
}
.library-theme-button:hover {
  color: #fff !important;
  border-color: var(--library-accent);
  background: transparent;
}
.library-theme-button:hover svg {
  color: #fff !important;
}
.library-theme-menu {
  position: fixed;
  z-index: 1002;
  top: calc(100% + 8px);
  left: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
  width: 160px;
  padding: 6px;
  border: 1px solid rgba(231, 234, 236, 0.2);
  border-radius: 8px;
  background: rgba(28, 31, 33, 0.98);
  box-shadow: 0 14px 35px rgba(0, 0, 0, 0.45);
}
.library-theme-menu button {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 29px;
  padding: 0 8px;
  border: 0;
  border-radius: 5px;
  color: #c8ccca;
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 11px;
  text-align: left;
}
.library-theme-menu button:hover,
.library-theme-menu button.active {
  color: #fff;
  background: color-mix(in srgb, var(--library-accent) 18%, transparent);
}
.theme-menu-swatch {
  width: 10px;
  height: 10px;
  border: 1px solid rgba(255, 255, 255, 0.35);
  border-radius: 50%;
  background: var(--library-accent);
}
.theme-menu-swatch[data-theme="light"] {
  background: #4db8ff;
}
.theme-menu-swatch[data-theme="skeleton"] {
  background: #d6d0c4;
}
.theme-menu-swatch[data-theme="forest"] {
  background: #6fce88;
}
.theme-menu-swatch[data-theme="orange-peel"] {
  background: #ff9a45;
}
.theme-menu-swatch[data-theme="dragonfruit"] {
  background: #ff66aa;
}
.theme-menu-swatch[data-theme="lava"] {
  background: #ff6b52;
}
.library-brand {
  display: flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 10px;
  padding-left: 0;
  color: #f2f2f1;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: -0.02em;
  white-space: nowrap;
}
.library-brand-mark {
  width: 27px;
  height: 27px;
  object-fit: contain;
  filter: grayscale(1) brightness(0.82) contrast(1.1);
  opacity: 0.86;
}
.steam-mark {
  width: 17px;
  height: 17px;
  fill: none;
  stroke: currentColor;
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: 1.75;
}
.library-steam-split {
  position: relative;
  display: inline-flex;
  align-items: stretch;
}
.library-steam-split .library-steam-button {
  margin-left: 0;
  border-radius: 8px 0 0 8px;
  border-right: 0;
}
.library-steam-gear {
  display: grid;
  place-items: center;
  width: 26px;
  min-height: 36px;
  border: 1px solid var(--library-control-border);
  border-left: 0;
  border-radius: 0 8px 8px 0;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  cursor: pointer;
  transition: background 0.18s ease;
}
.library-steam-gear:hover:not(:disabled) {
  background: var(--library-control-hover);
}
.library-steam-gear:disabled {
  opacity: 0.6;
  cursor: default;
}
.library-steam-fix-backdrop {
  position: fixed;
  inset: 0;
  z-index: 50;
}
.library-steam-fix-menu {
  position: absolute;
  z-index: 60;
  top: calc(100% + 6px);
  left: 0;
  min-width: 160px;
  padding: 5px;
  border: 1px solid var(--library-accent);
  border-radius: 8px;
  background: var(--library-control-bg);
  box-shadow: 0 14px 35px rgba(0, 0, 0, 0.45);
}
.library-steam-fix-menu button {
  display: flex;
  width: 100%;
  align-items: center;
  min-height: 30px;
  padding: 0 10px;
  border: 0;
  border-radius: 6px;
  color: var(--library-control-text);
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 12.5px;
  text-align: left;
}
.library-steam-fix-menu button:hover:not(:disabled) {
  background: var(--library-control-hover);
}
.library-steam-button {
  display: inline-flex;
  align-items: center;
  margin-left: 8px;
  justify-content: center;
  gap: 8px;
  min-height: 36px;
  padding: 0 15px;
  border: 1px solid var(--library-control-border);
  border-radius: 8px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  box-shadow:
    inset 0 1px rgba(255, 255, 255, 0.07),
    0 2px 8px rgba(0, 0, 0, 0.17);
  cursor: pointer;
  font: inherit;
  font-size: 13px;
  font-weight: 600;
  white-space: nowrap;
  transition:
    background 0.18s ease,
    border-color 0.18s ease,
    transform 0.18s ease;
}
.library-steam-button:hover {
  border-color: var(--library-control-border);
  background: var(--library-control-hover);
  transform: translateY(-1px);
}
.library-steam-button:active {
  transform: translateY(0);
}
.library-search {
  position: static;
  flex: 1 1 auto;
  min-width: 140px;
  width: auto;
  max-width: 320px;
  margin: 0 auto;
  transform: none;
  display: flex;
  align-items: center;
  gap: 11px;
  height: 38px;
  min-width: 0;
  padding: 0 12px;
  color: var(--library-control-text);
  background: var(--library-control-bg);
  border: 1px solid var(--library-control-border);
  border-radius: 8px;
  box-shadow:
    inset 0 1px rgba(255, 255, 255, 0.025),
    0 3px 8px rgba(0, 0, 0, 0.12);
  transition:
    border-color 0.18s ease,
    background 0.18s ease;
}
.library-search:focus-within {
  border-color: var(--library-control-border);
  background: var(--library-control-hover);
}
.library-search input {
  width: 100%;
  min-width: 0;
  border: 0;
  outline: 0;
  color: var(--library-control-text);
  background: transparent;
  font-size: 14px;
}
/* base.css paints all inputs black in the lava theme; the header search bar
   must stay a single themed surface instead of showing an inner input box. */
.library-topbar .library-search input,
.library-topbar .library-search input:focus {
  background: transparent !important;
  border: 0 !important;
  box-shadow: none !important;
}
.library-search input::placeholder {
  color: color-mix(in srgb, var(--library-control-text) 68%, transparent);
}
.library-search input::-webkit-search-cancel-button {
  filter: invert(1);
  opacity: 0.6;
}
.library-nav {
  display: flex;
  flex: 0 0 auto;
  align-items: center;
  justify-content: flex-start;
  gap: 12px;
  min-width: 0;
}
.library-tab-control {
  position: relative;
  z-index: 1001;
  flex: 0 0 auto;
}
.library-tab-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  min-height: 36px;
  padding: 0 13px;
  border: 1px solid var(--library-accent);
  border-radius: 7px;
  color: #fff !important;
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 13px;
  white-space: nowrap;
}
.library-tab-button:hover {
  background: transparent;
}
.library-tab-menu {
  position: fixed;
  z-index: 1002;
  display: flex;
  flex-direction: column;
  gap: 2px;
  width: 174px;
  padding: 6px;
  border: 1px solid var(--library-accent);
  border-radius: 8px;
  background: var(--library-control-bg);
  box-shadow: 0 14px 35px rgba(0, 0, 0, 0.45);
}
.library-topbar-overlay {
  z-index: 20000 !important;
  -webkit-app-region: no-drag;
}
.library-tab-menu button {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 31px;
  padding: 0 9px;
  border: 0;
  border-radius: 5px;
  color: #fff;
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 12px;
  text-align: left;
}
.library-tab-menu button:hover,
.library-tab-menu button.active {
  color: #fff;
  background: var(--library-control-hover);
}
.library-nav-item,
.library-settings-button {
  border: 0;
  color: #c6c8c9;
  cursor: pointer;
  font: inherit;
}
.library-nav-item {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  min-height: 36px;
  padding: 0 14px;
  background: transparent;
  border-radius: 8px;
  font-size: 14px;
  transition:
    color 0.18s ease,
    background 0.18s ease;
}
.library-nav-item:hover {
  color: #fff;
  background: rgba(255, 255, 255, 0.08);
}
.library-nav-item.active {
  color: var(--library-control-text);
  background: var(--library-control-bg);
  box-shadow:
    inset 0 1px rgba(255, 255, 255, 0.08),
    0 2px 7px rgba(0, 0, 0, 0.2);
}
.library-settings-button {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  min-width: 34px;
  flex: 0 0 34px;
  height: 34px;
  margin-left: 7px;
  border: 1px solid var(--library-control-border);
  border-radius: 8px;
  background: var(--library-control-bg);
  color: var(--library-control-text);
  transition:
    color 0.18s ease,
    border-color 0.18s ease,
    background 0.18s ease,
    transform 0.18s ease;
}
.library-settings-button:hover {
  border-color: var(--library-control-border);
  background: var(--library-control-hover);
  color: #fff;
  transform: translateY(-1px);
}

.library-steam-button,
.library-settings-button,
.library-tab-button {
  border: 1px solid var(--library-accent);
  color: #fff !important;
  background: transparent !important;
  box-shadow: none;
}
.library-steam-button:hover,
.library-settings-button:hover,
.library-tab-button:hover {
  border-color: var(--library-accent);
  color: #fff !important;
  background: transparent !important;
}
/* With the search hidden (Sharp Library / Logs), keep the theme control
   beside the tab dropdown at the right edge. */
.library-topbar.no-search .library-theme-control {
  margin-left: auto;
}
.library-topbar.no-search .library-nav {
  margin-left: 0;
}

/* Light theme: header chrome borders are white, and the Play dropdown overlay
   keeps the mono-black surface with white text used by every other theme
   (the light control tokens are near-white on white, which is unreadable). */
[data-theme="light"] .library-theme-button,
[data-theme="light"] .library-tab-button,
[data-theme="light"] .library-settings-button,
[data-theme="light"] .library-steam-button,
[data-theme="light"] .library-steam-gear {
  border-color: #fff;
}
[data-theme="light"] .library-theme-button:hover,
[data-theme="light"] .library-tab-button:hover,
[data-theme="light"] .library-settings-button:hover,
[data-theme="light"] .library-steam-button:hover {
  border-color: #fff;
}
[data-theme="light"] .library-tab-menu {
  background: #080a0d;
}
[data-theme="light"] .library-tab-menu button:hover,
[data-theme="light"] .library-tab-menu button.active {
  background: #171a1e;
}
</style>
