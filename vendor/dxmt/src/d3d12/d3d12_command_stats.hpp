#pragma once

#include "d3d12_command_defs.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace dxmt {

constexpr size_t kD3D12CommandTypeCount =
    static_cast<size_t>(CmdType::Count);
constexpr size_t kD3D12CommandTypeMaskWords =
    (kD3D12CommandTypeCount + 63) / 64;
constexpr std::array<uint64_t, kD3D12CommandTypeMaskWords>
    kD3D12AllCommandTypesMask = [] {
      std::array<uint64_t, kD3D12CommandTypeMaskWords> mask = {};
      for (size_t i = 0; i < kD3D12CommandTypeCount; ++i)
        mask[i / 64] |= uint64_t{1} << (i % 64);
      return mask;
    }();

inline bool D3D12IsKnownCommandType(CmdType type) {
  return static_cast<size_t>(type) < kD3D12CommandTypeCount;
}

inline const char *D3D12CommandTypeName(CmdType type) {
  switch (type) {
  case CmdType::DrawInstanced: return "DrawInstanced";
  case CmdType::DrawIndexedInstanced: return "DrawIndexedInstanced";
  case CmdType::Dispatch: return "Dispatch";
  case CmdType::DispatchMesh: return "DispatchMesh";
  case CmdType::ExecuteIndirect: return "ExecuteIndirect";
  case CmdType::SetPredication: return "SetPredication";
  case CmdType::SetMarker: return "SetMarker";
  case CmdType::BeginEvent: return "BeginEvent";
  case CmdType::EndEvent: return "EndEvent";
  case CmdType::SetSamplePositions: return "SetSamplePositions";
  case CmdType::SetViewInstanceMask: return "SetViewInstanceMask";
  case CmdType::SetStreamOutputTargets: return "SetStreamOutputTargets";
  case CmdType::BeginRenderPass: return "BeginRenderPass";
  case CmdType::EndRenderPass: return "EndRenderPass";
  case CmdType::SetProtectedResourceSession: return "SetProtectedResourceSession";
  case CmdType::InitializeMetaCommand: return "InitializeMetaCommand";
  case CmdType::ExecuteMetaCommand: return "ExecuteMetaCommand";
  case CmdType::CopyBufferRegion: return "CopyBufferRegion";
  case CmdType::CopyTextureRegion: return "CopyTextureRegion";
  case CmdType::CopyResource: return "CopyResource";
  case CmdType::SetPipelineState: return "SetPipelineState";
  case CmdType::SetGraphicsRootSignature: return "SetGraphicsRootSignature";
  case CmdType::SetComputeRootSignature: return "SetComputeRootSignature";
  case CmdType::SetGraphicsRoot32BitConstants: return "SetGraphicsRoot32BitConstants";
  case CmdType::SetComputeRoot32BitConstants: return "SetComputeRoot32BitConstants";
  case CmdType::SetGraphicsRootConstantBufferView: return "SetGraphicsRootConstantBufferView";
  case CmdType::SetComputeRootConstantBufferView: return "SetComputeRootConstantBufferView";
  case CmdType::SetGraphicsRootShaderResourceView: return "SetGraphicsRootShaderResourceView";
  case CmdType::SetComputeRootShaderResourceView: return "SetComputeRootShaderResourceView";
  case CmdType::SetGraphicsRootUnorderedAccessView: return "SetGraphicsRootUnorderedAccessView";
  case CmdType::SetComputeRootUnorderedAccessView: return "SetComputeRootUnorderedAccessView";
  case CmdType::SetGraphicsRootDescriptorTable: return "SetGraphicsRootDescriptorTable";
  case CmdType::SetComputeRootDescriptorTable: return "SetComputeRootDescriptorTable";
  case CmdType::IASetPrimitiveTopology: return "IASetPrimitiveTopology";
  case CmdType::IASetVertexBuffers: return "IASetVertexBuffers";
  case CmdType::IASetIndexBuffer: return "IASetIndexBuffer";
  case CmdType::RSSetViewports: return "RSSetViewports";
  case CmdType::RSSetScissorRects: return "RSSetScissorRects";
  case CmdType::OMSetRenderTargets: return "OMSetRenderTargets";
  case CmdType::OMSetBlendFactor: return "OMSetBlendFactor";
  case CmdType::OMSetStencilRef: return "OMSetStencilRef";
  case CmdType::ClearRenderTargetView: return "ClearRenderTargetView";
  case CmdType::ClearDepthStencilView: return "ClearDepthStencilView";
  case CmdType::ClearUnorderedAccessView: return "ClearUnorderedAccessView";
  case CmdType::DiscardResource: return "DiscardResource";
  case CmdType::ResourceBarrier: return "ResourceBarrier";
  case CmdType::EnhancedBarrier: return "EnhancedBarrier";
  case CmdType::SetDescriptorHeaps: return "SetDescriptorHeaps";
  case CmdType::ResolveSubresource: return "ResolveSubresource";
  case CmdType::WriteBufferImmediate: return "WriteBufferImmediate";
  case CmdType::BeginQuery: return "BeginQuery";
  case CmdType::EndQuery: return "EndQuery";
  case CmdType::ResolveQueryData: return "ResolveQueryData";
  case CmdType::BuildRaytracingAccelerationStructure: return "BuildRaytracingAccelerationStructure";
  case CmdType::CopyRaytracingAccelerationStructure: return "CopyRaytracingAccelerationStructure";
  case CmdType::EmitRaytracingAccelerationStructurePostbuildInfo: return "EmitRaytracingAccelerationStructurePostbuildInfo";
  case CmdType::SetPipelineState1: return "SetPipelineState1";
  case CmdType::DispatchRays: return "DispatchRays";
  case CmdType::OMSetDepthBounds: return "OMSetDepthBounds";
  case CmdType::RSSetShadingRate: return "RSSetShadingRate";
  case CmdType::RSSetShadingRateImage: return "RSSetShadingRateImage";
  case CmdType::OMSetFrontAndBackStencilRef: return "OMSetFrontAndBackStencilRef";
  case CmdType::RSSetDepthBias: return "RSSetDepthBias";
  case CmdType::IASetIndexBufferStripCutValue: return "IASetIndexBufferStripCutValue";
  case CmdType::SetProgram: return "SetProgram";
  case CmdType::DispatchGraph: return "DispatchGraph";
  case CmdType::Count: break;
  }
  return "Unknown";
}

