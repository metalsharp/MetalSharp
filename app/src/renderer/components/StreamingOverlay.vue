<script setup lang="ts">
import { computed, inject, onMounted, onUnmounted, ref } from "vue";
import { api } from "../composables/useApi";
import { useToast } from "../composables/useToast";
import IconX from "~icons/lucide/x";
import IconTv from "~icons/lucide/tv";
import IconPlay from "~icons/lucide/play";
import IconSquare from "~icons/lucide/square";
import IconDownload from "~icons/lucide/download";
import IconLink from "~icons/lucide/external-link";
import IconSmartphone from "~icons/lucide/smartphone";

const emit = defineEmits<{ close: [] }>();
const toast = useToast();
const config = inject<Ref<{ theme?: string } | null>>("config");

type StreamingStatus = {
  ok: boolean;
  installed: boolean;
  running: boolean;
  version: string;
  creds_configured: boolean;
  creds_valid: boolean;
  creds_username: string;
  web_url: string;
  installing: boolean;
  pairing_count: number;
  pairings_summary: string;
  progress_status: string | null;
  progress_detail: string | null;
};

const status = ref<StreamingStatus | null>(null);
const pin = ref("");
const pairing = ref(false);
const launching = ref(false);
const stopping = ref(false);
const installing = ref(false);
let pollTimer: ReturnType<typeof setInterval> | null = null;
let pollSession = 0;

const readyToPair = computed(() => !!status.value?.running && !!status.value?.creds_valid);

async function refresh(): Promise<StreamingStatus | null> {
  const s = await api<StreamingStatus>("GET", "/streaming/status");
  if (s?.ok) status.value = s;
  return s;
}

