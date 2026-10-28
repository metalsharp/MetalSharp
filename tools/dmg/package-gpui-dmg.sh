#!/usr/bin/env bash
# Build the distributable DMG from a packaged GPUI MetalSharp.app
# (app-gpui/package-app.sh). Same layout and name electron-builder produced:
# the app plus an /Applications link, MetalSharp-<version>-arm64.dmg.
# METALSHARP_VARIANT=fex builds MetalSharp-<version>-arm64-FEX.dmg from a fex
# app (Info.plist MetalSharpVariant=fex) carrying Resources/MetalSharp-Wine.app,
# which is copied as is and verified, never re-signed.
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
codesign --verify --deep --strict "$APP"
WINE_APP_REL="Contents/Resources/MetalSharp-Wine.app"
if [ "$VARIANT" = "fex" ]; then
  [ -x "$APP/$WINE_APP_REL/Contents/MacOS/wine" ] || { echo "FEX app is missing $WINE_APP_REL" >&2; exit 1; }
  codesign --verify --deep --strict "$APP/$WINE_APP_REL"
fi

mkdir -p "$OUT_DIR"
if [ "$VARIANT" = "fex" ]; then
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
if [ "$VARIANT" = "fex" ]; then
  codesign --verify --deep --strict "$STAGE/MetalSharp.app/$WINE_APP_REL"
fi
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