struct D3D12CommandStreamStats {
  uint32_t command_count = 0;
  uint32_t draw_count = 0;
  uint32_t indexed_draw_count = 0;
  uint32_t indirect_count = 0;
  uint32_t dispatch_count = 0;
  uint32_t clear_rtv_count = 0;
  uint32_t clear_dsv_count = 0;
  uint32_t clear_uav_count = 0;
  uint32_t set_pso_count = 0;
  uint32_t set_graphics_root_sig_count = 0;
  uint32_t set_graphics_root_constants_count = 0;
  uint32_t set_graphics_root_cbv_count = 0;
  uint32_t set_graphics_root_srv_count = 0;
  uint32_t set_graphics_root_uav_count = 0;
  uint32_t set_graphics_root_table_count = 0;
  uint32_t set_compute_root_sig_count = 0;
  uint32_t set_compute_root_constants_count = 0;
  uint32_t set_compute_root_cbv_count = 0;
  uint32_t set_compute_root_srv_count = 0;
  uint32_t set_compute_root_uav_count = 0;
  uint32_t set_compute_root_table_count = 0;
  uint32_t om_set_render_targets_count = 0;
  uint32_t ia_set_vertex_buffers_count = 0;
  uint32_t ia_set_index_buffer_count = 0;
  uint32_t rs_set_viewports_count = 0;
  uint32_t rs_set_scissors_count = 0;
  uint32_t set_view_instance_mask_count = 0;
  uint32_t set_stream_output_targets_count = 0;
  uint32_t begin_render_pass_count = 0;
  uint32_t end_render_pass_count = 0;
  uint32_t set_protected_resource_session_count = 0;
  uint32_t initialize_meta_command_count = 0;
  uint32_t execute_meta_command_count = 0;
  std::array<uint32_t, kD3D12CommandTypeCount> type_counts = {};
  std::array<uint64_t, kD3D12CommandTypeMaskWords> type_mask = {};
  uint32_t unknown_type_count = 0;
  bool corrupt = false;
  size_t corrupt_offset = 0;
  uint32_t corrupt_type = 0;
  uint32_t corrupt_size = 0;

  uint32_t DirectDrawCount() const {
    return draw_count + indexed_draw_count;
  }

  uint32_t AnyDrawCount() const {
    return DirectDrawCount() + indirect_count;
  }

  bool HasGraphicsSetup() const {
    return set_pso_count || set_graphics_root_sig_count ||
           set_graphics_root_constants_count || set_graphics_root_cbv_count ||
           set_graphics_root_srv_count || set_graphics_root_uav_count ||
           set_graphics_root_table_count ||
           om_set_render_targets_count || ia_set_vertex_buffers_count ||
           ia_set_index_buffer_count || rs_set_viewports_count ||
           rs_set_scissors_count || set_view_instance_mask_count ||
           set_stream_output_targets_count || begin_render_pass_count ||
           end_render_pass_count;
  }

  bool HasClearOrComputeWork() const {
    return dispatch_count || clear_rtv_count || clear_dsv_count ||
           clear_uav_count;
  }

  bool IsZeroDrawGraphicsList() const {
    return command_count && AnyDrawCount() == 0 && HasGraphicsSetup();
  }

  bool IsDrawBearing() const { return AnyDrawCount() != 0; }

  bool HasUnknownCommandTypes() const { return unknown_type_count != 0; }

  bool HasCompleteKnownTypeInventory() const {
    return !unknown_type_count && type_mask == kD3D12AllCommandTypesMask;
  }

  bool IsFrameProgressCandidate() const {
    return IsDrawBearing() || IsZeroDrawGraphicsList() || HasClearOrComputeWork();
  }
};