function beginPolling() {
  const session = ++pollSession;
  if (pollTimer) return;
  pollTimer = setInterval(async () => {
    if (session !== pollSession) {
      stopPolling();
      return;
    }
    const s = await refresh();
    if (!s) return;
    if (s.installing) return;
    // Terminal state reached: surface the outcome once.
    stopPolling();
    if (s.progress_status === "complete") {
      toast.show("Sunshine installed — launching it now", "success");
      await launch();
    } else if (s.progress_status === "error") {
      toast.show(s.progress_detail ?? "Sunshine installation failed", "error");
    }
  }, 1500);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

onMounted(() => {
  void refresh();
});
onUnmounted(() => {
  pollSession++;
  stopPolling();
});

async function install() {
  if (installing.value) return;
  installing.value = true;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/streaming/install", undefined, 30_000);
  if (!result?.ok) {
    installing.value = false;
    toast.show(result?.error ?? "Failed to start the Sunshine download", "error");
    return;
  }
  await refresh();
  beginPolling();
}

async function launch() {
  if (launching.value) return;
  launching.value = true;
  const result = await api<{ ok: boolean; error?: string; creds_recognized?: boolean }>(
    "POST",
    "/streaming/launch",
    undefined,
    90_000,
  );
  launching.value = false;
  if (!result?.ok) {
    toast.show(result?.error ?? "Could not launch Sunshine", "error");
    return;
  }
  if (result.creds_recognized === false) {
    toast.show(
      "Sunshine is running, but it uses different web credentials — manage them in the Sunshine web UI",
      "error",
    );
  } else {
    toast.show("Sunshine is running — pair your device below", "success");
  }
  await refresh();
}

async function stop() {
  if (stopping.value) return;
  stopping.value = true;
  await api("POST", "/streaming/stop");
  stopping.value = false;
  await refresh();
}

async function pair() {
  if (pairing.value) return;
  if (!/^\d{4}$/.test(pin.value)) {
    toast.show("Enter the 4-digit PIN shown in Moonlight", "error");
    return;
  }
  pairing.value = true;
  const result = await api<{ ok: boolean; error?: string }>("POST", "/streaming/pin", { pin: pin.value }, 95_000);
  pairing.value = false;
  if (result?.ok) {
    pin.value = "";
    toast.show("Paired! Open Moonlight on your device and tap your Mac to start streaming", "success");
  } else {
    toast.show(result?.error ?? "Pairing failed", "error");
  }
  await refresh();
}

async function unpairAll() {
  const result = await api<{ ok: boolean; error?: string }>("POST", "/streaming/unpair-all", undefined, 30_000);
  if (result?.ok) {
    toast.show("All paired devices removed", "success");
  } else {
    toast.show(result?.error ?? "Could not unpair devices", "error");
  }
  await refresh();
}

const stateLabel = computed(() => {
  if (!status.value) return "Checking…";
  if (status.value.installing) return status.value.progress_status ?? "Installing…";
  if (status.value.running) return "Running";
  if (status.value.installed) return "Installed — not running";
  return "Not installed";
});
</script>

<template>
  <div class="streaming-overlay" @click.self="emit('close')">
    <div class="streaming-panel" :data-theme="config?.theme === 'light' ? 'light' : 'dark'">
      <header class="streaming-header">
        <div class="streaming-header-title">
          <span class="streaming-header-icon"><IconTv width="20" height="20" /></span>
          <div>
            <h2>Game Streaming</h2>
            <p>Stream your MetalSharp games to a phone or tablet with Sunshine + Moonlight</p>
          </div>
        </div>
        <button class="streaming-close" type="button" aria-label="Close" @click="emit('close')">
          <IconX width="18" height="18" />
        </button>
      </header>

      <div class="streaming-body">
        <section class="streaming-card">
          <div class="streaming-card-head">
            <h3>Sunshine Host (this Mac)</h3>
            <span class="streaming-state" :class="{ running: status?.running, installed: status?.installed }">
              {{ stateLabel }}
            </span>
          </div>

          <p v-if="status && !status.installed && !status.installing" class="streaming-lede">
            Sunshine captures this Mac's screen and streams it over your local network. Install it once — about a
            40&nbsp;MB download from LizardByte.
          </p>
          <p v-else-if="status?.installing" class="streaming-lede">
            {{ status.progress_detail ?? "Downloading Sunshine…" }}
          </p>

          <div class="streaming-actions">
            <button
              v-if="status && !status.installed && !status.installing"
              class="streaming-btn primary"
              type="button"
              @click="install()"
            >
              <IconDownload width="15" height="15" /> Install Sunshine
            </button>
            <button
              v-if="status?.installed && !status.running"
              class="streaming-btn primary"
              type="button"
              :disabled="launching"
              @click="launch()"
            >
              <IconPlay width="15" height="15" /> {{ launching ? "Starting…" : "Start Streaming Host" }}
            </button>
            <button v-if="status?.running" class="streaming-btn" type="button" :disabled="stopping" @click="stop()">
              <IconSquare width="13" height="13" /> {{ stopping ? "Stopping…" : "Stop" }}
            </button>
            <a
              v-if="status?.running"
              class="streaming-btn ghost"
              :href="status.web_url"
              target="_blank"
              rel="noreferrer"
            >
              <IconLink width="13" height="13" /> Sunshine Web UI
            </a>
          </div>
          <p v-if="status?.installed" class="streaming-note">Version {{ status.version }} · {{ status.web_url }}</p>
        </section>

        <section class="streaming-card" :class="{ dimmed: !readyToPair }">
          <div class="streaming-card-head">
            <h3>Pair your device</h3>
            <span v-if="status?.pairing_count" class="streaming-state running">
              {{ status.pairing_count }} waiting: {{ status.pairings_summary }}
            </span>
          </div>

          <ol class="streaming-steps">
            <li>
              Install <strong>Moonlight</strong> on your phone or tablet —
              <a
                href="https://apps.apple.com/app/moonlight-game-streaming/id1000551566"
                target="_blank"
                rel="noreferrer"
                >App Store</a
              >
              /
              <a href="https://play.google.com/store/apps/details?id=com.limelight" target="_blank" rel="noreferrer"
                >Google Play</a
              >
              (<IconSmartphone width="11" height="11" />{{ " " }}works best on the same Wi-Fi).
            </li>
            <li>Start playing your game in MetalSharp on this Mac.</li>
            <li>Open Moonlight and tap this Mac — it shows a <strong>4-digit PIN</strong>.</li>
            <li>Enter the PIN below to pair, then tap the game in Moonlight to start streaming.</li>
          </ol>

          <div class="streaming-pin-row">
            <input
              v-model="pin"
              class="streaming-pin-input"
              type="text"
              inputmode="numeric"
              maxlength="4"
              placeholder="PIN"
              :disabled="!readyToPair || pairing"
              @keyup.enter="pair()"
            />
            <button class="streaming-btn primary" type="button" :disabled="!readyToPair || pairing" @click="pair()">
              {{ pairing ? "Pairing…" : "Pair Device" }}
            </button>
          </div>
          <p v-if="status?.running && !status.creds_valid" class="streaming-warn">
            Sunshine is running with web credentials MetalSharp doesn't know. Set them in the Sunshine Web UI, or delete
            <code>streaming-creds.json</code> in your MetalSharp folder and relaunch streaming.
          </p>
        </section>

        <section class="streaming-card">
          <div class="streaming-card-head">
            <h3>Good to know</h3>
          </div>
          <ul class="streaming-notes">
            <li>Sunshine on macOS is experimental: gamepads aren't supported yet — use touch controls in Moonlight.</li>
            <li>
              macOS asks for <strong>Screen Recording</strong> permission the first time you stream. Approve it once.
            </li>
            <li>Keep both devices on the same network; ports 47984–48010 must be reachable (firewall may prompt).</li>
          </ul>
          <button
            v-if="status?.installed"
            class="streaming-btn danger"
            type="button"
            title="Remove all paired devices from Sunshine"
            @click="unpairAll()"
          >
            Unpair all devices
          </button>
        </section>
      </div>
    </div>
  </div>
</template>

<style scoped>
.streaming-overlay {
  position: fixed;
  inset: 0;
  z-index: 900;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: 24px;
  background: rgba(4, 6, 8, 0.72);
  backdrop-filter: blur(6px);
}
.streaming-panel {
  display: flex;
  flex-direction: column;
  width: min(680px, 94vw);
  max-height: 86vh;
  border: 1px solid rgba(255, 255, 255, 0.09);
  border-radius: 16px;
  background: #14171a;
  box-shadow: 0 32px 90px rgba(0, 0, 0, 0.55);
  overflow: hidden;
}
.streaming-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 14px;
  padding: 18px 22px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
}
.streaming-header-title {
  display: flex;
  align-items: center;
  gap: 13px;
}
.streaming-header-icon {
  display: grid;
  place-items: center;
  width: 40px;
  height: 40px;
  border: 1px solid rgba(116, 210, 200, 0.35);
  border-radius: 11px;
  color: #74d2c8;
  background: rgba(116, 210, 200, 0.08);
}
.streaming-header h2 {
  margin: 0;
  color: #eef0ef;
  font-size: 18px;
  font-weight: 650;
}
.streaming-header p {
  margin: 2px 0 0;
  color: #989e9c;
  font-size: 12px;
}
.streaming-close {
  display: grid;
  place-items: center;
  width: 30px;
  height: 30px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 8px;
  color: rgba(255, 255, 255, 0.6);
  background: transparent;
  cursor: pointer;
}
.streaming-close:hover {
  color: #fff;
  border-color: rgba(255, 255, 255, 0.32);
}
.streaming-body {
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: 18px 22px 22px;
  overflow-y: auto;
}
.streaming-card {
  padding: 16px 18px;
  border: 1px solid rgba(255, 255, 255, 0.07);
  border-radius: 12px;
  background: rgba(255, 255, 255, 0.02);
}
.streaming-card.dimmed {
  opacity: 0.55;
}
.streaming-card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 10px;
}
.streaming-card-head h3 {
  margin: 0;
  color: #e6e8e7;
  font-size: 14px;
  font-weight: 650;
}
.streaming-state {
  padding: 3px 9px;
  border-radius: 999px;
  color: #b9bfbd;
  background: rgba(255, 255, 255, 0.06);
  font-size: 11px;
  font-weight: 600;
}
.streaming-state.running {
  color: #7ce0a3;
  background: rgba(116, 210, 138, 0.12);
}
.streaming-state.installed {
  color: #ffd47f;
  background: rgba(255, 193, 77, 0.1);
}
.streaming-lede {
  margin: 0 0 12px;
  color: #9aa09e;
  font-size: 12.5px;
  line-height: 1.55;
}
.streaming-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 9px;
}
.streaming-btn {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 8px 15px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 8px;
  color: #e2e4e3;
  background: rgba(255, 255, 255, 0.04);
  font: inherit;
  font-size: 12.5px;
  font-weight: 600;
  cursor: pointer;
  text-decoration: none;
  transition:
    border-color 0.16s ease,
    background 0.16s ease;
}
.streaming-btn:hover:not(:disabled) {
  border-color: rgba(116, 210, 200, 0.5);
  background: rgba(116, 210, 200, 0.07);
}
.streaming-btn:disabled {
  opacity: 0.5;
  cursor: default;
}
.streaming-btn.primary {
  color: #10201d;
  background: #74d2c8;
  border-color: #74d2c8;
}
.streaming-btn.primary:hover:not(:disabled) {
  background: #85ded5;
}
.streaming-btn.ghost {
  background: transparent;
}
.streaming-btn.danger {
  color: #ff9d9d;
  border-color: rgba(255, 122, 122, 0.3);
}
.streaming-btn.danger:hover:not(:disabled) {
  border-color: rgba(255, 122, 122, 0.6);
  background: rgba(255, 122, 122, 0.06);
}
.streaming-note {
  margin: 10px 0 0;
  color: #7d8381;
  font-size: 11.5px;
}
.streaming-steps {
  margin: 0 0 14px;
  padding-left: 18px;
  color: #9aa09e;
  font-size: 12.5px;
  line-height: 1.65;
}
.streaming-steps a {
  color: #74d2c8;
}
.streaming-pin-row {
  display: flex;
  gap: 9px;
}
.streaming-pin-input {
  width: 110px;
  padding: 8px 12px;
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 8px;
  color: #eceae3;
  background: rgba(255, 255, 255, 0.04);
  font: inherit;
  font-size: 16px;
  letter-spacing: 0.35em;
  text-align: center;
  outline: none;
}
.streaming-pin-input:focus {
  border-color: #74d2c8;
}
.streaming-warn {
  margin: 10px 0 0;
  color: #ffcf7d;
  font-size: 12px;
  line-height: 1.5;
}
.streaming-warn code {
  font-family: "SF Mono", ui-monospace, Menlo, monospace;
  font-size: 11px;
}
.streaming-notes {
  margin: 0 0 12px;
  padding-left: 16px;
  color: #9aa09e;
  font-size: 12px;
  line-height: 1.7;
}
</style>
