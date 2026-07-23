import { execFile, execFileSync, spawn } from "child_process";
import { app, BrowserWindow, clipboard, dialog, globalShortcut, ipcMain, shell } from "electron";
import * as fs from "fs";
import * as http from "http";
import * as os from "os";
import * as path from "path";
import type { ProcessManagerAction, ProcessManagerActionResult, ProcessManagerSample } from "./process-manager-types";
import { RustBridge } from "./rust-bridge";
import { UpdaterBridge } from "./updater-bridge";

let shellPath: string | undefined;

function ensureShellPath() {
  if (shellPath) return shellPath;
  const home = process.env.HOME || "";
  const candidates = [
    "/opt/homebrew/bin",
    "/usr/local/bin",
    "/usr/local/sbin",
    "/usr/bin",
    "/bin",
    "/usr/sbin",
    "/sbin",
    `${home}/.cargo/bin`,
  ];
  const existing = new Set((process.env.PATH || "").split(":"));
  const additions = candidates.filter((c) => !existing.has(c));
  shellPath = [...additions, process.env.PATH].filter(Boolean).join(":");
  return shellPath;
}

function findHomebrew(): string | null {
  const prefix = process.env.HOMEBREW_PREFIX?.trim();
  const candidates = [
    prefix ? path.join(prefix, "bin", "brew") : "",
    "/opt/homebrew/bin/brew",
    "/usr/local/bin/brew",
    ...ensureShellPath()
      .split(":")
      .filter(Boolean)
      .map((dir) => path.join(dir, "brew")),
  ];

  for (const candidate of new Set(candidates.filter(Boolean))) {
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      execFileSync(candidate, ["--version"], {
        env: { ...process.env, PATH: ensureShellPath() },
        stdio: "ignore",
      });
      return candidate;
    } catch {
      // continue to next candidate
    }
  }
  return null;
}

let mainWindow: BrowserWindow | null = null;
let processManagerWindow: BrowserWindow | null = null;
let bridge: RustBridge;
let updaterBridge: UpdaterBridge;
let steamappsWatcher: fs.FSWatcher | null = null;

function isDevRuntime(): boolean {
  return process.env.METALSHARP_DEV === "1" || !app.isPackaged;
}

function isUiOnlyRuntime(): boolean {
  return process.env.METALSHARP_UI_ONLY === "1";
}

function isDevPreviewRuntime(): boolean {
  return isDevRuntime() && process.env.METALSHARP_PREVIEW === "1";
}

function isProcessManagerOnlyRuntime(): boolean {
  return process.env.METALSHARP_PROCESS_MANAGER_ONLY === "1" || process.argv.includes("--process-manager-overlay");
}

function getMetalsharpDir(): string {
  if (process.env.METALSHARP_HOME?.trim()) {
    return path.resolve(process.env.METALSHARP_HOME);
  }
  return path.join(os.homedir(), isDevRuntime() ? ".metalsharp-dev" : ".metalsharp");
}

function uiOnlyVersion(): string {
  return `${app.getVersion()}-ui`;
}

function uiOnlyBackendResponse(method: string, url: string): unknown {
  if (url === "/status") {
    return { ok: true, data: { ok: true, version: uiOnlyVersion() } };
  }
  if (url === "/setup/state") {
    return {
      ok: true,
      completed: true,
      step: 4,
      deviceName: "UI Preview Rig",
      steamApiKeySet: true,
      runtimeMigrationRequired: false,
    };
  }
  if (url === "/steam/api-key") {
    return { ok: true, key: "ui-only" };
  }
  if (url === "/steam/status") {
    return {
      ok: true,
      installed: true,
      running: false,
      mac_installed: true,
      mac_running: false,
      metalsharp_wine_available: true,
    };
  }
  if (url === "/steam/library") {
    const games = [
      {
        appid: 1583230,
        name: "High Fidelity Render Probe",
        installed: true,
        state: "installed",
        cover_url: "",
        header_url: "",
        size_bytes: 48791234560,
        launch_method: "m12",
        launch_method_name: "M12",
        preferred_pipeline: "m12",
      },
      {
        appid: 3527290,
        name: "PEAK",
        installed: true,
        state: "installed",
        cover_url: "",
        header_url: "",
        size_bytes: 6281222144,
        launch_method: "m11",
        launch_method_name: "M11",
        preferred_pipeline: "m11",
      },
      {
        appid: 105600,
        name: "Terraria",
        installed: false,
        state: "not_installed",
        cover_url: "",
        header_url: "",
        size_bytes: 629145600,
        launch_method: "auto",
        launch_method_name: "Auto",
        preferred_pipeline: null,
      },
      {
        appid: 1962700,
        name: "Shader Cache Lab",
        installed: true,
        state: "downloading",
        cover_url: "",
        header_url: "",
        size_bytes: 15032385536,
        launch_method: "m12",
        launch_method_name: "M12",
        preferred_pipeline: "m12",
      },
    ];
    return { ok: true, total: games.length, installed_count: 3, games };
  }
  if (url === "/scan") {
    return { ok: true, steam: { installed: true, running: false } };
  }
  if (url === "/update/check") {
    const version = uiOnlyVersion();
    return { ok: true, available: false, current_version: version, latest_version: version };
  }
  if (url === "/steam/watch-steamapps") {
    return { ok: true, new_appids: [] };
  }
  if (url.startsWith("/mtsp/pipelines")) {
    return { ok: true, id: "m12", name: "M12", preferred: "m12", pipelines: [] };
  }
  if (method === "POST") {
    return { ok: true };
  }
  return { ok: true };
}

function isFirstLaunch(): boolean {
  const setupFile = path.join(getMetalsharpDir(), "setup.json");
  if (!fs.existsSync(setupFile)) return true;
  try {
    const cfg = JSON.parse(fs.readFileSync(setupFile, "utf8"));
    return !cfg.completed;
  } catch {
    return true;
  }
}

