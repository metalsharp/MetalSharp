<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed } from "vue";
import IconArrowRight from "~icons/lucide/arrow-right";

const status = ref("idle");
const step = ref(0);
const total = ref(0);
const message = ref("Checking migration status...");
const error = ref<string | null>(null);
const complete = ref(false);
const launching = ref(false);

let pollTimer: ReturnType<typeof setInterval> | null = null;

const percent = computed(() => {
  if (total.value === 0) return 0;
  return Math.round((step.value / total.value) * 100);
});

const stages = [
  { name: "[Data]" },
  { name: "[Runtime]" },
  { name: "[Prefixes]" },
  { name: "[Verify]" },
];

const MAX_START_RETRIES = 20;
const START_RETRY_DELAY_MS = 500;

async function startMigration(retriesLeft = MAX_START_RETRIES) {
  try {
    const res = await window.metalsharp.migrateStart();
    if (res?.ok) {
      message.value = "Migration started...";
      startPolling();
    } else if (res?.error?.includes("migration already in progress")) {
      message.value = "Migration already running...";
      startPolling();
    } else {
      const errorText = res?.error ?? "Failed to start migration";
      if (retriesLeft > 0 && shouldRetryBackendError(errorText)) {
        message.value = "Waiting for backend to start...";
        await new Promise((r) => setTimeout(r, START_RETRY_DELAY_MS));
        await startMigration(retriesLeft - 1);
      } else {
        error.value = errorText;
        message.value = `Error: ${error.value}`;
      }
    }
  } catch (e: unknown) {
    const errorText = e instanceof Error ? e.message : "Network error";
    if (retriesLeft > 0 && shouldRetryBackendError(errorText)) {
      message.value = "Waiting for backend to start...";
      await new Promise((r) => setTimeout(r, START_RETRY_DELAY_MS));
      await startMigration(retriesLeft - 1);
    } else {
      error.value = errorText;
      message.value = `Error: ${error.value}`;
    }
  }
}

function shouldRetryBackendError(errorText: string) {
  return (
    errorText.includes("ECONNREFUSED") ||
    errorText.includes("timeout") ||
    errorText.includes("did not start in time") ||
    errorText.includes("Migration backend unavailable")
  );
}

async function pollProgress() {
  try {
    const res = await window.metalsharp.migrateProgress();
    if (!res) return;
    const data = res.data ?? res;
    status.value = data.status ?? "idle";
    step.value = data.step ?? 0;
    total.value = data.total ?? 0;
    message.value = data.message ?? "";
    error.value = data.error ?? null;

    if (status.value === "complete") {
      complete.value = true;
      stopPolling();
    } else if (status.value === "error") {
      stopPolling();
    }
  } catch {}
}

