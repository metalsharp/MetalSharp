#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

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

static bool write_text_file(const char* path, const char* text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    DWORD size = static_cast<DWORD>(std::strlen(text));
    bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

static std::string read_text_file(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "";

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return "";
    }

    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(read);
    CloseHandle(file);
    return out;
}

static std::vector<uint8_t> read_binary_file(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }

    std::vector<uint8_t> out(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(read);
    CloseHandle(file);
    return out;
}

static DWORD run_process_wait(std::string command_line) {
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                  nullptr, &startup, &process);
    if (!created)
        return 0xffffffffu;

    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 0xffffffffu;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
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
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr))
        return hr;
    hr = queue->Signal(fence, 1);
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr)) {
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr) && WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event_handle)
        CloseHandle(event_handle);
    fence->Release();
    return hr;
}

struct AuditCase {
    const char* name;
    const char* entry;
    const char* target;
    const char* category;
    bool requires_runtime_proof;
};

struct CaseResult {
    std::string name;
    std::string target;
    std::string category;
    bool compile_ok = false;
    bool dxil_blob = false;
    bool pso_created = false;
    bool runtime_executed = false;
    bool readback_ok = false;
    HRESULT pso_hr = E_FAIL;
    HRESULT runtime_hr = E_FAIL;
    DWORD dxc_exit_code = 0xffffffffu;
    size_t dxil_size = 0;
    uint32_t mismatch_count = 4;
    uint32_t observed[32] = {};
    uint32_t expected[32] = {};
    uint32_t value_count = 4;
    std::string detail;
};

