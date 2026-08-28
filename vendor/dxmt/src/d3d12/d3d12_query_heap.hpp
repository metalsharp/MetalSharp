#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D12Device;

class MTLD3D12QueryHeap : public ID3D12QueryHeap {
public:
  MTLD3D12QueryHeap(MTLD3D12Device *device,
                    const D3D12_QUERY_HEAP_DESC &desc);
  ~MTLD3D12QueryHeap();

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

  D3D12_QUERY_HEAP_TYPE GetType() const { return m_desc.Type; }
  UINT GetCount() const { return m_desc.Count; }
  uint64_t *GetData() { return m_data.data(); }
  D3D12_QUERY_DATA_PIPELINE_STATISTICS1 *GetPipelineStatistics1Data(
      UINT index) {
    return index < m_pipeline_statistics1.size()
               ? &m_pipeline_statistics1[index]
               : nullptr;
  }
  void BeginPipelineStatistics1(
      UINT index, const D3D12_QUERY_DATA_PIPELINE_STATISTICS1 &current);
  void EndPipelineStatistics1(
      UINT index, const D3D12_QUERY_DATA_PIPELINE_STATISTICS1 &current);

private:
  MTLD3D12Device *m_device;
  D3D12_QUERY_HEAP_DESC m_desc;
  std::vector<uint64_t> m_data;
  std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS1> m_pipeline_statistics1;
  std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS1>
      m_pipeline_statistics1_begin;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
