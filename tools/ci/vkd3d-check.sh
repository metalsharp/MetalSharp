#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/app/bundles"
VKD3D_BUNDLE_HASH_MANIFEST="$ROOT_DIR/tools/ci/vkd3d-bundle-hashes.tsv"
DEFAULT_WINE_ROOT="$HOME/.metalsharp/tmp/vkd3d_check_runtime/wine"
WINE_ROOT="${METALSHARP_WINE_ROOT:-$DEFAULT_WINE_ROOT}"
VKD3D_DXMT_ROOT="${METALSHARP_VKD3D_DXMT_ROOT:-$WINE_ROOT/lib/dxmt_vkd3d}"
DXMT_BUILD_DIR="${DXMT_BUILD_DIR:-$ROOT_DIR/vendor/dxmt/build-metalsharp-x64}"
TOOLCHAIN_ROOT="${METALSHARP_X86_LLVM_ROOT:-$HOME/.metalsharp/toolchains}"
VKD3D_CHECK_SECONDS="${VKD3D_CHECK_SECONDS:-10}"
VKD3D_CHECK_RUN_LIVE="${VKD3D_CHECK_RUN_LIVE:-0}"
VKD3D_LOG="$HOME/.metalsharp/tmp/vkd3d_game_run/vkd3d_game.log"
LLVM_NAME="clang+llvm-15.0.7-x86_64-apple-darwin21.0"
LLVM_LIB="$TOOLCHAIN_ROOT/$LLVM_NAME/lib"
TMP_RUNTIME=""
TMP_DXMT=""

cleanup() {
  if [[ -n "$TMP_RUNTIME" ]]; then
    rm -rf "$TMP_RUNTIME"
  fi
  if [[ -n "$TMP_DXMT" ]]; then
    rm -rf "$TMP_DXMT"
  fi
}
trap cleanup EXIT

download_bundle() {
  local asset="$1"
  local target="$BUNDLE_DIR/$asset"
  if [[ ! -s "$target" ]]; then
    curl -fsSL --retry 3 -o "$target" "https://github.com/metalsharp/MetalSharp/releases/download/bundles/$asset"
  fi
  if ! "$ROOT_DIR/tools/ci/verify-bundle-sha256.sh" "$VKD3D_BUNDLE_HASH_MANIFEST" "$asset" "$target"; then
    rm -f "$target"
    exit 1
  fi
}

prepare_runtime() {
  if [[ "$WINE_ROOT" == "$DEFAULT_WINE_ROOT" ]]; then
    rm -rf "$WINE_ROOT"
  fi
  mkdir -p "$BUNDLE_DIR" "$WINE_ROOT" "$VKD3D_DXMT_ROOT/x86_64-unix" "$VKD3D_DXMT_ROOT/x86_64-windows"
  mkdir -p "$WINE_ROOT/lib/wine/x86_64-unix"

  download_bundle "metalsharp-runtime.tar.zst"
  download_bundle "metalsharp-graphics-dll.tar.zst"

  TMP_RUNTIME="$(mktemp -d)"
  TMP_DXMT="$(mktemp -d)"

  tar --use-compress-program=unzstd -xf "$BUNDLE_DIR/metalsharp-runtime.tar.zst" -C "$TMP_RUNTIME"
  if command -v ditto >/dev/null 2>&1; then
    ditto --noextattr --noqtn "$TMP_RUNTIME/runtime/wine" "$WINE_ROOT"
  else
    cp -R "$TMP_RUNTIME/runtime/wine/." "$WINE_ROOT/"
  fi

  tar --use-compress-program=unzstd -xf "$BUNDLE_DIR/metalsharp-graphics-dll.tar.zst" -C "$TMP_DXMT"
  local bundle_dxmt_root="$TMP_DXMT/Graphics/dll/dxmt-vkd3d"
  if [[ ! -d "$bundle_dxmt_root" ]]; then
    bundle_dxmt_root="$TMP_DXMT/Graphics/dll/dxmt"
  fi
  if command -v ditto >/dev/null 2>&1; then
    ditto --noextattr --noqtn "$bundle_dxmt_root/x86_64-unix" "$VKD3D_DXMT_ROOT/x86_64-unix"
    ditto --noextattr --noqtn "$bundle_dxmt_root/x86_64-windows" "$VKD3D_DXMT_ROOT/x86_64-windows"
  else
    cp -R "$bundle_dxmt_root/x86_64-unix/." "$VKD3D_DXMT_ROOT/x86_64-unix/"
    cp -R "$bundle_dxmt_root/x86_64-windows/." "$VKD3D_DXMT_ROOT/x86_64-windows/"
  fi
  cp "$VKD3D_DXMT_ROOT/x86_64-unix/winemetal.so" "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"

  if command -v xattr >/dev/null 2>&1; then
    xattr -cr "$WINE_ROOT" || true
  fi
}

