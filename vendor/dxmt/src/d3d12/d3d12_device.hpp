#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d12.h"
#include "dxmt_format.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_device.hpp"
#include <atomic>
#include <cstddef>
#include <unordered_map>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace dxmt {

// The FL12_2 promotion is gated by the repository-owned aggregate probe. Keep
// this value synchronized with the behavior-backed capability matrix.
inline constexpr D3D_FEATURE_LEVEL kD3D12BuildMaximumFeatureLevel =
    D3D_FEATURE_LEVEL_12_2;
inline constexpr UINT kD3D12ShadingRateImageTileSize = 16;

// Reads d3d12.maxFeatureLevel, but never exceeds the behavior-backed build
// maximum above.
D3D_FEATURE_LEVEL D3D12ConfiguredMaximumFeatureLevel();

class MTLD3D12Resource;
class MTLD3D12PipelineState;
class MTLD3D12CommandQueue;
class MTLD3D12InfoQueue;
struct D3D12ViewInstancingDesc;

enum D3D12SamplerFlagsCompat : UINT {
  D3D12SamplerFlagNoneCompat = 0x0,
  D3D12SamplerFlagUintBorderColorCompat = 0x01,
  D3D12SamplerFlagNonNormalizedCoordinatesCompat = 0x02,
};

struct D3D12SamplerDesc2Compat {
  D3D12_FILTER Filter;
  D3D12_TEXTURE_ADDRESS_MODE AddressU;
  D3D12_TEXTURE_ADDRESS_MODE AddressV;
  D3D12_TEXTURE_ADDRESS_MODE AddressW;
  FLOAT MipLODBias;
  UINT MaxAnisotropy;
  D3D12_COMPARISON_FUNC ComparisonFunc;
  union {
    FLOAT FloatBorderColor[4];
    UINT UintBorderColor[4];
  };
  FLOAT MinLOD;
  FLOAT MaxLOD;
  D3D12SamplerFlagsCompat Flags;
};

struct ID3D12Device11Compat : public ID3D12Device10 {
  virtual void STDMETHODCALLTYPE CreateSampler2(
      const D3D12SamplerDesc2Compat *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) = 0;
};

struct ID3D12Device12Compat : public ID3D12Device11Compat {
  virtual D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
      const UINT32 *num_castable_formats,
      const DXGI_FORMAT *const *castable_formats,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) = 0;
};