static HRESULT create_root_signature(ID3D12Device* device, D3D12SerializeRootSignatureFn serialize,
                                     ID3D12RootSignature** root, std::string& errors) {
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 8;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[2].NumDescriptors = 4;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.Num32BitValues = 4;
    params[3].Constants.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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

static void execute_case(ID3D12Device* device, ID3D12RootSignature* root, ID3D12PipelineState* pso,
                         const AuditCase& audit_case, CaseResult& result) {
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* dynamic_output = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* input0 = nullptr;
    ID3D12Resource* input1 = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* texture_alt = nullptr;
    ID3D12Resource* rw_texture0 = nullptr;
    ID3D12Resource* rw_texture1 = nullptr;
    ID3D12Resource* raw_texture = nullptr;
    ID3D12Resource* comparison_texture = nullptr;
    ID3D12Resource* comparison_array_texture = nullptr;
    ID3D12Resource* texture_upload = nullptr;
    ID3D12Device10* device10 = nullptr;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));

    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 12;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&resource_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        heap_desc.NumDescriptors = 4;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sampler_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap_desc.NumDescriptors = 2;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap));
    }

    auto create_buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags,
                             D3D12_RESOURCE_STATES state, const void* initial_data,
                             ID3D12Resource** resource) -> HRESULT {
        D3D12_HEAP_PROPERTIES props = heap_props(heap_type);
        D3D12_RESOURCE_DESC desc = buffer_desc(bytes, flags);
        HRESULT create_hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                            IID_PPV_ARGS(resource));
        if (SUCCEEDED(create_hr) && initial_data) {
            void* mapped = nullptr;
            D3D12_RANGE read_range = {0, 0};
            create_hr = (*resource)->Map(0, &read_range, &mapped);
            if (SUCCEEDED(create_hr) && mapped) {
                std::memcpy(mapped, initial_data, static_cast<size_t>(bytes));
                D3D12_RANGE written = {0, static_cast<SIZE_T>(bytes)};
                (*resource)->Unmap(0, &written);
            }
        }
        return create_hr;
    };

    const uint32_t input0_data[16] = {10, 20, 30, 40};
    const uint32_t input1_data[16] = {100, 200, 300, 400};
    if (SUCCEEDED(hr))
        hr = create_buffer(128, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, &output);
    if (SUCCEEDED(hr))
        hr = create_buffer(128, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                           &dynamic_output);
    if (SUCCEEDED(hr))
        hr = create_buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                           nullptr, &readback);
    if (SUCCEEDED(hr))
        hr = create_buffer(sizeof(input0_data), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, input0_data, &input0);
    if (SUCCEEDED(hr))
        hr = create_buffer(sizeof(input1_data), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, input1_data, &input1);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC texture_desc = {};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = 8;
        texture_desc.Height = 1;
        texture_desc.DepthOrArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
        if (SUCCEEDED(hr))
            hr = device->QueryInterface(IID_PPV_ARGS(&device10));
        if (SUCCEEDED(hr)) {
            texture_desc.Width = 1;
            D3D12_RESOURCE_DESC1 texture_desc1 = {};
            std::memcpy(&texture_desc1, &texture_desc, sizeof(texture_desc));
            DXGI_FORMAT castable_format = DXGI_FORMAT_R32_UINT;
            hr = device10->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &texture_desc1,
                                                    D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 1,
                                                    &castable_format, IID_PPV_ARGS(&raw_texture));
        }
        if (SUCCEEDED(hr)) {
            texture_desc.Width = 8;
            texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            hr = device->CreateCommittedResource(
                &props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&texture_alt));
        }
        if (SUCCEEDED(hr)) {
            texture_desc.Width = 4;
            texture_desc.Format = DXGI_FORMAT_R32_FLOAT;
            texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            hr = device->CreateCommittedResource(
                &props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&rw_texture0));
        }
        if (SUCCEEDED(hr))
            hr = device->CreateCommittedResource(
                &props, D3D12_HEAP_FLAG_NONE, &texture_desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&rw_texture1));
    }
    if (SUCCEEDED(hr)) {
        uint8_t upload_data[768] = {};
        upload_data[0] = 10;
        upload_data[1] = 20;
        upload_data[2] = 30;
        upload_data[3] = 40;
        for (uint32_t pixel = 0; pixel < 8; pixel++) {
            upload_data[pixel * 4 + 0] = static_cast<uint8_t>(10 + pixel * 10);
            upload_data[pixel * 4 + 1] = static_cast<uint8_t>(20 + pixel * 10);
            upload_data[pixel * 4 + 2] = static_cast<uint8_t>(30 + pixel * 10);
            upload_data[pixel * 4 + 3] = static_cast<uint8_t>(40 + pixel * 10);
            upload_data[512 + pixel * 4 + 0] =
                static_cast<uint8_t>(100 + pixel * 10);
            upload_data[512 + pixel * 4 + 1] = 1;
            upload_data[512 + pixel * 4 + 2] = 2;
            upload_data[512 + pixel * 4 + 3] = 255;
        }
        hr = create_buffer(sizeof(upload_data), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, upload_data, &texture_upload);
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC comparison_desc = {};
        comparison_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        comparison_desc.Width = 4;
        comparison_desc.Height = 1;
        comparison_desc.DepthOrArraySize = 1;
        comparison_desc.MipLevels = 2;
        comparison_desc.Format = DXGI_FORMAT_D32_FLOAT;
        comparison_desc.SampleDesc.Count = 1;
        comparison_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        comparison_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &comparison_desc,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
                                             IID_PPV_ARGS(&comparison_texture));
        if (SUCCEEDED(hr)) {
            const float mip0_depth[] = {0.25f, 0.25f, 0.75f, 0.75f};
            const float mip1_depth[] = {0.75f, 0.75f};
            hr = comparison_texture->WriteToSubresource(
                0, nullptr, mip0_depth, sizeof(mip0_depth), sizeof(mip0_depth));
            if (SUCCEEDED(hr))
                hr = comparison_texture->WriteToSubresource(
                    1, nullptr, mip1_depth, sizeof(mip1_depth), sizeof(mip1_depth));
        }
    }

    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC comparison_array_desc = {};
        comparison_array_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        comparison_array_desc.Width = 4;
        comparison_array_desc.Height = 1;
        comparison_array_desc.DepthOrArraySize = 6;
        comparison_array_desc.MipLevels = 1;
        comparison_array_desc.Format = DXGI_FORMAT_D32_FLOAT;
        comparison_array_desc.SampleDesc.Count = 1;
        comparison_array_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        comparison_array_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &comparison_array_desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
            IID_PPV_ARGS(&comparison_array_texture));
        for (UINT slice = 0; SUCCEEDED(hr) && slice < 6; ++slice) {
            const float depth_values[] = {0.75f, 0.75f, 0.75f, 0.75f};
            hr = comparison_array_texture->WriteToSubresource(
                slice, nullptr, depth_values, sizeof(depth_values),
                sizeof(depth_values));
        }
    }

    if (SUCCEEDED(hr)) {
        const UINT resource_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        const bool typed_atomic64 = std::strncmp(audit_case.name, "atomic64_typed_", 15) == 0 ||
                                    std::strncmp(audit_case.name, "atomic64_signed_typed_", 22) == 0;
        const bool structured_dynamic =
            std::strcmp(audit_case.name,
                        "rw_structured_descriptor_indexing") == 0;
        uav.Format = structured_dynamic
                         ? DXGI_FORMAT_UNKNOWN
                         : (typed_atomic64 ? DXGI_FORMAT_R32G32_UINT
                                           : DXGI_FORMAT_R32_TYPELESS);
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = typed_atomic64 ? 16 : (structured_dynamic ? 16 : 32);
        uav.Buffer.StructureByteStride = structured_dynamic ? 8 : 0;
        uav.Buffer.Flags =
            (typed_atomic64 || structured_dynamic)
                ? D3D12_BUFFER_UAV_FLAG_NONE
                : D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
        cpu.ptr += resource_stride;
        device->CreateUnorderedAccessView(dynamic_output, nullptr, &uav, cpu);
        const bool direct_texture_store_case =
            std::strcmp(audit_case.name,
                        "texture_store_direct_heap_descriptor_indexing") == 0;
        cpu.ptr += resource_stride;
        if (direct_texture_store_case) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC texture_uav = {};
            texture_uav.Format = DXGI_FORMAT_R32_FLOAT;
            texture_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(rw_texture0, nullptr,
                                              &texture_uav, cpu);
            cpu.ptr += resource_stride;
            device->CreateUnorderedAccessView(rw_texture1, nullptr,
                                              &texture_uav, cpu);
        } else {
            device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
            cpu.ptr += resource_stride;
            device->CreateUnorderedAccessView(dynamic_output, nullptr, &uav,
                                              cpu);
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = 16;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        cpu.ptr += resource_stride;
        device->CreateShaderResourceView(input0, &srv, cpu);
        cpu.ptr += resource_stride;
        device->CreateShaderResourceView(input1, &srv, cpu);
        cpu.ptr += resource_stride;
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, cpu);
        cpu.ptr += resource_stride;
        const bool direct_comparison_texture_case =
            std::strcmp(audit_case.name,
                        "comparison_texture_direct_heap_indexing") == 0 ||
            std::strcmp(audit_case.name,
                        "comparison_texture_direct_heap_indexing_base") == 0;
        const bool direct_texture_case =
            std::strcmp(audit_case.name,
                        "texture_direct_heap_descriptor_indexing") == 0 ||
            std::strcmp(audit_case.name,
                        "texture_sample_direct_heap_descriptor_indexing") == 0 ||
            std::strcmp(audit_case.name,
                        "texture_gather_direct_heap_descriptor_indexing") == 0 ||
            direct_comparison_texture_case;
        if (direct_comparison_texture_case) {
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            srv.Texture2D.MostDetailedMip = 1;
            device->CreateShaderResourceView(comparison_texture, &srv, cpu);
        } else {
            srv.Format = direct_texture_case ? DXGI_FORMAT_R8G8B8A8_UNORM
                                             : DXGI_FORMAT_R32_UINT;
            device->CreateShaderResourceView(
                direct_texture_case ? texture_alt : raw_texture, &srv, cpu);
        }
        cpu.ptr += resource_stride;
        if (direct_comparison_texture_case) {
            srv.Texture2D.MostDetailedMip = 0;
            device->CreateShaderResourceView(comparison_texture, &srv, cpu);
        } else {
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.Texture2D.MipLevels = 2;
            device->CreateShaderResourceView(comparison_texture, &srv, cpu);
        }
        cpu.ptr += resource_stride;
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = 6;
        device->CreateShaderResourceView(comparison_array_texture, &srv, cpu);
        cpu.ptr += resource_stride;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = 1;
        device->CreateShaderResourceView(comparison_array_texture, &srv, cpu);
        cpu.ptr += resource_stride;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MipLevels = 1;
        srv.TextureCubeArray.NumCubes = 1;
        device->CreateShaderResourceView(comparison_array_texture, &srv, cpu);

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(comparison_texture, &dsv, dsv_cpu);
        dsv.Texture2D.MipSlice = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_mip1 = dsv_cpu;
        dsv_mip1.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        device->CreateDepthStencilView(comparison_texture, &dsv, dsv_mip1);

        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
        const bool direct_sampler_case =
            std::strcmp(audit_case.name,
                        "sampler_direct_heap_descriptor_indexing") == 0;
        sampler.Filter = direct_sampler_case
                             ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                             : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        D3D12_CPU_DESCRIPTOR_HANDLE comparison_sampler = sampler_heap->GetCPUDescriptorHandleForHeapStart();
        comparison_sampler.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        device->CreateSampler(&sampler, comparison_sampler);
        sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        D3D12_CPU_DESCRIPTOR_HANDLE comparison_linear_sampler = comparison_sampler;
        comparison_linear_sampler.ptr +=
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        device->CreateSampler(&sampler, comparison_linear_sampler);
        sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        D3D12_TEXTURE_ADDRESS_MODE comparison_address =
            D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        if (std::strcmp(audit_case.name, "sample_cmp_wrap_sm67") == 0)
            comparison_address = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        else if (std::strcmp(audit_case.name, "sample_cmp_mirror_sm67") == 0)
            comparison_address = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        else if (std::strcmp(audit_case.name,
                             "sample_cmp_mirror_once_sm67") == 0)
            comparison_address = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        sampler.AddressU = comparison_address;
        sampler.AddressV = comparison_address;
        sampler.AddressW = comparison_address;
        sampler.BorderColor[0] = 1.0f;
        sampler.BorderColor[1] = 1.0f;
        sampler.BorderColor[2] = 1.0f;
        sampler.BorderColor[3] = 1.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE comparison_border_sampler =
            comparison_linear_sampler;
        comparison_border_sampler.ptr +=
            device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        device->CreateSampler(&sampler, comparison_border_sampler);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = texture_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = 8;
        src.PlacedFootprint.Footprint.Height = 1;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 256;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = raw_texture;
        src.PlacedFootprint.Footprint.Width = 1;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = texture_alt;
        src.PlacedFootprint.Offset = 512;
        src.PlacedFootprint.Footprint.Width = 8;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER texture_barrier = {};
        texture_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        texture_barrier.Transition.pResource = texture;
        texture_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        texture_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        texture_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER raw_texture_barrier = texture_barrier;
        raw_texture_barrier.Transition.pResource = raw_texture;
        D3D12_RESOURCE_BARRIER texture_barriers[] = {
            texture_barrier, raw_texture_barrier, raw_texture_barrier,
            raw_texture_barrier, raw_texture_barrier};
        texture_barriers[2].Transition.pResource = texture_alt;
        texture_barriers[3].Transition.pResource = comparison_texture;
        texture_barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        texture_barriers[4].Transition.pResource = comparison_array_texture;
        texture_barriers[4].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        list->ResourceBarrier(5, texture_barriers);

        ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
        list->SetDescriptorHeaps(2, heaps);
        list->SetComputeRootSignature(root);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        if (std::strncmp(audit_case.name, "atomic64_", 9) == 0) {
            const uint32_t zero[4] = {};
            list->ClearUnorderedAccessViewUint(gpu, resource_heap->GetCPUDescriptorHandleForHeapStart(), output, zero,
                                               0, nullptr);
        }
        gpu.ptr += 4 * resource_stride;
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRootDescriptorTable(2, sampler_heap->GetGPUDescriptorHandleForHeapStart());
        const bool wide_selector_case =
            std::strcmp(audit_case.name, "rw_descriptor_indexing4") == 0 ||
            std::strcmp(audit_case.name,
                        "rw_direct_heap_descriptor_indexing") == 0;
        const uint32_t selector_value =
            std::strcmp(audit_case.name,
                        "comparison_texture_direct_heap_indexing_base") == 0
                ? 0u
                : (wide_selector_case ? 3u : 1u);
        const uint32_t constants[4] = {selector_value, 3, 2, 0};
        list->SetComputeRoot32BitConstants(3, 4, constants, 0);
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER output_barriers[2] = {};
        output_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        const bool texture_store_case =
            std::strcmp(audit_case.name,
                        "texture_store_direct_heap_descriptor_indexing") == 0;
        const bool dynamic_write_case =
            std::strcmp(audit_case.name, "rw_descriptor_indexing") == 0 ||
            std::strcmp(audit_case.name, "rw_descriptor_indexing4") == 0 ||
            std::strcmp(audit_case.name,
                        "rw_direct_heap_descriptor_indexing") == 0 ||
            std::strcmp(audit_case.name,
                        "rw_structured_descriptor_indexing") == 0;
        ID3D12Resource* case_output = texture_store_case
                                          ? rw_texture1
                                          : (dynamic_write_case
                                                 ? dynamic_output
                                                 : output);
        output_barriers[0].UAV.pResource = case_output;
        output_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        output_barriers[1].Transition.pResource = case_output;
        output_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        output_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        output_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(2, output_barriers);
        if (texture_store_case) {
            D3D12_TEXTURE_COPY_LOCATION src_texture = {};
            src_texture.pResource = rw_texture1;
            src_texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst_buffer = {};
            dst_buffer.pResource = readback;
            dst_buffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_buffer.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
            dst_buffer.PlacedFootprint.Footprint.Width = 4;
            dst_buffer.PlacedFootprint.Footprint.Height = 1;
            dst_buffer.PlacedFootprint.Footprint.Depth = 1;
            dst_buffer.PlacedFootprint.Footprint.RowPitch = 256;
            list->CopyTextureRegion(&dst_buffer, 0, 0, 0, &src_texture,
                                    nullptr);
        } else {
            list->CopyResource(readback, case_output);
        }
        hr = execute_and_wait(device, queue, list);
    }

    result.runtime_hr = hr;
    result.runtime_executed = SUCCEEDED(hr);
    if (SUCCEEDED(hr) && readback) {
        uint32_t* mapped = nullptr;
        D3D12_RANGE range = {0, 32 * sizeof(uint32_t)};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            result.readback_ok = true;
            result.value_count =
                (std::strcmp(audit_case.name, "int64_arithmetic") == 0 ||
                 std::strcmp(audit_case.name,
                             "rw_structured_descriptor_indexing") == 0)
                    ? 8
                    : (std::strcmp(audit_case.name, "quad_vote_sm67") == 0
                           ? 32
                           : (std::strcmp(audit_case.name, "atomic64_raw_ops") == 0 ||
                                      std::strcmp(audit_case.name, "atomic64_group_ops") == 0
                                  ? 16
                                  : (std::strcmp(audit_case.name, "atomic64_typed_ops") == 0
                                         ? 16
                                         : (std::strcmp(audit_case.name, "atomic64_descriptor_heap_ops") == 0
                                                ? 18
                                                : (std::strcmp(audit_case.name, "atomic64_raw_add") == 0 ||
                                                           std::strcmp(audit_case.name, "atomic64_group_add") == 0 ||
                                                           std::strcmp(audit_case.name, "atomic64_typed_add") == 0 ||
                                                           std::strcmp(audit_case.name, "atomic64_compare_exchange") ==
                                                               0 ||
                                                           std::strcmp(audit_case.name,
                                                                       "atomic64_group_compare_exchange") == 0 ||
                                                           std::strcmp(audit_case.name,
                                                                       "atomic64_descriptor_heap_add") == 0
                                                       ? 2
                                                       : 4)))));
            for (uint32_t i = 0; i < result.value_count; ++i)
                result.observed[i] = mapped[i];
            if (std::strcmp(audit_case.name, "root_constants_uav") == 0) {
                const uint32_t expected[] = {6, 8, 10, 12};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "descriptor_indexing") == 0 ||
                       std::strcmp(audit_case.name, "structured_descriptor_indexing") == 0 ||
                       std::strcmp(audit_case.name,
                                   "srv_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {103, 203, 303, 403};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "structured_uint2_descriptor_indexing") == 0) {
                const uint32_t expected[] = {303, 703, 303, 703};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "rw_descriptor_indexing") == 0) {
                const uint32_t expected[] = {503, 504, 505, 506};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "rw_descriptor_indexing4") == 0) {
                const uint32_t expected[] = {803, 804, 805, 806};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "rw_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {903, 904, 905, 906};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "texture_direct_heap_descriptor_indexing") == 0 ||
                       std::strcmp(audit_case.name,
                                   "texture_sample_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {100, 110, 120, 130};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "sampler_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {15, 15, 15, 15};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "texture_gather_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {0x828c8c82, 0x828c8c82,
                                             0x828c8c82, 0x828c8c82};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "comparison_sampler_direct_heap_indexing") == 0 ||
                       std::strcmp(audit_case.name,
                                   "comparison_texture_direct_heap_indexing") == 0) {
                const uint32_t expected[] = {1, 1, 1, 1};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "comparison_texture_direct_heap_indexing_base") == 0) {
                const uint32_t expected[] = {0, 0, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "texture_store_direct_heap_descriptor_indexing") == 0) {
                const uint32_t expected[] = {0x447ac000, 0x447b0000,
                                             0x447b4000, 0x447b8000};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name,
                                   "rw_structured_descriptor_indexing") == 0) {
                const uint32_t expected[] = {600, 700, 601, 701,
                                             602, 702, 603, 703};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "int64_arithmetic") == 0) {
                const uint32_t expected[] = {5, 11, 6, 21, 7, 31, 8, 41};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomics_barriers") == 0) {
                const uint32_t expected[] = {4, 5, 6, 7};
                std::memcpy(result.expected, expected, sizeof(expected));
                std::sort(result.observed, result.observed + 4);
            } else if (std::strcmp(audit_case.name, "atomic64_raw_add") == 0) {
                const uint32_t expected[] = {2080, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_group_add") == 0) {
                const uint32_t expected[] = {2080, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_typed_add") == 0) {
                const uint32_t expected[] = {2080, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_typed_ops") == 0) {
                const uint32_t expected[] = {10, 0, 0, 0, 15, 0, 15, 0, 0, 0, 4, 0, 4, 0, 4, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
                if (result.observed[12] >= 1 && result.observed[12] <= 4)
                    result.expected[12] = result.observed[12];
                if (result.observed[14] >= 1 && result.observed[14] <= 4)
                    result.expected[14] = result.observed[14];
            } else if (std::strcmp(audit_case.name, "atomic64_signed_typed_ops") == 0) {
                const uint32_t expected[] = {0xfffffffcu, 0xffffffffu, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_raw_ops") == 0) {
                const uint32_t expected[] = {10, 0, 0, 0, 15, 0, 15, 0, 4, 0, 4, 0, 0xfffffffcu, 0xffffffffu, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
                if (result.observed[10] >= 1 && result.observed[10] <= 4)
                    result.expected[10] = result.observed[10];
            } else if (std::strcmp(audit_case.name, "atomic64_group_ops") == 0) {
                const uint32_t expected[] = {10, 0, 0, 0, 15, 0, 15, 0, 4, 0, 4, 0, 0xfffffffcu, 0xffffffffu, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
                if (result.observed[10] >= 1 && result.observed[10] <= 4)
                    result.expected[10] = result.observed[10];
            } else if (std::strcmp(audit_case.name, "atomic64_compare_exchange") == 0) {
                const uint32_t expected[] = {4, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_group_compare_exchange") == 0) {
                const uint32_t expected[] = {4, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
                if (result.observed[0] >= 1 && result.observed[0] <= 4)
                    result.expected[0] = result.observed[0];
            } else if (std::strcmp(audit_case.name, "atomic64_descriptor_heap_add") == 0) {
                const uint32_t expected[] = {2080, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "atomic64_descriptor_heap_ops") == 0) {
                const uint32_t expected[] = {10, 0, 0, 0,           15,          0, 15, 0, 4,
                                             0,  4, 0, 0xfffffffcu, 0xffffffffu, 0, 0,  4, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
                if (result.observed[10] >= 1 && result.observed[10] <= 4)
                    result.expected[10] = result.observed[10];
                if (result.observed[16] >= 1 && result.observed[16] <= 4)
                    result.expected[16] = result.observed[16];
            } else if (std::strcmp(audit_case.name, "quad_vote_sm67") == 0) {
                std::fill(result.expected, result.expected + 32, 3u);
            } else if (std::strcmp(audit_case.name, "sample_cmp_border_sm67") == 0) {
                const uint32_t expected[] = {1, 1, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_wrap_sm67") == 0) {
                const uint32_t expected[] = {1, 0, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_clamp_sm67") == 0 ||
                       std::strcmp(audit_case.name, "sample_cmp_mirror_sm67") == 0 ||
                       std::strcmp(audit_case.name, "sample_cmp_mirror_once_sm67") == 0) {
                const uint32_t expected[] = {0, 1, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_dimensions") == 0) {
                const uint32_t expected[] = {
                    0x3f800000u, 0x3f800000u, 0x3f800000u, 0x40400000u};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "raw_gather_sm67") == 0) {
                std::fill(result.expected, result.expected + 4, 0x281e140au);
            } else if (std::strcmp(audit_case.name, "typed_texture_load_sm67") == 0) {
                const uint32_t expected[] = {0x281e140au, 0, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "programmable_offset_sm67") == 0) {
                const uint32_t expected[] = {300, 341, 382, 383};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "static_offsets_sm67") == 0) {
                const uint32_t expected[] = {260, 300, 340, 380};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_level_sm67") == 0) {
                const uint32_t expected[] = {25, 75, 1, 1};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_filter_sm67") == 0) {
                const uint32_t expected[] = {0, 128, 255, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "texture_gather_sm67") == 0) {
                const uint32_t expected[] = {40, 50, 50, 40};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "texture_gather_offset_sm67") == 0) {
                const uint32_t expected[] = {50, 60, 60, 50};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "texture_gather_cmp_sm67") == 0) {
                const uint32_t expected[] = {0, 255, 255, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "texture_gather_cmp_offset_sm67") == 0) {
                const uint32_t expected[] = {255, 255, 255, 255};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else if (std::strcmp(audit_case.name, "sample_cmp_grad_sm68") == 0 ||
                       std::strcmp(audit_case.name, "sample_cmp_bias_sm68") == 0) {
                const uint32_t expected[] = {1, 0, 0, 0};
                std::memcpy(result.expected, expected, sizeof(expected));
            } else {
                const uint32_t expected[] = {260, 261, 262, 263};
                std::memcpy(result.expected, expected, sizeof(expected));
            }
            result.mismatch_count = 0;
            for (uint32_t i = 0; i < result.value_count; ++i) {
                if (result.observed[i] != result.expected[i])
                    ++result.mismatch_count;
            }
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
        result.runtime_hr = hr;
    }
    if (result.runtime_executed && result.readback_ok && result.mismatch_count == 0)
        result.detail = "compiled, linked, dispatched, and passed readback";
    else if (result.runtime_executed && result.readback_ok)
        result.detail = "runtime readback mismatch";
    else
        result.detail = "runtime dispatch or readback failed";

    safe_release(texture_upload);
    safe_release(comparison_array_texture);
    safe_release(comparison_texture);
    safe_release(raw_texture);
    safe_release(rw_texture1);
    safe_release(rw_texture0);
    safe_release(texture_alt);
    safe_release(texture);
    safe_release(device10);
    safe_release(input1);
    safe_release(input0);
    safe_release(readback);
    safe_release(dynamic_output);
    safe_release(output);
    safe_release(sampler_heap);
    safe_release(dsv_heap);
    safe_release(resource_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
}

static CaseResult run_case(ID3D12Device* device, ID3D12RootSignature* root, const AuditCase& audit_case) {
    CaseResult result;
    result.name = audit_case.name;
    result.target = audit_case.target;
    result.category = audit_case.category;

    const std::string base = std::string("Z:\\tmp\\dxmt_sm66_") + audit_case.entry;
    const std::string dxil_path = base + ".dxil";
    const std::string error_path = base + ".err";
    DeleteFileA(dxil_path.c_str());
    DeleteFileA(error_path.c_str());

    std::string command = "dxc.exe -nologo -T ";
    command += audit_case.target;
    command += " -E ";
    command += audit_case.entry;
    command += " -HV 2021 -Od -Fo ";
    command += dxil_path;
    command += " -Fe ";
    command += error_path;
    command += " Z:\\tmp\\dxmt_sm66_capabilities.hlsl";

    result.dxc_exit_code = run_process_wait(command);
    std::vector<uint8_t> dxil = read_binary_file(dxil_path.c_str());
    result.dxil_blob = !dxil.empty();
    result.dxil_size = dxil.size();
    result.compile_ok = result.dxc_exit_code == 0 && result.dxil_blob;

    if (result.compile_ok && device && root) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS.pShaderBytecode = dxil.data();
        desc.CS.BytecodeLength = dxil.size();
        ID3D12PipelineState* pso = nullptr;
        result.pso_hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        result.pso_created = SUCCEEDED(result.pso_hr) && pso;
        if (result.pso_created && audit_case.requires_runtime_proof)
            execute_case(device, root, pso, audit_case, result);
        safe_release(pso);
    }

    std::string dxc_errors = read_text_file(error_path.c_str());
    if (!result.compile_ok)
        result.detail = dxc_errors.empty() ? "DXC did not produce a DXIL blob" : dxc_errors;
    else if (!result.pso_created)
        result.detail = audit_case.requires_runtime_proof
                            ? "compiled to DXIL, but no linked compute PSO was created"
                            : "compiled to DXIL and rejected the unsupported PSO as required";
    else if (audit_case.requires_runtime_proof && !result.runtime_executed)
        result.detail = "compiled and linked; runtime execution proof is still required before SM 6.6 can be reported";
    else if (!audit_case.requires_runtime_proof)
        result.detail = "negative capability case recorded";

    return result;
}

int main() {
    const bool warmup_only = getenv_string("D3D12_METAL_SDK_SM66_MODE") == "warmup";
    const char* hlsl_path = "Z:\\tmp\\dxmt_sm66_capabilities.hlsl";
    const char* hlsl = R"(
RWByteAddressBuffer outbuf : register(u0);
RWByteAddressBuffer dynamic_outputs[2] : register(u0);
RWByteAddressBuffer dynamic_outputs4[4] : register(u0);
RWStructuredBuffer<uint2> dynamic_structured_outputs[2] : register(u0);
RWBuffer<uint64_t> typed_outbuf : register(u0);
RWBuffer<int64_t> signed_typed_outbuf : register(u0);
ByteAddressBuffer inputs[2] : register(t0);
StructuredBuffer<uint> structured_inputs[2] : register(t0);
StructuredBuffer<uint2> structured_pair_inputs[2] : register(t0);
Texture2D<float4> tex : register(t2);
Texture2D<uint> raw_tex : register(t3);
Texture2D<float> comparison_tex : register(t4);
Texture2DArray<float> comparison_array_tex : register(t5);
TextureCube<float> comparison_cube_tex : register(t6);
TextureCubeArray<float> comparison_cube_array_tex : register(t7);
SamplerState smp : register(s0);
SamplerComparisonState comparison_smp : register(s1);
SamplerComparisonState comparison_linear_smp : register(s2);
SamplerComparisonState comparison_address_smp : register(s3);

cbuffer RootConstants : register(b0) {
  uint selector;
  uint addend;
  uint multiplier;
  uint pad0;
};

groupshared uint group_counter;
groupshared uint64_t group_counter64;
groupshared uint64_t group_and64;
groupshared uint64_t group_or64;
groupshared uint64_t group_xor64;
groupshared uint64_t group_umax64;
groupshared uint64_t group_exchange64;
groupshared int64_t group_min64;
groupshared int64_t group_max64;
groupshared uint64_t group_compare64;

[numthreads(64, 1, 1)]
void cs_atomic64_raw_add(uint3 id : SV_DispatchThreadID) {
  uint64_t original = 0;
  outbuf.InterlockedAdd64(0, uint64_t(id.x + 1), original);
}

[numthreads(4, 1, 1)]
void cs_atomic64_raw_ops(uint3 id : SV_DispatchThreadID) {
  uint64_t original = 0;
  uint64_t value = uint64_t(id.x + 1);
  outbuf.InterlockedAdd64(0, value, original);
  outbuf.InterlockedAnd64(8, value, original);
  outbuf.InterlockedOr64(16, 1ull << id.x, original);
  outbuf.InterlockedXor64(24, 1ull << id.x, original);
  outbuf.InterlockedMax64(32, value, original);
  outbuf.InterlockedExchange64(40, value, original);
  int64_t signed_original = 0;
  int64_t signed_value = -int64_t(id.x + 1);
  outbuf.InterlockedMin64(48, signed_value, signed_original);
  outbuf.InterlockedMax64(56, signed_value, signed_original);
}

[numthreads(4, 1, 1)]
void cs_atomic64_compare_exchange(uint3 id : SV_DispatchThreadID) {
  uint64_t original = 0;
  outbuf.InterlockedCompareExchange64(
      0, uint64_t(id.x), uint64_t(id.x + 1), original);
}

[numthreads(64, 1, 1)]
void cs_atomic64_descriptor_heap_add(uint3 id : SV_DispatchThreadID) {
  RWByteAddressBuffer heap_out = ResourceDescriptorHeap[0];
  uint64_t original = 0;
  heap_out.InterlockedAdd64(0, uint64_t(id.x + 1), original);
}

[numthreads(4, 1, 1)]
void cs_atomic64_descriptor_heap_ops(uint3 id : SV_DispatchThreadID) {
  RWByteAddressBuffer heap_out = ResourceDescriptorHeap[0];
  uint64_t original = 0;
  uint64_t value = uint64_t(id.x + 1);
  heap_out.InterlockedAdd64(0, value, original);
  heap_out.InterlockedAnd64(8, value, original);
  heap_out.InterlockedOr64(16, 1ull << id.x, original);
  heap_out.InterlockedXor64(24, 1ull << id.x, original);
  heap_out.InterlockedMax64(32, value, original);
  heap_out.InterlockedExchange64(40, value, original);
  int64_t signed_original = 0;
  int64_t signed_value = -int64_t(id.x + 1);
  heap_out.InterlockedMin64(48, signed_value, signed_original);
  heap_out.InterlockedMax64(56, signed_value, signed_original);
  heap_out.InterlockedCompareExchange64(
      64, uint64_t(id.x), value, original);
}

[numthreads(64, 1, 1)]
void cs_atomic64_typed_add(uint3 id : SV_DispatchThreadID) {
  uint64_t original = 0;
  InterlockedAdd(typed_outbuf[0], uint64_t(id.x + 1), original);
}

[numthreads(4, 1, 1)]
void cs_atomic64_typed_ops(uint3 id : SV_DispatchThreadID) {
  uint64_t original = 0;
  uint64_t value = uint64_t(id.x + 1);
  InterlockedAdd(typed_outbuf[0], value, original);
  InterlockedAnd(typed_outbuf[1], value, original);
  InterlockedOr(typed_outbuf[2], 1ull << id.x, original);
  InterlockedXor(typed_outbuf[3], 1ull << id.x, original);
  InterlockedMin(typed_outbuf[4], value, original);
  InterlockedMax(typed_outbuf[5], value, original);
  InterlockedExchange(typed_outbuf[6], value, original);
  InterlockedCompareExchange(typed_outbuf[7], uint64_t(id.x),
                             value, original);
}

[numthreads(4, 1, 1)]
void cs_atomic64_signed_typed_ops(uint3 id : SV_DispatchThreadID) {
  int64_t original = 0;
  int64_t value = -int64_t(id.x + 1);
  InterlockedMin(signed_typed_outbuf[0], value, original);
  InterlockedMax(signed_typed_outbuf[1], value, original);
}

[numthreads(64, 1, 1)]
void cs_atomic64_group_add(uint group_index : SV_GroupIndex) {
  if (group_index == 0)
    group_counter64 = 0;
  GroupMemoryBarrierWithGroupSync();
  uint64_t original = 0;
  InterlockedAdd(group_counter64, uint64_t(group_index + 1), original);
  GroupMemoryBarrierWithGroupSync();
  if (group_index == 0)
    outbuf.Store<uint64_t>(0, group_counter64);
}

[numthreads(4, 1, 1)]
void cs_atomic64_group_ops(uint group_index : SV_GroupIndex) {
  if (group_index == 0) {
    group_counter64 = 0;
    group_and64 = 0;
    group_or64 = 0;
    group_xor64 = 0;
    group_umax64 = 0;
    group_exchange64 = 0;
    group_min64 = 0;
    group_max64 = 0;
  }
  GroupMemoryBarrierWithGroupSync();
  uint64_t original = 0;
  uint64_t value = uint64_t(group_index + 1);
  InterlockedAdd(group_counter64, value, original);
  InterlockedAnd(group_and64, value, original);
  InterlockedOr(group_or64, 1ull << group_index, original);
  InterlockedXor(group_xor64, 1ull << group_index, original);
  InterlockedMax(group_umax64, value, original);
  InterlockedExchange(group_exchange64, value, original);
  int64_t signed_original = 0;
  int64_t signed_value = -int64_t(group_index + 1);
  InterlockedMin(group_min64, signed_value, signed_original);
  InterlockedMax(group_max64, signed_value, signed_original);
  GroupMemoryBarrierWithGroupSync();
  if (group_index == 0) {
    outbuf.Store<uint64_t>(0, group_counter64);
    outbuf.Store<uint64_t>(8, group_and64);
    outbuf.Store<uint64_t>(16, group_or64);
    outbuf.Store<uint64_t>(24, group_xor64);
    outbuf.Store<uint64_t>(32, group_umax64);
    outbuf.Store<uint64_t>(40, group_exchange64);
    outbuf.Store<int64_t>(48, group_min64);
    outbuf.Store<int64_t>(56, group_max64);
  }
}

[numthreads(4, 1, 1)]
void cs_atomic64_group_compare_exchange(uint group_index : SV_GroupIndex) {
  if (group_index == 0)
    group_compare64 = 0;
  GroupMemoryBarrierWithGroupSync();
  uint64_t original = 0;
  InterlockedCompareExchange(group_compare64, uint64_t(group_index),
                             uint64_t(group_index + 1), original);
  GroupMemoryBarrierWithGroupSync();
  if (group_index == 0)
    outbuf.Store<uint64_t>(0, group_compare64);
}

[numthreads(4, 1, 1)]
void cs_root_constants(uint3 id : SV_DispatchThreadID) {
  outbuf.Store(id.x * 4, (id.x + addend) * multiplier);
}

[numthreads(4, 1, 1)]
void cs_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 1u;
  outbuf.Store(id.x * 4, inputs[descriptor_index].Load(id.x * 4) + addend);
}

[numthreads(4, 1, 1)]
void cs_structured_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 1u;
  outbuf.Store(id.x * 4,
               structured_inputs[descriptor_index][id.x] + addend);
}

[numthreads(4, 1, 1)]
void cs_structured_uint2_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 1u;
  uint2 value = structured_pair_inputs[descriptor_index][id.x & 1u];
  outbuf.Store(id.x * 4, value.x + value.y + addend);
}

[numthreads(4, 1, 1)]
void cs_rw_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 1u;
  dynamic_outputs[descriptor_index].Store(id.x * 4, 500u + id.x + addend);
}

[numthreads(4, 1, 1)]
void cs_rw_structured_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 1u;
  dynamic_structured_outputs[descriptor_index][id.x] =
      uint2(600u + id.x, 700u + id.x);
}

[numthreads(4, 1, 1)]
void cs_rw_descriptor_indexing4(uint3 id : SV_DispatchThreadID) {
  uint descriptor_index = selector & 3u;
  dynamic_outputs4[descriptor_index].Store(id.x * 4, 800u + id.x + addend);
}

[numthreads(4, 1, 1)]
void cs_rw_direct_heap_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  RWByteAddressBuffer selected = ResourceDescriptorHeap[selector & 3u];
  selected.Store(id.x * 4, 900u + id.x + addend);
}

[numthreads(4, 1, 1)]
void cs_srv_direct_heap_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  ByteAddressBuffer selected = ResourceDescriptorHeap[4u + (selector & 1u)];
  outbuf.Store(id.x * 4, selected.Load(id.x * 4) + addend);
}

[numthreads(4, 1, 1)]
void cs_texture_direct_heap_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  Texture2D<float4> selected = ResourceDescriptorHeap[6u + (selector & 1u)];
  float4 value = selected.Load(int3(id.x, 0, 0));
  outbuf.Store(id.x * 4, uint(round(value.x * 255.0)));
}

[numthreads(4, 1, 1)]
void cs_sampler_direct_heap_descriptor_indexing(uint3 id : SV_DispatchThreadID) {
  SamplerState selected = SamplerDescriptorHeap[selector & 1u];
  float4 value = tex.SampleLevel(selected, float2(0.125, 0.5), 0.0);
  outbuf.Store(id.x * 4, uint(round(value.x * 255.0)));
}

[numthreads(4, 1, 1)]
void cs_texture_sample_direct_heap_descriptor_indexing(
    uint3 id : SV_DispatchThreadID) {
  Texture2D<float4> selected = ResourceDescriptorHeap[6u + (selector & 1u)];
  float2 coordinate = float2((float(id.x) + 0.5) / 8.0, 0.5);
  float4 value = selected.SampleLevel(smp, coordinate, 0.0);
  outbuf.Store(id.x * 4, uint(round(value.x * 255.0)));
}

[numthreads(4, 1, 1)]
void cs_texture_store_direct_heap_descriptor_indexing(
    uint3 id : SV_DispatchThreadID) {
  RWTexture2D<float> selected = ResourceDescriptorHeap[2u + (selector & 1u)];
  selected[uint2(id.x, 0)] = float(1000u + id.x + addend);
}

[numthreads(4, 1, 1)]
void cs_texture_gather_direct_heap_descriptor_indexing(
    uint3 id : SV_DispatchThreadID) {
  Texture2D<float4> selected = ResourceDescriptorHeap[6u + (selector & 1u)];
  float4 gathered = selected.GatherRed(smp, float2(0.5, 0.5));
  uint4 bytes = uint4(round(gathered * 255.0));
  uint packed = bytes.x | (bytes.y << 8) | (bytes.z << 16) |
                (bytes.w << 24);
  outbuf.Store(id.x * 4, packed);
}

[numthreads(4, 1, 1)]
void cs_comparison_sampler_direct_heap_indexing(
    uint3 id : SV_DispatchThreadID) {
  SamplerComparisonState selected = SamplerDescriptorHeap[selector & 1u];
  float value = comparison_tex.SampleCmpLevelZero(
      selected, float2(0.875, 0.5), 0.5);
  outbuf.Store(id.x * 4, uint(value));
}

[numthreads(4, 1, 1)]
void cs_comparison_texture_direct_heap_indexing(
    uint3 id : SV_DispatchThreadID) {
  Texture2D<float> selected =
      ResourceDescriptorHeap[6u + (selector & 1u)];
  float value = selected.SampleCmpLevelZero(comparison_smp,
                                             float2(0.125, 0.5), 0.5);
  outbuf.Store(id.x * 4, uint(value));
}

[numthreads(1, 1, 1)]
void cs_counter_direct_heap_indexing(uint3 id : SV_DispatchThreadID) {
  AppendStructuredBuffer<uint> selected =
      ResourceDescriptorHeap[selector & 1u];
  selected.Append(1200u + id.x);
}

[numthreads(4, 1, 1)]
void cs_int64_arithmetic(uint3 id : SV_DispatchThreadID) {
  uint64_t wide = ((uint64_t)inputs[0].Load(id.x * 4) << 32) | (uint64_t)(id.x + addend);
  wide += 0x100000002ull;
  outbuf.Store(id.x * 8, (uint)(wide & 0xffffffffull));
  outbuf.Store(id.x * 8 + 4, (uint)(wide >> 32));
}

[numthreads(4, 1, 1)]
void cs_atomics_barriers(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  if (gi == 0)
    group_counter = 0;
  GroupMemoryBarrierWithGroupSync();
  uint original = 0;
  InterlockedAdd(group_counter, 1, original);
  GroupMemoryBarrierWithGroupSync();
  outbuf.Store(id.x * 4, group_counter + original);
}

[numthreads(4, 1, 1)]
void cs_texture_sampler(uint3 id : SV_DispatchThreadID) {
  float4 sample = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0);
  uint total = (uint)(sample.r * 255.0 + 0.5) + (uint)(sample.g * 255.0 + 0.5) +
               (uint)(sample.b * 255.0 + 0.5) + (uint)(sample.a * 255.0 + 0.5);
  outbuf.Store(id.x * 4, total + id.x);
}

