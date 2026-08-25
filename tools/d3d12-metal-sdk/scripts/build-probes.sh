#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
OUT_DIR="$SDK_DIR/out/bin"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
AGILITY_VERSION="${AGILITY_VERSION:-1.619.3}"
AGILITY_BIN="${AGILITY_BIN:-}"
DXC_BIN_DIR="${DXC_BIN_DIR:-}"

mkdir -p "$OUT_DIR"
mkdir -p "$OUT_DIR/D3D12"
mkdir -p "$OUT_DIR/D3D12/x64"

build_probe() {
  "$CXX" \
    -std=c++17 \
    -O2 \
    -static \
    -static-libgcc \
    -static-libstdc++ \
    -Wall \
    -Wextra \
    -Werror \
    "$@"
}

build_mini_probe() {
  local case_id="$1"
  local probe_name="$2"
  shift 2
  build_probe \
    "-DMINI_PROBE_CASE=$case_id" \
    "-DMINI_PROBE_NAME=\"$probe_name\"" \
    "$SDK_DIR/probes/probe_mini_suite/probe_mini_suite.cpp" \
    "$@" \
    -o "$OUT_DIR/probe_mini_$probe_name.exe"
}

if [[ -z "$AGILITY_BIN" ]]; then
  AGILITY_BIN="$("$SDK_DIR/scripts/fetch-agility.sh" --version "$AGILITY_VERSION")"
fi

if [[ -z "$DXC_BIN_DIR" ]]; then
  DXC_BIN_DIR="$("$SDK_DIR/scripts/fetch-dxc.sh")"
fi

for dll in dxc.exe dxcompiler.dll dxil.dll; do
  if [[ ! -f "$DXC_BIN_DIR/$dll" ]]; then
    echo "Missing DXC file: $DXC_BIN_DIR/$dll" >&2
    exit 2
  fi
  cp "$DXC_BIN_DIR/$dll" "$OUT_DIR/$dll"
done

cp "$DXC_BIN_DIR/dxil.dll" "$OUT_DIR/D3D12/dxil.dll"
cp "$DXC_BIN_DIR/dxil.dll" "$OUT_DIR/D3D12/x64/dxil.dll"

build_probe \
  "$SDK_DIR/probes/probe_loader/probe_loader.cpp" \
  -o "$OUT_DIR/probe_loader.exe"

for dll in D3D12Core.dll d3d12SDKLayers.dll D3D12StateObjectCompiler.dll; do
  if [[ ! -f "$AGILITY_BIN/$dll" ]]; then
    echo "Missing Agility SDK DLL: $AGILITY_BIN/$dll" >&2
    exit 2
  fi
  cp "$AGILITY_BIN/$dll" "$OUT_DIR/D3D12/$dll"
  cp "$AGILITY_BIN/$dll" "$OUT_DIR/D3D12/x64/$dll"
done

for optional in D3D12StateObjectCompiler.exe d3dconfig.exe; do
  if [[ -f "$AGILITY_BIN/$optional" ]]; then
    cp "$AGILITY_BIN/$optional" "$OUT_DIR/D3D12/$optional"
    cp "$AGILITY_BIN/$optional" "$OUT_DIR/D3D12/x64/$optional"
  else
    rm -f "$OUT_DIR/D3D12/$optional"
    rm -f "$OUT_DIR/D3D12/x64/$optional"
  fi
done

build_probe \
  "$SDK_DIR/probes/probe_agility_ue5/probe_agility_ue5.cpp" \
  -o "$OUT_DIR/probe_agility_ue5.exe"

build_probe \
  "$SDK_DIR/probes/probe_device_caps/probe_device_caps.cpp" \
  -o "$OUT_DIR/probe_device_caps.exe"

build_probe \
  "$SDK_DIR/probes/probe_feature_levels/probe_feature_levels.cpp" \
  -o "$OUT_DIR/probe_feature_levels.exe"

build_probe \
  "$SDK_DIR/probes/probe_dxgi_factory/probe_dxgi_factory.cpp" \
  -o "$OUT_DIR/probe_dxgi_factory.exe"

build_probe \
  "$SDK_DIR/probes/probe_m12_runtime_identity/probe_m12_runtime_identity.cpp" \
  -lole32 \
  -luuid \
  -o "$OUT_DIR/probe_m12_runtime_identity.exe"

build_probe \
  "$SDK_DIR/probes/probe_resources/probe_resources.cpp" \
  -o "$OUT_DIR/probe_resources.exe"

build_probe \
  "$SDK_DIR/probes/probe_queues/probe_queues.cpp" \
  -o "$OUT_DIR/probe_queues.exe"

build_probe \
  "$SDK_DIR/probes/probe_descriptors/probe_descriptors.cpp" \
  -o "$OUT_DIR/probe_descriptors.exe"

build_probe \
  "$SDK_DIR/probes/probe_shaders/probe_shaders.cpp" \
  -o "$OUT_DIR/probe_shaders.exe"

build_probe \
  "$SDK_DIR/probes/probe_dxil_semantics.cpp" \
  -o "$OUT_DIR/probe_dxil_semantics.exe"

build_probe \
  "$SDK_DIR/probes/probe_shader_corpus/probe_shader_corpus.cpp" \
  -o "$OUT_DIR/probe_shader_corpus.exe"

build_probe \
  "$SDK_DIR/probes/probe_sm66_capabilities/probe_sm66_capabilities.cpp" \
  -o "$OUT_DIR/probe_sm66_capabilities.exe"

build_probe \
  "$SDK_DIR/probes/probe_wave_ops/probe_wave_ops.cpp" \
  -o "$OUT_DIR/probe_wave_ops.exe"

