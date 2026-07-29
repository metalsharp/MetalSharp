<script setup lang="ts">
import { ref, computed, onMounted, onBeforeUnmount } from "vue";
import { getAPI, api } from "../composables/useApi";
import type {
  ProcessManagerAction,
  ProcessManagerActionResult,
  ProcessManagerProcess,
  ProcessManagerSample,
  MetalFxState,
} from "../api-types";

const sample = ref<ProcessManagerSample | null>(null);
const sampling = ref(true);
const gpuAccelArmed = ref(false);
const status = ref("Cmd+P toggles this overlay mid-session");
let pollTimer: ReturnType<typeof setInterval> | null = null;

// MetalFX Spatial Upscaling live toggle.
// DXMT reads DXMT_METALFX_SPATIAL_SWAPCHAIN at CreateSwapChain, so on/off applies
// on the game's next swapchain recreate (alt-enter / resolution change); the
// factor writes to dxmt.conf and applies on relaunch (Config is a cached singleton).
const metalfx = ref<MetalFxState | null>(null);
const metalfxBusy = ref(false);
const metalfxFactors = [1.5, 1.75, 2.0];

async function refreshMetalfx(): Promise<void> {
  try {
    metalfx.value = await api<MetalFxState>("GET", "/metalfx/state");
  } catch (e) {
    console.error("metalfx state failed", e);
  }
}

async function toggleMetalfx(enabled: boolean): Promise<void> {
  if (metalfxBusy.value) return;
  metalfxBusy.value = true;
  try {
    const factor = metalfx.value?.factor ?? 2.0;
    const res = await api<MetalFxState>("POST", "/metalfx/toggle", { enabled, factor });
    if (res) metalfx.value = res;
    status.value = res?.ok
      ? `MetalFX ${enabled ? "enabled" : "disabled"} — applies on next swapchain recreate (alt-enter/scene change)`
      : `MetalFX toggle failed: ${res?.error ?? "unknown"}`;
  } finally {
    metalfxBusy.value = false;
  }
}

async function setMetalfxFactor(factor: number): Promise<void> {
  if (metalfxBusy.value) return;
  metalfxBusy.value = true;
  try {
    const enabled = metalfx.value?.enabled ?? false;
    const res = await api<MetalFxState>("POST", "/metalfx/toggle", { enabled, factor });
    if (res) metalfx.value = res;
    status.value = res?.ok
      ? `MetalFX factor ${factor.toFixed(2)}× saved — applies on relaunch (DXMT Config reloads at launch)`
      : `MetalFX factor failed: ${res?.error ?? "unknown"}`;
  } finally {
    metalfxBusy.value = false;
  }
}

const ramPct = computed(() => {
  const s = sample.value;
  return s?.ram_total_bytes ? Math.max(0, Math.min(100, (s.ram_used_bytes / s.ram_total_bytes) * 100)) : 0;
});
const cpuPct = computed(() => Math.max(0, Math.min(100, sample.value?.cpu_percent ?? 0)));
const gpuPct = computed(() => Math.max(0, Math.min(100, sample.value?.gpu_percent ?? 0)));
const fpsLabel = computed(() => (sample.value?.fps == null ? "WAIT" : Math.round(sample.value.fps).toString()));
const tempLabel = computed(() => {
  const s = sample.value;
  if (s?.gpu_mem_used_bytes != null && s.gpu_mem_used_bytes > 0) {
    const gb = s.gpu_mem_used_bytes / (1024*1024*1024);
    return gb >= 1 ? `${gb.toFixed(1)}GB` : `${Math.round(s.gpu_mem_used_bytes/(1024*1024))}MB`;
  }
  return s?.cpu_temp_c != null ? `${Math.round(s.cpu_temp_c)}°C` : "VRAM";
});
const ramLabel = computed(() => {
  const s = sample.value;
  return s?.ram_total_bytes ? `${fmtBytes(s.ram_used_bytes)} / ${fmtBytes(s.ram_total_bytes)}` : "--";
});
const coresLabel = computed(() => {
  const s = sample.value;
  return s ? `${s.cores_used.toFixed(1)} / ${s.cores_total}` : "--";
});
const processes = computed<ProcessManagerProcess[]>(() => sample.value?.processes?.slice(0, 10) ?? []);

function fmtBytes(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return "--";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let n = value;
  let i = 0;
  while (n >= 1024 && i < units.length - 1) {
    n /= 1024;
    i += 1;
  }
  return `${n.toFixed(n >= 10 ? 1 : 2)} ${units[i]}`;
}

async function refresh(): Promise<void> {
  try {
    sample.value = await getAPI().processManagerSample();
  } catch (e) {
    console.error("process manager sample failed", e);
  } finally {
    sampling.value = false;
  }
}