[numthreads(4, 1, 1)]
void cs_programmable_offset_sm67(uint3 id : SV_DispatchThreadID) {
  int2 dynamic_offset = int2((int)((id.x + selector) & 7u), 0);
  float4 sample = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0,
                                  dynamic_offset);
  uint total = (uint)(sample.r * 255.0 + 0.5) +
               (uint)(sample.g * 255.0 + 0.5) +
               (uint)(sample.b * 255.0 + 0.5) +
               (uint)(sample.a * 255.0 + 0.5);
  outbuf.Store(id.x * 4, total + id.x);
}

[numthreads(1, 1, 1)]
void cs_static_offsets_sm67(uint3 id : SV_DispatchThreadID) {
  float4 s0 = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0, int2(0, 0));
  float4 s1 = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0, int2(1, 0));
  float4 s2 = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0, int2(2, 0));
  float4 s3 = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0, int2(3, 0));
  uint sum0 = (uint)(s0.r * 255.0 + 0.5) + (uint)(s0.g * 255.0 + 0.5) +
              (uint)(s0.b * 255.0 + 0.5) + (uint)(s0.a * 255.0 + 0.5);
  uint sum1 = (uint)(s1.r * 255.0 + 0.5) + (uint)(s1.g * 255.0 + 0.5) +
              (uint)(s1.b * 255.0 + 0.5) + (uint)(s1.a * 255.0 + 0.5);
  uint sum2 = (uint)(s2.r * 255.0 + 0.5) + (uint)(s2.g * 255.0 + 0.5) +
              (uint)(s2.b * 255.0 + 0.5) + (uint)(s2.a * 255.0 + 0.5);
  uint sum3 = (uint)(s3.r * 255.0 + 0.5) + (uint)(s3.g * 255.0 + 0.5) +
              (uint)(s3.b * 255.0 + 0.5) + (uint)(s3.a * 255.0 + 0.5);
  outbuf.Store4(0, uint4(sum0, sum1, sum2, sum3));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_dimensions() {
  float array_value = comparison_array_tex.SampleCmpLevelZero(
      comparison_smp, float3(0.5, 0.5, 1.0), 0.5);
  float cube_value = comparison_cube_tex.SampleCmpLevelZero(
      comparison_smp, float3(1.0, 0.0, 0.0), 0.5);
  float cube_array_value = comparison_cube_array_tex.SampleCmpLevelZero(
      comparison_smp, float4(1.0, 0.0, 0.0, 0.0), 0.5);
  outbuf.Store(0, asuint(array_value));
  outbuf.Store(4, asuint(cube_value));
  outbuf.Store(8, asuint(cube_array_value));
  outbuf.Store(12, asuint(array_value + cube_value + cube_array_value));
}