assert_vkd3d_log_passed() {
  if grep -E 'FAIL|unix_call_failed|encode failed|VKD3D render encoder encode failed|vertex_range_oob' "$VKD3D_LOG"; then
    echo "VKD3D Check found a failure marker in $VKD3D_LOG" >&2
    exit 1
  fi

  grep -q '\[PASS\] sparse_vertex_draws' "$VKD3D_LOG"
  grep -q '\[PASS\] texture_descriptor_draw' "$VKD3D_LOG"
  grep -q '\[PASS\] rgb_cube_10s' "$VKD3D_LOG"
  grep -q '=== vkd3d_game.exe PASS ===' "$VKD3D_LOG"
}

assert_file_nonempty() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    echo "VKD3D Check missing required file: $path" >&2
    exit 1
  fi
}

assert_vkd3d_contract() {
  local built_files=(
    "$DXMT_BUILD_DIR/src/d3d12/d3d12.dll"
    "$DXMT_BUILD_DIR/src/dxgi/dxgi.dll"
    "$DXMT_BUILD_DIR/src/dxgi/dxgi_dxmt.dll"
    "$DXMT_BUILD_DIR/src/winemetal/winemetal.dll"
    "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so"
    "$DXMT_BUILD_DIR/tests/d3d12_game/vkd3d_game.exe"
  )
  local runtime_files=(
    "$VKD3D_DXMT_ROOT/x86_64-windows/d3d10core.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/d3d11.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/d3d12.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/dxgi.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/dxgi_dxmt.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/winemetal.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/nvapi64.dll"
    "$VKD3D_DXMT_ROOT/x86_64-windows/nvngx.dll"
    "$VKD3D_DXMT_ROOT/x86_64-unix/winemetal.so"
    "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"
    "$VKD3D_DXMT_ROOT/x86_64-unix/libc++.1.dylib"
    "$VKD3D_DXMT_ROOT/x86_64-unix/libc++abi.1.dylib"
    "$VKD3D_DXMT_ROOT/x86_64-unix/libunwind.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libc++.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libc++abi.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libunwind.1.dylib"
  )

  for path in "${built_files[@]}" "${runtime_files[@]}"; do
    assert_file_nonempty "$path"
  done

  if ! cmp -s "$VKD3D_DXMT_ROOT/x86_64-unix/winemetal.so" "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"; then
    echo "VKD3D Check staged different DXMT and shared Wine winemetal.so files" >&2
    exit 1
  fi
  file "$DXMT_BUILD_DIR/tests/d3d12_game/vkd3d_game.exe" | grep -Eq 'PE32\+ executable|PE32 executable'
  file "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so" | grep -q 'x86_64'
  otool -L "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so" | grep -q '@rpath/winemetal.so'
}

prepare_runtime

python3 "$ROOT_DIR/tools/d3d12-metal-sdk/scripts/validate-vkd3d-pipeline-contract.py"

export METALSHARP_WINE_ROOT="$WINE_ROOT"
export WINE_ROOT
export METALSHARP_X86_LLVM_ROOT="$TOOLCHAIN_ROOT"
export DXMT_ENABLE_TESTS=1
export DYLD_LIBRARY_PATH="$LLVM_LIB:${DYLD_LIBRARY_PATH:-}"
export VKD3D_GAME_WINEPREFIX="${VKD3D_GAME_WINEPREFIX:-$HOME/.metalsharp/tmp/vkd3d_check_prefix}"
export VKD3D_GAME_TIMEOUT="${VKD3D_GAME_TIMEOUT:-120}"

"$ROOT_DIR/tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh"
python3 "$ROOT_DIR/tools/d3d12-metal-sdk/scripts/stage-dxmt-runtime.py" \
  --profile vkd3d-check \
  --build-dir "$DXMT_BUILD_DIR" \
  --runtime-dir "$VKD3D_DXMT_ROOT"

assert_vkd3d_contract

if [[ "$VKD3D_CHECK_RUN_LIVE" == "1" ]]; then
  "$ROOT_DIR/vendor/dxmt/tests/d3d12_game/run_vkd3d_game.sh" \
    "$DXMT_BUILD_DIR" \
    --quick-checks \
    --seconds "$VKD3D_CHECK_SECONDS"

  assert_vkd3d_log_passed
  tail -n 20 "$VKD3D_LOG"
else
  echo "VKD3D Check staged the DXMT runtime and built vkd3d_game.exe. Set VKD3D_CHECK_RUN_LIVE=1 for the 10s live cube run."
fi
