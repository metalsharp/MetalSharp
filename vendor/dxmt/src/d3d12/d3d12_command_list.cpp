#include "d3d12_command_list.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_stats.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pipeline_state.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstddef>

#define CLTRACE(fmt, ...) do { FILE *_tf = dxmt::openDiagnosticLog("dxmt-d3d12-trace.log"); if (_tf) { fprintf(_tf, "CmdList::" fmt "\n", ##__VA_ARGS__); fclose(_tf); } } while(0)

namespace dxmt {

static uint64_t g_command_list_debug_id = 0;
static uint32_t g_command_list_lifecycle_logs = 0;

static bool TakeCommandListLifecycleLogBudget(uint32_t limit) {
  return __atomic_add_fetch(&g_command_list_lifecycle_logs, 1,
                            __ATOMIC_RELAXED) <= limit;
}

static void LogCommandListLifecycle(const char *label, uint64_t id,
                                    D3D12_COMMAND_LIST_TYPE type,
                                    const std::vector<uint8_t> &commands,
                                    bool closed) {
  auto stats = D3D12CollectCommandStreamStats(commands.data(), commands.size());
  if (!stats.IsFrameProgressCandidate() && !stats.corrupt)
    return;
  if (!TakeCommandListLifecycleLogBudget(256))
    return;

  Logger::info(str::format(
      "M12 command list ", label, " id=", (unsigned long long)id,
      " type=", (unsigned)type, " closed=", closed,
      " cmds=", stats.command_count, " draws=", stats.draw_count,
      " indexed=", stats.indexed_draw_count,
      " indirect=", stats.indirect_count, " dispatch=", stats.dispatch_count,
      " clear_rtv=", stats.clear_rtv_count,
      " clear_dsv=", stats.clear_dsv_count,
      " clear_uav=", stats.clear_uav_count,
      " graphics_setup=", stats.HasGraphicsSetup(),
      " gfx_roots=", stats.set_graphics_root_sig_count,
      " gfx_tables=", stats.set_graphics_root_table_count,
      " gfx_cbv=", stats.set_graphics_root_cbv_count,
      " gfx_srv=", stats.set_graphics_root_srv_count,
      " gfx_uav=", stats.set_graphics_root_uav_count,
      " gfx_consts=", stats.set_graphics_root_constants_count,
      " comp_roots=", stats.set_compute_root_sig_count,
      " comp_tables=", stats.set_compute_root_table_count,
      " comp_cbv=", stats.set_compute_root_cbv_count,
      " comp_srv=", stats.set_compute_root_srv_count,
      " comp_uav=", stats.set_compute_root_uav_count,
      " comp_consts=", stats.set_compute_root_constants_count,
      " zero_draw=", stats.IsZeroDrawGraphicsList(),
      " draw_bearing=", stats.IsDrawBearing(),
      " corrupt=", stats.corrupt));
}

MTLD3D12GraphicsCommandList::MTLD3D12GraphicsCommandList(
    MTLD3D12Device *device, MTLD3D12CommandAllocator *allocator,
    D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState *initial_state)
    : m_device(device), m_allocator(allocator), m_type(type) {
  m_debug_id =
      __atomic_add_fetch(&g_command_list_debug_id, 1, __ATOMIC_RELAXED);
  m_device->AddRef();
  if (m_allocator)
    m_allocator->AddRef();
  if (initial_state) {
    CmdSetPipelineState cmd = {};
    cmd.header = {CmdType::SetPipelineState, sizeof(cmd)};
    cmd.pso = initial_state;
    RetainPipelineState(initial_state);
    Emit(cmd);
  }
  LogCommandListLifecycle("create", m_debug_id, m_type, m_cmds, m_closed);
}

MTLD3D12GraphicsCommandList::~MTLD3D12GraphicsCommandList() {
  ReleaseReferencedPipelineStates();
  if (m_allocator)
    m_allocator->Release();
  m_device->Release();
}

void MTLD3D12GraphicsCommandList::RetainPipelineState(
    ID3D12PipelineState *pipeline_state) {
  if (!pipeline_state)
    return;
  pipeline_state->AddRef();
  m_referenced_pipeline_states.push_back(pipeline_state);
}

void MTLD3D12GraphicsCommandList::RetainStateObject(
    ID3D12StateObject *state_object) {
  if (!state_object)
    return;
  state_object->AddRef();
  m_referenced_state_objects.push_back(state_object);
}

void MTLD3D12GraphicsCommandList::RetainResource(ID3D12Resource *resource) {
  if (!resource)
    return;
  resource->AddRef();
  m_referenced_resources.push_back(resource);
}

void MTLD3D12GraphicsCommandList::RetainGPUAddress(
    D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!address)
    return;
  RetainResource(m_device->LookupResourceByGPUAddress(address));
}

void MTLD3D12GraphicsCommandList::RetainDescriptor(
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!descriptor.ptr)
    return;
  auto *entry = reinterpret_cast<D3D12Descriptor *>(descriptor.ptr);
  if (!entry || !entry->owner)
    return;
  entry->owner->AddRef();
  m_referenced_descriptor_heaps.push_back(entry->owner);
  RetainResource(entry->resource);
  RetainResource(entry->resource_uav_counter);
  RetainResource(entry->sampler_feedback_target);
}

void MTLD3D12GraphicsCommandList::RetainRootSignature(
    ID3D12RootSignature *root_signature) {
  if (!root_signature)
    return;
  root_signature->AddRef();
  m_referenced_root_signatures.push_back(root_signature);
}

void MTLD3D12GraphicsCommandList::RetainQueryHeap(
    ID3D12QueryHeap *query_heap) {
  if (!query_heap)
    return;
  query_heap->AddRef();
  m_referenced_query_heaps.push_back(query_heap);
}

void MTLD3D12GraphicsCommandList::RetainCommandSignature(
    ID3D12CommandSignature *signature) {
  if (!signature)
    return;
  signature->AddRef();
  m_referenced_command_signatures.push_back(signature);
}

void MTLD3D12GraphicsCommandList::RetainReferencedObjectsInto(
    MTLD3D12GraphicsCommandList *target) const {
  if (!target)
    return;
  for (auto *pipeline_state : m_referenced_pipeline_states)
    target->RetainPipelineState(pipeline_state);
  for (auto *state_object : m_referenced_state_objects)
    target->RetainStateObject(state_object);
  for (auto *descriptor_heap : m_referenced_descriptor_heaps) {
    if (descriptor_heap) {
      descriptor_heap->AddRef();
      target->m_referenced_descriptor_heaps.push_back(descriptor_heap);
    }
  }
  for (auto *root_signature : m_referenced_root_signatures)
    target->RetainRootSignature(root_signature);
  for (auto *query_heap : m_referenced_query_heaps)
    target->RetainQueryHeap(query_heap);
  for (auto *signature : m_referenced_command_signatures)
    target->RetainCommandSignature(signature);
  for (auto *resource : m_referenced_resources)
    target->RetainResource(resource);
}

void MTLD3D12GraphicsCommandList::ReleaseReferencedPipelineStates() {
  for (auto *pipeline_state : m_referenced_pipeline_states) {
    if (pipeline_state)
      pipeline_state->Release();
  }
  m_referenced_pipeline_states.clear();
  for (auto *state_object : m_referenced_state_objects) {
    if (state_object)
      state_object->Release();
  }
  m_referenced_state_objects.clear();
  for (auto *descriptor_heap : m_referenced_descriptor_heaps) {
    if (descriptor_heap)
      descriptor_heap->Release();
  }
  m_referenced_descriptor_heaps.clear();
  for (auto *root_signature : m_referenced_root_signatures) {
    if (root_signature)
      root_signature->Release();
  }
  m_referenced_root_signatures.clear();
  for (auto *query_heap : m_referenced_query_heaps) {
    if (query_heap)
      query_heap->Release();
  }
  m_referenced_query_heaps.clear();
  for (auto *signature : m_referenced_command_signatures) {
    if (signature)
      signature->Release();
  }
  m_referenced_command_signatures.clear();
  for (auto *resource : m_referenced_resources) {
    if (resource)
      resource->Release();
  }
  m_referenced_resources.clear();
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12CommandList ||
      riid == IID_ID3D12GraphicsCommandList ||
      riid == IID_ID3D12GraphicsCommandList1 ||
      riid == IID_ID3D12GraphicsCommandList2 ||
      riid == IID_ID3D12GraphicsCommandList3 ||
      riid == IID_ID3D12GraphicsCommandList4 ||
      riid == IID_ID3D12GraphicsCommandList5 ||
      riid == IID_ID3D12GraphicsCommandList6 ||
      riid == IID_ID3D12GraphicsCommandList7) {
    *ppvObject = ref(this);
    return S_OK;
  }
  CLTRACE("QI unknown IID %s -> E_NOINTERFACE", str::format(riid).c_str());
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::AddRef() {
  return ++m_refCount;
}

