#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE_DIR="$ROOT_DIR/app/bundles"
M12_BUNDLE_HASH_MANIFEST="$ROOT_DIR/tools/ci/m12-bundle-hashes.tsv"
DEFAULT_WINE_ROOT="$HOME/.metalsharp/tmp/m12_check_runtime/wine"
WINE_ROOT="${METALSHARP_WINE_ROOT:-$DEFAULT_WINE_ROOT}"
M12_DXMT_ROOT="${METALSHARP_M12_DXMT_ROOT:-$WINE_ROOT/lib/dxmt_m12}"
DXMT_BUILD_DIR="${DXMT_BUILD_DIR:-$ROOT_DIR/vendor/dxmt/build-metalsharp-x64}"
TOOLCHAIN_ROOT="${METALSHARP_X86_LLVM_ROOT:-$HOME/.metalsharp/toolchains}"
M12_CHECK_SECONDS="${M12_CHECK_SECONDS:-10}"
M12_CHECK_RUN_LIVE="${M12_CHECK_RUN_LIVE:-0}"
M12_LOG="$HOME/.metalsharp/tmp/m12_game_run/m12_game.log"
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
    curl -fsSL --retry 3 -o "$target" "https://github.com/aaf2tbz/metalsharp/releases/download/bundles/$asset"
  fi
  if ! "$ROOT_DIR/tools/ci/verify-bundle-sha256.sh" "$M12_BUNDLE_HASH_MANIFEST" "$asset" "$target"; then
    rm -f "$target"
    exit 1
  fi
}

prepare_runtime() {
  if [[ "$WINE_ROOT" == "$DEFAULT_WINE_ROOT" ]]; then
    rm -rf "$WINE_ROOT"
  fi
  mkdir -p "$BUNDLE_DIR" "$WINE_ROOT" "$M12_DXMT_ROOT/x86_64-unix" "$M12_DXMT_ROOT/x86_64-windows"
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
  local bundle_dxmt_root="$TMP_DXMT/Graphics/dll/dxmt-m12"
  if [[ ! -d "$bundle_dxmt_root" ]]; then
    bundle_dxmt_root="$TMP_DXMT/Graphics/dll/dxmt"
  fi
  if command -v ditto >/dev/null 2>&1; then
    ditto --noextattr --noqtn "$bundle_dxmt_root/x86_64-unix" "$M12_DXMT_ROOT/x86_64-unix"
    ditto --noextattr --noqtn "$bundle_dxmt_root/x86_64-windows" "$M12_DXMT_ROOT/x86_64-windows"
  else
    cp -R "$bundle_dxmt_root/x86_64-unix/." "$M12_DXMT_ROOT/x86_64-unix/"
    cp -R "$bundle_dxmt_root/x86_64-windows/." "$M12_DXMT_ROOT/x86_64-windows/"
  fi
  cp "$M12_DXMT_ROOT/x86_64-unix/winemetal.so" "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"

  if command -v xattr >/dev/null 2>&1; then
    xattr -cr "$WINE_ROOT" || true
  fi
}

assert_m12_log_passed() {
  if grep -E 'FAIL|unix_call_failed|encode failed|M12 render encoder encode failed|vertex_range_oob' "$M12_LOG"; then
    echo "M12 Check found a failure marker in $M12_LOG" >&2
    exit 1
  fi

  grep -q '\[PASS\] sparse_vertex_draws' "$M12_LOG"
  grep -q '\[PASS\] texture_descriptor_draw' "$M12_LOG"
  grep -q '\[PASS\] rgb_cube_10s' "$M12_LOG"
  grep -q '=== m12_game.exe PASS ===' "$M12_LOG"
}

assert_file_nonempty() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    echo "M12 Check missing required file: $path" >&2
    exit 1
  fi
}

