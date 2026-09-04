#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "d3d12_resource_state.hpp"
#include "d3d12_residency.hpp"
#include "d3d12_shared_resource.hpp"
#include "../dxgi/dxgi_resource.hpp"
#include "Metal.hpp"
#include "winemetal.h"
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

// MinGW's D3D12 interfaces include sampler-feedback resource APIs, but its
// bundled dxgiformat.h predates the two opaque formats from SDK 19041.
#ifndef DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
#define DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ((DXGI_FORMAT)189)
#endif
#ifndef DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE
#define DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE ((DXGI_FORMAT)190)
#endif

namespace dxmt {

class MTLD3D12Device;
class MTLD3D12Heap;
class MTLD3D12SwapChain;

struct D3D12SamplerFeedbackLevelLayout {
  uint64_t offset = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t row_pitch = 0;
};

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

struct D3D12_RESOURCE_DESC1Compat {
  D3D12_RESOURCE_DESC base;
  uint8_t sampler_feedback_mip_region[16];
};
struct ID3D12Resource1Compat : public ID3D12Resource {
  virtual HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(
      REFIID riid, void **protected_session) = 0;
};
struct ID3D12Resource2Compat : public ID3D12Resource1Compat {
  virtual D3D12_RESOURCE_DESC1Compat *STDMETHODCALLTYPE GetDesc1(
      D3D12_RESOURCE_DESC1Compat *ret) = 0;
};

class MTLD3D12Resource : public ID3D12Resource2Compat {
public:
  MTLD3D12Resource(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                   D3D12_RESOURCE_STATES initial_state,
                   D3D12_HEAP_PROPERTIES heap_properties,
                   D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE,
                   bool reserved = false);
  MTLD3D12Resource(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                   D3D12_RESOURCE_STATES initial_state,
                   D3D12_HEAP_PROPERTIES heap_properties,
                   D3D12_HEAP_FLAGS heap_flags,
                   WMT::Reference<WMT::Buffer> backing_buffer,
                   void *backing_cpu_addr, uint64_t backing_gpu_addr,
                   uint64_t backing_offset);
  MTLD3D12Resource(MTLD3D12Device *device, const D3D12_RESOURCE_DESC &desc,
                   D3D12_RESOURCE_STATES initial_state,
                   D3D12_HEAP_PROPERTIES heap_properties,
                   D3D12_HEAP_FLAGS heap_flags,
                   WMT::Reference<WMT::Texture> backing_texture,
                   uint64_t backing_texture_gpu_id,
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
  HRESULT GetDeviceInterface(REFIID riid, void **device) {
    return GetDevice(riid, device);
  }
  HRESULT GetSharedHandle(HANDLE *handle);
  HRESULT CreateSharedHandle(const SECURITY_ATTRIBUTES *attributes, DWORD access,
                             const WCHAR *name, HANDLE *handle);
  HRESULT GetDXGIUsage(DXGI_USAGE *usage);
  void SetEvictionPriority(UINT priority);
  UINT GetEvictionPriority() const;

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
  HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(
      REFIID riid, void **protected_session) override;
  D3D12_RESOURCE_DESC1Compat *STDMETHODCALLTYPE GetDesc1(
      D3D12_RESOURCE_DESC1Compat *ret) override;
  bool ReadBufferRange(uint64_t offset, void *dst_data, uint64_t length);
  HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                    D3D12_HEAP_FLAGS *flags) override;
  HRESULT CreateSubresourceSurface(UINT subresource, IDXGISurface2 **surface);
  void SetProtectedResourceSession(ID3D12ProtectedResourceSession *session);
  ID3D12ProtectedResourceSession *GetProtectedResourceSession() const {
    return m_protected_session;
  }
  bool IsProtectedResource() const { return m_protected_session != nullptr; }