ULONG STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::Release() {
  uint32_t rc = --m_refCount;
  if (!rc) {
    uint32_t rp = --m_refPrivate;
    if (!rp) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }
  return rc;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::GetPrivateData(REFGUID guid, UINT *data_size,
                                            void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetPrivateData(REFGUID guid, UINT data_size,
                                            const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetPrivateDataInterface(REFGUID guid,
                                                     const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::GetDevice(REFIID riid, void **device) {
  return m_device->QueryInterface(riid, device);
}

D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::GetType() {
  return m_type;
}

HRESULT STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::Close() {
  CLTRACE("Close");
  if (m_closed)
    return E_FAIL;
  m_closed = true;
  LogCommandListLifecycle("close", m_debug_id, m_type, m_cmds, m_closed);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::Reset(
    ID3D12CommandAllocator *allocator, ID3D12PipelineState *initial_state) {
  CLTRACE("Reset");
  if (!m_closed || !allocator)
    return !m_closed ? E_FAIL : E_INVALIDARG;
  auto *new_allocator = static_cast<MTLD3D12CommandAllocator *>(allocator);
  if (!new_allocator || new_allocator->GetType() != m_type)
    return E_INVALIDARG;
  if (new_allocator != m_allocator) {
    new_allocator->AddRef();
    if (m_allocator)
      m_allocator->Release();
    m_allocator = new_allocator;
  }
  m_closed = false;
  ReleaseReferencedPipelineStates();
  m_cmds.clear();
  if (initial_state) {
    CmdSetPipelineState cmd = {};
    cmd.header = {CmdType::SetPipelineState, sizeof(cmd)};
    cmd.pso = initial_state;
    RetainPipelineState(initial_state);
    Emit(cmd);
  }
  LogCommandListLifecycle("reset", m_debug_id, m_type, m_cmds, m_closed);
  return S_OK;
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::ClearState(ID3D12PipelineState *pipeline_state) {
  ReleaseReferencedPipelineStates();
  m_cmds.clear();
  if (pipeline_state) {
    CmdSetPipelineState cmd = {};
    cmd.header = {CmdType::SetPipelineState, sizeof(cmd)};
    cmd.pso = pipeline_state;
    RetainPipelineState(pipeline_state);
    Emit(cmd);
  }
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::DrawInstanced(
    UINT vertex_count, UINT instance_count, UINT start_vertex,
    UINT start_instance) {
  CLTRACE("DrawInstanced v=%u i=%u", vertex_count, instance_count);
  CmdDrawInstanced cmd = {};
  cmd.header = {CmdType::DrawInstanced, sizeof(cmd)};
  cmd.vertex_count = vertex_count;
  cmd.instance_count = instance_count;
  cmd.start_vertex = start_vertex;
  cmd.start_instance = start_instance;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::DrawIndexedInstanced(
    UINT index_count, UINT instance_count, UINT start_vertex,
    INT base_vertex, UINT start_instance) {
  CLTRACE("DrawIndexedInstanced idx=%u i=%u", index_count, instance_count);
  CmdDrawIndexedInstanced cmd = {};
  cmd.header = {CmdType::DrawIndexedInstanced, sizeof(cmd)};
  cmd.index_count = index_count;
  cmd.instance_count = instance_count;
  cmd.start_index = start_vertex;
  cmd.base_vertex = base_vertex;
  cmd.start_instance = start_instance;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::Dispatch(UINT x, UINT y,
                                                             UINT z) {
  CLTRACE("Dispatch %ux%ux%u", x, y, z);
  CmdDispatch cmd = {};
  cmd.header = {CmdType::Dispatch, sizeof(cmd)};
  cmd.x = x;
  cmd.y = y;
  cmd.z = z;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::CopyBufferRegion(
    ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src,
    UINT64 src_offset, UINT64 byte_count) {
  CmdCopyBufferRegion cmd = {};
  cmd.header = {CmdType::CopyBufferRegion, sizeof(cmd)};
  cmd.dst = dst;
  cmd.dst_offset = dst_offset;
  cmd.src = src;
  cmd.src_offset = src_offset;
  cmd.byte_count = byte_count;
  RetainResource(dst);
  RetainResource(src);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::CopyTextureRegion(
    const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x, UINT dst_y,
    UINT dst_z, const D3D12_TEXTURE_COPY_LOCATION *src,
    const D3D12_BOX *src_box) {
  if (!dst || !src) return;
  CmdCopyTextureRegion cmd = {};
  cmd.header = {CmdType::CopyTextureRegion, sizeof(cmd)};
  cmd.dst_resource = dst->pResource;
  cmd.dst_type = dst->Type;
  cmd.dst_x = dst_x;
  cmd.dst_y = dst_y;
  cmd.dst_z = dst_z;
  if (dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    cmd.dst_subresource = dst->SubresourceIndex;
  } else {
    cmd.dst_offset = dst->PlacedFootprint.Offset;
    cmd.dst_footprint_format = dst->PlacedFootprint.Footprint.Format;
    cmd.dst_footprint_width = dst->PlacedFootprint.Footprint.Width;
    cmd.dst_footprint_height = dst->PlacedFootprint.Footprint.Height;
    cmd.dst_footprint_depth = dst->PlacedFootprint.Footprint.Depth;
    cmd.dst_footprint_row_pitch = dst->PlacedFootprint.Footprint.RowPitch;
  }
  cmd.src_resource = src->pResource;
  cmd.src_type = src->Type;
  if (src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
    cmd.src_subresource = src->SubresourceIndex;
  } else {
    cmd.src_offset = src->PlacedFootprint.Offset;
    cmd.src_footprint_format = src->PlacedFootprint.Footprint.Format;
    cmd.src_footprint_width = src->PlacedFootprint.Footprint.Width;
    cmd.src_footprint_height = src->PlacedFootprint.Footprint.Height;
    cmd.src_footprint_depth = src->PlacedFootprint.Footprint.Depth;
    cmd.src_footprint_row_pitch = src->PlacedFootprint.Footprint.RowPitch;
  }
  if (src_box) {
    cmd.src_box = *src_box;
    cmd.has_src_box = 1;
  }
  RetainResource(dst->pResource);
  RetainResource(src->pResource);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::CopyResource(ID3D12Resource *dst,
                                          ID3D12Resource *src) {
  if (!dst || !src) return;
  CmdCopyResource cmd = {};
  cmd.header = {CmdType::CopyResource, sizeof(cmd)};
  cmd.dst = dst;
  cmd.src = src;
  RetainResource(dst);
  RetainResource(src);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::CopyTiles(
    ID3D12Resource *tiled_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
    const D3D12_TILE_REGION_SIZE *tile_region_size,
    ID3D12Resource *buffer, UINT64 buffer_offset,
    D3D12_TILE_COPY_FLAGS flags) {
  auto *resource = static_cast<MTLD3D12Resource *>(tiled_resource);
  if (!resource || !resource->IsSparseBacked() || !tile_region_start_coordinate ||
      !tile_region_size || !buffer || !tile_region_size->NumTiles) {
    CLTRACE("CopyTiles rejected resource=%p buffer=%p tiles=%u flags=0x%x",
            (void *)tiled_resource, (void *)buffer,
            tile_region_size ? tile_region_size->NumTiles : 0,
            (unsigned)flags);
    return;
  }
  D3D12_RESOURCE_DESC resource_desc = {};
  resource->GetDesc(&resource_desc);
  const UINT allowed_flags =
      D3D12_TILE_COPY_FLAG_NO_HAZARD |
      D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE |
      D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
  if ((static_cast<UINT>(flags) & ~allowed_flags) != 0) {
    CLTRACE("CopyTiles rejected unknown flags=0x%x", (unsigned)flags);
    return;
  }
  const bool tile_buffer_to_resource =
      (flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) !=
      0;
  const bool tile_resource_to_buffer =
      (flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER) !=
      0;
  if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    D3D12_RESOURCE_DESC source_desc = {};
    static_cast<MTLD3D12Resource *>(buffer)->GetDesc(&source_desc);
    const auto &coordinate = *tile_region_start_coordinate;
    const uint64_t tile_size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    const uint64_t total_tiles =
        (resource_desc.Width + tile_size - 1) / tile_size;
    const uint64_t first_tile = coordinate.X;
    const uint64_t tile_count = tile_region_size->NumTiles;
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        tile_region_size->UseBox ||
        tile_buffer_to_resource == tile_resource_to_buffer || coordinate.Y ||
        coordinate.Z || coordinate.Subresource ||
        buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0 ||
        first_tile >= total_tiles || tile_count > total_tiles - first_tile ||
        buffer_offset > source_desc.Width ||
        tile_count * tile_size > source_desc.Width - buffer_offset) {
      CLTRACE("CopyTiles rejected buffer range x=%llu tiles=%llu offset=%llu",
              (unsigned long long)first_tile, (unsigned long long)tile_count,
              (unsigned long long)buffer_offset);
      return;
    }
    for (uint64_t tile = 0; tile < tile_count; tile++) {
      const uint64_t tiled_offset = (first_tile + tile) * tile_size;
      const uint64_t linear_offset = buffer_offset + tile * tile_size;
      if (tile_buffer_to_resource)
        CopyBufferRegion(tiled_resource, tiled_offset, buffer, linear_offset,
                         tile_size);
      else
        CopyBufferRegion(buffer, linear_offset, tiled_resource, tiled_offset,
                         tile_size);
    }
    CLTRACE("CopyTiles buffer %s tiles=%u buffer=%p+%llu",
            tile_buffer_to_resource ? "to_reserved" : "from_reserved",
            tile_region_size->NumTiles, (void *)buffer,
            (unsigned long long)buffer_offset);
    return;
  }
  resource->GetDesc(&resource_desc);
  const D3D12_TILE_SHAPE shape = resource->GetTiledResourceTileShape();
  const bool buffer_to_texture =
      (flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) !=
      0;
  const bool texture_to_buffer =
      (flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER) !=
      0;
  const bool volume =
      resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const bool array_texture =
      resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      resource_desc.DepthOrArraySize > 1;
  if (buffer_to_texture == texture_to_buffer ||
      (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
       !volume) ||
      resource_desc.SampleDesc.Count > 1 ||
      buffer_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) {
    CLTRACE("CopyTiles rejected direction/shape flags=0x%x dim=%u samples=%u",
            (unsigned)flags, (unsigned)resource_desc.Dimension,
            resource_desc.SampleDesc.Count);
    return;
  }
  D3D12_RESOURCE_DESC buffer_desc = {};
  static_cast<MTLD3D12Resource *>(buffer)->GetDesc(&buffer_desc);
  if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
    CLTRACE("CopyTiles rejected non-buffer destination/source=%p", (void *)buffer);
    return;
  }
  const UINT bytes_per_block = [&] {
    switch (resource_desc.Format) {
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
      return 1u;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
      return 2u;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return 4u;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
      return 8u;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      return 8u;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16u;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      return 8u;
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
      return 16u;
    default:
      return 0u;
    }
  }();
  const UINT block_extent = [&] {
    switch (resource_desc.Format) {
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
      return 4u;
    default:
      return 1u;
    }
  }();
  const uint64_t tile_blocks_x =
      (shape.WidthInTexels + block_extent - 1) / block_extent;
  const uint64_t tile_blocks_y =
      (shape.HeightInTexels + block_extent - 1) / block_extent;
  if (!bytes_per_block || tile_blocks_x * tile_blocks_y *
                              shape.DepthInTexels * bytes_per_block !=
                          D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) {
    CLTRACE("CopyTiles rejected unsupported tile format=%u shape=%ux%ux%u",
            (unsigned)resource_desc.Format, shape.WidthInTexels,
            shape.HeightInTexels, shape.DepthInTexels);
    return;
  }
  const auto &coordinate = *tile_region_start_coordinate;
  const UINT mip_levels = std::max<UINT>(1, resource_desc.MipLevels);
  const UINT subresource_count =
      volume ? mip_levels
             : mip_levels * std::max<UINT16>(resource_desc.DepthOrArraySize, 1);
  if (!resource_desc.MipLevels || coordinate.Subresource >= subresource_count) {
    CLTRACE("CopyTiles rejected coordinate z=%u subresource=%u", coordinate.Z,
            coordinate.Subresource);
    return;
  }
  const UINT mip = coordinate.Subresource % mip_levels;
  const UINT mip_width = std::max<UINT>(1, resource_desc.Width >> mip);
  const UINT mip_height = std::max<UINT>(1, resource_desc.Height >> mip);
  const UINT mip_depth =
      volume ? std::max<UINT16>(1, resource_desc.DepthOrArraySize >> mip) : 1;
  if (mip_width < shape.WidthInTexels || mip_height < shape.HeightInTexels ||
      mip_depth < shape.DepthInTexels) {
    CLTRACE("CopyTiles rejected partial mip=%u size=%ux%ux%u tile=%ux%ux%u",
            mip, mip_width, mip_height, mip_depth, shape.WidthInTexels,
            shape.HeightInTexels, shape.DepthInTexels);
    return;
  }
  const UINT tiles_x =
      (mip_width + shape.WidthInTexels - 1) / shape.WidthInTexels;
  const UINT tiles_y =
      (mip_height + shape.HeightInTexels - 1) / shape.HeightInTexels;
  const UINT tiles_z =
      (mip_depth + shape.DepthInTexels - 1) / shape.DepthInTexels;
  const UINT coordinate_array_slice = coordinate.Subresource / mip_levels;
  const UINT array_tiles_z =
      array_texture && coordinate_array_slice < resource_desc.DepthOrArraySize
          ? resource_desc.DepthOrArraySize - coordinate_array_slice
          : 1;
  struct TileLocation {
    UINT subresource;
    UINT x;
    UINT y;
    UINT z;
  };
  std::vector<TileLocation> locations;
  const uint64_t region_tile_count = tile_region_size->UseBox
                                         ? uint64_t(tile_region_size->Width) *
                                               tile_region_size->Height *
                                               tile_region_size->Depth
                                         : tile_region_size->NumTiles;
  if (!region_tile_count || region_tile_count > 1048576 ||
      uint64_t(buffer_offset) + region_tile_count *
                                    D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES >
          static_cast<MTLD3D12Resource *>(buffer)->GetBufferByteLength()) {
    CLTRACE("CopyTiles rejected tile count=%llu buffer_offset=%llu",
            (unsigned long long)region_tile_count,
            (unsigned long long)buffer_offset);
    return;
  }
  locations.reserve(static_cast<size_t>(region_tile_count));
  if (tile_region_size->UseBox) {
    const UINT box_width = tile_region_size->Width;
    const UINT box_height = tile_region_size->Height;
    const UINT box_depth = tile_region_size->Depth;
    const UINT box_z_limit = volume ? tiles_z : array_tiles_z;
    if (tile_region_size->NumTiles != region_tile_count || !box_width ||
        !box_height || !box_depth || coordinate.X >= tiles_x ||
        coordinate.Y >= tiles_y || coordinate.Z >= box_z_limit ||
        uint64_t(coordinate.X) + box_width > tiles_x ||
        uint64_t(coordinate.Y) + box_height > tiles_y ||
        uint64_t(coordinate.Z) + box_depth > box_z_limit) {
      CLTRACE("CopyTiles rejected box x=%u y=%u z=%u tiles=%ux%ux%u count=%u",
              coordinate.X, coordinate.Y, coordinate.Z, box_width, box_height,
              box_depth, tile_region_size->NumTiles);
      return;
    }
    const uint64_t box_plane = uint64_t(box_width) * box_height;
    for (uint64_t tile = 0; tile < region_tile_count; tile++) {
      const UINT z = static_cast<UINT>(tile / box_plane) + coordinate.Z;
      const uint64_t box_row = tile % box_plane;
      const UINT y = static_cast<UINT>(box_row / box_width) + coordinate.Y;
      const UINT x = static_cast<UINT>(box_row % box_width) + coordinate.X;
      const UINT subresource =
          array_texture
              ? ((coordinate_array_slice + z) * mip_levels + mip)
              : coordinate.Subresource;
      locations.push_back({subresource, x, y, array_texture ? 0 : z});
    }
  } else {
    if (!volume && coordinate.Z)
      return;
    // A non-box region walks X, Y, Z and then spills into subsequent
    // subresources.  Keeping the expanded locations here also makes the
    // generated CopyTextureRegion records preserve the correct mip/array
    // when one request crosses a row, volume plane, or mip boundary.
    UINT current_subresource = coordinate.Subresource;
    UINT current_x = coordinate.X;
    UINT current_y = coordinate.Y;
    UINT current_z = coordinate.Z;
    for (uint64_t tile = 0; tile < region_tile_count; tile++) {
      if (current_subresource >= subresource_count)
        return;
      const UINT current_mip = current_subresource % mip_levels;
      const UINT current_width =
          std::max<UINT>(1, resource_desc.Width >> current_mip);
      const UINT current_height =
          std::max<UINT>(1, resource_desc.Height >> current_mip);
      const UINT current_depth =
          volume ? std::max<UINT16>(
                       1, resource_desc.DepthOrArraySize >> current_mip)
                 : 1;
      const UINT current_tiles_x =
          (current_width + shape.WidthInTexels - 1) / shape.WidthInTexels;
      const UINT current_tiles_y =
          (current_height + shape.HeightInTexels - 1) /
          shape.HeightInTexels;
      const UINT current_tiles_z =
          (current_depth + shape.DepthInTexels - 1) / shape.DepthInTexels;
      if (current_width < shape.WidthInTexels ||
          current_height < shape.HeightInTexels ||
          current_depth < shape.DepthInTexels || current_x >= current_tiles_x ||
          current_y >= current_tiles_y || current_z >= current_tiles_z) {
        CLTRACE("CopyTiles rejected spill subresource=%u x=%u y=%u z=%u",
                current_subresource, current_x, current_y, current_z);
        return;
      }
      locations.push_back(
          {current_subresource, current_x, current_y, current_z});
      const uint64_t linear =
          (uint64_t(current_z) * current_tiles_y + current_y) *
              current_tiles_x +
          current_x + 1;
      if (linear == uint64_t(current_tiles_x) * current_tiles_y *
                        current_tiles_z) {
        current_subresource++;
        current_x = current_y = current_z = 0;
      } else {
        current_x = static_cast<UINT>(linear % current_tiles_x);
        const uint64_t plane = linear / current_tiles_x;
        current_y = static_cast<UINT>(plane % current_tiles_y);
        current_z = static_cast<UINT>(plane / current_tiles_y);
      }
    }
  }
  const UINT row_pitch =
      static_cast<UINT>(tile_blocks_x * bytes_per_block);
  for (uint64_t tile = 0; tile < locations.size(); tile++) {
    const TileLocation &location = locations[static_cast<size_t>(tile)];
    const UINT x = location.x;
    const UINT y = location.y;
    const UINT z = location.z;
    CmdCopyTextureRegion cmd = {};
    cmd.header = {CmdType::CopyTextureRegion, sizeof(cmd)};
    if (buffer_to_texture) {
      cmd.dst_resource = tiled_resource;
      cmd.dst_type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cmd.dst_subresource = location.subresource;
      cmd.dst_x = x * shape.WidthInTexels;
      cmd.dst_y = y * shape.HeightInTexels;
      cmd.dst_z = z * shape.DepthInTexels;
      cmd.src_resource = buffer;
      cmd.src_type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      cmd.src_offset = buffer_offset +
                       uint64_t(tile) *
                           D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
      cmd.src_footprint_format = resource_desc.Format;
      cmd.src_footprint_width = shape.WidthInTexels;
      cmd.src_footprint_height = shape.HeightInTexels;
      cmd.src_footprint_depth = shape.DepthInTexels;
      cmd.src_footprint_row_pitch = row_pitch;
    } else {
      cmd.dst_resource = buffer;
      cmd.dst_type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      cmd.dst_offset = buffer_offset +
                       uint64_t(tile) *
                           D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
      cmd.dst_footprint_format = resource_desc.Format;
      cmd.dst_footprint_width = shape.WidthInTexels;
      cmd.dst_footprint_height = shape.HeightInTexels;
      cmd.dst_footprint_depth = shape.DepthInTexels;
      cmd.dst_footprint_row_pitch = row_pitch;
      cmd.src_resource = tiled_resource;
      cmd.src_type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      cmd.src_subresource = location.subresource;
      cmd.has_src_box = 1;
      cmd.src_box = {x * shape.WidthInTexels, y * shape.HeightInTexels,
                     z * shape.DepthInTexels, (x + 1) * shape.WidthInTexels,
                     (y + 1) * shape.HeightInTexels,
                     (z + 1) * shape.DepthInTexels};
    }
    Emit(cmd);
  }
  CLTRACE("CopyTiles %s tiles=%llu tile=%ux%ux%u buffer=%p+%llu",
          buffer_to_texture ? "buffer_to_texture" : "texture_to_buffer",
          (unsigned long long)region_tile_count, shape.WidthInTexels,
          shape.HeightInTexels, shape.DepthInTexels, (void *)buffer,
          (unsigned long long)buffer_offset);
  RetainResource(tiled_resource);
  RetainResource(buffer);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ResolveSubresource(
    ID3D12Resource *dst, UINT dst_sub, ID3D12Resource *src, UINT src_sub,
    DXGI_FORMAT format) {
  CmdResolveSubresource cmd = {};
  cmd.header = {CmdType::ResolveSubresource, sizeof(cmd)};
  cmd.dst = dst;
  cmd.dst_sub = dst_sub;
  cmd.src = src;
  cmd.src_sub = src_sub;
  cmd.format = format;
  cmd.mode = D3D12_RESOLVE_MODE_DECOMPRESS;
  RetainResource(dst);
  RetainResource(src);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::IASetPrimitiveTopology(
    D3D12_PRIMITIVE_TOPOLOGY topology) {
  CmdIASetPrimitiveTopology cmd = {};
  cmd.header = {CmdType::IASetPrimitiveTopology, sizeof(cmd)};
  cmd.topology = topology;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::RSSetViewports(
    UINT count, const D3D12_VIEWPORT *viewports) {
  size_t extra = count * sizeof(D3D12_VIEWPORT);
  auto total = sizeof(CmdRSSetViewports) - sizeof(D3D12_VIEWPORT) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdRSSetViewports cmd = {};
  cmd.header = {CmdType::RSSetViewports, (uint32_t)total};
  cmd.count = count;
  memcpy(m_cmds.data() + offset, &cmd, sizeof(CmdRSSetViewports) - sizeof(D3D12_VIEWPORT));
  memcpy(m_cmds.data() + offset + sizeof(CmdRSSetViewports) - sizeof(D3D12_VIEWPORT),
         viewports, extra);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::RSSetScissorRects(
    UINT count, const D3D12_RECT *rects) {
  size_t extra = count * sizeof(D3D12_RECT);
  auto total = sizeof(CmdRSSetScissorRects) - sizeof(D3D12_RECT) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdRSSetScissorRects cmd = {};
  cmd.header = {CmdType::RSSetScissorRects, (uint32_t)total};
  cmd.count = count;
  memcpy(m_cmds.data() + offset, &cmd, sizeof(CmdRSSetScissorRects) - sizeof(D3D12_RECT));
  memcpy(m_cmds.data() + offset + sizeof(CmdRSSetScissorRects) - sizeof(D3D12_RECT),
         rects, extra);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::OMSetBlendFactor(const FLOAT blend_factor[4]) {
  CmdOMBlendFactor cmd = {};
  cmd.header = {CmdType::OMSetBlendFactor, sizeof(cmd)};
  memcpy(cmd.factor, blend_factor, 16);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::OMSetStencilRef(UINT stencil_ref) {
  CmdOMStencilRef cmd = {};
  cmd.header = {CmdType::OMSetStencilRef, sizeof(cmd)};
  cmd.stencil_ref = stencil_ref;
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetPipelineState(
    ID3D12PipelineState *pipeline_state) {
  CmdSetPipelineState cmd = {};
  cmd.header = {CmdType::SetPipelineState, sizeof(cmd)};
  cmd.pso = pipeline_state;
  RetainPipelineState(pipeline_state);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ResourceBarrier(
    UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) {
  if (barrier_count && !barriers)
    return;
  for (UINT i = 0; i < barrier_count; i++) {
    switch (barriers[i].Type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
      RetainResource(barriers[i].Transition.pResource);
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
      RetainResource(barriers[i].Aliasing.pResourceBefore);
      RetainResource(barriers[i].Aliasing.pResourceAfter);
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
      RetainResource(barriers[i].UAV.pResource);
      break;
    default:
      break;
    }
  }
  size_t extra = barrier_count * sizeof(D3D12_RESOURCE_BARRIER);
  auto total = sizeof(CmdResourceBarrier) - sizeof(D3D12_RESOURCE_BARRIER) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdResourceBarrier cmd = {};
  cmd.header = {CmdType::ResourceBarrier, (uint32_t)total};
  cmd.count = barrier_count;
  memcpy(m_cmds.data() + offset, &cmd, sizeof(CmdResourceBarrier) - sizeof(D3D12_RESOURCE_BARRIER));
  memcpy(m_cmds.data() + offset + sizeof(CmdResourceBarrier) - sizeof(D3D12_RESOURCE_BARRIER),
         barriers, extra);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::Barrier(
    UINT32 num_barrier_groups,
    const D3D12_BARRIER_GROUP *barrier_groups) {
  if (!num_barrier_groups || !barrier_groups)
    return;
  CmdEnhancedBarrier cmd = {};
  cmd.header = {CmdType::EnhancedBarrier, sizeof(cmd)};
  cmd.group_count = num_barrier_groups;
  for (UINT32 i = 0; i < num_barrier_groups; i++) {
    const auto &group = barrier_groups[i];
    if (!group.NumBarriers || !group.pGlobalBarriers)
      continue;
    switch (group.Type) {
    case D3D12_BARRIER_TYPE_GLOBAL:
      cmd.global_barrier_count += group.NumBarriers;
      break;
    case D3D12_BARRIER_TYPE_BUFFER:
      cmd.buffer_barrier_count += group.NumBarriers;
      if (group.pBufferBarriers) {
        for (UINT32 barrier = 0; barrier < group.NumBarriers; barrier++)
          RetainResource(group.pBufferBarriers[barrier].pResource);
      }
      break;
    case D3D12_BARRIER_TYPE_TEXTURE:
      cmd.texture_barrier_count += group.NumBarriers;
      if (group.pTextureBarriers) {
        for (UINT32 barrier = 0; barrier < group.NumBarriers; barrier++)
          RetainResource(group.pTextureBarriers[barrier].pResource);
      }
      break;
    default:
      CLTRACE("Barrier ignored unknown group type=%u", (unsigned)group.Type);
      break;
    }
  }
  Emit(cmd);
  CLTRACE("Barrier groups=%u global=%u buffer=%u texture=%u",
          cmd.group_count, cmd.global_barrier_count, cmd.buffer_barrier_count,
          cmd.texture_barrier_count);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ExecuteBundle(
    ID3D12GraphicsCommandList *command_list) {
  CLTRACE("ExecuteBundle cmds=%zu", command_list ? static_cast<MTLD3D12GraphicsCommandList*>(command_list)->GetCommands().size() : 0);
  if (command_list) {
    auto *bundle = static_cast<MTLD3D12GraphicsCommandList*>(command_list);
    const auto &bundle_cmds = bundle->GetCommands();
    m_cmds.insert(m_cmds.end(), bundle_cmds.begin(), bundle_cmds.end());
    bundle->RetainReferencedObjectsInto(this);
  }
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetDescriptorHeaps(
    UINT heap_count, ID3D12DescriptorHeap *const *heaps) {
  if (heap_count && !heaps)
    return;
  for (UINT i = 0; i < heap_count; i++) {
    if (heaps[i]) {
      heaps[i]->AddRef();
      m_referenced_descriptor_heaps.push_back(heaps[i]);
      auto *dxmt_heap = static_cast<MTLD3D12DescriptorHeap *>(heaps[i]);
      if (dxmt_heap) {
        for (uint32_t descriptor_index = 0;
             descriptor_index < dxmt_heap->GetDescriptorCount();
             descriptor_index++) {
          const auto &descriptor = dxmt_heap->GetDescriptors()[descriptor_index];
          RetainResource(descriptor.resource);
          RetainResource(descriptor.resource_uav_counter);
          RetainResource(descriptor.sampler_feedback_target);
        }
      }
    }
  }
  size_t extra = heap_count * sizeof(ID3D12DescriptorHeap *);
  auto total = sizeof(CmdSetDescriptorHeaps) - sizeof(ID3D12DescriptorHeap *) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdSetDescriptorHeaps cmd = {};
  cmd.header = {CmdType::SetDescriptorHeaps, (uint32_t)total};
  cmd.count = heap_count;
  memcpy(m_cmds.data() + offset, &cmd, sizeof(CmdSetDescriptorHeaps) - sizeof(ID3D12DescriptorHeap *));
  memcpy(m_cmds.data() + offset + sizeof(CmdSetDescriptorHeaps) - sizeof(ID3D12DescriptorHeap *),
         heaps, extra);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetComputeRootSignature(
    ID3D12RootSignature *root_signature) {
  CmdSetRootSignature cmd = {};
  cmd.header = {CmdType::SetComputeRootSignature, sizeof(cmd)};
  cmd.root_sig = root_signature;
  RetainRootSignature(root_signature);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetGraphicsRootSignature(
    ID3D12RootSignature *root_signature) {
  CmdSetRootSignature cmd = {};
  cmd.header = {CmdType::SetGraphicsRootSignature, sizeof(cmd)};
  cmd.root_sig = root_signature;
  RetainRootSignature(root_signature);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetComputeRootDescriptorTable(
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  CmdSetRootDescriptorTable cmd = {};
  cmd.header = {CmdType::SetComputeRootDescriptorTable, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.base_descriptor = base_descriptor;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetGraphicsRootDescriptorTable(
    UINT root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  CmdSetRootDescriptorTable cmd = {};
  cmd.header = {CmdType::SetGraphicsRootDescriptorTable, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.base_descriptor = base_descriptor;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetComputeRoot32BitConstant(
    UINT root_parameter_index, UINT data, UINT dst_offset) {
  CmdSetRoot32BitConstants cmd = {};
  cmd.header = {CmdType::SetComputeRoot32BitConstants, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.count = 1;
  cmd.dst_offset = dst_offset;
  memcpy(cmd.data, &data, 4);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetGraphicsRoot32BitConstant(
    UINT root_parameter_index, UINT data, UINT dst_offset) {
  CmdSetRoot32BitConstants cmd = {};
  cmd.header = {CmdType::SetGraphicsRoot32BitConstants, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.count = 1;
  cmd.dst_offset = dst_offset;
  memcpy(cmd.data, &data, 4);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetComputeRoot32BitConstants(
    UINT root_parameter_index, UINT constant_count, const void *data,
    UINT dst_offset) {
  size_t extra = constant_count * 4;
  const size_t data_offset = offsetof(CmdSetRoot32BitConstants, data);
  auto total = data_offset + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdSetRoot32BitConstants cmd = {};
  cmd.header = {CmdType::SetComputeRoot32BitConstants, (uint32_t)total};
  cmd.root_param_index = root_parameter_index;
  cmd.count = constant_count;
  cmd.dst_offset = dst_offset;
  memcpy(m_cmds.data() + offset, &cmd, data_offset);
  memcpy(m_cmds.data() + offset + data_offset, data, extra);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetGraphicsRoot32BitConstants(
    UINT root_parameter_index, UINT constant_count, const void *data,
    UINT dst_offset) {
  size_t extra = constant_count * 4;
  const size_t data_offset = offsetof(CmdSetRoot32BitConstants, data);
  auto total = data_offset + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdSetRoot32BitConstants cmd = {};
  cmd.header = {CmdType::SetGraphicsRoot32BitConstants, (uint32_t)total};
  cmd.root_param_index = root_parameter_index;
  cmd.count = constant_count;
  cmd.dst_offset = dst_offset;
  memcpy(m_cmds.data() + offset, &cmd, data_offset);
  memcpy(m_cmds.data() + offset + data_offset, data, extra);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetComputeRootConstantBufferView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetComputeRootConstantBufferView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetGraphicsRootConstantBufferView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetGraphicsRootConstantBufferView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetComputeRootShaderResourceView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetComputeRootShaderResourceView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetGraphicsRootShaderResourceView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetGraphicsRootShaderResourceView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetComputeRootUnorderedAccessView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetComputeRootUnorderedAccessView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView(
    UINT root_parameter_index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  CmdSetRootCBV cmd = {};
  cmd.header = {CmdType::SetGraphicsRootUnorderedAccessView, sizeof(cmd)};
  cmd.root_param_index = root_parameter_index;
  cmd.address = address;
  RetainGPUAddress(address);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::IASetIndexBuffer(
    const D3D12_INDEX_BUFFER_VIEW *view) {
  CmdIASetIndexBuffer cmd = {};
  cmd.header = {CmdType::IASetIndexBuffer, sizeof(cmd)};
  if (view) {
    cmd.view = *view;
    RetainGPUAddress(view->BufferLocation);
  }
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::IASetVertexBuffers(
    UINT start_slot, UINT count,
    const D3D12_VERTEX_BUFFER_VIEW *views) {
  size_t extra = count * sizeof(D3D12_VERTEX_BUFFER_VIEW);
  auto total = sizeof(CmdIASetVertexBuffers) - sizeof(D3D12_VERTEX_BUFFER_VIEW) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  CmdIASetVertexBuffers cmd = {};
  cmd.header = {CmdType::IASetVertexBuffers, (uint32_t)total};
  cmd.start_slot = start_slot;
  cmd.count = count;
  memcpy(m_cmds.data() + offset, &cmd, sizeof(CmdIASetVertexBuffers) - sizeof(D3D12_VERTEX_BUFFER_VIEW));
  if (views) {
    memcpy(m_cmds.data() + offset + sizeof(CmdIASetVertexBuffers) - sizeof(D3D12_VERTEX_BUFFER_VIEW),
           views, extra);
    for (UINT i = 0; i < count; i++)
      RetainGPUAddress(views[i].BufferLocation);
  }
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SOSetTargets(
    UINT start_slot, UINT view_count,
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) {}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::OMSetRenderTargets(
    UINT rt_count, const D3D12_CPU_DESCRIPTOR_HANDLE *rts,
    WINBOOL single_handle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dsv) {
  CmdOMSetRenderTargets cmd = {};
  cmd.header = {CmdType::OMSetRenderTargets, sizeof(cmd)};
  cmd.rt_count = rt_count;
  cmd.single_handle = single_handle != 0;
  cmd.has_dsv = dsv != nullptr;
  if (rts) {
    for (UINT i = 0; i < rt_count && i < 8; i++)
      cmd.rts[i] = rts[i];
  }
  if (dsv)
    cmd.dsv = *dsv;
  for (UINT i = 0; rts && i < rt_count && i < 8; i++)
    RetainDescriptor(rts[i]);
  if (dsv)
    RetainDescriptor(*dsv);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ClearDepthStencilView(
    D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth,
    UINT8 stencil, UINT rect_count, const D3D12_RECT *rects) {
  CmdClearDSV cmd = {};
  cmd.header = {CmdType::ClearDepthStencilView, sizeof(cmd)};
  cmd.dsv = dsv;
  cmd.flags = flags;
  cmd.depth = depth;
  cmd.stencil = stencil;
  RetainDescriptor(dsv);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ClearRenderTargetView(
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4], UINT rect_count,
    const D3D12_RECT *rects) {
  CmdClearRTV cmd = {};
  cmd.header = {CmdType::ClearRenderTargetView, sizeof(cmd)};
  cmd.rtv = rtv;
  if (color)
    memcpy(cmd.color, color, 16);
  else
    TRACE("ClearRenderTargetView called with null color pointer");
  RetainDescriptor(rtv);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ClearUnorderedAccessViewUint(
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
    const UINT values[4], UINT rect_count, const D3D12_RECT *rects) {
  CmdClearUAV cmd = {};
  cmd.header = {CmdType::ClearUnorderedAccessView, sizeof(cmd)};
  cmd.gpu_handle = gpu_handle;
  cmd.cpu_handle = cpu_handle;
  cmd.resource = resource;
  if (values)
    memcpy(cmd.values, values, sizeof(cmd.values));
  RetainResource(resource);
  RetainDescriptor(cpu_handle);
  Emit(cmd);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::ClearUnorderedAccessViewFloat(
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, ID3D12Resource *resource,
    const float values[4], UINT rect_count, const D3D12_RECT *rects) {
  CmdClearUAV cmd = {};
  cmd.header = {CmdType::ClearUnorderedAccessView, sizeof(cmd)};
  cmd.gpu_handle = gpu_handle;
  cmd.cpu_handle = cpu_handle;
  cmd.resource = resource;
  cmd.is_float = 1;
  if (values)
    memcpy(cmd.values, values, sizeof(cmd.values));
  RetainResource(resource);
  RetainDescriptor(cpu_handle);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::DiscardResource(
    ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) {
  CmdDiscardResource cmd = {};
  cmd.header = {CmdType::DiscardResource, sizeof(cmd)};
  cmd.resource = resource;
  if (region) {
    cmd.first_subresource = region->FirstSubresource;
    cmd.num_subresources = region->NumSubresources;
  } else {
    cmd.first_subresource = 0;
    cmd.num_subresources = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }
  RetainResource(resource);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::BeginQuery(
    ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  CmdQuery cmd = {};
  cmd.header = {CmdType::BeginQuery, sizeof(cmd)};
  cmd.heap = heap;
  cmd.type = type;
  cmd.index = index;
  RetainQueryHeap(heap);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::EndQuery(
    ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) {
  CmdQuery cmd = {};
  cmd.header = {CmdType::EndQuery, sizeof(cmd)};
  cmd.heap = heap;
  cmd.type = type;
  cmd.index = index;
  RetainQueryHeap(heap);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ResolveQueryData(
    ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index,
    UINT query_count, ID3D12Resource *dst_buffer,
    UINT64 aligned_dst_buffer_offset) {
  CmdResolveQueryData cmd = {};
  cmd.header = {CmdType::ResolveQueryData, sizeof(cmd)};
  cmd.heap = heap;
  cmd.type = type;
  cmd.start_index = start_index;
  cmd.query_count = query_count;
  cmd.dst_buffer = dst_buffer;
  cmd.dst_offset = aligned_dst_buffer_offset;
  RetainQueryHeap(heap);
  RetainResource(dst_buffer);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetPredication(
    ID3D12Resource *buffer, UINT64 aligned_buffer_offset,
    D3D12_PREDICATION_OP operation) {
  CmdSetPredication cmd = {};
  cmd.header = {CmdType::SetPredication, sizeof(cmd)};
  cmd.buffer = buffer;
  cmd.aligned_buffer_offset = aligned_buffer_offset;
  cmd.operation = operation;
  RetainResource(buffer);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetMarker(
    UINT metadata, const void *data, UINT size) {
  if (size && !data)
    return;
  const size_t base_size = sizeof(CmdDebugEvent) - 1;
  if (size > UINT32_MAX - base_size)
    return;
  CmdDebugEvent cmd = {};
  cmd.header = {CmdType::SetMarker, static_cast<uint32_t>(base_size + size)};
  cmd.metadata = metadata;
  cmd.data_size = size;
  const uint8_t empty = 0;
  EmitVar(cmd, data ? data : &empty, size);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::BeginEvent(
    UINT metadata, const void *data, UINT size) {
  if (size && !data)
    return;
  const size_t base_size = sizeof(CmdDebugEvent) - 1;
  if (size > UINT32_MAX - base_size)
    return;
  CmdDebugEvent cmd = {};
  cmd.header = {CmdType::BeginEvent, static_cast<uint32_t>(base_size + size)};
  cmd.metadata = metadata;
  cmd.data_size = size;
  const uint8_t empty = 0;
  EmitVar(cmd, data ? data : &empty, size);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::EndEvent() {
  CmdHeader cmd = {CmdType::EndEvent, sizeof(CmdHeader)};
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ExecuteIndirect(
    ID3D12CommandSignature *command_signature, UINT max_command_count,
    ID3D12Resource *arg_buffer, UINT64 arg_buffer_offset,
    ID3D12Resource *count_buffer, UINT64 count_buffer_offset) {
  CLTRACE("ExecuteIndirect max=%u", max_command_count);
  CmdExecuteIndirect cmd = {};
  cmd.header = {CmdType::ExecuteIndirect, sizeof(cmd)};
  cmd.signature = command_signature;
  cmd.max_command_count = max_command_count;
  cmd.argument_buffer = arg_buffer;
  cmd.argument_buffer_offset = arg_buffer_offset;
  cmd.count_buffer = count_buffer;
  cmd.count_buffer_offset = count_buffer_offset;
  RetainCommandSignature(command_signature);
  RetainResource(arg_buffer);
  RetainResource(count_buffer);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::AtomicCopyBufferUINT(
    ID3D12Resource *dst_buffer, UINT64 dst_offset,
    ID3D12Resource *src_buffer, UINT64 src_offset,
    UINT dependent_resource_count,
    ID3D12Resource *const *dependent_resources,
    const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) {
  (void)dependent_sub_resource_ranges;
  CmdCopyBufferRegion cmd = {};
  cmd.header = {CmdType::CopyBufferRegion, sizeof(cmd)};
  cmd.dst = dst_buffer;
  cmd.dst_offset = dst_offset;
  cmd.src = src_buffer;
  cmd.src_offset = src_offset;
  cmd.byte_count = sizeof(UINT);
  RetainResource(dst_buffer);
  RetainResource(src_buffer);
  for (UINT i = 0; i < dependent_resource_count; i++)
    RetainResource(dependent_resources ? dependent_resources[i] : nullptr);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::AtomicCopyBufferUINT64(
    ID3D12Resource *dst_buffer, UINT64 dst_offset,
    ID3D12Resource *src_buffer, UINT64 src_offset,
    UINT dependent_resource_count,
    ID3D12Resource *const *dependent_resources,
    const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) {
  (void)dependent_sub_resource_ranges;
  CmdCopyBufferRegion cmd = {};
  cmd.header = {CmdType::CopyBufferRegion, sizeof(cmd)};
  cmd.dst = dst_buffer;
  cmd.dst_offset = dst_offset;
  cmd.src = src_buffer;
  cmd.src_offset = src_offset;
  cmd.byte_count = sizeof(UINT64);
  RetainResource(dst_buffer);
  RetainResource(src_buffer);
  for (UINT i = 0; i < dependent_resource_count; i++)
    RetainResource(dependent_resources ? dependent_resources[i] : nullptr);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::OMSetDepthBounds(
    FLOAT min, FLOAT max) {
  CmdOMSetDepthBounds cmd = {};
  cmd.header = {CmdType::OMSetDepthBounds, sizeof(cmd)};
  cmd.min_depth = min;
  cmd.max_depth = max;
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetSamplePositions(
    UINT sample_count, UINT pixel_count,
    D3D12_SAMPLE_POSITION *sample_positions) {
  const bool valid_sample_count = sample_count == 1 || sample_count == 2 ||
                                  sample_count == 4 || sample_count == 8 ||
                                  sample_count == 16 || sample_count == 32;
  const bool reset = sample_count == 0 && pixel_count == 0 && !sample_positions;
  if ((!reset && (!valid_sample_count || pixel_count != 1 || !sample_positions)) ||
      sample_count > 32 || pixel_count > UINT32_MAX / std::max(sample_count, 1u))
    return;
  const uint64_t position_count = reset ? 0 : uint64_t(sample_count) * pixel_count;
  const size_t base_size = offsetof(CmdSetSamplePositions, positions);
  if (position_count > (UINT32_MAX - base_size) / sizeof(D3D12_SAMPLE_POSITION))
    return;
  const size_t total_size = base_size +
                            static_cast<size_t>(position_count) * sizeof(D3D12_SAMPLE_POSITION);
  const size_t offset = m_cmds.size();
  m_cmds.resize(offset + total_size);
  CmdSetSamplePositions cmd = {};
  cmd.header = {CmdType::SetSamplePositions, static_cast<uint32_t>(total_size)};
  cmd.sample_count = sample_count;
  cmd.pixel_count = pixel_count;
  cmd.position_count = static_cast<uint32_t>(position_count);
  memcpy(m_cmds.data() + offset, &cmd, base_size);
  if (position_count)
    memcpy(m_cmds.data() + offset + base_size, sample_positions,
           static_cast<size_t>(position_count) * sizeof(D3D12_SAMPLE_POSITION));
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ResolveSubresourceRegion(
    ID3D12Resource *dst_resource, UINT dst_sub_resource_idx,
    UINT dst_x, UINT dst_y,
    ID3D12Resource *src_resource, UINT src_sub_resource_idx,
    D3D12_RECT *src_rect, DXGI_FORMAT format,
    D3D12_RESOLVE_MODE mode) {
  CmdResolveSubresource cmd = {};
  cmd.header = {CmdType::ResolveSubresource, sizeof(cmd)};
  cmd.dst = dst_resource;
  cmd.dst_sub = dst_sub_resource_idx;
  cmd.dst_x = dst_x;
  cmd.dst_y = dst_y;
  cmd.src = src_resource;
  cmd.src_sub = src_sub_resource_idx;
  cmd.format = format;
  cmd.mode = mode;
  if (src_rect) {
    cmd.has_src_rect = 1;
    cmd.src_rect = *src_rect;
  }
  RetainResource(dst_resource);
  RetainResource(src_resource);
  Emit(cmd);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetViewInstanceMask(
    UINT mask) {}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::WriteBufferImmediate(
    UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) {
  if (!count || !parameters)
    return;
  std::vector<CmdWriteBufferImmediateEntry> entries(count);
  for (UINT i = 0; i < count; i++) {
    entries[i].parameter = parameters[i];
    entries[i].mode = modes ? modes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
  }
  CmdWriteBufferImmediate cmd = {};
  size_t extra = count * sizeof(CmdWriteBufferImmediateEntry);
  auto total = sizeof(CmdWriteBufferImmediate) -
               sizeof(CmdWriteBufferImmediateEntry) + extra;
  auto offset = m_cmds.size();
  m_cmds.resize(offset + total);
  cmd.header = {CmdType::WriteBufferImmediate, (uint32_t)total};
  cmd.count = count;
  memcpy(m_cmds.data() + offset, &cmd,
         sizeof(CmdWriteBufferImmediate) -
             sizeof(CmdWriteBufferImmediateEntry));
  memcpy(m_cmds.data() + offset + sizeof(CmdWriteBufferImmediate) -
             sizeof(CmdWriteBufferImmediateEntry),
         entries.data(), extra);
}

/*** ID3D12GraphicsCommandList3 ***/
void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetProtectedResourceSession(
    ID3D12ProtectedResourceSession *protected_session) {
  CLTRACE("SetProtectedResourceSession -> noop");
}

/*** ID3D12GraphicsCommandList4 ***/
void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::BeginRenderPass(
    UINT num_render_targets,
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
    D3D12_RENDER_PASS_FLAGS flags) {
  CLTRACE("BeginRenderPass numRT=%u flags=0x%x", num_render_targets, (unsigned)flags);

  if (render_targets && num_render_targets > 0) {
    D3D12_CPU_DESCRIPTOR_HANDLE rt_handles[8];
    for (UINT i = 0; i < num_render_targets && i < 8; i++) {
      rt_handles[i] = render_targets[i].cpuDescriptor;
    }
    OMSetRenderTargets(num_render_targets, rt_handles, FALSE,
                       depth_stencil ? &depth_stencil->cpuDescriptor : nullptr);

    for (UINT i = 0; i < num_render_targets && i < 8; i++) {
      if (render_targets[i].BeginningAccess.Type ==
          D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
        ClearRenderTargetView(render_targets[i].cpuDescriptor,
                              render_targets[i].BeginningAccess.Clear
                                  .ClearValue.Color,
                              0, nullptr);
      }
    }
  }

  if (depth_stencil) {
    D3D12_CLEAR_FLAGS clear_flags = (D3D12_CLEAR_FLAGS)0;
    FLOAT depth = 1.0f;
    UINT8 stencil = 0;
    if (depth_stencil->DepthBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear_flags =
          (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_DEPTH);
      depth = depth_stencil->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
    }
    if (depth_stencil->StencilBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear_flags =
          (D3D12_CLEAR_FLAGS)(clear_flags | D3D12_CLEAR_FLAG_STENCIL);
      stencil =
          depth_stencil->StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil;
    }
    if (clear_flags)
      ClearDepthStencilView(depth_stencil->cpuDescriptor, clear_flags, depth,
                            stencil, 0, nullptr);
  }
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::EndRenderPass() {
  CLTRACE("EndRenderPass");
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::InitializeMetaCommand(
    ID3D12MetaCommand *meta_command, const void *initialization_parameters_data,
    SIZE_T initialization_parameters_data_size_in_bytes) {}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::ExecuteMetaCommand(
    ID3D12MetaCommand *meta_command, const void *execution_parameters_data,
    SIZE_T execution_parameters_data_size_in_bytes) {}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::BuildRaytracingAccelerationStructure(
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
    UINT num_post_build_info_descs,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *post_build_info_descs) {
  if (!desc) {
    CLTRACE("BuildRaytracingAccelerationStructure ignored null descriptor");
    return;
  }
  if (desc->Inputs.Type ==
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
    if (!desc->Inputs.NumDescs ||
        desc->Inputs.NumDescs >
            CmdBuildRaytracingAccelerationStructure::kMaxGeometryDescs ||
        (desc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY &&
         !desc->Inputs.pGeometryDescs) ||
        (desc->Inputs.DescsLayout ==
             D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS &&
         !desc->Inputs.ppGeometryDescs)) {
      CLTRACE("BuildRaytracingAccelerationStructure unsupported BLAS "
              "descriptors=%u layout=%u",
              desc->Inputs.NumDescs, (unsigned)desc->Inputs.DescsLayout);
      return;
    }
  } else if (desc->Inputs.Type !=
                 D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL ||
             !desc->Inputs.NumDescs || !desc->Inputs.InstanceDescs) {
    CLTRACE("BuildRaytracingAccelerationStructure unsupported TLAS inputs");
    return;
  }

  CmdBuildRaytracingAccelerationStructure cmd = {};
  cmd.header = {CmdType::BuildRaytracingAccelerationStructure, sizeof(cmd)};
  cmd.dest_acceleration_structure = desc->DestAccelerationStructureData;
  cmd.scratch_acceleration_structure = desc->ScratchAccelerationStructureData;
  cmd.source_acceleration_structure = desc->SourceAccelerationStructureData;
  cmd.type = desc->Inputs.Type;
  cmd.flags = desc->Inputs.Flags;
  cmd.descs_layout = desc->Inputs.DescsLayout;
  cmd.num_descs = desc->Inputs.NumDescs;
  cmd.instance_descs = desc->Inputs.InstanceDescs;
  if (desc->Inputs.Type ==
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
    for (UINT i = 0; i < desc->Inputs.NumDescs; i++) {
      const auto *geometry =
          desc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY
              ? &desc->Inputs.pGeometryDescs[i]
              : desc->Inputs.ppGeometryDescs[i];
      if (!geometry) {
        CLTRACE("BuildRaytracingAccelerationStructure null geometry=%u", i);
        return;
      }
      cmd.geometries[i] = *geometry;
    }
  }
  RetainGPUAddress(cmd.dest_acceleration_structure);
  RetainGPUAddress(cmd.scratch_acceleration_structure);
  RetainGPUAddress(cmd.source_acceleration_structure);
  RetainGPUAddress(cmd.instance_descs);
  for (UINT i = 0; i < cmd.num_descs &&
                   cmd.type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
       i++) {
    const auto &geometry = cmd.geometries[i];
    if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
      RetainGPUAddress(geometry.Triangles.VertexBuffer.StartAddress);
      RetainGPUAddress(geometry.Triangles.IndexBuffer);
    } else if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
      RetainGPUAddress(geometry.AABBs.AABBs.StartAddress);
    }
  }
  Emit(cmd);
  for (UINT i = 0; post_build_info_descs &&
                   i < num_post_build_info_descs;
       i++) {
    EmitRaytracingAccelerationStructurePostbuildInfo(
        &post_build_info_descs[i], 1,
        &desc->DestAccelerationStructureData);
  }
  CLTRACE("BuildRaytracingAccelerationStructure type=%u dest=0x%llx "
          "scratch=0x%llx postbuild=%u",
          (unsigned)cmd.type,
          (unsigned long long)cmd.dest_acceleration_structure,
          (unsigned long long)cmd.scratch_acceleration_structure,
          num_post_build_info_descs);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::EmitRaytracingAccelerationStructurePostbuildInfo(
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *descs,
    UINT num_acceleration_structures,
    const D3D12_GPU_VIRTUAL_ADDRESS *source_acceleration_structure_data) {
  if (!descs || !source_acceleration_structure_data ||
      num_acceleration_structures != 1) {
    CLTRACE("EmitRaytracingPostbuildInfo unsupported count=%u",
            num_acceleration_structures);
    return;
  }
  CmdEmitRaytracingAccelerationStructurePostbuildInfo cmd = {};
  cmd.header = {CmdType::EmitRaytracingAccelerationStructurePostbuildInfo,
                sizeof(cmd)};
  cmd.info_type = descs->InfoType;
  cmd.dest_buffer = descs->DestBuffer;
  cmd.source_acceleration_structure = source_acceleration_structure_data[0];
  RetainGPUAddress(cmd.dest_buffer);
  RetainGPUAddress(cmd.source_acceleration_structure);
  Emit(cmd);
  CLTRACE("EmitRaytracingPostbuildInfo type=%u dest=0x%llx source=0x%llx",
          (unsigned)cmd.info_type, (unsigned long long)cmd.dest_buffer,
          (unsigned long long)cmd.source_acceleration_structure);
}

void STDMETHODCALLTYPE
MTLD3D12GraphicsCommandList::CopyRaytracingAccelerationStructure(
    D3D12_GPU_VIRTUAL_ADDRESS dest_acceleration_structure_data,
    D3D12_GPU_VIRTUAL_ADDRESS source_acceleration_structure_data,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) {
  if (mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE &&
      mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT &&
      mode != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE &&
      mode !=
          D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE) {
    CLTRACE("CopyRaytracingAccelerationStructure unsupported mode=%u",
            (unsigned)mode);
    return;
  }
  CmdCopyRaytracingAccelerationStructure cmd = {};
  cmd.header = {CmdType::CopyRaytracingAccelerationStructure, sizeof(cmd)};
  cmd.destination_acceleration_structure = dest_acceleration_structure_data;
  cmd.source_acceleration_structure = source_acceleration_structure_data;
  cmd.mode = mode;
  RetainGPUAddress(dest_acceleration_structure_data);
  RetainGPUAddress(source_acceleration_structure_data);
  Emit(cmd);
  CLTRACE("CopyRaytracingAccelerationStructure mode=%u source=0x%llx "
          "destination=0x%llx",
          (unsigned)mode,
          (unsigned long long)source_acceleration_structure_data,
          (unsigned long long)dest_acceleration_structure_data);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::SetPipelineState1(
    ID3D12StateObject *state_object) {
  CmdSetPipelineState1 cmd = {};
  cmd.header = {CmdType::SetPipelineState1, sizeof(cmd)};
  cmd.state_object = state_object;
  RetainStateObject(state_object);
  Emit(cmd);
  CLTRACE("SetPipelineState1 state=%p", (void *)state_object);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::DispatchRays(
    const D3D12_DISPATCH_RAYS_DESC *desc) {
  if (!desc) {
    CLTRACE("DispatchRays ignored null descriptor");
    return;
  }
  CmdDispatchRays cmd = {};
  cmd.header = {CmdType::DispatchRays, sizeof(cmd)};
  cmd.desc = *desc;
  RetainGPUAddress(desc->RayGenerationShaderRecord.StartAddress);
  RetainGPUAddress(desc->MissShaderTable.StartAddress);
  RetainGPUAddress(desc->HitGroupTable.StartAddress);
  RetainGPUAddress(desc->CallableShaderTable.StartAddress);
  Emit(cmd);
  CLTRACE("DispatchRays dimensions=%ux%ux%u raygen=0x%llx",
          desc->Width, desc->Height, desc->Depth,
          (unsigned long long)desc->RayGenerationShaderRecord.StartAddress);
}

/*** ID3D12GraphicsCommandList5 ***/
void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::RSSetShadingRate(
    D3D12_SHADING_RATE base_shading_rate,
    const D3D12_SHADING_RATE_COMBINER *combiners) {
  CmdRSSetShadingRate cmd = {};
  cmd.header = {CmdType::RSSetShadingRate, sizeof(cmd)};
  cmd.base_shading_rate = base_shading_rate;
  for (UINT i = 0; i < 2; ++i)
    cmd.combiners[i] = combiners ? combiners[i]
                                  : D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
  Emit(cmd);
  CLTRACE("RSSetShadingRate rate=%u combiners=%u,%u",
          (unsigned)base_shading_rate, (unsigned)cmd.combiners[0],
          (unsigned)cmd.combiners[1]);
}

void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::RSSetShadingRateImage(
    ID3D12Resource *shading_rate_image) {
  CmdRSSetShadingRateImage cmd = {};
  cmd.header = {CmdType::RSSetShadingRateImage, sizeof(cmd)};
  cmd.shading_rate_image = shading_rate_image;
  RetainResource(shading_rate_image);
  Emit(cmd);
  CLTRACE("RSSetShadingRateImage resource=%p", (void *)shading_rate_image);
}

/*** ID3D12GraphicsCommandList6 ***/
void STDMETHODCALLTYPE MTLD3D12GraphicsCommandList::DispatchMesh(
    UINT thread_group_count_x, UINT thread_group_count_y,
    UINT thread_group_count_z) {
  CLTRACE("DispatchMesh %ux%ux%u", thread_group_count_x,
          thread_group_count_y, thread_group_count_z);
  CmdDispatchMesh cmd = {};
  cmd.header = {CmdType::DispatchMesh, sizeof(cmd)};
  cmd.x = thread_group_count_x;
  cmd.y = thread_group_count_y;
  cmd.z = thread_group_count_z;
  Emit(cmd);
}

} // namespace dxmt
