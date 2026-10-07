#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${METALSHARP_GPUI_TEST_VERSION:-0.78.0}"
BUNDLE_SOURCE="${METALSHARP_GPUI_BUNDLE_SOURCE:-$ROOT/dist/bundles}"
OUT_DIR="$ROOT/dist/gpui-testing"
APP="$ROOT/app-gpui/target/MetalSharp-GPUI-Test.app"
DMG="$OUT_DIR/MetalSharp-GPUI-$VERSION-test-arm64.dmg"

if [[ ! "$VERSION" =~ ^[0-9]+([.][0-9]+){1,2}$ ]]; then
  echo "Invalid METALSHARP_GPUI_TEST_VERSION: $VERSION" >&2
  exit 1
fi

# Refuse stale, incomplete, or unexpected archives before packaging.
python3 - "$BUNDLE_SOURCE" <<'PY'
import csv
import hashlib
import pathlib
import sys

source = pathlib.Path(sys.argv[1])
expected = {
    "metalsharp-graphics-dll.tar.zst",
    "metalsharp-runtime.tar.zst",
    "metalsharp-assets.tar.zst",
    "fnalibs.tar.zst",
    "metalsharp-scripts-tools.tar.zst",
    "metalsharp-steam.tar.zst",
}
manifest = source / "metalsharp-bundle-manifest.tsv"
if not manifest.is_file():
    raise SystemExit(f"Missing verified release manifest: {manifest}")
with manifest.open(newline="") as stream:
    rows = list(csv.DictReader(stream, delimiter="\t"))
if {row["asset"] for row in rows} != expected:
    raise SystemExit("Release bundle manifest must contain exactly the six approved runtime archives")
if {path.name for path in source.glob("*.tar.zst")} != expected:
    raise SystemExit("Bundle source contains missing or unapproved tar.zst archives")
for row in rows:
    path = source / row["asset"]
    digest = hashlib.sha256()
    with path.open("rb") as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(chunk)
    if path.stat().st_size != int(row["size"]) or digest.hexdigest() != row["sha256"]:
        raise SystemExit(f"Bundle manifest mismatch: {path}")
print("Verified six approved runtime bundles against the release manifest")
PY

METALSHARP_GPUI_TEST_VERSION="$VERSION" \
METALSHARP_GPUI_BUNDLE_SOURCE="$BUNDLE_SOURCE" \
  "$ROOT/app-gpui/package-local-testing-app.sh"

/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" "$APP/Contents/Info.plist" | grep -Fx "$VERSION"
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" | grep -Fx 'dev.metalsharp.gpui-test'
codesign --verify --deep --strict "$APP"

mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d "$OUT_DIR/.stage.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT
ditto "$APP" "$STAGE/MetalSharp GPUI Test.app"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/READ-ME-GPUI-TEST.txt" <<EOF
MetalSharp GPUI $VERSION — local connected test build

This is the GPUI application and bundled C backend/runtime assets, not the synthetic UI preview.
It behaves exactly like the production MetalSharp app: it starts the backend on port 9274 with
~/.metalsharp, so the runtime installs to ~/.metalsharp/runtime and Steam to
~/.metalsharp/prefix-steam. Quit any other MetalSharp app before launching it; only one
backend can own port 9274. This local ad-hoc-signed build is not notarized or for distribution.
EOF

hdiutil create \
  -volname "MetalSharp GPUI $VERSION Test" \
  -srcfolder "$STAGE" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "$DMG"
hdiutil verify "$DMG"
"$ROOT/tools/dmg/verify-dmg-runtime-assets.sh" "$DMG"
echo "Built connected GPUI test DMG (not launched or installed): $DMG"
