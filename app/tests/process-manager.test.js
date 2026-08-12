const assert = require("node:assert/strict");
const { test } = require("node:test");

const {
  createProcessManagerScope,
  isMetalSharpOwnedWineProcess,
  isNonSteamMetalSharpWineProcess,
  isSteamWineProcess,
  stopMetalSharpWineProcesses,
} = require("../dist/main/process-manager.js");

const scope = createProcessManagerScope("/tmp/metalsharp-process-manager-test");
const currentPid = 999;

function row(pid, comm, command) {
  return { pid, comm, command };
}

test("only MetalSharp-owned Wine rows are eligible for process-manager cleanup", () => {
  assert.equal(
    isNonSteamMetalSharpWineProcess(
      row(100, "wine", "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine game.exe"),
      scope,
      currentPid,
    ),
    false,
  );
  assert.equal(
    isNonSteamMetalSharpWineProcess(row(101, "wineserver", "/opt/homebrew/bin/wineserver -f"), scope, currentPid),
    false,
  );
  assert.equal(
    isNonSteamMetalSharpWineProcess(
      row(102, "wine-preloader", "/tmp/metalsharp-process-manager-test-other/runtime/wine/bin/wine-preloader game.exe"),
      scope,
      currentPid,
    ),
    false,
  );
  assert.equal(
    isNonSteamMetalSharpWineProcess(
      row(103, "metalsharp-wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/metalsharp-wine game.exe"),
      scope,
      currentPid,
    ),
    true,
  );
  assert.equal(
    isNonSteamMetalSharpWineProcess(
      row(104, "wineboot", "/tmp/metalsharp-process-manager-test/prefix-steam/drive_c/windows/wineboot.exe --init"),
      scope,
      currentPid,
    ),
    true,
  );
  assert.equal(
    isNonSteamMetalSharpWineProcess(
      row(105, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/wine Steam.exe"),
      scope,
      currentPid,
    ),
    false,
  );
  assert.equal(
    isMetalSharpOwnedWineProcess(
      row(currentPid, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/wine game.exe"),
      scope,
      currentPid,
    ),
    false,
  );
});

test("wineserver shutdown receives the MetalSharp runtime and managed prefix", () => {
  const calls = [];
  const result = stopMetalSharpWineProcesses(
    [row(200, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/metalsharp-wine game.exe")],
    scope,
    {
      stopWithWineserver: (wineServer, prefix) => calls.push({ wineServer, prefix }),
      kill: () => assert.fail("scoped SIGKILL should not run after wineserver succeeds"),
    },
    true,
  );

  assert.deepEqual(calls, [{ wineServer: scope.wineServer, prefix: scope.managedPrefix }]);
  assert.deepEqual(result.killed, [200]);
  assert.equal(result.mode, "wineserver");
});

test("scoped SIGKILL is only the fallback when wineserver cannot stop the prefix", () => {
  const killed = [];
  const result = stopMetalSharpWineProcesses(
    [row(201, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/metalsharp-wine game.exe")],
    scope,
    {
      stopWithWineserver: () => {
        throw new Error("wineserver unavailable");
      },
      kill: (pid) => killed.push(pid),
    },
    true,
  );

  assert.deepEqual(killed, [201]);
  assert.deepEqual(result.killed, [201]);
  assert.equal(result.mode, "scoped-sigkill");
  assert.deepEqual(result.errors, []);
});

test("the graceful path can be disabled when the shared Steam prefix must remain alive", () => {
  const killed = [];
  const result = stopMetalSharpWineProcesses(
    [row(202, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/metalsharp-wine game.exe")],
    scope,
    {
      stopWithWineserver: () => assert.fail("shared Steam prefix should use the scoped fallback"),
      kill: (pid) => killed.push(pid),
    },
    false,
  );

  assert.deepEqual(killed, [202]);
  assert.equal(result.mode, "scoped-sigkill");
  assert.equal(
    isSteamWineProcess(row(203, "wine", "/tmp/metalsharp-process-manager-test/runtime/wine/bin/wine Steam.exe")),
    true,
  );
});
