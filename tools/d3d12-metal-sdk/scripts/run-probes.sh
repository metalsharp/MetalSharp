#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SDK_DIR="$ROOT_DIR/tools/d3d12-metal-sdk"
PROFILE="standalone-wine"
WINE_BIN="${WINE_BIN:-wine}"
WINE_PREFIX="${WINEPREFIX:-}"
DXMT_RUNTIME="${DXMT_RUNTIME:-}"
RESULTS_DIR="$SDK_DIR/results"
SHADER_CACHE_DIR="${DXMT_SHADER_CACHE_PATH:-}"
GEOMETRY_CORPUS_WIN_PATH=""
METAL_SHADER_CONVERTER="${METAL_SHADER_CONVERTER:-}"
AGILITY_SDK_VERSION="${AGILITY_SDK_VERSION:-}"
AGILITY_SDK_PATH="${AGILITY_SDK_PATH:-}"
GAME_DIR=""
RUN_LOADER=1
RUN_AGILITY=1
RUN_CAPS=1
RUN_LEGACY_REGRESSION=1
# FL12_2/SM6.7 target gate is opt-in until its implementation phases land.
RUN_FEATURE_LEVELS=0
RUN_FL12_2_GATE=0
RUN_OBJECT_CONTRACTS=0
RUN_META_COMMAND=0
RUN_VIDEO=0
RUN_VIDEO_PROCESS=0
RUN_INFOQUEUE=0
RUN_DISCARD_TEXTURE=0
RUN_MANUAL_WRITE_TRACKING=0
RUN_SHARING_CONTRACT=0
RUN_CPU_TEXTURE_MAP=0
RUN_DEBUG_INTERFACES=0
RUN_DIAGNOSTICS=0
RUN_DXGI=1
RUN_RESOURCES=1
RUN_QUEUES=1
RUN_DESCRIPTORS=1
RUN_SHADERS=1
RUN_DXIL_SEMANTICS=1
RUN_TEXTURE_DIMENSIONS=0
RUN_TEXTURE_DIMENSIONS_ONLY=0
RUN_SHADER_CORPUS=1
RUN_SM66_CAPABILITIES=1
RUN_WRITABLE_MSAA=1
RUN_WRITABLE_MSAA_ONLY=0
RUN_VRS=0
RUN_VRS_ONLY=0
RUN_ROV=0
RUN_ROV_ONLY=0
RUN_BARYCENTRICS=0
RUN_BARYCENTRICS_ONLY=0
RUN_SAMPLER_FEEDBACK=1
RUN_WAVE_OPS=1
RUN_REFLECTION_ABI=1
RUN_GRAPHICS_PSO=1
RUN_COMPUTE_PSO=1
RUN_COMMAND_REPLAY=1
RUN_WORK_GRAPH=0
RUN_ATTRIBUTE_AT_VERTEX=0
RUN_CYCLE_COUNTER=0
RUN_BARRIERS_RENDER_PASS=1
RUN_RESOURCE_VIEWS_FORMATS=1
RUN_RENDER_HEADLESS=1
RUN_MINI=1
RUN_WINEMETAL_ABI=1
RUN_PRESENT_WINDOWED=0
RUN_FULL_STRESS=0
MINI_PROBE_FILTER="${METALSHARP_MINI_PROBE_FILTER:-}"
# Keep every selected DXMT PE ahead of Wine's builtin implementation.  A
# comma-separated prefix is not a valid per-module Wine override and silently
# leaves D3D10/D3D11 on wined3d, which makes the legacy regression meaningless.
DLL_OVERRIDES="${DXMT_PROBE_DLL_OVERRIDES:-d3d12=n,b;dxgi=n,b;d3d11=n,b;d3d10core=n,b;winemetal=n,b}"
MINI_PROBES=(
  create_device
  command_queue
  swapchain_present
  rtv_clear
  compute_dispatch
  root_signature
  descriptors
  graphics_pso
  geometry_shader_pso
  geometry_system_matrix
  mesh_object_shader_pso
  texture_sample
  subnautica_geometry_dxil_replay
  dxil_texture_color_output
  compute_first_use_dispatch
  dxr_inline
  dxr_acceleration_structures
  tessellation_shader_pso
  tessellation_patch_constant
  start_draw_info
  inner_coverage
  view_id_instancing
  temp_registers
)

mini_probe_selected() {
  [[ -z "$MINI_PROBE_FILTER" || "$1" == "$MINI_PROBE_FILTER" ]]
}

usage() {
  cat <<'USAGE'
Usage:
  run-probes.sh [--profile metalsharp|standalone-wine] [options]

Options:
  --profile NAME        Runtime profile. Defaults to standalone-wine.
  --wine PATH           Wine binary path.
  --prefix PATH         WINEPREFIX path.
  --dxmt-runtime PATH   Runtime root containing x86_64-windows/ and x86_64-unix/.
  --results-dir PATH    Result output directory.
  --game-dir PATH       Optional game Win64 directory containing staged DXMT DLLs.
  --agility-sdk-version N
                        Override exported D3D12SDKVersion for Agility-sensitive probes.
  --agility-sdk-path REL
                        Override exported D3D12SDKPath for Agility-sensitive probes.
  --no-loader           Skip probe_loader.
  --no-agility          Skip probe_agility_ue5.
  --agility-only        Run only the Agility SDK surface probe.
  --no-caps             Skip probe_device_caps.
  --legacy-regression   Run the D3D10/D3D11 clear-copy-readback regression gate.
  --no-legacy-regression
                        Skip the D3D10/D3D11 clear-copy-readback regression gate.
  --legacy-regression-only
                        Run only the D3D10/D3D11 clear-copy-readback regression gate.
  --caps-only           Run only the feature support / unsupported policy probe.
  --feature-levels      Run the target FL11_0-through-12_2 and SM6.7 probe.
  --feature-levels-only Run only the target feature-level and SM6.7 probe.
  --fl12-2-gate         Run the full opt-in FL12_2 query/behavior/provenance gate.
  --object-contracts    Run D3D12 object private-data/COM semantics.
  --object-contracts-only
                        Run only D3D12 object private-data/COM semantics.
  --meta-command        Run the source-owned meta-command execution probe.
  --meta-command-only   Run only the source-owned meta-command execution probe.
  --no-meta-command     Skip the meta-command execution probe.
  --video               Run the bounded D3D12 video object/feature probe.
  --video-only          Run only the bounded D3D12 video object/feature probe.
  --video-process        Run the CPU-reference D3D12 video-process execution probe.
  --video-process-only   Run only the CPU-reference video-process execution probe.
  --infoqueue             Run the InfoQueue1 callback/filter probe.
  --infoqueue-only        Run only the InfoQueue1 callback/filter probe.
  --discard-texture       Run the texture DiscardResource region probe.
  --discard-texture-only  Run only the texture DiscardResource region probe.
  --manual-write-tracking Run the manual-write tracking interface probe.
  --manual-write-tracking-only
                        Run only the manual-write tracking interface probe.
  --sharing-contract    Run the D3D12 sharing-contract/fence provider probe.
  --sharing-contract-only
                        Run only the D3D12 sharing-contract/fence provider probe.
  --cpu-texture-map     Run the CPU-visible texture Map/Unmap provider probe.
  --cpu-texture-map-only
                        Run only the CPU-visible texture Map/Unmap provider probe.
  --debug-interfaces    Run the D3D12 debug-interface state probe.
  --debug-interfaces-only
                        Run only the D3D12 debug-interface state probe.
  --diagnostics         Run Agility diagnostics/tools/settings probes.
  --diagnostics-only    Run only Agility diagnostics/tools/settings probes.
  --no-dxgi             Skip probe_dxgi_factory.
  --dxgi-only           Run only the DXGI factory probe.
  --no-resources        Skip probe_resources.
  --resources-only      Run only the resource upload/copy/format probe.
  --no-queues           Skip probe_queues.
  --queues-only         Run only the command-queue/fence/timestamp probe.
  --no-descriptors      Skip probe_descriptors.
  --descriptors-only    Run only the descriptor ABI probe.
  --no-shaders          Skip probe_shaders.
  --dxil-semantics      Run the DXIL semantic opcode-group probe.
  --semantic-only       Run only the DXIL semantic opcode-group probe.
  --texture-dimensions  Run the DXIL 1D/array/3D/cube/MSAA texture matrix.
  --texture-dimensions-only
                        Run only the DXIL texture-dimension matrix.
  --no-shader-corpus    Skip the synthetic shader corpus probe.
  --shader-corpus-only  Run only the synthetic shader corpus probe.
  --no-sm66-capabilities
                        Skip the SM 6.6 capability audit probe.
  --sm66-capabilities-only
                        Run only the SM 6.6 capability audit probe.
  --no-writable-msaa   Skip the writable MSAA texture probe.
  --writable-msaa-only Run only the writable MSAA texture probe.
  --vrs                Run the opt-in VRS/rasterization-rate map probe.
  --vrs-only           Run only the opt-in VRS/rasterization-rate map probe.
  --rov                Run the opt-in rasterizer-ordered UAV probe.
  --rov-only           Run only the rasterizer-ordered UAV probe.
  --barycentrics       Run the bounded SV_Barycentrics graphics probe.
  --barycentrics-only  Run only the bounded SV_Barycentrics graphics probe.
  --no-sampler-feedback
                        Skip the sampler-feedback compute and pixel probes.
  --sampler-feedback    Run the sampler-feedback compute and pixel probes.
  --sampler-feedback-only
                        Run only the sampler-feedback compute and pixel probes.
  --no-wave-ops        Skip the WaveOps capability audit probe.
  --wave-ops-only      Run only the WaveOps capability audit probe.
  --no-reflection-abi  Skip the reflection/descriptor ABI probe.
  --reflection-abi-only
                        Run only the reflection/descriptor ABI probe.
  --no-graphics-pso     Skip probe_graphics_pso.
  --graphics-pso-only   Run only the graphics PSO matrix probe.
  --no-compute-pso      Skip probe_compute_pso.
  --compute-pso-only    Run only the compute PSO matrix probe.
  --no-command-replay   Skip probe_command_replay.
  --command-replay-only Run only the command recording/replay probe.
  --work-graph       Run the bounded GPU-native node shader opcode probe.
  --work-graph-only  Run only the bounded GPU-native node shader opcode probe.
  --attribute-at-vertex
                        Run the bounded GPU vertex-capture AttributeAtVertex probe.
  --attribute-at-vertex-only
                        Run only the bounded GPU vertex-capture AttributeAtVertex probe.
  --cycle-counter       Run the bounded single-read CycleCounterLegacy probe.
  --cycle-counter-only  Run only the bounded single-read CycleCounterLegacy probe.
  --no-barriers-render-pass
                        Skip probe_barriers_render_pass.
  --barriers-render-pass-only
                        Run only the resource barrier/render-pass probe.
  --no-resource-views-formats
                        Skip probe_resource_views_formats.
  --resource-views-formats-only
                        Run only the resource/view/format probe.
  --render-headless     Run optional probe_render_headless.
  --no-render-headless  Skip probe_render_headless.
  --no-winemetal-abi    Skip the WineMetal PE/Unix ABI export gate.
  --winemetal-abi-only  Run only the WineMetal PE/Unix ABI export gate.
  --no-mini             Skip one-purpose D3D12 mini-app probes.
  --mini-only           Run only one-purpose D3D12 mini-app probes.
  --windowed-present    Run the optional probe_present_windowed window/swapchain proof.
  --swapchain-only      Run only the windowed swapchain/present probe.
  --no-windowed-present Skip probe_present_windowed.
  --full-stress         Run the full Subnautica DXBC shader corpus stress probe.
  -h, --help            Show this help.

Environment:
  METALSHARP_NATIVE_IRCONVERTER=1
                        Use the host libmetalirconverter provider to materialize
                        native mesh/amplification and DXR cache libraries. This
                        does not change the required METAL_SHADER_CONVERTER setting.
  METALSHARP_IRCONVERTER_ROOT
                        Header/library root for the native provider (default /usr/local).

Examples:
  tools/d3d12-metal-sdk/scripts/run-probes.sh --profile metalsharp

  tools/d3d12-metal-sdk/scripts/run-probes.sh \
    --wine /opt/wine/bin/wine \
    --prefix "$HOME/wine-d3d12-test" \
    --dxmt-runtime "$HOME/dxmt-build"
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      PROFILE="$2"
      shift 2
      ;;
    --wine)
      WINE_BIN="$2"
      shift 2
      ;;
    --prefix)
      WINE_PREFIX="$2"
      shift 2
      ;;
    --dxmt-runtime)
      DXMT_RUNTIME="$2"
      shift 2
      ;;
    --results-dir)
      RESULTS_DIR="$2"
      shift 2
      ;;
    --game-dir)
      GAME_DIR="$2"
      shift 2
      ;;
    --agility-sdk-version)
      AGILITY_SDK_VERSION="$2"
      shift 2
      ;;
    --agility-sdk-path)
      AGILITY_SDK_PATH="$2"
      shift 2
      ;;
    --no-loader)
      RUN_LOADER=0
      shift
      ;;
    --no-agility)
      RUN_AGILITY=0
      shift
      ;;
    --agility-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=1
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-caps)
      RUN_CAPS=0
      shift
      ;;
    --legacy-regression)
      RUN_LEGACY_REGRESSION=1
      shift
      ;;
    --no-legacy-regression)
      RUN_LEGACY_REGRESSION=0
      shift
      ;;
    --legacy-regression-only)
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      RUN_LEGACY_REGRESSION=1
      shift
      ;;
    --caps-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --feature-levels)
      RUN_FEATURE_LEVELS=1
      shift
      ;;
    --feature-levels-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --fl12-2-gate)
      # The aggregate validator consumes every query and behavior dependency.
      # Force the complete required matrix here so an opt-in gate cannot
      # accidentally combine a fresh feature-level/VRS result with stale or
      # missing core, shader, ABI, or mini-probe evidence.  Later explicit
      # --no-* options may still narrow an ordinary probe run, but callers
      # should expect the aggregate gate to fail if they do so.
      RUN_FL12_2_GATE=1
      RUN_LOADER=1
      RUN_AGILITY=1
      RUN_CAPS=1
      RUN_LEGACY_REGRESSION=1
      RUN_FEATURE_LEVELS=1
      RUN_OBJECT_CONTRACTS=1
      RUN_META_COMMAND=1
      RUN_DXGI=1
      RUN_RESOURCES=1
      RUN_QUEUES=1
      RUN_DESCRIPTORS=1
      RUN_SHADERS=1
      RUN_DXIL_SEMANTICS=1
      RUN_SHADER_CORPUS=1
      RUN_SM66_CAPABILITIES=1
      RUN_WRITABLE_MSAA=1
      RUN_VRS=1
      RUN_SAMPLER_FEEDBACK=1
      RUN_WAVE_OPS=1
      RUN_REFLECTION_ABI=1
      RUN_GRAPHICS_PSO=1
      RUN_COMPUTE_PSO=1
      RUN_COMMAND_REPLAY=1
      RUN_WORK_GRAPH=1
      RUN_ATTRIBUTE_AT_VERTEX=1
      RUN_CYCLE_COUNTER=1
      RUN_BARRIERS_RENDER_PASS=1
      RUN_RESOURCE_VIEWS_FORMATS=1
      RUN_RENDER_HEADLESS=1
      RUN_MINI=1
      RUN_WINEMETAL_ABI=1
      RUN_VRS_ONLY=0
      RUN_WRITABLE_MSAA_ONLY=0
      MINI_PROBE_FILTER=""
      shift
      ;;
    --object-contracts)
      RUN_OBJECT_CONTRACTS=1
      shift
      ;;
    --object-contracts-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --meta-command)
      RUN_META_COMMAND=1
      shift
      ;;
    --no-meta-command)
      RUN_META_COMMAND=0
      shift
      ;;
    --video)
      RUN_VIDEO=1
      shift
      ;;
    --video-process)
      RUN_VIDEO_PROCESS=1
      shift
      ;;
    --infoqueue)
      RUN_INFOQUEUE=1
      shift
      ;;
    --discard-texture)
      RUN_DISCARD_TEXTURE=1
      shift
      ;;
    --manual-write-tracking)
      RUN_MANUAL_WRITE_TRACKING=1
      shift
      ;;
    --sharing-contract)
      RUN_SHARING_CONTRACT=1
      shift
      ;;
    --cpu-texture-map)
      RUN_CPU_TEXTURE_MAP=1
      shift
      ;;
    --debug-interfaces)
      RUN_DEBUG_INTERFACES=1
      shift
      ;;
    --diagnostics)
      RUN_DIAGNOSTICS=1
      shift
      ;;
    --diagnostics-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=0
      RUN_MANUAL_WRITE_TRACKING=0
      RUN_SHARING_CONTRACT=0
      RUN_CPU_TEXTURE_MAP=0
      RUN_DEBUG_INTERFACES=0
      RUN_DIAGNOSTICS=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --video-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=1
      RUN_VIDEO_PROCESS=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --video-process-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=1
      RUN_INFOQUEUE=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --infoqueue-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=1
      RUN_DISCARD_TEXTURE=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --discard-texture-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=1
      RUN_MANUAL_WRITE_TRACKING=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --manual-write-tracking-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=0
      RUN_MANUAL_WRITE_TRACKING=1
      RUN_SHARING_CONTRACT=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --sharing-contract-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=0
      RUN_MANUAL_WRITE_TRACKING=0
      RUN_SHARING_CONTRACT=1
      RUN_CPU_TEXTURE_MAP=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --cpu-texture-map-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=0
      RUN_MANUAL_WRITE_TRACKING=0
      RUN_SHARING_CONTRACT=0
      RUN_CPU_TEXTURE_MAP=1
      RUN_DEBUG_INTERFACES=0
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --debug-interfaces-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=0
      RUN_VIDEO=0
      RUN_VIDEO_PROCESS=0
      RUN_INFOQUEUE=0
      RUN_DISCARD_TEXTURE=0
      RUN_MANUAL_WRITE_TRACKING=0
      RUN_SHARING_CONTRACT=0
      RUN_CPU_TEXTURE_MAP=0
      RUN_DEBUG_INTERFACES=1
      RUN_DIAGNOSTICS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --meta-command-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_META_COMMAND=1
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_ROV=0
      RUN_BARYCENTRICS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --no-dxgi)
      RUN_DXGI=0
      shift
      ;;
    --dxgi-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=1
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-resources)
      RUN_RESOURCES=0
      shift
      ;;
    --resources-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=1
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-queues)
      RUN_QUEUES=0
      shift
      ;;
    --queues-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=1
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-descriptors)
      RUN_DESCRIPTORS=0
      shift
      ;;
    --descriptors-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=1
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --no-shaders)
      RUN_SHADERS=0
      shift
      ;;
    --dxil-semantics)
      RUN_DXIL_SEMANTICS=1
      shift
      ;;
    --semantic-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=1
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_RENDER_HEADLESS=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-shader-corpus)
      RUN_SHADER_CORPUS=0
      shift
      ;;
    --texture-dimensions)
      RUN_TEXTURE_DIMENSIONS=1
      shift
      ;;
    --texture-dimensions-only)
      RUN_TEXTURE_DIMENSIONS_ONLY=1
      shift
      ;;
    --shader-corpus-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=1
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-sm66-capabilities)
      RUN_SM66_CAPABILITIES=0
      shift
      ;;
    --sm66-capabilities-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=1
      RUN_WRITABLE_MSAA=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-writable-msaa)
      RUN_WRITABLE_MSAA=0
      shift
      ;;
    --writable-msaa-only)
      RUN_WRITABLE_MSAA_ONLY=1
      shift
      ;;
    --vrs)
      RUN_VRS=1
      shift
      ;;
    --vrs-only)
      RUN_VRS_ONLY=1
      shift
      ;;
    --no-rov)
      RUN_ROV=0
      shift
      ;;
    --rov)
      RUN_ROV=1
      shift
      ;;
    --rov-only)
      RUN_ROV_ONLY=1
      shift
      ;;
    --barycentrics)
      RUN_BARYCENTRICS=1
      shift
      ;;
    --barycentrics-only)
      RUN_BARYCENTRICS_ONLY=1
      shift
      ;;
    --no-sampler-feedback)
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --sampler-feedback)
      RUN_SAMPLER_FEEDBACK=1
      shift
      ;;
    --sampler-feedback-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_SAMPLER_FEEDBACK=1
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --no-wave-ops)
      RUN_WAVE_OPS=0
      shift
      ;;
    --wave-ops-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=1
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-reflection-abi)
      RUN_REFLECTION_ABI=0
      shift
      ;;
    --reflection-abi-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=1
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-graphics-pso)
      RUN_GRAPHICS_PSO=0
      shift
      ;;
    --graphics-pso-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=1
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-compute-pso)
      RUN_COMPUTE_PSO=0
      shift
      ;;
    --compute-pso-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=1
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-command-replay)
      RUN_COMMAND_REPLAY=0
      shift
      ;;
    --work-graph)
      RUN_WORK_GRAPH=1
      shift
      ;;
    --work-graph-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=1
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --attribute-at-vertex)
      RUN_ATTRIBUTE_AT_VERTEX=1
      shift
      ;;
    --attribute-at-vertex-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_TEXTURE_DIMENSIONS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=1
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --cycle-counter)
      RUN_CYCLE_COUNTER=1
      shift
      ;;
    --cycle-counter-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_FEATURE_LEVELS=0
      RUN_OBJECT_CONTRACTS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_TEXTURE_DIMENSIONS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WRITABLE_MSAA=0
      RUN_VRS=0
      RUN_SAMPLER_FEEDBACK=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_WORK_GRAPH=0
      RUN_ATTRIBUTE_AT_VERTEX=0
      RUN_CYCLE_COUNTER=1
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=0
      RUN_PRESENT_WINDOWED=0
      RUN_FULL_STRESS=0
      shift
      ;;
    --command-replay-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=1
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-barriers-render-pass)
      RUN_BARRIERS_RENDER_PASS=0
      shift
      ;;
    --barriers-render-pass-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=1
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-resource-views-formats)
      RUN_RESOURCE_VIEWS_FORMATS=0
      shift
      ;;
    --resource-views-formats-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=1
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-render-headless)
      RUN_RENDER_HEADLESS=0
      shift
      ;;
    --render-headless)
      RUN_RENDER_HEADLESS=1
      shift
      ;;
    --no-winemetal-abi)
      RUN_WINEMETAL_ABI=0
      shift
      ;;
    --winemetal-abi-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_RENDER_HEADLESS=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_MINI=0
      RUN_WINEMETAL_ABI=1
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-mini)
      RUN_MINI=0
      shift
      ;;
    --mini-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_RENDER_HEADLESS=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_MINI=1
      RUN_PRESENT_WINDOWED=0
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --windowed-present)
      RUN_PRESENT_WINDOWED=1
      shift
      ;;
    --swapchain-only)
      RUN_LEGACY_REGRESSION=0
      RUN_LOADER=0
      RUN_AGILITY=0
      RUN_CAPS=0
      RUN_DXGI=0
      RUN_RESOURCES=0
      RUN_QUEUES=0
      RUN_DESCRIPTORS=0
      RUN_SHADERS=0
      RUN_DXIL_SEMANTICS=0
      RUN_SHADER_CORPUS=0
      RUN_SM66_CAPABILITIES=0
      RUN_WAVE_OPS=0
      RUN_REFLECTION_ABI=0
      RUN_GRAPHICS_PSO=0
      RUN_COMPUTE_PSO=0
      RUN_COMMAND_REPLAY=0
      RUN_BARRIERS_RENDER_PASS=0
      RUN_RESOURCE_VIEWS_FORMATS=0
      RUN_RENDER_HEADLESS=0
      RUN_MINI=0
      RUN_PRESENT_WINDOWED=1
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --no-windowed-present)
      RUN_PRESENT_WINDOWED=0
      shift
      ;;
    --full-stress)
      RUN_FULL_STRESS=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$RUN_WRITABLE_MSAA_ONLY" == "1" ]]; then
  RUN_LEGACY_REGRESSION=0
  RUN_LOADER=0
  RUN_AGILITY=0
  RUN_CAPS=0
  RUN_FEATURE_LEVELS=0
  RUN_OBJECT_CONTRACTS=0
  RUN_DXGI=0
  RUN_RESOURCES=0
  RUN_QUEUES=0
  RUN_DESCRIPTORS=0
  RUN_SHADERS=0
  RUN_DXIL_SEMANTICS=0
  RUN_SHADER_CORPUS=0
  RUN_SM66_CAPABILITIES=0
  RUN_WRITABLE_MSAA=1
  RUN_VRS=0
  RUN_SAMPLER_FEEDBACK=0
  RUN_WAVE_OPS=0
  RUN_REFLECTION_ABI=0
  RUN_GRAPHICS_PSO=0
  RUN_COMPUTE_PSO=0
  RUN_COMMAND_REPLAY=0
  RUN_BARRIERS_RENDER_PASS=0
  RUN_RESOURCE_VIEWS_FORMATS=0
  RUN_RENDER_HEADLESS=0
  RUN_MINI=0
  RUN_WINEMETAL_ABI=0
  RUN_PRESENT_WINDOWED=0
  RUN_FULL_STRESS=0
fi

if [[ "$RUN_VRS_ONLY" == "1" ]]; then
  RUN_LEGACY_REGRESSION=0
  RUN_LOADER=0
  RUN_AGILITY=0
  RUN_CAPS=0
  RUN_FEATURE_LEVELS=0
  RUN_OBJECT_CONTRACTS=0
  RUN_DXGI=0
  RUN_RESOURCES=0
  RUN_QUEUES=0
  RUN_DESCRIPTORS=0
  RUN_SHADERS=0
  RUN_DXIL_SEMANTICS=0
  RUN_SHADER_CORPUS=0
  RUN_SM66_CAPABILITIES=0
  RUN_WRITABLE_MSAA=0
  RUN_VRS=1
  RUN_SAMPLER_FEEDBACK=0
  RUN_WAVE_OPS=0
  RUN_REFLECTION_ABI=0
  RUN_GRAPHICS_PSO=0
  RUN_COMPUTE_PSO=0
  RUN_COMMAND_REPLAY=0
  RUN_BARRIERS_RENDER_PASS=0
  RUN_RESOURCE_VIEWS_FORMATS=0
  RUN_RENDER_HEADLESS=0
  RUN_MINI=0
  RUN_WINEMETAL_ABI=0
  RUN_PRESENT_WINDOWED=0
  RUN_FULL_STRESS=0
fi

