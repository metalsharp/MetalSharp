#include "d3d12_heap.hpp"
#include "d3d12_device.hpp"
#include "d3d12_shared_resource.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

#define HTRACE(fmt, ...) DXMTD3D12Trace("Heap", fmt, ##__VA_ARGS__)

namespace dxmt {

namespace {
std::mutex g_heap_registry_mutex;
std::vector<MTLD3D12Heap *> g_heap_registry;
} // namespace

MTLD3D12Heap::MTLD3D12Heap(MTLD3D12Device *device, const D3D12_HEAP_DESC &desc)
    : m_device(device), m_desc(desc),
      m_residency((desc.Flags & D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT) == 0) {
  m_device->AddRef();
  HTRACE("ctor: size=%llu alignment=%llu type=%u flags=0x%x",
    (unsigned long long)desc.SizeInBytes, (unsigned long long)desc.Alignment,
    desc.Properties.Type, desc.Flags);

  const bool buffers_only =
      (desc.Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
          D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS ||
      (desc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) == 0;
  if (buffers_only && desc.SizeInBytes) {
    auto wmt_device = m_device->GetDXMTDevice().device();
    const UINT heap_type = static_cast<UINT>(desc.Properties.Type);
    bool cpu_accessible =
        heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_UPLOAD) ||
        heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_READBACK) ||
        heap_type == 5 ||
        (heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_CUSTOM) &&
         (desc.Properties.CPUPageProperty ==
              D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
          desc.Properties.CPUPageProperty ==
              D3D12_CPU_PAGE_PROPERTY_WRITE_BACK));
    m_buf_info.length = desc.SizeInBytes;
    m_buf_info.options =
        cpu_accessible ? WMTResourceStorageModeShared
                       : WMTResourceStorageModePrivate;
    m_buffer = wmt_device.newBuffer(m_buf_info);
    m_cpu_addr = m_buf_info.memory.get_accessible_or_null();
    m_gpu_addr = m_buf_info.gpu_address;
    HTRACE("ctor: backing buffer handle=%llu gpu=0x%llx cpu=%p opts=%u",
           (unsigned long long)m_buffer.handle, (unsigned long long)m_gpu_addr,
           m_cpu_addr, (unsigned)m_buf_info.options);
  }
  if (m_cpu_addr) {
    std::lock_guard lock(g_heap_registry_mutex);
    g_heap_registry.push_back(this);
  }
}

MTLD3D12Heap::~MTLD3D12Heap() {
  HTRACE("dtor");
  {
    std::lock_guard lock(g_heap_registry_mutex);
    auto it = std::remove(g_heap_registry.begin(), g_heap_registry.end(), this);
    g_heap_registry.erase(it, g_heap_registry.end());
  }
  if (m_shared_mapping_view)
    UnmapViewOfFile(m_shared_mapping_view);
  if (m_shared_mapping)
    CloseHandle(m_shared_mapping);
  m_shared_mapping_view = nullptr;
  m_shared_mapping = nullptr;
  m_heap = nullptr;
  m_buffer = nullptr;
  m_device->Release();
}

bool MTLD3D12Heap::ContainsAddress(const void *address) const {
  if (!address || !m_cpu_addr || !m_desc.SizeInBytes)
    return false;
  const auto base = reinterpret_cast<uintptr_t>(m_cpu_addr);
  const auto value = reinterpret_cast<uintptr_t>(address);
  return value >= base && value - base < m_desc.SizeInBytes;
}

MTLD3D12Heap *FindHeapContainingAddress(const void *address,
                                         const MTLD3D12Device *device) {
  std::lock_guard lock(g_heap_registry_mutex);
  for (auto *heap : g_heap_registry) {
    if ((!device || heap->IsOwnedBy(device)) &&
        heap->ContainsAddress(address)) {
      heap->AddRef();
      return heap;
    }
  }
  return nullptr;
}

