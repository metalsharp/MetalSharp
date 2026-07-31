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

cleanup() {
  hdiutil detach "$MOUNT_DIR" -quiet 2>/dev/null || true
  rm -rf "$MOUNT_DIR"
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
RUNTIME_BUNDLE="$RESOURCES/runtime-bundle"

for required in \
  "$BACKEND" \
  "$HOST/manifest.json" \
  "$HOST/HostRuntimeABI.h" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh" \
  "$RUNTIME_BUNDLE/install-metalsharp-wine-runtime.sh" \
  "$RUNTIME_BUNDLE/metalsharp-bundle-manifest.tsv" \
  "$RUNTIME_BUNDLE/MetalSharp-Wine-Public-Source-2026-07-31.tar.zst" \
  "$RUNTIME_BUNDLE/MetalSharp-Wine-Public-Source-2026-07-31.tar.zst.sha256" \
  "$RUNTIME_BUNDLE/PARTS-SHA256SUMS.txt" \
  "$RUNTIME_BUNDLE/REASSEMBLE.txt"
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

if find "$RESOURCES/bundles" -maxdepth 1 -type f -name '*.tar.zst' -print -quit 2>/dev/null | grep -q .; then
  echo "DMG still contains retired split runtime bundles" >&2
  exit 1
fi

"$PROJECT_ROOT/tools/dmg/prepare-complete-runtime-assets.sh" --verify-package "$RUNTIME_BUNDLE"

echo "DMG runtime assets verified: $DMG"
