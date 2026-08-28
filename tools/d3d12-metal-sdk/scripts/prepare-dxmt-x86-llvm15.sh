#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
DXMT_DIR="${ROOT_DIR}/vendor/dxmt"
BUILD_DIR="${DXMT_DIR}/build-metalsharp-x64"
WINE_ROOT="${METALSHARP_WINE_ROOT:-${HOME}/.metalsharp/runtime/wine}"
TOOLCHAIN_ROOT="${METALSHARP_X86_LLVM_ROOT:-${RUNNER_TEMP:-${HOME}/.cache/metalsharp}/toolchains}"
LLVM_NAME="clang+llvm-15.0.7-x86_64-apple-darwin21.0"
LLVM_DIR="${TOOLCHAIN_ROOT}/${LLVM_NAME}"
LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-15.0.7/${LLVM_NAME}.tar.xz"

mkdir -p "${TOOLCHAIN_ROOT}"

if [[ ! -x "${LLVM_DIR}/bin/llvm-config" ]]; then
  archive="${TOOLCHAIN_ROOT}/${LLVM_NAME}.tar.xz"
  if [[ ! -f "${archive}" ]]; then
    curl -L --fail --progress-bar -o "${archive}" "${LLVM_URL}"
  fi
  tar -C "${TOOLCHAIN_ROOT}" -xf "${archive}"
fi

if ! file "${LLVM_DIR}/bin/llvm-config" | grep -q 'x86_64'; then
  echo "error: ${LLVM_DIR}/bin/llvm-config is not x86_64" >&2
  exit 1
fi

if [[ ! -x "${WINE_ROOT}/bin/winebuild" ]]; then
  echo "error: ${WINE_ROOT}/bin/winebuild not found; install the MetalSharp Wine runtime first" >&2
  exit 1
fi

if [[ -f "${BUILD_DIR}/meson-info/intro-machines.json" ]] &&
  ! python3 - "${BUILD_DIR}/meson-info/intro-machines.json" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    machines = json.load(handle)
host = machines.get("host", {})
raise SystemExit(0 if host.get("system") == "windows" and host.get("cpu_family") == "x86_64" else 1)
PY
then
  rm -rf "${BUILD_DIR}"
fi

if [[ -f "${BUILD_DIR}/build.ninja" ]]; then
  meson setup "${BUILD_DIR}" "${DXMT_DIR}" --reconfigure \
    --cross-file "${DXMT_DIR}/build-win64.txt" \
    -Dnative_llvm_path="${LLVM_DIR}" \
    -Dwine_install_path="${WINE_ROOT}" \
    -Denable_nvapi=true \
    -Denable_nvngx=true
else
  meson setup "${BUILD_DIR}" "${DXMT_DIR}" \
    --cross-file "${DXMT_DIR}/build-win64.txt" \
    -Dnative_llvm_path="${LLVM_DIR}" \
    -Dwine_install_path="${WINE_ROOT}" \
    -Denable_nvapi=true \
    -Denable_nvngx=true
fi

# Git checkouts do not preserve source mtimes.  Discard prior target outputs so
# a retained build directory cannot silently stage a stale PE/Unix runtime
# (especially the Winemetal export surface) when the source contents changed.
ninja -C "${BUILD_DIR}" -t clean >/dev/null

ninja -C "${BUILD_DIR}" \
  src/dxgi/dxgi.dll \
  src/dxgi/dxgi_dxmt.dll \
  src/winemetal/unix/winemetal.so \
  src/winemetal/winemetal.dll \
  src/d3d10/d3d10core.dll \
  src/d3d11/d3d11.dll \
  src/d3d12/d3d12.dll \
  src/nvapi/nvapi64.dll \
  src/nvngx/nvngx.dll

echo "x86_64 LLVM ready: ${LLVM_DIR}"
echo "rebuilt: ${BUILD_DIR}/src/dxgi/dxgi.dll"
echo "rebuilt: ${BUILD_DIR}/src/dxgi/dxgi_dxmt.dll"
echo "rebuilt: ${BUILD_DIR}/src/d3d10/d3d10core.dll"
echo "rebuilt: ${BUILD_DIR}/src/d3d11/d3d11.dll"
echo "rebuilt: ${BUILD_DIR}/src/d3d12/d3d12.dll"
echo "rebuilt: ${BUILD_DIR}/src/winemetal/winemetal.dll"
echo "rebuilt: ${BUILD_DIR}/src/winemetal/unix/winemetal.so"
echo "rebuilt: ${BUILD_DIR}/src/nvapi/nvapi64.dll"
echo "rebuilt: ${BUILD_DIR}/src/nvngx/nvngx.dll"
