import { type ChildProcess, execFile, spawn } from "child_process";
import { randomBytes } from "crypto";
import * as fs from "fs";
import * as http from "http";
import * as path from "path";

function getShellPath(): string {
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
  return [...additions, process.env.PATH].filter(Boolean).join(":");
}

const shellPath = getShellPath();

interface RustBridgeOptions {
  devMode?: boolean;
  metalsharpHome?: string;
}

export class RustBridge {
  private proc: ChildProcess | null = null;
  private port: number = 9274;
  private base: string;
  private startPromise: Promise<{ ok: boolean; error?: string }> | null = null;
  private devMode: boolean;
  private metalsharpHome?: string;
  private readonly apiToken: string;

  constructor(options: RustBridgeOptions = {}) {
    this.devMode = options.devMode === true || process.env.METALSHARP_DEV === "1";
    this.metalsharpHome = options.metalsharpHome || process.env.METALSHARP_HOME;
    const defaultPort = this.devMode ? "9276" : "9274";
    this.port = parseInt(process.env.METALSHARP_PORT || defaultPort, 10);
    this.base = `http://127.0.0.1:${this.port}`;
    this.apiToken = randomBytes(32).toString("hex");
  }

  getPort(): number {
    return this.port;
  }

  getApiToken(): string {
    return this.apiToken;
  }

  async start(): Promise<{ ok: boolean; error?: string }> {
    if (this.startPromise) return this.startPromise;

    this.startPromise = this.startInner().finally(() => {
      this.startPromise = null;
    });

    return this.startPromise;
  }

  async ensureRunning(maxMs = 15000): Promise<{ ok: boolean; error?: string }> {
    if (await this.isAlive()) return { ok: true };

    // In dev mode, auto-restart the backend so binary swaps "just work".
    // In production, the app owns the lifecycle — only start() spawns.
    if (this.devMode) {
      const started = await this.start();
      if (!started.ok) return started;
      try {
        await this.waitForReady(maxMs);
        return { ok: true };
      } catch (e) {
        return { ok: false, error: (e as Error).message };
      }
    }

    return { ok: false, error: "Backend is not running" };
  }

  private async startInner(): Promise<{ ok: boolean; error?: string }> {
    const binPath = this.findBinary();
    if (!binPath) {
      console.warn("metalsharp-backend binary not found, running in offline mode");
      return { ok: false, error: "metalsharp-backend binary not found" };
    }

    const needsRestart = await this.shouldRestart(binPath);
    if (needsRestart) {
      console.log("Backend version mismatch or not running — restarting...");
      await this.killProcess();
    } else if (await this.isAlive()) {
      console.log("Backend already running and up to date");
      return { ok: true };
    }

    this.spawnBackend(binPath);
    try {
      await this.waitForReady();
      return { ok: true };
    } catch (e) {
      return { ok: false, error: (e as Error).message };
    }
  }

  async restart(): Promise<{ ok: boolean; error?: string }> {
    const binPath = this.findBinary();
    if (!binPath) {
      return { ok: false, error: "Backend binary not found" };
    }

    this.stop();
    await this.killProcess();

    try {
      this.spawnBackend(binPath);
      await this.waitForReady(10000);
      return { ok: true };
    } catch (e) {
      return { ok: false, error: (e as Error).message };
    }
  }

  stop() {
    this.killProcess();
  }

  async killProcess() {
    // Kill the child we spawned (if any).
    if (this.proc?.pid) {
      try {
        this.proc.kill("SIGTERM");
      } catch {}
    }
    this.proc = null;

    // Also find and kill any backend still listening on our port.
    const pid = (await this.getBackendPid()) ?? (await this.getListeningBackendPid());
    if (pid) {
      try {
        process.kill(pid, "SIGTERM");
      } catch {}

      // Wait up to 3s for graceful shutdown, then force-kill.
      const deadline = Date.now() + 3000;
      while (Date.now() < deadline) {
        try {
          process.kill(pid, 0);
          await new Promise((r) => setTimeout(r, 200));
        } catch {
          break;
        }
      }
      try {
        process.kill(pid, "SIGKILL");
      } catch {}
    }

    // Wait for the port to be released.
    const portDeadline = Date.now() + 2000;
    while (Date.now() < portDeadline) {
      if (!(await this.isAlive())) break;
      await new Promise((r) => setTimeout(r, 150));
    }
  }

