#!/usr/bin/env bash
# Verify a FEX DMG (MetalSharp-<ver>-arm64-FEX.dmg) without installing it:
# - MetalSharp.app is com.metalsharp.app with MetalSharpVariant=fex and the
#   backend, host runtime and updater the regular DMG also carries;
# - Resources/MetalSharp-Wine.app is present, passes codesign --deep --strict,
#   keeps its cross-architecture entitlement and provisioning profile, has
#   Contents/MacOS/wine, and is signed by the outer app's team;
# - none of the x86_64 build's runtime bundles, x87sidecar or D3DMetal ship.
# An optional source MetalSharp-Wine.app must have the same CDHash.
# METALSHARP_FEX_VERIFY_ALLOW_ADHOC=1 accepts an ad-hoc outer app (local and
# unsigned builds) and then skips the team check.
set -euo pipefail

DMG="${1:?usage: tools/dmg/verify-fex-dmg.sh path/to/MetalSharp-<ver>-arm64-FEX.dmg [source MetalSharp-Wine.app]}"
SOURCE_WINE_APP="${2:-}"

fail() { echo "FEX DMG check failed: $*" >&2; exit 1; }

[ -s "$DMG" ] || fail "missing DMG: $DMG"
case "$(basename "$DMG")" in
  MetalSharp-*-arm64-FEX.dmg) ;;
  *) fail "FEX DMG must be named MetalSharp-<version>-arm64-FEX.dmg: $DMG" ;;
esac

MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-fex-dmg-mount.XXXXXX")"
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
WINE_APP="$RESOURCES/MetalSharp-Wine.app"
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

[ "$(plist "$INFO" CFBundleIdentifier)" = "com.metalsharp.app" ] || fail "MetalSharp.app is not com.metalsharp.app"
[ "$(plist "$INFO" MetalSharpVariant)" = "fex" ] || fail "MetalSharp.app Info.plist MetalSharpVariant is not fex"
VERSION="$(plist "$INFO" CFBundleShortVersionString)"
[ "$(basename "$DMG")" = "MetalSharp-$VERSION-arm64-FEX.dmg" ] || fail "DMG name does not match app version $VERSION"

for required in \
  "$RESOURCES/runtime/metalsharp-backend" \
  "$RESOURCES/runtime/host/manifest.json" \
  "$RESOURCES/runtime/host/HostRuntimeABI.h" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh"
do
  [ -s "$required" ] || fail "missing ${required#"$APP/"}"
done

# MetalSharp-Wine.app: present, intact, entitled.
[ -d "$WINE_APP" ] || fail "missing Contents/Resources/MetalSharp-Wine.app"
[ -x "$WINE_APP/Contents/MacOS/wine" ] || fail "MetalSharp-Wine.app has no Contents/MacOS/wine"
[ -s "$WINE_APP/Contents/embedded.provisionprofile" ] || fail "MetalSharp-Wine.app has no embedded.provisionprofile"
codesign --verify --deep --strict --verbose=2 "$WINE_APP" || fail "MetalSharp-Wine.app does not verify"
codesign --verify --deep --strict --verbose=2 "$APP" || fail "MetalSharp.app does not verify"
WINE_ENTITLEMENTS="$(codesign --display --entitlements - --xml "$WINE_APP" 2>/dev/null || true)"
grep -q 'com.apple.developer.cross-architecture-support' <<<"$WINE_ENTITLEMENTS" \
  || fail "MetalSharp-Wine.app lost com.apple.developer.cross-architecture-support"

signature() { codesign --display --verbose=4 "$1" 2>&1; }
field() { sed -n "s/^$1=//p" | head -1; }
WINE_SIGNATURE="$(signature "$WINE_APP")"
APP_SIGNATURE="$(signature "$APP")"
WINE_TEAM="$(field TeamIdentifier <<<"$WINE_SIGNATURE")"
APP_TEAM="$(field TeamIdentifier <<<"$APP_SIGNATURE")"
grep -q 'Authority=Developer ID Application' <<<"$WINE_SIGNATURE" \
  || fail "MetalSharp-Wine.app is not Developer ID signed"
if [ -z "$APP_TEAM" ] || [ "$APP_TEAM" = "not set" ]; then
  [ "${METALSHARP_FEX_VERIFY_ALLOW_ADHOC:-0}" = "1" ] || fail "MetalSharp.app is ad-hoc signed (set METALSHARP_FEX_VERIFY_ALLOW_ADHOC=1 for unsigned builds)"
  echo "MetalSharp.app is ad-hoc signed; skipping the team id check (MetalSharp-Wine.app team $WINE_TEAM)."
elif [ "$APP_TEAM" != "$WINE_TEAM" ]; then
  fail "team id mismatch: MetalSharp.app $APP_TEAM, MetalSharp-Wine.app ${WINE_TEAM:-none}"
fi

if [ -n "$SOURCE_WINE_APP" ]; then
  SOURCE_CDHASH="$(signature "$SOURCE_WINE_APP" | field CDHash)"
  [ -n "$SOURCE_CDHASH" ] && [ "$SOURCE_CDHASH" = "$(field CDHash <<<"$WINE_SIGNATURE")" ] \
    || fail "MetalSharp-Wine.app differs from its source $SOURCE_WINE_APP"
fi

# No x86_64 runtime payload outside MetalSharp-Wine.app.
if [ -d "$RESOURCES/bundles" ] && [ -n "$(find "$RESOURCES/bundles" -type f -print -quit)" ]; then
  fail "FEX build must not ship runtime bundles: $(find "$RESOURCES/bundles" -type f | head -5 | tr '\n' ' ')"
fi
unexpected="$(find "$RESOURCES" -path "$WINE_APP" -prune -o \
  \( -iname '*x87sidecar*' -o -iname '*d3dmetal*' -o -name '*.tar.zst' \) -print | head -5)"
[ -z "$unexpected" ] || fail "FEX build carries x86_64 runtime payload: $unexpected"

echo "FEX DMG verified: $DMG (MetalSharp-Wine.app team ${WINE_TEAM:-?}, outer ${APP_TEAM:-ad-hoc})" | sed "s/outer not set)/outer ad-hoc)/"
