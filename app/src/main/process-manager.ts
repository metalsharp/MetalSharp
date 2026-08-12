import * as path from "path";

export interface ProcessRow {
  pid: number;
  comm: string;
  command: string;
}

export interface ProcessManagerScope {
  home: string;
  managedPrefix: string;
  wineServer: string;
}

export interface ProcessManagerStopOperations {
  stopWithWineserver: (wineServer: string, prefix: string) => void;
  kill: (pid: number) => void;
}

export interface ProcessManagerStopResult {
  killed: number[];
  errors: string[];
  mode: "no-targets" | "wineserver" | "scoped-sigkill";
}

export function createProcessManagerScope(home: string): ProcessManagerScope {
  const resolvedHome = path.resolve(home);
  return {
    home: resolvedHome,
    managedPrefix: path.join(resolvedHome, "prefix-steam"),
    wineServer: path.join(resolvedHome, "runtime", "wine", "bin", "wineserver"),
  };
}

function processName(row: ProcessRow): string {
  return path.basename(row.comm).toLowerCase();
}

function normalizedPath(value: string): string {
  return value.replaceAll("\\", "/").toLowerCase();
}

function pathReference(command: string, candidate: string): boolean {
  const haystack = normalizedPath(command);
  const needle = normalizedPath(path.resolve(candidate)).replace(/\/+$/, "");
  if (!needle || needle === "/") return false;

  let offset = haystack.indexOf(needle);
  while (offset !== -1) {
    const before = offset === 0 ? "" : haystack[offset - 1];
    const after = haystack[offset + needle.length] ?? "";
    const isPathBoundary = (value: string) => value === "" || !/[a-z0-9._-]/.test(value);
    if (isPathBoundary(before) && isPathBoundary(after)) return true;
    offset = haystack.indexOf(needle, offset + needle.length);
  }
  return false;
}

export function isWineProcess(row: ProcessRow): boolean {
  const command = normalizedPath(row.command);
  const comm = processName(row);
  return (
    comm === "wine" ||
    comm === "wineserver" ||
    comm.startsWith("wine") ||
    command.includes("/wine") ||
    command.includes("wineserver") ||
    command.includes("wine-preloader") ||
    command.includes("wine64-preloader") ||
    command.includes("wineboot") ||
    command.includes("drive_c/")
  );
}

export function isSteamWineProcess(row: ProcessRow): boolean {
  if (!isWineProcess(row)) return false;

  const haystack = normalizedPath(`${row.comm} ${row.command}`);
  return (
    /(?:^|[/\s"'=])steam(?:\.exe)?(?:$|[/\s"'])/.test(haystack) ||
    /(?:^|[/\s"'=])steamwebhelper(?:_real)?\.exe(?:$|[/\s"'])/.test(haystack) ||
    /(?:^|[/\s"'=])steam(?:service|errorreporter)\.exe(?:$|[/\s"'])/.test(haystack)
  );
}

export function isMetalSharpOwnedWineProcess(row: ProcessRow, scope: ProcessManagerScope, currentPid: number): boolean {
  if (row.pid <= 0 || row.pid === currentPid || !isWineProcess(row)) return false;

  // A Wine executable name is not an ownership proof: CrossOver, Whisky,
  // GPTK, and Homebrew Wine all expose the same wine/wineserver helpers.
  // Require a path reference to MetalSharp's resolved data root or its
  // managed prefix before a row can be considered safe to stop.
  const command = `${row.comm} ${row.command}`;
  return pathReference(command, scope.home) || pathReference(command, scope.managedPrefix);
}

export function isNonSteamMetalSharpWineProcess(
  row: ProcessRow,
  scope: ProcessManagerScope,
  currentPid: number,
): boolean {
  return isMetalSharpOwnedWineProcess(row, scope, currentPid) && !isSteamWineProcess(row);
}

export function stopMetalSharpWineProcesses(
  targets: ProcessRow[],
  scope: ProcessManagerScope,
  operations: ProcessManagerStopOperations,
  useGracefulStop: boolean,
): ProcessManagerStopResult {
  if (targets.length === 0) return { killed: [], errors: [], mode: "no-targets" };

  if (useGracefulStop) {
    try {
      operations.stopWithWineserver(scope.wineServer, scope.managedPrefix);
      return { killed: targets.map((row) => row.pid), errors: [], mode: "wineserver" };
    } catch {
      // Fall through to the already-scoped PID fallback when the runtime is
      // unavailable or wineserver cannot connect to the managed prefix.
    }
  }

  const killed: number[] = [];
  const errors: string[] = [];
  for (const row of targets) {
    try {
      operations.kill(row.pid);
      killed.push(row.pid);
    } catch (error) {
      const code =
        typeof error === "object" && error && "code" in error ? String((error as { code?: unknown }).code) : "";
      if (code !== "ESRCH") errors.push(`${row.pid}: ${error instanceof Error ? error.message : String(error)}`);
    }
  }
  return { killed, errors, mode: "scoped-sigkill" };
}