if [[ "$RUN_ROV_ONLY" == "1" ]]; then
  RUN_LEGACY_REGRESSION=0
  RUN_LOADER=0
  RUN_AGILITY=0
  RUN_CAPS=0
  RUN_FEATURE_LEVELS=0
  RUN_OBJECT_CONTRACTS=0
  RUN_DXGI=0
  RUN_RESOURCES=0
  RUN_QUEUES=0
  RUN_DESCRIPTORS=0
  RUN_SHADERS=0
  RUN_DXIL_SEMANTICS=0
  RUN_TEXTURE_DIMENSIONS=0
  RUN_SHADER_CORPUS=0
  RUN_SM66_CAPABILITIES=0
  RUN_WRITABLE_MSAA=0
  RUN_VRS=0
  RUN_SAMPLER_FEEDBACK=0
  RUN_WAVE_OPS=0
  RUN_REFLECTION_ABI=0
  RUN_GRAPHICS_PSO=0
  RUN_COMPUTE_PSO=0
  RUN_COMMAND_REPLAY=0
  RUN_WORK_GRAPH=0
  RUN_ATTRIBUTE_AT_VERTEX=0
  RUN_CYCLE_COUNTER=0
  RUN_BARRIERS_RENDER_PASS=0
  RUN_RESOURCE_VIEWS_FORMATS=0
  RUN_RENDER_HEADLESS=0
  RUN_MINI=0
  RUN_WINEMETAL_ABI=0
  RUN_PRESENT_WINDOWED=0
  RUN_FULL_STRESS=0
  RUN_ROV=1
fi

if [[ "$RUN_BARYCENTRICS_ONLY" == "1" ]]; then
  RUN_LEGACY_REGRESSION=0
  RUN_LOADER=0
  RUN_AGILITY=0
  RUN_CAPS=0
  RUN_FEATURE_LEVELS=0
  RUN_OBJECT_CONTRACTS=0
  RUN_DXGI=0
  RUN_RESOURCES=0
  RUN_QUEUES=0
  RUN_DESCRIPTORS=0
  RUN_SHADERS=0
  RUN_DXIL_SEMANTICS=0
  RUN_TEXTURE_DIMENSIONS=0
  RUN_SHADER_CORPUS=0
  RUN_SM66_CAPABILITIES=0
  RUN_WRITABLE_MSAA=0
  RUN_VRS=0
  RUN_ROV=0
  RUN_SAMPLER_FEEDBACK=0
  RUN_WAVE_OPS=0
  RUN_REFLECTION_ABI=0
  RUN_GRAPHICS_PSO=0
  RUN_COMPUTE_PSO=0
  RUN_COMMAND_REPLAY=0
  RUN_WORK_GRAPH=0
  RUN_ATTRIBUTE_AT_VERTEX=0
  RUN_CYCLE_COUNTER=0
  RUN_BARRIERS_RENDER_PASS=0
  RUN_RESOURCE_VIEWS_FORMATS=0
  RUN_RENDER_HEADLESS=0
  RUN_MINI=0
  RUN_WINEMETAL_ABI=0
  RUN_PRESENT_WINDOWED=0
  RUN_FULL_STRESS=0
  RUN_BARYCENTRICS=1
fi

if [[ "$RUN_TEXTURE_DIMENSIONS_ONLY" == "1" ]]; then
  RUN_LEGACY_REGRESSION=0
  RUN_LOADER=0
  RUN_AGILITY=0
  RUN_CAPS=0
  RUN_FEATURE_LEVELS=0
  RUN_OBJECT_CONTRACTS=0
  RUN_DXGI=0
  RUN_RESOURCES=0
  RUN_QUEUES=0
  RUN_DESCRIPTORS=0
  RUN_SHADERS=0
  RUN_DXIL_SEMANTICS=0
  RUN_SHADER_CORPUS=0
  RUN_SM66_CAPABILITIES=0
  RUN_WRITABLE_MSAA=0
  RUN_VRS=0
  RUN_SAMPLER_FEEDBACK=0
  RUN_WAVE_OPS=0
  RUN_REFLECTION_ABI=0
  RUN_GRAPHICS_PSO=0
  RUN_COMPUTE_PSO=0
  RUN_COMMAND_REPLAY=0
  RUN_BARRIERS_RENDER_PASS=0
  RUN_RESOURCE_VIEWS_FORMATS=0
  RUN_RENDER_HEADLESS=0
  RUN_MINI=0
  RUN_WINEMETAL_ABI=0
  RUN_PRESENT_WINDOWED=0
  RUN_FULL_STRESS=0
  RUN_TEXTURE_DIMENSIONS=1
fi

if [[ "$PROFILE" == "metalsharp" ]]; then
  WINE_BIN="${WINE_BIN:-$HOME/.metalsharp/runtime/wine/bin/wine}"
  if [[ "$WINE_BIN" == "wine" && -x "$HOME/.metalsharp/runtime/wine/bin/wine" ]]; then
    WINE_BIN="$HOME/.metalsharp/runtime/wine/bin/wine"
  fi
  WINE_PREFIX="${WINE_PREFIX:-$HOME/.metalsharp/prefix-steam}"
  DXMT_RUNTIME="${DXMT_RUNTIME:-$HOME/.metalsharp/runtime/wine/lib/dxmt}"
fi

if [[ -z "$WINE_PREFIX" ]]; then
  echo "WINEPREFIX is required. Pass --prefix or use --profile metalsharp." >&2
  exit 2
fi

if [[ -z "$DXMT_RUNTIME" ]]; then
  echo "DXMT runtime root is required. Pass --dxmt-runtime or use --profile metalsharp." >&2
  exit 2
fi

WINDOWS_DIR="$DXMT_RUNTIME/x86_64-windows"
UNIX_DIR="$DXMT_RUNTIME/x86_64-unix"
# Wine's builtin-module search expects a route root containing the architecture
# subdirectories, not the x86_64-windows directory itself.
PROBE_WINEDLLPATH="${DXMT_PROBE_WINEDLLPATH:-$DXMT_RUNTIME}"
RUNTIME_LIB_DIR="$(dirname "$DXMT_RUNTIME")"
WINE_RUNTIME_ROOT="$(dirname "$RUNTIME_LIB_DIR")"
WINE_INSTALL_ROOT="$(cd "$(dirname "$WINE_BIN")/.." && pwd)"
# Unix aliases must be registered with the executable's Wine installation,
# not an unrelated sibling directory of an externally staged DXMT route.
WINE_UNIX_DIR="$WINE_INSTALL_ROOT/lib/wine/x86_64-unix"
if [[ -n "$GAME_DIR" ]]; then
  if [[ ! -d "$GAME_DIR" ]]; then
    echo "Game DLL directory does not exist: $GAME_DIR" >&2
    exit 2
  fi
  WINDOWS_DIR="$GAME_DIR"
fi
# Prefer the selected DXMT runtime's Unix half.  The Wine runtime also ships a
# winemetal.so, but loading it first silently pairs source-built PE DLLs with a
# stale Unix call table and corrupts ABI return values (notably SM50Initialize).
# Keep Wine's own Unix loader first.  The selected DXMT Unix half is loaded
# through the unique builtin alias below; putting its directory directly in
# DYLD_LIBRARY_PATH can make Wine select the route's ntdll/winemac loader half
# and fail before the probe starts.
DXMT_DYLD_LIBRARY_PATH="$WINE_INSTALL_ROOT/lib/wine/x86_64-unix:${DYLD_LIBRARY_PATH:-}"
DXMT_WINEMETAL_UNIXLIB_NAME="winemetal.so"
PROBE_WINEMETAL_UNIXLIB_LINK=""
PROBE_D3D12_DLL_PATH=""
PROBE_D3D11_DLL_PATH=""
PROBE_D3D10CORE_DLL_PATH=""
PROBE_DXGI_DLL_PATH=""
cleanup_probe_winemetal_unixlib() {
  if [[ -n "$PROBE_WINEMETAL_UNIXLIB_LINK" ]]; then
    rm -f "$PROBE_WINEMETAL_UNIXLIB_LINK"
  fi
  if [[ -n "$PROBE_D3D12_DLL_PATH" ]]; then
    rm -f "$PROBE_D3D12_DLL_PATH"
  fi
  if [[ -n "$PROBE_D3D11_DLL_PATH" ]]; then
    rm -f "$PROBE_D3D11_DLL_PATH"
  fi
  if [[ -n "$PROBE_D3D10CORE_DLL_PATH" ]]; then
    rm -f "$PROBE_D3D10CORE_DLL_PATH"
  fi
  if [[ -n "$PROBE_DXGI_DLL_PATH" ]]; then
    rm -f "$PROBE_DXGI_DLL_PATH"
  fi
}
trap cleanup_probe_winemetal_unixlib EXIT

# Wine's Unix-library loader resolves module names from its own Unix DLL
# directory before DYLD_LIBRARY_PATH/WINEDLLPATH.  Register the selected DXMT
# runtime under a unique temporary module name so the PE and Unix halves always
# come from the same staged build, then remove the registration on exit.
if [[ -f "$UNIX_DIR/winemetal.so" ]]; then
  mkdir -p "$WINE_UNIX_DIR"
  DXMT_WINEMETAL_UNIXLIB_NAME="winemetal-d3d12-probe-$$-${RANDOM}.so"
  PROBE_WINEMETAL_UNIXLIB_LINK="$WINE_UNIX_DIR/$DXMT_WINEMETAL_UNIXLIB_NAME"
  ln -s "$UNIX_DIR/winemetal.so" "$PROBE_WINEMETAL_UNIXLIB_LINK"
fi
PROBE_EXE="$SDK_DIR/out/bin/probe_loader.exe"
AGILITY_PROBE_EXE="$SDK_DIR/out/bin/probe_agility_ue5.exe"
CAPS_PROBE_EXE="$SDK_DIR/out/bin/probe_device_caps.exe"
LEGACY_REGRESSION_PROBE_EXE="$SDK_DIR/out/bin/probe_legacy_regression.exe"
FEATURE_LEVELS_PROBE_EXE="$SDK_DIR/out/bin/probe_feature_levels.exe"
OBJECT_CONTRACTS_PROBE_EXE="$SDK_DIR/out/bin/probe_object_contracts.exe"
DXGI_PROBE_EXE="$SDK_DIR/out/bin/probe_dxgi_factory.exe"
RESOURCES_PROBE_EXE="$SDK_DIR/out/bin/probe_resources.exe"
QUEUES_PROBE_EXE="$SDK_DIR/out/bin/probe_queues.exe"
DESCRIPTORS_PROBE_EXE="$SDK_DIR/out/bin/probe_descriptors.exe"
SHADERS_PROBE_EXE="$SDK_DIR/out/bin/probe_shaders.exe"
DXIL_SEMANTICS_PROBE_EXE="$SDK_DIR/out/bin/probe_dxil_semantics.exe"
TEXTURE_DIMENSIONS_PROBE_EXE="$SDK_DIR/out/bin/probe_texture_dimensions.exe"
SHADER_CORPUS_PROBE_EXE="$SDK_DIR/out/bin/probe_shader_corpus.exe"
SM66_CAPABILITIES_PROBE_EXE="$SDK_DIR/out/bin/probe_sm66_capabilities.exe"
WRITABLE_MSAA_PROBE_EXE="$SDK_DIR/out/bin/probe_writable_msaa.exe"
VRS_PROBE_EXE="$SDK_DIR/out/bin/probe_vrs.exe"
ROV_PROBE_EXE="$SDK_DIR/out/bin/probe_rov.exe"
BARYCENTRICS_PROBE_EXE="$SDK_DIR/out/bin/probe_barycentrics.exe"
SAMPLER_FEEDBACK_PROBE_EXE="$SDK_DIR/out/bin/probe_sampler_feedback.exe"
SAMPLER_FEEDBACK_PIXEL_PROBE_EXE="$SDK_DIR/out/bin/probe_sampler_feedback_pixel.exe"
WAVE_OPS_PROBE_EXE="$SDK_DIR/out/bin/probe_wave_ops.exe"
REFLECTION_ABI_PROBE_EXE="$SDK_DIR/out/bin/probe_reflection_abi.exe"
GRAPHICS_PSO_PROBE_EXE="$SDK_DIR/out/bin/probe_graphics_pso.exe"
COMPUTE_PSO_PROBE_EXE="$SDK_DIR/out/bin/probe_compute_pso.exe"
COMMAND_REPLAY_PROBE_EXE="$SDK_DIR/out/bin/probe_command_replay.exe"
META_COMMAND_PROBE_EXE="$SDK_DIR/out/bin/probe_meta_command.exe"
VIDEO_PROBE_EXE="$SDK_DIR/out/bin/probe_video.exe"
VIDEO_PROCESS_PROBE_EXE="$SDK_DIR/out/bin/probe_video_process.exe"
INFOQUEUE_PROBE_EXE="$SDK_DIR/out/bin/probe_infoqueue_callback.exe"
DISCARD_TEXTURE_PROBE_EXE="$SDK_DIR/out/bin/probe_discard_texture.exe"
MANUAL_WRITE_TRACKING_PROBE_EXE="$SDK_DIR/out/bin/probe_manual_write_tracking.exe"
SHARING_CONTRACT_PROBE_EXE="$SDK_DIR/out/bin/probe_sharing_contract.exe"
CPU_TEXTURE_MAP_PROBE_EXE="$SDK_DIR/out/bin/probe_cpu_texture_map.exe"
DEBUG_INTERFACES_PROBE_EXE="$SDK_DIR/out/bin/probe_debug_interfaces.exe"
DIAGNOSTICS_PROBE_EXE="$SDK_DIR/out/bin/probe_diagnostics.exe"
WORK_GRAPH_EXECUTION_PROBE_EXE="$SDK_DIR/out/bin/probe_workgraph_execution.exe"
ATTRIBUTE_AT_VERTEX_PROBE_EXE="$SDK_DIR/out/bin/probe_attribute_at_vertex.exe"
CYCLE_COUNTER_PROBE_EXE="$SDK_DIR/out/bin/probe_cycle_counter.exe"
BARRIERS_RENDER_PASS_PROBE_EXE="$SDK_DIR/out/bin/probe_barriers_render_pass.exe"
RESOURCE_VIEWS_FORMATS_PROBE_EXE="$SDK_DIR/out/bin/probe_resource_views_formats.exe"
RENDER_HEADLESS_PROBE_EXE="$SDK_DIR/out/bin/probe_render_headless.exe"
PRESENT_WINDOWED_PROBE_EXE="$SDK_DIR/out/bin/probe_present_windowed.exe"

if [[ ! -x "$WINE_BIN" ]]; then
  echo "Wine binary is not executable: $WINE_BIN" >&2
  exit 2
fi

if [[ ! -d "$WINDOWS_DIR" ]]; then
  echo "Missing DXMT Windows runtime directory: $WINDOWS_DIR" >&2
  exit 2
fi

mkdir -p "$SDK_DIR/out/bin"

for dll in d3d12.dll dxgi.dll dxgi_dxmt.dll d3d11.dll d3d10core.dll winemetal.dll; do
  if [[ ! -f "$WINDOWS_DIR/$dll" ]]; then
    echo "Missing DXMT Windows runtime DLL: $WINDOWS_DIR/$dll" >&2
    exit 2
  fi
  # Wine resolves native PE DLLs from the application directory before
  # WINEDLLPATH. Keep the probe bin dir pinned to the selected runtime so
  # stale local DLLs cannot make the SDK report false results.
  cp "$WINDOWS_DIR/$dll" "$SDK_DIR/out/bin/$dll"
done

# DXMT's d3d12 PE is a Wine builtin module. A same-named app-local copy can
# still resolve the Wine runtime's builtin implementation, so give the
# selected build a unique native module name and let probe_runtime.hpp redirect
# only the probe-side d3d12 LoadLibraryA calls to it.
DXMT_D3D12_DLL_NAME="d3d12-metalsharp-probe-$$-${RANDOM}.dll"
PROBE_D3D12_DLL_PATH="$SDK_DIR/out/bin/$DXMT_D3D12_DLL_NAME"
cp "$WINDOWS_DIR/d3d12.dll" "$PROBE_D3D12_DLL_PATH"
export DXMT_PROBE_D3D12_DLL="$DXMT_D3D12_DLL_NAME"
# Legacy D3D10/D3D11 probes use LoadLibrary-based entrypoint routing.  Keep
# their aliases distinct from Wine's same-named builtin modules as well.
DXMT_D3D11_DLL_NAME="d3d11-metalsharp-probe-$$-${RANDOM}.dll"
PROBE_D3D11_DLL_PATH="$SDK_DIR/out/bin/$DXMT_D3D11_DLL_NAME"
cp "$WINDOWS_DIR/d3d11.dll" "$PROBE_D3D11_DLL_PATH"
export DXMT_PROBE_D3D11_DLL="$DXMT_D3D11_DLL_NAME"
DXMT_D3D10CORE_DLL_NAME="d3d10core-metalsharp-probe-$$-${RANDOM}.dll"
PROBE_D3D10CORE_DLL_PATH="$SDK_DIR/out/bin/$DXMT_D3D10CORE_DLL_NAME"
cp "$WINDOWS_DIR/d3d10core.dll" "$PROBE_D3D10CORE_DLL_PATH"
export DXMT_PROBE_D3D10CORE_DLL="$DXMT_D3D10CORE_DLL_NAME"
DXMT_DXGI_DLL_NAME="dxgi-metalsharp-probe-$$-${RANDOM}.dll"
PROBE_DXGI_DLL_PATH="$SDK_DIR/out/bin/$DXMT_DXGI_DLL_NAME"
# Redirect probe-side LoadLibraryA("dxgi.dll") directly to the implementation
# half.  The same-named dxgi.dll is only a Wine bootstrap; under a renamed
# alias its factory object can be returned with the bootstrap's stale vtable.
# The implementation export is ABI-compatible and avoids that indirection
# while production applications continue to use the normal bootstrap.
cp "$WINDOWS_DIR/dxgi_dxmt.dll" "$PROBE_DXGI_DLL_PATH"
export DXMT_PROBE_DXGI_DLL="$DXMT_DXGI_DLL_NAME"

if [[ ! -f "$UNIX_DIR/winemetal.so" ]]; then
  echo "Missing winemetal.so: $UNIX_DIR/winemetal.so" >&2
  exit 2
fi

for unix_dep in winemac.so ntdll.so; do
  if [[ ! -f "$WINE_UNIX_DIR/$unix_dep" ]]; then
    echo "Missing Wine Unix dependency for winemetal.so: $WINE_UNIX_DIR/$unix_dep" >&2
    exit 2
  fi
done

if [[ "$WINDOWS_DIR" == *"/gptk/"* || "$WINDOWS_DIR" == *"/lib/gptk/"* ]]; then
  echo "DXMT runtime points at GPTK/D3DMetal DLLs, not DXMT: $WINDOWS_DIR" >&2
  exit 2
fi

NEED_BUILD=0
if [[ ! -f "$PROBE_EXE" || ! -f "$AGILITY_PROBE_EXE" || ! -f "$CAPS_PROBE_EXE" || ! -f "$LEGACY_REGRESSION_PROBE_EXE" || ! -f "$FEATURE_LEVELS_PROBE_EXE" || ! -f "$OBJECT_CONTRACTS_PROBE_EXE" || ! -f "$DXGI_PROBE_EXE" || ! -f "$RESOURCES_PROBE_EXE" || ! -f "$QUEUES_PROBE_EXE" || ! -f "$DESCRIPTORS_PROBE_EXE" || ! -f "$SHADERS_PROBE_EXE" || ! -f "$DXIL_SEMANTICS_PROBE_EXE" || ! -f "$TEXTURE_DIMENSIONS_PROBE_EXE" || ! -f "$SHADER_CORPUS_PROBE_EXE" || ! -f "$SM66_CAPABILITIES_PROBE_EXE" || ! -f "$WRITABLE_MSAA_PROBE_EXE" || ! -f "$VRS_PROBE_EXE" || ! -f "$ROV_PROBE_EXE" || ! -f "$BARYCENTRICS_PROBE_EXE" || ! -f "$SAMPLER_FEEDBACK_PROBE_EXE" || ! -f "$SAMPLER_FEEDBACK_PIXEL_PROBE_EXE" || ! -f "$WAVE_OPS_PROBE_EXE" || ! -f "$REFLECTION_ABI_PROBE_EXE" || ! -f "$GRAPHICS_PSO_PROBE_EXE" || ! -f "$COMPUTE_PSO_PROBE_EXE" || ! -f "$COMMAND_REPLAY_PROBE_EXE" || ! -f "$ATTRIBUTE_AT_VERTEX_PROBE_EXE" || ! -f "$CYCLE_COUNTER_PROBE_EXE" || ! -f "$BARRIERS_RENDER_PASS_PROBE_EXE" || ! -f "$RESOURCE_VIEWS_FORMATS_PROBE_EXE" || ! -f "$RENDER_HEADLESS_PROBE_EXE" || ! -f "$PRESENT_WINDOWED_PROBE_EXE" || ! -f "$SDK_DIR/out/bin/D3D12/D3D12Core.dll" || ! -f "$SDK_DIR/out/bin/D3D12/d3d12SDKLayers.dll" || ! -f "$SDK_DIR/out/bin/D3D12/D3D12StateObjectCompiler.dll" || ! -f "$SDK_DIR/out/bin/D3D12/dxil.dll" || ! -f "$SDK_DIR/out/bin/dxc.exe" || ! -f "$SDK_DIR/out/bin/dxcompiler.dll" || ! -f "$SDK_DIR/out/bin/dxil.dll" ]]; then
  NEED_BUILD=1
fi
if [[ ! -f "$SDK_DIR/out/bin/compile-geometry-corpus.exe" ||
      ! -f "$WORK_GRAPH_EXECUTION_PROBE_EXE" ||
      ! -f "$SDK_DIR/out/bin/probe_workgraph_chain.exe" ||
      ! -f "$META_COMMAND_PROBE_EXE" ||
      ! -f "$VIDEO_PROBE_EXE" ||
      ! -f "$VIDEO_PROCESS_PROBE_EXE" ||
      ! -f "$INFOQUEUE_PROBE_EXE" ||
      ! -f "$DISCARD_TEXTURE_PROBE_EXE" ||
      ! -f "$MANUAL_WRITE_TRACKING_PROBE_EXE" ||
      ! -f "$SHARING_CONTRACT_PROBE_EXE" ||
      ! -f "$CPU_TEXTURE_MAP_PROBE_EXE" ||
      ! -f "$DEBUG_INTERFACES_PROBE_EXE" ||
      ! -f "$DIAGNOSTICS_PROBE_EXE" ]]; then
  NEED_BUILD=1
fi

for mini_probe in "${MINI_PROBES[@]}"; do
  if mini_probe_selected "$mini_probe" &&
     [[ ! -f "$SDK_DIR/out/bin/probe_mini_${mini_probe}.exe" ]]; then
    NEED_BUILD=1
  fi
done

if [[ "$NEED_BUILD" == "1" ]]; then
  "$SDK_DIR/scripts/build-probes.sh" >/dev/null
fi

mkdir -p "$RESULTS_DIR"
shader_cache_explicit=0
if [[ -n "$SHADER_CACHE_DIR" ]]; then
  shader_cache_explicit=1
else
  SHADER_CACHE_DIR="$RESULTS_DIR/shader-cache-$PROFILE"
fi
# Warm-up and final runs within this invocation intentionally share a cache,
# but a default profile cache must not inherit entries from an earlier runtime
# or source build. Converter cache keys are not a complete runtime identity,
# so stale entries can make a clean-prefix probe bind the wrong stage ABI.
if [[ "$shader_cache_explicit" == "0" ]]; then
  rm -rf "$SHADER_CACHE_DIR"
fi
mkdir -p "$SHADER_CACHE_DIR"

if [[ -z "${MS_ROOT:-}" ]]; then
  if [[ -f "$WINE_RUNTIME_ROOT/etc/mscompatdb_rules.toml" ]]; then
    export MS_ROOT="$WINE_RUNTIME_ROOT"
  else
    PROBE_MSCOMPATDB_ROOT="$RESULTS_DIR/mscompatdb-probe-root"
    mkdir -p "$PROBE_MSCOMPATDB_ROOT/etc"
    cat > "$PROBE_MSCOMPATDB_ROOT/etc/mscompatdb_rules.toml" <<'EOF_MSCOMPATDB_RULES'
version = 1

[rules.0]
desc = "D3D12 Metal SDK probe no-op rule"

[rules.0.match]
type = "path_suffix"
suffix = '\\__metalsharp_probe_never_matches__.exe'
EOF_MSCOMPATDB_RULES
    export MS_ROOT="$PROBE_MSCOMPATDB_ROOT"
  fi
fi

