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
bool D3D12ShaderCacheEnabled();
void SetD3D12ShaderCacheEnabled(bool enabled);
void ClearD3D12ShaderCache();

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

// The vendored D3D12 header predates the view-instancing stream subobject.
// Keep a private ABI-compatible copy so the parser can validate and retain
// the caller-owned locations without depending on the header version.
struct D3D12ViewInstanceLocation {
  UINT ViewportArrayIndex = 0;
  UINT RenderTargetArrayIndex = 0;
};

struct D3D12ViewInstancingDesc {
  UINT ViewInstanceCount = 0;
  const D3D12ViewInstanceLocation *pViewInstanceLocations = nullptr;
  UINT Flags = 0;
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
  void SetViewInstancing(const D3D12ViewInstancingDesc &desc);
  void SetDepthBoundsTestEnable(bool enable) {
    m_depth_bounds_test_enable = enable;
  }
  void SetMeshShaders(const D3D12_SHADER_BYTECODE &as,
                      const D3D12_SHADER_BYTECODE &ms);
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
  bool UsesIndependentLogicOpEmulation() const {
    return m_uses_independent_logic_op_emulation;
  }
  WMT::Reference<WMT::RenderPipelineState>
  GetIndependentLogicOpRenderPSO(UINT render_target) const {
    if (render_target < m_independent_logic_op_render_psos.size() &&
        m_independent_logic_op_render_psos[render_target].handle)
      return m_independent_logic_op_render_psos[render_target];
    return m_render_pso;
  }
  bool UsesIndependentLogicOpDepthReplay() const {
    return m_uses_independent_logic_op_depth_replay;
  }
  WMT::Reference<WMT::RenderPipelineState>
  GetIndependentLogicOpDepthOnlyPSO() const {
    return m_independent_logic_op_depth_only_pso;
  }
  WMT::Reference<WMT::DepthStencilState>
  GetIndependentLogicOpNoWriteDepthState() const {
    return m_independent_logic_op_no_write_depth_state;
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
  bool IsDepthBoundsTestEnabled() const {
    return m_depth_bounds_test_enable &&
           m_dsv_format != DXGI_FORMAT_UNKNOWN;
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
  struct WMTSize GetObjectThreadgroupSize() const {
    return {(uint64_t)m_object_threadgroup_size.width,
            (uint64_t)m_object_threadgroup_size.height,
            (uint64_t)m_object_threadgroup_size.depth};
  }
  struct WMTSize GetMeshThreadgroupSize() const {
    return {(uint64_t)m_mesh_threadgroup_size.width,
            (uint64_t)m_mesh_threadgroup_size.height,
            (uint64_t)m_mesh_threadgroup_size.depth};
  }

  const MTL_SHADER_REFLECTION &GetCSReflection() const {
    return m_cs_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetCSArguments() const {
    return m_cs_args;
  }
  bool CSUsesMSCArgumentABI() const { return m_cs_uses_msc_argument_abi; }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetCSConstantBuffers() const {
    return m_cs_cb_args;
  }
  const MTL_SHADER_REFLECTION &GetVSReflection() const {
    return m_vs_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetVSArguments() const {
    return m_vs_args;
  }
  bool VSUsesMSCArgumentABI() const { return m_vs_uses_msc_argument_abi; }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetVSConstantBuffers() const {
    return m_vs_cb_args;
  }
  std::string GetCSCacheHash() const;
  std::string GetVSCacheHash() const;
  const MTL_SHADER_REFLECTION &GetPSReflection() const {
    return m_ps_reflection;
  }
  const std::vector<MTL_SM50_SHADER_ARGUMENT> &GetPSArguments() const {
    return m_ps_args;
  }
  bool PSUsesMSCArgumentABI() const { return m_ps_uses_msc_argument_abi; }
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
  bool GSUsesMSCArgumentABI() const { return m_gs_uses_msc_argument_abi; }
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
  bool UsesNativeMeshPipeline() const { return m_uses_native_mesh_pipeline; }
  bool UsesNativeTessellationPath() const {
    return m_uses_native_tessellation_path;
  }
  uint32_t GetNativeTessellationControlPointCount() const {
    return m_native_tessellation_control_points;
  }
  bool UsesTessellationFallback() const { return m_uses_tessellation_fallback; }
  bool UsesAtomic64Emulation() const { return m_uses_atomic64_emulation; }
  bool UsesSamplerFeedbackEmulation() const {
    return m_uses_sampler_feedback_emulation;
  }
  bool UsesVRSRuntimeState() const { return m_uses_vrs_runtime_state; }
  bool UsesConservativeRasterization() const {
    return m_uses_conservative_rasterization;
  }
  bool UsesConservativeRasterizationReferenceModel() const {
    return m_uses_conservative_rasterization_reference_model;
  }
  bool UsesAttributeAtVertex() const { return m_uses_attribute_at_vertex; }
  bool UsesDirectResourceDescriptorHeap() const {
    return m_uses_direct_resource_descriptor_heap;
  }
  bool HasStreamOutput() const { return m_has_stream_output; }
  UINT GetStreamOutputStride(UINT slot = 0) const {
    return slot < m_stream_output_strides.size()
               ? m_stream_output_strides[slot]
               : 0;
  }
  UINT GetStreamOutputSlotCount() const {
    return static_cast<UINT>(m_stream_output_strides.size());
  }
  UINT GetViewInstanceCount() const { return m_view_instance_count; }
  const std::vector<D3D12ViewInstanceLocation> &GetViewInstanceLocations() const {
    return m_view_instance_locations;
  }
  UINT GetViewInstancingFlags() const { return m_view_instancing_flags; }
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE GetStripCutValue() const {
    return m_strip_cut_value;
  }
  bool UsesViewInstanceMasking() const {
    return (m_view_instancing_flags & 0x1u) != 0;
  }


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
  bool CompileGeometryPipelineShaders(
      WMT::Reference<WMT::Function> &object_func,
      WMT::Reference<WMT::Function> &mesh_func);
  void ClearCompileFailure();
  bool RecordCompileFailure(const char *stage, const std::string &detail);
  void BuildIAInputLayout(const void *bytecode, SIZE_T size,
                          std::vector<SM50_IA_INPUT_ELEMENT> &elements,
                          uint32_t &slot_mask);
  size_t ApplyShaderVariantHash(size_t hash, ShaderType type) const;

  friend void ClearD3D12ShaderCache();
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
  std::vector<uint8_t> m_vs, m_ps, m_gs, m_hs, m_ds, m_cs, m_as, m_ms;
  D3D12_BLEND_DESC m_blend_desc = {};
  D3D12_RASTERIZER_DESC m_rasterizer_desc = {};
  D3D12_DEPTH_STENCIL_DESC m_depth_stencil_desc = {};
  bool m_depth_bounds_test_enable = false;
  bool m_uses_atomic64_emulation = false;
  bool m_uses_sampler_feedback_emulation = false;
  bool m_uses_vrs_runtime_state = false;
  bool m_uses_conservative_rasterization = false;
  bool m_uses_conservative_rasterization_reference_model = false;
  bool m_uses_attribute_at_vertex = false;
  uint32_t m_attribute_at_vertex_input_id = UINT32_MAX;
  bool m_uses_independent_logic_op_emulation = false;
  bool m_uses_independent_logic_op_depth_replay = false;
  bool m_uses_direct_resource_descriptor_heap = false;
  D3D12_INPUT_LAYOUT_DESC m_input_layout = {};
  std::vector<D3D12_INPUT_ELEMENT_DESC> m_input_elements;
  std::vector<std::string> m_input_semantic_names;
  D3D12_STREAM_OUTPUT_DESC m_stream_output = {};
  std::vector<D3D12_SO_DECLARATION_ENTRY> m_stream_output_elements;
  std::vector<std::string> m_stream_output_semantic_names;
  std::vector<UINT> m_stream_output_strides;
  bool m_has_stream_output = false;
  UINT m_view_instance_count = 0;
  UINT m_view_instancing_flags = 0;
  std::vector<D3D12ViewInstanceLocation> m_view_instance_locations;
  bool m_vs_uses_stage_in = false;
  bool m_vs_requires_msc_stage_in = false;
  bool m_uses_geometry_mesh_pipeline = false;
  bool m_uses_native_mesh_pipeline = false;
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
  std::vector<WMT::Reference<WMT::RenderPipelineState>>
      m_independent_logic_op_render_psos;
  WMT::Reference<WMT::RenderPipelineState>
      m_independent_logic_op_depth_only_pso;
  WMT::Reference<WMT::DepthStencilState>
      m_independent_logic_op_no_write_depth_state;
  WMT::Reference<WMT::Library> m_conservative_vertex_library;
  WMT::Reference<WMT::Function> m_conservative_vertex_function;
  WMT::Reference<WMT::RenderPipelineState>
      m_native_tessellation_indexed_render_pso;
  WMT::Reference<WMT::ComputePipelineState> m_compute_pso;
  WMT::Reference<WMT::DepthStencilState> m_depth_stencil_state;
  struct ThreadgroupSize {
    uint32_t width = 1, height = 1, depth = 1;
  };
  ThreadgroupSize m_threadgroup_size;
  ThreadgroupSize m_object_threadgroup_size;
  ThreadgroupSize m_mesh_threadgroup_size;
  uint32_t m_mesh_payload_size = 0;

  MTL_SHADER_REFLECTION m_cs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_cs_args;
  bool m_cs_uses_msc_argument_abi = false;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_cs_cb_args;
  sm50_shader_t m_cs_shader = nullptr;
  MTL_SHADER_REFLECTION m_vs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_vs_args;
  bool m_vs_uses_msc_argument_abi = false;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_vs_cb_args;
  sm50_shader_t m_vs_shader = nullptr;
  MTL_SHADER_REFLECTION m_ps_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_ps_args;
  bool m_ps_uses_msc_argument_abi = false;
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_ps_cb_args;
  sm50_shader_t m_ps_shader = nullptr;
  MTL_SHADER_REFLECTION m_gs_reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> m_gs_args;
  bool m_gs_uses_msc_argument_abi = false;
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