async function close(): Promise<void> {
  try {
    await getAPI().processManagerClose();
  } catch (e) {
    console.error("process manager close failed", e);
  }
}

async function runAction(action: ProcessManagerAction): Promise<void> {
  if (action === "metalfx") {
    // Real toggle handled by toggleMetalfx; this branch is a no-op fallback.
    return;
  } else if (action === "gpu-acceleration") {
    gpuAccelArmed.value = !gpuAccelArmed.value;
    status.value = `GPU acceleration visual surface ${gpuAccelArmed.value ? "armed" : "parked"}; runtime hook pending`;
  } else if (action === "quit-game") {
    status.value = "Force-killing non-Steam Wine game PIDs...";
  } else {
    status.value = "Bringing Steam forward...";
  }
  let result: ProcessManagerActionResult | undefined;
  try {
    result = await getAPI().processManagerAction(action);
  } catch (e) {
    status.value = e instanceof Error ? e.message : String(e);
    return;
  }
  if (result?.ok) {
    if (action === "quit-game") {
      status.value = "Non-Steam Wine game kill signal sent";
      await refresh();
    }
  } else {
    status.value = result?.error ?? "Action failed";
  }
}

function onKey(e: KeyboardEvent): void {
  if (e.key === "Escape") void close();
}

onMounted(() => {
  document.body.classList.add("process-manager-overlay-body");
  window.addEventListener("keydown", onKey);
  void refresh();
  void refreshMetalfx();
  pollTimer = setInterval(() => void refresh(), 1500);
});

onBeforeUnmount(() => {
  if (pollTimer) clearInterval(pollTimer);
  window.removeEventListener("keydown", onKey);
  document.body.classList.remove("process-manager-overlay-body");
});
</script>

<template>
  <main class="pm-shell" aria-label="MetalSharp Process Manager">
    <section class="pm-panel">
      <header class="pm-header">
        <div class="pm-brand">
          <div class="pm-logo-wrap">
            <img src="../icon.png" alt="MetalSharp" />
          </div>
          <div>
            <p class="pm-kicker">LIVE SESSION HUD</p>
            <h1>MetalSharp Process Manager</h1>
          </div>
        </div>
        <button class="pm-close" type="button" title="Close" @click="close">×</button>
      </header>

      <div class="pm-grid">
        <article class="pm-stat pm-stat-fps">
          <span class="pm-label">Active FPS</span>
          <strong>{{ fpsLabel }}</strong>
          <small>{{
            sample?.fps == null ? "waiting for non-Steam Wine FPS" : sample?.fps_source ?? "frames/sec"
          }}</small>
        </article>
        <article class="pm-stat">
          <span class="pm-label">GPU Memory</span>
          <strong>{{ tempLabel }}</strong>
          <small>{{ sample?.gpu_mem_used_bytes ? "allocated VRAM" : sample?.cpu_temp_c == null ? "unavailable" : sample?.cpu_temp_source ?? "PMU sensor" }}</small>
        </article>
        <article class="pm-stat">
          <span class="pm-label">Cores Used</span>
          <strong>{{ coresLabel }}</strong>
          <small>{{ cpuPct.toFixed(1) }}% CPU</small>
          <div class="pm-meter">
            <span :style="{ width: cpuPct + '%' }"></span>
          </div>
        </article>
        <article class="pm-stat">
          <span class="pm-label">RAM Usage</span>
          <strong>{{ ramPct.toFixed(0) }}%</strong>
          <small>{{ ramLabel }}</small>
          <div class="pm-meter">
            <span :style="{ width: ramPct + '%' }"></span>
          </div>
        </article>
        <article class="pm-stat pm-stat-wide">
          <span class="pm-label">GPU Usage</span>
          <strong>{{ sample?.gpu_percent == null ? "SYS" : gpuPct.toFixed(0) + "%" }}</strong>
          <small>{{ sample?.gpu_label ?? "system GPU telemetry unavailable" }}</small>
          <div class="pm-meter pm-meter-magenta">
            <span :style="{ width: gpuPct + '%' }"></span>
          </div>
        </article>
      </div>

      <section class="pm-action-grid" aria-label="Process manager actions">
        <div class="pm-action pm-metalfx" :class="{ armed: metalfx?.enabled }">
          <span>MetalFX Spatial</span>
          <strong>{{ metalfx?.enabled ? "ON" : "OFF" }}</strong>
          <small>{{ metalfx?.enabled ? "upscaling armed" : "native res" }}</small>
          <div class="pm-metalfx-controls">
            <button
              class="pm-metalfx-toggle"
              type="button"
              :disabled="metalfxBusy"
              @click="toggleMetalfx(!(metalfx?.enabled ?? false))"
            >
              {{ metalfx?.enabled ? "Disable" : "Enable" }}
            </button>
            <select
              class="pm-metalfx-factor"
              :value="metalfx?.factor ?? 2.0"
              :disabled="metalfxBusy"
              @change="setMetalfxFactor(parseFloat(($event.target as HTMLSelectElement).value))"
            >
              <option v-for="f in metalfxFactors" :key="f" :value="f">{{ f.toFixed(2) }}×</option>
            </select>
          </div>
          <small class="pm-metalfx-note">on/off: next swapchain recreate · factor: relaunch</small>
        </div>
        <button class="pm-action danger" type="button" @click="runAction('quit-game')">
          <span>Quit Game</span>
          <strong>Force Kill</strong>
          <small>non-Steam Wine PIDs</small>
        </button>
        <button
          class="pm-action"
          :class="{ armed: gpuAccelArmed }"
          type="button"
          @click="runAction('gpu-acceleration')"
        >
          <span>GPU Acceleration</span>
          <strong>Planned</strong>
          <small>runtime control coming soon</small>
        </button>
      </section>

      <section class="pm-processes">
        <div class="pm-process-header">
          <div>
            <p class="pm-kicker">PROCESS VIEW</p>
            <h2>Wine / Steam Session</h2>
          </div>
          <span>{{ sampling ? "sampling" : processes.length + " rows" }}</span>
        </div>
        <div class="pm-process-list">
          <div v-if="processes.length === 0" class="pm-empty">
            No Wine/Steam/MetalSharp session processes detected yet.
          </div>
          <div v-for="p in processes" :key="p.pid" class="pm-process-row">
            <div>
              <strong>{{ p.name.split("/").pop() }}</strong>
              <small>pid {{ p.pid }} · {{ p.command }}</small>
            </div>
            <span>{{ p.fps_fresh && p.fps != null ? Math.round(p.fps) + " FPS" : "-- FPS" }}</span>
            <span>{{ p.cpu_percent.toFixed(1) }}% CPU</span>
            <span>{{ p.mem_percent.toFixed(1) }}% MEM</span>
          </div>
        </div>
      </section>

      <footer class="pm-footer">
        <span>{{ status }}</span>
        <span>{{ sample?.source ?? "waiting" }}</span>
      </footer>
    </section>
  </main>
