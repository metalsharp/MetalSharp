const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../src/renderer/views/LibraryView.vue'), 'utf8');
const css = source.split('<style scoped>')[1];
assert.ok(css, 'library styles present');
function rule(selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return css.match(new RegExp(escaped + '\\s*\\{([^}]+)\\}'))?.[1] ?? '';
}
for (const selector of ['.library-controls', '.library-control-button']) {
  assert.match(rule(selector), /-webkit-app-region:\s*no-drag;/, `${selector} must not intercept clicks for window dragging`);
}
assert.doesNotMatch(rule('.library-header'), /-webkit-app-region:/, 'no ancestor drag region may overlap controls');
assert.ok(!css.includes('.library-header::after'), 'no full-header overlay');
assert.match(rule('.library-drag-strip'), /height:\s*44px;/);
assert.match(rule('.library-drag-strip'), /-webkit-app-region:\s*drag;/);
assert.match(source, /class="library-drag-strip" aria-hidden="true"/);
assert.match(rule('.library-title-row'), /-webkit-app-region:\s*drag;/, 'title remains draggable');
assert.match(source, /@click="toggleSteam"/, 'Steam button handler retained');
console.log('library drag-region regression checks passed');
