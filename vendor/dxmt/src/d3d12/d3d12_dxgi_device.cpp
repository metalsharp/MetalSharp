#include "d3d12_dxgi_device.hpp"
#include "d3d12_dxgi_surface.hpp"
#include "d3d12_device.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_swapchain.hpp"
#include "d3d12_video_compat.hpp"
#include "../dxgi/dxgi_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <windows.h>

#define DDTRACE(fmt, ...) DXMTDXGITrace("DXGIDev", fmt, ##__VA_ARGS__)

namespace dxmt {

namespace {

constexpr GUID kIIDDevice11Compat = {
    0x5405c344, 0xd457, 0x444e,
    {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
constexpr GUID kIIDDevice12Compat = {
    0x5af5c532, 0x4c91, 0x4cd0,
    {0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5}};
constexpr GUID kIIDDevice13Compat = {
    0x14eecffc, 0x4df8, 0x40f7,
    {0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e}};
constexpr GUID kIIDDevice14Compat = {
    0x5f6e592d, 0xd895, 0x44c2,
    {0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23}};
constexpr GUID kIIDDevice15Compat = {
    0x76cff76f, 0x1e9b, 0x4450,
    {0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b}};

static bool QueryD3D12Residency(IUnknown *object, bool *resident) {
  if (!object || !resident)
    return false;
  ID3D12Resource *resource = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
    *resident = static_cast<MTLD3D12Resource *>(resource)->IsResident();
    resource->Release();
    return true;
  }
  ID3D12Heap *heap = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap)))) {
    *resident = static_cast<MTLD3D12Heap *>(heap)->IsResident();
    heap->Release();
    return true;
  }
  return false;
}

static bool SetD3D12Residency(IUnknown *object, bool resident) {
  if (!object)
    return false;
  ID3D12Resource *resource = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
    if (resident)
      static_cast<MTLD3D12Resource *>(resource)->MakeResident();
    else
      static_cast<MTLD3D12Resource *>(resource)->Evict();
    resource->Release();
    return true;
  }
  ID3D12Heap *heap = nullptr;
  if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&heap)))) {
    if (resident)
      static_cast<MTLD3D12Heap *>(heap)->MakeResident();
    else
      static_cast<MTLD3D12Heap *>(heap)->Evict();
    heap->Release();
    return true;
  }
  return false;
}

} // namespace

