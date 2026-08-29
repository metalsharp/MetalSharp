#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "d3d12_residency.hpp"
#include "Metal.hpp"
#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D12Device;
class MTLD3D12DescriptorHeap;

// This is the descriptor shape consumed by Metal Shader Converter's explicit
// descriptor-table ABI.  D3D12 descriptor handles remain opaque CPU-process
// handles for the Win32-facing API; shader-visible heaps additionally keep a
// GPU-resident mirror using this layout.
struct D3D12DescriptorTableEntry {
  uint64_t gpu_va = 0;
  uint64_t texture_view_id = 0;
  uint64_t metadata = 0;
};

struct D3D12Descriptor {
  MTLD3D12DescriptorHeap *owner = nullptr;
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
  bool IsShaderVisible() const {
    return (m_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
  }
  bool HasShaderVisibleMirror() const {
    return !IsShaderVisible() || !m_desc.NumDescriptors ||
           m_gpu_descriptor_buffer.handle != 0;
  }
  WMT::Reference<WMT::Buffer> GetShaderVisibleBuffer() {
    return m_residency.isResident() ? m_gpu_descriptor_buffer
                                    : WMT::Reference<WMT::Buffer>{};
  }
  uint64_t GetShaderVisibleGPUAddress(uint32_t index = 0) const {
    if (!m_residency.isResident() || !m_gpu_descriptor_buffer.handle ||
        index >= m_desc.NumDescriptors)
      return 0;
    return m_gpu_descriptor_gpu_address +
           uint64_t(index) * sizeof(D3D12DescriptorTableEntry);
  }
  bool IsResident() const { return m_residency.isResident(); }
  void MakeResident() { m_residency.makeResident(); }
  void Evict() { m_residency.evict(); }
  D3D12_RESIDENCY_PRIORITY GetResidencyPriority() const {
    return m_residency.priority();
  }
  void SetResidencyPriority(D3D12_RESIDENCY_PRIORITY priority) {
    m_residency.setPriority(priority);
  }
  uint32_t GetDescriptorIndex(const D3D12Descriptor *descriptor) const;
  uint32_t GetDescriptorIndexFromGPUHandle(
      D3D12_GPU_DESCRIPTOR_HANDLE handle, uint32_t offset = 0) const;
  void UpdateShaderVisibleDescriptor(
      const D3D12Descriptor *descriptor,
      const D3D12DescriptorTableEntry &entry);

  D3D12Descriptor *GetDescriptorFromGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    return GetDescriptorFromGPUHandle(handle, 0);
  }
  D3D12Descriptor *GetDescriptorFromGPUHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                              uint32_t offset) {
    const uint32_t index = GetDescriptorIndexFromGPUHandle(handle, offset);
    if (index == UINT32_MAX)
      return nullptr;
    return &m_data[index];
  }
  D3D12Descriptor *GetDescriptorFromCPUHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const uintptr_t address = static_cast<uintptr_t>(handle.ptr);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(m_data);
    const uintptr_t end = begin + m_data_size;
    if (!m_data || address < begin || address >= end ||
        (address - begin) % sizeof(D3D12Descriptor) != 0)
      return nullptr;
    const size_t index = (address - begin) / sizeof(D3D12Descriptor);
    return index < m_desc.NumDescriptors ? &m_data[index] : nullptr;
  }

private:
  MTLD3D12Device *m_device;
  D3D12_DESCRIPTOR_HEAP_DESC m_desc;
  ResidencyState m_residency;
  D3D12Descriptor *m_data = nullptr;
  bool m_data_is_virtual = false;
  size_t m_data_size = 0;
  WMT::Reference<WMT::Buffer> m_gpu_descriptor_buffer;
  uint64_t m_gpu_descriptor_gpu_address = 0;
  ComPrivateData m_private_data;
  std::atomic<uint32_t> m_refCount = {1ul};
};

} // namespace dxmt
