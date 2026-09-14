<script setup lang="ts">
import { computed, inject, type Ref } from "vue";
import IconArrowDown from "~icons/lucide/arrow-down-to-line";
import IconCheckCircle from "~icons/lucide/check-circle-2";
import type { UpdateStatus } from "../api-types";

const library = inject<Ref<{ total: number; installed_count: number } | null>>("library")!;
const backendConnected = inject<Ref<boolean>>("backendConnected")!;
const backendVersion = inject<Ref<string | null>>("backendVersion")!;
const updateStatus = inject<Ref<UpdateStatus | null>>("updateStatus")!;
const updateDownloading = inject<Ref<boolean>>("updateDownloading")!;
const startUpdateDownload = inject<() => void>("startUpdateDownload")!;

const gameCount = computed(() => library.value?.total ?? 0);
const installedCount = computed(() => library.value?.installed_count ?? 0);
const readyLabel = computed(() => (backendConnected.value ? "All games ready" : "Preview library"));
const readyDetail = computed(() =>
  backendConnected.value
    ? "You're all set to play"
    : backendVersion.value
      ? `Backend v${backendVersion.value}`
      : "Connect MetalSharp to play",
);
</script>

<template>
  <footer class="library-footer">
    <div class="library-footer-status">
      <span class="library-footer-icon"><IconCheckCircle width="20" height="20" /></span>
      <div>
        <strong>{{ readyLabel }}</strong>
        <span>{{ installedCount }} of {{ gameCount }} games · {{ readyDetail }}</span>
      </div>
    </div>
    <div class="library-update-status">
      <button
        class="library-update-icon"
        :class="{ available: updateStatus?.ok && updateStatus.available }"
        type="button"
        :disabled="updateDownloading || !(updateStatus?.ok && updateStatus.available)"
        aria-label="Download update"
        title="Download update"
        @click="startUpdateDownload()"
      >
        <IconArrowDown width="17" height="17" />
      </button>
      <div>
        <strong v-if="updateDownloading">Updating…</strong>
        <strong v-else-if="updateStatus?.ok && updateStatus.available">Updated Ready: Download Now?</strong>
        <strong v-else>Up To Date</strong>
        <span v-if="updateDownloading">{{ updateStatus?.latest_version || "Preparing update" }}</span>
        <span v-else-if="updateStatus?.ok && updateStatus.available">v{{ updateStatus.latest_version }} ready</span>
        <span v-else>Everything's up to date</span>
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
</style>
