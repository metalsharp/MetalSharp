export type BackendMethod = "GET" | "POST";
export type BackendRequestSource = "renderer" | "main";

type QueryValidator = (params: URLSearchParams) => boolean;

interface EndpointRule {
  sources: readonly BackendRequestSource[];
  validateQuery?: QueryValidator;
}

const RENDERER_ENDPOINTS = [
  "GET /bottles",
  "GET /bottles/profiles",
  "GET /cache/size",
  "GET /config",
  "GET /diagnostics/m12/dry-run",
  "GET /eac/status",
  "GET /goldberg/status",
  "GET /logs",
  "GET /logs/crash-reports",
  "GET /logs/stream",
  "GET /metalfx/state",
  "GET /mtsp/default-rules",
  "GET /mtsp/pipelines",
  "GET /scan",
  "GET /setup/dependencies",
  "GET /setup/device-name",
  "GET /setup/install-progress",
  "GET /setup/state",
  "GET /sharp-library",
  "GET /sharp-library/gog/games",
  "GET /sharp-library/gog/status",
  "GET /status",
  "GET /steam/api-key",
  "GET /steam/library",
  "GET /steam/status",
  "GET /steam/watch-steamapps",
  "GET /update/check",
  "GET /update/progress",
  "GET /wine-mono/status",
  "POST /bottles/doctor",
  "POST /bottles/edit",
  "POST /bottles/prepare",
  "POST /bottles/refresh",
  "POST /bottles/relaunch-installer",
  "POST /bottles/repair-component",
  "POST /bottles/set-runtime-profile",
  "POST /bottles/set-windows-version",
  "POST /cache/clear",
  "POST /config",
  "POST /d3dmetal/bottles/install-homebrew-gptk",
  "POST /d3dmetal/bottles/install-rosetta",
  "POST /d3dmetal/bottles/install-x64-redist",
  "POST /d3dmetal/bottles/play",
  "POST /d3dmetal/bottles/repair-gptk-payload",
  "POST /d3dmetal/bottles/save",
  "POST /d3dmetal/bottles/seed-prefix",
  "POST /d3dmetal/bottles/status",
  "POST /diagnostics/open",
  "POST /eac/toggle",
  "POST /game/launch-auto",
  "POST /goldberg/toggle",
  "POST /kill",
  "POST /logs/crash-report",
  "POST /metalfx/toggle",
  "POST /mtsp/doctor",
  "POST /processes/force-kill",
  "POST /setup/install-all",
  "POST /setup/install-vcpp-x64",
  "POST /setup/install-vcpp-x86",
  "POST /setup/save",
  "POST /sharp-library/add-asset",
  "POST /sharp-library/doctor",
  "POST /sharp-library/gog/auth-code",
  "POST /sharp-library/gog/initialize-prefix",
  "POST /sharp-library/gog/install",
  "POST /sharp-library/gog/logout",
  "POST /sharp-library/gog/play",
  "POST /sharp-library/gog/progress",
  "POST /sharp-library/gog/remove-prefix",
  "POST /sharp-library/gog/stop",
  "POST /sharp-library/gog/sync",
  "POST /sharp-library/gog/uninstall",
  "POST /sharp-library/import-bottle-app",
  "POST /sharp-library/install",
  "POST /sharp-library/launch",
  "POST /sharp-library/set-cover",
  "POST /sharp-library/set-cover-position",
  "POST /sharp-library/set-engine",
  "POST /sharp-library/stop",
  "POST /sharp-library/uninstall",
  "POST /steam/install",
  "POST /steam/launch",
  "POST /steam/launch-game",
  "POST /steam/mac-install",
  "POST /steam/mac-launch",
  "POST /steam/mac-stop",
  "POST /steam/runtime-doctor",
  "POST /steam/save-api-key",
  "POST /steam/stop",
  "POST /steam/uninstall-game",
  "POST /update/start",
  "POST /wine-mono/install",
  "POST /wine-mono/reset",
] as const;

