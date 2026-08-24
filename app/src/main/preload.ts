import { contextBridge, ipcRenderer } from "electron";
import type { ProcessManagerAction } from "./process-manager-types";

contextBridge.exposeInMainWorld("metalsharp", {
  request: (method: string, url: string, body?: Record<string, unknown>, timeoutMs?: number) =>
    ipcRenderer.invoke("backend:request", method, url, body, timeoutMs),
  isFirstLaunch: () => ipcRenderer.invoke("app:is-first-launch"),
  isMigrationMode: () => ipcRenderer.invoke("app:is-migration-mode"),
  restartAfterMigration: () => ipcRenderer.invoke("app:restart-after-migration"),
  ejectDmg: () => ipcRenderer.invoke("app:eject-dmg"),
  installDeps: (command: string) => ipcRenderer.invoke("app:install-deps", command),
  installHomebrew: () => ipcRenderer.invoke("app:install-homebrew"),
  homebrewStatus: () => ipcRenderer.invoke("app:homebrew-status"),
  onSteamappsChanged: (callback: () => void) => ipcRenderer.on("steamapps:changed", callback),
  onGameJoltDownload: (callback: (update: unknown) => void) => {
    const listener = (_event: Electron.IpcRendererEvent, update: unknown) => callback(update);
    ipcRenderer.on("gamejolt:download", listener);
    return () => ipcRenderer.removeListener("gamejolt:download", listener);
  },
  openInFinder: (path: string) => ipcRenderer.invoke("app:open-in-finder", path),
  openRpcs3Path: (path: string) => ipcRenderer.invoke("app:open-rpcs3-path", path),
  openRpcs3FirmwarePage: () => ipcRenderer.invoke("app:open-rpcs3-firmware-page"),
  openLogsFolder: () => ipcRenderer.invoke("app:open-logs-folder"),
  openMetalsharpFolder: () => ipcRenderer.invoke("app:open-metalsharp-folder"),
  repairDataAccess: () => ipcRenderer.invoke("app:repair-data-access"),
  copyText: (text: string) => ipcRenderer.invoke("app:copy-text", text),
  restartBackend: () => ipcRenderer.invoke("backend:restart"),
  isBackendAlive: () => ipcRenderer.invoke("backend:is-alive"),
  updaterEnsureReady: () => ipcRenderer.invoke("updater:ensure-ready"),
  updaterSpawnInstall: (dmgPath: string, backendPid: number, targetVersion: string) =>
    ipcRenderer.invoke("updater:spawn-install", dmgPath, backendPid, targetVersion),
  updaterInstallStatus: () => ipcRenderer.invoke("updater:install-status"),
  updaterClearStatus: () => ipcRenderer.invoke("updater:clear-status"),
  backendGetPid: () => ipcRenderer.invoke("backend:get-pid"),
  migrateCheck: () => ipcRenderer.invoke("migrate:check"),
  migrateStart: () => ipcRenderer.invoke("migrate:start"),
  migrateProgress: () => ipcRenderer.invoke("migrate:progress"),
  quitApp: () => ipcRenderer.send("app:quit"),
  uninstallApp: () => ipcRenderer.send("app:uninstall"),
  pickExeFile: () => ipcRenderer.invoke("app:pick-exe-file"),
  pickRpcs3File: (kind: "firmware" | "package") => ipcRenderer.invoke("app:pick-rpcs3-file", kind),
  pickImageFile: () => ipcRenderer.invoke("app:pick-image-file"),
  pickDirectory: (title?: string) => ipcRenderer.invoke("app:pick-directory", title),
  gogOAuthLogin: (authUrl: string) => ipcRenderer.invoke("gog:oauth-login", authUrl),
  processManagerToggle: () => ipcRenderer.invoke("process-manager:toggle"),
  processManagerClose: () => ipcRenderer.invoke("process-manager:close"),
  processManagerSample: () => ipcRenderer.invoke("process-manager:sample"),
  processManagerAction: (action: ProcessManagerAction) => ipcRenderer.invoke("process-manager:action", action),
});
