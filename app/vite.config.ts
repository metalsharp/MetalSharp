import { defineConfig, loadEnv } from "vite";
import vue from "@vitejs/plugin-vue";
import Icons from "unplugin-icons/vite";
import { resolve } from "path";
import { fileURLToPath, URL } from "node:url";

const here = fileURLToPath(new URL(".", import.meta.url));

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, here, "VITE_");
  return {
    define: {
      __POSTHOG_PROJECT_TOKEN__: JSON.stringify(env.VITE_POSTHOG_PROJECT_TOKEN ?? ""),
      __POSTHOG_HOST__: JSON.stringify(env.VITE_POSTHOG_HOST ?? ""),
    },
    plugins: [vue(), Icons({ compiler: "vue3", defaultStyle: "stroke", scale: 1.2 })],
    root: resolve(here, "src/renderer"),
    base: "./",
    build: {
      outDir: resolve(here, "dist/renderer"),
      emptyOutDir: true,
      rollupOptions: {
        output: {
          entryFileNames: "index.js",
          assetFileNames: "assets/[name].[ext]",
        },
      },
    },
    resolve: {
      alias: {
        "@": resolve(here, "src/renderer"),
      },
    },
    server: {
      port: 5173,
    },
  };
});
