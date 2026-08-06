#!/usr/bin/env bash
# Required Notice: Copyright (c) 2026 MetalSharp. Commercial licensing: averyfelts@aol.com
#
# Regenerate tools/bundles/fna-unity-hashes.tsv from the SOURCE_BUNDLES
# staging dir used to build metalsharp-assets.tar.zst. Run AFTER staging the
# unity-mono / xna / sdl3 payloads and BEFORE publishing the bundle, so the
# published .tsv matches the published archive (CI verify-bundles.sh enforces
# the hashes).
#
# Usage:
#   tools/bundles/update-fna-unity-hashes.sh <assets-staging-root>
# where <assets-staging-root> is the dir that becomes the assets/ tree inside
# metalsharp-assets.tar.zst (i.e. contains unity-mono/, xna/, sdl3/).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT_DIR/tools/bundles/fna-unity-hashes.tsv"
STAGING="${1:?usage: update-fna-unity-hashes.sh <assets-staging-root>}"

for sub in unity-mono xna sdl3 prebuilt-launchers; do
  if [ ! -d "$STAGING/$sub" ]; then
    echo "error: $STAGING/$sub not found; stage the payloads first" >&2
    exit 1
  fi
done

tmp="$(mktemp)"
{
  echo "# Regenerated $(date -u +%Y-%m-%dT%H:%M:%SZ) by ${0##*/} — do not edit by hand."
  echo "# Paths are relative to the assets bundle root (assets/<sub>/...)."
  echo "path	sha256"
  find "$STAGING/unity-mono" "$STAGING/xna" "$STAGING/sdl3" "$STAGING/prebuilt-launchers" -type f | sort | while read -r f; do
    rel="${f#"$STAGING"/}"
    printf '%s\t%s\n' "$rel" "$(shasum -a 256 "$f" | awk '{print $1}')"
  done
} > "$tmp"
mv "$tmp" "$OUT"
echo "wrote $(wc -l < "$OUT") lines to $OUT"
