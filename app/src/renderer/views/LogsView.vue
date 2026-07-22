<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick } from "vue";
import { api, getAPI } from "../composables/useApi";

const logs = ref<string[]>([]);
const logFiles = ref<{ name: string; lines: string[] }[]>([]);
const crashReports = ref<
  { file: string; name: string; source: string; pipeline: string; timestamp: string; size_bytes: number }[]
>([]);
const logLineCount = ref(0);
const activeLogName = ref("");
const logContentEl = ref<HTMLElement | null>(null);
const activeDrawer = ref<"live" | "crash" | "recent" | null>(null);
const crashReportLines = ref<Record<string, string[]>>({});
const crashReportLoading = ref<Record<string, boolean>>({});
let pollInterval: ReturnType<typeof setInterval> | null = null;
const LIVE_LOG_LINE_LIMIT = 1000;

function appendLiveLogLines(lines: string[]) {
  if (!lines.length) return;
  const next = [...logs.value, ...lines];
  const linesAfterLastThreshold = next.length % LIVE_LOG_LINE_LIMIT;
  logs.value = linesAfterLastThreshold === 0 ? [] : next.slice(-linesAfterLastThreshold);
}

const pipelineOrder = ["M12", "M11", "M9", "FNA/Mono", "System", "Other"];
const crashByPipeline = computed(() => {
  const groups: Record<string, typeof crashReports.value> = {};
  for (const r of crashReports.value) {
    const key = r.pipeline || "Other";
    (groups[key] ??= []).push(r);
  }
  return pipelineOrder.filter((p) => groups[p]?.length).map((p) => ({ pipeline: p, reports: groups[p] }));
});

function scrollToBottom() {
  nextTick(() => {
    if (logContentEl.value) logContentEl.value.scrollTop = logContentEl.value.scrollHeight;
  });
}

function toggleDrawer(drawer: "live" | "crash" | "recent") {
  activeDrawer.value = activeDrawer.value === drawer ? null : drawer;
  if (activeDrawer.value === "live") scrollToBottom();
}

async function pollLogs() {
  const result = await api<{ ok: boolean; name?: string; total: number; lines: string[] }>(
    "GET",
    `/logs/stream?after=${logLineCount.value}`,
  );
  if (result?.ok && result.name) activeLogName.value = result.name;
  if (result?.ok && result.lines?.length) {
    appendLiveLogLines(result.lines);
    if (activeDrawer.value === "live") scrollToBottom();
  }
  if (result?.ok) logLineCount.value = result.total;
}

async function loadCrashReports() {
  const result = await api<{
    ok: boolean;
    reports: { file: string; name: string; source: string; pipeline: string; timestamp: string; size_bytes: number }[];
  }>("GET", "/logs/crash-reports");
  if (!result?.ok || !result.reports?.length) return;
  crashReports.value = result.reports.slice(0, 20);
}

async function loadLogFiles() {
  const result = await api<{ ok: boolean; logs: { name: string; lines: string[] }[] }>("GET", "/logs");
  if (result?.ok) logFiles.value = result.logs.filter((entry) => entry.name !== activeLogName.value);
}

async function loadCrashReport(file: string) {
  if (crashReportLines.value[file] || crashReportLoading.value[file]) return;
  crashReportLoading.value[file] = true;
  const result = await api<{ ok: boolean; lines?: string[]; error?: string }>("POST", "/logs/crash-report", { file });
  crashReportLines.value[file] = result?.ok
    ? (result.lines ?? ["No readable crash details found."])
    : [result?.error ?? "Unable to read crash report."];
  crashReportLoading.value[file] = false;
}

function onCrashReportToggle(event: Event, file: string) {
  if ((event.currentTarget as HTMLDetailsElement).open) void loadCrashReport(file);
}

