import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import Icons from "unplugin-icons/vite";
import { resolve } from "path";

export default defineConfig({
  plugins: [vue(), Icons({ compiler: "vue3", defaultStyle: "stroke", scale: 1.2 })],
  root: resolve(__dirname, "src/renderer"),
  base: "./",
  build: {
    outDir: resolve(__dirname, "dist/renderer"),
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
      "@": resolve(__dirname, "src/renderer"),
    },
  },
  server: {
    port: 5173,
    proxy: {
      // Steam's store API sends no CORS headers, so the browser preview fetches
      // it through this dev-server proxy for artwork enrichment of games whose
      // assets only exist under hashed CDN paths.
      "/steam-store-api": {
        target: "https://store.steampowered.com",
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/steam-store-api/, ""),
      },
    },
  },
});
