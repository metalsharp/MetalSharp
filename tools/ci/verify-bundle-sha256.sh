#!/usr/bin/env bash
# Verify a downloaded bundle archive against the pinned SHA-256 manifest used
# by the VKD3D gate (tools/ci/vkd3d-bundle-hashes.tsv). Exits non-zero when the
# archive is missing, has no pinned digest, or does not match it.
set -euo pipefail

if [ "$#" -ne 3 ]; then
  echo "Usage: $0 MANIFEST ASSET FILE" >&2
  exit 2
fi

MANIFEST="$1"
ASSET="$2"
FILE="$3"

if [ ! -f "$MANIFEST" ]; then
  echo "ERROR: bundle hash manifest not found: $MANIFEST" >&2
  exit 1
fi

expected="$(awk -F '\t' -v asset="$ASSET" '$1 == asset { print $2; exit }' "$MANIFEST")"
if [ -z "$expected" ]; then
  echo "ERROR: no pinned SHA-256 for $ASSET in $MANIFEST" >&2
  exit 1
fi

if [ ! -s "$FILE" ]; then
  echo "ERROR: bundle $ASSET is missing or empty: $FILE" >&2
  exit 1
fi

if command -v shasum >/dev/null 2>&1; then
  SHA256_CMD=(shasum -a 256)
else
  SHA256_CMD=(sha256sum)
fi
actual="$("${SHA256_CMD[@]}" "$FILE" | awk '{print $1}')"
if [ "$actual" != "$expected" ]; then
  echo "ERROR: bundle checksum mismatch for $ASSET: expected $expected got $actual" >&2
  exit 1
fi

echo "OK: $ASSET SHA-256 matches pinned manifest"
