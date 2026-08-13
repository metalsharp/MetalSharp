import { createApp } from "vue";
import App from "./App.vue";
import { captureTelemetryException, setTelemetryBuildConfig } from "./composables/useTelemetry";
import "./styles/base.css";
import "./styles/transitions.css";

declare const __POSTHOG_PROJECT_TOKEN__: string;
declare const __POSTHOG_HOST__: string;

setTelemetryBuildConfig({
  projectToken: __POSTHOG_PROJECT_TOKEN__,
  host: __POSTHOG_HOST__,
});

// Browser-only dev fallback: when running the renderer outside Electron
// (e.g. `npm run dev:vite` for styling), stub the preload bridge so wizard
// views render standalone instead of throwing on mount. Every method
// resolves to an empty object; nothing is actually triggered.
if (!(window as unknown as { metalsharp?: unknown }).metalsharp) {
  (window as unknown as { metalsharp: unknown }).metalsharp = new Proxy({}, { get: () => async () => ({}) });
}

const app = createApp(App);
app.config.errorHandler = (error) => captureTelemetryException(error);
app.mount("#app");
