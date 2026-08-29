#pragma once

#include "d3d12.h"
#include <cstdint>

namespace dxmt {

enum class CmdType : uint32_t {
  DrawInstanced,
  DrawIndexedInstanced,
  Dispatch,
  DispatchMesh,
  ExecuteIndirect,
  SetPredication,
  SetMarker,
  BeginEvent,
  EndEvent,
  SetSamplePositions,
  CopyBufferRegion,
  CopyTextureRegion,
  CopyResource,
  SetPipelineState,
  SetGraphicsRootSignature,
  SetComputeRootSignature,
  SetGraphicsRoot32BitConstants,
  SetComputeRoot32BitConstants,
  SetGraphicsRootConstantBufferView,
  SetComputeRootConstantBufferView,
  SetGraphicsRootShaderResourceView,
  SetComputeRootShaderResourceView,
  SetGraphicsRootUnorderedAccessView,
  SetComputeRootUnorderedAccessView,
  SetGraphicsRootDescriptorTable,
  SetComputeRootDescriptorTable,
  IASetPrimitiveTopology,
  IASetVertexBuffers,
  IASetIndexBuffer,
  RSSetViewports,
  RSSetScissorRects,
  OMSetRenderTargets,
  OMSetBlendFactor,
  OMSetStencilRef,
  ClearRenderTargetView,
  ClearDepthStencilView,
  ClearUnorderedAccessView,
  DiscardResource,
  ResourceBarrier,
  EnhancedBarrier,
  SetDescriptorHeaps,
  ResolveSubresource,
  WriteBufferImmediate,
  BeginQuery,
  EndQuery,
  ResolveQueryData,
  BuildRaytracingAccelerationStructure,
  CopyRaytracingAccelerationStructure,
  EmitRaytracingAccelerationStructurePostbuildInfo,
  SetPipelineState1,
  DispatchRays,
  OMSetDepthBounds,
  RSSetShadingRate,
  RSSetShadingRateImage,
};

struct CmdHeader {
  CmdType type;
  uint32_t size;
};

struct CmdDrawInstanced {
  CmdHeader header;
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t start_vertex;
  uint32_t start_instance;
};

struct CmdDrawIndexedInstanced {
  CmdHeader header;
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t start_index;
  int32_t base_vertex;
  uint32_t start_instance;
};

struct CmdDispatch {
  CmdHeader header;
  uint32_t x, y, z;
};

struct CmdDispatchMesh {
  CmdHeader header;
  uint32_t x, y, z;
};

struct CmdSetPipelineState1 {
  CmdHeader header;
  ID3D12StateObject *state_object;
};

struct CmdDispatchRays {
  CmdHeader header;
  D3D12_DISPATCH_RAYS_DESC desc;
};

struct CmdEnhancedBarrier {
  CmdHeader header;
  uint32_t group_count;
  uint32_t global_barrier_count;
  uint32_t buffer_barrier_count;
  uint32_t texture_barrier_count;
};

struct CmdEmitRaytracingAccelerationStructurePostbuildInfo {
  CmdHeader header;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE info_type;
  D3D12_GPU_VIRTUAL_ADDRESS dest_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS source_acceleration_structure;
};

struct CmdBuildRaytracingAccelerationStructure {
  CmdHeader header;
  D3D12_GPU_VIRTUAL_ADDRESS dest_acceleration_structure;
  D3D12_GPU_VIRTUAL_ADDRESS scratch_acceleration_structure;
  D3D12_GPU_VIRTUAL_ADDRESS source_acceleration_structure;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS flags;
  D3D12_ELEMENTS_LAYOUT descs_layout;
  UINT num_descs;
  D3D12_GPU_VIRTUAL_ADDRESS instance_descs;
  static constexpr UINT kMaxGeometryDescs = 64;
  D3D12_RAYTRACING_GEOMETRY_DESC geometries[kMaxGeometryDescs];
};

struct CmdCopyRaytracingAccelerationStructure {
  CmdHeader header;
  D3D12_GPU_VIRTUAL_ADDRESS destination_acceleration_structure;
  D3D12_GPU_VIRTUAL_ADDRESS source_acceleration_structure;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode;
};

struct CmdExecuteIndirect {
  CmdHeader header;
  ID3D12CommandSignature *signature;
  uint32_t max_command_count;
  ID3D12Resource *argument_buffer;
  uint64_t argument_buffer_offset;
  ID3D12Resource *count_buffer;
  uint64_t count_buffer_offset;
};

struct CmdSetPredication {
  CmdHeader header;
  ID3D12Resource *buffer;
  uint64_t aligned_buffer_offset;
  D3D12_PREDICATION_OP operation;
};

struct CmdDebugEvent {
  CmdHeader header;
  uint32_t metadata;
  uint32_t data_size;
  uint8_t data[1];
};

struct CmdSetSamplePositions {
  CmdHeader header;
  uint32_t sample_count;
  uint32_t pixel_count;
  uint32_t position_count;
  D3D12_SAMPLE_POSITION positions[1];
};

struct CmdCopyBufferRegion {
  CmdHeader header;
  ID3D12Resource *dst;
  uint64_t dst_offset;
  ID3D12Resource *src;
  uint64_t src_offset;
  uint64_t byte_count;
};

struct CmdCopyTextureRegion {
  CmdHeader header;
  ID3D12Resource *dst_resource;
  D3D12_TEXTURE_COPY_TYPE dst_type;
  UINT dst_subresource;
  UINT64 dst_offset;
  DXGI_FORMAT dst_footprint_format;
  UINT dst_footprint_width;
  UINT dst_footprint_height;
  UINT dst_footprint_depth;
  UINT dst_footprint_row_pitch;
  UINT dst_x, dst_y, dst_z;
  ID3D12Resource *src_resource;
  D3D12_TEXTURE_COPY_TYPE src_type;
  UINT src_subresource;
  UINT64 src_offset;
  DXGI_FORMAT src_footprint_format;
  UINT src_footprint_width;
  UINT src_footprint_height;
  UINT src_footprint_depth;
  UINT src_footprint_row_pitch;
  D3D12_BOX src_box;
  UINT8 has_src_box;
};

struct CmdCopyResource {
  CmdHeader header;
  ID3D12Resource *dst;
  ID3D12Resource *src;
};

struct CmdSetPipelineState {
  CmdHeader header;
  ID3D12PipelineState *pso;
};

struct CmdSetRootSignature {
  CmdHeader header;
  ID3D12RootSignature *root_sig;
};

struct CmdSetRoot32BitConstants {
  CmdHeader header;
  uint32_t root_param_index;
  uint32_t count;
  uint32_t dst_offset;
  uint8_t data[1];
};

struct CmdSetRootCBV {
  CmdHeader header;
  uint32_t root_param_index;
  D3D12_GPU_VIRTUAL_ADDRESS address;
};

struct CmdSetRootDescriptorTable {
  CmdHeader header;
  uint32_t root_param_index;
  D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor;
};

struct CmdIASetPrimitiveTopology {
  CmdHeader header;
  D3D12_PRIMITIVE_TOPOLOGY topology;
};

struct CmdIASetVertexBuffers {
  CmdHeader header;
  uint32_t start_slot;
  uint32_t count;
  D3D12_VERTEX_BUFFER_VIEW views[1];
};

struct CmdIASetIndexBuffer {
  CmdHeader header;
  D3D12_INDEX_BUFFER_VIEW view;
};

struct CmdRSSetViewports {
  CmdHeader header;
  uint32_t count;
  D3D12_VIEWPORT viewports[1];
};

struct CmdRSSetScissorRects {
  CmdHeader header;
  uint32_t count;
  D3D12_RECT rects[1];
};

struct CmdOMSetRenderTargets {
  CmdHeader header;
  uint32_t rt_count;
  bool single_handle;
  D3D12_CPU_DESCRIPTOR_HANDLE rts[8];
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  bool has_dsv;
};

struct CmdOMBlendFactor {
  CmdHeader header;
  float factor[4];
};

struct CmdOMStencilRef {
  CmdHeader header;
  uint32_t stencil_ref;
};

struct CmdClearRTV {
  CmdHeader header;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  float color[4];
};

struct CmdClearDSV {
  CmdHeader header;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  D3D12_CLEAR_FLAGS flags;
  float depth;
  uint8_t stencil;
};

struct CmdClearUAV {
  CmdHeader header;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle;
  ID3D12Resource *resource;
  uint32_t values[4];
  uint8_t is_float;
};

struct CmdDiscardResource {
  CmdHeader header;
  ID3D12Resource *resource;
  uint32_t first_subresource;
  uint32_t num_subresources;
};

struct CmdResourceBarrier {
  CmdHeader header;
  uint32_t count;
  D3D12_RESOURCE_BARRIER barriers[1];
};

struct CmdSetDescriptorHeaps {
  CmdHeader header;
  uint32_t count;
  ID3D12DescriptorHeap *heaps[1];
};

struct CmdResolveSubresource {
  CmdHeader header;
  ID3D12Resource *dst;
  uint32_t dst_sub;
  uint32_t dst_x;
  uint32_t dst_y;
  ID3D12Resource *src;
  uint32_t src_sub;
  DXGI_FORMAT format;
  D3D12_RESOLVE_MODE mode;
  uint8_t has_src_rect;
  D3D12_RECT src_rect;
};

struct CmdOMSetDepthBounds {
  CmdHeader header;
  float min_depth;
  float max_depth;
};

struct CmdRSSetShadingRate {
  CmdHeader header;
  D3D12_SHADING_RATE base_shading_rate;
  D3D12_SHADING_RATE_COMBINER combiners[2];
};

struct CmdRSSetShadingRateImage {
  CmdHeader header;
  ID3D12Resource *shading_rate_image;
};

struct CmdWriteBufferImmediateEntry {
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER parameter;
  D3D12_WRITEBUFFERIMMEDIATE_MODE mode;
};

struct CmdWriteBufferImmediate {
  CmdHeader header;
  uint32_t count;
  CmdWriteBufferImmediateEntry entries[1];
};

struct CmdQuery {
  CmdHeader header;
  ID3D12QueryHeap *heap;
  D3D12_QUERY_TYPE type;
  uint32_t index;
};

struct CmdResolveQueryData {
  CmdHeader header;
  ID3D12QueryHeap *heap;
  D3D12_QUERY_TYPE type;
  uint32_t start_index;
  uint32_t query_count;
  ID3D12Resource *dst_buffer;
  uint64_t dst_offset;
};

} // namespace dxmt
