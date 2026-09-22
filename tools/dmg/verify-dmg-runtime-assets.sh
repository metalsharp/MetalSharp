#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DMG="${1:?usage: tools/dmg/verify-dmg-runtime-assets.sh path/to/MetalSharp.dmg}"

if [ ! -s "$DMG" ]; then
  echo "Missing DMG: $DMG" >&2
  exit 1
fi

MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-dmg-mount.XXXXXX")"
LIST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-dmg-bundles.XXXXXX")"

cleanup() {
  hdiutil detach "$MOUNT_DIR" -quiet 2>/dev/null || true
  rm -rf "$MOUNT_DIR" "$LIST_DIR"
}
trap cleanup EXIT

hdiutil attach "$DMG" -mountpoint "$MOUNT_DIR" -nobrowse -quiet

APP_DIR="$(find "$MOUNT_DIR" -maxdepth 1 -name '*.app' -type d | head -n 1)"
if [ -z "$APP_DIR" ]; then
  echo "DMG does not contain a top-level .app" >&2
  exit 1
fi

RESOURCES="$APP_DIR/Contents/Resources"
BACKEND="$RESOURCES/runtime/metalsharp-backend"
HOST="$RESOURCES/runtime/host"
BUNDLES="$RESOURCES/bundles"

for required in \
  "$BACKEND" \
  "$HOST/manifest.json" \
  "$HOST/HostRuntimeABI.h" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh" \
  "$BUNDLES/metalsharp-electron.tar.zst" \
  "$BUNDLES/metalsharp-graphics-dll.tar.zst" \
  "$BUNDLES/metalsharp-runtime.tar.zst" \
  "$BUNDLES/metalsharp-assets.tar.zst" \
  "$BUNDLES/fnalibs.tar.zst" \
  "$BUNDLES/metalsharp-scripts-tools.tar.zst" \
  "$BUNDLES/metalsharp-steam.tar.zst" \
  "$BUNDLES/metalsharp-d3d12-developer-sdk.tar.zst"
do
  if [ ! -s "$required" ]; then
    echo "DMG missing required runtime asset: ${required#$APP_DIR/}" >&2
    exit 1
  fi
done

if [ ! -s "$HOST/libmetalsharp_host_runtime.dylib" ] \
  && [ ! -s "$HOST/libmetalsharp_host_runtime.so" ] \
  && [ ! -s "$HOST/metalsharp_host_runtime.dll" ]; then
  echo "DMG host runtime has no non-empty shared library" >&2
  exit 1
fi

cp "$BUNDLES"/*.tar.zst "$LIST_DIR"/
METALSHARP_REQUIRE_X87SIDECAR=1 "$PROJECT_ROOT/tools/bundles/verify-bundles.sh" --bundle-dir "$LIST_DIR" --require mac
"$PROJECT_ROOT/tools/bundles/verify-developer-sdk.sh" "$BUNDLES/metalsharp-d3d12-developer-sdk.tar.zst"

echo "DMG runtime assets verified: $DMG"
