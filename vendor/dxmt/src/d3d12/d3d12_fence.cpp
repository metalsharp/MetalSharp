#include "d3d12_fence.hpp"
#include "d3d12_device.hpp"
#include "d3d12_trace.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstdlib>

#define FTRACE(fmt, ...)                                                       \
  DXMTD3D12Trace("Fence", fmt, ##__VA_ARGS__)

namespace dxmt {

namespace {

struct FenceEventWaitCtx {
  MTLD3D12Fence *self;
  WMT::Reference<WMT::SharedEvent> shared_event;
  uint64_t wait_value;
  HANDLE wait_event;
};

DWORD WINAPI FenceEventWaitThread(void *arg) {
  auto *ctx = static_cast<FenceEventWaitCtx *>(arg);
  FTRACE("SetEventOnCompletion async wait begin value=%llu this=%p event=%p",
         (unsigned long long)ctx->wait_value, (void *)ctx->self,
         (void *)(uintptr_t)ctx->wait_event);
  ctx->shared_event.waitUntilSignaledValue(ctx->wait_value, UINT64_MAX);
  uint64_t shared_value = ctx->shared_event.signaledValue();
  uint64_t current = ctx->self->GetCompletedValue();
  if (shared_value > current)
    ctx->self->Signal(shared_value);
  FTRACE("SetEventOnCompletion async wait end value=%llu shared=%llu this=%p",
         (unsigned long long)ctx->wait_value, (unsigned long long)shared_value,
         (void *)ctx->self);
  if (ctx->wait_event)
    SetEvent(ctx->wait_event);
  ctx->self->Release();
  delete ctx;
  return 0;
}

} // namespace

MTLD3D12Fence::MTLD3D12Fence(MTLD3D12Device *device, uint64_t initial_value,
                             D3D12_FENCE_FLAGS flags)
    : m_device(device), m_flags(flags), m_value(initial_value) {
  m_device->AddRef();
  auto wmt_device = m_device->GetDXMTDevice().device();
  m_shared_event = wmt_device.newSharedEvent();
  m_shared_event.signalValue(initial_value);
  Logger::info(str::format("D3D12Fence: created value=", initial_value));
}

MTLD3D12Fence::~MTLD3D12Fence() {
  m_shared_event = nullptr;
  m_device->Release();
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::QueryInterface(REFIID riid,
                                                        void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12Fence) {
    *ppvObject = ref(this);
    return S_OK;
  }
  FTRACE("QI unknown IID %s -> E_NOINTERFACE", str::format(riid).c_str());
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12Fence::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12Fence::Release() {
  uint32_t rc = --m_refCount;
  if (!rc)
    delete this;
  return rc;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::GetPrivateData(REFGUID guid,
                                                        UINT *data_size,
                                                        void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::SetPrivateData(REFGUID guid,
                                                        UINT data_size,
                                                        const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12Fence::SetPrivateDataInterface(REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::GetDevice(REFIID riid, void **device) {
  return m_device->QueryInterface(riid, device);
}

uint64_t STDMETHODCALLTYPE MTLD3D12Fence::GetCompletedValue() {
  uint64_t current = m_value.load(std::memory_order_acquire);
  if (m_shared_event.handle) {
    uint64_t shared_value = m_shared_event.signaledValue();
    if (shared_value > current) {
      m_value.store(shared_value, std::memory_order_release);
      current = shared_value;
    }
  }
  FTRACE("GetCompletedValue -> %llu", (unsigned long long)current);
  return current;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::SetEventOnCompletion(uint64_t value,
                                                              HANDLE event) {
  uint64_t current = GetCompletedValue();
  FTRACE("SetEventOnCompletion value=%llu current=%llu this=%p event=%p",
         (unsigned long long)value, (unsigned long long)current, (void *)this,
         (void *)(uintptr_t)event);
  if (current >= value) {
    if (event)
      SetEvent(event);
    return S_OK;
  }

  if (!m_shared_event.handle) {
    FTRACE("SetEventOnCompletion no shared event this=%p", (void *)this);
    return E_FAIL;
  }

  if (!event) {
    FTRACE("SetEventOnCompletion blocking wait begin value=%llu this=%p",
           (unsigned long long)value, (void *)this);
    m_shared_event.waitUntilSignaledValue(value, UINT64_MAX);
    uint64_t shared_value = m_shared_event.signaledValue();
    if (shared_value > m_value.load(std::memory_order_acquire))
      m_value.store(shared_value, std::memory_order_release);
    FTRACE("SetEventOnCompletion blocking wait end value=%llu shared=%llu "
           "this=%p",
           (unsigned long long)value, (unsigned long long)shared_value,
           (void *)this);
    return S_OK;
  }

  AddRef();
  auto *ctx = new FenceEventWaitCtx{this, m_shared_event, value, event};
  HANDLE thread = CreateThread(nullptr, 0, FenceEventWaitThread, ctx, 0, nullptr);
  if (!thread) {
    delete ctx;
    Release();
    return E_FAIL;
  }
  CloseHandle(thread);

  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12Fence::Signal(uint64_t value) {
  FTRACE("Signal value=%llu this=%p", (unsigned long long)value, (void *)this);
  m_value.store(value, std::memory_order_release);
  if (m_shared_event.handle) {
    m_shared_event.signalValue(value);
  }
  return S_OK;
}

} // namespace dxmt