  D3D12_RESOURCE_STATES GetTrackedState(
      UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) const {
    return m_state_tracker.snapshot(subresource).legacy_state;
  }
  D3D12_BARRIER_LAYOUT GetTrackedLayout(
      UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) const {
    return m_state_tracker.snapshot(subresource).layout;
  }
  uint64_t GetStateGeneration() const { return m_state_tracker.generation(); }
  bool ApplyLegacyStateTransition(UINT subresource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    return m_state_tracker.transitionLegacy(subresource, before, after);
  }
  bool ApplyLayoutTransition(UINT subresource, D3D12_BARRIER_LAYOUT before,
                             D3D12_BARRIER_LAYOUT after) {
    return m_state_tracker.transitionLayout(subresource, before, after);
  }
  bool ApplyEnhancedBufferAccess(D3D12_BARRIER_ACCESS before,
                                 D3D12_BARRIER_ACCESS after,
                                 uint64_t offset, uint64_t size) {
    return m_state_tracker.transitionEnhancedAccess(before, after, offset,
                                                    size);
  }
  void MarkAliasedState() { m_state_tracker.markAliased(); }
  bool IsResident() const;
  void MakeResident();
  void Evict();
  void ClearStencil(UINT mip, UINT slice, UINT8 value);
  void SetParentHeap(MTLD3D12Heap *heap);
  D3D12_RESIDENCY_PRIORITY GetResidencyPriority() const {
    return m_residency.priority();
  }
  void SetResidencyPriority(D3D12_RESIDENCY_PRIORITY priority) {
    m_residency.setPriority(priority);
  }
  HRESULT AttachSharedBacking(HANDLE mapping, void *mapping_view,
                              uint64_t mapping_size, uint64_t data_offset,
                              bool preserve_contents);
  void AdoptSharedMapping(HANDLE mapping, void *mapping_view,
                          uint64_t mapping_size, uint64_t data_offset,
                          bool writable = true) {
    m_shared_mapping = mapping;
    m_shared_mapping_view = mapping_view;
    m_shared_mapping_size = mapping_size;
    m_shared_data_offset = data_offset;
    m_shared_mapping_writable = writable;
  }
  bool IsSharedMappingWritable() const {
    return !m_shared_mapping || m_shared_mapping_writable;
  }

  bool IsBuffer() const {
    return m_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
  }
  bool IsValid() const {
    return IsBuffer() ? m_mtl_buffer.handle != 0
                      : (m_mtl_texture.handle != 0 ||
                         !m_planar_shadow.empty());
  }

