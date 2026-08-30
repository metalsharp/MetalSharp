#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                       ID3DBlob**, ID3DBlob**);

template <typename T> static void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char* name) {
    T fn = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(fn) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

static std::string getenv_string(const char* key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (needed == 0)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (written == 0)
        return "";
    value.resize(written);
    return value;
}

static std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

static bool read_binary_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    long size = std::ftell(file);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t read = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (read != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
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

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

static D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment, UINT index) {
    start.ptr += static_cast<SIZE_T>(increment) * index;
    return start;
}

static D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu(D3D12_GPU_DESCRIPTOR_HANDLE start, UINT increment, UINT index) {
    start.ptr += static_cast<UINT64>(increment) * index;
    return start;
}

static bool wait_for_fence(ID3D12Fence* fence, UINT64 value, HANDLE event_handle) {
    if (fence->GetCompletedValue() >= value)
        return true;
    if (FAILED(fence->SetEventOnCompletion(value, event_handle)))
        return false;
    return WaitForSingleObject(event_handle, 5000) == WAIT_OBJECT_0;
}

static HRESULT execute_and_wait(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;

    ID3D12Device* device = nullptr;
    hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr))
        return hr;

    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    safe_release(device);
    if (FAILED(hr))
        return hr;

    HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle) {
        safe_release(fence);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    ID3D12CommandList* base_list = list;
    queue->ExecuteCommandLists(1, &base_list);
    hr = queue->Signal(fence, 1);
    bool waited = SUCCEEDED(hr) && wait_for_fence(fence, 1, event_handle);
    CloseHandle(event_handle);
    safe_release(fence);
    return waited ? S_OK : E_FAIL;
}

static HRESULT create_device(ID3D12Device** device) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    if (!create)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe, reinterpret_cast<void**>(device));
}

static HRESULT create_root_signature(ID3D12Device* device, ID3D12RootSignature** root, std::string& errors) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;

    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

struct SemanticCase {
    const char* name;
    const char* path;
    std::vector<uint32_t> input;
    std::vector<uint32_t> expected;
    const char* group;
};

struct CaseResult {
    std::string name;
    std::string group;
    bool ok = false;
    HRESULT hr = E_FAIL;
    HRESULT pso_hr = E_FAIL;
    bool loaded = false;
    bool warmed = false;
    std::vector<uint32_t> actual;
    std::string detail;
};

static CaseResult run_case(ID3D12Device* device, const SemanticCase& semantic_case, bool warmup_only) {
    CaseResult result;
    result.name = semantic_case.name;
    result.group = semantic_case.group;

    std::vector<uint8_t> cso;
    result.loaded = read_binary_file(semantic_case.path, cso);
    if (!result.loaded) {
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.detail = "compiled DXIL blob missing";
        return result;
    }

    ID3D12RootSignature* root = nullptr;
    std::string errors;
    HRESULT hr = create_root_signature(device, &root, errors);
    if (FAILED(hr)) {
        result.hr = hr;
        result.detail = errors.empty() ? "root signature creation failed" : errors;
        return result;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.CS.pShaderBytecode = cso.data();
    pso_desc.CS.BytecodeLength = cso.size();

    ID3D12PipelineState* pso = nullptr;
    result.pso_hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    result.warmed = true;
    if (warmup_only) {
        result.ok = true;
        result.hr = S_OK;
        result.detail = "DXIL cache warmup attempted";
        safe_release(pso);
        safe_release(root);
        return result;
    }
    if (FAILED(result.pso_hr) || !pso) {
        result.hr = result.pso_hr;
        result.detail = "compute PSO creation failed";
        safe_release(root);
        return result;
    }

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* input = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 2;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&input));
    }
    if (SUCCEEDED(hr)) {
        uint32_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = input->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            for (size_t i = 0; i < semantic_case.input.size(); ++i)
                mapped[i] = semantic_case.input[i];
            D3D12_RANGE write_range = {0, static_cast<SIZE_T>(semantic_case.input.size() * sizeof(uint32_t))};
            input->Unmap(0, &write_range);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_start = heap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 64;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(input, &srv, offset_cpu(cpu_start, inc, 0));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, offset_cpu(cpu_start, inc, 1));

        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, offset_gpu(gpu_start, inc, 1));
        list->SetComputeRootDescriptorTable(1, offset_gpu(gpu_start, inc, 0));
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER uav_barrier = {};
        uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barrier.UAV.pResource = output;
        list->ResourceBarrier(1, &uav_barrier);
        D3D12_RESOURCE_BARRIER copy_barrier =
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &copy_barrier);
        list->CopyResource(readback, output);
        hr = execute_and_wait(queue, list);
    }

    if (SUCCEEDED(hr)) {
        uint32_t* data = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(semantic_case.expected.size() * sizeof(uint32_t))};
        HRESULT map_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
        if (SUCCEEDED(map_hr)) {
            result.actual.assign(data, data + semantic_case.expected.size());
            D3D12_RANGE write_range = {0, 0};
            readback->Unmap(0, &write_range);
        }
        hr = map_hr;
    }

    result.hr = hr;
    result.ok = SUCCEEDED(hr) && result.actual == semantic_case.expected;
    result.detail = result.ok ? "validated expected UAV readback" : "UAV readback did not match expected values";

    safe_release(readback);
    safe_release(output);
    safe_release(input);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    return result;
}

