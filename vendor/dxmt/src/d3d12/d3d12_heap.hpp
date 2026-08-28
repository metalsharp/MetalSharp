#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "Metal.hpp"
#include <atomic>

namespace dxmt {

class MTLD3D12Device;

class MTLD3D12Heap : public ID3D12Heap {
public:
  MTLD3D12Heap(MTLD3D12Device *device, const D3D12_HEAP_DESC &desc);
  ~MTLD3D12Heap();

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

  D3D12_HEAP_DESC *STDMETHODCALLTYPE GetDesc(D3D12_HEAP_DESC *__ret) override;

  const D3D12_HEAP_DESC &GetHeapDesc() const { return m_desc; }
  WMT::Reference<WMT::Buffer> GetMTLBuffer() const { return m_buffer; }
  WMT::Reference<WMT::Heap> GetMTLHeap();
  void *GetCPUAddress() const { return m_cpu_addr; }
  uint64_t GetGPUAddress() const { return m_gpu_addr; }

private:
  MTLD3D12Device *m_device;
  D3D12_HEAP_DESC m_desc;
  WMTBufferInfo m_buf_info = {};
  WMT::Reference<WMT::Buffer> m_buffer;
  WMT::Reference<WMT::Heap> m_heap;
  void *m_cpu_addr = nullptr;
  uint64_t m_gpu_addr = 0;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
