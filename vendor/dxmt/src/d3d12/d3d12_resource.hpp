#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "Metal.hpp"
#include "winemetal.h"
#include <atomic>
#include <utility>

namespace dxmt {

class MTLD3D12Device;
class MTLD3D12SwapChain;

struct D3D12SwapchainBackbufferWork {
  uint64_t serial = 0;
  uint32_t command_count = 0;
  uint32_t draw_count = 0;
  uint32_t indexed_draw_count = 0;
  uint32_t indirect_count = 0;
  uint32_t dispatch_count = 0;
  uint32_t clear_rtv_count = 0;
  uint32_t clear_dsv_count = 0;
  uint32_t clear_uav_count = 0;
  uint32_t graphics_setup = 0;
  uint32_t swapchain_work = 0;
  uint32_t has_swapchain_rt = 0;
  int32_t command_buffer_status = 0;
  int64_t replay_ms = 0;
  int64_t wait_ms = 0;
};

class MTLD3D12Resource : public ID3D12Resource {
public:
  MTLD3D12Resource(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                   D3D12_RESOURCE_STATES initial_state,
                   D3D12_HEAP_PROPERTIES heap_properties,
                   D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE);
  MTLD3D12Resource(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                   D3D12_RESOURCE_STATES initial_state,
                   D3D12_HEAP_PROPERTIES heap_properties,
                   D3D12_HEAP_FLAGS heap_flags,
                   WMT::Reference<WMT::Buffer> backing_buffer,
                   void *backing_cpu_addr, uint64_t backing_gpu_addr,
                   uint64_t backing_offset);
  ~MTLD3D12Resource();

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

  HRESULT STDMETHODCALLTYPE Map(UINT sub_resource,
                                const D3D12_RANGE *read_range,
                                void **data) override;
  void STDMETHODCALLTYPE Unmap(UINT sub_resource,
                               const D3D12_RANGE *written_range) override;
  D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) override;
  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() override;
  HRESULT STDMETHODCALLTYPE WriteToSubresource(
      UINT dst_sub_resource, const D3D12_BOX *dst_box, const void *src_data,
      UINT src_row_pitch, UINT src_slice_pitch) override;
  HRESULT STDMETHODCALLTYPE ReadFromSubresource(
      void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
      UINT src_sub_resource, const D3D12_BOX *src_box) override;
  HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                    D3D12_HEAP_FLAGS *flags) override;

  bool IsBuffer() const {
    return m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
  }

  WMT::Reference<WMT::Buffer> GetMTLBuffer() { return m_mtl_buffer; }
  WMT::Reference<WMT::Texture> GetMTLTexture();
  uint64_t GetTextureGPUResourceID() const { return m_tex_gpu_resource_id; }
  uint32_t GetTextureArrayLength() const;
  uint64_t GetBufferByteLength() const;
  WMT::Reference<WMT::AccelerationStructure> GetMTLAccelerationStructure() {
    return m_mtl_acceleration_structure;
  }
  void SetMTLAccelerationStructure(
      WMT::Reference<WMT::AccelerationStructure> acceleration_structure,
      uint64_t acceleration_structure_size) {
    m_mtl_acceleration_structure = std::move(acceleration_structure);
    m_mtl_acceleration_structure_size = acceleration_structure_size;
  }
  uint64_t GetMTLAccelerationStructureSize() const {
    return m_mtl_acceleration_structure_size;
  }

  void MarkSwapchainBackBuffer(uint32_t index, MTLD3D12SwapChain *swapchain) {
    m_is_swapchain_backbuffer = true;
    m_swapchain_buffer_index = index;
    m_swapchain = swapchain;
  }
  bool IsSwapchainBackBuffer() const { return m_is_swapchain_backbuffer; }
  uint32_t SwapchainBackBufferIndex() const { return m_swapchain_buffer_index; }
  MTLD3D12SwapChain *OwningSwapchain() const { return m_swapchain; }
  void RecordSwapchainQueueWork(const D3D12SwapchainBackbufferWork &work) {
    m_swapchain_work = work;
  }
  D3D12SwapchainBackbufferWork GetSwapchainQueueWork() const {
    return m_swapchain_work;
  }

private:
  void InitializeResource(WMT::Reference<WMT::Buffer> backing_buffer,
                          void *backing_cpu_addr,
                          uint64_t backing_gpu_addr,
                          uint64_t backing_offset);

  MTLD3D12Device *m_device;
  D3D12_RESOURCE_DESC m_desc;
  D3D12_RESOURCE_STATES m_state;
  D3D12_HEAP_PROPERTIES m_heap_properties;
  D3D12_HEAP_FLAGS m_heap_flags = D3D12_HEAP_FLAG_NONE;
  WMTBufferInfo m_buf_info = {};
  WMT::Reference<WMT::Buffer> m_mtl_buffer;
  WMT::Reference<WMT::Texture> m_mtl_texture;
  WMT::Reference<WMT::AccelerationStructure> m_mtl_acceleration_structure;
  uint64_t m_mtl_acceleration_structure_size = 0;
  uint64_t m_tex_gpu_resource_id = 0;
  uint64_t m_backing_offset = 0;
  bool m_is_swapchain_backbuffer = false;
  uint32_t m_swapchain_buffer_index = 0;
  MTLD3D12SwapChain *m_swapchain = nullptr;
  D3D12SwapchainBackbufferWork m_swapchain_work = {};
  ComPrivateData m_private_data;

  void *m_cpu_addr = nullptr;
  uint64_t m_gpu_addr = 0;
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
};

} // namespace dxmt