static void print_values(const char* key, const std::vector<uint32_t>& values, bool comma) {
    std::printf("      \"%s\": [", key);
    for (size_t i = 0; i < values.size(); ++i)
        std::printf("%s%u", i == 0 ? "" : ", ", values[i]);
    std::printf("]%s\n", comma ? "," : "");
}

int main() {
    const bool warmup_only = getenv_string("D3D12_METAL_SDK_DXIL_SEMANTICS_MODE") == "warmup";
    SemanticCase cases[] = {
        {"math_bits", "probe_dxil_semantic_math_bits.cso", {}, {22, 48, 0x3f800000u, 8}, "float_int_math_bitcasts"},
        {"buffer_load_store", "probe_dxil_semantic_buffer.cso", {2, 4, 6, 8}, {7, 13, 19, 25}, "buffer_load_store"},
        {"atomics_ids", "probe_dxil_semantic_atomics_ids.cso", {}, {4, 6, 8, 10}, "barrier_atomics_compute_ids"},
        {"wave_quad", "probe_dxil_semantic_wave_quad.cso", {}, {273, 273, 273, 273}, "wave_quad_ops"},
        {"sm67_vector_int64", "probe_dxil_semantic_sm67.cso", {}, {68, 69, 70, 71},
         "sm67_long_vector_and_int64"},
        {"sm68_vector_arithmetic", "probe_dxil_semantic_sm68.cso", {}, {68, 136, 204, 272},
         "sm68_vector_and_wide_arithmetic"},
        {"sm69_integer_float_mix", "probe_dxil_semantic_sm69.cso", {}, {69, 70, 72, 73},
         "sm69_integer_float16_conversion"},
    };

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device(&device);
    std::vector<CaseResult> results;
    bool ok = SUCCEEDED(create_hr);
    if (SUCCEEDED(create_hr)) {
        for (const auto& semantic_case : cases) {
            results.push_back(run_case(device, semantic_case, warmup_only));
            ok = ok && results.back().ok;
        }
    }

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.dxil-semantic-probe.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"mode\": \"%s\",\n", warmup_only ? "warmup" : "validate");
    std::printf("  \"ok\": %s,\n", ok ? "true" : "false");
    std::printf("  \"device_hr\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const auto& expected = cases[i].expected;
        std::printf("    {\n");
        std::printf("      \"name\": \"%s\",\n", json_escape(result.name).c_str());
        std::printf("      \"group\": \"%s\",\n", json_escape(result.group).c_str());
        std::printf("      \"ok\": %s,\n", result.ok ? "true" : "false");
        std::printf("      \"loaded\": %s,\n", result.loaded ? "true" : "false");
        std::printf("      \"warmed\": %s,\n", result.warmed ? "true" : "false");
        std::printf("      \"hr\": \"%s\",\n", hr_hex(result.hr).c_str());
        std::printf("      \"pso_hr\": \"%s\",\n", hr_hex(result.pso_hr).c_str());
        std::printf("      \"detail\": \"%s\",\n", json_escape(result.detail).c_str());
        print_values("expected", expected, true);
        print_values("actual", result.actual, false);
        std::printf("    }%s\n", i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ]\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), ok ? 0u : 3u);
    safe_release(device);
    return 0;
}
