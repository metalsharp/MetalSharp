const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const repo = path.resolve(__dirname, '..');
const winePatch = fs.readFileSync(
  path.join(repo, '..', 'tools', 'bundles', 'wine', '0001-x87sidecar-cooperative.patch'),
  'utf8',
);
const routes = fs.readFileSync(path.join(repo, 'src-c', 'runtime', 'steam_actions.c'), 'utf8');
const verifier = fs.readFileSync(path.join(repo, '..', 'tools', 'bundles', 'verify-bundles.sh'), 'utf8');
const repairer = fs.readFileSync(path.join(repo, '..', 'tools', 'dmg', 'repair-runtime-bundle.py'), 'utf8');
const bundler = fs.readFileSync(path.join(repo, '..', 'tools', 'dmg', 'create-bundles.sh'), 'utf8');
const sdkVerifier = fs.readFileSync(path.join(repo, '..', 'tools', 'bundles', 'verify-developer-sdk.sh'), 'utf8');
const sdkBuilder = fs.readFileSync(path.join(repo, '..', 'tools', 'bundles', 'create-developer-sdk.py'), 'utf8');
const dmgVerifier = fs.readFileSync(path.join(repo, '..', 'tools', 'dmg', 'verify-dmg-runtime-assets.sh'), 'utf8');

test('Wine sidecar selection uses Wine-resolved machine metadata', () => {
  assert.match(winePatch, /loader_exec\( argv, machine, TRUE \)/);
  assert.match(winePatch, /loader_exec\( new_argv, machine, FALSE \)/);
  assert.match(winePatch, /machine == IMAGE_FILE_MACHINE_I386/);
  assert.doesNotMatch(winePatch, /get_pe_file_machine\( argv\[2\] \)/);
  assert.match(winePatch, /--cooperative/);
  assert.match(winePatch, /x87sidecar_is_regular_file/);
  assert.doesNotMatch(winePatch, /atoi\( dot \+ 1 \)/);
  assert.match(winePatch, /x87sidecar\.\%d/);
  assert.ok(winePatch.indexOf('--cooperative') < winePatch.indexOf('#ifdef HAVE_WINE_PRELOADER'));
});

test('sidecar remains scoped to the restored D3D9 route and verified asset', () => {
  assert.match(routes, /pipeline_is_d3d9\(pipeline\)/);
  assert.match(routes, /ROSETTA_X87_PATH/);
  assert.match(verifier, /X87SIDECAR_SHA256=/);
  assert.match(verifier, /METALSHARP_REQUIRE_X87SIDECAR/);
  assert.match(verifier, /\[ -L .*x87sidecar/);
  assert.match(repairer, /METALSHARP_X87SIDECAR_PATH/);
  assert.match(repairer, /require_x87sidecar/);
  assert.match(repairer, /runtime_root \/ "wine" \/ "bin" \/ "x87sidecar"/);
  assert.match(bundler, /REPAIR_ARGS=\(/);
  assert.match(bundler, /METALSHARP_REQUIRE_X87SIDECAR=1/);
  assert.match(sdkVerifier, /X87SIDECAR_SHA256=/);
  assert.match(sdkVerifier, /ROSETTA_X87_PATH/);
  assert.match(sdkBuilder, /require_x87sidecar/);
  assert.match(sdkBuilder, /require_patched_ntdll/);
  assert.match(dmgVerifier, /METALSHARP_REQUIRE_X87SIDECAR=1/);
  assert.equal(dmgVerifier.includes('verify-developer-sdk.sh'), false);
  assert.equal(dmgVerifier.includes('metalsharp-d3d12-developer-sdk.tar.zst'), false);
});

test('runtime verifier uses file-backed extraction for selective sidecar checks', () => {
  assert.ok(verifier.includes('extract_bundle_members "$path" "$tmp" "$members" "RUNTIME x87sidecar"'));
});
