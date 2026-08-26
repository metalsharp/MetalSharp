#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "Metal.hpp"
#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D12Device;

struct D3D12Descriptor {
  D3D12_DESCRIPTOR_HEAP_TYPE type;
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  union {
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D12_SAMPLER_DESC sampler;
  };
  WMT::Reference<WMT::SamplerState> metal_sampler;
  WMT::Reference<WMT::SamplerState> metal_sampler_cube;
  WMT::Reference<WMT::Texture> metal_texture_view;
  uint64_t metal_sampler_gpu_id = 0;
  uint64_t metal_sampler_cube_gpu_id = 0;
  uint64_t metal_texture_gpu_id = 0;
  ID3D12Resource *resource = nullptr;
  ID3D12Resource *resource_uav_counter = nullptr;
  ID3D12Resource *sampler_feedback_target = nullptr;
  bool is_sampler_feedback = false;
};

class MTLD3D12DescriptorHeap : public ID3D12DescriptorHeap {
public:
  MTLD3D12DescriptorHeap(MTLD3D12Device *device,
                         const D3D12_DESCRIPTOR_HEAP_DESC &desc);
  ~MTLD3D12DescriptorHeap();

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

  D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override;

  D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override;
  D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override;

  D3D12Descriptor *GetDescriptors() { return m_data; }
  uint32_t GetDescriptorCount() { return m_desc.NumDescriptors; }

  D3D12Descriptor *GetDescriptorFromGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    return GetDescriptorFromGPUHandle(handle, 0);
  }
  D3D12Descriptor *GetDescriptorFromGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                              uint32_t offset) {
    auto *desc = reinterpret_cast<D3D12Descriptor *>(handle.ptr);
    if (desc)
      desc += offset;
    auto *end = m_data + m_desc.NumDescriptors;
    if (desc < m_data || desc >= end)
      return nullptr;
    return desc;
  }
  D3D12Descriptor *GetDescriptorFromCPUHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    auto *desc = reinterpret_cast<D3D12Descriptor *>(handle.ptr);
    auto *end = m_data + m_desc.NumDescriptors;
    if (desc < m_data || desc >= end)
      return nullptr;
    return desc;
  }

private:
  MTLD3D12Device *m_device;
  D3D12_DESCRIPTOR_HEAP_DESC m_desc;
  D3D12Descriptor *m_data = nullptr;
  bool m_data_is_virtual = false;
  size_t m_data_size = 0;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