REAL_WINE_BIN="$WINE_BIN"
PROBE_WINE_WRAPPER="$RESULTS_DIR/wine-probe-wrapper.sh"
cat > "$PROBE_WINE_WRAPPER" <<'EOF_WINE_WRAPPER'
#!/usr/bin/env bash
set -o pipefail
"$D3D12_METAL_SDK_REAL_WINE" "$@" 2> >(grep -v -E '^(mscompatdb:|mscompatdb:error:|mscompatdb:warn:|mscompatdb:trace:)' >&2)
exit $?
EOF_WINE_WRAPPER
chmod +x "$PROBE_WINE_WRAPPER"
export D3D12_METAL_SDK_REAL_WINE="$REAL_WINE_BIN"
WINE_BIN="$PROBE_WINE_WRAPPER"
RESULT_FILE="$RESULTS_DIR/probe-loader-${PROFILE}.json"
AGILITY_RESULT_FILE="$RESULTS_DIR/probe-agility-ue5-${PROFILE}.json"
CAPS_RESULT_FILE="$RESULTS_DIR/probe-device-caps-${PROFILE}.json"
FEATURE_LEVELS_RESULT_FILE="$RESULTS_DIR/probe-feature-levels-${PROFILE}.json"
OBJECT_CONTRACTS_RESULT_FILE="$RESULTS_DIR/probe-object-contracts-${PROFILE}.json"
DXGI_RESULT_FILE="$RESULTS_DIR/probe-dxgi-factory-${PROFILE}.json"
RESOURCES_RESULT_FILE="$RESULTS_DIR/probe-resources-${PROFILE}.json"
QUEUES_RESULT_FILE="$RESULTS_DIR/probe-queues-${PROFILE}.json"
DESCRIPTORS_RESULT_FILE="$RESULTS_DIR/probe-descriptors-${PROFILE}.json"
SHADERS_RESULT_FILE="$RESULTS_DIR/probe-shaders-${PROFILE}.json"
SHADERS_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-shaders-warmup-${PROFILE}.json"
DXIL_SEMANTICS_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-dxil-semantics-warmup-${PROFILE}.json"
DXIL_SEMANTICS_RESULT_FILE="$RESULTS_DIR/probe-dxil-semantics-${PROFILE}.json"
TEXTURE_DIMENSIONS_RESULT_FILE="$RESULTS_DIR/probe-texture-dimensions-${PROFILE}.json"
SHADER_CORPUS_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-shader-corpus-warmup-${PROFILE}.json"
SHADER_CORPUS_RESULT_FILE="$RESULTS_DIR/probe-shader-corpus-${PROFILE}.json"
DXIL_LOWERING_AUDIT_RESULT_FILE="$RESULTS_DIR/dxil-lowering-audit-${PROFILE}.json"
SM5_SM69_OPCODE_MATRIX_RESULT_FILE="$RESULTS_DIR/sm5-sm69-opcode-matrix-${PROFILE}.json"
WORK_GRAPH_EXECUTION_RESULT_FILE="$RESULTS_DIR/probe-workgraph-execution-${PROFILE}.json"
META_COMMAND_RESULT_FILE="$RESULTS_DIR/probe-meta-command-${PROFILE}.json"
VIDEO_RESULT_FILE="$RESULTS_DIR/probe-video-${PROFILE}.json"
VIDEO_PROCESS_RESULT_FILE="$RESULTS_DIR/probe-video-process-${PROFILE}.json"
INFOQUEUE_RESULT_FILE="$RESULTS_DIR/probe-infoqueue-${PROFILE}.json"
DISCARD_TEXTURE_RESULT_FILE="$RESULTS_DIR/probe-discard-texture-${PROFILE}.json"
MANUAL_WRITE_TRACKING_RESULT_FILE="$RESULTS_DIR/probe-manual-write-tracking-${PROFILE}.json"
SHARING_CONTRACT_RESULT_FILE="$RESULTS_DIR/probe-sharing-contract-${PROFILE}.json"
CPU_TEXTURE_MAP_RESULT_FILE="$RESULTS_DIR/probe-cpu-texture-map-${PROFILE}.json"
DEBUG_INTERFACES_RESULT_FILE="$RESULTS_DIR/probe-debug-interfaces-${PROFILE}.json"
DIAGNOSTICS_RESULT_FILE="$RESULTS_DIR/probe-diagnostics-${PROFILE}.json"
SM5_SM69_OPCODE_CONTRACT_RESULT_FILE="$RESULTS_DIR/sm5-sm69-opcode-contract-${PROFILE}.json"
SM66_CAPABILITIES_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-sm66-capabilities-warmup-${PROFILE}.json"
SM66_CAPABILITIES_RESULT_FILE="$RESULTS_DIR/probe-sm66-capabilities-${PROFILE}.json"
WRITABLE_MSAA_RESULT_FILE="$RESULTS_DIR/probe-writable-msaa-${PROFILE}.json"
SAMPLER_FEEDBACK_RESULT_FILE="$RESULTS_DIR/probe-sampler-feedback-${PROFILE}.json"
SAMPLER_FEEDBACK_PIXEL_RESULT_FILE="$RESULTS_DIR/probe-sampler-feedback-pixel-${PROFILE}.json"
WAVE_OPS_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-wave-ops-warmup-${PROFILE}.json"
WAVE_OPS_RESULT_FILE="$RESULTS_DIR/probe-wave-ops-${PROFILE}.json"
REFLECTION_ABI_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-reflection-abi-warmup-${PROFILE}.json"
REFLECTION_ABI_RESULT_FILE="$RESULTS_DIR/probe-reflection-abi-${PROFILE}.json"
GRAPHICS_PSO_RESULT_FILE="$RESULTS_DIR/probe-graphics-pso-${PROFILE}.json"
COMPUTE_PSO_RESULT_FILE="$RESULTS_DIR/probe-compute-pso-${PROFILE}.json"
COMMAND_REPLAY_RESULT_FILE="$RESULTS_DIR/probe-command-replay-${PROFILE}.json"
HITOBJECT_LOCAL_ROOT_RESULT_FILE="$RESULTS_DIR/probe-hitobject-local-root-${PROFILE}.json"
HITOBJECT_INVOKE_RESULT_FILE="$RESULTS_DIR/probe-hitobject-invoke-${PROFILE}.json"
HITOBJECT_ATTRIBUTES_RESULT_FILE="$RESULTS_DIR/probe-hitobject-attributes-${PROFILE}.json"
HITOBJECT_REORDER_RESULT_FILE="$RESULTS_DIR/probe-hitobject-reorder-${PROFILE}.json"
ATTRIBUTE_AT_VERTEX_RESULT_FILE="$RESULTS_DIR/probe-attribute-at-vertex-${PROFILE}.json"
CYCLE_COUNTER_RESULT_FILE="$RESULTS_DIR/probe-cycle-counter-${PROFILE}.json"
ROV_RESULT_FILE="$RESULTS_DIR/probe-rov-${PROFILE}.json"
BARYCENTRICS_RESULT_FILE="$RESULTS_DIR/probe-barycentrics-${PROFILE}.json"
WORK_GRAPH_RESULT_FILE="$RESULTS_DIR/probe-workgraph-${PROFILE}.json"
BARRIERS_RENDER_PASS_RESULT_FILE="$RESULTS_DIR/probe-barriers-render-pass-${PROFILE}.json"
RESOURCE_VIEWS_FORMATS_RESULT_FILE="$RESULTS_DIR/probe-resource-views-formats-${PROFILE}.json"
RENDER_HEADLESS_RESULT_FILE="$RESULTS_DIR/probe-render-headless-${PROFILE}.json"
PRESENT_WINDOWED_RESULT_FILE="$RESULTS_DIR/probe-present-windowed-${PROFILE}.json"
WINEMETAL_ABI_RESULT_FILE="$RESULTS_DIR/winemetal-abi-${PROFILE}.json"
VRS_RESULT_FILE="$RESULTS_DIR/probe-vrs-${PROFILE}.json"
LEGACY_REGRESSION_RESULT_FILE="$RESULTS_DIR/probe-legacy-regression-${PROFILE}.json"

if [[ "$RUN_FL12_2_GATE" == "1" ]]; then
  # The aggregate gate must never combine a newly captured identity with a
  # result left by an earlier partial or different-runtime invocation.  Every
  # dependency is rerun by this mode; deleting its output makes an interrupted
  # or skipped probe a visible missing-evidence failure.
  FL12_2_GATE_RESULT_STEMS=(
    probe-feature-levels
    probe-render-headless
    probe-descriptors
    probe-wave-ops
    probe-sm66-capabilities
    probe-mini-mesh_object_shader_pso
    probe-queues
    probe-command-replay
    probe-resource-views-formats
    probe-resources
    probe-sampler-feedback
    probe-sampler-feedback-pixel
    probe-vrs
    probe-graphics-pso
    probe-mini-dxr_acceleration_structures
    probe-writable-msaa
    winemetal-abi
    probe-legacy-regression
  )
  for stem in "${FL12_2_GATE_RESULT_STEMS[@]}"; do
    rm -f "$RESULTS_DIR/${stem}-${PROFILE}.json"
  done
fi

run_probe_exe() {
  local exe="$1"
  local result_file="$2"
  shift 2
  local -a probe_args=("$@")
  local enable_geometry_mesh="${DXMT_D3D12_ENABLE_GEOMETRY_MESH:-0}"
  local d3d12_trace="${DXMT_D3D12_TRACE:-0}"
  if [[ "$(basename "$exe")" == "probe_mini_subnautica_geometry_dxil_replay.exe" ]]; then
    enable_geometry_mesh=1
  fi
  if [[ "$(basename "$exe")" == "probe_shaders.exe" ]]; then
    d3d12_trace=1
  fi
  (
    cd "$SDK_DIR/out/bin"
    export WINEPREFIX="$WINE_PREFIX"
    export WINEDLLPATH="$PROBE_WINEDLLPATH"
    export WINEDLOVERRIDES="$DLL_OVERRIDES"
    export DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH"
    export DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME"
    export DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR"
    export DXMT_D3D12_ENABLE_GEOMETRY_MESH="$enable_geometry_mesh"
    export DXMT_D3D12_TRACE="$d3d12_trace"
    export D3D12_METAL_SDK_PROFILE="$PROFILE"
    if [[ -n "${GEOMETRY_CORPUS_WIN_PATH:-}" ]]; then
      export D3D12_METAL_SDK_GEOMETRY_CORPUS_DIR="$GEOMETRY_CORPUS_WIN_PATH"
    fi
    export D3D12_METAL_SDK_COMMAND_RAY_CSO="${D3D12_METAL_SDK_COMMAND_RAY_CSO:-}"
    export D3D12_METAL_SDK_COMMAND_RAY_LOCAL_ROOT="${D3D12_METAL_SDK_COMMAND_RAY_LOCAL_ROOT:-}"
    export D3D12_METAL_SDK_COMMAND_RAY_INVOKE="${D3D12_METAL_SDK_COMMAND_RAY_INVOKE:-}"
    export D3D12_METAL_SDK_COMMAND_RAY_ATTRIBUTES="${D3D12_METAL_SDK_COMMAND_RAY_ATTRIBUTES:-}"
    export D3D12_METAL_SDK_COMMAND_RAY_REORDER="${D3D12_METAL_SDK_COMMAND_RAY_REORDER:-}"
    if [[ "${#probe_args[@]}" -gt 0 ]]; then
      "$WINE_BIN" "$exe" "${probe_args[@]}" > "$result_file"
    else
      "$WINE_BIN" "$exe" > "$result_file"
    fi
  )
  echo "$result_file"
}

