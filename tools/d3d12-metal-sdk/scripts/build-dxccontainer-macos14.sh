#!/usr/bin/env bash
set -euo pipefail

# Build the small GPTK D3DMetal container ABI shim without lowering an
# existing Mach-O's metadata. The DXC library passed here must itself have
# been built with MACOSX_DEPLOYMENT_TARGET=14.0.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SOURCE_DIR="${DXC_SOURCE_DIR:-}"
BUILD_DIR="${DXC_BUILD_DIR:-${SOURCE_DIR:+$SOURCE_DIR/build-macos14-x86}}"
OUT="${DXC_CONTAINER_OUT:-${OUT:-$ROOT_DIR/tools/d3d12-metal-sdk/out/libdxccontainer.dylib}}"
CXX="${CXX:-clang++}"
ARCH="${ARCH:-x86_64}"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"

if [[ -z "$SOURCE_DIR" || -z "$BUILD_DIR" ]]; then
  echo "Set DXC_SOURCE_DIR to a checked-out DirectXShaderCompiler tree (and optionally DXC_BUILD_DIR)." >&2
  exit 2
fi

DXC_LIB="$BUILD_DIR/lib/libdxcompiler.dylib"
DXC_CONTAINER_CPP="$ROOT_DIR/tools/d3d12-metal-sdk/src/dxccontainer-macos14.cpp"
DXIL_CONTAINER_CPP="$SOURCE_DIR/lib/DxilContainer/DxilContainer.cpp"

for required in "$DXC_LIB" "$DXC_CONTAINER_CPP" "$DXIL_CONTAINER_CPP"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing required DXC build input: $required" >&2
    exit 2
  fi
done

mkdir -p "$(dirname "$OUT")"
"$CXX" \
  -arch "$ARCH" \
  -std=c++17 \
  -stdlib=libc++ \
  -dynamiclib \
  -fvisibility=default \
  -mmacosx-version-min="$DEPLOYMENT_TARGET" \
  -I"$SOURCE_DIR/include" \
  -I"$SOURCE_DIR/tools/clang/include" \
  -I"$SOURCE_DIR/include/dxc" \
  "$DXC_CONTAINER_CPP" \
  "$DXIL_CONTAINER_CPP" \
  -L"$BUILD_DIR/lib" \
  -Wl,-rpath,@loader_path \
  -ldxcompiler \
  -o "$OUT"

install_name_tool -id '@rpath/libdxccontainer.dylib' "$OUT"

if [[ -n "${GPTK_D3DMETAL_RESOURCES:-}" ]]; then
  cp -p "$OUT" "$GPTK_D3DMETAL_RESOURCES/libdxccontainer.dylib"
fi

printf '%s\n' "$OUT"