[numthreads(1, 1, 1)]
void cs_raw_gather_sm67(uint3 id : SV_DispatchThreadID) {
  uint4 gathered = raw_tex.GatherRaw(smp, float2(0.5, 0.5));
  outbuf.Store4(0, gathered);
}

[numthreads(1, 1, 1)]
void cs_typed_texture_load_sm67(uint3 id : SV_DispatchThreadID) {
  uint value = raw_tex.Load(int3(0, 0, 0));
  outbuf.Store(0, value);
}

[numthreads(1, 1, 1)]
void cs_texture_gather_sm67(uint3 id : SV_DispatchThreadID) {
  float4 gathered = tex.GatherRed(smp, float2(0.5, 0.5));
  outbuf.Store4(0, uint4(gathered * 255.0 + 0.5));
}

[numthreads(1, 1, 1)]
void cs_texture_gather_offset_sm67(uint3 id : SV_DispatchThreadID) {
  float4 gathered = tex.GatherRed(smp, float2(0.5, 0.5), int2(1, 0));
  outbuf.Store4(0, uint4(gathered * 255.0 + 0.5));
}

[numthreads(1, 1, 1)]
void cs_texture_gather_cmp_sm67(uint3 id : SV_DispatchThreadID) {
  float4 gathered = comparison_tex.GatherCmp(
      comparison_smp, float2(0.5, 0.5), 0.5);
  outbuf.Store4(0, uint4(gathered * 255.0 + 0.5));
}

