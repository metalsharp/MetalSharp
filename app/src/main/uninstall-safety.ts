import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

const METALSHARP_MARKER = "setup.json";

export type UninstallTargetValidation = { ok: true; path: string } | { ok: false; path: string; reason: string };

function rejected(targetPath: string, reason: string): UninstallTargetValidation {
  return { ok: false, path: targetPath, reason };
}

function canonicalPath(candidate: string): string | null {
  try {
    return fs.realpathSync.native(candidate);
  } catch {
    return null;
  }
}

function isStrictDescendant(parent: string, candidate: string): boolean {
  const relative = path.relative(parent, candidate);
  return (
    relative.length > 0 && relative !== ".." && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative)
  );
}

/**
 * Check that a path is an actual, marked MetalSharp data directory before it
 * is passed to a recursive delete. The home directory is injected for tests;
 * production callers use the current user's home by default.
 */
export function validateUninstallTarget(targetPath: string, homePath = os.homedir()): UninstallTargetValidation {
  const resolvedTarget = path.resolve(targetPath);
  const resolvedHome = path.resolve(homePath);

  let targetStats: fs.Stats;
  try {
    targetStats = fs.lstatSync(resolvedTarget);
  } catch {
    return rejected(resolvedTarget, "The MetalSharp data directory could not be inspected.");
  }

  if (targetStats.isSymbolicLink()) {
    return rejected(resolvedTarget, "The resolved MetalSharp data path is a symbolic link.");
  }
  if (!targetStats.isDirectory()) {
    return rejected(resolvedTarget, "The resolved MetalSharp data path is not a directory.");
  }

  const canonicalHome = canonicalPath(resolvedHome);
  if (!canonicalHome) {
    return rejected(resolvedTarget, "The user's home directory could not be resolved.");
  }

  let homeStats: fs.Stats;
  try {
    homeStats = fs.statSync(resolvedHome);
  } catch {
    return rejected(resolvedTarget, "The user's home directory could not be inspected.");
  }
  if (targetStats.dev !== homeStats.dev) {
    return rejected(resolvedTarget, "The MetalSharp data directory is on a different filesystem than the user's home.");
  }

  const canonicalTarget = canonicalPath(resolvedTarget);
  if (!canonicalTarget) {
    return rejected(resolvedTarget, "The resolved MetalSharp data path could not be resolved.");
  }

  if (canonicalHome === path.parse(canonicalHome).root) {
    return rejected(resolvedTarget, "The user's home directory cannot be the filesystem root.");
  }
  if (canonicalTarget === path.parse(canonicalTarget).root) {
    return rejected(resolvedTarget, "Refusing to delete the filesystem root.");
  }
  if (canonicalTarget === canonicalHome) {
    return rejected(resolvedTarget, "Refusing to delete the user's home directory.");
  }
  if (!isStrictDescendant(canonicalHome, canonicalTarget)) {
    return rejected(resolvedTarget, "The MetalSharp data directory must be inside the user's home directory.");
  }

  const markerPath = path.join(resolvedTarget, METALSHARP_MARKER);
  try {
    const markerStats = fs.lstatSync(markerPath);
    if (!markerStats.isFile() || markerStats.isSymbolicLink()) {
      return rejected(resolvedTarget, `The MetalSharp marker ${METALSHARP_MARKER} is not a regular file.`);
    }
  } catch {
    return rejected(resolvedTarget, `The MetalSharp marker ${METALSHARP_MARKER} was not found.`);
  }

  return { ok: true, path: resolvedTarget };
}
