#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUNDLE_DIR="${1:-$PROJECT_ROOT/app/bundles}"
STAGE_DIR="${2:-$PROJECT_ROOT/dist/staged-bundles}"
MANIFEST="$PROJECT_ROOT/tools/bundles/asset-manifest.tsv"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

while IFS=$'\t' read -r asset root platforms _notes; do
  case "$asset" in
    ""|\#*) continue ;;
  esac

  case ",$platforms," in
    *,mac,*) ;;
    *) continue ;;
  esac

  archive="$BUNDLE_DIR/$asset"
  if [ ! -s "$archive" ]; then
    echo "Missing bundle archive: $archive" >&2
    exit 1
  fi

  tar --use-compress-program=unzstd -xf "$archive" -C "$STAGE_DIR"
  if [ ! -e "$STAGE_DIR/$root" ]; then
    echo "Bundle $asset did not stage expected root $root/" >&2
    exit 1
  fi
  echo "STAGED: $asset -> $root/"
done < "$MANIFEST"
