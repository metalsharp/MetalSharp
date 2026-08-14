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
NATIVE="$RESOURCES/scripts/tools/native"
ARCH_CHECK="$PROJECT_ROOT/tools/package/verify-macos-architecture.sh"
# The two explicit bundle paths below are the installed EAC substrate contract:
# Contents/Resources/scripts/tools/native/metalsharp_eac_substrate.dylib
# Contents/Resources/scripts/tools/native/metalsharp_eac_libc.so.6

for required in \
  "$BACKEND" \
  "$HOST/manifest.json" \
  "$HOST/HostRuntimeABI.h" \
  "$NATIVE/metalsharp_eac_substrate.dylib" \
  "$NATIVE/metalsharp_eac_libc.so.6" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh" \
  "$BUNDLES/metalsharp-electron.tar.zst" \
  "$BUNDLES/metalsharp-graphics-dll.tar.zst" \
  "$BUNDLES/metalsharp-runtime.tar.zst" \
  "$BUNDLES/metalsharp-assets.tar.zst" \
  "$BUNDLES/fnalibs.tar.zst" \
  "$BUNDLES/metalsharp-scripts-tools.tar.zst" \
  "$BUNDLES/metalsharp-steam.tar.zst"
do
  if [ ! -s "$required" ]; then
    echo "DMG missing required runtime asset: ${required#$APP_DIR/}" >&2
    exit 1
  fi
done

if ! file "$NATIVE/metalsharp_eac_substrate.dylib" | grep -q "Mach-O"; then
  echo "DMG EAC substrate is not a Mach-O dylib" >&2
  exit 1
fi
if ! file "$NATIVE/metalsharp_eac_substrate.dylib" | grep -q "x86_64"; then
  echo "DMG EAC substrate does not contain the x86_64 Wine/Rosetta slice" >&2
  exit 1
fi
if ! file "$NATIVE/metalsharp_eac_libc.so.6" | grep -q "ELF 64-bit.*x86-64"; then
  echo "DMG EAC symbol image is not an ELF64 x86-64 image" >&2
  exit 1
fi

if [ ! -s "$HOST/libmetalsharp_host_runtime.dylib" ] \
  && [ ! -s "$HOST/libmetalsharp_host_runtime.so" ] \
  && [ ! -s "$HOST/metalsharp_host_runtime.dll" ]; then
  echo "DMG host runtime has no non-empty shared library" >&2
  exit 1
fi

"$ARCH_CHECK" arm64 "$HOST/libmetalsharp_host_runtime.dylib"
"$ARCH_CHECK" arm64 "$NATIVE/metalsharp_launcher"
"$ARCH_CHECK" x86_64 "$NATIVE/metalsharp"

cp "$BUNDLES"/*.tar.zst "$LIST_DIR"/
"$PROJECT_ROOT/tools/bundles/verify-bundles.sh" --bundle-dir "$LIST_DIR" --require mac

echo "DMG runtime assets verified: $DMG"
