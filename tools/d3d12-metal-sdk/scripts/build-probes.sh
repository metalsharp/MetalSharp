#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
OUT_DIR="$SDK_DIR/out/bin"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
AGILITY_VERSION="${AGILITY_VERSION:-1.619.5}"
AGILITY_BIN="${AGILITY_BIN:-}"
DXC_BIN_DIR="${DXC_BIN_DIR:-}"

mkdir -p "$OUT_DIR"
mkdir -p "$OUT_DIR/D3D12"
mkdir -p "$OUT_DIR/D3D12/x64"

copy_if_needed() {
  local source="$1"
  local destination="$2"
  if [[ "$source" == "$destination" ]] ||
     { [[ -f "$destination" ]] && cmp -s "$source" "$destination"; }; then
    return 0
  fi
  cp "$source" "$destination"
}

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
    -include "$SDK_DIR/probes/probe_runtime.hpp" \
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
  copy_if_needed "$DXC_BIN_DIR/$dll" "$OUT_DIR/$dll"
done

copy_if_needed "$DXC_BIN_DIR/dxil.dll" "$OUT_DIR/D3D12/dxil.dll"
copy_if_needed "$DXC_BIN_DIR/dxil.dll" "$OUT_DIR/D3D12/x64/dxil.dll"

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
  "$SDK_DIR/probes/probe_legacy_regression/probe_legacy_regression.cpp" \
  -o "$OUT_DIR/probe_legacy_regression.exe"

build_probe \
  "$SDK_DIR/probes/probe_feature_levels/probe_feature_levels.cpp" \
  -o "$OUT_DIR/probe_feature_levels.exe"

build_probe \
  "$SDK_DIR/probes/probe_object_contracts/probe_object_contracts.cpp" \
  -o "$OUT_DIR/probe_object_contracts.exe"

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
  "$SDK_DIR/probes/probe_texture_dimensions.cpp" \
  -o "$OUT_DIR/probe_texture_dimensions.exe"

build_probe \
  "$SDK_DIR/probes/probe_shader_corpus/probe_shader_corpus.cpp" \
  -o "$OUT_DIR/probe_shader_corpus.exe"

build_probe \
  "$SDK_DIR/probes/probe_sm66_capabilities/probe_sm66_capabilities.cpp" \
  -o "$OUT_DIR/probe_sm66_capabilities.exe"

build_probe \
  "$SDK_DIR/probes/probe_writable_msaa/probe_writable_msaa.cpp" \
  -o "$OUT_DIR/probe_writable_msaa.exe"

build_probe \
  "$SDK_DIR/probes/probe_graphics_msaa_breadth.cpp" \
  -o "$OUT_DIR/probe_graphics_msaa_breadth.exe"

build_probe \
  "$SDK_DIR/probes/probe_graphics_msaa_depth.cpp" \
  -o "$OUT_DIR/probe_graphics_msaa_depth.exe"

build_probe \
  "$SDK_DIR/probes/probe_conservative_msaa.cpp" \
  -o "$OUT_DIR/probe_conservative_msaa.exe"

build_probe \
  "$SDK_DIR/probes/probe_independent_logic_breadth.cpp" \
  -o "$OUT_DIR/probe_independent_logic_breadth.exe"

build_probe \
  "$SDK_DIR/probes/probe_sample_positions_breadth.cpp" \
  -o "$OUT_DIR/probe_sample_positions_breadth.exe"

build_probe \
  "$SDK_DIR/probes/probe_rov/probe_rov.cpp" \
  -o "$OUT_DIR/probe_rov.exe"

build_probe \
  "$SDK_DIR/probes/probe_rov_dimensions/probe_rov_dimensions.cpp" \
  -o "$OUT_DIR/probe_rov_dimensions.exe"

build_probe \
  "$SDK_DIR/probes/probe_rov_msaa.cpp" \
  -o "$OUT_DIR/probe_rov_msaa.exe"

build_probe \
  "$SDK_DIR/probes/probe_barycentrics/probe_barycentrics.cpp" \
  -o "$OUT_DIR/probe_barycentrics.exe"

build_probe \
  "$SDK_DIR/probes/probe_vrs/probe_vrs.cpp" \
  -o "$OUT_DIR/probe_vrs.exe"

build_probe \
  "$SDK_DIR/probes/probe_sampler_feedback/probe_sampler_feedback.cpp" \
  -o "$OUT_DIR/probe_sampler_feedback.exe"

