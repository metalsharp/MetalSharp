#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
OUT_DIR="$SDK_DIR/out/agility"
AGILITY_VERSION="${AGILITY_VERSION:-1.619.5}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      AGILITY_VERSION="$2"
      shift 2
      ;;
    -h|--help)
      cat <<'USAGE'
Usage:
  fetch-agility.sh [--version 1.619.5]

Downloads the official Microsoft.Direct3D.D3D12 NuGet package for the requested
Agility SDK version and extracts it under tools/d3d12-metal-sdk/out/agility/.
Prints the x64 payload directory on success.
USAGE
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

PACKAGE_DIR="$OUT_DIR/$AGILITY_VERSION"
BIN_DIR="$PACKAGE_DIR/build/native/bin/x64"

if [[ ! -f "$BIN_DIR/D3D12Core.dll" || ! -f "$BIN_DIR/d3d12SDKLayers.dll" ]]; then
  mkdir -p "$OUT_DIR"
  TMP_DIR="$(mktemp -d)"
  trap 'rm -rf "$TMP_DIR"' EXIT
  PACKAGE_FILE="$TMP_DIR/agility.nupkg"
  PACKAGE_VERSION_LOWER="$(printf '%s' "$AGILITY_VERSION" | tr '[:upper:]' '[:lower:]')"
  curl -L --fail -o "$PACKAGE_FILE" \
    "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$PACKAGE_VERSION_LOWER/microsoft.direct3d.d3d12.$PACKAGE_VERSION_LOWER.nupkg"
  rm -rf "$PACKAGE_DIR"
  mkdir -p "$PACKAGE_DIR"
  unzip -q "$PACKAGE_FILE" -d "$PACKAGE_DIR"
fi

if [[ ! -f "$BIN_DIR/D3D12Core.dll" || ! -f "$BIN_DIR/d3d12SDKLayers.dll" ]]; then
  echo "Agility package did not contain the expected x64 payload: $BIN_DIR" >&2
  exit 2
fi

echo "$BIN_DIR"