function ensureMetalsharpDirs() {
  const base = getMetalsharpDir();
  const dirs = [
    "games",
    "cache",
    "logs",
    "sharp-library",
    "shader-cache",
    "pipeline-cache",
    "runtime/fna",
    "runtime/shims",
    "runtime/mono-x86",
    "runtime/dxvk-1.10.3",
  ];
  for (const d of dirs) {
    fs.mkdirSync(path.join(base, d), { recursive: true });
  }
}

function verifyMetalsharpDataAccess() {
  ensureMetalsharpDirs();
  const base = getMetalsharpDir();
  const checks = ["logs", "sharp-library", "cache"].map((dir) => {
    const checkPath = path.join(base, dir, ".metalsharp-access-check");
    try {
      fs.writeFileSync(checkPath, String(Date.now()));
      fs.unlinkSync(checkPath);
      return { dir, ok: true };
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown filesystem error";
      return { dir, ok: false, error: message };
    }
  });
  return { ok: checks.every((check) => check.ok), path: base, checks };
}

function hasPostUpdateMigrationMarker(): { needed: boolean; targetVersion?: string } {
  const markerPath = path.join(getMetalsharpDir(), ".post-update-migration");
  try {
    if (!fs.existsSync(markerPath)) return { needed: false };
    const data = JSON.parse(fs.readFileSync(markerPath, "utf8"));
    return { needed: data.needed === true, targetVersion: data.target_version };
  } catch {
    return { needed: false };
  }
}

function clearPostUpdateMigrationMarker() {
  const markerPath = path.join(getMetalsharpDir(), ".post-update-migration");
  try {
    fs.unlinkSync(markerPath);
  } catch {}
}

function appBundleFromExe(exePath: string): string | null {
  const match = exePath.match(/^(.*?\.app)\/Contents\/MacOS\//);
  return match?.[1] ?? null;
}

function installedMetalSharpAppPath(): string | null {
  const candidates = ["/Applications/MetalSharp.app", appBundleFromExe(app.getPath("exe"))].filter(
    (candidate): candidate is string => !!candidate && !candidate.startsWith("/Volumes/"),
  );

  return candidates.find((candidate) => fs.existsSync(candidate)) ?? null;
}

function deleteInstalledUpdateDmg(): string | null {
  const status = updaterBridge?.readInstallStatus?.();
  const dmgPath = status?.dmg_path ? path.resolve(status.dmg_path) : null;
  if (!dmgPath?.toLowerCase().endsWith(".dmg")) return null;
  if (!fs.existsSync(dmgPath)) return null;

  const msDir = path.resolve(getMetalsharpDir());
  const updatesDir = path.join(msDir, "cache", "updates");
  const allowed =
    dmgPath.startsWith(updatesDir + path.sep) || path.basename(dmgPath).toLowerCase().startsWith("metalsharp-");
  if (!allowed) return null;

  fs.rmSync(dmgPath, { force: true });
  return dmgPath;
}

function spawnFreshInstalledAppAfterExit(appPath: string) {
  const script = `current_pid=${process.pid}\napp_path=${JSON.stringify(appPath)}\nwhile kill -0 "$current_pid" 2>/dev/null; do sleep 0.2; done\n/usr/bin/open -n "$app_path"\n`;
  const child = spawn("/bin/sh", ["-c", script], {
    detached: true,
    stdio: "ignore",
    env: { ...process.env, PATH: ensureShellPath() },
  });
  child.unref();
}

function forceKillBackendProcesses() {
  for (const signal of ["TERM", "KILL"]) {
    try {
      execFileSync("/usr/bin/pkill", [`-${signal}`, "-x", "metalsharp-backend"], { stdio: "ignore" });
    } catch {}
    if (signal === "TERM") {
      try {
        execFileSync("/bin/sleep", ["0.5"]);
      } catch {}
    }
  }
}

type ProcessRow = { pid: number; comm: string; command: string };

function parseProcessRows(output: string): ProcessRow[] {
  return output
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const match = line.match(/^(\d+)\s+(\S+)\s+(.*)$/);
      if (!match) return null;
      return { pid: Number(match[1]), comm: match[2], command: match[3] };
    })
    .filter((row): row is ProcessRow => !!row && Number.isFinite(row.pid));
}

function isMetalSharpMainProcess(row: ProcessRow): boolean {
  const command = row.command.toLowerCase();
  const comm = path.basename(row.comm).toLowerCase();
  return (
    comm === "metalsharp" ||
    command.includes("/metalsharp.app/contents/macos/metalsharp") ||
    command.includes("/metalsharp.app/contents/macos/metalsharp ")
  );
}

