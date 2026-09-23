#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
  echo "usage: $0 <app-or-dmg> [<app-or-dmg> ...]" >&2
  exit 2
fi

require_tool() {
  local tool="$1"
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required notarization verifier tool not found: $tool" >&2
    exit 2
  fi
}

verify_developer_id_app() {
  local app="$1"
  local signature_info

  if [ ! -d "$app" ]; then
    echo "Notarization app target is not an app bundle directory: $app" >&2
    exit 1
  fi
  if [ ! -s "$app/Contents/Info.plist" ]; then
    echo "App bundle is missing Contents/Info.plist: $app" >&2
    exit 1
  fi
  if [ ! -d "$app/Contents/MacOS" ]; then
    echo "App bundle is missing Contents/MacOS: $app" >&2
    exit 1
  fi

  codesign --verify --deep --strict --verbose=4 "$app"
  signature_info="$(mktemp)"
  codesign --display --verbose=4 "$app" >"$signature_info" 2>&1
  if ! grep -q "Authority=Developer ID Application" "$signature_info"; then
    echo "App is not signed with a Developer ID Application identity: $app" >&2
    cat "$signature_info" >&2
    rm -f "$signature_info"
    exit 1
  fi
  rm -f "$signature_info"

  spctl -a -vvv --type execute "$app"
}

verify_developer_id_dmg() {
  local dmg="$1"
  local signature_info

  if [ ! -f "$dmg" ]; then
    echo "Notarization DMG target is not a file: $dmg" >&2
    exit 1
  fi
  if [ ! -s "$dmg" ]; then
    echo "Notarization DMG target is empty: $dmg" >&2
    exit 1
  fi

  hdiutil verify "$dmg"
  codesign --verify --verbose=4 "$dmg"
  signature_info="$(codesign --display --verbose=4 "$dmg" 2>&1)"
  if ! printf '%s\n' "$signature_info" | grep -q "Authority=Developer ID Application"; then
    echo "DMG is not signed with a Developer ID Application identity: $dmg" >&2
    printf '%s\n' "$signature_info" >&2
    exit 1
  fi
  xcrun stapler validate "$dmg"
  spctl -a -vvv --type open --context context:primary-signature "$dmg"
}

require_tool codesign
require_tool hdiutil
require_tool spctl
require_tool xcrun

for target in "$@"; do
  if [ ! -e "$target" ]; then
    echo "Notarization target not found: $target" >&2
    exit 1
  fi

  case "$target" in
    *.app)
      verify_developer_id_app "$target"
      ;;
    *.dmg)
      verify_developer_id_dmg "$target"
      ;;
    *)
      echo "Unsupported notarization verification target: $target" >&2
      exit 2
      ;;
  esac
done

echo "Apple Developer ID notarization verified for $# target(s)."
