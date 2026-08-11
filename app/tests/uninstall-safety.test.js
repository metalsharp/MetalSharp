const { strict: assert } = require("node:assert");
const { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { afterEach, test } = require("node:test");
const { validateUninstallTarget } = require("../dist/main/uninstall-safety.js");

const temporaryRoots = [];

afterEach(() => {
  for (const root of temporaryRoots.splice(0)) {
    rmSync(root, { force: true, recursive: true });
  }
});

function fixture(withMarker = true) {
  const root = mkdtempSync(path.join(os.tmpdir(), "metalsharp-uninstall-safety-"));
  temporaryRoots.push(root);

  const home = path.join(root, "home");
  const target = path.join(home, ".metalsharp");
  mkdirSync(target, { recursive: true });
  if (withMarker) writeFileSync(path.join(target, "setup.json"), "{}\n");

  return { root, home, target };
}

test("allows a marked MetalSharp directory below the user's home", () => {
  const { home, target } = fixture();

  assert.deepEqual(validateUninstallTarget(target, home), { ok: true, path: target });
});

test("rejects the user's home directory even when it has a marker", () => {
  const { home } = fixture();
  writeFileSync(path.join(home, "setup.json"), "{}\n");

  const result = validateUninstallTarget(home, home);

  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /home directory/);
});

test("rejects the filesystem root", () => {
  const { home } = fixture();

  const result = validateUninstallTarget(path.parse(home).root, home);

  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /filesystem root/);
});

test("rejects a marked directory outside the user's home", () => {
  const { root, home } = fixture(false);
  const outside = path.join(root, "shared-data");
  mkdirSync(outside);
  writeFileSync(path.join(outside, "setup.json"), "{}\n");

  const result = validateUninstallTarget(outside, home);

  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /inside the user's home/);
});

test("rejects an unmarked directory", () => {
  const { home, target } = fixture(false);

  const result = validateUninstallTarget(target, home);

  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /marker.*not found/);
});

test("rejects a symbolic-link target", () => {
  const { home, target } = fixture();
  const link = path.join(home, "linked-metalsharp");
  symlinkSync(target, link, "dir");

  const result = validateUninstallTarget(link, home);

  assert.equal(result.ok, false);
  if (!result.ok) assert.match(result.reason, /symbolic link/);
});