function forceKillDuplicateMetalSharpApps(): { killed: number[]; errors: string[] } {
  const killed: number[] = [];
  const errors: string[] = [];
  let rows: ProcessRow[] = [];
  try {
    rows = parseProcessRows(execFileSync("/bin/ps", ["-axo", "pid=,comm=,command="], { encoding: "utf8" }));
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return { killed, errors: [message] };
  }

  const duplicatePids = rows
    .filter(isMetalSharpMainProcess)
    .map((row) => row.pid)
    .filter((pid) => pid > 0 && pid !== process.pid);

  for (const pid of duplicatePids) {
    try {
      process.kill(pid, "SIGTERM");
      killed.push(pid);
    } catch (error) {
      const code =
        typeof error === "object" && error && "code" in error ? String((error as { code?: unknown }).code) : "";
      if (code !== "ESRCH") errors.push(`SIGTERM ${pid}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  if (duplicatePids.length > 0) {
    for (let i = 0; i < 10; i++) {
      const stillAlive = duplicatePids.some((pid) => {
        try {
          process.kill(pid, 0);
          return true;
        } catch {
          return false;
        }
      });
      if (!stillAlive) break;
      try {
        execFileSync("/bin/sleep", ["0.25"]);
      } catch {}
    }
    for (const pid of duplicatePids) {
      try {
        process.kill(pid, "SIGKILL");
      } catch (error) {
        const code =
          typeof error === "object" && error && "code" in error ? String((error as { code?: unknown }).code) : "";
        if (code !== "ESRCH") errors.push(`SIGKILL ${pid}: ${error instanceof Error ? error.message : String(error)}`);
      }
    }
  }

  return { killed: [...new Set(killed)], errors };
}

function processManagerHelperCandidates(): string[] {
  const exeDir = path.dirname(app.getPath("exe"));
  return [
    path.join(process.resourcesPath ?? "", "scripts", "tools", "native", "metalsharp-process-manager-helper"),
    path.join(exeDir, "..", "Resources", "scripts", "tools", "native", "metalsharp-process-manager-helper"),
    path.join(getMetalsharpDir(), "scripts", "tools", "native", "metalsharp-process-manager-helper"),
    path.join(__dirname, "..", "..", "native", "metalsharp-process-manager-helper"),
    path.join(process.cwd(), "native", "metalsharp-process-manager-helper"),
  ].filter(Boolean);
}

function parseProcessManagerHelperOutput(stdout: string): ProcessManagerSample | null {
  try {
    const parsed = JSON.parse(stdout);
    return parsed && typeof parsed === "object" ? (parsed as ProcessManagerSample) : null;
  } catch {
    return null;
  }
}

function jsProcessManagerFallback(): ProcessManagerSample {
  const totalMem = os.totalmem();
  const freeMem = os.freemem();
  const load = os.loadavg()[0] ?? 0;
  const cores = os.cpus().length || 1;
  return {
    ok: true,
    source: "metalsharp-process-helper-js-fallback",
    timestamp: Math.floor(Date.now() / 1000),
    fps: null,
    cpu_percent: Math.min(100, Math.round((load / cores) * 1000) / 10),
    cpu_temp_c: null,
    cores_used: Math.round(load * 10) / 10,
    cores_total: cores,
    ram_used_bytes: totalMem - freeMem,
    ram_total_bytes: totalMem,
    gpu_percent: null,
    gpu_label: "Metal session telemetry hook pending",
    chip: os.cpus()[0]?.model ?? os.arch(),
    processes: [],
  };
}

async function processManagerSample(): Promise<ProcessManagerSample> {
  for (const candidate of processManagerHelperCandidates()) {
    if (!candidate || !fs.existsSync(candidate)) continue;
    const result = await new Promise<ProcessManagerSample | null>((resolve) => {
      execFile(candidate, [], { timeout: 2500, env: { ...process.env, PATH: ensureShellPath() } }, (error, stdout) => {
        if (error) {
          resolve(null);
          return;
        }
        resolve(parseProcessManagerHelperOutput(stdout));
      });
    });
    if (result) return { ...result, helper_path: candidate };
  }
  return jsProcessManagerFallback();
}

function isNonSteamWineProcess(row: ProcessRow): boolean {
  const command = row.command.toLowerCase();
  const comm = path.basename(row.comm).toLowerCase();
  const haystack = `${comm} ${command}`;
  const isSteam = haystack.includes("steam");
  const isWine =
    comm === "wine" ||
    comm === "wineserver" ||
    comm.startsWith("wine") ||
    command.includes("/wine") ||
    command.includes("wineserver") ||
    command.includes("wine-preloader") ||
    command.includes("wine64-preloader") ||
    command.includes("wineboot") ||
    command.includes("drive_c/");
  return row.pid > 0 && row.pid !== process.pid && isWine && !isSteam;
}

async function processManagerQuitGame(): Promise<ProcessManagerActionResult> {
  let rows: ProcessRow[] = [];
  try {
    rows = parseProcessRows(execFileSync("/bin/ps", ["-axo", "pid=,comm=,command="], { encoding: "utf8" }));
  } catch (error) {
    return {
      ok: false,
      error: error instanceof Error ? error.message : String(error),
      killed: [],
      errors: [],
    };
  }
  const targets = rows.filter(isNonSteamWineProcess);
  const killed: number[] = [];
  const errors: string[] = [];
  for (const row of targets) {
    try {
      process.kill(row.pid, "SIGKILL");
      killed.push(row.pid);
    } catch (error) {
      const code =
        typeof error === "object" && error && "code" in error ? String((error as { code?: unknown }).code) : "";
      if (code !== "ESRCH") errors.push(`${row.pid}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  const skippedSteamWinePids = rows.filter((row) => {
    const haystack = `${path.basename(row.comm).toLowerCase()} ${row.command.toLowerCase()}`;
    return haystack.includes("wine") && haystack.includes("steam");
  }).length;
  return { ok: errors.length === 0, killed, errors, skippedSteamWinePids };
}

async function createProcessManagerWindow(): Promise<BrowserWindow> {
  if (processManagerWindow && !processManagerWindow.isDestroyed()) {
    return processManagerWindow;
  }
  const uiOnly = isUiOnlyRuntime();
  processManagerWindow = new BrowserWindow({
    width: 720,
    height: 540,
    minWidth: 480,
    minHeight: 420,
    resizable: true,
    title: "MetalSharp Process Manager",
    backgroundColor: uiOnly ? "#09070f" : "#0b0d12",
    frame: !uiOnly,
    transparent: false,
    vibrancy: undefined,
    backgroundMaterial: undefined,
    icon: path.join(__dirname, "..", "..", "build", "icon.png"),
    titleBarStyle: uiOnly ? undefined : "hiddenInset",
    trafficLightPosition: { x: 16, y: 16 },
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });
  processManagerWindow.on("closed", () => {
    processManagerWindow = null;
  });
  await processManagerWindow.loadFile(path.join(__dirname, "..", "renderer", "index.html"), {
    query: { overlay: "process-manager", theme: "developer" },
  });
  return processManagerWindow;
}

async function toggleProcessManagerWindow(): Promise<void> {
  const win = await createProcessManagerWindow();
  if (win.isVisible()) {
    win.hide();
    return;
  }
  win.center();
  win.show();
  win.focus();
}

function registerProcessManagerShortcut(): void {
  const shortcuts = process.platform === "darwin" ? ["Command+P", "CommandOrControl+P"] : ["CommandOrControl+P"];
  let registered = false;
  for (const accelerator of shortcuts) {
    const ok = globalShortcut.register(accelerator, () => void toggleProcessManagerWindow());
    registered ||= ok || globalShortcut.isRegistered(accelerator);
    if (!ok && !globalShortcut.isRegistered(accelerator)) {
      console.warn(`MetalSharp Process Manager shortcut was not registered: ${accelerator}`);
    }
  }
  if (!registered) {
    console.warn(
      "MetalSharp Process Manager Cmd+P shortcut unavailable; overlay can still be opened from IPC/dev launch.",
    );
  }
}

async function checkNeedsMigration(): Promise<boolean> {
  const marker = hasPostUpdateMigrationMarker();
  return new Promise((resolve) => {
    const req = http.get("http://127.0.0.1:9274/update/migrate/check", (res) => {
      const chunks: Buffer[] = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => {
        try {
          const data = JSON.parse(Buffer.concat(chunks).toString());
          const needed = data.ok && data.needed === true;
          if (!needed && !marker.needed) clearPostUpdateMigrationMarker();
          resolve(needed);
        } catch {
          resolve(marker.needed);
        }
      });
    });
    req.on("error", () => resolve(marker.needed));
    req.setTimeout(3000, () => {
      req.destroy();
      resolve(marker.needed);
    });
  });
}

async function createWindow(migrating = false) {
  const uiOnly = isUiOnlyRuntime();
  mainWindow = new BrowserWindow({
    width: migrating ? 640 : 1200,
    height: migrating ? 420 : 800,
    minWidth: migrating ? 640 : 900,
    minHeight: migrating ? 420 : 600,
    resizable: !migrating,
    title: migrating ? "MetalSharp Migration" : "MetalSharp",
    backgroundColor: migrating ? "#1b2838" : uiOnly ? "#09070f" : undefined,
    frame: !uiOnly,
    transparent: false,
    vibrancy: migrating || uiOnly ? undefined : "sidebar",
    backgroundMaterial: migrating || uiOnly ? undefined : "acrylic",
    icon: path.join(__dirname, "..", "..", "build", "icon.png"),
    titleBarStyle: uiOnly ? undefined : "hiddenInset",
    trafficLightPosition: { x: 16, y: 16 },
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  mainWindow.loadFile(path.join(__dirname, "..", "renderer", "index.html"), {
    query: isUiOnlyRuntime() ? { theme: "developer" } : {},
  });
}

function startSteamappsWatcher() {
  const steamappsDir = path.join(
    getMetalsharpDir(),
    "prefix-steam",
    "drive_c",
    "Program Files (x86)",
    "Steam",
    "steamapps",
  );

  if (!fs.existsSync(steamappsDir)) return;

  let debounceTimer: ReturnType<typeof setTimeout> | null = null;

  try {
    steamappsWatcher = fs.watch(steamappsDir, (eventType, filename) => {
      if (!filename) return;
      if (filename.startsWith("appmanifest_") && filename.endsWith(".acf")) {
        if (debounceTimer) clearTimeout(debounceTimer);
        debounceTimer = setTimeout(() => {
          if (mainWindow && !mainWindow.isDestroyed()) {
            mainWindow.webContents.send("steamapps:changed");
          }
        }, 2000);
      }
    });
  } catch {
    // steamapps dir may not exist yet
  }
}

async function cleanup() {
  if (steamappsWatcher) {
    steamappsWatcher.close();
    steamappsWatcher = null;
  }
  await bridge?.killProcess();
}

let migrationMode = false;

app.whenReady().then(async () => {
  if (isProcessManagerOnlyRuntime()) {
    process.env.METALSHARP_DEV = "1";
    migrationMode = false;
    registerIpc();
    registerProcessManagerShortcut();
    await createProcessManagerWindow();
    app.on("activate", () => {
      if (BrowserWindow.getAllWindows().length === 0) void createProcessManagerWindow();
    });
    return;
  }

  if (isUiOnlyRuntime()) {
    process.env.METALSHARP_DEV = "1";
    migrationMode = false;
    registerIpc();
    registerProcessManagerShortcut();
    await createWindow(false);
    app.on("activate", () => {
      if (BrowserWindow.getAllWindows().length === 0) createWindow(false);
    });
    return;
  }

  process.env.METALSHARP_HOME = getMetalsharpDir();
  if (isDevRuntime()) process.env.METALSHARP_DEV = "1";
  ensureMetalsharpDirs();
  bridge = new RustBridge({ devMode: isDevRuntime(), metalsharpHome: getMetalsharpDir() });
  updaterBridge = new UpdaterBridge(bridge.getPort());
  const backendStart = await bridge.start();
  if (!backendStart.ok) {
    console.warn(`MetalSharp backend did not start during app launch: ${backendStart.error}`);
  }

  if (!backendStart.ok || !(await bridge.isAlive())) {
    console.log("Backend not responding after first start — retrying...");
    const retryStart = await bridge.start();
    if (!retryStart.ok) {
      console.warn(`MetalSharp backend retry failed: ${retryStart.error}`);
    }
  }

  const needsMigration = await checkNeedsMigration();
  if (needsMigration) {
    const duplicateKill = forceKillDuplicateMetalSharpApps();
    if (duplicateKill.killed.length > 0 || duplicateKill.errors.length > 0) {
      console.warn(
        `Migration guard handled duplicate MetalSharp apps: killed=${duplicateKill.killed.join(",") || "none"} errors=${duplicateKill.errors.join(" | ") || "none"}`,
      );
    }
  }
  migrationMode = needsMigration;

  registerIpc();
  registerProcessManagerShortcut();

  await createWindow(needsMigration);

  if (!needsMigration) {
    startSteamappsWatcher();
  }

  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow(false);
  });
});

app.on("window-all-closed", async () => {
  await cleanup();
  if (process.platform !== "darwin") app.quit();
});

app.on("before-quit", async () => {
  await cleanup();
});

function backendErrorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  if (typeof error === "string") return error;
  return "Backend request failed";
}

async function requestBackend(
  method: string,
  url: string,
  body?: Record<string, unknown>,
  timeoutMs?: number,
): Promise<unknown> {
  if (isUiOnlyRuntime()) {
    return uiOnlyBackendResponse(method, url);
  }

  const ready = await bridge.ensureRunning();
  if (!ready.ok) {
    return { ok: false, error: ready.error ?? "Backend is not available" };
  }

  try {
    return await bridge.request(method, url, body, timeoutMs);
  } catch (e) {
    return { ok: false, error: backendErrorMessage(e) };
  }
}

async function requestMigrationBackend(
  method: string,
  url: string,
  body?: Record<string, unknown>,
  timeoutMs?: number,
): Promise<unknown> {
  const ready = await bridge.ensureRunning(30000);
  if (!ready.ok) {
    return {
      ok: false,
      error: `Migration backend unavailable: ${ready.error ?? "could not start metalsharp-backend"}`,
    };
  }

  return requestBackend(method, url, body, timeoutMs);
}

function registerIpc() {
  ipcMain.handle(
    "backend:request",
    async (_e, method: string, url: string, body?: Record<string, unknown>, timeoutMs?: number) => {
      return requestBackend(method, url, body, timeoutMs);
    },
  );

  ipcMain.handle("app:is-first-launch", () => {
    if (isUiOnlyRuntime() || isDevPreviewRuntime()) return false;
    return isFirstLaunch();
  });

  ipcMain.handle("app:is-migration-mode", () => {
    return migrationMode;
  });

  ipcMain.handle("process-manager:toggle", async () => {
    await toggleProcessManagerWindow();
    return { ok: true };
  });
  ipcMain.handle("process-manager:close", () => {
    if (processManagerWindow && !processManagerWindow.isDestroyed()) processManagerWindow.hide();
    return { ok: true };
  });
  ipcMain.handle("process-manager:sample", async () => processManagerSample());
  ipcMain.handle(
    "process-manager:action",
    async (_e, action: ProcessManagerAction): Promise<ProcessManagerActionResult> => {
      if (action === "quit-game") return processManagerQuitGame();
      if (action === "metalfx" || action === "gpu-acceleration") {
        return { ok: true, visualOnly: true, action };
      }
      return { ok: false, error: `Unknown process manager action: ${action}` };
    },
  );

  ipcMain.handle("app:restart-after-migration", async () => {
    const appPath = installedMetalSharpAppPath();
    if (!appPath) {
      return { ok: false, error: "Installed MetalSharp.app was not found in /Applications." };
    }

    // A prefix update can leave Wine helpers alive after migration. Force-stop
    // every MetalSharp Wine/runtime process and verify none survived before
    // deleting the preservation cache or opening the updated app.
    const processCleanup = (await requestMigrationBackend("POST", "/processes/force-kill", undefined, 15000)) as {
      ok?: boolean;
      survivors?: unknown[];
      errors?: unknown[];
      error?: string;
    };
    if (!processCleanup?.ok || (processCleanup.survivors?.length ?? 0) > 0) {
      const survivorCount = processCleanup?.survivors?.length ?? 0;
      return {
        ok: false,
        error:
          processCleanup?.error ??
          `Could not stop all Wine processes (${survivorCount} still running). Try Launch MetalSharp again.`,
      };
    }

    // The restore is complete, so all exact-name migration preservation roots
    // in the system temp directory are now stale and safe to remove.
    const preserveCleanup = (await requestMigrationBackend(
      "POST",
      "/update/migrate/cleanup-preserved",
      undefined,
      30000,
    )) as {
      ok?: boolean;
      removed?: number;
      errors?: unknown[];
      error?: string;
    };
    if (!preserveCleanup?.ok) {
      console.warn(
        `Migration handoff: preserved temp cleanup was incomplete: ${preserveCleanup?.error ?? JSON.stringify(preserveCleanup?.errors ?? [])}`,
      );
    }

    // Remove cached updater DMGs before tearing down the backend; this keeps the
    // user's disk from retaining the old installer image after a successful update.
    try {
      await requestMigrationBackend("POST", "/update/cleanup", undefined, 10000);
    } catch (error) {
      console.warn("Migration handoff: update cleanup request failed", error);
    }

    let deletedDmg: string | null = null;
    try {
      deletedDmg = deleteInstalledUpdateDmg();
    } catch (error) {
      console.warn("Migration handoff: failed to delete update DMG", error);
    }
    updaterBridge.clearInstallStatus();

    clearPostUpdateMigrationMarker();

    // User-requested order: close any leftover MetalSharp app instances first,
    // then kill the backend, then launch the freshly installed app bundle.
    const duplicateKill = forceKillDuplicateMetalSharpApps();
    if (duplicateKill.errors.length > 0) {
      console.warn(`Migration handoff app cleanup errors: ${duplicateKill.errors.join(" | ")}`);
    }

    await bridge.killProcess();
    forceKillBackendProcesses();
    spawnFreshInstalledAppAfterExit(appPath);

    setTimeout(() => app.exit(0), 50);
    return {
      ok: true,
      deletedDmg,
      removedMigrationTempDirs: preserveCleanup?.removed ?? 0,
      launched: appPath,
    };
  });

  ipcMain.handle("app:eject-dmg", async () => {
    if (process.platform !== "darwin") {
      return { ok: false, error: "DMG ejection is only available on macOS." };
    }

    const exePath = app.getPath("exe");
    const dmgMatch = exePath.match(/\/Volumes\/([^/]+)/);
    if (dmgMatch) {
      const vol = dmgMatch[1];
      shell.openExternal("");
      const { execSync } = require("child_process");
      try {
        execSync(`hdiutil detach "/Volumes/${vol}" -quiet`);
        dialog.showMessageBox({
          type: "info",
          title: "MetalSharp",
          message: "MetalSharp has been ejected from the installer disk image.",
          buttons: ["OK"],
        });
      } catch {
        dialog.showMessageBox({
          type: "warning",
          title: "MetalSharp",
          message: "Could not eject the disk image. You can eject it manually from Finder.",
          buttons: ["OK"],
        });
      }
    } else {
      dialog.showMessageBox({
        type: "info",
        title: "MetalSharp",
        message: "MetalSharp is not running from a disk image.",
        buttons: ["OK"],
      });
    }
  });

  ipcMain.handle("app:install-deps", async (_e, command: string) => {
    return new Promise((resolve) => {
      const { spawn } = require("child_process");
      const env = { ...process.env, PATH: ensureShellPath() };
      const brewPath = ["/opt/homebrew/bin/brew", "/usr/local/bin/brew"].find((p) => {
        try {
          fs.accessSync(p);
          return true;
        } catch {
          return false;
        }
      });

      let proc;
      if (command.startsWith("brew ")) {
        if (!brewPath) {
          resolve({
            ok: false,
            error: "Homebrew is not installed. Install it from https://brew.sh first.",
          });
          return;
        }
        const args = command.split(/\s+/).slice(1);
        proc = spawn(brewPath, args, { env });
      } else if (command.startsWith("script:")) {
        const scriptName = command.slice(7);
        const candidates = [
          path.join(path.dirname(app.getPath("exe")), "..", "scripts", scriptName),
          path.join(getMetalsharpDir(), "scripts", scriptName),
          path.join(__dirname, "..", "..", "..", "scripts", scriptName),
        ];
        const resolved = candidates.find((p) => fs.existsSync(p)) ?? null;
        if (!resolved) {
          resolve({ ok: false, error: `Script not found: ${scriptName}` });
          return;
        }
        proc = spawn("/bin/bash", [resolved], { env });
      } else {
        const parts = command.split(/\s+/);
        proc = spawn(parts[0], parts.slice(1), { env });
      }

      let stdout = "";
      let stderr = "";
      let settled = false;

      const timer = setTimeout(
        () => {
          if (!settled) {
            settled = true;
            proc.kill();
            resolve({
              ok: false,
              error: "Installation timed out after 10 minutes.",
            });
          }
        },
        10 * 60 * 1000,
      );

      proc.stdout.on("data", (d: Buffer) => (stdout += d.toString()));
      proc.stderr.on("data", (d: Buffer) => (stderr += d.toString()));
      proc.on("error", (err: Error) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
        }
        resolve({
          ok: false,
          error: `Failed to run "${command}": ${err.message}`,
        });
      });
      proc.on("close", (code: number) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        const combined = `${stdout}${stderr}`;
        const ok = code === 0 || combined.includes("already installed");
        resolve({
          ok,
          error: ok ? null : (combined.split("\n").filter(Boolean).pop() ?? `exit code ${code}`),
        });
      });
    });
  });

  ipcMain.handle("app:install-homebrew", async () => {
    if (process.platform !== "darwin") {
      return { ok: false, error: "Homebrew setup is only available on macOS." };
    }

    const brewPath = findHomebrew();
    if (brewPath) {
      return { ok: true, installed: true, path: brewPath, message: "Homebrew is already installed" };
    }

    return new Promise((resolve) => {
      const commandPath = path.join(app.getPath("temp"), `metalsharp-homebrew-${process.pid}-${Date.now()}.command`);
      const command = [
        "#!/bin/bash",
        `trap 'rm -f "${commandPath.replace(/'/g, "'\\\\''")}"' EXIT`,
        '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"',
        "",
      ].join("\n");

      try {
        fs.writeFileSync(commandPath, command, { mode: 0o700 });
      } catch (err) {
        resolve({ ok: false, error: `Failed to prepare Homebrew installer: ${(err as Error).message}` });
        return;
      }

      execFile("/usr/bin/open", ["-a", "Terminal", commandPath], { timeout: 15000 }, (err: Error | null) => {
        if (err) {
          try {
            fs.unlinkSync(commandPath);
          } catch {}
          resolve({
            ok: false,
            error: `Failed to open Terminal for Homebrew install: ${err.message}`,
          });
        } else {
          resolve({
            ok: true,
            message: "Terminal opened — complete the Homebrew install there",
          });
        }
      });
    });
  });

  ipcMain.handle("app:homebrew-status", () => {
    const brewPath = process.platform === "darwin" ? findHomebrew() : null;
    return { installed: brewPath !== null, path: brewPath ?? undefined };
  });

  ipcMain.handle("app:open-in-finder", async (_e, inputPath: string) => {
    const home = os.homedir();
    const metalsharpDir = getMetalsharpDir();
    const resolved = inputPath.replace(/^~/, home);
    const fullPath = path.resolve(resolved);
    if (!fullPath.startsWith(metalsharpDir) && !fullPath.startsWith(home)) {
      return;
    }
    if (!fs.existsSync(fullPath)) {
      fs.mkdirSync(fullPath, { recursive: true });
    }
    shell.openPath(fullPath);
  });

  ipcMain.handle("app:open-logs-folder", async () => {
    if (isUiOnlyRuntime()) return { ok: true, path: "ui-only://logs" };
    const logsPath = path.join(getMetalsharpDir(), "logs");
    fs.mkdirSync(logsPath, { recursive: true });
    await shell.openPath(logsPath);
    return { ok: true, path: logsPath };
  });

  ipcMain.handle("app:open-metalsharp-folder", async () => {
    if (isUiOnlyRuntime()) return { ok: true, path: "ui-only://metalsharp" };
    const metalsharpPath = getMetalsharpDir();
    fs.mkdirSync(metalsharpPath, { recursive: true });
    await shell.openPath(metalsharpPath);
    return { ok: true, path: metalsharpPath };
  });

  ipcMain.handle("app:repair-data-access", async () => {
    if (isUiOnlyRuntime()) return { ok: true, path: "ui-only://metalsharp", checks: [] };
    return verifyMetalsharpDataAccess();
  });

  ipcMain.handle("app:copy-text", async (_e, text: string) => {
    clipboard.writeText(text);
    return { ok: true };
  });

  ipcMain.handle("backend:restart", async () => {
    if (isUiOnlyRuntime()) return { ok: true };
    return bridge.restart();
  });

  ipcMain.handle("backend:is-alive", async () => {
    if (isUiOnlyRuntime()) return true;
    return bridge.isAlive();
  });

  ipcMain.handle("updater:ensure-ready", async () => {
    if (isUiOnlyRuntime()) return { ok: false, error: "Updater is disabled in UI-only preview mode." };
    return updaterBridge.ensureReady();
  });

  ipcMain.handle("updater:spawn-install", async (_e, dmgPath: string, backendPid: number, targetVersion: string) => {
    if (isUiOnlyRuntime()) return { ok: false, error: "Updater is disabled in UI-only preview mode." };
    return updaterBridge.spawnInstallUpdater(dmgPath, backendPid, targetVersion);
  });

  ipcMain.handle("updater:install-status", async () => {
    if (isUiOnlyRuntime()) return null;
    return updaterBridge.readInstallStatus();
  });

  ipcMain.handle("updater:clear-status", async () => {
    if (isUiOnlyRuntime()) return;
    updaterBridge.clearInstallStatus();
  });

  ipcMain.handle("backend:get-pid", async () => {
    if (isUiOnlyRuntime()) return null;
    return bridge.getBackendPid();
  });

  ipcMain.handle("migrate:check", async () => {
    if (isUiOnlyRuntime()) return { ok: true, needed: false };
    return requestMigrationBackend("GET", "/update/migrate/check");
  });

  ipcMain.handle("migrate:start", async () => {
    if (isUiOnlyRuntime()) return { ok: true };
    return requestMigrationBackend("POST", "/update/migrate/start", undefined, 10000);
  });

  ipcMain.handle("migrate:progress", async () => {
    if (isUiOnlyRuntime()) return { ok: true, phase: "complete", percent: 100 };
    return requestMigrationBackend("GET", "/update/migrate/progress");
  });

  ipcMain.on("app:quit", () => {
    app.quit();
  });

  ipcMain.on("app:uninstall", async () => {
    if (!mainWindow) return;
    const choice = await dialog.showMessageBox(mainWindow, {
      type: "warning",
      title: "Uninstall MetalSharp",
      message: "Are you sure you want to uninstall MetalSharp?",
      detail:
        "This will permanently delete all Wine prefixes, bottles, game data, " +
        "Steam installation, Wine runtime, shader caches, and all settings. " +
        "MetalSharp will also be removed from Applications and moved to Trash. " +
        "This action cannot be undone.",
      buttons: ["Cancel", "Uninstall"],
      defaultId: 0,
      cancelId: 0,
      noLink: true,
    });
    if (choice.response !== 1) return;

    await cleanup();

    const msDir = getMetalsharpDir();
    let dataRemoved = false;
    try {
      fs.rmSync(msDir, { recursive: true, force: true });
      dataRemoved = true;
    } catch (e) {
      console.error("Failed to remove MetalSharp data:", e);
    }

    // Spawn a detached shell that waits for this process to exit, then
    // moves the app bundle to the macOS Trash.
    const exePath = app.getPath("exe");
    const appBundle = exePath.match(/^(\/Applications\/[^/]+\.app)\//)?.[1];
    if (appBundle && fs.existsSync(appBundle)) {
      const pid = process.pid;
      const script = `
        while kill -0 ${pid} 2>/dev/null; do sleep 0.5; done
        osascript -e 'tell application "Finder" to delete POSIX file "${appBundle}"'
      `;
      const { spawn } = require("child_process");
      spawn("/bin/bash", ["-c", script], { detached: true, stdio: "ignore" }).unref();
    }

    // Show success dialog — user must close the app to finish.
    if (mainWindow && !mainWindow.isDestroyed()) {
      await dialog.showMessageBox(mainWindow, {
        type: "info",
        title: "MetalSharp Uninstalled",
        message: "MetalSharp data has been removed successfully.",
        detail:
          "All Wine prefixes, bottles, Steam, runtime, and settings have been deleted. " +
          (appBundle
            ? "When you close this window, MetalSharp will be moved to the Trash."
            : "Close this window to exit MetalSharp.") +
          "\n\nClick OK to close the app.",
        buttons: ["OK"],
        defaultId: 0,
        noLink: true,
      });
    }

    app.quit();
  });

  ipcMain.handle("gog:oauth-login", async (_e, authUrl: string) => {
    if (!mainWindow) return { ok: false, error: "Main window is not ready." };
    let parsed: URL;
    try {
      parsed = new URL(authUrl);
    } catch {
      return { ok: false, error: "Invalid GOG auth URL." };
    }
    if (parsed.hostname !== "auth.gog.com") {
      return { ok: false, error: "Refusing to open non-GOG auth URL." };
    }

    // Match the icon path for the OAuth window.
    const iconCandidates = [
      path.join(process.resourcesPath ?? "", "build", "icon.png"),
      path.join(__dirname, "..", "..", "build", "icon.png"),
    ];
    const iconPath: string = iconCandidates.find((candidate) => fs.existsSync(candidate)) ?? "";

    // Write the OAuth helper HTML to a temp file. We embed the GOG URL
    // directly so there is no async injection race with the webview.
    const escapedUrl = authUrl.replace(/\\/g, "\\\\").replace(/'/g, "\\'").replace(/"/g, "&quot;");
    const htmlContent =
      `<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>Sign in to GOG</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{display:flex;flex-direction:column;height:100vh;background:#1c1c1e;font-family:-apple-system,sans-serif}
#header{display:flex;align-items:center;background:#2c2c2e;padding:6px 10px;border-bottom:1px solid #3a3a3c;gap:8px;flex-shrink:0}
#header span{font-weight:600;color:#f5f5f7;font-size:13px}
#url-display{flex:1;color:#aeaeb2;font-size:12px;font-family:ui-monospace,monospace;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#cancel-btn{background:none;border:1px solid #48484a;color:#f5f5f7;padding:3px 10px;border-radius:6px;cursor:pointer;font-size:12px}
#cancel-btn:hover{background:#3a3a3c}
webview{flex:1;border:none}
</style></head>
<body>
<div id="header">
  <span>Sign in to GOG</span>
  <span id="url-display">${escapedUrl}</span>
  <button id="cancel-btn">Cancel</button>
</div>
<webview id="wv" src="${escapedUrl}" allowpopups></webview>
<script>
(function(){
  const { ipcRenderer } = require('electron');
  const wv = document.getElementById('wv');
  const urlDisplay = document.getElementById('url-display');
  let settled = false;
  const CB_HOST = 'embed.gog.com', CB_PATH = '/on_login_success';

  function done(code, err) {
    if (settled) return;
    settled = true;
    if (code) ipcRenderer.send('gog-oauth-code', code);
    else if (err) ipcRenderer.send('gog-oauth-error', err);
  }
  function checkUrl(url) {
    if (!url) return;
    try {
      const u = new URL(url);
      if (u.hostname !== CB_HOST || u.pathname !== CB_PATH) return;
      const code = u.searchParams.get('code');
      const err = u.searchParams.get('error') || u.searchParams.get('error_description');
      if (code) done(code, null);
      else if (err) done(null, err);
    } catch(e) {}
  }
  wv.addEventListener('dom-ready', function() {
    wv.setUserAgent('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/200.0');
  });
  wv.addEventListener('did-navigate', function(e) { urlDisplay.textContent = e.url; checkUrl(e.url); });
  wv.addEventListener('did-navigate-in-page', function(e) { urlDisplay.textContent = e.url; checkUrl(e.url); });
  wv.addEventListener('new-window', function(e) { if (e.url) wv.loadURL(e.url); });
  document.getElementById('cancel-btn').addEventListener('click', function() {
    done(null, 'Cancelled');
    window.close();
  });
})();
<` +
      `/script>
</body>
</html>`;

    const htmlPath = path.join(os.tmpdir(), `metalsharp-gog-oauth-${Date.now()}.html`);
    fs.writeFileSync(htmlPath, htmlContent);

    return new Promise<{ ok: boolean; code?: string; redirectUrl?: string; error?: string }>((resolve) => {
      let settled = false;

      const finish = (result: { ok: boolean; code?: string; redirectUrl?: string; error?: string }) => {
        if (settled) return;
        settled = true;
        try {
          ipcMain.removeAllListeners("gog-oauth-code");
        } catch {}
        try {
          ipcMain.removeAllListeners("gog-oauth-error");
        } catch {}
        try {
          if (!win.isDestroyed()) win.close();
        } catch {}
        try {
          fs.unlinkSync(htmlPath);
        } catch {}
        resolve(result);
      };

      // Listen for IPC from the renderer (only once per listener type).
      ipcMain.once("gog-oauth-code", (_event: import("electron").IpcMainEvent, code: string) => {
        finish({ ok: true, code });
      });
      ipcMain.once("gog-oauth-error", (_event: import("electron").IpcMainEvent, err: string) => {
        finish({ ok: false, error: err });
      });

      const win = new BrowserWindow({
        width: 1024,
        height: 700,
        minWidth: 800,
        minHeight: 600,
        title: "Sign in to GOG",
        icon: iconPath || undefined,
        autoHideMenuBar: true,
        backgroundColor: "#1c1c1e",
        webPreferences: {
          webviewTag: true,
          nodeIntegration: true,
          contextIsolation: false,
        },
      });
      win.setMenuBarVisibility(false);

      win.on("closed", () => {
        finish({ ok: false, error: "GOG OAuth window was closed before completing login" });
      });

      win.loadFile(htmlPath).catch((err: Error) => {
        finish({ ok: false, error: `Failed to load OAuth page: ${err.message}` });
      });
    });
  });

  ipcMain.handle("app:pick-directory", async (_e, title?: string) => {
    if (!mainWindow) return null;
    const result = await dialog.showOpenDialog(mainWindow, {
      title: title || "Select a folder",
      properties: ["openDirectory", "createDirectory"],
    });
    if (result.canceled || result.filePaths.length === 0) return null;
    return result.filePaths[0];
  });

  ipcMain.handle("app:pick-exe-file", async () => {
    if (!mainWindow) return null;
    const result = await dialog.showOpenDialog(mainWindow, {
      title: "Select a Windows installer or executable",
      properties: ["openFile"],
      filters: [{ name: "Windows App", extensions: ["exe", "msi"] }],
    });
    if (result.canceled || result.filePaths.length === 0) return null;
    return result.filePaths[0];
  });

  ipcMain.handle("app:pick-image-file", async () => {
    if (!mainWindow) return null;
    const result = await dialog.showOpenDialog(mainWindow, {
      title: "Select a cover image",
      properties: ["openFile"],
      filters: [{ name: "Image", extensions: ["jpg", "jpeg", "png"] }],
    });
    if (result.canceled || result.filePaths.length === 0) return null;
    return result.filePaths[0];
  });

  ipcMain.handle("app:pick-asset-file", async () => {
    if (!mainWindow) return null;
    const runtimePath = path.join(app.getPath("home"), ".metalsharp", "runtime");
    fs.mkdirSync(runtimePath, { recursive: true });
    const result = await dialog.showOpenDialog(mainWindow, {
      title: "Add an asset to this app's bottle",
      defaultPath: runtimePath,
      properties: ["openFile"],
    });
    if (result.canceled || result.filePaths.length === 0) return null;
    return result.filePaths[0];
  });
}
