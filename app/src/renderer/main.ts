import { createApp } from "vue";
import App from "./App.vue";
import { installDomLocalization } from "./domLocalization";
import { i18n, setAppLocale } from "./i18n";
import "./styles/base.css";
import "./styles/transitions.css";

setAppLocale(i18n.global.locale.value);
const app = createApp(App).use(i18n);
app.mount("#app");
installDomLocalization(app);
