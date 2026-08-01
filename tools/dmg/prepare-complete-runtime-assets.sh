#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEST_DIR="${METALSHARP_RELEASE_RUNTIME_DIR:-$PROJECT_ROOT/app/release-runtime}"
REPO="${METALSHARP_RUNTIME_REPO:-metalsharp/MetalSharp}"
TAG="${METALSHARP_RUNTIME_TAG:-v0.60.0-dependency-bundles}"
ARCHIVE="MetalSharp-Wine-Runtime-COMPLETE-all-arch-2026-07-31.tar.zst"
ARCHIVE_SHA256="e44a84bceeca62f01fd95a133364ec82467cd8883ff81bcc1bdfdf4a6c3ad146"
MANIFEST_SHA256="1832a96f003e1e3ff2f16974c796f1ba315e6aa7d8ca966eea3f811cd189a8b7"

ASSETS=(
  metalsharp-bundle-manifest.tsv
  install-metalsharp-wine-runtime.sh
  MetalSharp-Wine-Public-Source-2026-07-31.tar.zst
  MetalSharp-Wine-Public-Source-2026-07-31.tar.zst.sha256
  MetalSharp-GOG-Support-arm64-1.2.2.tar.zst
  "$ARCHIVE.part01"
  "$ARCHIVE.part02"
  "$ARCHIVE.part03"
  "$ARCHIVE.part04"
  PARTS-SHA256SUMS.txt
  REASSEMBLE.txt
)
PACKAGE_ASSETS=(
  metalsharp-bundle-manifest.tsv
  install-metalsharp-wine-runtime.sh
  MetalSharp-Wine-Public-Source-2026-07-31.tar.zst
  MetalSharp-Wine-Public-Source-2026-07-31.tar.zst.sha256
  MetalSharp-GOG-Support-arm64-1.2.2.tar.zst
  PARTS-SHA256SUMS.txt
  REASSEMBLE.txt
)

usage() {
  cat <<EOF
Usage: $0 [--verify-only|--verify-package] [DEST_DIR]

Downloads and verifies the complete v0.60.0 runtime release payload embedded
alongside the MetalSharp DMG. --verify-only performs no network access and
checks every release asset. --verify-package checks only the lean metadata and
source payload embedded in the DMG.
EOF
}

VERIFY_ONLY=0
if [ "${1:-}" = "--verify-only" ]; then
  VERIFY_ONLY=1
  shift
elif [ "${1:-}" = "--verify-package" ]; then
  VERIFY_ONLY=2
  shift
fi
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  usage
  exit 0
fi
if [ "$#" -gt 1 ]; then
  usage >&2
  exit 2
fi
if [ "$#" -eq 1 ]; then
  DEST_DIR="$1"
fi

mkdir -p "$DEST_DIR"

download_asset() {
  local name="$1"
  local destination="$DEST_DIR/$name"
  local expected=""
  if [ "$name" = "metalsharp-bundle-manifest.tsv" ]; then
    expected="$MANIFEST_SHA256"
  elif [ -s "$DEST_DIR/metalsharp-bundle-manifest.tsv" ]; then
    expected="$(awk -F '\t' -v asset="$name" '$1 == asset { print $3; exit }' "$DEST_DIR/metalsharp-bundle-manifest.tsv")"
  fi
  if [ -s "$destination" ]; then
    local actual
    actual="$(shasum -a 256 "$destination" | awk '{print $1}')"
    if [ -n "$expected" ] && [ "$actual" = "$expected" ]; then
      return 0
    fi
  fi
  local partial="$destination.partial"
  curl --fail --location --silent --show-error --retry 4 \
    --output "$partial" "https://github.com/$REPO/releases/download/$TAG/$name"
  mv "$partial" "$destination"
}

if [ "$VERIFY_ONLY" -eq 0 ]; then
  for asset in "${ASSETS[@]}"; do
    echo "Fetching complete-runtime release asset: $asset"
    download_asset "$asset"
  done
fi

VERIFY_ASSETS=("${ASSETS[@]}")
if [ "$VERIFY_ONLY" -eq 2 ]; then
  VERIFY_ASSETS=("${PACKAGE_ASSETS[@]}")
fi

for asset in "${VERIFY_ASSETS[@]}"; do
  if [ ! -s "$DEST_DIR/$asset" ]; then
    echo "Missing complete-runtime release asset: $DEST_DIR/$asset" >&2
    exit 1
  fi
done

MANIFEST="$DEST_DIR/metalsharp-bundle-manifest.tsv"
manifest_hash() {
  awk -F '\t' -v asset="$1" '$1 == asset { print $3; exit }' "$MANIFEST"
}

for asset in "${VERIFY_ASSETS[@]}"; do
  if [ "$asset" = "metalsharp-bundle-manifest.tsv" ]; then
    expected="$MANIFEST_SHA256"
  else
    expected="$(manifest_hash "$asset")"
  fi
  if [ -z "$expected" ]; then
    echo "Release manifest has no hash for $asset" >&2
    exit 1
  fi
  actual="$(shasum -a 256 "$DEST_DIR/$asset" | awk '{print $1}')"
  if [ "$actual" != "$expected" ]; then
    echo "Release asset hash mismatch: $asset expected=$expected actual=$actual" >&2
    exit 1
  fi
done

if [ "$VERIFY_ONLY" -ne 2 ]; then
  for part in "$ARCHIVE.part01" "$ARCHIVE.part02" "$ARCHIVE.part03" "$ARCHIVE.part04"; do
    expected="$(awk -v part="$part" '$2 == part { print $1; exit }' "$DEST_DIR/PARTS-SHA256SUMS.txt")"
    actual="$(shasum -a 256 "$DEST_DIR/$part" | awk '{print $1}')"
    if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
      echo "Part checksum contract failed: $part" >&2
      exit 1
    fi
  done

  reassembled_sha="$(cat \
    "$DEST_DIR/$ARCHIVE.part01" \
    "$DEST_DIR/$ARCHIVE.part02" \
    "$DEST_DIR/$ARCHIVE.part03" \
    "$DEST_DIR/$ARCHIVE.part04" | shasum -a 256 | awk '{print $1}')"
  if [ "$reassembled_sha" != "$ARCHIVE_SHA256" ]; then
    echo "Reassembled runtime hash mismatch: expected=$ARCHIVE_SHA256 actual=$reassembled_sha" >&2
    exit 1
  fi
fi

bash -n "$DEST_DIR/install-metalsharp-wine-runtime.sh"
echo "Complete runtime release assets verified: $DEST_DIR"