HRESULT MTLD3D12Heap::AttachSharedBacking(
    HANDLE mapping, void *mapping_view, uint64_t mapping_size,
    uint64_t data_offset, bool preserve_contents, bool writable) {
  if (!mapping || !mapping_view || !m_desc.SizeInBytes ||
      data_offset >= mapping_size ||
      m_desc.SizeInBytes > mapping_size - data_offset)
    return E_INVALIDARG;
  std::vector<uint8_t> original;
  if (preserve_contents && m_cpu_addr) {
    try {
      original.resize(static_cast<size_t>(m_desc.SizeInBytes));
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
    std::memcpy(original.data(), m_cpu_addr, original.size());
  }
  WMTBufferInfo buffer_info = {};
  buffer_info.length = m_desc.SizeInBytes;
  buffer_info.options = WMTResourceStorageModeShared;
  buffer_info.memory.set(static_cast<uint8_t *>(mapping_view) + data_offset);
  auto shared_buffer = m_device->GetDXMTDevice().device().newBuffer(buffer_info);
  if (!shared_buffer.handle || !buffer_info.memory.get_accessible_or_null())
    return E_FAIL;
  if (preserve_contents && !original.empty())
    std::memcpy(static_cast<uint8_t *>(mapping_view) + data_offset,
                original.data(), original.size());
  if (m_shared_mapping_view)
    UnmapViewOfFile(m_shared_mapping_view);
  if (m_shared_mapping)
    CloseHandle(m_shared_mapping);
  m_heap = nullptr;
  m_buffer = std::move(shared_buffer);
  m_buf_info = buffer_info;
  m_cpu_addr = buffer_info.memory.get_accessible_or_null();
  m_gpu_addr = buffer_info.gpu_address;
  m_shared_mapping = mapping;
  m_shared_mapping_view = mapping_view;
  m_shared_mapping_size = mapping_size;
  m_shared_data_offset = data_offset;
  m_shared_mapping_writable = writable;
  HTRACE("attached shared heap mapping=%p size=%llu gpu=0x%llx", mapping,
         (unsigned long long)m_desc.SizeInBytes,
         (unsigned long long)m_gpu_addr);
  return S_OK;
}

namespace {
static bool IsValidHeapSharedAccess(DWORD access) {
  constexpr DWORD kGenericAccess = GENERIC_READ | GENERIC_WRITE | GENERIC_ALL;
  return access != 0 && (access & ~kGenericAccess) == 0;
}

static DWORD HeapMappingAccessForSharedHandle(DWORD access) {
  if (access & GENERIC_ALL)
    return FILE_MAP_ALL_ACCESS;
  DWORD mapping_access = 0;
  if (access & GENERIC_READ)
    mapping_access |= FILE_MAP_READ;
  if (access & GENERIC_WRITE)
    mapping_access |= FILE_MAP_WRITE;
  return mapping_access;
}

static HRESULT DuplicateHeapMappingHandle(HANDLE source, DWORD access,
                                           HANDLE *duplicate,
                                           bool inheritable = false) {
  if (!duplicate)
    return E_POINTER;
  *duplicate = nullptr;
  const DWORD desired_access = access ? HeapMappingAccessForSharedHandle(access) : 0;
  const DWORD options = access ? 0 : DUPLICATE_SAME_ACCESS;
  if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                       duplicate, desired_access, inheritable, options))
    return HRESULT_FROM_WIN32(GetLastError());
  return S_OK;
}

static bool MapHeapSharedView(HANDLE mapping, void **view) {
  if (!view)
    return false;
  *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
  if (*view)
    return true;
  *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  return false;
}
}

