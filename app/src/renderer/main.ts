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

const app = createApp(App);
app.config.errorHandler = (error) => captureTelemetryException(error);
app.mount("#app");
