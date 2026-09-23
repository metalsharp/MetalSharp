const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const routes = fs.readFileSync(
  path.join(__dirname, '../src-c/runtime/steam_actions.c'),
  'utf8',
);

test('advertises Rosetta AVX to Steam and direct Wine game launches', () => {
  assert.match(
    routes,
    /static void set_rosetta_avx_env\(void\)\s*\{[^}]*setenv\("ROSETTA_ADVERTISE_AVX",\s*"1",\s*1\);/s,
  );
  assert.match(
    routes,
    /static void set_route_paths\(const char\* home, const char\* pipeline\)\s*\{[^}]*set_rosetta_avx_env\(\);/s,
  );
  assert.match(
    routes,
    /static void set_pipeline_runtime_env\(const char\* home, const char\* pipeline\)\s*\{[^}]*set_rosetta_avx_env\(\);/s,
  );
});
