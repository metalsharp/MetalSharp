import { createApp } from "vue";
import App from "./App.vue";
import { i18n, setAppLocale } from "./i18n";
import "./styles/base.css";
import "./styles/transitions.css";

setAppLocale(i18n.global.locale.value);
createApp(App).use(i18n).mount("#app");
