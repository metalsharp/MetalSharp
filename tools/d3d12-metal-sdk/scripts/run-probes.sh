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
METAL_SHADER_CONVERTER="${METAL_SHADER_CONVERTER:-}"
AGILITY_SDK_VERSION="${AGILITY_SDK_VERSION:-}"
AGILITY_SDK_PATH="${AGILITY_SDK_PATH:-}"
GAME_DIR=""
RUN_LOADER=1
RUN_AGILITY=1
RUN_CAPS=1
# FL12_2/SM6.7 target gate is opt-in until its implementation phases land.
RUN_FEATURE_LEVELS=0
RUN_OBJECT_CONTRACTS=0
RUN_DXGI=1
RUN_RESOURCES=1
RUN_QUEUES=1
RUN_DESCRIPTORS=1
RUN_SHADERS=1
RUN_DXIL_SEMANTICS=1
RUN_SHADER_CORPUS=1
RUN_SM66_CAPABILITIES=1
RUN_WRITABLE_MSAA=1
RUN_WRITABLE_MSAA_ONLY=0
RUN_VRS=0
RUN_VRS_ONLY=0
RUN_SAMPLER_FEEDBACK=1
RUN_WAVE_OPS=1
RUN_REFLECTION_ABI=1
RUN_GRAPHICS_PSO=1
RUN_COMPUTE_PSO=1
RUN_COMMAND_REPLAY=1
RUN_BARRIERS_RENDER_PASS=1
RUN_RESOURCE_VIEWS_FORMATS=1
RUN_RENDER_HEADLESS=1
RUN_MINI=1
RUN_WINEMETAL_ABI=1
RUN_PRESENT_WINDOWED=0
RUN_FULL_STRESS=0
MINI_PROBE_FILTER="${METALSHARP_MINI_PROBE_FILTER:-}"
DLL_OVERRIDES="${DXMT_PROBE_DLL_OVERRIDES:-d3d12,dxgi,d3d11,d3d10core,winemetal=n,b}"
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
  mesh_object_shader_pso
  texture_sample
  subnautica_geometry_dxil_replay
  dxil_texture_color_output
  compute_first_use_dispatch
  dxr_acceleration_structures
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
  --caps-only           Run only the feature support / unsupported policy probe.
  --feature-levels      Run the target FL11_0-through-12_2 and SM6.7 probe.
  --feature-levels-only Run only the target feature-level and SM6.7 probe.
  --object-contracts    Run D3D12 object private-data/COM semantics.
  --object-contracts-only
                        Run only D3D12 object private-data/COM semantics.
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
    --caps-only)
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
    --object-contracts)
      RUN_OBJECT_CONTRACTS=1
      shift
      ;;
    --object-contracts-only)
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
    --no-dxgi)
      RUN_DXGI=0
      shift
      ;;
    --dxgi-only)
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
    --shader-corpus-only)
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
    --no-sampler-feedback)
      RUN_SAMPLER_FEEDBACK=0
      shift
      ;;
    --sampler-feedback)
      RUN_SAMPLER_FEEDBACK=1
      shift
      ;;
    --sampler-feedback-only)
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
    --command-replay-only)
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
WINE_UNIX_DIR="$RUNTIME_LIB_DIR/wine/x86_64-unix"
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
DXMT_DYLD_LIBRARY_PATH="$UNIX_DIR:$WINE_UNIX_DIR:${DYLD_LIBRARY_PATH:-}"
DXMT_WINEMETAL_UNIXLIB_NAME="winemetal.so"
PROBE_WINEMETAL_UNIXLIB_LINK=""
cleanup_probe_winemetal_unixlib() {
  if [[ -n "$PROBE_WINEMETAL_UNIXLIB_LINK" ]]; then
    rm -f "$PROBE_WINEMETAL_UNIXLIB_LINK"
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
FEATURE_LEVELS_PROBE_EXE="$SDK_DIR/out/bin/probe_feature_levels.exe"
OBJECT_CONTRACTS_PROBE_EXE="$SDK_DIR/out/bin/probe_object_contracts.exe"
DXGI_PROBE_EXE="$SDK_DIR/out/bin/probe_dxgi_factory.exe"
RESOURCES_PROBE_EXE="$SDK_DIR/out/bin/probe_resources.exe"
QUEUES_PROBE_EXE="$SDK_DIR/out/bin/probe_queues.exe"
DESCRIPTORS_PROBE_EXE="$SDK_DIR/out/bin/probe_descriptors.exe"
SHADERS_PROBE_EXE="$SDK_DIR/out/bin/probe_shaders.exe"
DXIL_SEMANTICS_PROBE_EXE="$SDK_DIR/out/bin/probe_dxil_semantics.exe"
SHADER_CORPUS_PROBE_EXE="$SDK_DIR/out/bin/probe_shader_corpus.exe"
SM66_CAPABILITIES_PROBE_EXE="$SDK_DIR/out/bin/probe_sm66_capabilities.exe"
WRITABLE_MSAA_PROBE_EXE="$SDK_DIR/out/bin/probe_writable_msaa.exe"
VRS_PROBE_EXE="$SDK_DIR/out/bin/probe_vrs.exe"
SAMPLER_FEEDBACK_PROBE_EXE="$SDK_DIR/out/bin/probe_sampler_feedback.exe"
SAMPLER_FEEDBACK_PIXEL_PROBE_EXE="$SDK_DIR/out/bin/probe_sampler_feedback_pixel.exe"
WAVE_OPS_PROBE_EXE="$SDK_DIR/out/bin/probe_wave_ops.exe"
REFLECTION_ABI_PROBE_EXE="$SDK_DIR/out/bin/probe_reflection_abi.exe"
GRAPHICS_PSO_PROBE_EXE="$SDK_DIR/out/bin/probe_graphics_pso.exe"
COMPUTE_PSO_PROBE_EXE="$SDK_DIR/out/bin/probe_compute_pso.exe"
COMMAND_REPLAY_PROBE_EXE="$SDK_DIR/out/bin/probe_command_replay.exe"
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
if [[ ! -f "$PROBE_EXE" || ! -f "$AGILITY_PROBE_EXE" || ! -f "$CAPS_PROBE_EXE" || ! -f "$FEATURE_LEVELS_PROBE_EXE" || ! -f "$OBJECT_CONTRACTS_PROBE_EXE" || ! -f "$DXGI_PROBE_EXE" || ! -f "$RESOURCES_PROBE_EXE" || ! -f "$QUEUES_PROBE_EXE" || ! -f "$DESCRIPTORS_PROBE_EXE" || ! -f "$SHADERS_PROBE_EXE" || ! -f "$DXIL_SEMANTICS_PROBE_EXE" || ! -f "$SHADER_CORPUS_PROBE_EXE" || ! -f "$SM66_CAPABILITIES_PROBE_EXE" || ! -f "$WRITABLE_MSAA_PROBE_EXE" || ! -f "$VRS_PROBE_EXE" || ! -f "$SAMPLER_FEEDBACK_PROBE_EXE" || ! -f "$SAMPLER_FEEDBACK_PIXEL_PROBE_EXE" || ! -f "$WAVE_OPS_PROBE_EXE" || ! -f "$REFLECTION_ABI_PROBE_EXE" || ! -f "$GRAPHICS_PSO_PROBE_EXE" || ! -f "$COMPUTE_PSO_PROBE_EXE" || ! -f "$COMMAND_REPLAY_PROBE_EXE" || ! -f "$BARRIERS_RENDER_PASS_PROBE_EXE" || ! -f "$RESOURCE_VIEWS_FORMATS_PROBE_EXE" || ! -f "$RENDER_HEADLESS_PROBE_EXE" || ! -f "$PRESENT_WINDOWED_PROBE_EXE" || ! -f "$SDK_DIR/out/bin/D3D12/D3D12Core.dll" || ! -f "$SDK_DIR/out/bin/D3D12/d3d12SDKLayers.dll" || ! -f "$SDK_DIR/out/bin/D3D12/D3D12StateObjectCompiler.dll" || ! -f "$SDK_DIR/out/bin/D3D12/dxil.dll" || ! -f "$SDK_DIR/out/bin/dxc.exe" || ! -f "$SDK_DIR/out/bin/dxcompiler.dll" || ! -f "$SDK_DIR/out/bin/dxil.dll" ]]; then
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
SHADER_CORPUS_WARMUP_RESULT_FILE="$RESULTS_DIR/probe-shader-corpus-warmup-${PROFILE}.json"
SHADER_CORPUS_RESULT_FILE="$RESULTS_DIR/probe-shader-corpus-${PROFILE}.json"
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
BARRIERS_RENDER_PASS_RESULT_FILE="$RESULTS_DIR/probe-barriers-render-pass-${PROFILE}.json"
RESOURCE_VIEWS_FORMATS_RESULT_FILE="$RESULTS_DIR/probe-resource-views-formats-${PROFILE}.json"
RENDER_HEADLESS_RESULT_FILE="$RESULTS_DIR/probe-render-headless-${PROFILE}.json"
PRESENT_WINDOWED_RESULT_FILE="$RESULTS_DIR/probe-present-windowed-${PROFILE}.json"
WINEMETAL_ABI_RESULT_FILE="$RESULTS_DIR/winemetal-abi-${PROFILE}.json"
VRS_RESULT_FILE="$RESULTS_DIR/probe-vrs-${PROFILE}.json"

run_probe_exe() {
  local exe="$1"
  local result_file="$2"
  local strict_deferred_pso=0
  local enable_geometry_mesh="${DXMT_D3D12_ENABLE_GEOMETRY_MESH:-0}"
  local d3d12_trace="${DXMT_D3D12_TRACE:-0}"
  if [[ "$(basename "$exe")" == "probe_mini_subnautica_geometry_dxil_replay.exe" ]]; then
    strict_deferred_pso=1
    enable_geometry_mesh=1
  fi
  if [[ "$(basename "$exe")" == "probe_shaders.exe" ]]; then
    d3d12_trace=1
  fi
  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLPATH="$PROBE_WINEDLLPATH" \
    WINEDLLOVERRIDES="$DLL_OVERRIDES" \
    DYLD_LIBRARY_PATH="$DXMT_DYLD_LIBRARY_PATH" \
    DXMT_WINEMETAL_UNIXLIB="$DXMT_WINEMETAL_UNIXLIB_NAME" \
    DXMT_SHADER_CACHE_PATH="$SHADER_CACHE_DIR" \
    DXMT_D3D12_ENABLE_GEOMETRY_MESH="$enable_geometry_mesh" \
    DXMT_D3D12_FAIL_DEFERRED_PSO="$strict_deferred_pso" \
    DXMT_D3D12_TRACE="$d3d12_trace" \
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$exe" > "$result_file"
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
    local layout="$base.vertex-layout.json"
    local fail_marker="$base.msc.fail"
    local dxbc_size
    dxbc_size="$(wc -c < "$dxbc" | tr -d '[:space:]')"
    if [[ -s "$metallib" && -s "$reflection" ]]; then
      continue
    fi
    rm -f "$fail_marker"
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
  DispatchMesh(amplification_enabled * amplification_control.Load(0),
               1, 1, payload);
}

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
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
    convert_dxil_shader_cache "$SHADER_CACHE_DIR"
  done
}

