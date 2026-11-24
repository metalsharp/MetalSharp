#!/usr/bin/env bash
# Build the distributable DMG from a packaged GPUI MetalSharp.app
# (app-gpui/package-app.sh). Same layout and name electron-builder produced:
# the app plus an /Applications link, MetalSharp-<version>-arm64.dmg.
# METALSHARP_VARIANT=fex builds MetalSharp-<version>-arm64-FEX.dmg from a fex
# app (Info.plist MetalSharpVariant=fex) carrying Resources/MetalSharp-Wine.app,
# which is copied as is and verified, never re-signed.
# METALSHARP_PACKAGE=all builds MetalSharp-<version>-All-Runtimes.dmg (#837) from the All Runtimes app (a regular app,
# MetalSharpPackage=all) carrying both MetalSharp-Wine-x86_64.app and MetalSharp-Wine.app. Its name has neither "FEX"
# nor "arm64", so no updater takes it for the regular or the FEX build's DMG: they pick by "FEX", and an "arm64" name
# over any other (app/src-c/runtime/updater.c release_assets_find, in every release since 1.0).
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <MetalSharp.app> <output-dir>" >&2
  exit 2
fi

APP="$1"
OUT_DIR="$2"
INFO="$APP/Contents/Info.plist"

if [ ! -s "$INFO" ] || [ ! -d "$APP/Contents/MacOS" ]; then
  echo "Not a packaged app bundle: $APP" >&2
  exit 1
fi
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$INFO")"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$INFO")"
if [ "$BUNDLE_ID" != "com.metalsharp.app" ]; then
  echo "Refusing to package $BUNDLE_ID; release DMGs carry com.metalsharp.app" >&2
  exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  echo "Invalid app version in $INFO: $VERSION" >&2
  exit 1
fi
VARIANT="${METALSHARP_VARIANT:-regular}"
APP_VARIANT="$(/usr/libexec/PlistBuddy -c 'Print :MetalSharpVariant' "$INFO" 2>/dev/null || echo regular)"
case "$VARIANT" in
  regular|fex) ;;
  *) echo "METALSHARP_VARIANT must be regular or fex" >&2; exit 1 ;;
esac
if [ "$APP_VARIANT" != "$VARIANT" ]; then
  echo "Refusing to package a $APP_VARIANT app as the $VARIANT DMG: $APP" >&2
  exit 1
fi
PACKAGE="${METALSHARP_PACKAGE:-single}"
APP_PACKAGE="$(/usr/libexec/PlistBuddy -c 'Print :MetalSharpPackage' "$INFO" 2>/dev/null || echo single)"
case "$PACKAGE" in
  single|all) ;;
  *) echo "METALSHARP_PACKAGE must be single or all" >&2; exit 1 ;;
esac
if [ "$APP_PACKAGE" != "$PACKAGE" ]; then
  echo "Refusing to package a MetalSharpPackage=$APP_PACKAGE app as the $PACKAGE DMG: $APP" >&2
  exit 1
fi
if [ "$PACKAGE" = "all" ] && [ "$VARIANT" != "regular" ]; then
  echo "The All Runtimes DMG is made from the regular app" >&2
  exit 1
fi
codesign --verify --deep --strict "$APP"
WINE_APP_REL="Contents/Resources/MetalSharp-Wine.app"
X86_64_WINE_APP_REL="Contents/Resources/MetalSharp-Wine-x86_64.app"
NESTED_WINE_APPS=()
[ "$VARIANT" = "fex" ] && NESTED_WINE_APPS=("$WINE_APP_REL")
[ "$PACKAGE" = "all" ] && NESTED_WINE_APPS=("$X86_64_WINE_APP_REL" "$WINE_APP_REL")
for nested in ${NESTED_WINE_APPS[@]+"${NESTED_WINE_APPS[@]}"}; do
  [ -x "$APP/$nested/Contents/MacOS/wine" ] || { echo "$APP_VARIANT app is missing $nested" >&2; exit 1; }
  codesign --verify --deep --strict "$APP/$nested"
done

mkdir -p "$OUT_DIR"
if [ "$PACKAGE" = "all" ]; then
  DMG="$OUT_DIR/MetalSharp-$VERSION-All-Runtimes.dmg"
  # two Wine apps of several GB each: stage beside the output, not in TMPDIR.
  STAGE="$(mktemp -d "$OUT_DIR/.metalsharp-dmg-stage.XXXXXX")"
elif [ "$VARIANT" = "fex" ]; then
  DMG="$OUT_DIR/MetalSharp-$VERSION-arm64-FEX.dmg"
  # MetalSharp-Wine.app is several GB: stage beside the output, not in TMPDIR.
  STAGE="$(mktemp -d "$OUT_DIR/.metalsharp-dmg-stage.XXXXXX")"
else
DMG="$OUT_DIR/MetalSharp-$VERSION-arm64.dmg"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-dmg-stage.XXXXXX")"
fi
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

ditto "$APP" "$STAGE/MetalSharp.app"
for nested in ${NESTED_WINE_APPS[@]+"${NESTED_WINE_APPS[@]}"}; do
  codesign --verify --deep --strict "$STAGE/MetalSharp.app/$nested"
done
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create \
  -volname "MetalSharp" \
  -srcfolder "$STAGE" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "$DMG"
hdiutil verify "$DMG"
echo "Built DMG: $DMG"