function clearView() {
  logs.value = [];
  logFiles.value = [];
  crashReports.value = [];
  crashReportLines.value = {};
  logLineCount.value = 0;
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
      <div class="logs-header-drag-region">
        <h1>Logs</h1>
        <p class="subtitle">Live MetalSharp runtime logs</p>
      </div>
      <div class="logs-actions">
        <button class="btn btn-secondary" @click="openLogFolder">Open Logs</button>
        <button class="btn btn-secondary" @click="clearView">Clear View</button>
      </div>
    </div>
    <div class="logs-body view-body-surface">
      <div class="log-selectors">
        <button class="log-selector" :class="{ active: activeDrawer === 'live' }" @click="toggleDrawer('live')">
          <span>Live log stream</span>
          <small>{{ logs.length }} / {{ LIVE_LOG_LINE_LIMIT }}</small>
          <span class="log-selector-arrow">{{ activeDrawer === "live" ? "v" : ">" }}</span>
        </button>
        <button class="log-selector" :class="{ active: activeDrawer === 'crash' }" @click="toggleDrawer('crash')">
          <span>Crash reports</span>
          <small>{{ crashReports.length }}</small>
          <span class="log-selector-arrow">{{ activeDrawer === "crash" ? "v" : ">" }}</span>
        </button>
        <button class="log-selector" :class="{ active: activeDrawer === 'recent' }" @click="toggleDrawer('recent')">
          <span>Recent log files</span>
          <small>{{ logFiles.length }}</small>
          <span class="log-selector-arrow">{{ activeDrawer === "recent" ? "v" : ">" }}</span>
        </button>
      </div>

      <div v-if="activeDrawer" class="log-panel">
        <div v-if="activeDrawer === 'live'" class="log-content" ref="logContentEl">
          <div v-for="(line, i) in logs" :key="i" class="log-line" :class="logClass(line)">
            {{ line }}
          </div>
        </div>
        <div v-else-if="activeDrawer === 'crash'" class="log-panel-body">
          <div v-if="crashByPipeline.length">
            <div v-for="group in crashByPipeline" :key="group.pipeline" class="crash-pipeline-section">
              <div class="crash-pipeline-label">{{ group.pipeline }}</div>
              <details
                v-for="report in group.reports"
                :key="report.file"
                class="report-row crash-report-drawer"
                @toggle="onCrashReportToggle($event, report.file)"
              >
                <summary>
                  <span class="report-summary-copy">
                    <strong>{{ report.name }}</strong>
                    <span>{{ report.source }} - {{ report.timestamp }} - {{ formatBytes(report.size_bytes) }}</span>
                    <small>{{ report.file }}</small>
                  </span>
                  <span class="nested-drawer-arrow">&gt;</span>
                </summary>
                <pre class="crash-log-content">{{
                  crashReportLoading[report.file]
                    ? "Loading crash details..."
                    : (crashReportLines[report.file] ?? []).join("\n")
                }}</pre>
              </details>
            </div>
          </div>
          <div v-else class="crash-empty">No crash reports found.</div>
        </div>
        <div v-else class="log-panel-body">
          <details v-for="entry in logFiles" :key="entry.name" class="file-log">
            <summary class="file-log-name">
              <span>{{ entry.name }}</span>
              <span class="nested-drawer-arrow">&gt;</span>
            </summary>
            <pre>{{ entry.lines.slice(-40).join("\n") }}</pre>
          </details>
          <div v-if="!logFiles.length" class="crash-empty">No recent log files found.</div>
        </div>
      </div>
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
  padding: 0 28px;
  height: 100%;
  min-height: 0;
  display: flex;
  flex-direction: column;
  overflow-y: auto;
}
.logs-body {
  flex: 1;
  min-height: 0;
  margin: 0 -28px;
  padding: 16px 28px 24px;
}
.logs-header {
  flex-shrink: 0;
  height: 160px;
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  margin: 0 -28px;
  padding: 44px 28px 14px;
  border-bottom: 1px solid var(--border);
  -webkit-app-region: drag;
  position: relative;
  overflow: hidden;
}
.logs-header::after {
  content: "";
  position: absolute;
  inset: 0;
  background:
    radial-gradient(ellipse 60% 80% at 20% 50%, rgba(95, 183, 232, 0.08) 0%, transparent 70%),
    radial-gradient(ellipse 40% 60% at 80% 50%, rgba(95, 183, 232, 0.05) 0%, transparent 60%);
  pointer-events: none;
}
.logs-header h1 {
  font-size: 24px;
  font-weight: 750;
  line-height: 1.1;
}
.logs-header-drag-region {
  position: relative;
  z-index: 1;
  flex: 1;
  align-self: stretch;
  min-width: 0;
  user-select: none;
  -webkit-app-region: drag;
}
.subtitle {
  font-size: 12px;
  color: var(--text-dim);
  margin-top: 6px;
}