prepare_dxr_acceleration_structure_probe() {
  local hlsl="$SDK_DIR/out/bin/probe_dxr_inline.hlsl"
  local raygen_hlsl="$SDK_DIR/out/bin/probe_dxr_raygen.hlsl"
  local raygen_cso="$SDK_DIR/out/bin/probe_dxr_raygen.cso"
  local raygen_root="$SDK_DIR/out/bin/probe_dxr_raygen_root.json"
  local closest_hit_local_root="$SDK_DIR/out/bin/probe_dxr_closest_hit_local_root.json"
  local procedural_compiler="$SDK_DIR/out/bin/compile-procedural-raytracing"

  DEVELOPER_DIR="${DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}" \
    xcrun clang++ -std=c++17 -I/usr/local/include \
      "$SDK_DIR/scripts/compile-procedural-raytracing.cpp" \
      -L/usr/local/lib -lmetalirconverter -o "$procedural_compiler"

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
  RayDesc ray;
  ray.Origin = ray_index == 2 ? float3(2.0, 0.0, -2.0)
                              : float3(0.0, 0.0, -2.0);
  ray.TMin = 0.0;
  ray.Direction = ray_index == 0 ? float3(0.0, 1.0, 0.0)
                                 : float3(0.0, 0.0, 1.0);
  ray.TMax = 10.0;
  MissPayload payload;
  payload.value = 0;
  TraceRay(scene, RAY_FLAG_NONE, 0x02, 0, 0,
           ray_index == 0 ? 1 : 0, ray, payload);
  output.Store(4 + ray_index * 4, payload.value);
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
  payload.value = any_hit_ran && recursive_payload.value == 0x4d495353 &&
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
    "$WINE_BIN" dxc.exe -nologo -T lib_6_5 \
      -Fo probe_dxr_raygen.cso probe_dxr_raygen.hlsl >/dev/null
  )

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
    if [[ -n "$converter" && -x "$converter" ]]; then
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

