#include "d3d12_command_queue.hpp"
#include "d3d12_binding_completeness.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_command_stats.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_device.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_query_heap.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_swapchain.hpp"
#include "d3d12_trace.hpp"
#include "d3d12_vertex_input.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "Metal.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define QTRACE(fmt, ...) DXMTD3D12Trace("Queue", fmt, ##__VA_ARGS__)

static uint64_t g_enc_id = 0;
#define ENC_CREATE(type, handle)                                               \
  do {                                                                         \
    uint64_t _eid = __atomic_add_fetch(&g_enc_id, 1, __ATOMIC_SEQ_CST);        \
    QTRACE("[ENC+%llu] CREATE %s handle=%llu", (unsigned long long)_eid, type, \
           (unsigned long long)(handle));                                      \
  } while (0)
#define ENC_END(handle)                                                        \
  do {                                                                         \
    QTRACE("[ENC] END handle=%llu", (unsigned long long)(handle));             \
  } while (0)
#define ENC_COMMIT(cmdbuf_handle)                                              \
  do {                                                                         \
    QTRACE("[ENC] COMMIT cmdbuf=%llu", (unsigned long long)(cmdbuf_handle));   \
  } while (0)

namespace dxmt {

namespace {

static constexpr uint32_t kD3D12RootParameterSlotCount = 64;

bool DXMTD3D12AutopresentSwapchain() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_AUTOPRESENT_SWAPCHAIN");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12SyncExecuteCommandBuffers() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_SYNC_EXECUTE");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12ForceSwapchainColor() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_FORCE_SWAPCHAIN_COLOR");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12SwapchainRenderReadback() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_SWAPCHAIN_RENDER_READBACK");
    if (!value || !value[0])
      value = std::getenv("DXMT_D3D12_SWAPCHAIN_READBACK");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12FinalRenderSnapshot() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_FINAL_RENDER_SNAPSHOT");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12SkipUnsafeMSCOffscreenPass() {
  static int enabled = [] {
    const char *value =
        std::getenv("DXMT_D3D12_SKIP_UNSAFE_MSC_OFFSCREEN_PASS");
    if (!value || !value[0])
      value = std::getenv("DXMT_D3D12_SKIP_UNSAFE_MSC_R16_DEPTH_PASS");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

bool DXMTD3D12DisableCBVStaging() {
  static int enabled = [] {
    const char *value = std::getenv("DXMT_D3D12_DISABLE_CBV_STAGING");
    return value && value[0] && value[0] != '0';
  }();
  return enabled != 0;
}

static uint32_t AlignReadbackPitch(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

const char *TraceCompileFailureStage(MTLD3D12PipelineState *pso) {
  static thread_local std::string stage;
  stage = pso ? pso->GetCompileFailureStage() : "no_pso";
  return stage.c_str();
}

const char *TraceCompileFailureDetail(MTLD3D12PipelineState *pso) {
  static thread_local std::string detail;
  detail = pso ? pso->GetCompileFailureDetail() : "";
  return detail.c_str();
}

const char *TracePsoShaderSummary(MTLD3D12PipelineState *pso) {
  static thread_local std::string summary;
  if (!pso) {
    summary = "vs=0 ps=0 gs=0";
    return summary.c_str();
  }

  summary = str::format(
      "vs=", pso->GetVSCacheHash(), " ps=", pso->GetPSCacheHash(),
      " gs=", pso->GetGSCacheHash(),
      " vs_args=", (unsigned)pso->GetVSArguments().size(),
      " vs_cb=", (unsigned)pso->GetVSConstantBuffers().size(),
      " vs_qwords=", (unsigned)pso->GetVSReflection().ArgumentTableQwords,
      " vs_cb_bind=", pso->GetVSReflection().ConstanttBufferTableBindIndex,
      " vs_arg_bind=", pso->GetVSReflection().ArgumentBufferBindIndex,
      " ps_args=", (unsigned)pso->GetPSArguments().size(),
      " ps_cb=", (unsigned)pso->GetPSConstantBuffers().size(),
      " ps_qwords=", (unsigned)pso->GetPSReflection().ArgumentTableQwords,
      " ps_cb_bind=", pso->GetPSReflection().ConstanttBufferTableBindIndex,
      " ps_arg_bind=", pso->GetPSReflection().ArgumentBufferBindIndex,
      " stage_in=", pso->UsesStageInVertexDescriptor(),
      " geom_mesh=", pso->UsesGeometryMeshPipeline(),
      " tess_fallback=", pso->UsesTessellationFallback());
  return summary.c_str();
}

static uint32_t g_swapchain_encoder_logs = 0;
static uint32_t g_swapchain_draw_logs = 0;
static uint32_t g_swapchain_indirect_draw_logs = 0;
static uint32_t g_swapchain_indirect_skip_logs = 0;
static uint32_t g_swapchain_clear_logs = 0;
static uint32_t g_swapchain_state_logs = 0;
static uint32_t g_swapchain_argbuf_logs = 0;
static uint32_t g_swapchain_vs_argbuf_logs = 0;
static uint32_t g_swapchain_vs_cbv_logs = 0;
static uint32_t g_swapchain_ps_cbv_logs = 0;
static uint32_t g_swapchain_stage_in_vb_logs = 0;
static uint32_t g_swapchain_forced_color_logs = 0;
static uint32_t g_swapchain_vertex_sample_logs = 0;
static uint32_t g_swapchain_texture_binding_logs = 0;
static uint32_t g_swapchain_fragment_prefill_logs = 0;
static uint32_t g_offscreen_indexed_draw_logs = 0;
static uint32_t g_swapchain_render_readback_captures = 0;
static uint32_t g_swapchain_final_snapshot_logs = 0;
static uint32_t g_swapchain_fragment_completeness_logs = 0;
static uint32_t g_tessellation_fallback_draw_logs = 0;
static uint32_t g_compute_completeness_logs = 0;
static uint32_t g_command_list_summary_logs = 0;
static uint32_t g_draw_safety_skip_logs = 0;
static uint32_t g_native_vertex_resolve_logs = 0;
static uint64_t g_queue_submit_serial = 0;

static uint32_t g_quarantine_zero_vb_offscreen = 0;
static uint32_t g_quarantine_r16_dsv = 0;
static uint32_t g_quarantine_rgba8_mrt_dsv = 0;
static uint32_t g_quarantine_b8g8r8a8_dsv_stencil = 0;
static uint32_t g_quarantine_r11g11b10_dsv_stencil = 0;
static uint32_t g_quarantine_r11g11b10_gbuffer = 0;
static uint32_t g_quarantine_indexed_stage_in = 0;
static uint32_t g_stage_in_snapshot_count = 0;

static bool TakeLogBudget(uint32_t *counter, uint32_t limit) {
  return __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED) <= limit;
}

static std::string FormatDebugBytes(const uint8_t *bytes, size_t count) {
  std::string out;
  char text[8] = {};
  for (size_t i = 0; i < count; i++) {
    if (i)
      out.push_back(' ');
    std::snprintf(text, sizeof(text), "%02x", bytes[i]);
    out += text;
  }
  return out;
}

static float ReadDebugFloat(const uint8_t *bytes) {
  float value = 0.0f;
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

static std::string FormatDebugFloat4(float x, float y, float z, float w) {
  char text[160] = {};
  std::snprintf(text, sizeof(text), "%.6g,%.6g,%.6g,%.6g", x, y, z, w);
  return text;
}

static std::string DecodeDebugVertexValue(const uint8_t *bytes,
                                          size_t available,
                                          DXGI_FORMAT format) {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;

  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
    if (available >= 16) {
      x = ReadDebugFloat(bytes + 0);
      y = ReadDebugFloat(bytes + 4);
      z = ReadDebugFloat(bytes + 8);
      w = ReadDebugFloat(bytes + 12);
      return FormatDebugFloat4(x, y, z, w);
    }
    break;
  case DXGI_FORMAT_R32G32B32_FLOAT:
    if (available >= 12) {
      x = ReadDebugFloat(bytes + 0);
      y = ReadDebugFloat(bytes + 4);
      z = ReadDebugFloat(bytes + 8);
      return FormatDebugFloat4(x, y, z, w);
    }
    break;
  case DXGI_FORMAT_R32G32_FLOAT:
    if (available >= 8) {
      x = ReadDebugFloat(bytes + 0);
      y = ReadDebugFloat(bytes + 4);
      return FormatDebugFloat4(x, y, z, w);
    }
    break;
  case DXGI_FORMAT_R32_FLOAT:
    if (available >= 4) {
      x = ReadDebugFloat(bytes);
      return FormatDebugFloat4(x, y, z, w);
    }
    break;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    if (available >= 4) {
      x = float(bytes[0]) / 255.0f;
      y = float(bytes[1]) / 255.0f;
      z = float(bytes[2]) / 255.0f;
      w = float(bytes[3]) / 255.0f;
      return FormatDebugFloat4(x, y, z, w);
    }
    break;
  case DXGI_FORMAT_R8G8B8A8_UINT:
    if (available >= 4)
      return str::format((unsigned)bytes[0], ",", (unsigned)bytes[1], ",",
                         (unsigned)bytes[2], ",", (unsigned)bytes[3]);
    break;
  default:
    break;
  }

  return "unavailable";
}

struct D3D12GeometryDrawArguments {
  uint32_t VertexCount;
  uint32_t InstanceCount;
  uint32_t StartVertex;
  uint32_t StartInstance;
};

struct D3D12GeometryDrawIndexedArguments {
  uint32_t IndexCount;
  uint32_t InstanceCount;
  uint32_t StartIndex;
  int32_t BaseVertex;
  uint32_t StartInstance;
};

static std::pair<uint32_t, uint32_t>
D3D12GeometryVertexCount(D3D_PRIMITIVE_TOPOLOGY primitive) {
  switch (primitive) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return {32, 31};
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
    return {32, 29};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
    return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return {32, 30};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    return {32, 28};
  default:
    break;
  }

  if (primitive >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
      primitive <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
    uint32_t control_points =
        uint32_t(primitive) -
        uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1;
    return {control_points, control_points};
  }

  return {32, 32};
}

static bool D3D12IsPatchTopology(D3D_PRIMITIVE_TOPOLOGY primitive) {
  return primitive >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
         primitive <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST;
}

static uint32_t D3D12PatchControlPointCount(D3D_PRIMITIVE_TOPOLOGY primitive) {
  if (!D3D12IsPatchTopology(primitive))
    return 0;
  return uint32_t(primitive) -
         uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1;
}

static uint64_t TextureMetadata(uint32_t array_length, float min_lod = 0.0f) {
  uint32_t min_lod_bits = 0;
  static_assert(sizeof(min_lod_bits) == sizeof(min_lod));
  memcpy(&min_lod_bits, &min_lod, sizeof(min_lod_bits));
  return ((uint64_t)array_length << 32) | min_lod_bits;
}

static uint64_t SamplerCubeGPUResourceID(const D3D12Descriptor *desc) {
  return desc->metal_sampler_cube_gpu_id ? desc->metal_sampler_cube_gpu_id
                                         : desc->metal_sampler_gpu_id;
}

static uint64_t SamplerLodBiasBits(const D3D12Descriptor *desc) {
  uint32_t bits = 0;
  float lod_bias = desc->sampler.MipLODBias;
  static_assert(sizeof(bits) == sizeof(lod_bias));
  memcpy(&bits, &lod_bias, sizeof(bits));
  return bits;
}

static size_t QueryResultStride(D3D12_QUERY_TYPE type) {
  switch (type) {
  case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
    return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
  case D3D12_QUERY_TYPE_PIPELINE_STATISTICS1:
    return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS1);
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
    return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
  default:
    return sizeof(uint64_t);
  }
}

static bool ShaderVisibilityMatches(uint32_t param_visibility,
                                    D3D12_SHADER_VISIBILITY shader_visibility,
                                    bool exact_pass) {
  if (exact_pass)
    return param_visibility == shader_visibility;
  return param_visibility == D3D12_SHADER_VISIBILITY_ALL;
}

static bool FindRootDescriptorParameter(
    MTLD3D12RootSignature *root_signature, D3D12_ROOT_PARAMETER_TYPE type,
    const MTL_SM50_SHADER_ARGUMENT &arg,
    D3D12_SHADER_VISIBILITY shader_visibility, uint32_t *root_index) {
  if (!root_signature || !root_index)
    return false;

  const auto &params = root_signature->GetParameters();
  for (uint32_t pass = 0; pass < 2; pass++) {
    for (uint32_t p = 0; p < params.size() && p < kD3D12RootParameterSlotCount;
         p++) {
      if (params[p].type == type &&
          params[p].register_index == arg.SM50BindingSlot &&
          params[p].register_space == arg.SM50RegisterSpace &&
          ShaderVisibilityMatches(params[p].shader_visibility,
                                  shader_visibility, pass == 0)) {
        *root_index = p;
        return true;
      }
    }
  }
  return false;
}

static uint32_t FormatByteSize(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return 16;
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return 12;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
    return 8;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
    return 4;
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
    return 2;
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
    return 1;
  default:
    return 4;
  }
}

static uint64_t SRVBufferByteLength(const D3D12Descriptor *desc,
                                    const MTLD3D12Resource *res) {
  if (!desc)
    return res ? res->GetBufferByteLength() : 0;
  uint64_t stride = desc->srv.Buffer.StructureByteStride;
  if (!stride)
    stride = FormatByteSize(desc->srv.Format);
  uint64_t length = (uint64_t)desc->srv.Buffer.NumElements * stride;
  return length ? length : (res ? res->GetBufferByteLength() : 0);
}

static uint64_t UAVBufferByteLength(const D3D12Descriptor *desc,
                                    const MTLD3D12Resource *res) {
  if (!desc)
    return res ? res->GetBufferByteLength() : 0;
  uint64_t stride = desc->uav.Buffer.StructureByteStride;
  if (!stride)
    stride = FormatByteSize(desc->uav.Format);
  uint64_t length = (uint64_t)desc->uav.Buffer.NumElements * stride;
  return length ? length : (res ? res->GetBufferByteLength() : 0);
}

static uint64_t SRVBufferByteOffset(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  uint64_t stride = desc->srv.Buffer.StructureByteStride;
  if (!stride)
    stride = FormatByteSize(desc->srv.Format);
  return (uint64_t)desc->srv.Buffer.FirstElement * stride;
}

static uint64_t UAVBufferByteOffset(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  uint64_t stride = desc->uav.Buffer.StructureByteStride;
  if (!stride)
    stride = FormatByteSize(desc->uav.Format);
  return (uint64_t)desc->uav.Buffer.FirstElement * stride;
}

static uint32_t SRVTextureArrayLength(const D3D12Descriptor *desc,
                                      const MTLD3D12Resource *res) {
  if (!desc)
    return res ? res->GetTextureArrayLength() : 1;
  switch (desc->srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    return desc->srv.Texture1DArray.ArraySize
               ? desc->srv.Texture1DArray.ArraySize
               : 1;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    return desc->srv.Texture2DArray.ArraySize
               ? desc->srv.Texture2DArray.ArraySize
               : 1;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return desc->srv.Texture2DMSArray.ArraySize
               ? desc->srv.Texture2DMSArray.ArraySize
               : 1;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return desc->srv.TextureCubeArray.NumCubes
               ? desc->srv.TextureCubeArray.NumCubes * 6
               : 6;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    return 6;
  default:
    return res ? res->GetTextureArrayLength() : 1;
  }
}

static uint32_t UAVTextureArrayLength(const D3D12Descriptor *desc,
                                      const MTLD3D12Resource *res) {
  if (!desc)
    return res ? res->GetTextureArrayLength() : 1;
  switch (desc->uav.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    return desc->uav.Texture1DArray.ArraySize
               ? desc->uav.Texture1DArray.ArraySize
               : 1;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    return desc->uav.Texture2DArray.ArraySize
               ? desc->uav.Texture2DArray.ArraySize
               : 1;
  default:
    return res ? res->GetTextureArrayLength() : 1;
  }
}

static WMT::Reference<WMT::Texture>
DescriptorTexture(const D3D12Descriptor *desc, MTLD3D12Resource *res) {
  if (desc && desc->metal_texture_view.handle)
    return desc->metal_texture_view;
  return res ? res->GetMTLTexture() : WMT::Reference<WMT::Texture>();
}

static uint64_t DescriptorTextureGPUResourceID(const D3D12Descriptor *desc,
                                               const MTLD3D12Resource *res) {
  if (desc && desc->metal_texture_gpu_id)
    return desc->metal_texture_gpu_id;
  return res ? res->GetTextureGPUResourceID() : 0;
}

static void WriteMSCTextureArgument(uint64_t *data,
                                    const MTL_SM50_SHADER_ARGUMENT &arg,
                                    uint64_t texture_view_id,
                                    uint32_t array_length,
                                    float min_lod = 0.0f) {
  data[arg.StructurePtrOffset] = texture_view_id;
  data[arg.StructurePtrOffset + 1] = TextureMetadata(array_length, min_lod);
}

// Metal Shader Converter's automatic linear layout uses the same three-qword
// descriptor shape as IRDescriptorTableEntry: buffer address, texture/sampler
// resource ID, and metadata.  This differs from DXMT's legacy SM50 AIR layout.
static void WriteMSCLinearTextureArgument(
    uint64_t *data, const MTL_SM50_SHADER_ARGUMENT &arg,
    uint64_t texture_view_id, float min_lod = 0.0f) {
  data[arg.StructurePtrOffset] = 0;
  data[arg.StructurePtrOffset + 1] = texture_view_id;
  data[arg.StructurePtrOffset + 2] = TextureMetadata(0, min_lod);
}

static void WriteMSCLinearSamplerArgument(
    uint64_t *data, const MTL_SM50_SHADER_ARGUMENT &arg,
    uint64_t sampler_resource_id, uint64_t lod_bias_bits) {
  data[arg.StructurePtrOffset] = 0;
  data[arg.StructurePtrOffset + 1] = sampler_resource_id;
  data[arg.StructurePtrOffset + 2] = lod_bias_bits;
}

static bool MSCArgumentAcceptsBuffer(const MTL_SM50_SHADER_ARGUMENT &arg,
                                     const MTLD3D12Resource *res) {
  if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER)
    return true;
  const auto resource_flags =
      MTL_SM50_SHADER_ARGUMENT_BUFFER | MTL_SM50_SHADER_ARGUMENT_TEXTURE;
  return res && res->IsBuffer() && (arg.Flags & resource_flags) == 0;
}

static bool FormatHasStencil(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
         format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
}

template <typename Encoder>
static void EndMetalEncoder(Encoder &encoder, const char *label) {
  if (!encoder.handle) {
    QTRACE("%s: no encoder handle to end", label ? label : "encoder");
    return;
  }
  ENC_END(encoder.handle);
  encoder.endEncoding();
  encoder = Encoder{};
}

template <typename Encoder> struct ScopedMetalEncoderEnd {
  Encoder &encoder;
  const char *label;

  ~ScopedMetalEncoderEnd() {
    if (encoder.handle)
      EndMetalEncoder(encoder, label);
  }
};

static bool DSVHasStencil(const D3D12Descriptor *desc) {
  if (!desc || !desc->resource)
    return false;
  DXGI_FORMAT format = desc->dsv.Format;
  if (format == DXGI_FORMAT_UNKNOWN) {
    D3D12_RESOURCE_DESC resource_desc = {};
    static_cast<MTLD3D12Resource *>(desc->resource)->GetDesc(&resource_desc);
    format = resource_desc.Format;
  }
  return FormatHasStencil(format);
}

static uint16_t DSVMipLevel(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  switch (desc->dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    return desc->dsv.Texture1D.MipSlice;
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return desc->dsv.Texture1DArray.MipSlice;
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    return desc->dsv.Texture2D.MipSlice;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return desc->dsv.Texture2DArray.MipSlice;
  default:
    return 0;
  }
}

static uint16_t DSVArraySlice(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  switch (desc->dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return desc->dsv.Texture1DArray.FirstArraySlice;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return desc->dsv.Texture2DArray.FirstArraySlice;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return desc->dsv.Texture2DMSArray.FirstArraySlice;
  default:
    return 0;
  }
}

static uint16_t DSVArrayLength(const D3D12Descriptor *desc) {
  if (!desc)
    return 1;
  switch (desc->dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return desc->dsv.Texture1DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return desc->dsv.Texture2DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return desc->dsv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static uint16_t RTVMipLevel(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  switch (desc->rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    return desc->rtv.Texture1D.MipSlice;
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return desc->rtv.Texture1DArray.MipSlice;
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    return desc->rtv.Texture2D.MipSlice;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return desc->rtv.Texture2DArray.MipSlice;
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    return desc->rtv.Texture3D.MipSlice;
  default:
    return 0;
  }
}

static uint16_t RTVArraySlice(const D3D12Descriptor *desc) {
  if (!desc)
    return 0;
  switch (desc->rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return desc->rtv.Texture1DArray.FirstArraySlice;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return desc->rtv.Texture2DArray.FirstArraySlice;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return desc->rtv.Texture2DMSArray.FirstArraySlice;
  default:
    return 0;
  }
}

static uint16_t RTVArrayLength(const D3D12Descriptor *desc) {
  if (!desc)
    return 1;
  switch (desc->rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return desc->rtv.Texture1DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return desc->rtv.Texture2DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return desc->rtv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static const char *DescriptorRangeTypeName(D3D12_DESCRIPTOR_RANGE_TYPE type) {
  switch (type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    return "SRV";
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    return "UAV";
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    return "CBV";
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return "SAMPLER";
  default:
    return "UNKNOWN";
  }
}

static const char *RootParameterTypeName(D3D12_ROOT_PARAMETER_TYPE type) {
  switch (type) {
  case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
    return "TABLE";
  case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
    return "CONSTANTS";
  case D3D12_ROOT_PARAMETER_TYPE_CBV:
    return "CBV";
  case D3D12_ROOT_PARAMETER_TYPE_SRV:
    return "SRV";
  case D3D12_ROOT_PARAMETER_TYPE_UAV:
    return "UAV";
  default:
    return "UNKNOWN";
  }
}

static const char *ShaderVisibilityName(uint32_t visibility) {
  switch ((D3D12_SHADER_VISIBILITY)visibility) {
  case D3D12_SHADER_VISIBILITY_ALL:
    return "ALL";
  case D3D12_SHADER_VISIBILITY_VERTEX:
    return "VS";
  case D3D12_SHADER_VISIBILITY_HULL:
    return "HS";
  case D3D12_SHADER_VISIBILITY_DOMAIN:
    return "DS";
  case D3D12_SHADER_VISIBILITY_GEOMETRY:
    return "GS";
  case D3D12_SHADER_VISIBILITY_PIXEL:
    return "PS";
  case D3D12_SHADER_VISIBILITY_AMPLIFICATION:
    return "AS";
  case D3D12_SHADER_VISIBILITY_MESH:
    return "MS";
  default:
    return "UNKNOWN";
  }
}

static std::string ResourceSummary(MTLD3D12Resource *res) {
  if (!res)
    return "res=null";

  D3D12_RESOURCE_DESC desc = {};
  res->GetDesc(&desc);
  auto tex = res->GetMTLTexture();
  auto buf = res->GetMTLBuffer();
  return str::format(
      "res=", (void *)res, " dim=", (unsigned)desc.Dimension,
      " fmt=", (unsigned)desc.Format, " size=", desc.Width, "x",
      (unsigned)desc.Height, "x", (unsigned)desc.DepthOrArraySize,
      " mips=", (unsigned)desc.MipLevels,
      " samples=", (unsigned)desc.SampleDesc.Count,
      " tex=", (unsigned long long)tex.handle,
      " tex_id=", (unsigned long long)res->GetTextureGPUResourceID(),
      " tex_array=", res->GetTextureArrayLength(),
      " buf=", (unsigned long long)buf.handle, " gpu=0x",
      (unsigned long long)res->GetGPUVirtualAddress(),
      " bytes=", (unsigned long long)res->GetBufferByteLength(),
      " swapchain=", res->IsSwapchainBackBuffer(), " bb=",
      res->IsSwapchainBackBuffer() ? res->SwapchainBackBufferIndex() : 0u);
}

static std::string DescriptorSummary(const D3D12Descriptor *desc,
                                     D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  if (!desc)
    return "desc=null";

  auto *res = desc->resource ? static_cast<MTLD3D12Resource *>(desc->resource)
                             : nullptr;
  std::string summary = str::format(
      "desc=", (const void *)desc, " heap_type=", (unsigned)desc->type,
      " range=", DescriptorRangeTypeName(range_type), " ",
      ResourceSummary(res));

  if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
    return str::format(
        summary, " sampler=", (unsigned long long)desc->metal_sampler.handle,
        " sampler_id=", (unsigned long long)desc->metal_sampler_gpu_id,
        " cube_id=", (unsigned long long)desc->metal_sampler_cube_gpu_id);
  }

  if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
    return str::format(summary, " cbv_gpu=0x",
                       (unsigned long long)desc->cbv.BufferLocation,
                       " cbv_size=", desc->cbv.SizeInBytes);
  }

  if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
    auto tex = DescriptorTexture(desc, res);
    return str::format(
        summary, " uav_fmt=", (unsigned)desc->uav.Format,
        " uav_dim=", (unsigned)desc->uav.ViewDimension,
        " uav_counter=", (const void *)desc->resource_uav_counter,
        " view=", (unsigned long long)desc->metal_texture_view.handle,
        " view_id=",
        (unsigned long long)DescriptorTextureGPUResourceID(desc, res),
        " tex=", (unsigned long long)tex.handle,
        " array_len=", UAVTextureArrayLength(desc, res));
  }

  auto tex = DescriptorTexture(desc, res);
  return str::format(
      summary, " srv_fmt=", (unsigned)desc->srv.Format,
      " srv_dim=", (unsigned)desc->srv.ViewDimension,
      " view=", (unsigned long long)desc->metal_texture_view.handle,
      " view_id=",
      (unsigned long long)DescriptorTextureGPUResourceID(desc, res),
      " tex=", (unsigned long long)tex.handle,
      " array_len=", SRVTextureArrayLength(desc, res));
}

struct ReplayState {
  static constexpr uint32_t kVertexBufferSlotCount = 29;
  static constexpr uint32_t kVertexBufferTableSlot = 16;
  static constexpr uint32_t kMSCArgumentBufferSlot = 2;
  static constexpr uint32_t kMSCDrawArgumentsSlot = 4;
  static constexpr uint32_t kMSCUniformsSlot = 5;
  static constexpr uint32_t kMSCVertexBufferBindPoint = 6;
  static constexpr uint32_t kM12VertexPullDrawArgumentsSlot = 29;
  static constexpr uint32_t kM12VertexPullDrawInfoSlot = 30;
  static constexpr uint32_t kM12VertexBufferSignatureSlotCount = 31;
  static constexpr uint16_t kMSCNonIndexedDraw = 0;

  struct VertexBufferEntry {
    uint64_t buffer_handle;
    uint32_t stride;
    uint32_t length;
  };

  struct MSCDrawArgument {
    uint32_t vertexCountPerInstance;
    uint32_t instanceCount;
    uint32_t startVertexLocation;
    uint32_t startInstanceLocation;
  };

  struct MSCDrawIndexedArgument {
    uint32_t indexCountPerInstance;
    uint32_t instanceCount;
    uint32_t startIndexLocation;
    int32_t baseVertexLocation;
    uint32_t startInstanceLocation;
  };

  union MSCDrawParams {
    MSCDrawArgument draw;
    MSCDrawIndexedArgument drawIndexed;
  };

  WMT::CommandBuffer cmdbuf;
  WMT::RenderCommandEncoder render_enc;
  bool render_enc_open = false;
  bool render_enc_has_dsv = false;
  DXGI_FORMAT render_enc_dsv_format = DXGI_FORMAT_UNKNOWN;
  WMT::Reference<WMT::Texture> depth_bounds_dsv_texture;
  uint32_t depth_bounds_dsv_slice = 0;
  uint64_t bound_vertex_buffer_slots = 0;
  uint64_t bound_fragment_buffer_slots = 0;
  uint64_t bound_fragment_texture_slots = 0;
  uint64_t bound_fragment_sampler_slots = 0;
  uint64_t fallback_fragment_buffer_slots = 0;
  uint64_t fallback_fragment_texture_slots = 0;
  uint64_t fallback_fragment_sampler_slots = 0;

  ~ReplayState() { CloseRenderEncoder(); }

  MTLD3D12PipelineState *pso = nullptr;
  ID3D12StateObject *raytracing_state = nullptr;
  WMT::Reference<WMT::ComputePipelineState> raytracing_compute_pso;
  WMT::Reference<WMT::VisibleFunctionTable>
      raytracing_visible_function_table;
  WMT::Reference<WMT::IntersectionFunctionTable>
      raytracing_intersection_function_table;
  D3D12_QUERY_DATA_PIPELINE_STATISTICS1 pipeline_statistics = {};
  MTLD3D12RootSignature *graphics_root_sig = nullptr;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
  D3D12_VERTEX_BUFFER_VIEW vbs[kVertexBufferSlotCount] = {};
  D3D12_INDEX_BUFFER_VIEW ib = {};
  D3D12_VIEWPORT viewports[16] = {};
  uint32_t viewport_count = 0;
  D3D12_RECT scissor_rects[16] = {};
  uint32_t scissor_count = 0;
  float blend_factor[4] = {1, 1, 1, 1};
  uint32_t stencil_ref = 0;
  float depth_bounds_min = 0.0f;
  float depth_bounds_max = 1.0f;
  bool depth_bounds_inverted = false;

  D3D12_CPU_DESCRIPTOR_HANDLE rt_handles[8] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
  uint32_t rt_count = 0;
  bool has_dsv = false;

  ID3D12DescriptorHeap *desc_heaps[2] = {};
  uint32_t desc_heap_count = 0;

  static constexpr uint32_t kRootConstantBytes = 256;
  static constexpr uint32_t kRootParameterSlotCount =
      kD3D12RootParameterSlotCount;
  D3D12_GPU_VIRTUAL_ADDRESS root_cbvs[kRootParameterSlotCount] = {};
  D3D12_GPU_VIRTUAL_ADDRESS root_srvs[kRootParameterSlotCount] = {};
  D3D12_GPU_VIRTUAL_ADDRESS root_uavs[kRootParameterSlotCount] = {};
  D3D12_GPU_DESCRIPTOR_HANDLE root_tables[kRootParameterSlotCount] = {};
  uint8_t root_constants_buf[kRootParameterSlotCount * kRootConstantBytes] = {};
  uint32_t root_constant_offsets[kRootParameterSlotCount] = {};
  uint32_t root_constant_sizes[kRootParameterSlotCount] = {};
  bool root_constant_set[kRootParameterSlotCount] = {};
  bool root_cbv_set[kRootParameterSlotCount] = {};
  bool root_srv_set[kRootParameterSlotCount] = {};
  bool root_uav_set[kRootParameterSlotCount] = {};
  bool root_table_set[kRootParameterSlotCount] = {};

  void ResetGraphicsRootBindings() {
    memset(root_cbvs, 0, sizeof(root_cbvs));
    memset(root_srvs, 0, sizeof(root_srvs));
    memset(root_uavs, 0, sizeof(root_uavs));
    memset(root_tables, 0, sizeof(root_tables));
    memset(root_constants_buf, 0, sizeof(root_constants_buf));
    memset(root_constant_offsets, 0, sizeof(root_constant_offsets));
    memset(root_constant_sizes, 0, sizeof(root_constant_sizes));
    memset(root_constant_set, 0, sizeof(root_constant_set));
    memset(root_cbv_set, 0, sizeof(root_cbv_set));
    memset(root_srv_set, 0, sizeof(root_srv_set));
    memset(root_uav_set, 0, sizeof(root_uav_set));
    memset(root_table_set, 0, sizeof(root_table_set));
  }

  bool HasUsableRenderPSO() const {
    return pso && pso->IsCompiled() && !pso->IsCompute() &&
           pso->GetRenderPSO().handle;
  }

  bool UsesGeometryMeshPipeline() const {
    return pso && pso->UsesGeometryMeshPipeline();
  }

  WMTRenderStages VertexInputStages() const {
    WMTRenderStages stages = WMTRenderStageVertex;
    if (UsesGeometryMeshPipeline())
      stages = (WMTRenderStages)(stages | WMTRenderStageObject);
    return stages;
  }

  WMTRenderStages RootBindingStages() const {
    WMTRenderStages stages =
        (WMTRenderStages)(WMTRenderStageVertex | WMTRenderStageFragment);
    if (UsesGeometryMeshPipeline()) {
      stages =
          (WMTRenderStages)(stages | WMTRenderStageObject | WMTRenderStageMesh);
    }
    return stages;
  }

  bool HasSwapchainRenderTarget() const {
    return SwapchainRenderTargetResource() != nullptr;
  }

  MTLD3D12Resource *SwapchainRenderTargetResource() const {
    for (uint32_t i = 0; i < rt_count && i < 8; i++) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(rt_handles[i].ptr);
      auto *res =
          desc ? static_cast<MTLD3D12Resource *>(desc->resource) : nullptr;
      if (res && res->IsSwapchainBackBuffer())
        return res;
    }
    return nullptr;
  }

  void TrackSwapchainResource(MTLD3D12Resource *resource) {
    if (!resource || !resource->IsSwapchainBackBuffer())
      return;

    if (!swapchain_rt_for_present)
      swapchain_rt_for_present = resource;

    for (uint32_t i = 0; i < swapchain_touched_count; i++) {
      if (swapchain_touched_resources[i] == resource)
        return;
    }
    if (swapchain_touched_count < 4)
      swapchain_touched_resources[swapchain_touched_count++] = resource;
  }

  void MarkSwapchainWorkEncoded(MTLD3D12Resource *resource = nullptr) {
    if (resource) {
      TrackSwapchainResource(resource);
    } else {
      TrackSwapchainResource(SwapchainRenderTargetResource());
    }
    if (swapchain_rt_for_present)
      swapchain_work_encoded = true;
  }

  bool swapchain_work_encoded = false;
  MTLD3D12Resource *swapchain_rt_for_present = nullptr;
  MTLD3D12Resource *swapchain_touched_resources[4] = {};
  uint32_t swapchain_touched_count = 0;
  static constexpr uint32_t kFaultBreadcrumbCount = 16;
  std::string fault_breadcrumbs[kFaultBreadcrumbCount] = {};
  uint32_t fault_breadcrumb_cursor = 0;
  std::string last_vertex_table_summary = "vb=unbound";
  uint32_t last_bound_vertex_buffers = 0;

  void AddFaultBreadcrumb(const std::string &summary) {
    fault_breadcrumbs[fault_breadcrumb_cursor % kFaultBreadcrumbCount] =
        summary;
    fault_breadcrumb_cursor++;
  }

  std::string FormatFaultBreadcrumbs() const {
    if (!fault_breadcrumb_cursor)
      return "none";

    std::string out;
    const uint32_t count =
        std::min<uint32_t>(fault_breadcrumb_cursor, kFaultBreadcrumbCount);
    const uint32_t first = fault_breadcrumb_cursor - count;
    for (uint32_t i = 0; i < count; i++) {
      const uint32_t seq = first + i;
      const auto &entry = fault_breadcrumbs[seq % kFaultBreadcrumbCount];
      if (entry.empty())
        continue;
      if (!out.empty())
        out += " | ";
      out += "#";
      out += std::to_string(seq);
      out += " ";
      out += entry;
    }
    return out.empty() ? "none" : out;
  }

  void AddRenderFaultBreadcrumb(const char *kind, uint32_t element_count,
                                uint32_t instance_count, uint32_t start_element,
                                int32_t base_vertex, uint64_t index_gpu,
                                bool indexed) {
    uint32_t rtv0 = 0;
    uint32_t sample_count = 0;
    uint32_t write_mask0 = 0;
    uint32_t depth_enabled = 0;
    uint32_t stencil_enabled = 0;
    uint32_t num_rts = 0;
    bool compiled = false;
    bool stage_in = false;
    bool geom_mesh = false;
    std::string pso_summary = TracePsoShaderSummary(pso);
    if (pso) {
      const auto &blend = pso->GetBlendDesc();
      const auto &ds = pso->GetDepthStencilDesc();
      compiled = pso->IsCompiled();
      stage_in = pso->UsesStageInVertexDescriptor();
      geom_mesh = pso->UsesGeometryMeshPipeline();
      num_rts = pso->GetNumRenderTargets();
      rtv0 = pso->GetRTVFormat(0);
      sample_count = pso->GetSampleCount();
      write_mask0 = blend.RenderTarget[0].RenderTargetWriteMask;
      depth_enabled = ds.DepthEnable ? 1u : 0u;
      stencil_enabled = ds.StencilEnable ? 1u : 0u;
    }

    AddFaultBreadcrumb(str::format(
        kind, " elems=", element_count, " inst=", instance_count, " start=",
        start_element, " base=", base_vertex, " indexed=", indexed ? 1u : 0u,
        " ib=0x", std::hex, (unsigned long long)index_gpu, std::dec,
        " enc=", render_enc_open, " pso=", (void *)pso, " compiled=", compiled,
        " rts=", num_rts, " rtv0=", rtv0, " sample=", sample_count,
        " write_mask0=0x", std::hex, write_mask0, std::dec,
        " depth=", depth_enabled, " stencil=", stencil_enabled,
        " stage_in=", stage_in, " geom_mesh=", geom_mesh,
        " swapchain=", HasSwapchainRenderTarget(), " rt_count=", rt_count,
        " dsv=", has_dsv, " ", last_vertex_table_summary, " ", pso_summary));
  }

  void AddComputeFaultBreadcrumb(const char *kind, uint32_t x, uint32_t y,
                                 uint32_t z) {
    AddFaultBreadcrumb(str::format(
        kind, " groups=", x, "x", y, "x", z, " pso=", (void *)pso,
        " compiled=", pso ? pso->IsCompiled() : false,
        " compute=", pso ? pso->IsCompute() : false, " heaps=", desc_heap_count,
        " stage=", TraceCompileFailureStage(pso), " detail=",
        TraceCompileFailureDetail(pso), " ", TracePsoShaderSummary(pso)));
  }

  std::string RenderTargetResourceSummary() const {
    std::string out;
    for (uint32_t i = 0; i < rt_count && i < 8; i++) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(rt_handles[i].ptr);
      auto *res = desc && desc->resource
                      ? static_cast<MTLD3D12Resource *>(desc->resource)
                      : nullptr;
      if (!out.empty())
        out += " ";
      out += str::format("rt", i, "={", ResourceSummary(res), "}");
    }
    if (has_dsv) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(dsv_handle.ptr);
      auto *res = desc && desc->resource
                      ? static_cast<MTLD3D12Resource *>(desc->resource)
                      : nullptr;
      if (!out.empty())
        out += " ";
      out += str::format("dsv={", ResourceSummary(res),
                         " stencil=", desc ? DSVHasStencil(desc) : false, "}");
    }
    return out.empty() ? "rt=none" : out;
  }

  bool ShouldSkipUnsafeMSCOffscreenPass() const {
    if (!DXMTD3D12SkipUnsafeMSCOffscreenPass() || !pso ||
        !pso->UsesStageInVertexDescriptor() || HasSwapchainRenderTarget())
      return false;

    const auto rtv0 = pso->GetRTVFormat(0);
    if (!has_dsv) {
      if (rt_count == 1 && last_bound_vertex_buffers == 0) {
        __atomic_add_fetch(&g_quarantine_zero_vb_offscreen, 1,
                           __ATOMIC_RELAXED);
        return true;
      }
      return false;
    }

    if (!pso->RequiresMSCStageInFunction())
      return false;

    if (rt_count == 1 && rtv0 == DXGI_FORMAT_R16G16B16A16_UNORM) {
      __atomic_add_fetch(&g_quarantine_r16_dsv, 1, __ATOMIC_RELAXED);
      return true;
    }

    if (rt_count >= 3 && pso->GetNumRenderTargets() >= 3 &&
        rtv0 == DXGI_FORMAT_R8G8B8A8_UNORM) {
      __atomic_add_fetch(&g_quarantine_rgba8_mrt_dsv, 1, __ATOMIC_RELAXED);
      return true;
    }

    const auto &ds = pso->GetDepthStencilDesc();
    if (rt_count == 1 && rtv0 == DXGI_FORMAT_B8G8R8A8_UNORM && ds.DepthEnable &&
        ds.StencilEnable) {
      __atomic_add_fetch(&g_quarantine_b8g8r8a8_dsv_stencil, 1,
                         __ATOMIC_RELAXED);
      return true;
    }

    if (rt_count == 1 && rtv0 == DXGI_FORMAT_R11G11B10_FLOAT &&
        ds.DepthEnable && ds.StencilEnable) {
      __atomic_add_fetch(&g_quarantine_r11g11b10_dsv_stencil, 1,
                         __ATOMIC_RELAXED);
      return true;
    }

    if (rt_count >= 5 && pso->GetNumRenderTargets() >= 5 && ds.StencilEnable &&
        rtv0 == DXGI_FORMAT_R11G11B10_FLOAT) {
      __atomic_add_fetch(&g_quarantine_r11g11b10_gbuffer, 1, __ATOMIC_RELAXED);
      return true;
    }

    return false;
  }

  const char *UnsafeMSCOffscreenPassReason() const {
    if (!pso)
      return "unknown";

    const auto rtv0 = pso->GetRTVFormat(0);
    if (rt_count == 1 && rtv0 == DXGI_FORMAT_R16G16B16A16_UNORM)
      return "r16_dsv";
    if (rt_count >= 3 && pso->GetNumRenderTargets() >= 3 &&
        rtv0 == DXGI_FORMAT_R8G8B8A8_UNORM)
      return "rgba8_mrt_dsv";
    const auto &ds = pso->GetDepthStencilDesc();
    if (rt_count == 1 && rtv0 == DXGI_FORMAT_B8G8R8A8_UNORM && ds.DepthEnable &&
        ds.StencilEnable)
      return "b8g8r8a8_scene_dsv_stencil_stage_in";
    if (rt_count == 1 && rtv0 == DXGI_FORMAT_R11G11B10_FLOAT &&
        ds.DepthEnable && ds.StencilEnable)
      return "r11g11b10_scene_dsv_stencil_stage_in";
    if (rt_count >= 5 && pso->GetNumRenderTargets() >= 5 && ds.StencilEnable &&
        rtv0 == DXGI_FORMAT_R11G11B10_FLOAT)
      return "r11g11b10_gbuffer_dsv_stencil";
    if (!has_dsv && rt_count == 1 && last_bound_vertex_buffers == 0)
      return "zero_vb_stage_in_offscreen";
    return "unknown";
  }

  bool ShouldSkipUnsafeMSCIndexedStageInDraw(
      MTLD3D12Device *device, uint32_t index_count, uint32_t instance_count,
      uint32_t start_index, int32_t base_vertex, uint32_t start_instance,
      std::string &reason) const {
    reason.clear();
    if (!device || !pso || HasSwapchainRenderTarget() ||
        !pso->UsesStageInVertexDescriptor() ||
        !pso->RequiresMSCStageInFunction() || !ib.BufferLocation ||
        !index_count || !instance_count || index_count < 4096)
      return false;

    const auto &input_layout = pso->GetInputLayout();
    if (!input_layout.NumElements || !input_layout.pInputElementDescs)
      return false;

    auto *ib_res = device->LookupResourceByGPUAddress(ib.BufferLocation);
    if (!ib_res && ib.BufferLocation)
      ib_res = reinterpret_cast<MTLD3D12Resource *>(ib.BufferLocation);
    if (!ib_res)
      return false;

    const uint32_t index_size = ib.Format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
    if (ib.Format != DXGI_FORMAT_R16_UINT && ib.Format != DXGI_FORMAT_R32_UINT)
      return false;

    D3D12_RESOURCE_DESC ib_desc = {};
    ib_res->GetDesc(&ib_desc);
    const uint64_t base_offset =
        ib.BufferLocation - ib_res->GetGPUVirtualAddress();
    const uint64_t index_offset =
        base_offset + uint64_t(start_index) * index_size;
    const uint64_t index_bytes = uint64_t(index_count) * index_size;
    if (index_offset > ib_desc.Width ||
        index_bytes > ib_desc.Width - index_offset) {
      reason = str::format("index_range_oob idx=", index_count,
                           " start=", start_index, " offset=", index_offset,
                           " bytes=", index_bytes, " ib_width=", ib_desc.Width);
      return true;
    }

    void *index_base = nullptr;
    D3D12_RANGE read_range = {index_offset, index_offset + index_bytes};
    HRESULT hr = ib_res->Map(0, &read_range, &index_base);
    if (FAILED(hr) || !index_base)
      return false;

    uint32_t min_index = std::numeric_limits<uint32_t>::max();
    uint32_t max_index = 0;
    const uint8_t *index_bytes_ptr =
        static_cast<const uint8_t *>(index_base) + index_offset;
    if (index_size == 4) {
      for (uint32_t i = 0; i < index_count; i++) {
        uint32_t value = 0;
        std::memcpy(&value, index_bytes_ptr + uint64_t(i) * index_size,
                    sizeof(value));
        min_index = std::min(min_index, value);
        max_index = std::max(max_index, value);
      }
    } else {
      for (uint32_t i = 0; i < index_count; i++) {
        uint16_t value = 0;
        std::memcpy(&value, index_bytes_ptr + uint64_t(i) * index_size,
                    sizeof(value));
        min_index = std::min<uint32_t>(min_index, value);
        max_index = std::max<uint32_t>(max_index, value);
      }
    }
    ib_res->Unmap(0, nullptr);

    const int64_t min_vertex_id = int64_t(base_vertex) + int64_t(min_index);
    const int64_t max_vertex_id = int64_t(base_vertex) + int64_t(max_index);
    if (min_vertex_id < 0) {
      reason = str::format("negative_vertex_id min=", min_vertex_id,
                           " base=", base_vertex, " min_index=", min_index);
      return true;
    }

    uint64_t required_per_slot[kVertexBufferSlotCount] = {};
    for (UINT i = 0; i < input_layout.NumElements; i++) {
      const auto &element = input_layout.pInputElementDescs[i];
      if (element.InputSlot >= kVertexBufferSlotCount)
        continue;

      uint64_t required = uint64_t(max_vertex_id) + 1ull;
      if (element.InputSlotClass ==
          D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
        if (element.InstanceDataStepRate == 0) {
          required = uint64_t(start_instance) + 1ull;
        } else if (!instance_count) {
          required = 0;
        } else {
          const uint64_t last_local_instance = uint64_t(instance_count) - 1ull;
          required = uint64_t(start_instance) +
                     (last_local_instance / element.InstanceDataStepRate) +
                     1ull;
        }
      }
      required_per_slot[element.InputSlot] =
          std::max(required_per_slot[element.InputSlot], required);
    }

    for (uint32_t slot = 0; slot < kVertexBufferSlotCount; slot++) {
      const uint64_t required = required_per_slot[slot];
      if (!required)
        continue;

      const auto &view = vbs[slot];
      if (!view.BufferLocation || !view.StrideInBytes) {
        reason = str::format("missing_vb slot=", slot, " required=", required,
                             " stride=", view.StrideInBytes);
        return true;
      }

      const uint64_t available =
          view.StrideInBytes ? view.SizeInBytes / view.StrideInBytes : 0;
      if (required > available) {
        reason =
            str::format("vb_range_oob slot=", slot, " required=", required,
                        " available=", available, " size=", view.SizeInBytes,
                        " stride=", view.StrideInBytes, " idx=", index_count,
                        " max_index=", max_index, " base=", base_vertex,
                        " inst=", instance_count);
        return true;
      }
    }

    return false;
  }

  D3D12DrawSafetyResult
  ValidateDrawSafety(MTLD3D12Device *device, uint32_t element_count,
                     uint32_t instance_count, uint32_t start_element,
                     int32_t base_vertex, uint32_t start_instance,
                     bool indexed) const {
    D3D12DrawSafetyDesc desc = {};
    desc.pso_present = pso != nullptr;
    desc.pso_compiled = pso && pso->IsCompiled();
    desc.pso_is_compute = pso && pso->IsCompute();
    desc.render_pso_ready = pso && pso->GetRenderPSO().handle;
    desc.expect_compact_vertex_table =
        pso && pso->GetIAInputSlotMask() && !pso->UsesStageInVertexDescriptor();
    desc.element_count = element_count;
    desc.instance_count = instance_count;
    desc.start_element = start_element;
    desc.base_vertex = base_vertex;
    desc.start_instance = start_instance;

    if (pso) {
      desc.inputs.reserve(pso->GetIAInputElements().size());
      for (const auto &input : pso->GetIAInputElements()) {
        D3D12ResolvedIAInputElementMetadata safe_input;
        safe_input.semantic_name = input.semantic_name;
        safe_input.semantic_index = input.semantic_index;
        safe_input.shader_register = input.shader_register;
        safe_input.input_slot = input.input_slot;
        safe_input.table_index = input.table_index;
        safe_input.table_indexing_mode = input.table_indexing_mode;
        safe_input.aligned_byte_offset = input.aligned_byte_offset;
        safe_input.dxgi_format = input.dxgi_format;
        safe_input.metal_format = input.metal_format;
        safe_input.bytes_per_element = input.bytes_per_element;
        safe_input.input_slot_class =
            input.per_instance ? D3D12VertexInputSlotClass::PerInstance
                               : D3D12VertexInputSlotClass::PerVertex;
        safe_input.instance_step_rate = input.instance_step_rate;
        safe_input.system_value = input.system_value;
        desc.inputs.push_back(std::move(safe_input));
      }
    }

    uint32_t used_slots = 0;
    for (const auto &input : desc.inputs) {
      if (!input.system_value && input.input_slot < kVertexBufferSlotCount)
        used_slots |= 1u << input.input_slot;
    }
    for (uint32_t slot = 0; slot < kVertexBufferSlotCount; slot++) {
      if (!(used_slots & (1u << slot)) && !vbs[slot].BufferLocation)
        continue;

      auto *res =
          vbs[slot].BufferLocation
              ? device->LookupResourceByGPUAddress(vbs[slot].BufferLocation)
              : nullptr;
      D3D12DrawSafetyVertexBuffer view = {};
      view.input_slot = slot;
      view.buffer_location = vbs[slot].BufferLocation;
      view.size_in_bytes = vbs[slot].SizeInBytes;
      view.stride_in_bytes = vbs[slot].StrideInBytes;
      view.view_supplied = vbs[slot].BufferLocation != 0;
      view.gpu_address_resolved = res && res->GetMTLBuffer().handle;
      desc.vertex_buffers.push_back(view);
    }

    desc.index_range.indexed = indexed;
    if (indexed) {
      desc.index_range.index_buffer_supplied = ib.BufferLocation != 0;
      desc.index_range.index_buffer_location = ib.BufferLocation;
      desc.index_range.index_size =
          ib.Format == DXGI_FORMAT_R32_UINT
              ? 4u
              : (ib.Format == DXGI_FORMAT_R16_UINT ? 2u : 0u);

      auto *ib_res = ib.BufferLocation
                         ? device->LookupResourceByGPUAddress(ib.BufferLocation)
                         : nullptr;
      desc.index_range.index_buffer_resolved =
          ib_res && ib_res->GetMTLBuffer().handle;
      if (ib_res) {
        D3D12_RESOURCE_DESC ib_desc = {};
        ib_res->GetDesc(&ib_desc);
        desc.index_range.index_buffer_size = ib_desc.Width;
        desc.index_range.index_buffer_offset =
            ib.BufferLocation - ib_res->GetGPUVirtualAddress();
        desc.index_range.index_buffer_offset +=
            uint64_t(start_element) * desc.index_range.index_size;

        if (desc.index_range.index_size) {
          const uint64_t index_bytes =
              uint64_t(element_count) * desc.index_range.index_size;
          if (desc.index_range.index_buffer_offset <= ib_desc.Width &&
              index_bytes <=
                  ib_desc.Width - desc.index_range.index_buffer_offset) {
            void *index_base = nullptr;
            D3D12_RANGE read_range = {desc.index_range.index_buffer_offset,
                                      desc.index_range.index_buffer_offset +
                                          index_bytes};
            HRESULT hr = ib_res->Map(0, &read_range, &index_base);
            if (SUCCEEDED(hr) && index_base) {
              const auto *index_bytes_ptr =
                  static_cast<const uint8_t *>(index_base) +
                  desc.index_range.index_buffer_offset;
              uint32_t min_index = std::numeric_limits<uint32_t>::max();
              uint32_t max_index = 0;
              if (desc.index_range.index_size == 4) {
                for (uint32_t i = 0; i < element_count; i++) {
                  uint32_t value = 0;
                  std::memcpy(&value,
                              index_bytes_ptr +
                                  uint64_t(i) * desc.index_range.index_size,
                              sizeof(value));
                  min_index = std::min(min_index, value);
                  max_index = std::max(max_index, value);
                }
              } else {
                for (uint32_t i = 0; i < element_count; i++) {
                  uint16_t value = 0;
                  std::memcpy(&value,
                              index_bytes_ptr +
                                  uint64_t(i) * desc.index_range.index_size,
                              sizeof(value));
                  min_index = std::min<uint32_t>(min_index, value);
                  max_index = std::max<uint32_t>(max_index, value);
                }
              }
              ib_res->Unmap(0, nullptr);
              desc.index_range.has_min_max_index = true;
              desc.index_range.min_index = min_index;
              desc.index_range.max_index = max_index;
            }
          }
        }
      }
    }

    return D3D12ValidateDrawSafety(desc);
  }

  void LogDrawSafetySkip(const char *draw_kind,
                         const D3D12DrawSafetyResult &result,
                         uint32_t element_count, uint32_t instance_count,
                         uint32_t start_element, int32_t base_vertex,
                         uint32_t start_instance, bool indexed) const {
    if (!D3D12DrawSafetySkipped(result) ||
        !TakeLogBudget(&g_draw_safety_skip_logs, 256))
      return;

    Logger::warn(str::format(
        "M12 skipping unsafe ", draw_kind,
        " reason=", D3D12DrawSafetySkipReasonName(result.reason),
        " pso=", (void *)pso, " ", TracePsoShaderSummary(pso),
        " slot=", result.input_slot, " table=", result.table_index, " gpu=0x",
        std::hex, (unsigned long long)result.gpu_address, std::dec,
        " size=", result.size_in_bytes, " stride=", result.stride_in_bytes,
        " required=", (unsigned long long)result.required_vertices,
        " available=", (unsigned long long)result.available_vertices,
        " elems=", element_count, " inst=", instance_count,
        " start=", start_element, " base=", base_vertex,
        " start_inst=", start_instance, " indexed=", indexed ? 1u : 0u,
        " enc_open=", render_enc_open, " render_handle=",
        (unsigned long long)(pso ? pso->GetRenderPSO().handle : 0),
        " stage=", TraceCompileFailureStage(pso),
        " detail=", TraceCompileFailureDetail(pso)));
  }

  void LogNativeVertexResolved(const char *draw_kind, uint32_t element_count,
                               uint32_t instance_count, uint32_t start_element,
                               int32_t base_vertex, uint32_t start_instance,
                               bool indexed) const {
    if (!indexed || !TakeLogBudget(&g_native_vertex_resolve_logs, 256))
      return;

    const uint32_t index_size =
        ib.Format == DXGI_FORMAT_R32_UINT
            ? 4u
            : (ib.Format == DXGI_FORMAT_R16_UINT ? 2u : 0u);
    Logger::info(str::format(
        "M12 native vertex path resolved draw=", draw_kind, " pso=",
        (void *)pso, " ", TracePsoShaderSummary(pso), " elems=", element_count,
        " inst=", instance_count, " start=", start_element,
        " base=", base_vertex, " start_inst=", start_instance,
        " index_format=", (unsigned)ib.Format, " index_size=", index_size,
        " ib_gpu=0x", std::hex, (unsigned long long)ib.BufferLocation, std::dec,
        " ib_view_size=", ib.SizeInBytes, " index_byte_offset=",
        uint64_t(start_element) * index_size, " input_slot_mask=0x", std::hex,
        (unsigned)(pso ? pso->GetIAInputSlotMask() : 0u), std::dec,
        " vb0_gpu=0x", std::hex, (unsigned long long)vbs[0].BufferLocation,
        std::dec, " vb0_size=", vbs[0].SizeInBytes,
        " vb0_stride=", vbs[0].StrideInBytes));
  }

  struct RenderReadbackProbe {
    WMT::Reference<WMT::Buffer> buffer;
    void *mapped = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bytes_per_row = 0;
    uint32_t backbuffer = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t capture = 0;
  } render_readback;

  void CaptureSwapchainRenderReadback(MTLD3D12Device *device,
                                      WMT::CommandBuffer &cmdbuf) {
    if (!DXMTD3D12SwapchainRenderReadback() || !swapchain_work_encoded ||
        !swapchain_rt_for_present || render_readback.buffer.handle)
      return;

    uint32_t capture = __atomic_add_fetch(&g_swapchain_render_readback_captures,
                                          1, __ATOMIC_RELAXED);
    if (capture > 16 && (capture % 30) != 0)
      return;

    auto tex = swapchain_rt_for_present->GetMTLTexture();
    if (!tex.handle)
      return;

    D3D12_RESOURCE_DESC desc = {};
    swapchain_rt_for_present->GetDesc(&desc);
    uint32_t width =
        (uint32_t)std::min<UINT64>(std::max<UINT64>(desc.Width, 1), 1920);
    uint32_t height =
        (uint32_t)std::min<UINT>(std::max<UINT>(desc.Height, 1), 1080);
    uint32_t bytes_per_row = AlignReadbackPitch(width * 4u, 256u);

    WMTBufferInfo info = {};
    info.length = uint64_t(bytes_per_row) * height;
    info.options =
        WMTResourceStorageModeShared | WMTResourceHazardTrackingModeTracked;
    info.memory.set(nullptr);
    auto buffer = device->GetDXMTDevice().device().newBuffer(info);
    void *mapped = info.memory.get();
    if (!buffer.handle || !mapped) {
      Logger::info(
          str::format("M12 swapchain render readback unavailable "
                      "capture=",
                      capture, " backbuffer=",
                      swapchain_rt_for_present->SwapchainBackBufferIndex()));
      return;
    }

    RetainMTLObjectForCompletion(tex);
    RetainMTLObjectForCompletion(buffer);
    auto blit = cmdbuf.blitCommandEncoder();
    ENC_CREATE("blit_swapchain_render_readback", blit.handle);
    ScopedMetalEncoderEnd blit_guard{blit, "blit_swapchain_render_readback"};
    if (!blit.handle)
      return;

    struct wmtcmd_blit_copy_from_texture_to_buffer copy = {};
    copy.type = WMTBlitCommandCopyFromTextureToBuffer;
    copy.next.set(nullptr);
    copy.src = tex;
    copy.slice = 0;
    copy.level = 0;
    copy.origin = {0, 0, 0};
    copy.size = {width, height, 1};
    copy.dst = buffer;
    copy.offset = 0;
    copy.bytes_per_row = bytes_per_row;
    copy.bytes_per_image = bytes_per_row * height;
    blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
    EndMetalEncoder(blit, "blit_swapchain_render_readback");

    render_readback.buffer = buffer;
    render_readback.mapped = mapped;
    render_readback.width = width;
    render_readback.height = height;
    render_readback.bytes_per_row = bytes_per_row;
    render_readback.backbuffer =
        swapchain_rt_for_present->SwapchainBackBufferIndex();
    render_readback.format = desc.Format;
    render_readback.capture = capture;
  }

  void LogSwapchainRenderReadback() {
    if (!render_readback.mapped || !render_readback.width ||
        !render_readback.height)
      return;

    uint64_t nonzero_pixels = 0;
    uint64_t nonzero_bytes = 0;
    uint8_t max_byte = 0;
    uint64_t checksum = 1469598103934665603ull;
    const auto *rows = static_cast<const uint8_t *>(render_readback.mapped);
    for (uint32_t y = 0; y < render_readback.height; y++) {
      const auto *row = rows + uint64_t(y) * render_readback.bytes_per_row;
      for (uint32_t x = 0; x < render_readback.width; x++) {
        const auto *px = row + x * 4u;
        bool pixel_nonzero = false;
        for (uint32_t i = 0; i < 4; i++) {
          uint8_t value = px[i];
          if (value) {
            pixel_nonzero = true;
            nonzero_bytes++;
            max_byte = std::max(max_byte, value);
          }
          checksum ^= value;
          checksum *= 1099511628211ull;
        }
        if (pixel_nonzero)
          nonzero_pixels++;
      }
    }

    Logger::info(str::format(
        "M12 swapchain render readback capture=", render_readback.capture,
        " backbuffer=", render_readback.backbuffer,
        " fmt=", (unsigned)render_readback.format,
        " sample=", render_readback.width, "x", render_readback.height,
        " nonzero_pixels=", nonzero_pixels, " nonzero_bytes=", nonzero_bytes,
        " max_byte=", (unsigned)max_byte, " checksum=0x", std::hex, checksum));
  }

  void ForceSwapchainDiagnosticColor(WMT::CommandBuffer &cmdbuf) {
    if (!DXMTD3D12ForceSwapchainColor() || !swapchain_work_encoded ||
        !swapchain_rt_for_present)
      return;

    auto tex = swapchain_rt_for_present->GetMTLTexture();
    if (!tex.handle)
      return;

    WMTRenderPassInfo rp = {};
    WMT::InitializeRenderPassInfo(rp);
    for (uint32_t i = 0; i < 8; i++) {
      rp.colors[i].texture = NULL_OBJECT_HANDLE;
      rp.colors[i].load_action = WMTLoadActionDontCare;
      rp.colors[i].store_action = WMTStoreActionDontCare;
    }
    rp.colors[0].texture = tex.handle;
    RetainMTLObjectForCompletion(tex);
    rp.colors[0].load_action = WMTLoadActionClear;
    rp.colors[0].store_action = WMTStoreActionStore;
    rp.colors[0].clear_color = {1.0, 0.0, 1.0, 1.0};
    rp.depth.texture = NULL_OBJECT_HANDLE;
    rp.stencil.texture = NULL_OBJECT_HANDLE;

    auto enc = cmdbuf.renderCommandEncoder(rp);
    ENC_CREATE("render_swapchain_forced_color", enc.handle);
    ScopedMetalEncoderEnd enc_guard{enc, "render_swapchain_forced_color"};
    EndMetalEncoder(enc, "render_swapchain_forced_color");
    if (TakeLogBudget(&g_swapchain_forced_color_logs, 16)) {
      Logger::info(str::format(
          "M12 swapchain forced diagnostic color "
          "backbuffer=",
          swapchain_rt_for_present->SwapchainBackBufferIndex(),
          " tex=", (unsigned long long)tex.handle, " color=1,0,1,1"));
    }
  }

  void EnsureSwapchainRenderPSOReady() {
    if (!pso || pso->IsCompiled())
      return;

    bool compiled = pso->EnsureCompiled();
    Logger::info(str::format("M12 render PSO ready compiled=", compiled,
                             " pso=", (void *)pso,
                             " swapchain=", HasSwapchainRenderTarget(),
                             " stage=", TraceCompileFailureStage(pso),
                             " detail=", TraceCompileFailureDetail(pso)));
  }

  void LogFinalRenderSnapshot(const char *draw_kind, uint32_t element_count,
                              uint32_t instance_count, uint32_t start_element) {
    if (!DXMTD3D12FinalRenderSnapshot() || !HasSwapchainRenderTarget() || !pso)
      return;

    uint32_t capture = __atomic_add_fetch(&g_swapchain_final_snapshot_logs, 1,
                                          __ATOMIC_RELAXED);
    if (capture > 32 && (capture % 60) != 0)
      return;

    Logger::info(str::format(
        "M12 final render snapshot #", capture, " draw=", draw_kind, " elems=",
        element_count, " inst=", instance_count, " start=", start_element,
        " enc=", (unsigned long long)render_enc.handle, " pso=", (void *)pso,
        " compiled=", pso->IsCompiled(),
        " render_handle=", (unsigned long long)pso->GetRenderPSO().handle,
        " geom_mesh=", pso->UsesGeometryMeshPipeline(), " ",
        TracePsoShaderSummary(pso)));

    const auto &blend = pso->GetBlendDesc();
    Logger::info(str::format(
        "M12 final pso color rts=", (unsigned)pso->GetNumRenderTargets(),
        " rtv0=", (unsigned)pso->GetRTVFormat(0),
        " sample_count=", (unsigned)pso->GetSampleCount(), " blend0=",
        (unsigned)blend.RenderTarget[0].BlendEnable, " write_mask0=0x",
        std::hex, (unsigned)blend.RenderTarget[0].RenderTargetWriteMask,
        std::dec, " logic_op0=", (unsigned)blend.RenderTarget[0].LogicOpEnable,
        " src0=", (unsigned)blend.RenderTarget[0].SrcBlend,
        " dst0=", (unsigned)blend.RenderTarget[0].DestBlend,
        " op0=", (unsigned)blend.RenderTarget[0].BlendOp));

    for (uint32_t i = 0; i < rt_count && i < 8; i++) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(rt_handles[i].ptr);
      auto *res =
          desc ? static_cast<MTLD3D12Resource *>(desc->resource) : nullptr;
      Logger::info(str::format(
          "M12 final RTV slot=", i, " handle=0x",
          (unsigned long long)rt_handles[i].ptr,
          " rtv_fmt=", desc ? (unsigned)desc->rtv.Format : 0u,
          " rtv_dim=", desc ? (unsigned)desc->rtv.ViewDimension : 0u, " ",
          ResourceSummary(res)));
    }

    if (has_dsv) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(dsv_handle.ptr);
      auto *res =
          desc ? static_cast<MTLD3D12Resource *>(desc->resource) : nullptr;
      Logger::info(str::format(
          "M12 final DSV handle=0x", (unsigned long long)dsv_handle.ptr,
          " dsv_fmt=", desc ? (unsigned)desc->dsv.Format : 0u,
          " dsv_dim=", desc ? (unsigned)desc->dsv.ViewDimension : 0u,
          " stencil=", desc ? DSVHasStencil(desc) : false, " ",
          ResourceSummary(res)));
    }

    auto *sig = graphics_root_sig;
    if (!sig && pso->GetRootSignature())
      sig = static_cast<MTLD3D12RootSignature *>(pso->GetRootSignature());
    if (!sig) {
      Logger::info("M12 final roots root_sig=null");
      return;
    }

    const auto &params = sig->GetParameters();
    Logger::info(
        str::format("M12 final root signature params=", (unsigned)params.size(),
                    " heaps=", desc_heap_count));
    for (uint32_t i = 0; i < params.size() && i < kRootParameterSlotCount;
         i++) {
      const auto &param = params[i];
      Logger::info(str::format(
          "M12 final root[", i, "] type=", RootParameterTypeName(param.type),
          " vis=", ShaderVisibilityName(param.shader_visibility),
          " reg=", param.register_index, " space=", param.register_space,
          " constants=", root_constant_set[i], " cbv=", root_cbv_set[i],
          " srv=", root_srv_set[i], " uav=", root_uav_set[i],
          " table=", root_table_set[i], " table_gpu=0x",
          (unsigned long long)root_tables[i].ptr,
          " const_size=", root_constant_sizes[i], " root_cbv=0x",
          (unsigned long long)root_cbvs[i], " root_srv=0x",
          (unsigned long long)root_srvs[i], " root_uav=0x",
          (unsigned long long)root_uavs[i]));

      if (param.type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ||
          !root_table_set[i])
        continue;

      for (uint32_t r = 0; r < param.ranges.size(); r++) {
        const auto &range = param.ranges[r];
        uint32_t descriptor_count =
            range.num_descriptors == UINT32_MAX ? 1u : range.num_descriptors;
        uint32_t inspect_count = std::min<uint32_t>(descriptor_count, 4u);
        Logger::info(str::format(
            "M12 final root[", i, "] range[", r,
            "] type=", DescriptorRangeTypeName(range.range_type),
            " base=", range.base_register, " space=", range.register_space,
            " offset=", range.offset_in_table,
            " descriptors=", descriptor_count, " inspect=", inspect_count));

        for (uint32_t d = 0; d < inspect_count; d++) {
          D3D12Descriptor *desc = nullptr;
          for (uint32_t h = 0; h < desc_heap_count && !desc; h++) {
            auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
            if (heap) {
              desc = heap->GetDescriptorFromGPUHandle(
                  root_tables[i], range.offset_in_table + d);
            }
          }
          Logger::info(str::format("M12 final root[", i, "] range[", r,
                                   "] desc[", d,
                                   "] reg=", range.base_register + d, " ",
                                   DescriptorSummary(desc, range.range_type)));
        }
      }
    }
  }

  void LogStageInVertexSnapshot(const char *draw_kind, uint32_t element_count,
                                uint32_t instance_count) {
    if (!pso || !pso->UsesStageInVertexDescriptor())
      return;
    if (!HasSwapchainRenderTarget())
      return;
    uint32_t capture =
        __atomic_add_fetch(&g_stage_in_snapshot_count, 1, __ATOMIC_RELAXED);
    if (capture > 16)
      return;

    const auto &input_layout = pso->GetInputLayout();
    Logger::info(str::format(
        "M12 stage_in vertex snapshot #", capture, " draw=", draw_kind,
        " elems=", element_count, " inst=", instance_count,
        " pso=", (void *)pso, " stage_in=", pso->UsesStageInVertexDescriptor(),
        " msc_stage_in=", pso->RequiresMSCStageInFunction(),
        " il_elements=", input_layout.NumElements, " slot_mask=0x", std::hex,
        pso->GetIAInputSlotMask(), std::dec, " bound_vbs=",
        last_bound_vertex_buffers, " ", TracePsoShaderSummary(pso)));

    for (UINT i = 0; i < input_layout.NumElements && i < 16; i++) {
      const auto &el = input_layout.pInputElementDescs[i];
      Logger::info(str::format(
          "M12 stage_in il[", i,
          "] semantic=", el.SemanticName ? el.SemanticName : "?",
          el.SemanticIndex, " fmt=", (unsigned)el.Format,
          " slot=", el.InputSlot, " offset=", el.AlignedByteOffset,
          " class=", el.InputSlotClass, " step=", el.InstanceDataStepRate));
    }

    for (uint32_t slot = 0; slot < kVertexBufferSlotCount; slot++) {
      if (!(pso->GetIAInputSlotMask() & (1u << slot)))
        continue;
      const auto &vb = vbs[slot];
      Logger::info(str::format("M12 stage_in vb[", slot, "] gpu=0x",
                               (unsigned long long)vb.BufferLocation, " size=",
                               vb.SizeInBytes, " stride=", vb.StrideInBytes));
    }
  }

  void LogNonStageInVertexSnapshot(MTLD3D12Device *device,
                                   const char *draw_kind, uint32_t vertex_count,
                                   uint32_t instance_count,
                                   uint32_t start_vertex,
                                   uint32_t start_instance) {
    if (!device || !pso || pso->UsesStageInVertexDescriptor() ||
        !HasSwapchainRenderTarget())
      return;
    if (!TakeLogBudget(&g_swapchain_vertex_sample_logs, 64))
      return;

    const auto &inputs = pso->GetIAInputElements();
    Logger::info(str::format(
        "M12 vertex-pull snapshot draw=", draw_kind, " v=", vertex_count,
        " i=", instance_count, " start_vertex=", start_vertex,
        " start_instance=", start_instance, " slot_mask=0x", std::hex,
        pso->GetIAInputSlotMask(), std::dec, " inputs=", inputs.size(),
        " bound_vbs=", last_bound_vertex_buffers,
        " table=", last_vertex_table_summary, " pso=", (void *)pso, " ",
        TracePsoShaderSummary(pso)));

    for (const auto &input : inputs) {
      if (input.system_value || input.input_slot >= kVertexBufferSlotCount)
        continue;

      const auto &vb = vbs[input.input_slot];
      Logger::info(str::format(
          "M12 vertex-pull input semantic=", input.semantic_name,
          input.semantic_index, " reg=", input.shader_register,
          " slot=", input.input_slot, " table=", input.table_index, " offset=",
          input.aligned_byte_offset, " fmt=", (unsigned)input.dxgi_format,
          " metal_fmt=", (unsigned)input.metal_format, " per_instance=",
          input.per_instance, " step=", input.instance_step_rate, " vb_gpu=0x",
          std::hex, (unsigned long long)vb.BufferLocation, std::dec,
          " vb_size=", vb.SizeInBytes, " vb_stride=", vb.StrideInBytes));
    }

    const uint32_t sample_vertices = std::min<uint32_t>(vertex_count, 4);
    for (uint32_t slot = 0; slot < kVertexBufferSlotCount; slot++) {
      const auto &vb = vbs[slot];
      if (!vb.BufferLocation || !vb.StrideInBytes)
        continue;

      bool slot_used = false;
      for (const auto &input : inputs) {
        if (!input.system_value && input.input_slot == slot) {
          slot_used = true;
          break;
        }
      }
      if (!slot_used)
        continue;

      auto *vb_res = device->LookupResourceByGPUAddress(vb.BufferLocation);
      if (!vb_res) {
        Logger::warn(str::format(
            "M12 vertex-pull sample slot=", slot, " unresolved vb_gpu=0x",
            std::hex, (unsigned long long)vb.BufferLocation, std::dec,
            " size=", vb.SizeInBytes, " stride=", vb.StrideInBytes));
        continue;
      }

      D3D12_RESOURCE_DESC vb_desc = {};
      vb_res->GetDesc(&vb_desc);
      const uint64_t vb_base_offset =
          vb.BufferLocation - vb_res->GetGPUVirtualAddress();
      const size_t sample_bytes =
          std::min<size_t>(std::max<UINT>(vb.StrideInBytes, 1), 128);

      for (uint32_t n = 0; n < sample_vertices; n++) {
        const uint64_t vertex_id = uint64_t(start_vertex) + n;
        const uint64_t vertex_offset =
            vb_base_offset + vertex_id * uint64_t(vb.StrideInBytes);
        HRESULT map_hr = E_FAIL;
        std::string bytes_text;
        std::string attrs;
        bool sampled = false;

        if (vertex_offset + sample_bytes <= vb_desc.Width) {
          void *base = nullptr;
          D3D12_RANGE read_range = {vertex_offset,
                                    vertex_offset + sample_bytes};
          map_hr = vb_res->Map(0, &read_range, &base);
          if (SUCCEEDED(map_hr) && base) {
            const auto *bytes =
                static_cast<const uint8_t *>(base) + vertex_offset;
            bytes_text = FormatDebugBytes(bytes, sample_bytes);
            for (const auto &input : inputs) {
              if (input.system_value || input.input_slot != slot)
                continue;
              if (!attrs.empty())
                attrs += " ";
              const uint32_t attr_offset = input.aligned_byte_offset;
              const size_t available =
                  attr_offset < sample_bytes ? sample_bytes - attr_offset : 0;
              attrs += str::format(input.semantic_name, input.semantic_index,
                                   "/r", input.shader_register, "=(",
                                   DecodeDebugVertexValue(bytes + attr_offset,
                                                          available,
                                                          input.dxgi_format),
                                   ")");
            }
            sampled = true;
            vb_res->Unmap(0, nullptr);
          }
        }

        Logger::info(str::format(
            "M12 vertex-pull sample slot=", slot, " n=", n,
            " vertex_id=", (unsigned long long)vertex_id, " vb_gpu=0x",
            std::hex, (unsigned long long)vb.BufferLocation, std::dec, " res=",
            (void *)vb_res, " base_off=", (unsigned long long)vb_base_offset,
            " vertex_off=", (unsigned long long)vertex_offset,
            " stride=", vb.StrideInBytes, " sampled=", sampled, " hr=0x",
            std::hex, (unsigned)map_hr, std::dec, " attrs=[", attrs,
            "] bytes=[", bytes_text, "]"));
      }
    }
  }

  MTLD3D12RootSignature *compute_root_sig = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS comp_cbvs[kRootParameterSlotCount] = {};
  D3D12_GPU_VIRTUAL_ADDRESS comp_srvs[kRootParameterSlotCount] = {};
  D3D12_GPU_VIRTUAL_ADDRESS comp_uavs[kRootParameterSlotCount] = {};
  D3D12_GPU_DESCRIPTOR_HANDLE comp_tables[kRootParameterSlotCount] = {};
  uint8_t comp_constants_buf[kRootParameterSlotCount * kRootConstantBytes] = {};
  uint32_t comp_constant_offsets[kRootParameterSlotCount] = {};
  uint32_t comp_constant_sizes[kRootParameterSlotCount] = {};
  bool comp_constant_set[kRootParameterSlotCount] = {};
  bool comp_cbv_set[kRootParameterSlotCount] = {};
  bool comp_srv_set[kRootParameterSlotCount] = {};
  bool comp_uav_set[kRootParameterSlotCount] = {};
  bool comp_table_set[kRootParameterSlotCount] = {};

  static constexpr uint32_t kArgBufSlot = 30;
  static constexpr uint32_t kArgBufMaxQwords = 128;
  static constexpr uint32_t kConstantBufferTableSlot = 29;
  static constexpr uint32_t kConstantBufferMaxQwords = 32;
  uint64_t arg_buf_data[kArgBufMaxQwords] = {};
  uint64_t cbv_table_data[kConstantBufferMaxQwords] = {};
  uint64_t vs_arg_buf_data[kArgBufMaxQwords] = {};
  uint64_t vs_cbv_table_data[kConstantBufferMaxQwords] = {};
  uint64_t gs_arg_buf_data[kArgBufMaxQwords] = {};
  uint64_t gs_cbv_table_data[kConstantBufferMaxQwords] = {};
  uint64_t comp_arg_buf_data[kArgBufMaxQwords] = {};
  uint64_t comp_cbv_table_data[kConstantBufferMaxQwords] = {};
  WMT::Reference<WMT::Buffer> arg_buf;
  uint64_t arg_buf_offset = 0;
  WMT::Reference<WMT::Buffer> cbv_table_buf;
  uint64_t cbv_table_buf_offset = 0;
  WMT::Reference<WMT::Buffer> vs_arg_buf;
  uint64_t vs_arg_buf_offset = 0;
  WMT::Reference<WMT::Buffer> vs_cbv_table_buf;
  uint64_t vs_cbv_table_buf_offset = 0;
  WMT::Reference<WMT::Buffer> gs_arg_buf;
  uint64_t gs_arg_buf_offset = 0;
  WMT::Reference<WMT::Buffer> gs_cbv_table_buf;
  uint64_t gs_cbv_table_buf_offset = 0;
  WMT::Reference<WMT::Buffer> comp_arg_buf;
  uint64_t comp_arg_buf_offset = 0;
  WMT::Reference<WMT::Buffer> comp_cbv_table_buf;
  uint64_t comp_cbv_table_buf_offset = 0;
  WMT::Reference<WMT::Buffer> root_constants_mtl_buf;
  uint64_t root_constants_mtl_buf_offset = 0;
  uint64_t root_constants_gpu_address = 0;
  WMT::Reference<WMT::Buffer> geometry_draw_args_buf;
  WMT::Reference<WMT::Buffer> msc_vertex_arg_buf;
  WMT::Reference<WMT::Buffer> msc_draw_args_buf;
  WMT::Reference<WMT::Buffer> msc_uniforms_buf;
  WMT::Reference<WMT::Buffer> null_vertex_arg_buf;
  WMT::Reference<WMT::Buffer> atomic64_lock_buf;
  WMT::Reference<WMT::Texture> null_direct_texture;
  WMT::Reference<WMT::SamplerState> null_direct_sampler;
  VertexBufferEntry vertex_table_data[kVertexBufferSlotCount] = {};
  WMT::Reference<WMT::Buffer> vertex_table_buf;
  WMT::Reference<WMT::Buffer> transient_table_slab;
  uint64_t transient_table_slab_offset = 0;
  uint64_t transient_table_slab_gpu_address = 0;
  std::vector<WMT::Reference<WMT::Buffer>> transient_buffers;
  std::vector<obj_handle_t> retained_completion_objects;
  uint32_t retained_completion_duplicate_count = 0;
  uint32_t retained_completion_overflow_count = 0;

  void RetainMTLObjectForCompletion(obj_handle_t handle) {
    if (!handle)
      return;
    if (std::find(retained_completion_objects.begin(),
                  retained_completion_objects.end(),
                  handle) != retained_completion_objects.end()) {
      retained_completion_duplicate_count++;
      return;
    }
    retained_completion_objects.push_back(handle);
  }

  void RetainMTLObjectForCompletion(WMT::Object object) {
    RetainMTLObjectForCompletion(object.handle);
  }

  void RetainResourceMetalObjectsForCompletion(MTLD3D12Resource *resource) {
    if (!resource)
      return;
    auto buffer = resource->GetMTLBuffer();
    if (buffer.handle)
      RetainMTLObjectForCompletion(buffer);
    auto texture = resource->GetMTLTexture();
    if (texture.handle)
      RetainMTLObjectForCompletion(texture);
  }

  void RetainSamplerPairForCompletion(WMT::SamplerState sampler,
                                      WMT::SamplerState sampler_cube) {
    if (sampler.handle)
      RetainMTLObjectForCompletion(sampler);
    if (sampler_cube.handle && sampler_cube.handle != sampler.handle)
      RetainMTLObjectForCompletion(sampler_cube);
  }

  void ArmCommandBufferResourceRetention(uint64_t command_list_id,
                                         uint32_t queue_type,
                                         uint32_t command_count,
                                         uint32_t draw_count,
                                         uint32_t dispatch_count) {
    if (!cmdbuf.handle || retained_completion_objects.empty())
      return;
    cmdbuf.retainObjectsUntilCompleted(retained_completion_objects.data(),
                                       retained_completion_objects.size());
    Logger::info(str::format(
        "M12 command buffer retained resources "
        "schema=metalsharp.m12.command-buffer-retention.v1 cmdlist_id=",
        (unsigned long long)command_list_id, " queue=", queue_type,
        " cmdbuf=", (unsigned long long)cmdbuf.handle,
        " retained=", retained_completion_objects.size(),
        " duplicates=", retained_completion_duplicate_count, " overflow=",
        retained_completion_overflow_count, " command_count=", command_count,
        " draw_count=", draw_count, " dispatch_count=", dispatch_count));
  }

  WMT::Reference<WMT::Buffer>
  MakeTransientBuffer(MTLD3D12Device *device, uint64_t length,
                      uint64_t *out_gpu_address = nullptr) {
    WMTBufferInfo buf_info = {};
    buf_info.length = length;
    buf_info.options =
        WMTResourceStorageModeShared | WMTResourceHazardTrackingModeTracked;
    auto buffer = device->GetDXMTDevice().device().newBuffer(buf_info);
    if (buffer.handle) {
      transient_buffers.push_back(buffer);
      RetainMTLObjectForCompletion(buffer);
      if (out_gpu_address)
        *out_gpu_address = buf_info.gpu_address;
    }
    return buffer;
  }

  static uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  WMT::Reference<WMT::Buffer>
  MakeTransientTableSlice(MTLD3D12Device *device, const void *data,
                          uint64_t length, uint64_t *out_offset,
                          uint64_t *out_gpu_address = nullptr) {
    constexpr uint64_t kTableSliceAlignment = 256;
    constexpr uint64_t kTableSlabSize = 64 * 1024;
    uint64_t aligned_offset =
        AlignUp64(transient_table_slab_offset, kTableSliceAlignment);
    uint64_t aligned_length = AlignUp64(length, kTableSliceAlignment);

    if (!transient_table_slab.handle ||
        aligned_offset + aligned_length > kTableSlabSize) {
      WMTBufferInfo buf_info = {};
      buf_info.length = std::max(kTableSlabSize, aligned_length);
      buf_info.options =
          WMTResourceStorageModeShared | WMTResourceHazardTrackingModeTracked;
      transient_table_slab =
          device->GetDXMTDevice().device().newBuffer(buf_info);
      transient_table_slab_offset = 0;
      transient_table_slab_gpu_address =
          transient_table_slab.handle ? buf_info.gpu_address : 0;
      aligned_offset = 0;
      if (transient_table_slab.handle) {
        transient_buffers.push_back(transient_table_slab);
        RetainMTLObjectForCompletion(transient_table_slab);
      }
    }

    if (!transient_table_slab.handle)
      return {};

    transient_table_slab.updateContents(aligned_offset, data, length);
    transient_table_slab_offset = aligned_offset + aligned_length;
    if (out_offset)
      *out_offset = aligned_offset;
    if (out_gpu_address)
      *out_gpu_address = transient_table_slab_gpu_address
                             ? transient_table_slab_gpu_address + aligned_offset
                             : 0;
    return transient_table_slab;
  }

  uint64_t StageConstantBufferAddress(MTLD3D12Device *device,
                                      uint64_t gpu_address,
                                      WMTRenderStages stages,
                                      const char *label) {
    if (!gpu_address)
      return 0;
    if (DXMTD3D12DisableCBVStaging())
      return gpu_address;

    auto *res = device->LookupResourceByGPUAddress(gpu_address);
    if (!res || !res->GetMTLBuffer().handle)
      return gpu_address;

    uint64_t offset = gpu_address - res->GetGPUVirtualAddress();
    uint64_t length = res->GetBufferByteLength();
    if (offset >= length)
      return gpu_address;
    length -= offset;
    length = std::min<uint64_t>(length, 64 * 1024);

    void *mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped)) || !mapped)
      return gpu_address;

    const float *mapped_floats = reinterpret_cast<const float *>(
        static_cast<const char *>(mapped) + offset);
    float cbv_probe[20] = {};
    if (length >= sizeof(cbv_probe))
      std::memcpy(cbv_probe, mapped_floats, sizeof(cbv_probe));
    uint64_t staged_gpu_address = 0;
    auto staged = MakeTransientBuffer(device, std::max<uint64_t>(length, 256),
                                      &staged_gpu_address);
    if (staged.handle)
      staged.updateContents(0, static_cast<const char *>(mapped) + offset,
                            length);
    res->Unmap(0, nullptr);
    if (!staged.handle || !staged_gpu_address)
      return gpu_address;

    if (render_enc_open) {
      render_enc.useResource(staged, WMTResourceUsageRead, stages);
    }
    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_vs_cbv_logs, 32)) {
      Logger::info(str::format(
          "M12 swapchain staged CBV ", label, " original=0x", std::hex,
          (unsigned long long)gpu_address, " staged=0x",
          (unsigned long long)staged_gpu_address, std::dec,
          " bytes=", (unsigned long long)length, " f0=", cbv_probe[0],
          " f5=", cbv_probe[5], " f15=", cbv_probe[15], " time=", cbv_probe[16],
          " frame=", cbv_probe[17], " width=", cbv_probe[18],
          " height=", cbv_probe[19]));
    }
    QTRACE("%s: staged CBV original=0x%llx staged=0x%llx bytes=%llu", label,
           (unsigned long long)gpu_address,
           (unsigned long long)staged_gpu_address, (unsigned long long)length);
    return staged_gpu_address;
  }

  uint64_t StageRootConstantsAsConstantBuffer(MTLD3D12Device *device,
                                              uint32_t root_idx,
                                              WMTRenderStages stages,
                                              const char *label) {
    if (root_idx >= kRootParameterSlotCount || !root_constant_set[root_idx])
      return 0;

    const uint32_t root_offset = root_constant_offsets[root_idx];
    const uint32_t root_size = root_constant_sizes[root_idx];
    if (root_offset >= sizeof(root_constants_buf) || root_size == 0)
      return 0;

    const uint64_t byte_count = std::min<uint64_t>(
        kRootConstantBytes, sizeof(root_constants_buf) - root_offset);
    uint64_t staged_gpu_address = 0;
    auto staged = MakeTransientBuffer(
        device, std::max<uint64_t>(byte_count, 256), &staged_gpu_address);
    if (staged.handle)
      staged.updateContents(0, root_constants_buf + root_offset, byte_count);
    if (!staged.handle || !staged_gpu_address)
      return root_constants_gpu_address
                 ? root_constants_gpu_address + root_offset
                 : 0;

    if (render_enc_open)
      render_enc.useResource(staged, WMTResourceUsageRead, stages);
    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_vs_cbv_logs, 32)) {
      const float *floats =
          reinterpret_cast<const float *>(root_constants_buf + root_offset);
      Logger::info(str::format("M12 swapchain root constants staged CBV ",
                               label, " root_idx=", root_idx, " gpu=0x",
                               std::hex, (unsigned long long)staged_gpu_address,
                               std::dec, " size=", root_size, " f0=", floats[0],
                               " f1=", floats[1], " f2=", floats[2],
                               " f3=", floats[3]));
    }
    return staged_gpu_address;
  }

  bool CopyConstantBufferBytes(MTLD3D12Device *device, uint64_t gpu_address,
                               uint64_t byte_count, uint8_t *dst,
                               uint64_t dst_offset) {
    if (!gpu_address || !byte_count || !dst)
      return false;

    auto *res = device->LookupResourceByGPUAddress(gpu_address);
    if (!res)
      return false;

    uint64_t src_offset = gpu_address - res->GetGPUVirtualAddress();
    uint64_t length = res->GetBufferByteLength();
    if (src_offset >= length)
      return false;
    byte_count = std::min<uint64_t>(byte_count, length - src_offset);

    void *mapped = nullptr;
    if (FAILED(res->Map(0, nullptr, &mapped)) || !mapped)
      return false;
    std::memcpy(dst + dst_offset,
                static_cast<const char *>(mapped) + src_offset, byte_count);
    res->Unmap(0, nullptr);
    return true;
  }

  uint32_t BindIndexOrFallback(uint32_t reflected, uint32_t fallback) const {
    return reflected != ~0u ? reflected : fallback;
  }

  void MarkVertexBufferBound(uint32_t slot) {
    if (slot < 64)
      bound_vertex_buffer_slots |= 1ull << slot;
  }

  bool SetVertexBufferTracked(WMT::Buffer buffer, uint64_t offset,
                              uint32_t slot) {
    if (slot > 0xffu)
      return false;
    bool ok = render_enc.setVertexBuffer(buffer, offset, (uint8_t)slot);
    if (ok) {
      MarkVertexBufferBound(slot);
      RetainMTLObjectForCompletion(buffer);
    }
    return ok;
  }

  void ResetTrackedRenderBindings() {
    bound_vertex_buffer_slots = 0;
    bound_fragment_buffer_slots = 0;
    bound_fragment_texture_slots = 0;
    bound_fragment_sampler_slots = 0;
    fallback_fragment_buffer_slots = 0;
    fallback_fragment_texture_slots = 0;
    fallback_fragment_sampler_slots = 0;
  }

  void MarkFragmentBufferBound(uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_fragment_buffer_slots |= 1ull << slot;
    if (fallback)
      fallback_fragment_buffer_slots |= 1ull << slot;
  }

  void MarkFragmentTextureBound(uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_fragment_texture_slots |= 1ull << slot;
    if (fallback)
      fallback_fragment_texture_slots |= 1ull << slot;
  }

  void MarkFragmentSamplerBound(uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_fragment_sampler_slots |= 1ull << slot;
    if (fallback)
      fallback_fragment_sampler_slots |= 1ull << slot;
  }

  bool SetFragmentBufferTracked(WMT::Buffer buffer, uint64_t offset,
                                uint32_t slot, bool fallback = false) {
    if (slot > 0xffu)
      return false;
    bool ok = render_enc.setFragmentBuffer(buffer, offset, (uint8_t)slot);
    if (ok) {
      MarkFragmentBufferBound(slot, fallback);
      RetainMTLObjectForCompletion(buffer);
    }
    return ok;
  }

  bool SetFragmentTextureTracked(WMT::Texture texture, uint32_t slot,
                                 bool fallback = false) {
    if (slot > 0xffu)
      return false;
    bool ok = render_enc.setFragmentTexture(texture, (uint8_t)slot);
    if (ok) {
      MarkFragmentTextureBound(slot, fallback);
      RetainMTLObjectForCompletion(texture);
    }
    return ok;
  }

  bool SetFragmentSamplerTracked(WMT::SamplerState sampler, uint32_t slot,
                                 bool fallback = false) {
    if (slot > 0xffu)
      return false;
    if (!sampler.handle)
      return false;

    bool ok = render_enc.setFragmentSamplerState(sampler, slot);
    if (!ok)
      return false;
    MarkFragmentSamplerBound(slot, fallback);
    RetainMTLObjectForCompletion(sampler);
    return true;
  }

  bool EnsureNullDirectTexture(MTLD3D12Device *device) {
    if (null_direct_texture.handle)
      return true;

    WMTTextureInfo info = {};
    info.pixel_format = WMTPixelFormatRGBA8Unorm;
    info.width = 1;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.type = WMTTextureType2D;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.usage = WMTTextureUsageShaderRead;
    null_direct_texture = device->GetMTLDevice().newTexture(info);
    return null_direct_texture.handle != 0;
  }

  bool EnsureAtomic64LockBuffer(MTLD3D12Device *device) {
    if (atomic64_lock_buf.handle)
      return true;
    uint32_t zero = 0;
    atomic64_lock_buf = MakeTransientBuffer(device, sizeof(zero));
    if (!atomic64_lock_buf.handle)
      return false;
    atomic64_lock_buf.updateContents(0, &zero, sizeof(zero));
    return true;
  }

  bool EnsureNullDirectSampler(MTLD3D12Device *device) {
    if (null_direct_sampler.handle)
      return true;

    WMTSamplerInfo info = {};
    info.min_filter = WMTSamplerMinMagFilterNearest;
    info.mag_filter = WMTSamplerMinMagFilterNearest;
    info.mip_filter = WMTSamplerMipFilterNearest;
    info.r_address_mode = WMTSamplerAddressModeClampToEdge;
    info.s_address_mode = WMTSamplerAddressModeClampToEdge;
    info.t_address_mode = WMTSamplerAddressModeClampToEdge;
    info.lod_min_clamp = 0.0f;
    info.lod_max_clamp = 1000.0f;
    info.normalized_coords = true;
    info.support_argument_buffers = true;
    null_direct_sampler = device->GetMTLDevice().newSamplerState(info);
    return null_direct_sampler.handle != 0;
  }

  void BindMissingNonStageInVertexBuffers(MTLD3D12Device *device) {
    if (!render_enc_open || !pso || pso->UsesStageInVertexDescriptor())
      return;

    if (!null_vertex_arg_buf.handle) {
      uint64_t zero_data[4] = {};
      null_vertex_arg_buf = MakeTransientBuffer(device, sizeof(zero_data));
      if (null_vertex_arg_buf.handle) {
        null_vertex_arg_buf.updateContents(0, zero_data, sizeof(zero_data));
      }
    }

    if (!null_vertex_arg_buf.handle)
      return;

    uint32_t filled = 0;
    for (uint32_t slot = 0; slot < kM12VertexBufferSignatureSlotCount; slot++) {
      if (slot == kVertexBufferTableSlot || slot == kConstantBufferTableSlot ||
          slot == kArgBufSlot)
        continue;
      if (bound_vertex_buffer_slots & (1ull << slot))
        continue;
      if (SetVertexBufferTracked(null_vertex_arg_buf, 0, slot))
        filled++;
    }

    if (filled) {
      render_enc.useResource(null_vertex_arg_buf, WMTResourceUsageRead,
                             WMTRenderStageVertex);
      if (HasSwapchainRenderTarget() &&
          TakeLogBudget(&g_swapchain_draw_logs, 384)) {
        Logger::info(str::format(
            "M12 non-stage-in filled missing vertex buffers count=", filled,
            " mask=0x", std::hex, bound_vertex_buffer_slots, std::dec,
            " pso=", (void *)pso, " ", TracePsoShaderSummary(pso)));
      }
    }

    if (vs_cbv_table_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetVSReflection().ConstanttBufferTableBindIndex,
          kConstantBufferTableSlot);
      SetVertexBufferTracked(vs_cbv_table_buf, vs_cbv_table_buf_offset,
                             bind_index);
      render_enc.useResource(vs_cbv_table_buf, WMTResourceUsageRead,
                             WMTRenderStageVertex);
    }
    if (vs_arg_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetVSReflection().ArgumentBufferBindIndex, kArgBufSlot);
      SetVertexBufferTracked(vs_arg_buf, vs_arg_buf_offset, bind_index);
      render_enc.useResource(vs_arg_buf, WMTResourceUsageRead,
                             WMTRenderStageVertex);
    }
  }

  D3D12ShaderBindingCompletenessSummary FragmentCompletenessSummary() const {
    D3D12ShaderBindingCompletenessDesc desc = {};
    desc.buffer_count = kD3D12M12DirectBufferSlots;
    desc.texture_count = kD3D12M12DirectFragmentTextureSlots;
    desc.sampler_count = kD3D12M12DirectFragmentSamplerSlots;
    desc.bound_buffers = bound_fragment_buffer_slots;
    desc.bound_textures = bound_fragment_texture_slots;
    desc.bound_samplers = bound_fragment_sampler_slots;
    desc.fallback_buffers = fallback_fragment_buffer_slots;
    desc.fallback_textures = fallback_fragment_texture_slots;
    desc.fallback_samplers = fallback_fragment_sampler_slots;
    return D3D12EvaluateShaderBindingCompleteness(desc);
  }

  void BindDirectFragmentCompleteness(MTLD3D12Device *device,
                                      const char *draw_label) {
    if (!render_enc_open || !pso || !HasUsableRenderPSO())
      return;

    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_fragment_prefill_logs, 96)) {
      uint64_t root_table_mask = 0;
      uint64_t root_cbv_mask = 0;
      uint64_t root_srv_mask = 0;
      uint64_t root_uav_mask = 0;
      for (uint32_t i = 0; i < kRootParameterSlotCount; i++) {
        if (root_table_set[i])
          root_table_mask |= 1ull << i;
        if (root_cbv_set[i])
          root_cbv_mask |= 1ull << i;
        if (root_srv_set[i])
          root_srv_mask |= 1ull << i;
        if (root_uav_set[i])
          root_uav_mask |= 1ull << i;
      }
      Logger::info(str::format(
          "M12 fragment prefill label=", draw_label ? draw_label : "draw",
          " bound_buf=0x", std::hex, bound_fragment_buffer_slots,
          " bound_tex=0x", bound_fragment_texture_slots, " bound_samp=0x",
          bound_fragment_sampler_slots, " root_tables=0x", root_table_mask,
          " root_cbv=0x", root_cbv_mask, " root_srv=0x", root_srv_mask,
          " root_uav=0x", root_uav_mask, std::dec, " heaps=", desc_heap_count,
          " pso=", (void *)pso, " ", TracePsoShaderSummary(pso)));
    }

    if (!null_vertex_arg_buf.handle) {
      uint64_t zero_data[4] = {};
      null_vertex_arg_buf = MakeTransientBuffer(device, sizeof(zero_data));
      if (null_vertex_arg_buf.handle)
        null_vertex_arg_buf.updateContents(0, zero_data, sizeof(zero_data));
    }

    if (null_vertex_arg_buf.handle) {
      uint64_t missing = D3D12DirectBindingMask(kD3D12M12DirectBufferSlots) &
                         ~bound_fragment_buffer_slots;
      for (uint32_t slot = 0; slot < kD3D12M12DirectBufferSlots; slot++) {
        if (!(missing & (1ull << slot)))
          continue;
        SetFragmentBufferTracked(null_vertex_arg_buf, 0, slot, true);
      }
      if (fallback_fragment_buffer_slots)
        render_enc.useResource(null_vertex_arg_buf, WMTResourceUsageRead,
                               WMTRenderStageFragment);
    }

    if (EnsureNullDirectTexture(device)) {
      uint64_t missing =
          D3D12DirectBindingMask(kD3D12M12DirectFragmentTextureSlots) &
          ~bound_fragment_texture_slots;
      for (uint32_t slot = 0; slot < kD3D12M12DirectFragmentTextureSlots;
           slot++) {
        if (!(missing & (1ull << slot)))
          continue;
        SetFragmentTextureTracked(null_direct_texture, slot, true);
      }
      if (fallback_fragment_texture_slots) {
        render_enc.useResource(
            null_direct_texture,
            (WMTResourceUsage)(WMTResourceUsageRead | WMTResourceUsageSample),
            WMTRenderStageFragment);
      }
    }

    if (EnsureNullDirectSampler(device)) {
      uint64_t missing =
          D3D12DirectBindingMask(kD3D12M12DirectFragmentSamplerSlots) &
          ~bound_fragment_sampler_slots;
      for (uint32_t slot = 0; slot < kD3D12M12DirectFragmentSamplerSlots;
           slot++) {
        if (!(missing & (1ull << slot)))
          continue;
        SetFragmentSamplerTracked(null_direct_sampler, slot, true);
      }
    }

    if (pso->IsDepthBoundsTestEnabled() &&
        depth_bounds_dsv_texture.handle) {
      const float depth_bounds[4] = {
          depth_bounds_min, depth_bounds_max,
          static_cast<float>(depth_bounds_dsv_slice),
          depth_bounds_inverted ? 1.0f : 0.0f};
      render_enc.setFragmentBytes(depth_bounds, sizeof(depth_bounds), 28);
      render_enc.setFragmentTexture(depth_bounds_dsv_texture, 126);
      RetainMTLObjectForCompletion(depth_bounds_dsv_texture);
      QTRACE("DepthBounds bind min=%.3f max=%.3f slice=%u inverted=%u "
             "texture=%p",
             depth_bounds_min, depth_bounds_max, depth_bounds_dsv_slice,
             depth_bounds_inverted ? 1u : 0u,
             (void *)depth_bounds_dsv_texture.handle);
    }

    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_fragment_completeness_logs, 128)) {
      auto summary = FragmentCompletenessSummary();
      Logger::info(str::format(
          "M12 fragment completeness label=", draw_label ? draw_label : "draw",
          " pso=", (void *)pso, " buffers ", summary.bound_buffer_count, "+",
          summary.fallback_buffer_count, "/", summary.required_buffer_count,
          " missing=0x", std::hex, summary.missing_buffers, " textures ",
          std::dec, summary.bound_texture_count, "+",
          summary.fallback_texture_count, "/", summary.required_texture_count,
          " missing=0x", std::hex, summary.missing_textures, " samplers ",
          std::dec, summary.bound_sampler_count, "+",
          summary.fallback_sampler_count, "/", summary.required_sampler_count,
          " missing=0x", std::hex, summary.missing_samplers, std::dec, " ",
          TracePsoShaderSummary(pso)));
    }
  }

  bool BindRootBufferArgument(MTLD3D12Device *device, uint64_t *data,
                              const MTL_SM50_SHADER_ARGUMENT &arg,
                              D3D12_GPU_VIRTUAL_ADDRESS address,
                              WMTResourceUsage usage,
                              WMTRenderStages render_stages,
                              const char *label) {
    if (!address)
      return false;

    auto *res = device->LookupResourceByGPUAddress(address);
    if (!res || !res->GetMTLBuffer().handle)
      return false;
    if (!MSCArgumentAcceptsBuffer(arg, res))
      return false;

    uint64_t offset = address - res->GetGPUVirtualAddress();
    uint64_t length = res->GetBufferByteLength();
    if (offset < length)
      length -= offset;
    else
      length = 0;

    data[arg.StructurePtrOffset] = address;
    data[arg.StructurePtrOffset + 1] = length;
    if (render_enc_open) {
      render_enc.useResource(res->GetMTLBuffer(), usage, render_stages);
    }
    RetainResourceMetalObjectsForCompletion(res);
    QTRACE("%s: RootBuffer slot=%u space=%u addr=0x%llx len=%llu offset=%u",
           label, arg.SM50BindingSlot, arg.SM50RegisterSpace,
           (unsigned long long)address, (unsigned long long)length,
           arg.StructurePtrOffset);
    return true;
  }

  bool WriteConstantBufferArgument(MTLD3D12Device *device, uint64_t *data,
                                   const MTL_SM50_SHADER_ARGUMENT &arg,
                                   D3D12_GPU_VIRTUAL_ADDRESS address,
                                   uint64_t size, WMTRenderStages stages,
                                   const char *label) {
    if (!address)
      return false;

    auto *res = device->LookupResourceByGPUAddress(address);
    if (res && res->GetMTLBuffer().handle) {
      uint64_t offset = address - res->GetGPUVirtualAddress();
      uint64_t length = res->GetBufferByteLength();
      if (offset < length)
        length -= offset;
      else
        length = 0;
      if (size == 0 || size > length)
        size = length;
      if (render_enc_open)
        render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                               stages);
      RetainResourceMetalObjectsForCompletion(res);
    }

    data[arg.StructurePtrOffset] = address;
    data[arg.StructurePtrOffset + 1] = size;
    data[arg.StructurePtrOffset + 2] = 0;
    QTRACE("%s: CBV slot=%u space=%u addr=0x%llx len=%llu offset=%u", label,
           arg.SM50BindingSlot, arg.SM50RegisterSpace,
           (unsigned long long)address, (unsigned long long)size,
           arg.StructurePtrOffset);
    return true;
  }

  void BuildArgumentBuffer(MTLD3D12Device *device) {
    if (!pso || pso->GetPSArguments().empty()) {
      QTRACE("BuildArgumentBuffer: no PSO or no args");
      return;
    }
    auto &args = pso->GetPSArguments();
    const bool msc_linear_abi = pso->PSUsesMSCArgumentABI();
    const uint32_t buffer_metadata_qword = msc_linear_abi ? 2u : 1u;
    uint32_t qword_count = pso->GetPSReflection().ArgumentTableQwords;
    QTRACE("BuildArgumentBuffer: %u args, %u qwords, NumArguments=%u",
           (unsigned)args.size(), qword_count,
           (unsigned)pso->GetPSReflection().NumArguments);
    if (qword_count == 0 || qword_count > kArgBufMaxQwords) {
      QTRACE("BuildArgumentBuffer: invalid qword_count=%u", qword_count);
      return;
    }
    memset(arg_buf_data, 0, qword_count * 8);

    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;

    for (auto &arg : args) {
      uint32_t root_idx = ~0u;
      uint32_t descriptor_offset = 0;
      if (dxmt_sig) {
        D3D12_DESCRIPTOR_RANGE_TYPE range_type =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bool table_arg = true;
        if (arg.Type == SM50BindingType::SRV) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        } else if (arg.Type == SM50BindingType::Sampler) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        } else if (arg.Type == SM50BindingType::UAV) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        } else if (arg.Type == SM50BindingType::ConstantBuffer) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        } else {
          table_arg = false;
        }

        if (table_arg) {
          dxmt_sig->FindDescriptorTableRangeForVisibility(
              range_type, arg.SM50BindingSlot, arg.SM50RegisterSpace,
              D3D12_SHADER_VISIBILITY_PIXEL, &root_idx, &descriptor_offset);
        }
      }
      if (root_idx == ~0u || !root_table_set[root_idx] ||
          desc_heap_count == 0) {
        uint32_t root_desc_idx = ~0u;
        if (arg.Type == SM50BindingType::SRV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                        arg, D3D12_SHADER_VISIBILITY_PIXEL,
                                        &root_desc_idx) &&
            root_srv_set[root_desc_idx] &&
            BindRootBufferArgument(device, arg_buf_data, arg,
                                   root_srvs[root_desc_idx],
                                   WMTResourceUsageRead,
                                   (WMTRenderStages)(WMTRenderStageVertex |
                                                     WMTRenderStageFragment),
                                   "BuildArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::UAV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                        arg, D3D12_SHADER_VISIBILITY_PIXEL,
                                        &root_desc_idx) &&
            root_uav_set[root_desc_idx] &&
            BindRootBufferArgument(device, arg_buf_data, arg,
                                   root_uavs[root_desc_idx],
                                   (WMTResourceUsage)(WMTResourceUsageRead |
                                                      WMTResourceUsageWrite),
                                   (WMTRenderStages)(WMTRenderStageVertex |
                                                     WMTRenderStageFragment),
                                   "BuildArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::ConstantBuffer &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                        arg, D3D12_SHADER_VISIBILITY_PIXEL,
                                        &root_desc_idx) &&
            root_cbv_set[root_desc_idx] &&
            WriteConstantBufferArgument(
                device, arg_buf_data, arg, root_cbvs[root_desc_idx], 0,
                (WMTRenderStages)(WMTRenderStageVertex |
                                  WMTRenderStageFragment),
                "BuildArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::Sampler && dxmt_sig) {
          if (auto *sampler = dxmt_sig->FindStaticSampler(
                  arg.SM50BindingSlot, arg.SM50RegisterSpace,
                  D3D12_SHADER_VISIBILITY_PIXEL)) {
            if (msc_linear_abi) {
              WriteMSCLinearSamplerArgument(arg_buf_data, arg,
                                            sampler->sampler_gpu_id,
                                            sampler->lod_bias_bits);
            } else {
              arg_buf_data[arg.StructurePtrOffset] = sampler->sampler_gpu_id;
              arg_buf_data[arg.StructurePtrOffset + 1] =
                  sampler->sampler_cube_gpu_id ? sampler->sampler_cube_gpu_id
                                               : sampler->sampler_gpu_id;
              arg_buf_data[arg.StructurePtrOffset + 2] =
                  sampler->lod_bias_bits;
            }
            RetainSamplerPairForCompletion(sampler->sampler,
                                           sampler->sampler_cube);
            QTRACE("BuildArgBuf: StaticSampler slot=%u space=%u gpu_id=0x%llx "
                   "offset=%u",
                   arg.SM50BindingSlot, arg.SM50RegisterSpace,
                   (unsigned long long)sampler->sampler_gpu_id,
                   arg.StructurePtrOffset);
            continue;
          }
        }
        QTRACE("BuildArgBuf: arg type=%d slot=%u root_idx=%u desc_off=%u "
               "table_set=%d heaps=%u skip",
               (int)arg.Type, arg.SM50BindingSlot, root_idx, descriptor_offset,
               root_idx != ~0u ? root_table_set[root_idx] : 0, desc_heap_count);
        continue;
      }

      for (uint32_t h = 0; h < desc_heap_count; h++) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
        if (!heap)
          continue;
        auto *desc = heap->GetDescriptorFromGPUHandle(root_tables[root_idx],
                                                      descriptor_offset);
        if (!desc)
          continue;

        if (arg.Type == SM50BindingType::SRV) {
          QTRACE("BuildArgBuf: SRV root=%u desc_off=%u desc=%p res=%p "
                 "flags=0x%x offset=%u",
                 root_idx, descriptor_offset, (void *)desc,
                 desc->resource ? (void *)desc->resource : nullptr, arg.Flags,
                 arg.StructurePtrOffset);
          if (desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            if (MSCArgumentAcceptsBuffer(arg, res) &&
                res->GetMTLBuffer().handle) {
              arg_buf_data[arg.StructurePtrOffset] =
                  res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
              arg_buf_data[arg.StructurePtrOffset +
                           buffer_metadata_qword] =
                  SRVBufferByteLength(desc, res);
              if (render_enc_open) {
                render_enc.useResource(
                    res->GetMTLBuffer(), WMTResourceUsageRead,
                    (WMTRenderStages)(WMTRenderStageVertex |
                                      WMTRenderStageFragment));
              }
              RetainResourceMetalObjectsForCompletion(res);
            } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
              uint64_t gpu_id = DescriptorTextureGPUResourceID(desc, res);
              QTRACE("BuildArgBuf: SRV tex_handle=%llu gpu_id=0x%llx view=%d",
                     (unsigned long long)tex.handle, (unsigned long long)gpu_id,
                     desc->metal_texture_view.handle ? 1 : 0);
              if (HasSwapchainRenderTarget() &&
                  TakeLogBudget(&g_swapchain_texture_binding_logs, 96)) {
                Logger::info(str::format(
                    "M12 swapchain PS SRV binding slot=", arg.SM50BindingSlot,
                    " space=", arg.SM50RegisterSpace, " root=", root_idx,
                    " desc_off=", descriptor_offset,
                    " qword_off=", arg.StructurePtrOffset, " gpu_id=0x",
                    std::hex, (unsigned long long)gpu_id, std::dec, " ",
                    DescriptorSummary(desc, D3D12_DESCRIPTOR_RANGE_TYPE_SRV),
                    " pso=", (void *)pso, " ", TracePsoShaderSummary(pso)));
              }
              if (msc_linear_abi)
                WriteMSCLinearTextureArgument(arg_buf_data, arg, gpu_id);
              else
                WriteMSCTextureArgument(arg_buf_data, arg, gpu_id,
                                        SRVTextureArrayLength(desc, res));
              if (render_enc_open) {
                render_enc.useResource(
                    tex,
                    (WMTResourceUsage)(WMTResourceUsageSample |
                                       WMTResourceUsageRead),
                    (WMTRenderStages)(WMTRenderStageVertex |
                                      WMTRenderStageFragment));
                QTRACE("BuildArgBuf: useResource texture handle=%llu",
                       (unsigned long long)tex.handle);
              }
              RetainMTLObjectForCompletion(tex);
            } else if (res->GetMTLBuffer().handle) {
              arg_buf_data[arg.StructurePtrOffset] =
                  res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
              arg_buf_data[arg.StructurePtrOffset +
                           buffer_metadata_qword] =
                  SRVBufferByteLength(desc, res);
              if (render_enc_open) {
                render_enc.useResource(
                    res->GetMTLBuffer(), WMTResourceUsageRead,
                    (WMTRenderStages)(WMTRenderStageVertex |
                                      WMTRenderStageFragment));
              }
              RetainResourceMetalObjectsForCompletion(res);
            }
          }
        } else if (arg.Type == SM50BindingType::Sampler) {
          QTRACE("BuildArgBuf: Sampler root=%u desc_off=%u desc_type=%u "
                 "gpu_id=0x%llx offset=%u",
                 root_idx, descriptor_offset, desc->type,
                 (unsigned long long)desc->metal_sampler_gpu_id,
                 arg.StructurePtrOffset);
          if (desc->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
              desc->metal_sampler_gpu_id) {
            if (msc_linear_abi) {
              WriteMSCLinearSamplerArgument(
                  arg_buf_data, arg, desc->metal_sampler_gpu_id,
                  SamplerLodBiasBits(desc));
            } else {
              arg_buf_data[arg.StructurePtrOffset] =
                  desc->metal_sampler_gpu_id;
              arg_buf_data[arg.StructurePtrOffset + 1] =
                  SamplerCubeGPUResourceID(desc);
              arg_buf_data[arg.StructurePtrOffset + 2] =
                  SamplerLodBiasBits(desc);
            }
            RetainSamplerPairForCompletion(desc->metal_sampler,
                                           desc->metal_sampler_cube);
          }
        } else if (arg.Type == SM50BindingType::UAV) {
          QTRACE("BuildArgBuf: UAV root=%u desc_off=%u desc=%p res=%p "
                 "flags=0x%x offset=%u",
                 root_idx, descriptor_offset, (void *)desc,
                 desc->resource ? (void *)desc->resource : nullptr, arg.Flags,
                 arg.StructurePtrOffset);
          if (desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            if (MSCArgumentAcceptsBuffer(arg, res) &&
                res->GetMTLBuffer().handle) {
              arg_buf_data[arg.StructurePtrOffset] =
                  res->GetGPUVirtualAddress() + UAVBufferByteOffset(desc);
              arg_buf_data[arg.StructurePtrOffset +
                           buffer_metadata_qword] =
                  UAVBufferByteLength(desc, res);
              if (render_enc_open) {
                render_enc.useResource(
                    res->GetMTLBuffer(),
                    (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageWrite),
                    (WMTRenderStages)(WMTRenderStageVertex |
                                      WMTRenderStageFragment));
              }
              RetainResourceMetalObjectsForCompletion(res);
            } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
              uint64_t gpu_id = DescriptorTextureGPUResourceID(desc, res);
              if (msc_linear_abi)
                WriteMSCLinearTextureArgument(arg_buf_data, arg, gpu_id);
              else
                WriteMSCTextureArgument(arg_buf_data, arg, gpu_id,
                                        UAVTextureArrayLength(desc, res));
              if (render_enc_open) {
                render_enc.useResource(
                    tex,
                    (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageWrite),
                    (WMTRenderStages)(WMTRenderStageVertex |
                                      WMTRenderStageFragment));
              }
              RetainMTLObjectForCompletion(tex);
            }
          }
        } else if (arg.Type == SM50BindingType::ConstantBuffer) {
          QTRACE("BuildArgBuf: CBV root=%u desc_off=%u addr=0x%llx size=%u "
                 "offset=%u",
                 root_idx, descriptor_offset,
                 (unsigned long long)desc->cbv.BufferLocation,
                 desc->cbv.SizeInBytes, arg.StructurePtrOffset);
          WriteConstantBufferArgument(
              device, arg_buf_data, arg, desc->cbv.BufferLocation,
              desc->cbv.SizeInBytes,
              (WMTRenderStages)(WMTRenderStageVertex | WMTRenderStageFragment),
              "BuildArgBuf");
        }
      }
    }

    arg_buf = MakeTransientBuffer(device, kArgBufMaxQwords * 8);
    if (arg_buf.handle) {
      arg_buf.updateContents(0, arg_buf_data, qword_count * 8);
      QTRACE("BuildArgumentBuffer: wrote %u qwords to argbuf", qword_count);
      for (uint32_t i = 0; i < qword_count && i < 8; i++) {
        QTRACE("  arg_buf[%u] = 0x%llx", i,
               (unsigned long long)arg_buf_data[i]);
      }
      if (HasSwapchainRenderTarget() &&
          TakeLogBudget(&g_swapchain_argbuf_logs, 48)) {
        Logger::info(str::format(
            "M12 swapchain PS argbuf ", TracePsoShaderSummary(pso),
            " bind_index=",
            BindIndexOrFallback(pso->GetPSReflection().ArgumentBufferBindIndex,
                                kArgBufSlot),
            " qwords=", qword_count, " data=[",
            (unsigned long long)arg_buf_data[0], ",",
            (unsigned long long)(qword_count > 1 ? arg_buf_data[1] : 0), ",",
            (unsigned long long)(qword_count > 2 ? arg_buf_data[2] : 0), ",",
            (unsigned long long)(qword_count > 3 ? arg_buf_data[3] : 0), ",",
            (unsigned long long)(qword_count > 4 ? arg_buf_data[4] : 0), ",",
            (unsigned long long)(qword_count > 5 ? arg_buf_data[5] : 0), ",",
            (unsigned long long)(qword_count > 6 ? arg_buf_data[6] : 0), ",",
            (unsigned long long)(qword_count > 7 ? arg_buf_data[7] : 0), ",",
            (unsigned long long)(qword_count > 8 ? arg_buf_data[8] : 0), "]"));
      }
      if (render_enc_open) {
        render_enc.useResource(
            arg_buf, WMTResourceUsageRead,
            (WMTRenderStages)(WMTRenderStageVertex | WMTRenderStageFragment));
        QTRACE("BuildArgumentBuffer: useResource argbuf handle=%llu",
               (unsigned long long)arg_buf.handle);
      }
    }
  }

  void BuildConstantBufferTable(MTLD3D12Device *device) {
    if (!pso || pso->GetPSConstantBuffers().empty()) {
      return;
    }

    memset(cbv_table_data, 0, sizeof(cbv_table_data));

    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;
    auto &cb_args = pso->GetPSConstantBuffers();
    uint32_t qword_count = 0;
    struct ResolvedConstantBuffer {
      const MTL_SM50_SHADER_ARGUMENT *arg;
      uint64_t gpu_address;
      uint64_t original_gpu_address;
      uint32_t root_idx;
    };
    std::vector<ResolvedConstantBuffer> resolved_cbuffers;
    bool has_inline_cbuffers = false;

    for (const auto &arg : cb_args) {
      if (arg.Type != SM50BindingType::ConstantBuffer ||
          arg.StructurePtrOffset >= kConstantBufferMaxQwords)
        continue;

      qword_count = std::max(qword_count, arg.StructurePtrOffset + 1);
      has_inline_cbuffers |=
          (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) != 0;
      uint64_t gpu_address = 0;

      uint32_t root_idx = ~0u;
      bool root_idx_is_constants = false;
      if (dxmt_sig) {
        auto &params = dxmt_sig->GetParameters();
        for (uint32_t pass = 0; pass < 2 && root_idx == ~0u; pass++) {
          for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
               p++) {
            if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_CBV &&
                params[p].register_index == arg.SM50BindingSlot &&
                params[p].register_space == arg.SM50RegisterSpace &&
                ShaderVisibilityMatches(params[p].shader_visibility,
                                        D3D12_SHADER_VISIBILITY_PIXEL,
                                        pass == 0)) {
              root_idx = p;
              break;
            }
          }
        }
        for (uint32_t pass = 0; pass < 2 && root_idx == ~0u; pass++) {
          for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
               p++) {
            if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                params[p].register_index == arg.SM50BindingSlot &&
                params[p].register_space == arg.SM50RegisterSpace &&
                ShaderVisibilityMatches(params[p].shader_visibility,
                                        D3D12_SHADER_VISIBILITY_PIXEL,
                                        pass == 0)) {
              root_idx = p;
              root_idx_is_constants = true;
              break;
            }
          }
        }
      }

      if (root_idx_is_constants && root_idx < kRootParameterSlotCount &&
          root_constant_set[root_idx]) {
        gpu_address = StageRootConstantsAsConstantBuffer(
            device, root_idx, WMTRenderStageFragment,
            "PSCBVTableRootConstants");
      } else if (root_idx != ~0u && root_cbv_set[root_idx]) {
        gpu_address = root_cbvs[root_idx];
      } else if (dxmt_sig) {
        uint32_t table_root_idx = ~0u;
        uint32_t descriptor_offset = 0;
        if (dxmt_sig->FindDescriptorTableRangeForVisibility(
                D3D12_DESCRIPTOR_RANGE_TYPE_CBV, arg.SM50BindingSlot,
                arg.SM50RegisterSpace, D3D12_SHADER_VISIBILITY_PIXEL,
                &table_root_idx, &descriptor_offset) &&
            table_root_idx < kRootParameterSlotCount &&
            root_table_set[table_root_idx]) {
          for (uint32_t h = 0; h < desc_heap_count; h++) {
            auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
            if (!heap)
              continue;
            auto *desc = heap->GetDescriptorFromGPUHandle(
                root_tables[table_root_idx], descriptor_offset);
            if (desc && desc->cbv.BufferLocation) {
              gpu_address = desc->cbv.BufferLocation;
              break;
            }
          }
        }
      }

      uint64_t original_gpu_address = gpu_address;
      resolved_cbuffers.push_back(
          {&arg, gpu_address, original_gpu_address, root_idx});

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) {
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_ps_cbv_logs, 96)) {
          Logger::info(str::format(
              "M12 swapchain PS cbv resolve inline slot=", arg.SM50BindingSlot,
              " space=", arg.SM50RegisterSpace,
              " field=", arg.StructurePtrOffset, " root_idx=", root_idx,
              " gpu=0x", std::hex, (unsigned long long)gpu_address, std::dec,
              " vec4=", arg.SizeInVec4, " ", TracePsoShaderSummary(pso)));
        }
        QTRACE(
            "BuildConstantBufferTable: inline cb slot=%u field=%u gpu=0x%llx",
            arg.SM50BindingSlot, arg.StructurePtrOffset,
            (unsigned long long)gpu_address);
        continue;
      }

      gpu_address = StageConstantBufferAddress(
          device, gpu_address, WMTRenderStageFragment, "PSCBVTable");
      cbv_table_data[arg.StructurePtrOffset] = gpu_address;
      if (HasSwapchainRenderTarget() &&
          TakeLogBudget(&g_swapchain_ps_cbv_logs, 96)) {
        Logger::info(str::format(
            "M12 swapchain PS cbv resolve slot=", arg.SM50BindingSlot,
            " space=", arg.SM50RegisterSpace,
            " qword_off=", arg.StructurePtrOffset, " root_idx=", root_idx,
            " gpu=0x", std::hex, (unsigned long long)gpu_address,
            " original=0x", (unsigned long long)original_gpu_address, std::dec,
            " ", TracePsoShaderSummary(pso)));
      }
      QTRACE("BuildConstantBufferTable: cb slot=%u offset=%u gpu=0x%llx",
             arg.SM50BindingSlot, arg.StructurePtrOffset,
             (unsigned long long)gpu_address);

      if (gpu_address && render_enc_open) {
        auto *res = device->LookupResourceByGPUAddress(gpu_address);
        if (res && res->GetMTLBuffer().handle) {
          render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                 WMTRenderStageFragment);
          RetainResourceMetalObjectsForCompletion(res);
        }
      }
    }

    if (qword_count == 0)
      return;

    uint64_t inline_table_bytes = 0;
    if (has_inline_cbuffers) {
      std::sort(
          resolved_cbuffers.begin(), resolved_cbuffers.end(),
          [](const ResolvedConstantBuffer &a, const ResolvedConstantBuffer &b) {
            return a.arg->StructurePtrOffset < b.arg->StructurePtrOffset;
          });
      std::vector<uint8_t> inline_table_data;
      for (const auto &entry : resolved_cbuffers) {
        const auto &arg = *entry.arg;
        bool inline_cbuffer =
            (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) != 0;
        uint64_t align = inline_cbuffer ? 16 : 8;
        uint64_t field_offset = AlignUp64(inline_table_bytes, align);
        uint64_t field_size =
            inline_cbuffer ? std::max<uint32_t>(arg.SizeInVec4, 1) * 16ull : 8;
        if (inline_table_data.size() < field_offset + field_size)
          inline_table_data.resize(field_offset + field_size);

        if (inline_cbuffer) {
          CopyConstantBufferBytes(device, entry.gpu_address, field_size,
                                  inline_table_data.data(), field_offset);
        } else {
          uint64_t staged_address = StageConstantBufferAddress(
              device, entry.gpu_address, WMTRenderStageFragment, "PSCBVTable");
          std::memcpy(inline_table_data.data() + field_offset, &staged_address,
                      sizeof(staged_address));
        }
        inline_table_bytes = field_offset + field_size;
      }
      cbv_table_buf =
          MakeTransientTableSlice(device, inline_table_data.data(),
                                  inline_table_bytes, &cbv_table_buf_offset);
    } else {
      cbv_table_buf = MakeTransientTableSlice(
          device, cbv_table_data, qword_count * 8, &cbv_table_buf_offset);
    }
    if (cbv_table_buf.handle) {
      if (render_enc_open) {
        uint32_t bind_index = BindIndexOrFallback(
            pso->GetPSReflection().ConstanttBufferTableBindIndex,
            kConstantBufferTableSlot);
        SetFragmentBufferTracked(cbv_table_buf, cbv_table_buf_offset,
                                 bind_index);
        render_enc.useResource(cbv_table_buf, WMTResourceUsageRead,
                               WMTRenderStageFragment);
        if (pso->GetVSConstantBuffers().empty()) {
          SetVertexBufferTracked(cbv_table_buf, cbv_table_buf_offset,
                                 bind_index);
          render_enc.useResource(cbv_table_buf, WMTResourceUsageRead,
                                 WMTRenderStageVertex);
        }
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_ps_cbv_logs, 96)) {
          Logger::info(str::format(
              "M12 swapchain PS cbv table bind slot=", bind_index,
              " qwords=", qword_count, " inline_bytes=", inline_table_bytes,
              " data0=0x", std::hex, (unsigned long long)cbv_table_data[0],
              " data1=0x", (unsigned long long)cbv_table_data[1], std::dec,
              " handle=", (unsigned long long)cbv_table_buf.handle,
              " offset=", (unsigned long long)cbv_table_buf_offset, " ",
              TracePsoShaderSummary(pso)));
        }
        QTRACE("BuildConstantBufferTable: bound slot=%u qwords=%u", bind_index,
               qword_count);
      }
    }
  }

  void BuildVertexConstantBufferTable(MTLD3D12Device *device) {
    if (!pso || pso->GetVSConstantBuffers().empty()) {
      if (pso && HasSwapchainRenderTarget() &&
          TakeLogBudget(&g_swapchain_vs_cbv_logs, 96)) {
        Logger::info(str::format("M12 swapchain VS cbv table empty ",
                                 TracePsoShaderSummary(pso)));
      }
      return;
    }

    memset(vs_cbv_table_data, 0, sizeof(vs_cbv_table_data));

    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;
    auto &cb_args = pso->GetVSConstantBuffers();
    uint32_t qword_count = 0;
    struct ResolvedConstantBuffer {
      const MTL_SM50_SHADER_ARGUMENT *arg;
      uint64_t gpu_address;
      uint64_t original_gpu_address;
      uint32_t root_idx;
    };
    std::vector<ResolvedConstantBuffer> resolved_cbuffers;
    bool has_inline_cbuffers = false;

    for (const auto &arg : cb_args) {
      if (arg.Type != SM50BindingType::ConstantBuffer ||
          arg.StructurePtrOffset >= kConstantBufferMaxQwords)
        continue;

      qword_count = std::max(qword_count, arg.StructurePtrOffset + 1);
      has_inline_cbuffers |=
          (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) != 0;
      uint64_t gpu_address = 0;

      uint32_t root_idx = ~0u;
      bool root_idx_is_constants = false;
      if (dxmt_sig) {
        auto &params = dxmt_sig->GetParameters();
        for (uint32_t pass = 0; pass < 2 && root_idx == ~0u; pass++) {
          for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
               p++) {
            if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_CBV &&
                params[p].register_index == arg.SM50BindingSlot &&
                params[p].register_space == arg.SM50RegisterSpace &&
                ShaderVisibilityMatches(params[p].shader_visibility,
                                        D3D12_SHADER_VISIBILITY_VERTEX,
                                        pass == 0)) {
              root_idx = p;
              break;
            }
          }
        }
        for (uint32_t pass = 0; pass < 2 && root_idx == ~0u; pass++) {
          for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
               p++) {
            if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS &&
                params[p].register_index == arg.SM50BindingSlot &&
                params[p].register_space == arg.SM50RegisterSpace &&
                ShaderVisibilityMatches(params[p].shader_visibility,
                                        D3D12_SHADER_VISIBILITY_VERTEX,
                                        pass == 0)) {
              root_idx = p;
              root_idx_is_constants = true;
              break;
            }
          }
        }
      }

      if (root_idx_is_constants && root_idx < kRootParameterSlotCount &&
          root_constant_set[root_idx]) {
        gpu_address = StageRootConstantsAsConstantBuffer(
            device, root_idx, WMTRenderStageVertex, "VSCBVTableRootConstants");
      } else if (root_idx != ~0u && root_cbv_set[root_idx]) {
        gpu_address = root_cbvs[root_idx];
      } else if (dxmt_sig) {
        uint32_t table_root_idx = ~0u;
        uint32_t descriptor_offset = 0;
        if (dxmt_sig->FindDescriptorTableRangeForVisibility(
                D3D12_DESCRIPTOR_RANGE_TYPE_CBV, arg.SM50BindingSlot,
                arg.SM50RegisterSpace, D3D12_SHADER_VISIBILITY_VERTEX,
                &table_root_idx, &descriptor_offset) &&
            table_root_idx < kRootParameterSlotCount &&
            root_table_set[table_root_idx]) {
          for (uint32_t h = 0; h < desc_heap_count; h++) {
            auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
            if (!heap)
              continue;
            auto *desc = heap->GetDescriptorFromGPUHandle(
                root_tables[table_root_idx], descriptor_offset);
            if (desc && desc->cbv.BufferLocation) {
              gpu_address = desc->cbv.BufferLocation;
              break;
            }
          }
        }
      }

      uint64_t original_gpu_address = gpu_address;
      resolved_cbuffers.push_back(
          {&arg, gpu_address, original_gpu_address, root_idx});

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) {
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_vs_cbv_logs, 96)) {
          Logger::info(str::format(
              "M12 swapchain VS cbv resolve inline slot=", arg.SM50BindingSlot,
              " space=", arg.SM50RegisterSpace,
              " field=", arg.StructurePtrOffset, " root_idx=", root_idx,
              " gpu=0x", std::hex, (unsigned long long)gpu_address, std::dec,
              " vec4=", arg.SizeInVec4, " ", TracePsoShaderSummary(pso)));
        }
        QTRACE("BuildVertexConstantBufferTable: inline cb slot=%u field=%u "
               "gpu=0x%llx",
               arg.SM50BindingSlot, arg.StructurePtrOffset,
               (unsigned long long)gpu_address);
        continue;
      }

      gpu_address = StageConstantBufferAddress(
          device, gpu_address, WMTRenderStageVertex, "VSCBVTable");
      vs_cbv_table_data[arg.StructurePtrOffset] = gpu_address;
      if (HasSwapchainRenderTarget() &&
          TakeLogBudget(&g_swapchain_vs_cbv_logs, 96)) {
        Logger::info(str::format(
            "M12 swapchain VS cbv resolve slot=", arg.SM50BindingSlot,
            " space=", arg.SM50RegisterSpace,
            " qword_off=", arg.StructurePtrOffset, " root_idx=", root_idx,
            " gpu=0x", std::hex, (unsigned long long)gpu_address,
            " original=0x", (unsigned long long)original_gpu_address, std::dec,
            " ", TracePsoShaderSummary(pso)));
      }
      QTRACE("BuildVertexConstantBufferTable: cb slot=%u offset=%u gpu=0x%llx",
             arg.SM50BindingSlot, arg.StructurePtrOffset,
             (unsigned long long)gpu_address);

      if (gpu_address && render_enc_open) {
        auto *res = device->LookupResourceByGPUAddress(gpu_address);
        if (res && res->GetMTLBuffer().handle) {
          render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                 WMTRenderStageVertex);
          RetainResourceMetalObjectsForCompletion(res);
        }
      }
    }

    if (qword_count == 0)
      return;

    uint64_t inline_table_bytes = 0;
    if (has_inline_cbuffers) {
      std::sort(
          resolved_cbuffers.begin(), resolved_cbuffers.end(),
          [](const ResolvedConstantBuffer &a, const ResolvedConstantBuffer &b) {
            return a.arg->StructurePtrOffset < b.arg->StructurePtrOffset;
          });
      std::vector<uint8_t> inline_table_data;
      for (const auto &entry : resolved_cbuffers) {
        const auto &arg = *entry.arg;
        bool inline_cbuffer =
            (arg.Flags & MTL_SM50_SHADER_ARGUMENT_INLINE_CBUFFER) != 0;
        uint64_t align = inline_cbuffer ? 16 : 8;
        uint64_t field_offset = AlignUp64(inline_table_bytes, align);
        uint64_t field_size =
            inline_cbuffer ? std::max<uint32_t>(arg.SizeInVec4, 1) * 16ull : 8;
        if (inline_table_data.size() < field_offset + field_size)
          inline_table_data.resize(field_offset + field_size);

        if (inline_cbuffer) {
          CopyConstantBufferBytes(device, entry.gpu_address, field_size,
                                  inline_table_data.data(), field_offset);
        } else {
          uint64_t staged_address = StageConstantBufferAddress(
              device, entry.gpu_address, WMTRenderStageVertex, "VSCBVTable");
          std::memcpy(inline_table_data.data() + field_offset, &staged_address,
                      sizeof(staged_address));
        }
        inline_table_bytes = field_offset + field_size;
      }
      vs_cbv_table_buf =
          MakeTransientTableSlice(device, inline_table_data.data(),
                                  inline_table_bytes, &vs_cbv_table_buf_offset);
    } else {
      vs_cbv_table_buf = MakeTransientTableSlice(
          device, vs_cbv_table_data, qword_count * 8, &vs_cbv_table_buf_offset);
    }
    if (vs_cbv_table_buf.handle) {
      if (render_enc_open) {
        uint32_t bind_index = BindIndexOrFallback(
            pso->GetVSReflection().ConstanttBufferTableBindIndex,
            kConstantBufferTableSlot);
        SetVertexBufferTracked(vs_cbv_table_buf, vs_cbv_table_buf_offset,
                               bind_index);
        render_enc.useResource(vs_cbv_table_buf, WMTResourceUsageRead,
                               WMTRenderStageVertex);
        if (pso->GetPSConstantBuffers().empty()) {
          SetFragmentBufferTracked(vs_cbv_table_buf, vs_cbv_table_buf_offset,
                                   bind_index);
          render_enc.useResource(vs_cbv_table_buf, WMTResourceUsageRead,
                                 WMTRenderStageFragment);
        }
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_vs_cbv_logs, 96)) {
          Logger::info(str::format(
              "M12 swapchain VS cbv table bind slot=", bind_index,
              " qwords=", qword_count, " inline_bytes=", inline_table_bytes,
              " data0=0x", std::hex, (unsigned long long)vs_cbv_table_data[0],
              " data1=0x", (unsigned long long)vs_cbv_table_data[1], std::dec,
              " handle=", (unsigned long long)vs_cbv_table_buf.handle,
              " offset=", (unsigned long long)vs_cbv_table_buf_offset, " ",
              TracePsoShaderSummary(pso)));
        }
        QTRACE("BuildVertexConstantBufferTable: bound slot=%u qwords=%u",
               bind_index, qword_count);
      }
    }
  }

  void BuildVertexArgumentBuffer(MTLD3D12Device *device) {
    if (!pso || pso->GetVSArguments().empty()) {
      return;
    }

    auto &args = pso->GetVSArguments();
    uint32_t qword_count = pso->GetVSReflection().ArgumentTableQwords;
    QTRACE("BuildVertexArgumentBuffer: %u args, %u qwords, NumArguments=%u",
           (unsigned)args.size(), qword_count,
           (unsigned)pso->GetVSReflection().NumArguments);
    if (qword_count == 0 || qword_count > kArgBufMaxQwords) {
      QTRACE("BuildVertexArgumentBuffer: invalid qword_count=%u", qword_count);
      return;
    }
    memset(vs_arg_buf_data, 0, qword_count * 8);

    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;

    const auto shader_visibility =
        pso->UsesNativeMeshPipeline()
            ? D3D12_SHADER_VISIBILITY_AMPLIFICATION
            : D3D12_SHADER_VISIBILITY_VERTEX;
    const auto render_stage = pso->UsesNativeMeshPipeline()
                                  ? WMTRenderStageObject
                                  : WMTRenderStageVertex;

    for (auto &arg : args) {
      uint32_t root_idx = ~0u;
      uint32_t descriptor_offset = 0;
      if (dxmt_sig) {
        D3D12_DESCRIPTOR_RANGE_TYPE range_type =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bool table_arg = true;
        if (arg.Type == SM50BindingType::SRV) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        } else if (arg.Type == SM50BindingType::Sampler) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        } else if (arg.Type == SM50BindingType::UAV) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        } else if (arg.Type == SM50BindingType::ConstantBuffer) {
          range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        } else {
          table_arg = false;
        }
        if (table_arg) {
          dxmt_sig->FindDescriptorTableRangeForVisibility(
              range_type, arg.SM50BindingSlot, arg.SM50RegisterSpace,
              shader_visibility, &root_idx, &descriptor_offset);
        }
      }
      if (root_idx == ~0u || !root_table_set[root_idx] ||
          desc_heap_count == 0) {
        uint32_t root_desc_idx = ~0u;
        if (arg.Type == SM50BindingType::SRV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_srv_set[root_desc_idx] &&
            BindRootBufferArgument(device, vs_arg_buf_data, arg,
                                   root_srvs[root_desc_idx],
                                   WMTResourceUsageRead, render_stage,
                                   "BuildVertexArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::UAV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_uav_set[root_desc_idx] &&
            BindRootBufferArgument(device, vs_arg_buf_data, arg,
                                   root_uavs[root_desc_idx],
                                   (WMTResourceUsage)(WMTResourceUsageRead |
                                                      WMTResourceUsageWrite),
                                   render_stage, "BuildVertexArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::ConstantBuffer &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_cbv_set[root_desc_idx] &&
            WriteConstantBufferArgument(
                device, vs_arg_buf_data, arg, root_cbvs[root_desc_idx], 0,
                render_stage, "BuildVertexArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::Sampler && dxmt_sig) {
          if (auto *sampler = dxmt_sig->FindStaticSampler(
                  arg.SM50BindingSlot, arg.SM50RegisterSpace,
                  shader_visibility)) {
            vs_arg_buf_data[arg.StructurePtrOffset] = sampler->sampler_gpu_id;
            vs_arg_buf_data[arg.StructurePtrOffset + 1] =
                sampler->sampler_cube_gpu_id ? sampler->sampler_cube_gpu_id
                                             : sampler->sampler_gpu_id;
            vs_arg_buf_data[arg.StructurePtrOffset + 2] =
                sampler->lod_bias_bits;
            RetainSamplerPairForCompletion(sampler->sampler,
                                           sampler->sampler_cube);
            QTRACE("BuildVertexArgBuf: StaticSampler slot=%u space=%u "
                   "gpu_id=0x%llx offset=%u",
                   arg.SM50BindingSlot, arg.SM50RegisterSpace,
                   (unsigned long long)sampler->sampler_gpu_id,
                   arg.StructurePtrOffset);
            continue;
          }
        }
        QTRACE("BuildVertexArgBuf: arg type=%d slot=%u root_idx=%u desc_off=%u "
               "table_set=%d heaps=%u skip",
               (int)arg.Type, arg.SM50BindingSlot, root_idx, descriptor_offset,
               root_idx != ~0u ? root_table_set[root_idx] : 0, desc_heap_count);
        continue;
      }

      for (uint32_t h = 0; h < desc_heap_count; h++) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
        if (!heap)
          continue;
        auto *desc = heap->GetDescriptorFromGPUHandle(root_tables[root_idx],
                                                      descriptor_offset);
        if (!desc)
          continue;

        if (arg.Type == SM50BindingType::SRV) {
          QTRACE("BuildVertexArgBuf: SRV root=%u desc_off=%u desc=%p res=%p "
                 "flags=0x%x offset=%u",
                 root_idx, descriptor_offset, (void *)desc,
                 desc->resource ? (void *)desc->resource : nullptr, arg.Flags,
                 arg.StructurePtrOffset);
          if (desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            if (MSCArgumentAcceptsBuffer(arg, res) &&
                res->GetMTLBuffer().handle) {
              vs_arg_buf_data[arg.StructurePtrOffset] =
                  res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
              vs_arg_buf_data[arg.StructurePtrOffset + 1] =
                  SRVBufferByteLength(desc, res);
              if (render_enc_open)
                render_enc.useResource(res->GetMTLBuffer(),
                                       WMTResourceUsageRead,
                                       render_stage);
              RetainResourceMetalObjectsForCompletion(res);
            } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
              WriteMSCTextureArgument(vs_arg_buf_data, arg,
                                      DescriptorTextureGPUResourceID(desc, res),
                                      SRVTextureArrayLength(desc, res));
              if (render_enc_open)
                render_enc.useResource(
                    tex,
                    (WMTResourceUsage)(WMTResourceUsageSample |
                                       WMTResourceUsageRead),
                    render_stage);
              RetainMTLObjectForCompletion(tex);
            } else if (res->GetMTLBuffer().handle) {
              vs_arg_buf_data[arg.StructurePtrOffset] =
                  res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
              vs_arg_buf_data[arg.StructurePtrOffset + 1] =
                  SRVBufferByteLength(desc, res);
              if (render_enc_open)
                render_enc.useResource(res->GetMTLBuffer(),
                                       WMTResourceUsageRead,
                                       render_stage);
              RetainResourceMetalObjectsForCompletion(res);
            }
          }
        } else if (arg.Type == SM50BindingType::Sampler) {
          QTRACE("BuildVertexArgBuf: Sampler root=%u desc_off=%u desc_type=%u "
                 "gpu_id=0x%llx offset=%u",
                 root_idx, descriptor_offset, desc->type,
                 (unsigned long long)desc->metal_sampler_gpu_id,
                 arg.StructurePtrOffset);
          if (desc->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
              desc->metal_sampler_gpu_id) {
            vs_arg_buf_data[arg.StructurePtrOffset] =
                desc->metal_sampler_gpu_id;
            vs_arg_buf_data[arg.StructurePtrOffset + 1] =
                SamplerCubeGPUResourceID(desc);
            vs_arg_buf_data[arg.StructurePtrOffset + 2] =
                SamplerLodBiasBits(desc);
            RetainSamplerPairForCompletion(desc->metal_sampler,
                                           desc->metal_sampler_cube);
          }
        } else if (arg.Type == SM50BindingType::UAV && desc->resource) {
          auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
          QTRACE("BuildVertexArgBuf: UAV root=%u desc_off=%u desc=%p res=%p "
                 "flags=0x%x offset=%u",
                 root_idx, descriptor_offset, (void *)desc, (void *)res,
                 arg.Flags, arg.StructurePtrOffset);
          if (MSCArgumentAcceptsBuffer(arg, res) &&
              res->GetMTLBuffer().handle) {
            vs_arg_buf_data[arg.StructurePtrOffset] =
                res->GetGPUVirtualAddress() + UAVBufferByteOffset(desc);
            vs_arg_buf_data[arg.StructurePtrOffset + 1] =
                UAVBufferByteLength(desc, res);
            if (render_enc_open)
              render_enc.useResource(res->GetMTLBuffer(),
                                     (WMTResourceUsage)(WMTResourceUsageRead |
                                                        WMTResourceUsageWrite),
                                     render_stage);
            RetainResourceMetalObjectsForCompletion(res);
          } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
            WriteMSCTextureArgument(vs_arg_buf_data, arg,
                                    DescriptorTextureGPUResourceID(desc, res),
                                    UAVTextureArrayLength(desc, res));
            if (render_enc_open)
              render_enc.useResource(tex,
                                     (WMTResourceUsage)(WMTResourceUsageRead |
                                                        WMTResourceUsageWrite),
                                     render_stage);
            RetainMTLObjectForCompletion(tex);
          }
        } else if (arg.Type == SM50BindingType::ConstantBuffer) {
          QTRACE("BuildVertexArgBuf: CBV root=%u desc_off=%u addr=0x%llx "
                 "size=%u offset=%u",
                 root_idx, descriptor_offset,
                 (unsigned long long)desc->cbv.BufferLocation,
                 desc->cbv.SizeInBytes, arg.StructurePtrOffset);
          WriteConstantBufferArgument(
              device, vs_arg_buf_data, arg, desc->cbv.BufferLocation,
              desc->cbv.SizeInBytes, render_stage, "BuildVertexArgBuf");
        }
      }
    }

    vs_arg_buf = MakeTransientBuffer(device, kArgBufMaxQwords * 8);
    if (vs_arg_buf.handle) {
      vs_arg_buf.updateContents(0, vs_arg_buf_data, qword_count * 8);
      if (render_enc_open) {
        uint32_t bind_index = BindIndexOrFallback(
            pso->GetVSReflection().ArgumentBufferBindIndex, kArgBufSlot);
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_vs_argbuf_logs, 64)) {
          Logger::info(str::format(
              "M12 swapchain VS argbuf ", TracePsoShaderSummary(pso),
              " bind_index=", bind_index, " qwords=", qword_count, " data=[",
              (unsigned long long)vs_arg_buf_data[0], ",",
              (unsigned long long)(qword_count > 1 ? vs_arg_buf_data[1] : 0),
              ",",
              (unsigned long long)(qword_count > 2 ? vs_arg_buf_data[2] : 0),
              ",",
              (unsigned long long)(qword_count > 3 ? vs_arg_buf_data[3] : 0),
              ",",
              (unsigned long long)(qword_count > 4 ? vs_arg_buf_data[4] : 0),
              ",",
              (unsigned long long)(qword_count > 5 ? vs_arg_buf_data[5] : 0),
              ",",
              (unsigned long long)(qword_count > 6 ? vs_arg_buf_data[6] : 0),
              ",",
              (unsigned long long)(qword_count > 7 ? vs_arg_buf_data[7] : 0),
              ",",
              (unsigned long long)(qword_count > 8 ? vs_arg_buf_data[8] : 0),
              "]"));
        }
        if (pso->UsesNativeMeshPipeline()) {
          render_enc.useResource(vs_arg_buf, WMTResourceUsageRead,
                                 WMTRenderStageObject);
        } else {
          SetVertexBufferTracked(vs_arg_buf, 0, bind_index);
          render_enc.useResource(vs_arg_buf, WMTResourceUsageRead,
                                 WMTRenderStageVertex);
        }
        QTRACE("BuildVertexArgumentBuffer: bound slot=%u qwords=%u handle=%llu",
               bind_index, qword_count, (unsigned long long)vs_arg_buf.handle);
      }
    }
  }

  void BindGeometryMeshBuffers() {
    if (!pso || !pso->UsesGeometryMeshPipeline() || !render_enc_open)
      return;

    if (vertex_table_buf.handle) {
      render_enc.setObjectBuffer(vertex_table_buf, 0, kVertexBufferTableSlot);
      render_enc.useResource(vertex_table_buf, WMTResourceUsageRead,
                             WMTRenderStageObject);
    }
    if (vs_cbv_table_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetVSReflection().ConstanttBufferTableBindIndex,
          kConstantBufferTableSlot);
      render_enc.setObjectBuffer(vs_cbv_table_buf, vs_cbv_table_buf_offset,
                                 bind_index);
      render_enc.useResource(vs_cbv_table_buf, WMTResourceUsageRead,
                             WMTRenderStageObject);
    }
    if (vs_arg_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetVSReflection().ArgumentBufferBindIndex, kArgBufSlot);
      render_enc.setObjectBuffer(vs_arg_buf, 0, bind_index);
      render_enc.useResource(vs_arg_buf, WMTResourceUsageRead,
                             WMTRenderStageObject);
    }
    if (gs_cbv_table_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetGSReflection().ConstanttBufferTableBindIndex,
          kConstantBufferTableSlot);
      render_enc.setMeshBuffer(gs_cbv_table_buf, 0, bind_index);
      render_enc.useResource(gs_cbv_table_buf, WMTResourceUsageRead,
                             WMTRenderStageMesh);
    }
    if (gs_arg_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetGSReflection().ArgumentBufferBindIndex, kArgBufSlot);
      render_enc.setMeshBuffer(gs_arg_buf, 0, bind_index);
      render_enc.useResource(gs_arg_buf, WMTResourceUsageRead,
                             WMTRenderStageMesh);
    }
    if (cbv_table_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetPSReflection().ConstanttBufferTableBindIndex,
          kConstantBufferTableSlot);
      SetFragmentBufferTracked(cbv_table_buf, cbv_table_buf_offset, bind_index);
      render_enc.useResource(cbv_table_buf, WMTResourceUsageRead,
                             WMTRenderStageFragment);
    }
    if (arg_buf.handle) {
      uint32_t bind_index = BindIndexOrFallback(
          pso->GetPSReflection().ArgumentBufferBindIndex, kArgBufSlot);
      SetFragmentBufferTracked(arg_buf, 0, bind_index);
      render_enc.useResource(arg_buf, WMTResourceUsageRead,
                             WMTRenderStageFragment);
    }

    QTRACE("BindGeometryMeshBuffers: vertex_table=%llu vs_cbv=%llu vs_arg=%llu "
           "gs_cbv=%llu gs_arg=%llu ps_cbv=%llu ps_arg=%llu",
           (unsigned long long)vertex_table_buf.handle,
           (unsigned long long)vs_cbv_table_buf.handle,
           (unsigned long long)vs_arg_buf.handle,
           (unsigned long long)gs_cbv_table_buf.handle,
           (unsigned long long)gs_arg_buf.handle,
           (unsigned long long)cbv_table_buf.handle,
           (unsigned long long)arg_buf.handle);
  }

  void BuildGeometryConstantBufferTable(MTLD3D12Device *device) {
    if (!pso || pso->GetGSConstantBuffers().empty())
      return;

    memset(gs_cbv_table_data, 0, sizeof(gs_cbv_table_data));
    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;
    uint32_t qword_count = 0;

    for (const auto &arg : pso->GetGSConstantBuffers()) {
      if (arg.Type != SM50BindingType::ConstantBuffer ||
          arg.StructurePtrOffset >= kConstantBufferMaxQwords)
        continue;

      qword_count = std::max(qword_count, arg.StructurePtrOffset + 1);
      uint64_t gpu_address = 0;
      uint32_t root_idx = ~0u;
      if (dxmt_sig) {
        auto &params = dxmt_sig->GetParameters();
        for (uint32_t pass = 0; pass < 2 && root_idx == ~0u; pass++) {
          for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
               p++) {
            if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_CBV &&
                params[p].register_index == arg.SM50BindingSlot &&
                params[p].register_space == arg.SM50RegisterSpace &&
                ShaderVisibilityMatches(params[p].shader_visibility,
                                        D3D12_SHADER_VISIBILITY_GEOMETRY,
                                        pass == 0)) {
              root_idx = p;
              break;
            }
          }
        }
      }

      if (root_idx != ~0u && root_cbv_set[root_idx]) {
        gpu_address = root_cbvs[root_idx];
      } else if (dxmt_sig) {
        uint32_t table_root_idx = ~0u;
        uint32_t descriptor_offset = 0;
        if (dxmt_sig->FindDescriptorTableRangeForVisibility(
                D3D12_DESCRIPTOR_RANGE_TYPE_CBV, arg.SM50BindingSlot,
                arg.SM50RegisterSpace, D3D12_SHADER_VISIBILITY_GEOMETRY,
                &table_root_idx, &descriptor_offset) &&
            table_root_idx < kRootParameterSlotCount &&
            root_table_set[table_root_idx]) {
          for (uint32_t h = 0; h < desc_heap_count; h++) {
            auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
            if (!heap)
              continue;
            auto *desc = heap->GetDescriptorFromGPUHandle(
                root_tables[table_root_idx], descriptor_offset);
            if (desc && desc->cbv.BufferLocation) {
              gpu_address = desc->cbv.BufferLocation;
              break;
            }
          }
        }
      }

      gs_cbv_table_data[arg.StructurePtrOffset] = gpu_address;
      if (gpu_address && render_enc_open) {
        auto *res = device->LookupResourceByGPUAddress(gpu_address);
        if (res && res->GetMTLBuffer().handle) {
          render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                 WMTRenderStageMesh);
          RetainResourceMetalObjectsForCompletion(res);
        }
      }
    }

    if (qword_count == 0)
      return;

    gs_cbv_table_buf =
        MakeTransientBuffer(device, kConstantBufferMaxQwords * 8);
    if (gs_cbv_table_buf.handle) {
      gs_cbv_table_buf.updateContents(0, gs_cbv_table_data, qword_count * 8);
      if (render_enc_open) {
        uint32_t bind_index = BindIndexOrFallback(
            pso->GetGSReflection().ConstanttBufferTableBindIndex,
            kConstantBufferTableSlot);
        render_enc.setMeshBuffer(gs_cbv_table_buf, 0, bind_index);
        render_enc.useResource(gs_cbv_table_buf, WMTResourceUsageRead,
                               WMTRenderStageMesh);
      }
    }
  }

  void BuildGeometryArgumentBuffer(MTLD3D12Device *device) {
    if (!pso || pso->GetGSArguments().empty())
      return;

    auto &args = pso->GetGSArguments();
    const bool msc_linear_abi = pso->GSUsesMSCArgumentABI();
    const uint32_t buffer_metadata_qword = msc_linear_abi ? 2u : 1u;
    uint32_t qword_count = pso->GetGSReflection().ArgumentTableQwords;
    if (qword_count == 0 || qword_count > kArgBufMaxQwords)
      return;
    memset(gs_arg_buf_data, 0, qword_count * 8);

    auto *root_sig = pso->GetRootSignature();
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;

    const auto shader_visibility =
        pso->UsesNativeMeshPipeline() ? D3D12_SHADER_VISIBILITY_MESH
                                      : D3D12_SHADER_VISIBILITY_GEOMETRY;

    for (auto &arg : args) {
      uint32_t root_idx = ~0u;
      uint32_t descriptor_offset = 0;
      D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      bool table_arg = true;
      if (arg.Type == SM50BindingType::SRV)
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      else if (arg.Type == SM50BindingType::Sampler)
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      else if (arg.Type == SM50BindingType::UAV)
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      else if (arg.Type == SM50BindingType::ConstantBuffer)
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
      else
        table_arg = false;

      if (dxmt_sig && table_arg) {
        dxmt_sig->FindDescriptorTableRangeForVisibility(
            range_type, arg.SM50BindingSlot, arg.SM50RegisterSpace,
            shader_visibility, &root_idx, &descriptor_offset);
      }

      if (root_idx == ~0u || !root_table_set[root_idx] ||
          desc_heap_count == 0) {
        uint32_t root_desc_idx = ~0u;
        if (arg.Type == SM50BindingType::SRV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_srv_set[root_desc_idx] &&
            BindRootBufferArgument(device, gs_arg_buf_data, arg,
                                   root_srvs[root_desc_idx],
                                   WMTResourceUsageRead, WMTRenderStageMesh,
                                   "BuildGeometryArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::UAV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_uav_set[root_desc_idx] &&
            BindRootBufferArgument(device, gs_arg_buf_data, arg,
                                   root_uavs[root_desc_idx],
                                   (WMTResourceUsage)(WMTResourceUsageRead |
                                                      WMTResourceUsageWrite),
                                   WMTRenderStageMesh, "BuildGeometryArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::ConstantBuffer &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                        arg, shader_visibility,
                                        &root_desc_idx) &&
            root_cbv_set[root_desc_idx] &&
            WriteConstantBufferArgument(
                device, gs_arg_buf_data, arg, root_cbvs[root_desc_idx], 0,
                WMTRenderStageMesh, "BuildGeometryArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::Sampler && dxmt_sig) {
          if (auto *sampler = dxmt_sig->FindStaticSampler(
                  arg.SM50BindingSlot, arg.SM50RegisterSpace,
                  shader_visibility)) {
            if (msc_linear_abi) {
              WriteMSCLinearSamplerArgument(gs_arg_buf_data, arg,
                                            sampler->sampler_gpu_id,
                                            sampler->lod_bias_bits);
            } else {
              gs_arg_buf_data[arg.StructurePtrOffset] = sampler->sampler_gpu_id;
              gs_arg_buf_data[arg.StructurePtrOffset + 1] =
                  sampler->sampler_cube_gpu_id ? sampler->sampler_cube_gpu_id
                                               : sampler->sampler_gpu_id;
              gs_arg_buf_data[arg.StructurePtrOffset + 2] =
                  sampler->lod_bias_bits;
            }
            RetainSamplerPairForCompletion(sampler->sampler,
                                           sampler->sampler_cube);
          }
        }
        continue;
      }

      for (uint32_t h = 0; h < desc_heap_count; h++) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
        if (!heap)
          continue;
        auto *desc = heap->GetDescriptorFromGPUHandle(root_tables[root_idx],
                                                      descriptor_offset);
        if (!desc)
          continue;

        if (arg.Type == SM50BindingType::SRV && desc->resource) {
          auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
          if (MSCArgumentAcceptsBuffer(arg, res) &&
              res->GetMTLBuffer().handle) {
            gs_arg_buf_data[arg.StructurePtrOffset] =
                res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
            gs_arg_buf_data[arg.StructurePtrOffset + buffer_metadata_qword] =
                SRVBufferByteLength(desc, res);
            if (render_enc_open)
              render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                     WMTRenderStageMesh);
            RetainResourceMetalObjectsForCompletion(res);
          } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
            if (msc_linear_abi)
              WriteMSCLinearTextureArgument(
                  gs_arg_buf_data, arg,
                  DescriptorTextureGPUResourceID(desc, res));
            else
              WriteMSCTextureArgument(
                  gs_arg_buf_data, arg,
                  DescriptorTextureGPUResourceID(desc, res),
                  SRVTextureArrayLength(desc, res));
            if (render_enc_open)
              render_enc.useResource(tex,
                                     (WMTResourceUsage)(WMTResourceUsageSample |
                                                        WMTResourceUsageRead),
                                     WMTRenderStageMesh);
            RetainMTLObjectForCompletion(tex);
          }
        } else if (arg.Type == SM50BindingType::Sampler &&
                   desc->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
                   desc->metal_sampler_gpu_id) {
          if (msc_linear_abi) {
            WriteMSCLinearSamplerArgument(
                gs_arg_buf_data, arg, desc->metal_sampler_gpu_id,
                SamplerLodBiasBits(desc));
          } else {
            gs_arg_buf_data[arg.StructurePtrOffset] = desc->metal_sampler_gpu_id;
            gs_arg_buf_data[arg.StructurePtrOffset + 1] =
                SamplerCubeGPUResourceID(desc);
            gs_arg_buf_data[arg.StructurePtrOffset + 2] =
                SamplerLodBiasBits(desc);
          }
          RetainSamplerPairForCompletion(desc->metal_sampler,
                                         desc->metal_sampler_cube);
        } else if (arg.Type == SM50BindingType::UAV && desc->resource) {
          auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
          if (MSCArgumentAcceptsBuffer(arg, res) &&
              res->GetMTLBuffer().handle) {
            gs_arg_buf_data[arg.StructurePtrOffset] =
                res->GetGPUVirtualAddress() + UAVBufferByteOffset(desc);
            gs_arg_buf_data[arg.StructurePtrOffset + buffer_metadata_qword] =
                UAVBufferByteLength(desc, res);
            if (render_enc_open)
              render_enc.useResource(res->GetMTLBuffer(),
                                     (WMTResourceUsage)(WMTResourceUsageRead |
                                                        WMTResourceUsageWrite),
                                     WMTRenderStageMesh);
            RetainResourceMetalObjectsForCompletion(res);
          } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
            if (msc_linear_abi)
              WriteMSCLinearTextureArgument(
                  gs_arg_buf_data, arg,
                  DescriptorTextureGPUResourceID(desc, res));
            else
              WriteMSCTextureArgument(
                  gs_arg_buf_data, arg,
                  DescriptorTextureGPUResourceID(desc, res),
                  UAVTextureArrayLength(desc, res));
            if (render_enc_open)
              render_enc.useResource(tex,
                                     (WMTResourceUsage)(WMTResourceUsageRead |
                                                        WMTResourceUsageWrite),
                                     WMTRenderStageMesh);
            RetainMTLObjectForCompletion(tex);
          }
        } else if (arg.Type == SM50BindingType::ConstantBuffer) {
          WriteConstantBufferArgument(
              device, gs_arg_buf_data, arg, desc->cbv.BufferLocation,
              desc->cbv.SizeInBytes, WMTRenderStageMesh, "BuildGeometryArgBuf");
        }
      }
    }

    gs_arg_buf = MakeTransientBuffer(device, kArgBufMaxQwords * 8);
    if (gs_arg_buf.handle) {
      gs_arg_buf.updateContents(0, gs_arg_buf_data, qword_count * 8);
      if (render_enc_open) {
        uint32_t bind_index = BindIndexOrFallback(
            pso->GetGSReflection().ArgumentBufferBindIndex, kArgBufSlot);
        render_enc.setMeshBuffer(gs_arg_buf, 0, bind_index);
        render_enc.useResource(gs_arg_buf, WMTResourceUsageRead,
                               WMTRenderStageMesh);
      }
    }
  }

  bool EncodeGeometryDraw(MTLD3D12Device *device, uint32_t vertex_count,
                          uint32_t instance_count, uint32_t start_vertex,
                          uint32_t start_instance) {
    if (!pso || !pso->UsesGeometryMeshPipeline() || !render_enc_open ||
        vertex_count == 0 || instance_count == 0)
      return false;

    D3D12GeometryDrawArguments args = {};
    args.VertexCount = vertex_count;
    args.InstanceCount = instance_count;
    args.StartVertex = start_vertex;
    args.StartInstance = start_instance;
    geometry_draw_args_buf = MakeTransientBuffer(device, sizeof(args));
    if (!geometry_draw_args_buf.handle)
      return false;
    geometry_draw_args_buf.updateContents(0, &args, sizeof(args));
    render_enc.setObjectBuffer(geometry_draw_args_buf, 0, 21);
    render_enc.useResource(geometry_draw_args_buf, WMTResourceUsageRead,
                           WMTRenderStageObject);

    auto [vertex_per_warp, vertex_increment_per_warp] =
        D3D12GeometryVertexCount(topology);
    uint32_t warp_count =
        vertex_increment_per_warp
            ? ((vertex_count - 1) / vertex_increment_per_warp + 1)
            : 1;

    struct wmtcmd_render_dxmt_geometry_draw draw = {};
    draw.type = WMTRenderCommandDXMTGeometryDraw;
    draw.next.set(nullptr);
    draw.draw_arguments_offset = 0;
    draw.warp_count = warp_count;
    draw.instance_count = instance_count;
    draw.vertex_per_warp = vertex_per_warp;
    EncodeRenderCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                         "geometry_draw");
    QTRACE("EncodeGeometryDraw v=%u i=%u start=%u instance_start=%u "
           "warp=%u vertex_per_warp=%u",
           vertex_count, instance_count, start_vertex, start_instance,
           warp_count, vertex_per_warp);
    return true;
  }

  bool EncodeNativeMeshDispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (!pso || !pso->UsesNativeMeshPipeline() || !render_enc_open || !x ||
        !y || !z)
      return false;

    auto object_size = pso->GetObjectThreadgroupSize();
    auto mesh_size = pso->GetMeshThreadgroupSize();
    if (!object_size.width || !object_size.height || !object_size.depth)
      object_size = {1, 1, 1};
    if (!mesh_size.width || !mesh_size.height || !mesh_size.depth)
      mesh_size = {1, 1, 1};

    struct wmtcmd_render_draw_meshthreadgroups draw = {};
    draw.type = WMTRenderCommandDrawMeshThreadgroups;
    draw.next.set(nullptr);
    draw.threadgroup_per_grid = {(uint64_t)x, (uint64_t)y, (uint64_t)z};
    draw.object_threadgroup_size = object_size;
    draw.mesh_threadgroup_size = mesh_size;
    if (!EncodeRenderCommands(
            reinterpret_cast<const wmtcmd_render_nop *>(&draw),
            "native_mesh_dispatch"))
      return false;

    QTRACE("EncodeNativeMeshDispatch groups=%ux%ux%u object=%llux%llux%llu "
           "mesh=%llux%llux%llu",
           x, y, z, (unsigned long long)object_size.width,
           (unsigned long long)object_size.height,
           (unsigned long long)object_size.depth,
           (unsigned long long)mesh_size.width,
           (unsigned long long)mesh_size.height,
           (unsigned long long)mesh_size.depth);
    MarkSwapchainWorkEncoded();
    return true;
  }

  bool EncodeNativeTessellationDraw(MTLD3D12Device *device,
                                    uint32_t vertex_count,
                                    uint32_t instance_count,
                                    uint32_t start_vertex,
                                    uint32_t start_instance) {
    if (!pso || !pso->UsesNativeTessellationPath() || !render_enc_open ||
        vertex_count == 0 || instance_count == 0)
      return false;

    const uint32_t control_points =
        pso->GetNativeTessellationControlPointCount();
    const uint32_t topology_control_points =
        D3D12PatchControlPointCount(topology);
    if (control_points == 0 || topology_control_points != control_points ||
        vertex_count % control_points != 0 ||
        start_vertex % control_points != 0) {
      if (TakeLogBudget(&g_swapchain_draw_logs, 64)) {
        Logger::warn(
            str::format("M12 native_tessellation_unsupported draw ",
                        "reason=topology_or_patch_alignment control_points=",
                        control_points, " topology=", (unsigned)topology,
                        " topology_control_points=", topology_control_points,
                        " vertex_count=", vertex_count,
                        " start_vertex=", start_vertex, " pso=", (void *)pso));
      }
      return false;
    }

    constexpr uint32_t kNativeTessellationProofStride = 28u;
    const uint64_t requested_vertex_bytes =
        uint64_t(start_vertex) * kNativeTessellationProofStride +
        uint64_t(vertex_count) * kNativeTessellationProofStride;
    if (!vbs[0].BufferLocation ||
        vbs[0].StrideInBytes != kNativeTessellationProofStride ||
        requested_vertex_bytes > vbs[0].SizeInBytes) {
      if (TakeLogBudget(&g_swapchain_draw_logs, 64)) {
        Logger::warn(str::format(
            "M12 native_tessellation_unsupported draw ",
            "reason=ia_stride_or_range stride=", vbs[0].StrideInBytes,
            " expected_stride=", kNativeTessellationProofStride,
            " requested_bytes=", requested_vertex_bytes,
            " view_bytes=", vbs[0].SizeInBytes, " pso=", (void *)pso));
      }
      return false;
    }

    auto render_pso = pso->GetRenderPSO();
    if (!render_pso.handle)
      return false;
    render_enc.setRenderPipelineState(render_pso);
    RetainMTLObjectForCompletion(render_pso);

    struct TriangleFactorsHalf {
      uint16_t edge[3];
      uint16_t inside;
    } factors = {{0x3c00u, 0x3c00u, 0x3c00u}, 0x3c00u};

    auto factor_buf = MakeTransientBuffer(device, 256);
    if (!factor_buf.handle)
      return false;
    factor_buf.updateContents(0, &factors, sizeof(factors));
    render_enc.useResource(factor_buf, WMTResourceUsageRead,
                           WMTRenderStageVertex);

    struct wmtcmd_render_set_tessellation_factor_buffer set_factors = {};
    set_factors.type = WMTRenderCommandSetTessellationFactorBuffer;
    set_factors.next.set(nullptr);
    set_factors.buffer = factor_buf.handle;
    set_factors.offset = 0;
    set_factors.instance_stride = 0;
    if (!EncodeRenderCommands(
            reinterpret_cast<const wmtcmd_render_nop *>(&set_factors),
            "native_tessellation_set_factors"))
      return false;

    struct wmtcmd_render_draw_patches draw = {};
    draw.type = WMTRenderCommandDrawPatches;
    draw.next.set(nullptr);
    draw.number_of_patch_control_points = control_points;
    draw.patch_start = start_vertex / control_points;
    draw.patch_count = vertex_count / control_points;
    draw.patch_index_buffer = NULL_OBJECT_HANDLE;
    draw.patch_index_buffer_offset = 0;
    draw.instance_count = instance_count;
    draw.base_instance = start_instance;
    if (!EncodeRenderCommands(
            reinterpret_cast<const wmtcmd_render_nop *>(&draw),
            "native_tessellation_draw_patches"))
      return false;

    QTRACE("EncodeNativeTessellationDraw cp=%u patches=%llu inst=%u start=%u "
           "base_inst=%u factor=1",
           control_points, (unsigned long long)draw.patch_count, instance_count,
           start_vertex, start_instance);
    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_draw_logs, 64)) {
      Logger::info(str::format(
          "M12 native_tessellation_path draw encoded control_points=",
          control_points, " patches=", (unsigned long long)draw.patch_count,
          " instances=", instance_count, " start_vertex=", start_vertex,
          " pso=", (void *)pso,
          " implementation=d3d12_native_tessellation_path"));
    }
    return true;
  }

  bool EncodeNativeTessellationDrawIndexed(
      MTLD3D12Device *device, uint32_t index_count, uint32_t instance_count,
      uint32_t start_index, int32_t base_vertex, uint32_t start_instance) {
    if (!pso || !pso->UsesNativeTessellationPath() || !render_enc_open ||
        index_count == 0 || instance_count == 0 || !ib.BufferLocation)
      return false;

    const uint32_t control_points =
        pso->GetNativeTessellationControlPointCount();
    const uint32_t topology_control_points =
        D3D12PatchControlPointCount(topology);
    if (control_points == 0 || topology_control_points != control_points ||
        index_count % control_points != 0 || base_vertex != 0 ||
        ib.Format != DXGI_FORMAT_R16_UINT) {
      if (TakeLogBudget(&g_swapchain_draw_logs, 64)) {
        Logger::warn(str::format(
            "M12 native_tessellation_unsupported indexed_draw ",
            "reason=topology_patch_alignment_base_vertex_or_index_format "
            "control_points=",
            control_points, " topology=", (unsigned)topology,
            " topology_control_points=", topology_control_points,
            " index_count=", index_count, " base_vertex=", base_vertex,
            " index_format=", (unsigned)ib.Format, " pso=", (void *)pso));
      }
      return false;
    }

    constexpr uint32_t kNativeTessellationProofStride = 28u;
    if (!vbs[0].BufferLocation ||
        vbs[0].StrideInBytes != kNativeTessellationProofStride) {
      if (TakeLogBudget(&g_swapchain_draw_logs, 64)) {
        Logger::warn(
            str::format("M12 native_tessellation_unsupported indexed_draw ",
                        "reason=ia_stride stride=", vbs[0].StrideInBytes,
                        " expected_stride=", kNativeTessellationProofStride,
                        " pso=", (void *)pso));
      }
      return false;
    }

    auto indexed_render_pso = pso->GetNativeTessellationIndexedRenderPSO();
    if (!indexed_render_pso.handle)
      return false;
    render_enc.setRenderPipelineState(indexed_render_pso);
    RetainMTLObjectForCompletion(indexed_render_pso);

    constexpr uint64_t kIndexBytes = 2ull;
    const uint64_t requested_index_bytes = uint64_t(start_index) * kIndexBytes +
                                           uint64_t(index_count) * kIndexBytes;
    if (requested_index_bytes > ib.SizeInBytes) {
      if (TakeLogBudget(&g_swapchain_draw_logs, 64)) {
        Logger::warn(
            str::format("M12 native_tessellation_unsupported indexed_draw ",
                        "reason=index_range_oob start_index=", start_index,
                        " index_count=", index_count,
                        " index_bytes=", requested_index_bytes,
                        " view_bytes=", ib.SizeInBytes, " pso=", (void *)pso));
      }
      return false;
    }

    auto *ib_res = device->LookupResourceByGPUAddress(ib.BufferLocation);
    if (!ib_res && ib.BufferLocation)
      ib_res = reinterpret_cast<MTLD3D12Resource *>(ib.BufferLocation);
    if (!ib_res || !ib_res->GetMTLBuffer().handle)
      return false;

    uint64_t index_buffer_offset =
        ib.BufferLocation - ib_res->GetGPUVirtualAddress();
    index_buffer_offset += uint64_t(start_index) * kIndexBytes;

    struct TriangleFactorsHalf {
      uint16_t edge[3];
      uint16_t inside;
    } factors = {{0x3c00u, 0x3c00u, 0x3c00u}, 0x3c00u};

    auto factor_buf = MakeTransientBuffer(device, 256);
    if (!factor_buf.handle)
      return false;
    factor_buf.updateContents(0, &factors, sizeof(factors));
    render_enc.useResource(factor_buf, WMTResourceUsageRead,
                           WMTRenderStageVertex);
    render_enc.useResource(ib_res->GetMTLBuffer(), WMTResourceUsageRead,
                           WMTRenderStageVertex);
    RetainMTLObjectForCompletion(ib_res->GetMTLBuffer());

    struct wmtcmd_render_set_tessellation_factor_buffer set_factors = {};
    set_factors.type = WMTRenderCommandSetTessellationFactorBuffer;
    set_factors.next.set(nullptr);
    set_factors.buffer = factor_buf.handle;
    set_factors.offset = 0;
    set_factors.instance_stride = 0;
    if (!EncodeRenderCommands(
            reinterpret_cast<const wmtcmd_render_nop *>(&set_factors),
            "native_tessellation_set_factors_indexed"))
      return false;

    struct wmtcmd_render_draw_indexed_patches draw = {};
    draw.type = WMTRenderCommandDrawIndexedPatches;
    draw.next.set(nullptr);
    draw.number_of_patch_control_points = control_points;
    draw.patch_start = 0;
    draw.patch_count = index_count / control_points;
    draw.patch_index_buffer = NULL_OBJECT_HANDLE;
    draw.patch_index_buffer_offset = 0;
    draw.control_point_index_buffer = ib_res->GetMTLBuffer().handle;
    draw.control_point_index_buffer_offset = index_buffer_offset;
    draw.instance_count = instance_count;
    draw.base_instance = start_instance;
    if (!EncodeRenderCommands(
            reinterpret_cast<const wmtcmd_render_nop *>(&draw),
            "native_tessellation_draw_indexed_patches"))
      return false;

    QTRACE("EncodeNativeTessellationDrawIndexed cp=%u patches=%llu inst=%u "
           "start_index=%u base_inst=%u ib_off=%llu factor=1",
           control_points, (unsigned long long)draw.patch_count, instance_count,
           start_index, start_instance,
           (unsigned long long)index_buffer_offset);
    if (HasSwapchainRenderTarget() &&
        TakeLogBudget(&g_swapchain_draw_logs, 64)) {
      Logger::info(str::format(
          "M12 native_tessellation_path indexed draw encoded control_points=",
          control_points, " patches=", (unsigned long long)draw.patch_count,
          " instances=", instance_count, " start_index=", start_index, " pso=",
          (void *)pso, " implementation=d3d12_native_tessellation_path"));
    }
    return true;
  }

  bool EncodeGeometryDrawIndexed(MTLD3D12Device *device, uint32_t index_count,
                                 uint32_t instance_count, uint32_t start_index,
                                 int32_t base_vertex, uint32_t start_instance) {
    if (!pso || !pso->UsesGeometryMeshPipeline() || !render_enc_open ||
        index_count == 0 || instance_count == 0 || !ib.BufferLocation)
      return false;

    auto *ib_res = device->LookupResourceByGPUAddress(ib.BufferLocation);
    if (!ib_res && ib.BufferLocation)
      ib_res = reinterpret_cast<MTLD3D12Resource *>(ib.BufferLocation);
    if (!ib_res || !ib_res->GetMTLBuffer().handle)
      return false;

    uint64_t index_buffer_offset =
        ib.BufferLocation - ib_res->GetGPUVirtualAddress();
    index_buffer_offset += uint64_t(start_index) *
                           (ib.Format == DXGI_FORMAT_R32_UINT ? 4ull : 2ull);

    D3D12GeometryDrawIndexedArguments args = {};
    args.IndexCount = index_count;
    args.InstanceCount = instance_count;
    args.StartIndex = start_index;
    args.BaseVertex = base_vertex;
    args.StartInstance = start_instance;
    geometry_draw_args_buf = MakeTransientBuffer(device, sizeof(args));
    if (!geometry_draw_args_buf.handle)
      return false;
    geometry_draw_args_buf.updateContents(0, &args, sizeof(args));
    render_enc.setObjectBuffer(geometry_draw_args_buf, 0, 21);
    render_enc.useResource(geometry_draw_args_buf, WMTResourceUsageRead,
                           WMTRenderStageObject);

    auto [vertex_per_warp, vertex_increment_per_warp] =
        D3D12GeometryVertexCount(topology);
    uint32_t warp_count =
        vertex_increment_per_warp
            ? ((index_count - 1) / vertex_increment_per_warp + 1)
            : 1;

    struct wmtcmd_render_dxmt_geometry_draw_indexed draw = {};
    draw.type = WMTRenderCommandDXMTGeometryDrawIndexed;
    draw.next.set(nullptr);
    draw.draw_arguments_offset = 0;
    draw.index_buffer = ib_res->GetMTLBuffer().handle;
    draw.index_buffer_offset = index_buffer_offset;
    draw.warp_count = warp_count;
    draw.instance_count = instance_count;
    draw.vertex_per_warp = vertex_per_warp;
    EncodeRenderCommands(reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                         "geometry_draw_indexed");
    render_enc.useResource(ib_res->GetMTLBuffer(), WMTResourceUsageRead,
                           WMTRenderStageObject);
    RetainMTLObjectForCompletion(ib_res->GetMTLBuffer());
    QTRACE("EncodeGeometryDrawIndexed idx=%u inst=%u start=%u base=%d "
           "warp=%u vertex_per_warp=%u ib_off=%llu",
           index_count, instance_count, start_index, base_vertex, warp_count,
           vertex_per_warp, (unsigned long long)index_buffer_offset);
    return true;
  }

  uint32_t BuildComputeConstantBufferTable(MTLD3D12Device *device) {
    if (!pso || pso->GetCSConstantBuffers().empty())
      return 0;

    memset(comp_cbv_table_data, 0, sizeof(comp_cbv_table_data));
    auto *dxmt_sig =
        compute_root_sig
            ? compute_root_sig
            : static_cast<MTLD3D12RootSignature *>(pso->GetRootSignature());
    auto &cb_args = pso->GetCSConstantBuffers();
    uint32_t qword_count = 0;

    for (const auto &arg : cb_args) {
      if (arg.Type != SM50BindingType::ConstantBuffer ||
          arg.StructurePtrOffset >= kConstantBufferMaxQwords)
        continue;

      qword_count = std::max(qword_count, arg.StructurePtrOffset + 1);
      uint64_t gpu_address = 0;

      uint32_t root_idx = ~0u;
      if (dxmt_sig) {
        auto &params = dxmt_sig->GetParameters();
        for (uint32_t p = 0; p < params.size() && p < kRootParameterSlotCount;
             p++) {
          if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_CBV &&
              params[p].register_index == arg.SM50BindingSlot &&
              params[p].register_space == arg.SM50RegisterSpace) {
            root_idx = p;
            break;
          }
        }
      }

      if (root_idx != ~0u && comp_cbv_set[root_idx]) {
        gpu_address = comp_cbvs[root_idx];
      } else if (root_idx != ~0u && root_cbv_set[root_idx]) {
        gpu_address = root_cbvs[root_idx];
      } else if (dxmt_sig) {
        uint32_t table_root_idx = ~0u;
        uint32_t descriptor_offset = 0;
        if (dxmt_sig->FindDescriptorTableRange(
                D3D12_DESCRIPTOR_RANGE_TYPE_CBV, arg.SM50BindingSlot,
                arg.SM50RegisterSpace, &table_root_idx, &descriptor_offset) &&
            table_root_idx < kRootParameterSlotCount) {
          bool table_set =
              comp_table_set[table_root_idx] || root_table_set[table_root_idx];
          D3D12_GPU_DESCRIPTOR_HANDLE table_handle =
              comp_table_set[table_root_idx] ? comp_tables[table_root_idx]
                                             : root_tables[table_root_idx];
          if (table_set) {
            for (uint32_t h = 0; h < desc_heap_count; h++) {
              auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
              if (!heap)
                continue;
              auto *desc = heap->GetDescriptorFromGPUHandle(table_handle,
                                                            descriptor_offset);
              if (desc && desc->cbv.BufferLocation) {
                gpu_address = desc->cbv.BufferLocation;
                break;
              }
            }
          }
        }
      }

      comp_cbv_table_data[arg.StructurePtrOffset] = gpu_address;
      QTRACE("BuildComputeCBVTable: cb slot=%u offset=%u gpu=0x%llx",
             arg.SM50BindingSlot, arg.StructurePtrOffset,
             (unsigned long long)gpu_address);
    }

    if (qword_count == 0)
      return 0;

    comp_cbv_table_buf =
        MakeTransientBuffer(device, kConstantBufferMaxQwords * 8);
    if (comp_cbv_table_buf.handle) {
      comp_cbv_table_buf.updateContents(0, comp_cbv_table_data,
                                        qword_count * 8);
      QTRACE("BuildComputeCBVTable: wrote qwords=%u", qword_count);
      return qword_count;
    }
    return 0;
  }

  uint32_t BuildComputeArgumentBuffer(MTLD3D12Device *device) {
    if (!pso || pso->GetCSArguments().empty())
      return 0;

    uint32_t qword_count = pso->GetCSReflection().ArgumentTableQwords;
    if (qword_count == 0 || qword_count > kArgBufMaxQwords) {
      QTRACE("BuildComputeArgBuf: invalid qword_count=%u", qword_count);
      return 0;
    }
    memset(comp_arg_buf_data, 0, qword_count * 8);
    const bool msc_linear_abi = pso->CSUsesMSCArgumentABI();
    const uint32_t buffer_metadata_qword = msc_linear_abi ? 2u : 1u;
    struct PendingRootConstants {
      uint32_t argument_qword;
      const uint8_t *data;
      uint32_t byte_count;
    } pending_root_constants[kRootParameterSlotCount] = {};
    uint32_t pending_root_constant_count = 0;

    auto *dxmt_sig =
        compute_root_sig
            ? compute_root_sig
            : static_cast<MTLD3D12RootSignature *>(pso->GetRootSignature());

    for (const auto &arg : pso->GetCSArguments()) {
      D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      bool table_arg = true;
      if (arg.Type == SM50BindingType::SRV) {
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      } else if (arg.Type == SM50BindingType::Sampler) {
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      } else if (arg.Type == SM50BindingType::UAV) {
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      } else if (arg.Type == SM50BindingType::ConstantBuffer) {
        range_type = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
      } else {
        table_arg = false;
      }

      uint32_t root_idx = ~0u;
      uint32_t descriptor_offset = 0;
      if (table_arg && dxmt_sig) {
        dxmt_sig->FindDescriptorTableRange(range_type, arg.SM50BindingSlot,
                                           arg.SM50RegisterSpace, &root_idx,
                                           &descriptor_offset);
      }
      if (root_idx == ~0u || root_idx >= 16 ||
          !(comp_table_set[root_idx] || root_table_set[root_idx]) ||
          desc_heap_count == 0) {
        uint32_t root_desc_idx = ~0u;
        if (arg.Type == SM50BindingType::SRV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                        arg, D3D12_SHADER_VISIBILITY_ALL,
                                        &root_desc_idx) &&
            comp_srv_set[root_desc_idx] &&
            BindRootBufferArgument(device, comp_arg_buf_data, arg,
                                   comp_srvs[root_desc_idx],
                                   WMTResourceUsageRead, WMTRenderStageVertex,
                                   "BuildComputeArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::UAV &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                        arg, D3D12_SHADER_VISIBILITY_ALL,
                                        &root_desc_idx) &&
            comp_uav_set[root_desc_idx] &&
            BindRootBufferArgument(
                device, comp_arg_buf_data, arg, comp_uavs[root_desc_idx],
                (WMTResourceUsage)(WMTResourceUsageRead |
                                   WMTResourceUsageWrite),
                WMTRenderStageVertex, "BuildComputeArgBuf")) {
          continue;
        }
        if (arg.Type == SM50BindingType::ConstantBuffer &&
            FindRootDescriptorParameter(dxmt_sig, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                        arg, D3D12_SHADER_VISIBILITY_ALL,
                                        &root_desc_idx) &&
            (comp_cbv_set[root_desc_idx] || root_cbv_set[root_desc_idx])) {
          D3D12_GPU_VIRTUAL_ADDRESS cbv_addr = comp_cbv_set[root_desc_idx]
                                                   ? comp_cbvs[root_desc_idx]
                                                   : root_cbvs[root_desc_idx];
          if (WriteConstantBufferArgument(device, comp_arg_buf_data, arg,
                                          cbv_addr, 0, WMTRenderStageVertex,
                                          "BuildComputeArgBuf"))
            continue;
        }
        if (msc_linear_abi &&
            arg.Type == SM50BindingType::ConstantBuffer && dxmt_sig &&
            arg.StructurePtrOffset + 2 < kArgBufMaxQwords) {
          bool staged_root_constants = false;
          const auto &params = dxmt_sig->GetParameters();
          for (uint32_t p = 0;
               p < params.size() && p < kRootParameterSlotCount; p++) {
            if (params[p].type !=
                    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ||
                params[p].register_index != arg.SM50BindingSlot ||
                params[p].register_space != arg.SM50RegisterSpace)
              continue;
            bool set = comp_constant_set[p] || root_constant_set[p];
            uint32_t size = comp_constant_set[p] ? comp_constant_sizes[p]
                                                 : root_constant_sizes[p];
            uint32_t offset = comp_constant_set[p] ? comp_constant_offsets[p]
                                                   : root_constant_offsets[p];
            const uint8_t *constants = comp_constant_set[p]
                                           ? comp_constants_buf
                                           : root_constants_buf;
            if (set && size && offset + size <=
                                   kRootParameterSlotCount *
                                       kRootConstantBytes &&
                pending_root_constant_count < kRootParameterSlotCount) {
              pending_root_constants[pending_root_constant_count++] = {
                  arg.StructurePtrOffset, constants + offset, size};
              staged_root_constants = true;
              QTRACE("BuildComputeArgBuf: staged root constants b%u "
                     "space=%u param=%u size=%u argument_offset=%u",
                     arg.SM50BindingSlot, arg.SM50RegisterSpace, p, size,
                     arg.StructurePtrOffset);
              break;
            }
          }
          if (staged_root_constants)
            continue;
        }
        if (arg.Type == SM50BindingType::Sampler && dxmt_sig) {
          if (auto *sampler = dxmt_sig->FindStaticSampler(
                  arg.SM50BindingSlot, arg.SM50RegisterSpace,
                  D3D12_SHADER_VISIBILITY_ALL)) {
            if (msc_linear_abi) {
              WriteMSCLinearSamplerArgument(comp_arg_buf_data, arg,
                                            sampler->sampler_gpu_id,
                                            sampler->lod_bias_bits);
            } else {
              comp_arg_buf_data[arg.StructurePtrOffset] =
                  sampler->sampler_gpu_id;
              comp_arg_buf_data[arg.StructurePtrOffset + 1] =
                  sampler->sampler_cube_gpu_id ? sampler->sampler_cube_gpu_id
                                               : sampler->sampler_gpu_id;
              comp_arg_buf_data[arg.StructurePtrOffset + 2] =
                  sampler->lod_bias_bits;
            }
            RetainSamplerPairForCompletion(sampler->sampler,
                                           sampler->sampler_cube);
            QTRACE("BuildComputeArgBuf: StaticSampler slot=%u space=%u "
                   "gpu_id=0x%llx offset=%u",
                   arg.SM50BindingSlot, arg.SM50RegisterSpace,
                   (unsigned long long)sampler->sampler_gpu_id,
                   arg.StructurePtrOffset);
            continue;
          }
        }
        QTRACE("BuildComputeArgBuf: arg type=%d slot=%u root_idx=%u "
               "desc_off=%u skip",
               (int)arg.Type, arg.SM50BindingSlot, root_idx, descriptor_offset);
        continue;
      }

      D3D12_GPU_DESCRIPTOR_HANDLE table_handle = comp_table_set[root_idx]
                                                     ? comp_tables[root_idx]
                                                     : root_tables[root_idx];
      for (uint32_t h = 0; h < desc_heap_count; h++) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
        if (!heap)
          continue;
        auto *desc =
            heap->GetDescriptorFromGPUHandle(table_handle, descriptor_offset);
        if (!desc)
          continue;

        if (arg.Type == SM50BindingType::Sampler) {
          QTRACE("BuildComputeArgBuf: Sampler root=%u desc_off=%u desc_type=%u "
                 "gpu_id=0x%llx offset=%u",
                 root_idx, descriptor_offset, desc->type,
                 (unsigned long long)desc->metal_sampler_gpu_id,
                 arg.StructurePtrOffset);
          if (desc->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
              desc->metal_sampler_gpu_id) {
            if (msc_linear_abi) {
              WriteMSCLinearSamplerArgument(
                  comp_arg_buf_data, arg, desc->metal_sampler_gpu_id,
                  SamplerLodBiasBits(desc));
            } else {
              comp_arg_buf_data[arg.StructurePtrOffset] =
                  desc->metal_sampler_gpu_id;
              comp_arg_buf_data[arg.StructurePtrOffset + 1] =
                  SamplerCubeGPUResourceID(desc);
              comp_arg_buf_data[arg.StructurePtrOffset + 2] =
                  SamplerLodBiasBits(desc);
            }
            RetainSamplerPairForCompletion(desc->metal_sampler,
                                           desc->metal_sampler_cube);
          }
          continue;
        }

        if (arg.Type == SM50BindingType::ConstantBuffer) {
          QTRACE("BuildComputeArgBuf: CBV root=%u desc_off=%u addr=0x%llx "
                 "size=%u offset=%u",
                 root_idx, descriptor_offset,
                 (unsigned long long)desc->cbv.BufferLocation,
                 desc->cbv.SizeInBytes, arg.StructurePtrOffset);
          WriteConstantBufferArgument(
              device, comp_arg_buf_data, arg, desc->cbv.BufferLocation,
              desc->cbv.SizeInBytes, WMTRenderStageVertex,
              "BuildComputeArgBuf");
          continue;
        }

        if (!desc->resource)
          continue;
        auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
        QTRACE("BuildComputeArgBuf: res arg type=%d root=%u desc_off=%u res=%p "
               "flags=0x%x offset=%u",
               (int)arg.Type, root_idx, descriptor_offset, (void *)res,
               arg.Flags, arg.StructurePtrOffset);
        if (arg.Type == SM50BindingType::SRV &&
            desc->srv.ViewDimension ==
                D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE &&
            res->GetMTLAccelerationStructure().handle) {
          uint64_t header_gpu_address =
              res->GetRaytracingHeaderGPUAddress();
          comp_arg_buf_data[arg.StructurePtrOffset] = header_gpu_address;
          comp_arg_buf_data[arg.StructurePtrOffset + 1] = 0;
          comp_arg_buf_data[arg.StructurePtrOffset + 2] = 0;
          RetainMTLObjectForCompletion(res->GetMTLAccelerationStructure());
          RetainMTLObjectForCompletion(res->GetRaytracingHeaderBuffer());
          RetainMTLObjectForCompletion(
              res->GetRaytracingInstanceContributionsBuffer());
          RetainResourceMetalObjectsForCompletion(res);
          QTRACE("BuildComputeArgBuf: acceleration structure header=0x%llx "
                 "offset=%u",
                 (unsigned long long)header_gpu_address,
                 arg.StructurePtrOffset);
        } else if (MSCArgumentAcceptsBuffer(arg, res) &&
                   res->GetMTLBuffer().handle) {
          if (arg.Type == SM50BindingType::UAV) {
            comp_arg_buf_data[arg.StructurePtrOffset] =
                res->GetGPUVirtualAddress() + UAVBufferByteOffset(desc);
            comp_arg_buf_data[arg.StructurePtrOffset +
                              buffer_metadata_qword] =
                UAVBufferByteLength(desc, res);
          } else {
            comp_arg_buf_data[arg.StructurePtrOffset] =
                res->GetGPUVirtualAddress() + SRVBufferByteOffset(desc);
            comp_arg_buf_data[arg.StructurePtrOffset +
                              buffer_metadata_qword] =
                SRVBufferByteLength(desc, res);
          }
          QTRACE("BuildComputeArgBuf: buffer ptr=0x%llx len=%llu offset=%u",
                 (unsigned long long)comp_arg_buf_data[arg.StructurePtrOffset],
                 (unsigned long long)comp_arg_buf_data[arg.StructurePtrOffset + 2],
                 arg.StructurePtrOffset);
          RetainResourceMetalObjectsForCompletion(res);
        } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
          uint64_t gpu_id = DescriptorTextureGPUResourceID(desc, res);
          if (msc_linear_abi)
            WriteMSCLinearTextureArgument(comp_arg_buf_data, arg, gpu_id);
          else
            WriteMSCTextureArgument(
                comp_arg_buf_data, arg, gpu_id,
                arg.Type == SM50BindingType::UAV
                    ? UAVTextureArrayLength(desc, res)
                    : SRVTextureArrayLength(desc, res));
          RetainMTLObjectForCompletion(tex);
        }
      }
    }

    uint32_t data_byte_count = qword_count * sizeof(uint64_t);
    uint32_t root_constant_offset =
        (uint32_t)AlignUp64(data_byte_count, 16);
    for (uint32_t i = 0; i < pending_root_constant_count; i++) {
      auto &pending = pending_root_constants[i];
      root_constant_offset = (uint32_t)AlignUp64(root_constant_offset, 16);
      if (root_constant_offset + pending.byte_count >
          kArgBufMaxQwords * sizeof(uint64_t)) {
        QTRACE("BuildComputeArgBuf: root constants overflow offset=%u size=%u",
               root_constant_offset, pending.byte_count);
        continue;
      }
      memcpy(reinterpret_cast<uint8_t *>(comp_arg_buf_data) +
                 root_constant_offset,
             pending.data, pending.byte_count);
      root_constant_offset += pending.byte_count;
    }

    uint64_t comp_arg_gpu_address = 0;
    comp_arg_buf = MakeTransientBuffer(device, kArgBufMaxQwords * 8,
                                       &comp_arg_gpu_address);
    if (comp_arg_buf.handle) {
      root_constant_offset = (uint32_t)AlignUp64(data_byte_count, 16);
      for (uint32_t i = 0; i < pending_root_constant_count; i++) {
        auto &pending = pending_root_constants[i];
        root_constant_offset = (uint32_t)AlignUp64(root_constant_offset, 16);
        if (root_constant_offset + pending.byte_count >
            kArgBufMaxQwords * sizeof(uint64_t))
          continue;
        comp_arg_buf_data[pending.argument_qword] =
            comp_arg_gpu_address + root_constant_offset;
        comp_arg_buf_data[pending.argument_qword + 1] = pending.byte_count;
        comp_arg_buf_data[pending.argument_qword + 2] = 0;
        QTRACE("BuildComputeArgBuf: root constants address=0x%llx len=%u "
               "argument_offset=%u",
               (unsigned long long)comp_arg_buf_data[pending.argument_qword],
               pending.byte_count, pending.argument_qword);
        root_constant_offset += pending.byte_count;
      }
      uint32_t total_byte_count = std::max(data_byte_count,
                                           root_constant_offset);
      comp_arg_buf.updateContents(0, comp_arg_buf_data, total_byte_count);
      uint32_t total_qwords = (total_byte_count + 7) / 8;
      QTRACE("BuildComputeArgBuf: wrote qwords=%u root_constants=%u",
             total_qwords, pending_root_constant_count);
      return total_qwords;
    }
    return 0;
  }

  void CloseRenderEncoder() {
    if (render_enc_open && render_enc.handle) {
      EndMetalEncoder(render_enc, "render_ensure");
    } else if (render_enc_open) {
      QTRACE("CloseRenderEncoder: open flag set without encoder handle");
    }
    render_enc_open = false;
    render_enc_has_dsv = false;
    render_enc_dsv_format = DXGI_FORMAT_UNKNOWN;
    ResetTrackedRenderBindings();
    render_enc = WMT::RenderCommandEncoder{};
  }

  DXGI_FORMAT EffectiveDSVFormatForPSO(MTLD3D12PipelineState *state) const {
    if (!has_dsv || !state)
      return DXGI_FORMAT_UNKNOWN;
    return state->GetDSVFormat();
  }

  bool EncodeRenderCommands(const wmtcmd_render_nop *cmd, const char *label) {
    if (!render_enc_open || !render_enc.handle) {
      QTRACE("%s: skipped because render encoder is not open",
             label ? label : "render_encode");
      return false;
    }

    if (render_enc.encodeCommands(cmd))
      return true;

    Logger::info(str::format("M12 render encoder encode failed label=",
                             label ? label : "render_encode",
                             " enc=", (unsigned long long)render_enc.handle,
                             " pso=", (void *)pso, " ",
                             TracePsoShaderSummary(pso)));
    QTRACE("%s: encode failed; closing poisoned render encoder",
           label ? label : "render_encode");
    CloseRenderEncoder();
    return false;
  }

  WMTPrimitiveType GetMetalPrimitiveType() {
    if (D3D12IsPatchTopology(topology)) {
      uint32_t control_points = D3D12PatchControlPointCount(topology);
      if (control_points == 1)
        return WMTPrimitiveTypePoint;
      if (control_points == 2)
        return WMTPrimitiveTypeLine;
      return WMTPrimitiveTypeTriangle;
    }

    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
      return WMTPrimitiveTypePoint;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
      return WMTPrimitiveTypeLine;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
      return WMTPrimitiveTypeLineStrip;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
      return WMTPrimitiveTypeTriangle;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
      return WMTPrimitiveTypeTriangleStrip;
    default:
      return WMTPrimitiveTypeTriangle;
    }
  }

  void LogTessellationFallbackDraw(const char *label, uint32_t element_count,
                                   uint32_t instance_count, bool indexed) {
    if (!pso || !pso->UsesTessellationFallback() ||
        !D3D12IsPatchTopology(topology))
      return;

    if (TakeLogBudget(&g_tessellation_fallback_draw_logs, 64)) {
      Logger::warn(str::format(
          "M12 tessellation fallback draw label=", label ? label : "draw",
          " indexed=", indexed,
          " patch_control_points=", D3D12PatchControlPointCount(topology),
          " elements=", element_count, " instances=", instance_count,
          " primitive_type=", (unsigned)GetMetalPrimitiveType(),
          " pso=", (void *)pso, " ", TracePsoShaderSummary(pso)));
    }
  }

  void EnsureRenderEncoder() {
    if (render_enc_open)
      return;

    if (rt_count == 0) {
      QTRACE("EnsureRenderEncoder: no render targets set, skipping");
      return;
    }

    WMTRenderPassInfo rp = {};
    bool has_swapchain_rt = false;
    uint32_t swapchain_rt_index = 0;
    uint32_t swapchain_backbuffer_index = 0;
    for (uint32_t i = 0; i < 8; i++) {
      rp.colors[i].texture = NULL_OBJECT_HANDLE;
      rp.colors[i].load_action = WMTLoadActionLoad;
      rp.colors[i].store_action = WMTStoreActionStore;
      rp.colors[i].level = 0;
      rp.colors[i].slice = 0;
    }
    rp.depth.texture = NULL_OBJECT_HANDLE;
    rp.depth.load_action = WMTLoadActionLoad;
    rp.depth.store_action = WMTStoreActionStore;
    rp.stencil.texture = NULL_OBJECT_HANDLE;
    rp.stencil.load_action = WMTLoadActionLoad;
    rp.stencil.store_action = WMTStoreActionStore;
    depth_bounds_dsv_texture = nullptr;
    depth_bounds_dsv_slice = 0;

    bool has_valid_rt = false;
    uint16_t render_target_array_length = 1;
    bool has_render_target_array = false;
    for (uint32_t i = 0; i < rt_count && i < 8; i++) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(rt_handles[i].ptr);
      if (desc && desc->resource) {
        auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
        auto tex = res->GetMTLTexture();
        QTRACE("EnsureRenderEncoder: rt[%u] desc=%p res=%p tex=%llu", i,
               (void *)desc, (void *)res, (unsigned long long)tex.handle);
        if (tex.handle) {
          rp.colors[i].texture = tex.handle;
          rp.colors[i].level = RTVMipLevel(desc);
          rp.colors[i].slice = RTVArraySlice(desc);
          const uint16_t attachment_array_length = RTVArrayLength(desc);
          if (attachment_array_length > 1) {
            render_target_array_length = has_render_target_array
                                             ? std::min(render_target_array_length,
                                                        attachment_array_length)
                                             : attachment_array_length;
            has_render_target_array = true;
          }
          RetainMTLObjectForCompletion(tex);
          has_valid_rt = true;
          if (res->IsSwapchainBackBuffer()) {
            has_swapchain_rt = true;
            swapchain_rt_index = i;
            swapchain_backbuffer_index = res->SwapchainBackBufferIndex();
          }
        }
      }
    }
    DXGI_FORMAT effective_dsv_format = EffectiveDSVFormatForPSO(pso);
    if (has_dsv && effective_dsv_format != DXGI_FORMAT_UNKNOWN) {
      auto *desc = reinterpret_cast<const D3D12Descriptor *>(dsv_handle.ptr);
      if (desc && desc->resource) {
        auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
        QTRACE("EnsureRenderEncoder: dsv desc=%p res=%p tex=%llu", (void *)desc,
               (void *)res, (unsigned long long)res->GetMTLTexture().handle);
        auto dsv_tex = res->GetMTLTexture();
        if (dsv_tex.handle) {
          rp.depth.texture = dsv_tex.handle;
          rp.depth.level = DSVMipLevel(desc);
          rp.depth.slice = DSVArraySlice(desc);
          const uint16_t attachment_array_length = DSVArrayLength(desc);
          if (attachment_array_length > 1) {
            render_target_array_length = has_render_target_array
                                             ? std::min(render_target_array_length,
                                                        attachment_array_length)
                                             : attachment_array_length;
            has_render_target_array = true;
          }
          RetainMTLObjectForCompletion(dsv_tex);
          if (DSVHasStencil(desc)) {
            rp.stencil.texture = dsv_tex.handle;
            rp.stencil.level = DSVMipLevel(desc);
            rp.stencil.slice = DSVArraySlice(desc);
          }
          has_valid_rt = true;
          render_enc_has_dsv = true;
          render_enc_dsv_format = effective_dsv_format;
          uint64_t view_gpu_id = 0;
          WMTTextureSwizzleChannels swizzle = {
              WMTTextureSwizzleRed, WMTTextureSwizzleGreen,
              WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha};
          const bool multisampled =
              desc->dsv.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS ||
              desc->dsv.ViewDimension ==
                  D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
          depth_bounds_dsv_texture = dsv_tex.newTextureView(
              dsv_tex.pixelFormat(),
              multisampled ? WMTTextureType2DMultisampleArray
                           : WMTTextureType2DArray,
              DSVMipLevel(desc), 1, DSVArraySlice(desc),
              DSVArrayLength(desc), swizzle, view_gpu_id);
          // The texture view begins at the DSV's first array slice, so the
          // fragment's render-target array index is already view-relative.
          depth_bounds_dsv_slice = 0;
        }
      }
    }

    rp.render_target_array_length = static_cast<uint8_t>(
        std::min<uint16_t>(render_target_array_length, UINT8_MAX));

    if (!has_valid_rt) {
      QTRACE("EnsureRenderEncoder: no valid RT texture found, skipping");
      return;
    }

    QTRACE("EnsureRenderEncoder: creating render encoder rt_count=%u pso=%p "
           "compiled=%d stage=%s detail=%s",
           rt_count, (void *)pso, pso ? pso->IsCompiled() : 0,
           TraceCompileFailureStage(pso), TraceCompileFailureDetail(pso));
    render_enc = cmdbuf.renderCommandEncoder(rp);
    ENC_CREATE("render_ensure", render_enc.handle);
    if (!render_enc.handle) {
      QTRACE("EnsureRenderEncoder: FAILED to create render encoder!");
      return;
    }
    render_enc_open = true;
    ResetTrackedRenderBindings();
    if (has_swapchain_rt && TakeLogBudget(&g_swapchain_encoder_logs, 24)) {
      Logger::info(str::format(
          "M12 swapchain render encoder created rt=", swapchain_rt_index,
          " backbuffer=", swapchain_backbuffer_index,
          " tex=", (unsigned long long)rp.colors[swapchain_rt_index].texture,
          " pso=", (void *)pso, " compiled=", pso ? pso->IsCompiled() : 0));
    }

    if (pso && pso->IsCompiled() && pso->GetRenderPSO().handle) {
      render_enc.setRenderPipelineState(pso->GetRenderPSO());
      RetainMTLObjectForCompletion(pso->GetRenderPSO());
      if (pso->GetDepthStencilState().handle) {
        render_enc.setDepthStencilState(pso->GetDepthStencilState());
        RetainMTLObjectForCompletion(pso->GetDepthStencilState());
      }
      ApplyFixedFunctionState();
    } else {
      QTRACE("EnsureRenderEncoder: RENDER_PSO_NOT_BOUND pso=%p compiled=%d "
             "render_handle=%llu stage=%s detail=%s",
             (void *)pso, pso ? pso->IsCompiled() : 0,
             (unsigned long long)(pso ? pso->GetRenderPSO().handle : 0),
             TraceCompileFailureStage(pso), TraceCompileFailureDetail(pso));
    }

    if (viewport_count > 0) {
      for (uint32_t i = 0; i < viewport_count; i++) {
        WMTViewport vp = {
            (double)viewports[i].TopLeftX, (double)viewports[i].TopLeftY,
            (double)viewports[i].Width,    (double)viewports[i].Height,
            viewports[i].MinDepth,         viewports[i].MaxDepth};
        render_enc.setViewport(vp);
      }
    }

    if (scissor_count > 0) {
      const auto &rect = scissor_rects[0];
      LONG left = std::max<LONG>(0, rect.left);
      LONG top = std::max<LONG>(0, rect.top);
      LONG right = std::max<LONG>(left, rect.right);
      LONG bottom = std::max<LONG>(top, rect.bottom);
      render_enc.setScissorRect({(uint64_t)left, (uint64_t)top,
                                 (uint64_t)(right - left),
                                 (uint64_t)(bottom - top)});
    }

    if (pso && pso->IsDepthBoundsTestEnabled() &&
        depth_bounds_dsv_texture.handle) {
      const float depth_bounds[4] = {
          depth_bounds_min, depth_bounds_max,
          static_cast<float>(depth_bounds_dsv_slice),
          depth_bounds_inverted ? 1.0f : 0.0f};
      render_enc.setFragmentBytes(depth_bounds, sizeof(depth_bounds), 28);
      render_enc.setFragmentTexture(depth_bounds_dsv_texture, 126);
      RetainMTLObjectForCompletion(depth_bounds_dsv_texture);
    }
  }

  void ApplyFixedFunctionState() {
    if (!render_enc_open || !pso)
      return;

    const auto &rast = pso->GetRasterizerDesc();
    WMTTriangleFillMode fill_mode = rast.FillMode == D3D12_FILL_MODE_WIREFRAME
                                        ? WMTTriangleFillModeLines
                                        : WMTTriangleFillModeFill;
    WMTCullMode cull_mode = WMTCullModeNone;
    if (rast.CullMode == D3D12_CULL_MODE_BACK)
      cull_mode = WMTCullModeBack;
    else if (rast.CullMode == D3D12_CULL_MODE_FRONT)
      cull_mode = WMTCullModeFront;
    // UE5 frontend paths can land in a stage-in coordinate convention that
    // flips quad winding relative to the D3D rasterizer state. If the active
    // render target is a swapchain backbuffer, prefer visibility over culling.
    if (HasSwapchainRenderTarget())
      cull_mode = WMTCullModeNone;
    WMTDepthClipMode depth_clip =
        rast.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
    WMTWinding winding = rast.FrontCounterClockwise ? WMTWindingCounterClockwise
                                                    : WMTWindingClockwise;
    render_enc.setRasterizerState(
        fill_mode, cull_mode, depth_clip, winding, (float)rast.DepthBias,
        rast.SlopeScaledDepthBias, rast.DepthBiasClamp);
    render_enc.setBlendFactorAndStencilRef(blend_factor, stencil_ref);
    QTRACE("ApplyFixedFunctionState: fill=%u cull=%u depth_clip=%u winding=%u "
           "blend=(%.3f,%.3f,%.3f,%.3f) stencil=%u depth_bounds=%u "
           "range=(%.3f,%.3f) inverted=%u",
           (unsigned)fill_mode, (unsigned)cull_mode, (unsigned)depth_clip,
           (unsigned)winding, blend_factor[0], blend_factor[1], blend_factor[2],
           blend_factor[3], stencil_ref,
           pso->IsDepthBoundsTestEnabled() ? 1u : 0u, depth_bounds_min,
           depth_bounds_max, depth_bounds_inverted ? 1u : 0u);
  }

  WMTCullMode EffectiveCullMode() const {
    if (!pso)
      return WMTCullModeNone;

    const auto &rast = pso->GetRasterizerDesc();
    WMTCullMode cull_mode = WMTCullModeNone;
    if (rast.CullMode == D3D12_CULL_MODE_BACK)
      cull_mode = WMTCullModeBack;
    else if (rast.CullMode == D3D12_CULL_MODE_FRONT)
      cull_mode = WMTCullModeFront;

    if (HasSwapchainRenderTarget())
      cull_mode = WMTCullModeNone;
    return cull_mode;
  }

  void ApplyRootBindings(MTLD3D12Device *device) {
    if (!render_enc_open || !pso)
      return;

    const bool stage_in_vertex_inputs = pso->UsesStageInVertexDescriptor();
    bool has_root_constants = false;
    for (uint32_t i = 0; i < kRootParameterSlotCount; i++)
      has_root_constants |= root_constant_set[i] && root_constant_sizes[i] > 0;

    root_constants_mtl_buf_offset = 0;
    root_constants_gpu_address = 0;
    if (has_root_constants) {
      root_constants_mtl_buf = MakeTransientTableSlice(
          device, root_constants_buf, sizeof(root_constants_buf),
          &root_constants_mtl_buf_offset, &root_constants_gpu_address);
      if (root_constants_mtl_buf.handle) {
        render_enc.useResource(root_constants_mtl_buf, WMTResourceUsageRead,
                               RootBindingStages());
      }
    }

    for (uint32_t i = 0; i < kRootParameterSlotCount; i++) {
      if (root_constant_set[i] && root_constant_sizes[i] > 0 &&
          root_constants_mtl_buf.handle) {
        const uint64_t root_constant_bind_offset =
            root_constants_mtl_buf_offset + root_constant_offsets[i];
        if (!stage_in_vertex_inputs) {
          SetVertexBufferTracked(root_constants_mtl_buf,
                                 root_constant_bind_offset, i);
        }
        SetFragmentBufferTracked(root_constants_mtl_buf,
                                 root_constant_bind_offset, i);
        if (!stage_in_vertex_inputs && UsesGeometryMeshPipeline()) {
          render_enc.setObjectBuffer(root_constants_mtl_buf,
                                     root_constant_bind_offset, i);
          render_enc.setMeshBuffer(root_constants_mtl_buf,
                                   root_constant_bind_offset, i);
        }
        QTRACE("ApplyRootBindings: constants idx=%u off=%u size=%u via buffer",
               i, root_constant_offsets[i], root_constant_sizes[i]);
      }

      auto root_register_and_vis = [&](D3D12_ROOT_PARAMETER_TYPE type,
                                       D3D12_SHADER_VISIBILITY *out_vis) {
        if (graphics_root_sig &&
            i < graphics_root_sig->GetParameters().size()) {
          const auto &param = graphics_root_sig->GetParameters()[i];
          if (param.type == type) {
            *out_vis =
                static_cast<D3D12_SHADER_VISIBILITY>(param.shader_visibility);
            return param.register_index;
          }
        }
        *out_vis = D3D12_SHADER_VISIBILITY_ALL;
        return i;
      };

      auto bind_root_buffer = [&](D3D12_GPU_VIRTUAL_ADDRESS address,
                                  D3D12_ROOT_PARAMETER_TYPE type,
                                  const char *label) {
        if (!address)
          return;
        auto *res = device->LookupResourceByGPUAddress(address);
        if (!res || !res->GetMTLBuffer().handle)
          return;
        D3D12_SHADER_VISIBILITY vis = D3D12_SHADER_VISIBILITY_ALL;
        uint32_t slot = root_register_and_vis(type, &vis);
        if (slot >= 31)
          return;
        uint64_t offset = address - res->GetGPUVirtualAddress();
        if (vis == D3D12_SHADER_VISIBILITY_ALL ||
            vis == D3D12_SHADER_VISIBILITY_VERTEX) {
          SetVertexBufferTracked(res->GetMTLBuffer(), offset, slot);
        }
        if (vis == D3D12_SHADER_VISIBILITY_ALL ||
            vis == D3D12_SHADER_VISIBILITY_PIXEL) {
          SetFragmentBufferTracked(res->GetMTLBuffer(), offset, slot);
        }
        if (!stage_in_vertex_inputs && UsesGeometryMeshPipeline()) {
          render_enc.setObjectBuffer(res->GetMTLBuffer(), offset, slot);
          render_enc.setMeshBuffer(res->GetMTLBuffer(), offset, slot);
        }
        render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                               RootBindingStages());
        RetainResourceMetalObjectsForCompletion(res);
        QTRACE("ApplyRootBindings: root %s param=%u -> slot=%u gpu=0x%llx",
               label, i, slot, (unsigned long long)address);
      };

      if (root_cbv_set[i]) {
        bind_root_buffer(root_cbvs[i], D3D12_ROOT_PARAMETER_TYPE_CBV, "CBV");
        if (stage_in_vertex_inputs && HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_draw_logs, 384)) {
          D3D12_SHADER_VISIBILITY cbv_vis = D3D12_SHADER_VISIBILITY_ALL;
          uint32_t cbv_slot =
              root_register_and_vis(D3D12_ROOT_PARAMETER_TYPE_CBV, &cbv_vis);
          Logger::info(str::format("M12 MSC root CBV param=", i,
                                   " slot=", cbv_slot, " vis=", (int)cbv_vis,
                                   " gpu=0x", (unsigned long long)root_cbvs[i],
                                   " set=", root_cbv_set[i]));
        }
      }
      if (root_srv_set[i])
        bind_root_buffer(root_srvs[i], D3D12_ROOT_PARAMETER_TYPE_SRV, "SRV");
      if (root_uav_set[i])
        bind_root_buffer(root_uavs[i], D3D12_ROOT_PARAMETER_TYPE_UAV, "UAV");

      auto bind_descriptor = [&](D3D12Descriptor *desc,
                                 D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                                 uint32_t shader_register,
                                 D3D12_SHADER_VISIBILITY vis) {
        if (!desc)
          return;
        if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
          if (shader_register < 4 && desc->metal_sampler.handle) {
            if (vis == D3D12_SHADER_VISIBILITY_ALL ||
                vis == D3D12_SHADER_VISIBILITY_PIXEL)
              SetFragmentSamplerTracked(desc->metal_sampler, shader_register);
            if (UsesGeometryMeshPipeline()) {
              if (vis == D3D12_SHADER_VISIBILITY_ALL ||
                  vis == D3D12_SHADER_VISIBILITY_MESH)
                render_enc.setMeshSamplerState(desc->metal_sampler,
                                               shader_register);
              if (vis == D3D12_SHADER_VISIBILITY_ALL ||
                  vis == D3D12_SHADER_VISIBILITY_AMPLIFICATION)
                render_enc.setObjectSamplerState(desc->metal_sampler,
                                                 shader_register);
            }
            QTRACE("ApplyRootBindings: table sampler s%u", shader_register);
          }
          return;
        }
        if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
          if (!desc->cbv.BufferLocation)
            return;
          uint32_t buf_slot = shader_register;
          if (buf_slot >= kD3D12M12DirectBufferSlots)
            return;
          auto *res =
              device->LookupResourceByGPUAddress(desc->cbv.BufferLocation);
          if (!res || !res->GetMTLBuffer().handle)
            return;

          uint64_t off = desc->cbv.BufferLocation - res->GetGPUVirtualAddress();
          if (vis == D3D12_SHADER_VISIBILITY_ALL ||
              vis == D3D12_SHADER_VISIBILITY_VERTEX)
            SetVertexBufferTracked(res->GetMTLBuffer(), off, buf_slot);
          if (vis == D3D12_SHADER_VISIBILITY_ALL ||
              vis == D3D12_SHADER_VISIBILITY_PIXEL)
            SetFragmentBufferTracked(res->GetMTLBuffer(), off, buf_slot);
          if (!stage_in_vertex_inputs && UsesGeometryMeshPipeline()) {
            render_enc.setObjectBuffer(res->GetMTLBuffer(), off, buf_slot);
            render_enc.setMeshBuffer(res->GetMTLBuffer(), off, buf_slot);
          }
          render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                 RootBindingStages());
          RetainResourceMetalObjectsForCompletion(res);
          if (HasSwapchainRenderTarget() &&
              (vis == D3D12_SHADER_VISIBILITY_ALL ||
               vis == D3D12_SHADER_VISIBILITY_PIXEL) &&
              TakeLogBudget(&g_swapchain_texture_binding_logs, 128)) {
            Logger::info(str::format(
                "M12 swapchain direct CBV binding root_table=", i, " reg=",
                shader_register, " vis=", ShaderVisibilityName(vis), " gpu=0x",
                std::hex, (unsigned long long)desc->cbv.BufferLocation,
                std::dec, " size=", desc->cbv.SizeInBytes, " off=", off, " ",
                ResourceSummary(res), " pso=", (void *)pso, " ",
                TracePsoShaderSummary(pso)));
          }
          QTRACE("ApplyRootBindings: table cbv reg=%u off=%llu",
                 shader_register, (unsigned long long)off);
          return;
        }
        if (!desc->resource)
          return;
        uint32_t buf_slot = shader_register;
        if (buf_slot >= kD3D12M12DirectBufferSlots)
          return;
        auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
        if (res->GetMTLBuffer().handle) {
          uint64_t off = 0;
          if (desc->cbv.BufferLocation) {
            auto *cbv_res =
                device->LookupResourceByGPUAddress(desc->cbv.BufferLocation);
            if (cbv_res)
              off = desc->cbv.BufferLocation - cbv_res->GetGPUVirtualAddress();
          }
          if (vis == D3D12_SHADER_VISIBILITY_ALL ||
              vis == D3D12_SHADER_VISIBILITY_VERTEX)
            SetVertexBufferTracked(res->GetMTLBuffer(), off, buf_slot);
          if (vis == D3D12_SHADER_VISIBILITY_ALL ||
              vis == D3D12_SHADER_VISIBILITY_PIXEL)
            SetFragmentBufferTracked(res->GetMTLBuffer(), off, buf_slot);
          if (!stage_in_vertex_inputs && UsesGeometryMeshPipeline()) {
            render_enc.setObjectBuffer(res->GetMTLBuffer(), off, buf_slot);
            render_enc.setMeshBuffer(res->GetMTLBuffer(), off, buf_slot);
          }
          WMTResourceUsage usage =
              range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                  ? (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageWrite)
                  : WMTResourceUsageRead;
          render_enc.useResource(res->GetMTLBuffer(), usage,
                                 RootBindingStages());
          RetainResourceMetalObjectsForCompletion(res);
          QTRACE("ApplyRootBindings: table buffer reg=%u type=%u off=%llu",
                 shader_register, range_type, (unsigned long long)off);
        } else if (auto tex = DescriptorTexture(desc, res);
                   tex.handle &&
                   range_type != D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
          if (vis == D3D12_SHADER_VISIBILITY_ALL ||
              vis == D3D12_SHADER_VISIBILITY_PIXEL)
            SetFragmentTextureTracked(tex, shader_register);
          if (UsesGeometryMeshPipeline()) {
            if (vis == D3D12_SHADER_VISIBILITY_ALL ||
                vis == D3D12_SHADER_VISIBILITY_MESH)
              render_enc.setMeshTexture(tex, shader_register);
            if (vis == D3D12_SHADER_VISIBILITY_ALL ||
                vis == D3D12_SHADER_VISIBILITY_AMPLIFICATION)
              render_enc.setObjectTexture(tex, shader_register);
          }
          if (HasSwapchainRenderTarget() &&
              (vis == D3D12_SHADER_VISIBILITY_ALL ||
               vis == D3D12_SHADER_VISIBILITY_PIXEL) &&
              TakeLogBudget(&g_swapchain_texture_binding_logs, 128)) {
            Logger::info(str::format(
                "M12 swapchain direct texture binding root_table=", i,
                " reg=", shader_register, " vis=", ShaderVisibilityName(vis),
                " range=", DescriptorRangeTypeName(range_type),
                " tex=", (unsigned long long)tex.handle, " ",
                DescriptorSummary(desc, range_type), " pso=", (void *)pso, " ",
                TracePsoShaderSummary(pso)));
          }
          WMTResourceUsage usage =
              range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                  ? (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageWrite)
                  : (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageSample);
          render_enc.useResource(
              tex, usage, RootBindingStages());
          RetainMTLObjectForCompletion(tex);
          QTRACE("ApplyRootBindings: table texture reg=%u type=%u tex=%llu",
                 shader_register, range_type, (unsigned long long)tex.handle);
        }
      };

      if (root_table_set[i] && desc_heap_count > 0) {
        for (uint32_t h = 0; h < desc_heap_count; h++) {
          auto *heap = static_cast<MTLD3D12DescriptorHeap *>(desc_heaps[h]);
          if (!heap)
            continue;

          if (graphics_root_sig &&
              i < graphics_root_sig->GetParameters().size()) {
            const auto &param = graphics_root_sig->GetParameters()[i];
            D3D12_SHADER_VISIBILITY table_vis =
                static_cast<D3D12_SHADER_VISIBILITY>(param.shader_visibility);
            if (param.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                !param.ranges.empty()) {
              for (const auto &range : param.ranges) {
                uint32_t max_slots =
                    range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                        ? 4
                        : 31;
                if (range.base_register >= max_slots)
                  continue;
                uint32_t count = range.num_descriptors == UINT32_MAX
                                     ? 1
                                     : range.num_descriptors;
                count =
                    std::min<uint32_t>(count, max_slots - range.base_register);
                for (uint32_t d = 0; d < count; d++) {
                  auto *desc = heap->GetDescriptorFromGPUHandle(
                      root_tables[i], range.offset_in_table + d);
                  bind_descriptor(desc, range.range_type,
                                  range.base_register + d, table_vis);
                }
              }
              continue;
            }
          }

          auto *desc = heap->GetDescriptorFromGPUHandle(root_tables[i]);
          if (!desc)
            continue;
          bind_descriptor(desc,
                          desc->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                              ? D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                              : D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                          i, D3D12_SHADER_VISIBILITY_ALL);
        }
      }
    }
  }

  void BindStaticSamplers() {
    if (!render_enc_open || !pso)
      return;
    auto *root_sig = pso ? pso->GetRootSignature() : nullptr;
    auto *dxmt_sig =
        root_sig ? static_cast<MTLD3D12RootSignature *>(root_sig) : nullptr;
    if (!dxmt_sig)
      return;
    for (uint32_t i = 0; i < dxmt_sig->GetNumStaticSamplers(); i++) {
      const auto *s = dxmt_sig->GetStaticSamplers().data() + i;
      if (!s || !s->sampler.handle)
        continue;
      uint32_t reg = s->shader_register;
      D3D12_SHADER_VISIBILITY vis =
          (D3D12_SHADER_VISIBILITY)s->shader_visibility;
      if (reg >= 16)
        continue;
      if (vis == D3D12_SHADER_VISIBILITY_ALL ||
          vis == D3D12_SHADER_VISIBILITY_PIXEL)
        SetFragmentSamplerTracked(s->sampler, reg);
    }
  }

  void BindMSCDrawParameters(MTLD3D12Device *device, uint32_t element_count,
                             uint32_t instance_count, uint32_t start_element,
                             int32_t base_vertex, uint32_t start_instance,
                             bool indexed, WMTIndexType index_type) {
    if (!render_enc_open || !pso)
      return;

    const bool stage_in = pso->UsesStageInVertexDescriptor();
    const bool vertex_pull = !stage_in;

    if (stage_in && pso->RequiresMSCStageInFunction() && !vs_arg_buf.handle) {
      uint64_t zero_ab[1] = {};
      msc_vertex_arg_buf = MakeTransientBuffer(device, sizeof(zero_ab));
      if (msc_vertex_arg_buf.handle) {
        msc_vertex_arg_buf.updateContents(0, zero_ab, sizeof(zero_ab));
        SetVertexBufferTracked(msc_vertex_arg_buf, 0, kMSCArgumentBufferSlot);
        render_enc.useResource(msc_vertex_arg_buf, WMTResourceUsageRead,
                               WMTRenderStageVertex);
      }
    }

    MSCDrawParams params = {};
    if (indexed) {
      params.drawIndexed.indexCountPerInstance = element_count;
      params.drawIndexed.instanceCount = instance_count;
      // Metal already applies D3D12 StartIndexLocation through
      // index_buffer_offset. Passing it again to the MSC stage-in helper makes
      // linked vertex fetches walk past the intended indices.
      params.drawIndexed.startIndexLocation = 0;
      params.drawIndexed.baseVertexLocation = base_vertex;
      params.drawIndexed.startInstanceLocation = start_instance;
    } else {
      params.draw.vertexCountPerInstance = element_count;
      params.draw.instanceCount = instance_count;
      // Metal's vertexStart is already reflected in [[vertex_id]].
      params.draw.startVertexLocation = 0;
      params.draw.startInstanceLocation = start_instance;
    }

    const uint16_t draw_info =
        indexed ? (uint16_t)((uint16_t)index_type + 1u) : kMSCNonIndexedDraw;

    msc_draw_args_buf = MakeTransientBuffer(device, sizeof(params));
    const uint32_t draw_args_slot =
        stage_in ? kMSCDrawArgumentsSlot : kM12VertexPullDrawArgumentsSlot;
    if (msc_draw_args_buf.handle) {
      msc_draw_args_buf.updateContents(0, &params, sizeof(params));
      SetVertexBufferTracked(msc_draw_args_buf, 0, draw_args_slot);
      render_enc.useResource(msc_draw_args_buf, WMTResourceUsageRead,
                             WMTRenderStageVertex);
    }

    msc_uniforms_buf = MakeTransientBuffer(device, sizeof(draw_info));
    const uint32_t draw_info_slot =
        stage_in ? kMSCUniformsSlot : kM12VertexPullDrawInfoSlot;
    if (msc_uniforms_buf.handle) {
      msc_uniforms_buf.updateContents(0, &draw_info, sizeof(draw_info));
      SetVertexBufferTracked(msc_uniforms_buf, 0, draw_info_slot);
      render_enc.useResource(msc_uniforms_buf, WMTResourceUsageRead,
                             WMTRenderStageVertex);
    }

    QTRACE("BindMSCDrawParameters: indexed=%u count=%u inst=%u start=%u "
           "msc_start=0 base=%d start_inst=%u index_type=%u slots=%u/%u "
           "stage_in=%u vertex_pull=%u",
           indexed ? 1u : 0u, element_count, instance_count, start_element,
           base_vertex, start_instance, (unsigned)index_type, draw_args_slot,
           draw_info_slot, stage_in ? 1u : 0u, vertex_pull ? 1u : 0u);
  }

  void ApplyVertexBuffers(MTLD3D12Device *device) {
    if (!render_enc_open)
      return;

    last_bound_vertex_buffers = 0;
    uint32_t slot_mask = pso ? pso->GetIAInputSlotMask() : 0;
    if (slot_mask) {
      if (pso && pso->UsesStageInVertexDescriptor()) {
        uint32_t bound_slots = 0;
        uint32_t table_entries = 0;
        uint32_t table_index = 0;
        const bool msc_stage_in = pso->RequiresMSCStageInFunction();
        memset(vertex_table_data, 0, sizeof(vertex_table_data));
        for (uint32_t slot = 0; slot < kVertexBufferSlotCount; slot++) {
          if (!(slot_mask & (1u << slot)))
            continue;

          auto &view = vbs[slot];
          const uint32_t table_slot_index = msc_stage_in ? slot : table_index++;
          auto *res =
              view.BufferLocation
                  ? device->LookupResourceByGPUAddress(view.BufferLocation)
                  : nullptr;
          if (res && res->GetMTLBuffer().handle) {
            uint64_t offset = view.BufferLocation - res->GetGPUVirtualAddress();
            uint32_t msc_slot = kMSCVertexBufferBindPoint + slot;
            if (!msc_stage_in) {
              SetVertexBufferTracked(res->GetMTLBuffer(), offset, msc_slot);
              if (UsesGeometryMeshPipeline())
                render_enc.setObjectBuffer(res->GetMTLBuffer(), offset,
                                           msc_slot);
            }
            if (table_slot_index < kVertexBufferSlotCount) {
              vertex_table_data[table_slot_index].buffer_handle =
                  view.BufferLocation;
              vertex_table_data[table_slot_index].stride = view.StrideInBytes;
              vertex_table_data[table_slot_index].length = view.SizeInBytes;
              table_entries =
                  std::max<uint32_t>(table_entries, table_slot_index + 1);
            }
            render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                   VertexInputStages());
            RetainResourceMetalObjectsForCompletion(res);
            bound_slots++;
            QTRACE(
                "ApplyVertexBuffers: stage_in slot=%u->msc_slot=%u gpu=0x%llx "
                "offset=%llu size=%u stride=%u table_index=%u msc=%u",
                slot, msc_slot, (unsigned long long)view.BufferLocation,
                (unsigned long long)offset, view.SizeInBytes,
                view.StrideInBytes, table_slot_index, msc_stage_in ? 1u : 0u);
          } else {
            QTRACE("ApplyVertexBuffers: stage_in slot=%u gpu=0x%llx "
                   "unresolved table_index=%u",
                   slot, (unsigned long long)view.BufferLocation,
                   table_slot_index);
          }
          if (table_slot_index < kVertexBufferSlotCount)
            table_entries =
                std::max<uint32_t>(table_entries, table_slot_index + 1);
        }
        if (table_entries > 0) {
          vertex_table_buf =
              MakeTransientBuffer(device, sizeof(vertex_table_data));
          if (vertex_table_buf.handle) {
            vertex_table_buf.updateContents(0, vertex_table_data,
                                            sizeof(vertex_table_data));
            const uint32_t table_slot = pso->RequiresMSCStageInFunction()
                                            ? kMSCVertexBufferBindPoint
                                            : kVertexBufferTableSlot;
            SetVertexBufferTracked(vertex_table_buf, 0, table_slot);
            SetVertexBufferTracked(vertex_table_buf, 0, kVertexBufferTableSlot);
            if (UsesGeometryMeshPipeline()) {
              render_enc.setObjectBuffer(vertex_table_buf, 0, table_slot);
              render_enc.setObjectBuffer(vertex_table_buf, 0,
                                         kVertexBufferTableSlot);
            }
            render_enc.useResource(vertex_table_buf, WMTResourceUsageRead,
                                   VertexInputStages());
            QTRACE("ApplyVertexBuffers: stage_in vertex table slot=%u "
                   "legacy_slot=%u "
                   "mask=0x%x entries=%u bound=%u",
                   table_slot, kVertexBufferTableSlot, slot_mask, table_entries,
                   bound_slots);
          }
        }
        last_vertex_table_summary =
            str::format("vb_stage_in mask=0x", std::hex, slot_mask, std::dec,
                        " entries=", table_entries, " bound=", bound_slots,
                        " msc=", msc_stage_in ? 1u : 0u);
        last_bound_vertex_buffers = bound_slots;
        if (HasSwapchainRenderTarget() &&
            TakeLogBudget(&g_swapchain_stage_in_vb_logs, 64)) {
          Logger::info(str::format("M12 swapchain stage_in vertex buffers "
                                   "mask=",
                                   slot_mask, " entries=", table_entries,
                                   " bound=", bound_slots, " pso=", (void *)pso,
                                   " ", TracePsoShaderSummary(pso)));
        }
        return;
      }

      memset(vertex_table_data, 0, sizeof(vertex_table_data));
      uint32_t table_entries = 0;
      bool table_bound[kVertexBufferSlotCount] = {};
      if (pso) {
        for (const auto &input : pso->GetIAInputElements()) {
          if (input.system_value ||
              input.table_indexing_mode !=
                  D3D12VertexTableIndexingMode::CompactBySlotMask ||
              input.input_slot >= kVertexBufferSlotCount ||
              input.table_index >= kVertexBufferSlotCount ||
              table_bound[input.table_index])
            continue;

          auto &view = vbs[input.input_slot];
          auto *res =
              view.BufferLocation
                  ? device->LookupResourceByGPUAddress(view.BufferLocation)
                  : nullptr;
          if (res && res->GetMTLBuffer().handle) {
            uint64_t offset = view.BufferLocation - res->GetGPUVirtualAddress();
            vertex_table_data[input.table_index].buffer_handle =
                view.BufferLocation;
            vertex_table_data[input.table_index].stride = view.StrideInBytes;
            vertex_table_data[input.table_index].length = view.SizeInBytes;
            SetVertexBufferTracked(res->GetMTLBuffer(), offset,
                                   input.table_index);
            render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                   VertexInputStages());
            RetainResourceMetalObjectsForCompletion(res);
            table_bound[input.table_index] = true;
            table_entries =
                std::max<uint32_t>(table_entries, input.table_index + 1);
            QTRACE("ApplyVertexBuffers: table[%u]<-slot=%u reg=%u gpu=0x%llx "
                   "offset=%llu size=%u stride=%u fmt=%u",
                   input.table_index, input.input_slot, input.shader_register,
                   (unsigned long long)view.BufferLocation,
                   (unsigned long long)offset, view.SizeInBytes,
                   view.StrideInBytes, (unsigned)input.dxgi_format);
          } else {
            QTRACE("ApplyVertexBuffers: table[%u]<-slot=%u reg=%u "
                   "unresolved gpu=0x%llx size=%u stride=%u fmt=%u",
                   input.table_index, input.input_slot, input.shader_register,
                   (unsigned long long)view.BufferLocation, view.SizeInBytes,
                   view.StrideInBytes, (unsigned)input.dxgi_format);
          }
        }
      }

      vertex_table_buf = MakeTransientBuffer(device, sizeof(vertex_table_data));
      if (vertex_table_buf.handle) {
        vertex_table_buf.updateContents(0, vertex_table_data,
                                        sizeof(vertex_table_data));
        SetVertexBufferTracked(vertex_table_buf, 0, kVertexBufferTableSlot);
        render_enc.useResource(vertex_table_buf, WMTResourceUsageRead,
                               WMTRenderStageVertex);
        QTRACE("ApplyVertexBuffers: bound IA vertex table slot=%u mask=0x%x "
               "entries=%u",
               kVertexBufferTableSlot, slot_mask, table_entries);
      }
      last_vertex_table_summary =
          str::format("vb_table mask=0x", std::hex, slot_mask, std::dec,
                      " entries=", table_entries);
      last_bound_vertex_buffers = table_entries;
      return;
    }

    uint32_t raw_bound_slots = 0;
    for (uint32_t i = 0; i < kVertexBufferSlotCount; i++) {
      if (vbs[i].BufferLocation) {
        auto *res = device->LookupResourceByGPUAddress(vbs[i].BufferLocation);
        if (res && res->GetMTLBuffer().handle) {
          uint64_t offset = vbs[i].BufferLocation - res->GetGPUVirtualAddress();
          QTRACE("ApplyVertexBuffers: slot=%u gpu=0x%llx offset=%llu size=%u "
                 "stride=%u",
                 i, (unsigned long long)vbs[i].BufferLocation,
                 (unsigned long long)offset, vbs[i].SizeInBytes,
                 vbs[i].StrideInBytes);
          SetVertexBufferTracked(res->GetMTLBuffer(), offset, i);
          if (UsesGeometryMeshPipeline())
            render_enc.setObjectBuffer(res->GetMTLBuffer(), offset, i);
          render_enc.useResource(res->GetMTLBuffer(), WMTResourceUsageRead,
                                 VertexInputStages());
          RetainResourceMetalObjectsForCompletion(res);
          raw_bound_slots++;
        } else {
          QTRACE("ApplyVertexBuffers: slot=%u gpu=0x%llx unresolved", i,
                 (unsigned long long)vbs[i].BufferLocation);
        }
      }
    }
    last_vertex_table_summary =
        str::format("vb_raw bound=", raw_bound_slots, " mask=0x", std::hex,
                    slot_mask, std::dec);
    last_bound_vertex_buffers = raw_bound_slots;
  }
};

WMTIndexType DXGIToWMTIndexFormat(DXGI_FORMAT fmt) {
  switch (fmt) {
  case DXGI_FORMAT_R16_UINT:
    return WMTIndexTypeUInt16;
  case DXGI_FORMAT_R32_UINT:
    return WMTIndexTypeUInt32;
  default:
    return WMTIndexTypeUInt16;
  }
}

static uint32_t SubresourceMipLevel(const D3D12_RESOURCE_DESC &desc,
                                    uint32_t subresource) {
  uint32_t mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return subresource % mip_levels;
}

static uint32_t SubresourceArraySlice(const D3D12_RESOURCE_DESC &desc,
                                      uint32_t subresource) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 0;
  uint32_t mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return subresource / mip_levels;
}

static uint32_t MipSize(uint64_t base, uint32_t mip) {
  uint64_t size = base >> mip;
  return (uint32_t)(size ? size : 1);
}

static uint32_t FormatBlockExtent(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return 4;
  default:
    return 1;
  }
}

static uint32_t FormatBytesPerBlock(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return 16;
  case DXGI_FORMAT_R32G32B32_TYPELESS:
  case DXGI_FORMAT_R32G32B32_FLOAT:
  case DXGI_FORMAT_R32G32B32_UINT:
  case DXGI_FORMAT_R32G32B32_SINT:
    return 12;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SNORM:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    return 8;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SNORM:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SNORM:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R32_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    return 4;
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SNORM:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R16_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_D16_UNORM:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SNORM:
  case DXGI_FORMAT_R16_SINT:
    return 2;
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SNORM:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_A8_UNORM:
    return 1;
  case DXGI_FORMAT_BC1_TYPELESS:
  case DXGI_FORMAT_BC1_UNORM:
  case DXGI_FORMAT_BC1_UNORM_SRGB:
  case DXGI_FORMAT_BC4_TYPELESS:
  case DXGI_FORMAT_BC4_UNORM:
  case DXGI_FORMAT_BC4_SNORM:
    return 8;
  case DXGI_FORMAT_BC2_TYPELESS:
  case DXGI_FORMAT_BC2_UNORM:
  case DXGI_FORMAT_BC2_UNORM_SRGB:
  case DXGI_FORMAT_BC3_TYPELESS:
  case DXGI_FORMAT_BC3_UNORM:
  case DXGI_FORMAT_BC3_UNORM_SRGB:
  case DXGI_FORMAT_BC5_TYPELESS:
  case DXGI_FORMAT_BC5_UNORM:
  case DXGI_FORMAT_BC5_SNORM:
  case DXGI_FORMAT_BC6H_TYPELESS:
  case DXGI_FORMAT_BC6H_UF16:
  case DXGI_FORMAT_BC6H_SF16:
  case DXGI_FORMAT_BC7_TYPELESS:
  case DXGI_FORMAT_BC7_UNORM:
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return 16;
  default:
    return 4;
  }
}

static uint64_t FootprintRows(uint32_t height, DXGI_FORMAT format) {
  uint32_t block = FormatBlockExtent(format);
  return std::max<uint32_t>(1, (height + block - 1) / block);
}

static uint64_t FootprintOffset(uint64_t base_offset, uint32_t row_pitch,
                                uint32_t footprint_height, DXGI_FORMAT format,
                                uint32_t x, uint32_t y, uint32_t z) {
  uint32_t block = FormatBlockExtent(format);
  uint32_t bytes_per_block = FormatBytesPerBlock(format);
  uint64_t rows_per_image =
      FootprintRows(footprint_height ? footprint_height : 1, format);
  return base_offset + uint64_t(z) * uint64_t(row_pitch) * rows_per_image +
         uint64_t(y / block) * uint64_t(row_pitch) +
         uint64_t(x / block) * uint64_t(bytes_per_block);
}

struct PreparedRayShaderTable {
  WMT::Reference<WMT::Buffer> buffer;
  uint64_t gpu_address = 0;
  uint64_t size_in_bytes = 0;
  uint64_t stride_in_bytes = 0;
  bool local_descriptor_table_seen = false;
};

static MTLD3D12DescriptorHeap *FindDescriptorHeapForGPUHandle(
    ReplayState &st, D3D12_GPU_DESCRIPTOR_HANDLE handle,
    uint32_t *descriptor_index) {
  if (descriptor_index)
    *descriptor_index = UINT32_MAX;
  for (uint32_t i = 0; i < st.desc_heap_count; i++) {
    auto *heap = static_cast<MTLD3D12DescriptorHeap *>(st.desc_heaps[i]);
    if (!heap || !heap->IsShaderVisible())
      continue;
    const uint32_t index = heap->GetDescriptorIndexFromGPUHandle(handle);
    if (index != UINT32_MAX) {
      if (descriptor_index)
        *descriptor_index = index;
      return heap;
    }
  }
  return nullptr;
}

static bool DescriptorRangeMatchesHeap(const RootDescriptorRange &range,
                                       MTLD3D12DescriptorHeap *heap) {
  if (!heap)
    return false;
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  heap->GetDesc(&desc);
  const D3D12_DESCRIPTOR_HEAP_TYPE expected_type =
      range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
          ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
          : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  return desc.Type == expected_type;
}

static bool PatchLocalRootDescriptorTables(
    ReplayState &st, MTLD3D12Device *device, uint8_t *record,
    uint64_t record_size, const void *shader_identifier,
    std::vector<obj_handle_t> &descriptor_mirror_resources) {
  if (!record || record_size < 32 || !shader_identifier)
    return false;

  ID3D12RootSignature *local_root_signature = nullptr;
  const bool known_identifier =
      GetD3D12StateObjectShaderRecordLocalRootSignature(
          st.raytracing_state, shader_identifier, &local_root_signature);
  bool all_zero_identifier = true;
  for (uint32_t i = 0; i < 32; i++)
    all_zero_identifier &= static_cast<const uint8_t *>(shader_identifier)[i] == 0;
  if (!known_identifier)
    return all_zero_identifier;
  if (!local_root_signature)
    return true;

  auto *root_signature =
      static_cast<MTLD3D12RootSignature *>(local_root_signature);
  const auto &static_samplers = root_signature->GetStaticSamplers();
  if (!static_samplers.empty()) {
    std::vector<D3D12DescriptorTableEntry> sampler_entries(
        static_samplers.size());
    for (size_t i = 0; i < static_samplers.size(); i++) {
      const auto &sampler = static_samplers[i];
      if (!sampler.sampler.handle || !sampler.sampler_gpu_id)
        return false;
      sampler_entries[i].gpu_va = sampler.sampler_gpu_id;
      sampler_entries[i].metadata = sampler.lod_bias_bits;
      st.RetainSamplerPairForCompletion(sampler.sampler,
                                        sampler.sampler_cube);
    }
    uint64_t sampler_table_gpu = 0;
    auto sampler_table = st.MakeTransientBuffer(
        device,
        std::max<uint64_t>(256, sampler_entries.size() *
                                    sizeof(D3D12DescriptorTableEntry)),
        &sampler_table_gpu);
    if (!sampler_table.handle || !sampler_table_gpu)
      return false;
    sampler_table.updateContents(
        0, sampler_entries.data(),
        sampler_entries.size() * sizeof(D3D12DescriptorTableEntry));
    memcpy(record + 16, &sampler_table_gpu, sizeof(sampler_table_gpu));
    if (std::find(descriptor_mirror_resources.begin(),
                  descriptor_mirror_resources.end(), sampler_table.handle) ==
        descriptor_mirror_resources.end())
      descriptor_mirror_resources.push_back(sampler_table.handle);
  }
  uint64_t local_offset = 0;
  for (const auto &parameter : root_signature->GetParameters()) {
    if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      const uint64_t byte_count = uint64_t(parameter.num_32bit_values) * 4;
      if (local_offset > record_size - 32 ||
          byte_count > record_size - 32 - local_offset)
        return false;
      local_offset += byte_count;
      continue;
    }

    local_offset = (local_offset + 7) & ~uint64_t(7);
    if (local_offset > record_size - 32 ||
        sizeof(uint64_t) > record_size - 32 - local_offset)
      return false;
    uint64_t *argument = reinterpret_cast<uint64_t *>(record + 32 + local_offset);
    if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      const uint64_t original_handle = *argument;
      if (original_handle) {
        uint32_t descriptor_index = UINT32_MAX;
        auto *heap = FindDescriptorHeapForGPUHandle(
            st, D3D12_GPU_DESCRIPTOR_HANDLE{original_handle},
            &descriptor_index);
        if (!heap || !heap->GetShaderVisibleGPUAddress(descriptor_index))
          return false;
        for (const auto &range : parameter.ranges) {
          if (!DescriptorRangeMatchesHeap(range, heap))
            return false;
          const uint32_t count = range.num_descriptors == UINT32_MAX
                                     ? 1u
                                     : range.num_descriptors;
          for (uint32_t i = 0; i < count; i++) {
            if (range.offset_in_table > UINT32_MAX - i)
              return false;
            auto *descriptor = heap->GetDescriptorFromGPUHandle(
                D3D12_GPU_DESCRIPTOR_HANDLE{original_handle},
                range.offset_in_table + i);
            if (!descriptor || descriptor->range_type != range.range_type)
              return false;
            if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
              if (!descriptor->metal_sampler.handle)
                return false;
              st.RetainSamplerPairForCompletion(
                  descriptor->metal_sampler, descriptor->metal_sampler_cube);
              continue;
            }
            auto *resource = descriptor->resource
                                 ? static_cast<MTLD3D12Resource *>(
                                       descriptor->resource)
                                 : nullptr;
            if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                !resource && descriptor->cbv.BufferLocation)
              resource = device->LookupResourceByGPUAddress(
                  descriptor->cbv.BufferLocation);
            if (!resource)
              return false;
            st.RetainResourceMetalObjectsForCompletion(resource);
            auto buffer = resource->GetMTLBuffer();
            auto texture = resource->GetMTLTexture();
            if (buffer.handle &&
                std::find(descriptor_mirror_resources.begin(),
                          descriptor_mirror_resources.end(),
                          buffer.handle) == descriptor_mirror_resources.end())
              descriptor_mirror_resources.push_back(buffer.handle);
            if (texture.handle &&
                std::find(descriptor_mirror_resources.begin(),
                          descriptor_mirror_resources.end(),
                          texture.handle) == descriptor_mirror_resources.end())
              descriptor_mirror_resources.push_back(texture.handle);
          }
        }
        *argument = heap->GetShaderVisibleGPUAddress(descriptor_index);
        const auto mirror = heap->GetShaderVisibleBuffer();
        if (!mirror.handle)
          return false;
        if (std::find(descriptor_mirror_resources.begin(),
                      descriptor_mirror_resources.end(),
                      mirror.handle) == descriptor_mirror_resources.end())
          descriptor_mirror_resources.push_back(mirror.handle);
        st.RetainMTLObjectForCompletion(mirror);
      }
    } else if (parameter.type == D3D12_ROOT_PARAMETER_TYPE_CBV ||
               parameter.type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
               parameter.type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
      const uint64_t resource_address = *argument;
      auto *resource = device->LookupResourceByGPUAddress(resource_address);
      if (resource) {
        st.RetainResourceMetalObjectsForCompletion(resource);
        auto buffer = resource->GetMTLBuffer();
        if (buffer.handle &&
            std::find(descriptor_mirror_resources.begin(),
                      descriptor_mirror_resources.end(), buffer.handle) ==
                descriptor_mirror_resources.end())
          descriptor_mirror_resources.push_back(buffer.handle);
        auto texture = resource->GetMTLTexture();
        if (texture.handle &&
            std::find(descriptor_mirror_resources.begin(),
                      descriptor_mirror_resources.end(), texture.handle) ==
                descriptor_mirror_resources.end())
          descriptor_mirror_resources.push_back(texture.handle);
      }
    }
    local_offset += sizeof(uint64_t);
  }
  return true;
}

// DXR shader records carry D3D12 GPU descriptor handles for local tables.
// DXMT's Win32 handles intentionally remain process-local CPU handles, so the
// dispatch path copies records into a transient buffer and rewrites those
// handles to the corresponding Metal Shader Converter descriptor-mirror GPU
// addresses immediately before encoding the ray dispatch.
static bool PrepareRayShaderTable(
    ReplayState &st, MTLD3D12Device *device, uint64_t start_address,
    uint64_t size_in_bytes, uint64_t stride_in_bytes, bool raygen_table,
    const char *label, PreparedRayShaderTable &prepared,
    std::vector<obj_handle_t> &descriptor_mirror_resources) {
  prepared = {};
  prepared.size_in_bytes = size_in_bytes;
  prepared.stride_in_bytes = stride_in_bytes;
  if (!start_address || !size_in_bytes)
    return true;
  const uint64_t effective_stride = raygen_table ? size_in_bytes : stride_in_bytes;
  if (effective_stride < 32 || effective_stride > size_in_bytes)
    return false;
  auto *resource = device->LookupResourceByGPUAddress(start_address);
  if (!resource || !resource->GetMTLBuffer().handle)
    return false;
  const uint64_t resource_offset =
      start_address - resource->GetGPUVirtualAddress();
  const uint64_t resource_size = resource->GetBufferByteLength();
  if (resource_offset > resource_size ||
      size_in_bytes > resource_size - resource_offset)
    return false;

  void *mapped = nullptr;
  if (FAILED(resource->Map(0, nullptr, &mapped)) || !mapped)
    return false;
  std::vector<uint8_t> data(size_in_bytes);
  std::memcpy(data.data(), static_cast<const uint8_t *>(mapped) + resource_offset,
              size_in_bytes);
  resource->Unmap(0, nullptr);

  bool local_table_seen = false;
  for (uint64_t offset = 0; offset + 32 <= size_in_bytes;
       offset += effective_stride) {
    const uint64_t record_size =
        std::min<uint64_t>(effective_stride, size_in_bytes - offset);
    bool record_has_local_table = false;
    // A record with an all-zero identifier is unused; preserve it without
    // trying to infer a local root signature.
    bool all_zero_identifier = true;
    for (uint32_t i = 0; i < 32; i++)
      all_zero_identifier &= data[offset + i] == 0;
    uint8_t original_identifier[32] = {};
    if (!all_zero_identifier) {
      std::memcpy(original_identifier, data.data() + offset,
                  sizeof(original_identifier));
      if (!PatchLocalRootDescriptorTables(
              st, device, data.data() + offset, record_size,
              original_identifier, descriptor_mirror_resources))
        return false;
      auto *local_root = static_cast<ID3D12RootSignature *>(nullptr);
      if (GetD3D12StateObjectShaderRecordLocalRootSignature(
              st.raytracing_state, original_identifier, &local_root) &&
          local_root) {
        auto *root = static_cast<MTLD3D12RootSignature *>(local_root);
        for (const auto &parameter : root->GetParameters())
          record_has_local_table |=
              parameter.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        record_has_local_table |= !root->GetStaticSamplers().empty();
      }
    }
    local_table_seen |= record_has_local_table;
  }
  if (!local_table_seen)
    return true;

  prepared.buffer = st.MakeTransientBuffer(device, size_in_bytes,
                                           &prepared.gpu_address);
  if (!prepared.buffer.handle || !prepared.gpu_address)
    return false;
  prepared.buffer.updateContents(0, data.data(), data.size());
  prepared.local_descriptor_table_seen = true;
  QTRACE("%s: copied shader table start=0x%llx size=%llu stride=%llu gpu=0x%llx",
         label ? label : "ray_table", (unsigned long long)start_address,
         (unsigned long long)size_in_bytes,
         (unsigned long long)effective_stride,
         (unsigned long long)prepared.gpu_address);
  return true;
}

static bool ReplayRaytracingDispatch(
    ReplayState &st, MTLD3D12Device *device, WMT::CommandBuffer cmdbuf,
    const D3D12_DISPATCH_RAYS_DESC &desc) {
  if (!st.raytracing_compute_pso.handle ||
      !st.raytracing_visible_function_table.handle || st.desc_heap_count == 0)
    return false;

  auto *root_signature = st.compute_root_sig;
  if (!root_signature)
    root_signature = static_cast<MTLD3D12RootSignature *>(
        GetD3D12StateObjectGlobalRootSignature(st.raytracing_state));
  if (!root_signature)
    return false;

  uint32_t root_index = ~0u;
  uint32_t uav_descriptor_offset = 0;
  if (!root_signature->FindDescriptorTableRange(
          D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 0, &root_index,
          &uav_descriptor_offset) ||
      root_index >= ReplayState::kRootParameterSlotCount ||
      !(st.comp_table_set[root_index] || st.root_table_set[root_index]))
    return false;
  D3D12_GPU_DESCRIPTOR_HANDLE table_handle = st.comp_table_set[root_index]
                                                   ? st.comp_tables[root_index]
                                                   : st.root_tables[root_index];
  uint32_t acceleration_root_index = ~0u;
  uint32_t acceleration_descriptor_offset = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE acceleration_table_handle = {};
  if (root_signature->FindDescriptorTableRange(
          D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 0,
          &acceleration_root_index, &acceleration_descriptor_offset) &&
      acceleration_root_index < ReplayState::kRootParameterSlotCount &&
      (st.comp_table_set[acceleration_root_index] ||
       st.root_table_set[acceleration_root_index])) {
    acceleration_table_handle =
        st.comp_table_set[acceleration_root_index]
            ? st.comp_tables[acceleration_root_index]
            : st.root_tables[acceleration_root_index];
  }
  D3D12Descriptor *output_descriptor = nullptr;
  D3D12Descriptor *acceleration_descriptor = nullptr;
  for (uint32_t h = 0; h < st.desc_heap_count; h++) {
    auto *heap = static_cast<MTLD3D12DescriptorHeap *>(st.desc_heaps[h]);
    if (!heap)
      continue;
    output_descriptor = heap->GetDescriptorFromGPUHandle(
        table_handle, uav_descriptor_offset);
    if (acceleration_table_handle.ptr) {
      acceleration_descriptor = heap->GetDescriptorFromGPUHandle(
          acceleration_table_handle, acceleration_descriptor_offset);
    }
    if (output_descriptor)
      break;
  }
  if (!output_descriptor || !output_descriptor->resource)
    return false;
  auto *output =
      static_cast<MTLD3D12Resource *>(output_descriptor->resource);
  auto *shader_table = device->LookupResourceByGPUAddress(
      desc.RayGenerationShaderRecord.StartAddress);
  if (!output->GetMTLBuffer().handle || !shader_table ||
      !shader_table->GetMTLBuffer().handle)
    return false;

  PreparedRayShaderTable prepared_raygen;
  PreparedRayShaderTable prepared_miss;
  PreparedRayShaderTable prepared_hit_group;
  PreparedRayShaderTable prepared_callable;
  std::vector<obj_handle_t> descriptor_mirror_resources;
  if (!PrepareRayShaderTable(
          st, device, desc.RayGenerationShaderRecord.StartAddress,
          desc.RayGenerationShaderRecord.SizeInBytes,
          desc.RayGenerationShaderRecord.SizeInBytes, true, "raygen_table",
          prepared_raygen, descriptor_mirror_resources) ||
      !PrepareRayShaderTable(st, device, desc.MissShaderTable.StartAddress,
                             desc.MissShaderTable.SizeInBytes,
                             desc.MissShaderTable.StrideInBytes, false,
                             "miss_table", prepared_miss,
                             descriptor_mirror_resources) ||
      !PrepareRayShaderTable(st, device, desc.HitGroupTable.StartAddress,
                             desc.HitGroupTable.SizeInBytes,
                             desc.HitGroupTable.StrideInBytes, false,
                             "hit_group_table", prepared_hit_group,
                             descriptor_mirror_resources) ||
      !PrepareRayShaderTable(st, device, desc.CallableShaderTable.StartAddress,
                             desc.CallableShaderTable.SizeInBytes,
                             desc.CallableShaderTable.StrideInBytes, false,
                             "callable_table", prepared_callable,
                             descriptor_mirror_resources))
    return false;

  const uint64_t raygen_table_address = prepared_raygen.buffer.handle
                                            ? prepared_raygen.gpu_address
                                            : desc.RayGenerationShaderRecord.StartAddress;
  const uint64_t miss_table_address = prepared_miss.buffer.handle
                                          ? prepared_miss.gpu_address
                                          : desc.MissShaderTable.StartAddress;
  const uint64_t hit_group_table_address =
      prepared_hit_group.buffer.handle ? prepared_hit_group.gpu_address
                                       : desc.HitGroupTable.StartAddress;
  const uint64_t callable_table_address =
      prepared_callable.buffer.handle ? prepared_callable.gpu_address
                                      : desc.CallableShaderTable.StartAddress;

  uint64_t descriptor_table_data[6] = {};
  if (acceleration_descriptor && acceleration_descriptor->resource) {
    auto *acceleration = static_cast<MTLD3D12Resource *>(
        acceleration_descriptor->resource);
    descriptor_table_data[0] = acceleration->GetRaytracingHeaderGPUAddress();
    st.RetainMTLObjectForCompletion(
        acceleration->GetMTLAccelerationStructure());
    st.RetainMTLObjectForCompletion(
        acceleration->GetRaytracingHeaderBuffer());
    st.RetainMTLObjectForCompletion(
        acceleration->GetRaytracingInstanceContributionsBuffer());
  }
  descriptor_table_data[3] = output->GetGPUVirtualAddress() +
                             UAVBufferByteOffset(output_descriptor);
  descriptor_table_data[5] = UAVBufferByteLength(output_descriptor, output);

  uint64_t descriptor_table_gpu = 0;
  auto descriptor_table_buffer = st.MakeTransientBuffer(
      device, sizeof(descriptor_table_data), &descriptor_table_gpu);
  if (!descriptor_table_buffer.handle || !descriptor_table_gpu)
    return false;
  descriptor_table_buffer.updateContents(0, descriptor_table_data,
                                         sizeof(descriptor_table_data));

  uint64_t global_root_data = descriptor_table_gpu;
  uint64_t global_root_gpu = 0;
  auto global_root_buffer =
      st.MakeTransientBuffer(device, sizeof(global_root_data),
                             &global_root_gpu);
  if (!global_root_buffer.handle || !global_root_gpu)
    return false;
  global_root_buffer.updateContents(0, &global_root_data,
                                    sizeof(global_root_data));

  struct RayVirtualAddressRange {
    uint64_t start_address;
    uint64_t size_in_bytes;
  };
  struct RayVirtualAddressRangeAndStride {
    uint64_t start_address;
    uint64_t size_in_bytes;
    uint64_t stride_in_bytes;
  };
  struct RayDispatchDescriptor {
    RayVirtualAddressRange ray_generation_shader_record;
    RayVirtualAddressRangeAndStride miss_shader_table;
    RayVirtualAddressRangeAndStride hit_group_table;
    RayVirtualAddressRangeAndStride callable_shader_table;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t padding;
  };
  struct RayDispatchArgument {
    RayDispatchDescriptor dispatch;
    uint64_t global_root_signature;
    uint64_t resource_descriptor_heap;
    uint64_t sampler_descriptor_heap;
    uint64_t visible_function_table;
    uint64_t intersection_function_table;
    uint64_t intersection_function_tables;
  } dispatch_argument = {};
  dispatch_argument.dispatch.ray_generation_shader_record = {
      raygen_table_address,
      desc.RayGenerationShaderRecord.SizeInBytes};
  dispatch_argument.dispatch.miss_shader_table = {
      miss_table_address, desc.MissShaderTable.SizeInBytes,
      desc.MissShaderTable.StrideInBytes};
  dispatch_argument.dispatch.hit_group_table = {
      hit_group_table_address, desc.HitGroupTable.SizeInBytes,
      desc.HitGroupTable.StrideInBytes};
  dispatch_argument.dispatch.callable_shader_table = {
      callable_table_address,
      desc.CallableShaderTable.SizeInBytes,
      desc.CallableShaderTable.StrideInBytes};
  dispatch_argument.dispatch.width = desc.Width;
  dispatch_argument.dispatch.height = desc.Height;
  dispatch_argument.dispatch.depth = desc.Depth;
  dispatch_argument.global_root_signature = global_root_gpu;
  dispatch_argument.visible_function_table =
      st.raytracing_visible_function_table.gpuResourceID();
  if (st.raytracing_intersection_function_table.handle)
    dispatch_argument.intersection_function_table =
        st.raytracing_intersection_function_table.gpuResourceID();

  uint64_t dispatch_argument_gpu = 0;
  auto dispatch_argument_buffer = st.MakeTransientBuffer(
      device, sizeof(dispatch_argument), &dispatch_argument_gpu);
  if (!dispatch_argument_buffer.handle || !dispatch_argument_gpu)
    return false;
  dispatch_argument_buffer.updateContents(0, &dispatch_argument,
                                          sizeof(dispatch_argument));

  struct wmtcmd_compute_setpso set_pipeline = {};
  struct wmtcmd_compute_setbuffer set_dispatch_argument = {};
  struct wmtcmd_compute_useresource use_table = {};
  struct wmtcmd_compute_useresource use_intersection_table = {};
  struct wmtcmd_compute_useresource use_shader_table = {};
  struct wmtcmd_compute_useresource use_output = {};
  struct wmtcmd_compute_useresource use_descriptor_table = {};
  struct wmtcmd_compute_useresource use_global_root = {};
  struct wmtcmd_compute_useresource use_dispatch_argument = {};
  struct wmtcmd_compute_dispatch dispatch = {};
  set_pipeline.type = WMTComputeCommandSetPSO;
  set_pipeline.pso = st.raytracing_compute_pso.handle;
  set_pipeline.threadgroup_size = {1, 1, 1};
  set_pipeline.next.set(&set_dispatch_argument);
  set_dispatch_argument.type = WMTComputeCommandSetBuffer;
  set_dispatch_argument.buffer = dispatch_argument_buffer.handle;
  set_dispatch_argument.offset = 0;
  set_dispatch_argument.index = 3;
  set_dispatch_argument.next.set(&use_table);
  use_table.type = WMTComputeCommandUseResource;
  use_table.resource = st.raytracing_visible_function_table.handle;
  use_table.usage = WMTResourceUsageRead;
  use_table.next.set(st.raytracing_intersection_function_table.handle
                         ? &use_intersection_table
                         : &use_shader_table);
  use_intersection_table.type = WMTComputeCommandUseResource;
  use_intersection_table.resource =
      st.raytracing_intersection_function_table.handle;
  use_intersection_table.usage = WMTResourceUsageRead;
  use_intersection_table.next.set(&use_shader_table);
  use_shader_table.type = WMTComputeCommandUseResource;
  use_shader_table.resource = prepared_raygen.buffer.handle
                                  ? prepared_raygen.buffer.handle
                                  : shader_table->GetMTLBuffer().handle;
  use_shader_table.usage = WMTResourceUsageRead;
  std::vector<obj_handle_t> supplemental_ray_resources;
  auto append_ray_resource = [&supplemental_ray_resources, &st](obj_handle_t handle) {
    if (!handle ||
        std::find(supplemental_ray_resources.begin(),
                  supplemental_ray_resources.end(), handle) !=
            supplemental_ray_resources.end())
      return;
    supplemental_ray_resources.push_back(handle);
    st.RetainMTLObjectForCompletion(handle);
  };
  auto table_resource_handle = [device, &st](uint64_t address) -> obj_handle_t {
    if (!address)
      return 0;
    auto *resource = device->LookupResourceByGPUAddress(address);
    if (!resource)
      return 0;
    st.RetainResourceMetalObjectsForCompletion(resource);
    return resource->GetMTLBuffer().handle;
  };
  append_ray_resource(
      prepared_miss.buffer.handle
          ? prepared_miss.buffer.handle
          : table_resource_handle(desc.MissShaderTable.StartAddress));
  append_ray_resource(
      prepared_hit_group.buffer.handle
          ? prepared_hit_group.buffer.handle
          : table_resource_handle(desc.HitGroupTable.StartAddress));
  append_ray_resource(
      prepared_callable.buffer.handle
          ? prepared_callable.buffer.handle
          : table_resource_handle(desc.CallableShaderTable.StartAddress));
  for (obj_handle_t handle : descriptor_mirror_resources)
    append_ray_resource(handle);
  std::vector<wmtcmd_compute_useresource> supplemental_ray_uses(
      supplemental_ray_resources.size());
  wmtcmd_compute_useresource *ray_resource_tail = &use_shader_table;
  for (size_t i = 0; i < supplemental_ray_resources.size(); i++) {
    supplemental_ray_uses[i] = {};
    supplemental_ray_uses[i].type = WMTComputeCommandUseResource;
    supplemental_ray_uses[i].resource = supplemental_ray_resources[i];
    supplemental_ray_uses[i].usage = WMTResourceUsageRead;
    ray_resource_tail->next.set(&supplemental_ray_uses[i]);
    ray_resource_tail = &supplemental_ray_uses[i];
  }
  ray_resource_tail->next.set(&use_output);
  use_output.type = WMTComputeCommandUseResource;
  use_output.resource = output->GetMTLBuffer().handle;
  use_output.usage =
      (WMTResourceUsage)(WMTResourceUsageRead | WMTResourceUsageWrite);
  use_output.next.set(&use_descriptor_table);
  use_descriptor_table.type = WMTComputeCommandUseResource;
  use_descriptor_table.resource = descriptor_table_buffer.handle;
  use_descriptor_table.usage = WMTResourceUsageRead;
  use_descriptor_table.next.set(&use_global_root);
  use_global_root.type = WMTComputeCommandUseResource;
  use_global_root.resource = global_root_buffer.handle;
  use_global_root.usage = WMTResourceUsageRead;
  use_global_root.next.set(&use_dispatch_argument);
  use_dispatch_argument.type = WMTComputeCommandUseResource;
  use_dispatch_argument.resource = dispatch_argument_buffer.handle;
  use_dispatch_argument.usage = WMTResourceUsageRead;
  use_dispatch_argument.next.set(&dispatch);
  dispatch.type = WMTComputeCommandDispatch;
  dispatch.size = {desc.Width, desc.Height, desc.Depth};
  dispatch.next.set(nullptr);

  st.CloseRenderEncoder();
  auto encoder = cmdbuf.computeCommandEncoder(false);
  ENC_CREATE("dispatch_rays", encoder.handle);
  ScopedMetalEncoderEnd encoder_guard{encoder, "dispatch_rays"};
  if (!encoder.handle || !encoder.encodeCommands(
                             reinterpret_cast<const wmtcmd_compute_nop *>(
                                 &set_pipeline)))
    return false;
  st.RetainMTLObjectForCompletion(st.raytracing_compute_pso);
  st.RetainMTLObjectForCompletion(st.raytracing_visible_function_table);
  if (st.raytracing_intersection_function_table.handle)
    st.RetainMTLObjectForCompletion(
        st.raytracing_intersection_function_table);
  st.RetainResourceMetalObjectsForCompletion(output);
  st.RetainResourceMetalObjectsForCompletion(shader_table);
  QTRACE("DispatchRays encoded dimensions=%ux%ux%u dispatch_arg=0x%llx "
         "grs=0x%llx table_id=0x%llx",
         desc.Width, desc.Height, desc.Depth,
         (unsigned long long)dispatch_argument_gpu,
         (unsigned long long)global_root_gpu,
         (unsigned long long)dispatch_argument.visible_function_table);
  return true;
}

static void ReplayComputeDispatch(ReplayState &st, MTLD3D12Device *device,
                                  WMT::CommandBuffer cmdbuf, uint32_t x,
                                  uint32_t y, uint32_t z,
                                  const char *trace_prefix) {
  st.AddComputeFaultBreadcrumb(trace_prefix, x, y, z);
  QTRACE("%s x=%u y=%u z=%u pso=%p compiled=%d compute=%d heaps=%u stage=%s "
         "detail=%s",
         trace_prefix, x, y, z, (void *)st.pso,
         st.pso ? st.pso->IsCompiled() : 0, st.pso ? st.pso->IsCompute() : 0,
         st.desc_heap_count, TraceCompileFailureStage(st.pso),
         TraceCompileFailureDetail(st.pso));
  if (!(st.pso && st.pso->IsCompute())) {
    QTRACE("%s SKIPPED no compute PSO pso=%p compute=%d", trace_prefix,
           (void *)st.pso, st.pso ? st.pso->IsCompute() : 0);
    return;
  }
  if (!st.pso->IsCompiled()) {
    Logger::info(str::format(
        "M12 compute dispatch first-use compile pso=", (void *)st.pso,
        " tg=", st.pso->GetThreadgroupSize().width, "x",
        st.pso->GetThreadgroupSize().height, "x",
        st.pso->GetThreadgroupSize().depth, " dispatch=", x, "x", y, "x", z,
        " stage=", TraceCompileFailureStage(st.pso),
        " detail=", TraceCompileFailureDetail(st.pso)));
    bool compiled = st.pso->EnsureCompiled();
    if (!compiled) {
      Logger::err(str::format(
          "M12 COMPUTE PSO FAILURE pso=", (void *)st.pso, " dispatch=", x, "x",
          y, "x", z, " root_sig=", (void *)st.pso->GetRootSignature(),
          " tg=", st.pso->GetThreadgroupSize().width, "x",
          st.pso->GetThreadgroupSize().height, "x",
          st.pso->GetThreadgroupSize().depth, " heaps=", st.desc_heap_count,
          " stage=", TraceCompileFailureStage(st.pso),
          " metal_error=", TraceCompileFailureDetail(st.pso),
          " cs_hash=", st.pso->GetCSReflection().NumArguments,
          " cs_args=", st.pso->GetCSArguments().size(),
          " cs_cb=", st.pso->GetCSConstantBuffers().size()));
      return;
    }
  }
  if (!st.pso->GetComputePSO().handle) {
    QTRACE("%s SKIPPED no Metal compute PSO handle pso=%p stage=%s detail=%s",
           trace_prefix, (void *)st.pso, TraceCompileFailureStage(st.pso),
           TraceCompileFailureDetail(st.pso));
    return;
  }

  uint8_t cmd_buf[8192];
  uint8_t *cmd_ptr = cmd_buf;
  wmtcmd_compute_nop *chain_head = nullptr;
  wmtcmd_base *chain_tail = nullptr;
  bool compute_cmd_overflow = false;
  uint32_t compute_cmd_count = 0;
  uint64_t bound_compute_buffer_slots = 0;
  uint64_t bound_compute_texture_slots = 0;
  uint64_t bound_compute_sampler_slots = 0;
  uint64_t fallback_compute_buffer_slots = 0;
  uint64_t fallback_compute_texture_slots = 0;
  uint64_t fallback_compute_sampler_slots = 0;

  auto append_cmd = [&](void *data, size_t sz) -> wmtcmd_base * {
    if (cmd_ptr + sz > cmd_buf + sizeof(cmd_buf)) {
      compute_cmd_overflow = true;
      return nullptr;
    }
    auto *c = (wmtcmd_base *)cmd_ptr;
    memcpy(cmd_ptr, data, sz);
    cmd_ptr += sz;
    c->next.set(nullptr);
    if (chain_tail)
      chain_tail->next.set(c);
    else
      chain_head = (wmtcmd_compute_nop *)c;
    chain_tail = c;
    compute_cmd_count++;
    return c;
  };

  auto mark_compute_buffer = [&](uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_compute_buffer_slots |= 1ull << slot;
    if (fallback)
      fallback_compute_buffer_slots |= 1ull << slot;
  };
  auto mark_compute_texture = [&](uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_compute_texture_slots |= 1ull << slot;
    if (fallback)
      fallback_compute_texture_slots |= 1ull << slot;
  };
  auto mark_compute_sampler = [&](uint32_t slot, bool fallback = false) {
    if (slot >= 64)
      return;
    bound_compute_sampler_slots |= 1ull << slot;
    if (fallback)
      fallback_compute_sampler_slots |= 1ull << slot;
  };
  auto append_compute_setbuffer = [&](obj_handle_t buffer, uint64_t offset,
                                      uint32_t index,
                                      bool fallback = false) -> bool {
    if (!buffer || index > 0xffu)
      return false;
    struct wmtcmd_compute_setbuffer sbuf = {};
    sbuf.type = WMTComputeCommandSetBuffer;
    sbuf.buffer = buffer;
    sbuf.offset = offset;
    sbuf.index = index;
    if (!append_cmd(&sbuf, sizeof(sbuf)))
      return false;
    st.RetainMTLObjectForCompletion(buffer);
    mark_compute_buffer(index, fallback);
    return true;
  };
  auto append_compute_settexture = [&](obj_handle_t texture, uint32_t index,
                                       bool fallback = false) -> bool {
    if (!texture || index > 0xffu)
      return false;
    struct wmtcmd_compute_settexture stex = {};
    stex.type = WMTComputeCommandSetTexture;
    stex.texture = texture;
    stex.index = index;
    if (!append_cmd(&stex, sizeof(stex)))
      return false;
    st.RetainMTLObjectForCompletion(texture);
    mark_compute_texture(index, fallback);
    return true;
  };
  auto append_compute_setsampler = [&](obj_handle_t sampler, uint32_t index,
                                       bool fallback = false) -> bool {
    if (!sampler || index > 0xffu)
      return false;
    struct wmtcmd_compute_setsamplerstate ssamp = {};
    ssamp.type = WMTComputeCommandSetSamplerState;
    ssamp.sampler = sampler;
    ssamp.index = index;
    if (!append_cmd(&ssamp, sizeof(ssamp)))
      return false;
    st.RetainMTLObjectForCompletion(sampler);
    mark_compute_sampler(index, fallback);
    return true;
  };
  auto append_compute_useresource = [&](obj_handle_t resource,
                                        WMTResourceUsage usage) -> bool {
    if (!resource)
      return false;
    struct wmtcmd_compute_useresource use = {};
    use.type = WMTComputeCommandUseResource;
    use.resource = resource;
    use.usage = usage;
    if (!append_cmd(&use, sizeof(use)))
      return false;
    st.RetainMTLObjectForCompletion(resource);
    return true;
  };

  struct wmtcmd_compute_setpso setpso = {};
  setpso.type = WMTComputeCommandSetPSO;
  setpso.pso = st.pso->GetComputePSO();
  setpso.threadgroup_size = st.pso->GetThreadgroupSize();
  if (append_cmd(&setpso, sizeof(setpso)))
    st.RetainMTLObjectForCompletion(st.pso->GetComputePSO());

  uint32_t comp_cb_qwords = st.BuildComputeConstantBufferTable(device);
  if (comp_cb_qwords > 0 && st.comp_cbv_table_buf.handle) {
    uint32_t bind_index = st.BindIndexOrFallback(
        st.pso->GetCSReflection().ConstanttBufferTableBindIndex,
        st.kConstantBufferTableSlot);
    append_compute_setbuffer(st.comp_cbv_table_buf.handle, 0, bind_index);
    QTRACE("%s: bound compute CBV table slot=%u qwords=%u handle=%llu",
           trace_prefix, bind_index, comp_cb_qwords,
           (unsigned long long)st.comp_cbv_table_buf.handle);
  }

  uint32_t comp_arg_qwords = st.BuildComputeArgumentBuffer(device);
  if (comp_arg_qwords > 0 && st.comp_arg_buf.handle) {
    uint32_t bind_index = st.BindIndexOrFallback(
        st.pso->GetCSReflection().ArgumentBufferBindIndex, st.kArgBufSlot);
    append_compute_setbuffer(st.comp_arg_buf.handle, 0, bind_index);
    QTRACE("%s: bound compute arg table slot=%u qwords=%u handle=%llu",
           trace_prefix, bind_index, comp_arg_qwords,
           (unsigned long long)st.comp_arg_buf.handle);
  }

  auto *compute_sig =
      st.compute_root_sig
          ? st.compute_root_sig
          : static_cast<MTLD3D12RootSignature *>(st.pso->GetRootSignature());

  bool is_uav_slot[ReplayState::kRootParameterSlotCount] = {};
  if (compute_sig) {
    auto &params = compute_sig->GetParameters();
    QTRACE("ECL UAV scan: root_sig=%p num_params=%u", (void *)compute_sig,
           (uint32_t)params.size());
    for (uint32_t p = 0;
         p < params.size() && p < ReplayState::kRootParameterSlotCount; p++) {
      QTRACE("  param[%u] type=%u range_type=%u vis=%u", p, params[p].type,
             params[p].range_type, params[p].shader_visibility);
      if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
          params[p].range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
        is_uav_slot[p] = true;
      } else if (params[p].type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        is_uav_slot[p] = true;
      }
    }
  } else {
    QTRACE("ECL UAV scan: no compute root signature available");
  }

  if (compute_sig) {
    for (uint32_t s = 0; s < 4; s++) {
      if (auto *sampler = compute_sig->FindStaticSampler(
              s, 0, D3D12_SHADER_VISIBILITY_ALL)) {
        if (!sampler->sampler.handle)
          continue;
        append_compute_setsampler(sampler->sampler.handle, s);
        QTRACE("%s: static sampler s%u", trace_prefix, s);
      }
    }
  }

  const auto &compute_reflection = st.pso->GetCSReflection();
  const bool has_compute_resource_masks =
      compute_reflection.ConstantBufferSlotMask ||
      compute_reflection.SamplerSlotMask || compute_reflection.UAVSlotMask ||
      compute_reflection.SRVSlotMaskLo || compute_reflection.SRVSlotMaskHi;
  auto compute_uses_descriptor = [&](D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                                     uint32_t shader_register) {
    if (!has_compute_resource_masks)
      return true;
    switch (range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
      return shader_register < 16 &&
             (compute_reflection.ConstantBufferSlotMask &
              (1u << shader_register));
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
      return shader_register < 16 &&
             (compute_reflection.SamplerSlotMask & (1u << shader_register));
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
      return shader_register < 64 &&
             (compute_reflection.UAVSlotMask & (1ull << shader_register));
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
      if (shader_register < 64)
        return (compute_reflection.SRVSlotMaskLo &
                (1ull << shader_register)) != 0;
      return shader_register < 128 &&
             (compute_reflection.SRVSlotMaskHi &
              (1ull << (shader_register - 64)));
    default:
      return false;
    }
  };

  for (uint32_t i = 0; i < ReplayState::kRootParameterSlotCount; i++) {
    bool const_set = st.comp_constant_set[i] || st.root_constant_set[i];
    uint32_t const_size = st.comp_constant_set[i] ? st.comp_constant_sizes[i]
                                                  : st.root_constant_sizes[i];
    uint32_t const_off = st.comp_constant_set[i] ? st.comp_constant_offsets[i]
                                                 : st.root_constant_offsets[i];
    uint8_t *const_buf =
        st.comp_constant_set[i] ? st.comp_constants_buf : st.root_constants_buf;

    bool cbv_set = st.comp_cbv_set[i] || st.root_cbv_set[i];
    D3D12_GPU_VIRTUAL_ADDRESS cbv_addr =
        st.comp_cbv_set[i] ? st.comp_cbvs[i] : st.root_cbvs[i];
    bool srv_set = st.comp_srv_set[i] || st.root_srv_set[i];
    D3D12_GPU_VIRTUAL_ADDRESS srv_addr =
        st.comp_srv_set[i] ? st.comp_srvs[i] : st.root_srvs[i];
    bool uav_set = st.comp_uav_set[i] || st.root_uav_set[i];
    D3D12_GPU_VIRTUAL_ADDRESS uav_addr =
        st.comp_uav_set[i] ? st.comp_uavs[i] : st.root_uavs[i];

    bool tbl_set = st.comp_table_set[i] || st.root_table_set[i];
    D3D12_GPU_DESCRIPTOR_HANDLE tbl_handle =
        st.comp_table_set[i] ? st.comp_tables[i] : st.root_tables[i];

    if (const_set && const_size > 0) {
      struct wmtcmd_compute_setbytes sb = {};
      sb.type = WMTComputeCommandSetBytes;
      sb.length = const_size;
      sb.index = i;
      sb.bytes.ptr = (void *)(const_buf + const_off);
      append_cmd(&sb, sizeof(sb));
    }
    auto compute_root_register = [&](D3D12_ROOT_PARAMETER_TYPE type) {
      if (compute_sig && i < compute_sig->GetParameters().size()) {
        const auto &param = compute_sig->GetParameters()[i];
        if (param.type == type)
          return param.register_index;
      }
      return i;
    };

    auto bind_compute_buffer_address = [&](D3D12_GPU_VIRTUAL_ADDRESS address,
                                           D3D12_ROOT_PARAMETER_TYPE type,
                                           bool writable, const char *label) {
      if (!address)
        return;
      auto *res = device->LookupResourceByGPUAddress(address);
      if (res && res->GetMTLBuffer().handle) {
        uint32_t slot = compute_root_register(type);
        D3D12_DESCRIPTOR_RANGE_TYPE range_type =
            type == D3D12_ROOT_PARAMETER_TYPE_CBV
                ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                : (type == D3D12_ROOT_PARAMETER_TYPE_UAV
                       ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                       : D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
        if (slot >= 31 || !compute_uses_descriptor(range_type, slot))
          return;
        append_compute_setbuffer(res->GetMTLBuffer().handle,
                                 address - res->GetGPUVirtualAddress(), slot);
        if (writable) {
          append_compute_useresource(
              res->GetMTLBuffer().handle,
              (WMTResourceUsage)(WMTResourceUsageRead | WMTResourceUsageWrite));
        }
        QTRACE("%s: root %s param=%u -> slot=%u gpu=0x%llx", trace_prefix,
               label, i, slot, (unsigned long long)address);
      }
    };

    if (cbv_set)
      bind_compute_buffer_address(cbv_addr, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                  false, "CBV");
    if (srv_set)
      bind_compute_buffer_address(srv_addr, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                  false, "SRV");
    if (uav_set)
      bind_compute_buffer_address(uav_addr, D3D12_ROOT_PARAMETER_TYPE_UAV, true,
                                  "UAV");

    auto bind_compute_descriptor = [&](D3D12Descriptor *desc,
                                       D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                                       uint32_t shader_register) {
      if (!desc || !compute_uses_descriptor(range_type, shader_register))
        return;
      if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
        if (shader_register < 4 && desc->metal_sampler.handle) {
          append_compute_setsampler(desc->metal_sampler.handle,
                                    shader_register);
          QTRACE("%s: table sampler s%u", trace_prefix, shader_register);
        }
        return;
      }
      if (!desc->resource)
        return;
      uint32_t buf_slot = shader_register;
      if (buf_slot >= 31)
        return;
      auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
      bool writable = range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV &&
          desc->srv.ViewDimension ==
              D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE &&
          res->GetMTLAccelerationStructure().handle) {
        append_compute_useresource(res->GetMTLAccelerationStructure().handle,
                                   WMTResourceUsageRead);
      } else if (res->GetMTLBuffer().handle) {
        uint64_t offset = 0;
        if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
            desc->cbv.BufferLocation) {
          auto *cbv_res =
              device->LookupResourceByGPUAddress(desc->cbv.BufferLocation);
          if (cbv_res)
            offset = desc->cbv.BufferLocation - cbv_res->GetGPUVirtualAddress();
        } else if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
          offset = desc->is_sampler_feedback ? 0 : UAVBufferByteOffset(desc);
        } else if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
          offset = SRVBufferByteOffset(desc);
        }
        append_compute_setbuffer(res->GetMTLBuffer().handle, offset, buf_slot);
        append_compute_useresource(
            res->GetMTLBuffer().handle,
            writable ? (WMTResourceUsage)(WMTResourceUsageRead |
                                          WMTResourceUsageWrite)
                     : WMTResourceUsageRead);
      } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
        append_compute_settexture(tex.handle, shader_register);
        append_compute_useresource(
            tex.handle, writable ? (WMTResourceUsage)(WMTResourceUsageRead |
                                                      WMTResourceUsageWrite)
                                 : WMTResourceUsageRead);
      }
    };

    if (tbl_set && st.desc_heap_count > 0) {
      for (uint32_t h = 0; h < st.desc_heap_count; h++) {
        auto *heap = static_cast<MTLD3D12DescriptorHeap *>(st.desc_heaps[h]);
        if (heap) {
          if (compute_sig && i < compute_sig->GetParameters().size()) {
            const auto &param = compute_sig->GetParameters()[i];
            if (param.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                !param.ranges.empty()) {
              for (const auto &range : param.ranges) {
                uint32_t max_slots =
                    range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
                        ? 4
                        : 31;
                if (range.base_register >= max_slots)
                  continue;
                uint32_t count = range.num_descriptors == UINT32_MAX
                                     ? 1
                                     : range.num_descriptors;
                count =
                    std::min<uint32_t>(count, max_slots - range.base_register);
                for (uint32_t d = 0; d < count; d++) {
                  auto *desc = heap->GetDescriptorFromGPUHandle(
                      tbl_handle, range.offset_in_table + d);
                  bind_compute_descriptor(desc, range.range_type,
                                          range.base_register + d);
                }
              }
              continue;
            }
          }

          auto *desc = heap->GetDescriptorFromGPUHandle(tbl_handle);
          QTRACE("  tbl[%u] heap=%u handle=0x%llx desc=%p res=%p", i, h,
                 (unsigned long long)tbl_handle.ptr, (void *)desc,
                 desc ? (void *)desc->resource : nullptr);
          if (desc && desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            if (res->GetMTLBuffer().handle) {
              append_compute_setbuffer(res->GetMTLBuffer().handle, 0, i);
              if (is_uav_slot[i]) {
                append_compute_useresource(
                    res->GetMTLBuffer().handle,
                    (WMTResourceUsage)(WMTResourceUsageRead |
                                       WMTResourceUsageWrite));
              }
            } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
              append_compute_settexture(tex.handle, i);
              if (is_uav_slot[i]) {
                QTRACE("  UAV UseResource tex slot=%u handle=%llu", i,
                       (unsigned long long)tex.handle);
                append_compute_useresource(
                    tex.handle, (WMTResourceUsage)(WMTResourceUsageRead |
                                                   WMTResourceUsageWrite));
              }
            }
          }
        }
      }
    }
  }

  if (compute_sig &&
      (compute_sig->GetFlags() &
       D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) &&
      st.pso->UsesDirectResourceDescriptorHeap()) {
    for (uint32_t h = 0; h < st.desc_heap_count; h++) {
      auto *heap = static_cast<MTLD3D12DescriptorHeap *>(st.desc_heaps[h]);
      if (!heap)
        continue;
      D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
      heap->GetDesc(&heap_desc);
      if (heap_desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        continue;
      const uint32_t direct_heap_limit =
          st.pso->UsesAtomic64Emulation() ? 28 : 31;
      uint32_t count = std::min<uint32_t>(heap->GetDescriptorCount(),
                                          direct_heap_limit);
      auto *descriptors = heap->GetDescriptors();
      for (uint32_t index = 0; index < count; index++) {
        auto *desc = descriptors + index;
        if (!desc->resource)
          continue;
        auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
        const bool writable =
            desc->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        if (res->GetMTLBuffer().handle) {
          uint64_t offset = 0;
          if (desc->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV)
            offset = desc->is_sampler_feedback ? 0 : UAVBufferByteOffset(desc);
          else if (desc->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
            offset = SRVBufferByteOffset(desc);
          else if (desc->range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV &&
                   desc->cbv.BufferLocation)
            offset = desc->cbv.BufferLocation - res->GetGPUVirtualAddress();
          append_compute_setbuffer(res->GetMTLBuffer().handle, offset, index);
          append_compute_useresource(
              res->GetMTLBuffer().handle,
              writable ? (WMTResourceUsage)(WMTResourceUsageRead |
                                            WMTResourceUsageWrite)
                       : WMTResourceUsageRead);
        } else if (auto tex = DescriptorTexture(desc, res); tex.handle) {
          append_compute_settexture(tex.handle, index);
          append_compute_useresource(
              tex.handle,
              writable ? (WMTResourceUsage)(WMTResourceUsageRead |
                                            WMTResourceUsageWrite)
                       : WMTResourceUsageRead);
        }
      }
      QTRACE("%s: directly indexed resource heap bound descriptors=%u",
             trace_prefix, count);
    }
  }

  int num_consts = 0, num_cbvs = 0, num_tables = 0;
  for (uint32_t i = 0; i < ReplayState::kRootParameterSlotCount; i++) {
    if ((st.comp_constant_set[i] || st.root_constant_set[i]) &&
        (st.comp_constant_sizes[i] > 0 || st.root_constant_sizes[i] > 0))
      num_consts++;
    if ((st.comp_cbv_set[i] && st.comp_cbvs[i]) ||
        (st.root_cbv_set[i] && st.root_cbvs[i]))
      num_cbvs++;
    if (st.comp_table_set[i] || st.root_table_set[i])
      num_tables++;
  }
  QTRACE("  bindings: consts=%d cbvs=%d tables=%d tg=%llux%llux%llu",
         num_consts, num_cbvs, num_tables, st.pso->GetThreadgroupSize().width,
         st.pso->GetThreadgroupSize().height,
         st.pso->GetThreadgroupSize().depth);

  QTRACE(
      "%s: compute fallback begin bound_buffers=0x%llx bound_textures=0x%llx "
      "bound_samplers=0x%llx",
      trace_prefix, (unsigned long long)bound_compute_buffer_slots,
      (unsigned long long)bound_compute_texture_slots,
      (unsigned long long)bound_compute_sampler_slots);
  if (!st.null_vertex_arg_buf.handle) {
    uint64_t zero_data[4] = {};
    QTRACE("%s: creating null compute buffer fallback", trace_prefix);
    st.null_vertex_arg_buf = st.MakeTransientBuffer(device, sizeof(zero_data));
    if (st.null_vertex_arg_buf.handle)
      st.null_vertex_arg_buf.updateContents(0, zero_data, sizeof(zero_data));
    QTRACE("%s: null compute buffer fallback handle=%llu", trace_prefix,
           (unsigned long long)st.null_vertex_arg_buf.handle);
  }
  if (st.null_vertex_arg_buf.handle) {
    uint64_t missing = D3D12DirectBindingMask(kD3D12M12DirectBufferSlots) &
                       ~bound_compute_buffer_slots;
    QTRACE("%s: compute buffer fallback missing=0x%llx", trace_prefix,
           (unsigned long long)missing);
    for (uint32_t slot = 0; slot < kD3D12M12DirectBufferSlots; slot++) {
      if (!(missing & (1ull << slot)))
        continue;
      append_compute_setbuffer(st.null_vertex_arg_buf.handle, 0, slot, true);
    }
    if (fallback_compute_buffer_slots)
      append_compute_useresource(st.null_vertex_arg_buf.handle,
                                 WMTResourceUsageRead);
  }
  if (st.EnsureNullDirectTexture(device)) {
    uint64_t missing =
        D3D12DirectBindingMask(kD3D12M12DirectComputeTextureSlots) &
        ~bound_compute_texture_slots;
    QTRACE("%s: compute texture fallback handle=%llu missing=0x%llx",
           trace_prefix, (unsigned long long)st.null_direct_texture.handle,
           (unsigned long long)missing);
    for (uint32_t slot = 0; slot < kD3D12M12DirectComputeTextureSlots; slot++) {
      if (!(missing & (1ull << slot)))
        continue;
      append_compute_settexture(st.null_direct_texture.handle, slot, true);
    }
    if (fallback_compute_texture_slots) {
      append_compute_useresource(
          st.null_direct_texture.handle,
          (WMTResourceUsage)(WMTResourceUsageRead | WMTResourceUsageSample));
    }
  }
  if (st.EnsureNullDirectSampler(device)) {
    uint64_t missing =
        D3D12DirectBindingMask(kD3D12M12DirectComputeSamplerSlots) &
        ~bound_compute_sampler_slots;
    QTRACE("%s: compute sampler fallback handle=%llu missing=0x%llx",
           trace_prefix, (unsigned long long)st.null_direct_sampler.handle,
           (unsigned long long)missing);
    for (uint32_t slot = 0; slot < kD3D12M12DirectComputeSamplerSlots; slot++) {
      if (!(missing & (1ull << slot)))
        continue;
      append_compute_setsampler(st.null_direct_sampler.handle, slot, true);
    }
  }
  if (st.pso->UsesAtomic64Emulation() &&
      st.EnsureAtomic64LockBuffer(device)) {
    append_compute_setbuffer(st.atomic64_lock_buf.handle, 0, 28);
    append_compute_useresource(
        st.atomic64_lock_buf.handle,
        (WMTResourceUsage)(WMTResourceUsageRead | WMTResourceUsageWrite));
    QTRACE("%s: atomic64 software lock buffer=%llu slot=28", trace_prefix,
           (unsigned long long)st.atomic64_lock_buf.handle);
  }
  QTRACE("%s: compute fallback complete fallback_buffers=0x%llx "
         "fallback_textures=0x%llx fallback_samplers=0x%llx",
         trace_prefix, (unsigned long long)fallback_compute_buffer_slots,
         (unsigned long long)fallback_compute_texture_slots,
         (unsigned long long)fallback_compute_sampler_slots);

  D3D12ShaderBindingCompletenessDesc compute_binding_desc = {};
  compute_binding_desc.buffer_count = kD3D12M12DirectBufferSlots;
  compute_binding_desc.texture_count = kD3D12M12DirectComputeTextureSlots;
  compute_binding_desc.sampler_count = kD3D12M12DirectComputeSamplerSlots;
  compute_binding_desc.bound_buffers = bound_compute_buffer_slots;
  compute_binding_desc.bound_textures = bound_compute_texture_slots;
  compute_binding_desc.bound_samplers = bound_compute_sampler_slots;
  compute_binding_desc.fallback_buffers = fallback_compute_buffer_slots;
  compute_binding_desc.fallback_textures = fallback_compute_texture_slots;
  compute_binding_desc.fallback_samplers = fallback_compute_sampler_slots;
  auto compute_binding_summary =
      D3D12EvaluateShaderBindingCompleteness(compute_binding_desc);

  const bool log_compute_diagnostics =
      TakeLogBudget(&g_compute_completeness_logs, 128);
  if (log_compute_diagnostics) {
    QTRACE("%s: compute completeness buffers=%u+%u/%u textures=%u+%u/%u "
           "samplers=%u+%u/%u missing=0x%llx/0x%llx/0x%llx",
           trace_prefix, compute_binding_summary.bound_buffer_count,
           compute_binding_summary.fallback_buffer_count,
           compute_binding_summary.required_buffer_count,
           compute_binding_summary.bound_texture_count,
           compute_binding_summary.fallback_texture_count,
           compute_binding_summary.required_texture_count,
           compute_binding_summary.bound_sampler_count,
           compute_binding_summary.fallback_sampler_count,
           compute_binding_summary.required_sampler_count,
           (unsigned long long)compute_binding_summary.missing_buffers,
           (unsigned long long)compute_binding_summary.missing_textures,
           (unsigned long long)compute_binding_summary.missing_samplers);
    Logger::info(str::format(
        "M12 compute completeness label=", trace_prefix,
        " pso=", (void *)st.pso, " dispatch=", x, "x", y, "x", z,
        " buffers=", compute_binding_summary.bound_buffer_count, "+",
        compute_binding_summary.fallback_buffer_count, "/",
        compute_binding_summary.required_buffer_count,
        " textures=", compute_binding_summary.bound_texture_count, "+",
        compute_binding_summary.fallback_texture_count, "/",
        compute_binding_summary.required_texture_count,
        " samplers=", compute_binding_summary.bound_sampler_count, "+",
        compute_binding_summary.fallback_sampler_count, "/",
        compute_binding_summary.required_sampler_count,
        " cs_args=", st.pso->GetCSArguments().size(),
        " cs_cb=", st.pso->GetCSConstantBuffers().size(),
        " cs_qwords=", st.pso->GetCSReflection().ArgumentTableQwords));
  }

  struct wmtcmd_compute_dispatch disp = {};
  disp.type = WMTComputeCommandDispatch;
  disp.size = {(uint64_t)x, (uint64_t)y, (uint64_t)z};
  append_cmd(&disp, sizeof(disp));

  if (compute_cmd_overflow) {
    Logger::err(str::format(
        "M12 compute command chain overflow label=", trace_prefix,
        " pso=", (void *)st.pso, " used=", (uint64_t)(cmd_ptr - cmd_buf),
        " cap=", (uint64_t)sizeof(cmd_buf), " dispatch=", x, "x", y, "x", z));
    return;
  }

  st.CloseRenderEncoder();
  auto comp = cmdbuf.computeCommandEncoder(false);
  ENC_CREATE("compute_dispatch", comp.handle);
  ScopedMetalEncoderEnd comp_guard{comp, "compute_dispatch"};
  if (!comp.handle) {
    QTRACE("%s: FAILED to create compute encoder", trace_prefix);
    return;
  }

  if (chain_head && !comp.encodeCommands(chain_head)) {
    Logger::info(str::format(
        "M12 compute encoder encode failed label=", trace_prefix,
        " pso=", (void *)st.pso, " dispatch=", x, "x", y, "x", z,
        " native_compute_resolve=failed root_sig=", (void *)compute_sig,
        " compute_pso=", (unsigned long long)st.pso->GetComputePSO().handle,
        " threadgroup=", st.pso->GetThreadgroupSize().width, "x",
        st.pso->GetThreadgroupSize().height, "x",
        st.pso->GetThreadgroupSize().depth, " cmd_count=", compute_cmd_count,
        " cbv_qwords=", comp_cb_qwords, " arg_qwords=", comp_arg_qwords,
        " cs_args=", st.pso->GetCSArguments().size(),
        " cs_cb=", st.pso->GetCSConstantBuffers().size(),
        " cs_qwords=", st.pso->GetCSReflection().ArgumentTableQwords,
        " bound_buffers=0x", std::hex,
        (unsigned long long)bound_compute_buffer_slots, " bound_textures=0x",
        (unsigned long long)bound_compute_texture_slots, " bound_samplers=0x",
        (unsigned long long)bound_compute_sampler_slots, " fallback_buffers=0x",
        (unsigned long long)fallback_compute_buffer_slots,
        " fallback_textures=0x",
        (unsigned long long)fallback_compute_texture_slots,
        " fallback_samplers=0x",
        (unsigned long long)fallback_compute_sampler_slots,
        " missing_buffers=0x",
        (unsigned long long)compute_binding_summary.missing_buffers,
        " missing_textures=0x",
        (unsigned long long)compute_binding_summary.missing_textures,
        " missing_samplers=0x",
        (unsigned long long)compute_binding_summary.missing_samplers, std::dec,
        " breadcrumbs=", st.FormatFaultBreadcrumbs()));
    EndMetalEncoder(comp, "compute_dispatch_failed");
    return;
  }
  if (chain_head && log_compute_diagnostics) {
    Logger::info(str::format(
        "M12 native_compute_resolve label=", trace_prefix,
        " implementation=d3d12_native_compute_resolver pso=", (void *)st.pso,
        " root_sig=", (void *)compute_sig, " compute_pso=",
        (unsigned long long)st.pso->GetComputePSO().handle, " dispatch=", x,
        "x", y, "x", z, " threadgroup=", st.pso->GetThreadgroupSize().width,
        "x", st.pso->GetThreadgroupSize().height, "x",
        st.pso->GetThreadgroupSize().depth, " cmd_count=", compute_cmd_count,
        " cbv_qwords=", comp_cb_qwords, " arg_qwords=", comp_arg_qwords,
        " cs_args=", st.pso->GetCSArguments().size(),
        " cs_cb=", st.pso->GetCSConstantBuffers().size(),
        " cs_qwords=", st.pso->GetCSReflection().ArgumentTableQwords,
        " bound_buffers=0x", std::hex,
        (unsigned long long)bound_compute_buffer_slots, " bound_textures=0x",
        (unsigned long long)bound_compute_texture_slots, " bound_samplers=0x",
        (unsigned long long)bound_compute_sampler_slots, " fallback_buffers=0x",
        (unsigned long long)fallback_compute_buffer_slots,
        " fallback_textures=0x",
        (unsigned long long)fallback_compute_texture_slots,
        " fallback_samplers=0x",
        (unsigned long long)fallback_compute_sampler_slots,
        " missing_buffers=0x",
        (unsigned long long)compute_binding_summary.missing_buffers,
        " missing_textures=0x",
        (unsigned long long)compute_binding_summary.missing_textures,
        " missing_samplers=0x",
        (unsigned long long)compute_binding_summary.missing_samplers,
        std::dec));
  }
  EndMetalEncoder(comp, "compute_dispatch");
}

} // anonymous namespace

static bool rt_handles_match(D3D12_CPU_DESCRIPTOR_HANDLE a,
                             D3D12_CPU_DESCRIPTOR_HANDLE b) {
  return a.ptr == b.ptr;
}

MTLD3D12CommandQueue::MTLD3D12CommandQueue(MTLD3D12Device *device,
                                           CommandQueue &queue,
                                           D3D12_COMMAND_QUEUE_DESC desc)
    : m_device(device), m_queue(queue), m_desc(desc) {
  m_device->AddRef();
  auto wmt_dev = m_device->GetDXMTDevice().device();
  m_wmt_queue = wmt_dev.newCommandQueue(1);
  m_barrier_event = wmt_dev.newEvent();
  m_completion_event = wmt_dev.newSharedEvent();
  m_device->RegisterCommandQueue(this);
  QTRACE(
      "CmdQueue::ctor this=%p device=%p type=%u priority=%d flags=0x%x node=%u",
      (void *)this, (void *)device, desc.Type, desc.Priority, desc.Flags,
      desc.NodeMask);
  Logger::info("D3D12CommandQueue created");
}

MTLD3D12CommandQueue::~MTLD3D12CommandQueue() {
  QTRACE("CmdQueue::dtor this=%p", (void *)this);
  m_device->UnregisterCommandQueue(this);
  m_device->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandQueue::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12CommandQueue) {
    *ppvObject = ref(this);
    QTRACE("CmdQueue::QI %s -> S_OK this=%p out=%p", str::format(riid).c_str(),
           (void *)this, *ppvObject);
    return S_OK;
  }

  if (riid == __uuidof(IMTLDXGIDevice)) {
    return m_device->QueryInterface(riid, ppvObject);
  }
  QTRACE("CmdQueue::QI unknown IID %s -> E_NOINTERFACE",
         str::format(riid).c_str());
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12CommandQueue::AddRef() {
  uint32_t rc = ++m_refCount;
  QTRACE("CmdQueue::AddRef this=%p rc=%u", (void *)this, rc);
  return rc;
}

ULONG STDMETHODCALLTYPE MTLD3D12CommandQueue::Release() {
  uint32_t rc = --m_refCount;
  QTRACE("CmdQueue::Release this=%p rc=%u", (void *)this, rc);
  if (!rc) {
    uint32_t rp = --m_refPrivate;
    if (!rp) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }
  return rc;
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::GetPrivateData(REFGUID guid,
                                                               UINT *data_size,
                                                               void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::SetPrivateData(
    REFGUID guid, UINT data_size, const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::SetPrivateDataInterface(
    REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::GetDevice(REFIID riid,
                                                          void **device) {
  QTRACE("CmdQueue::GetDevice this=%p riid=%s out=%p", (void *)this,
         str::format(riid).c_str(), device);
  return m_device->QueryInterface(riid, device);
}

static bool BuildSparseTextureMappings(
    MTLD3D12Resource *resource, UINT region_count,
    const D3D12_TILED_RESOURCE_COORDINATE *region_coordinates,
    const D3D12_TILE_REGION_SIZE *region_sizes, UINT range_count,
    const D3D12_TILE_RANGE_FLAGS *range_flags,
    const UINT *heap_range_offsets,
    const UINT *range_tile_counts,
    std::vector<WMTTextureMapping> &mappings) {
  if (!resource || !resource->IsSparseBacked() || !region_count ||
      !region_coordinates || !region_sizes)
    return false;
  D3D12_RESOURCE_DESC desc = {};
  resource->GetDesc(&desc);
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.SampleDesc.Count > 1 || !desc.MipLevels ||
      !desc.DepthOrArraySize)
    return false;
  if (range_count && !range_tile_counts)
    return false;
  for (UINT range = 0; range < range_count; range++) {
    const D3D12_TILE_RANGE_FLAGS flags =
        range_flags ? range_flags[range] : D3D12_TILE_RANGE_FLAG_NONE;
    if (flags != D3D12_TILE_RANGE_FLAG_NONE &&
        flags != D3D12_TILE_RANGE_FLAG_NULL)
      return false;
    if (flags == D3D12_TILE_RANGE_FLAG_NONE && !heap_range_offsets)
      return false;
  }
  const D3D12_TILE_SHAPE shape = resource->GetTiledResourceTileShape();
  const UINT mip_levels = desc.MipLevels;
  const UINT tiles_x =
      (std::max<UINT>(1, desc.Width) + shape.WidthInTexels - 1) /
      shape.WidthInTexels;
  const UINT tiles_y =
      (std::max<UINT>(1, desc.Height) + shape.HeightInTexels - 1) /
      shape.HeightInTexels;
  const UINT metal_tiles_x = std::max<UINT>(1, shape.WidthInTexels / 2);
  const UINT metal_tiles_y = std::max<UINT>(1, shape.HeightInTexels / 2);
  UINT range_index = 0;
  UINT range_remaining = range_count
                             ? (range_tile_counts ? range_tile_counts[0] : 1)
                             : 0;
  for (UINT region_index = 0; region_index < region_count; region_index++) {
    const auto &coordinate = region_coordinates[region_index];
    const auto &size = region_sizes[region_index];
    const UINT mip = coordinate.Subresource % mip_levels;
    const UINT slice = coordinate.Subresource / mip_levels;
    if (slice >= desc.DepthOrArraySize || mip >= mip_levels)
      return false;
    const UINT region_width = size.UseBox ? size.Width : size.NumTiles;
    const UINT region_height = size.UseBox ? size.Height : 1;
    const UINT region_depth = size.UseBox ? size.Depth : 1;
    if (!region_width || !region_height || !region_depth)
      continue;
    const uint64_t tile_count = uint64_t(region_width) * region_height *
                                region_depth;
    if (!size.UseBox && (coordinate.X >= tiles_x ||
                         uint64_t(coordinate.X) + region_width > tiles_x ||
                         coordinate.Y >= tiles_y))
      return false;
    if (size.UseBox &&
        (coordinate.X >= tiles_x || coordinate.Y >= tiles_y ||
         uint64_t(coordinate.X) + size.Width > tiles_x ||
         uint64_t(coordinate.Y) + size.Height > tiles_y))
      return false;
    for (uint64_t tile = 0; tile < tile_count; tile++) {
      while (range_count && range_index < range_count &&
             range_remaining == 0) {
        range_index++;
        range_remaining = range_index < range_count
                              ? (range_tile_counts ? range_tile_counts[range_index]
                                                    : 1)
                              : 0;
      }
      if (range_count && range_index >= range_count)
        return false;
      const D3D12_TILE_RANGE_FLAGS flags =
          range_count && range_flags ? range_flags[range_index]
                                     : D3D12_TILE_RANGE_FLAG_NONE;
      if (size.UseBox) {
        const UINT z = static_cast<UINT>(tile / (size.Width * size.Height));
        const UINT rem = static_cast<UINT>(tile % (size.Width * size.Height));
        const UINT y = rem / size.Width;
        const UINT x = rem % size.Width;
        mappings.push_back({
            flags == D3D12_TILE_RANGE_FLAG_NULL
                ? WMTTextureMappingModeUnmap
                : WMTTextureMappingModeMap,
            {uint64_t(coordinate.X + x) * metal_tiles_x,
             uint64_t(coordinate.Y + y) * metal_tiles_y,
             uint64_t(z)},
            {metal_tiles_x, metal_tiles_y, 1}, mip, slice});
      } else {
        const UINT x = coordinate.X + static_cast<UINT>(tile % tiles_x);
        const UINT y = coordinate.Y + static_cast<UINT>(tile / tiles_x);
        mappings.push_back({
            flags == D3D12_TILE_RANGE_FLAG_NULL
                ? WMTTextureMappingModeUnmap
                : WMTTextureMappingModeMap,
            {uint64_t(x) * metal_tiles_x, uint64_t(y) * metal_tiles_y, 0},
            {metal_tiles_x, metal_tiles_y, 1}, mip, slice});
      }
      if (range_count && range_remaining)
        range_remaining--;
    }
  }
  return !mappings.empty();
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::UpdateTileMappings(
    ID3D12Resource *resource, UINT region_count,
    const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
    const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap,
    UINT range_count, const D3D12_TILE_RANGE_FLAGS *range_flags,
    const UINT *heap_range_offsets, const UINT *range_tile_counts,
    D3D12_TILE_MAPPING_FLAGS flags) {
  QTRACE("CmdQueue::UpdateTileMappings this=%p resource=%p regions=%u heap=%p "
         "ranges=%u flags=0x%x",
         (void *)this, (void *)resource, region_count, (void *)heap,
         range_count, flags);
  if (!range_count) {
    QTRACE("CmdQueue::UpdateTileMappings rejected empty range list");
    return;
  }
  if ((static_cast<UINT>(flags) & ~static_cast<UINT>(
          D3D12_TILE_MAPPING_FLAG_NO_HAZARD)) != 0) {
    QTRACE("CmdQueue::UpdateTileMappings rejected unknown flags=0x%x", flags);
    return;
  }
  auto *sparse_resource = static_cast<MTLD3D12Resource *>(resource);
  auto *tile_heap = static_cast<MTLD3D12Heap *>(heap);
  if (!range_tile_counts)
    return;
  D3D12_RESOURCE_DESC sparse_desc = {};
  if (!sparse_resource || !sparse_resource->IsReservedResource() ||
      !sparse_resource->IsSparseBacked())
    return;
  sparse_resource->GetDesc(&sparse_desc);
  bool needs_heap = false;
  for (UINT range = 0; range < range_count; range++)
    needs_heap |= !range_flags ||
                  range_flags[range] == D3D12_TILE_RANGE_FLAG_NONE;
  if (needs_heap) {
    if (!tile_heap || !heap_range_offsets || !range_tile_counts)
      return;
    const UINT64 heap_tile_count =
        tile_heap->GetHeapDesc().SizeInBytes /
        D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    for (UINT range = 0; range < range_count; range++) {
      const D3D12_TILE_RANGE_FLAGS range_flag =
          range_flags ? range_flags[range] : D3D12_TILE_RANGE_FLAG_NONE;
      if (range_flag == D3D12_TILE_RANGE_FLAG_NONE &&
          (uint64_t(heap_range_offsets[range]) + range_tile_counts[range] >
           heap_tile_count)) {
        QTRACE("CmdQueue::UpdateTileMappings rejected heap range=%u offset=%u "
               "count=%u heap_tiles=%llu",
               range, heap_range_offsets[range], range_tile_counts[range],
               (unsigned long long)heap_tile_count);
        return;
      }
    }
  }
  if (sparse_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    const uint64_t tile_size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    const uint64_t total_tiles =
        (sparse_desc.Width + tile_size - 1) / tile_size;
    for (UINT range = 0; range < range_count; range++) {
      const auto range_flag =
          range_flags ? range_flags[range] : D3D12_TILE_RANGE_FLAG_NONE;
      if (range_flag != D3D12_TILE_RANGE_FLAG_NONE &&
          range_flag != D3D12_TILE_RANGE_FLAG_NULL)
        return;
    }
    uint64_t range_index = 0;
    uint64_t range_remaining = range_tile_counts[0];
    uint64_t mapped_tiles = 0;
    uint8_t *cpu = static_cast<uint8_t *>(sparse_resource->GetCPUAddress());
    for (UINT region = 0; region < region_count; region++) {
      const auto &coordinate = region_start_coordinates[region];
      const auto &size = region_sizes[region];
      if (coordinate.Y || coordinate.Z || coordinate.Subresource ||
          size.UseBox || !size.NumTiles || coordinate.X >= total_tiles ||
          size.NumTiles > total_tiles - coordinate.X)
        return;
      for (UINT tile = 0; tile < size.NumTiles; tile++) {
        while (range_index < range_count && range_remaining == 0) {
          range_index++;
          range_remaining = range_index < range_count
                                ? range_tile_counts[range_index]
                                : 0;
        }
        if (range_index >= range_count || !range_remaining)
          return;
        const auto range_flag = range_flags
                                    ? range_flags[range_index]
                                    : D3D12_TILE_RANGE_FLAG_NONE;
        if (range_flag == D3D12_TILE_RANGE_FLAG_NULL && cpu)
          std::memset(cpu + (uint64_t(coordinate.X) + tile) * tile_size, 0,
                      static_cast<size_t>(tile_size));
        if (range_flag == D3D12_TILE_RANGE_FLAG_NONE && !tile_heap)
          return;
        range_remaining--;
        mapped_tiles++;
      }
    }
    QTRACE("CmdQueue::UpdateTileMappings reserved buffer tiles=%llu "
           "mapped=%llu (full-buffer compatibility backing)",
           (unsigned long long)mapped_tiles,
           (unsigned long long)total_tiles);
    return;
  }
  std::vector<WMTTextureMapping> mappings;
  if (!BuildSparseTextureMappings(
          sparse_resource, region_count, region_start_coordinates,
          region_sizes, range_count, range_flags, heap_range_offsets,
          range_tile_counts,
          mappings)) {
    QTRACE("CmdQueue::UpdateTileMappings rejected unsupported sparse mapping");
    return;
  }
  auto cmdbuf = m_wmt_queue.commandBuffer();
  auto encoder = cmdbuf.resourceStateCommandEncoder();
  if (!cmdbuf.handle || !encoder.handle ||
      !encoder.updateTextureMappings(sparse_resource->GetMTLTexture(),
                                     mappings.data(), mappings.size())) {
    QTRACE("CmdQueue::UpdateTileMappings native encoder failed");
    return;
  }
  encoder.endEncoding();
  obj_handle_t retained[] = {sparse_resource->GetMTLTexture().handle,
                             sparse_resource->GetSparseHeap().handle};
  cmdbuf.retainObjectsUntilCompleted(retained, 2);
  cmdbuf.commit();
  QTRACE("CmdQueue::UpdateTileMappings native mappings=%zu committed",
         mappings.size());
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::CopyTileMappings(
    ID3D12Resource *dst_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
    ID3D12Resource *src_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
    const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags) {
  QTRACE("CmdQueue::CopyTileMappings this=%p dst=%p src=%p flags=0x%x",
         (void *)this, (void *)dst_resource, (void *)src_resource, flags);
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::ExecuteCommandLists(
    UINT command_list_count, ID3D12CommandList *const *command_lists) {
  QTRACE("ExecuteCommandLists count=%u", command_list_count);
  std::lock_guard submit_lock(m_submit_mutex);

  for (UINT li = 0; li < command_list_count; li++) {
    DXMTD3D12ScopedTimer list_timer("Queue", "ExecuteCommandList");
    QTRACE("ECL: processing list %u", li);
    auto *list = static_cast<MTLD3D12GraphicsCommandList *>(command_lists[li]);
    if (!list) {
      QTRACE("ECL: list %u is null, skipping", li);
      continue;
    }
    const uint64_t command_list_id = list->GetDebugId();

    QTRACE("ECL: creating cmdbuf from m_wmt_queue");
    auto cmdbuf = m_wmt_queue.commandBuffer();
    QTRACE("ECL: cmdbuf handle=%llu", (unsigned long long)cmdbuf.handle);
    if (!cmdbuf.handle) {
      Logger::err("ExecuteCommandLists: failed to create Metal command buffer");
      continue;
    }

    const auto cmds = list->GetCommands();
    const auto stream_stats =
        D3D12CollectCommandStreamStats(cmds.data(), cmds.size());
    QTRACE("ExecuteCommandLists: cmds.size=%zu empty=%d", cmds.size(),
           cmds.empty());
    list_timer.SetDetail("index=%u queue_type=%u cmds=%zu", li, m_desc.Type,
                         cmds.size());
    if (cmds.empty()) {
      QTRACE("ExecuteCommandLists: empty cmdlist, committing");
      cmdbuf.commit();
      QTRACE("ExecuteCommandLists: empty cmdlist committed ok");
      continue;
    }

    ReplayState st;
    st.cmdbuf = cmdbuf;

    QTRACE("ExecuteCommandLists: cmd_size=%zu", cmds.size());
    auto replay_begin = std::chrono::steady_clock::now();
    size_t offset = 0;
    size_t cmd_count = 0;
    uint32_t type_counts[40] = {};
    while (offset < cmds.size()) {
      if (offset + sizeof(CmdHeader) > cmds.size())
        break;
      auto *header = reinterpret_cast<const CmdHeader *>(cmds.data() + offset);
      if (header->size < sizeof(CmdHeader) || header->size > 65536 ||
          offset + header->size > cmds.size()) {
        QTRACE("ECL: corrupt cmd at offset=%zu type=%d size=%u cmds_size=%zu - "
               "skipping rest",
               offset, (int)header->type, header->size, cmds.size());
        break;
      }

      if ((uint32_t)header->type < 40)
        type_counts[(uint32_t)header->type]++;
      cmd_count++;

      if (cmd_count <= 5 || (cmd_count % 50) == 0)
        QTRACE("ECL cmd[%zu] type=%d size=%u offset=%zu", cmd_count,
               (int)header->type, (unsigned)header->size, offset);
      switch (header->type) {
      case CmdType::DrawInstanced: {
        auto *cmd = reinterpret_cast<const CmdDrawInstanced *>(header);
        st.EnsureSwapchainRenderPSOReady();
        st.EnsureRenderEncoder();
        st.ApplyRootBindings(m_device);
        st.BuildVertexConstantBufferTable(m_device);
        st.BuildVertexArgumentBuffer(m_device);
        st.BuildGeometryConstantBufferTable(m_device);
        st.BuildGeometryArgumentBuffer(m_device);
        st.BuildConstantBufferTable(m_device);
        st.BuildArgumentBuffer(m_device);
        if (st.render_enc_open && st.arg_buf.handle) {
          uint32_t bind_index = st.BindIndexOrFallback(
              st.pso->GetPSReflection().ArgumentBufferBindIndex,
              st.kArgBufSlot);
          st.SetFragmentBufferTracked(st.arg_buf, 0, bind_index);
        }
        st.BindStaticSamplers();
        st.ApplyVertexBuffers(m_device);
        st.BindGeometryMeshBuffers();
        st.AddRenderFaultBreadcrumb("DrawInstanced", cmd->vertex_count,
                                    cmd->instance_count, cmd->start_vertex, 0,
                                    0, false);
        auto safety = st.ValidateDrawSafety(
            m_device, cmd->vertex_count, cmd->instance_count, cmd->start_vertex,
            0, cmd->start_instance, false);
        if (D3D12DrawSafetySkipped(safety)) {
          st.LogDrawSafetySkip("DrawInstanced", safety, cmd->vertex_count,
                               cmd->instance_count, cmd->start_vertex, 0,
                               cmd->start_instance, false);
          break;
        }
        if (st.ShouldSkipUnsafeMSCOffscreenPass()) {
          if (TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::warn(str::format(
                "M12 skipping unsafe MSC offscreen DrawInstanced reason=",
                st.UnsafeMSCOffscreenPassReason(), " v=", cmd->vertex_count,
                " i=", cmd->instance_count, " pso=", (void *)st.pso, " ",
                TracePsoShaderSummary(st.pso)));
          }
          break;
        }
        QTRACE("DrawInstanced v=%u i=%u enc_open=%d pso=%p compiled=%d "
               "stage=%s detail=%s",
               cmd->vertex_count, cmd->instance_count, st.render_enc_open,
               (void *)st.pso, st.pso ? st.pso->IsCompiled() : 0,
               TraceCompileFailureStage(st.pso),
               TraceCompileFailureDetail(st.pso));

        if (st.pso && st.pso->UsesNativeTessellationPath()) {
          if (st.EncodeNativeTessellationDraw(
                  m_device, cmd->vertex_count, cmd->instance_count,
                  cmd->start_vertex, cmd->start_instance)) {
            st.LogFinalRenderSnapshot("NativeTessellationDraw",
                                      cmd->vertex_count, cmd->instance_count,
                                      cmd->start_vertex);
            st.MarkSwapchainWorkEncoded();
          }
        } else if (st.pso && st.pso->UsesGeometryMeshPipeline() &&
                   st.EncodeGeometryDraw(m_device, cmd->vertex_count,
                                         cmd->instance_count, cmd->start_vertex,
                                         cmd->start_instance)) {
          st.LogTessellationFallbackDraw("GeometryDraw", cmd->vertex_count,
                                         cmd->instance_count, false);
          st.LogFinalRenderSnapshot("GeometryDraw", cmd->vertex_count,
                                    cmd->instance_count, cmd->start_vertex);
          st.MarkSwapchainWorkEncoded();
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::info(str::format(
                "M12 swapchain GeometryDraw encoded v=", cmd->vertex_count,
                " i=", cmd->instance_count, " pso=", (void *)st.pso, " ",
                TracePsoShaderSummary(st.pso)));
          }
        } else if (cmd->instance_count > 0 && cmd->vertex_count > 0 &&
                   st.render_enc_open && st.HasUsableRenderPSO()) {
          st.LogTessellationFallbackDraw("DrawInstanced", cmd->vertex_count,
                                         cmd->instance_count, false);
          struct wmtcmd_render_draw draw = {};
          draw.type = WMTRenderCommandDraw;
          draw.next.set(nullptr);
          draw.primitive_type = st.GetMetalPrimitiveType();
          draw.vertex_start = cmd->start_vertex;
          draw.vertex_count = cmd->vertex_count;
          draw.base_instance = cmd->start_instance;
          draw.instance_count = cmd->instance_count;
          st.BindMSCDrawParameters(m_device, cmd->vertex_count,
                                   cmd->instance_count, cmd->start_vertex, 0,
                                   cmd->start_instance, false,
                                   WMTIndexTypeUInt16);
          st.LogFinalRenderSnapshot("DrawInstanced", cmd->vertex_count,
                                    cmd->instance_count, cmd->start_vertex);
          st.LogStageInVertexSnapshot("DrawInstanced", cmd->vertex_count,
                                      cmd->instance_count);
          st.LogNonStageInVertexSnapshot(
              m_device, "DrawInstanced", cmd->vertex_count, cmd->instance_count,
              cmd->start_vertex, cmd->start_instance);
          st.BindMissingNonStageInVertexBuffers(m_device);
          st.BindDirectFragmentCompleteness(m_device, "draw_instanced");
          WMTPrimitiveType primitive_type = draw.primitive_type;
          if (st.EncodeRenderCommands(
                  reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                  "draw_instanced"))
            st.MarkSwapchainWorkEncoded();
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::info(str::format(
                "M12 swapchain DrawInstanced encoded v=", cmd->vertex_count,
                " i=", cmd->instance_count, " start=", cmd->start_vertex,
                " topology=", (unsigned)st.topology, " primitive=",
                (unsigned)primitive_type, " pso=", (void *)st.pso,
                " enc=", (unsigned long long)st.render_enc.handle, " ",
                TracePsoShaderSummary(st.pso)));
          }
        } else {
          QTRACE(
              "DrawInstanced SKIPPED v=%u i=%u enc_open=%d pso=%p compiled=%d "
              "render_handle=%llu stage=%s detail=%s",
              cmd->vertex_count, cmd->instance_count, st.render_enc_open,
              (void *)st.pso, st.pso ? st.pso->IsCompiled() : 0,
              (unsigned long long)(st.pso ? st.pso->GetRenderPSO().handle : 0),
              TraceCompileFailureStage(st.pso),
              TraceCompileFailureDetail(st.pso));
          if (st.HasSwapchainRenderTarget()) {
            Logger::info(str::format(
                "M12 swapchain DrawInstanced skipped v=", cmd->vertex_count,
                " i=", cmd->instance_count, " enc_open=", st.render_enc_open,
                " pso=", (void *)st.pso,
                " compiled=", st.pso ? st.pso->IsCompiled() : 0,
                " stage=", TraceCompileFailureStage(st.pso),
                " detail=", TraceCompileFailureDetail(st.pso)));
          }
        }
        break;
      }
      case CmdType::DrawIndexedInstanced: {
        auto *cmd = reinterpret_cast<const CmdDrawIndexedInstanced *>(header);
        st.EnsureSwapchainRenderPSOReady();
        st.EnsureRenderEncoder();
        st.ApplyRootBindings(m_device);
        st.BuildVertexConstantBufferTable(m_device);
        st.BuildVertexArgumentBuffer(m_device);
        st.BuildGeometryConstantBufferTable(m_device);
        st.BuildGeometryArgumentBuffer(m_device);
        st.BuildConstantBufferTable(m_device);
        st.BuildArgumentBuffer(m_device);
        if (st.render_enc_open && st.arg_buf.handle) {
          uint32_t bind_index = st.BindIndexOrFallback(
              st.pso->GetPSReflection().ArgumentBufferBindIndex,
              st.kArgBufSlot);
          st.SetFragmentBufferTracked(st.arg_buf, 0, bind_index);
        }
        st.BindStaticSamplers();
        st.ApplyVertexBuffers(m_device);
        st.BindGeometryMeshBuffers();
        st.AddRenderFaultBreadcrumb(
            "DrawIndexedInstanced", cmd->index_count, cmd->instance_count,
            cmd->start_index, cmd->base_vertex, st.ib.BufferLocation, true);
        auto safety = st.ValidateDrawSafety(
            m_device, cmd->index_count, cmd->instance_count, cmd->start_index,
            cmd->base_vertex, cmd->start_instance, true);
        if (D3D12DrawSafetySkipped(safety)) {
          st.LogDrawSafetySkip("DrawIndexedInstanced", safety, cmd->index_count,
                               cmd->instance_count, cmd->start_index,
                               cmd->base_vertex, cmd->start_instance, true);
          break;
        }
        st.LogNativeVertexResolved("DrawIndexedInstanced", cmd->index_count,
                                   cmd->instance_count, cmd->start_index,
                                   cmd->base_vertex, cmd->start_instance, true);
        std::string unsafe_stage_in_reason;
        if (st.ShouldSkipUnsafeMSCIndexedStageInDraw(
                m_device, cmd->index_count, cmd->instance_count,
                cmd->start_index, cmd->base_vertex, cmd->start_instance,
                unsafe_stage_in_reason)) {
          __atomic_add_fetch(&g_quarantine_indexed_stage_in, 1,
                             __ATOMIC_RELAXED);
          if (TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::warn(str::format(
                "M12 skipping unsafe MSC indexed stage-in DrawIndexedInstanced "
                "reason=",
                unsafe_stage_in_reason, " pso=", (void *)st.pso, " ",
                TracePsoShaderSummary(st.pso)));
          }
          break;
        }
        if (st.ShouldSkipUnsafeMSCOffscreenPass()) {
          if (TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::warn(str::format(
                "M12 skipping unsafe MSC offscreen DrawIndexedInstanced "
                "reason=",
                st.UnsafeMSCOffscreenPassReason(), " idx=", cmd->index_count,
                " inst=", cmd->instance_count, " start=", cmd->start_index,
                " pso=", (void *)st.pso, " ", TracePsoShaderSummary(st.pso)));
          }
          break;
        }

        if (st.pso && st.pso->UsesNativeTessellationPath()) {
          if (st.EncodeNativeTessellationDrawIndexed(
                  m_device, cmd->index_count, cmd->instance_count,
                  cmd->start_index, cmd->base_vertex, cmd->start_instance)) {
            st.LogFinalRenderSnapshot("NativeTessellationDrawIndexed",
                                      cmd->index_count, cmd->instance_count,
                                      cmd->start_index);
            st.MarkSwapchainWorkEncoded();
          }
        } else if (st.pso && st.pso->UsesGeometryMeshPipeline() &&
                   st.EncodeGeometryDrawIndexed(
                       m_device, cmd->index_count, cmd->instance_count,
                       cmd->start_index, cmd->base_vertex,
                       cmd->start_instance)) {
          st.LogTessellationFallbackDraw("GeometryDrawIndexed",
                                         cmd->index_count, cmd->instance_count,
                                         true);
          st.LogFinalRenderSnapshot("GeometryDrawIndexed", cmd->index_count,
                                    cmd->instance_count, cmd->start_index);
          st.MarkSwapchainWorkEncoded();
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::info(str::format(
                "M12 swapchain GeometryDrawIndexed encoded idx=",
                cmd->index_count, " inst=", cmd->instance_count,
                " pso=", (void *)st.pso, " ", TracePsoShaderSummary(st.pso)));
          }
        } else if (cmd->instance_count > 0 && cmd->index_count > 0 &&
                   st.ib.BufferLocation && st.render_enc_open &&
                   st.HasUsableRenderPSO()) {
          st.LogTessellationFallbackDraw("DrawIndexedInstanced",
                                         cmd->index_count, cmd->instance_count,
                                         true);
          auto *ib_res =
              m_device->LookupResourceByGPUAddress(st.ib.BufferLocation);
          if (!ib_res && st.ib.BufferLocation) {
            ib_res = reinterpret_cast<MTLD3D12Resource *>(st.ib.BufferLocation);
          }
          uint64_t index_buffer_offset = 0;
          if (ib_res) {
            index_buffer_offset =
                st.ib.BufferLocation - ib_res->GetGPUVirtualAddress();
            index_buffer_offset +=
                uint64_t(cmd->start_index) *
                (st.ib.Format == DXGI_FORMAT_R32_UINT ? 4ull : 2ull);
            if (st.render_enc_open && ib_res->GetMTLBuffer().handle) {
              st.render_enc.useResource(ib_res->GetMTLBuffer(),
                                        WMTResourceUsageRead,
                                        WMTRenderStageVertex);
              st.RetainMTLObjectForCompletion(ib_res->GetMTLBuffer());
            }
          }
          QTRACE("DrawIndexedInstanced idx=%u inst=%u start_index=%u "
                 "base_vertex=%d "
                 "base_instance=%u ib_gpu=0x%llx ib_res=%p ib_off=%llu "
                 "enc_open=%d pso=%p compiled=%d stage=%s detail=%s",
                 cmd->index_count, cmd->instance_count, cmd->start_index,
                 cmd->base_vertex, cmd->start_instance,
                 (unsigned long long)st.ib.BufferLocation, (void *)ib_res,
                 (unsigned long long)index_buffer_offset, st.render_enc_open,
                 (void *)st.pso, st.pso ? st.pso->IsCompiled() : 0,
                 TraceCompileFailureStage(st.pso),
                 TraceCompileFailureDetail(st.pso));
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_vertex_sample_logs, 24)) {
            uint32_t first_index = cmd->start_index;
            bool index_sampled = false;
            HRESULT index_map_hr = E_FAIL;
            if (ib_res) {
              D3D12_RESOURCE_DESC ib_desc = {};
              ib_res->GetDesc(&ib_desc);
              const uint32_t index_size =
                  st.ib.Format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
              const uint64_t raw_index_offset =
                  st.ib.BufferLocation - ib_res->GetGPUVirtualAddress() +
                  uint64_t(cmd->start_index) * index_size;
              if (raw_index_offset + index_size <= ib_desc.Width) {
                void *index_base = nullptr;
                D3D12_RANGE read_range = {raw_index_offset,
                                          raw_index_offset + index_size};
                index_map_hr = ib_res->Map(0, &read_range, &index_base);
                if (SUCCEEDED(index_map_hr) && index_base) {
                  const auto *index_bytes =
                      static_cast<const uint8_t *>(index_base) +
                      raw_index_offset;
                  if (st.ib.Format == DXGI_FORMAT_R32_UINT) {
                    uint32_t value = 0;
                    std::memcpy(&value, index_bytes, sizeof(value));
                    first_index = value;
                  } else {
                    uint16_t value = 0;
                    std::memcpy(&value, index_bytes, sizeof(value));
                    first_index = value;
                  }
                  index_sampled = true;
                  ib_res->Unmap(0, nullptr);
                }
              }
            }

            const auto &vb0 = st.vbs[0];
            const int64_t vertex_id =
                int64_t(cmd->base_vertex) + int64_t(first_index);
            bool vertex_sampled = false;
            HRESULT vertex_map_hr = E_FAIL;
            std::string vertex_bytes;
            float f[4] = {};
            uint64_t vertex_gpu = 0;
            uint64_t vertex_offset = 0;
            MTLD3D12Resource *vb_res = nullptr;
            if (vb0.BufferLocation && vb0.StrideInBytes && vertex_id >= 0) {
              vertex_gpu = vb0.BufferLocation +
                           uint64_t(vertex_id) * uint64_t(vb0.StrideInBytes);
              vb_res = m_device->LookupResourceByGPUAddress(vertex_gpu);
              if (vb_res) {
                D3D12_RESOURCE_DESC vb_desc = {};
                vb_res->GetDesc(&vb_desc);
                vertex_offset = vertex_gpu - vb_res->GetGPUVirtualAddress();
                const size_t sample_bytes =
                    std::min<size_t>(vb0.StrideInBytes, 64);
                if (vertex_offset + sample_bytes <= vb_desc.Width) {
                  void *vertex_base = nullptr;
                  D3D12_RANGE read_range = {vertex_offset,
                                            vertex_offset + sample_bytes};
                  vertex_map_hr = vb_res->Map(0, &read_range, &vertex_base);
                  if (SUCCEEDED(vertex_map_hr) && vertex_base) {
                    const auto *bytes =
                        static_cast<const uint8_t *>(vertex_base) +
                        vertex_offset;
                    vertex_bytes = FormatDebugBytes(bytes, sample_bytes);
                    const size_t float_bytes =
                        std::min<size_t>(sample_bytes, sizeof(f));
                    std::memcpy(f, bytes, float_bytes);
                    vertex_sampled = true;
                    vb_res->Unmap(0, nullptr);
                  }
                }
              }
            }

            Logger::info(str::format(
                "M12 swapchain vertex sample idx_count=", cmd->index_count,
                " start_index=", cmd->start_index, " first_index=", first_index,
                " index_sampled=", index_sampled, " index_hr=0x", std::hex,
                (unsigned)index_map_hr, std::dec, " base_vertex=",
                cmd->base_vertex, " vertex_id=", (long long)vertex_id,
                " vb0_gpu=", (unsigned long long)vb0.BufferLocation,
                " vb0_stride=", vb0.StrideInBytes, " vertex_gpu=",
                (unsigned long long)vertex_gpu, " vb_res=", (void *)vb_res,
                " vertex_off=", (unsigned long long)vertex_offset,
                " vertex_sampled=", vertex_sampled, " vertex_hr=0x", std::hex,
                (unsigned)vertex_map_hr, std::dec, " f0=", f[0], " f1=", f[1],
                " f2=", f[2], " f3=", f[3], " bytes=[", vertex_bytes, "]"));
          }
          struct wmtcmd_render_draw_indexed draw = {};
          draw.type = WMTRenderCommandDrawIndexed;
          draw.next.set(nullptr);
          draw.primitive_type = st.GetMetalPrimitiveType();
          draw.index_type = DXGIToWMTIndexFormat(st.ib.Format);
          draw.index_count = cmd->index_count;
          draw.index_buffer =
              ib_res ? ib_res->GetMTLBuffer().handle : NULL_OBJECT_HANDLE;
          draw.index_buffer_offset = index_buffer_offset;
          draw.instance_count = cmd->instance_count;
          draw.base_vertex = cmd->base_vertex;
          draw.base_instance = cmd->start_instance;
          st.BindMSCDrawParameters(
              m_device, cmd->index_count, cmd->instance_count, cmd->start_index,
              cmd->base_vertex, cmd->start_instance, true, draw.index_type);
          st.LogFinalRenderSnapshot("DrawIndexedInstanced", cmd->index_count,
                                    cmd->instance_count, cmd->start_index);
          st.LogStageInVertexSnapshot("DrawIndexedInstanced", cmd->index_count,
                                      cmd->instance_count);
          st.BindMissingNonStageInVertexBuffers(m_device);
          st.BindDirectFragmentCompleteness(m_device, "draw_indexed_instanced");
          if (st.EncodeRenderCommands(
                  reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                  "draw_indexed_instanced")) {
            st.MarkSwapchainWorkEncoded();
            if (!st.HasSwapchainRenderTarget() &&
                TakeLogBudget(&g_offscreen_indexed_draw_logs, 128)) {
              auto fragment_summary = st.FragmentCompletenessSummary();
              Logger::info(str::format(
                  "M12 offscreen DrawIndexedInstanced encoded idx=",
                  cmd->index_count, " inst=", cmd->instance_count,
                  " start=", cmd->start_index, " base=", cmd->base_vertex,
                  " start_inst=", cmd->start_instance,
                  " primitive=", (unsigned)draw.primitive_type,
                  " ib_fmt=", (unsigned)st.ib.Format, " ib_gpu=0x", std::hex,
                  (unsigned long long)st.ib.BufferLocation, std::dec,
                  " ib_res=", (void *)ib_res,
                  " ib_handle=", (unsigned long long)draw.index_buffer,
                  " ib_off=", (unsigned long long)index_buffer_offset,
                  " vb_summary=", st.last_vertex_table_summary,
                  " vb_bound=", st.last_bound_vertex_buffers,
                  " frag buffers=", fragment_summary.bound_buffer_count, "+",
                  fragment_summary.fallback_buffer_count, "/",
                  fragment_summary.required_buffer_count,
                  " textures=", fragment_summary.bound_texture_count, "+",
                  fragment_summary.fallback_texture_count, "/",
                  fragment_summary.required_texture_count,
                  " samplers=", fragment_summary.bound_sampler_count, "+",
                  fragment_summary.fallback_sampler_count, "/",
                  fragment_summary.required_sampler_count,
                  " pso=", (void *)st.pso, " ", TracePsoShaderSummary(st.pso),
                  " ", st.RenderTargetResourceSummary()));
            }
          }
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_draw_logs, 384)) {
            Logger::info(str::format(
                "M12 swapchain DrawIndexedInstanced encoded idx=",
                cmd->index_count, " inst=", cmd->instance_count,
                " start=", cmd->start_index, " ib_res=", (void *)ib_res,
                " ib_off=", (unsigned long long)index_buffer_offset,
                " pso=", (void *)st.pso,
                " enc=", (unsigned long long)st.render_enc.handle, " ",
                TracePsoShaderSummary(st.pso)));
          }
          if (st.HasSwapchainRenderTarget() &&
              TakeLogBudget(&g_swapchain_state_logs, 32)) {
            const auto &vp = st.viewports[0];
            const auto &sc = st.scissor_rects[0];
            const auto &rast = st.pso->GetRasterizerDesc();
            const auto &ds = st.pso->GetDepthStencilDesc();
            const auto &blend = st.pso->GetBlendDesc();
            const auto &vb0 = st.vbs[0];
            const auto effective_cull = st.EffectiveCullMode();
            DXGI_FORMAT actual_rt_format = DXGI_FORMAT_UNKNOWN;
            auto *rt_desc =
                reinterpret_cast<const D3D12Descriptor *>(st.rt_handles[0].ptr);
            auto *rt_res =
                rt_desc ? static_cast<MTLD3D12Resource *>(rt_desc->resource)
                        : nullptr;
            if (rt_res) {
              D3D12_RESOURCE_DESC resource_desc = {};
              rt_res->GetDesc(&resource_desc);
              actual_rt_format = resource_desc.Format;
            }
            Logger::info(str::format(
                "M12 swapchain draw state vp_count=", st.viewport_count,
                " vp=", vp.TopLeftX, ",", vp.TopLeftY, " ", vp.Width, "x",
                vp.Height, " depth=", vp.MinDepth, "-", vp.MaxDepth,
                " sc_count=", st.scissor_count, " sc=", sc.left, ",", sc.top,
                "-", sc.right, ",", sc.bottom, " topology=",
                (unsigned)st.topology, " cull=", (unsigned)rast.CullMode,
                " front_ccw=", (unsigned)rast.FrontCounterClockwise,
                " depth_enable=", (unsigned)ds.DepthEnable,
                " depth_func=", (unsigned)ds.DepthFunc,
                " stencil_enable=", (unsigned)ds.StencilEnable,
                " effective_cull=", (unsigned)effective_cull,
                " pso_rts=", (unsigned)st.pso->GetNumRenderTargets(),
                " pso_rtv0=", (unsigned)st.pso->GetRTVFormat(0),
                " actual_rtv0=", (unsigned)actual_rt_format,
                " sample_count=", (unsigned)st.pso->GetSampleCount(),
                " blend0=", (unsigned)blend.RenderTarget[0].BlendEnable,
                " write_mask0=0x", std::hex,
                (unsigned)blend.RenderTarget[0].RenderTargetWriteMask, std::dec,
                " src_blend0=", (unsigned)blend.RenderTarget[0].SrcBlend,
                " dst_blend0=", (unsigned)blend.RenderTarget[0].DestBlend,
                " blend_op0=", (unsigned)blend.RenderTarget[0].BlendOp,
                " src_alpha0=", (unsigned)blend.RenderTarget[0].SrcBlendAlpha,
                " dst_alpha0=", (unsigned)blend.RenderTarget[0].DestBlendAlpha,
                " blend_op_alpha0=",
                (unsigned)blend.RenderTarget[0].BlendOpAlpha,
                " vb0_gpu=", (unsigned long long)vb0.BufferLocation,
                " vb0_size=", vb0.SizeInBytes, " vb0_stride=",
                vb0.StrideInBytes, " ib_fmt=", (unsigned)st.ib.Format));
          }
        } else {
          QTRACE(
              "DrawIndexedInstanced SKIPPED idx=%u inst=%u ib_gpu=0x%llx "
              "enc_open=%d pso=%p compiled=%d render_handle=%llu stage=%s "
              "detail=%s",
              cmd->index_count, cmd->instance_count,
              (unsigned long long)st.ib.BufferLocation, st.render_enc_open,
              (void *)st.pso, st.pso ? st.pso->IsCompiled() : 0,
              (unsigned long long)(st.pso ? st.pso->GetRenderPSO().handle : 0),
              TraceCompileFailureStage(st.pso),
              TraceCompileFailureDetail(st.pso));
          if (st.HasSwapchainRenderTarget()) {
            Logger::info(str::format(
                "M12 swapchain DrawIndexedInstanced skipped idx=",
                cmd->index_count, " inst=", cmd->instance_count,
                " ib_gpu=", (unsigned long long)st.ib.BufferLocation,
                " enc_open=", st.render_enc_open, " pso=", (void *)st.pso,
                " compiled=", st.pso ? st.pso->IsCompiled() : 0,
                " stage=", TraceCompileFailureStage(st.pso),
                " detail=", TraceCompileFailureDetail(st.pso)));
          }
        }
        break;
      }
      case CmdType::BuildRaytracingAccelerationStructure: {
        auto *cmd = reinterpret_cast<
            const CmdBuildRaytracingAccelerationStructure *>(header);
        auto *dest = m_device->LookupResourceByGPUAddress(
            cmd->dest_acceleration_structure);
        auto *scratch = m_device->LookupResourceByGPUAddress(
            cmd->scratch_acceleration_structure);
        if (!m_device->SupportsMetalRaytracing() || !dest || !scratch ||
            !scratch->GetMTLBuffer().handle) {
          QTRACE("BuildRaytracingAS SKIPPED type=%u dest=%p scratch=%p",
                 (unsigned)cmd->type, (void *)dest, (void *)scratch);
          break;
        }

        WMTAccelerationStructureSizes sizes = {};
        auto metal_device = m_device->GetMTLDevice();
        uint64_t scratch_offset = cmd->scratch_acceleration_structure -
                                  scratch->GetGPUVirtualAddress();
        st.CloseRenderEncoder();

        WMT::Reference<WMT::AccelerationStructure> acceleration_structure;
        bool encoded = false;
        uint64_t primitive_count = 0;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> bottom_level_pointers;
        const char *kind = "unknown";
        if (cmd->type ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
          D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
          inputs.Type = cmd->type;
          inputs.Flags = cmd->flags;
          inputs.NumDescs = cmd->num_descs;
          inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
          inputs.pGeometryDescs = cmd->geometries;
          if (cmd->num_descs > 1) {
            std::array<WMTPrimitiveAccelerationStructureInfo,
                       CmdBuildRaytracingAccelerationStructure::kMaxGeometryDescs>
                metal_infos = {};
            bool valid = cmd->num_descs <= metal_infos.size();
            for (UINT i = 0; valid && i < cmd->num_descs; i++) {
              D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS one =
                  inputs;
              one.NumDescs = 1;
              one.pGeometryDescs = &cmd->geometries[i];
              valid = D3D12ResolveTriangleAccelerationStructureInfo(
                  m_device, &one, metal_infos[i]);
              primitive_count += metal_infos[i].triangle_count;
            }
            if (!valid ||
                !metal_device.accelerationStructureSizesForTriangleGeometries(
                    metal_infos.data(), cmd->num_descs, sizes)) {
              QTRACE("BuildRaytracingAS SKIPPED multi-geometry descriptors");
              break;
            }
            acceleration_structure = metal_device.newAccelerationStructure(
                sizes.acceleration_structure_size);
            encoded = acceleration_structure.handle &&
                      cmdbuf.buildTriangleAccelerationStructures(
                          acceleration_structure, metal_infos.data(),
                          cmd->num_descs, scratch->GetMTLBuffer(),
                          scratch_offset);
            kind = "triangle geometries";
            for (UINT i = 0; i < cmd->num_descs; i++) {
              const auto &geometry = cmd->geometries[i];
              if (auto *vertex = m_device->LookupResourceByGPUAddress(
                      geometry.Triangles.VertexBuffer.StartAddress))
                st.RetainResourceMetalObjectsForCompletion(vertex);
              if (geometry.Triangles.IndexBuffer) {
                if (auto *index = m_device->LookupResourceByGPUAddress(
                        geometry.Triangles.IndexBuffer))
                  st.RetainResourceMetalObjectsForCompletion(index);
              }
            }
          } else if (cmd->geometries[0].Type ==
              D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
            WMTAABBAccelerationStructureInfo metal_info = {};
            if (!D3D12ResolveAABBAccelerationStructureInfo(
                    m_device, &inputs, metal_info) ||
                !metal_device.accelerationStructureSizesForAABBs(metal_info,
                                                                  sizes)) {
              QTRACE("BuildRaytracingAS SKIPPED AABB descriptor/size query");
              break;
            }
            const bool perform_update =
                (cmd->flags &
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) !=
                0;
            if (perform_update) {
              auto *source = m_device->LookupResourceByGPUAddress(
                  cmd->source_acceleration_structure);
              acceleration_structure = dest->GetMTLAccelerationStructure();
              if (!source || !source->GetMTLAccelerationStructure().handle ||
                  !acceleration_structure.handle || !metal_info.allow_refit) {
                QTRACE("BuildRaytracingAS SKIPPED AABB update state");
                break;
              }
              encoded = cmdbuf.refitAABBAccelerationStructure(
                  source->GetMTLAccelerationStructure(),
                  acceleration_structure, metal_info, scratch->GetMTLBuffer(),
                  scratch_offset);
              if (encoded)
                st.RetainResourceMetalObjectsForCompletion(source);
            } else {
              acceleration_structure = metal_device.newAccelerationStructure(
                  sizes.acceleration_structure_size);
              encoded = acceleration_structure.handle &&
                        cmdbuf.buildAABBAccelerationStructure(
                            acceleration_structure, metal_info,
                            scratch->GetMTLBuffer(), scratch_offset);
            }
            primitive_count = metal_info.bounding_box_count;
            kind = perform_update ? "AABB update" : "AABBs";
            if (auto *aabbs = m_device->LookupResourceByGPUAddress(
                    cmd->geometries[0].AABBs.AABBs.StartAddress))
              st.RetainResourceMetalObjectsForCompletion(aabbs);
          } else {
            WMTPrimitiveAccelerationStructureInfo metal_info = {};
            if (!D3D12ResolveTriangleAccelerationStructureInfo(
                    m_device, &inputs, metal_info) ||
                !metal_device.accelerationStructureSizesForTriangles(
                    metal_info, sizes)) {
              QTRACE("BuildRaytracingAS SKIPPED triangle descriptor/size query");
              break;
            }
            const bool perform_update =
                (cmd->flags &
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) !=
                0;
            if (perform_update) {
              auto *source = m_device->LookupResourceByGPUAddress(
                  cmd->source_acceleration_structure);
              acceleration_structure = dest->GetMTLAccelerationStructure();
              if (!source || !source->GetMTLAccelerationStructure().handle ||
                  !acceleration_structure.handle ||
                  !metal_info.allow_refit) {
                QTRACE("BuildRaytracingAS SKIPPED triangle update state");
                break;
              }
              encoded = cmdbuf.refitTriangleAccelerationStructure(
                  source->GetMTLAccelerationStructure(),
                  acceleration_structure, metal_info, scratch->GetMTLBuffer(),
                  scratch_offset);
              if (encoded)
                st.RetainResourceMetalObjectsForCompletion(source);
            } else {
              acceleration_structure = metal_device.newAccelerationStructure(
                  sizes.acceleration_structure_size);
              encoded = acceleration_structure.handle &&
                        cmdbuf.buildTriangleAccelerationStructure(
                            acceleration_structure, metal_info,
                            scratch->GetMTLBuffer(), scratch_offset);
            }
            primitive_count = metal_info.triangle_count;
            kind = perform_update ? "triangle update" : "triangles";
            if (auto *vertex = m_device->LookupResourceByGPUAddress(
                    cmd->geometries[0].Triangles.VertexBuffer.StartAddress))
              st.RetainResourceMetalObjectsForCompletion(vertex);
            if (cmd->geometries[0].Triangles.IndexBuffer) {
              if (auto *index = m_device->LookupResourceByGPUAddress(
                      cmd->geometries[0].Triangles.IndexBuffer))
                st.RetainResourceMetalObjectsForCompletion(index);
            }
          }
        } else if (cmd->type ==
                   D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
          auto *instance_resource =
              m_device->LookupResourceByGPUAddress(cmd->instance_descs);
          if (!instance_resource || !cmd->num_descs) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS instance resource");
            break;
          }
          uint64_t instance_offset =
              cmd->instance_descs - instance_resource->GetGPUVirtualAddress();
          uint64_t instance_bytes =
              uint64_t(cmd->num_descs) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
          if (instance_offset + instance_bytes >
              instance_resource->GetBufferByteLength()) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS descriptors out-of-bounds");
            break;
          }
          void *mapped = nullptr;
          D3D12_RANGE read_range = {
              static_cast<SIZE_T>(instance_offset),
              static_cast<SIZE_T>(instance_offset + instance_bytes)};
          HRESULT map_hr = instance_resource->Map(0, &read_range, &mapped);
          if (FAILED(map_hr) || !mapped) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS descriptors not CPU-visible "
                   "hr=0x%08x",
                   (unsigned)map_hr);
            break;
          }
          auto *d3d_instances = reinterpret_cast<
              const D3D12_RAYTRACING_INSTANCE_DESC *>(
              static_cast<const uint8_t *>(mapped) + instance_offset);
          std::vector<WMTAccelerationStructureInstanceDescriptor>
              metal_instances(cmd->num_descs);
          std::vector<obj_handle_t> instanced_structures(cmd->num_descs);
          std::vector<MTLD3D12Resource *> blas_resources(cmd->num_descs);
          std::vector<uint32_t> instance_contributions(cmd->num_descs);
          bool instances_valid = true;
          for (UINT i = 0; i < cmd->num_descs; i++) {
            const auto &source = d3d_instances[i];
            auto *blas = m_device->LookupResourceByGPUAddress(
                source.AccelerationStructure);
            if (!blas || !blas->GetMTLAccelerationStructure().handle) {
              instances_valid = false;
              break;
            }
            auto &target = metal_instances[i];
            for (uint32_t column = 0; column < 4; column++) {
              for (uint32_t row = 0; row < 3; row++) {
                target.transformation_matrix[column * 3 + row] =
                    source.Transform[row][column];
              }
            }
            target.options = source.Flags;
            target.mask = source.InstanceMask;
            // Metal's instance offset selects an intersection-function-table
            // entry. D3D12's instance contribution instead selects an SBT
            // record and is carried separately in the contributions buffer.
            target.intersection_function_table_offset = 0;
            target.acceleration_structure_index = i;
            target.user_id = source.InstanceID;
            instance_contributions[i] =
                source.InstanceContributionToHitGroupIndex;
            bottom_level_pointers.push_back(source.AccelerationStructure);
            instanced_structures[i] =
                blas->GetMTLAccelerationStructure().handle;
            blas_resources[i] = blas;
          }
          instance_resource->Unmap(0, nullptr);
          if (!instances_valid) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS missing BLAS instance");
            break;
          }
          auto metal_instance_buffer = st.MakeTransientBuffer(
              m_device, metal_instances.size() * sizeof(metal_instances[0]));
          if (!metal_instance_buffer.handle) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS descriptor allocation");
            break;
          }
          metal_instance_buffer.updateContents(
              0, metal_instances.data(),
              metal_instances.size() * sizeof(metal_instances[0]));
          const bool allow_update =
              (cmd->flags &
               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE) !=
              0;
          if (!metal_device.accelerationStructureSizesForInstances(
                  cmd->num_descs, allow_update, sizes)) {
            QTRACE("BuildRaytracingAS SKIPPED TLAS size query");
            break;
          }
          const bool perform_update =
              (cmd->flags &
               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) !=
              0;
          if (perform_update) {
            auto *source = m_device->LookupResourceByGPUAddress(
                cmd->source_acceleration_structure);
            acceleration_structure = dest->GetMTLAccelerationStructure();
            if (!source || !source->GetMTLAccelerationStructure().handle ||
                !acceleration_structure.handle || !allow_update) {
              QTRACE("BuildRaytracingAS SKIPPED TLAS update state");
              break;
            }
            encoded = cmdbuf.refitInstanceAccelerationStructure(
                source->GetMTLAccelerationStructure(), acceleration_structure,
                metal_instance_buffer, 0, cmd->num_descs,
                instanced_structures.data(), instanced_structures.size(),
                scratch->GetMTLBuffer(), scratch_offset);
            if (encoded)
              st.RetainResourceMetalObjectsForCompletion(source);
          } else {
            acceleration_structure = metal_device.newAccelerationStructure(
                sizes.acceleration_structure_size);
            encoded = acceleration_structure.handle &&
                      cmdbuf.buildInstanceAccelerationStructure(
                          acceleration_structure, metal_instance_buffer, 0,
                          cmd->num_descs, instanced_structures.data(),
                          instanced_structures.size(), allow_update,
                          scratch->GetMTLBuffer(), scratch_offset);
          }
          if (encoded) {
            st.RetainMTLObjectForCompletion(metal_instance_buffer);
            st.RetainResourceMetalObjectsForCompletion(instance_resource);
            for (auto *blas : blas_resources)
              st.RetainResourceMetalObjectsForCompletion(blas);

            uint64_t contributions_gpu_address = 0;
            auto contributions_buffer = st.MakeTransientBuffer(
                m_device,
                std::max<uint64_t>(instance_contributions.size() *
                                       sizeof(instance_contributions[0]),
                                   16),
                &contributions_gpu_address);
            if (contributions_buffer.handle) {
              contributions_buffer.updateContents(
                  0, instance_contributions.data(),
                  instance_contributions.size() *
                      sizeof(instance_contributions[0]));
            }
            struct RaytracingAccelerationStructureHeader {
              uint64_t acceleration_structure_id;
              uint64_t instance_contributions_gpu_address;
              uint64_t reserved[4];
              uint32_t indirect_dispatch[3];
              uint32_t padding;
            } header_data = {};
            static_assert(sizeof(header_data) == 64);
            header_data.acceleration_structure_id =
                acceleration_structure.gpuResourceID();
            header_data.instance_contributions_gpu_address =
                contributions_gpu_address;
            uint64_t header_gpu_address = 0;
            auto header_buffer = st.MakeTransientBuffer(
                m_device, sizeof(header_data), &header_gpu_address);
            if (header_buffer.handle)
              header_buffer.updateContents(0, &header_data,
                                           sizeof(header_data));
            if (header_buffer.handle && contributions_buffer.handle &&
                header_gpu_address) {
              dest->SetRaytracingHeaderBuffers(
                  header_buffer, header_gpu_address, contributions_buffer,
                  contributions_gpu_address);
              st.RetainMTLObjectForCompletion(header_buffer);
              st.RetainMTLObjectForCompletion(contributions_buffer);
              QTRACE("BuildRaytracingAS TLAS header gpu=0x%llx id=0x%llx "
                     "contributions=0x%llx",
                     (unsigned long long)header_gpu_address,
                     (unsigned long long)
                         header_data.acceleration_structure_id,
                     (unsigned long long)contributions_gpu_address);
            } else {
              encoded = false;
            }
          }
          primitive_count = cmd->num_descs;
          kind = perform_update ? "instance update" : "instances";
        }

        if (encoded) {
          dest->SetMTLAccelerationStructure(
              acceleration_structure, sizes.acceleration_structure_size);
          dest->SetRaytracingBuildInfo(
              cmd->type,
              cmd->type ==
                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
                  ? cmd->num_descs
                  : 0,
              bottom_level_pointers);
          st.RetainMTLObjectForCompletion(acceleration_structure);
          st.RetainResourceMetalObjectsForCompletion(dest);
          st.RetainResourceMetalObjectsForCompletion(scratch);
          QTRACE("BuildRaytracingAS encoded %s=%llu as=%llu "
                 "result_bytes=%llu scratch_bytes=%llu",
                 kind, (unsigned long long)primitive_count,
                 (unsigned long long)acceleration_structure.handle,
                 (unsigned long long)sizes.acceleration_structure_size,
                 (unsigned long long)sizes.build_scratch_buffer_size);
        } else {
          QTRACE("BuildRaytracingAS SKIPPED Metal encoder failed");
        }
        break;
      }
      case CmdType::CopyRaytracingAccelerationStructure: {
        auto *cmd = reinterpret_cast<
            const CmdCopyRaytracingAccelerationStructure *>(header);
        auto *source = m_device->LookupResourceByGPUAddress(
            cmd->source_acceleration_structure);
        auto *destination = m_device->LookupResourceByGPUAddress(
            cmd->destination_acceleration_structure);
        if (cmd->mode ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE) {
          const uint64_t bottom_level_pointer_count =
              source ? source->GetRaytracingBottomLevelPointerCount() : 0;
          const uint64_t blob_size =
              MTLD3D12Resource::SerializedAccelerationStructureBlobSize(
                  bottom_level_pointer_count);
          if (!source || !destination ||
              !source->GetMTLAccelerationStructure().handle ||
              !source->GetMTLAccelerationStructureSize() ||
              !destination->GetMTLBuffer().handle) {
            QTRACE("CopyRaytracingAS SERIALIZE skipped source=%p destination=%p",
                   (void *)source, (void *)destination);
            break;
          }
          const uint64_t destination_offset =
              cmd->destination_acceleration_structure -
              destination->GetGPUVirtualAddress();
          if (destination_offset + blob_size >
              destination->GetBufferByteLength()) {
            QTRACE("CopyRaytracingAS SERIALIZE skipped out-of-bounds");
            break;
          }
          struct MetalSharpSerializedAccelerationStructureData {
            uint64_t magic;
            uint32_t version;
            uint32_t type;
            uint64_t acceleration_structure_size;
            uint64_t reserved[4];
          } driver_data = {};
          D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER
              serialized_header = {};
          serialized_header.DriverMatchingIdentifier =
              m_device->GetRaytracingSerializationIdentifier();
          serialized_header.SerializedSizeInBytesIncludingHeader = blob_size;
          serialized_header.DeserializedSizeInBytes =
              source->GetMTLAccelerationStructureSize();
          serialized_header.NumBottomLevelAccelerationStructurePointersAfterHeader =
              bottom_level_pointer_count;
          driver_data.magic = 0x4d54534153455231ull; // "MTSASER1"
          driver_data.version = 1;
          driver_data.type = source->GetRaytracingType();
          driver_data.acceleration_structure_size =
              source->GetMTLAccelerationStructureSize();
          std::vector<uint8_t> serialized_blob(blob_size);
          std::memcpy(serialized_blob.data(), &serialized_header,
                      sizeof(serialized_header));
          const uint64_t driver_data_offset =
              sizeof(serialized_header) +
              bottom_level_pointer_count *
                  sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
          const auto &bottom_level_pointers =
              source->GetRaytracingBottomLevelPointers();
          for (uint64_t i = 0; i < bottom_level_pointer_count; i++) {
            const D3D12_GPU_VIRTUAL_ADDRESS pointer =
                i < bottom_level_pointers.size() ? bottom_level_pointers[i]
                                                 : 0;
            std::memcpy(serialized_blob.data() + sizeof(serialized_header) +
                            i * sizeof(pointer),
                        &pointer, sizeof(pointer));
          }
          std::memcpy(serialized_blob.data() + driver_data_offset,
                      &driver_data, sizeof(driver_data));
          destination->GetMTLBuffer().updateContents(
              destination_offset, serialized_blob.data(),
              serialized_blob.size());
          destination->SetSerializedAccelerationStructure(
              source->GetMTLAccelerationStructure(),
              source->GetMTLAccelerationStructureSize(),
              source->GetRaytracingType(),
              source->GetRaytracingBottomLevelPointerCount(),
              source->GetRaytracingBottomLevelPointers(),
              source->GetRaytracingInstanceContributionsBuffer(),
              source->GetRaytracingInstanceContributionsGPUAddress(),
              destination_offset, blob_size);
          st.RetainResourceMetalObjectsForCompletion(source);
          st.RetainResourceMetalObjectsForCompletion(destination);
          QTRACE("CopyRaytracingAS SERIALIZE bytes=%llu source=%p destination=%p+%llu",
                 (unsigned long long)blob_size, (void *)source,
                 (void *)destination,
                 (unsigned long long)destination_offset);
          break;
        }

        const bool deserialize =
            cmd->mode ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE;
        WMT::Reference<WMT::AccelerationStructure> copy_source;
        uint64_t copy_source_size = 0;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE copy_source_type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        uint64_t bottom_level_pointer_count = 0;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> bottom_level_pointers;
        WMT::Reference<WMT::Buffer> instance_contributions_buffer;
        uint64_t instance_contributions_gpu_address = 0;
        if (deserialize && source) {
          const uint64_t source_offset =
              cmd->source_acceleration_structure -
              source->GetGPUVirtualAddress();
          if (source->HasSerializedAccelerationStructureAt(source_offset)) {
            copy_source = source->GetSerializedAccelerationStructure();
            copy_source_size =
                source->GetSerializedAccelerationStructureSize();
            copy_source_type = source->GetSerializedRaytracingType();
            bottom_level_pointer_count =
                source->GetSerializedBottomLevelPointerCount();
            bottom_level_pointers =
                source->GetSerializedBottomLevelPointers();
            instance_contributions_buffer =
                source->GetSerializedInstanceContributionsBuffer();
            instance_contributions_gpu_address =
                source->GetSerializedInstanceContributionsGPUAddress();
          }
        } else if (source) {
          copy_source = source->GetMTLAccelerationStructure();
          copy_source_size = source->GetMTLAccelerationStructureSize();
          copy_source_type = source->GetRaytracingType();
          bottom_level_pointer_count =
              source->GetRaytracingBottomLevelPointerCount();
          bottom_level_pointers = source->GetRaytracingBottomLevelPointers();
          instance_contributions_buffer =
              source->GetRaytracingInstanceContributionsBuffer();
          instance_contributions_gpu_address =
              source->GetRaytracingInstanceContributionsGPUAddress();
        }
        const bool compact =
            cmd->mode ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT;
        if ((!compact && !deserialize &&
             cmd->mode !=
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE) ||
            !source || !destination || !copy_source.handle ||
            !copy_source_size) {
          QTRACE("CopyRaytracingAS SKIPPED mode=%u source=%p destination=%p",
                 (unsigned)cmd->mode, (void *)source, (void *)destination);
          break;
        }
        st.CloseRenderEncoder();
        auto copied = m_device->GetMTLDevice().newAccelerationStructure(
            copy_source_size);
        const bool copied_ok =
            copied.handle &&
            (compact ? cmdbuf.copyAndCompactAccelerationStructure(
                           copy_source, copied)
                     : cmdbuf.copyAccelerationStructure(
                           copy_source, copied));
        if (!copied_ok) {
          QTRACE("CopyRaytracingAS SKIPPED Metal copy failed mode=%u",
                 (unsigned)cmd->mode);
          break;
        }
        destination->SetMTLAccelerationStructure(
            copied, copy_source_size);
        destination->SetRaytracingBuildInfo(copy_source_type,
                                            bottom_level_pointer_count,
                                            bottom_level_pointers);
        if (copy_source_type ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
          struct RaytracingAccelerationStructureHeader {
            uint64_t acceleration_structure_id;
            uint64_t instance_contributions_gpu_address;
            uint64_t reserved[4];
            uint32_t indirect_dispatch[3];
            uint32_t padding;
          } header_data = {};
          static_assert(sizeof(header_data) == 64);
          header_data.acceleration_structure_id = copied.gpuResourceID();
          header_data.instance_contributions_gpu_address =
              instance_contributions_gpu_address;
          uint64_t header_gpu_address = 0;
          auto header_buffer = st.MakeTransientBuffer(
              m_device, sizeof(header_data), &header_gpu_address);
          if (!header_buffer.handle || !instance_contributions_buffer.handle ||
              !header_gpu_address) {
            QTRACE("CopyRaytracingAS SKIPPED TLAS header rebuild mode=%u",
                   (unsigned)cmd->mode);
            break;
          }
          header_buffer.updateContents(0, &header_data, sizeof(header_data));
          destination->SetRaytracingHeaderBuffers(
              header_buffer, header_gpu_address,
              instance_contributions_buffer,
              instance_contributions_gpu_address);
          st.RetainMTLObjectForCompletion(header_buffer);
          st.RetainMTLObjectForCompletion(instance_contributions_buffer);
        }
        st.RetainMTLObjectForCompletion(copied);
        st.RetainResourceMetalObjectsForCompletion(source);
        st.RetainResourceMetalObjectsForCompletion(destination);
        QTRACE("CopyRaytracingAS mode=%u allocation_bytes=%llu source=%p destination=%p",
               (unsigned)cmd->mode,
               (unsigned long long)copy_source_size,
               (void *)source, (void *)destination);
        break;
      }
      case CmdType::EmitRaytracingAccelerationStructurePostbuildInfo: {
        auto *cmd = reinterpret_cast<const
            CmdEmitRaytracingAccelerationStructurePostbuildInfo *>(header);
        auto *source = m_device->LookupResourceByGPUAddress(
            cmd->source_acceleration_structure);
        auto *dest = m_device->LookupResourceByGPUAddress(cmd->dest_buffer);
        const bool current_size_info =
            cmd->info_type ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
        const bool compacted_size_info =
            cmd->info_type ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
        const bool serialization_info =
            cmd->info_type ==
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
        if ((!current_size_info && !compacted_size_info &&
             !serialization_info) || !source ||
            !source->GetMTLAccelerationStructure().handle || !dest ||
            !dest->GetMTLBuffer().handle) {
          QTRACE("EmitRaytracingPostbuildInfo SKIPPED type=%u source=%p "
                 "dest=%p",
                 (unsigned)cmd->info_type, (void *)source, (void *)dest);
          break;
        }
        uint64_t dest_offset = cmd->dest_buffer - dest->GetGPUVirtualAddress();
        const uint64_t info_size =
            serialization_info
                ? sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC)
                : sizeof(uint64_t);
        if (dest_offset + info_size > dest->GetBufferByteLength()) {
          QTRACE("EmitRaytracingPostbuildInfo SKIPPED out-of-bounds");
          break;
        }
        uint64_t current_size = source->GetMTLAccelerationStructureSize();
        if (current_size_info) {
          dest->GetMTLBuffer().updateContents(dest_offset, &current_size,
                                              sizeof(current_size));
        } else if (serialization_info) {
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC
              serialization = {};
          serialization.SerializedSizeInBytes =
              MTLD3D12Resource::SerializedAccelerationStructureBlobSize(
                  source->GetRaytracingBottomLevelPointerCount());
          serialization.NumBottomLevelAccelerationStructurePointers =
              source->GetRaytracingBottomLevelPointerCount();
          dest->GetMTLBuffer().updateContents(dest_offset, &serialization,
                                              sizeof(serialization));
        } else {
          st.CloseRenderEncoder();
          if (!cmdbuf.writeCompactedAccelerationStructureSize(
                  source->GetMTLAccelerationStructure(), dest->GetMTLBuffer(),
                  dest_offset)) {
            QTRACE("EmitRaytracingPostbuildInfo SKIPPED compacted-size encoder");
            break;
          }
        }
        st.RetainResourceMetalObjectsForCompletion(source);
        st.RetainResourceMetalObjectsForCompletion(dest);
        QTRACE("EmitRaytracingPostbuildInfo type=%u current_allocation=%llu dest=0x%llx",
               (unsigned)cmd->info_type,
               (unsigned long long)current_size,
               (unsigned long long)cmd->dest_buffer);
        break;
      }
      case CmdType::SetPipelineState1: {
        auto *cmd = reinterpret_cast<const CmdSetPipelineState1 *>(header);
        auto pipeline = GetD3D12StateObjectRaygenComputePipeline(
            cmd->state_object);
        auto visible_table =
            GetD3D12StateObjectRaygenVisibleFunctionTable(cmd->state_object);
        auto intersection_table =
            GetD3D12StateObjectIntersectionFunctionTable(cmd->state_object);
        if (pipeline.handle && visible_table.handle) {
          st.raytracing_state = cmd->state_object;
          st.raytracing_compute_pso = pipeline;
          st.raytracing_visible_function_table = visible_table;
          st.raytracing_intersection_function_table = intersection_table;
          QTRACE("SetPipelineState1 state=%p raygen_pso=%llu table=%llu",
                 (void *)cmd->state_object,
                 (unsigned long long)pipeline.handle,
                 (unsigned long long)visible_table.handle);
        } else {
          QTRACE("SetPipelineState1 SKIPPED state=%p pipeline=%llu table=%llu",
                 (void *)cmd->state_object,
                 (unsigned long long)pipeline.handle,
                 (unsigned long long)visible_table.handle);
        }
        break;
      }
      case CmdType::DispatchRays: {
        auto *cmd = reinterpret_cast<const CmdDispatchRays *>(header);
        if (!st.raytracing_compute_pso.handle ||
            !cmd->desc.RayGenerationShaderRecord.StartAddress ||
            cmd->desc.RayGenerationShaderRecord.SizeInBytes < 32 ||
            !cmd->desc.Width || !cmd->desc.Height || !cmd->desc.Depth) {
          QTRACE("DispatchRays SKIPPED pso=%p dimensions=%ux%ux%u "
                 "raygen=0x%llx size=%llu",
                 (void *)(uintptr_t)st.raytracing_compute_pso.handle,
                 cmd->desc.Width, cmd->desc.Height,
                 cmd->desc.Depth,
                 (unsigned long long)
                     cmd->desc.RayGenerationShaderRecord.StartAddress,
                 (unsigned long long)
                     cmd->desc.RayGenerationShaderRecord.SizeInBytes);
          break;
        }
        if (!ReplayRaytracingDispatch(st, m_device, cmdbuf, cmd->desc))
          QTRACE("DispatchRays SKIPPED ray dispatch encoding failed");
        break;
      }
      case CmdType::Dispatch: {
        auto *cmd = reinterpret_cast<const CmdDispatch *>(header);
        ReplayComputeDispatch(st, m_device, cmdbuf, cmd->x, cmd->y, cmd->z,
                              "Dispatch");
        break;
      }
      case CmdType::DispatchMesh: {
        auto *cmd = reinterpret_cast<const CmdDispatchMesh *>(header);
        QTRACE("DispatchMesh groups=%ux%ux%u pso=%p", cmd->x, cmd->y, cmd->z,
               (void *)st.pso);
        st.EnsureRenderEncoder();
        st.ApplyRootBindings(m_device);
        st.BuildVertexArgumentBuffer(m_device);
        st.BuildGeometryArgumentBuffer(m_device);
        st.BuildConstantBufferTable(m_device);
        st.BuildArgumentBuffer(m_device);
        st.BindGeometryMeshBuffers();
        st.BindDirectFragmentCompleteness(m_device, "dispatch_mesh");
        if (st.EncodeNativeMeshDispatch(cmd->x, cmd->y, cmd->z)) {
          const uint64_t group_count = uint64_t(cmd->x) * cmd->y * cmd->z;
          st.pipeline_statistics.ASInvocations += group_count;
          st.pipeline_statistics.MSInvocations += group_count;
          st.pipeline_statistics.MSPrimitives += group_count;
        } else {
          QTRACE("DispatchMesh SKIPPED groups=%ux%ux%u enc_open=%d pso=%p "
                 "compiled=%d native_mesh=%d stage=%s detail=%s",
                 cmd->x, cmd->y, cmd->z, st.render_enc_open, (void *)st.pso,
                 st.pso ? st.pso->IsCompiled() : 0,
                 st.pso ? st.pso->UsesNativeMeshPipeline() : 0,
                 TraceCompileFailureStage(st.pso),
                 TraceCompileFailureDetail(st.pso));
        }
        break;
      }
      case CmdType::ExecuteIndirect: {
        auto *cmd = reinterpret_cast<const CmdExecuteIndirect *>(header);
        const auto *sig_desc = GetD3D12CommandSignatureDesc(cmd->signature);
        QTRACE("ExecuteIndirect max=%u sig=%p args=%p+%llu count=%p+%llu",
               cmd->max_command_count, (void *)cmd->signature,
               (void *)cmd->argument_buffer,
               (unsigned long long)cmd->argument_buffer_offset,
               (void *)cmd->count_buffer,
               (unsigned long long)cmd->count_buffer_offset);
        if (!sig_desc || !sig_desc->pArgumentDescs ||
            sig_desc->ByteStride == 0 || !cmd->argument_buffer) {
          QTRACE("ExecuteIndirect SKIPPED invalid signature/argument buffer");
          break;
        }

        auto *arg_res = static_cast<MTLD3D12Resource *>(cmd->argument_buffer);
        void *arg_base = nullptr;
        HRESULT map_hr = arg_res->Map(0, nullptr, &arg_base);
        if (FAILED(map_hr) || !arg_base) {
          QTRACE("ExecuteIndirect SKIPPED argument buffer not CPU-visible "
                 "hr=0x%08x",
                 (unsigned)map_hr);
          break;
        }

        uint32_t command_count = cmd->max_command_count;
        if (cmd->count_buffer) {
          auto *count_res = static_cast<MTLD3D12Resource *>(cmd->count_buffer);
          void *count_base = nullptr;
          HRESULT count_hr = count_res->Map(0, nullptr, &count_base);
          if (SUCCEEDED(count_hr) && count_base &&
              cmd->count_buffer_offset + sizeof(uint32_t) <=
                  count_res->GetBufferByteLength()) {
            uint32_t gpu_count = *reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(count_base) +
                cmd->count_buffer_offset);
            command_count = std::min(command_count, gpu_count);
            QTRACE("ExecuteIndirect count buffer value=%u clamped=%u",
                   gpu_count, command_count);
          } else {
            QTRACE("ExecuteIndirect count buffer unavailable hr=0x%08x",
                   (unsigned)count_hr);
          }
          count_res->Unmap(0, nullptr);
        }

        auto replay_indirect_draw = [&](const D3D12_DRAW_ARGUMENTS &args) {
          st.EnsureRenderEncoder();
          st.ApplyRootBindings(m_device);
          st.BuildVertexConstantBufferTable(m_device);
          st.BuildVertexArgumentBuffer(m_device);
          st.BuildConstantBufferTable(m_device);
          st.BuildArgumentBuffer(m_device);
          if (st.render_enc_open && st.arg_buf.handle) {
            uint32_t bind_index = st.BindIndexOrFallback(
                st.pso->GetPSReflection().ArgumentBufferBindIndex,
                st.kArgBufSlot);
            st.SetFragmentBufferTracked(st.arg_buf, 0, bind_index);
          }
          st.BindStaticSamplers();
          st.ApplyVertexBuffers(m_device);
          QTRACE("ExecuteIndirect DRAW v=%u i=%u start_v=%u start_i=%u "
                 "enc_open=%d",
                 args.VertexCountPerInstance, args.InstanceCount,
                 args.StartVertexLocation, args.StartInstanceLocation,
                 st.render_enc_open);
          auto safety = st.ValidateDrawSafety(
              m_device, args.VertexCountPerInstance, args.InstanceCount,
              args.StartVertexLocation, 0, args.StartInstanceLocation, false);
          if (D3D12DrawSafetySkipped(safety)) {
            st.LogDrawSafetySkip("ExecuteIndirectDraw", safety,
                                 args.VertexCountPerInstance,
                                 args.InstanceCount, args.StartVertexLocation,
                                 0, args.StartInstanceLocation, false);
            return;
          }
          if (args.InstanceCount > 0 && args.VertexCountPerInstance > 0 &&
              st.render_enc_open) {
            struct wmtcmd_render_draw draw = {};
            draw.type = WMTRenderCommandDraw;
            draw.next.set(nullptr);
            draw.primitive_type = st.GetMetalPrimitiveType();
            draw.vertex_start = args.StartVertexLocation;
            draw.vertex_count = args.VertexCountPerInstance;
            draw.base_instance = args.StartInstanceLocation;
            draw.instance_count = args.InstanceCount;
            st.BindMSCDrawParameters(
                m_device, args.VertexCountPerInstance, args.InstanceCount,
                args.StartVertexLocation, 0, args.StartInstanceLocation, false,
                WMTIndexTypeUInt16);
            st.BindMissingNonStageInVertexBuffers(m_device);
            st.BindDirectFragmentCompleteness(m_device,
                                              "execute_indirect_draw");
            if (st.EncodeRenderCommands(
                    reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                    "execute_indirect_draw")) {
              st.MarkSwapchainWorkEncoded();
              if (st.HasSwapchainRenderTarget() &&
                  TakeLogBudget(&g_swapchain_indirect_draw_logs, 128)) {
                Logger::info(str::format(
                    "M12 swapchain ExecuteIndirect DrawInstanced encoded v=",
                    args.VertexCountPerInstance, " i=", args.InstanceCount,
                    " start_v=", args.StartVertexLocation, " start_i=",
                    args.StartInstanceLocation, " pso=", (void *)st.pso,
                    " enc=", (unsigned long long)st.render_enc.handle, " ",
                    TracePsoShaderSummary(st.pso)));
              }
            }
          } else if (st.HasSwapchainRenderTarget() &&
                     TakeLogBudget(&g_swapchain_indirect_skip_logs, 48)) {
            Logger::info(str::format(
                "M12 swapchain ExecuteIndirect DrawInstanced skipped v=",
                args.VertexCountPerInstance, " i=", args.InstanceCount,
                " start_v=", args.StartVertexLocation,
                " start_i=", args.StartInstanceLocation,
                " enc_open=", st.render_enc_open, " pso=", (void *)st.pso,
                " compiled=", st.pso ? st.pso->IsCompiled() : 0,
                " stage=", TraceCompileFailureStage(st.pso),
                " detail=", TraceCompileFailureDetail(st.pso)));
          }
        };

        auto replay_indirect_draw_indexed =
            [&](const D3D12_DRAW_INDEXED_ARGUMENTS &args) {
              st.EnsureRenderEncoder();
              st.ApplyRootBindings(m_device);
              st.BuildVertexConstantBufferTable(m_device);
              st.BuildVertexArgumentBuffer(m_device);
              st.BuildConstantBufferTable(m_device);
              st.BuildArgumentBuffer(m_device);
              if (st.render_enc_open && st.arg_buf.handle) {
                uint32_t bind_index = st.BindIndexOrFallback(
                    st.pso->GetPSReflection().ArgumentBufferBindIndex,
                    st.kArgBufSlot);
                st.SetFragmentBufferTracked(st.arg_buf, 0, bind_index);
              }
              st.BindStaticSamplers();
              st.ApplyVertexBuffers(m_device);
              auto safety = st.ValidateDrawSafety(
                  m_device, args.IndexCountPerInstance, args.InstanceCount,
                  args.StartIndexLocation, args.BaseVertexLocation,
                  args.StartInstanceLocation, true);
              if (D3D12DrawSafetySkipped(safety)) {
                st.LogDrawSafetySkip(
                    "ExecuteIndirectDrawIndexed", safety,
                    args.IndexCountPerInstance, args.InstanceCount,
                    args.StartIndexLocation, args.BaseVertexLocation,
                    args.StartInstanceLocation, true);
                return;
              }
              st.LogNativeVertexResolved(
                  "ExecuteIndirectDrawIndexed", args.IndexCountPerInstance,
                  args.InstanceCount, args.StartIndexLocation,
                  args.BaseVertexLocation, args.StartInstanceLocation, true);
              if (args.InstanceCount > 0 && args.IndexCountPerInstance > 0 &&
                  st.ib.BufferLocation) {
                auto *ib_res =
                    m_device->LookupResourceByGPUAddress(st.ib.BufferLocation);
                if (!ib_res && st.ib.BufferLocation)
                  ib_res = reinterpret_cast<MTLD3D12Resource *>(
                      st.ib.BufferLocation);
                uint64_t index_buffer_offset = 0;
                if (ib_res) {
                  index_buffer_offset =
                      st.ib.BufferLocation - ib_res->GetGPUVirtualAddress();
                  index_buffer_offset +=
                      uint64_t(args.StartIndexLocation) *
                      (st.ib.Format == DXGI_FORMAT_R32_UINT ? 4ull : 2ull);
                  if (st.render_enc_open && ib_res->GetMTLBuffer().handle) {
                    st.render_enc.useResource(ib_res->GetMTLBuffer(),
                                              WMTResourceUsageRead,
                                              WMTRenderStageVertex);
                    st.RetainMTLObjectForCompletion(ib_res->GetMTLBuffer());
                  }
                }
                QTRACE("ExecuteIndirect DRAW_INDEXED idx=%u inst=%u start=%u "
                       "base=%d ib=0x%llx enc_open=%d",
                       args.IndexCountPerInstance, args.InstanceCount,
                       args.StartIndexLocation, args.BaseVertexLocation,
                       (unsigned long long)st.ib.BufferLocation,
                       st.render_enc_open);
                struct wmtcmd_render_draw_indexed draw = {};
                draw.type = WMTRenderCommandDrawIndexed;
                draw.next.set(nullptr);
                draw.primitive_type = st.GetMetalPrimitiveType();
                draw.index_type = DXGIToWMTIndexFormat(st.ib.Format);
                draw.index_count = args.IndexCountPerInstance;
                draw.index_buffer =
                    ib_res ? ib_res->GetMTLBuffer().handle : NULL_OBJECT_HANDLE;
                draw.index_buffer_offset = index_buffer_offset;
                draw.instance_count = args.InstanceCount;
                draw.base_vertex = args.BaseVertexLocation;
                draw.base_instance = args.StartInstanceLocation;
                st.BindMSCDrawParameters(
                    m_device, args.IndexCountPerInstance, args.InstanceCount,
                    args.StartIndexLocation, args.BaseVertexLocation,
                    args.StartInstanceLocation, true, draw.index_type);
                st.BindMissingNonStageInVertexBuffers(m_device);
                st.BindDirectFragmentCompleteness(
                    m_device, "execute_indirect_draw_indexed");
                if (st.render_enc_open)
                  st.EncodeRenderCommands(
                      reinterpret_cast<const wmtcmd_render_nop *>(&draw),
                      "execute_indirect_draw_indexed");
              } else {
                QTRACE("ExecuteIndirect DRAW_INDEXED SKIPPED idx=%u inst=%u "
                       "ib=0x%llx enc_open=%d",
                       args.IndexCountPerInstance, args.InstanceCount,
                       (unsigned long long)st.ib.BufferLocation,
                       st.render_enc_open);
              }
            };

        const auto arg_len = arg_res->GetBufferByteLength();
        const uint8_t *arg_bytes = static_cast<const uint8_t *>(arg_base);
        for (uint32_t ci = 0; ci < command_count; ci++) {
          uint64_t record_off =
              cmd->argument_buffer_offset + uint64_t(ci) * sig_desc->ByteStride;
          uint64_t cursor = 0;
          bool valid_record = true;
          for (uint32_t ai = 0; ai < sig_desc->NumArgumentDescs; ai++) {
            const auto &arg_desc = sig_desc->pArgumentDescs[ai];
            auto can_read = [&](uint64_t size) {
              bool ok = record_off + cursor + size <= arg_len &&
                        cursor + size <= sig_desc->ByteStride;
              if (!ok) {
                QTRACE("ExecuteIndirect cmd=%u arg=%u type=%u out-of-bounds "
                       "cursor=%llu size=%llu stride=%u len=%llu",
                       ci, ai, (unsigned)arg_desc.Type,
                       (unsigned long long)cursor, (unsigned long long)size,
                       sig_desc->ByteStride, (unsigned long long)arg_len);
              }
              return ok;
            };
            const uint8_t *src = arg_bytes + record_off + cursor;
            switch (arg_desc.Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
              if (!can_read(sizeof(D3D12_DRAW_ARGUMENTS))) {
                valid_record = false;
                break;
              }
              D3D12_DRAW_ARGUMENTS args = {};
              memcpy(&args, src, sizeof(args));
              cursor += sizeof(args);
              replay_indirect_draw(args);
              break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
              if (!can_read(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS))) {
                valid_record = false;
                break;
              }
              D3D12_DRAW_INDEXED_ARGUMENTS args = {};
              memcpy(&args, src, sizeof(args));
              cursor += sizeof(args);
              replay_indirect_draw_indexed(args);
              break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
              if (!can_read(sizeof(D3D12_DISPATCH_ARGUMENTS))) {
                valid_record = false;
                break;
              }
              {
                D3D12_DISPATCH_ARGUMENTS args = {};
                memcpy(&args, src, sizeof(args));
                cursor += sizeof(args);
                ReplayComputeDispatch(
                    st, m_device, cmdbuf, args.ThreadGroupCountX,
                    args.ThreadGroupCountY, args.ThreadGroupCountZ,
                    "ExecuteIndirect DISPATCH");
              }
              break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS:
              if (!can_read(sizeof(D3D12_DISPATCH_RAYS_DESC))) {
                valid_record = false;
                break;
              }
              {
                D3D12_DISPATCH_RAYS_DESC args = {};
                memcpy(&args, src, sizeof(args));
                cursor += sizeof(args);
                QTRACE("ExecuteIndirect DISPATCH_RAYS dimensions=%ux%ux%u",
                       args.Width, args.Height, args.Depth);
                if (!st.raytracing_compute_pso.handle ||
                    !args.RayGenerationShaderRecord.StartAddress ||
                    args.RayGenerationShaderRecord.SizeInBytes < 32 ||
                    !args.Width || !args.Height || !args.Depth ||
                    !ReplayRaytracingDispatch(st, m_device, cmdbuf, args)) {
                  QTRACE("ExecuteIndirect DISPATCH_RAYS skipped invalid state or encoding failure");
                }
              }
              break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH:
              if (!can_read(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS))) {
                valid_record = false;
                break;
              }
              {
                D3D12_DISPATCH_MESH_ARGUMENTS args = {};
                memcpy(&args, src, sizeof(args));
                cursor += sizeof(args);
                QTRACE("ExecuteIndirect DISPATCH_MESH groups=%ux%ux%u",
                       args.ThreadGroupCountX, args.ThreadGroupCountY,
                       args.ThreadGroupCountZ);
                st.EnsureRenderEncoder();
                st.ApplyRootBindings(m_device);
                st.BuildVertexArgumentBuffer(m_device);
                st.BuildGeometryArgumentBuffer(m_device);
                st.BuildConstantBufferTable(m_device);
                st.BuildArgumentBuffer(m_device);
                st.BindGeometryMeshBuffers();
                st.BindDirectFragmentCompleteness(
                    m_device, "execute_indirect_dispatch_mesh");
                st.EncodeNativeMeshDispatch(args.ThreadGroupCountX,
                                            args.ThreadGroupCountY,
                                            args.ThreadGroupCountZ);
              }
              break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
              if (!can_read(sizeof(D3D12_VERTEX_BUFFER_VIEW))) {
                valid_record = false;
                break;
              }
              D3D12_VERTEX_BUFFER_VIEW view = {};
              memcpy(&view, src, sizeof(view));
              cursor += sizeof(view);
              uint32_t slot = arg_desc.VertexBuffer.Slot;
              if (slot < ReplayState::kVertexBufferSlotCount)
                st.vbs[slot] = view;
              QTRACE("ExecuteIndirect VBV slot=%u gpu=0x%llx size=%u stride=%u",
                     slot, (unsigned long long)view.BufferLocation,
                     view.SizeInBytes, view.StrideInBytes);
              break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
              if (!can_read(sizeof(D3D12_INDEX_BUFFER_VIEW))) {
                valid_record = false;
                break;
              }
              memcpy(&st.ib, src, sizeof(st.ib));
              cursor += sizeof(D3D12_INDEX_BUFFER_VIEW);
              QTRACE("ExecuteIndirect IBV gpu=0x%llx size=%u format=%u",
                     (unsigned long long)st.ib.BufferLocation,
                     st.ib.SizeInBytes, st.ib.Format);
              break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
              uint32_t byte_count = arg_desc.Constant.Num32BitValuesToSet * 4;
              if (!can_read(byte_count)) {
                valid_record = false;
                break;
              }
              uint32_t idx = arg_desc.Constant.RootParameterIndex;
              uint32_t local_off =
                  arg_desc.Constant.DestOffsetIn32BitValues * 4;
              if (idx < st.kRootParameterSlotCount &&
                  local_off + byte_count <= st.kRootConstantBytes) {
                uint32_t off = idx * st.kRootConstantBytes + local_off;
                memcpy(st.root_constants_buf + off, src, byte_count);
                memcpy(st.comp_constants_buf + off, src, byte_count);
                st.root_constant_offsets[idx] = idx * st.kRootConstantBytes;
                st.comp_constant_offsets[idx] = idx * st.kRootConstantBytes;
                st.root_constant_sizes[idx] = std::max(
                    st.root_constant_sizes[idx], local_off + byte_count);
                st.comp_constant_sizes[idx] = std::max(
                    st.comp_constant_sizes[idx], local_off + byte_count);
                st.root_constant_set[idx] = true;
                st.comp_constant_set[idx] = true;
              }
              cursor += byte_count;
              break;
            }
            case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
            case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
              if (!can_read(sizeof(D3D12_GPU_VIRTUAL_ADDRESS))) {
                valid_record = false;
                break;
              }
              D3D12_GPU_VIRTUAL_ADDRESS addr = 0;
              memcpy(&addr, src, sizeof(addr));
              cursor += sizeof(addr);
              uint32_t idx = 0;
              if (arg_desc.Type ==
                  D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW) {
                idx = arg_desc.ConstantBufferView.RootParameterIndex;
              } else if (arg_desc.Type ==
                         D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW) {
                idx = arg_desc.ShaderResourceView.RootParameterIndex;
              } else {
                idx = arg_desc.UnorderedAccessView.RootParameterIndex;
              }
              if (idx < st.kRootParameterSlotCount) {
                if (arg_desc.Type ==
                    D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW) {
                  st.root_cbvs[idx] = addr;
                  st.comp_cbvs[idx] = addr;
                  st.root_cbv_set[idx] = true;
                  st.comp_cbv_set[idx] = true;
                } else if (arg_desc.Type ==
                           D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW) {
                  st.root_srvs[idx] = addr;
                  st.comp_srvs[idx] = addr;
                  st.root_srv_set[idx] = true;
                  st.comp_srv_set[idx] = true;
                } else {
                  st.root_uavs[idx] = addr;
                  st.comp_uavs[idx] = addr;
                  st.root_uav_set[idx] = true;
                  st.comp_uav_set[idx] = true;
                }
              }
              QTRACE("ExecuteIndirect root addr type=%u idx=%u gpu=0x%llx",
                     (unsigned)arg_desc.Type, idx, (unsigned long long)addr);
              break;
            }
            default:
              QTRACE("ExecuteIndirect unsupported arg type=%u",
                     (unsigned)arg_desc.Type);
              valid_record = false;
              break;
            }
            if (!valid_record)
              break;
          }
        }
        arg_res->Unmap(0, nullptr);
        break;
      }
      case CmdType::CopyBufferRegion: {
        auto *cmd = reinterpret_cast<const CmdCopyBufferRegion *>(header);
        QTRACE("CopyBufferRegion dst=%p +%llu src=%p +%llu bytes=%llu",
               (void *)cmd->dst, (unsigned long long)cmd->dst_offset,
               (void *)cmd->src, (unsigned long long)cmd->src_offset,
               (unsigned long long)cmd->byte_count);
        if (cmd->dst && cmd->src) {
          st.CloseRenderEncoder();
          auto *dst_res = static_cast<MTLD3D12Resource *>(cmd->dst);
          auto *src_res = static_cast<MTLD3D12Resource *>(cmd->src);
          if (dst_res->GetMTLBuffer().handle &&
              src_res->GetMTLBuffer().handle) {
            auto source_serialized_acceleration_structure =
                src_res->GetSerializedAccelerationStructure();
            const uint64_t source_serialized_size =
                src_res->GetSerializedAccelerationStructureSize();
            const auto source_serialized_type =
                src_res->GetSerializedRaytracingType();
            const uint64_t source_serialized_pointer_count =
                src_res->GetSerializedBottomLevelPointerCount();
            const auto source_serialized_pointers =
                src_res->GetSerializedBottomLevelPointers();
            auto source_serialized_contributions =
                src_res->GetSerializedInstanceContributionsBuffer();
            const uint64_t source_serialized_contributions_gpu_address =
                src_res->GetSerializedInstanceContributionsGPUAddress();
            const uint64_t existing_serialized_offset =
                dst_res->GetSerializedAccelerationStructureOffset();
            const uint64_t existing_serialized_blob_size =
                dst_res->GetSerializedAccelerationStructureBlobSize();
            if (dst_res->GetSerializedAccelerationStructure().handle &&
                cmd->dst_offset <
                    existing_serialized_offset +
                        existing_serialized_blob_size &&
                existing_serialized_offset <
                    cmd->dst_offset + cmd->byte_count) {
              dst_res->ClearSerializedAccelerationStructure();
            }
            const uint64_t source_serialized_offset =
                src_res->GetSerializedAccelerationStructureOffset();
            const uint64_t source_serialized_blob_size =
                src_res->GetSerializedAccelerationStructureBlobSize();
            if (source_serialized_acceleration_structure.handle &&
                cmd->src_offset <= source_serialized_offset &&
                source_serialized_offset + source_serialized_blob_size <=
                    cmd->src_offset + cmd->byte_count) {
              const uint64_t copied_serialized_offset =
                  cmd->dst_offset +
                  (source_serialized_offset - cmd->src_offset);
              if (copied_serialized_offset + source_serialized_blob_size <=
                  dst_res->GetBufferByteLength()) {
                dst_res->SetSerializedAccelerationStructure(
                    source_serialized_acceleration_structure,
                    source_serialized_size, source_serialized_type,
                    source_serialized_pointer_count,
                    source_serialized_pointers,
                    source_serialized_contributions,
                    source_serialized_contributions_gpu_address,
                    copied_serialized_offset,
                    source_serialized_blob_size);
              }
            }
            st.RetainResourceMetalObjectsForCompletion(dst_res);
            st.RetainResourceMetalObjectsForCompletion(src_res);
            auto blit = cmdbuf.blitCommandEncoder();
            ENC_CREATE("blit_copybuf", blit.handle);
            ScopedMetalEncoderEnd blit_guard{blit, "blit_copybuf"};
            if (!blit.handle) {
              QTRACE("CopyBufferRegion: FAILED to create blit encoder");
              break;
            }
            struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {};
            copy.type = WMTBlitCommandCopyFromBufferToBuffer;
            copy.next.set(nullptr);
            copy.src = src_res->GetMTLBuffer().handle;
            copy.src_offset = cmd->src_offset;
            copy.dst = dst_res->GetMTLBuffer().handle;
            copy.dst_offset = cmd->dst_offset;
            copy.copy_length = cmd->byte_count;
            blit.encodeCommands(
                reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
            EndMetalEncoder(blit, "blit_copybuf");
          }
        }
        break;
      }
      case CmdType::CopyTextureRegion: {
        auto *cmd = reinterpret_cast<const CmdCopyTextureRegion *>(header);
        auto *dst_res = static_cast<MTLD3D12Resource *>(cmd->dst_resource);
        auto *src_res = static_cast<MTLD3D12Resource *>(cmd->src_resource);
        QTRACE(
            "CopyTextureRegion dst=%p(%p) src=%p(%p) dst_type=%u src_type=%u",
            (void *)dst_res,
            dst_res ? (void *)dst_res->GetMTLTexture().handle : nullptr,
            (void *)src_res,
            src_res ? (void *)src_res->GetMTLTexture().handle : nullptr,
            cmd->dst_type, cmd->src_type);
        if (!dst_res || !src_res)
          break;

        QTRACE("CopyTextureRegion dst=%p src=%p dst_type=%u src_type=%u",
               (void *)dst_res, (void *)src_res, cmd->dst_type, cmd->src_type);

        st.CloseRenderEncoder();
        auto blit = cmdbuf.blitCommandEncoder();
        ENC_CREATE("blit_copytex", blit.handle);
        ScopedMetalEncoderEnd blit_guard{blit, "blit_copytex"};
        if (!blit.handle) {
          QTRACE("CopyTextureRegion: FAILED to create blit encoder");
          break;
        }

        bool src_is_buffer =
            (cmd->src_type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);
        bool dst_is_buffer =
            (cmd->dst_type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT);

        auto src_tex = src_res->GetMTLTexture();
        auto dst_tex = dst_res->GetMTLTexture();
        auto src_buf = src_res->GetMTLBuffer();
        auto dst_buf = dst_res->GetMTLBuffer();
        st.RetainResourceMetalObjectsForCompletion(src_res);
        st.RetainResourceMetalObjectsForCompletion(dst_res);

        if (!src_is_buffer && !src_tex.handle)
          src_is_buffer = (src_buf.handle != 0);
        if (!dst_is_buffer && !dst_tex.handle)
          dst_is_buffer = (dst_buf.handle != 0);

        QTRACE("CopyTextureRegion src_tex=%llu src_buf=%llu dst_tex=%llu "
               "dst_buf=%llu src_buf_flag=%d dst_buf_flag=%d",
               (unsigned long long)src_tex.handle,
               (unsigned long long)src_buf.handle,
               (unsigned long long)dst_tex.handle,
               (unsigned long long)dst_buf.handle, src_is_buffer,
               dst_is_buffer);

        D3D12_RESOURCE_DESC src_desc = {};
        D3D12_RESOURCE_DESC dst_desc = {};
        src_res->GetDesc(&src_desc);
        dst_res->GetDesc(&dst_desc);
        UINT src_level =
            src_is_buffer ? 0
                          : SubresourceMipLevel(src_desc, cmd->src_subresource);
        UINT src_slice =
            src_is_buffer
                ? 0
                : SubresourceArraySlice(src_desc, cmd->src_subresource);
        UINT dst_level =
            dst_is_buffer ? 0
                          : SubresourceMipLevel(dst_desc, cmd->dst_subresource);
        UINT dst_slice =
            dst_is_buffer
                ? 0
                : SubresourceArraySlice(dst_desc, cmd->dst_subresource);

        UINT copy_w, copy_h, copy_d;
        if (cmd->has_src_box) {
          copy_w = cmd->src_box.right - cmd->src_box.left;
          copy_h = cmd->src_box.bottom - cmd->src_box.top;
          copy_d = cmd->src_box.back - cmd->src_box.front;
        } else {
          if (src_is_buffer && cmd->src_footprint_width &&
              cmd->src_footprint_height) {
            copy_w = cmd->src_footprint_width;
            copy_h = cmd->src_footprint_height;
            copy_d = cmd->src_footprint_depth ? cmd->src_footprint_depth : 1;
          } else if (dst_is_buffer && cmd->dst_footprint_width &&
                     cmd->dst_footprint_height) {
            copy_w = cmd->dst_footprint_width;
            copy_h = cmd->dst_footprint_height;
            copy_d = cmd->dst_footprint_depth ? cmd->dst_footprint_depth : 1;
          } else if (!dst_is_buffer && dst_tex.handle) {
            copy_w = MipSize(dst_desc.Width, dst_level);
            copy_h = MipSize(dst_desc.Height, dst_level);
            copy_d = dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         ? MipSize(dst_desc.DepthOrArraySize, dst_level)
                         : 1;
          } else if (!src_is_buffer && src_tex.handle) {
            copy_w = MipSize(src_desc.Width, src_level);
            copy_h = MipSize(src_desc.Height, src_level);
            copy_d = src_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         ? MipSize(src_desc.DepthOrArraySize, src_level)
                         : 1;
          } else {
            copy_w = 1;
            copy_h = 1;
            copy_d = 1;
          }
          if (copy_w == 0)
            copy_w = 1;
          if (copy_h == 0)
            copy_h = 1;
        }

        if (src_is_buffer && !dst_is_buffer && dst_tex.handle) {
          UINT row_pitch = cmd->src_footprint_row_pitch;
          if (row_pitch == 0)
            row_pitch = copy_w * 4;
          DXGI_FORMAT src_format =
              cmd->src_footprint_format != DXGI_FORMAT_UNKNOWN
                  ? cmd->src_footprint_format
                  : dst_desc.Format;
          UINT src_x = cmd->has_src_box ? cmd->src_box.left : 0;
          UINT src_y = cmd->has_src_box ? cmd->src_box.top : 0;
          UINT src_z = cmd->has_src_box ? cmd->src_box.front : 0;
          uint64_t src_offset = FootprintOffset(
              cmd->src_offset, row_pitch, cmd->src_footprint_height, src_format,
              src_x, src_y, src_z);
          uint64_t rows_per_image = FootprintRows(
              cmd->src_footprint_height ? cmd->src_footprint_height : copy_h,
              src_format);
          struct wmtcmd_blit_copy_from_buffer_to_texture copy = {};
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.next.set(nullptr);
          copy.src = src_buf.handle;
          copy.src_offset = src_offset;
          copy.bytes_per_row = row_pitch;
          copy.bytes_per_image = row_pitch * rows_per_image;
          copy.size = {copy_w, copy_h, copy_d};
          copy.dst = dst_tex.handle;
          copy.slice = dst_slice;
          copy.level = dst_level;
          copy.origin = {cmd->dst_x, cmd->dst_y, cmd->dst_z};
          QTRACE("CopyTextureRegion buffer->texture dst_level=%u dst_slice=%u "
                 "offset=%llu base=%llu row_pitch=%u image_pitch=%llu "
                 "format=%u box_origin=%ux%ux%u size=%ux%ux%u",
                 dst_level, dst_slice, (unsigned long long)src_offset,
                 (unsigned long long)cmd->src_offset, row_pitch,
                 (unsigned long long)copy.bytes_per_image, (unsigned)src_format,
                 src_x, src_y, src_z, copy_w, copy_h, copy_d);
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        } else if (!src_is_buffer && dst_is_buffer && src_tex.handle) {
          UINT row_pitch = cmd->dst_footprint_row_pitch;
          if (row_pitch == 0)
            row_pitch = copy_w * 4;
          DXGI_FORMAT dst_format =
              cmd->dst_footprint_format != DXGI_FORMAT_UNKNOWN
                  ? cmd->dst_footprint_format
                  : src_desc.Format;
          uint64_t dst_offset = FootprintOffset(
              cmd->dst_offset, row_pitch, cmd->dst_footprint_height, dst_format,
              cmd->dst_x, cmd->dst_y, cmd->dst_z);
          uint64_t rows_per_image = FootprintRows(
              cmd->dst_footprint_height ? cmd->dst_footprint_height : copy_h,
              dst_format);
          struct wmtcmd_blit_copy_from_texture_to_buffer copy = {};
          copy.type = WMTBlitCommandCopyFromTextureToBuffer;
          copy.next.set(nullptr);
          copy.src = src_tex.handle;
          copy.slice = src_slice;
          copy.level = src_level;
          UINT src_x = cmd->has_src_box ? cmd->src_box.left : 0;
          UINT src_y = cmd->has_src_box ? cmd->src_box.top : 0;
          UINT src_z = cmd->has_src_box ? cmd->src_box.front : 0;
          copy.origin = {src_x, src_y, src_z};
          copy.size = {copy_w, copy_h, copy_d};
          copy.dst = dst_buf.handle;
          copy.offset = dst_offset;
          copy.bytes_per_row = row_pitch;
          copy.bytes_per_image = row_pitch * rows_per_image;
          QTRACE("CopyTextureRegion texture->buffer src_level=%u src_slice=%u "
                 "offset=%llu base=%llu row_pitch=%u image_pitch=%llu "
                 "format=%u dst_origin=%ux%ux%u size=%ux%ux%u",
                 src_level, src_slice, (unsigned long long)dst_offset,
                 (unsigned long long)cmd->dst_offset, row_pitch,
                 (unsigned long long)copy.bytes_per_image, (unsigned)dst_format,
                 cmd->dst_x, cmd->dst_y, cmd->dst_z, copy_w, copy_h, copy_d);
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        } else if (!src_is_buffer && !dst_is_buffer && src_tex.handle &&
                   dst_tex.handle) {
          struct wmtcmd_blit_copy_from_texture_to_texture copy = {};
          copy.type = WMTBlitCommandCopyFromTextureToTexture;
          copy.next.set(nullptr);
          copy.src = src_tex.handle;
          copy.src_slice = src_slice;
          copy.src_level = src_level;
          UINT src_x = cmd->has_src_box ? cmd->src_box.left : 0;
          UINT src_y = cmd->has_src_box ? cmd->src_box.top : 0;
          UINT src_z = cmd->has_src_box ? cmd->src_box.front : 0;
          copy.src_origin = {src_x, src_y, src_z};
          copy.src_size = {copy_w, copy_h, copy_d};
          copy.dst = dst_tex.handle;
          copy.dst_slice = dst_slice;
          copy.dst_level = dst_level;
          copy.dst_origin = {cmd->dst_x, cmd->dst_y, cmd->dst_z};
          QTRACE("CopyTextureRegion texture->texture src_level=%u src_slice=%u "
                 "dst_level=%u dst_slice=%u size=%ux%ux%u",
                 src_level, src_slice, dst_level, dst_slice, copy_w, copy_h,
                 copy_d);
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        } else {
          QTRACE("CopyTextureRegion: unhandled buffer-to-buffer or null "
                 "resources");
        }

        QTRACE("CopyTextureRegion: blit.endEncoding src_buf=%d dst_buf=%d w=%u "
               "h=%u d=%u",
               src_is_buffer, dst_is_buffer, copy_w, copy_h, copy_d);
        EndMetalEncoder(blit, "blit_copytex");
        break;
      }
      case CmdType::CopyResource: {
        auto *cmd = reinterpret_cast<const CmdCopyResource *>(header);
        auto *dst_res = static_cast<MTLD3D12Resource *>(cmd->dst);
        auto *src_res = static_cast<MTLD3D12Resource *>(cmd->src);
        if (!dst_res || !src_res)
          break;
        st.CloseRenderEncoder();

        if (dst_res->GetMTLBuffer().handle && src_res->GetMTLBuffer().handle) {
          st.RetainResourceMetalObjectsForCompletion(dst_res);
          st.RetainResourceMetalObjectsForCompletion(src_res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_copyres_buf", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit, "blit_copyres_buf"};
          if (!blit.handle) {
            QTRACE("CopyResource buffer: FAILED to create blit encoder");
            break;
          }
          struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {};
          copy.type = WMTBlitCommandCopyFromBufferToBuffer;
          copy.next.set(nullptr);
          copy.src = src_res->GetMTLBuffer().handle;
          copy.src_offset = 0;
          copy.dst = dst_res->GetMTLBuffer().handle;
          copy.dst_offset = 0;
          D3D12_RESOURCE_DESC src_desc;
          src_res->GetDesc(&src_desc);
          copy.copy_length = src_desc.Width;
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_copyres_buf");
        } else if (dst_res->GetMTLTexture().handle &&
                   src_res->GetMTLTexture().handle) {
          st.RetainResourceMetalObjectsForCompletion(dst_res);
          st.RetainResourceMetalObjectsForCompletion(src_res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_copyres_tex", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit, "blit_copyres_tex"};
          if (!blit.handle) {
            QTRACE("CopyResource texture: FAILED to create blit encoder");
            break;
          }
          D3D12_RESOURCE_DESC src_desc;
          src_res->GetDesc(&src_desc);
          struct wmtcmd_blit_copy_from_texture_to_texture copy = {};
          copy.type = WMTBlitCommandCopyFromTextureToTexture;
          copy.next.set(nullptr);
          copy.src = src_res->GetMTLTexture().handle;
          copy.src_slice = 0;
          copy.src_level = 0;
          copy.src_origin = {0, 0, 0};
          copy.src_size = {src_desc.Width, src_desc.Height, 1};
          copy.dst = dst_res->GetMTLTexture().handle;
          copy.dst_slice = 0;
          copy.dst_level = 0;
          copy.dst_origin = {0, 0, 0};
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_copyres_tex");
        }
        break;
      }
      case CmdType::ResolveSubresource: {
        auto *cmd = reinterpret_cast<const CmdResolveSubresource *>(header);
        auto *dst_res = static_cast<MTLD3D12Resource *>(cmd->dst);
        auto *src_res = static_cast<MTLD3D12Resource *>(cmd->src);
        if (!dst_res || !src_res)
          break;

        D3D12_RESOURCE_DESC src_desc = {};
        D3D12_RESOURCE_DESC dst_desc = {};
        src_res->GetDesc(&src_desc);
        dst_res->GetDesc(&dst_desc);
        QTRACE("ResolveSubresource dst=%p sub=%u src=%p sub=%u fmt=%u mode=%u "
               "rect=%u dst=%u,%u",
               (void *)dst_res, cmd->dst_sub, (void *)src_res, cmd->src_sub,
               cmd->format, cmd->mode, cmd->has_src_rect, cmd->dst_x,
               cmd->dst_y);

        if (dst_res->IsSamplerFeedback() &&
            cmd->mode == D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK) {
          const uint32_t feedback_mip =
              dst_desc.Format ==
                      DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
                  ? 0
                  : SubresourceMipLevel(dst_desc, cmd->dst_sub);
          const auto *feedback_layout =
              dst_res->GetSamplerFeedbackLevelLayout(feedback_mip);
          if (!feedback_layout || cmd->format != DXGI_FORMAT_R8_UINT ||
              !src_res->GetMTLTexture().handle ||
              !dst_res->GetMTLBuffer().handle) {
            QTRACE("ResolveSubresource sampler-feedback encode rejected src_fmt=%u dst_fmt=%u src_tex=%llu dst_buf=%llu",
                   (unsigned)src_desc.Format, (unsigned)cmd->format,
                   (unsigned long long)src_res->GetMTLTexture().handle,
                   (unsigned long long)dst_res->GetMTLBuffer().handle);
            break;
          }
          const uint32_t src_mip =
              SubresourceMipLevel(src_desc, cmd->src_sub);
          const uint32_t src_slice =
              SubresourceArraySlice(src_desc, cmd->src_sub);
          const uint32_t dst_slice =
              SubresourceArraySlice(dst_desc, cmd->dst_sub);
          const uint32_t full_w = MipSize(src_desc.Width, src_mip);
          const uint32_t full_h =
              MipSize(std::max<UINT>(src_desc.Height, 1), src_mip);
          const uint32_t src_x = cmd->has_src_rect
                                     ? std::max<LONG>(cmd->src_rect.left, 0)
                                     : 0;
          const uint32_t src_y = cmd->has_src_rect
                                     ? std::max<LONG>(cmd->src_rect.top, 0)
                                     : 0;
          const uint32_t copy_w =
              std::min<uint32_t>(
                  cmd->has_src_rect
                      ? std::max<LONG>(cmd->src_rect.right -
                                           cmd->src_rect.left,
                                       0)
                      : full_w,
                  feedback_layout->width -
                      std::min<uint32_t>(cmd->dst_x,
                                         feedback_layout->width));
          const uint32_t copy_h =
              std::min<uint32_t>(
                  cmd->has_src_rect
                      ? std::max<LONG>(cmd->src_rect.bottom -
                                           cmd->src_rect.top,
                                       0)
                      : full_h,
                  feedback_layout->height -
                      std::min<uint32_t>(cmd->dst_y,
                                         feedback_layout->height));
          if (!copy_w || !copy_h)
            break;
          const uint64_t bytes_per_image =
              uint64_t(feedback_layout->row_pitch) *
              feedback_layout->height;
          const uint64_t dst_offset =
              feedback_layout->offset +
              uint64_t(dst_slice) * bytes_per_image +
              uint64_t(cmd->dst_y) * feedback_layout->row_pitch + cmd->dst_x;
          st.CloseRenderEncoder();
          st.RetainResourceMetalObjectsForCompletion(src_res);
          st.RetainResourceMetalObjectsForCompletion(dst_res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_encode_sampler_feedback", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit,
                                           "blit_encode_sampler_feedback"};
          if (!blit.handle)
            break;
          struct wmtcmd_blit_copy_from_texture_to_buffer copy = {};
          copy.type = WMTBlitCommandCopyFromTextureToBuffer;
          copy.next.set(nullptr);
          copy.src = src_res->GetMTLTexture().handle;
          copy.slice = src_slice;
          copy.level = src_mip;
          copy.origin = {src_x, src_y, 0};
          copy.size = {copy_w, copy_h, 1};
          copy.dst = dst_res->GetMTLBuffer().handle;
          copy.offset = dst_offset;
          copy.bytes_per_row = feedback_layout->row_pitch;
          copy.bytes_per_image = bytes_per_image;
          blit.encodeCommands(
              reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_encode_sampler_feedback");
          QTRACE("ResolveSubresource sampler-feedback encode mip=%u slice=%u rect=%u,%u %ux%u dst=%u,%u",
                 feedback_mip, dst_slice, src_x, src_y, copy_w, copy_h,
                 cmd->dst_x, cmd->dst_y);
          break;
        }

        if (src_res->IsSamplerFeedback() &&
            cmd->mode == D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK) {
          const uint32_t feedback_mip =
              src_desc.Format ==
                      DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
                  ? 0
                  : SubresourceMipLevel(src_desc, cmd->src_sub);
          const auto *feedback_layout =
              src_res->GetSamplerFeedbackLevelLayout(feedback_mip);
          if (!feedback_layout || cmd->format != DXGI_FORMAT_R8_UINT ||
              !src_res->GetMTLBuffer().handle ||
              !dst_res->GetMTLTexture().handle) {
            QTRACE("ResolveSubresource sampler-feedback decode rejected src_fmt=%u dst_fmt=%u src_buf=%llu dst_tex=%llu",
                   (unsigned)src_desc.Format, (unsigned)cmd->format,
                   (unsigned long long)src_res->GetMTLBuffer().handle,
                   (unsigned long long)dst_res->GetMTLTexture().handle);
            break;
          }
          const uint32_t map_width = feedback_layout->width;
          const uint32_t map_height = feedback_layout->height;
          const uint32_t row_pitch = feedback_layout->row_pitch;
          const uint32_t src_x = cmd->has_src_rect
                                     ? std::max<LONG>(cmd->src_rect.left, 0)
                                     : 0;
          const uint32_t src_y = cmd->has_src_rect
                                     ? std::max<LONG>(cmd->src_rect.top, 0)
                                     : 0;
          const uint32_t copy_w = cmd->has_src_rect
                                      ? std::min<uint32_t>(
                                            map_width -
                                                std::min(src_x, map_width),
                                            std::max<LONG>(
                                                cmd->src_rect.right -
                                                    cmd->src_rect.left,
                                                0))
                                      : map_width;
          const uint32_t copy_h = cmd->has_src_rect
                                      ? std::min<uint32_t>(
                                            map_height -
                                                std::min(src_y, map_height),
                                            std::max<LONG>(
                                                cmd->src_rect.bottom -
                                                    cmd->src_rect.top,
                                                0))
                                      : map_height;
          if (!copy_w || !copy_h)
            break;
          const uint32_t dst_mip =
              SubresourceMipLevel(dst_desc, cmd->dst_sub);
          const uint32_t dst_slice =
              SubresourceArraySlice(dst_desc, cmd->dst_sub);
          const uint32_t src_slice =
              SubresourceArraySlice(src_desc, cmd->src_sub);
          const uint64_t bytes_per_image = uint64_t(row_pitch) * map_height;
          const uint64_t src_offset =
              feedback_layout->offset +
              uint64_t(src_slice) * bytes_per_image +
              uint64_t(src_y) * row_pitch + src_x;
          st.CloseRenderEncoder();
          st.RetainResourceMetalObjectsForCompletion(src_res);
          st.RetainResourceMetalObjectsForCompletion(dst_res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_decode_sampler_feedback", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit,
                                           "blit_decode_sampler_feedback"};
          if (!blit.handle)
            break;
          struct wmtcmd_blit_copy_from_buffer_to_texture copy = {};
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.next.set(nullptr);
          copy.src = src_res->GetMTLBuffer().handle;
          copy.src_offset = src_offset;
          copy.bytes_per_row = row_pitch;
          copy.bytes_per_image = bytes_per_image;
          copy.size = {copy_w, copy_h, 1};
          copy.dst = dst_res->GetMTLTexture().handle;
          copy.slice = dst_slice;
          copy.level = dst_mip;
          copy.origin = {cmd->dst_x, cmd->dst_y, 0};
          blit.encodeCommands(
              reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_decode_sampler_feedback");
          QTRACE("ResolveSubresource sampler-feedback min-mip decode physical=%ux%u rect=%u,%u %ux%u dst=%u,%u",
                 map_width, map_height, src_x, src_y, copy_w, copy_h,
                 cmd->dst_x, cmd->dst_y);
          break;
        }

        if (!src_res->GetMTLTexture().handle ||
            !dst_res->GetMTLTexture().handle) {
          QTRACE("ResolveSubresource SKIPPED non-texture resource");
          break;
        }

        uint32_t src_mip = SubresourceMipLevel(src_desc, cmd->src_sub);
        uint32_t src_slice = SubresourceArraySlice(src_desc, cmd->src_sub);
        uint32_t dst_mip = SubresourceMipLevel(dst_desc, cmd->dst_sub);
        uint32_t dst_slice = SubresourceArraySlice(dst_desc, cmd->dst_sub);
        uint32_t full_w = MipSize(src_desc.Width, src_mip);
        uint32_t full_h =
            MipSize(src_desc.Height ? src_desc.Height : 1, src_mip);

        bool full_rect = !cmd->has_src_rect ||
                         (cmd->src_rect.left == 0 && cmd->src_rect.top == 0 &&
                          (uint32_t)cmd->src_rect.right == full_w &&
                          (uint32_t)cmd->src_rect.bottom == full_h);
        bool full_dst = cmd->dst_x == 0 && cmd->dst_y == 0;
        bool multisample = src_desc.SampleDesc.Count > 1;
        st.RetainResourceMetalObjectsForCompletion(src_res);
        st.RetainResourceMetalObjectsForCompletion(dst_res);

        if (multisample && full_rect && full_dst) {
          st.CloseRenderEncoder();
          WMTRenderPassInfo rp = {};
          WMT::InitializeRenderPassInfo(rp);
          rp.colors[0].texture = src_res->GetMTLTexture().handle;
          rp.colors[0].load_action = WMTLoadActionLoad;
          rp.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
          rp.colors[0].level = src_mip;
          rp.colors[0].slice = src_slice;
          rp.colors[0].resolve_texture = dst_res->GetMTLTexture().handle;
          rp.colors[0].resolve_level = dst_mip;
          rp.colors[0].resolve_slice = dst_slice;
          auto enc = cmdbuf.renderCommandEncoder(rp);
          ENC_CREATE("render_resolve", enc.handle);
          ScopedMetalEncoderEnd enc_guard{enc, "render_resolve"};
          EndMetalEncoder(enc, "render_resolve");
        } else if (!multisample) {
          st.CloseRenderEncoder();
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_resolve_copy", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit, "blit_resolve_copy"};
          if (!blit.handle) {
            QTRACE("ResolveSubresource: FAILED to create blit encoder");
            break;
          }
          uint32_t src_x = cmd->has_src_rect ? cmd->src_rect.left : 0;
          uint32_t src_y = cmd->has_src_rect ? cmd->src_rect.top : 0;
          uint32_t copy_w =
              cmd->has_src_rect
                  ? std::max<LONG>(0, cmd->src_rect.right - cmd->src_rect.left)
                  : full_w;
          uint32_t copy_h =
              cmd->has_src_rect
                  ? std::max<LONG>(0, cmd->src_rect.bottom - cmd->src_rect.top)
                  : full_h;
          struct wmtcmd_blit_copy_from_texture_to_texture copy = {};
          copy.type = WMTBlitCommandCopyFromTextureToTexture;
          copy.next.set(nullptr);
          copy.src = src_res->GetMTLTexture().handle;
          copy.src_slice = src_slice;
          copy.src_level = src_mip;
          copy.src_origin = {src_x, src_y, 0};
          copy.src_size = {copy_w, copy_h, 1};
          copy.dst = dst_res->GetMTLTexture().handle;
          copy.dst_slice = dst_slice;
          copy.dst_level = dst_mip;
          copy.dst_origin = {cmd->dst_x, cmd->dst_y, 0};
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_resolve_copy");
        } else {
          QTRACE("ResolveSubresource SKIPPED partial multisample resolve "
                 "rect=%u dst=%u,%u",
                 cmd->has_src_rect, cmd->dst_x, cmd->dst_y);
        }
        break;
      }
      case CmdType::WriteBufferImmediate: {
        auto *cmd = reinterpret_cast<const CmdWriteBufferImmediate *>(header);
        auto *entries = reinterpret_cast<const CmdWriteBufferImmediateEntry *>(
            reinterpret_cast<const uint8_t *>(cmd) +
            sizeof(CmdWriteBufferImmediate) -
            sizeof(CmdWriteBufferImmediateEntry));
        st.CloseRenderEncoder();
        for (uint32_t i = 0; i < cmd->count; i++) {
          D3D12_GPU_VIRTUAL_ADDRESS dest = entries[i].parameter.Dest;
          uint32_t value = entries[i].parameter.Value;
          auto *res = m_device->LookupResourceByGPUAddress(dest);
          QTRACE("WriteBufferImmediate[%u] dest=0x%llx value=0x%08x mode=%u "
                 "res=%p",
                 i, (unsigned long long)dest, value, entries[i].mode,
                 (void *)res);
          if (!res || !res->GetMTLBuffer().handle) {
            QTRACE("WriteBufferImmediate[%u] SKIPPED unresolved buffer", i);
            continue;
          }

          uint64_t dst_offset = dest - res->GetGPUVirtualAddress();
          void *mapped = nullptr;
          HRESULT map_hr = res->Map(0, nullptr, &mapped);
          if (SUCCEEDED(map_hr) && mapped &&
              dst_offset + sizeof(value) <= res->GetBufferByteLength()) {
            memcpy(static_cast<uint8_t *>(mapped) + dst_offset, &value,
                   sizeof(value));
            res->Unmap(0, nullptr);
            continue;
          }

          WMTBufferInfo buf_info = {};
          buf_info.length = sizeof(value);
          buf_info.options = WMTResourceStorageModeShared;
          auto staging = m_device->GetDXMTDevice().device().newBuffer(buf_info);
          if (!staging.handle) {
            QTRACE("WriteBufferImmediate[%u] SKIPPED staging allocation failed",
                   i);
            continue;
          }
          staging.updateContents(0, &value, sizeof(value));
          st.RetainMTLObjectForCompletion(staging);
          st.RetainResourceMetalObjectsForCompletion(res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_writeimm", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit, "blit_writeimm"};
          if (!blit.handle) {
            QTRACE(
                "WriteBufferImmediate[%u] SKIPPED blit encoder create failed",
                i);
            continue;
          }
          struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {};
          copy.type = WMTBlitCommandCopyFromBufferToBuffer;
          copy.next.set(nullptr);
          copy.src = staging.handle;
          copy.src_offset = 0;
          copy.dst = res->GetMTLBuffer().handle;
          copy.dst_offset = dst_offset;
          copy.copy_length = sizeof(value);
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
          EndMetalEncoder(blit, "blit_writeimm");
        }
        break;
      }
      case CmdType::SetPipelineState: {
        auto *cmd = reinterpret_cast<const CmdSetPipelineState *>(header);
        auto *next_pso = static_cast<MTLD3D12PipelineState *>(cmd->pso);
        DXGI_FORMAT next_dsv_format = st.EffectiveDSVFormatForPSO(next_pso);
        if (st.render_enc_open &&
            ((next_dsv_format == DXGI_FORMAT_UNKNOWN) !=
                 !st.render_enc_has_dsv ||
             (st.render_enc_has_dsv &&
              st.render_enc_dsv_format != next_dsv_format))) {
          QTRACE("SetPipelineState closing render encoder for dsv transition "
                 "current_has=%u current_fmt=%u next_fmt=%u",
                 st.render_enc_has_dsv ? 1u : 0u,
                 (unsigned)st.render_enc_dsv_format, (unsigned)next_dsv_format);
          st.CloseRenderEncoder();
        }
        st.pso = next_pso;
        QTRACE(
            "SetPipelineState pso=%p compiled=%d compute=%d stage=%s detail=%s",
            (void *)st.pso, st.pso ? st.pso->IsCompiled() : 0,
            st.pso ? st.pso->IsCompute() : 0, TraceCompileFailureStage(st.pso),
            TraceCompileFailureDetail(st.pso));
        if (st.render_enc_open && st.pso && st.pso->IsCompiled() &&
            st.pso->GetRenderPSO().handle) {
          st.render_enc.setRenderPipelineState(st.pso->GetRenderPSO());
          st.RetainMTLObjectForCompletion(st.pso->GetRenderPSO());
          if (st.pso->GetDepthStencilState().handle) {
            st.render_enc.setDepthStencilState(st.pso->GetDepthStencilState());
            st.RetainMTLObjectForCompletion(st.pso->GetDepthStencilState());
          }
          st.ApplyFixedFunctionState();
        }
        break;
      }
      case CmdType::ResourceBarrier: {
        auto *cmd = reinterpret_cast<const CmdResourceBarrier *>(header);
        QTRACE("ResourceBarrier count=%u", cmd->count);
        for (uint32_t i = 0; i < cmd->count; i++) {
          const auto &barrier = cmd->barriers[i];
          if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            QTRACE("  barrier[%u] transition res=%p sub=%u before=0x%x "
                   "after=0x%x flags=0x%x",
                   i, (void *)barrier.Transition.pResource,
                   barrier.Transition.Subresource,
                   barrier.Transition.StateBefore,
                   barrier.Transition.StateAfter, barrier.Flags);
          } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            QTRACE("  barrier[%u] uav res=%p flags=0x%x", i,
                   (void *)barrier.UAV.pResource, barrier.Flags);
          } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            QTRACE("  barrier[%u] alias before=%p after=%p flags=0x%x", i,
                   (void *)barrier.Aliasing.pResourceBefore,
                   (void *)barrier.Aliasing.pResourceAfter, barrier.Flags);
          } else {
            QTRACE("  barrier[%u] type=%u flags=0x%x", i, barrier.Type,
                   barrier.Flags);
          }
        }

        st.CloseRenderEncoder();
        if (m_barrier_event.handle) {
          uint64_t seq = ++m_barrier_seq;
          QTRACE("ResourceBarrier queue-order seq=%llu event=%llu",
                 (unsigned long long)seq,
                 (unsigned long long)m_barrier_event.handle);
          cmdbuf.encodeSignalEvent(m_barrier_event, seq);
          cmdbuf.encodeWaitForEvent(m_barrier_event, seq);
        } else {
          QTRACE("ResourceBarrier queue-order skipped: no event");
        }
        break;
      }
      case CmdType::EnhancedBarrier: {
        auto *cmd = reinterpret_cast<const CmdEnhancedBarrier *>(header);
        QTRACE("EnhancedBarrier groups=%u global=%u buffer=%u texture=%u",
               cmd->group_count, cmd->global_barrier_count,
               cmd->buffer_barrier_count, cmd->texture_barrier_count);
        st.CloseRenderEncoder();
        if (m_barrier_event.handle) {
          uint64_t seq = ++m_barrier_seq;
          QTRACE("EnhancedBarrier queue-order seq=%llu event=%llu",
                 (unsigned long long)seq,
                 (unsigned long long)m_barrier_event.handle);
          cmdbuf.encodeSignalEvent(m_barrier_event, seq);
          cmdbuf.encodeWaitForEvent(m_barrier_event, seq);
        } else {
          QTRACE("EnhancedBarrier queue-order skipped: no event");
        }
        break;
      }
      case CmdType::OMSetRenderTargets: {
        auto *cmd = reinterpret_cast<const CmdOMSetRenderTargets *>(header);
        st.CloseRenderEncoder();
        st.rt_count = cmd->rt_count;
        QTRACE("OMSetRenderTargets count=%u single=%u has_dsv=%u",
               cmd->rt_count, cmd->single_handle ? 1 : 0, cmd->has_dsv ? 1 : 0);
        for (uint32_t i = 0; i < cmd->rt_count && i < 8; i++) {
          st.rt_handles[i] = cmd->rts[i];
          auto *desc =
              reinterpret_cast<const D3D12Descriptor *>(st.rt_handles[i].ptr);
          auto *res =
              desc ? static_cast<MTLD3D12Resource *>(desc->resource) : nullptr;
          QTRACE(
              "OMSetRenderTargets rt[%u] handle=0x%llx desc=%p res=%p tex=%llu",
              i, (unsigned long long)st.rt_handles[i].ptr, (void *)desc,
              (void *)res,
              res ? (unsigned long long)res->GetMTLTexture().handle : 0ull);
          if (res && res->IsSwapchainBackBuffer()) {
            Logger::info(str::format(
                "M12 OMSetRenderTargets swapchain idx=",
                res->SwapchainBackBufferIndex(), " slot=", i,
                " handle=", (unsigned long long)st.rt_handles[i].ptr,
                " tex=", (unsigned long long)res->GetMTLTexture().handle));
          }
        }
        st.has_dsv = cmd->has_dsv;
        if (cmd->has_dsv) {
          st.dsv_handle = cmd->dsv;
          auto *desc =
              reinterpret_cast<const D3D12Descriptor *>(st.dsv_handle.ptr);
          auto *res =
              desc ? static_cast<MTLD3D12Resource *>(desc->resource) : nullptr;
          QTRACE("OMSetRenderTargets dsv handle=0x%llx desc=%p res=%p tex=%llu",
                 (unsigned long long)st.dsv_handle.ptr, (void *)desc,
                 (void *)res,
                 res ? (unsigned long long)res->GetMTLTexture().handle : 0ull);
        }
        break;
      }
      case CmdType::ClearRenderTargetView: {
        auto *cmd = reinterpret_cast<const CmdClearRTV *>(header);
        st.CloseRenderEncoder();

        WMTRenderPassInfo rp = {};
        for (uint32_t i = 0; i < 8; i++) {
          rp.colors[i].texture = NULL_OBJECT_HANDLE;
          rp.colors[i].load_action = WMTLoadActionDontCare;
          rp.colors[i].store_action = WMTStoreActionDontCare;
        }
        rp.depth.texture = NULL_OBJECT_HANDLE;
        rp.depth.load_action = WMTLoadActionDontCare;
        rp.depth.store_action = WMTStoreActionDontCare;
        rp.stencil.texture = NULL_OBJECT_HANDLE;
        rp.stencil.load_action = WMTLoadActionDontCare;
        rp.stencil.store_action = WMTStoreActionDontCare;

        {
          auto *desc = reinterpret_cast<const D3D12Descriptor *>(cmd->rtv.ptr);
          if (desc && desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            QTRACE("ClearRenderTargetView handle=0x%llx desc=%p res=%p "
                   "tex=%llu color=%f,%f,%f,%f",
                   (unsigned long long)cmd->rtv.ptr, (void *)desc, (void *)res,
                   (unsigned long long)res->GetMTLTexture().handle,
                   cmd->color[0], cmd->color[1], cmd->color[2], cmd->color[3]);
            auto tex = res->GetMTLTexture();
            if (tex.handle) {
              rp.colors[0].texture = tex.handle;
              rp.colors[0].level = RTVMipLevel(desc);
              rp.colors[0].slice = RTVArraySlice(desc);
              rp.render_target_array_length = static_cast<uint8_t>(
                  std::min<uint16_t>(RTVArrayLength(desc), UINT8_MAX));
              st.RetainMTLObjectForCompletion(tex);
              rp.colors[0].load_action = WMTLoadActionClear;
              rp.colors[0].store_action = WMTStoreActionStore;
              rp.colors[0].clear_color = {cmd->color[0], cmd->color[1],
                                          cmd->color[2], cmd->color[3]};
              if (res->IsSwapchainBackBuffer())
                st.MarkSwapchainWorkEncoded(res);
              if (res->IsSwapchainBackBuffer() &&
                  TakeLogBudget(&g_swapchain_clear_logs, 24)) {
                Logger::info(str::format(
                    "M12 swapchain ClearRTV backbuffer=",
                    res->SwapchainBackBufferIndex(),
                    " tex=", (unsigned long long)res->GetMTLTexture().handle,
                    " color=", cmd->color[0], ",", cmd->color[1], ",",
                    cmd->color[2], ",", cmd->color[3]));
              }
            }
          }
        }

        auto enc = cmdbuf.renderCommandEncoder(rp);
        ENC_CREATE("render_clearrtv", enc.handle);
        ScopedMetalEncoderEnd enc_guard{enc, "render_clearrtv"};
        EndMetalEncoder(enc, "render_clearrtv");
        break;
      }
      case CmdType::ClearDepthStencilView: {
        auto *cmd = reinterpret_cast<const CmdClearDSV *>(header);
        st.CloseRenderEncoder();

        WMTRenderPassInfo rp = {};
        for (uint32_t i = 0; i < 8; i++) {
          rp.colors[i].texture = NULL_OBJECT_HANDLE;
          rp.colors[i].load_action = WMTLoadActionDontCare;
          rp.colors[i].store_action = WMTStoreActionDontCare;
        }

        rp.depth.texture = NULL_OBJECT_HANDLE;
        rp.depth.load_action = WMTLoadActionDontCare;
        rp.depth.store_action = WMTStoreActionDontCare;
        rp.stencil.texture = NULL_OBJECT_HANDLE;
        rp.stencil.load_action = WMTLoadActionDontCare;
        rp.stencil.store_action = WMTStoreActionDontCare;

        {
          auto *desc = reinterpret_cast<const D3D12Descriptor *>(cmd->dsv.ptr);
          if (desc && desc->resource) {
            auto *res = static_cast<MTLD3D12Resource *>(desc->resource);
            auto tex = res->GetMTLTexture();
            if (tex.handle) {
              rp.depth.texture = tex.handle;
              rp.depth.level = DSVMipLevel(desc);
              rp.depth.slice = DSVArraySlice(desc);
              rp.render_target_array_length = static_cast<uint8_t>(
                  std::min<uint16_t>(DSVArrayLength(desc), UINT8_MAX));
              st.RetainMTLObjectForCompletion(tex);
              rp.depth.load_action = (cmd->flags & D3D12_CLEAR_FLAG_DEPTH)
                                         ? WMTLoadActionClear
                                         : WMTLoadActionLoad;
              rp.depth.store_action = WMTStoreActionStore;
              if (cmd->flags & D3D12_CLEAR_FLAG_DEPTH)
                rp.depth.clear_depth = cmd->depth;
              if (DSVHasStencil(desc)) {
                rp.stencil.texture = tex.handle;
                rp.stencil.level = DSVMipLevel(desc);
                rp.stencil.slice = DSVArraySlice(desc);
                rp.stencil.load_action = (cmd->flags & D3D12_CLEAR_FLAG_STENCIL)
                                             ? WMTLoadActionClear
                                             : WMTLoadActionLoad;
                rp.stencil.store_action = WMTStoreActionStore;
                if (cmd->flags & D3D12_CLEAR_FLAG_STENCIL)
                  rp.stencil.clear_stencil = cmd->stencil;
              }
              QTRACE("ClearDepthStencilView handle=0x%llx flags=0x%x "
                     "stencil_attached=%d depth=%f stencil=%u",
                     (unsigned long long)cmd->dsv.ptr, cmd->flags,
                     DSVHasStencil(desc), cmd->depth, cmd->stencil);
            }
          }
        }

        auto enc = cmdbuf.renderCommandEncoder(rp);
        ENC_CREATE("render_cleardsv", enc.handle);
        ScopedMetalEncoderEnd enc_guard{enc, "render_cleardsv"};
        EndMetalEncoder(enc, "render_cleardsv");
        break;
      }
      case CmdType::ClearUnorderedAccessView: {
        auto *cmd = reinterpret_cast<const CmdClearUAV *>(header);
        st.CloseRenderEncoder();
        auto *desc =
            reinterpret_cast<const D3D12Descriptor *>(cmd->cpu_handle.ptr);
        auto *res = cmd->resource
                        ? static_cast<MTLD3D12Resource *>(cmd->resource)
                        : (desc && desc->resource
                               ? static_cast<MTLD3D12Resource *>(desc->resource)
                               : nullptr);
        const uint8_t *clear_pattern =
            reinterpret_cast<const uint8_t *>(cmd->values);
        bool uniform_byte_clear = true;
        for (uint32_t i = 1; i < 16; i++)
          uniform_byte_clear &= clear_pattern[i] == clear_pattern[0];
        bool zero_clear = uniform_byte_clear && clear_pattern[0] == 0;
        QTRACE("ClearUnorderedAccessView%s cpu=0x%llx gpu=0x%llx res=%p "
               "desc=%p zero=%d",
               cmd->is_float ? "Float" : "Uint",
               (unsigned long long)cmd->cpu_handle.ptr,
               (unsigned long long)cmd->gpu_handle.ptr, (void *)res,
               (void *)desc, zero_clear);
        if (!res || !res->GetMTLBuffer().handle) {
          QTRACE("ClearUnorderedAccessView SKIPPED non-buffer or missing "
                 "resource");
          break;
        }

        uint64_t clear_offset = desc ? UAVBufferByteOffset(desc) : 0;
        uint64_t clear_length =
            desc ? UAVBufferByteLength(desc, res) : res->GetBufferByteLength();
        clear_length = std::min(clear_length,
                                res->GetBufferByteLength() > clear_offset
                                    ? res->GetBufferByteLength() - clear_offset
                                    : 0);
        if (!clear_length) {
          QTRACE(
              "ClearUnorderedAccessView SKIPPED empty range off=%llu len=%llu",
              (unsigned long long)clear_offset,
              (unsigned long long)clear_length);
          break;
        }

        if (uniform_byte_clear) {
          st.RetainResourceMetalObjectsForCompletion(res);
          auto blit = cmdbuf.blitCommandEncoder();
          ENC_CREATE("blit_clearuav", blit.handle);
          ScopedMetalEncoderEnd blit_guard{blit, "blit_clearuav"};
          if (!blit.handle) {
            QTRACE(
                "ClearUnorderedAccessView SKIPPED blit encoder create failed");
            break;
          }
          struct wmtcmd_blit_fillbuffer fill = {};
          fill.type = WMTBlitCommandFillBuffer;
          fill.next.set(nullptr);
          fill.buffer = res->GetMTLBuffer().handle;
          fill.offset = clear_offset;
          fill.length = clear_length;
          fill.value = clear_pattern[0];
          blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&fill));
          EndMetalEncoder(blit, "blit_clearuav");
          break;
        }

        void *mapped = nullptr;
        HRESULT map_hr = res->Map(0, nullptr, &mapped);
        if (FAILED(map_hr) || !mapped) {
          QTRACE("ClearUnorderedAccessView SKIPPED nonzero clear not "
                 "CPU-visible hr=0x%08x",
                 (unsigned)map_hr);
          break;
        }
        uint8_t *dst = static_cast<uint8_t *>(mapped) + clear_offset;
        for (uint64_t off = 0; off < clear_length; off++)
          dst[off] = clear_pattern[off & 15];
        res->Unmap(0, nullptr);
        QTRACE("ClearUnorderedAccessView CPU pattern clear off=%llu len=%llu",
               (unsigned long long)clear_offset,
               (unsigned long long)clear_length);
        break;
      }
      case CmdType::BeginQuery: {
        auto *cmd = reinterpret_cast<const CmdQuery *>(header);
        auto *heap = static_cast<MTLD3D12QueryHeap *>(cmd->heap);
        QTRACE("BeginQuery heap=%p type=%u index=%u", (void *)heap,
               (unsigned)cmd->type, cmd->index);
        if (heap && cmd->index < heap->GetCount()) {
          if (cmd->type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1)
            heap->BeginPipelineStatistics1(cmd->index,
                                           st.pipeline_statistics);
          else
            heap->GetData()[cmd->index] = 0;
        }
        break;
      }
      case CmdType::EndQuery: {
        auto *cmd = reinterpret_cast<const CmdQuery *>(header);
        auto *heap = static_cast<MTLD3D12QueryHeap *>(cmd->heap);
        QTRACE("EndQuery heap=%p type=%u index=%u", (void *)heap,
               (unsigned)cmd->type, cmd->index);
        if (heap && cmd->index < heap->GetCount()) {
          if (cmd->type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
            heap->EndPipelineStatistics1(cmd->index, st.pipeline_statistics);
          } else {
            uint64_t value = 1;
            if (cmd->type == D3D12_QUERY_TYPE_TIMESTAMP)
              value = m_barrier_seq + cmd->index + 1;
            heap->GetData()[cmd->index] = value;
          }
        }
        break;
      }
      case CmdType::ResolveQueryData: {
        auto *cmd = reinterpret_cast<const CmdResolveQueryData *>(header);
        auto *heap = static_cast<MTLD3D12QueryHeap *>(cmd->heap);
        auto *dst = static_cast<MTLD3D12Resource *>(cmd->dst_buffer);
        size_t stride = QueryResultStride(cmd->type);
        size_t bytes = stride * cmd->query_count;
        QTRACE("ResolveQueryData heap=%p type=%u start=%u count=%u dst=%p "
               "off=%llu stride=%zu bytes=%zu",
               (void *)heap, (unsigned)cmd->type, cmd->start_index,
               cmd->query_count, (void *)dst,
               (unsigned long long)cmd->dst_offset, stride, bytes);
        if (!heap || !dst || !bytes)
          break;

        if (cmd->type == D3D12_QUERY_TYPE_TIMESTAMP &&
            dst->GetMTLBuffer().handle &&
            cmd->start_index + cmd->query_count <= heap->GetCount() &&
            cmdbuf.writeTimestampResults(dst->GetMTLBuffer(), cmd->dst_offset,
                                         cmd->query_count)) {
          st.RetainResourceMetalObjectsForCompletion(dst);
          QTRACE("ResolveQueryData scheduled GPU-end timestamp results "
                 "count=%u dst=%llu off=%llu",
                 cmd->query_count,
                 (unsigned long long)dst->GetMTLBuffer().handle,
                 (unsigned long long)cmd->dst_offset);
          break;
        }

        std::vector<uint8_t> results(bytes, 0);
        for (uint32_t i = 0; i < cmd->query_count; i++) {
          uint64_t value = 0;
          uint32_t heap_index = cmd->start_index + i;
          if (heap_index < heap->GetCount()) {
            if (cmd->type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS1) {
              auto *statistics =
                  heap->GetPipelineStatistics1Data(heap_index);
              if (statistics)
                memcpy(results.data() + i * stride, statistics,
                       sizeof(*statistics));
              continue;
            }
            value = heap->GetData()[heap_index];
          }
          if ((cmd->type == D3D12_QUERY_TYPE_OCCLUSION ||
               cmd->type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) &&
              value == 0)
            value = 1;
          if (cmd->type == D3D12_QUERY_TYPE_TIMESTAMP && value == 0)
            value = m_barrier_seq + heap_index + 1;
          memcpy(results.data() + i * stride, &value,
                 std::min(stride, sizeof(value)));
        }

        void *mapped = nullptr;
        HRESULT map_hr = dst->Map(0, nullptr, &mapped);
        if (SUCCEEDED(map_hr) && mapped &&
            cmd->dst_offset + bytes <= dst->GetBufferByteLength()) {
          memcpy(static_cast<uint8_t *>(mapped) + cmd->dst_offset,
                 results.data(), bytes);
          dst->Unmap(0, nullptr);
          break;
        }

        WMTBufferInfo buf_info = {};
        buf_info.length = bytes;
        buf_info.options = WMTResourceStorageModeShared;
        auto staging = m_device->GetDXMTDevice().device().newBuffer(buf_info);
        if (!staging.handle || !dst->GetMTLBuffer().handle) {
          QTRACE("ResolveQueryData SKIPPED staging=%llu dst_buf=%llu",
                 (unsigned long long)staging.handle,
                 (unsigned long long)dst->GetMTLBuffer().handle);
          break;
        }
        staging.updateContents(0, results.data(), bytes);
        st.RetainMTLObjectForCompletion(staging);
        st.RetainResourceMetalObjectsForCompletion(dst);
        auto blit = cmdbuf.blitCommandEncoder();
        ENC_CREATE("blit_query", blit.handle);
        ScopedMetalEncoderEnd blit_guard{blit, "blit_query"};
        if (!blit.handle) {
          QTRACE("ResolveQueryData SKIPPED blit encoder create failed");
          break;
        }
        struct wmtcmd_blit_copy_from_buffer_to_buffer copy = {};
        copy.type = WMTBlitCommandCopyFromBufferToBuffer;
        copy.next.set(nullptr);
        copy.src = staging.handle;
        copy.src_offset = 0;
        copy.dst = dst->GetMTLBuffer().handle;
        copy.dst_offset = cmd->dst_offset;
        copy.copy_length = bytes;
        blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        EndMetalEncoder(blit, "blit_query");
        break;
      }
      case CmdType::RSSetViewports: {
        auto *cmd = reinterpret_cast<const CmdRSSetViewports *>(header);
        auto *vps = reinterpret_cast<const D3D12_VIEWPORT *>(
            reinterpret_cast<const uint8_t *>(cmd) + sizeof(CmdRSSetViewports) -
            sizeof(D3D12_VIEWPORT));
        st.viewport_count = cmd->count > 16 ? 16 : cmd->count;
        for (uint32_t i = 0; i < st.viewport_count; i++)
          st.viewports[i] = vps[i];
        if (st.render_enc_open) {
          for (uint32_t i = 0; i < st.viewport_count; i++) {
            WMTViewport vp = {(double)vps[i].TopLeftX, (double)vps[i].TopLeftY,
                              (double)vps[i].Width,    (double)vps[i].Height,
                              vps[i].MinDepth,         vps[i].MaxDepth};
            st.render_enc.setViewport(vp);
          }
        }
        break;
      }
      case CmdType::RSSetScissorRects: {
        auto *cmd = reinterpret_cast<const CmdRSSetScissorRects *>(header);
        auto *rects = reinterpret_cast<const D3D12_RECT *>(
            reinterpret_cast<const uint8_t *>(cmd) +
            sizeof(CmdRSSetScissorRects) - sizeof(D3D12_RECT));
        st.scissor_count = cmd->count > 16 ? 16 : cmd->count;
        for (uint32_t i = 0; i < st.scissor_count; i++)
          st.scissor_rects[i] = rects[i];
        if (st.render_enc_open && st.scissor_count > 0) {
          const auto &rect = st.scissor_rects[0];
          LONG left = std::max<LONG>(0, rect.left);
          LONG top = std::max<LONG>(0, rect.top);
          LONG right = std::max<LONG>(left, rect.right);
          LONG bottom = std::max<LONG>(top, rect.bottom);
          st.render_enc.setScissorRect({(uint64_t)left, (uint64_t)top,
                                        (uint64_t)(right - left),
                                        (uint64_t)(bottom - top)});
        }
        break;
      }
      case CmdType::IASetPrimitiveTopology: {
        auto *cmd = reinterpret_cast<const CmdIASetPrimitiveTopology *>(header);
        st.topology = cmd->topology;
        break;
      }
      case CmdType::SetGraphicsRootSignature: {
        auto *cmd = reinterpret_cast<const CmdSetRootSignature *>(header);
        auto *next_root_sig =
            static_cast<MTLD3D12RootSignature *>(cmd->root_sig);
        if (st.graphics_root_sig != next_root_sig) {
          st.ResetGraphicsRootBindings();
          st.graphics_root_sig = next_root_sig;
        }
        break;
      }
      case CmdType::SetGraphicsRoot32BitConstants: {
        auto *cmd = reinterpret_cast<const CmdSetRoot32BitConstants *>(header);
        QTRACE("SetGraphicsRoot32BitConstants idx=%u count=%u",
               cmd->root_param_index, cmd->count);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          uint32_t sz = cmd->count * 4;
          uint32_t local_off = cmd->dst_offset * 4;
          uint32_t off =
              cmd->root_param_index * st.kRootConstantBytes + local_off;
          if (local_off + sz <= st.kRootConstantBytes &&
              off + sz <= sizeof(st.root_constants_buf)) {
            memcpy(st.root_constants_buf + off, cmd->data, sz);
            st.root_constant_offsets[cmd->root_param_index] =
                cmd->root_param_index * st.kRootConstantBytes;
            st.root_constant_sizes[cmd->root_param_index] = std::max(
                st.root_constant_sizes[cmd->root_param_index], local_off + sz);
            st.root_constant_set[cmd->root_param_index] = true;
          } else {
            QTRACE("SetGraphicsRoot32BitConstants idx=%u overflow local_off=%u "
                   "size=%u",
                   cmd->root_param_index, local_off, sz);
          }
        }
        break;
      }
      case CmdType::SetGraphicsRootConstantBufferView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.root_cbvs[cmd->root_param_index] = cmd->address;
          st.root_cbv_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetGraphicsRootShaderResourceView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.root_srvs[cmd->root_param_index] = cmd->address;
          st.root_srv_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetGraphicsRootUnorderedAccessView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.root_uavs[cmd->root_param_index] = cmd->address;
          st.root_uav_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetGraphicsRootDescriptorTable: {
        auto *cmd = reinterpret_cast<const CmdSetRootDescriptorTable *>(header);
        QTRACE("SetGraphicsRootDescriptorTable idx=%u handle=0x%llx",
               cmd->root_param_index,
               (unsigned long long)cmd->base_descriptor.ptr);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.root_tables[cmd->root_param_index] = cmd->base_descriptor;
          st.root_table_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetComputeRootSignature: {
        auto *cmd = reinterpret_cast<const CmdSetRootSignature *>(header);
        st.compute_root_sig =
            static_cast<MTLD3D12RootSignature *>(cmd->root_sig);
        break;
      }
      case CmdType::SetComputeRoot32BitConstants: {
        auto *cmd = reinterpret_cast<const CmdSetRoot32BitConstants *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          uint32_t sz = cmd->count * 4;
          uint32_t local_off = cmd->dst_offset * 4;
          uint32_t off =
              cmd->root_param_index * st.kRootConstantBytes + local_off;
          if (local_off + sz <= st.kRootConstantBytes &&
              off + sz <= sizeof(st.comp_constants_buf)) {
            memcpy(st.comp_constants_buf + off, cmd->data, sz);
            st.comp_constant_offsets[cmd->root_param_index] =
                cmd->root_param_index * st.kRootConstantBytes;
            st.comp_constant_sizes[cmd->root_param_index] = std::max(
                st.comp_constant_sizes[cmd->root_param_index], local_off + sz);
            st.comp_constant_set[cmd->root_param_index] = true;
          } else {
            QTRACE("SetComputeRoot32BitConstants idx=%u overflow local_off=%u "
                   "size=%u",
                   cmd->root_param_index, local_off, sz);
          }
        }
        break;
      }
      case CmdType::SetComputeRootConstantBufferView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.comp_cbvs[cmd->root_param_index] = cmd->address;
          st.comp_cbv_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetComputeRootShaderResourceView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.comp_srvs[cmd->root_param_index] = cmd->address;
          st.comp_srv_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetComputeRootUnorderedAccessView: {
        auto *cmd = reinterpret_cast<const CmdSetRootCBV *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.comp_uavs[cmd->root_param_index] = cmd->address;
          st.comp_uav_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::SetComputeRootDescriptorTable: {
        auto *cmd = reinterpret_cast<const CmdSetRootDescriptorTable *>(header);
        if (cmd->root_param_index < st.kRootParameterSlotCount) {
          st.comp_tables[cmd->root_param_index] = cmd->base_descriptor;
          st.comp_table_set[cmd->root_param_index] = true;
        }
        break;
      }
      case CmdType::IASetVertexBuffers: {
        auto *cmd = reinterpret_cast<const CmdIASetVertexBuffers *>(header);
        auto *views = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW *>(
            reinterpret_cast<const uint8_t *>(cmd) +
            sizeof(CmdIASetVertexBuffers) - sizeof(D3D12_VERTEX_BUFFER_VIEW));
        for (uint32_t i = 0; i < cmd->count; i++) {
          uint32_t slot = cmd->start_slot + i;
          if (slot >= ReplayState::kVertexBufferSlotCount) {
            QTRACE("IASetVertexBuffers: skip slot=%u outside Metal-backed slot "
                   "cap %u",
                   slot, ReplayState::kVertexBufferSlotCount);
            continue;
          }
          st.vbs[cmd->start_slot + i] = views[i];
          QTRACE("IASetVertexBuffers: slot=%u gpu=0x%llx size=%u stride=%u",
                 slot, (unsigned long long)views[i].BufferLocation,
                 views[i].SizeInBytes, views[i].StrideInBytes);
        }
        break;
      }
      case CmdType::IASetIndexBuffer: {
        auto *cmd = reinterpret_cast<const CmdIASetIndexBuffer *>(header);
        st.ib = cmd->view;
        break;
      }
      case CmdType::OMSetBlendFactor: {
        auto *cmd = reinterpret_cast<const CmdOMBlendFactor *>(header);
        memcpy(st.blend_factor, cmd->factor, 16);
        break;
      }
      case CmdType::OMSetStencilRef: {
        auto *cmd = reinterpret_cast<const CmdOMStencilRef *>(header);
        st.stencil_ref = cmd->stencil_ref;
        break;
      }
      case CmdType::OMSetDepthBounds: {
        auto *cmd = reinterpret_cast<const CmdOMSetDepthBounds *>(header);
        float min_depth = std::isnan(cmd->min_depth) ? 0.0f : cmd->min_depth;
        float max_depth = std::isnan(cmd->max_depth) ? 0.0f : cmd->max_depth;
        min_depth = std::clamp(min_depth, 0.0f, 1.0f);
        max_depth = std::clamp(max_depth, 0.0f, 1.0f);
        st.depth_bounds_inverted = min_depth > max_depth;
        st.depth_bounds_min = st.depth_bounds_inverted ? 0.0f : min_depth;
        st.depth_bounds_max = st.depth_bounds_inverted ? 0.0f : max_depth;
        if (st.render_enc_open && st.pso &&
            st.pso->IsDepthBoundsTestEnabled()) {
          const float depth_bounds[4] = {
              st.depth_bounds_min, st.depth_bounds_max,
              static_cast<float>(st.depth_bounds_dsv_slice),
              st.depth_bounds_inverted ? 1.0f : 0.0f};
          st.render_enc.setFragmentBytes(depth_bounds, sizeof(depth_bounds),
                                         28);
        }
        QTRACE("OMSetDepthBounds requested=(%.3f,%.3f) applied=(%.3f,%.3f) "
               "inverted=%u",
               cmd->min_depth, cmd->max_depth, st.depth_bounds_min,
               st.depth_bounds_max, st.depth_bounds_inverted ? 1u : 0u);
        break;
      }
      case CmdType::SetDescriptorHeaps: {
        auto *cmd = reinterpret_cast<const CmdSetDescriptorHeaps *>(header);
        st.desc_heap_count = cmd->count > 2 ? 2 : cmd->count;
        auto *heaps = reinterpret_cast<ID3D12DescriptorHeap *const *>(
            reinterpret_cast<const uint8_t *>(cmd) +
            sizeof(CmdSetDescriptorHeaps) - sizeof(ID3D12DescriptorHeap *));
        for (uint32_t i = 0; i < st.desc_heap_count; i++)
          st.desc_heaps[i] = heaps[i];
        break;
      }
      default:
        break;
      }
      offset += header->size;
    }
    auto replay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - replay_begin)
                         .count();
    if (replay_ms >= DXMTD3D12TimingMinMs()) {
      QTRACE(
          "ExecuteCommandLists replay_ms=%lld queue_type=%u cmds=%zu list=%u",
          (long long)replay_ms, m_desc.Type, cmd_count, li);
    }
    QTRACE("ECL: replayed %zu cmds, types:", cmd_count);
    for (int i = 0; i < 30; i++)
      if (type_counts[i])
        QTRACE("  type[%d]=%u", i, type_counts[i]);

    st.CloseRenderEncoder();
    st.CaptureSwapchainRenderReadback(m_device, cmdbuf);
    st.ForceSwapchainDiagnosticColor(cmdbuf);
    st.ArmCommandBufferResourceRetention(
        command_list_id, m_desc.Type, stream_stats.command_count,
        stream_stats.draw_count + stream_stats.indexed_draw_count +
            stream_stats.indirect_count,
        stream_stats.dispatch_count);
    QTRACE("ExecuteCommandLists: committing cmdbuf");
    ENC_COMMIT(cmdbuf.handle);
    cmdbuf.commit();
    const bool sync_execute = DXMTD3D12SyncExecuteCommandBuffers() ||
                              DXMTD3D12SwapchainRenderReadback() ||
                              DXMTD3D12AutopresentSwapchain();
    int64_t wait_ms = 0;
    if (sync_execute) {
      auto wait_begin = std::chrono::steady_clock::now();
      cmdbuf.waitUntilCompleted();
      wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - wait_begin)
                    .count();
    }
    list_timer.SetDetail(
        "index=%u queue_type=%u cmds=%zu replay_ms=%lld wait_ms=%lld sync=%u",
        li, m_desc.Type, cmd_count, (long long)replay_ms, (long long)wait_ms,
        sync_execute ? 1u : 0u);

    auto status =
        sync_execute ? cmdbuf.status() : WMTCommandBufferStatusCommitted;
    QTRACE("ExecuteCommandLists: cmdbuf status=%d wait_ms=%lld sync=%u "
           "queue_type=%u",
           (int)status, (long long)wait_ms, sync_execute ? 1u : 0u,
           m_desc.Type);
    const uint32_t draw_count = stream_stats.draw_count;
    const uint32_t indexed_draw_count = stream_stats.indexed_draw_count;
    const uint32_t indirect_count = stream_stats.indirect_count;
    const uint32_t dispatch_count = stream_stats.dispatch_count;
    const uint32_t clear_rtv_count = stream_stats.clear_rtv_count;
    const uint32_t clear_dsv_count = stream_stats.clear_dsv_count;
    const uint32_t clear_uav_count = stream_stats.clear_uav_count;
    const bool interesting_list =
        draw_count || indexed_draw_count || indirect_count || dispatch_count ||
        clear_rtv_count || clear_dsv_count || clear_uav_count ||
        st.swapchain_work_encoded || st.HasSwapchainRenderTarget();
    auto *swapchain_backbuffer = st.swapchain_rt_for_present
                                     ? st.swapchain_rt_for_present
                                     : st.SwapchainRenderTargetResource();
    if (swapchain_backbuffer)
      st.TrackSwapchainResource(swapchain_backbuffer);
    const bool has_swapchain_work_target =
        st.HasSwapchainRenderTarget() || st.swapchain_touched_count;
    uint64_t queue_serial = 0;
    if (st.swapchain_touched_count) {
      queue_serial =
          __atomic_add_fetch(&g_queue_submit_serial, 1, __ATOMIC_RELAXED);
      D3D12SwapchainBackbufferWork work = {};
      work.serial = queue_serial;
      work.command_count = stream_stats.command_count;
      work.draw_count = draw_count;
      work.indexed_draw_count = indexed_draw_count;
      work.indirect_count = indirect_count;
      work.dispatch_count = dispatch_count;
      work.clear_rtv_count = clear_rtv_count;
      work.clear_dsv_count = clear_dsv_count;
      work.clear_uav_count = clear_uav_count;
      work.graphics_setup = stream_stats.HasGraphicsSetup() ? 1u : 0u;
      work.swapchain_work = st.swapchain_work_encoded ? 1u : 0u;
      work.has_swapchain_rt = has_swapchain_work_target ? 1u : 0u;
      work.command_buffer_status = (int32_t)status;
      work.replay_ms = replay_ms;
      work.wait_ms = wait_ms;
      for (uint32_t i = 0; i < st.swapchain_touched_count; i++)
        st.swapchain_touched_resources[i]->RecordSwapchainQueueWork(work);
    }
    if (interesting_list && TakeLogBudget(&g_command_list_summary_logs, 192)) {
      Logger::info(str::format(
          "M12 command list summary queue=", (unsigned)m_desc.Type,
          " list=", li, " cmdlist_id=", (unsigned long long)command_list_id,
          " serial=", (unsigned long long)queue_serial,
          " cmds=", stream_stats.command_count, " draws=", draw_count,
          " indexed=", indexed_draw_count, " indirect=", indirect_count,
          " dispatch=", dispatch_count,
          " clears=", clear_rtv_count + clear_dsv_count + clear_uav_count,
          " swapchain_work=", st.swapchain_work_encoded,
          " has_swapchain_rt=", has_swapchain_work_target,
          " status=", (int)status, " replay_ms=", (long long)replay_ms,
          " wait_ms=", (long long)wait_ms));
    }
    if (sync_execute && status != WMTCommandBufferStatusCompleted) {
      auto err = cmdbuf.error();
      auto err_desc_string =
          err.handle ? err.description().getUTF8String() : std::string();
      Logger::err(str::format(
          "ExecuteCommandLists: cmdbuf status=", status,
          " error_handle=", err.handle, " error=",
          err_desc_string.empty() ? "unknown" : err_desc_string.c_str()));
      Logger::err(str::format("ExecuteCommandLists fault breadcrumbs: ",
                              st.FormatFaultBreadcrumbs()));
    } else if (sync_execute) {
      st.LogSwapchainRenderReadback();
    }
    if (sync_execute && status == WMTCommandBufferStatusCompleted &&
        DXMTD3D12AutopresentSwapchain() && st.swapchain_work_encoded &&
        st.swapchain_rt_for_present &&
        st.swapchain_rt_for_present->OwningSwapchain()) {
      auto *swapchain = st.swapchain_rt_for_present->OwningSwapchain();
      HRESULT hr =
          swapchain->PresentBackBufferFromQueue(st.swapchain_rt_for_present);
      if (FAILED(hr)) {
        Logger::err(str::format("M12 autopresent failed hr=", (unsigned)hr));
      }
    }
  }
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::SetMarker(UINT metadata,
                                                       const void *data,
                                                       UINT size) {
  QTRACE("CmdQueue::SetMarker this=%p metadata=%u data=%p size=%u",
         (void *)this, metadata, data, size);
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::BeginEvent(UINT metadata,
                                                        const void *data,
                                                        UINT size) {
  QTRACE("CmdQueue::BeginEvent this=%p metadata=%u data=%p size=%u",
         (void *)this, metadata, data, size);
}

void STDMETHODCALLTYPE MTLD3D12CommandQueue::EndEvent() {
  QTRACE("CmdQueue::EndEvent this=%p", (void *)this);
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::Signal(ID3D12Fence *fence,
                                                       UINT64 value) {
  QTRACE("CmdQueue::Signal value=%llu fence_iface=%p",
         (unsigned long long)value, (void *)fence);
  if (!fence)
    return E_POINTER;
  auto dxmt_fence = static_cast<MTLD3D12Fence *>(fence);
  auto shared_event = dxmt_fence->GetMTLSharedEvent();
  if (!shared_event.handle)
    return E_FAIL;
  std::lock_guard submit_lock(m_submit_mutex);
  {
    FILE *f = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log");
    if (f) {
      fprintf(f, "CmdQueue::Signal value=%llu fence=%p\n",
              (unsigned long long)value, (void *)fence);
      fclose(f);
    }
  }
  auto cmdbuf = m_wmt_queue.commandBuffer();
  if (!cmdbuf.handle)
    return E_FAIL;
  cmdbuf.encodeSignalEvent(shared_event, value);
  DXMTD3D12ScopedTimer signal_timer("Queue", "SignalFence");
  signal_timer.SetDetail("queue_type=%u value=%llu fence=%p", m_desc.Type,
                         (unsigned long long)value, (void *)fence);
  cmdbuf.commit();
  QTRACE("CmdQueue::Signal queued queue_type=%u value=%llu fence=%p",
         m_desc.Type, (unsigned long long)value, (void *)fence);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::Wait(ID3D12Fence *fence,
                                                     UINT64 value) {
  QTRACE("CmdQueue::Wait this=%p fence=%p value=%llu", (void *)this,
         (void *)fence, (unsigned long long)value);
  if (!fence)
    return E_POINTER;
  auto dxmt_fence = static_cast<MTLD3D12Fence *>(fence);
  auto shared_event = dxmt_fence->GetMTLSharedEvent();
  if (!shared_event.handle)
    return E_FAIL;
  std::lock_guard submit_lock(m_submit_mutex);
  auto cmdbuf = m_wmt_queue.commandBuffer();
  if (!cmdbuf.handle)
    return E_FAIL;
  cmdbuf.encodeWaitForEvent(shared_event, value);
  DXMTD3D12ScopedTimer wait_timer("Queue", "WaitFence");
  wait_timer.SetDetail("queue_type=%u value=%llu fence=%p", m_desc.Type,
                       (unsigned long long)value, (void *)fence);
  cmdbuf.commit();
  QTRACE("CmdQueue::Wait queued queue_type=%u value=%llu fence=%p", m_desc.Type,
         (unsigned long long)value, (void *)fence);
  return S_OK;
}

bool MTLD3D12CommandQueue::EnqueueCompletionSignal(
    WMT::Reference<WMT::SharedEvent> &completion_event,
    uint64_t &completion_value) {
  std::lock_guard submit_lock(m_submit_mutex);
  if (!m_completion_event.handle)
    return false;
  auto cmdbuf = m_wmt_queue.commandBuffer();
  if (!cmdbuf.handle)
    return false;
  const uint64_t value = ++m_completion_seq;
  cmdbuf.encodeSignalEvent(m_completion_event, value);
  cmdbuf.commit();
  completion_event = m_completion_event;
  completion_value = value;
  QTRACE("EnqueueCompletionSignal queue_type=%u value=%llu event=%llu",
         m_desc.Type, (unsigned long long)value,
         (unsigned long long)m_completion_event.handle);
  return true;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12CommandQueue::GetTimestampFrequency(UINT64 *frequency) {
  QTRACE("CmdQueue::GetTimestampFrequency this=%p out=%p", (void *)this,
         frequency);
  if (frequency)
    *frequency = 1000000000;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12CommandQueue::GetClockCalibration(
    UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) {
  QTRACE("CmdQueue::GetClockCalibration this=%p gpu=%p cpu=%p", (void *)this,
         gpu_timestamp, cpu_timestamp);
  if (gpu_timestamp)
    *gpu_timestamp = 0;
  if (cpu_timestamp)
    *cpu_timestamp = 0;
  return S_OK;
}

D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE
MTLD3D12CommandQueue::GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) {
  QTRACE("CmdQueue::GetDesc this=%p out=%p type=%u", (void *)this, __ret,
         m_desc.Type);
  *__ret = m_desc;
  return __ret;
}

} // namespace dxmt
