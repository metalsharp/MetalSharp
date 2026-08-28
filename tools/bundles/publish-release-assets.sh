#!/usr/bin/env bash
set -euo pipefail

TAG="${1:-}"
shift || true

if [ -z "$TAG" ] || [ "$#" -eq 0 ]; then
  cat >&2 <<'USAGE'
Usage: tools/bundles/publish-release-assets.sh TAG FILE...

Uploads release assets one at a time and verifies the remote sha256/size after
each upload. If GitHub reports a transient upload error but the final asset is
present and matches, the script treats that asset as published.
USAGE
  exit 2
fi

REPO="${METALSHARP_BUNDLE_REPO:-${GITHUB_REPOSITORY:-metalsharp/MetalSharp}}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-release-assets.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

asset_info() {
  local name="$1"
  local json="$TMP_DIR/assets.json"

  gh release view "$TAG" --repo "$REPO" --json assets > "$json"
  python3 - "$name" "$json" <<'PY'
import json
import sys

name = sys.argv[1]
path = sys.argv[2]
with open(path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

for asset in data.get("assets", []):
    if asset.get("name") == name:
        print(f"{asset.get('digest', '')}\t{asset.get('size', '')}")
        break
PY
}

asset_matches() {
  local file="$1"
  local name
  name="$(basename "$file")"

  if [ ! -s "$file" ]; then
    echo "ERROR: missing or empty asset: $file" >&2
    return 1
  fi

  local local_sha local_size info remote_digest remote_size
  local_sha="$(shasum -a 256 "$file" | awk '{print $1}')"
  local_size="$(wc -c < "$file" | tr -d ' ')"
  info="$(asset_info "$name" || true)"

  if [ -z "$info" ]; then
    return 1
  fi

  remote_digest="${info%%$'\t'*}"
  remote_size="${info#*$'\t'}"
  remote_digest="${remote_digest#sha256:}"

  [ "$remote_digest" = "$local_sha" ] && [ "$remote_size" = "$local_size" ]
}

publish_one() {
  local file="$1"
  local name
  name="$(basename "$file")"

  if asset_matches "$file"; then
    echo "RELEASE ASSET OK: $name already matches"
    return 0
  fi

  local attempt status
  for attempt in 1 2 3; do
    echo "Uploading release asset: $name (attempt $attempt/3)"
    set +e
    gh release upload "$TAG" "$file" --repo "$REPO" --clobber
    status="$?"
    set -e

    if asset_matches "$file"; then
      if [ "$status" -ne 0 ]; then
        echo "RELEASE ASSET OK: $name matched after upload exit $status"
      else
        echo "RELEASE ASSET OK: $name uploaded"
      fi
      return 0
    fi

    echo "RELEASE ASSET RETRY: $name did not verify after attempt $attempt" >&2
    sleep "$((attempt * 10))"
  done

  echo "ERROR: release asset did not publish cleanly: $name" >&2
  return 1
}

for file in "$@"; do
  publish_one "$file"
done