HRESULT CreateSharedHeapMapping(
    MTLD3D12Heap *heap, const SECURITY_ATTRIBUTES *attributes,
    DWORD access, const WCHAR *name, HANDLE *mapping) {
  if (!heap || !name || !name[0] || !mapping || !heap->GetCPUAddress() ||
      !IsValidHeapSharedAccess(access))
    return E_INVALIDARG;
  *mapping = nullptr;
  const auto &desc = heap->GetHeapDesc();
  if (!desc.SizeInBytes || desc.SizeInBytes > UINT64_MAX - kD3D12SharedResourceDataOffset)
    return E_INVALIDARG;
  const uint64_t mapping_size = kD3D12SharedResourceDataOffset + desc.SizeInBytes;
  HANDLE section = CreateFileMappingW(
      INVALID_HANDLE_VALUE, const_cast<SECURITY_ATTRIBUTES *>(attributes),
      PAGE_READWRITE,
      static_cast<DWORD>(mapping_size >> 32),
      static_cast<DWORD>(mapping_size & std::numeric_limits<DWORD>::max()),
      name);
  if (!section)
    return HRESULT_FROM_WIN32(GetLastError());
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return DXGI_ERROR_NAME_ALREADY_EXISTS;
  }
  void *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (!view) {
    HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(section);
    return hr;
  }
  D3D12SharedHeapMetadata metadata = {};
  metadata.mapping_size = mapping_size;
  metadata.data_size = desc.SizeInBytes;
  metadata.reserved = access;
  metadata.heap_desc = desc;
  ID3D12Device *heap_device = nullptr;
  if (FAILED(heap->GetDevice(IID_PPV_ARGS(&heap_device)))) {
    UnmapViewOfFile(view);
    CloseHandle(section);
    return E_FAIL;
  }
  LUID heap_luid = {};
  if (!heap_device->GetAdapterLuid(&heap_luid)) {
    heap_device->Release();
    UnmapViewOfFile(view);
    CloseHandle(section);
    return E_FAIL;
  }
  metadata.adapter_luid =
      (static_cast<uint64_t>(static_cast<uint32_t>(heap_luid.HighPart)) << 32) |
      static_cast<uint32_t>(heap_luid.LowPart);
  heap_device->Release();
  if (!metadata.adapter_luid) {
    UnmapViewOfFile(view);
    CloseHandle(section);
    return E_FAIL;
  }
  std::memcpy(view, &metadata, sizeof(metadata));
  HANDLE owner_mapping = nullptr;
  HANDLE public_mapping = nullptr;
  HRESULT hr = DuplicateHeapMappingHandle(section, GENERIC_ALL,
                                           &owner_mapping);
  if (SUCCEEDED(hr))
    hr = DuplicateHeapMappingHandle(
        section, access, &public_mapping,
        attributes && attributes->bInheritHandle != FALSE);
  if (SUCCEEDED(hr))
    hr = heap->AttachSharedBacking(owner_mapping, view, mapping_size,
                                   metadata.data_offset, true, true);
  if (FAILED(hr)) {
    if (owner_mapping)
      CloseHandle(owner_mapping);
    if (public_mapping)
      CloseHandle(public_mapping);
    UnmapViewOfFile(view);
    CloseHandle(section);
    return hr;
  }
  CloseHandle(section);
  *mapping = public_mapping;
  return S_OK;
}

