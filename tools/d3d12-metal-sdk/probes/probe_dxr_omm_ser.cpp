#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

#ifndef D3D12_FEATURE_D3D12_OPTIONS22_PROBE
#define D3D12_FEATURE_D3D12_OPTIONS22_PROBE 65
#endif

using D3D12CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                               REFIID, void **);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI *)(
    const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
    ID3DBlob **);

static const GUID IID_D3D12DeviceProbe = {
    0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

struct ProbeResult {
  bool ok = false;
  HRESULT hr = E_FAIL;
  std::string detail;
  std::string extra;
};

template <typename T> static void safe_release(T *&value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
  T result = nullptr;
  FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
  static_assert(sizeof(result) == sizeof(proc), "function pointer size mismatch");
  std::memcpy(&result, &proc, sizeof(result));
  return result;
}

static std::string hr_hex(HRESULT hr) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                static_cast<unsigned long>(static_cast<uint32_t>(hr)));
  return buffer;
}

static std::string json_escape(const std::string &input) {
  std::string output;
  for (char value : input) {
    if (value == '\\')
      output += "\\\\";
    else if (value == '"')
      output += "\\\"";
    else if (value == '\n')
      output += "\\n";
    else
      output += value;
  }
  return output;
}

static bool read_binary_file(const char *path, std::vector<uint8_t> &data) {
  FILE *file = std::fopen(path, "rb");
  if (!file)
    return false;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(file);
    return false;
  }
  data.resize(static_cast<size_t>(size));
  const size_t read = std::fread(data.data(), 1, data.size(), file);
  std::fclose(file);
  return read == data.size();
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result = {};
  result.Type = type;
  result.CreationNodeMask = 1;
  result.VisibleNodeMask = 1;
  return result;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes,
                                       D3D12_RESOURCE_FLAGS flags =
                                           D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC result = {};
  result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  result.Width = bytes;
  result.Height = 1;
  result.DepthOrArraySize = 1;
  result.MipLevels = 1;
  result.Format = DXGI_FORMAT_UNKNOWN;
  result.SampleDesc.Count = 1;
  result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  result.Flags = flags;
  return result;
}

static HRESULT create_device(ID3D12Device **device) {
  HMODULE module = LoadLibraryA("d3d12.dll");
  if (!module)
    return HRESULT_FROM_WIN32(GetLastError());
  D3D12CreateDeviceFn create = load_proc<D3D12CreateDeviceFn>(
      module, "D3D12CreateDevice");
  if (!create)
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  return create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                reinterpret_cast<void **>(device));
}

static HRESULT serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC &desc,
                                        ID3DBlob **blob) {
  HMODULE module = LoadLibraryA("d3d12.dll");
  D3D12SerializeRootSignatureFn serialize =
      load_proc<D3D12SerializeRootSignatureFn>(module,
                                                "D3D12SerializeRootSignature");
  if (!serialize)
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
  ID3DBlob *errors = nullptr;
  HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &errors);
  safe_release(errors);
  return hr;
}

static HRESULT execute_and_wait(ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
  HRESULT hr = list->Close();
  if (FAILED(hr))
    return hr;
  ID3D12Device *device = nullptr;
  hr = queue->GetDevice(IID_PPV_ARGS(&device));
  if (FAILED(hr))
    return hr;
  ID3D12Fence *fence = nullptr;
  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  safe_release(device);
  if (FAILED(hr))
    return hr;
  HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!event_handle) {
    safe_release(fence);
    return HRESULT_FROM_WIN32(GetLastError());
  }
  ID3D12CommandList *base = list;
  queue->ExecuteCommandLists(1, &base);
  hr = queue->Signal(fence, 1);
  if (SUCCEEDED(hr) && fence->GetCompletedValue() < 1) {
    hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr) && WaitForSingleObject(event_handle, 5000) != WAIT_OBJECT_0)
      hr = E_FAIL;
  }
  CloseHandle(event_handle);
  safe_release(fence);
  return hr;
}

// Agility 1.619.5 records these new DXR 1.2 PODs in the existing union slots.
// Keep the probe independent of the pinned MinGW header, while retaining the
// exact field order and bit widths used by the Windows ABI.
constexpr UINT kOmmArrayType = 2;
constexpr UINT kOmmTrianglesType = 2;
constexpr UINT kPipelineConfig1SubobjectType = 12;
constexpr UINT kPipelineFlagSkipTriangles = 0x100;
constexpr UINT kPipelineFlagAllowOpacityMicromaps = 0x400;
constexpr UINT kOmmFormatOc1TwoState = 1;
constexpr UINT kOmmFormatOc1FourState = 2;
constexpr UINT kBuildFlagAllowUpdate = 0x1;
constexpr UINT kBuildFlagAllowCompaction = 0x2;
constexpr UINT kBuildFlagPerformUpdate = 0x20;
constexpr UINT kBuildFlagAllowOmmUpdate = 0x40;
constexpr UINT kBuildFlagAllowDisableOmms = 0x80;
constexpr UINT kInstanceFlagForceOmm2State = 0x10;
constexpr UINT kInstanceFlagDisableOmms = 0x20;
constexpr D3D12_FEATURE kOptions22Feature =
    static_cast<D3D12_FEATURE>(D3D12_FEATURE_D3D12_OPTIONS22_PROBE);

struct Config1Probe {
  UINT max_trace_recursion_depth;
  UINT flags;
};
static_assert(sizeof(Config1Probe) == 8, "CONFIG1 ABI");

struct Options22Probe {
  BOOL shader_execution_reordering_actually_reorders;
  BOOL create_byte_offset_views_supported;
  UINT max_1d_dispatch_size;
  UINT max_1d_dispatch_mesh_size;
};
static_assert(sizeof(Options22Probe) == 16, "OPTIONS22 ABI");

struct OmmDescProbe {
  UINT byte_offset;
  UINT subdivision_level : 16;
  UINT format : 16;
};
static_assert(sizeof(OmmDescProbe) == 8, "OMM descriptor ABI");

struct OmmLinkageProbe {
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE index_buffer;
  DXGI_FORMAT index_format;
  UINT base_location;
  D3D12_GPU_VIRTUAL_ADDRESS opacity_micromap_array;
};
static_assert(sizeof(OmmLinkageProbe) == 32, "OMM linkage ABI");

struct OmmTrianglesProbe {
  const D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC *triangles;
  const OmmLinkageProbe *linkage;
};
static_assert(sizeof(OmmTrianglesProbe) == 16, "OMM geometry ABI");

struct OmmHistogramProbe {
  UINT count;
  UINT subdivision_level;
  UINT format;
};
static_assert(sizeof(OmmHistogramProbe) == 12, "OMM histogram ABI");

struct OmmArrayProbe {
  UINT histogram_entry_count;
  const OmmHistogramProbe *histogram;
  D3D12_GPU_VIRTUAL_ADDRESS input_buffer;
  D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE per_omm_descs;
};
static_assert(sizeof(OmmArrayProbe) == 40, "OMM array ABI");

