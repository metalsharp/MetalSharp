#include "d3d12_shared_resource.hpp"

#include "d3d12_device.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_trace.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#define SHARETRACE(fmt, ...) DXMTD3D12Trace("SharedResource", fmt, ##__VA_ARGS__)

namespace dxmt {

namespace {

static HRESULT HResultFromLastError() {
  const DWORD error = GetLastError();
  return HRESULT_FROM_WIN32(error ? error : ERROR_FUNCTION_FAILED);
}

static bool ValidMetadata(const D3D12SharedResourceMetadata &metadata,
                          SIZE_T mapped_size) {
  if (metadata.magic != kD3D12SharedResourceMagic ||
      metadata.version != kD3D12SharedResourceVersion ||
      metadata.kind != kD3D12SharedResourceKindBuffer ||
      metadata.data_offset < sizeof(D3D12SharedResourceMetadata) ||
      metadata.mapping_size < metadata.data_offset ||
      metadata.data_size > metadata.mapping_size - metadata.data_offset)
    return false;
  if (mapped_size && metadata.mapping_size > mapped_size)
    return false;
  const auto &desc = metadata.resource_desc;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
         desc.Width == metadata.data_size && metadata.data_size != 0;
}

static HRESULT DuplicateMappingHandle(HANDLE source, HANDLE *duplicate) {
  if (!duplicate)
    return E_POINTER;
  *duplicate = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                       duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return HResultFromLastError();
  return S_OK;
}

} // namespace

HRESULT MTLD3D12Resource::AttachSharedBacking(
    HANDLE mapping, void *mapping_view, uint64_t mapping_size,
    uint64_t data_offset, bool preserve_contents) {
  if (!mapping || !mapping_view || !IsBuffer() || !m_mtl_buffer.handle ||
      !mapping_size || data_offset >= mapping_size ||
      m_desc.Width > mapping_size - data_offset)
    return E_INVALIDARG;

  const uint64_t data_size = m_desc.Width;
  std::vector<uint8_t> original;
  if (preserve_contents) {
    try {
      original.resize(static_cast<size_t>(data_size));
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    if (m_cpu_addr) {
      std::memcpy(original.data(), m_cpu_addr, original.size());
    } else if (data_size > std::numeric_limits<UINT>::max() ||
               FAILED(ReadFromSubresource(original.data(),
                                           static_cast<UINT>(data_size),
                                           static_cast<UINT>(data_size), 0,
                                           nullptr))) {
      return E_FAIL;
    }
  }

  auto *shared_data = static_cast<uint8_t *>(mapping_view) + data_offset;
  WMTBufferInfo buffer_info = {};
  buffer_info.length = data_size;
  buffer_info.options = WMTResourceStorageModeShared;
  buffer_info.memory.set(shared_data);
  auto shared_buffer = m_device->GetDXMTDevice().device().newBuffer(buffer_info);
  if (!shared_buffer.handle ||
      !buffer_info.memory.get_accessible_or_null())
    return E_FAIL;
  if (preserve_contents && !original.empty())
    std::memcpy(shared_data, original.data(), original.size());

  m_device->UnregisterResource(this);
  if (m_shared_mapping_view)
    UnmapViewOfFile(m_shared_mapping_view);
  if (m_shared_mapping)
    CloseHandle(m_shared_mapping);
  m_mtl_buffer = std::move(shared_buffer);
  m_buf_info = buffer_info;
  m_cpu_addr = shared_data;
  m_gpu_addr = buffer_info.gpu_address;
  m_shared_mapping = mapping;
  m_shared_mapping_view = mapping_view;
  m_shared_mapping_size = mapping_size;
  m_shared_data_offset = data_offset;
  m_device->RegisterResource(this);
  SHARETRACE("attached shared buffer resource=%p mapping=%p size=%llu gpu=0x%llx",
             (void *)this, mapping, (unsigned long long)data_size,
             (unsigned long long)m_gpu_addr);
  return S_OK;
}

HRESULT CreateSharedBufferMapping(
    MTLD3D12Resource *resource, const SECURITY_ATTRIBUTES *attributes,
    const WCHAR *name, HANDLE *mapping) {
  if (!resource || !name || !name[0] || !mapping)
    return E_INVALIDARG;
  *mapping = nullptr;
  if (!resource->IsBuffer() || !resource->IsValid())
    return E_INVALIDARG;

  D3D12_RESOURCE_DESC desc = {};
  resource->GetDesc(&desc);
  if (!desc.Width || desc.Width > UINT64_MAX - kD3D12SharedResourceDataOffset)
    return E_INVALIDARG;
  const uint64_t mapping_size =
      kD3D12SharedResourceDataOffset + desc.Width;
  if (mapping_size > ((uint64_t(std::numeric_limits<DWORD>::max()) << 32) |
                      std::numeric_limits<DWORD>::max()))
    return E_OUTOFMEMORY;

  HANDLE section = CreateFileMappingW(
      INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(attributes),
      PAGE_READWRITE,
      static_cast<DWORD>(mapping_size >> 32),
      static_cast<DWORD>(mapping_size & std::numeric_limits<DWORD>::max()),
      name);
  if (!section)
    return HResultFromLastError();
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return DXGI_ERROR_NAME_ALREADY_EXISTS;
  }

  void *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view) {
    HRESULT hr = HResultFromLastError();
    CloseHandle(section);
    return hr;
  }

