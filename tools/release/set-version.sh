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

python3 - "$VERSION" <<'PY'
import re
import sys

version = sys.argv[1]
major, minor, patch = (int(part) for part in version.split("."))
synthetic_test_version = f"{major}.{minor}.{patch + 1}"

for path, pattern in [
    ("app/src-c/runtime/migration.c", r'(\\"version\\":\\")[0-9]+\.[0-9]+\.[0-9]+'),
    ("app/src-c/runtime/updater.c", r'(\\"current_version\\":\\")[0-9]+\.[0-9]+\.[0-9]+'),
]:
    with open(path, encoding="utf-8") as stream:
        source = stream.read()
    with open(path, "w", encoding="utf-8") as stream:
        stream.write(re.sub(pattern, lambda match: match.group(1) + version, source))

updater_test_path = "app/src-c/tests/updater_test.py"
with open(updater_test_path, encoding="utf-8") as stream:
    updater_test = stream.read()
with open(updater_test_path, "w", encoding="utf-8") as stream:
    stream.write(re.sub(r'(VERSION = ")[0-9]+\.[0-9]+\.[0-9]+', lambda match: match.group(1) + synthetic_test_version, updater_test, count=1))
PY

python3 - "$VERSION" <<'PY'
import sys

version = sys.argv[1]
major, minor, patch = (int(part) for part in version.split("."))
synthetic_test_version = f"{major}.{minor}.{patch + 1}"


def read(path):
    with open(path, encoding="utf-8") as stream:
        return stream.read()


cmake = read("CMakeLists.txt")
c_makefile = read("app/src-c/Makefile")
backend_header = read("app/src-c/include/metalsharp_backend/backend.h")
migration = read("app/src-c/runtime/migration.c")
updater = read("app/src-c/runtime/updater.c")
setup = read("app/src-c/runtime/setup.c")
smoke = read("app/src-c/tests/smoke.sh")
updater_test = read("app/src-c/tests/updater_test.py")
readme = read("README.md")

checks = [
    ("app/src-c/Makefile backend version", f"VERSION ?= {version}" in c_makefile),
    ("CMakeLists.txt project version", f"project(metalsharp VERSION {version} LANGUAGES C CXX OBJC OBJCXX)" in cmake),
    ("C backend default version", f'MS_BACKEND_DEFAULT_VERSION "{version}"' in backend_header),
    ("migration version", f'MIGRATION_VERSION "{version}"' in migration),
    ("migration fallback version", f'\\"version\\":\\"{version}\\"' in migration),
    ("updater fallback version", f'\\"current_version\\":\\"{version}\\"' in updater),
    ("setup DXMT runtime contract", 'MS_BACKEND_VERSION "-dxmt-v0.80-baseline-v1"' in setup),
    ("C smoke expected version", f'assert v["version"] == "{version}"' in smoke),
    ("updater synthetic release remains newer than app version", f'VERSION = "{synthetic_test_version}"' in updater_test),
    ("README release link", f"/releases/tag/v{version}" in readme),
    ("README release badge", f"filter=v{version}" in readme),
]

failed = [name for name, ok in checks if not ok]
if failed:
    print(f"version bump verification failed for {len(failed)} location(s):", file=sys.stderr)
    for name in failed:
        print(f"- {name}", file=sys.stderr)
    sys.exit(1)
print(f"Version bump verified for {version}: {len(checks)} metadata/contract checks passed.")
PY
