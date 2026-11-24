#!/usr/bin/env bash
# Verify an All Runtimes DMG (MetalSharp-<ver>-All-Runtimes.dmg, #837) without installing it:
# - MetalSharp.app is com.metalsharp.app with MetalSharpVariant=regular and MetalSharpPackage=all, reads the All
#   Runtimes appcast (appcast-all.xml) when it has Sparkle, and has the backend, host runtime, updater and both rule
#   sets;
# - Resources/MetalSharp-Wine-x86_64.app and Resources/MetalSharp-Wine.app are both present, pass codesign --deep
#   --strict, have Contents/MacOS/wine, are Developer ID signed and by the outer app's team; MetalSharp-Wine.app keeps
#   its cross-architecture entitlement and provisioning profile;
# - no runtime bundles ship: the two Wine apps are the runtimes.
# Optional source MetalSharp-Wine-x86_64.app and MetalSharp-Wine.app must have the same CDHashes.
# METALSHARP_ALL_RUNTIMES_VERIFY_ALLOW_ADHOC=1 accepts an ad-hoc outer app (local and unsigned builds) and then skips
# the team check.
set -euo pipefail

DMG="${1:?usage: tools/dmg/verify-all-runtimes-dmg.sh path/to/MetalSharp-<ver>-All-Runtimes.dmg [source MetalSharp-Wine-x86_64.app] [source MetalSharp-Wine.app]}"
SOURCE_X86_64_WINE_APP="${2:-}"
SOURCE_FEX_WINE_APP="${3:-}"

fail() { echo "All Runtimes DMG check failed: $*" >&2; exit 1; }

[ -s "$DMG" ] || fail "missing DMG: $DMG"
case "$(basename "$DMG")" in
  MetalSharp-*-All-Runtimes.dmg) ;;
  *) fail "DMG must be named MetalSharp-<version>-All-Runtimes.dmg: $DMG" ;;
esac
# updaters pick the regular and FEX DMGs by "FEX" and "arm64" in the name (updater.c release_assets_find)
case "$(basename "$DMG" | tr '[:upper:]' '[:lower:]')" in
  *fex*|*arm64*) fail "the All Runtimes DMG's name must contain neither FEX nor arm64: $DMG" ;;
esac

MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-all-runtimes-dmg-mount.XXXXXX")"
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
X86_64_WINE_APP="$RESOURCES/MetalSharp-Wine-x86_64.app"
FEX_WINE_APP="$RESOURCES/MetalSharp-Wine.app"
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null || true; }

[ "$(plist "$INFO" CFBundleIdentifier)" = "com.metalsharp.app" ] || fail "MetalSharp.app is not com.metalsharp.app"
[ "$(plist "$INFO" MetalSharpVariant)" = "regular" ] || fail "MetalSharp.app Info.plist MetalSharpVariant is not regular"
[ "$(plist "$INFO" MetalSharpPackage)" = "all" ] || fail "MetalSharp.app Info.plist MetalSharpPackage is not all"
FEED="$(plist "$INFO" SUFeedURL)"
if [ -n "$FEED" ]; then
  case "$FEED" in
    */appcast-all.xml) ;;
    *) fail "the All Runtimes app must read appcast-all.xml, not $FEED" ;;
  esac
fi
VERSION="$(plist "$INFO" CFBundleShortVersionString)"
[ "$(basename "$DMG")" = "MetalSharp-$VERSION-All-Runtimes.dmg" ] || fail "DMG name does not match app version $VERSION"

for required in \
  "$RESOURCES/runtime/metalsharp-backend" \
  "$RESOURCES/runtime/host/manifest.json" \
  "$RESOURCES/runtime/host/HostRuntimeABI.h" \
  "$RESOURCES/scripts/tools/updater/update.py" \
  "$RESOURCES/scripts/tools/updater/update.sh" \
  "$RESOURCES/configs/mtsp-rules.toml" \
  "$RESOURCES/configs/mtsp-rules-fex.toml"
do
  [ -s "$required" ] || fail "missing ${required#"$APP/"}"
done

for wine_app in "$X86_64_WINE_APP" "$FEX_WINE_APP"; do
  name="$(basename "$wine_app")"
  [ -d "$wine_app" ] || fail "missing Contents/Resources/$name"
  [ -x "$wine_app/Contents/MacOS/wine" ] || fail "$name has no Contents/MacOS/wine"
  codesign --verify --deep --strict --verbose=2 "$wine_app" || fail "$name does not verify"
done
[ -s "$FEX_WINE_APP/Contents/embedded.provisionprofile" ] || fail "MetalSharp-Wine.app has no embedded.provisionprofile"
codesign --verify --deep --strict --verbose=2 "$APP" || fail "MetalSharp.app does not verify"
FEX_ENTITLEMENTS="$(codesign --display --entitlements - --xml "$FEX_WINE_APP" 2>/dev/null || true)"
grep -q 'com.apple.developer.cross-architecture-support' <<<"$FEX_ENTITLEMENTS" \
  || fail "MetalSharp-Wine.app lost com.apple.developer.cross-architecture-support"

signature() { codesign --display --verbose=4 "$1" 2>&1; }
field() { sed -n "s/^$1=//p" | head -1; }
APP_TEAM="$(signature "$APP" | field TeamIdentifier)"
for pair in "$X86_64_WINE_APP|$SOURCE_X86_64_WINE_APP" "$FEX_WINE_APP|$SOURCE_FEX_WINE_APP"; do
  wine_app="${pair%%|*}"
  source_app="${pair#*|}"
  name="$(basename "$wine_app")"
  wine_signature="$(signature "$wine_app")"
  wine_team="$(field TeamIdentifier <<<"$wine_signature")"
  grep -q 'Authority=Developer ID Application' <<<"$wine_signature" || fail "$name is not Developer ID signed"
  if [ -z "$APP_TEAM" ] || [ "$APP_TEAM" = "not set" ]; then
    [ "${METALSHARP_ALL_RUNTIMES_VERIFY_ALLOW_ADHOC:-0}" = "1" ] \
      || fail "MetalSharp.app is ad-hoc signed (set METALSHARP_ALL_RUNTIMES_VERIFY_ALLOW_ADHOC=1 for unsigned builds)"
    echo "MetalSharp.app is ad-hoc signed; skipping the team id check ($name team $wine_team)."
  elif [ "$APP_TEAM" != "$wine_team" ]; then
    fail "team id mismatch: MetalSharp.app $APP_TEAM, $name ${wine_team:-none}"
  fi
  if [ -n "$source_app" ]; then
    source_cdhash="$(signature "$source_app" | field CDHash)"
    [ -n "$source_cdhash" ] && [ "$source_cdhash" = "$(field CDHash <<<"$wine_signature")" ] \
      || fail "$name differs from its source $source_app"
  fi
done

# The runtimes are the two Wine apps: no runtime bundles beside them.
if [ -d "$RESOURCES/bundles" ] && [ -n "$(find "$RESOURCES/bundles" -type f -print -quit)" ]; then
  fail "the All Runtimes build must not ship runtime bundles: $(find "$RESOURCES/bundles" -type f | head -5 | tr '\n' ' ')"
fi
unexpected="$(find "$RESOURCES" \( -path "$X86_64_WINE_APP" -o -path "$FEX_WINE_APP" \) -prune -o -name '*.tar.zst' -print | head -5)"
[ -z "$unexpected" ] || fail "the All Runtimes build carries runtime bundles: $unexpected"

echo "All Runtimes DMG verified: $DMG (outer ${APP_TEAM:-ad-hoc})" | sed "s/outer not set)/outer ad-hoc)/"