  D3D12SharedResourceMetadata metadata = {};
  metadata.mapping_size = mapping_size;
  metadata.data_size = desc.Width;
  metadata.resource_desc = desc;
  resource->GetHeapProperties(&metadata.heap_properties,
                              &metadata.heap_flags);
  metadata.initial_state = resource->GetTrackedState();
  std::memcpy(view, &metadata, sizeof(metadata));

  HANDLE owner_mapping = nullptr;
  HRESULT hr = DuplicateMappingHandle(section, &owner_mapping);
  if (SUCCEEDED(hr))
    hr = resource->AttachSharedBacking(owner_mapping, view, mapping_size,
                                       metadata.data_offset, true);
  if (FAILED(hr)) {
    if (owner_mapping)
      CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    CloseHandle(section);
    return hr;
  }

  *mapping = section;
  SHARETRACE("created named shared buffer resource=%p name=%ls mapping=%p size=%llu",
             (void *)resource, name, section,
             (unsigned long long)desc.Width);
  return S_OK;
}

HRESULT OpenSharedBufferFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                    ID3D12Resource **resource) {
  if (!device || !mapping || !resource)
    return E_INVALIDARG;
  *resource = nullptr;
  void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view)
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view)
    return HResultFromLastError();
  D3D12SharedResourceMetadata metadata = {};
  std::memcpy(&metadata, view, sizeof(metadata));
  if (!ValidMetadata(metadata, 0)) {
    UnmapViewOfFile(view);
    return DXGI_ERROR_INVALID_CALL;
  }

  HANDLE owner_mapping = nullptr;
  HRESULT hr = DuplicateMappingHandle(mapping, &owner_mapping);
  if (FAILED(hr)) {
    UnmapViewOfFile(view);
    return hr;
  }
  auto wmt_device = device->GetMTLDevice();
  WMTBufferInfo buffer_info = {};
  buffer_info.length = metadata.data_size;
  buffer_info.options = WMTResourceStorageModeShared;
  buffer_info.memory.set(static_cast<uint8_t *>(view) + metadata.data_offset);
  auto backing = wmt_device.newBuffer(buffer_info);
  if (!backing.handle || !buffer_info.memory.get_accessible_or_null()) {
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return E_FAIL;
  }

  MTLD3D12Resource *created = nullptr;
  try {
    created = new MTLD3D12Resource(
        device, metadata.resource_desc, metadata.initial_state,
        metadata.heap_properties, metadata.heap_flags, std::move(backing),
        static_cast<uint8_t *>(view) + metadata.data_offset,
        buffer_info.gpu_address, 0);
  } catch (const std::bad_alloc &) {
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return E_OUTOFMEMORY;
  }
  if (!created->IsValid()) {
    created->Release();
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return E_FAIL;
  }
  created->AdoptSharedMapping(owner_mapping, view, metadata.mapping_size,
                              metadata.data_offset);
  hr = created->QueryInterface(IID_ID3D12Resource,
                               reinterpret_cast<void **>(resource));
  created->Release();
  if (FAILED(hr)) {
    *resource = nullptr;
    return hr;
  }
  SHARETRACE("opened shared buffer mapping=%p resource=%p size=%llu",
             mapping, (void *)*resource,
             (unsigned long long)metadata.data_size);
  return S_OK;
}

