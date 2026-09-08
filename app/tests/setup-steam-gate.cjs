const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const ts = require('typescript');
const { test } = require('node:test');

const source = fs.readFileSync(require('node:path').join(__dirname, '../src/renderer/components/SetupWizard.vue'), 'utf8');
function fixture(api) {
  const script = source.split('<script setup lang="ts">')[1].split('</script>')[0].replace(/^import .*;\n/gm, '');
  const context = vm.createContext({
    ref: value => ({ value }), inject: (_, fallback) => fallback,
    defineEmits: () => () => {}, useToast: () => ({ show() {} }), api,
  });
  vm.runInContext(ts.transpileModule(script + '\nglobalThis.state = { step, installStatus, steamInstalled, steamInstalling, steamChecking, goToVcppStep, checkSteam };', {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.None },
  }).outputText, context);
  const s = context.state;
  s.step.value = 2;
  s.installStatus.value = 'complete';
  return s;
}

test('Next is bound to the Steam gate rather than direct navigation', () => {
  assert.match(source, /:disabled="!steamInstalled \|\| steamInstalling \|\| steamChecking"/);
  assert.match(source, /@click="goToVcppStep"/);
  assert.doesNotMatch(source, /@click="step = 3"/);
});
test('runtime completion alone cannot advance', async () => {
  const s = fixture(() => { throw Error('must not request'); });
  await s.goToVcppStep();
  assert.equal(s.step.value, 2);
});
test('fresh installed detection is required; running, installing and failed responses block', async () => {
  for (const response of [null, { running: true }, { installed: false }, { installed: true, installing: true }]) {
    const s = fixture(async () => response);
    s.steamInstalled.value = true;
    await s.goToVcppStep();
    assert.equal(s.step.value, 2);
    assert.equal(s.steamInstalled.value, false);
  }
});
test('pending confirmation blocks duplicate clicks and only advances after detection', async () => {
  let resolve, calls = 0;
  const s = fixture(() => { calls++; return new Promise(r => { resolve = r; }); });
  s.steamInstalled.value = true;
  const pending = s.goToVcppStep();
  assert.equal(s.step.value, 2);
  assert.equal(s.steamChecking.value, true);
  await s.goToVcppStep();
  assert.equal(calls, 1);
  resolve({ installed: true, installing: false });
  await pending;
  assert.equal(s.step.value, 3);
  assert.equal(s.steamChecking.value, false);
});
test('request errors fail closed', async () => {
  const s = fixture(async () => { throw Error('offline'); });
  s.steamInstalled.value = true;
  await s.goToVcppStep();
  assert.equal(s.step.value, 2);
  assert.equal(s.steamInstalled.value, false);
  assert.equal(s.steamChecking.value, false);
});
