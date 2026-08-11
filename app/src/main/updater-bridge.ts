import { spawn, spawnSync } from "child_process";
import * as fs from "fs";
import * as http from "http";
import * as os from "os";
import * as path from "path";
import { BACKEND_TOKEN_HEADER } from "./rust-bridge";
import { type VerifiedUpdateArtifact, validateDownloadedDmg } from "./updater-security";

function getMetalsharpDir(): string {
  if (process.env.METALSHARP_HOME?.trim()) {
    return path.resolve(process.env.METALSHARP_HOME);
  }
  return path.join(os.homedir(), ".metalsharp");
}

function getUpdatesDir(): string {
  return path.join(getMetalsharpDir(), "cache", "updates");
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
  private authTokenProvider: () => string | null;

  constructor(port: number = 9274, authTokenProvider: () => string | null = () => null) {
    this.backendPort = port;
    this.authTokenProvider = authTokenProvider;
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
      const token = this.authTokenProvider();
      if (!token) {
        resolve(null);
        return;
      }
      const req = http.get(
        {
          hostname: "127.0.0.1",
          port: this.backendPort,
          path: "/status",
          headers: { [BACKEND_TOKEN_HEADER]: token },
        },
        (res) => {
          const chunks: Buffer[] = [];
          res.on("data", (c) => chunks.push(c));
          res.on("end", () => {
            if (res.statusCode !== 200) {
              resolve(null);
              return;
            }
            try {
              const data = JSON.parse(Buffer.concat(chunks).toString());
              const pid = Number(data.pid);
              resolve(Number.isInteger(pid) && pid > 0 ? pid : null);
            } catch {
              resolve(null);
            }
          });
        },
      );
      req.on("error", () => resolve(null));
      req.setTimeout(1500, () => {
        req.destroy();
        resolve(null);
      });
    });
  }

  private async getDownloadedDmg(): Promise<
    { ok: true; artifact: VerifiedUpdateArtifact } | { ok: false; error: string }
  > {
    return new Promise((resolve) => {
      const token = this.authTokenProvider();
      if (!token) {
        resolve({ ok: false, error: "Could not authenticate with the MetalSharp backend" });
        return;
      }
      const req = http.get(
        {
          hostname: "127.0.0.1",
          port: this.backendPort,
          path: "/update/dmg-path",
          headers: { [BACKEND_TOKEN_HEADER]: token },
        },
        (res) => {
          const chunks: Buffer[] = [];
          res.on("data", (chunk) => chunks.push(chunk));
          res.on("end", () => {
            if (res.statusCode !== 200) {
              resolve({ ok: false, error: "Backend did not provide a downloaded update" });
              return;
            }
            try {
              const data = JSON.parse(Buffer.concat(chunks).toString());
              const validation = validateDownloadedDmg(data, getUpdatesDir());
              resolve(validation);
            } catch {
              resolve({ ok: false, error: "Backend returned an invalid update artifact" });
            }
          });
        },
      );
      req.on("error", () => resolve({ ok: false, error: "Could not verify the downloaded update with the backend" }));
      req.setTimeout(1500, () => {
        req.destroy();
        resolve({ ok: false, error: "Timed out while verifying the downloaded update" });
      });
    });
  }

  async spawnInstallUpdater(): Promise<{ ok: boolean; error?: string }> {
    if (!this.scriptPath) {
      return { ok: false, error: "Updater not ready — update.sh missing" };
    }

    const downloaded = await this.getDownloadedDmg();
    if (!downloaded.ok) return downloaded;

    const backendPid = await this.getBackendPid();
    if (!backendPid) {
      return { ok: false, error: "Could not verify the MetalSharp backend process" };
    }

    fs.mkdirSync(getMetalsharpDir(), { recursive: true });

    const child = spawn(
      "/bin/bash",
      [
        this.scriptPath,
        "--dmg",
        downloaded.artifact.path,
        "--dmg-size",
        String(downloaded.artifact.size),
        "--dmg-sha256",
        downloaded.artifact.sha256,
        "--backend-pid",
        String(backendPid),
        "--target-version",
        downloaded.artifact.version,
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

    console.log(`Updater: spawned install script (pid=${child.pid}) for v${downloaded.artifact.version}`);

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
    const root = path.resolve(getMetalsharpDir());
    return resolved.startsWith(`${root}${path.sep}`);
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
