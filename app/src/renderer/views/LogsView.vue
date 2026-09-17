<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, nextTick, watch } from "vue";
import { api, getAPI } from "../composables/useApi";
import { useLibraryThemeStyle } from "../composables/useLibraryTheme";
import LibraryTopbar from "../components/LibraryTopbar.vue";
import LibraryFooter from "../components/LibraryFooter.vue";

const emit = defineEmits<{ navigate: [view: string] }>();
const libraryThemeStyle = useLibraryThemeStyle();

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
  if (target) target.open = !target.open;
}

const pipelineOrder = ["VKD3D", "M11", "M9", "FNA/Mono", "System", "Other"];
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
  <div class="logs-view" :style="libraryThemeStyle">
    <LibraryTopbar active-tab="logs" :show-search="false" @navigate="emit('navigate', $event)" />
    <div class="logs-header glass-header">
      <div class="logs-drag-strip" aria-hidden="true"></div>
      <div class="logs-title-row">
        <div>
          <span class="logs-eyebrow">METALSHARP · DIAGNOSTICS</span>
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

    <LibraryFooter />
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
/* Library-look diagnostics: dark surfaces + live theme accent via --library-* */
.logs-view {
  padding: 0;
  height: 100%;
  display: flex;
  flex-direction: column;
  background:
    radial-gradient(ellipse 70% 30% at 50% -6%, color-mix(in srgb, var(--library-accent) 9%, transparent), transparent 68%),
    linear-gradient(180deg, #171a1d 0%, #111416 46%, #111416 100%);
}
.logs-header {
  flex-shrink: 0;
  display: flex;
  flex-direction: column;
  gap: 14px;
  min-width: 0;
  margin: 0;
  padding: 26px 28px 16px;
  background: transparent;
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  position: relative;
  overflow: hidden;
}
.logs-header::after {
  display: none;
}
.logs-drag-strip {
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 44px;
  -webkit-app-region: drag;
}
.logs-title-row {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  min-width: 0;
  -webkit-app-region: drag;
}
.logs-title-row > div {
  min-width: 0;
}
.logs-eyebrow {
  display: block;
  margin-bottom: 10px;
  color: rgba(240, 239, 231, 0.6);
  font-family: Georgia, "Times New Roman", serif;
  font-size: 10px;
  letter-spacing: 0.52em;
  text-transform: uppercase;
}
.logs-header h1 {
  margin: 0;
  color: #eee9dd;
  font-family: Georgia, "Times New Roman", serif;
  font-size: clamp(34px, 3.6vw, 54px);
  font-weight: 500;
  line-height: 1;
  text-transform: uppercase;
  letter-spacing: 0.01em;
}
.subtitle {
  margin-top: 8px;
  color: #aeb3b2;
  font-size: 13.5px;
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
  gap: 8px;
}
.section-btn {
  display: inline-flex;
  align-items: center;
  min-height: 30px;
  padding: 0 13px;
  border: 1px solid var(--library-control-border);
  border-radius: 7px;
  color: var(--library-control-text);
  background: transparent;
  font: inherit;
  font-size: 12px;
  font-weight: 650;
  cursor: pointer;
  transition: border-color 0.16s ease, background 0.16s ease, transform 0.16s ease;
}
.section-btn:hover {
  border-color: var(--library-accent);
  background: color-mix(in srgb, var(--library-accent) 12%, transparent);
  transform: translateY(-1px);
}
.logs-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  -webkit-app-region: no-drag;
}
.logs-actions .btn,
.live-toolbar .btn {
  border: 1px solid var(--library-control-border);
  border-radius: 6px;
  color: var(--library-control-text);
  background: transparent;
  font-weight: 650;
}
.logs-actions .btn:hover:not(:disabled),
.live-toolbar .btn:hover:not(:disabled) {
  border-color: var(--library-accent);
  background: color-mix(in srgb, var(--library-accent) 12%, transparent);
}

.logs-view > .library-footer {
  margin-top: auto;
  position: sticky;
  bottom: 0;
  z-index: 20;
}
.log-drawers {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin: 16px 28px 0;
}
.log-drawer {
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 12px;
  background: rgba(255, 255, 255, 0.025);
  color: #cfd1d0;
  overflow: hidden;
}
.log-drawer summary {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  padding: 13px 16px;
  cursor: pointer;
  color: #eeeeed;
  font: inherit;
  font-size: 13px;
  font-weight: 700;
  letter-spacing: 0.02em;
  list-style: none;
  transition: background 0.16s ease;
}
.log-drawer summary:hover {
  background: rgba(255, 255, 255, 0.03);
}
.log-drawer summary::-webkit-details-marker {
  display: none;
}
.log-drawer summary::after {
  content: "v";
  color: rgba(255, 255, 255, 0.4);
  transition: transform 0.16s ease;
}
.log-drawer:not([open]) summary::after {
  transform: rotate(-90deg);
}
.log-drawer summary span {
  margin-left: auto;
  color: color-mix(in srgb, var(--library-accent) 80%, #fff);
  font-weight: 600;
}
.report-row {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 10px 16px;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
  font-size: 12px;
}
.report-row strong {
  color: #f0f0ed;
}
.report-row span,
.report-row small {
  color: #8f958f;
}
.report-row small {
  overflow-wrap: anywhere;
}
.crash-pipeline-section {
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}
.crash-pipeline-section:first-child {
  border-top: none;
}
.crash-pipeline-label {
  padding: 10px 16px 4px;
  color: var(--library-accent);
  font-size: 11px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.14em;
}
.crash-empty {
  padding: 18px 16px;
  color: #8f958f;
  font-size: 12px;
  text-align: center;
}
.file-log {
  padding: 12px 16px;
  border-top: 1px solid rgba(255, 255, 255, 0.06);
}
.file-log-name {
  margin-bottom: 8px;
  color: #e6e8e6;
  font-size: 12px;
  font-weight: 700;
}
.file-log pre {
  max-height: 180px;
  overflow: auto;
  margin: 0;
  padding: 12px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 8px;
  background: #0c0f11;
  color: #b9bfb9;
  font-family: "SF Mono", ui-monospace, Menlo, monospace;
  font-size: 10.5px;
  line-height: 1.6;
  white-space: pre-wrap;
}

.log-content {
  flex: 1;
  max-height: min(54vh, 560px);
  overflow: auto;
  background: #0c0f11;
  border-top: 1px solid rgba(255, 255, 255, 0.07);
  padding: 14px 16px;
  font-family: "SF Mono", ui-monospace, Menlo, monospace;
  font-size: 12px;
  line-height: 1.75;
  color: #c9ceca;
  white-space: pre-wrap;
}
.live-toolbar {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 12px;
  order: -1;
  border-top: 1px solid rgba(255, 255, 255, 0.07);
}
.live-log-drawer {
  min-height: 0;
  margin: 16px 28px 0;
  border-color: color-mix(in srgb, var(--library-accent) 40%, rgba(255, 255, 255, 0.08));
  box-shadow: 0 14px 40px rgba(0, 0, 0, 0.35);
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
  color: #7cbf6a;
}
.log-line.log-event-stop {
  color: #ffb84d;
}
.log-line.log-event-error {
  color: #ff5c5c;
}
.log-line.log-event-engine {
  color: var(--library-accent);
}
.log-line.log-event-warn {
  color: #ffb84d;
}
.logs-view ::-webkit-scrollbar-thumb {
  background: rgba(255, 255, 255, 0.14);
  border-radius: 999px;
}
</style>
