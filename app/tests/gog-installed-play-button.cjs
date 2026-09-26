const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "../src/renderer/views/SharpView.vue"), "utf8");
const gogStart = source.indexOf("<template v-else-if=\"sourceMode === 'gog'\">");
const epicStart = source.indexOf("<template v-else-if=\"sourceMode === 'epic'\">", gogStart);
assert.ok(gogStart >= 0 && epicStart > gogStart, "GOG template section is present");

const gogTemplate = source.slice(gogStart, epicStart);
const gogGameStart = source.indexOf("interface GogGame {");
const gogGameEnd = source.indexOf("\ninterface Rpcs3Status", gogGameStart);
assert.ok(gogGameStart >= 0 && gogGameEnd > gogGameStart, "GOG game type is present");
const gogGameType = source.slice(gogGameStart, gogGameEnd);
assert.doesNotMatch(gogGameType, /bottleInitialized/, "GOG game response type has no per-game bottle state");
assert.match(
  gogTemplate,
  /v-else-if="game\.installed"\s+class="btn btn-play"[\s\S]*?@click="playGogGame\(game\)"/,
  "an installed GOG game must expose its Play action without relying on per-game bottle state",
);
assert.doesNotMatch(
  gogTemplate,
  /game\.bottleInitialized/,
  "GOG Play does not depend on nonexistent per-game bottle state",
);
assert.match(gogTemplate, /v-if="game\.installed"[\s\S]*?@click="uninstallGogGame\(game\)"/);

console.log("GOG installed Play button regression check passed");