build_probe \
  "$SDK_DIR/probes/probe_reflection_abi/probe_reflection_abi.cpp" \
  -o "$OUT_DIR/probe_reflection_abi.exe"

build_probe \
  "$SDK_DIR/probes/probe_graphics_pso/probe_graphics_pso.cpp" \
  -o "$OUT_DIR/probe_graphics_pso.exe"

build_probe \
  "$SDK_DIR/probes/probe_compute_pso/probe_compute_pso.cpp" \
  -o "$OUT_DIR/probe_compute_pso.exe"

build_probe \
  "$SDK_DIR/probes/probe_command_replay/probe_command_replay.cpp" \
  -o "$OUT_DIR/probe_command_replay.exe"

build_probe \
  "$SDK_DIR/probes/probe_barriers_render_pass/probe_barriers_render_pass.cpp" \
  -o "$OUT_DIR/probe_barriers_render_pass.exe"

build_probe \
  "$SDK_DIR/probes/probe_resource_views_formats/probe_resource_views_formats.cpp" \
  -o "$OUT_DIR/probe_resource_views_formats.exe"

build_probe \
  "$SDK_DIR/probes/probe_render_headless/probe_render_headless.cpp" \
  -o "$OUT_DIR/probe_render_headless.exe"

build_probe \
  "$SDK_DIR/probes/probe_present_windowed/probe_present_windowed.cpp" \
  -lole32 \
  -luuid \
  -lgdi32 \
  -o "$OUT_DIR/probe_present_windowed.exe"

build_probe \
  "$SDK_DIR/probes/m12_fresh_game/m12_fresh_game.cpp" \
  -lole32 \
  -luuid \
  -lgdi32 \
  -o "$OUT_DIR/m12_fresh_game.exe"

build_probe \
  "$SDK_DIR/probes/probe_subnautica_stress_game/probe_subnautica_stress_game.cpp" \
  -lole32 \
  -luuid \
  -lgdi32 \
  -o "$OUT_DIR/probe_subnautica_stress_game.exe"

build_probe \
  "$SDK_DIR/probes/probe_subnautica_full_stress/probe_subnautica_full_stress.cpp" \
  -lole32 \
  -luuid \
  -lgdi32 \
  -o "$OUT_DIR/probe_subnautica_full_stress.exe"

build_mini_probe 1 create_device
build_mini_probe 2 command_queue
build_mini_probe 3 swapchain_present -lole32 -luuid -lgdi32
build_mini_probe 4 rtv_clear
build_mini_probe 5 compute_dispatch
build_mini_probe 6 root_signature
build_mini_probe 7 descriptors
build_mini_probe 8 graphics_pso
build_mini_probe 9 geometry_shader_pso
build_mini_probe 10 mesh_object_shader_pso
build_mini_probe 11 texture_sample
build_mini_probe 12 subnautica_geometry_dxil_replay
build_mini_probe 13 dxil_texture_color_output
build_mini_probe 14 compute_first_use_dispatch

echo "$OUT_DIR/probe_loader.exe"
echo "$OUT_DIR/probe_agility_ue5.exe"
echo "$OUT_DIR/probe_device_caps.exe"
echo "$OUT_DIR/probe_feature_levels.exe"
echo "$OUT_DIR/probe_dxgi_factory.exe"
echo "$OUT_DIR/probe_m12_runtime_identity.exe"
echo "$OUT_DIR/probe_resources.exe"
echo "$OUT_DIR/probe_queues.exe"
echo "$OUT_DIR/probe_descriptors.exe"
echo "$OUT_DIR/probe_shaders.exe"
echo "$OUT_DIR/probe_dxil_semantics.exe"
echo "$OUT_DIR/probe_shader_corpus.exe"
echo "$OUT_DIR/probe_sm66_capabilities.exe"
echo "$OUT_DIR/probe_wave_ops.exe"
echo "$OUT_DIR/probe_reflection_abi.exe"
echo "$OUT_DIR/probe_graphics_pso.exe"
echo "$OUT_DIR/probe_compute_pso.exe"
echo "$OUT_DIR/probe_command_replay.exe"
echo "$OUT_DIR/probe_barriers_render_pass.exe"
echo "$OUT_DIR/probe_resource_views_formats.exe"
echo "$OUT_DIR/probe_render_headless.exe"
echo "$OUT_DIR/probe_present_windowed.exe"
echo "$OUT_DIR/m12_fresh_game.exe"
echo "$OUT_DIR/probe_subnautica_stress_game.exe"
echo "$OUT_DIR/probe_subnautica_full_stress.exe"
echo "$OUT_DIR/probe_mini_create_device.exe"
echo "$OUT_DIR/probe_mini_command_queue.exe"
echo "$OUT_DIR/probe_mini_swapchain_present.exe"
echo "$OUT_DIR/probe_mini_rtv_clear.exe"
echo "$OUT_DIR/probe_mini_compute_dispatch.exe"
echo "$OUT_DIR/probe_mini_root_signature.exe"
echo "$OUT_DIR/probe_mini_descriptors.exe"
echo "$OUT_DIR/probe_mini_graphics_pso.exe"
echo "$OUT_DIR/probe_mini_geometry_shader_pso.exe"
echo "$OUT_DIR/probe_mini_mesh_object_shader_pso.exe"
echo "$OUT_DIR/probe_mini_texture_sample.exe"
echo "$OUT_DIR/probe_mini_subnautica_geometry_dxil_replay.exe"
echo "$OUT_DIR/probe_mini_dxil_texture_color_output.exe"
echo "$OUT_DIR/probe_mini_compute_first_use_dispatch.exe"
