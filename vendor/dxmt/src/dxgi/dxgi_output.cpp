#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <numeric>
#include <new>
#include <vector>
#include <d3d12.h>

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_output.hpp"
#include "dxgi_options.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"

namespace dxmt {

static constexpr GUID kIID_ID3D12DeviceForDuplication = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static constexpr GUID kIID_ID3D12ResourceForSurface = {
    0x696442be, 0xa72e, 0x4059,
    {0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad}};

class MTLDXGIOutputImpl;
static HRESULT CreateOutputDuplication(MTLDXGIOutputImpl *output,
                                       IUnknown *device,
                                       IDXGIOutputDuplication **duplication,
                                       UINT format_count = 0,
                                       const DXGI_FORMAT *formats = nullptr);

/*
 * \see
 * https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/DirectXTexUtil.cpp
 */
uint32_t GetMonitorFormatBpp(DXGI_FORMAT Format) {
  switch (Format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    return 32;

  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return 64;

  default:
    Logger::warn(str::format("GetMonitorFormatBpp: Unknown format: ", Format));
    return 32;
  }
}

DXGI_MODE_DESC1 ConvertDisplayMode(const wsi::WsiMode &WsiMode) {
  DXGI_MODE_DESC1 dxgiMode = {};
  dxgiMode.Width = WsiMode.width;
  dxgiMode.Height = WsiMode.height;
  dxgiMode.RefreshRate = DXGI_RATIONAL{WsiMode.refreshRate.numerator,
                                       WsiMode.refreshRate.denominator};
  dxgiMode.Format = WsiMode.bitsPerPixel >= 64
                         ? DXGI_FORMAT_R16G16B16A16_FLOAT
                         : (WsiMode.bitsPerPixel >= 30
                                ? DXGI_FORMAT_R10G10B10A2_UNORM
                                : DXGI_FORMAT_R8G8B8A8_UNORM);
  dxgiMode.ScanlineOrdering = WsiMode.interlaced
                                  ? DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST
                                  : DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
  dxgiMode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  dxgiMode.Stereo = FALSE;
  return dxgiMode;
}

void FilterModesByDesc(std::vector<DXGI_MODE_DESC1> &Modes,
                       const DXGI_MODE_DESC1 &TargetMode) {
  // Filter modes based on format properties
  bool testScanlineOrder = false;
  bool testScaling = false;
  bool testFormat = false;

  for (const auto &mode : Modes) {
    testScanlineOrder |=
        TargetMode.ScanlineOrdering != DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED &&
        TargetMode.ScanlineOrdering == mode.ScanlineOrdering;
    testScaling |= TargetMode.Scaling != DXGI_MODE_SCALING_UNSPECIFIED &&
                   TargetMode.Scaling == mode.Scaling;
    testFormat |= TargetMode.Format != DXGI_FORMAT_UNKNOWN &&
                  TargetMode.Format == mode.Format;
  }

  for (auto it = Modes.begin(); it != Modes.end();) {
    bool skipMode = it->Stereo != TargetMode.Stereo;

    if (testScanlineOrder)
      skipMode |= it->ScanlineOrdering != TargetMode.ScanlineOrdering;

    if (testScaling)
      skipMode |= it->Scaling != TargetMode.Scaling;

    if (testFormat)
      skipMode |= it->Format != TargetMode.Format;

    it = skipMode ? Modes.erase(it) : ++it;
  }

  // Filter by closest resolution
  uint32_t minDiffResolution = 0;

  if (TargetMode.Width) {
    minDiffResolution = std::accumulate(
        Modes.begin(), Modes.end(), std::numeric_limits<uint32_t>::max(),
        [&TargetMode](uint32_t current, const DXGI_MODE_DESC1 &mode) {
          uint32_t diff = std::abs(int32_t(TargetMode.Width - mode.Width)) +
                          std::abs(int32_t(TargetMode.Height - mode.Height));
          return std::min(current, diff);
        });

    for (auto it = Modes.begin(); it != Modes.end();) {
      uint32_t diff = std::abs(int32_t(TargetMode.Width - it->Width)) +
                      std::abs(int32_t(TargetMode.Height - it->Height));

      bool skipMode = diff != minDiffResolution;
      it = skipMode ? Modes.erase(it) : ++it;
    }
  }

  // Filter by closest refresh rate
  uint32_t minDiffRefreshRate = 0;

  if (TargetMode.RefreshRate.Numerator && TargetMode.RefreshRate.Denominator) {
    minDiffRefreshRate = std::accumulate(
        Modes.begin(), Modes.end(), std::numeric_limits<uint64_t>::max(),
        [&TargetMode](uint64_t current, const DXGI_MODE_DESC1 &mode) {
          uint64_t rate = uint64_t(mode.RefreshRate.Numerator) *
                          uint64_t(TargetMode.RefreshRate.Denominator) /
                          uint64_t(mode.RefreshRate.Denominator);
          uint64_t diff = std::abs(
              int64_t(rate - uint64_t(TargetMode.RefreshRate.Numerator)));
          return std::min(current, diff);
        });

    for (auto it = Modes.begin(); it != Modes.end();) {
      uint64_t rate = uint64_t(it->RefreshRate.Numerator) *
                      uint64_t(TargetMode.RefreshRate.Denominator) /
                      uint64_t(it->RefreshRate.Denominator);
      uint64_t diff =
          std::abs(int64_t(rate - uint64_t(TargetMode.RefreshRate.Numerator)));

      bool skipMode = diff != minDiffRefreshRate;
      it = skipMode ? Modes.erase(it) : ++it;
    }
  }
}

class MTLDXGIOutputImpl : public MTLDXGIOutput {
public:
  MTLDXGIOutputImpl(IMTLDXGIAdapter *adapter, HMONITOR monitor, DxgiOptions &options)
      : adapter_(adapter), monitor_(monitor), options_(options) {
    WMTGetDisplayDescription(monitor_ == wsi::getDefaultMonitor()
                                 ? WMTGetPrimaryDisplayId()
                                 : WMTGetSecondaryDisplayId(),
                             &native_desc_);
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_ramp_.red[i] = gamma_ramp_.green[i] = gamma_ramp_.blue[i] =
          float(i) / float(DXMT_GAMMA_CP_COUNT - 1);
    }
    gamma_ramp_.version = 0;
  }