  WMT::Reference<WMT::Buffer> GetMTLBuffer() { return m_mtl_buffer; }
  WMT::Reference<WMT::Texture> GetMTLTexture();
  uint64_t GetTextureGPUResourceID() const { return m_tex_gpu_resource_id; }
  mach_port_t GetSharedTextureMachPort() const {
    return m_shared_texture_mach_port;
  }
  uint32_t GetTextureArrayLength() const;
  uint64_t GetBufferByteLength() const;
  bool IsReservedResource() const { return m_is_reserved; }
  bool IsPlanarResource() const { return !m_planar_shadow.empty(); }
  bool IsSparseBacked() const {
    return m_is_reserved &&
           ((IsBuffer() && m_mtl_buffer.handle) ||
            (!IsBuffer() && m_mtl_texture.handle &&
             (m_sparse_heap.handle || m_native_placement_sparse_texture)));
  }
  bool IsNativeSparseBuffer() const {
    return m_is_reserved && IsBuffer() && m_native_sparse_buffer;
  }
  bool IsNativePlacementSparseTexture() const {
    return m_is_reserved && !IsBuffer() &&
           m_native_placement_sparse_texture;
  }
  bool IsWritableMSAAEmulated() const { return m_writable_msaa_emulated; }
  bool IsShadingRateImage() const { return m_is_shading_rate_image; }
  bool HasShadingRateImageData() const {
    return m_is_shading_rate_image && m_shading_rate_image_initialized;
  }
  const std::vector<uint8_t> &GetShadingRateImageData() const {
    return m_shading_rate_image_data;
  }
  void UpdateShadingRateImage(const void *data, uint32_t row_pitch,
                              uint32_t dst_x, uint32_t dst_y, uint32_t width,
                              uint32_t height);
  WMT::Reference<WMT::Heap> GetSparseHeap() { return m_sparse_heap; }
  void SetSparseHeap(WMT::Reference<WMT::Heap> heap) {
    m_sparse_heap = std::move(heap);
  }
  void *GetCPUAddress() const { return m_cpu_addr; }
  D3D12_TILE_SHAPE GetTiledResourceTileShape() const;
  bool ConfigureSamplerFeedback(const D3D12_MIP_REGION &region);
  bool IsSamplerFeedback() const { return m_is_sampler_feedback; }
  uint32_t GetSamplerFeedbackWidth() const {
    return m_sampler_feedback_width;
  }
  uint32_t GetSamplerFeedbackHeight() const {
    return m_sampler_feedback_height;
  }
  uint32_t GetSamplerFeedbackRowPitch() const {
    return m_sampler_feedback_row_pitch;
  }
  uint64_t GetSamplerFeedbackDataOffset() const {
    return m_sampler_feedback_data_offset;
  }
  const D3D12SamplerFeedbackLevelLayout *GetSamplerFeedbackLevelLayout(
      uint32_t mip) const {
    return mip < m_sampler_feedback_levels.size()
               ? &m_sampler_feedback_levels[mip]
               : nullptr;
  }
  void SetCastableFormats(UINT32 count, const DXGI_FORMAT *formats) {
    m_has_explicit_castable_formats = count != 0;
    if (count)
      m_castable_formats.assign(formats, formats + count);
    else
      m_castable_formats.clear();
  }
  bool IsViewFormatAllowed(DXGI_FORMAT format) const {
    if (!m_has_explicit_castable_formats || format == DXGI_FORMAT_UNKNOWN ||
        format == m_desc.Format)
      return true;
    for (DXGI_FORMAT allowed : m_castable_formats) {
      if (allowed == format)
        return true;
    }
    return false;
  }
  WMT::Reference<WMT::AccelerationStructure> GetMTLAccelerationStructure() {
    return m_mtl_acceleration_structure;
  }
  void SetMTLAccelerationStructure(
      WMT::Reference<WMT::AccelerationStructure> acceleration_structure,
      uint64_t acceleration_structure_size) {
    m_mtl_acceleration_structure = std::move(acceleration_structure);
    m_mtl_acceleration_structure_size = acceleration_structure_size;
    m_mixed_triangle_acceleration_structure = nullptr;
    m_mixed_aabb_acceleration_structure = nullptr;
  }
  void SetMixedAccelerationStructures(
      WMT::Reference<WMT::AccelerationStructure> triangle_acceleration_structure,
      WMT::Reference<WMT::AccelerationStructure> aabb_acceleration_structure) {
    m_mixed_triangle_acceleration_structure =
        std::move(triangle_acceleration_structure);
    m_mixed_aabb_acceleration_structure = std::move(aabb_acceleration_structure);
  }
  WMT::Reference<WMT::AccelerationStructure>
  GetMixedTriangleAccelerationStructure() {
    return m_mixed_triangle_acceleration_structure;
  }
  WMT::Reference<WMT::AccelerationStructure>
  GetMixedAABBAccelerationStructure() {
    return m_mixed_aabb_acceleration_structure;
  }
  bool HasMixedAccelerationStructures() const {
    return m_mixed_triangle_acceleration_structure.handle &&
           m_mixed_aabb_acceleration_structure.handle;
  }
  uint64_t GetMTLAccelerationStructureSize() const {
    return m_mtl_acceleration_structure_size;
  }
  void SetRaytracingHeaderBuffers(
      WMT::Reference<WMT::Buffer> header_buffer, uint64_t header_gpu_address,
      WMT::Reference<WMT::Buffer> instance_contributions_buffer,
      uint64_t instance_contributions_gpu_address = 0) {
    m_raytracing_header_buffer = std::move(header_buffer);
    m_raytracing_header_gpu_address = header_gpu_address;
    m_raytracing_instance_contributions_buffer =
        std::move(instance_contributions_buffer);
    m_raytracing_instance_contributions_gpu_address =
        instance_contributions_gpu_address;
  }
  uint64_t GetRaytracingHeaderGPUAddress() const {
    return m_raytracing_header_gpu_address;
  }
  WMT::Reference<WMT::Buffer> GetRaytracingHeaderBuffer() {
    return m_raytracing_header_buffer;
  }
  WMT::Reference<WMT::Buffer> GetRaytracingInstanceContributionsBuffer() {
    return m_raytracing_instance_contributions_buffer;
  }
  uint64_t GetRaytracingInstanceContributionsGPUAddress() const {
    return m_raytracing_instance_contributions_gpu_address;
  }
  void SetRaytracingBuildInfo(
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type,
      uint64_t bottom_level_pointer_count,
      const std::vector<D3D12_GPU_VIRTUAL_ADDRESS> &bottom_level_pointers = {}) {
    m_raytracing_type = type;
    m_raytracing_bottom_level_pointer_count = bottom_level_pointer_count;
    m_raytracing_bottom_level_pointers = bottom_level_pointers;
  }
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE GetRaytracingType() const {
    return m_raytracing_type;
  }
  uint64_t GetRaytracingBottomLevelPointerCount() const {
    return m_raytracing_bottom_level_pointer_count;
  }
  const std::vector<D3D12_GPU_VIRTUAL_ADDRESS> &
  GetRaytracingBottomLevelPointers() const {
    return m_raytracing_bottom_level_pointers;
  }

