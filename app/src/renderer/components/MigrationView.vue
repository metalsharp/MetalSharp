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
  { name: "Preserving Settings" },
  { name: "Installing Update" },
  { name: "Updating Prefix" },
  { name: "Finishing up" },
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
      <div class="migration-foil" aria-hidden="true"></div>
      <div class="migration-content">
      <div class="migration-header">
        <div class="loading-icon" :class="{ complete, error: !!error }" aria-hidden="true">
          <img class="loading-icon-logo" src="../assets/metalsharp-logo.png" alt="" />
        </div>
        <h1 class="migration-title">MetalSharp Update Migration</h1>
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
      <p v-if="error" class="error-hint">Try restarting the app. If the issue persists, check the logs.</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
.migration-overlay {
  position: fixed;
  inset: 0;
  z-index: 9999;
  display: flex;
  align-items: center;
  justify-content: center;
  background:
    radial-gradient(ellipse 90% 70% at 50% 12%, rgba(95, 183, 232, 0.05), transparent 60%),
    #0e1218;
}

.migration-card {
  position: relative;
  overflow: hidden;
  text-align: center;
  width: min(560px, calc(100vw - 40px));
  padding: 40px 28px;
  background-color: #171d24;
  backdrop-filter: blur(42px) saturate(210%) brightness(1.12);
  -webkit-backdrop-filter: blur(42px) saturate(210%) brightness(1.12);
  border: 1px solid rgba(224, 232, 240, 0.22);
  border-radius: var(--radius-lg);
  box-shadow:
    inset 0 0 0 4px rgba(224, 232, 240, 0.4),
    inset 0 1px rgba(255, 255, 255, 0.09),
    inset 0 -1px rgba(255, 255, 255, 0.08),
    0 0 0 1px rgba(255, 255, 255, 0.04),
    0 0 24px rgba(200, 215, 228, 0.18),
    0 0 60px rgba(200, 215, 228, 0.1),
    0 24px 60px rgba(0, 0, 0, 0.5);
}

.migration-foil {
  position: absolute;
  inset: -8px;
  background: url("../assets/textures/metal-foil.png") center / cover;
  opacity: 0.55;
  filter: blur(5px);
}

.migration-content {
  position: relative;
}

.migration-header {
  margin-bottom: 32px;
}

.loading-icon {
  position: relative;
  width: 40px;
  height: 40px;
  margin: 0 auto 20px auto;
}

.loading-icon::before {
  content: "";
  position: absolute;
  inset: 0;
  border: 2px solid rgba(214, 226, 236, 0.16);
  border-top-color: #aebcc9;
  border-radius: 50%;
  animation: spin 0.9s linear infinite;
  box-shadow: 0 0 18px rgba(190, 205, 218, 0.12);
}

.loading-icon-logo {
  position: absolute;
  inset: 0;
  margin: auto;
  width: 21px;
  height: 21px;
  object-fit: contain;
}

.loading-icon.complete::before {
  animation: none;
  border-color: #4caf50;
  box-shadow: 0 0 22px rgba(76, 175, 80, 0.35);
}

.loading-icon.error::before {
  animation: none;
  border-color: rgba(239, 83, 80, 0.7);
  box-shadow: 0 0 24px rgba(239, 83, 80, 0.3);
}

.migration-title {
  font-family: var(--font-rethink);
  font-size: 24px;
  font-weight: 700;
  margin: 0 0 8px 0;
  background: linear-gradient(180deg, #f4f7fa 0%, #cdd6de 55%, #93a1ad 100%);
  -webkit-background-clip: text;
  background-clip: text;
  -webkit-text-fill-color: transparent;
  color: transparent;
}

.migration-subtitle {
  font-size: 14px;
  color: var(--text-dim);
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
  font-size: 10px;
  font-weight: 600;
  color: #9fc4dd;
  background-color: color-mix(in srgb, var(--bg-card) 45%, transparent);
  border: 1px solid rgba(159, 196, 221, 0.18);
  border-radius: 6px;
  padding: 4px 8px;
  transition: all 0.3s ease;
  white-space: nowrap;
  backdrop-filter: blur(12px);
  -webkit-backdrop-filter: blur(12px);
}

.pipeline-stage.active .stage-label {
  animation: pulse 2s ease-in-out infinite;
  background-color: color-mix(in srgb, var(--bg-card) 60%, transparent);
  border-color: rgba(159, 196, 221, 0.4);
  box-shadow: 0 0 14px rgba(159, 196, 221, 0.15);
}

@keyframes pulse {
  0%, 100% { opacity: 0.6; }
  50% { opacity: 1; }
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.pipeline-arrow {
  color: rgba(159, 196, 221, 0.3);
  margin: 0 4px;
}

.progress-section {
  margin-bottom: 20px;
}

.progress-bar-track {
  width: 100%;
  height: 6px;
  background: rgba(255, 255, 255, 0.06);
  border-radius: 3px;
  overflow: hidden;
  box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.4);
}

.progress-bar-fill {
  height: 100%;
  background: linear-gradient(180deg, #c7d4de 0%, #93a1ad 100%);
  border-radius: 3px;
  transition: width 0.4s ease;
  box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.3);
}

.progress-bar-fill.complete {
  background: linear-gradient(180deg, #6fd083 0%, #3d9c53 100%);
}

.progress-bar-fill.error {
  background: linear-gradient(180deg, #f2807e 0%, #c94a48 100%);
}

.progress-info {
  display: flex;
  justify-content: space-between;
  margin-top: 8px;
  font-size: 12px;
  color: rgba(255, 255, 255, 0.5);
}

.status-message {
  font-family: var(--font-rethink);
  font-size: 13px;
  color: rgba(255, 255, 255, 0.6);
  margin: 0 0 24px 0;
  min-height: 20px;
}

.status-message.complete {
  color: #4caf50;
}

.status-message.error {
  color: #f2a1a0;
  background: rgba(239, 83, 80, 0.07);
  border: 1px solid rgba(239, 83, 80, 0.25);
  border-radius: 8px;
  padding: 10px 14px;
  box-shadow: 0 0 22px rgba(239, 83, 80, 0.12);
}

.restart-btn {
  font-family: var(--font-rethink);
  background-color: #3a4149;
  background-image: linear-gradient(180deg, rgba(255, 255, 255, 0.1), rgba(255, 255, 255, 0) 45%);
  color: var(--text-primary);
  border: 2px solid rgba(214, 226, 236, 0.35);
  border-radius: 8px;
  padding: 10px 28px;
  font-size: 14px;
  font-weight: 600;
  cursor: pointer;
  box-shadow:
    inset 0 1px rgba(255, 255, 255, 0.1),
    inset 0 -1px rgba(255, 255, 255, 0.05),
    0 2px 6px rgba(0, 0, 0, 0.45),
    0 8px 20px rgba(0, 0, 0, 0.45),
    0 16px 38px rgba(0, 0, 0, 0.4);
  transition: background-color 0.2s, border-color 0.2s;
}

.restart-btn:hover:not(:disabled) {
  background-color: #454d56;
  border-color: rgba(214, 226, 236, 0.5);
}

.restart-btn:disabled {
  opacity: 0.6;
  cursor: default;
}

.error-hint {
  font-size: 12px;
  color: rgba(255, 255, 255, 0.35);
  margin-top: 12px;
}
</style>