HRESULT OpenSharedHeapFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                  ID3D12Heap **heap) {
  if (!device || !mapping || !heap)
    return E_INVALIDARG;
  *heap = nullptr;
  void *view = nullptr;
  const bool writable = MapHeapSharedView(mapping, &view);
  if (!view)
    return HRESULT_FROM_WIN32(GetLastError());
  D3D12SharedHeapMetadata metadata = {};
  std::memcpy(&metadata, view, sizeof(metadata));
  if (metadata.magic != kD3D12SharedResourceMagic ||
      metadata.version != kD3D12SharedResourceVersion ||
      metadata.kind != kD3D12SharedResourceKindHeap ||
      !IsValidHeapSharedAccess(metadata.reserved) ||
      metadata.data_offset < sizeof(metadata) ||
      metadata.mapping_size < metadata.data_offset ||
      metadata.data_size != metadata.heap_desc.SizeInBytes ||
      metadata.data_size > metadata.mapping_size - metadata.data_offset ||
      !metadata.data_size || metadata.adapter_luid == 0) {
    UnmapViewOfFile(view);
    return DXGI_ERROR_INVALID_CALL;
  }
  ID3D12Device *current_device = nullptr;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&current_device)))) {
    UnmapViewOfFile(view);
    return E_FAIL;
  }
  LUID current_luid = {};
  const bool luid_ok = current_device->GetAdapterLuid(&current_luid) != nullptr;
  const uint64_t current_luid_value =
      luid_ok
          ? (static_cast<uint64_t>(static_cast<uint32_t>(current_luid.HighPart))
             << 32) |
                static_cast<uint32_t>(current_luid.LowPart)
          : 0;
  current_device->Release();
  if (!luid_ok || current_luid_value != metadata.adapter_luid) {
    UnmapViewOfFile(view);
    return DXGI_ERROR_INVALID_CALL;
  }
  HANDLE owner_mapping = nullptr;
  HRESULT hr = DuplicateHeapMappingHandle(mapping, 0, &owner_mapping);
  if (FAILED(hr)) {
    UnmapViewOfFile(view);
    return hr;
  }
  auto *created = new (std::nothrow) MTLD3D12Heap(device, metadata.heap_desc);
  if (!created) {
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return E_OUTOFMEMORY;
  }
  hr = created->AttachSharedBacking(owner_mapping, view, metadata.mapping_size,
                                    metadata.data_offset, false, writable);
  if (FAILED(hr)) {
    created->Release();
    CloseHandle(owner_mapping);
    UnmapViewOfFile(view);
    return hr;
  }
  hr = created->QueryInterface(IID_ID3D12Heap,
                               reinterpret_cast<void **>(heap));
  created->Release();
  if (FAILED(hr)) {
    *heap = nullptr;
    return hr;
  }
  return S_OK;
}

WMT::Reference<WMT::Heap> MTLD3D12Heap::GetMTLHeap() {
  const UINT heap_type = static_cast<UINT>(m_desc.Properties.Type);
  const bool gpu_upload = heap_type == 5;
  const bool custom_cpu_visible =
      heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_CUSTOM) &&
      (m_desc.Properties.CPUPageProperty ==
           D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
       m_desc.Properties.CPUPageProperty ==
           D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
  const bool placement_heap =
      heap_type == static_cast<UINT>(D3D12_HEAP_TYPE_DEFAULT) ||
      gpu_upload || custom_cpu_visible;
  if (m_heap.handle || !placement_heap ||
      !m_desc.SizeInBytes ||
      (m_desc.Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
          D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS)
    return m_heap;
  auto wmt_device = m_device->GetDXMTDevice().device();
  WMTHeapInfo info = {};
  info.size = m_desc.SizeInBytes;
  info.options = ((gpu_upload || custom_cpu_visible)
                      ? WMTResourceStorageModeShared
                      : WMTResourceStorageModePrivate) |
                 WMTResourceHazardTrackingModeTracked;
  info.type = WMTHeapTypePlacement;
  info.max_compatible_placement_sparse_page_size = WMTSparsePageSize64;
  m_heap = wmt_device.newHeap(info);
  HTRACE("GetMTLHeap placement handle=%llu size=%llu flags=0x%x",
         (unsigned long long)m_heap.handle,
         (unsigned long long)m_desc.SizeInBytes,
         (unsigned)m_desc.Flags);
  return m_heap;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Heap::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12Heap) {
    *ppvObject = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12Heap::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12Heap::Release() {
  uint32_t rc = --m_refCount;
  if (!rc)
    delete this;
  return rc;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Heap::GetPrivateData(REFGUID guid, UINT *data_size, void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Heap::SetPrivateData(REFGUID guid, UINT data_size,
                              const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Heap::SetPrivateDataInterface(REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Heap::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Heap::GetDevice(REFIID riid, void **device) {
  return m_device->QueryInterface(riid, device);
}

D3D12_HEAP_DESC *STDMETHODCALLTYPE
MTLD3D12Heap::GetDesc(D3D12_HEAP_DESC *__ret) {
  if (!__ret)
    return nullptr;
  *__ret = m_desc;
  return __ret;
}

} // namespace dxmt
