#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "d3d12_vertex_input.hpp"
#include "Metal.hpp"
#include "airconv_public.h"
#include "thread.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt {

class MTLD3D12Device;

void ShutdownAsyncPipelineCompiler();

struct StageInVertexAttributeInfo {
  uint32_t register_index = 0;
  uint32_t attribute_index = 0;
  WMTAttributeFormat format = WMTAttributeFormatInvalid;
  std::string semantic_name;
};

struct D3D12IAInputElementInfo {
  std::string semantic_name;
  uint32_t semantic_index = 0;
  uint32_t shader_register = 0;
  uint32_t input_slot = 0;
  uint32_t table_index = 0;
  D3D12VertexTableIndexingMode table_indexing_mode =
      D3D12VertexTableIndexingMode::CompactBySlotMask;
  uint32_t aligned_byte_offset = 0;
  DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
  WMTAttributeFormat metal_format = WMTAttributeFormatInvalid;
  uint32_t bytes_per_element = 0;
  D3D12_INPUT_CLASSIFICATION input_slot_class =
      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  bool per_instance = false;
  uint32_t instance_step_rate = 1;
  bool system_value = false;
};

struct CompiledShader {
  sm50_shader_t handle = nullptr;
  MTL_SHADER_REFLECTION reflection = {};
};

