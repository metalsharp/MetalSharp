#include "d3d12_swapchain.hpp"
#include "d3d12_device.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "Metal.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#define SCTRACE(fmt, ...) DXMTD3D12Trace("SwapChain", fmt, ##__VA_ARGS__)

static uint64_t g_sc_enc_id = 0;

namespace dxmt {

static uint64_t PresentLogInterval() {
  static uint64_t interval = [] {
    const char *raw = std::getenv("DXMT_D3D12_PRESENT_LOG_INTERVAL");
    if (!raw || !raw[0])
      return 120ull;

    char *end = nullptr;
    auto parsed = std::strtoull(raw, &end, 10);
    if (end == raw || parsed == 0)
      return 120ull;
    return parsed;
  }();
  return interval;
}

static bool LivePresentEnabled() {
  static bool enabled = [] {
    const char *raw = std::getenv("DXMT_D3D12_LIVE_PRESENT");
    return raw && raw[0] && raw[0] != '0';
  }();
  return enabled;
}

static bool WindowHandoffEnabled() {
  static bool enabled = [] {
    const char *raw = std::getenv("DXMT_D3D12_REASSERT_WINDOW_HANDOFF");
    return raw && raw[0] && raw[0] != '0';
  }();
  return enabled;
}

static WMTPixelFormat DXGIToMTL(DXGI_FORMAT fmt) {
  switch (fmt) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatRGBA8Unorm;
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatRGBA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  default:
    return WMTPixelFormatBGRA8Unorm;
  }
}

static WMTPixelFormat DXGIToDisplayLayerMTL(DXGI_FORMAT fmt) {
  switch (fmt) {
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  default:
    return DXGIToMTL(fmt);
  }
}

static bool CanUseRawSwapchainBlit(DXGI_FORMAT fmt) {
  switch (fmt) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return true;
  default:
    return false;
  }
}

static bool IsSupportedColorSpaceForFormat(DXGI_FORMAT fmt,
                                           DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return fmt == DXGI_FORMAT_R10G10B10A2_UNORM;
  default:
    return false;
  }
}

struct SwapchainReadbackProbe {
  WMT::Reference<WMT::Buffer> buffer;
  void *mapped = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytes_per_row = 0;
  uint64_t present_count = 0;
};

static bool SwapchainReadbackEnabled() {
  static bool enabled = [] {
    const char *raw = std::getenv("DXMT_D3D12_SWAPCHAIN_READBACK");
    return raw && raw[0] && raw[0] != '0';
  }();
  return enabled;
}

static uint64_t SwapchainReadbackInterval() {
  static uint64_t interval = [] {
    const char *raw = std::getenv("DXMT_D3D12_SWAPCHAIN_READBACK_INTERVAL");
    if (!raw || !raw[0])
      return 30ull;

    char *end = nullptr;
    auto parsed = std::strtoull(raw, &end, 10);
    if (end == raw || parsed == 0)
      return 30ull;
    return parsed;
  }();
  return interval;
}

static bool ShouldReadbackPresent(uint64_t present_count) {
  if (!SwapchainReadbackEnabled())
    return false;
  return present_count <= 12 ||
         (present_count % SwapchainReadbackInterval()) == 0;
}

static const char *
SwapchainWorkClassification(const D3D12SwapchainBackbufferWork &work) {
  if (!work.serial)
    return "no_queue_work";
  if (work.draw_count || work.indexed_draw_count || work.indirect_count)
    return "drawn";
  if (work.clear_rtv_count && work.dispatch_count)
    return "compute_clear_no_draw";
  if (work.clear_rtv_count)
    return "clear_only_no_draw";
  if (work.dispatch_count)
    return "compute_only_no_draw";
  if (work.graphics_setup)
    return "graphics_setup_no_draw";
  return "no_draw";
}

