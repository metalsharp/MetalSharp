#include "d3d12_query_heap.hpp"
#include "d3d12_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt {

MTLD3D12QueryHeap::MTLD3D12QueryHeap(MTLD3D12Device *device,
                                     const D3D12_QUERY_HEAP_DESC &desc)
    : m_device(device), m_desc(desc) {
  m_device->AddRef();
  m_data.resize(desc.Count, 0);
  if (desc.Type == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS1) {
    m_pipeline_statistics1.resize(desc.Count);
    m_pipeline_statistics1_begin.resize(desc.Count);
  }
  Logger::info(str::format("D3D12QueryHeap: type=", desc.Type,
                            " count=", desc.Count));
}

MTLD3D12QueryHeap::~MTLD3D12QueryHeap() { m_device->Release(); }

HRESULT STDMETHODCALLTYPE
MTLD3D12QueryHeap::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12QueryHeap) {
    *ppvObject = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12QueryHeap::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12QueryHeap::Release() {
  uint32_t rc = --m_refCount;
  if (!rc)
    delete this;
  return rc;
}

HRESULT STDMETHODCALLTYPE
MTLD3D12QueryHeap::GetPrivateData(REFGUID guid, UINT *data_size, void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12QueryHeap::SetPrivateData(REFGUID guid, UINT data_size,
                                  const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12QueryHeap::SetPrivateDataInterface(REFGUID guid,
                                           const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12QueryHeap::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12QueryHeap::GetDevice(REFIID riid, void **device) {
  return m_device->QueryInterface(riid, device);
}

void MTLD3D12QueryHeap::BeginPipelineStatistics1(
    UINT index, const D3D12_QUERY_DATA_PIPELINE_STATISTICS1 &current) {
  if (index >= m_pipeline_statistics1.size())
    return;
  m_pipeline_statistics1_begin[index] = current;
  m_pipeline_statistics1[index] = {};
}

void MTLD3D12QueryHeap::EndPipelineStatistics1(
    UINT index, const D3D12_QUERY_DATA_PIPELINE_STATISTICS1 &current) {
  if (index >= m_pipeline_statistics1.size())
    return;
  const auto &begin = m_pipeline_statistics1_begin[index];
  auto &result = m_pipeline_statistics1[index];
#define DXMT_QUERY_STAT_DELTA(field) result.field = current.field - begin.field
  DXMT_QUERY_STAT_DELTA(IAVertices);
  DXMT_QUERY_STAT_DELTA(IAPrimitives);
  DXMT_QUERY_STAT_DELTA(VSInvocations);
  DXMT_QUERY_STAT_DELTA(GSInvocations);
  DXMT_QUERY_STAT_DELTA(GSPrimitives);
  DXMT_QUERY_STAT_DELTA(CInvocations);
  DXMT_QUERY_STAT_DELTA(CPrimitives);
  DXMT_QUERY_STAT_DELTA(PSInvocations);
  DXMT_QUERY_STAT_DELTA(HSInvocations);
  DXMT_QUERY_STAT_DELTA(DSInvocations);
  DXMT_QUERY_STAT_DELTA(CSInvocations);
  DXMT_QUERY_STAT_DELTA(ASInvocations);
  DXMT_QUERY_STAT_DELTA(MSInvocations);
  DXMT_QUERY_STAT_DELTA(MSPrimitives);
#undef DXMT_QUERY_STAT_DELTA
}

} // namespace dxmt
