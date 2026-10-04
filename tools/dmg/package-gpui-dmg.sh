#!/usr/bin/env bash
# Build the distributable DMG from a packaged GPUI MetalSharp.app
# (app-gpui/package-app.sh). Same layout and name electron-builder produced:
# the app plus an /Applications link, MetalSharp-<version>-arm64.dmg.
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
codesign --verify --deep --strict "$APP"

mkdir -p "$OUT_DIR"
DMG="$OUT_DIR/MetalSharp-$VERSION-arm64.dmg"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-dmg-stage.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

ditto "$APP" "$STAGE/MetalSharp.app"
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
