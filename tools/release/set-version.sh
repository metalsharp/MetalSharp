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

node - "$VERSION" <<'NODE'
const fs = require("fs");
const version = process.argv[2];
const packageJson = JSON.parse(fs.readFileSync("app/package.json", "utf8"));
const packageLock = JSON.parse(fs.readFileSync("app/package-lock.json", "utf8"));
const cmake = fs.readFileSync("CMakeLists.txt", "utf8");
const cMakefile = fs.readFileSync("app/src-c/Makefile", "utf8");

const checks = [
  ["app/package.json version", packageJson.version === version],
  ["app/package-lock.json top-level version", packageLock.version === version],
  ['app/package-lock.json packages[""] version', packageLock.packages?.[""]?.version === version],
  ["app/src-c/Makefile backend version", cMakefile.includes(`VERSION ?= ${version}`)],
  ["CMakeLists.txt project version", cmake.includes(`project(metalsharp VERSION ${version} LANGUAGES C CXX OBJC OBJCXX)`)],
];

const failed = checks.filter(([, ok]) => !ok).map(([name]) => name);
if (failed.length) {
  console.error(`version bump verification failed for ${failed.length} location(s):`);
  for (const name of failed) console.error(`- ${name}`);
  process.exit(1);
}
console.log(`Updated ${checks.length} synchronized version locations to ${version}.`);
NODE
