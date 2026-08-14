#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="$repo_root/vendor/dxmt/build-metalsharp-x64-tests"
if [[ $# -gt 0 && "$1" != --* ]]; then
  build_dir="$1"
  shift
fi
wine_root="${WINE_ROOT:-$HOME/.metalsharp/runtime/wine}"
vkd3d_dxmt_root="${METALSHARP_VKD3D_DXMT_ROOT:-$wine_root/lib/dxmt-vkd3d}"
wine_bin="${WINE_BIN:-$wine_root/bin/wine}"
run_root="${VKD3D_GAME_RUN_ROOT:-$HOME/.metalsharp/tmp/vkd3d_game_run}"
prefix="${VKD3D_GAME_WINEPREFIX:-$HOME/.metalsharp/tmp/vkd3d_game_prefix}"
loops="${VKD3D_GAME_LOOPS:-1}"
timeout_seconds="${VKD3D_GAME_TIMEOUT:-45}"
exe_name="${VKD3D_GAME_EXE:-vkd3d_game.exe}"
toolchain_root="${METALSHARP_X86_LLVM_ROOT:-$HOME/.metalsharp/toolchains}"
llvm_name="clang+llvm-15.0.7-x86_64-apple-darwin21.0"

if [[ $# -gt 0 && "$1" == --exe ]]; then
  exe_name="$2"
  shift 2
fi
if [[ "$exe_name" != *.exe ]]; then
  exe_name="$exe_name.exe"
fi

copy_llvm_dylib() {
  local dep="$1"
  local candidate
  local candidates=(
    "$build_dir/src/winemetal/unix/$dep"
    "$build_dir/src/winemetal/$dep"
    "$build_dir/$dep"
    "$toolchain_root/$llvm_name/lib/$dep"
    "/Volumes/AverySSD/toolchains/$llvm_name/lib/$dep"
  )

  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      cp "$candidate" "$run_root/unix/$dep"
      cp "$candidate" "$run_root/$dep"
      return
    fi
  done

  echo "Missing LLVM dylib required by winemetal.so: $dep" >&2
  exit 1
}

mkdir -p "$run_root" "$run_root/unix" "$(dirname "$prefix")"

cp "$build_dir/tests/d3d12_game/$exe_name" "$run_root/"
cp "$build_dir/src/d3d12/d3d12.dll" "$run_root/"
cp "$build_dir/src/dxgi/dxgi.dll" "$run_root/"
cp "$build_dir/src/dxgi/dxgi_dxmt.dll" "$run_root/"
if [[ -f "$build_dir/src/winemetal/winemetal.dll" ]]; then
  cp "$build_dir/src/winemetal/winemetal.dll" "$run_root/"
else
  cp "$vkd3d_dxmt_root/x86_64-windows/winemetal.dll" "$run_root/"
fi
cp "$wine_root/lib/wine/x86_64-windows/d3dcompiler_47.dll" "$run_root/"
if [[ -f "$build_dir/src/winemetal/unix/winemetal.so" ]]; then
  cp "$build_dir/src/winemetal/unix/winemetal.so" "$run_root/unix/"
else
  cp "$vkd3d_dxmt_root/x86_64-unix/winemetal.so" "$run_root/unix/"
fi
cp "$wine_root/lib/wine/x86_64-unix/winemac.so" "$run_root/unix/"
cp "$wine_root/lib/wine/x86_64-unix/ntdll.so" "$run_root/unix/"
cp "$run_root/unix/winemetal.so" "$run_root/"
cp "$run_root/unix/winemac.so" "$run_root/"
cp "$run_root/unix/ntdll.so" "$run_root/"
copy_llvm_dylib "libc++.1.dylib"
copy_llvm_dylib "libc++abi.1.dylib"
copy_llvm_dylib "libunwind.1.dylib"

mirror_wine_unix_sidecar() {
  local dep="$1"
  local src="$run_root/unix/$dep"
  local dxmt_dst="$vkd3d_dxmt_root/x86_64-unix/$dep"
  local wine_dst="$wine_root/lib/wine/x86_64-unix/$dep"

  if [[ ! -f "$src" ]]; then
    echo "Missing staged Unix sidecar: $src" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dxmt_dst")" "$(dirname "$wine_dst")"
  cp "$src" "$dxmt_dst"
  cp "$src" "$wine_dst"

  if ! cmp -s "$src" "$dxmt_dst" || ! cmp -s "$src" "$wine_dst"; then
    echo "Failed to mirror Unix sidecar into Wine search path: $dep" >&2
    exit 1
  fi
}

mirror_wine_unix_sidecar "winemetal.so"
mirror_wine_unix_sidecar "libc++.1.dylib"
mirror_wine_unix_sidecar "libc++abi.1.dylib"
mirror_wine_unix_sidecar "libunwind.1.dylib"

cd "$run_root"
log_name="${exe_name%.exe}.log"
rm -f "$log_name" winemetal-unix-debug.log winemetal-pe-debug.log

export WINEPREFIX="$prefix"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d12,dxgi,dxgi_dxmt,winemetal=n,b}"
export DXMT_WINEMETAL_DEBUG="${DXMT_WINEMETAL_DEBUG:-1}"
export DXMT_WINEMETAL_UNIXLIB="${DXMT_WINEMETAL_UNIXLIB:-winemetal.so}"
export DXMT_D3D12_TRACE="${DXMT_D3D12_TRACE:-1}"
export DXMT_DXGI_TRACE="${DXMT_DXGI_TRACE:-1}"
export DXMT_SHADER_CACHE_PATH="${DXMT_SHADER_CACHE_PATH:-$run_root/shader-cache}"
export DYLD_LIBRARY_PATH="$toolchain_root/$llvm_name/lib:/Volumes/AverySSD/toolchains/$llvm_name/lib:$run_root/unix:$wine_root/lib/wine/x86_64-unix:${DYLD_LIBRARY_PATH:-}"

/usr/bin/perl -e 'alarm shift; exec @ARGV' "$timeout_seconds" \
  "$wine_bin" "./$exe_name" --loops "$loops" "$@" > "$run_root/$log_name" 2>&1
