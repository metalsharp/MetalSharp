#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CXX="${CXX:-clang++}"
TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/metalsharp-provider-probe.XXXXXX")"
trap 'rm -rf "$TEMP_DIR"' EXIT

"$CXX" \
  -std=c++20 \
  -Wall \
  -Wextra \
  -Werror \
  -I"$ROOT_DIR/vendor/dxmt/src/dxmt" \
  -I"$ROOT_DIR/vendor/dxmt/src/winemetal" \
  -I"$ROOT_DIR/vendor/dxmt/src/util" \
  -I"$ROOT_DIR/vendor/dxmt/include/native/directx" \
  "$ROOT_DIR/tools/d3d12-metal-sdk/probes/probe_provider_architecture/probe_provider_architecture.cpp" \
  "$ROOT_DIR/vendor/dxmt/src/dxmt/dxmt_provider.cpp" \
  -o "$TEMP_DIR/probe_provider_architecture"

"$TEMP_DIR/probe_provider_architecture"