const MAIN_ONLY_ENDPOINTS = [
  "GET /update/dmg-path",
  "GET /update/migrate/check",
  "GET /update/migrate/progress",
  "POST /games/stop-active",
  "POST /update/cleanup",
  "POST /update/migrate/cleanup-preserved",
  "POST /update/migrate/start",
] as const;

const appIdQuery: QueryValidator = (params) => {
  if (params.size !== 1 || params.getAll("appid").length !== 1) return false;
  const value = params.get("appid") ?? "";
  if (!/^\d+$/.test(value)) return false;
  const appid = Number(value);
  return Number.isSafeInteger(appid) && appid >= 0 && appid <= 0xffffffff;
};

const prefixQuery: QueryValidator = (params) =>
  params.size === 1 && params.getAll("prefix").length === 1 && ["gog", "steam"].includes(params.get("prefix") ?? "");

const logStreamQuery: QueryValidator = (params) => {
  if (params.size !== 1 || params.getAll("after").length !== 1) return false;
  return /^(0|[1-9]\d*)$/.test(params.get("after") ?? "");
};

const QUERY_VALIDATORS = new Map<string, QueryValidator>([
  ["GET /diagnostics/m12/dry-run", appIdQuery],
  ["GET /eac/status", appIdQuery],
  ["GET /goldberg/status", appIdQuery],
  ["GET /logs/stream", logStreamQuery],
  ["GET /mtsp/pipelines", appIdQuery],
  ["GET /wine-mono/status", prefixQuery],
]);

const ENDPOINTS = new Map<string, EndpointRule>();
for (const key of RENDERER_ENDPOINTS) {
  ENDPOINTS.set(key, {
    sources: ["renderer", "main"],
    validateQuery: QUERY_VALIDATORS.get(key),
  });
}
for (const key of MAIN_ONLY_ENDPOINTS) {
  ENDPOINTS.set(key, { sources: ["main"] });
}

export type BackendRequestValidation = { ok: true; method: BackendMethod; url: string } | { ok: false; error: string };

export function validateBackendRequest(
  method: unknown,
  url: unknown,
  source: BackendRequestSource = "renderer",
): BackendRequestValidation {
  if (method !== "GET" && method !== "POST") {
    return { ok: false, error: "Backend method is not allowed" };
  }
  if (typeof url !== "string" || url.length === 0 || !url.startsWith("/") || url.startsWith("//")) {
    return { ok: false, error: "Backend URL must be a relative path" };
  }
  if (
    [...url].some((character) => character.charCodeAt(0) < 0x20 || character.charCodeAt(0) === 0x7f) ||
    /[\\%#]/.test(url)
  ) {
    return { ok: false, error: "Backend URL contains unsupported characters" };
  }

  let parsed: URL;
  try {
    parsed = new URL(url, "http://127.0.0.1");
  } catch {
    return { ok: false, error: "Backend URL is invalid" };
  }

  const pathPart = url.split("?", 1)[0];
  if (parsed.origin !== "http://127.0.0.1" || parsed.pathname !== pathPart || parsed.hash) {
    return { ok: false, error: "Backend URL is invalid" };
  }

  const key = `${method} ${parsed.pathname}`;
  const rule = ENDPOINTS.get(key);
  if (!rule?.sources.includes(source)) {
    return { ok: false, error: "Backend endpoint is not allowed" };
  }
  if (rule.validateQuery ? !rule.validateQuery(parsed.searchParams) : parsed.search !== "") {
    return { ok: false, error: "Backend query is not allowed" };
  }

  return { ok: true, method, url };
}

export function isBackendRequestBody(value: unknown): value is Record<string, unknown> | undefined {
  if (value === undefined) return true;
  if (value === null || typeof value !== "object" || Array.isArray(value)) return false;
  if (Object.getPrototypeOf(value) !== Object.prototype && Object.getPrototypeOf(value) !== null) return false;
  try {
    JSON.stringify(value);
    return true;
  } catch {
    return false;
  }
}
