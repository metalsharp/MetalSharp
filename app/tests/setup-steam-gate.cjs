const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { test } = require('node:test');

const backend = fs.readFileSync(
  path.join(__dirname, '../src-c/runtime/steam_actions.c'),
  'utf8',
);
const status = fs.readFileSync(
  path.join(__dirname, '../src-c/runtime/steam.c'),
  'utf8',
);
const wizard = fs.readFileSync(
  path.join(__dirname, '../src/renderer/components/SetupWizard.vue'),
  'utf8',
);

test('Steam install bootstraps fresh prefixes with wineboot', () => {
  assert.match(backend, /spawn_wine_install\(home, "wineboot", "--init", NULL, &pid\)/);
  assert.doesNotMatch(backend, /spawn_wine_install\(home, "cmd", "\/c", "exit 0"/);
});

test('Steam CDN responses are required to be valid PE payloads', () => {
  assert.match(backend, /"--fail", "--location", "--proto", "=https", "--tlsv1\.2"/);
  assert.match(backend, /steam_installer_payload_valid\(installer\)/);
});

test('Steam installer exit failures terminate promptly and report an error', () => {
  assert.match(backend, /steam_install_child_failed\(pid, waited, wait_status\)/);
  assert.match(backend, /stop_and_reap_steam_installer\(pid\)/);
  assert.match(backend, /write_steam_install_error\(home, failure_reason\)/);
  assert.match(status, /"install_error"/);
});

test('Setup wizard surfaces terminal backend failures and status read errors', () => {
  assert.match(wizard, /s\.install_stage === "failed"/);
  assert.match(wizard, /s\.install_error \?\? "Steam installation failed"/);
  assert.match(wizard, /Steam installation status could not be read/);
  assert.match(wizard, /reclaimFocusFromInstaller\(\)/);
});
