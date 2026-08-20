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
  "$RESOURCES/scripts/tools/migrator/migrate-to-vkmt-runtime.sh" \
  "$BUNDLES/metalsharp-steam.tar.zst" \
  "$BUNDLES/goldberg.tar.zst"
do
  if [ ! -s "$required" ]; then
    echo "DMG missing required runtime asset: ${required#"$APP_DIR"/}" >&2
    exit 1
  fi
done

bundle_count="$(find "$BUNDLES" -maxdepth 1 -type f -name '*.tar.zst' | wc -l | tr -d ' ')"
if [ "$bundle_count" -ne 2 ]; then
  echo "DMG must contain exactly two runtime bundles (Steam and Goldberg); found $bundle_count" >&2
  find "$BUNDLES" -maxdepth 1 -type f -name '*.tar.zst' -print >&2
  exit 1
fi

if [ ! -s "$HOST/libmetalsharp_host_runtime.dylib" ] \
  && [ ! -s "$HOST/libmetalsharp_host_runtime.so" ] \
  && [ ! -s "$HOST/metalsharp_host_runtime.dll" ]; then
  echo "DMG host runtime has no non-empty shared library" >&2
  exit 1
fi

cp "$BUNDLES/metalsharp-steam.tar.zst" "$LIST_DIR"/
cp "$BUNDLES/goldberg.tar.zst" "$LIST_DIR"/
"$PROJECT_ROOT/tools/bundles/verify-bundles.sh" --bundle-dir "$LIST_DIR" --require mac metalsharp-steam.tar.zst

goldberg_tmp="$LIST_DIR/goldberg"
mkdir -p "$goldberg_tmp"
tar --use-compress-program=unzstd -xf "$BUNDLES/goldberg.tar.zst" -C "$goldberg_tmp"
for required in \
  "$goldberg_tmp/x86/steam_api.dll" \
  "$goldberg_tmp/x64/steam_api64.dll"; do
  if [ ! -s "$required" ]; then
    echo "DMG Goldberg bundle missing: ${required#"$goldberg_tmp"/}" >&2
    exit 1
  fi
done

echo "DMG runtime assets verified: $DMG"
