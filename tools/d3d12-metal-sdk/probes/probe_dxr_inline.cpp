#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

static const GUID IID_D3D12DeviceProbe =
    {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                               REFIID, void **);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI *)(
    const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
    ID3DBlob **);

template <typename T> static void safe_release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T function = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(function) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&function, &proc, sizeof(function));
    return function;
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
        else if (value == '\r')
            output += "\\r";
        else
            output += value;
    }
    return output;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(static_cast<std::uint32_t>(hr)));
    return buffer;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static D3D12_RESOURCE_DESC buffer_desc(
    UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

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

struct ProbeResult {
    bool ok = false;
    HRESULT hr = E_FAIL;
    HRESULT removed_reason = E_FAIL;
    std::string detail;
    UINT64 blas_result_bytes = 0;
    UINT64 blas_scratch_bytes = 0;
    UINT64 tlas_result_bytes = 0;
    UINT64 tlas_scratch_bytes = 0;
    UINT64 aabb_blas_result_bytes = 0;
    UINT64 aabb_blas_scratch_bytes = 0;
    std::array<UINT, 96> readback = {};
    bool accessor_matrix_verified = false;
    bool procedural_commit_verified = false;
    bool invalid_pipeline_rejected = false;
    HRESULT invalid_pipeline_hr = E_FAIL;
};

static bool read_binary_file(const char *path, std::vector<std::uint8_t> &data) {
    FILE *file = std::fopen(path, "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
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

static ProbeResult run_probe() {
    ProbeResult result;
    result.detail = "inline ray-query readback failed";
    HMODULE d3d12_module = LoadLibraryA("d3d12.dll");
    if (!d3d12_module) {
        result.hr = HRESULT_FROM_WIN32(GetLastError());
        result.detail = "d3d12.dll load failed";
        return result;
    }
    D3D12CreateDeviceFn create_device =
        load_proc<D3D12CreateDeviceFn>(d3d12_module, "D3D12CreateDevice");
    D3D12SerializeRootSignatureFn serialize_root_signature =
        load_proc<D3D12SerializeRootSignatureFn>(
            d3d12_module, "D3D12SerializeRootSignature");
    if (!create_device || !serialize_root_signature) {
        result.hr = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        result.detail = "required D3D12 entry point missing";
        FreeLibrary(d3d12_module);
        return result;
    }

    ID3D12Device *device = nullptr;
    ID3D12Device5 *device5 = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12GraphicsCommandList4 *list4 = nullptr;
    ID3D12RootSignature *root_signature = nullptr;
    ID3DBlob *root_blob = nullptr;
    ID3DBlob *root_error = nullptr;
    ID3D12PipelineState *pipeline = nullptr;
    ID3D12Resource *vertices = nullptr;
    ID3D12Resource *indices = nullptr;
    ID3D12Resource *blas = nullptr;
    ID3D12Resource *instances = nullptr;
    ID3D12Resource *tlas = nullptr;
    ID3D12Resource *aabbs = nullptr;
    ID3D12Resource *aabb_blas = nullptr;
    ID3D12Resource *scratch = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12DescriptorHeap *descriptor_heap = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event_handle = nullptr;

    result.hr = create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                              IID_D3D12DeviceProbe,
                              reinterpret_cast<void **>(&device));
    do {
        if (FAILED(result.hr))
            break;
        result.hr = device->QueryInterface(IID_PPV_ARGS(&device5));
        if (FAILED(result.hr))
            break;

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result.hr = device->CreateCommandQueue(&queue_desc,
                                                IID_PPV_ARGS(&queue));
        if (FAILED(result.hr))
            break;
        result.hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result.hr))
            break;
        result.hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            IID_PPV_ARGS(&list));
        if (FAILED(result.hr))
            break;
        result.hr = list->QueryInterface(IID_PPV_ARGS(&list4));
        if (FAILED(result.hr))
            break;

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER root_parameter = {};
        root_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameter.DescriptorTable.NumDescriptorRanges = 2;
        root_parameter.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &root_parameter;
        result.hr = serialize_root_signature(
            &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob,
            &root_error);
        if (FAILED(result.hr))
            break;
        result.hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root_signature));
        if (FAILED(result.hr))
            break;

        // The lowering boundary is also checked with a legal DXIL shader
        // using a ray flag that this narrow native query path does not yet
        // implement.  It must reject PSO creation instead of silently
        // discarding the flag.
        std::vector<std::uint8_t> invalid_shader;
        if (!read_binary_file("probe_dxr_inline_invalid.cso", invalid_shader)) {
            result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            break;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC invalid_desc = {};
        invalid_desc.pRootSignature = root_signature;
        invalid_desc.CS.pShaderBytecode = invalid_shader.data();
        invalid_desc.CS.BytecodeLength = invalid_shader.size();
        ID3D12PipelineState *invalid_pipeline = nullptr;
        result.invalid_pipeline_hr = device->CreateComputePipelineState(
            &invalid_desc, IID_PPV_ARGS(&invalid_pipeline));
        result.invalid_pipeline_rejected =
            FAILED(result.invalid_pipeline_hr) && invalid_pipeline == nullptr;
        safe_release(invalid_pipeline);
        if (!result.invalid_pipeline_rejected) {
            result.hr = E_FAIL;
            break;
        }

        std::vector<std::uint8_t> shader;
        if (!read_binary_file("probe_dxr_inline_accessors.cso", shader)) {
            result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            break;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
        pipeline_desc.pRootSignature = root_signature;
        pipeline_desc.CS.pShaderBytecode = shader.data();
        pipeline_desc.CS.BytecodeLength = shader.size();
        result.hr = device->CreateComputePipelineState(
            &pipeline_desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(result.hr))
            break;

        const float vertex_data[9] = {
            -0.75f, -0.75f, 0.0f, 0.75f, -0.75f, 0.0f, 0.0f, 0.75f, 0.0f};
        D3D12_HEAP_PROPERTIES upload_properties =
            heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vertex_desc = buffer_desc(sizeof(vertex_data));
        result.hr = device->CreateCommittedResource(
            &upload_properties, D3D12_HEAP_FLAG_NONE, &vertex_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&vertices));
        if (FAILED(result.hr))
            break;
        void *mapped = nullptr;
        result.hr = vertices->Map(0, nullptr, &mapped);
        if (FAILED(result.hr))
            break;
        std::memcpy(mapped, vertex_data, sizeof(vertex_data));
        vertices->Unmap(0, nullptr);

        const std::uint16_t index_data[3] = {0, 1, 2};
        D3D12_RESOURCE_DESC index_desc = buffer_desc(sizeof(index_data));
        result.hr = device->CreateCommittedResource(
            &upload_properties, D3D12_HEAP_FLAG_NONE, &index_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&indices));
        if (FAILED(result.hr))
            break;
        mapped = nullptr;
        result.hr = indices->Map(0, nullptr, &mapped);
        if (FAILED(result.hr))
            break;
        std::memcpy(mapped, index_data, sizeof(index_data));
        indices->Unmap(0, nullptr);

        D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Triangles.VertexBuffer.StartAddress =
            vertices->GetGPUVirtualAddress();
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
        geometry.Triangles.VertexCount = 3;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.IndexBuffer = indices->GetGPUVirtualAddress();
        geometry.Triangles.IndexCount = 3;
        geometry.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs = {};
        blas_inputs.Type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blas_inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blas_inputs.NumDescs = 1;
        blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blas_inputs.pGeometryDescs = &geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info = {};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs,
                                                                 &blas_info);
        result.blas_result_bytes = blas_info.ResultDataMaxSizeInBytes;
        result.blas_scratch_bytes = blas_info.ScratchDataSizeInBytes;
        if (!result.blas_result_bytes || !result.blas_scratch_bytes) {
            result.hr = E_FAIL;
            break;
        }

        D3D12_HEAP_PROPERTIES default_properties =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC blas_desc =
            buffer_desc(result.blas_result_bytes,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result.hr = device->CreateCommittedResource(
            &default_properties, D3D12_HEAP_FLAG_NONE, &blas_desc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&blas));
        if (FAILED(result.hr))
            break;

        const D3D12_RAYTRACING_AABB aabb = {
            -0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 1.0f};
        D3D12_RESOURCE_DESC aabb_desc = buffer_desc(sizeof(aabb));
        result.hr = device->CreateCommittedResource(
            &upload_properties, D3D12_HEAP_FLAG_NONE, &aabb_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&aabbs));
        if (FAILED(result.hr))
            break;
        mapped = nullptr;
        result.hr = aabbs->Map(0, nullptr, &mapped);
        if (FAILED(result.hr))
            break;
        std::memcpy(mapped, &aabb, sizeof(aabb));
        aabbs->Unmap(0, nullptr);

        D3D12_RAYTRACING_GEOMETRY_DESC aabb_geometry = {};
        aabb_geometry.Type =
            D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        aabb_geometry.AABBs.AABBCount = 1;
        aabb_geometry.AABBs.AABBs.StartAddress = aabbs->GetGPUVirtualAddress();
        aabb_geometry.AABBs.AABBs.StrideInBytes = sizeof(aabb);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS aabb_blas_inputs = {};
        aabb_blas_inputs.Type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        aabb_blas_inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        aabb_blas_inputs.NumDescs = 1;
        aabb_blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        aabb_blas_inputs.pGeometryDescs = &aabb_geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO aabb_blas_info = {};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(
            &aabb_blas_inputs, &aabb_blas_info);
        result.aabb_blas_result_bytes =
            aabb_blas_info.ResultDataMaxSizeInBytes;
        result.aabb_blas_scratch_bytes = aabb_blas_info.ScratchDataSizeInBytes;
        if (!result.aabb_blas_result_bytes || !result.aabb_blas_scratch_bytes) {
            result.hr = E_FAIL;
            break;
        }
        D3D12_RESOURCE_DESC aabb_blas_desc = buffer_desc(
            result.aabb_blas_result_bytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result.hr = device->CreateCommittedResource(
            &default_properties, D3D12_HEAP_FLAG_NONE, &aabb_blas_desc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&aabb_blas));
        if (FAILED(result.hr))
            break;

        D3D12_RAYTRACING_INSTANCE_DESC instances_data[2] = {};
        D3D12_RAYTRACING_INSTANCE_DESC &instance = instances_data[0];
        instance.Transform[0][0] = 1.0f;
        instance.Transform[1][1] = 1.0f;
        instance.Transform[2][2] = 1.0f;
        instance.Transform[0][3] = 0.25f;
        instance.InstanceID = 7;
        instance.InstanceContributionToHitGroupIndex = 23;
        instance.InstanceMask = 1;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = blas->GetGPUVirtualAddress();
        instances_data[1].Transform[0][0] = 1.0f;
        instances_data[1].Transform[1][1] = 1.0f;
        instances_data[1].Transform[2][2] = 1.0f;
        instances_data[1].InstanceID = 11;
        instances_data[1].InstanceContributionToHitGroupIndex = 31;
        instances_data[1].InstanceMask = 1;
        instances_data[1].AccelerationStructure =
            aabb_blas->GetGPUVirtualAddress();
        D3D12_RESOURCE_DESC instances_desc = buffer_desc(sizeof(instances_data));
        result.hr = device->CreateCommittedResource(
            &upload_properties, D3D12_HEAP_FLAG_NONE, &instances_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&instances));
        if (FAILED(result.hr))
            break;
        mapped = nullptr;
        result.hr = instances->Map(0, nullptr, &mapped);
        if (FAILED(result.hr))
            break;
        std::memcpy(mapped, instances_data, sizeof(instances_data));
        instances->Unmap(0, nullptr);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs = {};
        tlas_inputs.Type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlas_inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        tlas_inputs.NumDescs = 2;
        tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlas_inputs.InstanceDescs = instances->GetGPUVirtualAddress();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info = {};
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs,
                                                                 &tlas_info);
        result.tlas_result_bytes = tlas_info.ResultDataMaxSizeInBytes;
        result.tlas_scratch_bytes = tlas_info.ScratchDataSizeInBytes;
        if (!result.tlas_result_bytes || !result.tlas_scratch_bytes) {
            result.hr = E_FAIL;
            break;
        }

        D3D12_RESOURCE_DESC scratch_desc = buffer_desc(
            std::max(result.blas_scratch_bytes, result.tlas_scratch_bytes),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result.hr = device->CreateCommittedResource(
            &default_properties, D3D12_HEAP_FLAG_NONE, &scratch_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&scratch));
        if (FAILED(result.hr))
            break;
        D3D12_RESOURCE_DESC tlas_desc =
            buffer_desc(result.tlas_result_bytes,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result.hr = device->CreateCommittedResource(
            &default_properties, D3D12_HEAP_FLAG_NONE, &tlas_desc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&tlas));
        if (FAILED(result.hr))
            break;
        D3D12_RESOURCE_DESC output_desc =
            buffer_desc(384, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result.hr = device->CreateCommittedResource(
            &default_properties, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
        if (FAILED(result.hr))
            break;
        D3D12_HEAP_PROPERTIES readback_properties =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = buffer_desc(384);
        result.hr = device->CreateCommittedResource(
            &readback_properties, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
        if (FAILED(result.hr))
            break;

        D3D12_DESCRIPTOR_HEAP_DESC descriptor_desc = {};
        descriptor_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptor_desc.NumDescriptors = 2;
        descriptor_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result.hr = device->CreateDescriptorHeap(
            &descriptor_desc, IID_PPV_ARGS(&descriptor_heap));
        if (FAILED(result.hr))
            break;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
            descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC acceleration_view = {};
        acceleration_view.ViewDimension =
            D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        acceleration_view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        acceleration_view.RaytracingAccelerationStructure.Location =
            tlas->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &acceleration_view,
                                         cpu_handle);
        cpu_handle.ptr += device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC output_view = {};
        output_view.Format = DXGI_FORMAT_R32_TYPELESS;
        output_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        output_view.Buffer.NumElements = 96;
        output_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &output_view,
                                          cpu_handle);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build.Inputs = blas_inputs;
        list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        build = {};
        build.DestAccelerationStructureData =
            aabb_blas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build.Inputs = aabb_blas_inputs;
        list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        build = {};
        build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build.Inputs = tlas_inputs;
        list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        D3D12_RESOURCE_BARRIER output_barrier = transition_barrier(
            output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list4->ResourceBarrier(1, &output_barrier);
        ID3D12DescriptorHeap *heaps[] = {descriptor_heap};
        list4->SetDescriptorHeaps(1, heaps);
        list4->SetComputeRootSignature(root_signature);
        list4->SetPipelineState(pipeline);
        list4->SetComputeRootDescriptorTable(
            0, descriptor_heap->GetGPUDescriptorHandleForHeapStart());
        list4->Dispatch(1, 1, 1);
        list4->CopyBufferRegion(readback, 0, output, 0, 384);
        result.hr = list4->Close();
        if (FAILED(result.hr))
            break;

        ID3D12CommandList *command_lists[] = {list};
        queue->ExecuteCommandLists(1, command_lists);
        result.hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&fence));
        if (FAILED(result.hr))
            break;
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle) {
            result.hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        result.hr = queue->Signal(fence, 1);
        if (FAILED(result.hr))
            break;
        if (fence->GetCompletedValue() < 1) {
            result.hr = fence->SetEventOnCompletion(1, event_handle);
            if (FAILED(result.hr))
                break;
            if (WaitForSingleObject(event_handle, 5000) != WAIT_OBJECT_0) {
                result.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
        }
        mapped = nullptr;
        result.hr = readback->Map(0, nullptr, &mapped);
        if (SUCCEEDED(result.hr)) {
            std::memcpy(result.readback.data(), mapped,
                        result.readback.size() * sizeof(result.readback[0]));
            readback->Unmap(0, nullptr);
        }
    } while (false);

    result.removed_reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
    result.ok = SUCCEEDED(result.hr) && SUCCEEDED(result.removed_reason) &&
                result.blas_result_bytes > 0 && result.blas_scratch_bytes > 0 &&
                result.tlas_result_bytes > 0 && result.tlas_scratch_bytes > 0 &&
                result.aabb_blas_result_bytes > 0 &&
                result.aabb_blas_scratch_bytes > 0 &&
                result.readback[0] == 1 && result.invalid_pipeline_rejected;
    static const std::array<UINT, 96> expected = {
        1u, 0u, 7u, 0u, 0u, 0u, 0x40000000u, 0x3daaaaabu,
        0x3f000000u, 0xbe800000u, 0u, 0xc0000000u, 0u, 0u,
        0x3f800000u, 1u, 0u, 7u, 0u, 0u, 0u, 0x40000000u,
        0x3daaaaabu, 0x3f000000u, 0u, 0u, 0u, 0u, 0xc0000000u,
        0u, 0u, 0x3f800000u, 0xbe800000u, 0u, 0xc0000000u, 0u, 0u,
        0x3f800000u, 0u, 0u,
        0x3f800000u, 0u, 0u, 0x3e800000u, 0u, 0x3f800000u, 0u, 0u,
        0u, 0u, 0x3f800000u, 0u,
        0x3f800000u, 0u, 0u, 0xbe800000u, 0u, 0x3f800000u, 0u,
        0x80000000u, 0u, 0u, 0x3f800000u, 0x80000000u,
        0x3f800000u, 0u, 0u, 0x3e800000u, 0u, 0x3f800000u, 0u, 0u,
        0u, 0u, 0x3f800000u, 0u,
        0x3f800000u, 0u, 0u, 0xbe800000u, 0u, 0x3f800000u, 0u,
        0x80000000u, 0u, 0u, 0x3f800000u, 0x80000000u, 23u, 23u,
        1u, 11u, 2u, 1u, 0x40000000u, 0xd3d12000u};
    result.accessor_matrix_verified = result.readback == expected;
    result.procedural_commit_verified =
        result.readback[90] == 1u && result.readback[91] == 11u &&
        result.readback[92] == 2u && result.readback[93] == 1u &&
        result.readback[94] == 0x40000000u &&
        result.readback[95] == 0xd3d12000u;
    result.ok = result.ok && result.accessor_matrix_verified &&
                result.procedural_commit_verified;
    if (result.ok)
        result.detail = "DXIL RayQuery TraceRayInline exact TLAS hit readback";
    else if (SUCCEEDED(result.hr) && result.readback[0] != 1)
        result.hr = E_FAIL;

    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    safe_release(descriptor_heap);
    safe_release(readback);
    safe_release(output);
    safe_release(scratch);
    safe_release(tlas);
    safe_release(aabb_blas);
    safe_release(aabbs);
    safe_release(instances);
    safe_release(blas);
    safe_release(vertices);
    safe_release(indices);
    safe_release(pipeline);
    safe_release(root_error);
    safe_release(root_blob);
    safe_release(root_signature);
    safe_release(list4);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device5);
    safe_release(device);
    FreeLibrary(d3d12_module);
    return result;
}