static D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

static HRESULT create_buffer(ID3D12Device *device, D3D12_HEAP_TYPE heap_type,
                             UINT64 bytes, D3D12_RESOURCE_STATES state,
                             D3D12_RESOURCE_FLAGS flags, ID3D12Resource **out) {
  D3D12_HEAP_PROPERTIES props = heap_props(heap_type);
  D3D12_RESOURCE_DESC desc = buffer_desc(bytes, flags);
  return device->CreateCommittedResource(
      &props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
      IID_PPV_ARGS(out));
}

static HRESULT upload_buffer(ID3D12Device *device, const void *data,
                             UINT64 bytes, ID3D12Resource **out) {
  HRESULT hr = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes,
                             D3D12_RESOURCE_STATE_GENERIC_READ,
                             D3D12_RESOURCE_FLAG_NONE, out);
  if (FAILED(hr))
    return hr;
  void *mapped = nullptr;
  hr = (*out)->Map(0, nullptr, &mapped);
  if (SUCCEEDED(hr)) {
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    (*out)->Unmap(0, nullptr);
  }
  return hr;
}

static void set_omm_geometry_union(D3D12_RAYTRACING_GEOMETRY_DESC &geometry,
                                    const OmmTrianglesProbe &omm) {
  std::memset(&geometry.Triangles, 0, sizeof(geometry.Triangles));
  std::memcpy(&geometry.Triangles, &omm, sizeof(omm));
}

