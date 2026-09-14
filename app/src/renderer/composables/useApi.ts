/// <reference path="../api-types.ts" />

const BROWSER_BACKEND_URL = "http://127.0.0.1:9274";

async function browserRequest(
  method: string,
  url: string,
  body?: Record<string, unknown>,
  timeoutMs = 30_000,
): Promise<BackendResponse> {
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(`${BROWSER_BACKEND_URL}${url}`, {
      method,
      // Keep browser requests CORS-simple. The backend parses JSON bodies
      // independently of the content type, while application/json would
      // trigger an OPTIONS preflight that the tiny HTTP server intentionally
      // does not handle.
      headers: body ? { "Content-Type": "text/plain;charset=UTF-8" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
      signal: controller.signal,
    });
    const data = (await response.json().catch(() => ({}))) as BackendResponse;
    if (!response.ok && data.ok !== false) {
      return { ok: false, error: `Backend returned HTTP ${response.status}` };
    }
    return data;
  } finally {
    window.clearTimeout(timeout);
  }
}

const browserAPI = {
  request: browserRequest,
  isFirstLaunch: async () => false,
  isMigrationMode: async () => false,
  isBackendAlive: async () => {
    try {
      return (await browserRequest("GET", "/status", undefined, 3_000)).ok === true;
    } catch {
      return false;
    }
  },
  onGameJoltDownload: () => () => {},
} as unknown as MetalsharpAPI;

// Preview isolation mode: appending ?mock to the dev-server URL detaches the
// renderer from the real backend entirely. Every API call is answered by this
// in-memory stub (including a simulated runtime install), so setup-wizard
// work can never touch a live MetalSharp installation.
const MOCK_MODE = typeof window !== "undefined" && new URLSearchParams(window.location.search).has("mock");

let mockInstallStep = 0;
let mockSteamInstalled = false;
const MOCK_INSTALL_STEPS = [
  "Checking Rosetta 2",
  "Extracting runtime",
  "Installing graphics runtime",
  "Configuring Steam support",
];

const mockSetupAPI = {
  request: async (_method: string, url: string): Promise<BackendResponse> => {
    // Single cast point: mock payloads carry whatever shape the caller asked
    // for, so the strict BackendResponse fields don't need enumerating.
    const respond = (payload: Record<string, unknown> = {}): BackendResponse =>
      ({ ok: true, ...payload }) as BackendResponse;
    const route = url.split("?")[0];
    await new Promise((resolve) => setTimeout(resolve, 120));
    if (route === "/setup/install-all") {
      mockInstallStep = 0;
      return respond();
    }
    if (route === "/setup/install-progress") {
      mockInstallStep = Math.min(mockInstallStep + 1, MOCK_INSTALL_STEPS.length + 1);
      const done = mockInstallStep > MOCK_INSTALL_STEPS.length;
      const index = Math.min(mockInstallStep, MOCK_INSTALL_STEPS.length) - 1;
      return respond({
        step: mockInstallStep,
        total: MOCK_INSTALL_STEPS.length,
        current: MOCK_INSTALL_STEPS[index] ?? "Working",
        status: done ? "complete" : "installing",
        log: done ? "Runtime installed" : MOCK_INSTALL_STEPS[index],
        error: null,
      });
    }
    if (route === "/steam/install") {
      mockSteamInstalled = true;
      return respond();
    }
    if (route === "/steam/status") {
      return respond({ installed: mockSteamInstalled, running: false, installing: false });
    }
    if (route === "/setup/device-name") return respond({ name: "Preview-MacBook" });
    if (route === "/setup/state") return respond({ deviceName: "Preview-MacBook", runtimeMigrationRequired: false });
    if (route === "/steam/library") return respond({ total: 0, installed_count: 0, games: [] });
    if (route === "/steam/api-key") return respond({ key: null });
    if (route === "/steam/save-api-key")
      return respond({
        sync: { api_key_set: true, steam_id_detected: true },
        library: { ok: true, total: 0, installed_count: 0, games: [] },
      });
    if (route === "/status") return respond({ version: "0.61.0-preview" });
    if (route === "/update/check") return respond({ available: false, current_version: "0.61.0" });
    if (route === "/config") return respond();
    return respond();
  },
  isFirstLaunch: async () => true,
  isMigrationMode: async () => false,
  isBackendAlive: async () => true,
  runSteamFix: async () => {
    await new Promise((resolve) => setTimeout(resolve, 900));
    return { ok: true, output: "Deployed MetalSharp Steam webhelper wrapper to 1 CEF directories." };
  },
  onGameJoltDownload: () => () => {},
} as unknown as MetalsharpAPI;

function getAPI(): MetalsharpAPI {
  if (MOCK_MODE) return mockSetupAPI;
  return (window as unknown as { metalsharp?: MetalsharpAPI }).metalsharp || browserAPI;
}

export async function steamFix(): Promise<{ ok: boolean; output?: string; error?: string }> {
  const bridge = getAPI() as MetalsharpAPI & {
    runSteamFix?: () => Promise<{ ok: boolean; output?: string; error?: string }>;
  };
  if (typeof bridge.runSteamFix === "function") return bridge.runSteamFix();
  // Browser preview without the desktop bridge cannot run shell commands.
  return { ok: false, error: "Steam Fix requires the MetalSharp desktop app." };
}

export async function api<T = unknown>(
  method: string,
  url: string,
  body?: Record<string, unknown>,
  timeoutMs?: number,
): Promise<T | null> {
  try {
    const res = await getAPI().request(method, url, body, timeoutMs);
    return (res.data ?? res) as T;
  } catch (e) {
    console.error(`API ${method} ${url}:`, e);
    return null;
  }
}

export { getAPI };
