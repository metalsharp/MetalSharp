#!/usr/bin/env bash
set -euo pipefail

write_output() {
  local key="$1"
  local value="$2"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    printf '%s=%s\n' "$key" "$value" >>"$GITHUB_OUTPUT"
  else
    printf '%s=%s\n' "$key" "$value"
  fi
}

missing=()
if [ -z "${MACOS_CERTIFICATE_P12:-}" ]; then
  missing+=("MACOS_CERTIFICATE_P12")
fi
if [ -z "${MACOS_CERTIFICATE_PASSWORD:-}" ]; then
  missing+=("MACOS_CERTIFICATE_PASSWORD")
fi

password_notary_ready=0
api_notary_ready=0
if [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_APP_SPECIFIC_PASSWORD:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ]; then
  password_notary_ready=1
fi
if [ -n "${APPLE_API_KEY_P8_BASE64:-}" ] && [ -n "${APPLE_API_KEY_ID:-}" ] && [ -n "${APPLE_API_ISSUER:-}" ]; then
  api_notary_ready=1
fi
if [ "$password_notary_ready" -ne 1 ] && [ "$api_notary_ready" -ne 1 ]; then
  missing+=("APPLE_NOTARIZATION_CREDENTIAL_SET")
fi

if [ "${#missing[@]}" -eq 0 ]; then
  write_output ready true
  echo "Apple Developer ID signing and notarization credentials are configured."
  exit 0
fi

write_output ready false
{
  echo "Apple Developer ID signing will be skipped for this release."
  echo "Missing required release secret(s): ${missing[*]}"
  echo "The workflow will still package and upload an unsigned DMG artifact."
} >&2

if [ -n "${GITHUB_ENV:-}" ]; then
  echo "METALSHARP_UNSIGNED_DMG=1" >>"$GITHUB_ENV"
fi
