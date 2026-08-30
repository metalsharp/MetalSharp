#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
WINE_ROOT="${METALSHARP_WINE_ROOT:-$HOME/.metalsharp/runtime/wine}"
WINE_BIN="${METALSHARP_WINE_BIN:-$WINE_ROOT/bin/wine}"
WINESERVER_BIN="${METALSHARP_WINESERVER_BIN:-$WINE_ROOT/bin/wineserver}"
OUT_DIR="$SDK_DIR/out/bin"

if [[ ! -x "$WINE_BIN" || ! -x "$WINESERVER_BIN" ]]; then
  echo "error: texture shader preparation requires MetalSharp Wine" >&2
  exit 2
fi

dxc_bin_dir="$("$SDK_DIR/scripts/fetch-dxc.sh")"
mkdir -p "$OUT_DIR"
cp "$dxc_bin_dir"/{dxc.exe,dxcompiler.dll,dxil.dll} "$OUT_DIR/"
cp "$SDK_DIR/probes/probe_texture_dimensions.hlsl" \
   "$OUT_DIR/probe_texture_dimensions.hlsl"

compiler_root="$(mktemp -d /private/tmp/metalsharp-texture-dxc.XXXXXX)"
compiler_prefix="$compiler_root/prefix"
cleanup_started=0
cleanup() {
  status=$?
  if [[ "$cleanup_started" == "0" ]]; then
    cleanup_started=1
    WINEPREFIX="$compiler_prefix" "$WINESERVER_BIN" -k >/dev/null 2>&1 || true
    WINEPREFIX="$compiler_prefix" "$WINESERVER_BIN" -w >/dev/null 2>&1 || true
    rm -rf "$compiler_root"
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

mkdir -p "$compiler_prefix"
WINEPREFIX="$compiler_prefix" WINEDEBUG=-all \
  "$WINE_BIN" wineboot -u >/dev/null 2>&1

entries=(
  cs_texture_1d cs_texture_1d_array cs_texture_1d_mip
  cs_texture_1d_array_mip cs_texture_2d cs_texture_2d_array
  cs_texture_3d cs_texture_cube cs_texture_cube_array cs_texture_2d_ms
  cs_texture_2d_ms_array cs_texture_typed_uint cs_texture_typed_sint
  cs_texture_typed_uint2 cs_texture_typed_uint4 cs_texture_typed_sint4
  cs_texture_typed_float16 cs_texture_typed_uint64 cs_texture_typed_sint64
  cs_store_1d cs_store_1d_array cs_store_2d
  cs_store_2d_array cs_store_3d cs_store_typed_uint cs_store_typed_sint
  cs_store_typed_uint4 cs_store_typed_sint4 cs_store_typed_uint64
  cs_store_typed_sint64
)

for entry in "${entries[@]}"; do
  output="$OUT_DIR/${entry}.cso"
  wine_output="$(WINEPREFIX="$compiler_prefix" WINEDEBUG=-all \
    "$WINE_BIN" winepath -w "$output")"
  defines=(-D M12_TEXTURE_PROBE=1)
  case "$entry" in
    cs_texture_typed_uint) defines=(-D M12_TYPED_UINT=1) ;;
    cs_texture_typed_sint) defines=(-D M12_TYPED_SINT=1) ;;
    cs_texture_typed_uint2) defines=(-D M12_TYPED_UINT2=1) ;;
    cs_texture_typed_uint4) defines=(-D M12_TYPED_UINT4=1) ;;
    cs_texture_typed_sint4) defines=(-D M12_TYPED_SINT4=1) ;;
    cs_texture_typed_float16) defines=(-D M12_TYPED_FLOAT16=1) ;;
    cs_texture_typed_uint64) defines=(-D M12_TYPED_UINT64=1) ;;
    cs_texture_typed_sint64) defines=(-D M12_TYPED_SINT64=1) ;;
    cs_store_typed_uint) defines=(-D M12_STORE_UINT=1) ;;
    cs_store_typed_sint) defines=(-D M12_STORE_SINT=1) ;;
    cs_store_typed_uint4) defines=(-D M12_STORE_UINT4=1) ;;
    cs_store_typed_sint4) defines=(-D M12_STORE_SINT4=1) ;;
    cs_store_typed_uint64) defines=(-D M12_STORE_UINT64=1) ;;
    cs_store_typed_sint64) defines=(-D M12_STORE_SINT64=1) ;;
  esac
  (
    cd "$OUT_DIR"
    WINEPREFIX="$compiler_prefix" WINEDEBUG=-all \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E "$entry" -T cs_6_6 -HV 2021 \
      "${defines[@]}" -Fo "$wine_output" probe_texture_dimensions.hlsl \
      >/dev/null
  )
  if [[ ! -s "$output" ]]; then
    echo "error: DXC did not produce $output" >&2
    exit 1
  fi
done

printf 'prepared %u texture-dimension shaders in %s\n' "${#entries[@]}" "$OUT_DIR"
