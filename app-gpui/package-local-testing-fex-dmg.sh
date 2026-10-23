#!/usr/bin/env bash
# Local FEX test DMG: package MetalSharp.app (com.metalsharp.app, the release
# identity, so it installs over 0.80.0 and is seen by update.sh) as the fex
# variant around a given MetalSharp-Wine.app, wrap it in
# MetalSharp-<ver>-arm64-FEX.dmg and verify it. Nothing is launched or installed.
#
#   METALSHARP_WINE_APP           signed MetalSharp-Wine.app to carry (default below)
#   METALSHARP_GPUI_SIGN_IDENTITY Developer ID Application identity; ad-hoc (-) by default
#   METALSHARP_GPUI_APP_VERSION   app version (default: CMakeLists.txt)
#   METALSHARP_BUILD_DIR          C++ build dir holding the host runtime (default: build/)
#   METALSHARP_DMG_OUT_DIR        output directory (default: dist/gpui-testing-fex)
#
# MetalSharp-Wine.app is copied with ditto and never re-signed; a Developer ID
# outer signature is not notarized here (tools/dmg/sign-notarize-dmg.sh does that).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_APP="${METALSHARP_WINE_APP:-/Volumes/AverySSD/MetalSharp-Wine-Arm64-FEX/dist/MetalSharp-Wine.app}"
OUT_DIR="${METALSHARP_DMG_OUT_DIR:-$ROOT/dist/gpui-testing-fex}"
BUILD_DIR="${METALSHARP_BUILD_DIR:-$ROOT/build}"
APP="$ROOT/app-gpui/target/MetalSharp.app"

[ "$(uname)" = "Darwin" ] || { echo "DMG creation requires macOS" >&2; exit 1; }
# rustup's cargo when this runs outside a login shell.
if ! command -v cargo >/dev/null && [ -x "$HOME/.cargo/bin/cargo" ]; then
  export PATH="$HOME/.cargo/bin:$PATH"
fi
[ -x "$WINE_APP/Contents/MacOS/wine" ] || { echo "Not a MetalSharp-Wine.app: $WINE_APP" >&2; exit 1; }
codesign --verify --deep --strict --verbose=2 "$WINE_APP"

# The host runtime comes from the C++ engine build, as in release CI.
if ! ls "$BUILD_DIR"/libmetalsharp_host_runtime.* >/dev/null 2>&1; then
  if [ ! -f "$ROOT/vendor/glslang/CMakeLists.txt" ]; then
    echo "No host runtime in $BUILD_DIR and the C++ engine's submodules are not checked out." >&2
    echo "Run 'git submodule update --init --recursive', or set METALSHARP_BUILD_DIR to a directory holding libmetalsharp_host_runtime.dylib." >&2
    exit 1
  fi
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  cmake --build "$BUILD_DIR" --target metalsharp_host_runtime --parallel "$(sysctl -n hw.ncpu)"
fi
export METALSHARP_BUILD_DIR="$BUILD_DIR"

METALSHARP_VARIANT=fex \
METALSHARP_WINE_APP="$WINE_APP" \
METALSHARP_GPUI_REQUIRE_BUNDLES=0 \
  "$ROOT/app-gpui/package-app.sh"

/usr/libexec/PlistBuddy -c 'Print :MetalSharpVariant' "$APP/Contents/Info.plist" | grep -Fx fex
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Contents/Info.plist" | grep -Fx com.metalsharp.app
codesign --verify --deep --strict "$APP"

mkdir -p "$OUT_DIR"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")"
METALSHARP_VARIANT=fex "$ROOT/tools/dmg/package-gpui-dmg.sh" "$APP" "$OUT_DIR"
DMG="$OUT_DIR/MetalSharp-$VERSION-arm64-FEX.dmg"

if [ "${METALSHARP_GPUI_SIGN_IDENTITY:--}" = "-" ]; then
  export METALSHARP_FEX_VERIFY_ALLOW_ADHOC=1
fi
"$ROOT/tools/dmg/verify-fex-dmg.sh" "$DMG" "$WINE_APP"
echo "Built local FEX test DMG (not launched, installed or notarized): $DMG"
