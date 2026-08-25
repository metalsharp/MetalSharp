#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_defs.hpp"
#include "d3d12.h"
#include "Metal.hpp"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dxmt {

class MTLD3D12Device;
class MTLD3D12CommandAllocator;


class MTLD3D12GraphicsCommandList : public ID3D12GraphicsCommandList6 {
public:
  MTLD3D12GraphicsCommandList(MTLD3D12Device *device,
                              MTLD3D12CommandAllocator *allocator,
                              D3D12_COMMAND_LIST_TYPE type,
                              ID3D12PipelineState *initial_state);
  ~MTLD3D12GraphicsCommandList();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override;
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override;

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override;

  HRESULT STDMETHODCALLTYPE Close() override;
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state) override;
  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pipeline_state) override;

  void STDMETHODCALLTYPE DrawInstanced(UINT vertex_count_per_instance,
                                       UINT instance_count,
                                       UINT start_vertex_location,
                                       UINT start_instance_location) override;
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT index_count_per_instance,
                                              UINT instance_count,
                                              UINT start_vertex_location,
                                              INT base_vertex_location,
                                              UINT start_instance_location) override;
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT u, UINT z) override;
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst_buffer,
                                          UINT64 dst_offset,
                                          ID3D12Resource *src_buffer,
                                          UINT64 src_offset,
                                          UINT64 byte_count) override;
  void STDMETHODCALLTYPE CopyTextureRegion(
      const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y,
      UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src,
      const D3D12_BOX *src_box) override;
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst_resource,
                                      ID3D12Resource *src_resource) override;
  void STDMETHODCALLTYPE CopyTiles(
      ID3D12Resource *tiled_resource,
      const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
      const D3D12_TILE_REGION_SIZE *tile_region_size,
      ID3D12Resource *buffer, UINT64 buffer_offset,
      D3D12_TILE_COPY_FLAGS flags) override;
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *dst_resource,
                                            UINT dst_sub_resource,
                                            ID3D12Resource *src_resource,
                                            UINT src_sub_resource,
                                            DXGI_FORMAT format) override;
  void STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) override;
  void STDMETHODCALLTYPE RSSetViewports(UINT viewport_count,
                                       const D3D12_VIEWPORT *viewports) override;
  void STDMETHODCALLTYPE RSSetScissorRects(UINT rect_count,
                                           const D3D12_RECT *rects) override;
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT blend_factor[4]) override;
  void STDMETHODCALLTYPE OMSetStencilRef(UINT stencil_ref) override;
  void STDMETHODCALLTYPE SetPipelineState(
      ID3D12PipelineState *pipeline_state) override;
  void STDMETHODCALLTYPE ResourceBarrier(
      UINT barrier_count,
      const D3D12_RESOURCE_BARRIER *barriers) override;
  void STDMETHODCALLTYPE ExecuteBundle(
      ID3D12GraphicsCommandList *command_list) override;
  void STDMETHODCALLTYPE SetDescriptorHeaps(
      UINT heap_count,
      ID3D12DescriptorHeap *const *heaps) override;
  void STDMETHODCALLTYPE
  SetComputeRootSignature(ID3D12RootSignature *root_signature) override;
  void STDMETHODCALLTYPE
  SetGraphicsRootSignature(ID3D12RootSignature *root_signature) override;
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(
      UINT root_parameter_index,
      D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override;
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(
      UINT root_parameter_index,
      D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override;
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT root_parameter_index,
                                                     UINT data,
                                                     UINT dst_offset) override;
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(
      UINT root_parameter_index, UINT data, UINT dst_offset) override;
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(
      UINT root_parameter_index, UINT constant_count, const void *data,
      UINT dst_offset) override;
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(
      UINT root_parameter_index, UINT constant_count, const void *data,
      UINT dst_offset) override;
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(
      UINT root_parameter_index,
      D3D12_GPU_VIRTUAL_ADDRESS address) override;
  void STDMETHODCALLTYPE
  IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) override;
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start_slot, UINT view_count,
                                           const D3D12_VERTEX_BUFFER_VIEW *views) override;
  void STDMETHODCALLTYPE SOSetTargets(
      UINT start_slot, UINT view_count,
      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) override;
  void STDMETHODCALLTYPE OMSetRenderTargets(
      UINT render_target_descriptor_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
      WINBOOL single_descriptor_handle,
      const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) override;
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                               D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil,
                                               UINT rect_count,
                                               const D3D12_RECT *rects) override;
  void STDMETHODCALLTYPE ClearRenderTargetView(
      D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count,
      const D3D12_RECT *rects) override;
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
      D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
      const UINT values[4], UINT rect_count,
      const D3D12_RECT *rects) override;
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
      D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
      const float values[4], UINT rect_count,
      const D3D12_RECT *rects) override;
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *resource,
                                        const D3D12_DISCARD_REGION *region) override;
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap,
                                    D3D12_QUERY_TYPE type,
                                    UINT index) override;
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap,
                                  D3D12_QUERY_TYPE type,
                                  UINT index) override;
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap,
                                          D3D12_QUERY_TYPE type,
                                          UINT start_index, UINT query_count,
                                          ID3D12Resource *dst_buffer,
                                          UINT64 aligned_dst_buffer_offset) override;
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *buffer,
                                       UINT64 aligned_buffer_offset,
                                       D3D12_PREDICATION_OP operation) override;
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data,
                                   UINT size) override;
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data,
                                    UINT size) override;
  void STDMETHODCALLTYPE EndEvent() override;
  void STDMETHODCALLTYPE ExecuteIndirect(
      ID3D12CommandSignature *command_signature, UINT max_command_count,
      ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset,
      ID3D12Resource *count_buffer,
      UINT64 count_buffer_offset) override;

  void STDMETHODCALLTYPE AtomicCopyBufferUINT(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override;
  void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override;
  void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT min, FLOAT max) override;
  void STDMETHODCALLTYPE SetSamplePositions(
      UINT sample_count, UINT pixel_count,
      D3D12_SAMPLE_POSITION *sample_positions) override;
  void STDMETHODCALLTYPE ResolveSubresourceRegion(
      ID3D12Resource *dst_resource, UINT dst_sub_resource_idx,
      UINT dst_x, UINT dst_y,
      ID3D12Resource *src_resource, UINT src_sub_resource_idx,
      D3D12_RECT *src_rect, DXGI_FORMAT format,
      D3D12_RESOLVE_MODE mode) override;
  void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override;
  void STDMETHODCALLTYPE WriteBufferImmediate(
      UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
      const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override;

  /*** ID3D12GraphicsCommandList3 ***/
  void STDMETHODCALLTYPE SetProtectedResourceSession(
      ID3D12ProtectedResourceSession *protected_session) override;

  /*** ID3D12GraphicsCommandList4 ***/
  void STDMETHODCALLTYPE BeginRenderPass(
      UINT num_render_targets,
      const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
      D3D12_RENDER_PASS_FLAGS flags) override;
  void STDMETHODCALLTYPE EndRenderPass() override;
  void STDMETHODCALLTYPE InitializeMetaCommand(
      ID3D12MetaCommand *meta_command, const void *initialization_parameters_data,
      SIZE_T initialization_parameters_data_size_in_bytes) override;
  void STDMETHODCALLTYPE ExecuteMetaCommand(
      ID3D12MetaCommand *meta_command, const void *execution_parameters_data,
      SIZE_T execution_parameters_data_size_in_bytes) override;
  void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
      UINT num_post_build_info_descs,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *post_build_info_descs) override;
  void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *descs,
      UINT num_acceleration_structures,
      const D3D12_GPU_VIRTUAL_ADDRESS *source_acceleration_structure_data) override;
  void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS dest_acceleration_structure_data,
      D3D12_GPU_VIRTUAL_ADDRESS source_acceleration_structure_data,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override;
  void STDMETHODCALLTYPE SetPipelineState1(
      ID3D12StateObject *state_object) override;
  void STDMETHODCALLTYPE DispatchRays(
      const D3D12_DISPATCH_RAYS_DESC *desc) override;

  /*** ID3D12GraphicsCommandList5 ***/
  void STDMETHODCALLTYPE RSSetShadingRate(
      D3D12_SHADING_RATE base_shading_rate,
      const D3D12_SHADING_RATE_COMBINER *combiners) override;
  void STDMETHODCALLTYPE RSSetShadingRateImage(
      ID3D12Resource *shading_rate_image) override;

  /*** ID3D12GraphicsCommandList6 ***/
  void STDMETHODCALLTYPE DispatchMesh(
      UINT thread_group_count_x, UINT thread_group_count_y,
      UINT thread_group_count_z) override;

  const std::vector<uint8_t> &GetCommands() const { return m_cmds; }
  void ClearCommands() {
    ReleaseReferencedPipelineStates();
    m_cmds.clear();
  }
  uint64_t GetDebugId() const { return m_debug_id; }

