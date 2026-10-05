#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/app-gpui"
APP_BUNDLE_NAME="${METALSHARP_GPUI_APP_BUNDLE_NAME:-MetalSharp-GPUI-Preview.app}"
APP_DISPLAY_NAME="${METALSHARP_GPUI_APP_DISPLAY_NAME:-MetalSharp GPUI Preview}"
APP_BUNDLE_ID="${METALSHARP_GPUI_APP_BUNDLE_ID:-dev.metalsharp.gpui-preview}"
APP_VERSION="${METALSHARP_GPUI_APP_VERSION:-0.1.0-preview}"
APP_BUILD_VERSION="${METALSHARP_GPUI_APP_BUILD_VERSION:-${APP_VERSION%%-*}}"
APP_EXECUTABLE="${METALSHARP_GPUI_APP_EXECUTABLE:-MetalSharp-GPUI}"
CARGO_PROFILE="${METALSHARP_GPUI_CARGO_PROFILE:-debug}"
BUNDLE="$APP_DIR/target/$APP_BUNDLE_NAME"
CONTENTS="$BUNDLE/Contents"
CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$APP_DIR/target}"
if [[ ! "$APP_VERSION" =~ ^[0-9]+([.][0-9]+){1,2}(-[A-Za-z0-9.-]+)?$ ]]; then
  echo "Invalid METALSHARP_GPUI_APP_VERSION: $APP_VERSION" >&2
  exit 1
fi
if [[ ! "$APP_BUILD_VERSION" =~ ^[0-9]+([.][0-9]+){1,2}$ ]]; then
  echo "Invalid METALSHARP_GPUI_APP_BUILD_VERSION: $APP_BUILD_VERSION" >&2
  exit 1
fi
DISPLAY_NAME_PATTERN='^[A-Za-z0-9][A-Za-z0-9 .-]*$'
BUNDLE_ID_PATTERN='^[A-Za-z0-9.-]+$'
if [[ ! "$APP_DISPLAY_NAME" =~ $DISPLAY_NAME_PATTERN ]] || [[ ! "$APP_BUNDLE_ID" =~ $BUNDLE_ID_PATTERN ]] || [[ ! "$APP_EXECUTABLE" =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]]; then
  echo "Invalid local app display name or bundle identifier" >&2
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

CARGO_ARGS=(--locked --manifest-path "$APP_DIR/Cargo.toml")
if [ -n "${METALSHARP_GPUI_CARGO_FEATURES:-}" ]; then
  CARGO_ARGS+=(--features "$METALSHARP_GPUI_CARGO_FEATURES")
fi
case "$CARGO_PROFILE" in
  debug) ;;
  release) CARGO_ARGS+=(--release) ;;
  *) echo "METALSHARP_GPUI_CARGO_PROFILE must be debug or release" >&2; exit 1 ;;
esac
# No MACOSX_DEPLOYMENT_TARGET here: with the macOS 27 linker it also applies to
# host proc-macro dylibs and produces ones dyld rejects ("mis-aligned LINKEDIT
# string pool" -> E0463). Info.plist's LSMinimumSystemVersion sets the floor.
CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-3}" cargo build "${CARGO_ARGS[@]}"

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

# "MetalSharp Steam.app": the backend starts the Wine Steam client through this
# LaunchServices helper so its first window gets the MetalSharp Steam icon
# instead of being attributed to MetalSharp (see steam_helper_app_path).
STEAM_HELPER="$CONTENTS/Resources/MetalSharp Steam.app/Contents"
STEAM_ICON_OUTPUT="$ICON_TMP/steam-compiled"
mkdir -p "$STEAM_HELPER/MacOS" "$STEAM_HELPER/Resources" "$STEAM_ICON_OUTPUT"
"$ACTOOL_BIN" \
  --compile "$STEAM_ICON_OUTPUT" \
  --platform macosx \
  --minimum-deployment-target "${MACOSX_DEPLOYMENT_TARGET:-13.0}" \
  --app-icon metalsharp-steam \
  --output-partial-info-plist "$STEAM_ICON_OUTPUT/partial-Info.plist" \
  "$APP_DIR/assets/metalsharp-steam.icon"
