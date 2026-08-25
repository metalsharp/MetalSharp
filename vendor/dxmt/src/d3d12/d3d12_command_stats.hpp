#pragma once

#include "d3d12_command_defs.hpp"
#include <cstddef>
#include <cstdint>

namespace dxmt {

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
           rs_set_scissors_count;
  }

  bool HasClearOrComputeWork() const {
    return dispatch_count || clear_rtv_count || clear_dsv_count ||
           clear_uav_count;
  }

  bool IsZeroDrawGraphicsList() const {
    return command_count && AnyDrawCount() == 0 && HasGraphicsSetup();
  }

  bool IsDrawBearing() const { return AnyDrawCount() != 0; }

  bool IsFrameProgressCandidate() const {
    return IsDrawBearing() || IsZeroDrawGraphicsList() || HasClearOrComputeWork();
  }
};

inline void D3D12AccumulateCommandType(D3D12CommandStreamStats &stats,
                                       CmdType type) {
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
  default:
    break;
  }
}

inline D3D12CommandStreamStats
D3D12CollectCommandStreamStats(const uint8_t *data, size_t size) {
  D3D12CommandStreamStats stats = {};
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
