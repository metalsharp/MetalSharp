#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: tools/release/set-version.sh VERSION" >&2
  exit 2
fi

VERSION="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *)
    echo "invalid semver version: $VERSION" >&2
    exit 2
    ;;
esac

cd "$PROJECT_ROOT"

node - "$VERSION" <<'NODE'
const fs = require("fs");
const version = process.argv[2];

const packageJson = JSON.parse(fs.readFileSync("app/package.json", "utf8"));
packageJson.version = version;
fs.writeFileSync("app/package.json", `${JSON.stringify(packageJson, null, 2)}\n`);

const packageLock = JSON.parse(fs.readFileSync("app/package-lock.json", "utf8"));
packageLock.version = version;
if (!packageLock.packages || !packageLock.packages[""]) {
  throw new Error('app/package-lock.json is missing packages[""]');
}
packageLock.packages[""].version = version;
fs.writeFileSync("app/package-lock.json", `${JSON.stringify(packageLock, null, 2)}\n`);
NODE

perl -0pi -e "s/project\(metalsharp VERSION \K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" CMakeLists.txt
perl -0pi -e "s/^VERSION \?= \K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/m" app/src-c/Makefile
perl -0pi -e "s/#define MS_BACKEND_DEFAULT_VERSION \"\K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" \
  app/src-c/include/metalsharp_backend/backend.h
perl -0pi -e "s/#define MIGRATION_VERSION \"\K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" app/src-c/runtime/migration.c
perl -0pi -e "s/Bundled D3D9\/D3D10\/D3D11-to-Metal runtime \(\K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" \
  app/src-c/runtime/setup.c
perl -0pi -e "s/assert v\[\"version\"\] == \"\K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" app/src-c/tests/smoke.sh
perl -0pi -e "s{/releases/tag/v\K[0-9]+\.[0-9]+\.[0-9]+}{$VERSION}; s/filter=v\K[0-9]+\.[0-9]+\.[0-9]+/$VERSION/" \
  README.md

node - "$VERSION" <<'NODE'
const fs = require("fs");
const version = process.argv[2];

const replacements = [
  ["app/src-c/runtime/migration.c", /(\\"version\\":\\")[0-9]+\.[0-9]+\.[0-9]+/g],
  ["app/src-c/runtime/updater.c", /(\\"current_version\\":\\")[0-9]+\.[0-9]+\.[0-9]+/g],
];
for (const [path, pattern] of replacements) {
  const source = fs.readFileSync(path, "utf8");
  fs.writeFileSync(path, source.replace(pattern, `$1${version}`));
}
NODE

node - "$VERSION" <<'NODE'
const fs = require("fs");
const version = process.argv[2];
const packageJson = JSON.parse(fs.readFileSync("app/package.json", "utf8"));
const packageLock = JSON.parse(fs.readFileSync("app/package-lock.json", "utf8"));
const cmake = fs.readFileSync("CMakeLists.txt", "utf8");
const cMakefile = fs.readFileSync("app/src-c/Makefile", "utf8");
const backendHeader = fs.readFileSync("app/src-c/include/metalsharp_backend/backend.h", "utf8");
const migration = fs.readFileSync("app/src-c/runtime/migration.c", "utf8");
const updater = fs.readFileSync("app/src-c/runtime/updater.c", "utf8");
const setup = fs.readFileSync("app/src-c/runtime/setup.c", "utf8");
const smoke = fs.readFileSync("app/src-c/tests/smoke.sh", "utf8");
const readme = fs.readFileSync("README.md", "utf8");

const checks = [
  ["app/package.json version", packageJson.version === version],
  ["app/package-lock.json top-level version", packageLock.version === version],
  ['app/package-lock.json packages[""] version', packageLock.packages?.[""]?.version === version],
  ["app/src-c/Makefile backend version", cMakefile.includes(`VERSION ?= ${version}`)],
  ["CMakeLists.txt project version", cmake.includes(`project(metalsharp VERSION ${version} LANGUAGES C CXX OBJC OBJCXX)`)],
  ["C backend default version", backendHeader.includes(`MS_BACKEND_DEFAULT_VERSION "${version}"`)],
  ["migration version", migration.includes(`MIGRATION_VERSION "${version}"`)],
  ["migration fallback version", migration.includes(`\\"version\\":\\"${version}\\"`)],
  ["updater fallback version", updater.includes(`\\"current_version\\":\\"${version}\\"`)],
  ["setup runtime description", setup.includes(`runtime (${version}-m12-isolated-surface-v1)`) ],
  ["C smoke expected version", smoke.includes(`assert v["version"] == "${version}"`)],
  ["README release link", readme.includes(`/releases/tag/v${version}`)],
  ["README release badge", readme.includes(`filter=v${version}`)],
];

const failed = checks.filter(([, ok]) => !ok).map(([name]) => name);
if (failed.length) {
  console.error(`version bump verification failed for ${failed.length} location(s):`);
  for (const name of failed) console.error(`- ${name}`);
  process.exit(1);
}
console.log(`Updated ${checks.length} synchronized version locations to ${version}.`);
NODE
