#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
#define DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ((DXGI_FORMAT)189)
#endif
#ifndef DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE
#define DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE ((DXGI_FORMAT)190)
#endif

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                  ID3DBlob**, ID3DBlob**);

template <typename T> static void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

template <typename T> static T proc(HMODULE module, const char* name) {
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    T result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static bool write_file(const char* path, const char* text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const DWORD length = static_cast<DWORD>(std::strlen(text));
    const bool ok = WriteFile(file, text, length, &written, nullptr) && written == length;
    CloseHandle(file);
    return ok;
}

static std::vector<uint8_t> read_file(const char* path) {
    HANDLE file =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    data.resize(read);
    return data;
}

static DWORD run_process(const char* command_line) {
    std::vector<char> command(command_line, command_line + std::strlen(command_line) + 1);
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process))
        return UINT32_MAX;
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = UINT32_MAX;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = nullptr;
    if (SUCCEEDED(hr))
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr)) {
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event)
        CloseHandle(event);
    release(fence);
    return hr;
}

int main() {
    const char* shader_path = "Z:\\tmp\\metalsharp_sampler_feedback.hlsl";
    const char* dxil_path = "Z:\\tmp\\metalsharp_sampler_feedback.cso";
    const char* shader = R"HLSL(
Texture2D<float4> paired_texture : register(t0);
Texture2DArray<float4> paired_array : register(t1);
SamplerState paired_sampler : register(s0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedback : register(u0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> contention_feedback : register(u1);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIP_REGION_USED> used_feedback : register(u2);
FeedbackTexture2DArray<SAMPLER_FEEDBACK_MIN_MIP> array_feedback : register(u3);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 cell = id.xy & 3;
  float2 uv = (float2(cell) + 0.5) / 4.0;
  feedback.WriteSamplerFeedbackLevel(
      paired_texture, paired_sampler, uv, float((cell.x + 2 * cell.y) & 3));
  contention_feedback.WriteSamplerFeedbackLevel(
      paired_texture, paired_sampler, float2(0.125, 0.125),
      float((id.x + id.y) & 3));
  used_feedback.WriteSamplerFeedbackLevel(
      paired_texture, paired_sampler, uv, float((cell.x + 2 * cell.y) & 3));
  uint layer = (id.x >> 2) & 1;
  array_feedback.WriteSamplerFeedbackLevel(
      paired_array, paired_sampler, float3(uv, float(layer)),
      float((cell.x + 2 * cell.y + layer) & 3));
}
)HLSL";

    HRESULT hr = E_FAIL;
    DWORD dxc_exit = UINT32_MAX;
    bool device8_ok = false;
    bool format_support_ok = false;
    bool feedback_created = false;
    bool placed_feedback_created = false;
    bool invalid_feedback_desc_rejected = false;
    bool logical_desc_ok = false;
    bool pso_created = false;
    bool unique_map_ok = false;
    bool contention_min_ok = false;
    bool mip_region_used_ok = false;
    bool encode_roundtrip_ok = false;
    bool array_feedback_ok = false;
    uint32_t mismatches = 102;
    uint8_t observed[2][4][4] = {};
    uint8_t used_observed[22] = {};

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create_device = proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = proc<SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    ID3D12Device* device = nullptr;
    ID3D12Device8* device8 = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12Heap* feedback_heap = nullptr;
    ID3D12Resource* paired = nullptr;
    ID3D12Resource* paired_array_resource = nullptr;
    ID3D12Resource* feedback_resource[5] = {};
    ID3D12Resource* decoded[2] = {};
    ID3D12Resource* used_decoded[4] = {};
    ID3D12Resource* encoded_decoded = nullptr;
    ID3D12Resource* array_decoded[2] = {};
    ID3D12Resource* readback = nullptr;

    if (!write_file(shader_path, shader))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else {
        DeleteFileA(dxil_path);
        dxc_exit = run_process("dxc.exe -nologo -T cs_6_5 -E main -HV 2021 -Od "
                               "-Fo Z:\\tmp\\metalsharp_sampler_feedback.cso "
                               "Z:\\tmp\\metalsharp_sampler_feedback.hlsl");
        auto dxil = read_file(dxil_path);
        if (dxc_exit != 0 || dxil.empty())
            hr = E_FAIL;
        else if (!create_device || !serialize)
            hr = E_NOINTERFACE;
        else
            hr = create_device(nullptr, D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device),
                               reinterpret_cast<void**>(&device));

        if (SUCCEEDED(hr)) {
            hr = device->QueryInterface(IID_PPV_ARGS(&device8));
            device8_ok = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr)) {
            format_support_ok = true;
            const DXGI_FORMAT formats[2] = {DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE,
                                            DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE};
            for (DXGI_FORMAT format : formats) {
                D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
                support.Format = format;
                HRESULT support_hr =
                    device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
                format_support_ok &= SUCCEEDED(support_hr) && (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0;
            }
        }

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 4;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        }
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 3;
        root_desc.pParameters = params;
        ID3DBlob* root_blob = nullptr;
        ID3DBlob* root_errors = nullptr;
        if (SUCCEEDED(hr))
            hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_blob, &root_errors);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        release(root_errors);
        release(root_blob);

        D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 64;
            desc.Height = 64;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 4;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&paired));
            desc.DepthOrArraySize = 2;
            if (SUCCEEDED(hr))
                hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                     IID_PPV_ARGS(&paired_array_resource));
        }
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 64;
            desc.Height = 64;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 4;
            desc.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            desc.SamplerFeedbackMipRegion = {16, 16, 1};
            D3D12_HEAP_DESC heap_desc = {};
            heap_desc.SizeInBytes = 1024 * 1024;
            heap_desc.Properties = default_heap;
            heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
            hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&feedback_heap));
            logical_desc_ok = true;
            for (uint32_t i = 0; i < 2 && SUCCEEDED(hr); ++i) {
                if (i == 0)
                    hr = device8->CreatePlacedResource1(feedback_heap, 0, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr, IID_PPV_ARGS(&feedback_resource[i]));
                else
                    hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                           IID_PPV_ARGS(&feedback_resource[i]));
                placed_feedback_created = i != 0 || SUCCEEDED(hr);
                if (feedback_resource[i]) {
                    D3D12_RESOURCE_DESC actual = feedback_resource[i]->GetDesc();
                    logical_desc_ok &= actual.Width == 64 && actual.Height == 64 && actual.MipLevels == 4 &&
                                       actual.Format == desc.Format;
                }
            }
            desc.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE;
            if (SUCCEEDED(hr))
                hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                       IID_PPV_ARGS(&feedback_resource[2]));
            if (feedback_resource[2]) {
                D3D12_RESOURCE_DESC actual = feedback_resource[2]->GetDesc();
                logical_desc_ok &=
                    actual.Width == 64 && actual.Height == 64 && actual.MipLevels == 4 && actual.Format == desc.Format;
            }
            desc.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
            if (SUCCEEDED(hr))
                hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, nullptr,
                                                       IID_PPV_ARGS(&feedback_resource[3]));
            desc.DepthOrArraySize = 2;
            if (SUCCEEDED(hr))
                hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                       IID_PPV_ARGS(&feedback_resource[4]));
            feedback_created = SUCCEEDED(hr) && feedback_resource[0] && feedback_resource[1] && feedback_resource[2] &&
                               feedback_resource[3] && feedback_resource[4];
            D3D12_RESOURCE_DESC1 invalid_desc = desc;
            invalid_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource* invalid_resource = nullptr;
            HRESULT invalid_hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &invalid_desc,
                                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                   nullptr, IID_PPV_ARGS(&invalid_resource));
            invalid_feedback_desc_rejected = invalid_hr == E_INVALIDARG && invalid_resource == nullptr;
            release(invalid_resource);
        }
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 4;
            desc.Height = 4;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8_UINT;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            for (uint32_t i = 0; i < 2 && SUCCEEDED(hr); ++i)
                hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                     IID_PPV_ARGS(&decoded[i]));
            if (SUCCEEDED(hr))
                hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                     IID_PPV_ARGS(&encoded_decoded));
            for (uint32_t i = 0; i < 2 && SUCCEEDED(hr); ++i)
                hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                     IID_PPV_ARGS(&array_decoded[i]));
            const uint32_t used_sizes[4] = {4, 2, 1, 1};
            for (uint32_t i = 0; i < 4 && SUCCEEDED(hr); ++i) {
                desc.Width = used_sizes[i];
                desc.Height = used_sizes[i];
                hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                     IID_PPV_ARGS(&used_decoded[i]));
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
            D3D12_RESOURCE_DESC desc = buffer_desc(7168);
            hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        }

        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 6;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&resource_heap));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sampler_heap));
        }
        if (SUCCEEDED(hr)) {
            UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
            for (uint32_t i = 0; i < 3; ++i) {
                device8->CreateSamplerFeedbackUnorderedAccessView(paired, feedback_resource[i], cpu);
                cpu.ptr += stride;
            }
            device8->CreateSamplerFeedbackUnorderedAccessView(paired_array_resource, feedback_resource[4], cpu);
            cpu.ptr += stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 4;
            device->CreateShaderResourceView(paired, &srv, cpu);
            cpu.ptr += stride;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = 4;
            srv.Texture2DArray.ArraySize = 2;
            device->CreateShaderResourceView(paired_array_resource, &srv, cpu);
            D3D12_SAMPLER_DESC sampler = {};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
        }

        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = root;
            desc.CS.pShaderBytecode = dxil.data();
            desc.CS.BytecodeLength = dxil.size();
            hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
            pso_created = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr)) {
            D3D12_COMMAND_QUEUE_DESC desc = {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        }
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr))
            hr = list->QueryInterface(IID_PPV_ARGS(&list1));

        if (SUCCEEDED(hr)) {
            ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
            list->SetDescriptorHeaps(2, heaps);
            list->SetComputeRootSignature(root);
            const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_GPU_DESCRIPTOR_HANDLE resource_gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = resource_gpu;
            srv_gpu.ptr += 4 * stride;
            list->SetComputeRootDescriptorTable(0, resource_gpu);
            list->SetComputeRootDescriptorTable(1, srv_gpu);
            list->SetComputeRootDescriptorTable(2, sampler_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetPipelineState(pso);
            const UINT clear_min[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
            const UINT clear_used[4] = {};
            D3D12_CPU_DESCRIPTOR_HANDLE feedback_cpu[4] = {resource_heap->GetCPUDescriptorHandleForHeapStart(),
                                                           resource_heap->GetCPUDescriptorHandleForHeapStart(),
                                                           resource_heap->GetCPUDescriptorHandleForHeapStart(),
                                                           resource_heap->GetCPUDescriptorHandleForHeapStart()};
            feedback_cpu[1].ptr += stride;
            feedback_cpu[2].ptr += 2 * stride;
            feedback_cpu[3].ptr += 3 * stride;

            auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = resource;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = before;
                barrier.Transition.StateAfter = after;
                list->ResourceBarrier(1, &barrier);
            };
            auto uav_barrier = [&](uint32_t index) {
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = feedback_resource[index];
                list->ResourceBarrier(1, &barrier);
            };

            for (uint32_t i = 0; i < 3; ++i) {
                D3D12_GPU_DESCRIPTOR_HANDLE feedback_gpu = resource_gpu;
                feedback_gpu.ptr += i * stride;
                list->ClearUnorderedAccessViewUint(feedback_gpu, feedback_cpu[i], feedback_resource[i],
                                                   i < 2 ? clear_min : clear_used, 0, nullptr);
            }
            D3D12_GPU_DESCRIPTOR_HANDLE array_feedback_gpu = resource_gpu;
            array_feedback_gpu.ptr += 3 * stride;
            list->ClearUnorderedAccessViewUint(array_feedback_gpu, feedback_cpu[3], feedback_resource[4], clear_min, 0,
                                               nullptr);
            list->Dispatch(1, 1, 1);
            for (uint32_t i = 0; i < 3; ++i) {
                uav_barrier(i);
                transition(feedback_resource[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            }
            uav_barrier(4);
            transition(feedback_resource[4], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            for (uint32_t i = 0; i < 2; ++i)
                list1->ResolveSubresourceRegion(decoded[i], 0, 0, 0, feedback_resource[i], 0, nullptr,
                                                DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            for (uint32_t mip = 0; mip < 4; ++mip)
                list1->ResolveSubresourceRegion(used_decoded[mip], 0, 0, 0, feedback_resource[2], mip, nullptr,
                                                DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            list1->ResolveSubresourceRegion(array_decoded[0], 0, 0, 0, feedback_resource[4], 0, nullptr,
                                            DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            list1->ResolveSubresourceRegion(array_decoded[1], 0, 0, 0, feedback_resource[4], 4, nullptr,
                                            DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            transition(decoded[0], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            list1->ResolveSubresourceRegion(feedback_resource[3], 0, 0, 0, decoded[0], 0, nullptr, DXGI_FORMAT_R8_UINT,
                                            D3D12_RESOLVE_MODE_ENCODE_SAMPLER_FEEDBACK);
            transition(feedback_resource[3], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            list1->ResolveSubresourceRegion(encoded_decoded, 0, 0, 0, feedback_resource[3], 0, nullptr,
                                            DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            transition(decoded[0], D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(decoded[1], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (uint32_t i = 0; i < 4; ++i)
                transition(used_decoded[i], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(encoded_decoded, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (uint32_t i = 0; i < 2; ++i)
                transition(array_decoded[i], D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

            auto copy_to_readback = [&](ID3D12Resource* source, UINT width, UINT height, UINT64 offset) {
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = source;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = offset;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
                dst.PlacedFootprint.Footprint.Width = width;
                dst.PlacedFootprint.Footprint.Height = height;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = 256;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            };
            copy_to_readback(decoded[0], 4, 4, 0);
            copy_to_readback(decoded[1], 4, 4, 1024);
            const UINT used_sizes[4] = {4, 2, 1, 1};
            const UINT64 used_offsets[4] = {2048, 3072, 3584, 3840};
            for (uint32_t mip = 0; mip < 4; ++mip)
                copy_to_readback(used_decoded[mip], used_sizes[mip], used_sizes[mip], used_offsets[mip]);
            copy_to_readback(encoded_decoded, 4, 4, 4096);
            copy_to_readback(array_decoded[0], 4, 4, 5120);
            copy_to_readback(array_decoded[1], 4, 4, 6144);
            hr = execute_and_wait(device, queue, list);
        }
    }

    if (SUCCEEDED(hr) && readback) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, 7168};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            mismatches = 0;
            for (uint32_t slice = 0; slice < 2; ++slice) {
                for (uint32_t y = 0; y < 4; ++y) {
                    for (uint32_t x = 0; x < 4; ++x) {
                        observed[slice][y][x] = mapped[slice * 1024 + y * 256 + x];
                        const uint8_t expected = slice == 0 ? static_cast<uint8_t>((x + 2 * y) & 3)
                                                            : static_cast<uint8_t>((x == 0 && y == 0) ? 0 : 255);
                        if (observed[slice][y][x] != expected)
                            ++mismatches;
                    }
                }
            }
            unique_map_ok = true;
            contention_min_ok = true;
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    unique_map_ok &= observed[0][y][x] == ((x + 2 * y) & 3);
                    contention_min_ok &= observed[1][y][x] == ((x == 0 && y == 0) ? 0 : 255);
                }
            }
            const UINT used_sizes[4] = {4, 2, 1, 1};
            const UINT64 used_offsets[4] = {2048, 3072, 3584, 3840};
            uint32_t used_index = 0;
            mip_region_used_ok = true;
            for (uint32_t mip = 0; mip < 4; ++mip) {
                for (uint32_t y = 0; y < used_sizes[mip]; ++y) {
                    for (uint32_t x = 0; x < used_sizes[mip]; ++x) {
                        const uint8_t value = mapped[used_offsets[mip] + y * 256 + x];
                        used_observed[used_index++] = value;
                        uint8_t expected = 255;
                        if (mip == 0)
                            expected = ((x == 0 && (y == 0 || y == 2)) || (x == 2 && (y == 1 || y == 3))) ? 255 : 0;
                        mip_region_used_ok &= value == expected;
                        if (value != expected)
                            ++mismatches;
                    }
                }
            }
            encode_roundtrip_ok = true;
            array_feedback_ok = true;
            for (uint32_t y = 0; y < 4; ++y) {
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint8_t expected = static_cast<uint8_t>((x + 2 * y) & 3);
                    const uint8_t value = mapped[4096 + y * 256 + x];
                    encode_roundtrip_ok &= value == expected;
                    if (value != expected)
                        ++mismatches;
                    for (uint32_t layer = 0; layer < 2; ++layer) {
                        const uint8_t array_expected = static_cast<uint8_t>((x + 2 * y + layer) & 3);
                        const uint8_t array_value = mapped[5120 + layer * 1024 + y * 256 + x];
                        array_feedback_ok &= array_value == array_expected;
                        if (array_value != array_expected)
                            ++mismatches;
                    }
                }
            }
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    HRESULT options_hr =
        device ? device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)) : E_FAIL;
    const bool passed = SUCCEEDED(hr) && dxc_exit == 0 && device8_ok && format_support_ok && feedback_created &&
                        placed_feedback_created && invalid_feedback_desc_rejected && logical_desc_ok && pso_created &&
                        unique_map_ok && contention_min_ok && mip_region_used_ok && encode_roundtrip_ok &&
                        array_feedback_ok && SUCCEEDED(options_hr) &&
                        options7.SamplerFeedbackTier >= D3D12_SAMPLER_FEEDBACK_TIER_0_9 && mismatches == 0;

    std::printf("{\n");
    std::printf("  \"probe\": \"sampler_feedback\",\n");
    std::printf("  \"passed\": %s,\n", passed ? "true" : "false");
    std::printf("  \"dxc_exit_code\": %lu,\n", static_cast<unsigned long>(dxc_exit));
    std::printf("  \"runtime_hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"device8_available\": %s,\n", device8_ok ? "true" : "false");
    std::printf("  \"format_support_ok\": %s,\n", format_support_ok ? "true" : "false");
    std::printf("  \"feedback_resource_created\": %s,\n", feedback_created ? "true" : "false");
    std::printf("  \"placed_feedback_created\": %s,\n", placed_feedback_created ? "true" : "false");
    std::printf("  \"invalid_feedback_desc_rejected\": %s,\n", invalid_feedback_desc_rejected ? "true" : "false");
    std::printf("  \"logical_resource_desc_preserved\": %s,\n", logical_desc_ok ? "true" : "false");
    std::printf("  \"compute_pso_created\": %s,\n", pso_created ? "true" : "false");
    std::printf("  \"unique_region_map_ok\": %s,\n", unique_map_ok ? "true" : "false");
    std::printf("  \"contention_minimum_ok\": %s,\n", contention_min_ok ? "true" : "false");
    std::printf("  \"mip_region_used_ok\": %s,\n", mip_region_used_ok ? "true" : "false");
    std::printf("  \"encode_roundtrip_ok\": %s,\n", encode_roundtrip_ok ? "true" : "false");
    std::printf("  \"array_feedback_ok\": %s,\n", array_feedback_ok ? "true" : "false");
    std::printf("  \"mismatch_count\": %u,\n", mismatches);
    std::printf("  \"options7_hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(options_hr)));
    std::printf("  \"reported_sampler_feedback_tier\": %u,\n", static_cast<unsigned>(options7.SamplerFeedbackTier));
    std::printf("  \"observed_unique\": [");
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            std::printf("%s%u", x || y ? ", " : "", observed[0][y][x]);
    std::printf("],\n  \"observed_contention\": [");
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
            std::printf("%s%u", x || y ? ", " : "", observed[1][y][x]);
    std::printf("],\n  \"observed_mip_region_used\": [");
    for (uint32_t i = 0; i < 22; ++i)
        std::printf("%s%u", i ? ", " : "", used_observed[i]);
    std::printf("]\n}\n");

    release(readback);
    release(array_decoded[1]);
    release(array_decoded[0]);
    release(encoded_decoded);
    for (uint32_t i = 0; i < 4; ++i)
        release(used_decoded[i]);
    release(decoded[1]);
    release(decoded[0]);
    release(feedback_resource[4]);
    release(feedback_resource[3]);
    release(feedback_resource[2]);
    release(feedback_resource[1]);
    release(feedback_resource[0]);
    release(paired_array_resource);
    release(paired);
    release(sampler_heap);
    release(resource_heap);
    release(feedback_heap);
    release(list1);
    release(list);
    release(allocator);
    release(queue);
    release(pso);
    release(root);
    release(device8);
    release(device);
    if (d3d12)
        FreeLibrary(d3d12);
    return passed ? 0 : 1;
}