test -s "$STEAM_ICON_OUTPUT/metalsharp-steam.icns" || { echo "actool did not produce the Steam helper icon" >&2; exit 1; }
install -m 0644 "$STEAM_ICON_OUTPUT/Assets.car" "$STEAM_HELPER/Resources/Assets.car"
install -m 0644 "$STEAM_ICON_OUTPUT/metalsharp-steam.icns" "$STEAM_HELPER/Resources/metalsharp-steam.icns"
cat > "$STEAM_HELPER/MacOS/metalsharp-steam" <<'LAUNCHER'
#!/bin/sh
# Started by LaunchServices with the Wine argv; exec keeps this app's identity.
if [ -n "$METALSHARP_LAUNCH_CWD" ]; then cd "$METALSHARP_LAUNCH_CWD" || exit 1; fi
exec "$@"
LAUNCHER
chmod 0755 "$STEAM_HELPER/MacOS/metalsharp-steam"
cat > "$STEAM_HELPER/Info.plist" <<STEAMPLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>metalsharp-steam</string>
  <key>CFBundleIdentifier</key><string>com.metalsharp.steam</string>
  <key>CFBundleName</key><string>MetalSharp Steam</string>
  <key>CFBundleDisplayName</key><string>MetalSharp Steam</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>metalsharp-steam</string>
  <key>CFBundleIconName</key><string>metalsharp-steam</string>
  <key>CFBundleShortVersionString</key><string>$APP_VERSION</string>
  <key>CFBundleVersion</key><string>$APP_BUILD_VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
</dict>
</plist>
STEAMPLIST
/usr/bin/plutil -lint "$STEAM_HELPER/Info.plist" >/dev/null
install -m 0755 "$CARGO_TARGET_DIR/$CARGO_PROFILE/metalsharp-gpui" "$CONTENTS/MacOS/$APP_EXECUTABLE"
cp -R "$APP_DIR"/assets/. "$CONTENTS/Resources/assets/"