@media (max-width: 1040px) {
  .logs-header {
    height: 202px;
  }
}
.logs-actions {
  position: relative;
  z-index: 1;
  display: flex;
  gap: 8px;
  -webkit-app-region: no-drag;
}

.log-selectors {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 10px;
}
.log-selector {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto auto;
  align-items: center;
  gap: 10px;
  min-width: 0;
  padding: 10px 12px;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
  color: var(--text-secondary);
  cursor: pointer;
  font-size: 12px;
  font-weight: 700;
  text-align: left;
}
.log-selector:hover,
.log-selector.active {
  border-color: var(--border-strong);
  background: color-mix(in srgb, var(--accent) 8%, var(--bg-surface));
}
.log-selector:focus-visible {
  outline: 2px solid var(--accent);
  outline-offset: 1px;
}
.log-selector > span:first-child {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
}
.log-selector small,
.log-selector-arrow {
  color: var(--text-dim);
  font-weight: 500;
}
.log-selector-arrow {
  width: 8px;
  text-align: center;
}
.log-panel {
  min-height: 0;
  max-height: min(54vh, 560px);
  margin-top: 10px;
  overflow: hidden;
  border: 1px solid var(--border);
  border-radius: var(--radius-md);
  background: var(--bg-surface);
  color: var(--text-secondary);
}
.log-panel-body {
  max-height: min(54vh, 560px);
  overflow: auto;
}
.report-row {
  padding: 8px 12px;
  border-top: 1px solid var(--border);
  font-size: 12px;
  min-width: 0;
}
.report-row > summary,
.file-log > summary {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  cursor: pointer;
  list-style: none;
}
.report-row > summary::-webkit-details-marker,
.file-log > summary::-webkit-details-marker {
  display: none;
}
.report-summary-copy {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}
.nested-drawer-arrow {
  flex: 0 0 auto;
  color: var(--text-dim);
  transition: transform 0.15s ease;
}
details[open] > summary .nested-drawer-arrow {
  transform: rotate(90deg);
}
.report-row strong,
.report-row span,
.report-row small {
  min-width: 0;
  overflow-wrap: anywhere;
  word-break: break-word;
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
  color: var(--text-secondary);
  font-size: 12px;
  font-weight: 700;
}
.file-log[open] .file-log-name {
  margin-bottom: 8px;
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
.crash-log-content {
  max-height: 220px;
  overflow: auto;
  margin: 8px 0 0;
  padding: 10px;
  border: 1px solid var(--border);
  border-radius: var(--radius-sm);
  background: var(--bg-deep);
  color: var(--text-secondary);
  font-family: var(--font-mono);
  font-size: 10px;
  line-height: 1.5;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}

.log-content {
  max-height: min(54vh, 560px);
  overflow: auto;
  background: var(--bg-deep);
  padding: 14px 16px;
  font-family: var(--font-mono);
  font-size: 12px;
  line-height: 1.8;
  color: var(--text-secondary);
  white-space: pre-wrap;
}

@media (max-width: 760px) {
  .log-selectors {
    grid-template-columns: 1fr;
  }
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