private:
  template <typename T> void Emit(const T &cmd) {
    auto offset = m_cmds.size();
    m_cmds.resize(offset + sizeof(T));
    memcpy(m_cmds.data() + offset, &cmd, sizeof(T));
  }

  template <typename T>
  void EmitVar(T &cmd, const void *extra, uint32_t extra_size) {
    auto offset = m_cmds.size();
    m_cmds.resize(offset + sizeof(T) - 1 + extra_size);
    cmd.header.size = sizeof(T) - 1 + extra_size;
    memcpy(m_cmds.data() + offset, &cmd, sizeof(T) - 1);
    memcpy(m_cmds.data() + offset + sizeof(T) - 1, extra, extra_size);
  }

  void RetainPipelineState(ID3D12PipelineState *pipeline_state);
  void RetainStateObject(ID3D12StateObject *state_object);
  void ReleaseReferencedPipelineStates();

  MTLD3D12Device *m_device;
  MTLD3D12CommandAllocator *m_allocator;
  D3D12_COMMAND_LIST_TYPE m_type;
  bool m_closed = false;
  uint64_t m_debug_id = 0;
  std::vector<uint8_t> m_cmds;
  std::vector<ID3D12PipelineState *> m_referenced_pipeline_states;
  std::vector<ID3D12StateObject *> m_referenced_state_objects;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
};

} // namespace dxmt