static uint32_t AlignTo(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static SwapchainReadbackProbe
EncodeSwapchainReadback(WMT::Device device, WMT::CommandBuffer cmdbuf,
                        WMT::Texture texture, UINT width, UINT height,
                        DXGI_FORMAT format, uint64_t present_count) {
  SwapchainReadbackProbe probe = {};
  if (!texture.handle || !width || !height ||
      !ShouldReadbackPresent(present_count))
    return probe;

  probe.width = std::min<uint32_t>(width, 1920u);
  probe.height = std::min<uint32_t>(height, 1080u);
  probe.bytes_per_row = AlignTo(probe.width * 4u, 256u);
  probe.present_count = present_count;

  WMTBufferInfo info = {};
  info.length = uint64_t(probe.bytes_per_row) * probe.height;
  info.options =
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeTracked;
  info.memory.set(nullptr);
  probe.buffer = device.newBuffer(info);
  probe.mapped = info.memory.get();
  if (!probe.buffer.handle || !probe.mapped) {
    Logger::info(str::format("M12 swapchain readback unavailable count=",
                             present_count, " fmt=", (unsigned)format));
    return {};
  }

  auto blit = cmdbuf.blitCommandEncoder();
  if (!blit.handle) {
    Logger::info(
        str::format("M12 swapchain readback skipped: no blit encoder count=",
                    present_count));
    return {};
  }

  struct wmtcmd_blit_copy_from_texture_to_buffer copy = {};
  copy.type = WMTBlitCommandCopyFromTextureToBuffer;
  copy.next.set(nullptr);
  copy.src = texture;
  copy.slice = 0;
  copy.level = 0;
  copy.origin = {0, 0, 0};
  copy.size = {probe.width, probe.height, 1};
  copy.dst = probe.buffer;
  copy.offset = 0;
  copy.bytes_per_row = probe.bytes_per_row;
  copy.bytes_per_image = probe.bytes_per_row * probe.height;
  blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
  blit.endEncoding();
  return probe;
}

static void LogSwapchainReadback(const SwapchainReadbackProbe &probe,
                                 DXGI_FORMAT format) {
  if (!probe.mapped || !probe.width || !probe.height)
    return;

  uint64_t nonzero_pixels = 0;
  uint64_t nonzero_bytes = 0;
  uint8_t max_byte = 0;
  uint64_t checksum = 1469598103934665603ull;
  auto *rows = static_cast<const uint8_t *>(probe.mapped);
  for (uint32_t y = 0; y < probe.height; y++) {
    auto *row = rows + uint64_t(y) * probe.bytes_per_row;
    for (uint32_t x = 0; x < probe.width; x++) {
      auto *px = row + x * 4u;
      bool pixel_nonzero = false;
      for (uint32_t i = 0; i < 4; i++) {
        auto value = px[i];
        if (value) {
          pixel_nonzero = true;
          nonzero_bytes++;
          max_byte = std::max(max_byte, value);
        }
        checksum ^= value;
        checksum *= 1099511628211ull;
      }
      if (pixel_nonzero)
        nonzero_pixels++;
    }
  }

  Logger::info(str::format(
      "M12 swapchain readback count=", probe.present_count,
      " fmt=", (unsigned)format, " sample=", probe.width, "x", probe.height,
      " nonzero_pixels=", nonzero_pixels, " nonzero_bytes=", nonzero_bytes,
      " max_byte=", (unsigned)max_byte, " checksum=0x", std::hex, checksum));
}

MTLD3D12SwapChain::MTLD3D12SwapChain(
    IDXGIFactory1 *factory, MTLD3D12Device *device, IMTLDXGIDevice *dxgi_device,
    HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc)
    : m_factory(factory), m_dxgi_device(dxgi_device), m_device(device),
      m_hwnd(hWnd), m_desc(*desc) {
  if (m_device)
    m_device->AddRef();

  if (fs_desc) {
    m_fs_desc = *fs_desc;
  } else {
    m_fs_desc = {};
    m_fs_desc.Windowed = true;
  }
  m_source_width = m_desc.Width;
  m_source_height = m_desc.Height;
  if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    m_frame_latency_event = CreateEventW(nullptr, FALSE, TRUE, nullptr);
  if (m_hwnd && IsWindow(m_hwnd)) {
    m_monitor = wsi::getWindowMonitor(m_hwnd);
    m_window_state.style =
        static_cast<LONG>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    m_window_state.exstyle =
        static_cast<LONG>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    GetWindowRect(m_hwnd, &m_window_state.rect);
  }

  auto wmt_dev = dxgi_device->GetMTLDevice();
  m_present_queue = wmt_dev.newCommandQueue(1);
  m_present_library = std::make_unique<InternalCommandLibrary>(wmt_dev);
  ReassertWindowForHandoff("create");
  EnsureMetalView();

  ResizeBuffers(0, m_desc.Width, m_desc.Height, m_desc.Format, m_desc.Flags);
  Logger::info(str::format("D3D12SwapChain: ", m_desc.Width, "x", m_desc.Height,
                           " fmt=", m_desc.Format, " hwnd=", (void *)hWnd));
}

MTLD3D12SwapChain::~MTLD3D12SwapChain() {
  for (uint32_t i = 0; i < 4; i++)
    m_backbuffers[i] = nullptr;
  if (m_frame_latency_event) {
    CloseHandle(m_frame_latency_event);
    m_frame_latency_event = nullptr;
  }
  if (m_native_view.handle)
    WMT::ReleaseMetalView(m_native_view);
  if (m_device)
    m_device->Release();
}

bool MTLD3D12SwapChain::EnsureMetalView() {
  if (m_layer.handle) {
    if (!m_presenter && m_present_library) {
      m_presenter = Rc(new Presenter(m_dxgi_device->GetMTLDevice(), m_layer,
                                     *m_present_library, 1.0f, 1));
    }
    return true;
  }

  RECT rect = {};
  WINBOOL has_rect = GetClientRect(m_hwnd, &rect);
  DWORD window_pid = 0;
  DWORD window_tid = GetWindowThreadProcessId(m_hwnd, &window_pid);
  SCTRACE("EnsureMetalView attempt hwnd=%p is_window=%d tid=%lu pid=%lu "
          "current_pid=%lu rect=%ld,%ld %ldx%ld",
          (void *)m_hwnd, (int)IsWindow(m_hwnd), (unsigned long)window_tid,
          (unsigned long)window_pid, (unsigned long)GetCurrentProcessId(),
          has_rect ? rect.left : 0, has_rect ? rect.top : 0,
          has_rect ? rect.right - rect.left : 0,
          has_rect ? rect.bottom - rect.top : 0);

  if (m_native_view.handle) {
    WMT::ReleaseMetalView(m_native_view);
    m_native_view = {};
  }
  m_layer = {};

  HWND candidates[5] = {
      m_hwnd,
      GetAncestor(m_hwnd, GA_ROOT),
      GetAncestor(m_hwnd, GA_ROOTOWNER),
      ::GetParent(m_hwnd),
      GetForegroundWindow(),
  };
  for (HWND candidate : candidates) {
    if (!candidate)
      continue;
    if (AttachMetalViewForHWND(candidate)) {
      if (candidate != m_hwnd) {
        SCTRACE("EnsureMetalView attached via fallback hwnd=%p original=%p",
                (void *)candidate, (void *)m_hwnd);
      }
      break;
    }
  }

  if (!m_layer.handle) {
    SCTRACE("EnsureMetalView FAILED hwnd=%p native_view=%llu layer=%llu",
            (void *)m_hwnd, (unsigned long long)m_native_view.handle,
            (unsigned long long)m_layer.handle);
    return false;
  }

  if (!m_present_library)
    m_present_library =
        std::make_unique<InternalCommandLibrary>(m_dxgi_device->GetMTLDevice());
  m_presenter = Rc(new Presenter(m_dxgi_device->GetMTLDevice(), m_layer,
                                 *m_present_library, 1.0f, 1));
  ConfigureLayer();
  return true;
}

bool MTLD3D12SwapChain::AttachMetalViewForHWND(HWND hwnd) {
  DWORD candidate_pid = 0;
  DWORD candidate_tid = GetWindowThreadProcessId(hwnd, &candidate_pid);
  RECT rect = {};
  WINBOOL has_rect = GetClientRect(hwnd, &rect);
  SCTRACE("AttachMetalView hwnd=%p is_window=%d tid=%lu pid=%lu rect=%ld,%ld "
          "%ldx%ld",
          (void *)hwnd, (int)IsWindow(hwnd), (unsigned long)candidate_tid,
          (unsigned long)candidate_pid, has_rect ? rect.left : 0,
          has_rect ? rect.top : 0, has_rect ? rect.right - rect.left : 0,
          has_rect ? rect.bottom - rect.top : 0);

  WMT::MetalLayer candidate_layer = {};
  auto candidate_view = WMT::CreateMetalViewFromHWND(
      (intptr_t)hwnd, m_dxgi_device->GetMTLDevice(), candidate_layer);
  SCTRACE("AttachMetalView result hwnd=%p native_view=%llu layer=%llu",
          (void *)hwnd, (unsigned long long)candidate_view.handle,
          (unsigned long long)candidate_layer.handle);
  if (!candidate_view.handle || !candidate_layer.handle)
    return false;

  m_native_view = candidate_view;
  m_layer = candidate_layer;
  return true;
}

void MTLD3D12SwapChain::ReassertWindowForHandoff(const char *reason) {
  if (!WindowHandoffEnabled())
    return;
  if (!m_hwnd || !IsWindow(m_hwnd)) {
    SCTRACE("ReassertWindowForHandoff skipped reason=%s hwnd=%p valid=%d",
            reason ? reason : "unknown", (void *)m_hwnd,
            m_hwnd ? (int)IsWindow(m_hwnd) : 0);
    return;
  }

  RECT client = {};
  RECT window = {};
  GetClientRect(m_hwnd, &client);
  GetWindowRect(m_hwnd, &window);

  UINT width = client.right > client.left
                   ? static_cast<UINT>(client.right - client.left)
                   : (m_desc.Width ? m_desc.Width : 1280u);
  UINT height = client.bottom > client.top
                    ? static_cast<UINT>(client.bottom - client.top)
                    : (m_desc.Height ? m_desc.Height : 720u);

  ShowWindow(m_hwnd, SW_SHOWNORMAL);
  if (client.right <= client.left || client.bottom <= client.top) {
    RECT target = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&target, style, FALSE, ex_style);
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOMOVE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  } else {
    SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  }

  SetForegroundWindow(m_hwnd);
  SCTRACE("ReassertWindowForHandoff reason=%s hwnd=%p client=%ldx%ld "
          "window=%ld,%ld %ldx%ld fs_windowed=%u",
          reason ? reason : "unknown", (void *)m_hwnd,
          client.right - client.left, client.bottom - client.top, window.left,
          window.top, window.right - window.left, window.bottom - window.top,
          m_fs_desc.Windowed ? 1 : 0);
  Logger::info(str::format(
      "M12 reassert window handoff reason=", reason ? reason : "unknown",
      " hwnd=", (void *)m_hwnd, " client=", width, "x", height,
      " windowed=", m_fs_desc.Windowed ? 1 : 0));
}

