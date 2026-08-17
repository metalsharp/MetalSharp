<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from "vue";
import { api, getAPI } from "../composables/useApi";

const logs = ref<string[]>([]);
const logFiles = ref<{ name: string; lines: string[] }[]>([]);
const crashReports = ref<{ file: string; name: string; source: string; pipeline: string; timestamp: string; size_bytes: number }[]>([]);
const logLineCount = ref(0);
const logContentEl = ref<HTMLElement | null>(null);
const liveOpen = ref(false);
let pollInterval: ReturnType<typeof setInterval> | null = null;

const liveDrawerEl = ref<HTMLDetailsElement | null>(null);
const crashDrawerEl = ref<HTMLDetailsElement | null>(null);
const filesDrawerEl = ref<HTMLDetailsElement | null>(null);
function showSection(section: string) {
  const target =
    section === "live" ? liveDrawerEl.value : section === "crashes" ? crashDrawerEl.value : filesDrawerEl.value;
  if (target) target.open = true;
}

const pipelineOrder = ["M12", "M11", "M9", "FNA/Mono", "System", "Other"];
const crashByPipeline = computed(() => {
  const groups: Record<string, typeof crashReports.value> = {};
  for (const r of crashReports.value) {
    const key = r.pipeline || "Other";
    (groups[key] ??= []).push(r);
  }
  return pipelineOrder
    .filter((p) => groups[p]?.length)
    .map((p) => ({ pipeline: p, reports: groups[p] }));
});

function scrollToBottom() {
  nextTick(() => {
    if (logContentEl.value) logContentEl.value.scrollTop = logContentEl.value.scrollHeight;
  });
}

async function pollLogs() {
  const result = await api<{ ok: boolean; total: number; lines: string[] }>(
    "GET",
    `/logs/stream?after=${logLineCount.value}`,
  );
  if (result?.ok && result.lines?.length) {
    logs.value.push(...result.lines);
    if (liveOpen.value) scrollToBottom();
  }
  if (result?.ok) logLineCount.value = result.total;
}

async function loadCrashReports() {
  const result = await api<{
    ok: boolean;
    reports: { file: string; name: string; source: string; timestamp: string; size_bytes: number }[];
  }>("GET", "/logs/crash-reports");
  if (!result?.ok || !result.reports?.length) return;
  crashReports.value = result.reports.slice(0, 20);
}

async function loadLogFiles() {
  const result = await api<{ ok: boolean; logs: { name: string; lines: string[] }[] }>("GET", "/logs");
  if (result?.ok) logFiles.value = result.logs;
}

function clearView() {
  logs.value = [];
  logFiles.value = [];
  crashReports.value = [];
  logLineCount.value = 0;
}

function copyLiveLog() {
  const text = logs.value.join("\n");
  if (!text) return;
  navigator.clipboard.writeText(text).catch(() => {});
}

async function openLogFolder() {
  await getAPI().openLogsFolder();
}

onMounted(async () => {
  await pollLogs();
  await loadCrashReports();
  await loadLogFiles();
  pollInterval = setInterval(pollLogs, 2000);
});

onUnmounted(() => {
  if (pollInterval) clearInterval(pollInterval);
});
</script>

<template>
  <div class="logs-view">
    <div class="logs-header glass-header">
      <div class="logs-title-row">
        <div>
          <h1>Logs</h1>
          <p class="subtitle">Live MetalSharp runtime logs</p>
        </div>
      </div>
      <div class="logs-controls">
        <div class="logs-sections">
          <button class="section-btn" type="button" @click="showSection('live')">Live</button>
          <button class="section-btn" type="button" @click="showSection('crashes')">Crash Reports</button>
          <button class="section-btn" type="button" @click="showSection('files')">Log Files</button>
        </div>
        <div class="logs-actions">
          <button class="btn btn-secondary btn-sm" @click="openLogFolder">Open Logs</button>
          <button class="btn btn-secondary btn-sm" @click="copyLiveLog" :disabled="!logs.length">Copy</button>
          <button class="btn btn-secondary btn-sm" @click="clearView">Clear View</button>
        </div>
      </div>
    </div>
    <details ref="liveDrawerEl" class="log-drawer live-log-drawer" @toggle="liveOpen = ($event.target as HTMLDetailsElement).open; if (liveOpen) scrollToBottom()">
      <summary>
        Live log stream <span>{{ logs.length }} lines</span>
      </summary>
      <div class="live-toolbar">
        <button class="btn btn-secondary btn-sm" @click="copyLiveLog" :disabled="!logs.length">Copy</button>
      </div>
      <div class="log-content" ref="logContentEl">
        <div v-for="(line, i) in logs" :key="i" class="log-line" :class="logClass(line)">
          {{ line }}
        </div>
      </div>
    </details>
    <div class="log-drawers">
      <details ref="crashDrawerEl" class="log-drawer">
        <summary>
          Crash reports <span>{{ crashReports.length }}</span>
        </summary>
        <div v-if="crashByPipeline.length">
          <div v-for="group in crashByPipeline" :key="group.pipeline" class="crash-pipeline-section">
            <div class="crash-pipeline-label">{{ group.pipeline }}</div>
            <div v-for="report in group.reports" :key="report.file" class="report-row">
              <strong>{{ report.name }}</strong>
              <span>{{ report.source }} - {{ report.timestamp }} - {{ formatBytes(report.size_bytes) }}</span>
              <small>{{ report.file }}</small>
            </div>
          </div>
        </div>
        <div v-else class="crash-empty">No crash reports found.</div>
      </details>
      <details v-if="logFiles.length" ref="filesDrawerEl" class="log-drawer">
        <summary>
          Recent log files <span>{{ logFiles.length }}</span>
        </summary>
        <div v-for="entry in logFiles" :key="entry.name" class="file-log">
          <div class="file-log-name">{{ entry.name }}</div>
          <pre>{{ entry.lines.slice(-40).join("\n") }}</pre>
        </div>
      </details>
    </div>
  </div>
