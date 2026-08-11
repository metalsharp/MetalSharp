import { spawn, spawnSync } from "child_process";
import * as fs from "fs";
import * as http from "http";
import * as os from "os";
import * as path from "path";

function getMetalsharpDir(): string {
  if (process.env.METALSHARP_HOME?.trim()) {
    return path.resolve(process.env.METALSHARP_HOME);
  }
  return path.join(os.homedir(), ".metalsharp");
}

function getStatusFile(): string {
  return path.join(getMetalsharpDir(), "update_install_status.json");
}

export interface InstallStatus {
  phase: string;
  percent: number;
  message: string;
  error: string | null;
  new_version: string | null;
  dmg_path?: string | null;
  timestamp: number;
}

export interface UpdaterReadyResult {
  ok: boolean;
  error?: string;
  scriptPath?: string;
  candidates?: string[];
}

export class UpdaterBridge {
  private scriptPath: string | null = null;
  private backendPort: number;

  constructor(port: number = 9274) {
    this.backendPort = port;
  }

  async ensureReady(): Promise<UpdaterReadyResult> {
    if (this.scriptPath && fs.existsSync(this.scriptPath)) {
      return { ok: true, scriptPath: this.scriptPath };
    }
    this.scriptPath = null;

    const resourcesDir = process.resourcesPath || "";
    const devRoot = path.join(__dirname, "..", "..");

    const candidates = [
      path.join(resourcesDir, "scripts", "tools", "updater", "update.sh"),
      path.join(resourcesDir, "updater", "update.sh"),
      path.join(resourcesDir, "app.asar.unpacked", "updater", "update.sh"),
      path.join(devRoot, "updater", "update.sh"),
    ];

    for (const c of candidates) {
      try {
        fs.accessSync(c, fs.constants.R_OK);
        this.scriptPath = c;
        break;
      } catch {}
    }

    if (!this.scriptPath) {
      const extracted = this.extractBundledUpdater(resourcesDir);
      if (extracted) this.scriptPath = extracted;
    }

    if (!this.scriptPath) {
      const error = `Updater install script not found. Checked: ${candidates.join(", ")}`;
      console.error(`Updater: ${error}`);
      return { ok: false, error, candidates };
    }

    return { ok: true, scriptPath: this.scriptPath, candidates };
  }

  async getBackendPid(): Promise<number | null> {
    return new Promise((resolve) => {
      const req = http.get(`http://127.0.0.1:${this.backendPort}/status`, (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          try {
            const data = JSON.parse(Buffer.concat(chunks).toString());
            resolve(data.pid ?? null);
          } catch {
            resolve(null);
          }
        });
      });
      req.on("error", () => resolve(null));
      req.setTimeout(1500, () => {
        req.destroy();
        resolve(null);
      });
    });
  }

  spawnInstallUpdater(
    dmgPath: string,
    backendPid: number,
    targetVersion: string,
    dmgSize: number,
    dmgSha256: string,
  ): { ok: boolean; error?: string } {
    if (!this.scriptPath) {
      return { ok: false, error: "Updater not ready — update.sh missing" };
    }

    if (typeof dmgPath !== "string" || typeof dmgSha256 !== "string") {
      return { ok: false, error: "DMG path or integrity metadata is invalid" };
    }
    const updatesDir = path.resolve(getMetalsharpDir(), "cache", "updates");
    const resolvedDmgPath = path.resolve(dmgPath);
    if (!resolvedDmgPath.startsWith(`${updatesDir}${path.sep}`) || !fs.existsSync(resolvedDmgPath)) {
      return { ok: false, error: `DMG file not found: ${dmgPath}` };
    }
    let realDmgPath: string;
    try {
      realDmgPath = fs.realpathSync(resolvedDmgPath);
    } catch {
      return { ok: false, error: `DMG file cannot be resolved: ${dmgPath}` };
    }
    let realUpdatesDir: string;
    try {
      realUpdatesDir = fs.realpathSync(updatesDir);
    } catch {
      return { ok: false, error: "MetalSharp update cache cannot be resolved" };
    }
    if (!realDmgPath.startsWith(`${realUpdatesDir}${path.sep}`)) {
      return { ok: false, error: "DMG path must remain inside the MetalSharp update cache" };
    }
    if (!Number.isSafeInteger(dmgSize) || dmgSize <= 0 || !/^[0-9a-fA-F]{64}$/.test(dmgSha256)) {
      return { ok: false, error: "DMG integrity metadata is missing or invalid" };
    }

    fs.mkdirSync(getMetalsharpDir(), { recursive: true });

    const child = spawn(
      "/bin/bash",
      [
        this.scriptPath,
        "--dmg",
        resolvedDmgPath,
        "--dmg-size",
        String(dmgSize),
        "--dmg-sha256",
        dmgSha256,
        "--backend-pid",
        String(backendPid),
        "--target-version",
        targetVersion,
        "--status-file",
        getStatusFile(),
        "--metalsharp-home",
        getMetalsharpDir(),
        "--app-pid",
        String(process.pid),
      ],
      {
        detached: true,
        stdio: "ignore",
        env: {
          ...process.env,
          METALSHARP_HOME: getMetalsharpDir(),
          PATH: ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"].join(":"),
        },
      },
    );

    child.unref();

    console.log(`Updater: spawned install script (pid=${child.pid}) for v${targetVersion}`);

    return { ok: true };
  }

  private extractBundledUpdater(resourcesDir: string): string | null {
    if (!resourcesDir) return null;
    const bundle = path.join(resourcesDir, "bundles", "metalsharp-scripts-tools.tar.zst");
    if (!fs.existsSync(bundle)) return null;

    const extractRoot = path.join(getMetalsharpDir(), "cache", "updater-tools");
    const script = path.join(extractRoot, "scripts", "tools", "updater", "update.sh");
    try {
      fs.rmSync(extractRoot, { recursive: true, force: true });
      fs.mkdirSync(extractRoot, { recursive: true });
      const result = spawnSync(
        "tar",
        ["--use-compress-program=unzstd", "-xf", bundle, "-C", extractRoot, "scripts/tools/updater/update.sh"],
        {
          env: {
            ...process.env,
            PATH: ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"].join(":"),
          },
          stdio: "ignore",
        },
      );
      if ((result.status === 0 || fs.existsSync(script)) && fs.statSync(script).size > 0) {
        fs.chmodSync(script, 0o755);
        return script;
      }
    } catch (error) {
      console.error("Updater: failed to extract bundled updater", error);
    }
    return null;
  }

  private static validateStatusPath(): boolean {
    const resolved = path.resolve(getStatusFile());
    return resolved.startsWith(path.resolve(getMetalsharpDir()));
  }

  readInstallStatus(): InstallStatus | null {
    try {
      if (!UpdaterBridge.validateStatusPath()) return null;
      const raw = fs.readFileSync(getStatusFile(), "utf8");
      return JSON.parse(raw);
    } catch {
      return null;
    }
  }

  clearInstallStatus(): void {
    try {
      if (!UpdaterBridge.validateStatusPath()) return;
      fs.unlinkSync(getStatusFile());
    } catch {}
  }

  static getStatusFilePath(): string {
    return getStatusFile();
  }
}
