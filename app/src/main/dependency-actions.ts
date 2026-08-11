export type BrewDependency = "game-porting-toolkit" | "molten-vk" | "mono";
export type DependencyScript = "install-metalsharp-wine-runtime";

export type InstallDepsAction = { kind: "brew"; package: BrewDependency } | { kind: "script"; name: DependencyScript };

const BREW_DEPENDENCIES: readonly BrewDependency[] = ["game-porting-toolkit", "molten-vk", "mono"];
const DEPENDENCY_SCRIPTS: Readonly<Record<DependencyScript, string>> = {
  "install-metalsharp-wine-runtime": "install-metalsharp-wine-runtime.sh",
};

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function hasExactKeys(value: Record<string, unknown>, keys: readonly string[]): boolean {
  const actual = Object.keys(value).sort();
  return actual.length === keys.length && actual.every((key, index) => key === [...keys].sort()[index]);
}

export function parseInstallDepsAction(value: unknown): InstallDepsAction | null {
  if (!isRecord(value) || typeof value.kind !== "string") return null;

  if (
    value.kind === "brew" &&
    typeof value.package === "string" &&
    BREW_DEPENDENCIES.includes(value.package as BrewDependency)
  ) {
    if (!hasExactKeys(value, ["kind", "package"])) return null;
    return { kind: "brew", package: value.package as BrewDependency };
  }

  if (
    value.kind === "script" &&
    typeof value.name === "string" &&
    Object.prototype.hasOwnProperty.call(DEPENDENCY_SCRIPTS, value.name)
  ) {
    if (!hasExactKeys(value, ["kind", "name"])) return null;
    return { kind: "script", name: value.name as DependencyScript };
  }

  return null;
}

export function dependencyScriptFileName(name: DependencyScript): string {
  return DEPENDENCY_SCRIPTS[name];
}