</template>

<script lang="ts">
export default {
  methods: {
    formatBytes(bytes: number): string {
      if (bytes < 1024) return `${bytes} B`;
      if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
      if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
      return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
    },
    logClass(line: string): string {
      if (line.includes("[LAUNCH]") || line.includes("[LAUNCHED]")) return "log-event-launch";
      if (line.includes("[STOP]") || line.includes("[STOPPED]")) return "log-event-stop";
      if (line.includes("[STOP FAILED]") || line.includes("[LAUNCH FAILED]")) return "log-event-error";
      if (line.includes("engine:")) return "log-event-engine";
      if (
        line.toLowerCase().includes("crash") ||
        line.toLowerCase().includes("error") ||
        line.toLowerCase().includes("failed")
      )
        return "log-event-warn";
      return "";
    },
  },
};
</script>

<style scoped>
.logs-view {
  padding: 0 28px 24px;
  height: 100%;
  display: flex;
  flex-direction: column;
}
.logs-header {
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  gap: 14px;
  min-width: 0;
  margin: 0 -28px;
  padding: 44px 28px 14px;
  border-bottom: 1px solid var(--border);
  -webkit-app-region: drag;
  position: relative;
  overflow: hidden;
}
.logs-title-row {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  min-width: 0;
}
.logs-title-row > div {
  min-width: 0;
}
.logs-controls {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
  -webkit-app-region: no-drag;
}
.logs-sections {
  display: flex;
  align-items: center;
  gap: 6px;
}
.section-btn {
  border: 1px solid var(--border);
  background: var(--bg-surface);
  color: var(--text-secondary);
  border-radius: var(--radius-sm);
  padding: 5px 11px;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: border-color 120ms ease, color 120ms ease;
}
.section-btn:hover {
  border-color: var(--accent);
  color: var(--text-primary);
}
.logs-header::after {
  content: "";
  position: absolute;
  inset: 0;
  background: radial-gradient(ellipse 60% 80% at 20% 50%, rgba(95, 183, 232, 0.08) 0%, transparent 70%),
              radial-gradient(ellipse 40% 60% at 80% 50%, rgba(95, 183, 232, 0.05) 0%, transparent 60%);
  pointer-events: none;
}
.logs-header h1 {
  font-size: 24px;
  font-weight: 750;
  line-height: 1.1;
}
.subtitle {
  font-size: 12px;
  color: var(--text-dim);
  margin-top: 2px;
}
.logs-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  -webkit-app-region: no-drag;
}

.log-drawers {
  display: flex;
  flex-direction: column;
  gap: 10px;
  margin-top: 12px;
}
.log-drawer {
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
  color: var(--text-secondary);
}
.log-drawer summary {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  padding: 10px 12px;
  cursor: pointer;
  font-size: 12px;
  font-weight: 700;
  list-style: none;
}
.log-drawer summary::-webkit-details-marker {
  display: none;
}
.log-drawer summary::after {
  content: "v";
  color: var(--text-dim);
  transition: transform 120ms ease;
}
.log-drawer:not([open]) summary::after {
  transform: rotate(-90deg);
}
.log-drawer summary span {
  margin-left: auto;
  color: var(--text-dim);
  font-weight: 500;
}
.report-row {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 8px 12px;
  border-top: 1px solid var(--border);
  font-size: 12px;
}
.report-row span,
.report-row small {
  color: var(--text-dim);
}
.report-row small {
  overflow-wrap: anywhere;
}
.crash-pipeline-section {
  border-top: 1px solid var(--border);
}
.crash-pipeline-section:first-child {
  border-top: none;
}
.crash-pipeline-label {
  padding: 8px 12px 4px;
  font-size: 11px;
  font-weight: 700;
  color: var(--accent);
  text-transform: uppercase;
  letter-spacing: 0.5px;
}
.crash-empty {
  padding: 16px 12px;
  color: var(--text-dim);
  font-size: 12px;
  text-align: center;
}
.file-log {
  padding: 10px 12px;
  border-top: 1px solid var(--border);
}
.file-log-name {
  margin-bottom: 6px;
  color: var(--text-secondary);
  font-size: 12px;
  font-weight: 700;
}
.file-log pre {
  max-height: 180px;
  overflow: auto;
  margin: 0;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--bg-deep);
  color: var(--text-secondary);
  font-family: var(--font-mono);
  font-size: 10px;
  line-height: 1.5;
  white-space: pre-wrap;
}

.log-content {
  flex: 1;
  max-height: min(54vh, 560px);
  overflow: auto;
  background: var(--bg-deep);
  border-top: 1px solid var(--border);
  padding: 14px 16px;
  font-family: var(--font-mono);
  font-size: 12px;
  line-height: 1.8;
  color: var(--text-secondary);
  white-space: pre-wrap;
}
.live-toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  order: -1;
  border-top: 1px solid var(--border);
}
.live-log-drawer {
  min-height: 0;
}
.live-log-drawer[open] {
  display: flex;
  flex-direction: column;
}
.live-log-drawer[open] .log-content {
  max-height: min(40vh, 380px);
}

.log-line {
  word-break: break-word;
}
.log-line.log-event-launch {
  color: var(--success);
}
.log-line.log-event-stop {
  color: var(--warn);
}
.log-line.log-event-error {
  color: var(--error);
}
.log-line.log-event-engine {
  color: var(--accent);
}
.log-line.log-event-warn {
  color: var(--warn);
}
</style>