function startPolling() {
  pollTimer = setInterval(pollProgress, 500);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

async function retryMigration() {
  error.value = null;
  complete.value = false;
  step.value = 0;
  total.value = 0;
  message.value = "Retrying migration from the last safe checkpoint...";
  await startMigration();
}

async function restartApp() {
  launching.value = true;
  message.value = "Closing old MetalSharp, stopping the backend, and launching the updated app...";
  const result = await window.metalsharp.restartAfterMigration();
  if (!result?.ok) {
    launching.value = false;
    error.value = result?.error ?? "Failed to launch the updated MetalSharp app";
    message.value = `Error: ${error.value}`;
  }
}

onMounted(async () => {
  await startMigration();
});

onUnmounted(() => {
  stopPolling();
});
</script>

<template>
  <div class="migration-overlay">
    <div class="migration-card">
      <div class="migration-header">
        <div class="loading-icon" :class="{ complete, error: !!error }" aria-hidden="true" />
        <h1 class="migration-title">MetalSharp Update Migration</h1>
        <p class="migration-subtitle">Preserving your settings and refreshing the runtime</p>
      </div>

      <div class="pipeline-vis">
        <div v-for="(stage, i) in stages" :key="stage.name" class="pipeline-stage" :class="{ active: !complete && !error }">
          <span class="stage-label">{{ stage.name }}</span>
          <div v-if="i < stages.length - 1" class="pipeline-arrow">
            <IconArrowRight width="16" height="12" />
          </div>
        </div>
      </div>

      <div class="progress-section">
        <div class="progress-bar-track">
          <div class="progress-bar-fill" :style="{ width: percent + '%' }" :class="{ complete, error: !!error }" />
        </div>
        <div class="progress-info">
          <span class="progress-percent">{{ percent }}%</span>
          <span v-if="total > 0" class="progress-step">Step {{ step }}/{{ total }}</span>
        </div>
      </div>

      <p class="status-message" :class="{ error: !!error, complete }">{{ message }}</p>

      <button v-if="complete" class="restart-btn" :disabled="launching" @click="restartApp()">
        {{ launching ? "Launching..." : "Launch MetalSharp" }}
      </button>
      <button v-if="error" class="restart-btn" @click="retryMigration()">Retry Migration</button>
      <p v-if="error" class="error-hint">Your Steam installation and user data remain preserved at the last safe checkpoint.</p>
    </div>
  </div>
</template>

<style scoped>
.migration-overlay {
  position: fixed;
  inset: 0;
  background: #1b2838;
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 9999;
}

.migration-card {
  text-align: center;
  width: min(560px, calc(100vw - 40px));
  padding: 40px 28px;
}

.migration-header {
  margin-bottom: 32px;
}

.loading-icon {
  width: 34px;
  height: 34px;
  margin: 0 auto 18px auto;
  border: 2px solid rgba(102, 192, 244, 0.18);
  border-top-color: #66c0f4;
  border-radius: 50%;
  animation: spin 0.9s linear infinite;
}

.loading-icon.complete {
  animation: none;
  border-color: #4caf50;
}

.loading-icon.error {
  animation: none;
  border-color: #ef5350;
}

.migration-title {
  font-size: 24px;
  font-weight: 700;
  color: #fff;
  margin: 0 0 8px 0;
}

.migration-subtitle {
  font-size: 14px;
  color: #66c0f4;
  opacity: 0.8;
  margin: 0;
}

.pipeline-vis {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 0;
  margin-bottom: 32px;
  flex-wrap: nowrap;
}

.pipeline-stage {
  display: flex;
  align-items: center;
  gap: 0;
}

.stage-label {
  font-family: "SF Mono", "Menlo", monospace;
  font-size: 11px;
  font-weight: 600;
  color: #66c0f4;
  background: rgba(102, 192, 244, 0.08);
  border: 1px solid rgba(102, 192, 244, 0.2);
  border-radius: 6px;
  padding: 4px 10px;
  transition: all 0.3s ease;
  white-space: nowrap;
}

.pipeline-stage.active .stage-label {
  animation: pulse 2s ease-in-out infinite;
  background: rgba(102, 192, 244, 0.15);
  border-color: rgba(102, 192, 244, 0.4);
}

@keyframes pulse {
  0%, 100% { opacity: 0.6; }
  50% { opacity: 1; }
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.pipeline-arrow {
  color: rgba(102, 192, 244, 0.3);
  margin: 0 4px;
}

.progress-section {
  margin-bottom: 16px;
}

.progress-bar-track {
  width: 100%;
  height: 6px;
  background: rgba(255, 255, 255, 0.06);
  border-radius: 3px;
  overflow: hidden;
}

.progress-bar-fill {
  height: 100%;
  background: #66c0f4;
  border-radius: 3px;
  transition: width 0.4s ease;
}

.progress-bar-fill.complete {
  background: #4caf50;
}

.progress-bar-fill.error {
  background: #ef5350;
}

.progress-info {
  display: flex;
  justify-content: space-between;
  margin-top: 8px;
  font-size: 12px;
  color: rgba(255, 255, 255, 0.5);
}

.status-message {
  font-size: 13px;
  color: rgba(255, 255, 255, 0.6);
  margin: 0 0 24px 0;
  min-height: 20px;
}

.status-message.complete {
  color: #4caf50;
}

.status-message.error {
  color: #ef5350;
}

.restart-btn {
  background: #66c0f4;
  color: #1b2838;
  border: none;
  border-radius: 8px;
  padding: 10px 28px;
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  transition: background 0.2s;
}

.restart-btn:hover {
  background: #4db8e8;
}

.error-hint {
  font-size: 12px;
  color: rgba(255, 255, 255, 0.3);
  margin-top: 12px;
}
</style>