</template>

<style scoped>
.pm-shell {
  width: 100vw;
  height: 100vh;
  color: var(--text-primary);
  background:
    radial-gradient(circle at 18% 0, #ff2ef738, #0000 36%),
    radial-gradient(circle at 88% 14%, #00f5ff2e, #0000 34%),
    radial-gradient(circle at 50% 100%, #b9ff4d21, #0000 42%);
  justify-content: center;
  align-items: center;
  padding: 12px;
  display: flex;
}
.pm-panel {
  border: 1px solid var(--border-strong);
  -webkit-backdrop-filter: blur(26px) saturate(170%);
  background: linear-gradient(140deg, #09070fd1, #131022bd 46%, #05232ca3), #09070fbd;
  border-radius: 16px;
  width: min(720px, 100vw - 18px);
  max-height: calc(100vh - 18px);
  overflow: auto;
  box-shadow: inset 0 0 0 1px #ffffff0d, 0 0 42px #b9ff4d1f, 0 30px 90px #00000094;
}
.pm-header {
  -webkit-app-region: drag;
  justify-content: space-between;
  align-items: center;
  gap: 20px;
  padding: 14px 18px 9px;
  display: flex;
}
.pm-brand {
  align-items: center;
  gap: 11px;
  display: flex;
}
.pm-logo-wrap {
  background: linear-gradient(135deg, #b9ff4d29, #00f5ff1f, #ff2ef724), #0d0b17d1;
  border: 1px solid #b9ff4d61;
  border-radius: 14px;
  place-items: center;
  width: 44px;
  height: 44px;
  display: grid;
  box-shadow: 0 0 24px #b9ff4d29;
}
.pm-logo-wrap img {
  object-fit: contain;
  width: 32px;
  height: 32px;
}
.pm-kicker {
  font-family: var(--font-mono);
  letter-spacing: 0.2em;
  color: var(--accent-hover);
  text-transform: uppercase;
  margin: 0 0 4px;
  font-size: 9px;
}
.pm-close {
  -webkit-app-region: no-drag;
  color: #ffd7df;
  cursor: pointer;
  background: #ff4f771a;
  border: 1px solid #ff4f7770;
  border-radius: 999px;
  width: 32px;
  height: 32px;
  font-size: 20px;
  line-height: 1;
}
.pm-grid {
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 9px;
  padding: 6px 18px 10px;
  display: grid;
}
.pm-stat {
  border: 1px solid var(--border);
  background: linear-gradient(#ffffff0e, #0000), #0d0b179e;
  border-radius: 16px;
  min-height: 84px;
  padding: 10px 12px;
  box-shadow: inset 0 0 0 1px #ffffff09;
}
.pm-stat-wide {
  grid-column: span 4;
  min-height: 64px;
}
.pm-label,
.pm-stat small {
  color: var(--text-secondary);
  font-size: 10px;
  display: block;
}
.pm-stat strong {
  font-family: var(--font-mono);
  color: var(--text-bright);
  margin: 5px 0 3px;
  font-size: 20px;
  display: block;
}
.pm-stat-fps strong {
  color: var(--accent);
}
.pm-meter {
  background: #00f5ff1c;
  border-radius: 999px;
  height: 5px;
  margin-top: 7px;
  overflow: hidden;
}
.pm-meter span {
  border-radius: inherit;
  background: linear-gradient(90deg, var(--accent), var(--accent-hover));
  height: 100%;
  display: block;
}
.pm-meter-magenta span {
  background: linear-gradient(90deg, var(--accent-dim), var(--accent-hover));
}
.pm-action-grid {
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 9px;
  padding: 0 18px 10px;
  display: grid;
}
.pm-action {
  min-height: 74px;
  color: var(--text-primary);
  text-align: left;
  cursor: pointer;
  transition:
    transform var(--transition),
    border-color var(--transition),
    box-shadow var(--transition);
  background: linear-gradient(135deg, #00f5ff1f, #ff2ef714), #0d0b17bd;
  border: 1px solid #00f5ff3d;
  border-radius: 16px;
  padding: 10px 12px;
}
.pm-action:hover,
.pm-action.armed {
  border-color: var(--accent);
  transform: translateY(-2px);
  box-shadow: 0 0 26px #b9ff4d29;
}
.pm-action span,
.pm-action small {
  color: var(--text-secondary);
  font-size: 10px;
  display: block;
}
.pm-action strong {
  color: var(--accent);
  margin: 5px 0;
  font-size: 14px;
  display: block;
}
.pm-action.danger {
  border-color: #ff4f7761;
}
.pm-action.danger strong {
  color: var(--error);
}
.pm-action.cyan strong {
  color: var(--accent-hover);
}
.pm-metalfx {
  text-align: left;
  cursor: default;
}
.pm-metalfx-controls {
  display: flex;
  gap: 6px;
  margin-top: 6px;
  align-items: center;
}
.pm-metalfx-toggle {
  cursor: pointer;
  color: var(--text-bright);
  background: linear-gradient(135deg, #b9ff4d29, #00f5ff1f), #0d0b17bd;
  border: 1px solid var(--accent);
  border-radius: 10px;
  padding: 4px 10px;
  font-size: 11px;
  font-family: var(--font-mono);
}
.pm-metalfx-toggle:disabled {
  opacity: 0.5;
  cursor: progress;
}
.pm-metalfx-factor {
  cursor: pointer;
  color: var(--text-bright);
  background: #0d0b17bd;
  border: 1px solid var(--border-strong);
  border-radius: 10px;
  padding: 4px 6px;
  font-size: 11px;
  font-family: var(--font-mono);
}
.pm-metalfx-factor:disabled {
  opacity: 0.5;
  cursor: progress;
}
.pm-metalfx-note {
  margin-top: 6px;
  color: var(--text-dim);
}
.pm-processes {
  border: 1px solid var(--border);
  background: #09070f75;
  border-radius: 16px;
  margin: 0 18px 10px;
  overflow: hidden;
}
.pm-process-header {
  border-bottom: 1px solid var(--border);
  justify-content: space-between;
  align-items: center;
  padding: 10px 12px;
  display: flex;
}
.pm-process-header > span,
.pm-footer {
  color: var(--text-dim);
  font-family: var(--font-mono);
  font-size: 10px;
}
.pm-process-list {
  max-height: 96px;
  overflow: auto;
}
.pm-process-row,
.pm-empty {
  font-family: var(--font-mono);
  border-bottom: 1px solid #00f5ff14;
  grid-template-columns: minmax(0, 1fr) 64px 64px 64px;
  align-items: center;
  gap: 8px;
  padding: 7px 10px;
  font-size: 10px;
  display: grid;
}
.pm-process-row strong,
.pm-process-row small {
  text-overflow: ellipsis;
  white-space: nowrap;
  display: block;
  overflow: hidden;
}
.pm-process-row strong {
  color: var(--text-primary);
}
.pm-process-row small,
.pm-empty {
  color: var(--text-dim);
}
.pm-empty {
  display: block;
}
.pm-footer {
  justify-content: space-between;
  gap: 12px;
  padding: 0 18px 12px;
  display: flex;
}
@media (width <= 760px) {
  .pm-grid,
  .pm-action-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
  .pm-stat-wide {
    grid-column: span 2;
  }
  h1 {
    font-size: 13px;
  }
}
</style>