inline void D3D12AccumulateCommandType(D3D12CommandStreamStats &stats,
                                       CmdType type) {
  const size_t type_index = static_cast<size_t>(type);
  if (type_index < kD3D12CommandTypeCount) {
    stats.type_counts[type_index]++;
    stats.type_mask[type_index / 64] |= uint64_t{1} << (type_index % 64);
  } else {
    stats.unknown_type_count++;
  }
  switch (type) {
  case CmdType::DrawInstanced:
  case CmdType::DispatchMesh:
    stats.draw_count++;
    break;
  case CmdType::DrawIndexedInstanced:
    stats.indexed_draw_count++;
    break;
  case CmdType::ExecuteIndirect:
    stats.indirect_count++;
    break;
  case CmdType::Dispatch:
  case CmdType::DispatchGraph:
  case CmdType::DispatchRays:
  case CmdType::BuildRaytracingAccelerationStructure:
  case CmdType::EmitRaytracingAccelerationStructurePostbuildInfo:
    stats.dispatch_count++;
    break;
  case CmdType::ClearRenderTargetView:
    stats.clear_rtv_count++;
    break;
  case CmdType::ClearDepthStencilView:
    stats.clear_dsv_count++;
    break;
  case CmdType::ClearUnorderedAccessView:
    stats.clear_uav_count++;
    break;
  case CmdType::SetPipelineState:
  case CmdType::SetPipelineState1:
  case CmdType::SetProgram:
    stats.set_pso_count++;
    break;
  case CmdType::SetGraphicsRootSignature:
    stats.set_graphics_root_sig_count++;
    break;
  case CmdType::SetGraphicsRoot32BitConstants:
    stats.set_graphics_root_constants_count++;
    break;
  case CmdType::SetGraphicsRootConstantBufferView:
    stats.set_graphics_root_cbv_count++;
    break;
  case CmdType::SetGraphicsRootShaderResourceView:
    stats.set_graphics_root_srv_count++;
    break;
  case CmdType::SetGraphicsRootUnorderedAccessView:
    stats.set_graphics_root_uav_count++;
    break;
  case CmdType::SetGraphicsRootDescriptorTable:
    stats.set_graphics_root_table_count++;
    break;
  case CmdType::SetComputeRootSignature:
    stats.set_compute_root_sig_count++;
    break;
  case CmdType::SetComputeRoot32BitConstants:
    stats.set_compute_root_constants_count++;
    break;
  case CmdType::SetComputeRootConstantBufferView:
    stats.set_compute_root_cbv_count++;
    break;
  case CmdType::SetComputeRootShaderResourceView:
    stats.set_compute_root_srv_count++;
    break;
  case CmdType::SetComputeRootUnorderedAccessView:
    stats.set_compute_root_uav_count++;
    break;
  case CmdType::SetComputeRootDescriptorTable:
    stats.set_compute_root_table_count++;
    break;
  case CmdType::OMSetRenderTargets:
    stats.om_set_render_targets_count++;
    break;
  case CmdType::IASetVertexBuffers:
    stats.ia_set_vertex_buffers_count++;
    break;
  case CmdType::IASetIndexBuffer:
    stats.ia_set_index_buffer_count++;
    break;
  case CmdType::RSSetViewports:
    stats.rs_set_viewports_count++;
    break;
  case CmdType::RSSetScissorRects:
    stats.rs_set_scissors_count++;
    break;
  case CmdType::SetViewInstanceMask:
    stats.set_view_instance_mask_count++;
    break;
  case CmdType::SetStreamOutputTargets:
    stats.set_stream_output_targets_count++;
    break;
  case CmdType::BeginRenderPass:
    stats.begin_render_pass_count++;
    break;
  case CmdType::EndRenderPass:
    stats.end_render_pass_count++;
    break;
  case CmdType::SetProtectedResourceSession:
    stats.set_protected_resource_session_count++;
    break;
  case CmdType::InitializeMetaCommand:
    stats.initialize_meta_command_count++;
    break;
  case CmdType::ExecuteMetaCommand:
    stats.execute_meta_command_count++;
    break;
  default:
    break;
  }
}

inline D3D12CommandStreamStats
D3D12CollectCommandStreamStats(const uint8_t *data, size_t size) {
  D3D12CommandStreamStats stats = {};
  if (size && !data) {
    stats.corrupt = true;
    stats.corrupt_size = size > UINT32_MAX ? UINT32_MAX
                                            : static_cast<uint32_t>(size);
    return stats;
  }
  size_t offset = 0;
  while (offset < size) {
    if (offset + sizeof(CmdHeader) > size) {
      stats.corrupt = true;
      stats.corrupt_offset = offset;
      break;
    }

    const auto *header = reinterpret_cast<const CmdHeader *>(data + offset);
    if (header->size < sizeof(CmdHeader) || header->size > 65536 ||
        offset + header->size > size) {
      stats.corrupt = true;
      stats.corrupt_offset = offset;
      stats.corrupt_type = static_cast<uint32_t>(header->type);
      stats.corrupt_size = header->size;
      break;
    }

    stats.command_count++;
    D3D12AccumulateCommandType(stats, header->type);
    offset += header->size;
  }
  return stats;
}

} // namespace dxmt
