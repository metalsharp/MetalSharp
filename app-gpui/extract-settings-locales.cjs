#!/usr/bin/env node
// Build-time extraction only. Uses the project's trusted message definitions;
// never invokes Vue, initialLocale(), localStorage, or runtime backend APIs.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { execFileSync } = require('node:child_process');
let ts;
const candidates = [process.env.TYPESCRIPT_PATH, 'typescript'].filter(Boolean);
try { candidates.push(require.resolve('typescript', { paths: [path.resolve(__dirname, '../app')] })); } catch {}
for (const candidate of candidates) {
  try { ts = require(candidate); break; } catch {}
}
if (!ts) {
  try {
    const globalModules = execFileSync('npm', ['root', '-g'], { encoding: 'utf8', timeout: 10000 }).trim();
    ts = require(path.join(globalModules, 'typescript'));
  } catch {}
}
if (!ts) {
  try {
    const compiler = fs.realpathSync(execFileSync('which', ['tsc'], { encoding: 'utf8', timeout: 10000 }).trim());
    ts = require(path.resolve(path.dirname(compiler), '..'));
  } catch {}
}
if (!ts) throw new Error('An existing TypeScript compiler is required; set TYPESCRIPT_PATH. No packages are installed.');
const renderer = path.resolve(__dirname, '../app/src/renderer');
const source = fs.readFileSync(path.join(renderer, 'i18n.ts'), 'utf8');
const boundary = source.indexOf('function initialLocale():');
if (boundary < 0) throw new Error('i18n data/runtime boundary changed; review extraction before executing.');
const pure = source.slice(0, boundary).replace(/^import .*;\n/m, '');
const output = ts.transpileModule(pure, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS },
}).outputText;
const sandbox = { exports: {} };
vm.runInNewContext(output, sandbox, { timeout: 1000 });
const overlay = fs.readFileSync(path.join(renderer, 'components/SettingsOverlay.vue'), 'utf8');
const localeSources = overlay + fs.readFileSync(path.join(renderer, 'components/LanguagePicker.vue'), 'utf8');
const keys = [...new Set([...localeSources.matchAll(/\bt\(["']([^"']+)["']/g)].map(match => match[1]))].sort();
const lookup = (messages, key) => key.split('.').reduce((value, part) => value?.[part], messages);
const locales = {};
let fallbacks = 0;
for (const [code, messages] of Object.entries(sandbox.exports.messages)) {
  locales[code] = {};
  for (const key of keys) {
    let value = lookup(messages, key);
    if (typeof value !== 'string') {
      value = lookup(sandbox.exports.messages.en, key);
      fallbacks++;
    }
    if (typeof value !== 'string') throw new Error(`Missing Settings translation: ${code}: ${key}`);
    locales[code][key] = value;
  }
}
fs.writeFileSync(path.join(__dirname, 'assets/settings-locales.json'), JSON.stringify(locales, null, 2) + '\n');
console.log(`Extracted ${keys.length} keys for ${Object.keys(locales).length} locales (${fallbacks} existing English fallbacks).`);