MTLD3D12DXGIDevice::MTLD3D12DXGIDevice(std::unique_ptr<Device> &&device,
                                       IMTLDXGIAdapter *adapter)
    : m_adapter(adapter) {
  if (m_adapter)
    m_adapter->AddRef();
  void *dev_mem = VirtualAlloc((void*)0x500000000ULL, sizeof(MTLD3D12Device),
    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!dev_mem) dev_mem = VirtualAlloc(nullptr, sizeof(MTLD3D12Device),
    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  m_d3d12_device = ::new(dev_mem) MTLD3D12Device(std::move(device), m_adapter.ptr());
  DDTRACE("D3D12Device at %p (VirtualAlloc)", (void*)m_d3d12_device);
  m_d3d12_device->SetDXGIDevice(this);
  Logger::info("D3D12DXGIDevice created");
}

MTLD3D12DXGIDevice::~MTLD3D12DXGIDevice() {
  {
    std::lock_guard lock(m_offered_resource_mutex);
    for (auto *resource : m_offered_resources)
      if (resource)
        resource->Release();
    m_offered_resources.clear();
  }
  if (m_d3d12_device) {
    m_d3d12_device->SetDXGIDevice(nullptr);
    m_d3d12_device->Release();
  }
}

ULONG STDMETHODCALLTYPE MTLD3D12DXGIDevice::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12DXGIDevice::Release() {
  uint32_t rc = --m_refCount;
  if (!rc) {
    this->~MTLD3D12DXGIDevice();
    VirtualFree(this, 0, MEM_RELEASE);
  }
  return rc;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) {
  return m_private_data.getData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::SetPrivateData(REFGUID Name, UINT DataSize,
                                   const void *pData) {
  return m_private_data.setData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::SetPrivateDataInterface(REFGUID Name,
                                            const IUnknown *pUnknown) {
  return m_private_data.setInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::QueryInterface(REFIID riid, void **ppvObject) {
  DDTRACE("QI(%s)", str::format(riid).c_str());
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid.Data1 == 0x1f052807u)
    return m_d3d12_device->QueryInterface(riid, ppvObject);

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
      riid == __uuidof(IDXGIDevice) || riid == __uuidof(IDXGIDevice1) ||
      riid == __uuidof(IDXGIDevice2) || riid == __uuidof(IDXGIDevice3) ||
      riid == __uuidof(IMTLDXGIDevice)) {
    *ppvObject = ref(this);
    return S_OK;
  }

  if (riid == __uuidof(ID3D12Device) || riid == IID_ID3D12Device1 ||
      riid == IID_ID3D12Device2 || riid == IID_ID3D12Device3 ||
      riid == IID_ID3D12Device4 || riid == IID_ID3D12Device5 ||
      riid == IID_ID3D12Device6 || riid == IID_ID3D12Device7 ||
      riid == IID_ID3D12Device8 || riid == IID_ID3D12Device9 ||
      riid == IID_ID3D12Device10 || riid == kIIDDevice11Compat ||
      riid == kIIDDevice12Compat || riid == kIIDDevice13Compat ||
      riid == kIIDDevice14Compat || riid == kIIDDevice15Compat ||
      riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild)) {
    return m_d3d12_device->QueryInterface(riid, ppvObject);
  }

  Logger::warn(str::format("D3D12DXGIDevice::QueryInterface: unknown ", riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::GetParent(REFIID riid, void **ppParent) {
  return m_adapter->QueryInterface(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::GetAdapter(IDXGIAdapter **pAdapter) {
  if (!pAdapter)
    return DXGI_ERROR_INVALID_CALL;
  *pAdapter = m_adapter.ref();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::CreateSurface(const DXGI_SURFACE_DESC *desc,
                                   UINT surface_count, DXGI_USAGE usage,
                                   const DXGI_SHARED_RESOURCE *shared_resource,
                                   IDXGISurface **surface) {
  DDTRACE("CreateSurface count=%u usage=0x%x desc=%p", surface_count,
          static_cast<unsigned>(usage), (const void *)desc);
  if (!desc || !surface)
    return E_POINTER;
  if (!surface_count || surface_count > 16 ||
      desc->Width == 0 || desc->Height == 0 ||
      desc->SampleDesc.Count != 1 || desc->SampleDesc.Quality != 0)
    return DXGI_ERROR_INVALID_CALL;
  constexpr DXGI_USAGE known_usage =
      static_cast<DXGI_USAGE>(DXGI_USAGE_SHADER_INPUT |
                              DXGI_USAGE_RENDER_TARGET_OUTPUT |
                              DXGI_USAGE_BACK_BUFFER |
                              DXGI_USAGE_SHARED |
                              DXGI_USAGE_READ_ONLY |
                              DXGI_USAGE_DISCARD_ON_PRESENT |
                              DXGI_USAGE_UNORDERED_ACCESS);
  if (usage & ~known_usage)
    return DXGI_ERROR_INVALID_CALL;
  if (shared_resource) {
    // The bounded shared-surface provider is backed by the same portable
    // shared-texture mapping used by OpenSharedHandle.  Do not accept an
    // arbitrary handle or manufacture a surface around a mismatched resource:
    // open the resource, validate the exact one-surface descriptor, and keep
    // the opened COM reference alive until the wrapper has retained it.
    if (surface_count != 1 || !shared_resource->Handle ||
        !(usage & DXGI_USAGE_SHARED))
      return DXGI_ERROR_INVALID_CALL;
    surface[0] = nullptr;
    ID3D12Resource *opened_resource = nullptr;
    HRESULT open_hr = m_d3d12_device->OpenSharedHandle(
        shared_resource->Handle, IID_PPV_ARGS(&opened_resource));
    if (FAILED(open_hr) || !opened_resource)
      return FAILED(open_hr) ? open_hr : DXGI_ERROR_INVALID_CALL;

    D3D12_RESOURCE_DESC opened_desc = {};
    opened_resource->GetDesc(&opened_desc);
    const bool descriptor_matches =
        opened_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        opened_desc.Width == desc->Width &&
        opened_desc.Height == desc->Height &&
        opened_desc.DepthOrArraySize == 1 && opened_desc.MipLevels == 1 &&
        opened_desc.Format == desc->Format &&
        opened_desc.SampleDesc.Count == desc->SampleDesc.Count &&
        opened_desc.SampleDesc.Quality == desc->SampleDesc.Quality;
    if (!descriptor_matches) {
      opened_resource->Release();
      return DXGI_ERROR_INVALID_CALL;
    }

    auto *surface_impl = CreateD3D12DXGISurface(
        this, static_cast<MTLD3D12Resource *>(opened_resource), 0, *desc);
    opened_resource->Release();
    if (!surface_impl)
      return DXGI_ERROR_UNSUPPORTED;
    surface[0] = surface_impl;
    return S_OK;
  }
  for (UINT i = 0; i < surface_count; ++i)
    surface[i] = nullptr;
  D3D12_RESOURCE_DESC resource_desc = {};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resource_desc.Width = desc->Width;
  resource_desc.Height = desc->Height;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  resource_desc.Format = desc->Format;
  resource_desc.SampleDesc = desc->SampleDesc;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  if (usage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
    resource_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        resource_desc.Flags | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  if (usage & DXGI_USAGE_UNORDERED_ACCESS)
    resource_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        resource_desc.Flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
  if (!(usage & (DXGI_USAGE_RENDER_TARGET_OUTPUT |
                 DXGI_USAGE_UNORDERED_ACCESS))) {
    initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
  }

  for (UINT i = 0; i < surface_count; ++i) {
    ID3D12Resource *resource = nullptr;
    HRESULT hr = m_d3d12_device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resource_desc, initial_state, nullptr,
        IID_PPV_ARGS(&resource));
    if (FAILED(hr) || !resource) {
      for (UINT j = 0; j < i; ++j)
        surface[j]->Release();
      return FAILED(hr) ? hr : E_FAIL;
    }
    auto *dxmt_resource = static_cast<MTLD3D12Resource *>(resource);
    surface[i] = CreateD3D12DXGISurface(this, dxmt_resource, 0, *desc);
    resource->Release();
    if (!surface[i]) {
      for (UINT j = 0; j < i; ++j)
        surface[j]->Release();
      return DXGI_ERROR_UNSUPPORTED;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::QueryResourceResidency(IUnknown *const *ppResources,
                                           DXGI_RESIDENCY *pResidency,
                                           UINT ResourceCount) {
  if (ResourceCount && (!ppResources || !pResidency))
    return E_INVALIDARG;
  for (UINT i = 0; i < ResourceCount; i++) {
    bool resident = false;
    if (!QueryD3D12Residency(ppResources[i], &resident))
      return E_INVALIDARG;
    pResidency[i] = resident ? DXGI_RESIDENCY_FULLY_RESIDENT
                             : DXGI_RESIDENCY_EVICTED_TO_DISK;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::SetGPUThreadPriority(INT Priority) {
  if (Priority < -7 || Priority > 7)
    return E_INVALIDARG;
  m_gpu_thread_priority.store(Priority, std::memory_order_release);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::GetGPUThreadPriority(INT *pPriority) {
  if (!pPriority)
    return E_POINTER;
  *pPriority = m_gpu_thread_priority.load(std::memory_order_acquire);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::SetMaximumFrameLatency(UINT MaxLatency) {
  if (MaxLatency > 16)
    return E_INVALIDARG;
  m_maximum_frame_latency.store(MaxLatency ? MaxLatency : 3,
                                std::memory_order_release);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::GetMaximumFrameLatency(UINT *pMaxLatency) {
  if (!pMaxLatency)
    return E_POINTER;
  *pMaxLatency = m_maximum_frame_latency.load(std::memory_order_acquire);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::OfferResources(UINT NumResources,
                                   IDXGIResource *const *ppResources,
                                   DXGI_OFFER_RESOURCE_PRIORITY Priority) {
  if (Priority < DXGI_OFFER_RESOURCE_PRIORITY_LOW ||
      Priority > DXGI_OFFER_RESOURCE_PRIORITY_HIGH ||
      (NumResources && !ppResources))
    return E_INVALIDARG;
  for (UINT i = 0; i < NumResources; i++) {
    bool resident = false;
    if (!ppResources[i] || !QueryD3D12Residency(ppResources[i], &resident))
      return E_INVALIDARG;
  }
  std::lock_guard lock(m_offered_resource_mutex);
  for (UINT i = 0; i < NumResources; i++) {
    if (m_offered_resources.insert(ppResources[i]).second)
      ppResources[i]->AddRef();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::ReclaimResources(UINT NumResources,
                                     IDXGIResource *const *ppResources,
                                     WINBOOL *pDiscarded) {
  if (NumResources && !ppResources)
    return E_INVALIDARG;
  if (pDiscarded) {
    for (UINT i = 0; i < NumResources; i++)
      pDiscarded[i] = FALSE;
  }
  std::lock_guard lock(m_offered_resource_mutex);
  for (UINT i = 0; i < NumResources; i++) {
    if (!ppResources[i] || !m_offered_resources.contains(ppResources[i]))
      return E_INVALIDARG;
  }
  for (UINT i = 0; i < NumResources; i++) {
    bool resident = true;
    QueryD3D12Residency(ppResources[i], &resident);
    if (pDiscarded)
      pDiscarded[i] = resident ? FALSE : TRUE;
    SetD3D12Residency(ppResources[i], true);
    if (m_offered_resources.erase(ppResources[i]))
      ppResources[i]->Release();
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12DXGIDevice::EnqueueSetEvent(HANDLE hEvent) {
  return m_d3d12_device->EnqueueSetEvent(hEvent);
}

void STDMETHODCALLTYPE MTLD3D12DXGIDevice::Trim() {
  {
    std::lock_guard lock(m_offered_resource_mutex);
    for (auto *resource : m_offered_resources)
      SetD3D12Residency(resource, false);
  }
  if (m_d3d12_device)
    m_d3d12_device->NotifyTrimCallbacks();
}

WMT::Device STDMETHODCALLTYPE MTLD3D12DXGIDevice::GetMTLDevice() {
  return m_adapter->GetMTLDevice();
}

D3DKMT_HANDLE STDMETHODCALLTYPE MTLD3D12DXGIDevice::GetLocalD3DKMT() {
  return m_kmt;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGIDevice::CreateSwapChain(
    IDXGIFactory1 *pFactory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGISwapChain1 **ppSwapChain) {
  DDTRACE("CreateSwapChain called");
  return dxmt::CreateD3D12SwapChain(pFactory, m_d3d12_device, this, hWnd,
                                    pDesc, pFullscreenDesc, ppSwapChain);
}

} // namespace dxmt
