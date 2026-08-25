#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
BUILD_DIR="${METALSHARP_DXMT_BUILD_DIR:-$ROOT_DIR/vendor/dxmt/build-metalsharp-x64}"
SOURCE_WINE_ROOT="${METALSHARP_WINE_ROOT:-$HOME/.metalsharp/runtime/wine}"
TOOLCHAIN_ROOT="${METALSHARP_X86_LLVM_ROOT:-/Volumes/AverySSD/toolchains}"
LLVM_NAME="clang+llvm-15.0.7-x86_64-apple-darwin21.0"
LLVM_LIB="$TOOLCHAIN_ROOT/$LLVM_NAME/lib"

usage() {
  cat <<'EOF'
Usage: run-source-probes.sh [run-isolated-probes.sh options]

Clones MetalSharp's vendored Wine 11.5 with APFS copy-on-write, stages the
current external-tree DXMT build into that disposable clone, invokes
run-isolated-probes.sh, and removes the clone afterward. The child wrapper
separately creates, stops, and removes its disposable Wine prefix.

Environment overrides:
  METALSHARP_DXMT_BUILD_DIR   DXMT Meson build directory
  METALSHARP_WINE_ROOT       MetalSharp Wine root
  METALSHARP_X86_LLVM_ROOT   Parent of the pinned x86_64 LLVM toolchain
EOF
}

for arg in "$@"; do
  case "$arg" in
    -h|--help)
      usage
      exit 0
      ;;
  esac
done

windows_artifacts=(
  "src/d3d10/d3d10core.dll:d3d10core.dll"
  "src/d3d11/d3d11.dll:d3d11.dll"
  "src/d3d12/d3d12.dll:d3d12.dll"
  "src/dxgi/dxgi.dll:dxgi.dll"
  "src/dxgi/dxgi_dxmt.dll:dxgi_dxmt.dll"
  "src/winemetal/winemetal.dll:winemetal.dll"
)
unix_artifact="src/winemetal/unix/winemetal.so"
sidecars=(libc++.1.dylib libc++abi.1.dylib libunwind.1.dylib)

for entry in "${windows_artifacts[@]}"; do
  source_rel="${entry%%:*}"
  if [[ ! -f "$BUILD_DIR/$source_rel" ]]; then
    echo "error: missing DXMT build artifact: $BUILD_DIR/$source_rel" >&2
    exit 2
  fi
done
if [[ ! -f "$BUILD_DIR/$unix_artifact" ]]; then
  echo "error: missing DXMT build artifact: $BUILD_DIR/$unix_artifact" >&2
  exit 2
fi
for sidecar in "${sidecars[@]}"; do
  if [[ ! -f "$LLVM_LIB/$sidecar" ]]; then
    echo "error: missing x86_64 LLVM sidecar: $LLVM_LIB/$sidecar" >&2
    exit 2
  fi
done

# Wine classifies DXMT's PE modules as builtins and resolves those modules from
# <wine>/lib/wine/x86_64-windows before app-local or WINEDLLPATH copies. Clone
# the vendored Wine 11.5 tree with APFS copy-on-write, replace modules only in
# the disposable clone, and remove it after the isolated-prefix wrapper exits.
wine_clone_parent="$(mktemp -d /private/tmp/metalsharp-source-wine.XXXXXX)"
WINE_ROOT="$wine_clone_parent/wine"
cp -cR "$SOURCE_WINE_ROOT" "$WINE_ROOT"
mkdir -p "$WINE_ROOT/lib"
runtime_dir="$(mktemp -d "$WINE_ROOT/lib/.dxmt-source-probe.XXXXXX")"
cleanup_started=0
cleanup() {
  status=$?
  if [[ "$cleanup_started" == "0" ]]; then
    cleanup_started=1
    WINEPREFIX="$wine_clone_parent/prefix" "$WINE_ROOT/bin/wineserver" -k \
      >/dev/null 2>&1 || true
    rm -rf "$wine_clone_parent"
    if [[ -e "$wine_clone_parent" ]]; then
      echo "error: failed to remove disposable source Wine: $wine_clone_parent" >&2
      status=1
    else
      echo "source probe Wine clone removed: $wine_clone_parent"
    fi
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

mkdir -p "$runtime_dir/x86_64-windows" "$runtime_dir/x86_64-unix"
for entry in "${windows_artifacts[@]}"; do
  source_rel="${entry%%:*}"
  destination="${entry#*:}"
  cp "$BUILD_DIR/$source_rel" "$runtime_dir/x86_64-windows/$destination"
  cp "$BUILD_DIR/$source_rel" \
    "$WINE_ROOT/lib/wine/x86_64-windows/$destination"
done
cp "$BUILD_DIR/$unix_artifact" "$runtime_dir/x86_64-unix/winemetal.so"
cp "$BUILD_DIR/$unix_artifact" \
  "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"
for sidecar in "${sidecars[@]}"; do
  cp "$LLVM_LIB/$sidecar" "$runtime_dir/x86_64-unix/$sidecar"
done

METALSHARP_WINE_ROOT="$WINE_ROOT" \
METALSHARP_DXMT_RUNTIME="$runtime_dir" \
  "$SDK_DIR/scripts/run-isolated-probes.sh" "$@"