convert_dxil_shader_cache() {
  local cache_dir="$1"
  local converter="$METAL_SHADER_CONVERTER"
  if [[ -z "$converter" ]]; then
    converter="$(command -v metal-shaderconverter || true)"
  fi
  if [[ -z "$converter" || ! -x "$converter" ]]; then
    echo "metal-shaderconverter not found; DXIL probes may fail until shader cache is prebuilt" >&2
    return 0
  fi

  local dxbc
  shopt -s nullglob
  for dxbc in "$cache_dir"/*.dxbc; do
    local base="${dxbc%.dxbc}"
    local metallib="$base.metallib"
    local reflection="$base.json"
    local msl="$base.msl"
    local layout="$base.vertex-layout.json"
    local fail_marker="$base.msc.fail"
    local dxbc_size
    dxbc_size="$(wc -c < "$dxbc" | tr -d '[:space:]')"
    if [[ -s "$metallib" && -s "$reflection" && ! -s "${base}.msl.err.txt" ]]; then
      continue
    fi
    if [[ "${DXMT_D3D12_PRESERVE_TYPED_MSL_ENTRIES:-}" == *"cs_sample_cmp_level_sm67"* &&
          -s "$msl" && ! -s "${base}.msl.err.txt" &&
          -s "${base}.module.txt" &&
          "$(grep -c '^  name=cs_sample_cmp_level_sm67 ' "$base.module.txt" || true)" -gt 0 ]]; then
      continue
    fi
    # DXMT can leave an internally generated metallib beside a failed MSL
    # source compile.  Replace that failed artifact with MSC output instead of
    # treating the mere presence of the file as a successful conversion.
    rm -f "$metallib" "$reflection" "$fail_marker"
    if [[ "$dxbc_size" -lt 256 ]]; then
      printf 'skipped tiny or intentionally invalid DXIL container: %s bytes\n' "$dxbc_size" > "$fail_marker"
      continue
    fi
    if [[ -s "$layout" ]]; then
      if ! "$converter" -o "$metallib" "$dxbc" \
        --output-reflection-file="$reflection" \
        --deployment-os=macOS \
        --minimum-os-build-version=15.0.0 \
        --vertex-input-layout-file="$layout" >"$fail_marker" 2>&1; then
        continue
      fi
    else
      if ! "$converter" -o "$metallib" "$dxbc" \
        --output-reflection-file="$reflection" \
        --deployment-os=macOS \
        --minimum-os-build-version=15.0.0 >"$fail_marker" 2>&1; then
        continue
      fi
    fi
    rm -f "$fail_marker"
  done
  shopt -u nullglob
}

prepare_native_mesh_converter() {
  local converter="$SDK_DIR/out/bin/compile-mesh-shader"
  local ir_root="${METALSHARP_IRCONVERTER_ROOT:-/usr/local}"
  if [[ -x "$converter" && "${METALSHARP_NATIVE_IRCONVERTER_REBUILD:-0}" != "1" ]]; then
    printf '%s\n' "$converter"
    return 0
  fi
  if [[ ! -f "$ir_root/include/metal_irconverter/metal_irconverter.h" ||
        ! -f "$ir_root/lib/libmetalirconverter.dylib" ]]; then
    echo "native Metal IRConverter mesh provider is unavailable under $ir_root" >&2
    return 1
  fi
  DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 -Wall -Wextra -Werror \
      -I"$ir_root/include" "$SDK_DIR/scripts/compile-mesh-shader.cpp" \
      -L"$ir_root/lib" -Wl,-rpath,"$ir_root/lib" \
      -lmetalirconverter -o "$converter"
  printf '%s\n' "$converter"
}

convert_mesh_shader_cache() {
  local cache_dir="$1"
  if [[ "${METALSHARP_NATIVE_IRCONVERTER:-0}" != "1" ]]; then
    convert_dxil_shader_cache "$cache_dir"
    return 0
  fi
  local converter
  converter="$(prepare_native_mesh_converter)" || return 0
  local ir_root="${METALSHARP_IRCONVERTER_ROOT:-/usr/local}"
  DYLD_LIBRARY_PATH="$ir_root/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
    METAL_SHADER_CONVERTER="$converter" \
    convert_dxil_shader_cache "$cache_dir"
}

prepare_dxil_color_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_dxil_color.hlsl"
  local vs="$SDK_DIR/out/bin/probe_dxil_color_vs.cso"
  local ps="$SDK_DIR/out/bin/probe_dxil_color_ps.cso"

  cat > "$hlsl" <<'HLSL'
struct VSIn {
  float3 pos : POSITION;
  float2 uv : TEXCOORD0;
};

struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D tx : register(t0);
SamplerState smp : register(s0);

VSOut vs_main(VSIn input) {
  VSOut output;
  output.pos = float4(input.pos, 1.0);
  output.uv = input.uv;
  return output;
}

float4 ps_main(VSOut input) : SV_Target0 {
  if (input.pos.x < 32.0) discard;
  return tx.Sample(smp, input.uv);
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 -Fo probe_dxil_color_vs.cso probe_dxil_color.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 -Fo probe_dxil_color_ps.cso probe_dxil_color.hlsl >/dev/null
  )

  mkdir -p "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" probe_mini_dxil_texture_color_output.exe >/dev/null || true
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
}

prepare_mesh_shader_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_mesh_shader.hlsl"

  cat > "$hlsl" <<'HLSL'
struct MeshVertex {
  float4 position : SV_Position;
};

struct MeshPrimitive {
  uint render_target_index : SV_RenderTargetArrayIndex;
};

struct MeshPayload {
  float horizontal_offset;
  uint signature;
  uint render_target_index;
  float depth;
  uint tail0;
  uint tail1;
  uint tail2;
  uint tail3;
  uint tail4;
  uint tail5;
  uint tail6;
  uint tail7;
  uint tail8;
  uint tail9;
  uint tail10;
  uint tail11;
  uint tail12;
  uint tail13;
  uint tail14;
  uint tail15;
  uint tail16;
  uint tail17;
  uint tail18;
  uint tail19;
  uint tail20;
  uint tail21;
  uint tail22;
  uint tail23;
  uint tail24;
  uint tail25;
  uint tail26;
  uint tail27;
  uint tail28;
  uint tail29;
  uint tail30;
  uint tail31;
  uint tail32;
  uint tail33;
  uint tail34;
  uint tail35;
  uint tail36;
  uint tail37;
  uint tail38;
  uint tail39;
  uint tail40;
  uint tail41;
  uint tail42;
  uint tail43;
  uint tail44;
  uint tail45;
  uint tail46;
  uint tail47;
  uint tail48;
  uint tail49;
  uint tail50;
  uint tail51;
  uint tail52;
  uint tail53;
  uint tail54;
  uint tail55;
  uint tail56;
  uint tail57;
  uint tail58;
  uint tail59;
};

groupshared MeshPayload payload;

cbuffer MeshConstants : register(b0) {
  float mesh_scale;
};

cbuffer AmplificationConstants : register(b1) {
  uint amplification_enabled;
};

ByteAddressBuffer mesh_control : register(t0);
ByteAddressBuffer amplification_control : register(t1);
Texture2D<float> mesh_texture : register(t1);
SamplerState mesh_sampler : register(s0);
RWByteAddressBuffer mesh_output : register(u0);

[numthreads(1, 1, 1)]
void as_main(uint3 group_id : SV_GroupID) {
  payload.horizontal_offset = (group_id.x & 1) ? 0.05 : 0.0;
  payload.signature = 0x4153504c;
  payload.render_target_index = group_id.x & 1;
  payload.depth = (group_id.x & 1) ? 0.75 : 0.25;
  payload.tail0 = 0x50415930;
  payload.tail1 = 0x50415931;
  payload.tail2 = 0x50415932;
  payload.tail3 = 0x50415933;
  payload.tail4 = 0x50415934;
  payload.tail5 = 0x50415935;
  payload.tail6 = 0x50415936;
  payload.tail7 = 0x50415937;
  payload.tail8 = 0x50415938;
  payload.tail9 = 0x50415939;
  payload.tail10 = 0x5041593a;
  payload.tail11 = 0x5041593b;
  payload.tail12 = 0x5041593c;
  payload.tail13 = 0x5041593d;
  payload.tail14 = 0x5041593e;
  payload.tail15 = 0x5041593f;
  payload.tail16 = 0x50415940;
  payload.tail17 = 0x50415941;
  payload.tail18 = 0x50415942;
  payload.tail19 = 0x50415943;
  payload.tail20 = 0x50415944;
  payload.tail21 = 0x50415945;
  payload.tail22 = 0x50415946;
  payload.tail23 = 0x50415947;
  payload.tail24 = 0x50415948;
  payload.tail25 = 0x50415949;
  payload.tail26 = 0x5041594a;
  payload.tail27 = 0x5041594b;
  payload.tail28 = 0x5041594c;
  payload.tail29 = 0x5041594d;
  payload.tail30 = 0x5041594e;
  payload.tail31 = 0x5041594f;
  payload.tail32 = 0x50415950;
  payload.tail33 = 0x50415951;
  payload.tail34 = 0x50415952;
  payload.tail35 = 0x50415953;
  payload.tail36 = 0x50415954;
  payload.tail37 = 0x50415955;
  payload.tail38 = 0x50415956;
  payload.tail39 = 0x50415957;
  payload.tail40 = 0x50415958;
  payload.tail41 = 0x50415959;
  payload.tail42 = 0x5041595a;
  payload.tail43 = 0x5041595b;
  payload.tail44 = 0x5041595c;
  payload.tail45 = 0x5041595d;
  payload.tail46 = 0x5041595e;
  payload.tail47 = 0x5041595f;
  payload.tail48 = 0x50415960;
  payload.tail49 = 0x50415961;
  payload.tail50 = 0x50415962;
  payload.tail51 = 0x50415963;
  payload.tail52 = 0x50415964;
  payload.tail53 = 0x50415965;
  payload.tail54 = 0x50415966;
  payload.tail55 = 0x50415967;
  payload.tail56 = 0x50415968;
  payload.tail57 = 0x50415969;
  payload.tail58 = 0x5041596a;
  payload.tail59 = 0x5041596b;
  DispatchMesh(amplification_enabled * amplification_control.Load(0),
               1, 1, payload);
}

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void ms_main(in payload MeshPayload payload,
             out vertices MeshVertex vertices[3],
             out primitives MeshPrimitive primitives[1],
             out indices uint3 triangles[1],
             uint group_thread_id : SV_GroupIndex) {
  SetMeshOutputCounts(3, 1);
  float texture_control = mesh_texture.SampleLevel(mesh_sampler, float2(0.5, 0.5), 0.0);
  float resolved_scale = mesh_scale * asfloat(mesh_control.Load(0)) * texture_control;
  mesh_output.Store(0, 0x4d534831);
  mesh_output.Store(8 + group_thread_id * 4,
                    payload.signature + group_thread_id);
  if (group_thread_id < 60) {
    uint payload_tail = group_thread_id == 0 ? payload.tail0 :
                        group_thread_id == 1 ? payload.tail1 :
                        group_thread_id == 2 ? payload.tail2 :
                        group_thread_id == 3 ? payload.tail3 :
                        group_thread_id == 4 ? payload.tail4 :
                        group_thread_id == 5 ? payload.tail5 :
                        group_thread_id == 6 ? payload.tail6 :
                        group_thread_id == 7 ? payload.tail7 :
                        group_thread_id == 8 ? payload.tail8 :
                        group_thread_id == 9 ? payload.tail9 :
                        group_thread_id == 10 ? payload.tail10 :
                        group_thread_id == 11 ? payload.tail11 :
                        group_thread_id == 12 ? payload.tail12 :
                        group_thread_id == 13 ? payload.tail13 :
                        group_thread_id == 14 ? payload.tail14 :
                        group_thread_id == 15 ? payload.tail15 :
                        group_thread_id == 16 ? payload.tail16 :
                        group_thread_id == 17 ? payload.tail17 :
                        group_thread_id == 18 ? payload.tail18 :
                        group_thread_id == 19 ? payload.tail19 :
                        group_thread_id == 20 ? payload.tail20 :
                        group_thread_id == 21 ? payload.tail21 :
                        group_thread_id == 22 ? payload.tail22 :
                        group_thread_id == 23 ? payload.tail23 :
                        group_thread_id == 24 ? payload.tail24 :
                        group_thread_id == 25 ? payload.tail25 :
                        group_thread_id == 26 ? payload.tail26 :
                        group_thread_id == 27 ? payload.tail27 :
                        group_thread_id == 28 ? payload.tail28 :
                        group_thread_id == 29 ? payload.tail29 :
                        group_thread_id == 30 ? payload.tail30 :
                        group_thread_id == 31 ? payload.tail31 :
                        group_thread_id == 32 ? payload.tail32 :
                        group_thread_id == 33 ? payload.tail33 :
                        group_thread_id == 34 ? payload.tail34 :
                        group_thread_id == 35 ? payload.tail35 :
                        group_thread_id == 36 ? payload.tail36 :
                        group_thread_id == 37 ? payload.tail37 :
                        group_thread_id == 38 ? payload.tail38 :
                        group_thread_id == 39 ? payload.tail39 :
                        group_thread_id == 40 ? payload.tail40 :
                        group_thread_id == 41 ? payload.tail41 :
                        group_thread_id == 42 ? payload.tail42 :
                        group_thread_id == 43 ? payload.tail43 :
                        group_thread_id == 44 ? payload.tail44 :
                        group_thread_id == 45 ? payload.tail45 :
                        group_thread_id == 46 ? payload.tail46 :
                        group_thread_id == 47 ? payload.tail47 :
                        group_thread_id == 48 ? payload.tail48 :
                        group_thread_id == 49 ? payload.tail49 :
                        group_thread_id == 50 ? payload.tail50 :
                        group_thread_id == 51 ? payload.tail51 :
                        group_thread_id == 52 ? payload.tail52 :
                        group_thread_id == 53 ? payload.tail53 :
                        group_thread_id == 54 ? payload.tail54 :
                        group_thread_id == 55 ? payload.tail55 :
                        group_thread_id == 56 ? payload.tail56 :
                        group_thread_id == 57 ? payload.tail57 :
                        group_thread_id == 58 ? payload.tail58 : payload.tail59;
    mesh_output.Store(264 + group_thread_id * 4, payload_tail);
  }
  if (group_thread_id == 0) {
    vertices[0].position = float4((-0.8 + payload.horizontal_offset) * resolved_scale, -0.8 * resolved_scale, payload.depth, 1.0);
    primitives[0].render_target_index = payload.render_target_index;
    triangles[0] = uint3(0, 1, 2);
  } else if (group_thread_id == 1) {
    vertices[1].position = float4(( 0.0 + payload.horizontal_offset) * resolved_scale,  0.8 * resolved_scale, payload.depth, 1.0);
  } else if (group_thread_id == 2) {
    vertices[2].position = float4(( 0.8 + payload.horizontal_offset) * resolved_scale, -0.8 * resolved_scale, payload.depth, 1.0);
  }
}

float4 ps_main() : SV_Target0 {
  return float4(0.25, 0.5, 0.75, 0.5);
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E as_main -T as_6_5 -Fo probe_mesh_shader_as.cso probe_mesh_shader.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ms_main -T ms_6_5 -Fo probe_mesh_shader_ms.cso probe_mesh_shader.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 -Fo probe_mesh_shader_ps.cso probe_mesh_shader.hlsl >/dev/null
  )

  mkdir -p "$SHADER_CACHE_DIR"
  # Each failed stage emits its DXIL blob before PSO construction stops. Walk
  # AS -> MS -> PS in bounded warm-up passes, converting newly emitted blobs
  # after each pass, so the final mini gate executes the complete pipeline.
  for _mesh_warmup_pass in 1 2 3; do
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      "$WINE_BIN" probe_mini_mesh_object_shader_pso.exe >/dev/null || true
    )
    convert_mesh_shader_cache "$SHADER_CACHE_DIR"
  done
}

prepare_geometry_replay_corpus() {
  local corpus="$SHADER_CACHE_DIR/geometry-corpus"
  mkdir -p "$corpus"
  (
    cd "$SDK_DIR/out/bin"
    env -u WINEDLLPATH -u DYLD_LIBRARY_PATH -u DXMT_WINEMETAL_UNIXLIB \
      WINEDEBUG=-all WINEPREFIX="$WINE_PREFIX" \
      WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" compile-geometry-corpus.exe >/dev/null
  )
  cp "$SDK_DIR/out/bin/phase6_geometry_attribute_vs.dxbc" \
    "$corpus/414b1f3b4509d720.geom.gsvs.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_attribute_gs.dxbc" \
    "$corpus/414b1f3b4509d720.geom.gsmesh.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_attribute_vs.dxbc" \
    "$corpus/8b12f030dd908c1b.geom.gsvs.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_attribute_gs.dxbc" \
    "$corpus/8b12f030dd908c1b.geom.gsmesh.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_float2_vs.dxbc" \
    "$corpus/8c4a1c6f7f8e81fc.geom.gsvs.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_float2_gs.dxbc" \
    "$corpus/8c4a1c6f7f8e81fc.geom.gsmesh.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_float2_vs.dxbc" \
    "$corpus/a0df6264a1b2037c.geom.gsvs.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_float2_gs.dxbc" \
    "$corpus/a0df6264a1b2037c.geom.gsmesh.dxbc"
  cp "$SDK_DIR/out/bin/phase6_geometry_pixel.dxbc" \
    "$corpus/phase6_geometry_pixel.dxbc"
  GEOMETRY_CORPUS_WIN_PATH="Z:${corpus//\//\\}"
}

prepare_conservative_raster_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_conservative_raster.hlsl"
  cat > "$hlsl" <<'HLSL'
struct VSIn { float3 position : POSITION; };
struct VSOut { float4 position : SV_Position; };
VSOut vs_main(VSIn input) {
  VSOut output;
  output.position = float4(input.position, 1.0);
  return output;
}
float4 ps_main() : SV_Target0 {
  return float4(1.0, 0.0, 0.0, 1.0);
}
HLSL
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    "$WINE_BIN" dxc.exe -nologo -T vs_6_0 -E vs_main -HV 2021 \
      -Fo probe_conservative_raster_vs.cso probe_conservative_raster.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    "$WINE_BIN" dxc.exe -nologo -T ps_6_0 -E ps_main -HV 2021 \
      -Fo probe_conservative_raster_ps.cso probe_conservative_raster.hlsl >/dev/null
  )
}

prepare_start_draw_info_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_start_draw_info.hlsl"

  cat > "$hlsl" <<'HLSL'
struct VSOut {
  float4 position : SV_Position;
  float4 color : COLOR0;
};

VSOut vs_main(uint vertex_id : SV_VertexID,
             int start_vertex : SV_StartVertexLocation,
             uint start_instance : SV_StartInstanceLocation,
             uint view_id : SV_ViewID) {
  float2 position = vertex_id == 4 ? float2(-0.8, -0.8) :
                    vertex_id == 5 ? float2(0.0, 0.8) :
                                      float2(0.8, -0.8);
  VSOut output;
  output.position = float4(position, 0.0, 1.0);
  output.color = float4(start_vertex == 4 && view_id == 0 ? 1.0 : 0.0,
                        start_instance == 7 ? 0.5 : 0.0,
                        0.25, 1.0);
  return output;
}

float4 ps_main(VSOut input) : SV_Target {
  return input.color;
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_8 \
      -Fo probe_start_draw_vs.cso probe_start_draw_info.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_start_draw_ps.cso probe_start_draw_info.hlsl >/dev/null
  )
}

prepare_inner_coverage_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_inner_coverage.hlsl"

  cat > "$hlsl" <<'HLSL'
struct VSOut {
  float4 position : SV_Position;
};

VSOut vs_main(float3 position : POSITION) {
  VSOut output;
  output.position = float4(position, 1.0);
  return output;
}

float4 ps_main(float4 position : SV_Position,
               uint inner_coverage : SV_InnerCoverage) : SV_Target {
  return inner_coverage != 0 ? float4(1.0, 1.0, 1.0, 1.0)
                              : float4(0.0, 0.0, 0.0, 1.0);
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 \
      -Fo probe_inner_coverage_vs.cso probe_inner_coverage.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_inner_coverage_ps.cso probe_inner_coverage.hlsl >/dev/null
  )
}

prepare_view_id_instancing_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_view_id_instancing.hlsl"

  cat > "$hlsl" <<'HLSL'
struct VSOut {
  float4 position : SV_Position;
  float4 color : COLOR0;
};

VSOut vs_main(uint vertex_id : SV_VertexID, uint view_id : SV_ViewID) {
  float2 position = vertex_id == 0 ? float2(-1.0, -1.0) :
                    vertex_id == 1 ? float2(3.0, -1.0) :
                                      float2(-1.0, 3.0);
  VSOut output;
  output.position = float4(position, 0.0, 1.0);
  output.color = view_id == 0 ? float4(1.0, 0.0, 0.0, 1.0)
                              : float4(0.0, 1.0, 0.0, 1.0);
  return output;
}

float4 ps_main(VSOut input) : SV_Target {
  return input.color;
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_8 \
      -Fo probe_view_id_instancing_vs.cso probe_view_id_instancing.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_view_id_instancing_ps.cso probe_view_id_instancing.hlsl >/dev/null
  )
}

prepare_temp_register_probe() {
  local source="$SDK_DIR/probes/probe_temp_registers.ll"
  local raw="$SDK_DIR/out/bin/probe_temp_registers.bc"
  local output="$SDK_DIR/out/bin/probe_temp_registers.dxil"
  local llvm_root="${METALSHARP_X86_LLVM_ROOT:-/Volumes/AverySSD/toolchains}/clang+llvm-15.0.7-x86_64-apple-darwin21.0"
  local llvm_as="$llvm_root/bin/llvm-as"
  if [[ ! -x "$llvm_as" ]]; then
    echo "missing pinned llvm-as for temporary-register probe: $llvm_as" >&2
    return 1
  fi
  "$llvm_as" "$source" -o "$raw"
  python3 - "$raw" "$output" <<'PY'
import struct
import sys

raw_path, output_path = sys.argv[1:]
bitcode = open(raw_path, "rb").read()
# DXMT's DXIL-part parser expects a DXIL program header followed by the
# bitcode at offset 24.  The outer DXBC wrapper makes the generated module a
# normal D3D12 shader bytecode blob rather than a test-only raw bitcode file.
program_version = (5 << 16) | (6 << 4)  # compute, SM 6.0
program = struct.pack(
    "<II4sHHII", program_version, 24 + len(bitcode), b"DXIL", 0, 1,
    16, len(bitcode)
) + bitcode
chunk = struct.pack("<4sI", b"DXIL", len(program)) + program
part_offset = 36
container_size = part_offset + len(chunk)
container = (
    b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, container_size, 1) +
    struct.pack("<I", part_offset) + chunk
)
open(output_path, "wb").write(container)
PY
}

materialize_native_dxr_cache() {
  local cache_dir="$1"
  local raygen_cso="$2"
  local raygen_root="$3"
  local closest_hit_local_root="$4"
  local procedural_compiler="$5"
  local ir_root="${METALSHARP_IRCONVERTER_ROOT:-/usr/local}"
  local dxbc
  shopt -s nullglob
  for dxbc in "$cache_dir"/*.dxbc; do
    if ! cmp -s "$dxbc" "$raygen_cso"; then
      continue
    fi
    local base="${dxbc%.dxbc}"
    if DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" raygen "$base.metallib" \
      "$closest_hit_local_root" \
      >"$base.raygen-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" @ray-dispatch \
      "$base.raydispatch.metallib" "$closest_hit_local_root" \
      >"$base.raydispatch-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" miss_shader "$base.miss.metallib" \
      "$closest_hit_local_root" \
      >"$base.miss-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" closest_hit "$base.closesthit.metallib" \
      "$closest_hit_local_root" \
      >"$base.closesthit-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" callable_shader "$base.callable.metallib" \
      "$closest_hit_local_root" \
      >"$base.callable-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" any_hit "$base.anyhit.metallib" \
      "$closest_hit_local_root" \
      >"$base.anyhit-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" @triangle-wrapper \
      "$base.rayintersection.metallib" "$closest_hit_local_root" \
      >"$base.rayintersection-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" procedural_intersection \
      "$base.proceduralintersection.metallib" "$closest_hit_local_root" \
      >"$base.proceduralintersection-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" procedural_closest_hit \
      "$base.proceduralclosesthit.metallib" "$closest_hit_local_root" \
      >"$base.proceduralclosesthit-msc.log" 2>&1 &&
      DYLD_LIBRARY_PATH="$ir_root/lib" "$procedural_compiler" \
      "$dxbc" "$raygen_root" @procedural-wrapper \
      "$base.proceduralwrapper.metallib" "$closest_hit_local_root" \
      >"$base.proceduralwrapper-msc.log" 2>&1; then
      rm -f "$base.msc.fail"
    else
      : >"$base.msc.fail"
    fi
  done
  shopt -u nullglob
}

prepare_dxr_acceleration_structure_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_dxr_inline.hlsl"
  local raygen_hlsl="$SDK_DIR/out/bin/probe_dxr_raygen.hlsl"
  local raygen_cso="$SDK_DIR/out/bin/probe_dxr_raygen.cso"
  local raygen_root="$SDK_DIR/out/bin/probe_dxr_raygen_root.json"
  local closest_hit_local_root="$SDK_DIR/out/bin/probe_dxr_closest_hit_local_root.json"
  local procedural_compiler="$SDK_DIR/out/bin/compile-procedural-raytracing"

  cat > "$hlsl" <<'HLSL'
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

[numthreads(1, 1, 1)]
void cs_main() {
  RayQuery<RAY_FLAG_NONE> query;
  RayDesc ray;
  ray.Origin = float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
      query.CommitNonOpaqueTriangleHit();
  }
  output.Store(0, query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1 : 0);
}
HLSL

  local invalid_hlsl="$SDK_DIR/out/bin/probe_dxr_inline_invalid.hlsl"
  cat > "$invalid_hlsl" <<'HLSL'
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

[numthreads(1, 1, 1)]
void cs_main() {
  RayQuery<RAY_FLAG_NONE> query;
  RayDesc ray;
  ray.Origin = float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  query.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0x01, ray);
  output.Store(0, query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1 : 0);
}
HLSL

  local accessor_hlsl="$SDK_DIR/out/bin/probe_dxr_inline_accessors.hlsl"
  cat > "$accessor_hlsl" <<'HLSL'
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

[numthreads(1, 1, 1)]
void cs_main() {
  RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> query;
  RayDesc ray;
  ray.Origin = float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  query.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      output.Store(4, query.CandidateInstanceIndex());
      output.Store(8, query.CandidateInstanceID());
      output.Store(12, query.CandidateGeometryIndex());
      output.Store(16, query.CandidatePrimitiveIndex());
      output.Store(20, query.CandidateTriangleFrontFace() ? 1 : 0);
      output.Store(24, asuint(query.CandidateTriangleRayT()));
      float2 bary = query.CandidateTriangleBarycentrics();
      output.Store(28, asuint(bary.x));
      output.Store(32, asuint(bary.y));
      float3 origin = query.CandidateObjectRayOrigin();
      output.Store(36, asuint(origin.x));
      output.Store(40, asuint(origin.y));
      output.Store(44, asuint(origin.z));
      float3 direction = query.CandidateObjectRayDirection();
      output.Store(48, asuint(direction.x));
      output.Store(52, asuint(direction.y));
      output.Store(56, asuint(direction.z));
      output.Store(152, query.CandidateProceduralPrimitiveNonOpaque() ? 1 : 0);
      float3x4 candidate_object_to_world = query.CandidateObjectToWorld3x4();
      output.Store(160, asuint(candidate_object_to_world[0][0]));
      output.Store(164, asuint(candidate_object_to_world[0][1]));
      output.Store(168, asuint(candidate_object_to_world[0][2]));
      output.Store(172, asuint(candidate_object_to_world[0][3]));
      output.Store(176, asuint(candidate_object_to_world[1][0]));
      output.Store(180, asuint(candidate_object_to_world[1][1]));
      output.Store(184, asuint(candidate_object_to_world[1][2]));
      output.Store(188, asuint(candidate_object_to_world[1][3]));
      output.Store(192, asuint(candidate_object_to_world[2][0]));
      output.Store(196, asuint(candidate_object_to_world[2][1]));
      output.Store(200, asuint(candidate_object_to_world[2][2]));
      output.Store(204, asuint(candidate_object_to_world[2][3]));
      float3x4 candidate_world_to_object = query.CandidateWorldToObject3x4();
      output.Store(208, asuint(candidate_world_to_object[0][0]));
      output.Store(212, asuint(candidate_world_to_object[0][1]));
      output.Store(216, asuint(candidate_world_to_object[0][2]));
      output.Store(220, asuint(candidate_world_to_object[0][3]));
      output.Store(224, asuint(candidate_world_to_object[1][0]));
      output.Store(228, asuint(candidate_world_to_object[1][1]));
      output.Store(232, asuint(candidate_world_to_object[1][2]));
      output.Store(236, asuint(candidate_world_to_object[1][3]));
      output.Store(240, asuint(candidate_world_to_object[2][0]));
      output.Store(244, asuint(candidate_world_to_object[2][1]));
      output.Store(248, asuint(candidate_world_to_object[2][2]));
      output.Store(252, asuint(candidate_world_to_object[2][3]));
      output.Store(352, query.CandidateInstanceContributionToHitGroupIndex());
      query.CommitNonOpaqueTriangleHit();
    }
  }
  output.Store(60, query.CommittedStatus());
  output.Store(64, query.CommittedInstanceIndex());
  output.Store(68, query.CommittedInstanceID());
  output.Store(72, query.CommittedGeometryIndex());
  output.Store(76, query.CommittedPrimitiveIndex());
  output.Store(80, query.CommittedTriangleFrontFace() ? 1 : 0);
  output.Store(84, asuint(query.CommittedRayT()));
  float2 committed_bary = query.CommittedTriangleBarycentrics();
  output.Store(88, asuint(committed_bary.x));
  output.Store(92, asuint(committed_bary.y));
  output.Store(96, query.RayFlags());
  output.Store(100, asuint(query.RayTMin()));
  float3 world_origin = query.WorldRayOrigin();
  output.Store(104, asuint(world_origin.x));
  output.Store(108, asuint(world_origin.y));
  output.Store(112, asuint(world_origin.z));
  float3 world_direction = query.WorldRayDirection();
  output.Store(116, asuint(world_direction.x));
  output.Store(120, asuint(world_direction.y));
  output.Store(124, asuint(world_direction.z));
  float3 committed_origin = query.CommittedObjectRayOrigin();
  output.Store(128, asuint(committed_origin.x));
  output.Store(132, asuint(committed_origin.y));
  output.Store(136, asuint(committed_origin.z));
  float3 committed_direction = query.CommittedObjectRayDirection();
  output.Store(140, asuint(committed_direction.x));
  output.Store(144, asuint(committed_direction.y));
  output.Store(148, asuint(committed_direction.z));
  float3x4 committed_object_to_world = query.CommittedObjectToWorld3x4();
  output.Store(256, asuint(committed_object_to_world[0][0]));
  output.Store(260, asuint(committed_object_to_world[0][1]));
  output.Store(264, asuint(committed_object_to_world[0][2]));
  output.Store(268, asuint(committed_object_to_world[0][3]));
  output.Store(272, asuint(committed_object_to_world[1][0]));
  output.Store(276, asuint(committed_object_to_world[1][1]));
  output.Store(280, asuint(committed_object_to_world[1][2]));
  output.Store(284, asuint(committed_object_to_world[1][3]));
  output.Store(288, asuint(committed_object_to_world[2][0]));
  output.Store(292, asuint(committed_object_to_world[2][1]));
  output.Store(296, asuint(committed_object_to_world[2][2]));
  output.Store(300, asuint(committed_object_to_world[2][3]));
  float3x4 committed_world_to_object = query.CommittedWorldToObject3x4();
  output.Store(304, asuint(committed_world_to_object[0][0]));
  output.Store(308, asuint(committed_world_to_object[0][1]));
  output.Store(312, asuint(committed_world_to_object[0][2]));
  output.Store(316, asuint(committed_world_to_object[0][3]));
  output.Store(320, asuint(committed_world_to_object[1][0]));
  output.Store(324, asuint(committed_world_to_object[1][1]));
  output.Store(328, asuint(committed_world_to_object[1][2]));
  output.Store(332, asuint(committed_world_to_object[1][3]));
  output.Store(336, asuint(committed_world_to_object[2][0]));
  output.Store(340, asuint(committed_world_to_object[2][1]));
  output.Store(344, asuint(committed_world_to_object[2][2]));
  output.Store(348, asuint(committed_world_to_object[2][3]));
  output.Store(356, query.CommittedInstanceContributionToHitGroupIndex());
  RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> aborted;
  aborted.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
  while (aborted.Proceed()) {
    aborted.Abort();
  }
  output.Store(156, aborted.CommittedStatus());
  RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> procedural_query;
  procedural_query.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
  uint procedural_candidate = 0;
  while (procedural_query.Proceed()) {
    procedural_candidate = procedural_query.CandidateProceduralPrimitiveNonOpaque() ? 1 : 0;
    if (procedural_candidate != 0)
      procedural_query.CommitProceduralPrimitiveHit(2.0);
  }
  output.Store(360, procedural_candidate);
  output.Store(364, procedural_query.CommittedInstanceID());
  output.Store(368, procedural_query.CommittedStatus());
  output.Store(372, procedural_query.CommittedInstanceIndex());
  output.Store(376, asuint(procedural_query.CommittedRayT()));
  output.Store(380, 0xd3d12000);
  output.Store(0, query.CommittedStatus());
}
HLSL

  cat > "$raygen_root" <<'JSON'
{
  "RootSignature": {
    "Flags": "IRRootSignatureFlagNone",
    "NumParameters": 1,
    "NumStaticSamplers": 0,
    "Parameters": [{
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 0,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeSRV",
          "RegisterSpace": 0
        }, {
          "BaseShaderRegister": 0,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 1,
          "RangeType": "IRDescriptorRangeTypeUAV",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 2
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }],
    "StaticSamplers": []
  },
  "version": "IRRootSignatureVersion_1_1"
}
JSON

  cat > "$closest_hit_local_root" <<'JSON'
{
  "RootSignature": {
    "Flags": "IRRootSignatureFlagLocalRootSignature",
    "NumParameters": 4,
    "NumStaticSamplers": 1,
    "Parameters": [{
      "Constants": {
        "Num32BitValues": 1,
        "ShaderRegister": 1,
        "RegisterSpace": 0
      },
      "ParameterType": "IRRootParameterType32BitConstants",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }, {
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 1,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeSRV",
          "RegisterSpace": 0
        }, {
          "BaseShaderRegister": 2,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 1,
          "RangeType": "IRDescriptorRangeTypeSRV",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 2
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }, {
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 1,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeUAV",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 1
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }, {
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 2,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeCBV",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 1
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }, {
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 0,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeSampler",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 1
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }],
    "StaticSamplers": [{
      "AddressU": "IRTextureAddressModeClamp",
      "AddressV": "IRTextureAddressModeClamp",
      "AddressW": "IRTextureAddressModeClamp",
      "BorderColor": "IRStaticBorderColorOpaqueBlack",
      "ComparisonFunc": "IRComparisonFunctionAlways",
      "Filter": "IRFilterMinMagMipPoint",
      "MaxAnisotropy": 1,
      "MaxLOD": 3.4028234663852886e+38,
      "MinLOD": 0,
      "MipLODBias": 0,
      "RegisterSpace": 0,
      "ShaderRegister": 1,
      "ShaderVisibility": "IRShaderVisibilityAll"
    }]
  },
  "version": "IRRootSignatureVersion_1_1"
}
JSON

  cat > "$raygen_hlsl" <<'HLSL'
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer ClosestHitLocalRoot : register(b1) {
  uint closest_hit_local_marker;
};
ByteAddressBuffer closest_hit_local_buffer : register(t1);
RWByteAddressBuffer closest_hit_local_output : register(u1);
cbuffer ClosestHitLocalCBV : register(b2) {
  uint closest_hit_local_cbv_marker;
};
Texture2D<float4> closest_hit_local_texture : register(t2);
SamplerState closest_hit_local_sampler : register(s0);
SamplerState closest_hit_local_static_sampler : register(s1);

struct MissPayload {
  uint value;
};

struct CallablePayload {
  uint value;
};

[shader("raygeneration")]
void raygen() {
  uint ray_index = DispatchRaysIndex().x;
  uint3 dispatch_dimensions = DispatchRaysDimensions();
  if (ray_index == 0) {
    output.Store(32, dispatch_dimensions.x);
    output.Store(36, dispatch_dimensions.y);
    output.Store(40, dispatch_dimensions.z);
  }
  RayDesc ray;
  ray.Origin = ray_index == 2 ? float3(2.0, 0.0, -2.0)
                              : ray_index == 3 ? float3(4.6, -0.6, -2.0)
                              : ray_index == 4 ? float3(4.0, 0.0, -2.0)
                                               : float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = ray_index == 0 ? float3(0.0, 1.0, 0.0)
                                 : float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  MissPayload payload;
  payload.value = ray_index == 3 ? 0xffffffff
                                : ray_index == 4 ? 0xfffffffe : 0;
  TraceRay(scene, RAY_FLAG_NONE, 0x02, 0, 0,
           ray_index == 0 ? 1 : 0, ray, payload);
  output.Store(ray_index < 3 ? 4 + ray_index * 4
                              : ray_index == 3 ? 24 : 28,
               payload.value);
  if (ray_index == 0) {
    CallablePayload callable_payload;
    callable_payload.value = 0;
    CallShader(1, callable_payload);
    output.Store(16, callable_payload.value);
    output.Store(20, 42);
  }
}

[shader("miss")]
void miss_shader(inout MissPayload payload) {
  payload.value = closest_hit_local_marker == 0x4c4f434c
                      ? 0x4d495353
                      : 0x4d495346;
}

[shader("closesthit")]
void closest_hit(inout MissPayload payload,
                 BuiltInTriangleIntersectionAttributes attributes) {
  bool any_hit_ran = payload.value == 0x414e5948;
  bool accepted_hit = payload.value == 0xfffffffe;
  uint ray_index = DispatchRaysIndex().x;
  if (ray_index == 1) {
    output.Store(44, InstanceID());
    output.Store(48, InstanceIndex());
    output.Store(52, HitKind());
    output.Store(56, RayFlags());
    output.Store(60, PrimitiveIndex());
    output.Store(64, GeometryIndex());
    output.Store(68, asuint(RayTMin()));
    output.Store(72, asuint(RayTCurrent()));
    float3 world_origin = WorldRayOrigin();
    output.Store(76, asuint(world_origin.x));
    output.Store(80, asuint(world_origin.y));
    output.Store(84, asuint(world_origin.z));
    float3 world_direction = WorldRayDirection();
    output.Store(88, asuint(world_direction.x));
    output.Store(92, asuint(world_direction.y));
    output.Store(96, asuint(world_direction.z));
    float3 object_origin = ObjectRayOrigin();
    output.Store(100, asuint(object_origin.x));
    output.Store(104, asuint(object_origin.y));
    output.Store(108, asuint(object_origin.z));
    float3 object_direction = ObjectRayDirection();
    output.Store(112, asuint(object_direction.x));
    output.Store(116, asuint(object_direction.y));
    output.Store(120, asuint(object_direction.z));
    float3x4 object_to_world = ObjectToWorld3x4();
    output.Store(124, asuint(object_to_world[0][0]));
    output.Store(128, asuint(object_to_world[1][1]));
    output.Store(132, asuint(object_to_world[2][2]));
    float3x4 world_to_object = WorldToObject3x4();
    output.Store(136, asuint(world_to_object[0][0]));
    output.Store(140, asuint(world_to_object[1][1]));
    output.Store(144, asuint(world_to_object[2][2]));
  }
  RayDesc recursive_ray;
  recursive_ray.Origin = float3(0.0, 0.0, -2.0);
  recursive_ray.TMin = 0.0;
  recursive_ray.Direction = float3(0.0, 1.0, 0.0);
  recursive_ray.TMax = 10.0;
  MissPayload recursive_payload;
  recursive_payload.value = 0;
  TraceRay(scene, RAY_FLAG_NONE, 0xff, 0, 0, 0,
           recursive_ray, recursive_payload);
  closest_hit_local_output.Store(0, 0x4c525557);
  payload.value = accepted_hit
                      ? 0x41434350
                      : any_hit_ran && recursive_payload.value == 0x4d495353 &&
                                closest_hit_local_marker == 0x4c4f434c &&
                                closest_hit_local_buffer.Load(0) == 0x53525631 &&
                                closest_hit_local_cbv_marker == 0x43425631 &&
                                closest_hit_local_texture.SampleLevel(
                                    closest_hit_local_sampler, float2(0.5, 0.5), 0).r >
                                    0.9 &&
                                closest_hit_local_texture.SampleLevel(
                                    closest_hit_local_static_sampler,
                                    float2(0.5, 0.5), 0).r > 0.9
                            ? 0x52454332
                            : 0x48495431;
}

[shader("anyhit")]
void any_hit(inout MissPayload payload,
             BuiltInTriangleIntersectionAttributes attributes) {
  uint incoming_payload = payload.value;
  if (incoming_payload == 0xffffffff) {
    IgnoreHit();
    return;
  }
  if (incoming_payload == 0xfffffffe) {
    AcceptHitAndEndSearch();
    return;
  }
  payload.value = 0x414e5948;
}

[shader("callable")]
void callable_shader(inout CallablePayload payload) {
  payload.value = closest_hit_local_marker == 0x4c4f434c
                      ? 0x43414c4c
                      : 0x43414c46;
}

struct ProceduralAttributes {
  float marker;
};

[shader("intersection")]
void procedural_intersection() {
  ProceduralAttributes attributes;
  attributes.marker = 19.0;
  ReportHit(1.0, 0, attributes);
}

[shader("closesthit")]
void procedural_closest_hit(inout MissPayload payload,
                            ProceduralAttributes attributes) {
  payload.value = attributes.marker == 19.0 ? 0x50524f43 : 0x50524f46;
}
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_main -T cs_6_5 \
      -Fo probe_dxr_inline.cso probe_dxr_inline.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_main -T cs_6_5 \
      -Fo probe_dxr_inline_invalid.cso probe_dxr_inline_invalid.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_main -T cs_6_9 \
      -Fo probe_dxr_inline_accessors.cso probe_dxr_inline_accessors.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -T lib_6_5 \
      -Fo probe_dxr_raygen.cso probe_dxr_raygen.hlsl >/dev/null
  )

  if [[ "${DXR_INLINE_ONLY:-0}" == "1" ]]; then
    return
  fi

  DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 -I/usr/local/include \
      "$SDK_DIR/scripts/compile-procedural-raytracing.cpp" \
      -L/usr/local/lib -lmetalirconverter -o "$procedural_compiler"

  mkdir -p "$SHADER_CACHE_DIR"
  for _dxr_warmup_pass in 1 2; do
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      "$WINE_BIN" probe_mini_dxr_acceleration_structures.exe >/dev/null || true
    )
    convert_dxil_shader_cache "$SHADER_CACHE_DIR"
    local converter="$METAL_SHADER_CONVERTER"
    if [[ -z "$converter" ]]; then
      converter="$(command -v metal-shaderconverter || true)"
    fi
    if [[ "${METALSHARP_NATIVE_IRCONVERTER:-0}" == "1" ]]; then
      materialize_native_dxr_cache "$SHADER_CACHE_DIR" "$raygen_cso" \
        "$raygen_root" "$closest_hit_local_root" "$procedural_compiler"
    elif [[ -n "$converter" && -x "$converter" ]]; then
      local dxbc
      shopt -s nullglob
      for dxbc in "$SHADER_CACHE_DIR"/*.dxbc; do
        if ! cmp -s "$dxbc" "$raygen_cso"; then
          continue
        fi
        local base="${dxbc%.dxbc}"
        if DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" raygen "$base.metallib" \
          "$closest_hit_local_root" \
          >"$base.raygen-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" @ray-dispatch \
          "$base.raydispatch.metallib" "$closest_hit_local_root" \
          >"$base.raydispatch-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" miss_shader "$base.miss.metallib" \
          "$closest_hit_local_root" \
          >"$base.miss-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" closest_hit "$base.closesthit.metallib" \
          "$closest_hit_local_root" \
          >"$base.closesthit-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" callable_shader "$base.callable.metallib" \
          "$closest_hit_local_root" \
          >"$base.callable-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" any_hit "$base.anyhit.metallib" \
          "$closest_hit_local_root" \
          >"$base.anyhit-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" @triangle-wrapper \
          "$base.rayintersection.metallib" "$closest_hit_local_root" \
          >"$base.rayintersection-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" procedural_intersection \
          "$base.proceduralintersection.metallib" "$closest_hit_local_root" \
          >"$base.proceduralintersection-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" procedural_closest_hit \
          "$base.proceduralclosesthit.metallib" "$closest_hit_local_root" \
          >"$base.proceduralclosesthit-msc.log" 2>&1 &&
          DYLD_LIBRARY_PATH=/usr/local/lib "$procedural_compiler" \
          "$dxbc" "$raygen_root" @procedural-wrapper \
          "$base.proceduralwrapper.metallib" "$closest_hit_local_root" \
          >"$base.proceduralwrapper-msc.log" 2>&1; then
          rm -f "$base.msc.fail"
        fi
      done
      shopt -u nullglob
    fi
  done
}

prepare_command_replay_advanced_probes() {
  local raygen_hlsl="$SDK_DIR/out/bin/probe_command_replay_raygen.hlsl"
  local mesh_hlsl="$SDK_DIR/out/bin/probe_command_replay_mesh.hlsl"
  local ray_compiler="$SDK_DIR/out/bin/compile-command-raytracing"

  DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 -I/usr/local/include \
      "$SDK_DIR/scripts/compile-procedural-raytracing.cpp" \
      -L/usr/local/lib -lmetalirconverter -o "$ray_compiler"

  cat > "$raygen_hlsl" <<'HLSL'
RWByteAddressBuffer output : register(u0);

[shader("raygeneration")]
void raygen() {
  output.Store(0, 0x52415931);
}
HLSL

  local hitobject_local_root_hlsl="$SDK_DIR/out/bin/probe_command_replay_hitobject_local_root.hlsl"
  cat > "$hitobject_local_root_hlsl" <<'HLSL'
RWByteAddressBuffer output : register(u0);

[shader("raygeneration")]
void raygen() {
  dx::HitObject nop;
  nop.SetShaderTableIndex(0);
  const uint nop_value = nop.LoadLocalRootTableConstant(0);

  RayDesc ray = {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 0.0, 100.0};
  dx::HitObject miss = dx::HitObject::MakeMiss(0, 0, ray);
  const uint miss_value = miss.LoadLocalRootTableConstant(0);
  output.Store(0, nop_value == 0u && miss_value == 0xa1b2c3d4u
                        ? 0xa1b2c3d4u
                        : 0u);
}
HLSL

  local hitobject_invoke_hlsl="$SDK_DIR/out/bin/probe_command_replay_hitobject_invoke.hlsl"
  cat > "$hitobject_invoke_hlsl" <<'HLSL'
RWByteAddressBuffer output : register(u0);

struct [raypayload] Payload {
  uint value : read(caller, miss) : write(caller, miss);
};

[shader("raygeneration")]
void raygen() {
  RayDesc ray = {{0.0, 0.0, -2.0}, {0.0, 0.0, 1.0}, 0.0, 10.0};
  Payload payload;
  payload.value = 0x1234;
  dx::HitObject miss = dx::HitObject::MakeMiss(0, 0, ray);
  dx::HitObject::Invoke(miss, payload);
  output.Store(0, payload.value);
}

[shader("miss")]
void miss_shader(inout Payload payload) {
  payload.value = 0x5678;
}
HLSL

  local hitobject_reorder_hlsl="$SDK_DIR/out/bin/probe_command_replay_hitobject_reorder.hlsl"
  cat > "$hitobject_reorder_hlsl" <<'HLSL'
RWByteAddressBuffer output : register(u0);

[shader("raygeneration")]
void raygen() {
  dx::HitObject hit;
  dx::MaybeReorderThread(0xf2, 7);
  dx::MaybeReorderThread(hit, 0xf1, 3);
  output.Store(0, hit.IsNop() ? 1u : 0u);
}
HLSL

  local hitobject_attributes_hlsl="$SDK_DIR/out/bin/probe_command_replay_hitobject_attributes.hlsl"
  cat > "$hitobject_attributes_hlsl" <<'HLSL'
RaytracingAccelerationStructure Scene : register(t0);
RWByteAddressBuffer output : register(u0);

struct [raypayload] Payload {
  uint value : read(caller, closesthit, miss) : write(caller, closesthit, miss);
};

[shader("raygeneration")]
void raygen() {
  RayDesc ray;
  ray.Origin = float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  Payload payload = {0};
  dx::HitObject hit = dx::HitObject::TraceRay(Scene, RAY_FLAG_NONE, 0xff, 0, 1, 0, ray, payload);
  BuiltInTriangleIntersectionAttributes attributes;
  hit.GetAttributes(attributes);
  output.Store(0, asuint(attributes.barycentrics.x));
  output.Store(4, asuint(attributes.barycentrics.y));
}
HLSL

  cat > "$mesh_hlsl" <<'HLSL'
RWByteAddressBuffer output : register(u0);

struct MeshVertex {
  float4 position : SV_Position;
};

struct MeshPrimitive {
  uint render_target_index : SV_RenderTargetArrayIndex;
};

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_main(out vertices MeshVertex vertices[3],
             out primitives MeshPrimitive primitives[1],
             out indices uint3 triangles[1],
             uint group_thread_id : SV_GroupIndex) {
  SetMeshOutputCounts(3, 1);
  output.Store(0, 0x4d455348);
  if (group_thread_id == 0) {
    vertices[0].position = float4(-0.8, -0.8, 0.0, 1.0);
    vertices[1].position = float4(0.0, 0.8, 0.0, 1.0);
    vertices[2].position = float4(0.8, -0.8, 0.0, 1.0);
    primitives[0].render_target_index = 0;
    triangles[0] = uint3(0, 1, 2);
  }
}

float4 ps_main() : SV_Target0 {
  return float4(0.0, 1.0, 0.0, 1.0);
}
HLSL

  local raygen_root="$SDK_DIR/out/bin/probe_command_replay_raygen_root.json"
  cat > "$raygen_root" <<'JSON'
{
  "RootSignature": {
    "Flags": "IRRootSignatureFlagNone",
    "NumParameters": 1,
    "NumStaticSamplers": 0,
    "Parameters": [{
      "DescriptorTable": {
        "DescriptorRanges": [{
          "BaseShaderRegister": 0,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 0,
          "RangeType": "IRDescriptorRangeTypeSRV",
          "RegisterSpace": 0
        }, {
          "BaseShaderRegister": 0,
          "Flags": "IRDescriptorRangeFlagNone",
          "NumDescriptors": 1,
          "OffsetInDescriptorsFromTableStart": 1,
          "RangeType": "IRDescriptorRangeTypeUAV",
          "RegisterSpace": 0
        }],
        "NumDescriptorRanges": 2
      },
      "ParameterType": "IRRootParameterTypeDescriptorTable",
      "ShaderVisibility": "IRShaderVisibilityAll"
    }],
    "StaticSamplers": []
  },
  "version": "IRRootSignatureVersion_1_1"
}
JSON

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E raygen -T lib_6_5 \
      -Fo probe_command_replay_raygen.cso probe_command_replay_raygen.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E raygen -T lib_6_9 \
      -Fo probe_command_replay_hitobject_local_root.cso \
      probe_command_replay_hitobject_local_root.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E raygen -T lib_6_9 \
      -Fo probe_command_replay_hitobject_invoke.cso \
      probe_command_replay_hitobject_invoke.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E raygen -T lib_6_9 \
      -Fo probe_command_replay_hitobject_attributes.cso \
      probe_command_replay_hitobject_attributes.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E raygen -T lib_6_9 \
      -Fo probe_command_replay_hitobject_reorder.cso \
      probe_command_replay_hitobject_reorder.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ms_main -T ms_6_5 \
      -Fo probe_command_replay_mesh_ms.cso probe_command_replay_mesh.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_command_replay_mesh_ps.cso probe_command_replay_mesh.hlsl >/dev/null
  )

  mkdir -p "$SHADER_CACHE_DIR"
  for _command_replay_warmup_pass in 1 2 3; do
    run_probe_exe "$COMMAND_REPLAY_PROBE_EXE" \
      "$RESULTS_DIR/probe-command-replay-warmup-$PROFILE.json" || true
    convert_dxil_shader_cache "$SHADER_CACHE_DIR"
    local converter="$METAL_SHADER_CONVERTER"
    if [[ -z "$converter" ]]; then
      converter="$(command -v metal-shaderconverter || true)"
    fi
    local dxbc
    shopt -s nullglob
    for dxbc in "$SHADER_CACHE_DIR"/*.dxbc; do
      if ! cmp -s "$dxbc" "$SDK_DIR/out/bin/probe_command_replay_raygen.cso"; then
        continue
      fi
      local base="${dxbc%.dxbc}"
      rm -f "$base.metallib" "$base.json" "$base.raydispatch.metallib" "$base.msc.fail"
      if [[ -n "$converter" && -x "$converter" ]] &&
         "$converter" -o "$base.metallib" "$dxbc" \
           --root-signature "$raygen_root" \
           --entry-point=raygen \
           --rt-ray-generation-compilation=kernel \
           --output-reflection-file="$base.json" \
           --deployment-os=macOS \
           --minimum-os-build-version=15.0.0 \
           >"$base.raygen-msc.log" 2>&1 &&
         DYLD_LIBRARY_PATH=/usr/local/lib "$ray_compiler" \
           "$dxbc" "$raygen_root" @ray-dispatch "$base.raydispatch.metallib" \
           >"$base.raydispatch-msc.log" 2>&1; then
        :
      elif [[ "${METALSHARP_NATIVE_IRCONVERTER:-0}" == "1" ]] &&
           DYLD_LIBRARY_PATH=/usr/local/lib "$ray_compiler" \
             "$dxbc" "$raygen_root" raygen "$base.metallib" \
             >"$base.raygen-native.log" 2>&1 &&
           DYLD_LIBRARY_PATH=/usr/local/lib "$ray_compiler" \
             "$dxbc" "$raygen_root" @ray-dispatch "$base.raydispatch.metallib" \
             >"$base.raydispatch-native.log" 2>&1; then
        rm -f "$base.msc.fail"
      else
        : >"$base.msc.fail"
      fi
    done
    shopt -u nullglob
  done

  # The custom HitObject provider compiles ray generation in-process, so the
  # miss stage must be materialized separately under the exact cache key that
  # MTLD3D12StateObject computes for this DXIL library.  Run one intentionally
  # cache-cold probe to expose that key, then use the selected host converter
  # to materialize only the visible miss function.  The final proof can
  # therefore run with METAL_SHADER_CONVERTER=/nonexistent.
  local invoke_cso="$SDK_DIR/out/bin/probe_command_replay_hitobject_invoke.cso"
  if [[ -f "$invoke_cso" ]]; then
    rm -f "$SDK_DIR/out/bin/dxmt-d3d12-trace.log"
    find "$RESULTS_DIR" -maxdepth 1 -type f \
      -name '*dxmt-d3d12-trace.log' -delete
    export D3D12_METAL_SDK_COMMAND_RAY_CSO="$invoke_cso"
    export D3D12_METAL_SDK_COMMAND_RAY_LOCAL_ROOT=
    export D3D12_METAL_SDK_COMMAND_RAY_INVOKE=1
    export DXMT_D3D12_TRACE=1
    export DXMT_LOG_PATH="$RESULTS_DIR"
    run_probe_exe "$COMMAND_REPLAY_PROBE_EXE" \
      "$RESULTS_DIR/probe-command-replay-invoke-warmup-$PROFILE.json" || true
    local invoke_miss_path=""
    local invoke_trace=""
    invoke_trace="$(find "$RESULTS_DIR" -maxdepth 1 -type f \
      -name '*dxmt-d3d12-trace.log' -print -quit 2>/dev/null || true)"
    if [[ -n "$invoke_trace" ]]; then
      invoke_miss_path="$(grep -oE 'miss=[^ ]+\.miss\.metallib' \
        "$invoke_trace" | tail -1 | cut -d= -f2- || true)"
    fi
    unset D3D12_METAL_SDK_COMMAND_RAY_CSO
    unset D3D12_METAL_SDK_COMMAND_RAY_LOCAL_ROOT
    unset D3D12_METAL_SDK_COMMAND_RAY_INVOKE
    unset DXMT_D3D12_TRACE
    unset DXMT_LOG_PATH
    local converter_for_invoke="$METAL_SHADER_CONVERTER"
    if [[ -z "$converter_for_invoke" || ! -x "$converter_for_invoke" ]]; then
      converter_for_invoke="$(command -v metal-shaderconverter || true)"
    fi
    if [[ -n "$invoke_miss_path" &&
          ("${METALSHARP_NATIVE_IRCONVERTER:-0}" == "1" ||
           ( -n "$converter_for_invoke" && -x "$converter_for_invoke" )) ]]; then
      DYLD_LIBRARY_PATH=/usr/local/lib "$ray_compiler" \
        "$invoke_cso" "$raygen_root" miss_shader "$invoke_miss_path" \
        >"$invoke_miss_path.msc.log" 2>&1 || true
    fi
  fi
}

prepare_attribute_at_vertex_probe() {
  local source="$SDK_DIR/probes/probe_attribute_at_vertex/attribute_at_vertex.hlsl"
  local staged_source="$SDK_DIR/out/bin/probe_attribute_at_vertex.hlsl"
  local vertex_shader="$SDK_DIR/out/bin/probe_attribute_at_vertex_vs.cso"
  local pixel_shader="$SDK_DIR/out/bin/probe_attribute_at_vertex_ps.cso"
  cp "$source" "$staged_source"
  if ! (
    cd "$SDK_DIR/out/bin"
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 \
      -Fo probe_attribute_at_vertex_vs.cso probe_attribute_at_vertex.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_1 \
      -Fo probe_attribute_at_vertex_ps.cso probe_attribute_at_vertex.hlsl >/dev/null; then
      exit 1
    fi
  ); then
    echo "failed to compile AttributeAtVertex DXIL fixtures" >&2
    return 1
  fi
  [[ -s "$vertex_shader" && -s "$pixel_shader" ]] || {
    echo "AttributeAtVertex DXIL fixtures are missing" >&2
    return 1
  }
}

prepare_rov_probe() {
  local source_dir="$SDK_DIR/probes/probe_rov"
  local vertex_source="$SDK_DIR/out/bin/probe_rov.hlsl"
  local texture_source="$SDK_DIR/out/bin/probe_rov_texture.hlsl"
  local structured_source="$SDK_DIR/out/bin/probe_rov_structured.hlsl"
  local typed_source="$SDK_DIR/out/bin/probe_rov_typed.hlsl"
  local array_source="$SDK_DIR/out/bin/probe_rov_array.hlsl"
  local float_source="$SDK_DIR/out/bin/probe_rov_float.hlsl"
  local vertex_rov_source="$SDK_DIR/out/bin/probe_rov_vertex.hlsl"
  local compute_rov_source="$SDK_DIR/out/bin/probe_rov_compute.hlsl"
  local vertex_shader="$SDK_DIR/out/bin/probe_rov_vs.cso"
  local raw_shader="$SDK_DIR/out/bin/probe_rov_raw_ps.cso"
  local texture_shader="$SDK_DIR/out/bin/probe_rov_texture_ps.cso"
  local structured_shader="$SDK_DIR/out/bin/probe_rov_structured_ps.cso"
  local typed_shader="$SDK_DIR/out/bin/probe_rov_typed_ps.cso"
  local array_shader="$SDK_DIR/out/bin/probe_rov_array_ps.cso"
  local float_shader="$SDK_DIR/out/bin/probe_rov_float_ps.cso"
  local vertex_rov_shader="$SDK_DIR/out/bin/probe_rov_vertex.cso"
  local compute_rov_shader="$SDK_DIR/out/bin/probe_rov_compute.cso"
  cp "$source_dir/rov.hlsl" "$vertex_source"
  cp "$source_dir/rov_texture.hlsl" "$texture_source"
  cp "$source_dir/rov_structured.hlsl" "$structured_source"
  cp "$source_dir/rov_typed.hlsl" "$typed_source"
  cp "$source_dir/rov_array.hlsl" "$array_source"
  cp "$source_dir/rov_float.hlsl" "$float_source"
  cp "$source_dir/rov_vertex.hlsl" "$vertex_rov_source"
  cp "$source_dir/rov_compute.hlsl" "$compute_rov_source"
  if ! (
    cd "$SDK_DIR/out/bin"
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 \
      -Fo probe_rov_vs.cso probe_rov.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_raw_ps.cso probe_rov.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_texture_ps.cso probe_rov_texture.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_structured_ps.cso probe_rov_structured.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_typed_ps.cso probe_rov_typed.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_array_ps.cso probe_rov_array.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_0 \
      -Fo probe_rov_float_ps.cso probe_rov_float.hlsl >/dev/null; then
      exit 1
    fi
    # DXC's normal validator forbids ROV objects outside pixel shaders.  Build
    # these two intentional negative fixtures with validation disabled so the
    # runtime lowering boundary gets to reject them fail-closed itself.
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -Vd -E vs_main -T vs_6_0 \
      -Fo probe_rov_vertex.cso probe_rov_vertex.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -Vd -E cs_main -T cs_6_0 \
      -Fo probe_rov_compute.cso probe_rov_compute.hlsl >/dev/null; then
      exit 1
    fi
  ); then
    echo "failed to compile rasterizer-ordered UAV DXIL fixtures" >&2
    return 1
  fi
  [[ -s "$vertex_shader" && -s "$raw_shader" && -s "$texture_shader" &&
     -s "$structured_shader" && -s "$typed_shader" && -s "$array_shader" &&
     -s "$float_shader" && -s "$vertex_rov_shader" &&
     -s "$compute_rov_shader" ]] || {
    echo "rasterizer-ordered UAV DXIL fixtures are missing" >&2
    return 1
  }
}

prepare_barycentrics_probe() {
  local source="$SDK_DIR/probes/probe_barycentrics/barycentrics.hlsl"
  local staged_source="$SDK_DIR/out/bin/probe_barycentrics.hlsl"
  local vertex_shader="$SDK_DIR/out/bin/probe_barycentrics_vs.cso"
  local pixel_shader="$SDK_DIR/out/bin/probe_barycentrics_ps.cso"
  cp "$source" "$staged_source"
  if ! (
    cd "$SDK_DIR/out/bin"
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 \
      -Fo probe_barycentrics_vs.cso probe_barycentrics.hlsl >/dev/null; then
      exit 1
    fi
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E ps_main -T ps_6_1 \
      -Fo probe_barycentrics_ps.cso probe_barycentrics.hlsl >/dev/null; then
      exit 1
    fi
  ); then
    echo "failed to compile SV_Barycentrics DXIL fixtures" >&2
    return 1
  fi
  [[ -s "$vertex_shader" && -s "$pixel_shader" ]] || {
    echo "SV_Barycentrics DXIL fixtures are missing" >&2
    return 1
  }
}

prepare_cycle_counter_probe() {
  local source_dir="$SDK_DIR/probes/probe_cycle_counter"
  local llvm_root="${METALSHARP_X86_LLVM_ROOT:-/Volumes/AverySSD/toolchains}/clang+llvm-15.0.7-x86_64-apple-darwin21.0"
  local llvm_as="$llvm_root/bin/llvm-as"
  if [[ ! -x "$llvm_as" ]]; then
    echo "missing pinned llvm-as for CycleCounterLegacy probe: $llvm_as" >&2
    return 1
  fi

  cp "$source_dir/cycle_counter_vertex.hlsl" \
    "$SDK_DIR/out/bin/probe_cycle_counter_vertex.hlsl"
  if ! (
    cd "$SDK_DIR/out/bin"
    if ! WINEPREFIX="$WINE_PREFIX" WINEDLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -E vs_main -T vs_6_0 \
      -Fo probe_cycle_counter_vs.cso probe_cycle_counter_vertex.hlsl >/dev/null; then
      exit 1
    fi
  ); then
    echo "failed to compile CycleCounterLegacy vertex fixture" >&2
    return 1
  fi

  local stem raw output
  for stem in single multiple; do
    raw="$SDK_DIR/out/bin/probe_cycle_counter_${stem}.bc"
    output="$SDK_DIR/out/bin/probe_cycle_counter_${stem}.cso"
    if ! "$llvm_as" "$source_dir/cycle_counter_${stem}.ll" -o "$raw"; then
      echo "failed to assemble CycleCounterLegacy fixture: $stem" >&2
      return 1
    fi
    if ! python3 - "$raw" "$output" <<'PY'
import struct
import sys

raw_path, output_path = sys.argv[1:]
bitcode = open(raw_path, "rb").read()
if not bitcode or len(bitcode) % 4:
    raise SystemExit("invalid LLVM bitcode size")
program_version = (0 << 16) | (6 << 4)  # pixel, SM 6.0
program = struct.pack(
    "<II4sHHII", program_version, (24 + len(bitcode)) // 4,
    b"DXIL", 0, 1, 16, len(bitcode)
) + bitcode
part_offset = 36
chunk = struct.pack("<4sI", b"DXIL", len(program)) + program
container_size = part_offset + len(chunk)
container = (
    b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, container_size, 1) +
    struct.pack("<I", part_offset) + chunk
)
open(output_path, "wb").write(container)
PY
    then
      echo "failed to wrap CycleCounterLegacy fixture: $stem" >&2
      return 1
    fi
  done

  [[ -s "$SDK_DIR/out/bin/probe_cycle_counter_vs.cso" &&
     -s "$SDK_DIR/out/bin/probe_cycle_counter_single.cso" &&
     -s "$SDK_DIR/out/bin/probe_cycle_counter_multiple.cso" ]] || {
    echo "CycleCounterLegacy fixture outputs are missing" >&2
    return 1
  }
}

prepare_work_graph_probe() {
  local node_source_dir="$SDK_DIR/probes/probe_workgraph"
  local work_dir="$RESULTS_DIR/workgraph-generated"
  local d3d12_node_cso="$SDK_DIR/out/bin/probe_workgraph_node.cso"
  local d3d12_node_multi_cso="$SDK_DIR/out/bin/probe_workgraph_node_multi.cso"
  local d3d12_node_layout_cso="$SDK_DIR/out/bin/probe_workgraph_node_layout.cso"
  local node_compiler="$SDK_DIR/out/bin/compile-node-workgraph"
  local node_probe="$SDK_DIR/out/bin/probe-node-workgraph"
  local aggregate="$WORK_GRAPH_RESULT_FILE"
  mkdir -p "$work_dir"

  if ! DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 -I"$ROOT_DIR" \
      -I"$ROOT_DIR/vendor/dxmt/src/airconv/dxil" \
      "$SDK_DIR/scripts/compile-node-workgraph.cpp" \
      "$ROOT_DIR/vendor/dxmt/src/airconv/dxil/msl_lowering.cpp" \
      "$ROOT_DIR/vendor/dxmt/src/airconv/dxil/dxil_ir.cpp" \
      "$ROOT_DIR/vendor/dxmt/src/airconv/dxil/llvm_bitcode.cpp" \
      "$ROOT_DIR/vendor/dxmt/src/airconv/dxil/dxil_container.cpp" \
      -o "$node_compiler"; then
    echo "failed to build node DXIL lowering helper" >&2
    return 1
  fi
  if ! DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 "$SDK_DIR/scripts/probe-node-workgraph.mm" \
      -framework Foundation -framework Metal -o "$node_probe"; then
    echo "failed to build node Metal probe" >&2
    return 1
  fi
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -Fo "$d3d12_node_cso" \
      "$node_source_dir/node_records.hlsl" >/dev/null
  )
  [[ -s "$d3d12_node_cso" ]] || {
    echo "D3D12 node shader compilation produced no bytecode" >&2
    return 1
  }
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -Fo "$d3d12_node_multi_cso" \
      "$node_source_dir/node_multi.hlsl" >/dev/null
  )
  [[ -s "$d3d12_node_multi_cso" ]] || {
    echo "D3D12 multi-node shader compilation produced no bytecode" >&2
    return 1
  }

  local chain_cso="$SDK_DIR/out/bin/probe_workgraph_chain.cso"
  rm -f "$chain_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -Fo "$chain_cso" \
      "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$chain_cso" ]]; then
    echo "D3D12 node chain compilation failed" >&2
    return 1
  fi

  rm -f "$SDK_DIR/out/bin/probe_workgraph_chain_offset.cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DGRID_OFFSET=1 \
      -Fo "$SDK_DIR/out/bin/probe_workgraph_chain_offset.cso" \
      "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$SDK_DIR/out/bin/probe_workgraph_chain_offset.cso" ]]; then
    echo "D3D12 offset-grid compilation failed" >&2
    return 1
  fi

  local vector_cso="$SDK_DIR/out/bin/probe_workgraph_chain_vector.cso"
  rm -f "$vector_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DGRID_VECTOR=1 \
      -Fo "$vector_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$vector_cso" ]]; then
    echo "D3D12 vector-grid compilation failed" >&2
    return 1
  fi

  local fanout_cso="$SDK_DIR/out/bin/probe_workgraph_chain_fanout.cso"
  rm -f "$fanout_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DFANOUT=1 \
      -Fo "$fanout_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$fanout_cso" ]]; then
    echo "D3D12 fan-out compilation failed" >&2
    return 1
  fi

  local bad_target_cso="$SDK_DIR/out/bin/probe_workgraph_chain_bad_target.cso"
  rm -f "$bad_target_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DBAD_TARGET=1 \
      -Fo "$bad_target_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$bad_target_cso" ]]; then
    echo "D3D12 unsupported-target fixture compilation failed" >&2
    return 1
  fi

  local cycle_cso="$SDK_DIR/out/bin/probe_workgraph_chain_cycle.cso"
  rm -f "$cycle_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DCYCLE=1 \
      -Fo "$cycle_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$cycle_cso" ]]; then
    echo "D3D12 cycle fixture compilation failed" >&2
    return 1
  fi

  local u16_cso="$SDK_DIR/out/bin/probe_workgraph_chain_u16.cso"
  rm -f "$u16_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -enable-16bit-types -DGRID_U16=1 \
      -Fo "$u16_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$u16_cso" ]]; then
    echo "D3D12 U16-grid fixture compilation failed" >&2
    return 1
  fi

  local oversized_cso="$SDK_DIR/out/bin/probe_workgraph_chain_oversized.cso"
  rm -f "$oversized_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -DOVERSIZED_OUTPUT=1 \
      -Fo "$oversized_cso" "$node_source_dir/node_chain.hlsl" >/dev/null
  ) || [[ ! -s "$oversized_cso" ]]; then
    echo "D3D12 oversized-output fixture compilation failed" >&2
    return 1
  fi

  rm -f "$d3d12_node_layout_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -enable-16bit-types \
      -Fo "$d3d12_node_layout_cso" "$node_source_dir/node_input_records.hlsl" >/dev/null
  ) || [[ ! -s "$d3d12_node_layout_cso" ]]; then
    echo "D3D12 node input layout compilation failed" >&2
    return 1
  fi

  local collision_cso="$SDK_DIR/out/bin/probe_workgraph_node_collision.cso"
  rm -f "$collision_cso"
  if ! (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
      "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -Fo "$collision_cso" \
      "$node_source_dir/node_binding_collision.hlsl" >/dev/null
  ) || [[ ! -s "$collision_cso" ]]; then
    echo "D3D12 node binding collision compilation failed" >&2
    return 1
  fi

  local -a node_cases=(
    "node_handles|1|240,247,248,249,252"
    "node_system|1,32|242,253"
    "node_records|2882400001,305419896|238,239,241,251"
    "node_finished|1|243,250,251"
    "node_barriers|610800471|244,245"
    "node_record_barrier|324478056|238,239,241,246,251"
  )
  local case_spec stem expected opcode_list cso metal result
  local -a result_files=()
  for case_spec in "${node_cases[@]}"; do
    IFS='|' read -r stem expected opcode_list <<< "$case_spec"
    cso="$work_dir/$stem.cso"
    metal="$work_dir/$stem.metal"
    result="$work_dir/$stem.json"
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
        "$WINE_BIN" dxc.exe -nologo -T lib_6_8 -Fo "$cso" \
        "$node_source_dir/$stem.hlsl" >/dev/null
    )
    [[ -s "$cso" ]] || {
      echo "node shader compilation produced no bytecode: $stem" >&2
      return 1
    }
    DXMT_LOG_PATH="$work_dir" "$node_compiler" "$cso" node_main "$metal"
    [[ -s "$metal" ]] || {
      echo "node lowering produced no Metal source: $stem" >&2
      return 1
    }
    if ! "$node_probe" "$metal" "$expected" > "$result"; then
      echo "node runtime probe failed: $stem" >&2
      cat "$result" >&2 || true
      return 1
    fi
    result_files+=("$result|$opcode_list")
  done

  python3 - "$aggregate" "${result_files[@]}" <<'PY'
import json
import sys
from pathlib import Path

aggregate = Path(sys.argv[1])
cases = []
opcodes = set()
for item in sys.argv[2:]:
    result_path, opcode_text = item.split("|", 1)
    result = json.loads(Path(result_path).read_text(encoding="utf-8"))
    result["name"] = Path(result_path).stem
    result["opcodes"] = [int(value) for value in opcode_text.split(",")]
    opcodes.update(result["opcodes"])
    cases.append(result)

aggregate.write_text(json.dumps({
    "schema": "metalsharp.d3d12-metal.workgraph-opcode-probe.v1",
    "provider": "bounded_gpu_native_node_kernel",
    "d3d12_work_graph_api_supported": False,
    "cpu_scheduler": False,
    "pass": all(case.get("pass", False) for case in cases),
    "opcode_rows": sorted(opcodes),
    "opcode_count": len(opcodes),
    "cases": cases,
}, indent=2) + "\n", encoding="utf-8")
PY
  echo "$aggregate"
}

prepare_dxil_semantic_probes() {
  local hlsl="$SDK_DIR/out/bin/probe_dxil_semantics.hlsl"

  rm -f "$SDK_DIR"/out/bin/probe_dxil_semantic_*.cso

  cat > "$hlsl" <<'HLSL'
RWByteAddressBuffer outbuf : register(u0);
ByteAddressBuffer inbuf : register(t0);

[numthreads(4, 1, 1)]
void cs_math_bits(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0) {
    float f = sqrt(144.0) + abs(-5.0) + floor(2.9) + ceil(2.1);
    uint bits = (1u << 5) | (0xf0u & 0x0fu) | (0x12u ^ 0x02u);
    outbuf.Store(0, (uint)f);
    outbuf.Store(4, bits);
    outbuf.Store(8, asuint(asfloat(0x3f800000u)));
    outbuf.Store(12, countbits(0xf0f0u));
    outbuf.Store(16, (uint)firstbitlow(0x10u));
    outbuf.Store(20, (uint)firstbithigh(0x10u));
    outbuf.Store(24, (uint)firstbithigh(-16));
    outbuf.Store(28, reversebits(1u));
    outbuf.Store(32, min(7u, 3u));
    outbuf.Store(36, max(7u, 3u));
    outbuf.Store(40, mad(2u, 3u, 4u));
  }
}

[numthreads(1, 1, 1)]
void cs_math_intrinsics(uint3 id : SV_DispatchThreadID) {
  float one = asfloat(inbuf.Load(0));
  float zero = one - one;
  outbuf.Store(0, asuint(abs(-one)));
  outbuf.Store(4, asuint(saturate(-one)));
  outbuf.Store(8, asuint(cos(zero)));
  outbuf.Store(12, asuint(sin(zero)));
  outbuf.Store(16, asuint(tan(zero)));
  outbuf.Store(20, asuint(acos(one)));
  outbuf.Store(24, asuint(asin(zero)));
  outbuf.Store(28, asuint(atan(zero)));
  outbuf.Store(32, asuint(exp2(one + one + one)));
  outbuf.Store(36, asuint(log2(one + one + one + one + one + one + one + one)));
  outbuf.Store(40, asuint(rsqrt(one + one + one + one)));
  outbuf.Store(44, (uint)round(2.25));
  outbuf.Store(48, asuint(frac(2.75)));
  outbuf.Store(52, (uint)min(-2, 3));
  outbuf.Store(56, (uint)max(-2, 3));
  outbuf.Store(60, min(7u, 3u));
  outbuf.Store(64, max(7u, 3u));
  outbuf.Store(68, 13u / 3u);
  outbuf.Store(72, 5u * 7u);
  outbuf.Store(76, 13u - 5u);
}

[numthreads(1, 1, 1)]
void cs_math_extended(uint3 id : SV_DispatchThreadID) {
  uint4 sad = msad4(0x01020304u, uint2(0x01020304u, 0u), uint4(7u, 0u, 0u, 0u));
  outbuf.Store(0, asuint(cosh(0.0)));
  outbuf.Store(4, asuint(sinh(0.0)));
  outbuf.Store(8, asuint(tanh(0.0)));
  outbuf.Store(12, isnormal(1.0) ? 1u : 0u);
  outbuf.Store(16, sad.x);
}

[numthreads(1, 1, 1)]
void cs_core_opcode_matrix(uint3 id : SV_DispatchThreadID) {
  // Feed the matrix from UAV-visible input values so DXC cannot fold the
  // intrinsic calls into constants and erase their DXIL opcodes. The probe
  // supplies one and zero at t0[0:1], preserving the historical outputs.
  float one = asfloat(inbuf.Load(0));
  float zero = asfloat(inbuf.Load(4));
  float neg = -2.25f * one;
  float pos = 3.5f * one;
  float2 a2 = float2(1.0f * one, 2.0f * one);
  float2 b2 = float2(3.0f * one, 4.0f * one);
  float3 a3 = float3(1.0f * one, 2.0f * one, 3.0f * one);
  float3 b3 = float3(3.0f * one, 2.0f * one, 1.0f * one);
  float4 a4 = float4(1.0f * one, 2.0f * one, 3.0f * one, 4.0f * one);
  float4 b4 = float4(4.0f * one, 3.0f * one, 2.0f * one, 1.0f * one);
  int int_neg = asint(inbuf.Load(8));
  int int_pos = asint(inbuf.Load(12));
  uint unsigned_max = inbuf.Load(16);
  uint unsigned_min = inbuf.Load(20);
  uint bit_one = inbuf.Load(24);
  uint bit_sixteen = inbuf.Load(28);
  uint unsigned_thirteen = inbuf.Load(32);
  uint unsigned_three = inbuf.Load(36);
  uint unsigned_two = inbuf.Load(40);
  uint unsigned_four = inbuf.Load(44);
  outbuf.Store(0, asuint(abs(neg)));
  outbuf.Store(4, asuint(saturate(neg)));
  outbuf.Store(8, asuint(cos(zero)));
  outbuf.Store(12, asuint(sin(zero)));
  outbuf.Store(16, asuint(tan(zero)));
  outbuf.Store(20, asuint(acos(one)));
  outbuf.Store(24, asuint(asin(zero)));
  outbuf.Store(28, asuint(atan(zero)));
  outbuf.Store(32, asuint(cosh(zero)));
  outbuf.Store(36, asuint(sinh(zero)));
  outbuf.Store(40, asuint(tanh(zero)));
  outbuf.Store(44, asuint(exp2(3.0f * one)));
  outbuf.Store(48, asuint(frac(2.75f * one)));
  outbuf.Store(52, asuint(log2(8.0f * one)));
  outbuf.Store(56, asuint(sqrt(9.0f * one)));
  outbuf.Store(60, asuint(rsqrt(4.0f * one)));
  outbuf.Store(64, uint(round(2.25f * one)));
  outbuf.Store(68, uint(floor(2.75f * one)));
  outbuf.Store(72, uint(ceil(2.25f * one)));
  outbuf.Store(76, asuint(int(trunc(neg))));
  outbuf.Store(80, asuint(max(neg, pos)));
  outbuf.Store(84, asuint(min(neg, pos)));
  outbuf.Store(88, max(int_neg, int_pos));
  outbuf.Store(92, min(int_neg, int_pos));
  outbuf.Store(96, max(unsigned_max, unsigned_min));
  outbuf.Store(100, min(unsigned_max, unsigned_min));
  outbuf.Store(104, unsigned_thirteen / unsigned_three);
  outbuf.Store(108, mad(unsigned_two, unsigned_three, unsigned_four));
  outbuf.Store(112, asuint(mad(2.0f * one, 3.0f * one, 4.0f * one)));
  outbuf.Store(116, asuint(dot(a2, b2)));
  outbuf.Store(120, asuint(dot(a3, b3)));
  outbuf.Store(124, asuint(dot(a4, b4)));
  outbuf.Store(128, reversebits(bit_one));
  outbuf.Store(132, countbits(0xf0f0u * bit_one));
  outbuf.Store(136, uint(firstbitlow(bit_one * bit_sixteen)));
  outbuf.Store(140, uint(firstbithigh(bit_one * bit_sixteen)));
}

[numthreads(1, 1, 1)]
void cs_dot4add_unsigned(uint3 id : SV_DispatchThreadID) {
  uint value = dot4add_u8packed(0x01020304u, 0x04030201u, inbuf.Load(0));
  outbuf.Store(0, value);
}

[numthreads(1, 1, 1)]
void cs_dot4add_signed(uint3 id : SV_DispatchThreadID) {
  int value = dot4add_i8packed(0x0102fefdu, 0x0403fffeu, (int)inbuf.Load(0));
  outbuf.Store(0, (uint)value);
}

[numthreads(1, 1, 1)]
void cs_dot2add_half(uint3 id : SV_DispatchThreadID) {
  float value = dot2add(half2(1.0h, 2.0h), half2(3.0h, 4.0h), (float)inbuf.Load(0));
  outbuf.Store(0, asuint(value));
}

[numthreads(1, 1, 1)]
void cs_special_float(uint3 id : SV_DispatchThreadID) {
  float nan_value = asfloat(0x7fc00000u);
  float inf_value = asfloat(0x7f800000u);
  float finite_value = 1.5;
  outbuf.Store(0, isnan(nan_value) ? 1u : 0u);
  outbuf.Store(4, isinf(inf_value) ? 1u : 0u);
  outbuf.Store(8, isfinite(finite_value) ? 1u : 0u);
  outbuf.Store(12, isfinite(inf_value) ? 1u : 0u);
}

[numthreads(4, 1, 1)]
void cs_buffer(uint3 id : SV_DispatchThreadID) {
  uint v = inbuf.Load(id.x * 4);
  outbuf.Store(id.x * 4, v * 3 + 1);
}

[numthreads(1, 1, 1)]
void cs_atomic_uav(uint3 id : SV_DispatchThreadID) {
  uint original = 0;
  outbuf.InterlockedExchange(0, 5, original);
  outbuf.Store(4, original);
  outbuf.InterlockedAdd(0, 3, original);
  outbuf.Store(8, original);
  outbuf.InterlockedCompareExchange(0, 8, 7, original);
  outbuf.Store(12, original);
}

[numthreads(1, 1, 1)]
void cs_atomic_matrix(uint3 id : SV_DispatchThreadID) {
  uint original = 0;
  outbuf.Store(0, 10u);
  outbuf.InterlockedAdd(0, 3u, original);
  outbuf.Store(4, original);
  outbuf.Store(8, 15u);
  outbuf.InterlockedAnd(8, 6u, original);
  outbuf.Store(12, original);
  outbuf.Store(16, 15u);
  outbuf.InterlockedOr(16, 6u, original);
  outbuf.Store(20, original);
  outbuf.Store(24, 15u);
  outbuf.InterlockedXor(24, 6u, original);
  outbuf.Store(28, original);
  outbuf.Store(32, 0xffffffffu);
  outbuf.InterlockedMin(32, 3u, original);
  outbuf.Store(36, original);
  outbuf.Store(40, 0u);
  outbuf.InterlockedMax(40, 3u, original);
  outbuf.Store(44, original);
  outbuf.Store(48, 0xffffffffu);
  outbuf.InterlockedMin(48, 3u, original);
  outbuf.Store(52, original);
  outbuf.Store(56, 0u);
  outbuf.InterlockedMax(56, 3u, original);
  outbuf.Store(60, original);
  outbuf.Store(64, 5u);
  outbuf.InterlockedExchange(64, 9u, original);
  outbuf.Store(68, original);
}

[numthreads(4, 1, 1)]
void cs_vector_shuffle(uint3 id : SV_DispatchThreadID) {
  uint4 lanes = uint4(id.x + 1u, id.x + 2u, id.x + 3u, id.x + 4u);
  uint4 reversed = lanes.wzyx;
  outbuf.Store(id.x * 4, reversed.x + reversed.y * 10u +
                          reversed.z * 100u + reversed.w * 1000u);
}

groupshared uint g_counter;

uint helper_leaf(uint value) {
  return value * value + 3u;
}

uint4 helper_vector(uint base) {
  return uint4(helper_leaf(base), helper_leaf(base + 1u),
               helper_leaf(base + 2u), helper_leaf(base + 3u));
}

uint helper_reduce(uint4 value) {
  return value.x + value.y + value.z + value.w;
}

[numthreads(4, 1, 1)]
void cs_helper_aggregate(uint3 id : SV_DispatchThreadID) {
  outbuf.Store(id.x * 4, helper_reduce(helper_vector(id.x + 1u)));
}

[numthreads(4, 1, 1)]
void cs_matrix_aggregate(uint3 id : SV_DispatchThreadID) {
  float2x2 transform;
  transform[0] = float2(1.0, 2.0);
  transform[1] = float2(3.0, 4.0);
  float2 value = float2(float(id.x + 1u), float(id.x + 2u));
  float2 transformed = mul(value, transform);
  outbuf.Store(id.x * 4, uint(transformed.x + transformed.y + 0.5));
}

[numthreads(4, 1, 1)]
void cs_atomics_ids(uint3 gid : SV_GroupID,
                    uint3 tid : SV_GroupThreadID,
                    uint gi : SV_GroupIndex,
                    uint3 did : SV_DispatchThreadID) {
  if (gi == 0)
    g_counter = 0;
  GroupMemoryBarrierWithGroupSync();
  InterlockedAdd(g_counter, 1);
  GroupMemoryBarrierWithGroupSync();
  outbuf.Store(gi * 4, g_counter + did.x + tid.x + gid.x);
}

[numthreads(4, 1, 1)]
void cs_wave_quad(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  uint first = WaveReadLaneFirst(id.x + 1);
  uint sum = WaveActiveSum(1);
  uint across = QuadReadAcrossX(id.x);
  uint valid = (sum >= 4 ? 0x100u : 0u) |
               (first == 1 ? 0x10u : 0u) |
               (across == (id.x ^ 1u) ? 0x1u : 0u);
  outbuf.Store(gi * 4, valid);
}

[numthreads(4, 1, 1)]
void cs_sm67(uint3 id : SV_DispatchThreadID) {
  uint4 vector = uint4(id.x + 1u, id.x + 2u, id.x + 3u, id.x + 4u);
  uint64_t wide = (uint64_t(vector.x) << 32) | uint64_t(vector.x + 67u);
  outbuf.Store(id.x * 4, uint(wide));
}

[numthreads(4, 1, 1)]
void cs_sm68(uint3 id : SV_DispatchThreadID) {
  uint64_t wide = uint64_t(id.x + 1u) * uint64_t(68u);
  outbuf.Store(id.x * 4, uint(wide));
}

[numthreads(1, 1, 1)]
void cs_fdot_sm69(uint3 id : SV_DispatchThreadID) {
  float4 a = float4(1.0f, 2.0f, 3.0f, 4.0f);
  float4 b = float4(4.0f, 3.0f, 2.0f, 1.0f);
  outbuf.Store(0, asuint(dot(a, b)));
}

[numthreads(4, 1, 1)]
void cs_control_aggregate(uint3 id : SV_DispatchThreadID) {
  uint4 lanes = uint4(id.x + 1u, id.x + 2u, id.x + 3u, id.x + 4u);
  uint value = lanes.x + lanes.y + lanes.z + lanes.w;
  if ((id.x & 1u) == 0u)
    value += 10u;
  else
    value -= 2u;
  switch (id.x) {
  case 0u:
    value += 100u;
    break;
  case 1u:
    value += 200u;
    break;
  default:
    value += 300u;
    break;
  }
  outbuf.Store(id.x * 4, value);
}

[numthreads(4, 1, 1)]
void cs_loop_aggregate(uint3 id : SV_DispatchThreadID) {
  uint value = 0u;
  [loop]
  for (uint i = 0u; i <= id.x; ++i)
    value += i + 1u;
  outbuf.Store(id.x * 4, value);
}

HLSL

  local double_bitcast_hlsl="$SDK_DIR/out/bin/probe_dxil_semantic_double_bitcast.hlsl"
  cat > "$double_bitcast_hlsl" <<'HLSL_DOUBLE_BITCAST'
RWByteAddressBuffer outbuf : register(u0);
ByteAddressBuffer inbuf : register(t0);

[numthreads(1, 1, 1)]
void cs_double_arithmetic(uint3 id : SV_DispatchThreadID) {
  double value = 1.5 + 2.25;
  outbuf.Store(0, (uint)(value * 10.0));
}

[numthreads(1, 1, 1)]
void cs_double_bitcast(uint3 id : SV_DispatchThreadID) {
  uint low = inbuf.Load(0);
  uint high = inbuf.Load(4);
  double value = asdouble(low, high);
  uint split_low = 0;
  uint split_high = 0;
  asuint(value, split_low, split_high);
  outbuf.Store(0, split_low);
  outbuf.Store(4, split_high);
}

[numthreads(1, 1, 1)]
void cs_double_dynamic_arithmetic(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  double sum = lhs + rhs;
  double adjustment = asdouble(inbuf.Load(16), inbuf.Load(20));
  double adjusted = sum - adjustment;
  uint split_low = 0;
  uint split_high = 0;
  asuint(adjusted, split_low, split_high);
  outbuf.Store(0, split_low);
  outbuf.Store(4, split_high);
  outbuf.Store(8, id.x);
}

[numthreads(1, 1, 1)]
void cs_double_add_matrix(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint input_offset = i * 16;
    double lhs = asdouble(inbuf.Load(input_offset),
                          inbuf.Load(input_offset + 4));
    double rhs = asdouble(inbuf.Load(input_offset + 8),
                          inbuf.Load(input_offset + 12));
    double sum = lhs + rhs;
    uint split_low = 0;
    uint split_high = 0;
    asuint(sum, split_low, split_high);
    outbuf.Store(i * 8, split_low);
    outbuf.Store(i * 8 + 4, split_high);
  }
}

[numthreads(1, 1, 1)]
void cs_double_multiply(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  double result = lhs * rhs;
  uint low = 0;
  uint high = 0;
  asuint(result, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
}

[numthreads(1, 1, 1)]
void cs_double_divide(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  double result = lhs / rhs;
  uint low = 0;
  uint high = 0;
  asuint(result, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
}

[numthreads(1, 1, 1)]
void cs_double_remainder(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  double result = fmod(lhs, rhs);
  uint low = 0;
  uint high = 0;
  asuint(result, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
}

[numthreads(1, 1, 1)]
void cs_double_compare(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  outbuf.Store(0, lhs == rhs ? 1u : 0u);
  outbuf.Store(4, lhs < rhs ? 1u : 0u);
  outbuf.Store(8, lhs > rhs ? 1u : 0u);
  outbuf.Store(12, lhs != rhs ? 1u : 0u);
}

[numthreads(1, 1, 1)]
void cs_double_multiply_matrix(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint input_offset = i * 16;
    double lhs = asdouble(inbuf.Load(input_offset), inbuf.Load(input_offset + 4));
    double rhs = asdouble(inbuf.Load(input_offset + 8), inbuf.Load(input_offset + 12));
    uint low = 0;
    uint high = 0;
    asuint(lhs * rhs, low, high);
    outbuf.Store(i * 8, low);
    outbuf.Store(i * 8 + 4, high);
  }
}

[numthreads(1, 1, 1)]
void cs_double_divide_matrix(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint input_offset = i * 16;
    double lhs = asdouble(inbuf.Load(input_offset), inbuf.Load(input_offset + 4));
    double rhs = asdouble(inbuf.Load(input_offset + 8), inbuf.Load(input_offset + 12));
    uint low = 0;
    uint high = 0;
    asuint(lhs / rhs, low, high);
    outbuf.Store(i * 8, low);
    outbuf.Store(i * 8 + 4, high);
  }
}

[numthreads(1, 1, 1)]
void cs_double_compare_matrix(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 6; ++i) {
    uint input_offset = i * 16;
    double lhs = asdouble(inbuf.Load(input_offset), inbuf.Load(input_offset + 4));
    double rhs = asdouble(inbuf.Load(input_offset + 8), inbuf.Load(input_offset + 12));
    outbuf.Store(i * 16, lhs == rhs ? 1u : 0u);
    outbuf.Store(i * 16 + 4, lhs < rhs ? 1u : 0u);
    outbuf.Store(i * 16 + 8, lhs > rhs ? 1u : 0u);
    outbuf.Store(i * 16 + 12, lhs != rhs ? 1u : 0u);
  }
}

[numthreads(1, 1, 1)]
void cs_double_float_conversion(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 6; ++i) {
    uint input_offset = i * 8;
    double source = asdouble(inbuf.Load(input_offset), inbuf.Load(input_offset + 4));
    double roundtrip = (double)((float)source);
    uint low = 0;
    uint high = 0;
    asuint(roundtrip, low, high);
    outbuf.Store(i * 8, low);
    outbuf.Store(i * 8 + 4, high);
  }
}

[numthreads(1, 1, 1)]
void cs_double_integer_conversion(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 3; ++i) {
    double source = asdouble(inbuf.Load(i * 8), inbuf.Load(i * 8 + 4));
    uint unsigned_value = (uint)source;
    int signed_value = (int)source;
    uint unsigned_low = 0;
    uint unsigned_high = 0;
    uint signed_low = 0;
    uint signed_high = 0;
    asuint((double)unsigned_value, unsigned_low, unsigned_high);
    asuint((double)signed_value, signed_low, signed_high);
    outbuf.Store(i * 24, unsigned_value);
    outbuf.Store(i * 24 + 4, asuint(signed_value));
    outbuf.Store(i * 24 + 8, unsigned_low);
    outbuf.Store(i * 24 + 12, unsigned_high);
    outbuf.Store(i * 24 + 16, signed_low);
    outbuf.Store(i * 24 + 20, signed_high);
  }
}

[numthreads(1, 1, 1)]
void cs_float_to_double(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    float source = asfloat(inbuf.Load(i * 4));
    uint low = 0;
    uint high = 0;
    asuint((double)source, low, high);
    outbuf.Store(i * 8, low);
    outbuf.Store(i * 8 + 4, high);
  }
}

[numthreads(1, 1, 1)]
void cs_double_unary(uint3 id : SV_DispatchThreadID) {
  double source = asdouble(inbuf.Load(0), inbuf.Load(4));
  double absolute = abs(source);
  double minimum = min(source, 2.0);
  double maximum = max(source, -2.0);
  uint low = 0;
  uint high = 0;
  asuint(absolute, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
  asuint(minimum, low, high);
  outbuf.Store(8, low);
  outbuf.Store(12, high);
  asuint(maximum, low, high);
  outbuf.Store(16, low);
  outbuf.Store(20, high);
}

[numthreads(1, 1, 1)]
void cs_double_unary_extended(uint3 id : SV_DispatchThreadID) {
  double source = asdouble(inbuf.Load(0), inbuf.Load(4));
  double sqrt_value = sqrt(source);
  double rsqrt_value = rsqrt(source);
  double trunc_value = trunc(source);
  double floor_value = floor(source);
  double ceil_value = ceil(source);
  double round_value = round(source);
  double frac_value = frac(source);
  uint low = 0;
  uint high = 0;
  asuint(sqrt_value, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
  asuint(rsqrt_value, low, high);
  outbuf.Store(8, low);
  outbuf.Store(12, high);
  asuint(trunc_value, low, high);
  outbuf.Store(16, low);
  outbuf.Store(20, high);
  asuint(floor_value, low, high);
  outbuf.Store(24, low);
  outbuf.Store(28, high);
  asuint(ceil_value, low, high);
  outbuf.Store(32, low);
  outbuf.Store(36, high);
  asuint(round_value, low, high);
  outbuf.Store(40, low);
  outbuf.Store(44, high);
  asuint(frac_value, low, high);
  outbuf.Store(48, low);
  outbuf.Store(52, high);
}

[numthreads(1, 1, 1)]
void cs_double_predicates(uint3 id : SV_DispatchThreadID) {
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    double source = asdouble(inbuf.Load(i * 8), inbuf.Load(i * 8 + 4));
    outbuf.Store(i * 16, isnan(source) ? 1u : 0u);
    outbuf.Store(i * 16 + 4, isinf(source) ? 1u : 0u);
    outbuf.Store(i * 16 + 8, isfinite(source) ? 1u : 0u);
    outbuf.Store(i * 16 + 12, isnormal(source) ? 1u : 0u);
  }
}

[numthreads(1, 1, 1)]
void cs_double_fma(uint3 id : SV_DispatchThreadID) {
  double lhs = asdouble(inbuf.Load(0), inbuf.Load(4));
  double rhs = asdouble(inbuf.Load(8), inbuf.Load(12));
  double addend = asdouble(inbuf.Load(16), inbuf.Load(20));
  double result = fma(lhs, rhs, addend);
  uint low = 0;
  uint high = 0;
  asuint(result, low, high);
  outbuf.Store(0, low);
  outbuf.Store(4, high);
}

[numthreads(1, 1, 1)]
void cs_raw_vector(uint3 id : SV_DispatchThreadID) {
  uint4 values = inbuf.Load4(0);
  outbuf.Store4(0, values);
}

[numthreads(1, 1, 1)]
void cs_vector_reductions(uint3 id : SV_DispatchThreadID) {
  uint4 all_true = uint4(1u, 2u, 3u, 4u);
  uint4 mixed = uint4(0u, 2u, 0u, 4u);
  outbuf.Store(0, all(all_true != 0u) ? 1u : 0u);
  outbuf.Store(4, any(all_true != 0u) ? 1u : 0u);
  outbuf.Store(8, all(mixed != 0u) ? 1u : 0u);
  outbuf.Store(12, any(mixed != 0u) ? 1u : 0u);
}

[numthreads(1, 1, 1)]
void cs_pack_unpack_8(uint3 id : SV_DispatchThreadID) {
  uint packed_u = pack_u8(uint4(1u, 258u, 65535u, 0xffffffffu));
  uint packed_s = pack_s8(int4(-1, 127, -128, 128));
  uint packed_uc = pack_clamp_u8(int4(-1, 127, 256, 2));
  uint packed_sc = pack_clamp_s8(int4(-200, -128, 127, 200));
  uint4 unpack_u32 = unpack_u8u32(packed_u);
  int4 unpack_s32 = unpack_s8s32(packed_s);
  uint4 unpack_u16 = unpack_u8u16(packed_u);
  int4 unpack_s16 = unpack_s8s16(packed_s);
  outbuf.Store(0, packed_u);
  outbuf.Store(4, packed_s);
  outbuf.Store(8, packed_uc);
  outbuf.Store(12, packed_sc);
  outbuf.Store(16, unpack_u32.x);
  outbuf.Store(20, unpack_u32.y);
  outbuf.Store(24, unpack_u32.z);
  outbuf.Store(28, unpack_u32.w);
  outbuf.Store(32, asuint(unpack_s32.x));
  outbuf.Store(36, asuint(unpack_s32.y));
  outbuf.Store(40, asuint(unpack_s32.z));
  outbuf.Store(44, asuint(unpack_s32.w));
  outbuf.Store(48, uint(unpack_u16.x));
  outbuf.Store(52, uint(unpack_u16.y));
  outbuf.Store(56, uint(unpack_u16.z));
  outbuf.Store(60, uint(unpack_u16.w));
  outbuf.Store(64, asuint(int(unpack_s16.x)));
  outbuf.Store(68, asuint(int(unpack_s16.y)));
  outbuf.Store(72, asuint(int(unpack_s16.z)));
  outbuf.Store(76, asuint(int(unpack_s16.w)));
}
HLSL_DOUBLE_BITCAST

  local sm69_hlsl="$SDK_DIR/out/bin/probe_dxil_semantic_sm69.hlsl"
  cat > "$sm69_hlsl" <<'HLSL_SM69'
RWByteAddressBuffer outbuf : register(u0);
ByteAddressBuffer inbuf : register(t0);

[numthreads(4, 1, 1)]
void cs_sm69(uint3 id : SV_DispatchThreadID) {
  float16_t value = float16_t(69.0) + float16_t(id.x) * float16_t(1.5);
  outbuf.Store(id.x * 4, uint(value));
}

[numthreads(4, 1, 1)]
void cs_sm69_native16(uint3 id : SV_DispatchThreadID) {
  uint16_t unsigned_value = uint16_t(65000u) + uint16_t(id.x * 3u);
  int16_t signed_value = int16_t(-120) + int16_t(id.x * 7u);
  float16_t float_value = float16_t(2.5) + float16_t(id.x) * float16_t(0.5);
  uint result = uint(unsigned_value) + uint(signed_value + int16_t(200)) +
                uint(float_value * float16_t(2.0));
  outbuf.Store(id.x * 4, result);
}

[numthreads(1, 1, 1)]
void cs_sm69_native16_math(uint3 id : SV_DispatchThreadID) {
  float16_t value = float16_t(4.0);
  float16_t root = sqrt(value);
  float16_t reciprocal_root = rsqrt(value);
  float16_t magnitude = abs(-float16_t(3.5));
  float16_t minimum = min(float16_t(1.5), float16_t(2.5));
  float16_t maximum = max(float16_t(1.5), float16_t(2.5));
  uint result = uint(root) + uint(reciprocal_root * float16_t(4.0)) +
                uint(magnitude) + uint(minimum) + uint(maximum);
  outbuf.Store(0, result);
}

[numthreads(1, 1, 1)]
void cs_sm69_fdot_wide(uint3 id : SV_DispatchThreadID) {
  vector<float, 8> a = vector<float, 8>(1.0f, 2.0f, 3.0f, 4.0f,
                                         5.0f, 6.0f, 7.0f, 8.0f);
  vector<float, 8> b = vector<float, 8>(8.0f, 7.0f, 6.0f, 5.0f,
                                         4.0f, 3.0f, 2.0f, 1.0f);
  outbuf.Store(0, asuint(dot(a, b)));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_arithmetic(uint3 id : SV_DispatchThreadID) {
  vector<float, 8> a = vector<float, 8>(1.0f, 2.0f, 3.0f, 4.0f,
                                         5.0f, 6.0f, 7.0f, 8.0f);
  vector<float, 8> b = vector<float, 8>(8.0f, 7.0f, 6.0f, 5.0f,
                                         4.0f, 3.0f, 2.0f, 1.0f);
  vector<float, 8> sum = a + b;
  float total = sum[0] + sum[1] + sum[2] + sum[3] +
                sum[4] + sum[5] + sum[6] + sum[7];
  outbuf.Store(0, asuint(total));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_integer(uint3 id : SV_DispatchThreadID) {
  vector<uint, 8> a = vector<uint, 8>(1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u);
  vector<uint, 8> b = vector<uint, 8>(8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u);
  vector<uint, 8> mixed = a ^ b;
  vector<uint, 8> inverted = ~a;
  uint total = mixed[0] + mixed[1] + mixed[2] + mixed[3] +
               mixed[4] + mixed[5] + mixed[6] + mixed[7];
  uint inverted_total = inverted[0] + inverted[1] + inverted[2] + inverted[3] +
                        inverted[4] + inverted[5] + inverted[6] + inverted[7];
  outbuf.Store(0, total);
  outbuf.Store(4, inverted_total);
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_dynamic(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> a = vector<uint, 8>(seed + 1u, seed + 2u, seed + 3u, seed + 4u,
                                      seed + 5u, seed + 6u, seed + 7u, seed + 8u);
  vector<uint, 8> b = vector<uint, 8>(8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u);
  vector<uint, 8> sum = a + b;
  uint total = sum[0] + sum[1] + sum[2] + sum[3] +
               sum[4] + sum[5] + sum[6] + sum[7];
  outbuf.Store(0, total);
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_dynamic_float(uint3 id : SV_DispatchThreadID) {
  float seed = float(inbuf.Load(0));
  vector<float, 8> a = vector<float, 8>(seed + 1.0f, seed + 2.0f,
                                         seed + 3.0f, seed + 4.0f,
                                         seed + 5.0f, seed + 6.0f,
                                         seed + 7.0f, seed + 8.0f);
  vector<float, 8> b = vector<float, 8>(8.0f, 7.0f, 6.0f, 5.0f,
                                         4.0f, 3.0f, 2.0f, 1.0f);
  outbuf.Store(0, asuint(dot(a, b)));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_conversion(uint3 id : SV_DispatchThreadID) {
  vector<int, 8> signed_values = vector<int, 8>(-4, -3, -2, -1, 0, 1, 2, 3);
  vector<float, 8> float_values = vector<float, 8>(
      (float)signed_values[0], (float)signed_values[1],
      (float)signed_values[2], (float)signed_values[3],
      (float)signed_values[4], (float)signed_values[5],
      (float)signed_values[6], (float)signed_values[7]);
  vector<uint, 8> uint_values = vector<uint, 8>(
      (uint)signed_values[0], (uint)signed_values[1],
      (uint)signed_values[2], (uint)signed_values[3],
      (uint)signed_values[4], (uint)signed_values[5],
      (uint)signed_values[6], (uint)signed_values[7]);
  float float_total = float_values[0] + float_values[1] +
                      float_values[2] + float_values[3] +
                      float_values[4] + float_values[5] +
                      float_values[6] + float_values[7];
  uint uint_total = uint_values[0] + uint_values[1] + uint_values[2] + uint_values[3] +
                    uint_values[4] + uint_values[5] + uint_values[6] + uint_values[7];
  outbuf.Store(0, asuint(float_total));
  outbuf.Store(4, uint_total);
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_reduce(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> values = vector<uint, 8>(seed + 1u, seed + 2u,
                                           seed + 3u, seed + 4u,
                                           seed + 5u, seed + 6u,
                                           seed + 7u, seed + 8u);
  bool all_set = all(values != 0u);
  bool any_set = any(values != 0u);
  outbuf.Store(0, all_set ? 1u : 0u);
  outbuf.Store(4, any_set ? 1u : 0u);
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_unary(uint3 id : SV_DispatchThreadID) {
  float seed = float(inbuf.Load(0));
  vector<float, 8> a = vector<float, 8>(seed + 1.0f, seed + 2.0f,
                                         seed + 3.0f, seed + 4.0f,
                                         seed + 5.0f, seed + 6.0f,
                                         seed + 7.0f, seed + 8.0f);
  vector<float, 8> magnitude = abs(-a);
  float total = magnitude[0] + magnitude[1] + magnitude[2] + magnitude[3] +
                magnitude[4] + magnitude[5] + magnitude[6] + magnitude[7];
  vector<float, 8> squares = vector<float, 8>(1.0f, 4.0f, 9.0f, 16.0f,
                                                25.0f, 36.0f, 49.0f, 64.0f);
  vector<float, 8> roots = sqrt(squares);
  float root_total = roots[0] + roots[1] + roots[2] + roots[3] +
                     roots[4] + roots[5] + roots[6] + roots[7];
  vector<float, 8> ones = vector<float, 8>(1.0f, 1.0f, 1.0f, 1.0f,
                                            1.0f, 1.0f, 1.0f, 1.0f);
  vector<float, 8> reciprocal_roots = rsqrt(ones);
  float reciprocal_total = reciprocal_roots[0] + reciprocal_roots[1] +
                            reciprocal_roots[2] + reciprocal_roots[3] +
                            reciprocal_roots[4] + reciprocal_roots[5] +
                            reciprocal_roots[6] + reciprocal_roots[7];
  vector<float, 8> logarithms = log2(ones);
  vector<float, 8> powers = exp2(logarithms);
  float power_total = powers[0] + powers[1] + powers[2] + powers[3] +
                      powers[4] + powers[5] + powers[6] + powers[7];
  vector<float, 8> zeros = vector<float, 8>(seed - seed, seed - seed,
                                              seed - seed, seed - seed,
                                              seed - seed, seed - seed,
                                              seed - seed, seed - seed);
  vector<float, 8> sines = sin(zeros);
  vector<float, 8> cosines = cos(zeros);
  float sine_total = sines[0] + sines[1] + sines[2] + sines[3] +
                     sines[4] + sines[5] + sines[6] + sines[7];
  float cosine_total = cosines[0] + cosines[1] + cosines[2] + cosines[3] +
                       cosines[4] + cosines[5] + cosines[6] + cosines[7];
  outbuf.Store(0, asuint(total));
  outbuf.Store(4, asuint(root_total));
  outbuf.Store(8, asuint(reciprocal_total));
  outbuf.Store(12, asuint(power_total));
  outbuf.Store(16, asuint(sine_total));
  outbuf.Store(20, asuint(cosine_total));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_minmax(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> a = vector<uint, 8>(seed + 1u, seed + 2u, seed + 3u, seed + 4u,
                                      seed + 5u, seed + 6u, seed + 7u, seed + 8u);
  vector<uint, 8> b = vector<uint, 8>(8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u);
  vector<uint, 8> lo = min(a, b);
  vector<uint, 8> hi = max(a, b);
  uint total_lo = lo[0] + lo[1] + lo[2] + lo[3] + lo[4] + lo[5] + lo[6] + lo[7];
  uint total_hi = hi[0] + hi[1] + hi[2] + hi[3] + hi[4] + hi[5] + hi[6] + hi[7];
  vector<float, 8> float_a = vector<float, 8>(1.0f, 2.0f, 3.0f, 4.0f,
                                                5.0f, 6.0f, 7.0f, 8.0f);
  vector<float, 8> float_b = vector<float, 8>(8.0f, 7.0f, 6.0f, 5.0f,
                                                4.0f, 3.0f, 2.0f, 1.0f);
  vector<float, 8> float_lo = min(float_a, float_b);
  vector<float, 8> float_hi = max(float_a, float_b);
  float float_total_lo = float_lo[0] + float_lo[1] + float_lo[2] + float_lo[3] +
                         float_lo[4] + float_lo[5] + float_lo[6] + float_lo[7];
  float float_total_hi = float_hi[0] + float_hi[1] + float_hi[2] + float_hi[3] +
                         float_hi[4] + float_hi[5] + float_hi[6] + float_hi[7];
  outbuf.Store(0, total_lo);
  outbuf.Store(4, total_hi);
  outbuf.Store(8, asuint(float_total_lo));
  outbuf.Store(12, asuint(float_total_hi));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_tertiary(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> a = vector<uint, 8>(seed + 1u, seed + 2u, seed + 3u, seed + 4u,
                                      seed + 5u, seed + 6u, seed + 7u, seed + 8u);
  vector<uint, 8> factor = vector<uint, 8>(2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u);
  vector<uint, 8> addend = vector<uint, 8>(1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u);
  vector<uint, 8> value = mad(a, factor, addend);
  uint total = value[0] + value[1] + value[2] + value[3] +
               value[4] + value[5] + value[6] + value[7];
  vector<float, 8> float_a = vector<float, 8>(1.0f, 2.0f, 3.0f, 4.0f,
                                                5.0f, 6.0f, 7.0f, 8.0f);
  vector<float, 8> float_factor = vector<float, 8>(2.0f, 2.0f, 2.0f, 2.0f,
                                                    2.0f, 2.0f, 2.0f, 2.0f);
  vector<float, 8> float_addend = vector<float, 8>(1.0f, 1.0f, 1.0f, 1.0f,
                                                    1.0f, 1.0f, 1.0f, 1.0f);
  vector<float, 8> float_value = mad(float_a, float_factor, float_addend);
  float float_total = float_value[0] + float_value[1] +
                      float_value[2] + float_value[3] +
                      float_value[4] + float_value[5] +
                      float_value[6] + float_value[7];
  outbuf.Store(0, total);
  outbuf.Store(4, asuint(float_total));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_divide_shift(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> a = vector<uint, 8>(seed + 1u, seed + 2u, seed + 3u, seed + 4u,
                                      seed + 5u, seed + 6u, seed + 7u, seed + 8u);
  vector<uint, 8> two = vector<uint, 8>(2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u);
  vector<uint, 8> three = vector<uint, 8>(3u, 3u, 3u, 3u, 3u, 3u, 3u, 3u);
  vector<uint, 8> quotient = a / two;
  vector<uint, 8> shifted = a << 1u;
  vector<uint, 8> shifted_right = a >> 1u;
  vector<uint, 8> remainder = a % three;
  uint sum_quotient = quotient[0] + quotient[1] + quotient[2] + quotient[3] +
                       quotient[4] + quotient[5] + quotient[6] + quotient[7];
  uint sum_shifted = shifted[0] + shifted[1] + shifted[2] + shifted[3] +
                     shifted[4] + shifted[5] + shifted[6] + shifted[7];
  uint sum_remainder = remainder[0] + remainder[1] + remainder[2] + remainder[3] +
                       remainder[4] + remainder[5] + remainder[6] + remainder[7];
  uint sum_shifted_right = shifted_right[0] + shifted_right[1] +
                           shifted_right[2] + shifted_right[3] +
                           shifted_right[4] + shifted_right[5] +
                           shifted_right[6] + shifted_right[7];
  vector<int, 8> signed_a = vector<int, 8>(-8, -7, -6, -5, -4, -3, -2, -1);
  vector<int, 8> signed_two = vector<int, 8>(2, 2, 2, 2, 2, 2, 2, 2);
  vector<int, 8> signed_three = vector<int, 8>(3, 3, 3, 3, 3, 3, 3, 3);
  vector<int, 8> signed_quotient = signed_a / signed_two;
  vector<int, 8> signed_shifted = signed_a >> 1;
  vector<int, 8> signed_remainder = signed_a % signed_three;
  int signed_sum_quotient = signed_quotient[0] + signed_quotient[1] + signed_quotient[2] + signed_quotient[3] +
                             signed_quotient[4] + signed_quotient[5] + signed_quotient[6] + signed_quotient[7];
  int signed_sum_shifted = signed_shifted[0] + signed_shifted[1] + signed_shifted[2] + signed_shifted[3] +
                           signed_shifted[4] + signed_shifted[5] + signed_shifted[6] + signed_shifted[7];
  int signed_sum_remainder = signed_remainder[0] + signed_remainder[1] + signed_remainder[2] + signed_remainder[3] +
                             signed_remainder[4] + signed_remainder[5] + signed_remainder[6] + signed_remainder[7];
  outbuf.Store(0, sum_quotient);
  outbuf.Store(4, sum_shifted);
  outbuf.Store(8, sum_remainder);
  outbuf.Store(12, sum_shifted_right);
  outbuf.Store(16, asuint(signed_sum_quotient));
  outbuf.Store(20, asuint(signed_sum_shifted));
  outbuf.Store(24, asuint(signed_sum_remainder));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_float_remainder(uint3 id : SV_DispatchThreadID) {
  vector<float, 8> values = vector<float, 8>(2.0f, 3.0f, 4.0f, 5.0f,
                                               6.0f, 7.0f, 8.0f, 9.0f);
  vector<float, 8> divisors = vector<float, 8>(2.0f, 2.0f, 2.0f, 2.0f,
                                                2.0f, 2.0f, 2.0f, 2.0f);
  vector<float, 8> remainders = fmod(values, divisors);
  float total = remainders[0] + remainders[1] + remainders[2] + remainders[3] +
                remainders[4] + remainders[5] + remainders[6] + remainders[7];
  outbuf.Store(0, asuint(total));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_select(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 8> a = vector<uint, 8>(seed + 1u, seed + 2u, seed + 3u, seed + 4u,
                                      seed + 5u, seed + 6u, seed + 7u, seed + 8u);
  vector<uint, 8> b = vector<uint, 8>(8u, 7u, 6u, 5u, 4u, 3u, 2u, 1u);
  vector<uint, 8> selected = select(a > b, a, b);
  uint total = selected[0] + selected[1] + selected[2] + selected[3] +
               selected[4] + selected[5] + selected[6] + selected[7];
  vector<float, 8> fa = vector<float, 8>(1.0f, 2.0f, 3.0f, 4.0f,
                                         5.0f, 6.0f, 7.0f, 8.0f);
  vector<float, 8> fb = vector<float, 8>(8.0f, 7.0f, 6.0f, 5.0f,
                                         4.0f, 3.0f, 2.0f, 1.0f);
  vector<float, 8> selected_float = select(fa > fb, fa, fb);
  float total_float = selected_float[0] + selected_float[1] +
                      selected_float[2] + selected_float[3] +
                      selected_float[4] + selected_float[5] +
                      selected_float[6] + selected_float[7];
  outbuf.Store(0, total);
  outbuf.Store(4, asuint(total_float));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_16(uint3 id : SV_DispatchThreadID) {
  vector<float, 16> a = vector<float, 16>(1.0f, 2.0f, 3.0f, 4.0f,
                                           5.0f, 6.0f, 7.0f, 8.0f,
                                           9.0f, 10.0f, 11.0f, 12.0f,
                                           13.0f, 14.0f, 15.0f, 16.0f);
  vector<float, 16> b = vector<float, 16>(16.0f, 15.0f, 14.0f, 13.0f,
                                           12.0f, 11.0f, 10.0f, 9.0f,
                                           8.0f, 7.0f, 6.0f, 5.0f,
                                           4.0f, 3.0f, 2.0f, 1.0f);
  outbuf.Store(0, asuint(dot(a, b)));
}

[numthreads(1, 1, 1)]
void cs_sm69_long_vector_16_dynamic(uint3 id : SV_DispatchThreadID) {
  uint seed = inbuf.Load(0);
  vector<uint, 16> a = vector<uint, 16>(
      seed + 1u, seed + 2u, seed + 3u, seed + 4u,
      seed + 5u, seed + 6u, seed + 7u, seed + 8u,
      seed + 9u, seed + 10u, seed + 11u, seed + 12u,
      seed + 13u, seed + 14u, seed + 15u, seed + 16u);
  vector<uint, 16> doubled = a + a;
  uint total = doubled[0] + doubled[1] + doubled[2] + doubled[3] +
               doubled[4] + doubled[5] + doubled[6] + doubled[7] +
               doubled[8] + doubled[9] + doubled[10] + doubled[11] +
               doubled[12] + doubled[13] + doubled[14] + doubled[15];
  outbuf.Store(0, total);
}
HLSL_SM69

  local texture_hlsl="$SDK_DIR/out/bin/probe_dxil_semantic_texture_ops.hlsl"
  cat > "$texture_hlsl" <<'HLSL_TEXTURE'
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
RWByteAddressBuffer outbuf : register(u0);

[numthreads(4, 1, 1)]
void cs_texture_ops(uint3 id : SV_DispatchThreadID) {
  float2 uv = float2(0.5, 0.5);
  float4 loaded = tex.Load(int3(1, 1, 0));
  float4 sampled = tex.SampleLevel(smp, uv, 0.0);
  float4 gradient = tex.SampleGrad(smp, uv, float2(0.0, 0.0), float2(0.0, 0.0));
  float4 biased = tex.SampleBias(smp, uv, 1.0);
  float4 gathered = tex.GatherRed(smp, uv);
  uint width = 0;
  uint height = 0;
  tex.GetDimensions(width, height);
  if (id.x == 0) {
    outbuf.Store(0, uint(loaded.r * 255.0 + 0.5));
    outbuf.Store(4, uint(sampled.r * 255.0 + 0.5));
    outbuf.Store(8, uint(gradient.r * 255.0 + 0.5));
    outbuf.Store(12, uint(biased.r * 255.0 + 0.5));
    outbuf.Store(16, uint(gathered.x * 255.0 + 0.5));
    outbuf.Store(20, width + height * 16);
  }
}
HLSL_TEXTURE

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_math_bits -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_math_bits.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_math_intrinsics -T cs_6_0 -HV 2021 -Od \
      -Fo probe_dxil_semantic_math_intrinsics.cso probe_dxil_semantics.hlsl >/dev/null
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_math_extended -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_math_extended.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_core_opcode_matrix -T cs_6_0 -HV 2021 -Od \
      -Fo probe_dxil_semantic_core_opcode_matrix.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_dot4add_unsigned -T cs_6_4 -HV 2021 \
      -Fo probe_dxil_semantic_dot4add_unsigned.cso probe_dxil_semantics.hlsl >/dev/null
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_dot4add_signed -T cs_6_4 -HV 2021 \
      -Fo probe_dxil_semantic_dot4add_signed.cso probe_dxil_semantics.hlsl >/dev/null
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_dot2add_half -T cs_6_4 -HV 2021 -enable-16bit-types \
      -Fo probe_dxil_semantic_dot2add_half.cso probe_dxil_semantics.hlsl >/dev/null
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_special_float -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_special_float.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_buffer -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_buffer.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_atomic_uav -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_atomic_uav.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_atomic_matrix -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_atomic_matrix.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_vector_shuffle -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_vector_shuffle.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_matrix_aggregate -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_matrix_aggregate.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_helper_aggregate -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_helper_aggregate.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_atomics_ids -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_atomics_ids.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_wave_quad -T cs_6_6 -HV 2021 \
      -Fo probe_dxil_semantic_wave_quad.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm67 -T cs_6_7 -HV 2021 \
      -Fo probe_dxil_semantic_sm67.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm68 -T cs_6_8 -HV 2021 \
      -Fo probe_dxil_semantic_sm68.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_fdot_sm69 -T cs_6_9 -HV 2021 \
      -Fo probe_dxil_semantic_fdot_sm69.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_control_aggregate -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_control_aggregate.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_loop_aggregate -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_loop_aggregate.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_arithmetic -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_arithmetic.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_bitcast -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_bitcast.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_dynamic_arithmetic -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_dynamic_arithmetic.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_add_matrix -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_add_matrix.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_multiply -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_multiply.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_divide -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_divide.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_remainder -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_remainder.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_compare -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_compare.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_multiply_matrix -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_multiply_matrix.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_divide_matrix -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_divide_matrix.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_compare_matrix -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_compare_matrix.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_float_conversion -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_float_conversion.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_integer_conversion -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_integer_conversion.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_float_to_double -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_float_to_double.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_unary -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_unary.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_unary_extended -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_unary_extended.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_predicates -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_predicates.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_double_fma -T cs_6_0 -HV 2021 \
      -Fo probe_dxil_semantic_double_fma.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_pack_unpack_8 -T cs_6_6 -HV 2021 \
      -Fo probe_dxil_semantic_pack_unpack_8.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_vector_reductions -T cs_6_9 -HV 2021 \
      -Fo probe_dxil_semantic_vector_reductions.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_raw_vector -T cs_6_9 -HV 2021 \
      -Fo probe_dxil_semantic_raw_vector.cso probe_dxil_semantic_double_bitcast.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69 -T cs_6_9 -HV 2021 -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_native16 -T cs_6_9 -HV 2021 -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_native16.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_native16_math -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_native16_math.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_fdot_wide -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_fdot_wide.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_arithmetic -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_arithmetic.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_integer -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_integer.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_dynamic -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_dynamic.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_dynamic_float -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_dynamic_float.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_conversion -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_conversion.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_reduce -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_reduce.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_unary -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_unary.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_minmax -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_minmax.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_tertiary -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_tertiary.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_divide_shift -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_divide_shift.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_float_remainder -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_float_remainder.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_select -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_select.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_16 -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_16.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_sm69_long_vector_16_dynamic -T cs_6_9 -HV 2021 -Od -enable-16bit-types \
      -Fo probe_dxil_semantic_sm69_long_vector_16_dynamic.cso probe_dxil_semantic_sm69.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_texture_ops -T cs_6_9 -HV 2021 \
      -Fo probe_dxil_semantic_texture_ops.cso probe_dxil_semantic_texture_ops.hlsl >/dev/null
  )

  mkdir -p "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_DXIL_SEMANTICS_MODE="warmup" \
    "$WINE_BIN" "$DXIL_SEMANTICS_PROBE_EXE" > "$DXIL_SEMANTICS_WARMUP_RESULT_FILE"
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
}

prepare_texture_dimension_probes() {
  local wineserver_bin="$(dirname "$WINE_BIN")/wineserver"
  METALSHARP_WINE_BIN="$WINE_BIN" \
  METALSHARP_WINESERVER_BIN="$wineserver_bin" \
    "$SDK_DIR/scripts/prepare-texture-dimension-shaders.sh"
  # Refresh Wine's Z:-drive directory cache so a caller-supplied prefix sees
  # the newly generated shader files. Isolated runs prepare before wineboot
  # and skip this fallback entirely.
  WINEPREFIX="$WINE_PREFIX" "$wineserver_bin" -k >/dev/null 2>&1 || true
  WINEPREFIX="$WINE_PREFIX" "$wineserver_bin" -w >/dev/null 2>&1 || true
}

SOURCE_COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || printf 'unknown')"
SOURCE_TREE_SHA256="$(python3 - "$ROOT_DIR" <<'PY'
import hashlib
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
try:
    listed = subprocess.check_output(
        ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard",
         "vendor/dxmt", "tools/d3d12-metal-sdk"],
        text=True,
    ).splitlines()
except (OSError, subprocess.CalledProcessError):
    listed = []

def is_source_path(relative):
    return not (
        relative.startswith("vendor/dxmt/build-")
        or relative.startswith("tools/d3d12-metal-sdk/results/")
        or relative.startswith("tools/d3d12-metal-sdk/out/")
        or relative.startswith("tools/d3d12-metal-sdk/cache/")
        or "/__pycache__/" in relative
        or relative.endswith("/__pycache__")
        or relative.endswith(".pyc")
    )

digest = hashlib.sha256()
for relative in sorted(path for path in listed if is_source_path(path) and (root / path).is_file()):
    path = root / relative
    digest.update(relative.encode("utf-8"))
    digest.update(b"\0")
    digest.update(path.read_bytes())
    digest.update(b"\0")
print(digest.hexdigest())
PY
)"
# Generated build outputs, probe results, and shader caches are deliberately
# excluded from source identity.  Otherwise a clean source checkout appears
# dirty merely because the manifest-driven build produced tracked PE/Unix
# artifacts, weakening the provenance check in the aggregate gate.
SOURCE_DIRTY="$(python3 - "$ROOT_DIR" <<'PY'
import subprocess
import sys

root = sys.argv[1]
status = subprocess.check_output(
    ["git", "-C", root, "status", "--porcelain", "--untracked-files=all",
     "--", "vendor/dxmt", "tools/d3d12-metal-sdk"],
    text=True,
)

def generated(path):
    return (
        path.startswith("vendor/dxmt/build-")
        or path.startswith("tools/d3d12-metal-sdk/results/")
        or path.startswith("tools/d3d12-metal-sdk/out/")
        or path.startswith("tools/d3d12-metal-sdk/cache/")
        or "/__pycache__/" in path
        or path.endswith("/__pycache__")
        or path.endswith(".pyc")
    )

changed = []
for line in status.splitlines():
    if len(line) >= 4:
        path = line[3:]
        if " -> " in path:
            path = path.rsplit(" -> ", 1)[-1]
        if not generated(path):
            changed.append(path)
print("true" if changed else "false")
PY
)"
RUN_STARTED_AT="$(date +%s)"

cat > "$RESULTS_DIR/host-runtime-${PROFILE}.json" <<EOF
{
  "schema": "metalsharp.d3d12-metal.host-runtime.v1",
  "profile": "$PROFILE",
  "source_commit": "$SOURCE_COMMIT",
  "source_tree_sha256": "$SOURCE_TREE_SHA256",
  "source_dirty": $SOURCE_DIRTY,
  "run_started_at": $RUN_STARTED_AT,
  "wine": "$REAL_WINE_BIN",
  "prefix": "$WINE_PREFIX",
  "dxmt_runtime": "$DXMT_RUNTIME",
  "windows_runtime": "$WINDOWS_DIR",
  "game_dir": "$GAME_DIR",
  "ms_root": "${MS_ROOT:-}",
  "unix_runtime": "$UNIX_DIR",
  "wine_runtime": "$WINE_RUNTIME_ROOT",
  "wine_unix_runtime": "$WINE_UNIX_DIR",
  "dyld_runtime_path": "$DXMT_DYLD_LIBRARY_PATH",
  "shader_cache": "$SHADER_CACHE_DIR",
  "winemetal_unixlib": "$DXMT_WINEMETAL_UNIXLIB_NAME",
  "winemetal_so": "$UNIX_DIR/winemetal.so",
  "winemetal_abi_contract": "$SDK_DIR/contracts/winemetal-bridge-contract.json",
  "winemetal_abi_result": "$WINEMETAL_ABI_RESULT_FILE",
  "required_windows_dlls": [
    "$WINDOWS_DIR/d3d12.dll",
    "$WINDOWS_DIR/dxgi.dll",
    "$WINDOWS_DIR/dxgi_dxmt.dll",
    "$WINDOWS_DIR/d3d11.dll",
    "$WINDOWS_DIR/d3d10core.dll",
    "$WINDOWS_DIR/winemetal.dll"
  ]
}
EOF

if [[ "$RUN_WINEMETAL_ABI" == "1" ]]; then
  python3 "$SDK_DIR/scripts/check-winemetal-abi.py" \
    --profile "$PROFILE" \
    --dxmt-runtime "$DXMT_RUNTIME" \
    --wine-runtime "$WINE_RUNTIME_ROOT" \
    --prefix "$WINE_PREFIX" \
    --results-dir "$RESULTS_DIR"
fi

if [[ "$RUN_MINI" == "1" ]]; then
  if mini_probe_selected dxil_texture_color_output; then
    prepare_dxil_color_probe
  fi
  if mini_probe_selected mesh_object_shader_pso; then
    prepare_mesh_shader_probe
  fi
  if mini_probe_selected dxr_acceleration_structures; then
    prepare_dxr_acceleration_structure_probe
  fi
  if mini_probe_selected subnautica_geometry_dxil_replay; then
    prepare_geometry_replay_corpus
  fi
  if mini_probe_selected dxr_inline; then
    DXR_INLINE_ONLY=1 prepare_dxr_acceleration_structure_probe
  fi
  if mini_probe_selected start_draw_info; then
    prepare_start_draw_info_probe
  fi
  if mini_probe_selected inner_coverage; then
    prepare_inner_coverage_probe
  fi
  if mini_probe_selected view_id_instancing; then
    prepare_view_id_instancing_probe
  fi
  if mini_probe_selected temp_registers; then
    prepare_temp_register_probe
  fi
fi
if [[ "$RUN_COMMAND_REPLAY" == "1" ]]; then
  prepare_command_replay_advanced_probes
fi

if [[ "$RUN_LOADER" == "1" ]]; then
  # DXMT is shipped as PE DLLs; native-first avoids Wine resolving stale builtin shims.
  WINEPREFIX="$WINE_PREFIX" \
  WINEDLLPATH="$PROBE_WINEDLLPATH" \
  WINEDLLOVERRIDES="$DLL_OVERRIDES" \
  DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
  DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
  D3D12_METAL_SDK_PROFILE="$PROFILE" \
  D3D12_METAL_SDK_EXPECT_WINDOWS_SUBSTR="$(basename "$SDK_DIR")" \
  "$WINE_BIN" "$PROBE_EXE" > "$RESULT_FILE"
  echo "$RESULT_FILE"
fi

if [[ "$RUN_AGILITY" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_EXPECT_WINDOWS_SUBSTR="$(basename "$SDK_DIR")" \
    D3D12_METAL_SDK_AGILITY_VERSION="$AGILITY_SDK_VERSION" \
    D3D12_METAL_SDK_AGILITY_PATH="$AGILITY_SDK_PATH" \
    "$WINE_BIN" "$AGILITY_PROBE_EXE" > "$AGILITY_RESULT_FILE"
  )
  echo "$AGILITY_RESULT_FILE"
fi

# Feature-policy probes can intentionally return nonzero while still emitting useful
# JSON. Keep collecting the rest of the matrix and let compare-contract decide pass/fail.
set +e

if [[ "$RUN_LEGACY_REGRESSION" == "1" ]]; then
  run_probe_exe \
    "$LEGACY_REGRESSION_PROBE_EXE" \
    "$LEGACY_REGRESSION_RESULT_FILE"
fi

if [[ "$RUN_CAPS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_EXPECT_WINDOWS_SUBSTR="system32" \
    D3D12_METAL_SDK_AGILITY_VERSION="$AGILITY_SDK_VERSION" \
    D3D12_METAL_SDK_AGILITY_PATH="$AGILITY_SDK_PATH" \
    "$WINE_BIN" "$CAPS_PROBE_EXE" > "$CAPS_RESULT_FILE"
  )
  echo "$CAPS_RESULT_FILE"
fi

if [[ "$RUN_FEATURE_LEVELS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_AGILITY_VERSION="$AGILITY_SDK_VERSION" \
    D3D12_METAL_SDK_AGILITY_PATH="$AGILITY_SDK_PATH" \
    "$WINE_BIN" "$FEATURE_LEVELS_PROBE_EXE" > "$FEATURE_LEVELS_RESULT_FILE"
  )
  echo "$FEATURE_LEVELS_RESULT_FILE"
fi

if [[ "$RUN_OBJECT_CONTRACTS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$OBJECT_CONTRACTS_PROBE_EXE" > "$OBJECT_CONTRACTS_RESULT_FILE"
  )
  echo "$OBJECT_CONTRACTS_RESULT_FILE"
fi

if [[ "$RUN_DXGI" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$DXGI_PROBE_EXE" > "$DXGI_RESULT_FILE"
  )
  echo "$DXGI_RESULT_FILE"
fi

if [[ "$RUN_RESOURCES" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$RESOURCES_PROBE_EXE" > "$RESOURCES_RESULT_FILE"
  )
  echo "$RESOURCES_RESULT_FILE"
fi

if [[ "$RUN_QUEUES" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$QUEUES_PROBE_EXE" > "$QUEUES_RESULT_FILE"
  )
  echo "$QUEUES_RESULT_FILE"
fi

if [[ "$RUN_DESCRIPTORS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$DESCRIPTORS_PROBE_EXE" > "$DESCRIPTORS_RESULT_FILE"
  )
  echo "$DESCRIPTORS_RESULT_FILE"
fi

if [[ "$RUN_SHADERS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    DXMT_D3D12_TRACE=1 \
    DXMT_D3D12_TRACE_COMPONENTS=PSO \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_EXPECT_DXC="1" \
    "$WINE_BIN" "$SHADERS_PROBE_EXE" > "$SHADERS_WARMUP_RESULT_FILE" || true
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    DXMT_D3D12_TRACE=1 \
    DXMT_D3D12_TRACE_COMPONENTS=PSO \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_EXPECT_DXC="1" \
    "$WINE_BIN" "$SHADERS_PROBE_EXE" > "$SHADERS_RESULT_FILE"
  )
  echo "$SHADERS_WARMUP_RESULT_FILE"
  echo "$SHADERS_RESULT_FILE"
fi

if [[ "$RUN_DXIL_SEMANTICS" == "1" ]]; then
  prepare_dxil_semantic_probes
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$DXIL_SEMANTICS_PROBE_EXE" > "$DXIL_SEMANTICS_RESULT_FILE"
  )
  echo "$DXIL_SEMANTICS_WARMUP_RESULT_FILE"
  echo "$DXIL_SEMANTICS_RESULT_FILE"
fi

if [[ "$RUN_TEXTURE_DIMENSIONS" == "1" ]]; then
  if [[ "${METALSHARP_TEXTURE_SHADERS_PREPARED:-0}" != "1" ]]; then
    prepare_texture_dimension_probes
  fi
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$TEXTURE_DIMENSIONS_PROBE_EXE" > "$TEXTURE_DIMENSIONS_RESULT_FILE"
  )
  echo "$TEXTURE_DIMENSIONS_RESULT_FILE"
fi

if [[ "$RUN_SHADER_CORPUS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_SHADER_CORPUS_MODE="warmup" \
    "$WINE_BIN" "$SHADER_CORPUS_PROBE_EXE" > "$SHADER_CORPUS_WARMUP_RESULT_FILE" || true
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$SHADER_CORPUS_PROBE_EXE" > "$SHADER_CORPUS_RESULT_FILE"
  )
  echo "$SHADER_CORPUS_WARMUP_RESULT_FILE"
  echo "$SHADER_CORPUS_RESULT_FILE"
fi

if [[ "$RUN_SHADERS" == "1" || "$RUN_DXIL_SEMANTICS" == "1" ||
      "$RUN_SHADER_CORPUS" == "1" ]]; then
  python3 "$SDK_DIR/scripts/validate-dxil-lowering.py" \
    --corpus "$SHADER_CACHE_DIR" \
    --json-out "$DXIL_LOWERING_AUDIT_RESULT_FILE"
  echo "$DXIL_LOWERING_AUDIT_RESULT_FILE"
  if [[ "$RUN_SHADER_CORPUS" == "1" || "$RUN_DXIL_SEMANTICS" == "1" ]]; then
    python3 "$SDK_DIR/scripts/validate-sm5-sm69-opcode-matrix.py" \
      --matrix "$SDK_DIR/contracts/phase5-sm5-sm69-opcode-stage-resource-matrix.json" \
      --corpus "$SHADER_CACHE_DIR" \
      --json-out "$SM5_SM69_OPCODE_MATRIX_RESULT_FILE"
    python3 "$SDK_DIR/scripts/validate-sm5-sm69-opcode-matrix.py" \
      --matrix "$SDK_DIR/contracts/phase5-sm5-sm69-opcode-stage-resource-matrix.json" \
      --strict \
      --json-out "$SM5_SM69_OPCODE_CONTRACT_RESULT_FILE"
    echo "$SM5_SM69_OPCODE_MATRIX_RESULT_FILE"
    echo "$SM5_SM69_OPCODE_CONTRACT_RESULT_FILE"
  fi
fi

if [[ "$RUN_SM66_CAPABILITIES" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_SM66_MODE="warmup" \
    "$WINE_BIN" "$SM66_CAPABILITIES_PROBE_EXE" > "$SM66_CAPABILITIES_WARMUP_RESULT_FILE" || true
  )
  DXMT_D3D12_PRESERVE_TYPED_MSL_ENTRIES=cs_sample_cmp_level_sm67 \
    convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$SM66_CAPABILITIES_PROBE_EXE" > "$SM66_CAPABILITIES_RESULT_FILE"
  )
  echo "$SM66_CAPABILITIES_WARMUP_RESULT_FILE"
  echo "$SM66_CAPABILITIES_RESULT_FILE"
fi

if [[ "$RUN_WRITABLE_MSAA" == "1" &&
      ("$RUN_SM66_CAPABILITIES" == "1" || "$RUN_WRITABLE_MSAA_ONLY" == "1") ]]; then
  run_probe_exe \
    "$WRITABLE_MSAA_PROBE_EXE" \
    "$WRITABLE_MSAA_RESULT_FILE"
fi

if [[ "$RUN_VRS" == "1" ]]; then
  run_probe_exe "$VRS_PROBE_EXE" "$VRS_RESULT_FILE"
fi

if [[ "$RUN_SAMPLER_FEEDBACK" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$SAMPLER_FEEDBACK_PROBE_EXE" > "$SAMPLER_FEEDBACK_RESULT_FILE"
  )
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$SAMPLER_FEEDBACK_PIXEL_PROBE_EXE" > "$SAMPLER_FEEDBACK_PIXEL_RESULT_FILE"
  )
  echo "$SAMPLER_FEEDBACK_RESULT_FILE"
  echo "$SAMPLER_FEEDBACK_PIXEL_RESULT_FILE"
fi

if [[ "$RUN_WAVE_OPS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_WAVE_OPS_MODE="warmup" \
    "$WINE_BIN" "$WAVE_OPS_PROBE_EXE" > "$WAVE_OPS_WARMUP_RESULT_FILE" || true
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$WAVE_OPS_PROBE_EXE" > "$WAVE_OPS_RESULT_FILE"
  )
  echo "$WAVE_OPS_WARMUP_RESULT_FILE"
  echo "$WAVE_OPS_RESULT_FILE"
fi

if [[ "$RUN_REFLECTION_ABI" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    D3D12_METAL_SDK_REFLECTION_ABI_MODE="warmup" \
    "$WINE_BIN" "$REFLECTION_ABI_PROBE_EXE" > "$REFLECTION_ABI_WARMUP_RESULT_FILE" || true
  )
  convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$REFLECTION_ABI_PROBE_EXE" > "$REFLECTION_ABI_RESULT_FILE"
  )
  echo "$REFLECTION_ABI_WARMUP_RESULT_FILE"
  echo "$REFLECTION_ABI_RESULT_FILE"
fi

if [[ "$RUN_GRAPHICS_PSO" == "1" ]]; then
  prepare_conservative_raster_probe
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$GRAPHICS_PSO_PROBE_EXE" > "$GRAPHICS_PSO_RESULT_FILE"
  )
  echo "$GRAPHICS_PSO_RESULT_FILE"
fi

if [[ "$RUN_COMPUTE_PSO" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$COMPUTE_PSO_PROBE_EXE" > "$COMPUTE_PSO_RESULT_FILE"
  )
  echo "$COMPUTE_PSO_RESULT_FILE"
fi

if [[ "$RUN_COMMAND_REPLAY" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$COMMAND_REPLAY_RESULT_FILE"
  )
  echo "$COMMAND_REPLAY_RESULT_FILE"
  if [[ -f "$SDK_DIR/out/bin/probe_command_replay_hitobject_local_root.cso" ]]; then
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      D3D12_METAL_SDK_COMMAND_RAY_CSO="$SDK_DIR/out/bin/probe_command_replay_hitobject_local_root.cso" \
      D3D12_METAL_SDK_COMMAND_RAY_LOCAL_ROOT=1 \
      "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$HITOBJECT_LOCAL_ROOT_RESULT_FILE"
    )
    echo "$HITOBJECT_LOCAL_ROOT_RESULT_FILE"
  fi
  if [[ -f "$SDK_DIR/out/bin/probe_command_replay_hitobject_invoke.cso" ]]; then
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      D3D12_METAL_SDK_COMMAND_RAY_CSO="$SDK_DIR/out/bin/probe_command_replay_hitobject_invoke.cso" \
      D3D12_METAL_SDK_COMMAND_RAY_INVOKE=1 \
      METAL_SHADER_CONVERTER=/nonexistent \
      "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$HITOBJECT_INVOKE_RESULT_FILE"
    )
    echo "$HITOBJECT_INVOKE_RESULT_FILE"
  fi
  if [[ -f "$SDK_DIR/out/bin/probe_command_replay_hitobject_attributes.cso" ]]; then
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      D3D12_METAL_SDK_COMMAND_RAY_CSO="$SDK_DIR/out/bin/probe_command_replay_hitobject_attributes.cso" \
      D3D12_METAL_SDK_COMMAND_RAY_ATTRIBUTES=1 \
      METAL_SHADER_CONVERTER=/nonexistent \
      "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$HITOBJECT_ATTRIBUTES_RESULT_FILE"
    )
    echo "$HITOBJECT_ATTRIBUTES_RESULT_FILE"
  fi
  if [[ -f "$SDK_DIR/out/bin/probe_command_replay_hitobject_reorder.cso" ]]; then
    (
      cd "$SDK_DIR/out/bin"
      WINEPREFIX="$WINE_PREFIX" \
      WINEDLLPATH="$PROBE_WINEDLLPATH" \
      WINEDLOVERRIDES="$DLL_OVERRIDES" \
      DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
      DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
      DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
      D3D12_METAL_SDK_PROFILE="$PROFILE" \
      D3D12_METAL_SDK_COMMAND_RAY_CSO="$SDK_DIR/out/bin/probe_command_replay_hitobject_reorder.cso" \
      D3D12_METAL_SDK_COMMAND_RAY_REORDER=1 \
      METAL_SHADER_CONVERTER=/nonexistent \
      "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$HITOBJECT_REORDER_RESULT_FILE"
    )
    echo "$HITOBJECT_REORDER_RESULT_FILE"
  fi
fi

if [[ "$RUN_META_COMMAND" == "1" ]]; then
  run_probe_exe "$META_COMMAND_PROBE_EXE" "$META_COMMAND_RESULT_FILE"
fi

if [[ "$RUN_VIDEO" == "1" ]]; then
  run_probe_exe "$VIDEO_PROBE_EXE" "$VIDEO_RESULT_FILE"
fi

if [[ "$RUN_VIDEO_PROCESS" == "1" ]]; then
  run_probe_exe "$VIDEO_PROCESS_PROBE_EXE" "$VIDEO_PROCESS_RESULT_FILE"
fi

if [[ "$RUN_INFOQUEUE" == "1" ]]; then
  run_probe_exe "$INFOQUEUE_PROBE_EXE" "$INFOQUEUE_RESULT_FILE"
fi

if [[ "$RUN_DISCARD_TEXTURE" == "1" ]]; then
  run_probe_exe "$DISCARD_TEXTURE_PROBE_EXE" "$DISCARD_TEXTURE_RESULT_FILE"
fi

if [[ "$RUN_MANUAL_WRITE_TRACKING" == "1" ]]; then
  run_probe_exe "$MANUAL_WRITE_TRACKING_PROBE_EXE" "$MANUAL_WRITE_TRACKING_RESULT_FILE"
fi

if [[ "$RUN_SHARING_CONTRACT" == "1" ]]; then
  run_probe_exe "$SHARING_CONTRACT_PROBE_EXE" "$SHARING_CONTRACT_RESULT_FILE"
fi

if [[ "$RUN_CPU_TEXTURE_MAP" == "1" ]]; then
  run_probe_exe "$CPU_TEXTURE_MAP_PROBE_EXE" "$CPU_TEXTURE_MAP_RESULT_FILE"
fi

if [[ "$RUN_DEBUG_INTERFACES" == "1" ]]; then
  run_probe_exe "$DEBUG_INTERFACES_PROBE_EXE" "$DEBUG_INTERFACES_RESULT_FILE"
fi

if [[ "$RUN_DIAGNOSTICS" == "1" ]]; then
  run_probe_exe "$DIAGNOSTICS_PROBE_EXE" "$DIAGNOSTICS_RESULT_FILE"
fi

if [[ "$RUN_WORK_GRAPH" == "1" ]]; then
  prepare_work_graph_probe
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-chain-${PROFILE}.json"
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-zero-grid-${PROFILE}.json" zero-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-empty-grid-${PROFILE}.json" empty-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-offset-grid-${PROFILE}.json" offset-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-vector-grid-${PROFILE}.json" vector-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-gpu-vector-grid-${PROFILE}.json" gpu-vector-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-fanout-${PROFILE}.json" fanout
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-fanout-capacity-${PROFILE}.json" fanout-capacity
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-unsupported-target-${PROFILE}.json" unsupported-target
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-cycle-rejection-${PROFILE}.json" cycle
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-u16-grid-${PROFILE}.json" u16-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-gpu-copy-grid-${PROFILE}.json" gpu-copy-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-cross-queue-grid-${PROFILE}.json" cross-queue-grid
  run_probe_exe "$SDK_DIR/out/bin/probe_workgraph_chain.exe" \
    "$RESULTS_DIR/probe-workgraph-oversized-output-${PROFILE}.json" oversized-output
  run_probe_exe "$WORK_GRAPH_EXECUTION_PROBE_EXE" \
    "$WORK_GRAPH_EXECUTION_RESULT_FILE"
fi

if [[ "$RUN_ATTRIBUTE_AT_VERTEX" == "1" ]]; then
  prepare_attribute_at_vertex_probe
  run_probe_exe "$ATTRIBUTE_AT_VERTEX_PROBE_EXE" \
    "$ATTRIBUTE_AT_VERTEX_RESULT_FILE"
fi

if [[ "$RUN_ROV" == "1" ]]; then
  prepare_rov_probe
  run_probe_exe "$ROV_PROBE_EXE" "$ROV_RESULT_FILE" \
    "$SDK_DIR/out/bin/probe_rov_vs.cso" \
    "$SDK_DIR/out/bin/probe_rov_raw_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_texture_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_structured_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_typed_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_array_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_float_ps.cso" \
    "$SDK_DIR/out/bin/probe_rov_vertex.cso" \
    "$SDK_DIR/out/bin/probe_rov_compute.cso"
fi

if [[ "$RUN_BARYCENTRICS" == "1" ]]; then
  prepare_barycentrics_probe
  run_probe_exe "$BARYCENTRICS_PROBE_EXE" "$BARYCENTRICS_RESULT_FILE"
fi

if [[ "$RUN_CYCLE_COUNTER" == "1" ]]; then
  prepare_cycle_counter_probe
  run_probe_exe "$CYCLE_COUNTER_PROBE_EXE" "$CYCLE_COUNTER_RESULT_FILE"
fi

if [[ "$RUN_BARRIERS_RENDER_PASS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$BARRIERS_RENDER_PASS_PROBE_EXE" > "$BARRIERS_RENDER_PASS_RESULT_FILE"
  )
  echo "$BARRIERS_RENDER_PASS_RESULT_FILE"
fi

if [[ "$RUN_RESOURCE_VIEWS_FORMATS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$RESOURCE_VIEWS_FORMATS_PROBE_EXE" > "$RESOURCE_VIEWS_FORMATS_RESULT_FILE"
  )
  echo "$RESOURCE_VIEWS_FORMATS_RESULT_FILE"
fi

if [[ "$RUN_RENDER_HEADLESS" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$RENDER_HEADLESS_PROBE_EXE" > "$RENDER_HEADLESS_RESULT_FILE"
  )
  echo "$RENDER_HEADLESS_RESULT_FILE"
fi

if [[ "$RUN_MINI" == "1" ]]; then
  for mini_probe in "${MINI_PROBES[@]}"; do
    if ! mini_probe_selected "$mini_probe"; then
      continue
    fi
    run_probe_exe \
      "$SDK_DIR/out/bin/probe_mini_${mini_probe}.exe" \
      "$RESULTS_DIR/probe-mini-${mini_probe}-${PROFILE}.json"
  done
fi

if [[ "$RUN_PRESENT_WINDOWED" == "1" ]]; then
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$PRESENT_WINDOWED_PROBE_EXE" > "$PRESENT_WINDOWED_RESULT_FILE"
  )
  echo "$PRESENT_WINDOWED_RESULT_FILE"
fi

FULL_STRESS_RESULT_DIR="$RESULTS_DIR/full-stress"
FULL_STRESS_PROBE_EXE="$SDK_DIR/out/bin/probe_subnautica_full_stress.exe"

if [[ "$RUN_FULL_STRESS" == "1" ]]; then
  mkdir -p "$FULL_STRESS_RESULT_DIR"
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    DXMT_SHADER_CACHE="/tmp/dxmt_shader_cache" \
    "$WINE_BIN" "$FULL_STRESS_PROBE_EXE" \
      > "$FULL_STRESS_RESULT_DIR/probe_full_stress.stdout.jsonl" \
      2> "$FULL_STRESS_RESULT_DIR/probe_full_stress.stderr.log"
  )
  echo "$FULL_STRESS_RESULT_DIR"
fi

if [[ "$RUN_FL12_2_GATE" == "1" ]]; then
  python3 "$SDK_DIR/scripts/validate-fl12-2-gate.py" \
    --profile "$PROFILE" \
    --results-dir "$RESULTS_DIR" \
    --source-root "$ROOT_DIR"
  gate_status=$?
  if [[ "$gate_status" != "0" ]]; then
    exit "$gate_status"
  fi
fi
