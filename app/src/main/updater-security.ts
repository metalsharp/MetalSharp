import * as fs from "fs";
import * as path from "path";

export interface VerifiedUpdateArtifact {
  path: string;
  version: string;
  size: number;
  sha256: string;
}

export type UpdateArtifactValidation = { ok: true; artifact: VerifiedUpdateArtifact } | { ok: false; error: string };

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isWithinDirectory(candidate: string, directory: string): boolean {
  return candidate.startsWith(`${directory}${path.sep}`);
}

export function validateDownloadedDmg(artifact: unknown, updatesDir: string): UpdateArtifactValidation {
  if (
    !isRecord(artifact) ||
    artifact.ok !== true ||
    typeof artifact.path !== "string" ||
    typeof artifact.version !== "string" ||
    typeof artifact.size !== "number" ||
    !Number.isSafeInteger(artifact.size) ||
    artifact.size <= 0 ||
    typeof artifact.sha256 !== "string" ||
    !/^[0-9a-fA-F]{64}$/.test(artifact.sha256)
  ) {
    return { ok: false, error: "Backend did not report a downloadable update with integrity metadata" };
  }
  if (!/^\d+(?:\.\d+){1,3}$/.test(artifact.version)) {
    return { ok: false, error: "Backend reported an invalid update version" };
  }

  const updatesRoot = path.resolve(updatesDir);
  const candidate = path.resolve(artifact.path);
  if (!isWithinDirectory(candidate, updatesRoot)) {
    return { ok: false, error: "Update DMG must be inside the MetalSharp update cache" };
  }

  const expectedNames = new Set([`MetalSharp-${artifact.version}.dmg`, `MetalSharp-${artifact.version}-arm64.dmg`]);
  if (!expectedNames.has(path.basename(candidate))) {
    return { ok: false, error: "Update DMG filename does not match the backend-reported version" };
  }

  try {
    const linkStat = fs.lstatSync(candidate);
    if (!linkStat.isFile() || linkStat.isSymbolicLink()) {
      return { ok: false, error: "Update DMG is not a regular file" };
    }
    const resolvedRoot = fs.realpathSync(updatesRoot);
    const resolvedCandidate = fs.realpathSync(candidate);
    if (!isWithinDirectory(resolvedCandidate, resolvedRoot)) {
      return { ok: false, error: "Update DMG resolves outside the MetalSharp update cache" };
    }
    const fileStat = fs.statSync(resolvedCandidate);
    if (!fileStat.isFile() || fileStat.size === 0 || fileStat.size !== artifact.size) {
      return { ok: false, error: "Update DMG is empty or does not match the reported size" };
    }
    return {
      ok: true,
      artifact: {
        path: resolvedCandidate,
        version: artifact.version,
        size: artifact.size,
        sha256: artifact.sha256.toLowerCase(),
      },
    };
  } catch {
    return { ok: false, error: "Update DMG is not available in the MetalSharp update cache" };
  }
}