void MTLD3D12SwapChain::ConfigureLayer() {
  if (!m_layer.handle)
    return;

  auto source_format = DXGIToMTL(m_desc.Format);
  auto layer_format = DXGIToDisplayLayerMTL(m_desc.Format);
  auto width =
      m_source_width ? m_source_width : (m_desc.Width ? m_desc.Width : 1);
  auto height =
      m_source_height ? m_source_height : (m_desc.Height ? m_desc.Height : 1);
  auto sample_count = m_desc.SampleDesc.Count ? m_desc.SampleDesc.Count : 1;

  if (m_presenter) {
    m_presenter->changeLayerProperties(source_format, layer_format,
                                       WMTColorSpaceSRGB, width, height,
                                       sample_count);
  } else {
    WMTLayerProps props = {};
    m_layer.getProps(props);
    props.device = m_dxgi_device->GetMTLDevice();
    props.opaque = true;
    props.display_sync_enabled = false;
    props.framebuffer_only = false;
    if (props.contents_scale == 0.0)
      props.contents_scale = 1.0;
    props.drawable_width = width;
    props.drawable_height = height;
    props.pixel_format = layer_format;
    m_layer.setProps(props);
  }

  WMTLayerProps props = {};
  m_layer.getProps(props);
  Logger::info(str::format("M12 ConfigureLayer drawable=", props.drawable_width,
                           "x", props.drawable_height,
                           " src_fmt=", (unsigned)source_format,
                           " layer_fmt=", (unsigned)props.pixel_format,
                           " framebuffer_only=", (int)props.framebuffer_only));
  SCTRACE("ConfigureLayer drawable=%gx%g scale=%g src_fmt=%u layer_fmt=%u "
          "framebuffer_only=%d presenter=%d",
          props.drawable_width, props.drawable_height, props.contents_scale,
          (unsigned)source_format, (unsigned)props.pixel_format,
          (int)props.framebuffer_only, m_presenter ? 1 : 0);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::QueryInterface(REFIID riid,
                                                            void **ppv) {
  if (!ppv)
    return E_POINTER;
  *ppv = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
      riid == __uuidof(IDXGIDeviceSubObject) ||
      riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
      riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) ||
      riid == __uuidof(IDXGISwapChain4)) {
    *ppv = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12SwapChain::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12SwapChain::Release() {
  uint32_t rc = --m_refCount;
  if (!rc)
    delete this;
  return rc;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetPrivateData(REFGUID Name,
                                                            UINT *pDataSize,
                                                            void *pData) {
  return m_private_data.getData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::SetPrivateData(REFGUID Name,
                                                            UINT DataSize,
                                                            const void *pData) {
  return m_private_data.setData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::SetPrivateDataInterface(
    REFGUID Name, const IUnknown *pUnknown) {
  return m_private_data.setInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetDevice(REFIID riid,
                                                       void **device) {
  return m_device->QueryInterface(riid, device);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetParent(REFIID riid,
                                                       void **ppParent) {
  return m_factory->QueryInterface(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::Present(UINT sync_interval,
                                                     UINT flags) {
  SCTRACE("Present sync=%u flags=0x%x", sync_interval, flags);
  return Present1(sync_interval, flags, nullptr);
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetBuffer(UINT buffer_idx,
                                                       REFIID riid,
                                                       void **surface) {
  SCTRACE("GetBuffer idx=%u", buffer_idx);
  if (!surface)
    return E_POINTER;
  if (buffer_idx >= 4 || !m_backbuffers[buffer_idx]) {
    SCTRACE("GetBuffer idx=%u FAILED (no buffer)", buffer_idx);
    return DXGI_ERROR_INVALID_CALL;
  }
  return m_backbuffers[buffer_idx]->QueryInterface(riid, surface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetFullscreenState(BOOL fullscreen, IDXGIOutput *target) {
  SCTRACE("SetFullscreenState fullscreen=%u target=%p hwnd=%p",
          fullscreen ? 1 : 0, target, m_hwnd);
  if (!m_hwnd || !IsWindow(m_hwnd))
    return DXGI_ERROR_INVALID_CALL;

  m_fs_desc.Windowed = !fullscreen;
  m_fullscreen_target = target;
  m_monitor = wsi::getWindowMonitor(m_hwnd);
  if (fullscreen) {
    if (!wsi::enterFullscreenMode(m_monitor, m_hwnd, &m_window_state, false)) {
      SCTRACE("SetFullscreenState enter fullscreen failed hwnd=%p", m_hwnd);
      return DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
  } else {
    wsi::leaveFullscreenMode(m_hwnd, &m_window_state, false);
  }
  ReassertWindowForHandoff(fullscreen ? "set_fullscreen" : "set_windowed");
  EnsureMetalView();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) {
  if (fullscreen)
    *fullscreen = !m_fs_desc.Windowed;
  if (target)
    *target = m_fullscreen_target.ref();
  SCTRACE("GetFullscreenState fullscreen=%u target=%p hwnd=%p",
          m_fs_desc.Windowed ? 0 : 1, target ? *target : nullptr, m_hwnd);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC *desc) {
  if (!desc)
    return E_INVALIDARG;
  desc->BufferDesc.Width = m_desc.Width;
  desc->BufferDesc.Height = m_desc.Height;
  desc->BufferDesc.RefreshRate = m_fs_desc.RefreshRate;
  desc->BufferDesc.Format = m_desc.Format;
  desc->BufferDesc.ScanlineOrdering = m_fs_desc.ScanlineOrdering;
  desc->BufferDesc.Scaling = m_fs_desc.Scaling;
  desc->SampleDesc = m_desc.SampleDesc;
  desc->BufferUsage = m_desc.BufferUsage;
  desc->BufferCount = m_desc.BufferCount;
  desc->OutputWindow = m_hwnd;
  desc->Windowed = m_fs_desc.Windowed;
  desc->SwapEffect = m_desc.SwapEffect;
  desc->Flags = m_desc.Flags;
  SCTRACE("GetDesc hwnd=%p w=%u h=%u fmt=%u buffers=%u windowed=%u", m_hwnd,
          m_desc.Width, m_desc.Height, (unsigned)m_desc.Format,
          m_desc.BufferCount, m_fs_desc.Windowed ? 1 : 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::ResizeBuffers(UINT buffer_count,
                                                           UINT width,
                                                           UINT height,
                                                           DXGI_FORMAT format,
                                                           UINT flags) {
  SCTRACE("ResizeBuffers count=%u w=%u h=%u fmt=%u flags=0x%x (old: w=%u h=%u)",
          buffer_count, width, height, (unsigned)format, flags, m_desc.Width,
          m_desc.Height);
  for (uint32_t i = 0; i < 4; i++)
    m_backbuffers[i] = nullptr;

  if (buffer_count)
    m_desc.BufferCount = buffer_count;
  if (format != DXGI_FORMAT_UNKNOWN)
    m_desc.Format = format;
  m_desc.Flags = flags;

  if (width == 0 || height == 0) {
    RECT rect;
    if (GetClientRect(m_hwnd, &rect)) {
      width = rect.right - rect.left;
      height = rect.bottom - rect.top;
    }
    if (width == 0)
      width = 1;
    if (height == 0)
      height = 1;
  }
  m_desc.Width = width;
  m_desc.Height = height;
  if (m_source_width == 0)
    m_source_width = width;
  if (m_source_height == 0)
    m_source_height = height;
  ReassertWindowForHandoff("resize_buffers");
  ConfigureLayer();

  D3D12_RESOURCE_DESC res_desc = {};
  res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  res_desc.Width = m_desc.Width;
  res_desc.Height = m_desc.Height;
  res_desc.DepthOrArraySize = 1;
  res_desc.MipLevels = 1;
  res_desc.Format = m_desc.Format;
  res_desc.SampleDesc.Count = 1;
  res_desc.SampleDesc.Quality = 0;
  res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_HEAP_PROPERTIES heap_props = {};
  heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

  uint32_t count = m_desc.BufferCount ? m_desc.BufferCount : 2;
  if (count > 4)
    count = 4;
  for (uint32_t i = 0; i < count; i++) {
    m_backbuffers[i] = Com<MTLD3D12Resource>::transfer(new MTLD3D12Resource(
        m_device, res_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, heap_props));
    auto *res = static_cast<MTLD3D12Resource *>(m_backbuffers[i].ptr());
    if (res)
      res->MarkSwapchainBackBuffer(i, this);
    SCTRACE("ResizeBuffers backbuffer[%u] res=%p tex=%llu w=%u h=%u fmt=%u", i,
            (void *)res,
            res ? (unsigned long long)res->GetMTLTexture().handle : 0ull,
            m_desc.Width, m_desc.Height, (unsigned)m_desc.Format);
    Logger::info(str::format(
        "M12 swapchain backbuffer[", i, "] res=", (void *)res,
        " tex=", res ? (unsigned long long)res->GetMTLTexture().handle : 0ull,
        " size=", m_desc.Width, "x", m_desc.Height,
        " fmt=", (unsigned)m_desc.Format));
  }
  if (m_frame_latency_event)
    SetEvent(m_frame_latency_event);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::ResizeTarget(const DXGI_MODE_DESC *new_target_params) {
  SCTRACE("ResizeTarget hwnd=%p w=%u h=%u fmt=%u", m_hwnd,
          new_target_params ? new_target_params->Width : 0,
          new_target_params ? new_target_params->Height : 0,
          new_target_params ? (unsigned)new_target_params->Format : 0);
  if (!new_target_params)
    return DXGI_ERROR_INVALID_CALL;
  if (!m_hwnd || !IsWindow(m_hwnd))
    return DXGI_ERROR_INVALID_CALL;

  if (new_target_params->RefreshRate.Numerator)
    m_fs_desc.RefreshRate = new_target_params->RefreshRate;
  m_fs_desc.ScanlineOrdering = new_target_params->ScanlineOrdering;
  m_fs_desc.Scaling = new_target_params->Scaling;

  if (m_fs_desc.Windowed && new_target_params->Width &&
      new_target_params->Height) {
    RECT client = {0, 0, static_cast<LONG>(new_target_params->Width),
                   static_cast<LONG>(new_target_params->Height)};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&client, style, FALSE, ex_style);
    SetWindowPos(m_hwnd, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    ReassertWindowForHandoff("resize_target_windowed");
  } else if (!m_fs_desc.Windowed) {
    m_monitor = wsi::getWindowMonitor(m_hwnd);
    wsi::WsiMode mode{new_target_params->Width,
                      new_target_params->Height,
                      {new_target_params->RefreshRate.Numerator,
                       new_target_params->RefreshRate.Denominator
                           ? new_target_params->RefreshRate.Denominator
                           : 1u},
                      32,
                      new_target_params->ScanlineOrdering ==
                              DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST ||
                          new_target_params->ScanlineOrdering ==
                              DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST};
    wsi::setWindowMode(m_monitor, m_hwnd, mode);
    wsi::updateFullscreenWindow(m_monitor, m_hwnd, false);
    ReassertWindowForHandoff("resize_target_fullscreen");
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetContainingOutput(IDXGIOutput **output) {
  if (!output)
    return E_POINTER;
  *output = nullptr;
  Com<IDXGIAdapter> adapter;
  HRESULT hr = m_factory->EnumAdapters(0, &adapter);
  if (FAILED(hr))
    return hr;
  hr = adapter->EnumOutputs(0, output);
  SCTRACE("GetContainingOutput -> hr=0x%lx output=%p", hr,
          output ? *output : nullptr);
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) {
  if (!stats)
    return DXGI_ERROR_INVALID_CALL;
  memset(stats, 0, sizeof(*stats));
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetLastPresentCount(UINT *last_present_count) {
  if (!last_present_count)
    return DXGI_ERROR_INVALID_CALL;
  *last_present_count = (UINT)m_present_count;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) {
  if (!desc)
    return E_INVALIDARG;
  *desc = m_desc;
  SCTRACE("GetDesc1 hwnd=%p w=%u h=%u fmt=%u buffers=%u", m_hwnd, m_desc.Width,
          m_desc.Height, (unsigned)m_desc.Format, m_desc.BufferCount);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) {
  if (!desc)
    return E_INVALIDARG;
  *desc = m_fs_desc;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetHwnd(HWND *hWnd) {
  if (!hWnd)
    return E_INVALIDARG;
  *hWnd = m_hwnd;
  SCTRACE("GetHwnd -> %p", m_hwnd);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetCoreWindow(REFIID riid,
                                                           void **core_window) {
  SCTRACE("GetCoreWindow E_NOTIMPL");
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::Present1(
    UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *params) {
  m_present_count++;
  if (m_present_count <= 8 || (m_present_count % PresentLogInterval()) == 0)
    ReassertWindowForHandoff("present");

  if (!m_backbuffers[m_current_buffer]) {
    SCTRACE("SwapChain::Present sync=%u flags=0x%x NO BACKBUFFER idx=%u",
            sync_interval, flags, m_current_buffer);
    if (m_present_count <= 20 ||
        (m_present_count % PresentLogInterval()) == 0) {
      Logger::info(str::format(
          "M12 present entry count=", m_present_count, " sync=",
          sync_interval, " flags=0x", std::hex, flags, std::dec, " idx=",
          m_current_buffer, " backbuffer=0 fmt=", (unsigned)m_desc.Format,
          " size=", m_desc.Width, "x", m_desc.Height));
    }
    return S_OK;
  }

  if (!m_present_queue.handle) {
    auto wmt_device = m_dxgi_device->GetMTLDevice();
    m_present_queue = wmt_device.newCommandQueue(1);
  }

  auto cmdbuf = m_present_queue.commandBuffer();

  auto *res =
      static_cast<MTLD3D12Resource *>(m_backbuffers[m_current_buffer].ptr());
  auto src_texture = res->GetMTLTexture();
  if (m_present_count <= 20 ||
      (m_present_count % PresentLogInterval()) == 0) {
    auto work = res->GetSwapchainQueueWork();
    Logger::info(str::format(
        "M12 present entry count=", m_present_count, " sync=", sync_interval,
        " flags=0x", std::hex, flags, std::dec, " idx=", m_current_buffer,
        " backbuffer=", (void *)res, " src=",
        (unsigned long long)src_texture.handle, " fmt=",
        (unsigned)m_desc.Format, " size=", m_desc.Width, "x", m_desc.Height));
    Logger::info(str::format(
        "M12 present backbuffer work count=", m_present_count, " idx=",
        m_current_buffer, " serial=", (unsigned long long)work.serial,
        " cmds=", work.command_count, " draws=", work.draw_count,
        " indexed=", work.indexed_draw_count,
        " indirect=", work.indirect_count, " dispatch=",
        work.dispatch_count, " clear_rtv=", work.clear_rtv_count,
        " clear_dsv=", work.clear_dsv_count, " clear_uav=",
        work.clear_uav_count, " graphics_setup=", work.graphics_setup,
        " swapchain_work=", work.swapchain_work,
        " has_swapchain_rt=", work.has_swapchain_rt,
        " status=", work.command_buffer_status,
        " replay_ms=", (long long)work.replay_ms, " wait_ms=",
        (long long)work.wait_ms, " classification=",
        SwapchainWorkClassification(work)));
  }
  if (!EnsureMetalView()) {
    Logger::err("D3D12SwapChain::Present: failed to create Metal view/layer");
    SCTRACE("SwapChain::Present sync=%u flags=0x%x NO METAL VIEW",
            sync_interval, flags);
    return DXGI_STATUS_OCCLUDED;
  }

  auto &dxmt_queue = m_device->GetDXMTDevice().queue();
  uint64_t present_wait_seq = dxmt_queue.CurrentSeqId();
  if (present_wait_seq > 0)
    present_wait_seq -= 1;
  if (present_wait_seq > m_last_present_wait_seq) {
    if (LivePresentEnabled()) {
      if (m_present_count <= 20 ||
          (m_present_count % PresentLogInterval()) == 0) {
        SCTRACE("Present live mode skipping CPU render wait seq=%llu buffer=%u",
                (unsigned long long)present_wait_seq, m_current_buffer);
      }
    } else {
      SCTRACE("Present waiting for render seq=%llu",
              (unsigned long long)present_wait_seq);
      auto wait_begin = std::chrono::steady_clock::now();
      dxmt_queue.WaitCPUFence(present_wait_seq);
      auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - wait_begin)
                         .count();
      if (wait_ms >= DXMTD3D12TimingMinMs()) {
        SCTRACE("Present wait_cpu_fence_ms=%lld seq=%llu buffer=%u",
                (long long)wait_ms, (unsigned long long)present_wait_seq,
                m_current_buffer);
      }
    }
    m_last_present_wait_seq = present_wait_seq;
  }

  const bool force_raw_blit_requested =
      std::getenv("DXMT_D3D12_FORCE_SWAPCHAIN_BLIT") != nullptr;
  const bool force_raw_blit =
      force_raw_blit_requested && CanUseRawSwapchainBlit(m_desc.Format);
  if (force_raw_blit_requested && !force_raw_blit &&
      (m_present_count <= 20 ||
       (m_present_count % PresentLogInterval()) == 0)) {
    Logger::info(str::format(
        "M12 present using presenter for non-raw-blit swapchain fmt=",
        (unsigned)m_desc.Format, " count=", m_present_count));
  }
  if (force_raw_blit && m_present_count <= 20) {
    SCTRACE("Present forcing raw swapchain blit path idx=%u", m_current_buffer);
  }

  if (!force_raw_blit && m_presenter && src_texture.handle) {
    auto readback_probe = EncodeSwapchainReadback(
        m_dxgi_device->GetMTLDevice(), cmdbuf, src_texture, m_desc.Width,
        m_desc.Height, m_desc.Format, m_present_count);
    auto state = m_presenter->synchronizeLayerProperties();
    auto drawable = m_presenter->encodeCommands(
        cmdbuf, src_texture, state.metadata, [](WMT::RenderCommandEncoder) {},
        [](WMT::RenderCommandEncoder) {});
    if (!drawable.handle) {
      SCTRACE("SwapChain::Present presenter sync=%u flags=0x%x NO DRAWABLE",
              sync_interval, flags);
      Logger::err("D3D12SwapChain::Present: presenter returned no drawable");
      cmdbuf.commit();
      return DXGI_STATUS_OCCLUDED;
    }

    SCTRACE("Present presenter: idx=%u res=%p src=%llu drawable=%llu w=%u h=%u",
            m_current_buffer, (void *)res,
            (unsigned long long)src_texture.handle,
            (unsigned long long)drawable.texture().handle, m_desc.Width,
            m_desc.Height);
    if (m_present_count <= 20 ||
        (m_present_count % PresentLogInterval()) == 0) {
      Logger::info(str::format(
          "M12 present presenter count=", m_present_count, " idx=",
          m_current_buffer, " src=", (unsigned long long)src_texture.handle,
          " drawable=", (unsigned long long)drawable.texture().handle,
          " size=", m_desc.Width, "x", m_desc.Height));
    }

    cmdbuf.presentDrawable(drawable);
    SCTRACE("[SC_ENC] COMMIT presenter cmdbuf=%llu",
            (unsigned long long)cmdbuf.handle);
    cmdbuf.commit();
    if (readback_probe.mapped) {
      cmdbuf.waitUntilCompleted();
      LogSwapchainReadback(readback_probe, m_desc.Format);
    }
  } else {
    auto drawable = m_layer.nextDrawable();
    if (!drawable.handle) {
      SCTRACE("SwapChain::Present sync=%u flags=0x%x NO DRAWABLE",
              sync_interval, flags);
      Logger::err("D3D12SwapChain::Present: layer returned no drawable");
      cmdbuf.commit();
      return DXGI_STATUS_OCCLUDED;
    }

    auto dst_texture = drawable.texture();

    SCTRACE(
        "Present blit: idx=%u res=%p src=%llu dst=%llu w=%u h=%u",
        m_current_buffer, (void *)res, (unsigned long long)src_texture.handle,
        (unsigned long long)dst_texture.handle, m_desc.Width, m_desc.Height);
    if (m_present_count <= 20 ||
        (m_present_count % PresentLogInterval()) == 0) {
      Logger::info(str::format(
          "M12 present blit count=", m_present_count, " idx=", m_current_buffer,
          " src=", (unsigned long long)src_texture.handle,
          " drawable=", (unsigned long long)dst_texture.handle,
          " size=", m_desc.Width, "x", m_desc.Height));
    }

    auto readback_probe = EncodeSwapchainReadback(
        m_dxgi_device->GetMTLDevice(), cmdbuf, src_texture, m_desc.Width,
        m_desc.Height, m_desc.Format, m_present_count);
    if (src_texture.handle && dst_texture.handle) {
      auto blit = cmdbuf.blitCommandEncoder();
      uint64_t _sc_eid = __atomic_add_fetch(&g_sc_enc_id, 1, __ATOMIC_SEQ_CST);
      SCTRACE("[SC_ENC+%llu] CREATE blit handle=%llu",
              (unsigned long long)_sc_eid, (unsigned long long)blit.handle);
      if (!blit.handle) {
        SCTRACE("[SC_ENC] missing blit encoder handle; skipping present copy");
      } else {
        struct wmtcmd_blit_copy_from_texture_to_texture copy = {};
        copy.type = WMTBlitCommandCopyFromTextureToTexture;
        copy.next.set(nullptr);
        copy.src = src_texture;
        copy.src_slice = 0;
        copy.src_level = 0;
        copy.src_origin = {0, 0, 0};
        copy.src_size = {m_desc.Width, m_desc.Height, 1};
        copy.dst = dst_texture;
        copy.dst_slice = 0;
        copy.dst_level = 0;
        copy.dst_origin = {0, 0, 0};
        blit.encodeCommands(reinterpret_cast<const wmtcmd_blit_nop *>(&copy));
        SCTRACE("[SC_ENC] END handle=%llu", (unsigned long long)blit.handle);
        blit.endEncoding();
      }
    }

    cmdbuf.presentDrawable(drawable);
    SCTRACE("[SC_ENC] COMMIT cmdbuf=%llu", (unsigned long long)cmdbuf.handle);
    cmdbuf.commit();
    if (readback_probe.mapped) {
      cmdbuf.waitUntilCompleted();
      LogSwapchainReadback(readback_probe, m_desc.Format);
    }
  }

  m_current_buffer =
      (m_current_buffer + 1) % (m_desc.BufferCount ? m_desc.BufferCount : 2);
  if (m_frame_latency_event)
    SetEvent(m_frame_latency_event);

  return S_OK;
}

HRESULT
MTLD3D12SwapChain::PresentBackBufferFromQueue(MTLD3D12Resource *resource) {
  if (!resource || !resource->IsSwapchainBackBuffer())
    return DXGI_ERROR_INVALID_CALL;

  uint32_t buffer = resource->SwapchainBackBufferIndex();
  if (buffer >= m_backbuffers.size() || !m_backbuffers[buffer])
    return DXGI_ERROR_INVALID_CALL;

  Logger::info(str::format("M12 autopresent swapchain backbuffer=", buffer,
                           " res=", (void *)resource,
                           " logical_idx=", m_current_buffer));
  uint32_t logical_buffer = m_current_buffer;
  m_current_buffer = buffer;
  HRESULT hr = Present1(0, 0, nullptr);
  // Autopresent is a diagnostic/compatibility fallback for command streams that
  // render swapchain buffers without a timely DXGI Present. It must not advance
  // UE's app-visible swapchain index behind GetCurrentBackBufferIndex().
  m_current_buffer = logical_buffer;
  return hr;
}

WINBOOL STDMETHODCALLTYPE MTLD3D12SwapChain::IsTemporaryMonoSupported() {
  return false;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetRestrictToOutput(IDXGIOutput **output) {
  *output = nullptr;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetBackgroundColor(const DXGI_RGBA *color) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetBackgroundColor(DXGI_RGBA *color) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetRotation(DXGI_MODE_ROTATION rotation) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetRotation(DXGI_MODE_ROTATION *rotation) {
  if (rotation)
    *rotation = DXGI_MODE_ROTATION_IDENTITY;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::SetSourceSize(UINT Width,
                                                           UINT Height) {
  if (Width == 0 || Height == 0)
    return E_INVALIDARG;
  m_source_width = Width;
  m_source_height = Height;
  ConfigureLayer();
  SCTRACE("SetSourceSize w=%u h=%u", Width, Height);
  return S_OK;
}
HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::GetSourceSize(UINT *pWidth,
                                                           UINT *pHeight) {
  if (pWidth)
    *pWidth = m_source_width ? m_source_width : m_desc.Width;
  if (pHeight)
    *pHeight = m_source_height ? m_source_height : m_desc.Height;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
  if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
    return E_INVALIDARG;
  m_frame_latency = MaxLatency;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetMaximumFrameLatency(UINT *pMaxLatency) {
  if (pMaxLatency)
    *pMaxLatency = m_frame_latency;
  return S_OK;
}
HANDLE STDMETHODCALLTYPE MTLD3D12SwapChain::GetFrameLatencyWaitableObject() {
  return m_frame_latency_event;
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) {
  return S_OK;
}
UINT STDMETHODCALLTYPE MTLD3D12SwapChain::GetCurrentBackBufferIndex() {
  return m_current_buffer;
}
HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::CheckColorSpaceSupport(
    DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pSupport) {
  if (!pSupport)
    return E_INVALIDARG;
  *pSupport = IsSupportedColorSpaceForFormat(m_desc.Format, ColorSpace)
                  ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
                  : 0;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
  UINT support = 0;
  CheckColorSpaceSupport(ColorSpace, &support);
  if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
    return E_INVALIDARG;
  m_color_space = ColorSpace;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE MTLD3D12SwapChain::ResizeBuffers1(
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
    UINT SwapChainFlags, const UINT *, IUnknown *const *) {
  SCTRACE("ResizeBuffers1 count=%u w=%u h=%u fmt=%u flags=0x%x", BufferCount,
          Width, Height, (unsigned)Format, SwapChainFlags);
  return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}
HRESULT STDMETHODCALLTYPE
MTLD3D12SwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void *) {
  return S_OK;
}

HRESULT CreateD3D12SwapChain(IDXGIFactory1 *factory, MTLD3D12Device *device,
                             IMTLDXGIDevice *dxgi_device, HWND hWnd,
                             const DXGI_SWAP_CHAIN_DESC1 *desc,
                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                             IDXGISwapChain1 **pp_swap_chain) {
  if (!pp_swap_chain)
    return E_POINTER;
  *pp_swap_chain = nullptr;

  auto swapchain =
      new MTLD3D12SwapChain(factory, device, dxgi_device, hWnd, desc, fs_desc);
  HRESULT hr = swapchain->QueryInterface(IID_PPV_ARGS(pp_swap_chain));
  swapchain->Release();
  return hr;
}

} // namespace dxmt