// Agility 1.619.5 adds three device interfaces beyond the MinGW header used
// for the PE build.  Their vtable extensions are ABI-only here; the pointer
// and scalar argument layout is identical to the SDK declarations.
struct ID3D12Device13Compat : public ID3D12Device12Compat {
  virtual HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(
      const void *address, SIZE_T size, REFIID riid, void **heap) = 0;
};
struct ID3D12Device14Compat : public ID3D12Device13Compat {
  virtual HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT node_mask, const void *library_blob, SIZE_T blob_length,
      LPCWSTR subobject_name, REFIID riid, void **root_signature) = 0;
};
struct ID3D12Device15Compat : public ID3D12Device14Compat {
  virtual HRESULT STDMETHODCALLTYPE RegisterTrimNotificationCallback(
      void *data) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnregisterTrimNotificationCallback(
      DWORD cookie) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateShaderResourceView(
      ID3D12Resource *resource,
      const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateUnorderedAccessView(
      ID3D12Resource *resource, ID3D12Resource *counter_resource,
      const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateConstantBufferView(
      const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSampler2(
      const D3D12SamplerDesc2Compat *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateRenderTargetView(
      ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateDepthStencilView(
      ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE TryCreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateQueryHeap1(
      const D3D12_QUERY_HEAP_DESC *desc, UINT flags, REFIID riid,
      void **heap) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResolveQueryData(
      ID3D12QueryHeap *query_heap, D3D12_QUERY_TYPE type, UINT start_index,
      UINT query_count, void *resolved_query_data) = 0;
};

const D3D12_COMMAND_SIGNATURE_DESC *
GetD3D12CommandSignatureDesc(ID3D12CommandSignature *signature);

bool D3D12ResolveTriangleAccelerationStructureInfo(
    MTLD3D12Device *device,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs,
    WMTPrimitiveAccelerationStructureInfo &info);
bool D3D12ResolveAABBAccelerationStructureInfo(
    MTLD3D12Device *device,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *inputs,
    WMTAABBAccelerationStructureInfo &info);

WMT::Reference<WMT::ComputePipelineState>
GetD3D12StateObjectRaygenComputePipeline(ID3D12StateObject *state_object);
WMT::Reference<WMT::VisibleFunctionTable>
GetD3D12StateObjectRaygenVisibleFunctionTable(
    ID3D12StateObject *state_object);
WMT::Reference<WMT::IntersectionFunctionTable>
GetD3D12StateObjectIntersectionFunctionTable(
    ID3D12StateObject *state_object);
ID3D12RootSignature *
GetD3D12StateObjectGlobalRootSignature(ID3D12StateObject *state_object);
bool GetD3D12StateObjectShaderRecordLocalRootSignature(
    ID3D12StateObject *state_object, const void *shader_identifier,
    ID3D12RootSignature **local_root_signature);
bool InitializeMetalSharpMetaCommand(ID3D12MetaCommand *command,
                                      const void *data, size_t data_size);
bool ExecuteMetalSharpMetaCommand(ID3D12MetaCommand *command,
                                  const void *data, size_t data_size,
                                  MTLD3D12Device *device,
                                  WMT::CommandBuffer command_buffer);

class MTLD3D12Device : public ID3D12Device15Compat {
public:
  MTLD3D12Device(std::unique_ptr<Device> &&device,
                   IMTLDXGIAdapter *pAdapter);
  ~MTLD3D12Device();

  void *operator new(size_t size) {
    void *ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!ptr) throw std::bad_alloc();
    return ptr;
  }
  void operator delete(void *ptr) {
    VirtualFree(ptr, 0, MEM_RELEASE);
  }

  void SetDXGIDevice(IMTLDXGIDevice *dxgi_device) { m_dxgi_device = dxgi_device; }
  IMTLDXGIDevice *GetDXGIDevice() const { return m_dxgi_device; }

  WMT::Device GetMTLDevice();
  Device &GetDXMTDevice();
  const HostCapabilities &GetHostCapabilities() const {
    return m_device->capabilities();
  }
  ProviderSelection SelectProvider(const ProviderRequirements &requirements) const {
    return m_device->selectProvider(requirements);
  }
  bool SupportsMetalRaytracing() const {
    return m_metal_raytracing_supported;
  }
  UINT GetStateObjectCount() const {
    return m_state_object_count.load(std::memory_order_acquire);
  }
  const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER &
  GetRaytracingSerializationIdentifier() const {
    return m_raytracing_serialization_identifier;
  }

  void RegisterResource(MTLD3D12Resource *res);
  void UnregisterResource(MTLD3D12Resource *res);
  MTLD3D12Resource *LookupResourceByGPUAddress(D3D12_GPU_VIRTUAL_ADDRESS addr);
  void RegisterCommandQueue(MTLD3D12CommandQueue *queue);
  void UnregisterCommandQueue(MTLD3D12CommandQueue *queue);
  void RegisterWorkGraphProgram(const uint8_t *identifier,
                                size_t identifier_size,
                                const std::vector<std::string> &node_msl);
  bool LookupWorkGraphNodeShader(const uint8_t *identifier,
                                 size_t identifier_size,
                                 UINT node_index, std::string &msl) const;
  bool HasWorkGraphProgram(const uint8_t *identifier,
                           size_t identifier_size) const;
  HRESULT EnqueueSetEvent(HANDLE event);
  void NotifyTrimCallbacks(UINT64 bytes_to_trim = 0);

  /*** IUnknown ***/
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                            void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  /*** ID3D12Object ***/
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                            void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                            const void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
      REFGUID guid, const IUnknown *data) override;
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;

  /*** ID3D12Device ***/
  UINT STDMETHODCALLTYPE GetNodeCount() override;

  HRESULT STDMETHODCALLTYPE CreateCommandQueue(
      const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid,
      void **command_queue) override;

  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE type, REFIID riid,
      void **command_allocator) override;

  HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) override;

  HRESULT STDMETHODCALLTYPE CreateComputePipelineState(
      const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state) override;

  HRESULT STDMETHODCALLTYPE CreateCommandList(
      UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
      ID3D12CommandAllocator *command_allocator,
      ID3D12PipelineState *initial_pipeline_state, REFIID riid,
      void **command_list) override;

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
      D3D12_FEATURE feature, void *feature_data,
      UINT feature_data_size) override;

  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(
      const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid,
      void **descriptor_heap) override;

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override;

  HRESULT STDMETHODCALLTYPE CreateRootSignature(
      UINT node_mask, const void *bytecode, SIZE_T bytecode_length,
      REFIID riid, void **root_signature) override;

  void STDMETHODCALLTYPE CreateConstantBufferView(
      const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CreateShaderResourceView(
      ID3D12Resource *resource,
      const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CreateUnorderedAccessView(
      ID3D12Resource *resource, ID3D12Resource *counter_resource,
      const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CreateRenderTargetView(
      ID3D12Resource *resource,
      const D3D12_RENDER_TARGET_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CreateDepthStencilView(
      ID3D12Resource *resource,
      const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CreateSampler(
      const D3D12_SAMPLER_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  void STDMETHODCALLTYPE CopyDescriptors(
      UINT dst_descriptor_range_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
      const UINT *dst_descriptor_range_sizes,
      UINT src_descriptor_range_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
      const UINT *src_descriptor_range_sizes,
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override;

  void STDMETHODCALLTYPE CopyDescriptorsSimple(
      UINT descriptor_count,
      const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
      const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
      D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override;

  D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_desc_count,
      const D3D12_RESOURCE_DESC *resource_descs) override;

  D3D12_HEAP_PROPERTIES* STDMETHODCALLTYPE GetCustomHeapProperties(
      D3D12_HEAP_PROPERTIES *__ret, UINT node_mask,
      D3D12_HEAP_TYPE heap_type) override;

  HRESULT STDMETHODCALLTYPE CreateCommittedResource(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override;

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc,
                                        REFIID riid,
                                        void **heap) override;

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(
      ID3D12Heap *heap, UINT64 heap_offset,
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override;

  HRESULT STDMETHODCALLTYPE CreateReservedResource(
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value, REFIID riid,
      void **resource) override;

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(
      ID3D12DeviceChild *object, const SECURITY_ATTRIBUTES *attributes,
      DWORD access, const WCHAR *name, HANDLE *handle) override;

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid,
                                              void **object) override;

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name,
                                                    DWORD access,
                                                    HANDLE *handle) override;

  HRESULT STDMETHODCALLTYPE MakeResident(
      UINT object_count, ID3D12Pageable *const *objects) override;

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count,
                                    ID3D12Pageable *const *objects) override;

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value,
                                          D3D12_FENCE_FLAGS flags, REFIID riid,
                                          void **fence) override;

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override;

  void STDMETHODCALLTYPE GetCopyableFootprints(
      const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
      UINT sub_resource_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
      UINT64 *row_size, UINT64 *total_bytes) override;

  HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                              REFIID riid,
                                              void **heap) override;

  HRESULT STDMETHODCALLTYPE SetStablePowerState(WINBOOL enable) override;

  HRESULT STDMETHODCALLTYPE CreateCommandSignature(
      const D3D12_COMMAND_SIGNATURE_DESC *desc,
      ID3D12RootSignature *root_signature, REFIID riid,
      void **command_signature) override;

  void STDMETHODCALLTYPE GetResourceTiling(
      ID3D12Resource *resource, UINT *total_tile_count,
      D3D12_PACKED_MIP_INFO *packed_mip_info,
      D3D12_TILE_SHAPE *standard_tile_shape,
      UINT *sub_resource_tiling_count,
      UINT first_sub_resource_tiling,
      D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override;

  LUID* STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) override;

  HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(
      const void *blob, SIZE_T blob_size, REFIID riid,
      void **lib) override;

  HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count,
      D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) override;

  HRESULT STDMETHODCALLTYPE SetResidencyPriority(
      UINT object_count, ID3D12Pageable *const *objects,
      const D3D12_RESIDENCY_PRIORITY *priorities) override;

  /*** ID3D12Device2 ***/
  HRESULT STDMETHODCALLTYPE CreatePipelineState(
      const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid,
      void **ppPipelineState) override;

  /*** ID3D12Device3 ***/
  HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress(
      const void *address, REFIID riid, void **heap) override;
  HRESULT STDMETHODCALLTYPE OpenExistingHeapFromFileMapping(
      HANDLE file_mapping, REFIID riid, void **heap) override;
  HRESULT STDMETHODCALLTYPE EnqueueMakeResident(
      D3D12_RESIDENCY_FLAGS flags, UINT num_objects,
      ID3D12Pageable *const *objects, ID3D12Fence *fence,
      UINT64 fence_value) override;

  /*** ID3D12Device4 ***/
  HRESULT STDMETHODCALLTYPE CreateCommandList1(
      UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
      D3D12_COMMAND_LIST_FLAGS flags, REFIID riid,
      void **command_list) override;
  HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC *desc, REFIID riid,
      void **session) override;
  HRESULT STDMETHODCALLTYPE CreateCommittedResource1(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_resource_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      REFIID riid_resource, void **resource) override;
  HRESULT STDMETHODCALLTYPE CreateHeap1(const D3D12_HEAP_DESC *desc,
      ID3D12ProtectedResourceSession *protected_session,
      REFIID riid, void **heap) override;
  HRESULT STDMETHODCALLTYPE CreateReservedResource1(
      const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      REFIID riid, void **resource) override;
  D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo1(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_descs_count, const D3D12_RESOURCE_DESC *resource_descs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) override;

  /*** ID3D12Device5 ***/
  HRESULT STDMETHODCALLTYPE CreateLifetimeTracker(
      ID3D12LifetimeOwner *owner, REFIID riid, void **tracker) override;
  void STDMETHODCALLTYPE RemoveDevice() override;
  HRESULT STDMETHODCALLTYPE EnumerateMetaCommands(
      UINT *meta_commands_count, D3D12_META_COMMAND_DESC *descs) override;
  HRESULT STDMETHODCALLTYPE EnumerateMetaCommandParameters(
      REFGUID command_id, D3D12_META_COMMAND_PARAMETER_STAGE stage,
      UINT *total_structure_size_in_bytes, UINT *parameter_count,
      D3D12_META_COMMAND_PARAMETER_DESC *parameter_descs) override;
  HRESULT STDMETHODCALLTYPE CreateMetaCommand(
      REFGUID command_id, UINT node_mask,
      const void *creation_parameters_data,
      SIZE_T creation_parameters_data_size_in_bytes,
      REFIID riid, void **meta_command) override;
  HRESULT STDMETHODCALLTYPE CreateStateObject(
      const D3D12_STATE_OBJECT_DESC *desc, REFIID riid,
      void **state_object) override;
  void STDMETHODCALLTYPE GetRaytracingAccelerationStructurePrebuildInfo(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS *desc,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO *info) override;
  D3D12_DRIVER_MATCHING_IDENTIFIER_STATUS STDMETHODCALLTYPE CheckDriverMatchingIdentifier(
      D3D12_SERIALIZED_DATA_TYPE serialized_data_type,
      const D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER *identifier_to_check) override;

  /*** ID3D12Device6 ***/
  HRESULT STDMETHODCALLTYPE SetBackgroundProcessingMode(
      D3D12_BACKGROUND_PROCESSING_MODE mode,
      D3D12_MEASUREMENTS_ACTION action, HANDLE event,
      WINBOOL *further_measurements_desired) override;

  /*** ID3D12Device7 ***/
  HRESULT STDMETHODCALLTYPE AddToStateObject(
      const D3D12_STATE_OBJECT_DESC *addition,
      ID3D12StateObject *state_object_to_grow_from,
      REFIID riid, void **new_state_object) override;
  HRESULT STDMETHODCALLTYPE CreateProtectedResourceSession1(
      const D3D12_PROTECTED_RESOURCE_SESSION_DESC1 *desc,
      REFIID riid, void **session) override;

  /*** ID3D12Device8 ***/
  D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo2(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) override;
  HRESULT STDMETHODCALLTYPE CreateCommittedResource2(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc,
      D3D12_RESOURCE_STATES initial_resource_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      REFIID riid_resource, void **resource) override;
  HRESULT STDMETHODCALLTYPE CreatePlacedResource1(
      ID3D12Heap *heap, UINT64 heap_offset,
      const D3D12_RESOURCE_DESC1 *desc,
      D3D12_RESOURCE_STATES initial_state,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      REFIID riid, void **resource) override;
  void STDMETHODCALLTYPE CreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource,
      D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor) override;
  void STDMETHODCALLTYPE GetCopyableFootprints1(
      const D3D12_RESOURCE_DESC1 *resource_desc, UINT first_subresource,
      UINT subresources_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows_count,
      UINT64 *row_size_in_bytes, UINT64 *total_bytes) override;

  /*** ID3D12Device9 ***/
  HRESULT STDMETHODCALLTYPE CreateShaderCacheSession(
      const D3D12_SHADER_CACHE_SESSION_DESC *desc, REFIID riid,
      void **session) override;
  HRESULT STDMETHODCALLTYPE ShaderCacheControl(
      D3D12_SHADER_CACHE_KIND_FLAGS kinds,
      D3D12_SHADER_CACHE_CONTROL_FLAGS control) override;
  HRESULT STDMETHODCALLTYPE CreateCommandQueue1(
      const D3D12_COMMAND_QUEUE_DESC *desc, REFIID creator_id,
      REFIID riid, void **command_queue) override;

  /*** ID3D12Device10 ***/
  HRESULT STDMETHODCALLTYPE CreateCommittedResource3(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC1 *desc,
      D3D12_BARRIER_LAYOUT initial_layout,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      UINT32 castable_formats_count,
      DXGI_FORMAT *castable_formats,
      REFIID riid_resource, void **resource) override;
  HRESULT STDMETHODCALLTYPE CreatePlacedResource2(
      ID3D12Heap *heap, UINT64 heap_offset,
      const D3D12_RESOURCE_DESC1 *desc,
      D3D12_BARRIER_LAYOUT initial_layout,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      UINT32 castable_formats_count,
      DXGI_FORMAT *castable_formats,
      REFIID riid, void **resource) override;
  HRESULT STDMETHODCALLTYPE CreateReservedResource2(
      const D3D12_RESOURCE_DESC *desc,
      D3D12_BARRIER_LAYOUT initial_layout,
      const D3D12_CLEAR_VALUE *optimized_clear_value,
      ID3D12ProtectedResourceSession *protected_session,
      UINT32 castable_formats_count,
      DXGI_FORMAT *castable_formats,
      REFIID riid, void **resource) override;

  /*** ID3D12Device11Compat ***/
  void STDMETHODCALLTYPE CreateSampler2(
      const D3D12SamplerDesc2Compat *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;

  /*** ID3D12Device12Compat ***/
  D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE GetResourceAllocationInfo3(
      D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
      UINT resource_descs_count, const D3D12_RESOURCE_DESC1 *resource_descs,
      const UINT32 *num_castable_formats,
      const DXGI_FORMAT *const *castable_formats,
      D3D12_RESOURCE_ALLOCATION_INFO1 *resource_allocation_info1) override;

  HRESULT STDMETHODCALLTYPE OpenExistingHeapFromAddress1(
      const void *address, SIZE_T size, REFIID riid, void **heap) override;
  HRESULT STDMETHODCALLTYPE CreateRootSignatureFromSubobjectInLibrary(
      UINT node_mask, const void *library_blob, SIZE_T blob_length,
      LPCWSTR subobject_name, REFIID riid, void **root_signature) override;
  HRESULT STDMETHODCALLTYPE RegisterTrimNotificationCallback(
      void *data) override;
  HRESULT STDMETHODCALLTYPE UnregisterTrimNotificationCallback(
      DWORD cookie) override;
  HRESULT STDMETHODCALLTYPE TryCreateShaderResourceView(
      ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateUnorderedAccessView(
      ID3D12Resource *resource, ID3D12Resource *counter_resource,
      const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateConstantBufferView(
      const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateSampler2(
      const D3D12SamplerDesc2Compat *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateRenderTargetView(
      ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateDepthStencilView(
      ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE TryCreateSamplerFeedbackUnorderedAccessView(
      ID3D12Resource *targeted_resource, ID3D12Resource *feedback_resource,
      D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
  HRESULT STDMETHODCALLTYPE CreateQueryHeap1(
      const D3D12_QUERY_HEAP_DESC *desc, UINT flags, REFIID riid,
      void **heap) override;
  HRESULT STDMETHODCALLTYPE ResolveQueryData(
      ID3D12QueryHeap *query_heap, D3D12_QUERY_TYPE type, UINT start_index,
      UINT query_count, void *resolved_query_data) override;

private:
  HRESULT CreateGraphicsPipelineStateInternal(
      const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid,
      void **pipeline_state, bool depth_bounds_test_enable,
      const D3D12ViewInstancingDesc *view_instancing = nullptr,
      UINT rasterizer_desc2_line_mode = UINT_MAX);

  std::unique_ptr<Device> m_device;
  FormatCapabilityInspector m_format_inspector;
  bool m_metal_raytracing_supported = false;
  std::atomic<UINT> m_state_object_count = {0};
  D3D12_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER
      m_raytracing_serialization_identifier = {};
  Com<IMTLDXGIAdapter> m_adapter;
  IMTLDXGIDevice *m_dxgi_device = nullptr;
  std::atomic_bool m_dxgi_owner_released = false;
  ComPrivateData m_private_data;
  MTLD3D12InfoQueue *m_info_queue = nullptr;
  std::mutex m_info_queue_mutex;
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
  std::mutex m_resource_mutex;
  std::unordered_map<uint64_t, MTLD3D12Resource *> m_resources_by_gpu_addr;
  std::mutex m_command_queue_mutex;
  std::vector<MTLD3D12CommandQueue *> m_command_queues;
  mutable std::mutex m_work_graph_mutex;
  std::unordered_map<std::string, std::vector<std::string>>
      m_work_graph_programs;
  std::mutex m_background_mutex;
  HANDLE m_background_event = nullptr;
  D3D12_BACKGROUND_PROCESSING_MODE m_background_mode =
      D3D12_BACKGROUND_PROCESSING_MODE_ALLOWED;
  D3D12_MEASUREMENTS_ACTION m_background_action =
      D3D12_MEASUREMENTS_ACTION_KEEP_ALL;
  std::atomic_bool m_stable_power_state = false;
  std::atomic<HRESULT> m_device_removed_reason = S_OK;
  std::mutex m_trim_callback_mutex;
  std::unordered_map<DWORD, std::pair<uintptr_t, void *>> m_trim_callbacks;
  DWORD m_next_trim_cookie = 1;
  void *m_expected_vtable = nullptr;
  void CheckVtable(const char *where);
};

} // namespace dxmt
