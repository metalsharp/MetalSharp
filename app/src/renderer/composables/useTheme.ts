import { type Component, ref, watch } from "vue";
import IconAxe from "~icons/lucide/axe";
import IconBinoculars from "~icons/lucide/binoculars";
import IconBone from "~icons/lucide/bone";
import IconCitrus from "~icons/lucide/citrus";
import IconClipboardList from "~icons/lucide/clipboard-list";
import IconCrosshair from "~icons/lucide/crosshair";
import IconFileText from "~icons/lucide/file-text";
import IconFlame from "~icons/lucide/flame";
import IconFlameKindling from "~icons/lucide/flame-kindling";
import IconGem from "~icons/lucide/gem";
import IconGrape from "~icons/lucide/grape";
import IconLayers from "~icons/lucide/layers";
import IconLeaf from "~icons/lucide/leaf";
import IconRefreshCcw from "~icons/lucide/refresh-ccw";
import IconRotateCw from "~icons/lucide/rotate-cw";
import IconScroll from "~icons/lucide/scroll";
import IconScrollText from "~icons/lucide/scroll-text";
import IconServer from "~icons/lucide/server";
import IconSkull from "~icons/lucide/skull";
import IconTrees from "~icons/lucide/trees";
import IconZap from "~icons/lucide/zap";

// prettier-ignore
export type ThemeName = "dark" | "light" | "skeleton" | "forest" | "orange-peel" | "dragonfruit" | "lava";

export const themes: ThemeName[] = ["dark", "light", "skeleton", "forest", "orange-peel", "dragonfruit", "lava"];

function readSavedTheme(): ThemeName {
  const requested = new URLSearchParams(window.location.search).get("theme");
  if (themes.includes(requested as ThemeName)) return requested as ThemeName;
  const saved = localStorage.getItem("metalsharp-theme");
  return themes.includes(saved as ThemeName) ? (saved as ThemeName) : "dark";
}

const theme = ref<ThemeName>(readSavedTheme());

watch(theme, (val) => {
  document.documentElement.dataset.theme = val;
  document.body.classList.toggle("light", val === "light");
  localStorage.setItem("metalsharp-theme", val);
});

document.documentElement.dataset.theme = theme.value;
document.body.classList.toggle("light", theme.value === "light");

export type NavIconKey = "library" | "sharp" | "logs" | "refresh" | "steam";

const defaultNavIcons: Record<NavIconKey, Component> = {
  library: IconServer,
  sharp: IconLayers,
  logs: IconFileText,
  refresh: IconRefreshCcw,
  steam: IconCrosshair,
};

const themeNavIcons: Partial<Record<ThemeName, Partial<Record<NavIconKey, Component>>>> = {
  skeleton: {
    library: IconSkull,
    sharp: IconBone,
    logs: IconScroll,
    refresh: IconRotateCw,
    steam: IconAxe,
  },
  forest: {
    library: IconTrees,
    sharp: IconLeaf,
    logs: IconScroll,
    refresh: IconRotateCw,
    steam: IconBinoculars,
  },
  "orange-peel": {
    library: IconFlame,
    sharp: IconCitrus,
    logs: IconClipboardList,
    refresh: IconRotateCw,
    steam: IconZap,
  },
  dragonfruit: {
    library: IconZap,
    sharp: IconGem,
    logs: IconScrollText,
    refresh: IconRotateCw,
    steam: IconGrape,
  },
  lava: {
    library: IconFlameKindling,
    sharp: IconFlame,
    logs: IconScrollText,
    refresh: IconRotateCw,
    steam: IconZap,
  },
};

export function themedNavIcon(key: NavIconKey): Component {
  return themeNavIcons[theme.value]?.[key] ?? defaultNavIcons[key];
}

export function useTheme() {
  function toggle() {
    const currentIndex = themes.indexOf(theme.value);
    theme.value = themes[(currentIndex + 1) % themes.length];
  }

  function setTheme(name: ThemeName) {
    if (themes.includes(name)) theme.value = name;
  }

  return { theme, toggle, setTheme };
}
