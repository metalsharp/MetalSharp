#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <dmg>" >&2
  exit 2
fi

dmg="$1"
if [ ! -s "$dmg" ]; then
  echo "DMG is missing or empty: $dmg" >&2
  exit 1
fi

for tool in codesign python3 xcrun; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Required signing tool not found: $tool" >&2
    exit 2
  fi
done

if [ -z "${APPLE_SIGNING_IDENTITY:-}" ]; then
  echo "Missing Developer ID Application signing identity." >&2
  exit 1
fi

password_credentials=0
api_key_credentials=0
if [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ]; then
  password_credentials=1
fi
if [ -n "${APPLE_API_KEY:-}" ] && [ -s "$APPLE_API_KEY" ] && [ -n "${APPLE_API_KEY_ID:-}" ] && [ -n "${APPLE_API_ISSUER:-}" ]; then
  api_key_credentials=1
fi
if [ "$password_credentials" -ne 1 ] && [ "$api_key_credentials" -ne 1 ]; then
  echo "Missing complete Apple notarization credentials." >&2
  exit 1
fi

codesign --force --sign "$APPLE_SIGNING_IDENTITY" --timestamp -i com.metalsharp.app.dmg "$dmg"
codesign --verify --verbose=4 "$dmg"

notary_args=(notarytool submit "$dmg" --wait --timeout 2h --output-format json)
notary_auth_args=()
if [ "$api_key_credentials" -eq 1 ]; then
  notary_auth_args=(--key "$APPLE_API_KEY" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER")
else
  notary_auth_args=(--apple-id "$APPLE_ID" --password "$APPLE_APP_SPECIFIC_PASSWORD" --team-id "$APPLE_TEAM_ID")
fi
notary_args+=("${notary_auth_args[@]}")
set +e
notary_output="$(xcrun "${notary_args[@]}" 2>&1)"
notary_exit=$?
set -e
printf '%s\n' "$notary_output"
notary_status="$(printf '%s\n' "$notary_output" | python3 -c 'import json, sys; print(json.load(sys.stdin).get("status", ""))' 2>/dev/null || true)"
if [ "$notary_exit" -ne 0 ] || [ "$notary_status" != "Accepted" ]; then
  echo "Apple notarization did not accept the DMG (exit: $notary_exit, status: ${notary_status:-missing})." >&2
  submission_id="$(printf '%s\n' "$notary_output" | python3 -c 'import json, sys
try:
    result = json.load(sys.stdin)
except Exception:
    print("")
else:
    print(result.get("id", ""))' 2>/dev/null || true)"
  if [ -n "$submission_id" ]; then
    echo "Fetching Apple notarization log for submission $submission_id." >&2
    xcrun notarytool log "${notary_auth_args[@]}" "$submission_id" || true
  fi
  exit 1
fi

xcrun stapler staple "$dmg"
xcrun stapler validate "$dmg"
echo "Developer ID signed and notarized DMG: $dmg"
