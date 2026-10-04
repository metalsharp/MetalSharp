#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/app-gpui"
BUNDLE="$APP_DIR/target/MetalSharp-GPUI-Preview.app"
CONTENTS="$BUNDLE/Contents"
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$APP_DIR/target}"
PREVIEW_VERSION="${METALSHARP_GPUI_PREVIEW_VERSION:-0.1.0-preview}"
PREVIEW_BUILD_VERSION="${METALSHARP_GPUI_PREVIEW_BUILD_VERSION:-${PREVIEW_VERSION%%-*}}"
if [[ ! "$PREVIEW_VERSION" =~ ^[0-9]+([.][0-9]+){1,2}(-[A-Za-z0-9.-]+)?$ ]]; then
  echo "Invalid METALSHARP_GPUI_PREVIEW_VERSION: $PREVIEW_VERSION" >&2
  exit 1
fi
if [[ ! "$PREVIEW_BUILD_VERSION" =~ ^[0-9]+([.][0-9]+){1,2}$ ]]; then
  echo "Invalid METALSHARP_GPUI_PREVIEW_BUILD_VERSION: $PREVIEW_BUILD_VERSION" >&2
  exit 1
fi
mkdir -p "$CARGO_TARGET_DIR"
CARGO_TARGET_DIR="$(cd "$CARGO_TARGET_DIR" && pwd)"
export CARGO_TARGET_DIR

command -v cargo >/dev/null || { echo "cargo is required" >&2; exit 1; }
command -v codesign >/dev/null || { echo "codesign is required" >&2; exit 1; }
command -v swift >/dev/null || { echo "swift is required to generate dock artwork" >&2; exit 1; }

# Derived tilt variants are generated locally, not committed to the repository.
(cd "$APP_DIR" && swift generate-dock-art.swift)

CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-3}" MACOSX_DEPLOYMENT_TARGET=13.0 cargo build --locked --manifest-path "$APP_DIR/Cargo.toml"

rm -rf "$BUNDLE"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources/assets"
ICON_TMP="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-icon.XXXXXX")"
ICON_NAME="metalsharp-liquid-glass"
ICON_SOURCE="$APP_DIR/assets/$ICON_NAME.icon"
ICON_OUTPUT="$ICON_TMP/compiled"
trap 'rm -rf "$ICON_TMP"' EXIT
mkdir -p "$ICON_OUTPUT"
if [ -n "${ACTOOL:-}" ]; then
  ACTOOL_BIN="$ACTOOL"
elif ACTOOL_BIN="$(xcrun --find actool 2>/dev/null)"; then
  :
else
  echo "Apple actool from full Xcode is required to compile the Liquid Glass icon; set DEVELOPER_DIR or ACTOOL." >&2
  exit 1
fi
"$ACTOOL_BIN" \
  --compile "$ICON_OUTPUT" \
  --platform macosx \
  --minimum-deployment-target "${MACOSX_DEPLOYMENT_TARGET:-13.0}" \
  --app-icon "$ICON_NAME" \
  --output-partial-info-plist "$ICON_OUTPUT/partial-Info.plist" \
  "$ICON_SOURCE"
test -s "$ICON_OUTPUT/Assets.car" || { echo "actool did not produce Assets.car" >&2; exit 1; }
test -s "$ICON_OUTPUT/$ICON_NAME.icns" || { echo "actool did not produce the legacy ICNS icon" >&2; exit 1; }
install -m 0644 "$ICON_OUTPUT/Assets.car" "$CONTENTS/Resources/Assets.car"
install -m 0644 "$ICON_OUTPUT/$ICON_NAME.icns" "$CONTENTS/Resources/$ICON_NAME.icns"
install -m 0755 "$CARGO_TARGET_DIR/debug/metalsharp-gpui" "$CONTENTS/MacOS/MetalSharp-GPUI"
cp -R "$APP_DIR"/assets/. "$CONTENTS/Resources/assets/"

# Explicitly package the authoritative C backend/resources for connected testing.
# This builds/copies only; it never launches the backend, installers or accounts.
if [ "${METALSHARP_GPUI_PACKAGE_BACKEND:-0}" = "1" ]; then
  make -C "$ROOT/app/src-c"
  mkdir -p "$CONTENTS/Resources/runtime" "$CONTENTS/Resources/tools" "$CONTENTS/Resources/scripts/tools" "$CONTENTS/Resources/bundles"
  install -m 0755 "$ROOT/app/src-c/build/metalsharp-backend" "$CONTENTS/Resources/runtime/metalsharp-backend"
  for tool in zstd unzstd wrestool icotool unar lsar; do
    [ -f "$ROOT/app/tools/$tool" ] || { echo "Missing required packaged tool: $tool" >&2; exit 1; }
    install -m 0755 "$ROOT/app/tools/$tool" "$CONTENTS/Resources/tools/$tool"
  done
  for directory in lib licenses; do
    [ ! -d "$ROOT/app/tools/$directory" ] || cp -R "$ROOT/app/tools/$directory" "$CONTENTS/Resources/tools/"
  done
  install -m 0755 "$ROOT/tools/install-homebrew.sh" "$CONTENTS/Resources/scripts/tools/install-homebrew.sh"
  cp -R "$ROOT/app/updater" "$CONTENTS/Resources/scripts/tools/"
  cp -R "$ROOT/configs" "$CONTENTS/Resources/"
  # Do not rebuild archives or copy the Electron desktop payload.
  for archive in metalsharp-runtime metalsharp-assets metalsharp-graphics-dll metalsharp-scripts-tools metalsharp-steam metalsharp-d3d12-developer-sdk fnalibs; do
    [ ! -f "$ROOT/app/bundles/$archive.tar.zst" ] || cp "$ROOT/app/bundles/$archive.tar.zst" "$CONTENTS/Resources/bundles/"
  done
fi

cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>MetalSharp-GPUI</string>
  <key>CFBundleIdentifier</key><string>dev.metalsharp.gpui-preview</string>
  <key>CFBundleName</key><string>MetalSharp GPUI Preview</string>
  <key>CFBundleDisplayName</key><string>MetalSharp GPUI Preview</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>metalsharp-liquid-glass</string>
  <key>CFBundleIconName</key><string>metalsharp-liquid-glass</string>
  <key>CFBundleShortVersionString</key><string>$PREVIEW_VERSION</string>
  <key>CFBundleVersion</key><string>$PREVIEW_BUILD_VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST

/usr/bin/plutil -lint "$CONTENTS/Info.plist"
codesign --force --deep --sign - "$BUNDLE"
codesign --verify --deep --strict --verbose=2 "$BUNDLE"
echo "Built local-only preview: $BUNDLE"
echo "Bundle id: dev.metalsharp.gpui-preview (does not replace the production app)"
