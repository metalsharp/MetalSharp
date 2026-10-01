const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "../src/renderer/views/SharpView.vue"), "utf8");

assert.match(
  source,
  /async function openAppBottleFolder\(app: SharpApp\)\s*\{\s*const bottle = bottleForApp\(app\);\s*if \(!bottle\)\s*\{[\s\S]*?return;\s*\}\s*await openBottleFolder\(bottle\);\s*\}/,
  "the app folder action must resolve and open that app's bottle",
);
assert.match(source, /@click="openAppBottleFolder\(app\)"/, "the Tools action must open the app bottle");
assert.match(source, /:disabled="!bottleForApp\(app\)"/, "the action must be disabled when no bottle is linked");
assert.doesNotMatch(source, /pickAssetFile|\/sharp-library\/add-asset/, "do not call the unimplemented asset APIs");

console.log("Sharp Library bottle-folder regression checks passed");
