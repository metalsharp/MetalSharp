#!/usr/bin/env bash
# Verify an x86_64-build DMG (MetalSharp-<ver>-arm64.dmg) without installing it:
# - MetalSharp.app is com.metalsharp.app with MetalSharpVariant=regular and the
#   backend, host runtime and updater;
# - Resources/MetalSharp-Wine-x86_64.app is present, passes codesign --deep
#   --strict, has Contents/MacOS/wine, is Developer ID signed and by the outer
#   app's team;
# - no runtime bundles ship: MetalSharp-Wine-x86_64.app is the whole runtime.
# An optional source MetalSharp-Wine-x86_64.app must have the same CDHash.
# METALSHARP_X86_64_VERIFY_ALLOW_ADHOC=1 accepts an ad-hoc outer app (local and
# unsigned builds) and then skips the team check.
set -euo pipefail

DMG="${1:?usage: tools/dmg/verify-x86_64-dmg.sh path/to/MetalSharp-<ver>-arm64.dmg [source MetalSharp-Wine-x86_64.app]}"
SOURCE_WINE_APP="${2:-}"

fail() { echo "x86_64 DMG check failed: $*" >&2; exit 1; }

[ -s "$DMG" ] || fail "missing DMG: $DMG"
case "$(basename "$DMG")" in
  MetalSharp-*-arm64-FEX.dmg) fail "this is the FEX DMG: $DMG" ;;
  MetalSharp-*-arm64.dmg) ;;
  *) fail "DMG must be named MetalSharp-<version>-arm64.dmg: $DMG" ;;
esac

MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-x86_64-dmg-mount.XXXXXX")"
cleanup() {
  hdiutil detach "$MOUNT_DIR" -quiet 2>/dev/null || true
  rmdir "$MOUNT_DIR" 2>/dev/null || true
}
trap cleanup EXIT
hdiutil attach "$DMG" -mountpoint "$MOUNT_DIR" -nobrowse -readonly -quiet

APP="$MOUNT_DIR/MetalSharp.app"
[ -d "$APP" ] || fail "DMG does not contain MetalSharp.app"
INFO="$APP/Contents/Info.plist"
RESOURCES="$APP/Contents/Resources"
WINE_APP="$RESOURCES/MetalSharp-Wine-x86_64.app"
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

[ "$(plist "$INFO" CFBundleIdentifier)" = "com.metalsharp.app" ] || fail "MetalSharp.app is not com.metalsharp.app"
[ "$(plist "$INFO" MetalSharpVariant)" = "regular" ] || fail "MetalSharp.app Info.plist MetalSharpVariant is not regular"
VERSION="$(plist "$INFO" CFBundleShortVersionString)"
[ "$(basename "$DMG")" = "MetalSharp-$VERSION-arm64.dmg" ] || fail "DMG name does not match app version $VERSION"

for required in \
  "$RESOURCES/runtime/metalsharp-backend" \
  "$RESOURCES/runtime/host/manifest.json" \
  "$RESOURCES/runtime/host/HostRuntimeABI.h" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh"
do
  [ -s "$required" ] || fail "missing ${required#"$APP/"}"
done

[ -d "$WINE_APP" ] || fail "missing Contents/Resources/MetalSharp-Wine-x86_64.app"
[ -x "$WINE_APP/Contents/MacOS/wine" ] || fail "MetalSharp-Wine-x86_64.app has no Contents/MacOS/wine"
codesign --verify --deep --strict --verbose=2 "$WINE_APP" || fail "MetalSharp-Wine-x86_64.app does not verify"
codesign --verify --deep --strict --verbose=2 "$APP" || fail "MetalSharp.app does not verify"

signature() { codesign --display --verbose=4 "$1" 2>&1; }
field() { sed -n "s/^$1=//p" | head -1; }
WINE_SIGNATURE="$(signature "$WINE_APP")"
APP_SIGNATURE="$(signature "$APP")"
WINE_TEAM="$(field TeamIdentifier <<<"$WINE_SIGNATURE")"
APP_TEAM="$(field TeamIdentifier <<<"$APP_SIGNATURE")"
grep -q 'Authority=Developer ID Application' <<<"$WINE_SIGNATURE" \
  || fail "MetalSharp-Wine-x86_64.app is not Developer ID signed"
if [ -z "$APP_TEAM" ] || [ "$APP_TEAM" = "not set" ]; then
  [ "${METALSHARP_X86_64_VERIFY_ALLOW_ADHOC:-0}" = "1" ] || fail "MetalSharp.app is ad-hoc signed (set METALSHARP_X86_64_VERIFY_ALLOW_ADHOC=1 for unsigned builds)"
  echo "MetalSharp.app is ad-hoc signed; skipping the team id check (MetalSharp-Wine-x86_64.app team $WINE_TEAM)."
elif [ "$APP_TEAM" != "$WINE_TEAM" ]; then
  fail "team id mismatch: MetalSharp.app $APP_TEAM, MetalSharp-Wine-x86_64.app ${WINE_TEAM:-none}"
fi

if [ -n "$SOURCE_WINE_APP" ]; then
  SOURCE_CDHASH="$(signature "$SOURCE_WINE_APP" | field CDHash)"
  [ -n "$SOURCE_CDHASH" ] && [ "$SOURCE_CDHASH" = "$(field CDHash <<<"$WINE_SIGNATURE")" ] \
    || fail "MetalSharp-Wine-x86_64.app differs from its source $SOURCE_WINE_APP"
fi

# The runtime is MetalSharp-Wine-x86_64.app: no runtime bundles beside it.
if [ -d "$RESOURCES/bundles" ] && [ -n "$(find "$RESOURCES/bundles" -type f -print -quit)" ]; then
  fail "x86_64 build must not ship runtime bundles: $(find "$RESOURCES/bundles" -type f | head -5 | tr '\n' ' ')"
fi
unexpected="$(find "$RESOURCES" -path "$WINE_APP" -prune -o -name '*.tar.zst' -print | head -5)"
[ -z "$unexpected" ] || fail "x86_64 build carries runtime bundles: $unexpected"

echo "x86_64 DMG verified: $DMG (MetalSharp-Wine-x86_64.app team ${WINE_TEAM:-?}, outer ${APP_TEAM:-ad-hoc})" | sed "s/outer not set)/outer ad-hoc)/"
