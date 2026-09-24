<script setup lang="ts">
import { computed, inject, type Ref } from "vue";
import { useI18n } from "vue-i18n";
import IconArrowDown from "~icons/lucide/arrow-down-to-line";
import IconCheckCircle from "~icons/lucide/check-circle-2";
import IconTv from "~icons/lucide/tv";
import IconWifi from "~icons/lucide/wifi";
import type { UpdateStatus } from "../api-types";

const library = inject<Ref<{ total: number; installed_count: number } | null>>("library")!;
const backendConnected = inject<Ref<boolean>>("backendConnected")!;
const backendVersion = inject<Ref<string | null>>("backendVersion")!;
const updateStatus = inject<Ref<UpdateStatus | null>>("updateStatus")!;
const updateDownloading = inject<Ref<boolean>>("updateDownloading")!;
const updateProgress = inject<Ref<number>>("updateProgress")!;
const updateMessage = inject<Ref<string>>("updateMessage")!;
const { t } = useI18n();
const startUpdateDownload = inject<() => void>("startUpdateDownload")!;
const openStreaming = inject<(() => void) | null>("openStreaming", null);
const clampedProgress = computed(() => Math.min(100, Math.max(3, Math.round(updateProgress.value))));

const gameCount = computed(() => library.value?.total ?? 0);
const installedCount = computed(() => library.value?.installed_count ?? 0);
const readyLabel = computed(() =>
  backendConnected.value ? t("ui.footer.allGamesReady") : t("ui.footer.previewLibrary"),
);
const readyDetail = computed(() =>
  backendConnected.value
    ? t("ui.footer.allSet")
    : backendVersion.value
      ? `Backend v${backendVersion.value}`
      : t("ui.footer.connectToPlay"),
);
</script>

<template>
  <footer class="library-footer">
    <div class="library-footer-status">
      <span class="library-footer-icon"><IconCheckCircle width="20" height="20" /></span>
      <div>
        <strong>{{ readyLabel }}</strong>
        <span>{{ installedCount }} {{ t("ui.footer.of") }} {{ gameCount }} games · {{ readyDetail }}</span>
      </div>
    </div>
    <button
      v-if="openStreaming"
      class="library-footer-streaming"
      type="button"
      :title="t('ui.streaming.title')"
      @click="openStreaming()"
    >
      <span class="library-footer-streaming-icon" aria-hidden="true">
        <IconTv width="18" height="18" />
        <span class="library-footer-streaming-wifi"><IconWifi width="9" height="9" /></span>
      </span>
      <span class="library-footer-streaming-text">
        <strong>{{ t("ui.footer.stream") }}</strong>
        <span>{{ t("ui.footer.toPhoneTablet") }}</span>
      </span>
    </button>
    <div class="library-update-status">
      <button
        class="library-update-icon"
        :class="{ available: updateStatus?.ok && updateStatus.available }"
        type="button"
        :disabled="updateDownloading || !(updateStatus?.ok && updateStatus.available)"
        :aria-label="t('ui.footer.downloadUpdate')"
        :title="t('ui.footer.downloadUpdate')"
        @click="startUpdateDownload()"
      >
        <IconArrowDown width="17" height="17" />
      </button>
      <div>
        <strong v-if="updateDownloading">Updating… {{ clampedProgress }}%</strong>
        <strong v-else-if="updateStatus?.ok && updateStatus.available">{{ t("ui.footer.updatedReady") }}</strong>
        <strong v-else>{{ t("ui.footer.upToDate") }}</strong>
        <span v-if="updateDownloading">{{ updateMessage || updateStatus?.latest_version || t("ui.footer.preparingUpdate") }}</span>
        <span v-else-if="updateStatus?.ok && updateStatus.available">v{{ updateStatus.latest_version }} ready</span>
        <span v-else>{{ t("ui.footer.everythingUpToDate") }}</span>
        <div v-if="updateDownloading" class="library-update-progress">
          <div class="library-update-progress-bar" :style="{ width: `${clampedProgress}%` }"></div>
        </div>
      </div>
    </div>
  </footer>
</template>

<style scoped>
.library-footer {
  flex: 0 0 68px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 22px;
  padding: 10px 26px;
  border-top: 1px solid var(--line);
  color: #d8dad9;
  background: #1b1e20;
  -webkit-app-region: drag;
}
.library-footer-status,
.library-update-status {
  display: flex;
  align-items: center;
  gap: 11px;
  min-width: 0;
}
.library-update-status {
  justify-content: flex-end;
}
.library-update-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  width: 30px;
  height: 30px;
  border: 1px solid rgba(231, 234, 236, 0.18);
  border-radius: 50%;
  color: #777d7b;
  background: #282c2d;
  cursor: default;
  -webkit-app-region: no-drag;
}
.library-update-icon.available {
  border-color: rgba(104, 205, 125, 0.65);
  color: #102315;
  background: #74d28a;
  box-shadow: 0 0 16px rgba(104, 205, 125, 0.28);
  cursor: pointer;
}
.library-update-icon:disabled {
  opacity: 0.72;
}
.library-footer-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  color: #74d28a;
}
.library-footer-streaming {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  padding: 7px 14px;
  border: 1px solid var(--line);
  border-radius: 10px;
  color: #d8dad9;
  background: rgba(255, 255, 255, 0.03);
  cursor: pointer;
  /* the footer is a window-drag region: without no-drag, macOS swallows
     every click on this button as a window move */
  -webkit-app-region: no-drag;
  transition:
    border-color 0.16s ease,
    background 0.16s ease;
}
.library-footer-streaming:hover {
  border-color: rgba(116, 210, 200, 0.45);
  background: rgba(116, 210, 200, 0.06);
}
.library-footer-streaming-icon {
  position: relative;
  display: inline-flex;
  color: #74d2c8;
}
.library-footer-streaming-wifi {
  position: absolute;
  right: -5px;
  bottom: -3px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 14px;
  height: 14px;
  border-radius: 50%;
  color: #0f1214;
  background: #74d2c8;
}
.library-footer-streaming-text {
  display: flex;
  flex-direction: column;
  text-align: left;
}
.library-footer-streaming-text strong {
  color: #e2e4e3;
  font-size: 12.5px;
  font-weight: 650;
}
.library-footer-streaming-text span {
  color: #8b9290;
  font-size: 10.5px;
}
.library-footer-status strong,
.library-update-status strong {
  display: block;
  color: #e2e4e3;
  font-size: 13px;
  font-weight: 650;
  line-height: 1.2;
}
.library-footer-status span:not(.library-footer-icon),
.library-update-status span:not(.library-footer-icon) {
  display: block;
  margin-top: 3px;
  color: #969b9a;
  font-size: 11px;
}
.library-update-progress {
  width: 190px;
  height: 5px;
  margin-top: 7px;
  border-radius: 999px;
  background: rgba(231, 234, 236, 0.14);
  overflow: hidden;
}
.library-update-progress-bar {
  height: 100%;
  border-radius: 999px;
  background: linear-gradient(90deg, #74d28a, #efcf9d);
  transition: width 0.4s ease;
}
</style>
