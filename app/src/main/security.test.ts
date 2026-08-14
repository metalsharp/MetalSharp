import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { parseInstallDepsAction } from "./dependency-actions";
import { isBackendRequestBody, validateBackendRequest } from "./ipc-security";
import { validateDownloadedDmg } from "./updater-security";

test("backend IPC only accepts allowlisted renderer routes and queries", () => {
  assert.deepEqual(validateBackendRequest("GET", "/status"), {
    ok: true,
    method: "GET",
    url: "/status",
  });
  assert.equal(validateBackendRequest("GET", "/diagnostics/vkd3d/dry-run?appid=1583230").ok, true);
  assert.equal(validateBackendRequest("GET", "/wine-mono/status?prefix=steam").ok, true);
  assert.equal(validateBackendRequest("POST", "/processes/force-kill").ok, true);
  assert.equal(validateBackendRequest("POST", "/update/start").ok, true);

  assert.equal(validateBackendRequest("DELETE", "/status").ok, false);
  assert.equal(validateBackendRequest("GET", "http://127.0.0.1/processes/force-kill").ok, false);
  assert.equal(validateBackendRequest("GET", "/processes/force-kill").ok, false);
  assert.equal(validateBackendRequest("GET", "/status?endpoint=/processes/force-kill").ok, false);
  assert.equal(validateBackendRequest("GET", "/diagnostics/vkd3d/dry-run?appid=not-a-number").ok, false);
  assert.equal(validateBackendRequest("GET", "/wine-mono/status?prefix=other").ok, false);

  assert.equal(validateBackendRequest("GET", "/update/migrate/check").ok, false);
  assert.equal(validateBackendRequest("GET", "/update/migrate/check", "main").ok, true);
});

test("backend IPC bodies are plain JSON records", () => {
  assert.equal(isBackendRequestBody(undefined), true);
  assert.equal(isBackendRequestBody({ appid: 123 }), true);
  assert.equal(isBackendRequestBody(Object.create(null)), true);
  assert.equal(isBackendRequestBody(null), false);
  assert.equal(isBackendRequestBody(["/processes/force-kill"]), false);
});

test("dependency IPC rejects free-form commands and unknown actions", () => {
  assert.deepEqual(parseInstallDepsAction({ kind: "brew", package: "mono" }), {
    kind: "brew",
    package: "mono",
  });
  assert.deepEqual(parseInstallDepsAction({ kind: "script", name: "install-metalsharp-wine-runtime" }), {
    kind: "script",
    name: "install-metalsharp-wine-runtime",
  });
  assert.equal(parseInstallDepsAction("brew install --formula malicious"), null);
  assert.equal(parseInstallDepsAction({ kind: "brew", package: "mono", args: ["--debug"] }), null);
  assert.equal(parseInstallDepsAction({ kind: "script", name: "../../tmp/payload" }), null);
  assert.equal(parseInstallDepsAction({ kind: "script", name: "toString" }), null);
  assert.equal(parseInstallDepsAction({ kind: "script", name: "__proto__" }), null);
  assert.equal(parseInstallDepsAction({ kind: "script", name: "constructor" }), null);
});

test("updater accepts only the backend-matched DMG in the update cache", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "metalsharp-447-"));
  const updatesDir = path.join(root, "cache", "updates");
  fs.mkdirSync(updatesDir, { recursive: true });
  const dmgPath = path.join(updatesDir, "MetalSharp-1.2.3.dmg");
  fs.writeFileSync(dmgPath, "dmg");

  try {
    const valid = validateDownloadedDmg(
      {
        ok: true,
        path: dmgPath,
        version: "1.2.3",
        size: 3,
        sha256: "00cbbd0ddbda2762798f7009838ed34ca1f12b93965813c7df22943bc62166d1",
      },
      updatesDir,
    );
    assert.equal(valid.ok, true);
    if (valid.ok) assert.equal(valid.artifact.path, fs.realpathSync(dmgPath));

    assert.equal(
      validateDownloadedDmg(
        {
          ok: true,
          path: path.join(root, "MetalSharp-1.2.3.dmg"),
          version: "1.2.3",
          size: 3,
          sha256: "00cbbd0ddbda2762798f7009838ed34ca1f12b93965813c7df22943bc62166d1",
        },
        updatesDir,
      ).ok,
      false,
    );
    assert.equal(
      validateDownloadedDmg(
        {
          ok: true,
          path: dmgPath,
          version: "1.2.4",
          size: 3,
          sha256: "00cbbd0ddbda2762798f7009838ed34ca1f12b93965813c7df22943bc62166d1",
        },
        updatesDir,
      ).ok,
      false,
    );

    const symlinkPath = path.join(updatesDir, "MetalSharp-1.2.3-arm64.dmg");
    fs.symlinkSync(dmgPath, symlinkPath);
    assert.equal(
      validateDownloadedDmg(
        {
          ok: true,
          path: symlinkPath,
          version: "1.2.3",
          size: 3,
          sha256: "00cbbd0ddbda2762798f7009838ed34ca1f12b93965813c7df22943bc62166d1",
        },
        updatesDir,
      ).ok,
      false,
    );
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