  async isAlive(): Promise<boolean> {
    return new Promise((resolve) => {
      const req = http.get(`http://127.0.0.1:${this.port}/status`, { headers: this.authHeaders() }, (res) => {
        res.resume();
        resolve(res.statusCode === 200);
      });
      req.on("error", () => resolve(false));
      // Dev backends can be busy with a first-run bottle scan; don't treat a
      // slow /status as dead or ensureRunning will kill and restart it forever.
      req.setTimeout(this.devMode ? 10000 : 1500, () => {
        req.destroy();
        resolve(false);
      });
    });
  }

  async getBackendPid(): Promise<number | null> {
    return new Promise((resolve) => {
      const req = http.get(`http://127.0.0.1:${this.port}/status`, { headers: this.authHeaders() }, (res) => {
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
      });
      req.on("error", () => resolve(null));
      req.setTimeout(1500, () => {
        req.destroy();
        resolve(null);
      });
    });
  }

  async request(method: string, url: string, body?: Record<string, unknown>, timeoutMs?: number): Promise<unknown> {
    return new Promise((resolve, reject) => {
      const payload = body ? JSON.stringify(body) : "";
      const opts: http.RequestOptions = {
        hostname: "127.0.0.1",
        port: this.port,
        path: url,
        method,
        headers: {
          "Content-Type": "application/json",
          "Content-Length": Buffer.byteLength(payload),
          ...this.authHeaders(),
        },
        timeout: timeoutMs ?? 30000,
      };

      const req = http.request(opts, (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          try {
            resolve(JSON.parse(Buffer.concat(chunks).toString()));
          } catch {
            resolve({});
          }
        });
      });

      req.on("error", reject);
      req.on("timeout", () => {
        req.destroy();
        reject(new Error("request timeout"));
      });

      if (payload) req.write(payload);
      req.end();
    });
  }

  async requestImage(url: string, timeoutMs = 10000): Promise<string | null> {
    return new Promise((resolve) => {
      const opts: http.RequestOptions = {
        hostname: "127.0.0.1",
        port: this.port,
        path: url,
        method: "GET",
        headers: this.authHeaders(),
        timeout: timeoutMs,
      };
      let settled = false;
      const finish = (value: string | null) => {
        if (settled) return;
        settled = true;
        resolve(value);
      };

      const req = http.request(opts, (res) => {
        if (res.statusCode !== 200) {
          res.resume();
          finish(null);
          return;
        }

        const contentType = res.headers["content-type"];
        const mime = typeof contentType === "string" ? contentType.split(";", 1)[0].trim() : "";
        if (!/^image\/[a-z0-9.+-]+$/i.test(mime)) {
          res.resume();
          finish(null);
          return;
        }

        const chunks: Buffer[] = [];
        let total = 0;
        res.on("data", (chunk: Buffer) => {
          total += chunk.length;
          if (total > 8 * 1024 * 1024) {
            req.destroy();
            finish(null);
            return;
          }
          chunks.push(chunk);
        });
        res.on("end", () => {
          if (!settled) finish(`data:${mime};base64,${Buffer.concat(chunks).toString("base64")}`);
        });
        res.on("error", () => finish(null));
      });

      req.on("error", () => finish(null));
      req.on("timeout", () => {
        req.destroy();
        finish(null);
      });
      req.end();
    });
  }

  private spawnBackend(binPath: string) {
    this.proc = spawn(binPath, [], {
      env: {
        ...process.env,
        PATH: shellPath,
        METALSHARP_PORT: String(this.port),
        ...(this.metalsharpHome ? { METALSHARP_HOME: this.metalsharpHome } : {}),
        METALSHARP_API_TOKEN: this.apiToken,
        ...(this.devMode ? { METALSHARP_DEV: "1" } : {}),
      },
      stdio: ["ignore", "pipe", "pipe"],
    });

    this.proc.stdout?.on("data", (d: Buffer) => {
      try {
        process.stdout.write(d);
      } catch {
        /* parent pipe closed */
      }
    });
    this.proc.stderr?.on("data", (d: Buffer) => {
      try {
        process.stderr.write(d);
      } catch {
        /* parent pipe closed */
      }
    });
    // Prevent uncaught EPIPE when parent stdout/stderr pipes are broken
    // (e.g. terminal closed). write() returns false on backpressure, then the
    // stream emits 'error' asynchronously — outside try-catch reach.
    process.stdout.on("error", () => {});
    process.stderr.on("error", () => {});

    this.proc.on("error", (e) => {
      console.error(`metalsharp-backend failed to start: ${e.message}`);
      this.proc = null;
    });

    this.proc.on("exit", (code) => {
      console.log(`metalsharp-backend exited with code ${code}`);
      this.proc = null;
    });
  }

  private async shouldRestart(binPath: string): Promise<boolean> {
    if (!(await this.isAlive())) return true;

    try {
      const binStat = fs.statSync(binPath);
      const runningPid = await this.getBackendPid();
      if (!runningPid) return false;

      const runningVersion = await this.getBackendVersion();
      if (runningVersion && runningVersion !== this.getOwnVersion()) return true;

      const runningPath = await this.getProcessPath(runningPid);
      if (runningPath && runningPath !== binPath) return true;

      const runningMtime = runningPath ? fs.statSync(runningPath).mtimeMs : 0;
      if (binStat.mtimeMs > runningMtime + 1000) return true;
    } catch {}

    return false;
  }

  private getOwnVersion(): string {
    const candidates = [
      path.join(__dirname, "..", "package.json"),
      path.join(__dirname, "..", "..", "package.json"),
      path.join(process.resourcesPath || "", "app.asar", "package.json"),
      path.join(process.resourcesPath || "", "app", "package.json"),
    ];
    for (const candidate of candidates) {
      try {
        const pkg = JSON.parse(fs.readFileSync(candidate, "utf8"));
        if (pkg.version) return pkg.version;
      } catch {}
    }
    return "";
  }

  private async getBackendVersion(): Promise<string | null> {
    return new Promise((resolve) => {
      const req = http.get(`http://127.0.0.1:${this.port}/status`, { headers: this.authHeaders() }, (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          if (res.statusCode !== 200) {
            resolve(null);
            return;
          }
          try {
            const data = JSON.parse(Buffer.concat(chunks).toString());
            resolve(data.version ?? null);
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

  private async getProcessPath(pid: number): Promise<string | null> {
    if (!Number.isInteger(pid) || pid <= 0) return null;
    return new Promise((resolve) => {
      execFile("ps", ["-o", "comm=", "-p", String(Math.floor(pid))], (err, stdout) => {
        if (err || !stdout.trim()) {
          resolve(null);
        } else {
          resolve(stdout.trim());
        }
      });
    });
  }

  private findBinary(): string | null {
    const devCandidates = [
      path.join(__dirname, "..", "..", "src-rust", "target", "debug", "metalsharp-backend"),
      path.join(__dirname, "..", "..", "src-rust", "target", "release", "metalsharp-backend"),
    ];
    const packagedCandidates = [
      path.join(process.resourcesPath || "", "runtime", "metalsharp-backend"),
      path.join(__dirname, "..", "..", "src-rust", "target", "release", "metalsharp-backend"),
      path.join(__dirname, "..", "..", "src-rust", "target", "debug", "metalsharp-backend"),
      "/usr/local/bin/metalsharp-backend",
      "/usr/bin/metalsharp-backend",
    ];
    const candidates = this.devMode ? [...devCandidates, ...packagedCandidates] : packagedCandidates;

    for (const c of candidates) {
      try {
        fs.accessSync(c, fs.constants.X_OK);
        return c;
      } catch {}
    }
    return null;
  }

  private waitForReady(maxMs = 15000): Promise<void> {
    const start = Date.now();
    return new Promise((resolve, reject) => {
      const check = () => {
        if (Date.now() - start > maxMs) {
          reject(new Error("backend did not start in time"));
          return;
        }
        let settled = false;
        const req = http.get(`http://127.0.0.1:${this.port}/status`, { headers: this.authHeaders() }, (res) => {
          if (settled) return;
          if (res.statusCode !== 200) {
            settled = true;
            res.resume();
            setTimeout(check, 200);
            return;
          }
          settled = true;
          res.resume();
          resolve();
        });
        req.on("error", () => {
          if (settled) return;
          settled = true;
          setTimeout(check, 200);
        });
        req.setTimeout(1000, () => {
          if (settled) return;
          settled = true;
          req.destroy();
          setTimeout(check, 200);
        });
      };
      setTimeout(check, 300);
    });
  }

  private authHeaders(): Record<string, string> {
    return { Authorization: `Bearer ${this.apiToken}` };
  }

  private async getListeningBackendPid(): Promise<number | null> {
    const pids = await new Promise<number[]>((resolve) => {
      execFile("lsof", ["-nP", `-tiTCP:${this.port}`, "-sTCP:LISTEN"], (err, stdout) => {
        if (err) {
          resolve([]);
          return;
        }
        resolve(
          stdout
            .split(/\s+/)
            .map((value) => Number.parseInt(value, 10))
            .filter((pid) => Number.isInteger(pid) && pid > 0),
        );
      });
    });

    for (const pid of pids) {
      const processPath = await this.getProcessPath(pid);
      if (processPath?.endsWith("metalsharp-backend")) return pid;
    }
    return null;
  }
}
