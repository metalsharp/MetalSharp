#!/usr/bin/env bash
set -euo pipefail

required_secret() {
  local name="$1"
  if [ -z "${!name:-}" ]; then
    echo "Missing required Apple signing secret: $name" >&2
    return 1
  fi
}

decode_base64() {
  local source="$1"
  local dest="$2"
  if ! printf '%s' "$source" | base64 --decode >"$dest" 2>/dev/null; then
    printf '%s' "$source" | base64 -D >"$dest"
  fi
}

required_secret MACOS_CERTIFICATE_P12
required_secret MACOS_CERTIFICATE_PASSWORD

password_notary_ready=0
api_notary_ready=0
if [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ]; then
  password_notary_ready=1
fi
if [ -n "${APPLE_API_KEY_P8_BASE64:-}" ] && [ -n "${APPLE_API_KEY_ID:-}" ] && [ -n "${APPLE_API_ISSUER:-}" ]; then
  api_notary_ready=1
fi
if [ "$password_notary_ready" -ne 1 ] && [ "$api_notary_ready" -ne 1 ]; then
  cat >&2 <<'EOF'
Missing Apple notarization credentials. Configure one of:
- APPLE_ID, APPLE_APP_SPECIFIC_PASSWORD, APPLE_TEAM_ID
- APPLE_API_KEY_P8_BASE64, APPLE_API_KEY_ID, APPLE_API_ISSUER
EOF
  exit 1
fi

if [ -z "${RUNNER_TEMP:-}" ]; then
  RUNNER_TEMP="$(mktemp -d)"
fi
if [ -z "${GITHUB_ENV:-}" ]; then
  GITHUB_ENV="$RUNNER_TEMP/github-env"
  touch "$GITHUB_ENV"
fi

KEYCHAIN_PASSWORD="$(uuidgen)"
KEYCHAIN_PATH="$RUNNER_TEMP/metalsharp-signing.keychain-db"
CERT_PATH="$RUNNER_TEMP/metalsharp-developer-id.p12"

decode_base64 "$MACOS_CERTIFICATE_P12" "$CERT_PATH"

security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
security import "$CERT_PATH" \
  -P "$MACOS_CERTIFICATE_PASSWORD" \
  -A \
  -T /usr/bin/codesign \
  -T /usr/bin/security \
  -k "$KEYCHAIN_PATH"

existing_keychains=()
while IFS= read -r keychain; do
  keychain="${keychain//\"/}"
  existing_keychains+=("$keychain")
done < <(security list-keychains -d user)
security list-keychains -d user -s "$KEYCHAIN_PATH" "${existing_keychains[@]}"
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
identity_listing="$(security find-identity -v -p codesigning "$KEYCHAIN_PATH")"
printf '%s\n' "$identity_listing"
APPLE_SIGNING_IDENTITY="$(printf '%s\n' "$identity_listing" | awk -F '"' '/Developer ID Application:/ {print $2; exit}')"
if [ -z "$APPLE_SIGNING_IDENTITY" ]; then
  echo "Imported keychain has no Developer ID Application identity." >&2
  exit 1
fi
echo "APPLE_SIGNING_IDENTITY=$APPLE_SIGNING_IDENTITY" >>"$GITHUB_ENV"

if [ "$api_notary_ready" -eq 1 ]; then
  API_KEY_PATH="$RUNNER_TEMP/AuthKey_${APPLE_API_KEY_ID}.p8"
  decode_base64 "$APPLE_API_KEY_P8_BASE64" "$API_KEY_PATH"
  echo "APPLE_API_KEY=$API_KEY_PATH" >>"$GITHUB_ENV"
fi

echo "CSC_KEYCHAIN=$KEYCHAIN_PATH" >>"$GITHUB_ENV"
echo "METALSHARP_REQUIRE_NOTARIZATION=1" >>"$GITHUB_ENV"
echo "Apple Developer ID signing environment prepared."
