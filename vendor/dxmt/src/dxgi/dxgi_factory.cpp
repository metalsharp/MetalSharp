#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_trace.hpp"
#include "dxgi_object.hpp"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_window.hpp"
#include "Metal.hpp"
#include <mutex>
#include <unordered_map>

#define DGTRACE(fmt, ...) DXMTDXGITrace("DXGI", fmt, ##__VA_ARGS__)

namespace dxmt {

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config);

static constexpr GUID IID_IDXGIFactory6_MetalSharp =
    dxmt::guid::make_guid("c1b6694f-ff09-44a9-b03c-779002a47b07");
static constexpr GUID IID_IDXGIFactory7_MetalSharp =
    dxmt::guid::make_guid("a4966eed-76db-44da-84c1-ee9a7afb20a8");
// UE5/Subnautica 2 queries this non-WinSDK factory IID during adapter
// selection. It follows the IDXGIFactory6 family and only needs the factory
// vtable for enumeration, so accept it as a compatibility alias.
static constexpr GUID IID_IDXGIFactory6_UEShim =
    dxmt::guid::make_guid("c1b6694f-ff09-44a9-b03c-77900a0a1d17");

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory7> {

public:
  MTLDXGIFactory(UINT Flags) : flags_(Flags) {};
  ~MTLDXGIFactory() {
    std::lock_guard lock(adapter_event_mutex_);
    for (const auto &[cookie, event] : adapter_events_) {
      (void)cookie;
      CloseHandle(event);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    
    DGTRACE("Factory::QI riid=%s", str::format(riid).c_str());

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
        riid == __uuidof(IDXGIFactory7) ||
        riid == IID_IDXGIFactory6_MetalSharp ||
        riid == IID_IDXGIFactory7_MetalSharp ||
        riid == IID_IDXGIFactory6_UEShim) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) final {
    DGTRACE("CreateSwapChain (legacy) called");
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(pDevice, pDesc->OutputWindow, &desc,
                                        &descFs, nullptr, &swapChain);

    *ppSwapChain = swapChain;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    DGTRACE("CreateSwapChainForHwnd called hwnd=%p desc=%p fullscreen=%p assoc=%p flags=0x%x",
            hWnd, pDesc, pFullscreenDesc, associated_window_, window_assoc_flags_);
    InitReturnPtr(ppSwapChain);

    if (!hWnd && associated_window_) {
      DGTRACE("CreateSwapChainForHwnd using associated hwnd=%p", associated_window_);
      hWnd = associated_window_;
    }

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice) {
      DGTRACE("CreateSwapChainForHwnd -> DXGI_ERROR_INVALID_CALL (null args)");
      return DXGI_ERROR_INVALID_CALL;
    }

    Com<IMTLDXGIDevice> metal_dxgi_device;
    HRESULT qhr = pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device));
    if (FAILED(qhr)) {
      DGTRACE("CreateSwapChainForHwnd -> QI IMTLDXGIDevice FAILED hr=0x%lx", qhr);
      ERR("Unsupported device type");
      return DXGI_ERROR_UNSUPPORTED;
    }

    // Make sure the back buffer size is not zero
    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    bool valid_window = wsi::isWindow(hWnd);
    DGTRACE("CreateSwapChainForHwnd hwnd=%p valid=%u requested=%ux%u fmt=%u buffers=%u swapEffect=%u flags=0x%x",
            hWnd, valid_window ? 1 : 0, desc.Width, desc.Height,
            (unsigned)desc.Format, desc.BufferCount, (unsigned)desc.SwapEffect,
            desc.Flags);

    if (!valid_window)
      return DXGI_ERROR_INVALID_CALL;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    return metal_dxgi_device->CreateSwapChain(this, hWnd, &desc, &fsDesc,
                                              ppSwapChain);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    DGTRACE("EnumAdapters(%u, %p)", Adapter, ppAdapter);
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    DGTRACE("EnumAdapters(%u) -> hr=0x%lx adapter=%p", Adapter, hr,
            *ppAdapter);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    DGTRACE("EnumAdapters1(%u, %p)", Adapter, ppAdapter);
    InitReturnPtr(ppAdapter);

    auto devices = WMT::CopyAllDevices();
    UINT adapter_count = devices.count();
    auto &config = Config::getInstance();
    bool mirror_adapter_0_for_adapter_1 =
        config.getOption<bool>("dxgi.mirrorAdapter0ForAdapter1", false);
    DGTRACE("EnumAdapters1: adapter_count=%u mirror0for1=%u", adapter_count,
            mirror_adapter_0_for_adapter_1 ? 1 : 0);

    if (Adapter >= adapter_count) {
      if (mirror_adapter_0_for_adapter_1 && adapter_count == 1 && Adapter == 1) {
        DGTRACE("EnumAdapters1: profile mirror adapter 1 -> adapter 0");
        Adapter = 0;
      } else {
        return DXGI_ERROR_NOT_FOUND;
      }
    }

    UINT adjusted_adapter = Adapter;
    if (adapter_count > 1) {
      UINT preferred_adapter = 0;
      for (unsigned i = 0; i < adapter_count; i++) {
        if (!devices.object(i).hasUnifiedMemory())
          preferred_adapter = i;
      }
      if (Adapter == 0)
        adjusted_adapter = preferred_adapter;
      else
        adjusted_adapter = Adapter <= preferred_adapter ? Adapter - 1 : Adapter;
    }

    auto device = devices.object(adjusted_adapter);

    *ppAdapter = CreateAdapter(device, this, config);
    // devices->release(); // no you should not release it...
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pWindowHandle = associated_window_;
    DGTRACE("GetWindowAssociation -> hwnd=%p flags=0x%x",
            associated_window_, window_assoc_flags_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) final {
    associated_window_ = WindowHandle;
    window_assoc_flags_ = Flags;
    DGTRACE("MakeWindowAssociation hwnd=%p flags=0x%x valid=%u", WindowHandle,
            Flags, WindowHandle ? (wsi::isWindow(WindowHandle) ? 1 : 0) : 0);
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return TRUE; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    if (!pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    *pdwCookie = next_status_cookie_++;
    DGTRACE("RegisterOcclusionStatusWindow hwnd=%p msg=%u cookie=%u",
            WindowHandle, wMsg, *pdwCookie);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    if (!pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    *pdwCookie = next_status_cookie_++;
    DGTRACE("RegisterStereoStatusEvent event=%p cookie=%u", hEvent, *pdwCookie);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    if (!pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    *pdwCookie = next_status_cookie_++;
    DGTRACE("RegisterStereoStatusWindow hwnd=%p msg=%u cookie=%u",
            WindowHandle, wMsg, *pdwCookie);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    if (!pdwCookie)
      return DXGI_ERROR_INVALID_CALL;
    *pdwCookie = next_status_cookie_++;
    DGTRACE("RegisterOcclusionStatusEvent event=%p cookie=%u", hEvent,
            *pdwCookie);
    return S_OK;
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    DGTRACE("UnregisterStereoStatus cookie=%u", dwCookie);
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    DGTRACE("UnregisterOcclusionStatus cookie=%u", dwCookie);
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid,
                                              void **adapter) override {
    DGTRACE("EnumAdapterByLuid luid=%08lx:%08lx iid=%s adapter=%p",
            luid.HighPart, luid.LowPart, str::format(iid).c_str(), adapter);
    InitReturnPtr(adapter);

    if (!adapter)
      return DXGI_ERROR_INVALID_CALL;

    auto devices = WMT::CopyAllDevices();
    UINT adapter_count = devices.count();
    for (UINT i = 0; i < adapter_count; i++) {
      auto candidate = CreateAdapter(devices.object(i), this, Config::getInstance());
      DXGI_ADAPTER_DESC1 desc = {};
      HRESULT desc_hr = candidate->GetDesc1(&desc);
      DGTRACE("EnumAdapterByLuid candidate=%u desc_hr=0x%lx luid=%08lx:%08lx",
              i, desc_hr, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
      if (SUCCEEDED(desc_hr) && desc.AdapterLuid.HighPart == luid.HighPart &&
          desc.AdapterLuid.LowPart == luid.LowPart) {
        HRESULT hr = candidate->QueryInterface(iid, adapter);
        DGTRACE("EnumAdapterByLuid match candidate=%u -> hr=0x%lx out=%p",
                i, hr, adapter ? *adapter : nullptr);
        return hr;
      }
    }

    DGTRACE("EnumAdapterByLuid -> DXGI_ERROR_NOT_FOUND");
    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    ERR("DXGIFactory::EnumWrapAdapter: not implemented");
    return DXGI_ERROR_NOT_FOUND;
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return E_INVALIDARG;

      *info = TRUE;
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      return E_INVALIDARG;
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    DGTRACE("EnumAdapterByGpuPreference adapter=%u preference=%u riid=%s out=%p",
            Adapter, GpuPreference, str::format(riid).c_str(), ppvAdapter);
    // GpuPreference ignored, since Apple Silicon has only 1 GPU anyway
    // FIXME: support Intel Mac with dedicated GPU
    Com<IDXGIAdapter1> adapter;
    HRESULT hr = this->EnumAdapters1(Adapter, &adapter);

    if (FAILED(hr)) {
      DGTRACE("EnumAdapterByGpuPreference EnumAdapters1 failed hr=0x%lx", hr);
      return hr;
    }
    hr = adapter->QueryInterface(riid, ppvAdapter);
    DGTRACE("EnumAdapterByGpuPreference -> hr=0x%lx out=%p", hr,
            ppvAdapter ? *ppvAdapter : nullptr);
    return hr;
  };

  HRESULT STDMETHODCALLTYPE
  RegisterAdaptersChangedEvent(HANDLE event, DWORD *cookie) override {
    if (cookie)
      *cookie = 0;
    if (!event || !cookie)
      return DXGI_ERROR_INVALID_CALL;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                         &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return HRESULT_FROM_WIN32(GetLastError());
    std::lock_guard lock(adapter_event_mutex_);
    DWORD value = next_adapter_event_cookie_++;
    while (!value || adapter_events_.contains(value))
      value = next_adapter_event_cookie_++;
    adapter_events_.emplace(value, duplicate);
    *cookie = value;
    DGTRACE("RegisterAdaptersChangedEvent event=%p cookie=%u", event, value);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  UnregisterAdaptersChangedEvent(DWORD cookie) override {
    std::lock_guard lock(adapter_event_mutex_);
    auto entry = adapter_events_.find(cookie);
    if (entry == adapter_events_.end())
      return DXGI_ERROR_INVALID_CALL;
    CloseHandle(entry->second);
    adapter_events_.erase(entry);
    DGTRACE("UnregisterAdaptersChangedEvent cookie=%u", cookie);
    return S_OK;
  }

private:
  UINT flags_;

  HWND associated_window_ = nullptr;
  UINT window_assoc_flags_ = 0;
  DWORD next_status_cookie_ = 1;
  std::mutex adapter_event_mutex_;
  std::unordered_map<DWORD, HANDLE> adapter_events_;
  DWORD next_adapter_event_cookie_ = 1;
};

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid,
                                                void **ppFactory) {
  DXMTDXGITrace("DXGI", "CreateDXGIFactory2 called Flags=0x%x riid=%s", Flags,
                str::format(riid).c_str());
  try {
    MTLDXGIFactory* factory = new MTLDXGIFactory(Flags);
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    DXMTDXGITrace("DXGI", "CreateDXGIFactory2 QI hr=0x%lx", hr);

    if (FAILED(hr))
      return hr;

    return S_OK;
  } catch (const MTLD3DError &e) {
    Logger::err(e.message());
    return E_FAIL;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  return CreateDXGIFactory2(0, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  return CreateDXGIFactory2(0, riid, factory);
}

} // namespace dxmt