class MTLD3D12PipelineState : public ID3D12PipelineState {
public:
  MTLD3D12PipelineState(MTLD3D12Device *device, bool is_compute);
  ~MTLD3D12PipelineState();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override;
  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override;
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override;
  HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **blob) override;

  void SetGraphicsDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc);
  void SetComputeDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc);

  bool RequestCompile(bool allow_async);
  bool Compile();
  bool TryCompilePendingInline();
  void RunAsyncCompile();

  bool EnsureCompiled() {
    if (!IsCompiled())
      Compile();
    return IsCompiled();
  }

  bool IsCompute() const { return m_is_compute; }
  bool IsCompiled() const;
  bool IsCompilePending() const;
  std::string GetCompileFailureStage() const;
  std::string GetCompileFailureDetail() const;

  WMT::Reference<WMT::RenderPipelineState> GetRenderPSO() const {
    return m_render_pso;
  }
  WMT::Reference<WMT::RenderPipelineState>
  GetNativeTessellationIndexedRenderPSO() const {
    return m_native_tessellation_indexed_render_pso;
  }
  WMT::Reference<WMT::DepthStencilState> GetDepthStencilState() const {
    return m_depth_stencil_state;
  }
  bool IsDepthEnabled() const { return m_depth_stencil_desc.DepthEnable; }
  bool IsDepthStencilEnabled() const {
    return m_depth_stencil_desc.DepthEnable ||
           m_depth_stencil_desc.StencilEnable;
  }
  const D3D12_RASTERIZER_DESC &GetRasterizerDesc() const {
    return m_rasterizer_desc;
  }
  const D3D12_DEPTH_STENCIL_DESC &GetDepthStencilDesc() const {
    return m_depth_stencil_desc;
  }
  const D3D12_BLEND_DESC &GetBlendDesc() const { return m_blend_desc; }
  UINT GetNumRenderTargets() const { return m_num_render_targets; }
  DXGI_FORMAT GetRTVFormat(UINT index) const {
    return index < 8 ? m_rtv_formats[index] : DXGI_FORMAT_UNKNOWN;
  }
  UINT GetSampleCount() const { return m_sample_count; }
  DXGI_FORMAT GetDSVFormat() const { return m_dsv_format; }
  WMT::Reference<WMT::ComputePipelineState> GetComputePSO() const {
    return m_compute_pso;
  }
  ID3D12RootSignature *GetRootSignature() const { return m_root_sig; }
  struct WMTSize GetThreadgroupSize() const {
    return {(uint64_t)m_threadgroup_size.width,
            (uint64_t)m_threadgroup_size.height,
            (uint64_t)m_threadgroup_size.depth};
  }

  const MTL_SHADER_REFLECTION &GetCSReflection() const {
    return m_cs_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetCSArguments() const {
    return m_cs_args;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetCSConstantBuffers() const {
    return m_cs_cb_args;
  }
  const MTL_SHADER_REFLECTION &GetVSReflection() const {
    return m_vs_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetVSArguments() const {
    return m_vs_args;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetVSConstantBuffers() const {
    return m_vs_cb_args;
  }
  std::string GetVSCacheHash() const;
  const MTL_SHADER_REFLECTION &GetPSReflection() const {
    return m_ps_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetPSArguments() const {
    return m_ps_args;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetPSConstantBuffers() const {
    return m_ps_cb_args;
  }
  std::string GetPSCacheHash() const;
  const MTL_SHADER_REFLECTION &GetGSReflection() const {
    return m_gs_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetGSArguments() const {
    return m_gs_args;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetGSConstantBuffers() const {
    return m_gs_cb_args;
  }
  std::string GetGSCacheHash() const;
  uint32_t GetPSArgumentBufferSize() const {
    return m_ps_reflection.ArgumentTableQwords * 8;
  }
  uint32_t GetIAInputSlotMask() const { return m_ia_slot_mask; }
  const std::vector<D3D12IAInputElementInfo> &GetIAInputElements() const {
    return m_ia_input_elements;
  }
  const D3D12_INPUT_LAYOUT_DESC &GetInputLayout() const {
    return m_input_layout;
  }
  bool UsesStageInVertexDescriptor() const { return m_vs_uses_stage_in; }
  bool RequiresMSCStageInFunction() const { return m_vs_requires_msc_stage_in; }
  bool UsesGeometryMeshPipeline() const {
    return m_uses_geometry_mesh_pipeline;
  }
  bool UsesNativeTessellationPath() const {
    return m_uses_native_tessellation_path;
  }
  uint32_t GetNativeTessellationControlPointCount() const {
    return m_native_tessellation_control_points;
  }
  bool UsesTessellationFallback() const { return m_uses_tessellation_fallback; }

  static WMTPixelFormat DXGIToMTLPixelFormat(DXGI_FORMAT format);

private:
  enum class CompileState : uint8_t {
    NotStarted = 0,
    Pending,
    Compiling,
    Compiled,
    Failed,
  };

  bool CompileImpl();
  bool IsSupportedNativeTessellationProofShape() const;
  bool CompileNativeTessellationProofShape();
  bool CompileShader(const void *bytecode, SIZE_T size, ShaderType type,
                     const char *func_name,
                     WMT::Reference<WMT::Function> &out_func,
                     sm50_shader_t *out_shader_handle = nullptr,
                     MTL_SHADER_REFLECTION *out_reflection = nullptr);
  void ClearCompileFailure();
  bool RecordCompileFailure(const char *stage, const std::string &detail);
  void BuildIAInputLayout(const void *bytecode, SIZE_T size,
                          std::vector<SM50_IA_INPUT_ELEMENT> &elements,
                          uint32_t &slot_mask);

  static std::mutex s_shader_mutex;
  static std::unordered_map<size_t, WMT::Reference<WMT::Function>>
      s_shader_cache;

  MTLD3D12Device *m_device;
  bool m_is_compute;
  std::atomic<CompileState> m_compile_state = {CompileState::NotStarted};
  mutable dxmt::mutex m_compile_mutex;
  dxmt::condition_variable m_compile_cv;
  std::string m_compile_failure_stage;
  std::string m_compile_failure_detail;
  ID3D12RootSignature *m_root_sig = nullptr;
  std::vector<uint8_t> m_vs, m_ps, m_gs, m_hs, m_ds, m_cs;
  D3D12_BLEND_DESC m_blend_desc = {};
  D3D12_RASTERIZER_DESC m_rasterizer_desc = {};
  D3D12_DEPTH_STENCIL_DESC m_depth_stencil_desc = {};
  D3D12_INPUT_LAYOUT_DESC m_input_layout = {};
  std::vector<D3D12_INPUT_ELEMENT_DESC> m_input_elements;
  std::vector<std::string> m_input_semantic_names;
  bool m_has_stream_output = false;
  bool m_vs_uses_stage_in = false;
  bool m_vs_requires_msc_stage_in = false;
  bool m_uses_geometry_mesh_pipeline = false;
  bool m_uses_native_tessellation_path = false;
  bool m_uses_tessellation_fallback = false;
  uint32_t m_native_tessellation_control_points = 0;
  uint32_t m_gs_passthrough = ~0u;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE m_strip_cut_value = {};
  D3D12_PRIMITIVE_TOPOLOGY_TYPE m_topology = {};
  UINT m_num_render_targets = 0;
  DXGI_FORMAT m_rtv_formats[8] = {};
  DXGI_FORMAT m_dsv_format = DXGI_FORMAT_UNKNOWN;
  UINT m_sample_mask = UINT_MAX;
  UINT m_sample_count = 1;
  std::vector<uint8_t> m_cached_pso_blob;

  WMT::Reference<WMT::RenderPipelineState> m_render_pso;
  WMT::Reference<WMT::RenderPipelineState>
      m_native_tessellation_indexed_render_pso;
  WMT::Reference<WMT::ComputePipelineState> m_compute_pso;
  WMT::Reference<WMT::DepthStencilState> m_depth_stencil_state;
  struct {
    uint32_t width = 1, height = 1, depth = 1;
  } m_threadgroup_size;

  MTL_SHADER_REFLECTION m_cs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_cs_args;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_cs_cb_args;
  sm50_shader_t m_cs_shader = nullptr;
  MTL_SHADER_REFLECTION m_vs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_vs_args;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_vs_cb_args;
  sm50_shader_t m_vs_shader = nullptr;
  MTL_SHADER_REFLECTION m_ps_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_ps_args;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_ps_cb_args;
  sm50_shader_t m_ps_shader = nullptr;
  MTL_SHADER_REFLECTION m_gs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_gs_args;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_gs_cb_args;
  sm50_shader_t m_gs_shader = nullptr;
  uint32_t m_ia_slot_mask = 0;
  std::vector<D3D12IAInputElementInfo> m_ia_input_elements;
  std::unordered_map<uint32_t, StageInVertexAttributeInfo>
      m_vs_stage_in_register_map;
  std::vector<StageInVertexAttributeInfo> m_vs_stage_in_attribute_order;

  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