static ProbeResult probe_omm_ser() {
  std::vector<uint8_t> accessor_shader;
  std::vector<uint8_t> raygen_shader;
  // The runner stages these existing shaders beside the executable.
  if (!read_binary_file("probe_dxr_inline_accessors.cso", accessor_shader))
    return {false, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            "probe_dxr_inline_accessors.cso is missing or unreadable", ""};
  if (!read_binary_file("probe_dxr_raygen.cso", raygen_shader))
    return {false, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            "probe_dxr_raygen.cso is missing or unreadable", ""};

  ID3D12Device *device = nullptr;
  ID3D12Device5 *device5 = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *base_list = nullptr;
  ID3D12GraphicsCommandList4 *list4 = nullptr;
  ID3D12RootSignature *root = nullptr;
  ID3D12PipelineState *pso = nullptr;
  ID3D12DescriptorHeap *heap = nullptr;
  ID3D12Resource *vertices = nullptr;
  ID3D12Resource *indices = nullptr;
  ID3D12Resource *omm_input = nullptr;
  ID3D12Resource *omm_descs = nullptr;
  ID3D12Resource *omm_array_opaque = nullptr;
  ID3D12Resource *omm_array_transparent = nullptr;
  ID3D12Resource *omm_array_unknown = nullptr;
  ID3D12Resource *omm_scratch = nullptr;
  ID3D12Resource *omm_postbuild_opaque = nullptr;
  ID3D12Resource *omm_postbuild_transparent = nullptr;
  ID3D12Resource *omm_postbuild_unknown = nullptr;
  ID3D12Resource *blas_opaque = nullptr;
  ID3D12Resource *blas_transparent = nullptr;
  ID3D12Resource *blas_scratch = nullptr;
  ID3D12Resource *tlas_opaque = nullptr;
  ID3D12Resource *tlas_transparent = nullptr;
  ID3D12Resource *tlas_disabled = nullptr;
  ID3D12Resource *tlas_scratch = nullptr;
  ID3D12Resource *instances_opaque = nullptr;
  ID3D12Resource *instances_transparent = nullptr;
  ID3D12Resource *instances_disabled = nullptr;
  ID3D12Resource *output = nullptr;
  ID3D12Resource *readback = nullptr;
  ID3D12StateObject *config1_omm_state = nullptr;
  ID3D12StateObject *config1_skip_state = nullptr;
  bool config1_omm_accepted = false;
  bool config1_skip_rejected = false;
  bool omm_refit_recorded = false;
  bool instance_force_omm2_state_verified = false;
  bool instance_disable_omms_verified = false;
  HRESULT config1_omm_hr = E_FAIL;
  HRESULT config1_skip_hr = E_FAIL;
  HRESULT invalid_recording_close_hr = E_FAIL;
  HRESULT reset_after_invalid_hr = E_FAIL;
  HRESULT reset_after_invalid_close_hr = E_FAIL;
  HRESULT reset_after_misaligned_dest_hr = E_FAIL;
  HRESULT misaligned_dest_close_hr = E_FAIL;
  HRESULT reset_after_misaligned_dest_close_hr = E_FAIL;
  HRESULT reset_after_misaligned_scratch_hr = E_FAIL;
  HRESULT misaligned_scratch_close_hr = E_FAIL;
  HRESULT reset_after_misaligned_scratch_close_hr = E_FAIL;
  HRESULT hr = create_device(&device);
  HRESULT device5_hr = E_FAIL;
  HRESULT list4_hr = E_FAIL;
  if (SUCCEEDED(hr))
    device5_hr = device->QueryInterface(IID_PPV_ARGS(&device5));
  if (SUCCEEDED(hr))
    hr = device5_hr;
  if (SUCCEEDED(hr)) {
    D3D12_DXIL_LIBRARY_DESC library = {};
    library.DXILLibrary.pShaderBytecode = raygen_shader.data();
    library.DXILLibrary.BytecodeLength = raygen_shader.size();
    Config1Probe config1 = {};
    config1.max_trace_recursion_depth = 1;
    config1.flags = kPipelineFlagAllowOpacityMicromaps;
    D3D12_STATE_SUBOBJECT subobjects[2] = {};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &library;
    subobjects[1].Type = static_cast<D3D12_STATE_SUBOBJECT_TYPE>(
        kPipelineConfig1SubobjectType);
    subobjects[1].pDesc = &config1;
    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_desc.NumSubobjects = 2;
    state_desc.pSubobjects = subobjects;
    config1_omm_hr = device5->CreateStateObject(
        &state_desc, IID_PPV_ARGS(&config1_omm_state));
    config1_omm_accepted = SUCCEEDED(config1_omm_hr) &&
                           config1_omm_state != nullptr;
    config1.flags = kPipelineFlagSkipTriangles;
    config1_skip_hr = device5->CreateStateObject(
        &state_desc, IID_PPV_ARGS(&config1_skip_state));
    config1_skip_rejected = FAILED(config1_skip_hr) &&
                            config1_skip_state == nullptr;
    if (!config1_omm_accepted || !config1_skip_rejected)
      hr = E_FAIL;
  }
  if (SUCCEEDED(hr)) {
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  }
  if (SUCCEEDED(hr))
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
  if (SUCCEEDED(hr))
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   allocator, nullptr,
                                   IID_PPV_ARGS(&base_list));
  if (SUCCEEDED(hr))
    list4_hr = base_list->QueryInterface(IID_PPV_ARGS(&list4));
  if (SUCCEEDED(hr))
    hr = list4_hr;

  Options22Probe options22 = {};
  HRESULT options22_hr = E_FAIL;
  HRESULT options22_bad_size_hr = E_FAIL;
  if (SUCCEEDED(hr)) {
    options22_hr = device->CheckFeatureSupport(
        kOptions22Feature, &options22, sizeof(options22));
    options22_bad_size_hr = device->CheckFeatureSupport(
        kOptions22Feature, &options22, sizeof(options22) - 1);
  }

  const float vertex_data[] = {
      -1.0f, -1.0f, 0.0f,
      1.0f, -1.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
  };
  const uint32_t index_data[] = {0, 1, 2};
  const uint8_t omm_state_data[] = {1, 0};
  const OmmDescProbe omm_desc_data[2] = {
      {0, 0, kOmmFormatOc1TwoState},
      {1, 0, kOmmFormatOc1TwoState},
  };
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, vertex_data, sizeof(vertex_data), &vertices);
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, index_data, sizeof(index_data), &indices);
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, omm_state_data, sizeof(omm_state_data),
                       &omm_input);
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, omm_desc_data, sizeof(omm_desc_data),
                       &omm_descs);

  OmmHistogramProbe histogram = {1, 0, kOmmFormatOc1TwoState};
  OmmArrayProbe array_opaque = {};
  array_opaque.histogram_entry_count = 1;
  array_opaque.histogram = &histogram;
  array_opaque.input_buffer = 0;
  array_opaque.per_omm_descs = {};
  OmmArrayProbe array_transparent = array_opaque;
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS omm_inputs = {};
  omm_inputs.Type = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE>(
      kOmmArrayType);
  omm_inputs.NumDescs = 1;
  omm_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO omm_prebuild = {};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO aligned_128_input_prebuild = {};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO invalid_input_alignment_prebuild = {};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO invalid_desc_alignment_prebuild = {};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO invalid_format_prebuild = {};
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO invalid_layout_prebuild = {};
  bool aligned_128_input_accepted = false;
  bool invalid_input_alignment_rejected = false;
  bool invalid_desc_alignment_rejected = false;
  bool invalid_format_rejected = false;
  bool invalid_layout_rejected = false;
  bool invalid_four_state_rejected = false;
  bool invalid_update_flags_rejected = false;
  bool invalid_disable_flags_rejected = false;
  bool allow_disable_blas_prebuild_accepted = false;
  bool unknown_state_rejected = false;
  if (SUCCEEDED(hr)) {
    array_opaque.input_buffer = omm_input->GetGPUVirtualAddress();
    array_transparent.input_buffer = omm_input->GetGPUVirtualAddress();
    array_opaque.per_omm_descs.StartAddress = omm_descs->GetGPUVirtualAddress();
    array_opaque.per_omm_descs.StrideInBytes = sizeof(OmmDescProbe);
    array_transparent.per_omm_descs = array_opaque.per_omm_descs;
    array_transparent.per_omm_descs.StartAddress += sizeof(OmmDescProbe);
    omm_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &array_opaque);
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&omm_inputs,
                                                             &omm_prebuild);
    if (!omm_prebuild.ResultDataMaxSizeInBytes ||
        !omm_prebuild.ScratchDataSizeInBytes)
      hr = E_NOTIMPL;

    // The stable D3D12 contract deliberately accepts a raw InputBuffer at
    // 128-byte alignment.  Exercise that boundary independently of the
    // committed resource base (which is commonly aligned more strictly).
    OmmArrayProbe aligned_128_input_array = array_opaque;
    aligned_128_input_array.input_buffer += 128;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS aligned_128_inputs =
        omm_inputs;
    aligned_128_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &aligned_128_input_array);
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &aligned_128_inputs, &aligned_128_input_prebuild);
    aligned_128_input_accepted =
        aligned_128_input_prebuild.ResultDataMaxSizeInBytes ==
            omm_prebuild.ResultDataMaxSizeInBytes &&
        aligned_128_input_prebuild.ScratchDataSizeInBytes ==
            omm_prebuild.ScratchDataSizeInBytes;

    OmmArrayProbe invalid_input_alignment_array = array_opaque;
    invalid_input_alignment_array.input_buffer += 64;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
        invalid_input_alignment_inputs = omm_inputs;
    invalid_input_alignment_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &invalid_input_alignment_array);
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &invalid_input_alignment_inputs, &invalid_input_alignment_prebuild);
    invalid_input_alignment_rejected =
        invalid_input_alignment_prebuild.ResultDataMaxSizeInBytes == 0 &&
        invalid_input_alignment_prebuild.ScratchDataSizeInBytes == 0;

    OmmArrayProbe invalid_desc_alignment_array = array_opaque;
    invalid_desc_alignment_array.per_omm_descs.StartAddress += 2;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
        invalid_desc_alignment_inputs = omm_inputs;
    invalid_desc_alignment_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &invalid_desc_alignment_array);
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &invalid_desc_alignment_inputs, &invalid_desc_alignment_prebuild);
    invalid_desc_alignment_rejected =
        invalid_desc_alignment_prebuild.ResultDataMaxSizeInBytes == 0 &&
        invalid_desc_alignment_prebuild.ScratchDataSizeInBytes == 0;

    OmmHistogramProbe invalid_histogram = {1, 0, 0xffff};
    OmmArrayProbe invalid_array = array_opaque;
    invalid_array.histogram = &invalid_histogram;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS invalid_inputs =
        omm_inputs;
    invalid_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &invalid_array);
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &invalid_inputs, &invalid_format_prebuild);
    invalid_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &invalid_inputs, &invalid_layout_prebuild);
    invalid_format_rejected =
        invalid_format_prebuild.ResultDataMaxSizeInBytes == 0 &&
        invalid_format_prebuild.ScratchDataSizeInBytes == 0;
    invalid_layout_rejected =
        invalid_layout_prebuild.ResultDataMaxSizeInBytes == 0 &&
        invalid_layout_prebuild.ScratchDataSizeInBytes == 0;

    OmmHistogramProbe four_state_histogram = {
        1, 0, kOmmFormatOc1FourState};
    OmmArrayProbe four_state_array = array_opaque;
    four_state_array.histogram = &four_state_histogram;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS four_state_inputs =
        omm_inputs;
    four_state_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &four_state_array);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO four_state_prebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &four_state_inputs, &four_state_prebuild);
    invalid_four_state_rejected =
        four_state_prebuild.ResultDataMaxSizeInBytes == 0 &&
        four_state_prebuild.ScratchDataSizeInBytes == 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS update_flag_inputs =
        omm_inputs;
    update_flag_inputs.Flags = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
        kBuildFlagAllowOmmUpdate);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO update_flag_prebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &update_flag_inputs, &update_flag_prebuild);
    invalid_update_flags_rejected =
        update_flag_prebuild.ResultDataMaxSizeInBytes == 0 &&
        update_flag_prebuild.ScratchDataSizeInBytes == 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS disable_flag_inputs =
        omm_inputs;
    disable_flag_inputs.Flags = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
        kBuildFlagAllowDisableOmms);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO disable_flag_prebuild = {};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(
        &disable_flag_inputs, &disable_flag_prebuild);
    invalid_disable_flags_rejected =
        disable_flag_prebuild.ResultDataMaxSizeInBytes == 0 &&
        disable_flag_prebuild.ScratchDataSizeInBytes == 0;
  }
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       omm_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &omm_array_opaque);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       omm_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &omm_array_transparent);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       omm_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &omm_array_unknown);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       omm_prebuild.ScratchDataSizeInBytes,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &omm_scratch);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256,
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_NONE, &omm_postbuild_opaque);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256,
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_NONE, &omm_postbuild_transparent);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256,
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_NONE, &omm_postbuild_unknown);

  D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles = {};
  triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
  triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
  triangles.IndexCount = 3;
  triangles.VertexCount = 3;
  triangles.IndexBuffer = indices ? indices->GetGPUVirtualAddress() : 0;
  triangles.VertexBuffer.StartAddress =
      vertices ? vertices->GetGPUVirtualAddress() : 0;
  triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
  D3D12_RAYTRACING_GEOMETRY_DESC omm_geometry = {};
  omm_geometry.Type = static_cast<D3D12_RAYTRACING_GEOMETRY_TYPE>(
      kOmmTrianglesType);
  omm_geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
  OmmLinkageProbe linkage_opaque = {};
  linkage_opaque.index_buffer.StartAddress =
      indices ? indices->GetGPUVirtualAddress() : 0;
  linkage_opaque.index_buffer.StrideInBytes = sizeof(uint32_t);
  linkage_opaque.index_format = DXGI_FORMAT_R32_UINT;
  linkage_opaque.opacity_micromap_array =
      omm_array_opaque ? omm_array_opaque->GetGPUVirtualAddress() : 0;
  OmmTrianglesProbe omm_triangles = {&triangles, &linkage_opaque};
  set_omm_geometry_union(omm_geometry, omm_triangles);

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
  blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  blas_inputs.Flags = static_cast<
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
          kBuildFlagAllowUpdate | kBuildFlagAllowDisableOmms);
  blas_inputs.NumDescs = 1;
  blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  blas_inputs.pGeometryDescs = &omm_geometry;
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_prebuild = {};
  if (SUCCEEDED(hr)) {
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs,
                                                             &blas_prebuild);
    if (!blas_prebuild.ResultDataMaxSizeInBytes ||
        !blas_prebuild.ScratchDataSizeInBytes)
      hr = E_NOTIMPL;
    allow_disable_blas_prebuild_accepted =
        blas_prebuild.ResultDataMaxSizeInBytes != 0 &&
        blas_prebuild.ScratchDataSizeInBytes != 0;
  }
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       blas_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &blas_opaque);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       blas_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &blas_transparent);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       blas_prebuild.ScratchDataSizeInBytes,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &blas_scratch);

  // The same geometry is built twice, once with an opaque OMM array and once
  // with a transparent OMM array.  Each BLAS remains a real Metal triangle
  // AS; the provider changes the native geometry opacity bit from the decoded
  // OMM state rather than silently ignoring the linkage.
  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_prebuild = {};
  D3D12_RAYTRACING_INSTANCE_DESC instance_opaque = {};
  D3D12_RAYTRACING_INSTANCE_DESC instance_transparent = {};
  D3D12_RAYTRACING_INSTANCE_DESC instance_disabled = {};
  for (UINT row = 0; row < 3; ++row) {
    instance_opaque.Transform[row][row] = 1.0f;
    instance_transparent.Transform[row][row] = 1.0f;
    instance_disabled.Transform[row][row] = 1.0f;
  }
  instance_opaque.InstanceID = 0x004d4d54;
  instance_transparent.InstanceID = 0x004d4d54;
  instance_disabled.InstanceID = 0x004d4d54;
  instance_opaque.InstanceMask = 1;
  instance_transparent.InstanceMask = 1;
  instance_disabled.InstanceMask = 1;
  // FORCE_OMM_2_STATE is a no-op for this bounded two-state array.  The
  // disabled instance selects the provider's ordinary non-opaque BLAS
  // variant, which is only available because the BLAS opted in above.
  instance_transparent.Flags = kInstanceFlagForceOmm2State;
  instance_disabled.Flags = kInstanceFlagDisableOmms;
  if (SUCCEEDED(hr)) {
    // Create the upload buffers before assigning the instance GPUVAs.
    hr = upload_buffer(device, &instance_opaque, sizeof(instance_opaque),
                       &instances_opaque);
  }
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, &instance_transparent, sizeof(instance_transparent),
                       &instances_transparent);
  if (SUCCEEDED(hr))
    hr = upload_buffer(device, &instance_disabled, sizeof(instance_disabled),
                       &instances_disabled);
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
  tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  tlas_inputs.NumDescs = 1;
  tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  tlas_inputs.InstanceDescs = instances_opaque
                                  ? instances_opaque->GetGPUVirtualAddress()
                                  : 0;
  if (SUCCEEDED(hr)) {
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs,
                                                             &tlas_prebuild);
    if (!tlas_prebuild.ResultDataMaxSizeInBytes ||
        !tlas_prebuild.ScratchDataSizeInBytes)
      hr = E_NOTIMPL;
  }
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       tlas_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &tlas_opaque);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       tlas_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &tlas_transparent);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       tlas_prebuild.ResultDataMaxSizeInBytes,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       &tlas_disabled);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                       tlas_prebuild.ScratchDataSizeInBytes,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &tlas_scratch);
  if (SUCCEEDED(hr)) {
    instance_opaque.AccelerationStructure = blas_opaque->GetGPUVirtualAddress();
    instance_transparent.AccelerationStructure =
        blas_transparent->GetGPUVirtualAddress();
    instance_disabled.AccelerationStructure = blas_opaque->GetGPUVirtualAddress();
    void *mapped = nullptr;
    hr = instances_opaque->Map(0, nullptr, &mapped);
    if (SUCCEEDED(hr)) {
      std::memcpy(mapped, &instance_opaque, sizeof(instance_opaque));
      instances_opaque->Unmap(0, nullptr);
    }
    if (SUCCEEDED(hr)) {
      hr = instances_transparent->Map(0, nullptr, &mapped);
      if (SUCCEEDED(hr)) {
        std::memcpy(mapped, &instance_transparent, sizeof(instance_transparent));
        instances_transparent->Unmap(0, nullptr);
      }
    }
    if (SUCCEEDED(hr)) {
      hr = instances_disabled->Map(0, nullptr, &mapped);
      if (SUCCEEDED(hr)) {
        std::memcpy(mapped, &instance_disabled, sizeof(instance_disabled));
        instances_disabled->Unmap(0, nullptr);
      }
    }
  }

  ID3DBlob *root_blob = nullptr;
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable.NumDescriptorRanges = 2;
  parameter.DescriptorTable.pDescriptorRanges = ranges;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 1;
  root_desc.pParameters = &parameter;
  if (SUCCEEDED(hr))
    hr = serialize_root_signature(root_desc, &root_blob);
  if (SUCCEEDED(hr))
    hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                     root_blob->GetBufferSize(),
                                     IID_PPV_ARGS(&root));
  safe_release(root_blob);
  if (SUCCEEDED(hr)) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.CS.pShaderBytecode = accessor_shader.data();
    pso_desc.CS.BytecodeLength = accessor_shader.size();
    hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
  }
  if (SUCCEEDED(hr)) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = 6;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
  }
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 512,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &output);
  if (SUCCEEDED(hr))
    hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 192,
                       D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_FLAG_NONE, &readback);

  UINT descriptor_increment = 0;
  if (SUCCEEDED(hr)) {
    descriptor_increment =
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.RaytracingAccelerationStructure.Location =
        tlas_opaque->GetGPUVirtualAddress();
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    cpu.ptr += descriptor_increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 128;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
    cpu.ptr += descriptor_increment;
    srv.RaytracingAccelerationStructure.Location =
        tlas_transparent->GetGPUVirtualAddress();
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    cpu.ptr += descriptor_increment;
    device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
    cpu.ptr += descriptor_increment;
    srv.RaytracingAccelerationStructure.Location =
        tlas_disabled->GetGPUVirtualAddress();
    device->CreateShaderResourceView(nullptr, &srv, cpu);
    cpu.ptr += descriptor_increment;
    device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
  }

  uint64_t omm_opaque_size = 0;
  uint64_t omm_transparent_size = 0;
  uint32_t opaque_candidate_id = 0;
  uint32_t transparent_candidate_id = 0;
  uint32_t disabled_candidate_id = 0;
  uint32_t opaque_committed_status = 0;
  uint32_t transparent_committed_status = 0;
  uint32_t disabled_committed_status = 0;
  if (SUCCEEDED(hr)) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC omm_build = {};
    omm_build.ScratchAccelerationStructureData =
        omm_scratch->GetGPUVirtualAddress();
    omm_build.Inputs = omm_inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC omm_post = {};
    omm_post.InfoType =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    omm_post.DestBuffer = omm_postbuild_opaque->GetGPUVirtualAddress();
    omm_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &array_opaque);
    omm_build.Inputs = omm_inputs;
    omm_build.DestAccelerationStructureData =
        omm_array_opaque->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&omm_build, 1, &omm_post);
    omm_post.DestBuffer = omm_postbuild_transparent->GetGPUVirtualAddress();
    omm_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &array_transparent);
    omm_build.Inputs = omm_inputs;
    omm_build.DestAccelerationStructureData =
        omm_array_transparent->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&omm_build, 1, &omm_post);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build = {};
    blas_build.ScratchAccelerationStructureData =
        blas_scratch->GetGPUVirtualAddress();
    blas_build.Inputs = blas_inputs;
    blas_build.DestAccelerationStructureData = blas_opaque->GetGPUVirtualAddress();
    linkage_opaque.opacity_micromap_array =
        omm_array_opaque->GetGPUVirtualAddress();
    omm_triangles.linkage = &linkage_opaque;
    set_omm_geometry_union(omm_geometry, omm_triangles);
    blas_build.Inputs.pGeometryDescs = &omm_geometry;
    list4->BuildRaytracingAccelerationStructure(&blas_build, 0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_refit = blas_build;
    blas_refit.SourceAccelerationStructureData =
        blas_opaque->GetGPUVirtualAddress();
    blas_refit.DestAccelerationStructureData =
        blas_opaque->GetGPUVirtualAddress();
    blas_refit.Inputs.Flags = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(
            kBuildFlagAllowUpdate | kBuildFlagAllowDisableOmms |
            kBuildFlagPerformUpdate);
    list4->BuildRaytracingAccelerationStructure(&blas_refit, 0, nullptr);
    omm_refit_recorded = true;
    linkage_opaque.opacity_micromap_array =
        omm_array_transparent->GetGPUVirtualAddress();
    omm_triangles.linkage = &linkage_opaque;
    set_omm_geometry_union(omm_geometry, omm_triangles);
    blas_build.DestAccelerationStructureData =
        blas_transparent->GetGPUVirtualAddress();
    blas_build.Inputs.pGeometryDescs = &omm_geometry;
    list4->BuildRaytracingAccelerationStructure(&blas_build, 0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build = {};
    tlas_build.ScratchAccelerationStructureData =
        tlas_scratch->GetGPUVirtualAddress();
    tlas_build.Inputs = tlas_inputs;
    tlas_build.DestAccelerationStructureData = tlas_opaque->GetGPUVirtualAddress();
    tlas_build.Inputs.InstanceDescs = instances_opaque->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);
    tlas_build.DestAccelerationStructureData =
        tlas_transparent->GetGPUVirtualAddress();
    tlas_build.Inputs.InstanceDescs = instances_transparent->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);
    tlas_build.DestAccelerationStructureData =
        tlas_disabled->GetGPUVirtualAddress();
    tlas_build.Inputs.InstanceDescs = instances_disabled->GetGPUVirtualAddress();
    list4->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);

    ID3D12DescriptorHeap *heaps[] = {heap};
    list4->SetDescriptorHeaps(1, heaps);
    list4->SetComputeRootSignature(root);
    list4->SetPipelineState(pso);
    D3D12_GPU_DESCRIPTOR_HANDLE table = heap->GetGPUDescriptorHandleForHeapStart();
    list4->SetComputeRootDescriptorTable(0, table);
    list4->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER output_to_copy = transition_barrier(
        output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    list4->ResourceBarrier(1, &output_to_copy);
    list4->CopyBufferRegion(readback, 0, output, 0, sizeof(uint32_t) * 16);
    D3D12_RESOURCE_BARRIER output_to_uav = transition_barrier(
        output, D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list4->ResourceBarrier(1, &output_to_uav);
    table.ptr += descriptor_increment * 2;
    list4->SetComputeRootDescriptorTable(0, table);
    list4->Dispatch(1, 1, 1);
    list4->ResourceBarrier(1, &output_to_copy);
    list4->CopyBufferRegion(readback, sizeof(uint32_t) * 16, output, 0,
                            sizeof(uint32_t) * 16);
    list4->ResourceBarrier(1, &output_to_uav);
    table.ptr += descriptor_increment * 2;
    list4->SetComputeRootDescriptorTable(0, table);
    list4->Dispatch(1, 1, 1);
    list4->ResourceBarrier(1, &output_to_copy);
    list4->CopyBufferRegion(readback, sizeof(uint32_t) * 32, output, 0,
                            sizeof(uint32_t) * 16);
    // Prove that command recording owns the OMM CPU descriptors.  The replay
    // below must use the copied records rather than these deliberately
    // invalidated caller-owned objects.
    histogram.count = 64;
    histogram.format = 0xffff;
    array_opaque.input_buffer = 0;
    array_transparent.input_buffer = 0;
    triangles.IndexCount = 0;
    linkage_opaque.opacity_micromap_array = 0;
    omm_triangles.linkage = nullptr;
    hr = execute_and_wait(queue, base_list);
  }

  if (SUCCEEDED(hr)) {
    // A two-state descriptor carrying an unknown state must not be coerced to
    // opaque or transparent.  Use a separate destination and a sentinel
    // postbuild buffer so the negative result is observable without disturbing
    // the valid opaque/transparent arrays used by the visibility readback.
    void *mapped_input = nullptr;
    hr = omm_input->Map(0, nullptr, &mapped_input);
    if (SUCCEEDED(hr)) {
      static_cast<uint8_t *>(mapped_input)[0] = 2;
      omm_input->Unmap(0, nullptr);
    }
    const uint64_t unknown_sentinel = 0x5a5aa5a55a5aa5a5ull;
    uint64_t *mapped_unknown_postbuild = nullptr;
    if (SUCCEEDED(hr))
      hr = omm_postbuild_unknown->Map(
          0, nullptr, reinterpret_cast<void **>(&mapped_unknown_postbuild));
    if (SUCCEEDED(hr)) {
      *mapped_unknown_postbuild = unknown_sentinel;
      omm_postbuild_unknown->Unmap(0, nullptr);
    }
    OmmHistogramProbe unknown_histogram = {1, 0, kOmmFormatOc1TwoState};
    OmmArrayProbe unknown_array = {};
    unknown_array.histogram_entry_count = 1;
    unknown_array.histogram = &unknown_histogram;
    unknown_array.input_buffer = omm_input->GetGPUVirtualAddress();
    unknown_array.per_omm_descs.StartAddress =
        omm_descs->GetGPUVirtualAddress();
    unknown_array.per_omm_descs.StrideInBytes = sizeof(OmmDescProbe);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS unknown_inputs = {};
    unknown_inputs.Type = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE>(kOmmArrayType);
    unknown_inputs.NumDescs = 1;
    unknown_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    unknown_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &unknown_array);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC unknown_build = {};
    unknown_build.DestAccelerationStructureData =
        omm_array_unknown->GetGPUVirtualAddress();
    unknown_build.ScratchAccelerationStructureData =
        omm_scratch->GetGPUVirtualAddress();
    unknown_build.Inputs = unknown_inputs;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC unknown_post = {};
    unknown_post.InfoType =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    unknown_post.DestBuffer = omm_postbuild_unknown->GetGPUVirtualAddress();
    if (SUCCEEDED(hr))
      hr = base_list->Reset(allocator, nullptr);
    if (SUCCEEDED(hr)) {
      list4->BuildRaytracingAccelerationStructure(&unknown_build, 1,
                                                   &unknown_post);
      hr = execute_and_wait(queue, base_list);
    }
    uint64_t unknown_observed = 0;
    if (SUCCEEDED(hr)) {
      D3D12_RANGE range = {0, sizeof(unknown_observed)};
      void *mapped = nullptr;
      hr = omm_postbuild_unknown->Map(0, &range, &mapped);
      if (SUCCEEDED(hr)) {
        std::memcpy(&unknown_observed, mapped, sizeof(unknown_observed));
        omm_postbuild_unknown->Unmap(0, nullptr);
      }
    }
    unknown_state_rejected =
        SUCCEEDED(hr) && unknown_observed == unknown_sentinel;
    // Restore the upload byte for deterministic cleanup and any later
    // diagnostic replay, without changing the negative result above.
    if (SUCCEEDED(omm_input->Map(0, nullptr, &mapped_input))) {
      static_cast<uint8_t *>(mapped_input)[0] = 1;
      omm_input->Unmap(0, nullptr);
    }
  }

  if (SUCCEEDED(hr)) {
    OmmHistogramProbe invalid_histogram = {1, 0, 0xffff};
    OmmArrayProbe invalid_array = array_opaque;
    invalid_array.histogram = &invalid_histogram;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS invalid_inputs = {};
    invalid_inputs.Type = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE>(kOmmArrayType);
    invalid_inputs.NumDescs = 1;
    invalid_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    invalid_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &invalid_array);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC invalid_build = {};
    invalid_build.DestAccelerationStructureData =
        omm_array_opaque->GetGPUVirtualAddress();
    invalid_build.ScratchAccelerationStructureData =
        omm_scratch->GetGPUVirtualAddress();
    invalid_build.Inputs = invalid_inputs;
    reset_after_invalid_hr = base_list->Reset(allocator, nullptr);
    if (SUCCEEDED(reset_after_invalid_hr)) {
      list4->BuildRaytracingAccelerationStructure(&invalid_build, 0, nullptr);
      invalid_recording_close_hr = base_list->Close();
      reset_after_invalid_close_hr = base_list->Reset(allocator, nullptr);
      if (SUCCEEDED(reset_after_invalid_close_hr))
        reset_after_invalid_close_hr = base_list->Close();
    }
    if (reset_after_invalid_hr != S_OK ||
        invalid_recording_close_hr != E_INVALIDARG ||
        reset_after_invalid_close_hr != S_OK)
      hr = E_FAIL;
  }

  if (SUCCEEDED(hr)) {
    // OMM array results use the public 128-byte OMM alignment, while the
    // build scratch address remains a normal 256-byte AS address.  These
    // recordings must fail before queue replay rather than being rounded or
    // silently accepted by the provider.
    OmmHistogramProbe valid_recording_histogram = {
        1, 0, kOmmFormatOc1TwoState};
    OmmArrayProbe valid_recording_array = {};
    valid_recording_array.histogram_entry_count = 1;
    valid_recording_array.histogram = &valid_recording_histogram;
    valid_recording_array.input_buffer = omm_input->GetGPUVirtualAddress();
    valid_recording_array.per_omm_descs.StartAddress =
        omm_descs->GetGPUVirtualAddress();
    valid_recording_array.per_omm_descs.StrideInBytes = sizeof(OmmDescProbe);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS
        valid_recording_inputs = {};
    valid_recording_inputs.Type = static_cast<
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE>(kOmmArrayType);
    valid_recording_inputs.NumDescs = 1;
    valid_recording_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    valid_recording_inputs.pGeometryDescs =
        reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC *>(
            &valid_recording_array);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC misaligned_dest_build = {};
    misaligned_dest_build.DestAccelerationStructureData =
        omm_array_opaque->GetGPUVirtualAddress() + 64;
    misaligned_dest_build.ScratchAccelerationStructureData =
        omm_scratch->GetGPUVirtualAddress();
    misaligned_dest_build.Inputs = valid_recording_inputs;
    reset_after_misaligned_dest_hr = base_list->Reset(allocator, nullptr);
    if (SUCCEEDED(reset_after_misaligned_dest_hr)) {
      list4->BuildRaytracingAccelerationStructure(&misaligned_dest_build, 0,
                                                   nullptr);
      misaligned_dest_close_hr = base_list->Close();
      reset_after_misaligned_dest_close_hr =
          base_list->Reset(allocator, nullptr);
      if (SUCCEEDED(reset_after_misaligned_dest_close_hr))
        reset_after_misaligned_dest_close_hr = base_list->Close();
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC
        misaligned_scratch_build = misaligned_dest_build;
    misaligned_scratch_build.DestAccelerationStructureData =
        omm_array_opaque->GetGPUVirtualAddress();
    misaligned_scratch_build.ScratchAccelerationStructureData =
        omm_scratch->GetGPUVirtualAddress() + 128;
    reset_after_misaligned_scratch_hr = base_list->Reset(allocator, nullptr);
    if (SUCCEEDED(reset_after_misaligned_scratch_hr)) {
      list4->BuildRaytracingAccelerationStructure(&misaligned_scratch_build, 0,
                                                   nullptr);
      misaligned_scratch_close_hr = base_list->Close();
      reset_after_misaligned_scratch_close_hr =
          base_list->Reset(allocator, nullptr);
      if (SUCCEEDED(reset_after_misaligned_scratch_close_hr))
        reset_after_misaligned_scratch_close_hr = base_list->Close();
    }

    if (reset_after_misaligned_dest_hr != S_OK ||
        misaligned_dest_close_hr != E_INVALIDARG ||
        reset_after_misaligned_dest_close_hr != S_OK ||
        reset_after_misaligned_scratch_hr != S_OK ||
        misaligned_scratch_close_hr != E_INVALIDARG ||
        reset_after_misaligned_scratch_close_hr != S_OK)
      hr = E_FAIL;
  }

  if (SUCCEEDED(hr)) {
    void *mapped = nullptr;
    D3D12_RANGE range = {0, sizeof(uint32_t) * 48};
    hr = omm_postbuild_opaque->Map(0, &range, &mapped);
    if (SUCCEEDED(hr)) {
      std::memcpy(&omm_opaque_size, mapped, sizeof(omm_opaque_size));
      omm_postbuild_opaque->Unmap(0, nullptr);
    }
    if (SUCCEEDED(hr)) {
      hr = omm_postbuild_transparent->Map(0, &range, &mapped);
      if (SUCCEEDED(hr)) {
        std::memcpy(&omm_transparent_size, mapped,
                    sizeof(omm_transparent_size));
        omm_postbuild_transparent->Unmap(0, nullptr);
      }
    }
    if (SUCCEEDED(hr)) {
      hr = readback->Map(0, &range, &mapped);
      if (SUCCEEDED(hr)) {
        const auto *words = static_cast<const uint32_t *>(mapped);
        opaque_candidate_id = words[2];
        opaque_committed_status = words[15];
        transparent_candidate_id = words[18];
        transparent_committed_status = words[31];
        disabled_candidate_id = words[34];
        disabled_committed_status = words[47];
        readback->Unmap(0, nullptr);
      }
    }
  }

  const bool options_verified =
      SUCCEEDED(options22_hr) &&
      options22.shader_execution_reordering_actually_reorders == FALSE &&
      options22_bad_size_hr == E_INVALIDARG;
  const bool sizes_verified =
      omm_opaque_size == omm_prebuild.ResultDataMaxSizeInBytes &&
      omm_transparent_size == omm_prebuild.ResultDataMaxSizeInBytes;
  const bool alignment_verified =
      omm_input && omm_descs && omm_scratch && omm_array_opaque &&
      omm_array_transparent &&
      (omm_input->GetGPUVirtualAddress() & 127u) == 0 &&
      (omm_descs->GetGPUVirtualAddress() & 3u) == 0 &&
      (omm_scratch->GetGPUVirtualAddress() & 255u) == 0 &&
      (omm_array_opaque->GetGPUVirtualAddress() & 127u) == 0 &&
      (omm_array_transparent->GetGPUVirtualAddress() & 127u) == 0 &&
      aligned_128_input_accepted && invalid_input_alignment_rejected &&
      invalid_desc_alignment_rejected;
  const bool alignment_recording_verified =
      reset_after_misaligned_dest_hr == S_OK &&
      misaligned_dest_close_hr == E_INVALIDARG &&
      reset_after_misaligned_dest_close_hr == S_OK &&
      reset_after_misaligned_scratch_hr == S_OK &&
      misaligned_scratch_close_hr == E_INVALIDARG &&
      reset_after_misaligned_scratch_close_hr == S_OK;
  const bool lifetime_verified = sizes_verified;
  const bool config1_verified = config1_omm_accepted && config1_skip_rejected;
  const bool recording_error_verified =
      reset_after_invalid_hr == S_OK &&
      invalid_recording_close_hr == E_INVALIDARG &&
      reset_after_invalid_close_hr == S_OK;
  // Accessor shader offset 8 is CandidateInstanceID.  Opaque geometry never
  // enters the NON_OPAQUE branch; transparent OMM does, so the same exact
  // readback distinguishes the two provider states without CPU scheduling.
  instance_force_omm2_state_verified =
      transparent_candidate_id == 0x004d4d54 &&
      transparent_committed_status != 0;
  instance_disable_omms_verified =
      disabled_candidate_id == 0x004d4d54 && disabled_committed_status != 0;
  const bool visibility_verified =
      opaque_candidate_id == 0 && transparent_candidate_id == 0x004d4d54 &&
      disabled_candidate_id == 0x004d4d54 && opaque_committed_status != 0 &&
      transparent_committed_status != 0 && disabled_committed_status != 0;
  const bool verified = SUCCEEDED(hr) && options_verified && sizes_verified &&
                        visibility_verified && invalid_format_rejected &&
                        invalid_layout_rejected && invalid_four_state_rejected &&
                        invalid_update_flags_rejected &&
                        invalid_disable_flags_rejected &&
                        allow_disable_blas_prebuild_accepted &&
                        unknown_state_rejected && lifetime_verified &&
                        alignment_verified && alignment_recording_verified &&
                        config1_verified && omm_refit_recorded &&
                        instance_force_omm2_state_verified &&
                        instance_disable_omms_verified && recording_error_verified;
  const std::string extra =
      "\"config1_omm_hr\":\"" + hr_hex(config1_omm_hr) +
      "\",\"config1_skip_hr\":\"" + hr_hex(config1_skip_hr) +
      "\",\"config1_omm_accepted\":" +
      (config1_omm_accepted ? "true" : "false") +
      ",\"config1_skip_rejected\":" +
      (config1_skip_rejected ? "true" : "false") +
      ",\"options22_hr\":\"" + hr_hex(options22_hr) +
      "\",\"options22_bad_size_hr\":\"" + hr_hex(options22_bad_size_hr) +
      "\",\"shader_execution_reordering_actually_reorders\":" +
      (options22.shader_execution_reordering_actually_reorders ? "true" : "false") +
      ",\"omm_prebuild_result_bytes\":" +
      std::to_string(omm_prebuild.ResultDataMaxSizeInBytes) +
      ",\"omm_prebuild_scratch_bytes\":" +
      std::to_string(omm_prebuild.ScratchDataSizeInBytes) +
      ",\"omm_opaque_current_size_bytes\":" +
      std::to_string(omm_opaque_size) +
      ",\"omm_transparent_current_size_bytes\":" +
      std::to_string(omm_transparent_size) +
      ",\"opaque_candidate_instance_id\":" +
      std::to_string(opaque_candidate_id) +
      ",\"transparent_candidate_instance_id\":" +
      std::to_string(transparent_candidate_id) +
      ",\"disabled_candidate_instance_id\":" +
      std::to_string(disabled_candidate_id) +
      ",\"opaque_committed_status\":" +
      std::to_string(opaque_committed_status) +
      ",\"transparent_committed_status\":" +
      std::to_string(transparent_committed_status) +
      ",\"disabled_committed_status\":" +
      std::to_string(disabled_committed_status) +
      ",\"options22_verified\":" + (options_verified ? "true" : "false") +
      ",\"omm_sizes_verified\":" + (sizes_verified ? "true" : "false") +
      ",\"omm_alignment_verified\":" +
      (alignment_verified ? "true" : "false") +
      ",\"omm_aligned_128_input_accepted\":" +
      (aligned_128_input_accepted ? "true" : "false") +
      ",\"omm_invalid_input_alignment_rejected\":" +
      (invalid_input_alignment_rejected ? "true" : "false") +
      ",\"omm_invalid_desc_alignment_rejected\":" +
      (invalid_desc_alignment_rejected ? "true" : "false") +
      ",\"omm_alignment_recording_verified\":" +
      (alignment_recording_verified ? "true" : "false") +
      ",\"omm_visibility_verified\":" +
      (visibility_verified ? "true" : "false") +
      ",\"omm_invalid_format_rejected\":" +
      (invalid_format_rejected ? "true" : "false") +
      ",\"omm_invalid_layout_rejected\":" +
      (invalid_layout_rejected ? "true" : "false") +
      ",\"omm_invalid_four_state_rejected\":" +
      (invalid_four_state_rejected ? "true" : "false") +
      ",\"omm_invalid_update_flags_rejected\":" +
      (invalid_update_flags_rejected ? "true" : "false") +
      ",\"omm_invalid_disable_flags_rejected\":" +
      (invalid_disable_flags_rejected ? "true" : "false") +
      ",\"omm_allow_disable_blas_prebuild_accepted\":" +
      (allow_disable_blas_prebuild_accepted ? "true" : "false") +
      ",\"omm_instance_force_omm2_state_verified\":" +
      (instance_force_omm2_state_verified ? "true" : "false") +
      ",\"omm_instance_disable_omms_verified\":" +
      (instance_disable_omms_verified ? "true" : "false") +
      ",\"omm_unknown_state_rejected\":" +
      (unknown_state_rejected ? "true" : "false") +
      ",\"omm_command_lifetime_verified\":" +
      (lifetime_verified ? "true" : "false") +
      ",\"omm_refit_recorded\":" +
      (omm_refit_recorded ? "true" : "false") +
      ",\"config1_verified\":" +
      (config1_verified ? "true" : "false") +
      ",\"invalid_recording_close_hr\":\"" +
      hr_hex(invalid_recording_close_hr) +
      "\",\"reset_after_invalid_hr\":\"" +
      hr_hex(reset_after_invalid_hr) +
      "\",\"reset_after_invalid_close_hr\":\"" +
      hr_hex(reset_after_invalid_close_hr) +
      "\",\"misaligned_dest_close_hr\":\"" +
      hr_hex(misaligned_dest_close_hr) +
      "\",\"reset_after_misaligned_dest_hr\":\"" +
      hr_hex(reset_after_misaligned_dest_hr) +
      "\",\"reset_after_misaligned_dest_close_hr\":\"" +
      hr_hex(reset_after_misaligned_dest_close_hr) +
      "\",\"misaligned_scratch_close_hr\":\"" +
      hr_hex(misaligned_scratch_close_hr) +
      "\",\"reset_after_misaligned_scratch_hr\":\"" +
      hr_hex(reset_after_misaligned_scratch_hr) +
      "\",\"reset_after_misaligned_scratch_close_hr\":\"" +
      hr_hex(reset_after_misaligned_scratch_close_hr) +
      "\",\"omm_recording_error_verified\":" +
      (recording_error_verified && alignment_recording_verified ? "true" :
       "false");

  safe_release(config1_skip_state);
  safe_release(config1_omm_state);
  safe_release(readback);
  safe_release(output);
  safe_release(heap);
  safe_release(pso);
  safe_release(root);
  safe_release(tlas_scratch);
  safe_release(tlas_disabled);
  safe_release(tlas_transparent);
  safe_release(tlas_opaque);
  safe_release(instances_disabled);
  safe_release(instances_transparent);
  safe_release(instances_opaque);
  safe_release(blas_scratch);
  safe_release(blas_transparent);
  safe_release(blas_opaque);
  safe_release(omm_postbuild_unknown);
  safe_release(omm_postbuild_transparent);
  safe_release(omm_postbuild_opaque);
  safe_release(omm_scratch);
  safe_release(omm_array_unknown);
  safe_release(omm_array_transparent);
  safe_release(omm_array_opaque);
  safe_release(omm_descs);
  safe_release(omm_input);
  safe_release(indices);
  safe_release(vertices);
  safe_release(list4);
  safe_release(base_list);
  safe_release(allocator);
  safe_release(queue);
  safe_release(device5);
  safe_release(device);
  return {verified,
          verified ? S_OK : hr,
          verified ? "OMM provider decoded opaque/transparent level-0 states, rejected four-state/unknown/update-disable boundaries, reported exact postbuild sizes, proved inline ray-query visibility, and preserved OPTIONS22 SER truthfulness"
                   : "OMM provider or OPTIONS22/SER truthfulness gate failed",
          extra};
}

int main() {
  ProbeResult result = probe_omm_ser();
  std::printf("{\n");
  std::printf("  \"schema\": \"metalsharp.d3d12-metal.omm-ser.v1\",\n");
  std::printf("  \"probe\": \"dxr_omm_ser\",\n");
  std::printf("  \"ok\": %s,\n", result.ok ? "true" : "false");
  std::printf("  \"hr\": \"%s\",\n", hr_hex(result.hr).c_str());
  std::printf("  \"detail\": \"%s\"", json_escape(result.detail).c_str());
  if (!result.extra.empty())
    std::printf(",\n  %s", result.extra.c_str());
  std::printf("\n}\n");
  return result.ok ? 0 : 1;
}