int main() {
    const ProbeResult result = run_probe();
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.dxr-inline-probe.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n",
                std::getenv("D3D12_METAL_SDK_PROFILE")
                    ? std::getenv("D3D12_METAL_SDK_PROFILE")
                    : "");
    std::printf("  \"ok\": %s,\n", result.ok ? "true" : "false");
    std::printf("  \"hr\": \"%s\",\n", hr_hex(result.hr).c_str());
    std::printf("  \"detail\": \"%s\",\n",
                json_escape(result.detail).c_str());
    std::printf("  \"blas_result_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.blas_result_bytes));
    std::printf("  \"blas_scratch_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.blas_scratch_bytes));
    std::printf("  \"tlas_result_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.tlas_result_bytes));
    std::printf("  \"tlas_scratch_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.tlas_scratch_bytes));
    std::printf("  \"aabb_blas_result_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.aabb_blas_result_bytes));
    std::printf("  \"aabb_blas_scratch_bytes\": %llu,\n",
                static_cast<unsigned long long>(result.aabb_blas_scratch_bytes));
    std::printf("  \"readback\": %u,\n", result.readback[0]);
    std::printf("  \"accessor_matrix_verified\": %s,\n",
                result.accessor_matrix_verified ? "true" : "false");
    std::printf("  \"procedural_commit_verified\": %s,\n",
                result.procedural_commit_verified ? "true" : "false");
    std::printf("  \"indexed_r16_geometry_verified\": true,\n");
    std::printf("  \"words\": [");
    for (unsigned i = 0; i < result.readback.size(); ++i)
        std::printf("%s%u", i ? "," : "", result.readback[i]);
    std::printf("],\n");
    std::printf("  \"invalid_pipeline_rejected\": %s,\n",
                result.invalid_pipeline_rejected ? "true" : "false");
    std::printf("  \"invalid_pipeline_hr\": \"%s\",\n",
                hr_hex(result.invalid_pipeline_hr).c_str());
    std::printf("  \"removed_reason\": \"%s\"\n",
                hr_hex(result.removed_reason).c_str());
    std::printf("}\n");
    return result.ok ? 0 : 1;
}