assert_m12_contract() {
  local built_files=(
    "$DXMT_BUILD_DIR/src/d3d12/d3d12.dll"
    "$DXMT_BUILD_DIR/src/dxgi/dxgi.dll"
    "$DXMT_BUILD_DIR/src/dxgi/dxgi_dxmt.dll"
    "$DXMT_BUILD_DIR/src/winemetal/winemetal.dll"
    "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so"
    "$DXMT_BUILD_DIR/tests/d3d12_game/m12_game.exe"
  )
  local runtime_files=(
    "$M12_DXMT_ROOT/x86_64-windows/d3d10core.dll"
    "$M12_DXMT_ROOT/x86_64-windows/d3d11.dll"
    "$M12_DXMT_ROOT/x86_64-windows/d3d12.dll"
    "$M12_DXMT_ROOT/x86_64-windows/dxgi.dll"
    "$M12_DXMT_ROOT/x86_64-windows/dxgi_dxmt.dll"
    "$M12_DXMT_ROOT/x86_64-windows/winemetal.dll"
    "$M12_DXMT_ROOT/x86_64-windows/nvapi64.dll"
    "$M12_DXMT_ROOT/x86_64-windows/nvngx.dll"
    "$M12_DXMT_ROOT/x86_64-unix/winemetal.so"
    "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"
    "$M12_DXMT_ROOT/x86_64-unix/libc++.1.dylib"
    "$M12_DXMT_ROOT/x86_64-unix/libc++abi.1.dylib"
    "$M12_DXMT_ROOT/x86_64-unix/libunwind.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libc++.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libc++abi.1.dylib"
    "$WINE_ROOT/lib/wine/x86_64-unix/libunwind.1.dylib"
  )

  for path in "${built_files[@]}" "${runtime_files[@]}"; do
    assert_file_nonempty "$path"
  done

  if ! cmp -s "$M12_DXMT_ROOT/x86_64-unix/winemetal.so" "$WINE_ROOT/lib/wine/x86_64-unix/winemetal.so"; then
    echo "M12 Check staged different DXMT and shared Wine winemetal.so files" >&2
    exit 1
  fi
  file "$DXMT_BUILD_DIR/tests/d3d12_game/m12_game.exe" | grep -Eq 'PE32\+ executable|PE32 executable'
  file "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so" | grep -q 'x86_64'
  otool -L "$DXMT_BUILD_DIR/src/winemetal/unix/winemetal.so" | grep -q '@rpath/winemetal.so'
}

prepare_runtime

python3 "$ROOT_DIR/tools/d3d12-metal-sdk/scripts/validate-m12-pipeline-contract.py"

export METALSHARP_WINE_ROOT="$WINE_ROOT"
export WINE_ROOT
export METALSHARP_X86_LLVM_ROOT="$TOOLCHAIN_ROOT"
export DXMT_ENABLE_TESTS=1
export DYLD_LIBRARY_PATH="$LLVM_LIB:${DYLD_LIBRARY_PATH:-}"
export M12_GAME_WINEPREFIX="${M12_GAME_WINEPREFIX:-$HOME/.metalsharp/tmp/m12_check_prefix}"
export M12_GAME_TIMEOUT="${M12_GAME_TIMEOUT:-120}"

"$ROOT_DIR/tools/d3d12-metal-sdk/scripts/prepare-dxmt-x86-llvm15.sh"
python3 "$ROOT_DIR/tools/d3d12-metal-sdk/scripts/stage-dxmt-runtime.py" \
  --profile m12-check \
  --build-dir "$DXMT_BUILD_DIR" \
  --runtime-dir "$M12_DXMT_ROOT"

assert_m12_contract

if [[ "$M12_CHECK_RUN_LIVE" == "1" ]]; then
  "$ROOT_DIR/vendor/dxmt/tests/d3d12_game/run_m12_game.sh" \
    "$DXMT_BUILD_DIR" \
    --quick-checks \
    --seconds "$M12_CHECK_SECONDS"

  assert_m12_log_passed
  tail -n 20 "$M12_LOG"
else
  echo "M12 Check staged the DXMT runtime and built m12_game.exe. Set M12_CHECK_RUN_LIVE=1 for the 10s live cube run."
fi
