#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/app-gpui"
BUNDLE="$APP_DIR/target/MetalSharp-GPUI-Preview.app"
CONTENTS="$BUNDLE/Contents"

command -v cargo >/dev/null || { echo "cargo is required" >&2; exit 1; }
command -v codesign >/dev/null || { echo "codesign is required" >&2; exit 1; }
command -v swift >/dev/null || { echo "swift is required to generate dock artwork" >&2; exit 1; }

# Derived tilt variants are generated locally, not committed to the repository.
(cd "$APP_DIR" && swift generate-dock-art.swift)

CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-3}" MACOSX_DEPLOYMENT_TARGET=13.0 cargo build --locked --manifest-path "$APP_DIR/Cargo.toml"

rm -rf "$BUNDLE"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources/assets"
ICON_TMP="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-icon.XXXXXX")"
ICONSET="$ICON_TMP/MetalSharp.iconset"
trap 'rm -rf "$ICON_TMP"' EXIT
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -s format png -z "$size" "$size" "$APP_DIR/assets/metalsharp-logo.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  if [ "$size" -lt 512 ]; then
    double=$((size * 2))
    sips -s format png -z "$double" "$double" "$APP_DIR/assets/metalsharp-logo.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
  fi
done
iconutil -c icns "$ICONSET" -o "$CONTENTS/Resources/MetalSharp.icns"
install -m 0755 "$APP_DIR/target/debug/metalsharp-gpui" "$CONTENTS/MacOS/MetalSharp-GPUI"
cp -R "$APP_DIR"/assets/. "$CONTENTS/Resources/assets/"

cat > "$CONTENTS/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>MetalSharp-GPUI</string>
  <key>CFBundleIdentifier</key><string>dev.metalsharp.gpui-preview</string>
  <key>CFBundleName</key><string>MetalSharp GPUI Preview</string>
  <key>CFBundleDisplayName</key><string>MetalSharp GPUI Preview</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>MetalSharp</string>
  <key>CFBundleShortVersionString</key><string>0.1.0-preview</string>
  <key>CFBundleVersion</key><string>0.1.0</string>
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