  static uint64_t SerializedAccelerationStructureBlobSize(
      uint64_t bottom_level_pointer_count) {
    const uint64_t raw_size =
        sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER) +
        bottom_level_pointer_count * sizeof(D3D12_GPU_VIRTUAL_ADDRESS) + 64;
    return (raw_size + 255) & ~uint64_t(255);
  }
  void SetSerializedAccelerationStructure(
      WMT::Reference<WMT::AccelerationStructure> acceleration_structure,
      uint64_t acceleration_structure_size,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type,
      uint64_t bottom_level_pointer_count,
      const std::vector<D3D12_GPU_VIRTUAL_ADDRESS> &bottom_level_pointers,
      WMT::Reference<WMT::Buffer> instance_contributions_buffer,
      uint64_t instance_contributions_gpu_address, uint64_t byte_offset,
      uint64_t blob_size) {
    m_serialized_acceleration_structure = std::move(acceleration_structure);
    m_serialized_acceleration_structure_size = acceleration_structure_size;
    m_serialized_raytracing_type = type;
    m_serialized_bottom_level_pointer_count = bottom_level_pointer_count;
    m_serialized_bottom_level_pointers = bottom_level_pointers;
    m_serialized_instance_contributions_buffer =
        std::move(instance_contributions_buffer);
    m_serialized_instance_contributions_gpu_address =
        instance_contributions_gpu_address;
    m_serialized_acceleration_structure_offset = byte_offset;
    m_serialized_acceleration_structure_blob_size = blob_size;
  }
  void ClearSerializedAccelerationStructure() {
    m_serialized_acceleration_structure = {};
    m_serialized_acceleration_structure_size = 0;
    m_serialized_bottom_level_pointer_count = 0;
    m_serialized_bottom_level_pointers.clear();
    m_serialized_instance_contributions_buffer = {};
    m_serialized_instance_contributions_gpu_address = 0;
    m_serialized_acceleration_structure_offset = 0;
    m_serialized_acceleration_structure_blob_size = 0;
  }
  bool HasSerializedAccelerationStructureAt(uint64_t byte_offset) const {
    return m_serialized_acceleration_structure.handle &&
           m_serialized_acceleration_structure_size &&
           m_serialized_acceleration_structure_offset == byte_offset;
  }
  WMT::Reference<WMT::AccelerationStructure>
  GetSerializedAccelerationStructure() {
    return m_serialized_acceleration_structure;
  }
  uint64_t GetSerializedAccelerationStructureSize() const {
    return m_serialized_acceleration_structure_size;
  }
  uint64_t GetSerializedAccelerationStructureOffset() const {
    return m_serialized_acceleration_structure_offset;
  }
  uint64_t GetSerializedAccelerationStructureBlobSize() const {
    return m_serialized_acceleration_structure_blob_size;
  }
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE
  GetSerializedRaytracingType() const {
    return m_serialized_raytracing_type;
  }
  uint64_t GetSerializedBottomLevelPointerCount() const {
    return m_serialized_bottom_level_pointer_count;
  }
  const std::vector<D3D12_GPU_VIRTUAL_ADDRESS> &
  GetSerializedBottomLevelPointers() const {
    return m_serialized_bottom_level_pointers;
  }
  WMT::Reference<WMT::Buffer> GetSerializedInstanceContributionsBuffer() {
    return m_serialized_instance_contributions_buffer;
  }
  uint64_t GetSerializedInstanceContributionsGPUAddress() const {
    return m_serialized_instance_contributions_gpu_address;
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
  ResourceStateTracker m_state_tracker;
  ResidencyState m_residency;
  D3D12_HEAP_PROPERTIES m_heap_properties;
  D3D12_HEAP_FLAGS m_heap_flags = D3D12_HEAP_FLAG_NONE;
  WMTBufferInfo m_buf_info = {};
  WMT::Reference<WMT::Heap> m_sparse_heap;
  WMT::Reference<WMT::Buffer> m_mtl_buffer;
  WMT::Reference<WMT::Texture> m_mtl_texture;
  ID3D12ProtectedResourceSession *m_protected_session = nullptr;
  WMT::Reference<WMT::AccelerationStructure> m_mtl_acceleration_structure;
  uint64_t m_mtl_acceleration_structure_size = 0;
  WMT::Reference<WMT::AccelerationStructure>
      m_mixed_triangle_acceleration_structure;
  WMT::Reference<WMT::AccelerationStructure>
      m_mixed_aabb_acceleration_structure;
  WMT::Reference<WMT::Buffer> m_raytracing_header_buffer;
  WMT::Reference<WMT::Buffer>
      m_raytracing_instance_contributions_buffer;
  uint64_t m_raytracing_header_gpu_address = 0;
  uint64_t m_raytracing_instance_contributions_gpu_address = 0;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE m_raytracing_type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  uint64_t m_raytracing_bottom_level_pointer_count = 0;
  std::vector<D3D12_GPU_VIRTUAL_ADDRESS> m_raytracing_bottom_level_pointers;
  WMT::Reference<WMT::AccelerationStructure>
      m_serialized_acceleration_structure;
  uint64_t m_serialized_acceleration_structure_size = 0;
  uint64_t m_serialized_acceleration_structure_offset = 0;
  uint64_t m_serialized_acceleration_structure_blob_size = 0;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE m_serialized_raytracing_type =
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  uint64_t m_serialized_bottom_level_pointer_count = 0;
  std::vector<D3D12_GPU_VIRTUAL_ADDRESS>
      m_serialized_bottom_level_pointers;
  WMT::Reference<WMT::Buffer>
      m_serialized_instance_contributions_buffer;
  uint64_t m_serialized_instance_contributions_gpu_address = 0;
  uint64_t m_tex_gpu_resource_id = 0;
  mach_port_t m_shared_texture_mach_port = 0;
  uint64_t m_backing_offset = 0;
  bool m_is_reserved = false;
  bool m_native_sparse_buffer = false;
  bool m_native_placement_sparse_texture = false;
  bool m_writable_msaa_emulated = false;
  bool m_is_shading_rate_image = false;
  bool m_shading_rate_image_initialized = false;
  std::vector<uint8_t> m_shading_rate_image_data;
  bool m_is_sampler_feedback = false;
  uint8_t m_sampler_feedback_mip_region[16] = {};
  uint32_t m_sampler_feedback_width = 0;
  uint32_t m_sampler_feedback_height = 0;
  uint32_t m_sampler_feedback_row_pitch = 0;
  uint64_t m_sampler_feedback_data_offset = 0;
  std::vector<D3D12SamplerFeedbackLevelLayout> m_sampler_feedback_levels;
  bool m_has_explicit_castable_formats = false;
  std::vector<DXGI_FORMAT> m_castable_formats;
  bool m_is_swapchain_backbuffer = false;
  uint32_t m_swapchain_buffer_index = 0;
  MTLD3D12SwapChain *m_swapchain = nullptr;
  D3D12SwapchainBackbufferWork m_swapchain_work = {};
  std::unique_ptr<MTLDXGIResource<MTLD3D12Resource>> m_dxgi_resource;
  ComPrivateData m_private_data;
  HANDLE m_shared_mapping = nullptr;
  void *m_shared_mapping_view = nullptr;
  uint64_t m_shared_mapping_size = 0;
  uint64_t m_shared_data_offset = 0;
  bool m_shared_mapping_writable = true;
  MTLD3D12Heap *m_parent_heap = nullptr;

  void *m_cpu_addr = nullptr;
  std::vector<uint8_t> m_stencil_shadow;
  std::vector<uint8_t> m_planar_shadow;
  std::vector<uint8_t> m_packed_shadow;
  uint64_t m_gpu_addr = 0;
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
};

} // namespace dxmt
