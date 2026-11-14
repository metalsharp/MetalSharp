#!/bin/bash
# End-to-end test of MetalSharp's Sparkle path, on this Mac: a host app (host.m) linking MSSparkleBridge.m updates
# itself from 1.0.1 to 1.0.2 through a local feed made by sparkle_release.py publish, and must get there with the
# .delta, not the full archive. Needs a Developer ID Application identity (SPARKLE_E2E_IDENTITY, else the first one
# found) and the EdDSA key in the login keychain (account metalsharp-releases). Work goes to $1 (a scratch folder).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
WORK="${1:?usage: run-e2e.sh <scratch folder>}"
PORT="${SPARKLE_E2E_PORT:-8586}"
SPARKLE="$("$ROOT/tools/sparkle/fetch-sparkle.sh")"
IDENTITY="${SPARKLE_E2E_IDENTITY:-$(security find-identity -v -p codesigning | awk '/Developer ID Application/ {print $2; exit}')}"
[ -n "$IDENTITY" ] || { echo "no Developer ID Application identity" >&2; exit 1; }
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
LOG="$WORK/host.log"
FEED="$WORK/feed"
mkdir -p "$FEED"
: > "$LOG"

build_host() {  # build_host <version> <dir>
  local version=$1 app="$2/SparkleHost.app"
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks"
  clang -fobjc-arc -mmacosx-version-min=13.0 -F"$SPARKLE" -framework Sparkle -framework AppKit \
    -Wl,-rpath,@executable_path/../Frameworks -o "$app/Contents/MacOS/SparkleHost" \
    "$ROOT/tools/sparkle/e2e/host.m" "$ROOT/app-gpui/sparkle/MSSparkleBridge.m"
  ditto "$SPARKLE/Sparkle.framework" "$app/Contents/Frameworks/Sparkle.framework"
  cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>SparkleHost</string>
  <key>CFBundleIdentifier</key><string>com.metalsharp.sparkle-e2e</string>
  <key>CFBundleName</key><string>SparkleHost</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>SUFeedURL</key><string>http://127.0.0.1:$PORT/appcast.xml</string>
  <key>SUPublicEDKey</key><string>$(tr -d '[:space:]' < "$ROOT/tools/sparkle/public-key.txt")</string>
  <key>SUEnableAutomaticChecks</key><false/>
  <key>SUVerifyUpdateBeforeExtraction</key><true/>
  <key>MSE2ELog</key><string>$LOG</string>
  <key>MSE2ETarget</key><string>1.0.2</string>
</dict></plist>
PLIST
  local b="$app/Contents/Frameworks/Sparkle.framework/Versions/B" sign=(codesign --force --timestamp --options runtime --sign "$IDENTITY")
  "${sign[@]}" "$b/XPCServices/Installer.xpc"
  "${sign[@]}" --preserve-metadata=entitlements "$b/XPCServices/Downloader.xpc"
  "${sign[@]}" "$b/Autoupdate"
  "${sign[@]}" "$b/Updater.app"
  "${sign[@]}" "$app/Contents/Frameworks/Sparkle.framework"
  "${sign[@]}" "$app"
  codesign --verify --deep --strict "$app"
}

rm -rf "${WORK:?}/1.0.1" "${WORK:?}/1.0.2" "${WORK:?}/old" "${WORK:?}/installed"
build_host 1.0.1 "$WORK/1.0.1"
build_host 1.0.2 "$WORK/1.0.2"
mkdir -p "$WORK/old"
ditto -c -k --sequesterRsrc --keepParent "$WORK/1.0.1/SparkleHost.app" "$WORK/old/MetalSharp-1.0.1-arm64.zip"

KEY_DIR="$(mktemp -d -t sparkle-e2e)"
KEY="$KEY_DIR/ed.key"  # generate_keys -x will not write over an existing file
trap 'rm -rf "${KEY_DIR:?}"; [ -n "${SERVER:-}" ] && kill "$SERVER" 2>/dev/null || true' EXIT
"$SPARKLE/bin/generate_keys" --account metalsharp-releases -x "$KEY" >/dev/null
SPARKLE_DIR="$SPARKLE" python3 "$ROOT/tools/sparkle/sparkle_release.py" publish --app "$WORK/1.0.2/SparkleHost.app" \
  --variant regular --version 1.0.2 --out "$FEED" --from-version 1.0.1 --old-archive-dir "$WORK/old" \
  --ed-key-file "$KEY" --base-url "http://127.0.0.1:$PORT"
rm -f "$KEY"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$FEED" > "$WORK/server.log" 2>&1 &
SERVER=$!
sleep 1

mkdir -p "$WORK/installed"
ditto "$WORK/1.0.1/SparkleHost.app" "$WORK/installed/SparkleHost.app"
open "$WORK/installed/SparkleHost.app"
for _ in $(seq 1 240); do
  grep -q -E '^.* (done: running the target version|failed)$' "$LOG" && break
  sleep 0.5
done
cat "$LOG"
echo "--- feed requests"
grep -o '"GET [^"]*"' "$WORK/server.log" || true
grep -q 'done: running the target version' "$LOG" || { echo "FAIL: did not update" >&2; exit 1; }
[ "$(defaults read "$WORK/installed/SparkleHost.app/Contents/Info" CFBundleVersion)" = 1.0.2 ] || { echo "FAIL: installed app is not 1.0.2" >&2; exit 1; }
codesign --verify --deep --strict "$WORK/installed/SparkleHost.app"
grep -q 'GET /MetalSharp-1.0.2-arm64-from1.0.1.delta' "$WORK/server.log" || { echo "FAIL: the update did not use the delta" >&2; exit 1; }
if grep -q 'GET /MetalSharp-1.0.2-arm64.zip' "$WORK/server.log"; then
  echo "FAIL: the full archive was downloaded" >&2
  exit 1
fi
echo "PASS: 1.0.1 updated itself to 1.0.2 through the delta, and the installed app verifies"
