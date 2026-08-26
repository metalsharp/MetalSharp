#include "d3d12_heap.hpp"
#include "d3d12_device.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#define HTRACE(fmt, ...) DXMTD3D12Trace("Heap", fmt, ##__VA_ARGS__)

namespace dxmt {

MTLD3D12Heap::MTLD3D12Heap(MTLD3D12Device *device, const D3D12_HEAP_DESC &desc)
    : m_device(device), m_desc(desc) {
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
    bool cpu_accessible = (desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
                           desc.Properties.Type == D3D12_HEAP_TYPE_READBACK);
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
}

MTLD3D12Heap::~MTLD3D12Heap() {
  HTRACE("dtor");
  m_heap = nullptr;
  m_buffer = nullptr;
  m_device->Release();
}

WMT::Reference<WMT::Heap> MTLD3D12Heap::GetMTLHeap() {
  if (m_heap.handle || m_desc.Properties.Type != D3D12_HEAP_TYPE_DEFAULT ||
      !m_desc.SizeInBytes ||
      (m_desc.Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) ==
          D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS)
    return m_heap;
  auto wmt_device = m_device->GetDXMTDevice().device();
  WMTHeapInfo info = {};
  info.size = m_desc.SizeInBytes;
  info.options = WMTResourceStorageModePrivate |
                 WMTResourceHazardTrackingModeTracked;
  info.type = WMTHeapTypePlacement;
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
  *__ret = m_desc;
  return __ret;
}

} // namespace dxmt