[numthreads(1, 1, 1)]
void cs_texture_gather_cmp_offset_sm67(uint3 id : SV_DispatchThreadID) {
  float4 gathered = comparison_tex.GatherCmp(
      comparison_smp, float2(0.5, 0.5), 0.5, int2(1, 0));
  outbuf.Store4(0, uint4(gathered * 255.0 + 0.5));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_level_sm67(uint3 id : SV_DispatchThreadID) {
  float load0 = comparison_tex.Load(int3(0, 0, 0));
  float load1 = comparison_tex.Load(int3(0, 0, 1));
  float mip0 = comparison_tex.SampleCmpLevel(
      comparison_smp, float2(0.5, 0.5), 0.5, 0.0);
  float mip1 = comparison_tex.SampleCmpLevel(
      comparison_smp, float2(0.5, 0.5), 0.5, 1.0);
  outbuf.Store4(0, uint4((uint)(load0 * 100.0), (uint)(load1 * 100.0),
                         (uint)mip0, (uint)mip1));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_filter_sm67(uint3 id : SV_DispatchThreadID) {
  float point_value = comparison_tex.SampleCmpLevel(
      comparison_smp, float2(0.125, 0.5), 0.5, 0.0);
  float linear_value = comparison_tex.SampleCmpLevel(
      comparison_linear_smp, float2(0.5, 0.5), 0.5, 0.0);
  float offset_value = comparison_tex.SampleCmpLevel(
      comparison_smp, float2(0.125, 0.5), 0.5, 0.0, int2(2, 0));
  outbuf.Store3(0, uint3((uint)(point_value * 255.0 + 0.5),
                             (uint)(linear_value * 255.0 + 0.5),
                             (uint)(offset_value * 255.0 + 0.5)));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_clamp_sm67() {
  float low_value = comparison_tex.SampleCmpLevelZero(
      comparison_smp, float2(-0.125, 0.5), 0.5);
  float high_value = comparison_tex.SampleCmpLevelZero(
      comparison_smp, float2(1.125, 0.5), 0.5);
  outbuf.Store2(0, uint2((uint)low_value, (uint)high_value));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_address_sm67() {
  float low_value = comparison_tex.SampleCmpLevelZero(
      comparison_address_smp, float2(-0.125, 0.5), 0.5);
  float high_value = comparison_tex.SampleCmpLevelZero(
      comparison_address_smp, float2(1.125, 0.5), 0.5);
  outbuf.Store2(0, uint2((uint)low_value, (uint)high_value));
}

[numthreads(1, 1, 1)]
void cs_sample_cmp_grad_sm68(uint3 id : SV_DispatchThreadID) {
  float value = comparison_tex.SampleCmpGrad(
      comparison_smp, float2(0.5, 0.5), 0.5,
      float2(0.0, 0.0), float2(0.0, 0.0));
  outbuf.Store(0, (uint)value);
}

[numthreads(4, 1, 1)]
void cs_sample_cmp_bias_sm68(uint3 id : SV_DispatchThreadID) {
  float value = comparison_tex.SampleCmpBias(
      comparison_smp, float2(0.5, 0.5), 0.5, 0.0);
  outbuf.Store(0, (uint)value);
}

[numthreads(32, 1, 1)]
void cs_quad_vote_sm67(uint3 id : SV_DispatchThreadID) {
  bool any_vote = QuadAny((id.x & 3u) == 2u);
  bool all_vote = QuadAll((id.x & 3u) < 4u);
  outbuf.Store(id.x * 4, (any_vote ? 1u : 0u) | (all_vote ? 2u : 0u));
}
)";

    bool hlsl_written = write_text_file(hlsl_path, hlsl);

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxcompiler = LoadLibraryA("dxcompiler.dll");
    HMODULE dxil = LoadLibraryA("dxil.dll");
    auto create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11 = {};
    HRESULT options1_hr =
        device ? device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)) : E_FAIL;
    HRESULT options9_hr =
        device ? device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9, sizeof(options9)) : E_FAIL;
    HRESULT options11_hr =
        device ? device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11)) : E_FAIL;

    ID3D12RootSignature* root = nullptr;
    std::string root_errors;
    HRESULT root_hr = (device && serialize) ? create_root_signature(device, serialize, &root, root_errors)
                                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const AuditCase audit_cases[] = {
        {"root_constants_uav", "cs_root_constants", "cs_6_6", "root_constants_cbv_srv_uav_tables", true},
        {"descriptor_indexing", "cs_descriptor_indexing", "cs_6_6", "descriptor_indexing", true},
        {"structured_descriptor_indexing", "cs_structured_descriptor_indexing", "cs_6_6", "structured_resource_descriptor_indexing", true},
        {"structured_uint2_descriptor_indexing", "cs_structured_uint2_descriptor_indexing", "cs_6_6", "structured_uint2_resource_descriptor_indexing", true},
        {"rw_descriptor_indexing", "cs_rw_descriptor_indexing", "cs_6_6", "writable_resource_descriptor_indexing", true},
        {"rw_structured_descriptor_indexing", "cs_rw_structured_descriptor_indexing", "cs_6_6", "writable_structured_resource_descriptor_indexing", true},
        {"rw_descriptor_indexing4", "cs_rw_descriptor_indexing4", "cs_6_6", "writable_resource_descriptor_indexing4", true},
        {"rw_direct_heap_descriptor_indexing", "cs_rw_direct_heap_descriptor_indexing", "cs_6_6", "writable_direct_heap_descriptor_indexing", true},
        {"srv_direct_heap_descriptor_indexing", "cs_srv_direct_heap_descriptor_indexing", "cs_6_6", "readable_direct_heap_descriptor_indexing", true},
        {"texture_direct_heap_descriptor_indexing", "cs_texture_direct_heap_descriptor_indexing", "cs_6_6", "texture_direct_heap_descriptor_indexing", true},
        {"sampler_direct_heap_descriptor_indexing", "cs_sampler_direct_heap_descriptor_indexing", "cs_6_6", "sampler_direct_heap_descriptor_indexing", true},
        {"texture_sample_direct_heap_descriptor_indexing", "cs_texture_sample_direct_heap_descriptor_indexing", "cs_6_6", "texture_sample_direct_heap_descriptor_indexing", true},
        {"texture_store_direct_heap_descriptor_indexing", "cs_texture_store_direct_heap_descriptor_indexing", "cs_6_6", "texture_store_direct_heap_descriptor_indexing", true},
        {"texture_gather_direct_heap_descriptor_indexing", "cs_texture_gather_direct_heap_descriptor_indexing", "cs_6_6", "texture_gather_direct_heap_descriptor_indexing", true},
        {"comparison_sampler_direct_heap_indexing", "cs_comparison_sampler_direct_heap_indexing", "cs_6_6", "comparison_sampler_direct_heap_indexing", true},
        {"comparison_texture_direct_heap_indexing_base", "cs_comparison_texture_direct_heap_indexing", "cs_6_6", "comparison_texture_direct_heap_indexing_base", true},
        {"comparison_texture_direct_heap_indexing", "cs_comparison_texture_direct_heap_indexing", "cs_6_6", "comparison_texture_direct_heap_indexing", true},
        {"counter_direct_heap_indexing_rejected", "cs_counter_direct_heap_indexing", "cs_6_6", "counter_direct_heap_indexing_fail_closed", false},
        {"int64_arithmetic", "cs_int64_arithmetic", "cs_6_6", "64_bit_shader_arithmetic", true},
        {"atomics_barriers", "cs_atomics_barriers", "cs_6_6", "atomics_barriers", true},
        {"atomic64_raw_add", "cs_atomic64_raw_add", "cs_6_6", "atomic64_software_lock", true},
        {"atomic64_raw_ops", "cs_atomic64_raw_ops", "cs_6_6", "atomic64_software_lock_operation_matrix", true},
        {"atomic64_compare_exchange", "cs_atomic64_compare_exchange", "cs_6_6",
         "atomic64_compare_exchange_software_lock", true},
        {"atomic64_descriptor_heap_add", "cs_atomic64_descriptor_heap_add", "cs_6_6",
         "atomic64_resource_descriptor_heap", true},
        {"atomic64_descriptor_heap_ops", "cs_atomic64_descriptor_heap_ops", "cs_6_6",
         "atomic64_resource_descriptor_heap_operation_matrix", true},
        {"atomic64_typed_add", "cs_atomic64_typed_add", "cs_6_6", "atomic64_typed_resource_software_lock", true},
        {"atomic64_typed_ops", "cs_atomic64_typed_ops", "cs_6_6", "atomic64_typed_resource_operation_matrix", true},
        {"atomic64_signed_typed_ops", "cs_atomic64_signed_typed_ops", "cs_6_6",
         "atomic64_signed_typed_resource_operations", true},
        {"atomic64_group_add", "cs_atomic64_group_add", "cs_6_6", "atomic64_group_shared_software_lock", true},
        {"atomic64_group_ops", "cs_atomic64_group_ops", "cs_6_6", "atomic64_group_shared_operation_matrix", true},
        {"atomic64_group_compare_exchange", "cs_atomic64_group_compare_exchange", "cs_6_6",
         "atomic64_group_shared_compare_exchange", true},
        {"texture_sampler", "cs_texture_sampler", "cs_6_6", "samplers_texture_paths", true},
        {"programmable_offset_sm67", "cs_programmable_offset_sm67", "cs_6_7", "sm67_programmable_texture_offsets",
         true},
        {"static_offsets_sm67", "cs_static_offsets_sm67", "cs_6_7", "sm67_static_texture_offsets", true},
        {"raw_gather_sm67", "cs_raw_gather_sm67", "cs_6_7", "sm67_raw_gather", true},
        {"typed_texture_load_sm67", "cs_typed_texture_load_sm67", "cs_6_7", "typed_texture_element_type", true},
        {"texture_gather_sm67", "cs_texture_gather_sm67", "cs_6_7", "sm67_texture_gather", true},
        {"texture_gather_offset_sm67", "cs_texture_gather_offset_sm67", "cs_6_7", "sm67_texture_gather_offset", true},
        {"texture_gather_cmp_sm67", "cs_texture_gather_cmp_sm67", "cs_6_7", "sm67_texture_gather_cmp", true},
        {"texture_gather_cmp_offset_sm67", "cs_texture_gather_cmp_offset_sm67", "cs_6_7", "sm67_texture_gather_cmp_offset", true},
        {"sample_cmp_level_sm67", "cs_sample_cmp_level_sm67", "cs_6_7", "sm67_sample_cmp_level", true},
        {"sample_cmp_filter_sm67", "cs_sample_cmp_filter_sm67", "cs_6_7", "sm67_sample_cmp_filter", true},
        {"sample_cmp_clamp_sm67", "cs_sample_cmp_clamp_sm67", "cs_6_7", "sm67_sample_cmp_clamp", true},
        {"sample_cmp_border_sm67", "cs_sample_cmp_address_sm67", "cs_6_7", "sm67_sample_cmp_border", true},
        {"sample_cmp_wrap_sm67", "cs_sample_cmp_address_sm67", "cs_6_7", "sm67_sample_cmp_wrap", true},
        {"sample_cmp_mirror_sm67", "cs_sample_cmp_address_sm67", "cs_6_7", "sm67_sample_cmp_mirror", true},
        {"sample_cmp_mirror_once_sm67", "cs_sample_cmp_address_sm67", "cs_6_7", "sm67_sample_cmp_mirror_once", true},
        {"sample_cmp_grad_sm68", "cs_sample_cmp_grad_sm68", "cs_6_8", "sm68_sample_cmp_gradient", true},
        {"sample_cmp_bias_sm68", "cs_sample_cmp_bias_sm68", "cs_6_8", "sm68_sample_cmp_bias", true},
        {"sample_cmp_dimensions", "cs_sample_cmp_dimensions", "cs_6_7", "sample_cmp_array_cube", true},
        {"quad_vote_sm67", "cs_quad_vote_sm67", "cs_6_7", "sm67_quad_vote", true},
    };

    std::vector<CaseResult> results;
    if (hlsl_written) {
        for (const auto& audit_case : audit_cases)
            results.push_back(run_case(device, root, audit_case));
    }

    bool entrypoints_ok =
        d3d12 && dxcompiler && dxil && create_device && serialize && SUCCEEDED(create_hr) && SUCCEEDED(root_hr);
    bool compiler_acceptance_complete = hlsl_written && !results.empty();
    bool pso_link_complete = compiler_acceptance_complete;
    bool runtime_complete = compiler_acceptance_complete;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const bool requires_runtime = audit_cases[i].requires_runtime_proof;
        compiler_acceptance_complete = compiler_acceptance_complete && result.compile_ok;
        pso_link_complete =
            pso_link_complete &&
            (requires_runtime ? result.pso_created : !result.pso_created);
        runtime_complete =
            runtime_complete &&
            (requires_runtime
                 ? (result.runtime_executed && result.readback_ok &&
                    result.mismatch_count == 0)
                 : !result.runtime_executed);
    }

    auto atomic_case_passed = [&](const char* name) {
        for (const auto& result : results) {
            if (result.name == name)
                return result.compile_ok && result.pso_created && result.runtime_executed && result.readback_ok &&
                       result.mismatch_count == 0;
        }
        return false;
    };
    bool atomic64_conservative =
        SUCCEEDED(options9_hr) && options9.AtomicInt64OnTypedResourceSupported &&
        options9.AtomicInt64OnGroupSharedSupported && atomic_case_passed("atomic64_raw_add") &&
        atomic_case_passed("atomic64_raw_ops") && atomic_case_passed("atomic64_compare_exchange") &&
        atomic_case_passed("atomic64_typed_add") && atomic_case_passed("atomic64_typed_ops") &&
        atomic_case_passed("atomic64_signed_typed_ops") && atomic_case_passed("atomic64_group_add") &&
        atomic_case_passed("atomic64_group_ops") && atomic_case_passed("atomic64_group_compare_exchange") &&
        SUCCEEDED(options11_hr) && options11.AtomicInt64OnDescriptorHeapResourceSupported &&
        atomic_case_passed("atomic64_descriptor_heap_add") && atomic_case_passed("atomic64_descriptor_heap_ops");
    bool sm66_reportable = entrypoints_ok && atomic64_conservative;
    bool sm67_reportable = entrypoints_ok && atomic64_conservative;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        const bool requires_runtime = audit_cases[i].requires_runtime_proof;
        const bool case_passed =
            result.compile_ok &&
            (requires_runtime
                 ? (result.pso_created && result.runtime_executed &&
                    result.readback_ok && result.mismatch_count == 0)
                 : !result.pso_created);
        if (result.target == "cs_6_6")
            sm66_reportable = sm66_reportable && case_passed;
        sm67_reportable = sm67_reportable && case_passed;
    }
    const bool sm67_breadth_complete =
        sm67_reportable && atomic_case_passed("programmable_offset_sm67") && atomic_case_passed("raw_gather_sm67") &&
        atomic_case_passed("typed_texture_load_sm67") && atomic_case_passed("static_offsets_sm67") &&
        atomic_case_passed("texture_gather_sm67") &&
        atomic_case_passed("texture_gather_offset_sm67") &&
        atomic_case_passed("texture_gather_cmp_sm67") &&
        atomic_case_passed("texture_gather_cmp_offset_sm67") &&
        atomic_case_passed("sample_cmp_level_sm67") &&
        atomic_case_passed("sample_cmp_filter_sm67") &&
        atomic_case_passed("quad_vote_sm67");
    bool pass = entrypoints_ok && compiler_acceptance_complete && pso_link_complete && runtime_complete &&
                atomic64_conservative;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.sm66-capabilities.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"mode\": \"%s\",\n", warmup_only ? "warmup" : "audit");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"dxcompiler_loaded\": %s,\n", dxcompiler ? "true" : "false");
    std::printf("    \"dxil_loaded\": %s,\n", dxil ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", create_device ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s,\n", serialize ? "true" : "false");
    std::printf("    \"complete\": %s\n", entrypoints_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"device\": {\n");
    std::printf("    \"create_hr\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("    \"root_signature_hr\": \"%s\"\n", hr_hex(root_hr).c_str());
    std::printf("  },\n");
    std::printf("  \"feature_negatives\": {\n");
    std::printf("    \"options1_hr\": \"%s\",\n", hr_hex(options1_hr).c_str());
    std::printf("    \"int64_shader_ops_reported\": %s,\n", options1.Int64ShaderOps ? "true" : "false");
    std::printf("    \"options9_hr\": \"%s\",\n", hr_hex(options9_hr).c_str());
    std::printf("    \"atomic64_typed_resource_reported\": %s,\n",
                options9.AtomicInt64OnTypedResourceSupported ? "true" : "false");
    std::printf("    \"atomic64_group_shared_reported\": %s,\n",
                options9.AtomicInt64OnGroupSharedSupported ? "true" : "false");
    std::printf("    \"options11_hr\": \"%s\",\n", hr_hex(options11_hr).c_str());
    std::printf("    \"atomic64_descriptor_heap_reported\": %s,\n",
                options11.AtomicInt64OnDescriptorHeapResourceSupported ? "true" : "false");
    std::printf("    \"atomic64_conservative\": %s\n", atomic64_conservative ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"summary\": {\n");
    std::printf("    \"compiler_acceptance_complete\": %s,\n", compiler_acceptance_complete ? "true" : "false");
    std::printf("    \"pso_link_complete\": %s,\n", pso_link_complete ? "true" : "false");
    std::printf("    \"runtime_correctness_complete\": %s,\n", runtime_complete ? "true" : "false");
    std::printf("    \"sm66_reportable\": %s,\n", sm66_reportable ? "true" : "false");
    std::printf("    \"sm67_reportable\": %s,\n", sm67_reportable ? "true" : "false");
    std::printf("    \"decision\": \"%s\",\n", sm67_reportable
                                                   ? "SM 6.7 may be reported"
                                                   : "SM 6.7 must not be reported until all runtime cases execute");
    std::printf("    \"sm67_breadth_complete\": %s\n", sm67_breadth_complete ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        std::printf("    {\n");
        std::printf("      \"name\": \"%s\",\n", json_escape(result.name).c_str());
        std::printf("      \"target\": \"%s\",\n", json_escape(result.target).c_str());
        std::printf("      \"category\": \"%s\",\n", json_escape(result.category).c_str());
        std::printf("      \"compile_ok\": %s,\n", result.compile_ok ? "true" : "false");
        std::printf("      \"dxil_blob\": %s,\n", result.dxil_blob ? "true" : "false");
        std::printf("      \"dxil_size\": %zu,\n", result.dxil_size);
        std::printf("      \"dxc_exit_code\": %lu,\n", static_cast<unsigned long>(result.dxc_exit_code));
        std::printf("      \"pso_created\": %s,\n", result.pso_created ? "true" : "false");
        std::printf("      \"pso_hr\": \"%s\",\n", hr_hex(result.pso_hr).c_str());
        std::printf("      \"runtime_hr\": \"%s\",\n", hr_hex(result.runtime_hr).c_str());
        std::printf("      \"runtime_executed\": %s,\n", result.runtime_executed ? "true" : "false");
        std::printf("      \"readback_ok\": %s,\n", result.readback_ok ? "true" : "false");
        std::printf("      \"mismatch_count\": %u,\n", result.mismatch_count);
        std::printf("      \"observed\": [");
        for (uint32_t value = 0; value < result.value_count; ++value)
            std::printf("%s%u", value ? "," : "", result.observed[value]);
        std::printf("],\n");
        std::printf("      \"expected\": [");
        for (uint32_t value = 0; value < result.value_count; ++value)
            std::printf("%s%u", value ? "," : "", result.expected[value]);
        std::printf("],\n");
        std::printf("      \"detail\": \"%s\"\n", json_escape(result.detail).c_str());
        std::printf("    }%s\n", i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ]\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    safe_release(root);
    safe_release(device);
    return 0;
}
