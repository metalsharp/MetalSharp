#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: $0 <release-version> <output-dmg>" >&2
  exit 2
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="$1"
OUTPUT_DMG="$2"
case "$VERSION" in
  ''|*[!0-9.]*|.*|*.) echo "Invalid release version: $VERSION" >&2; exit 2 ;;
esac
SIGNING_IDENTITY="${APPLE_SIGNING_IDENTITY:-}"
if [ -z "$SIGNING_IDENTITY" ] && [ "${ALLOW_ADHOC_SIGNING:-0}" = "1" ]; then
  SIGNING_IDENTITY="-"
fi
if [ -z "$SIGNING_IDENTITY" ]; then
  echo "APPLE_SIGNING_IDENTITY is required to build the signed recovery assistant." >&2
  exit 1
fi
for tool in osacompile codesign hdiutil ditto; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Required tool not found: $tool" >&2; exit 2; }
done

mkdir -p "$(dirname "$OUTPUT_DMG")"
OUTPUT_DIR="$(cd "$(dirname "$OUTPUT_DMG")" && pwd)"
OUTPUT_DMG="$OUTPUT_DIR/$(basename "$OUTPUT_DMG")"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-recovery-build.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
APP_PATH="$WORK_DIR/MetalSharp Update Assistant.app"
STAGE_DIR="$WORK_DIR/dmg-root"
mkdir -p "$STAGE_DIR"

osacompile -o "$APP_PATH" "$ROOT_DIR/app/updater/RecoveryAssistant.applescript"
RESOURCES="$APP_PATH/Contents/Resources"
cp "$ROOT_DIR/app/updater/recover-update.sh" "$RESOURCES/recover-update.sh"
cp "$ROOT_DIR/app/updater/update.sh" "$RESOURCES/update.sh"
sed "s/@RECOVERY_VERSION@/$VERSION/g" \
  "$ROOT_DIR/app/updater/recovery-config.plist.in" > "$RESOURCES/recovery-config.plist"
chmod 755 "$RESOURCES/recover-update.sh" "$RESOURCES/update.sh"

plist_set() {
  local key="$1" value="$2"
  if /usr/libexec/PlistBuddy -c "Print :$key" "$APP_PATH/Contents/Info.plist" >/dev/null 2>&1; then
    /usr/libexec/PlistBuddy -c "Set :$key $value" "$APP_PATH/Contents/Info.plist"
  else
    /usr/libexec/PlistBuddy -c "Add :$key string $value" "$APP_PATH/Contents/Info.plist"
  fi
}
plist_set CFBundleIdentifier com.metalsharp.updateassistant
plist_set CFBundleName "MetalSharp Update Assistant"
plist_set CFBundleShortVersionString "$VERSION"
plist_set CFBundleVersion "$VERSION"

codesign_args=(--force --deep --sign "$SIGNING_IDENTITY")
if [ "$SIGNING_IDENTITY" != "-" ]; then
  codesign_args+=(--options runtime --timestamp)
fi
codesign "${codesign_args[@]}" "$APP_PATH"
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

ditto "$APP_PATH" "$STAGE_DIR/MetalSharp Update Assistant.app"
cat > "$STAGE_DIR/README.txt" <<EOF
MetalSharp Update Assistant ($VERSION)

Use this recovery assistant if MetalSharp's in-app updater is stuck.
Open the app and follow the prompts. It downloads the official MetalSharp
$VERSION update, verifies the disk image and Developer ID signature, then
installs it using the bounded legacy updater included in this assistant.

The installer closes MetalSharp and Steam/Wine processes. Save game work
before continuing. Your MetalSharp user data is preserved; the normal
post-update migration runs after the app relaunches.
EOF

rm -f "$OUTPUT_DMG"
hdiutil create -volname "MetalSharp Update Assistant" \
  -srcfolder "$STAGE_DIR" -ov -format UDZO "$OUTPUT_DMG"
if [ "$SIGNING_IDENTITY" = "-" ]; then
  echo "Built ad-hoc recovery assistant test DMG: $OUTPUT_DMG"
else
  echo "Built Developer ID recovery assistant DMG: $OUTPUT_DMG"
fi