  ~MTLDXGIOutputImpl() {}

  const DXMTGammaRamp *
  STDMETHODCALLTYPE
  GetGammaRamp() override {
    return &gamma_ramp_;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {

    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIOutput) || riid == __uuidof(IDXGIOutput1) ||
        riid == __uuidof(IDXGIOutput2) || riid == __uuidof(IDXGIOutput3) ||
        riid == __uuidof(IDXGIOutput4) || riid == __uuidof(IDXGIOutput5) ||
        riid == __uuidof(IDXGIOutput6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIOutput), riid)) {
      WARN("DXGIOutput: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **ppParent) final {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_OUTPUT_DESC *pDesc) final {
    if (pDesc == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::getDesktopCoordinates(monitor_, &pDesc->DesktopCoordinates)) {
      ERR("Failed to query monitor coords");
      return E_FAIL;
    }

    if (!wsi::getDisplayName(monitor_, pDesc->DeviceName)) {
      ERR("Failed to query monitor name");
      return E_FAIL;
    }

    pDesc->AttachedToDesktop = 1;
    pDesc->Rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    pDesc->Monitor = monitor_;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes,
                     DXGI_MODE_DESC *pDesc) final {
    if (pNumModes == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    std::vector<DXGI_MODE_DESC1> modes;

    if (pDesc)
      modes.resize(std::max(1u, *pNumModes));

    HRESULT hr = GetDisplayModeList1(EnumFormat, Flags, pNumModes,
                                     pDesc ? modes.data() : nullptr);

    if (pDesc) {
      for (uint32_t i = 0; i < *pNumModes && i < modes.size(); i++) {
        pDesc[i].Width = modes[i].Width;
        pDesc[i].Height = modes[i].Height;
        pDesc[i].RefreshRate = modes[i].RefreshRate;
        pDesc[i].Format = modes[i].Format;
        pDesc[i].ScanlineOrdering = modes[i].ScanlineOrdering;
        pDesc[i].Scaling = modes[i].Scaling;
      }
    }

    return hr;
  }

  HRESULT
  STDMETHODCALLTYPE
  FindClosestMatchingMode(const DXGI_MODE_DESC *pModeToMatch,
                          DXGI_MODE_DESC *pClosestMatch,
                          IUnknown *pConcernedDevice) final {
    if (!pModeToMatch || !pClosestMatch)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_MODE_DESC1 modeToMatch;
    modeToMatch.Width = pModeToMatch->Width;
    modeToMatch.Height = pModeToMatch->Height;
    modeToMatch.RefreshRate = pModeToMatch->RefreshRate;
    modeToMatch.Format = pModeToMatch->Format;
    modeToMatch.ScanlineOrdering = pModeToMatch->ScanlineOrdering;
    modeToMatch.Scaling = pModeToMatch->Scaling;
    modeToMatch.Stereo = FALSE;

    DXGI_MODE_DESC1 closestMatch = {};

    HRESULT hr =
        FindClosestMatchingMode1(&modeToMatch, &closestMatch, pConcernedDevice);

    if (FAILED(hr))
      return hr;

    pClosestMatch->Width = closestMatch.Width;
    pClosestMatch->Height = closestMatch.Height;
    pClosestMatch->RefreshRate = closestMatch.RefreshRate;
    pClosestMatch->Format = closestMatch.Format;
    pClosestMatch->ScanlineOrdering = closestMatch.ScanlineOrdering;
    pClosestMatch->Scaling = closestMatch.Scaling;
    return hr;
  }

  HRESULT
  STDMETHODCALLTYPE
  WaitForVBlank() final {
    const uint32_t display_id =
        monitor_ == wsi::getDefaultMonitor() ? WMTGetPrimaryDisplayId()
                                              : WMTGetSecondaryDisplayId();
    if (!display_id || !WMTWaitForVBlank(display_id, 2000)) {
      ERR("WaitForVBlank: CoreVideo display-link wait failed display=",
          display_id);
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  TakeOwnership(IUnknown *device, BOOL exclusive) final {
    if (!device)
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard lock(surface_mutex_);
    if (exclusive && ownership_taken_)
      return DXGI_ERROR_ACCESS_DENIED;
    ownership_taken_ = true;
    exclusive_ = exclusive != FALSE;
    return S_OK;
  }

  void STDMETHODCALLTYPE ReleaseOwnership() final {
    std::lock_guard lock(surface_mutex_);
    ownership_taken_ = false;
    exclusive_ = false;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetGammaControlCapabilities(
      DXGI_GAMMA_CONTROL_CAPABILITIES *gamma_caps) final {
    if (gamma_caps == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    gamma_caps->ScaleAndOffsetSupported = false;
    gamma_caps->MaxConvertedValue = 1.0f;
    gamma_caps->MinConvertedValue = 0.0f;
    gamma_caps->NumGammaControlPoints = DXMT_GAMMA_CP_COUNT;
    for (uint32_t i = 0; i < gamma_caps->NumGammaControlPoints; i++)
      gamma_caps->ControlPointPositions[i] = float(i) / float(DXMT_GAMMA_CP_COUNT - 1);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  SetGammaControl(const DXGI_GAMMA_CONTROL *gamma_control) final {
    if (gamma_control == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_ramp_.red[i] = gamma_control->GammaCurve[i].Red;
      gamma_ramp_.green[i] = gamma_control->GammaCurve[i].Green;
      gamma_ramp_.blue[i] = gamma_control->GammaCurve[i].Blue;
    }
    gamma_ramp_.version++;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetGammaControl(DXGI_GAMMA_CONTROL *gamma_control) final {
    if (gamma_control == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    gamma_control->Scale = { 1.0f, 1.0f, 1.0f };
    gamma_control->Offset = { 0.0f, 0.0f, 0.0f };
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_control->GammaCurve[i].Red = gamma_ramp_.red[i];
      gamma_control->GammaCurve[i].Green = gamma_ramp_.green[i];
      gamma_control->GammaCurve[i].Blue = gamma_ramp_.blue[i];
    }
  
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  SetDisplaySurface(IDXGISurface *surface) final {
    if (!surface)
      return DXGI_ERROR_INVALID_CALL;
    DXGI_SURFACE_DESC desc = {};
    HRESULT hr = surface->GetDesc(&desc);
    if (FAILED(hr) || !desc.Width || !desc.Height ||
        desc.SampleDesc.Count != 1)
      return FAILED(hr) ? hr : DXGI_ERROR_INVALID_CALL;
    std::lock_guard lock(surface_mutex_);
    display_surface_ = surface;
    display_surface_desc_ = desc;
    ++frame_statistics_.PresentCount;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplaySurfaceData(IDXGISurface *surface) final {
    if (!surface)
      return DXGI_ERROR_INVALID_CALL;
    Com<IDXGISurface> source;
    DXGI_SURFACE_DESC source_desc = {};
    {
      std::lock_guard lock(surface_mutex_);
      if (!display_surface_)
        return DXGI_ERROR_NOT_FOUND;
      source = display_surface_;
      source_desc = display_surface_desc_;
    }
    DXGI_SURFACE_DESC destination_desc = {};
    HRESULT hr = surface->GetDesc(&destination_desc);
    if (FAILED(hr))
      return hr;
    if (destination_desc.Width != source_desc.Width ||
        destination_desc.Height != source_desc.Height ||
        destination_desc.Format != source_desc.Format ||
        destination_desc.SampleDesc.Count != source_desc.SampleDesc.Count)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_MAPPED_RECT source_map = {};
    hr = source->Map(&source_map, DXGI_MAP_READ);
    if (FAILED(hr))
      return hr;
    const UINT copy_pitch = static_cast<UINT>(std::max(source_map.Pitch, 0));
    if (!source_map.pBits || !copy_pitch) {
      source->Unmap();
      return DXGI_ERROR_INVALID_CALL;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(copy_pitch) *
                                source_desc.Height);
    for (UINT y = 0; y < source_desc.Height; ++y)
      std::memcpy(pixels.data() + size_t(y) * copy_pitch,
                  source_map.pBits + size_t(y) * source_map.Pitch,
                  copy_pitch);
    source->Unmap();

    DXGI_MAPPED_RECT destination_map = {};
    hr = surface->Map(&destination_map, DXGI_MAP_WRITE | DXGI_MAP_DISCARD);
    if (FAILED(hr))
      return hr;
    if (!destination_map.pBits || destination_map.Pitch < 0 ||
        static_cast<UINT>(destination_map.Pitch) < copy_pitch) {
      surface->Unmap();
      return DXGI_ERROR_INVALID_CALL;
    }
    for (UINT y = 0; y < source_desc.Height; ++y)
      std::memcpy(destination_map.pBits + size_t(y) * destination_map.Pitch,
                  pixels.data() + size_t(y) * copy_pitch, copy_pitch);
    return surface->Unmap();
  }

  HRESULT
  STDMETHODCALLTYPE
  GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) final {
    if (!stats)
      return DXGI_ERROR_INVALID_CALL;
    std::lock_guard lock(surface_mutex_);
    *stats = frame_statistics_;
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    stats->SyncQPCTime = qpc;
    stats->SyncGPUTime.QuadPart = 0;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplayModeList1(DXGI_FORMAT EnumFormat, UINT Flags, UINT *pNumModes,
                      DXGI_MODE_DESC1 *pDesc) final {
    if (pNumModes == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    // Special case, just return zero modes
    if (EnumFormat == DXGI_FORMAT_UNKNOWN) {
      *pNumModes = 0;
      return S_OK;
    }
    MTL_DXGI_FORMAT_DESC formatDesc;
    if (FAILED(MTLQueryDXGIFormat(this->adapter_->GetMTLDevice(), EnumFormat, formatDesc))
      || !(formatDesc.Flag & MTL_DXGI_FORMAT_BACKBUFFER)) {
      *pNumModes = 0;
      return S_OK;
    }

    // Walk over all modes that the display supports and
    // return those that match the requested format etc.
    wsi::WsiMode devMode = {};

    uint32_t srcModeId = 0;
    uint32_t dstModeId = 0;

    std::vector<DXGI_MODE_DESC1> modeList;

    while (wsi::getDisplayMode(monitor_, srcModeId++, &devMode)) {
      // Only enumerate interlaced modes if requested.
      if (devMode.interlaced && !(Flags & DXGI_ENUM_MODES_INTERLACED))
        continue;

      // Skip modes with incompatible formats
      if (devMode.bitsPerPixel != GetMonitorFormatBpp(EnumFormat))
        continue;

      if (pDesc != nullptr) {
        DXGI_MODE_DESC1 mode = ConvertDisplayMode(devMode);
        // Fix up the DXGI_FORMAT to match what we were enumerating.
        mode.Format = EnumFormat;

        modeList.push_back(mode);
      }

      dstModeId += 1;
    }

    // Sort display modes by width, height and refresh rate,
    // in that order. Some games rely on correct ordering.
    std::sort(modeList.begin(), modeList.end(),
              [](const DXGI_MODE_DESC1 &a, const DXGI_MODE_DESC1 &b) {
                if (a.Width < b.Width)
                  return true;
                if (a.Width > b.Width)
                  return false;

                if (a.Height < b.Height)
                  return true;
                if (a.Height > b.Height)
                  return false;

                return (a.RefreshRate.Numerator / a.RefreshRate.Denominator) <
                       (b.RefreshRate.Numerator / b.RefreshRate.Denominator);
              });

    // If requested, write out the first set of display
    // modes to the destination array.
    if (pDesc != nullptr) {
      for (uint32_t i = 0; i < *pNumModes && i < dstModeId; i++)
        pDesc[i] = modeList[i];

      if (dstModeId > *pNumModes)
        return DXGI_ERROR_MORE_DATA;
    }

    *pNumModes = dstModeId;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  FindClosestMatchingMode1(const DXGI_MODE_DESC1 *pModeToMatch,
                           DXGI_MODE_DESC1 *pClosestMatch,
                           IUnknown *pConcernedDevice) final {
    if (!pModeToMatch || !pClosestMatch)
      return DXGI_ERROR_INVALID_CALL;

    if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN && !pConcernedDevice)
      return DXGI_ERROR_INVALID_CALL;

    // Both or neither must be zero
    if ((pModeToMatch->Width == 0) ^ (pModeToMatch->Height == 0))
      return DXGI_ERROR_INVALID_CALL;

    wsi::WsiMode activeWsiMode = {};
    wsi::getCurrentDisplayMode(monitor_, &activeWsiMode);

    DXGI_MODE_DESC1 activeMode = ConvertDisplayMode(activeWsiMode);

    DXGI_MODE_DESC1 defaultMode;
    defaultMode.Width = 0;
    defaultMode.Height = 0;
    defaultMode.RefreshRate = {0, 0};
    defaultMode.Format = DXGI_FORMAT_UNKNOWN;
    defaultMode.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    defaultMode.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    defaultMode.Stereo = pModeToMatch->Stereo;

    if (pModeToMatch->ScanlineOrdering == DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED)
      defaultMode.ScanlineOrdering = activeMode.ScanlineOrdering;

    if (pModeToMatch->Scaling == DXGI_MODE_SCALING_UNSPECIFIED)
      defaultMode.Scaling = activeMode.Scaling;

    DXGI_FORMAT targetFormat = pModeToMatch->Format;

    if (pModeToMatch->Format == DXGI_FORMAT_UNKNOWN) {
      defaultMode.Format = activeMode.Format;
      targetFormat = activeMode.Format;
    }

    if (!pModeToMatch->Width) {
      defaultMode.Width = activeMode.Width;
      defaultMode.Height = activeMode.Height;
    }

    if (!pModeToMatch->RefreshRate.Numerator ||
        !pModeToMatch->RefreshRate.Denominator) {
      defaultMode.RefreshRate.Numerator = activeMode.RefreshRate.Numerator;
      defaultMode.RefreshRate.Denominator = activeMode.RefreshRate.Denominator;
    }

    UINT modeCount = 0;
    GetDisplayModeList1(targetFormat, DXGI_ENUM_MODES_SCALING, &modeCount,
                        nullptr);

    if (modeCount == 0) {
      ERR("No modes found");
      return DXGI_ERROR_NOT_FOUND;
    }

    std::vector<DXGI_MODE_DESC1> modes(modeCount);
    GetDisplayModeList1(targetFormat, DXGI_ENUM_MODES_SCALING, &modeCount,
                        modes.data());

    FilterModesByDesc(modes, *pModeToMatch);
    FilterModesByDesc(modes, defaultMode);

    if (modes.empty())
      return DXGI_ERROR_NOT_FOUND;

    *pClosestMatch = modes[0];

    Logger::debug(str::format("DXGI: For mode ", pModeToMatch->Width, "x",
                              pModeToMatch->Height, "@",
                              pModeToMatch->RefreshRate.Denominator
                                  ? (pModeToMatch->RefreshRate.Numerator /
                                     pModeToMatch->RefreshRate.Denominator)
                                  : 0,
                              " found closest mode ", pClosestMatch->Width, "x",
                              pClosestMatch->Height, "@",
                              pClosestMatch->RefreshRate.Denominator
                                  ? (pClosestMatch->RefreshRate.Numerator /
                                     pClosestMatch->RefreshRate.Denominator)
                                  : 0));
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDisplaySurfaceData1(IDXGIResource *destination) final {
    if (!destination)
      return DXGI_ERROR_INVALID_CALL;
    Com<IDXGISurface> source;
    DXGI_SURFACE_DESC source_desc = {};
    {
      std::lock_guard lock(surface_mutex_);
      if (!display_surface_)
        return DXGI_ERROR_NOT_FOUND;
      source = display_surface_;
      source_desc = display_surface_desc_;
    }
    ID3D12Resource *d3d_destination = nullptr;
    if (FAILED(destination->QueryInterface(
            kIID_ID3D12ResourceForSurface,
            reinterpret_cast<void **>(&d3d_destination))))
      return DXGI_ERROR_UNSUPPORTED;
    D3D12_RESOURCE_DESC destination_desc = {};
    d3d_destination->GetDesc(&destination_desc);
    if (destination_desc.Width != source_desc.Width ||
        destination_desc.Height != source_desc.Height ||
        destination_desc.Format != source_desc.Format ||
        destination_desc.SampleDesc.Count != source_desc.SampleDesc.Count) {
      d3d_destination->Release();
      return DXGI_ERROR_INVALID_CALL;
    }
    DXGI_MAPPED_RECT source_map = {};
    HRESULT hr = source->Map(&source_map, DXGI_MAP_READ);
    if (FAILED(hr) || !source_map.pBits || source_map.Pitch <= 0) {
      d3d_destination->Release();
      return FAILED(hr) ? hr : DXGI_ERROR_INVALID_CALL;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(source_map.Pitch) *
                                source_desc.Height);
    for (UINT y = 0; y < source_desc.Height; ++y)
      std::memcpy(pixels.data() + size_t(y) * source_map.Pitch,
                  source_map.pBits + size_t(y) * source_map.Pitch,
                  static_cast<size_t>(source_map.Pitch));
    source->Unmap();
    hr = d3d_destination->WriteToSubresource(
        0, nullptr, pixels.data(), static_cast<UINT>(source_map.Pitch),
        static_cast<UINT>(pixels.size()));
    d3d_destination->Release();
    return hr;
  }

  HRESULT
  STDMETHODCALLTYPE
  DuplicateOutput(IUnknown *pDevice,
                  IDXGIOutputDuplication **ppOutputDuplication) final {
    InitReturnPtr(ppOutputDuplication);
    if (!pDevice)
      return E_INVALIDARG;
    return CreateOutputDuplication(this, pDevice, ppOutputDuplication);
  }

  HRESULT
  STDMETHODCALLTYPE
  CheckOverlaySupport(DXGI_FORMAT enum_format,
                              IUnknown *concerned_device,
                              UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  WINBOOL STDMETHODCALLTYPE SupportsOverlays() override { return FALSE; }

  HRESULT STDMETHODCALLTYPE CheckOverlayColorSpaceSupport(DXGI_FORMAT format,
                                        DXGI_COLOR_SPACE_TYPE colour_space,
                                        IUnknown *device,
                                        UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_OUTPUT_DESC1 *pDesc) override {
    if (pDesc == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    if (!wsi::getDesktopCoordinates(monitor_, &pDesc->DesktopCoordinates)) {
      ERR("Failed to query monitor coords");
      return E_FAIL;
    }

    if (!wsi::getDisplayName(monitor_, pDesc->DeviceName)) {
      ERR("Failed to query monitor name");
      return E_FAIL;
    }

    pDesc->AttachedToDesktop = 1;
    pDesc->Rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    pDesc->Monitor = monitor_;
    if (options_.forceSDR) {
      pDesc->BitsPerColor = 8;
      pDesc->ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    } else {
      pDesc->BitsPerColor =
          native_desc_.maximum_potential_edr_color_component_value > 1 ? 10 : 8;
      pDesc->ColorSpace =
          native_desc_.maximum_potential_edr_color_component_value > 1
              ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
              : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    memcpy(pDesc->RedPrimary, native_desc_.red_primaries, 8);
    memcpy(pDesc->GreenPrimary, native_desc_.green_primaries, 8);
    memcpy(pDesc->BluePrimary, native_desc_.blue_primaries, 8);
    memcpy(pDesc->WhitePoint, native_desc_.white_points, 8);
    pDesc->MinLuminance = 0.0f;
    pDesc->MaxLuminance = native_desc_.maximum_potential_edr_color_component_value * 100.0f;
    pDesc->MaxFullFrameLuminance = pDesc->MaxLuminance;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  CheckHardwareCompositionSupport(UINT *flags) override {
    if (flags) {
      *flags = 0;
    }
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  DuplicateOutput1(IUnknown *pDevice, UINT flags, UINT format_count,
                   const DXGI_FORMAT *formats,
                   IDXGIOutputDuplication **ppOutputDuplication) override {
    InitReturnPtr(ppOutputDuplication);
    if (!pDevice || flags != 0 || (format_count && !formats) ||
        format_count > 16)
      return E_INVALIDARG;
    return CreateOutputDuplication(this, pDevice, ppOutputDuplication,
                                   format_count, formats);
  }

private:
  friend class MTLDXGIOutputDuplication;
  Com<IMTLDXGIAdapter> adapter_ = nullptr;
  HMONITOR monitor_ = nullptr;
  WMTDisplayDescription native_desc_;
  DxgiOptions &options_;
  DXMTGammaRamp gamma_ramp_;
  std::mutex surface_mutex_;
  Com<IDXGISurface> display_surface_;
  DXGI_SURFACE_DESC display_surface_desc_ = {};
  DXGI_FRAME_STATISTICS frame_statistics_ = {};
  bool ownership_taken_ = false;
  bool exclusive_ = false;
};

class MTLDXGIOutputDuplication final : public IDXGIOutputDuplication {
public:
  MTLDXGIOutputDuplication(MTLDXGIOutputImpl *output, IUnknown *device,
                           const DXGI_OUTPUT_DESC &output_desc,
                           const DXGI_FORMAT *formats, UINT format_count)
      : output_(output), device_(device), output_desc_(output_desc) {
    if (output_)
      output_->AddRef();
    if (device_)
      device_->AddRef();
    desc_ = {};
    const LONG width = std::max<LONG>(
        1, output_desc.DesktopCoordinates.right -
               output_desc.DesktopCoordinates.left);
    const LONG height = std::max<LONG>(
        1, output_desc.DesktopCoordinates.bottom -
               output_desc.DesktopCoordinates.top);
    desc_.ModeDesc.Width = static_cast<UINT>(width);
    desc_.ModeDesc.Height = static_cast<UINT>(height);
    desc_.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc_.ModeDesc.RefreshRate = {60, 1};
    desc_.ModeDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    desc_.ModeDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc_.Rotation = output_desc.Rotation;
    desc_.DesktopImageInSystemMemory = FALSE;
    if (format_count && formats &&
        (formats[0] == DXGI_FORMAT_B8G8R8A8_UNORM ||
         formats[0] == DXGI_FORMAT_R8G8B8A8_UNORM))
      format_ = formats[0];
    desc_.ModeDesc.Format = format_;
  }

  ~MTLDXGIOutputDuplication() {
    if (desktop_resource_)
      desktop_resource_->Release();
    if (device_)
      device_->Release();
    if (output_)
      output_->Release();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_IDXGIObject ||
        riid == IID_IDXGIOutputDuplication) {
      *object = static_cast<IDXGIOutputDuplication *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                           const void *data) override {
    return private_data_.setData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                     const IUnknown *object) override {
    return private_data_.setInterface(guid, object);
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                           void *data) override {
    return private_data_.getData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {
    if (!parent)
      return E_POINTER;
    *parent = nullptr;
    return output_ ? output_->QueryInterface(riid, parent) : E_NOINTERFACE;
  }
  void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC *desc) override {
    if (desc)
      *desc = desc_;
  }
  HRESULT STDMETHODCALLTYPE AcquireNextFrame(
      UINT timeout, DXGI_OUTDUPL_FRAME_INFO *frame_info,
      IDXGIResource **desktop_resource) override {
    if (!frame_info || !desktop_resource)
      return DXGI_ERROR_INVALID_CALL;
    *frame_info = {};
    *desktop_resource = nullptr;
    if (held_)
      return DXGI_ERROR_INVALID_CALL;
    if (!EnsureDesktopResource())
      return DXGI_ERROR_UNSUPPORTED;
    held_ = true;
    ++frame_id_;
    frame_info->LastPresentTime.QuadPart = static_cast<LONGLONG>(frame_id_);
    frame_info->LastMouseUpdateTime.QuadPart =
        static_cast<LONGLONG>(frame_id_);
    frame_info->AccumulatedFrames = 1;
    frame_info->RectsCoalesced = TRUE;
    frame_info->PointerPosition.Visible = FALSE;
    *desktop_resource = desktop_resource_;
    desktop_resource_->AddRef();
    (void)timeout;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT size, RECT *rects,
                                                UINT *required) override {
    if (!required || !held_)
      return DXGI_ERROR_INVALID_CALL;
    *required = sizeof(RECT);
    if (!rects || size < sizeof(RECT))
      return DXGI_ERROR_MORE_DATA;
    rects[0] = output_desc_.DesktopCoordinates;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT size,
                                               DXGI_OUTDUPL_MOVE_RECT *rects,
                                               UINT *required) override {
    if (!required || !held_)
      return DXGI_ERROR_INVALID_CALL;
    *required = 0;
    if (size && !rects)
      return DXGI_ERROR_INVALID_CALL;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFramePointerShape(
      UINT size, void *shape, UINT *required,
      DXGI_OUTDUPL_POINTER_SHAPE_INFO *info) override {
    if (!required || !info || !held_)
      return DXGI_ERROR_INVALID_CALL;
    *required = 0;
    *info = {};
    if (size && !shape)
      return DXGI_ERROR_INVALID_CALL;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT *locked) override {
    if (!locked || !held_)
      return DXGI_ERROR_INVALID_CALL;
    locked->Pitch = 0;
    locked->pBits = nullptr;
    return DXGI_ERROR_UNSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override {
    return held_ ? S_OK : DXGI_ERROR_INVALID_CALL;
  }
  HRESULT STDMETHODCALLTYPE ReleaseFrame() override {
    if (!held_)
      return DXGI_ERROR_INVALID_CALL;
    held_ = false;
    return S_OK;
  }

private:
  bool EnsureDesktopResource() {
    if (desktop_resource_)
      return true;
    if (!device_)
      return false;
    ID3D12Device *device12 = nullptr;
    HRESULT hr = device_->QueryInterface(
        kIID_ID3D12DeviceForDuplication,
        reinterpret_cast<void **>(&device12));
    if (FAILED(hr) || !device12)
      return false;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = desc_.ModeDesc.Width;
    resource_desc.Height = desc_.ModeDesc.Height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = format_;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource *resource = nullptr;
    hr = device12->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        kIID_ID3D12ResourceForSurface, reinterpret_cast<void **>(&resource));
    device12->Release();
    if (FAILED(hr) || !resource)
      return false;
    if (output_) {
      Com<IDXGISurface> source_surface;
      DXGI_SURFACE_DESC source_desc = {};
      {
        std::lock_guard lock(output_->surface_mutex_);
        source_surface = output_->display_surface_;
        source_desc = output_->display_surface_desc_;
      }
      if (source_surface && source_desc.Width == desc_.ModeDesc.Width &&
          source_desc.Height == desc_.ModeDesc.Height &&
          source_desc.Format == format_ && source_desc.SampleDesc.Count == 1) {
        DXGI_MAPPED_RECT mapped = {};
        hr = source_surface->Map(&mapped, DXGI_MAP_READ);
        if (FAILED(hr) || !mapped.pBits || mapped.Pitch <= 0) {
          resource->Release();
          return false;
        }
        const uint64_t required = uint64_t(mapped.Pitch) * source_desc.Height;
        if (required > UINT32_MAX) {
          source_surface->Unmap();
          resource->Release();
          return false;
        }
        std::vector<uint8_t> pixels;
        try {
          pixels.resize(static_cast<size_t>(required));
        } catch (const std::bad_alloc &) {
          source_surface->Unmap();
          resource->Release();
          return false;
        }
        for (UINT y = 0; y < source_desc.Height; ++y)
          std::memcpy(pixels.data() + size_t(y) * mapped.Pitch,
                      mapped.pBits + size_t(y) * mapped.Pitch,
                      static_cast<size_t>(mapped.Pitch));
        source_surface->Unmap();
        hr = resource->WriteToSubresource(
            0, nullptr, pixels.data(), static_cast<UINT>(mapped.Pitch),
            static_cast<UINT>(required));
        if (FAILED(hr)) {
          resource->Release();
          return false;
        }
      }
    }
    hr = resource->QueryInterface(
        IID_IDXGIResource, reinterpret_cast<void **>(&desktop_resource_));
    resource->Release();
    return SUCCEEDED(hr) && desktop_resource_;
  }

  std::atomic<ULONG> ref_count_ = {1};
  MTLDXGIOutputImpl *output_ = nullptr;
  IUnknown *device_ = nullptr;
  IDXGIResource *desktop_resource_ = nullptr;
  DXGI_OUTPUT_DESC output_desc_ = {};
  DXGI_OUTDUPL_DESC desc_ = {};
  DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
  uint64_t frame_id_ = 0;
  bool held_ = false;
  ComPrivateData private_data_;
};

static HRESULT CreateOutputDuplication(MTLDXGIOutputImpl *output,
                                       IUnknown *device,
                                       IDXGIOutputDuplication **duplication,
                                       UINT format_count,
                                       const DXGI_FORMAT *formats) {
  if (!duplication)
    return E_POINTER;
  *duplication = nullptr;
  if (!output || !device)
    return E_INVALIDARG;
  for (UINT i = 0; i < format_count; ++i) {
    if (formats[i] != DXGI_FORMAT_B8G8R8A8_UNORM &&
        formats[i] != DXGI_FORMAT_R8G8B8A8_UNORM)
      return DXGI_ERROR_UNSUPPORTED;
  }
  DXGI_OUTPUT_DESC output_desc = {};
  HRESULT hr = output->GetDesc(&output_desc);
  if (FAILED(hr))
    return hr;
  auto *created = new (std::nothrow)
      MTLDXGIOutputDuplication(output, device, output_desc, formats,
                               format_count);
  if (!created)
    return E_OUTOFMEMORY;
  *duplication = created;
  return S_OK;
}

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options) {
  return Com<IDXGIOutput>::transfer(new MTLDXGIOutputImpl(pAadapter, monitor, options));
};

} // namespace dxmt