# Explicitly package the authoritative C backend/resources for connected testing.
# This builds/copies only; it never launches the backend, installers or accounts.
if [ "${METALSHARP_GPUI_PACKAGE_BACKEND:-0}" = "1" ]; then
  make -C "$ROOT/app/src-c"
  "$ROOT/tools/package/create-host-runtime.sh"
  mkdir -p "$CONTENTS/Resources/runtime" "$CONTENTS/Resources/tools" "$CONTENTS/Resources/scripts/tools/native" "$CONTENTS/Resources/bundles" "$CONTENTS/Resources/licenses" "$CONTENTS/Resources/runtime/shim-sources/fna/shims"
  install -m 0755 "$ROOT/app/src-c/build/metalsharp-backend" "$CONTENTS/Resources/runtime/metalsharp-backend"
  cp -R "$ROOT/app/native/host" "$CONTENTS/Resources/runtime/host"
  for tool in zstd unzstd wrestool icotool unar lsar; do
    [ -f "$ROOT/app/tools/$tool" ] || { echo "Missing required packaged tool: $tool" >&2; exit 1; }
    install -m 0755 "$ROOT/app/tools/$tool" "$CONTENTS/Resources/tools/$tool"
  done
  for directory in lib licenses; do
    [ ! -d "$ROOT/app/tools/$directory" ] || cp -R "$ROOT/app/tools/$directory" "$CONTENTS/Resources/tools/"
  done
  cp -R "$ROOT/LICENSES/." "$CONTENTS/Resources/licenses/"
  install -m 0644 "$ROOT/THIRD_PARTY_LICENSES" "$CONTENTS/Resources/THIRD_PARTY_LICENSES"
  cp -R "$ROOT/src/fna/shims/." "$CONTENTS/Resources/runtime/shim-sources/fna/shims/"
  for entry in "$ROOT"/app/native/*; do
    [ -e "$entry" ] || continue
    [ "$(basename "$entry")" = host ] && continue
    cp -R "$entry" "$CONTENTS/Resources/scripts/tools/native/"
  done
  [ ! -d "$ROOT/app/tools/steam-art-manager" ] || cp -R "$ROOT/app/tools/steam-art-manager" "$CONTENTS/Resources/tools/"
  install -m 0755 "$ROOT/tools/install-homebrew.sh" "$CONTENTS/Resources/scripts/tools/install-homebrew.sh"
  cp -R "$ROOT/app/updater" "$CONTENTS/Resources/scripts/tools/"
  cp -R "$ROOT/configs" "$CONTENTS/Resources/"
  # Do not rebuild archives or copy the developer SDK.
  BUNDLE_SOURCE="${METALSHARP_GPUI_BUNDLE_SOURCE:-$ROOT/app/bundles}"
  for archive in metalsharp-runtime metalsharp-assets metalsharp-graphics-dll metalsharp-scripts-tools metalsharp-steam fnalibs; do
    source="$BUNDLE_SOURCE/$archive.tar.zst"
    if [ ! -s "$source" ]; then
      if [ "${METALSHARP_GPUI_REQUIRE_BUNDLES:-0}" = "1" ]; then
        echo "Missing required runtime bundle: $source" >&2
        exit 1
      fi
      continue
    fi
    cp "$source" "$CONTENTS/Resources/bundles/"
  done
  # Bundle builds silently drop the Vulkan lanes when the build machine has no
  # ~/.metalsharp/vkd3d; never ship a graphics bundle that cannot serve VKD3D.
  graphics="$CONTENTS/Resources/bundles/metalsharp-graphics-dll.tar.zst"
  if [ "${METALSHARP_GPUI_REQUIRE_BUNDLES:-0}" = "1" ] && [ -s "$graphics" ]; then
    members="$("$ROOT/app/tools/zstd" -dc "$graphics" | tar -t)"
    for lane in vkd3d-proton/x86_64-windows/d3d12.dll vkd3d-proton/x86_64-windows/d3d12core.dll \
      dxvk/x86_64-windows/d3d11.dll dxmt/x86_64-windows/d3d11.dll; do
      if ! grep -qx "Graphics/dll/$lane" <<<"$members"; then
        echo "Graphics bundle is missing Graphics/dll/$lane: $graphics" >&2
        exit 1
      fi
    done
  fi
fi

cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$APP_EXECUTABLE</string>
  <key>CFBundleIdentifier</key><string>$APP_BUNDLE_ID</string>
  <key>CFBundleName</key><string>$APP_DISPLAY_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_DISPLAY_NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>metalsharp-liquid-glass</string>
  <key>CFBundleIconName</key><string>metalsharp-liquid-glass</string>
  <key>CFBundleShortVersionString</key><string>$APP_VERSION</string>
  <key>CFBundleVersion</key><string>$APP_BUILD_VERSION</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>CFBundleURLTypes</key>
  <array>
    <dict>
      <key>CFBundleURLName</key><string>MetalSharp Game</string>
      <key>CFBundleURLSchemes</key><array><string>metalsharp</string></array>
    </dict>
  </array>
</dict>
</plist>
PLIST

/usr/bin/plutil -lint "$CONTENTS/Info.plist"

# Ad-hoc by default. METALSHARP_GPUI_SIGN_IDENTITY (a Developer ID Application
# identity) signs inside-out with the hardened runtime, a secure timestamp and
# the release entitlements, as notarization requires; --deep is not used there.
SIGN_IDENTITY="${METALSHARP_GPUI_SIGN_IDENTITY:--}"
if [ "$SIGN_IDENTITY" = "-" ]; then
  codesign --force --deep --sign - "$BUNDLE"
else
  ENTITLEMENTS="${METALSHARP_GPUI_ENTITLEMENTS:-$ROOT/tools/dmg/entitlements.mac.plist}"
  [ -s "$ENTITLEMENTS" ] || { echo "Missing signing entitlements: $ENTITLEMENTS" >&2; exit 1; }
  sign_code() {
    codesign --force --timestamp --options runtime --entitlements "$ENTITLEMENTS" \
      --sign "$SIGN_IDENTITY" "$1"
  }
  # Loose Mach-O code in Resources (backend, host runtime, native helpers,
  # tools), deepest paths first; nested apps are signed as bundles below.
  while IFS= read -r file; do
    case "${file#"$CONTENTS/Resources/"}" in *.app/*) continue ;; esac
    if file -b "$file" | grep -q 'Mach-O'; then
      sign_code "$file"
    fi
  done < <(find "$CONTENTS/Resources" -type f | awk '{ print length($0) "\t" $0 }' | sort -rn | cut -f2-)
  while IFS= read -r nested; do
    sign_code "$nested"
  done < <(find "$CONTENTS/Resources" -type d -name '*.app' | awk '{ print length($0) "\t" $0 }' | sort -rn | cut -f2-)
  sign_code "$BUNDLE"
fi
codesign --verify --deep --strict --verbose=2 "$BUNDLE"
echo "Built local-only GPUI app: $BUNDLE"
echo "Bundle id: $APP_BUNDLE_ID"