prepare_dxil_semantic_probes() {
  local hlsl="$SDK_DIR/out/bin/probe_dxil_semantics.hlsl"

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
  }
}

[numthreads(4, 1, 1)]
void cs_buffer(uint3 id : SV_DispatchThreadID) {
  uint v = inbuf.Load(id.x * 4);
  outbuf.Store(id.x * 4, v * 3 + 1);
}

groupshared uint g_counter;

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
HLSL

  (
    cd "$SDK_DIR/out/bin"
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_math_bits -T cs_6_0 -Fo probe_dxil_semantic_math_bits.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_buffer -T cs_6_0 -Fo probe_dxil_semantic_buffer.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_atomics_ids -T cs_6_0 -Fo probe_dxil_semantic_atomics_ids.cso probe_dxil_semantics.hlsl >/dev/null
    WINEPREFIX="$WINE_PREFIX" \
    WINEDLLOVERRIDES="dxcompiler,dxil=n,b" \
    "$WINE_BIN" dxc.exe -nologo -E cs_wave_quad -T cs_6_6 -Fo probe_dxil_semantic_wave_quad.cso probe_dxil_semantics.hlsl >/dev/null
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

cat > "$RESULTS_DIR/host-runtime-${PROFILE}.json" <<EOF
{
  "schema": "metalsharp.d3d12-metal.host-runtime.v1",
  "profile": "$PROFILE",
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
    D3D12_METAL_SDK_PROFILE="$PROFILE" \
    "$WINE_BIN" "$COMMAND_REPLAY_PROBE_EXE" > "$COMMAND_REPLAY_RESULT_FILE"
  )
  echo "$COMMAND_REPLAY_RESULT_FILE"
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