HRESULT CreateSharedFenceMapping(
    MTLD3D12Fence *fence, const SECURITY_ATTRIBUTES *attributes,
    const WCHAR *name, HANDLE *mapping) {
  if (!fence || !name || !name[0] || !mapping)
    return E_INVALIDARG;
  *mapping = nullptr;
  constexpr uint64_t mapping_size = 4096;
  HANDLE section = CreateFileMappingW(
      INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(attributes),
      PAGE_READWRITE, 0,
                                      static_cast<DWORD>(mapping_size), name);
  if (!section)
    return HResultFromLastError();
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return DXGI_ERROR_NAME_ALREADY_EXISTS;
  }
  void *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view) {
    HRESULT hr = HResultFromLastError();
    CloseHandle(section);
    return hr;
  }
  D3D12SharedFenceMetadata metadata = {};
  metadata.mapping_size = mapping_size;
  metadata.initial_value = fence->GetCompletedValue();
  metadata.flags = fence->GetFlags();
  std::memcpy(view, &metadata, sizeof(metadata));
  auto *shared_value = reinterpret_cast<volatile LONG64 *>(
      static_cast<uint8_t *>(view) + metadata.value_offset);
  InterlockedExchange64(shared_value, static_cast<LONG64>(metadata.initial_value));
  HANDLE owner_mapping = nullptr;
  HRESULT hr = DuplicateMappingHandle(section, &owner_mapping);
  if (SUCCEEDED(hr))
    fence->AdoptSharedMapping(owner_mapping, view, mapping_size,
                              metadata.value_offset);
  if (FAILED(hr)) {
    if (owner_mapping)
      CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    CloseHandle(section);
    return hr;
  }
  *mapping = section;
  return S_OK;
}

HRESULT OpenSharedFenceFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                   ID3D12Fence **fence) {
  if (!device || !mapping || !fence)
    return E_INVALIDARG;
  *fence = nullptr;
  void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view)
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view)
    return HResultFromLastError();
  D3D12SharedFenceMetadata metadata = {};
  std::memcpy(&metadata, view, sizeof(metadata));
  if (metadata.magic != kD3D12SharedResourceMagic ||
      metadata.version != kD3D12SharedResourceVersion ||
      metadata.kind != kD3D12SharedResourceKindFence ||
      metadata.mapping_size < metadata.value_offset + sizeof(LONG64) ||
      metadata.value_offset < sizeof(metadata) ||
      metadata.value_offset % alignof(LONG64) != 0) {
    UnmapViewOfFile(view);
    return DXGI_ERROR_INVALID_CALL;
  }
  auto *shared_value = reinterpret_cast<volatile LONG64 *>(
      static_cast<uint8_t *>(view) + metadata.value_offset);
  const uint64_t initial_value = static_cast<uint64_t>(
      InterlockedCompareExchange64(shared_value, 0, 0));
  HANDLE owner_mapping = nullptr;
  HRESULT hr = DuplicateMappingHandle(mapping, &owner_mapping);
  if (FAILED(hr)) {
    UnmapViewOfFile(view);
    return hr;
  }
  auto *created = new (std::nothrow)
      MTLD3D12Fence(device, initial_value, metadata.flags);
  if (!created) {
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return E_OUTOFMEMORY;
  }
  created->AdoptSharedMapping(owner_mapping, view, metadata.mapping_size,
                              metadata.value_offset);
  hr = created->QueryInterface(IID_ID3D12Fence,
                               reinterpret_cast<void **>(fence));
  created->Release();
  if (FAILED(hr)) {
    *fence = nullptr;
    return hr;
  }
  return S_OK;
}

} // namespace dxmt
