#pragma once

#include "d3d12.h"
#include "d3d12_dxr_compat.hpp"
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
  SetViewInstanceMask,
  SetStreamOutputTargets,
  BeginRenderPass,
  EndRenderPass,
  SetProtectedResourceSession,
  InitializeMetaCommand,
  ExecuteMetaCommand,
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
  OMSetFrontAndBackStencilRef,
  RSSetDepthBias,
  IASetIndexBufferStripCutValue,
  SetProgram,
  DispatchGraph,
  Count,
};

struct CmdHeader {
  CmdType type;
  uint32_t size;
};

inline constexpr uint32_t kNoViewInstanceIndex = UINT32_MAX;

struct CmdDrawInstanced {
  CmdHeader header;
  uint32_t vertex_count;
  uint32_t instance_count;
  uint32_t start_vertex;
  uint32_t start_instance;
  // UINT32_MAX means the command is not a recording-time expansion.
  uint32_t view_instance_index;
  uint32_t sample_pixel_index;
};

struct CmdDrawIndexedInstanced {
  CmdHeader header;
  uint32_t index_count;
  uint32_t instance_count;
  uint32_t start_index;
  int32_t base_vertex;
  uint32_t start_instance;
  uint32_t view_instance_index;
  uint32_t sample_pixel_index;
};

struct CmdDispatch {
  CmdHeader header;
  uint32_t x, y, z;
};

struct CmdDispatchMesh {
  CmdHeader header;
  uint32_t x, y, z;
};

struct CmdSetProgram {
  CmdHeader header;
  uint32_t program_type;
  uint32_t descriptor_size;
  uint8_t descriptor[88];
};

// Pointer-free representation of one D3D12 multi-node input.  CPU dispatch
// records are packed behind an array of these descriptors before the command
// is emitted; GPU dispatches use the same shape only as a transient replay
// description after the queue has validated the source ranges.
struct PackedWorkGraphNodeInput {
  uint32_t entrypoint_index;
  uint32_t num_records;
  uint64_t record_stride;
  uint32_t data_offset;
  uint32_t reserved;
};

struct CmdDispatchGraph {
  CmdHeader header;
  uint32_t dispatch_mode;
  uint32_t entrypoint_index;
  // For NODE_* modes this is the record count.  For MULTI_NODE_* modes it is
  // the number of node-input descriptors and node_input_count is identical.
  uint32_t num_records;
  uint32_t record_data_size;
  uint32_t node_input_count;
  uint32_t reserved;
  uint64_t record_gpu_address;
  uint64_t record_stride;
  uint64_t node_input_gpu_address;
  uint64_t node_input_stride;
  // Mode 0/1 contains record bytes.  Mode 2 contains a packed descriptor
  // array followed by each descriptor's copied CPU records.
  uint8_t record_data[1024];
};

struct CmdSetPipelineState1 {
  CmdHeader header;
  ID3D12StateObject *state_object;
};

struct CmdDispatchRays {
  CmdHeader header;
  D3D12_DISPATCH_RAYS_DESC desc;
};

struct CmdEnhancedBarrierRecord {
  D3D12_BARRIER_TYPE type;
  union {
    D3D12_GLOBAL_BARRIER global;
    D3D12_BUFFER_BARRIER buffer;
    D3D12_TEXTURE_BARRIER texture;
  } barrier;
};

struct CmdEnhancedBarrier {
  CmdHeader header;
  uint32_t group_count;
  uint32_t global_barrier_count;
  uint32_t buffer_barrier_count;
  uint32_t texture_barrier_count;
  uint32_t record_count;
  CmdEnhancedBarrierRecord records[1];
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
  // OMM geometry uses a second pointer pair in the same union slot as the
  // legacy triangles descriptor. Copy the pair and its child descriptors so
  // command recording never retains caller-owned OMM structs.
  D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC omm_triangles[kMaxGeometryDescs];
  D3D12OpacityMicromapLinkageDescCompat omm_linkages[kMaxGeometryDescs];
  static constexpr UINT kMaxOmmHistogramEntries = 64;
  D3D12OpacityMicromapArrayDescCompat opacity_micromap_array_desc = {};
  D3D12OpacityMicromapHistogramEntryCompat
      omm_histogram[kMaxOmmHistogramEntries];
  UINT omm_histogram_count = 0;
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

struct CmdSetViewInstanceMask {
  CmdHeader header;
  uint32_t mask;
};

struct CmdSetStreamOutputTargets {
  CmdHeader header;
  uint32_t start_slot;
  uint32_t view_count;
  D3D12_STREAM_OUTPUT_BUFFER_VIEW views[1];
};

struct CmdBeginRenderPass {
  CmdHeader header;
  uint32_t render_target_count;
  uint32_t flags;
  uint8_t has_depth_stencil;
};

struct CmdSetProtectedResourceSession {
  CmdHeader header;
  ID3D12ProtectedResourceSession *protected_session;
};

struct CmdMetaCommand {
  CmdHeader header;
  ID3D12MetaCommand *meta_command;
  uint32_t data_size;
  uint8_t data[1];
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

struct CmdOMFrontAndBackStencilRef {
  CmdHeader header;
  uint32_t front_stencil_ref;
  uint32_t back_stencil_ref;
};

struct CmdRSSetDepthBias {
  CmdHeader header;
  float depth_bias;
  float depth_bias_clamp;
  float slope_scaled_depth_bias;
};

struct CmdIASetIndexBufferStripCutValue {
  CmdHeader header;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut_value;
};

struct CmdClearRTV {
  CmdHeader header;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  float color[4];
  uint32_t rect_count;
  RECT rects[1];
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
  uint32_t rect_count;
  RECT rects[1];
};

struct CmdDiscardResource {
  CmdHeader header;
  ID3D12Resource *resource;
  uint32_t first_subresource;
  uint32_t num_subresources;
  uint32_t rect_count;
  RECT rects[1];
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
