import { computed } from "vue";
import { type ThemeName, useTheme } from "./useTheme";

// The library's runtime theme tokens, shared so the Sharp Library and Logs
// views can render with the exact same themed surface language.
export const libraryThemeAccent: Record<ThemeName, string> = {
  dark: "#e8d6b7",
  light: "#4db8ff",
  skeleton: "#d6d0c4",
  forest: "#6fce88",
  "orange-peel": "#ff9a45",
  dragonfruit: "#ff66aa",
  lava: "#ff6b52",
};

export const libraryThemeControlTokens: Record<ThemeName, { bg: string; text: string; hover: string; border: string }> =
  {
    dark: { bg: "#080a0d", text: "#ffffff", hover: "#171a1e", border: "rgba(255,255,255,.22)" },
    light: { bg: "#fdfbf7", text: "#1e2732", hover: "#ffffff", border: "rgba(30,39,50,.24)" },
    skeleton: { bg: "#242424", text: "#eeeeee", hover: "#303030", border: "rgba(238,238,238,.24)" },
    forest: { bg: "#182219", text: "#dce8dc", hover: "#1e2b20", border: "rgba(140,190,150,.24)" },
    "orange-peel": { bg: "#231610", text: "#f2e4d8", hover: "#2b1b12", border: "rgba(255,170,120,.24)" },
    dragonfruit: { bg: "#2c182a", text: "#f8e4f0", hover: "#361e33", border: "rgba(255,170,210,.24)" },
    lava: { bg: "#2b0d12", text: "#fff2ee", hover: "#3b1117", border: "rgba(255,110,90,.3)" },
  };

export function useLibraryThemeStyle() {
  const { theme } = useTheme();
  return computed<Record<string, string>>(() => {
    const tokens = libraryThemeControlTokens[theme.value];
    return {
      "--library-accent": libraryThemeAccent[theme.value],
      "--library-control-bg": tokens.bg,
      "--library-control-text": tokens.text,
      "--library-control-hover": tokens.hover,
      "--library-control-border": tokens.border,
    };
  });
}
