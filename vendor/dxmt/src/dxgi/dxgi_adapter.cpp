#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_trace.hpp"
#include "dxgi_options.hpp"
#include "util_string.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3d10_1.h"
#include "d3d12.h"
#include "Metal.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#define DATRACE(fmt, ...) DXMTDXGITrace("DXGIAdapter", fmt, ##__VA_ARGS__)

namespace dxmt {

static constexpr uint64_t kGiB = 1024ull * 1024ull * 1024ull;

static uint64_t umaBudgetFromWorkingSet(uint64_t working_set) {
  // Metal's recommended working set is a pressure hint, not the same contract
  // as DXGI's budget on unified memory. UE5's D3D12 transient allocator treats
  // the DXGI budget as hard placement headroom, so keep a conservative UMA
  // floor that leaves OS headroom on 16 GB Apple Silicon while avoiding false
  // 128 MB Nanite allocation failures.
  uint64_t budget = std::max<uint64_t>(working_set, 10ull * kGiB);
  return std::min<uint64_t>(budget, 12ull * kGiB);
}

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

LUID GetAdapterLuid(WMT::Device device) {
    // NOTE: use big-endian registryID, be consistent with MVK
  return std::bit_cast<LUID>(__builtin_bswap64(device.registryID()));
}

class MTLDXGIAdapter : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdapter(WMT::Device device, IDXGIFactory *factory, Config &config)
      : device_(device), factory_(factory), options_(config) {
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = GetAdapterLuid(device_);
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdapter() {
    {
      std::lock_guard lock(budget_event_mutex_);
      for (const auto &[cookie, event] : budget_events_) {
        (void)cookie;
        CloseHandle(event);
      }
      budget_events_.clear();
    }
    if (local_kmt_) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = local_kmt_;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT adapter");
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    DATRACE("QI this=%p riid=%s out=%p", this, str::format(riid).c_str(),
            ppvObject);
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
        riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
        riid == __uuidof(IDXGIAdapter4) || riid == __uuidof(IMTLDXGIAdapter)) {
      *ppvObject = ref(this);
      DATRACE("QI this=%p riid=%s -> S_OK out=%p", this,
              str::format(riid).c_str(), *ppvObject);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIAdapter), riid)) {
      WARN("DXGIAdapter: Unknown interface query ", str::format(riid));
    }

    DATRACE("QI this=%p riid=%s -> E_NOINTERFACE",
            this, str::format(riid).c_str());
    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    return factory_->QueryInterface(riid, ppParent);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) final {
    DATRACE("GetDesc this=%p desc=%p", this, pDesc);
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
    }

    DATRACE("GetDesc this=%p -> hr=0x%lx", this, hr);
    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) final {
    DATRACE("GetDesc1 this=%p desc=%p", this, pDesc);
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
    }

    DATRACE("GetDesc1 this=%p -> hr=0x%lx", this, hr);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) final {
    DATRACE("GetDesc2 this=%p desc=%p", this, pDesc);
    if (pDesc == nullptr)
      return E_INVALIDARG;

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
      pDesc->GraphicsPreemptionGranularity = desc.GraphicsPreemptionGranularity;
      pDesc->ComputePreemptionGranularity = desc.ComputePreemptionGranularity;
    }

    DATRACE("GetDesc2 this=%p -> hr=0x%lx", this, hr);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3 *pDesc) final {
    DATRACE("GetDesc3 this=%p desc=%p", this, pDesc);
    if (pDesc == nullptr)
      return E_INVALIDARG;

    std::memset(pDesc->Description, 0, sizeof(pDesc->Description));

    if (!options_.customDeviceDesc.empty()) {
      str::transcodeString(
          pDesc->Description,
          sizeof(pDesc->Description) / sizeof(pDesc->Description[0]) - 1,
          options_.customDeviceDesc.c_str(), options_.customDeviceDesc.size());
    } else {
      device_.name().getCString((char *)pDesc->Description, sizeof(pDesc->Description), WMTUTF16StringEncoding);
    }

    if (options_.customVendorId >= 0) {
      pDesc->VendorId = options_.customVendorId;
    } else {
      pDesc->VendorId = 0x1002;
      if (g_extension_enabled == VendorExtension::Nvidia) {
        pDesc->VendorId = 0x10DE;
      }
    }

    if (options_.customDeviceId >= 0) {
      pDesc->DeviceId = options_.customDeviceId;
    } else if (g_extension_enabled == VendorExtension::Nvidia) {
      pDesc->DeviceId = 0x2484; // GeForce RTX 3070
    } else {
      pDesc->DeviceId = 0x7340; // Radeon Pro 5300M
    }

    pDesc->SubSysId = 0;
    pDesc->Revision = 0;
    uint64_t working_set = device_.recommendedMaxWorkingSetSize();
    uint64_t uma_budget = umaBudgetFromWorkingSet(working_set);
    if (device_.hasUnifiedMemory())
      pDesc->DedicatedVideoMemory = uma_budget;
    else
      pDesc->DedicatedVideoMemory = working_set;
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = device_.hasUnifiedMemory() ? uma_budget : 0;
    pDesc->AdapterLuid = GetAdapterLuid(device_);
    pDesc->Flags = DXGI_ADAPTER_FLAG3_NONE;
    pDesc->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    pDesc->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    if (DXMTDXGITraceEnabled()) {
      FILE *f = fopen("Z:\\tmp\\dxmt_dxgi_trace.log", "a");
      if (f) {
        fwprintf(f, L"DXGIAdapter::GetDesc3: %s VendorId=0x%04x DeviceId=0x%04x VRAM=%lluMB Flags=0x%x\n",
                 pDesc->Description, pDesc->VendorId, pDesc->DeviceId,
                 (unsigned long long)pDesc->DedicatedVideoMemory / (1024 * 1024),
                 pDesc->Flags);
        fclose(f);
      }
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output,
                                        IDXGIOutput **ppOutput) final {
    DATRACE("EnumOutputs this=%p output=%u out=%p", this, Output, ppOutput);
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr)
      return E_INVALIDARG;

    HMONITOR monitor = wsi::enumMonitors(Output);
    if (monitor == nullptr) {
      DATRACE("EnumOutputs this=%p output=%u -> DXGI_ERROR_NOT_FOUND", this,
              Output);
      return DXGI_ERROR_NOT_FOUND;
    }

    *ppOutput = CreateOutput(this, monitor, options_);
    DATRACE("EnumOutputs this=%p output=%u -> S_OK out=%p", this, Output,
            *ppOutput);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CheckInterfaceSupport(const GUID &guid, LARGE_INTEGER *umd_version) final {
    DATRACE("CheckInterfaceSupport: %08lx-%04x-%04x", guid.Data1,
            guid.Data2, guid.Data3);
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;

    if (guid == __uuidof(IDXGIDevice) || guid == __uuidof(ID3D10Device) ||
        guid == __uuidof(ID3D10Device1) || guid == __uuidof(ID3D12Device))
      hr = S_OK;

    // We can't really reconstruct the version numbers
    // returned by Windows drivers from Metal
    if (SUCCEEDED(hr) && umd_version)
      umd_version->QuadPart = ~0ull;

    if (FAILED(hr)) {
      Logger::err("DXGI: CheckInterfaceSupport: Unsupported interface");
      Logger::err(str::format(guid));
    }

    DATRACE("CheckInterfaceSupport this=%p guid=%s -> hr=0x%lx", this,
            str::format(guid).c_str(), hr);
    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event,
                                                       DWORD *cookie) override {
    if (cookie)
      *cookie = 0;
    if (!event || !cookie)
      return E_INVALIDARG;
    DATRACE("RegisterHardwareContentProtectionTeardownStatusEvent -> unsupported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override {
    DATRACE("UnregisterHardwareContentProtectionTeardownStatus cookie=%u",
            cookie);
  }

  HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) override {
    DATRACE("QueryVideoMemoryInfo this=%p node=%u group=%u info=%p", this,
            NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    // Metal unified-memory devices still need a Windows-like local segment
    // budget. UE5's transient allocator treats zero reservation headroom as an
    // exhausted heap, so expose the remaining recommended working set instead
    // of claiming reservations are impossible.
    uint64_t working_set = device_.recommendedMaxWorkingSetSize();
    uint64_t budget = device_.hasUnifiedMemory()
                          ? umaBudgetFromWorkingSet(working_set)
                          : working_set;
    {
      std::lock_guard lock(budget_event_mutex_);
      const uint32_t group = uint32_t(MemorySegmentGroup);
      if (last_budget_[group] && last_budget_[group] != budget) {
        for (const auto &[cookie, event] : budget_events_) {
          (void)cookie;
          SetEvent(event);
        }
      }
      last_budget_[group] = budget;
    }
    uint64_t usage = MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL
                         ? device_.currentAllocatedSize()
                         : 0;
    uint64_t reservation = mem_reserved_[uint32_t(MemorySegmentGroup)];
    uint64_t committed = std::min<uint64_t>(budget, reservation);

    pVideoMemoryInfo->Budget = budget;
    pVideoMemoryInfo->CurrentUsage = usage;
    pVideoMemoryInfo->AvailableForReservation = budget - committed;
    pVideoMemoryInfo->CurrentReservation =
        reservation;
    static uint32_t s_query_log_count = 0;
    uint32_t query_log_count =
        __atomic_add_fetch(&s_query_log_count, 1, __ATOMIC_RELAXED);
    if (query_log_count <= 32 || (query_log_count % 128) == 0) {
      Logger::info(str::format("M12 DXGI video memory group=",
                               (uint32_t)MemorySegmentGroup,
                               " budget=", budget,
                               " usage=", usage,
                               " reservation=", reservation,
                               " available=", pVideoMemoryInfo->AvailableForReservation,
                               " working_set=", working_set));
    }
    DATRACE("QueryVideoMemoryInfo this=%p -> budget=%llu usage=%llu available=%llu reservation=%llu",
            this, (unsigned long long)pVideoMemoryInfo->Budget,
            (unsigned long long)pVideoMemoryInfo->CurrentUsage,
            (unsigned long long)pVideoMemoryInfo->AvailableForReservation,
            (unsigned long long)pVideoMemoryInfo->CurrentReservation);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      UINT64 Reservation) override {
    DATRACE("SetVideoMemoryReservation this=%p node=%u group=%u reservation=%llu",
            this, NodeIndex, MemorySegmentGroup,
            (unsigned long long)Reservation);
    if (NodeIndex > 0)
      return E_INVALIDARG;

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return E_INVALIDARG;

    mem_reserved_[uint32_t(MemorySegmentGroup)] = Reservation;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
      HANDLE event, DWORD *cookie) override {
    if (cookie)
      *cookie = 0;
    if (!event || !cookie)
      return E_INVALIDARG;
    HANDLE duplicate = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                         &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return HRESULT_FROM_WIN32(GetLastError());
    std::lock_guard lock(budget_event_mutex_);
    DWORD value = next_budget_event_cookie_++;
    while (!value || budget_events_.contains(value))
      value = next_budget_event_cookie_++;
    budget_events_.emplace(value, duplicate);
    *cookie = value;
    DATRACE("RegisterVideoMemoryBudgetChangeNotificationEvent event=%p cookie=%u",
            event, value);
    return S_OK;
  }

  void STDMETHODCALLTYPE
  UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override {
    std::lock_guard lock(budget_event_mutex_);
    auto entry = budget_events_.find(cookie);
    if (entry == budget_events_.end())
      return;
    CloseHandle(entry->second);
    budget_events_.erase(entry);
    DATRACE("UnregisterVideoMemoryBudgetChangeNotification cookie=%u",
            cookie);
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() final { return device_; }
  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

private:
  WMT::Reference<WMT::Device> device_;
  D3DKMT_HANDLE local_kmt_ = 0;
  Com<IDXGIFactory> factory_;
  DxgiOptions options_;
  uint64_t mem_reserved_[2] = {0, 0};
  uint64_t last_budget_[2] = {0, 0};
  std::mutex budget_event_mutex_;
  std::unordered_map<DWORD, HANDLE> budget_events_;
  DWORD next_budget_event_cookie_ = 1;
};

Com<IMTLDXGIAdapter> CreateAdapter(WMT::Device Device,
                                   IDXGIFactory2 *pFactory, Config &config) {
  return Com<IMTLDXGIAdapter>::transfer(
      new MTLDXGIAdapter(Device, pFactory, config));
}

} // namespace dxmt
