#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "Metal.hpp"
#include <atomic>

namespace dxmt {

class MTLD3D12Device;

class MTLD3D12Fence : public ID3D12Fence {
public:
  MTLD3D12Fence(MTLD3D12Device *device, uint64_t initial_value,
                D3D12_FENCE_FLAGS flags);
  ~MTLD3D12Fence();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                          void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                          const void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override;
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override;

  uint64_t STDMETHODCALLTYPE GetCompletedValue() override;
  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(uint64_t value,
                                                 HANDLE event) override;
  HRESULT STDMETHODCALLTYPE Signal(uint64_t value) override;
  void AdoptSharedMapping(HANDLE mapping, void *mapping_view,
                          uint64_t mapping_size, uint64_t value_offset,
                          bool writable = true);
  bool IsShared() const { return m_shared_value != nullptr; }
  bool IsSharedMappingWritable() const {
    return !m_shared_mapping || m_shared_mapping_writable;
  }
  D3D12_FENCE_FLAGS GetFlags() const { return m_flags; }

  WMT::Reference<WMT::SharedEvent> GetMTLSharedEvent() {
    return m_shared_event;
  }
  bool HasSharedMapping() const { return m_shared_value != nullptr; }
  bool ScheduleSharedMappingSignal(uint64_t value);
  bool ScheduleLocalEventSignalFromMapping(uint64_t value);

private:
  MTLD3D12Device *m_device;
  D3D12_FENCE_FLAGS m_flags;
  std::atomic<uint64_t> m_value;
  WMT::Reference<WMT::SharedEvent> m_shared_event;
  HANDLE m_shared_mapping = nullptr;
  void *m_shared_mapping_view = nullptr;
  uint64_t m_shared_mapping_size = 0;
  volatile LONG64 *m_shared_value = nullptr;
  bool m_shared_mapping_writable = true;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
