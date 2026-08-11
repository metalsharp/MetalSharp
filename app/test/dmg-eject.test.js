const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const { ejectDmgVolume } = require("../dist/main/dmg-eject.js");

test("passes an untrusted volume name as an argument without invoking a shell", async () => {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "metalsharp-dmg-eject-"));
  const capturePath = path.join(tempDir, "args");
  const markerPath = path.join(tempDir, "shell-executed");
  const fakeHdiutilPath = path.join(tempDir, "hdiutil");
  const volumeName = `Installer $(touch ${markerPath}) \`echo pwned\`;"`;
  const previousPath = process.env.PATH;
  const previousCapturePath = process.env.METALSHARP_TEST_CAPTURE;

  fs.writeFileSync(fakeHdiutilPath, '#!/bin/sh\nprintf \'%s\\n\' "$@" > "$METALSHARP_TEST_CAPTURE"\n');
  fs.chmodSync(fakeHdiutilPath, 0o755);
  process.env.PATH = `${tempDir}${path.delimiter}${previousPath ?? ""}`;
  process.env.METALSHARP_TEST_CAPTURE = capturePath;

  try {
    await ejectDmgVolume(volumeName);

    assert.equal(fs.existsSync(markerPath), false);
    assert.deepEqual(fs.readFileSync(capturePath, "utf8").trimEnd().split("\n"), [
      "detach",
      `/Volumes/${volumeName}`,
      "-quiet",
    ]);
  } finally {
    if (previousPath === undefined) delete process.env.PATH;
    else process.env.PATH = previousPath;
    if (previousCapturePath === undefined) delete process.env.METALSHARP_TEST_CAPTURE;
    else process.env.METALSHARP_TEST_CAPTURE = previousCapturePath;
    fs.rmSync(tempDir, { force: true, recursive: true });
  }
});

test("propagates hdiutil failures", async () => {
  const error = new Error("detach failed");

  await assert.rejects(
    ejectDmgVolume("Installer", (_file, _args, _options, callback) => callback(error)),
    error,
  );
});