build_probe \
  "$SDK_DIR/probes/probe_sampler_feedback_pixel/probe_sampler_feedback_pixel.cpp" \
  -o "$OUT_DIR/probe_sampler_feedback_pixel.exe"

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
  "$SDK_DIR/probes/probe_rasterization_breadth/probe_rasterization_breadth.cpp" \
  -o "$OUT_DIR/probe_rasterization_breadth.exe"

build_probe \
  "$SDK_DIR/probes/probe_phase6_invalid_descriptors.cpp" \
  -o "$OUT_DIR/probe_phase6_invalid_descriptors.exe"

build_probe \
  "$SDK_DIR/probes/probe_view_instancing_breadth.cpp" \
  -o "$OUT_DIR/probe_view_instancing_breadth.exe"

build_probe \
  "$SDK_DIR/probes/probe_interpolation/probe_interpolation.cpp" \
  -o "$OUT_DIR/probe_interpolation.exe"

build_probe \
  "$SDK_DIR/probes/probe_attribute_at_vertex/probe_attribute_at_vertex.cpp" \
  -o "$OUT_DIR/probe_attribute_at_vertex.exe"

build_probe \
  "$SDK_DIR/probes/probe_cycle_counter/probe_cycle_counter.cpp" \
  -o "$OUT_DIR/probe_cycle_counter.exe"

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
build_mini_probe 22 geometry_system_matrix
build_mini_probe 10 mesh_object_shader_pso
build_mini_probe 11 texture_sample
build_mini_probe 12 subnautica_geometry_dxil_replay
build_mini_probe 13 dxil_texture_color_output
build_mini_probe 14 compute_first_use_dispatch
build_mini_probe 15 dxr_acceleration_structures
build_mini_probe 16 tessellation_shader_pso
build_mini_probe 24 tessellation_patch_constant
build_mini_probe 17 start_draw_info
build_mini_probe 18 inner_coverage
build_mini_probe 20 view_id_instancing
build_mini_probe 21 temp_registers

build_probe \
  "$SDK_DIR/probes/probe_dxr_inline.cpp" \
  -o "$OUT_DIR/probe_mini_dxr_inline.exe"

echo "$OUT_DIR/probe_loader.exe"
echo "$OUT_DIR/probe_agility_ue5.exe"
echo "$OUT_DIR/probe_device_caps.exe"
echo "$OUT_DIR/probe_legacy_regression.exe"
echo "$OUT_DIR/probe_feature_levels.exe"
echo "$OUT_DIR/probe_object_contracts.exe"
echo "$OUT_DIR/probe_dxgi_factory.exe"
echo "$OUT_DIR/probe_m12_runtime_identity.exe"
echo "$OUT_DIR/probe_resources.exe"
echo "$OUT_DIR/probe_queues.exe"
echo "$OUT_DIR/probe_descriptors.exe"
echo "$OUT_DIR/probe_shaders.exe"
echo "$OUT_DIR/probe_dxil_semantics.exe"
echo "$OUT_DIR/probe_shader_corpus.exe"
echo "$OUT_DIR/probe_sm66_capabilities.exe"
echo "$OUT_DIR/probe_writable_msaa.exe"
echo "$OUT_DIR/probe_rov.exe"
echo "$OUT_DIR/probe_barycentrics.exe"
echo "$OUT_DIR/probe_vrs.exe"
echo "$OUT_DIR/probe_sampler_feedback.exe"
echo "$OUT_DIR/probe_sampler_feedback_pixel.exe"
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
echo "$OUT_DIR/probe_mini_geometry_system_matrix.exe"
echo "$OUT_DIR/probe_mini_mesh_object_shader_pso.exe"
echo "$OUT_DIR/probe_mini_texture_sample.exe"
echo "$OUT_DIR/probe_mini_subnautica_geometry_dxil_replay.exe"
echo "$OUT_DIR/probe_mini_dxil_texture_color_output.exe"
echo "$OUT_DIR/probe_mini_compute_first_use_dispatch.exe"
echo "$OUT_DIR/probe_mini_dxr_acceleration_structures.exe"
echo "$OUT_DIR/probe_mini_dxr_inline.exe"
echo "$OUT_DIR/probe_mini_tessellation_shader_pso.exe"
echo "$OUT_DIR/probe_mini_tessellation_patch_constant.exe"
echo "$OUT_DIR/probe_mini_start_draw_info.exe"
echo "$OUT_DIR/probe_mini_inner_coverage.exe"
echo "$OUT_DIR/probe_mini_view_id_instancing.exe"
echo "$OUT_DIR/probe_mini_temp_registers.exe"
