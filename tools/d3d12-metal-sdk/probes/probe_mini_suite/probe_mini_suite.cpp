#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#ifndef MINI_PROBE_CASE
#define MINI_PROBE_CASE 0
#endif

#ifndef MINI_PROBE_NAME
#define MINI_PROBE_NAME "unknown"
#endif

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                       ID3DBlob**, ID3DBlob**);
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

struct ProbeResult {
    bool ok = false;
    HRESULT hr = E_FAIL;
    std::string detail = "";
    std::string extra = "";
};

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, typename T>
struct alignas(void*) PipelineStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value = {};
};

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

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
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

static D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
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

static bool wait_for_fence(ID3D12Fence* fence, UINT64 value, HANDLE event_handle) {
    if (fence->GetCompletedValue() >= value)
        return true;
    if (FAILED(fence->SetEventOnCompletion(value, event_handle)))
        return false;
    return WaitForSingleObject(event_handle, 5000) == WAIT_OBJECT_0;
}

static std::string module_path(HMODULE module) {
    char buffer[4096];
    DWORD written = module ? GetModuleFileNameA(module, buffer, sizeof(buffer)) : 0;
    if (written == 0)
        return "";
    if (written >= sizeof(buffer))
        written = sizeof(buffer) - 1;
    return std::string(buffer, written);
}

static std::string g_d3d12_load_source;
static std::string g_d3d12_loaded_path;
static DWORD g_d3d12_load_error = 0;
static DWORD g_d3d12_proc_error = 0;

static HRESULT create_device(ID3D12Device** device) {
    g_d3d12_load_source = getenv_string("D3D12_METAL_SDK_D3D12_DLL");
    if (g_d3d12_load_source.empty())
        g_d3d12_load_source = "d3d12.dll";
    g_d3d12_loaded_path.clear();
    g_d3d12_load_error = 0;
    g_d3d12_proc_error = 0;

    HMODULE d3d12 = LoadLibraryA(g_d3d12_load_source.c_str());
    if (!d3d12) {
        g_d3d12_load_error = GetLastError();
        return HRESULT_FROM_WIN32(g_d3d12_load_error);
    }
    g_d3d12_loaded_path = module_path(d3d12);
    D3D12CreateDeviceFn create = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    if (!create) {
        g_d3d12_proc_error = GetLastError();
        return HRESULT_FROM_WIN32(g_d3d12_proc_error ? g_d3d12_proc_error : ERROR_PROC_NOT_FOUND);
    }
    return create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe, reinterpret_cast<void**>(device));
}

static HRESULT create_queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
}

static HRESULT compile_shader(const char* source, const char* entry, const char* target, ID3DBlob** blob,
                              std::string& errors) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompileFn compile = load_proc<D3DCompileFn>(compiler, "D3DCompile");
    if (!compile)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ID3DBlob* error_blob = nullptr;
    HRESULT hr = compile(source, std::strlen(source), "probe_mini_suite.hlsl", nullptr, nullptr, entry, target, 0, 0,
                         blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    return hr;
}

static HRESULT serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** blob, std::string& errors) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    D3D12SerializeRootSignatureFn serialize =
        load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    return hr;
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

static ProbeResult probe_create_device() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    safe_release(device);
    return {SUCCEEDED(hr), hr, SUCCEEDED(hr) ? "D3D12CreateDevice succeeded" : "D3D12CreateDevice failed", ""};
}

static ProbeResult probe_command_queue() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    ID3D12CommandQueue* direct = nullptr;
    ID3D12CommandQueue* compute = nullptr;
    ID3D12CommandQueue* copy = nullptr;
    HRESULT direct_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &direct);
    HRESULT compute_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &compute);
    HRESULT copy_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COPY, &copy);
    safe_release(direct);
    safe_release(compute);
    safe_release(copy);
    safe_release(device);

    bool ok = SUCCEEDED(direct_hr) && SUCCEEDED(compute_hr) && SUCCEEDED(copy_hr);
    std::string extra = "\"direct_hr\":\"" + hr_hex(direct_hr) + "\",\"compute_hr\":\"" + hr_hex(compute_hr) +
                        "\",\"copy_hr\":\"" + hr_hex(copy_hr) + "\"";
    return {ok, ok ? S_OK : direct_hr, ok ? "direct/compute/copy queues created" : "one or more queues failed", extra};
}

static ProbeResult probe_root_signature() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* blob = nullptr;
    std::string errors;
    hr = serialize_root_signature(desc, &blob, errors);
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));

    safe_release(root);
    safe_release(blob);
    safe_release(device);
    return {SUCCEEDED(hr), hr, SUCCEEDED(hr) ? "descriptor-table root signature created" : errors, ""};
}

static ProbeResult probe_descriptors() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc = {};
    cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_desc.NumDescriptors = 3;
    cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* cbv_heap = nullptr;
    HRESULT cbv_heap_hr = device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&cbv_heap));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    HRESULT rtv_heap_hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));

    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {};
    sampler_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_heap_desc.NumDescriptors = 1;
    sampler_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    HRESULT sampler_heap_hr = device->CreateDescriptorHeap(&sampler_heap_desc, IID_PPV_ARGS(&sampler_heap));

    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC upload_desc = buffer_desc(256);
    ID3D12Resource* upload = nullptr;
    HRESULT upload_hr =
        device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC uav_desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* uav = nullptr;
    HRESULT uav_hr =
        device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uav));

    D3D12_RESOURCE_DESC rt_desc =
        texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Resource* rt = nullptr;
    HRESULT rt_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&rt));

    if (SUCCEEDED(cbv_heap_hr) && SUCCEEDED(upload_hr) && SUCCEEDED(uav_hr) && SUCCEEDED(rt_hr)) {
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE start = cbv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
        cbv.BufferLocation = upload->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        device->CreateConstantBufferView(&cbv, start);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 16;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(upload, &srv, offset_cpu(start, inc, 1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view = {};
        uav_view.Format = DXGI_FORMAT_R32_TYPELESS;
        uav_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_view.Buffer.NumElements = 16;
        uav_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(uav, nullptr, &uav_view, offset_cpu(start, inc, 2));
    }

    if (SUCCEEDED(rtv_heap_hr) && SUCCEEDED(rt_hr))
        device->CreateRenderTargetView(rt, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

    if (SUCCEEDED(sampler_heap_hr)) {
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
    }

    bool ok = SUCCEEDED(cbv_heap_hr) && SUCCEEDED(rtv_heap_hr) && SUCCEEDED(sampler_heap_hr) && SUCCEEDED(upload_hr) &&
              SUCCEEDED(uav_hr) && SUCCEEDED(rt_hr);
    std::string extra = "\"cbv_srv_uav_heap_hr\":\"" + hr_hex(cbv_heap_hr) + "\",\"rtv_heap_hr\":\"" +
                        hr_hex(rtv_heap_hr) + "\",\"sampler_heap_hr\":\"" + hr_hex(sampler_heap_hr) +
                        "\",\"upload_hr\":\"" + hr_hex(upload_hr) + "\",\"uav_hr\":\"" + hr_hex(uav_hr) +
                        "\",\"rtv_resource_hr\":\"" + hr_hex(rt_hr) + "\"";
    safe_release(rt);
    safe_release(uav);
    safe_release(upload);
    safe_release(sampler_heap);
    safe_release(rtv_heap);
    safe_release(cbv_heap);
    safe_release(device);
    return {ok, ok ? S_OK : cbv_heap_hr, ok ? "descriptor heaps/views created" : "descriptor creation failed", extra};
}

static ProbeResult probe_compute_dispatch() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    const char* hlsl = "RWByteAddressBuffer outbuf:register(u0);"
                       "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){outbuf.Store(id.x*4,id.x+11);}";
    ID3DBlob* cs = nullptr;
    std::string errors;
    hr = compile_shader(hlsl, "main", "cs_5_0", &cs, errors);
    if (FAILED(hr)) {
        safe_release(device);
        return {false, hr, errors.empty() ? "compute shader compile failed" : errors, ""};
    }

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &param;
    ID3DBlob* root_blob = nullptr;
    hr = serialize_root_signature(root_desc, &root_blob, errors);
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));

    ID3D12PipelineState* pso = nullptr;
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root;
        pso_desc.CS.pShaderBytecode = cs->GetBufferPointer();
        pso_desc.CS.BytecodeLength = cs->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    }

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* uav = nullptr;
    ID3D12Resource* readback = nullptr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uav));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 64;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(uav, nullptr, &view, heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = uav;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, uav);
        hr = execute_and_wait(queue, list);
    }

    bool verified = false;
    if (SUCCEEDED(hr)) {
        uint32_t* data = nullptr;
        D3D12_RANGE read_range = {0, 16};
        HRESULT map_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
        verified = SUCCEEDED(map_hr) && data[0] == 11 && data[1] == 12 && data[2] == 13 && data[3] == 14;
        if (SUCCEEDED(map_hr)) {
            D3D12_RANGE write_range = {0, 0};
            readback->Unmap(0, &write_range);
        }
    }

    safe_release(readback);
    safe_release(uav);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(device);
    return {SUCCEEDED(hr) && verified, hr,
            verified ? "compute dispatch wrote expected UAV values" : "compute verification failed",
            "\"verified\":" + std::string(verified ? "true" : "false")};
}

static ProbeResult probe_compute_first_use_dispatch() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    const char* hlsl = "RWByteAddressBuffer outbuf:register(u0);"
                       "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){outbuf.Store(id.x*4,(id.x+1)*7);}";
    ID3DBlob* cs = nullptr;
    std::string errors;
    hr = compile_shader(hlsl, "main", "cs_5_0", &cs, errors);
    if (FAILED(hr)) {
        safe_release(device);
        return {false, hr, errors.empty() ? "compute shader compile failed" : errors, ""};
    }

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &param;
    ID3DBlob* root_blob = nullptr;
    hr = serialize_root_signature(root_desc, &root_blob, errors);
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));

    ID3D12PipelineState* pso = nullptr;
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root;
        pso_desc.CS.pShaderBytecode = cs->GetBufferPointer();
        pso_desc.CS.BytecodeLength = cs->GetBufferSize();
        hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    }

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* uav = nullptr;
    ID3D12Resource* readback = nullptr;

    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);

    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uav));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }

    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    }

    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, pso, IID_PPV_ARGS(&list));
    }

    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 64;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(uav, nullptr, &view, heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = uav;
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, uav);
        hr = execute_and_wait(queue, list);
    }

    bool verified = false;
    if (SUCCEEDED(hr)) {
        uint32_t* data = nullptr;
        D3D12_RANGE read_range = {0, 16};
        HRESULT map_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
        verified = SUCCEEDED(map_hr) && data[0] == 7 && data[1] == 14 && data[2] == 21 && data[3] == 28;
        if (SUCCEEDED(map_hr)) {
            D3D12_RANGE write_range = {0, 0};
            readback->Unmap(0, &write_range);
        }
    }

    safe_release(readback);
    safe_release(uav);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(device);
    return {SUCCEEDED(hr) && verified, hr,
            verified ? "compute first-use dispatch compiled and executed correctly"
                     : "compute first-use dispatch verification failed",
            "\"verified\":" + std::string(verified ? "true" : "false")};
}

static ProbeResult probe_rtv_clear() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* target = nullptr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc =
            texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear.Color[0] = 0.25f;
        clear.Color[1] = 0.5f;
        clear.Color[2] = 0.75f;
        clear.Color[3] = 1.0f;
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target));
    }
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        const float clear_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        hr = execute_and_wait(queue, list);
    }

    safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return {SUCCEEDED(hr), hr, SUCCEEDED(hr) ? "offscreen RTV clear executed" : "offscreen RTV clear failed", ""};
}

static HRESULT create_basic_graphics_pso(ID3D12Device* device, const char* vs_target, const char* ps_target,
                                         const char* gs_target, ID3D12PipelineState** pso_out,
                                         ID3D12RootSignature** root_out, std::string& detail) {
    const char* hlsl =
        "struct VSIn{float3 pos:POSITION;float2 uv:TEXCOORD0;};"
        "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
        "VSOut vs_main(VSIn input){VSOut o;o.pos=float4(input.pos,1);o.uv=input.uv;return o;}"
        "float4 ps_main(VSOut input):SV_Target{return float4(input.uv,0.25,1);}"
        "struct GSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
        "[maxvertexcount(3)] void gs_main(triangle VSOut input[3], inout TriangleStream<GSOut> outstream){"
        "for(int i=0;i<3;i++){GSOut o;o.pos=input[i].pos;o.uv=input[i].uv;outstream.Append(o);}}";
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* gs = nullptr;
    HRESULT hr = compile_shader(hlsl, "vs_main", vs_target, &vs, detail);
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "ps_main", ps_target, &ps, detail);
    if (SUCCEEDED(hr) && gs_target)
        hr = compile_shader(hlsl, "gs_main", gs_target, &gs, detail);
    if (FAILED(hr)) {
        safe_release(gs);
        safe_release(ps);
        safe_release(vs);
        return hr;
    }

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    hr = serialize_root_signature(root_desc, &root_blob, detail);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(root_out));

    if (SUCCEEDED(hr)) {
        D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = *root_out;
        desc.VS.pShaderBytecode = vs->GetBufferPointer();
        desc.VS.BytecodeLength = vs->GetBufferSize();
        desc.PS.pShaderBytecode = ps->GetBufferPointer();
        desc.PS.BytecodeLength = ps->GetBufferSize();
        if (gs) {
            desc.GS.pShaderBytecode = gs->GetBufferPointer();
            desc.GS.BytecodeLength = gs->GetBufferSize();
        }
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.InputLayout.pInputElementDescs = input;
        desc.InputLayout.NumElements = 2;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso_out));
    }

    safe_release(root_blob);
    safe_release(gs);
    safe_release(ps);
    safe_release(vs);
    return hr;
}

static ProbeResult probe_graphics_pso() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    std::string detail;
    hr = create_basic_graphics_pso(device, "vs_5_0", "ps_5_0", nullptr, &pso, &root, detail);
    safe_release(pso);
    safe_release(root);
    safe_release(device);
    return {SUCCEEDED(hr), hr, SUCCEEDED(hr) ? "vertex/pixel graphics PSO created" : detail, ""};
}

static ProbeResult probe_geometry_shader_pso() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* vertices = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;
    hr = create_basic_graphics_pso(device, "vs_5_0", "ps_5_0", "gs_5_0", &pso, &root, detail);

    struct Vertex {
        float position[3];
        float uv[2];
    };
    const Vertex triangle[] = {
        {{-0.8f, -0.8f, 0.0f}, {0.1f, 0.1f}},
        {{0.0f, 0.8f, 0.0f}, {0.5f, 0.9f}},
        {{0.8f, -0.8f, 0.0f}, {0.9f, 0.1f}},
    };
    D3D12_RESOURCE_DESC target_desc =
        texture_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 readback_bytes = 0;

    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vertex_desc = buffer_desc(sizeof(triangle));
        hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertices));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = vertices->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, triangle, sizeof(triangle));
            vertices->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &readback_bytes);
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = buffer_desc(readback_bytes);
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        list->SetGraphicsRootSignature(root);
        list->SetPipelineState(pso);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW view = {};
        view.BufferLocation = vertices->GetGPUVirtualAddress();
        view.SizeInBytes = sizeof(triangle);
        view.StrideInBytes = sizeof(Vertex);
        list->IASetVertexBuffers(0, 1, &view);
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, 64, 64};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = execute_and_wait(queue, list);
    }

    uint64_t nonzero_pixels = 0;
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            for (UINT y = 0; y < 64; y++) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(
                    mapped + footprint.Footprint.RowPitch * y);
                for (UINT x = 0; x < 64; x++)
                    nonzero_pixels += row[x] != 0;
            }
            readback->Unmap(0, nullptr);
        }
    }

    safe_release(readback);
    safe_release(target);
    safe_release(vertices);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(device);
    bool verified = SUCCEEDED(hr) && nonzero_pixels > 0;
    return {verified, verified ? S_OK : hr,
            verified ? "geometry shader object/mesh emulation rendered and passed readback"
                     : (detail.empty() ? "geometry shader render readback stayed empty" : detail),
            "\"nonzero_pixels\":" + std::to_string(nonzero_pixels)};
}

static ProbeResult probe_subnautica_geometry_dxil_replay() {
    const std::string corpus_dir_env = getenv_string("D3D12_METAL_SDK_GEOMETRY_CORPUS_DIR");
    std::string corpus_dir = corpus_dir_env.empty() ? "Z:/tmp/dxmt_shader_cache" : corpus_dir_env;
    while (!corpus_dir.empty() && (corpus_dir.back() == '/' || corpus_dir.back() == '\\'))
        corpus_dir.pop_back();

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr)) {
        std::string extra = "\"corpus_dir\":\"" + json_escape(corpus_dir) + "\",\"d3d12_load_source\":\"" +
                            json_escape(g_d3d12_load_source) + "\",\"d3d12_loaded_path\":\"" +
                            json_escape(g_d3d12_loaded_path) +
                            "\",\"d3d12_load_error\":" + std::to_string(g_d3d12_load_error) +
                            ",\"d3d12_proc_error\":" + std::to_string(g_d3d12_proc_error);
        return {false, hr, "device creation failed", extra};
    }

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    std::string detail;
    hr = serialize_root_signature(root_desc, &root_blob, detail);
    ID3D12RootSignature* root = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    safe_release(root_blob);
    if (FAILED(hr)) {
        safe_release(device);
        return {false, hr, detail.empty() ? "root signature creation failed" : detail,
                "\"corpus_dir\":\"" + json_escape(corpus_dir) + "\""};
    }

    const D3D12_INPUT_ELEMENT_DESC layout_attribute0_attribute13[] = {
        {"ATTRIBUTE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"ATTRIBUTE", 13, DXGI_FORMAT_R32_UINT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };
    const D3D12_INPUT_ELEMENT_DESC layout_attribute0_vec2[] = {
        {"ATTRIBUTE", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    struct ReplayCase {
        const char* hash;
        const D3D12_INPUT_ELEMENT_DESC* layout;
        UINT layout_count;
    };
    const ReplayCase cases[] = {
        {"414b1f3b4509d720", layout_attribute0_attribute13, 2},
        {"8b12f030dd908c1b", layout_attribute0_attribute13, 2},
        {"8c4a1c6f7f8e81fc", layout_attribute0_vec2, 1},
        {"a0df6264a1b2037c", layout_attribute0_vec2, 1},
    };

    UINT attempted = 0;
    UINT passed = 0;
    std::string first_failure;
    HRESULT first_hr = S_OK;
    for (const ReplayCase& replay : cases) {
        const std::string base = corpus_dir + "/" + replay.hash + ".geom";
        std::vector<uint8_t> vs;
        std::vector<uint8_t> gs;
        if (!read_binary_file(base + ".gsvs.dxbc", vs) || !read_binary_file(base + ".gsmesh.dxbc", gs)) {
            if (first_failure.empty()) {
                first_failure = std::string("missing captured DXIL blobs for ") + replay.hash;
                first_hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
            continue;
        }
        attempted++;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS.pShaderBytecode = vs.data();
        desc.VS.BytecodeLength = vs.size();
        desc.GS.pShaderBytecode = gs.data();
        desc.GS.BytecodeLength = gs.size();
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.InputLayout.pInputElementDescs = replay.layout;
        desc.InputLayout.NumElements = replay.layout_count;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        ID3D12PipelineState* pso = nullptr;
        HRESULT pso_hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        if (SUCCEEDED(pso_hr)) {
            passed++;
        } else if (first_failure.empty()) {
            first_failure = std::string("captured Subnautica geometry DXIL PSO failed for ") + replay.hash;
            first_hr = pso_hr;
        }
        safe_release(pso);
    }

    safe_release(root);
    safe_release(device);
    const bool ok = attempted == static_cast<UINT>(std::size(cases)) && passed == attempted;
    std::string extra = "\"corpus_dir\":\"" + json_escape(corpus_dir) +
                        "\",\"attempted\":" + std::to_string(attempted) + ",\"passed\":" + std::to_string(passed) +
                        ",\"total\":" + std::to_string(static_cast<UINT>(std::size(cases)));
    return {ok, ok ? S_OK : first_hr,
            ok ? "captured Subnautica geometry DXIL PSOs replayed through CreateGraphicsPipelineState" : first_failure,
            extra};
}

static ProbeResult probe_texture_sample() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12RootSignature* root = nullptr;
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC tex_desc = texture_desc(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* texture = nullptr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap));
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 upload_bytes = 0;
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &rows, &row_size, &upload_bytes);
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_bytes);
        hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    }
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = upload->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const uint8_t pixels[16] = {
                255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
            };
            for (UINT y = 0; y < 2; ++y)
                std::memcpy(mapped + footprint.Footprint.RowPitch * y, pixels + y * 2 * 4, 2 * 4);
            D3D12_RANGE write_range = {0, static_cast<SIZE_T>(upload_bytes)};
            upload->Unmap(0, &write_range);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        hr = execute_and_wait(queue, list);
    }
    if (SUCCEEDED(hr)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    std::string detail;
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* root_blob = nullptr;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        safe_release(root_blob);
    }
    if (SUCCEEDED(hr)) {
        const char* hlsl = "struct VSIn{float3 pos:POSITION;float2 uv:TEXCOORD0;};"
                           "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;};"
                           "Texture2D tx:register(t0);SamplerState smp:register(s0);"
                           "VSOut vs_main(VSIn input){VSOut o;o.pos=float4(input.pos,1);o.uv=input.uv;return o;}"
                           "float4 ps_main(VSOut input):SV_Target{return tx.Sample(smp,input.uv);}";
        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        hr = compile_shader(hlsl, "vs_main", "vs_5_0", &vs, detail);
        if (SUCCEEDED(hr))
            hr = compile_shader(hlsl, "ps_main", "ps_5_0", &ps, detail);
        if (SUCCEEDED(hr)) {
            D3D12_INPUT_ELEMENT_DESC input[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = root;
            desc.VS.pShaderBytecode = vs->GetBufferPointer();
            desc.VS.BytecodeLength = vs->GetBufferSize();
            desc.PS.pShaderBytecode = ps->GetBufferPointer();
            desc.PS.BytecodeLength = ps->GetBufferSize();
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.StencilEnable = FALSE;
            desc.InputLayout.pInputElementDescs = input;
            desc.InputLayout.NumElements = 2;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        }
        safe_release(ps);
        safe_release(vs);
    }

    safe_release(pso);
    safe_release(root);
    safe_release(upload);
    safe_release(texture);
    safe_release(srv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return {SUCCEEDED(hr), hr, SUCCEEDED(hr) ? "texture upload, SRV, static sampler, and sampling PSO created" : detail,
            ""};
}

static ProbeResult probe_dxil_texture_color_output() {
    std::string vs_path = getenv_string("D3D12_METAL_SDK_DXIL_COLOR_VS");
    std::string ps_path = getenv_string("D3D12_METAL_SDK_DXIL_COLOR_PS");
    if (vs_path.empty())
        vs_path = "probe_dxil_color_vs.cso";
    if (ps_path.empty())
        ps_path = "probe_dxil_color_ps.cso";

    std::vector<uint8_t> vs_blob;
    std::vector<uint8_t> ps_blob;
    if (!read_binary_file(vs_path, vs_blob) || !read_binary_file(ps_path, ps_blob)) {
        std::string extra = "\"vs_path\":\"" + json_escape(vs_path) + "\",\"ps_path\":\"" + json_escape(ps_path) + "\"";
        return {false, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "DXIL color probe shader blobs are missing", extra};
    }

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* texture_upload = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;

    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }

    D3D12_RESOURCE_DESC src_tex_desc = texture_desc(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC rt_desc =
        texture_desc(64, 64, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT src_footprint = {};
    UINT src_rows = 0;
    UINT64 src_row_size = 0;
    UINT64 src_upload_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rb_footprint = {};
    UINT rb_rows = 0;
    UINT64 rb_row_size = 0;
    UINT64 rb_bytes = 0;

    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &src_tex_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&src_tex_desc, 0, 1, 0, &src_footprint, &src_rows, &src_row_size,
                                      &src_upload_bytes);
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(src_upload_bytes);
        hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture_upload));
    }
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = texture_upload->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const uint8_t pixels[16] = {
                255, 64, 0, 255, 0, 255, 64, 255, 64, 0, 255, 255, 255, 255, 255, 255,
            };
            for (UINT y = 0; y < 2; ++y)
                std::memcpy(mapped + src_footprint.Footprint.RowPitch * y, pixels + y * 2 * 4, 2 * 4);
            D3D12_RANGE write_range = {0, static_cast<SIZE_T>(src_upload_bytes)};
            texture_upload->Unmap(0, &write_range);
        }
    }
    if (SUCCEEDED(hr)) {
        struct Vertex {
            float px, py, pz;
            float u, v;
        };
        const Vertex vertices[] = {
            {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
            {-1.0f, 3.0f, 0.0f, 0.0f, -1.0f},
            {3.0f, -1.0f, 0.0f, 2.0f, 1.0f},
        };
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(sizeof(vertices));
        hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertex_buffer));
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            D3D12_RANGE read_range = {0, 0};
            hr = vertex_buffer->Map(0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                std::memcpy(mapped, vertices, sizeof(vertices));
                D3D12_RANGE write_range = {0, sizeof(vertices)};
                vertex_buffer->Unmap(0, &write_range);
            }
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 0.0f;
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&rt_desc, 0, 1, 0, &rb_footprint, &rb_rows, &rb_row_size, &rb_bytes);
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC rb_desc = buffer_desc(rb_bytes);
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &srv, srv_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(render_target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    std::string detail;
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* root_blob = nullptr;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        safe_release(root_blob);
    }
    if (SUCCEEDED(hr)) {
        D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS.pShaderBytecode = vs_blob.data();
        desc.VS.BytecodeLength = vs_blob.size();
        desc.PS.pShaderBytecode = ps_blob.data();
        desc.PS.BytecodeLength = ps_blob.size();
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.InputLayout.pInputElementDescs = input;
        desc.InputLayout.NumElements = 2;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr)) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = texture_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = src_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER tex_barrier =
            transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &tex_barrier);

        ID3D12DescriptorHeap* heaps[] = {srv_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root);
        list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
        vbv.SizeInBytes = static_cast<UINT>(sizeof(float) * 5 * 3);
        vbv.StrideInBytes = static_cast<UINT>(sizeof(float) * 5);
        list->IASetVertexBuffers(0, 1, &vbv);
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, 64, 64};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER rt_barrier =
            transition_barrier(render_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &rt_barrier);
        D3D12_TEXTURE_COPY_LOCATION rb_dst = {};
        rb_dst.pResource = readback;
        rb_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        rb_dst.PlacedFootprint = rb_footprint;
        D3D12_TEXTURE_COPY_LOCATION rb_src = {};
        rb_src.pResource = render_target;
        rb_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        rb_src.SubresourceIndex = 0;
        list->CopyTextureRegion(&rb_dst, 0, 0, 0, &rb_src, nullptr);
        hr = execute_and_wait(queue, list);
    }

    uint64_t nonzero_words = 0;
    if (SUCCEEDED(hr)) {
        uint8_t* data = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(rb_bytes)};
        HRESULT map_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
        if (SUCCEEDED(map_hr)) {
            for (UINT y = 0; y < 64; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(data + rb_footprint.Footprint.RowPitch * y);
                for (UINT x = 0; x < 64; ++x) {
                    if (row[x] != 0)
                        nonzero_words++;
                }
            }
            D3D12_RANGE write_range = {0, 0};
            readback->Unmap(0, &write_range);
        } else {
            hr = map_hr;
        }
    }

    safe_release(pso);
    safe_release(root);
    safe_release(readback);
    safe_release(render_target);
    safe_release(vertex_buffer);
    safe_release(texture_upload);
    safe_release(texture);
    safe_release(rtv_heap);
    safe_release(srv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);

    const bool ok = SUCCEEDED(hr) && nonzero_words > 0;
    std::string extra = "\"nonzero_words\":" + std::to_string(nonzero_words) +
                        ",\"rt_format\":\"R10G10B10A2_UNORM\",\"vs_path\":\"" + json_escape(vs_path) +
                        "\",\"ps_path\":\"" + json_escape(ps_path) + "\"";
    return {ok, ok ? S_OK : hr,
            ok ? "SM6 DXIL textured triangle wrote nonzero R10 render-target color"
               : (detail.empty() ? "DXIL color-output readback stayed black or failed" : detail),
            extra};
}

static LRESULT CALLBACK probe_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static ProbeResult probe_swapchain_present() {
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    CreateDXGIFactory2Fn create_factory = load_proc<CreateDXGIFactory2Fn>(dxgi, "CreateDXGIFactory2");
    IDXGIFactory2* factory = nullptr;
    if (create_factory)
        hr = create_factory(0, IID_PPV_ARGS(&factory));
    else
        hr = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = probe_window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MetalSharpD3D12MiniProbeWindow";
    ATOM atom = 0;
    HWND hwnd = nullptr;
    if (SUCCEEDED(hr)) {
        atom = RegisterClassW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"MetalSharp D3D12 Mini Probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, 128, 128, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }

    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* swapchain1 = nullptr;
    HRESULT make_assoc_hr = E_FAIL;
    HRESULT get_assoc_hr = E_FAIL;
    HRESULT get_hwnd_hr = E_FAIL;
    HRESULT get_desc_hr = E_FAIL;
    HRESULT register_occ_hr = E_FAIL;
    HWND associated_hwnd = nullptr;
    HWND swapchain_hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC swapchain_desc = {};
    DWORD occ_cookie = 0;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr)) {
        make_assoc_hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
        get_assoc_hr = factory->GetWindowAssociation(&associated_hwnd);
        register_occ_hr = factory->RegisterOcclusionStatusWindow(hwnd, WM_USER + 17, &occ_cookie);
        if (occ_cookie)
            factory->UnregisterOcclusionStatus(occ_cookie);
    }
    if (SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = 64;
        desc.Height = 64;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swapchain1);
    }
    if (SUCCEEDED(hr)) {
        get_hwnd_hr = swapchain1->GetHwnd(&swapchain_hwnd);
        get_desc_hr = swapchain1->GetDesc(&swapchain_desc);
    }
    HRESULT present_hr = E_FAIL;
    if (SUCCEEDED(hr))
        present_hr = swapchain1->Present(0, 0);

    bool ok = SUCCEEDED(hr) && SUCCEEDED(present_hr) && SUCCEEDED(make_assoc_hr) && SUCCEEDED(get_assoc_hr) &&
              SUCCEEDED(get_hwnd_hr) && SUCCEEDED(get_desc_hr) && SUCCEEDED(register_occ_hr) &&
              associated_hwnd == hwnd && swapchain_hwnd == hwnd && swapchain_desc.OutputWindow == hwnd;
    std::string extra = "\"make_window_association_hr\":\"" + hr_hex(make_assoc_hr) +
                        "\",\"get_window_association_hr\":\"" + hr_hex(get_assoc_hr) +
                        "\",\"register_occlusion_status_window_hr\":\"" + hr_hex(register_occ_hr) +
                        "\",\"create_swapchain_hr\":\"" + hr_hex(hr) + "\",\"get_hwnd_hr\":\"" + hr_hex(get_hwnd_hr) +
                        "\",\"get_desc_hr\":\"" + hr_hex(get_desc_hr) + "\",\"present_hr\":\"" + hr_hex(present_hr) +
                        "\",\"associated_matches\":" + (associated_hwnd == hwnd ? "true" : "false") +
                        ",\"swapchain_hwnd_matches\":" + (swapchain_hwnd == hwnd ? "true" : "false") +
                        ",\"desc_output_window_matches\":" + (swapchain_desc.OutputWindow == hwnd ? "true" : "false");
    safe_release(swapchain1);
    safe_release(queue);
    if (hwnd)
        DestroyWindow(hwnd);
    if (atom)
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    safe_release(factory);
    safe_release(device);
    return {ok, ok ? S_OK : hr, ok ? "CreateSwapChainForHwnd and Present succeeded" : "swapchain/present failed",
            extra};
}

static ProbeResult probe_mesh_shader_pso() {
    std::vector<uint8_t> amplification_shader;
    std::vector<uint8_t> mesh_shader;
    std::vector<uint8_t> pixel_shader;
    if (!read_binary_file("probe_mesh_shader_as.cso", amplification_shader) ||
        !read_binary_file("probe_mesh_shader_ms.cso", mesh_shader) ||
        !read_binary_file("probe_mesh_shader_ps.cso", pixel_shader)) {
        return {false, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                "mesh/pixel shader probe blobs are missing",
                "\"pso_attempted\":false"};
    }

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    const HRESULT options7_hr = device->CheckFeatureSupport(
        D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));

    ID3D12Device2* device2 = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12PipelineState* repeated_pso = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList6* list6 = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12CommandSignature* mesh_signature = nullptr;
    ID3D12Resource* indirect_args = nullptr;
    ID3D12Resource* stage_constants = nullptr;
    ID3D12Resource* stage_srvs = nullptr;
    ID3D12Resource* mesh_output = nullptr;
    ID3D12Resource* mesh_output_readback = nullptr;
    ID3D12Resource* mesh_texture = nullptr;
    ID3D12Resource* mesh_texture_upload = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;
    hr = device->QueryInterface(IID_PPV_ARGS(&device2));

    D3D12_DESCRIPTOR_RANGE srv_ranges[5] = {};
    srv_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[0].NumDescriptors = 1;
    srv_ranges[0].BaseShaderRegister = 0;
    srv_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    srv_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[1].NumDescriptors = 1;
    srv_ranges[1].BaseShaderRegister = 1;
    srv_ranges[1].OffsetInDescriptorsFromTableStart = 0;
    srv_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    srv_ranges[2].NumDescriptors = 1;
    srv_ranges[2].BaseShaderRegister = 0;
    srv_ranges[2].OffsetInDescriptorsFromTableStart = 0;
    srv_ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[3].NumDescriptors = 1;
    srv_ranges[3].BaseShaderRegister = 1;
    srv_ranges[3].OffsetInDescriptorsFromTableStart = 0;
    srv_ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    srv_ranges[4].NumDescriptors = 1;
    srv_ranges[4].BaseShaderRegister = 0;
    srv_ranges[4].OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER root_params[7] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[0].Descriptor.ShaderRegister = 0;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_params[1].Descriptor.ShaderRegister = 1;
    root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    root_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[2].DescriptorTable.NumDescriptorRanges = 1;
    root_params[2].DescriptorTable.pDescriptorRanges = &srv_ranges[0];
    root_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    root_params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[3].DescriptorTable.NumDescriptorRanges = 1;
    root_params[3].DescriptorTable.pDescriptorRanges = &srv_ranges[1];
    root_params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
    root_params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[4].DescriptorTable.NumDescriptorRanges = 1;
    root_params[4].DescriptorTable.pDescriptorRanges = &srv_ranges[2];
    root_params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    root_params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[5].DescriptorTable.NumDescriptorRanges = 1;
    root_params[5].DescriptorTable.pDescriptorRanges = &srv_ranges[3];
    root_params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    root_params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[6].DescriptorTable.NumDescriptorRanges = 1;
    root_params[6].DescriptorTable.pDescriptorRanges = &srv_ranges[4];
    root_params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 7;
    root_desc.pParameters = root_params;
    if (SUCCEEDED(hr))
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));

    struct alignas(void*) MeshPipelineStream {
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
                                ID3D12RootSignature*> root;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS,
                                D3D12_SHADER_BYTECODE> amplification;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,
                                D3D12_SHADER_BYTECODE> mesh;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
                                D3D12_SHADER_BYTECODE> pixel;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,
                                D3D12_BLEND_DESC> blend;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,
                                UINT> sample_mask;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,
                                D3D12_RASTERIZER_DESC> rasterizer;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,
                                D3D12_DEPTH_STENCIL_DESC> depth_stencil;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,
                                D3D12_RT_FORMAT_ARRAY> render_targets;
        PipelineStreamSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,
                                DXGI_SAMPLE_DESC> sample_desc;
    } stream = {};
    stream.root.value = root;
    stream.amplification.value = {amplification_shader.data(), amplification_shader.size()};
    stream.mesh.value = {mesh_shader.data(), mesh_shader.size()};
    stream.pixel.value = {pixel_shader.data(), pixel_shader.size()};
    stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sample_mask.value = UINT_MAX;
    stream.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.rasterizer.value.DepthClipEnable = TRUE;
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    stream.render_targets.value.NumRenderTargets = 1;
    stream.render_targets.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.sample_desc.value.Count = 1;

    if (SUCCEEDED(hr)) {
        D3D12_PIPELINE_STATE_STREAM_DESC desc = {sizeof(stream), &stream};
        hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (SUCCEEDED(hr))
            hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(&repeated_pso));
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                       IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list6));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 4;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&resource_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&sampler_heap));
    }

    D3D12_RESOURCE_DESC target_desc =
        texture_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 readback_bytes = 0;
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&target));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows,
                                      &row_bytes, &readback_bytes);
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = buffer_desc(readback_bytes);
        hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                             &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_INDIRECT_ARGUMENT_DESC argument_desc = {};
        argument_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
        signature_desc.ByteStride = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        signature_desc.NumArgumentDescs = 1;
        signature_desc.pArgumentDescs = &argument_desc;
        hr = device->CreateCommandSignature(&signature_desc, nullptr,
                                            IID_PPV_ARGS(&mesh_signature));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC args_desc =
            buffer_desc(sizeof(D3D12_DISPATCH_MESH_ARGUMENTS));
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &args_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&indirect_args));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = indirect_args->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            const D3D12_DISPATCH_MESH_ARGUMENTS args = {2, 1, 1};
            std::memcpy(mapped, &args, sizeof(args));
            indirect_args->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC constants_desc = buffer_desc(512);
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &constants_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&stage_constants));
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = stage_constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const float mesh_scale = 1.0f;
            const uint32_t amplification_enabled = 1;
            std::memcpy(mapped, &mesh_scale, sizeof(mesh_scale));
            std::memcpy(mapped + 256, &amplification_enabled,
                        sizeof(amplification_enabled));
            stage_constants->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC srv_buffer_desc = buffer_desc(512);
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &srv_buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&stage_srvs));
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = stage_srvs->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const float mesh_control = 1.0f;
            const uint32_t amplification_control = 1;
            std::memcpy(mapped, &mesh_control, sizeof(mesh_control));
            std::memcpy(mapped + 256, &amplification_control,
                        sizeof(amplification_control));
            stage_srvs->Unmap(0, nullptr);
        }
        if (SUCCEEDED(hr)) {
            const UINT increment = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                resource_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = 1;
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            srv.Buffer.FirstElement = 0;
            device->CreateShaderResourceView(stage_srvs, &srv, cpu);
            cpu.ptr += increment;
            srv.Buffer.FirstElement = 64;
            device->CreateShaderResourceView(stage_srvs, &srv, cpu);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC output_desc = buffer_desc(256);
        output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&mesh_output));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC output_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mesh_output_readback));
    }
    if (SUCCEEDED(hr)) {
        const UINT increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            resource_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += 2 * increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(mesh_output, nullptr, &uav, cpu);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT mesh_texture_footprint = {};
    UINT mesh_texture_rows = 0;
    UINT64 mesh_texture_row_bytes = 0;
    UINT64 mesh_texture_upload_bytes = 0;
    D3D12_RESOURCE_DESC mesh_texture_desc =
        texture_desc(1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&mesh_texture_desc, 0, 1, 0,
                                      &mesh_texture_footprint,
                                      &mesh_texture_rows,
                                      &mesh_texture_row_bytes,
                                      &mesh_texture_upload_bytes);
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &mesh_texture_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mesh_texture));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(mesh_texture_upload_bytes);
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&mesh_texture_upload));
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = mesh_texture_upload->Map(0, nullptr,
                                          reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const float value = 0.5f;
            std::memcpy(mapped + mesh_texture_footprint.Offset, &value,
                        sizeof(value));
            mesh_texture_upload->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        const UINT increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            resource_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += 3 * increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mesh_texture, &srv, cpu);

        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        device->CreateSampler(
            &sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
    }
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        list6->SetGraphicsRootSignature(root);
        list6->SetGraphicsRootConstantBufferView(
            0, stage_constants->GetGPUVirtualAddress());
        list6->SetGraphicsRootConstantBufferView(
            1, stage_constants->GetGPUVirtualAddress() + 256);
        ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
        list6->SetDescriptorHeaps(2, heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            resource_heap->GetGPUDescriptorHandleForHeapStart();
        list6->SetGraphicsRootDescriptorTable(2, gpu);
        gpu.ptr += device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list6->SetGraphicsRootDescriptorTable(3, gpu);
        gpu.ptr += device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list6->SetGraphicsRootDescriptorTable(4, gpu);
        gpu.ptr += device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list6->SetGraphicsRootDescriptorTable(5, gpu);
        list6->SetGraphicsRootDescriptorTable(
            6, sampler_heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = mesh_texture_upload;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        texture_src.PlacedFootprint = mesh_texture_footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_dst = {};
        texture_dst.pResource = mesh_texture;
        texture_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list6->CopyTextureRegion(&texture_dst, 0, 0, 0, &texture_src, nullptr);
        D3D12_RESOURCE_BARRIER texture_barrier = transition_barrier(
            mesh_texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list6->ResourceBarrier(1, &texture_barrier);
        list6->SetPipelineState(pso);
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 64.0f, 64.0f, 0.0f, 1.0f};
        D3D12_RECT left_scissor = {0, 0, 32, 64};
        D3D12_RECT right_scissor = {32, 0, 64, 64};
        list6->RSSetViewports(1, &viewport);
        list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4] = {};
        list6->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list6->RSSetScissorRects(1, &left_scissor);
        list6->DispatchMesh(2, 1, 1);
        list6->RSSetScissorRects(1, &right_scissor);
        list6->ExecuteIndirect(mesh_signature, 1, indirect_args, 0, nullptr, 0);
        D3D12_RESOURCE_BARRIER output_barrier = transition_barrier(
            mesh_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list6->ResourceBarrier(1, &output_barrier);
        list6->CopyBufferRegion(mesh_output_readback, 0, mesh_output, 0, 256);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list6->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list6->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = execute_and_wait(queue, list6);
    }

    uint64_t nonzero_pixels = 0;
    uint64_t direct_pixels = 0;
    uint64_t indirect_pixels = 0;
    uint32_t mesh_output_value = 0;
    uint32_t mesh_lane_values[32] = {};
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            for (UINT y = 0; y < 64; y++) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(
                    mapped + footprint.Footprint.RowPitch * y);
                for (UINT x = 0; x < 64; x++) {
                    const bool nonzero = row[x] != 0;
                    nonzero_pixels += nonzero;
                    if (x < 32)
                        direct_pixels += nonzero;
                    else
                        indirect_pixels += nonzero;
                }
            }
            readback->Unmap(0, nullptr);
        }
    }

    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE read_range = {0, 256};
        hr = mesh_output_readback->Map(0, &read_range, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(&mesh_output_value, mapped, sizeof(mesh_output_value));
            std::memcpy(mesh_lane_values, static_cast<uint8_t*>(mapped) + 8,
                        sizeof(mesh_lane_values));
            D3D12_RANGE written = {0, 0};
            mesh_output_readback->Unmap(0, &written);
        }
    }

    safe_release(mesh_texture_upload);
    safe_release(mesh_texture);
    safe_release(mesh_output_readback);
    safe_release(mesh_output);
    safe_release(readback);
    safe_release(target);
    safe_release(stage_srvs);
    safe_release(stage_constants);
    safe_release(indirect_args);
    safe_release(mesh_signature);
    safe_release(sampler_heap);
    safe_release(resource_heap);
    safe_release(rtv_heap);
    safe_release(list6);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(root_blob);
    safe_release(repeated_pso);
    safe_release(pso);
    safe_release(root);
    safe_release(device2);
    safe_release(device);
    bool mesh_lane_values_verified = true;
    for (uint32_t lane = 0; lane < 32; lane++)
        mesh_lane_values_verified &=
            mesh_lane_values[lane] == 0x4153504c + lane;
    const bool verified =
        SUCCEEDED(hr) && SUCCEEDED(options7_hr) &&
        options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED &&
        direct_pixels >= 100 && direct_pixels <= 400 &&
        indirect_pixels >= 100 && indirect_pixels <= 400 &&
        mesh_output_value == 0x4d534831 && mesh_lane_values_verified;
    return {verified, verified ? S_OK : hr,
            verified ? "native D3D12 AS/MS direct and indirect DispatchMesh rendered; tier remains conservative"
                     : (detail.empty() ? "native mesh shader dispatch/readback failed" : detail),
            "\"pso_attempted\":true,\"repeated_pso_created\":true" +
                std::string(",\"stage_cbvs_bound\":true") +
                std::string(",\"stage_srvs_bound\":true") +
                std::string(",\"mesh_uav_bound\":true") +
                std::string(",\"mesh_texture_sampler_bound\":true") +
                std::string(",\"amplification_shader_bytes\":") +
                std::to_string(amplification_shader.size()) +
                ",\"mesh_shader_bytes\":" + std::to_string(mesh_shader.size()) +
                ",\"mesh_shader_tier\":" +
                std::to_string(static_cast<UINT>(options7.MeshShaderTier)) +
                ",\"nonzero_pixels\":" + std::to_string(nonzero_pixels) +
                ",\"direct_pixels\":" + std::to_string(direct_pixels) +
                ",\"indirect_pixels\":" + std::to_string(indirect_pixels) +
                ",\"mesh_output_value\":" + std::to_string(mesh_output_value) +
                ",\"mesh_texture_scale\":0.5" +
                std::string(",\"mesh_threadgroup_width\":32") +
                std::string(",\"dispatch_mesh_groups_x\":2") +
                ",\"mesh_lane_values_verified\":" +
                (mesh_lane_values_verified ? "true" : "false") +
                ",\"d3d12_loaded_path\":\"" + json_escape(g_d3d12_loaded_path) + "\""};
}

static ProbeResult probe_dxr_acceleration_structures() {
    std::vector<uint8_t> ray_query_shader;
    std::vector<uint8_t> raygen_library;
    if (!read_binary_file("probe_dxr_inline.cso", ray_query_shader) ||
        !read_binary_file("probe_dxr_raygen.cso", raygen_library)) {
        return {false, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                "inline ray-query or raygen shader blob is missing", ""};
    }
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    if (FAILED(hr))
        return {false, hr, "device creation failed", ""};

    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr;
    ID3D12RootSignature* compute_root = nullptr;
    ID3D12PipelineState* compute_pso = nullptr;
    ID3D12StateObject* raytracing_state = nullptr;
    ID3D12StateObjectProperties* raytracing_properties = nullptr;
    ID3D12DescriptorHeap* ray_query_heap = nullptr;
    ID3D12Resource* vertices = nullptr;
    ID3D12Resource* acceleration_structure = nullptr;
    ID3D12Resource* scratch = nullptr;
    ID3D12Resource* postbuild = nullptr;
    ID3D12Resource* instances = nullptr;
    ID3D12Resource* top_level_acceleration_structure = nullptr;
    ID3D12Resource* top_level_scratch = nullptr;
    ID3D12Resource* top_level_postbuild = nullptr;
    ID3D12Resource* ray_query_output = nullptr;
    ID3D12Resource* ray_query_readback = nullptr;
    ID3D12Resource* raygen_shader_table = nullptr;
    hr = device->QueryInterface(IID_PPV_ARGS(&device5));
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list4));

    D3D12_DESCRIPTOR_RANGE compute_ranges[2] = {};
    compute_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compute_ranges[0].NumDescriptors = 1;
    compute_ranges[0].BaseShaderRegister = 0;
    compute_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    compute_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compute_ranges[1].NumDescriptors = 1;
    compute_ranges[1].BaseShaderRegister = 0;
    compute_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER compute_param = {};
    compute_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compute_param.DescriptorTable.NumDescriptorRanges = 2;
    compute_param.DescriptorTable.pDescriptorRanges = compute_ranges;
    compute_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC compute_root_desc = {};
    compute_root_desc.NumParameters = 1;
    compute_root_desc.pParameters = &compute_param;
    ID3DBlob* compute_root_blob = nullptr;
    std::string compute_detail;
    if (SUCCEEDED(hr))
        hr = serialize_root_signature(compute_root_desc, &compute_root_blob,
                                      compute_detail);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(
            0, compute_root_blob->GetBufferPointer(),
            compute_root_blob->GetBufferSize(), IID_PPV_ARGS(&compute_root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {};
        compute_desc.pRootSignature = compute_root;
        compute_desc.CS = {ray_query_shader.data(), ray_query_shader.size()};
        hr = device->CreateComputePipelineState(&compute_desc,
                                                IID_PPV_ARGS(&compute_pso));
    }
    safe_release(compute_root_blob);

    D3D12_EXPORT_DESC raygen_exports[5] = {};
    raygen_exports[0].Name = L"raygen";
    raygen_exports[1].Name = L"miss_shader";
    raygen_exports[2].Name = L"closest_hit";
    raygen_exports[3].Name = L"callable_shader";
    raygen_exports[4].Name = L"any_hit";
    D3D12_DXIL_LIBRARY_DESC raygen_library_desc = {};
    raygen_library_desc.DXILLibrary = {raygen_library.data(),
                                      raygen_library.size()};
    raygen_library_desc.NumExports = 5;
    raygen_library_desc.pExports = raygen_exports;
    D3D12_GLOBAL_ROOT_SIGNATURE global_root = {compute_root};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config = {};
    shader_config.MaxPayloadSizeInBytes = 4;
    shader_config.MaxAttributeSizeInBytes = 8;
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = {};
    pipeline_config.MaxTraceRecursionDepth = 1;
    D3D12_HIT_GROUP_DESC hit_group = {};
    hit_group.HitGroupExport = L"hit_group";
    hit_group.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hit_group.ClosestHitShaderImport = L"closest_hit";
    hit_group.AnyHitShaderImport = L"any_hit";
    D3D12_STATE_SUBOBJECT state_subobjects[5] = {};
    state_subobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
                           &raygen_library_desc};
    state_subobjects[1] = {
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root};
    state_subobjects[2] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
                           &shader_config};
    state_subobjects[3] = {
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &pipeline_config};
    state_subobjects[4] = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit_group};
    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_desc.NumSubobjects = 5;
    state_desc.pSubobjects = state_subobjects;
    if (SUCCEEDED(hr))
        hr = device5->CreateStateObject(&state_desc,
                                        IID_PPV_ARGS(&raytracing_state));
    if (SUCCEEDED(hr))
        hr = raytracing_state->QueryInterface(
            IID_PPV_ARGS(&raytracing_properties));

    const void* raygen_identifier =
        SUCCEEDED(hr) ? raytracing_properties->GetShaderIdentifier(L"raygen")
                      : nullptr;
    const void* miss_identifier =
        SUCCEEDED(hr)
            ? raytracing_properties->GetShaderIdentifier(L"miss_shader")
            : nullptr;
    const void* hit_group_identifier =
        SUCCEEDED(hr)
            ? raytracing_properties->GetShaderIdentifier(L"hit_group")
            : nullptr;
    const void* callable_identifier =
        SUCCEEDED(hr)
            ? raytracing_properties->GetShaderIdentifier(L"callable_shader")
            : nullptr;
    const void* repeated_raygen_identifier =
        SUCCEEDED(hr) ? raytracing_properties->GetShaderIdentifier(L"raygen")
                      : nullptr;
    const void* unknown_identifier =
        SUCCEEDED(hr)
            ? raytracing_properties->GetShaderIdentifier(L"unknown_export")
            : nullptr;
    const bool distinct_shader_identifiers =
        raygen_identifier && miss_identifier && hit_group_identifier &&
        callable_identifier &&
        std::memcmp(raygen_identifier, miss_identifier, 32) != 0 &&
        std::memcmp(raygen_identifier, hit_group_identifier, 32) != 0 &&
        std::memcmp(miss_identifier, hit_group_identifier, 32) != 0 &&
        std::memcmp(raygen_identifier, callable_identifier, 32) != 0 &&
        std::memcmp(miss_identifier, callable_identifier, 32) != 0 &&
        std::memcmp(hit_group_identifier, callable_identifier, 32) != 0;
    const bool stable_shader_identifiers =
        raygen_identifier && repeated_raygen_identifier &&
        std::memcmp(raygen_identifier, repeated_raygen_identifier, 32) == 0 &&
        unknown_identifier == nullptr;
    if (SUCCEEDED(hr) &&
        (!distinct_shader_identifiers || !stable_shader_identifiers))
        hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC table_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &table_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&raygen_shader_table));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = raygen_shader_table->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, raygen_identifier, 32);
            std::memcpy(static_cast<uint8_t*>(mapped) + 64, miss_identifier,
                        32);
            std::memcpy(static_cast<uint8_t*>(mapped) + 128,
                        hit_group_identifier, 32);
            std::memcpy(static_cast<uint8_t*>(mapped) + 192,
                        callable_identifier, 32);
            raygen_shader_table->Unmap(0, nullptr);
        }
    }

    const float triangle[9] = {
        -0.75f, -0.75f, 0.0f,
         0.0f,   0.75f, 0.0f,
         0.75f, -0.75f, 0.0f,
    };
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vertex_desc = buffer_desc(sizeof(triangle));
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&vertices));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = vertices->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, triangle, sizeof(triangle));
            vertices->Unmap(0, nullptr);
        }
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.VertexBuffer.StartAddress =
        vertices ? vertices->GetGPUVirtualAddress() : 0;
    geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    geometry.Triangles.VertexCount = 3;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = 1;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = &geometry;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild = {};
    if (SUCCEEDED(hr)) {
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,
                                                                 &prebuild);
        if (!prebuild.ResultDataMaxSizeInBytes ||
            !prebuild.ScratchDataSizeInBytes)
            hr = E_NOTIMPL;
    }

    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC as_desc = buffer_desc(prebuild.ResultDataMaxSizeInBytes);
        as_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &as_desc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&acceleration_structure));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC scratch_desc = buffer_desc(prebuild.ScratchDataSizeInBytes);
        scratch_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &scratch_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&scratch));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC postbuild_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &postbuild_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&postbuild));
    }

    D3D12_RAYTRACING_INSTANCE_DESC instance = {};
    instance.Transform[0][0] = 1.0f;
    instance.Transform[1][1] = 1.0f;
    instance.Transform[2][2] = 1.0f;
    instance.InstanceID = 7;
    instance.InstanceMask = 0xff;
    instance.Flags =
        D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    instance.AccelerationStructure =
        acceleration_structure ? acceleration_structure->GetGPUVirtualAddress() : 0;
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC instance_desc = buffer_desc(sizeof(instance));
        hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &instance_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&instances));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = instances->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, &instance, sizeof(instance));
            instances->Unmap(0, nullptr);
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top_level_inputs = {};
    top_level_inputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top_level_inputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top_level_inputs.NumDescs = 1;
    top_level_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    top_level_inputs.InstanceDescs =
        instances ? instances->GetGPUVirtualAddress() : 0;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO top_level_prebuild = {};
    if (SUCCEEDED(hr)) {
        device5->GetRaytracingAccelerationStructurePrebuildInfo(
            &top_level_inputs, &top_level_prebuild);
        if (!top_level_prebuild.ResultDataMaxSizeInBytes ||
            !top_level_prebuild.ScratchDataSizeInBytes)
            hr = E_NOTIMPL;
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC tlas_desc =
            buffer_desc(top_level_prebuild.ResultDataMaxSizeInBytes);
        tlas_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tlas_desc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
            IID_PPV_ARGS(&top_level_acceleration_structure));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC scratch_desc =
            buffer_desc(top_level_prebuild.ScratchDataSizeInBytes);
        scratch_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &scratch_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&top_level_scratch));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC postbuild_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &postbuild_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&top_level_postbuild));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 2;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&ray_query_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC output_desc = buffer_desc(256);
        output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&ray_query_output));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&ray_query_readback));
    }
    if (SUCCEEDED(hr)) {
        const UINT increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            ray_query_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC acceleration_srv = {};
        acceleration_srv.ViewDimension =
            D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        acceleration_srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        acceleration_srv.RaytracingAccelerationStructure.Location =
            top_level_acceleration_structure->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &acceleration_srv, cpu);

        cpu.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav = {};
        output_uav.Format = DXGI_FORMAT_R32_TYPELESS;
        output_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        output_uav.Buffer.NumElements = 64;
        output_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(ray_query_output, nullptr,
                                          &output_uav, cpu);
    }
    if (SUCCEEDED(hr)) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
        build.DestAccelerationStructureData =
            acceleration_structure->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        build.Inputs = inputs;
        list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post = {};
        post.DestBuffer = postbuild->GetGPUVirtualAddress();
        post.InfoType =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
        D3D12_GPU_VIRTUAL_ADDRESS source =
            acceleration_structure->GetGPUVirtualAddress();
        list4->EmitRaytracingAccelerationStructurePostbuildInfo(&post, 1,
                                                                 &source);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top_level_build = {};
        top_level_build.DestAccelerationStructureData =
            top_level_acceleration_structure->GetGPUVirtualAddress();
        top_level_build.ScratchAccelerationStructureData =
            top_level_scratch->GetGPUVirtualAddress();
        top_level_build.Inputs = top_level_inputs;
        list4->BuildRaytracingAccelerationStructure(&top_level_build, 0,
                                                     nullptr);
        post.DestBuffer = top_level_postbuild->GetGPUVirtualAddress();
        source = top_level_acceleration_structure->GetGPUVirtualAddress();
        list4->EmitRaytracingAccelerationStructurePostbuildInfo(&post, 1,
                                                                 &source);

        ID3D12DescriptorHeap* heaps[] = {ray_query_heap};
        list4->SetDescriptorHeaps(1, heaps);
        list4->SetComputeRootSignature(compute_root);
        list4->SetPipelineState(compute_pso);
        list4->SetComputeRootDescriptorTable(
            0, ray_query_heap->GetGPUDescriptorHandleForHeapStart());
        list4->Dispatch(1, 1, 1);
        list4->SetPipelineState1(raytracing_state);
        D3D12_DISPATCH_RAYS_DESC dispatch_rays = {};
        dispatch_rays.RayGenerationShaderRecord.StartAddress =
            raygen_shader_table->GetGPUVirtualAddress();
        dispatch_rays.RayGenerationShaderRecord.SizeInBytes = 32;
        dispatch_rays.MissShaderTable.StartAddress =
            raygen_shader_table->GetGPUVirtualAddress() + 64;
        dispatch_rays.MissShaderTable.SizeInBytes = 32;
        dispatch_rays.MissShaderTable.StrideInBytes = 32;
        dispatch_rays.HitGroupTable.StartAddress =
            raygen_shader_table->GetGPUVirtualAddress() + 128;
        dispatch_rays.HitGroupTable.SizeInBytes = 32;
        dispatch_rays.HitGroupTable.StrideInBytes = 32;
        dispatch_rays.CallableShaderTable.StartAddress =
            raygen_shader_table->GetGPUVirtualAddress() + 192;
        dispatch_rays.CallableShaderTable.SizeInBytes = 32;
        dispatch_rays.CallableShaderTable.StrideInBytes = 32;
        dispatch_rays.Width = 2;
        dispatch_rays.Height = 1;
        dispatch_rays.Depth = 1;
        list4->DispatchRays(&dispatch_rays);
        D3D12_RESOURCE_BARRIER output_barrier = transition_barrier(
            ray_query_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list4->ResourceBarrier(1, &output_barrier);
        list4->CopyBufferRegion(ray_query_readback, 0, ray_query_output, 0,
                                sizeof(uint32_t) * 5);
        hr = execute_and_wait(queue, list4);
    }

    uint64_t current_size = 0;
    uint64_t top_level_current_size = 0;
    uint32_t ray_hit = 0;
    uint32_t miss_value = 0;
    uint32_t closest_hit_value = 0;
    uint32_t callable_value = 0;
    uint32_t raygen_value = 0;
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(current_size)};
        hr = postbuild->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(&current_size, mapped, sizeof(current_size));
            postbuild->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(top_level_current_size)};
        hr = top_level_postbuild->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(&top_level_current_size, mapped,
                        sizeof(top_level_current_size));
            top_level_postbuild->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        void* mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(uint32_t) * 5};
        hr = ray_query_readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(&ray_hit, mapped, sizeof(ray_hit));
            std::memcpy(&miss_value,
                        static_cast<const uint8_t*>(mapped) + sizeof(ray_hit),
                        sizeof(miss_value));
            std::memcpy(&closest_hit_value,
                        static_cast<const uint8_t*>(mapped) +
                            sizeof(ray_hit) + sizeof(miss_value),
                        sizeof(closest_hit_value));
            std::memcpy(&callable_value,
                        static_cast<const uint8_t*>(mapped) +
                            sizeof(ray_hit) + sizeof(miss_value) +
                            sizeof(closest_hit_value),
                        sizeof(callable_value));
            std::memcpy(&raygen_value,
                        static_cast<const uint8_t*>(mapped) +
                            sizeof(ray_hit) + sizeof(miss_value) +
                            sizeof(closest_hit_value) + sizeof(callable_value),
                        sizeof(raygen_value));
            ray_query_readback->Unmap(0, nullptr);
        }
    }
    const HRESULT removed_reason = device->GetDeviceRemovedReason();
    const bool verified = SUCCEEDED(hr) && SUCCEEDED(removed_reason) &&
                          current_size > 0 &&
                          current_size <= prebuild.ResultDataMaxSizeInBytes &&
                          top_level_current_size > 0 &&
                          top_level_current_size <=
                              top_level_prebuild.ResultDataMaxSizeInBytes &&
                          ray_hit == 1 && miss_value == 0x4d495353 &&
                          closest_hit_value == 0x48495441 &&
                          callable_value == 0x43414c4c &&
                          raygen_value == 42;

    safe_release(raygen_shader_table);
    safe_release(raytracing_properties);
    safe_release(raytracing_state);
    safe_release(ray_query_readback);
    safe_release(ray_query_output);
    safe_release(top_level_postbuild);
    safe_release(top_level_scratch);
    safe_release(top_level_acceleration_structure);
    safe_release(instances);
    safe_release(postbuild);
    safe_release(scratch);
    safe_release(acceleration_structure);
    safe_release(vertices);
    safe_release(ray_query_heap);
    safe_release(compute_pso);
    safe_release(compute_root);
    safe_release(list4);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device5);
    safe_release(device);
    return {verified, verified ? S_OK : hr,
            verified ? "Metal BLAS/TLAS, inline RayQuery, and raygen/miss/any-hit/closest-hit/callable DispatchRays passed"
                     : "DXR acceleration-structure, inline-ray, or raygen gate failed",
            "\"prebuild_result_bytes\":" +
                std::to_string(prebuild.ResultDataMaxSizeInBytes) +
                ",\"ray_query_pso_created\":true" +
                ",\"prebuild_scratch_bytes\":" +
                std::to_string(prebuild.ScratchDataSizeInBytes) +
                ",\"current_size_bytes\":" + std::to_string(current_size) +
                ",\"tlas_prebuild_result_bytes\":" +
                std::to_string(top_level_prebuild.ResultDataMaxSizeInBytes) +
                ",\"tlas_prebuild_scratch_bytes\":" +
                std::to_string(top_level_prebuild.ScratchDataSizeInBytes) +
                ",\"tlas_current_size_bytes\":" +
                std::to_string(top_level_current_size) +
                ",\"inline_ray_hit\":" + std::to_string(ray_hit) +
                ",\"miss_dispatch_value\":" +
                std::to_string(miss_value) +
                ",\"closest_hit_dispatch_value\":" +
                std::to_string(closest_hit_value) +
                ",\"callable_dispatch_value\":" +
                std::to_string(callable_value) +
                ",\"raygen_dispatch_value\":" +
                std::to_string(raygen_value) +
                ",\"miss_identifier_nonnull\":" +
                (miss_identifier ? "true" : "false") +
                ",\"distinct_shader_identifiers\":" +
                (distinct_shader_identifiers ? "true" : "false") +
                ",\"stable_shader_identifiers\":" +
                (stable_shader_identifiers ? "true" : "false") +
                ",\"unknown_identifier_null\":" +
                (!unknown_identifier ? "true" : "false") +
                ",\"removed_reason\":\"" + hr_hex(removed_reason) + "\""};
}

static ProbeResult run_probe() {
    switch (MINI_PROBE_CASE) {
    case 1:
        return probe_create_device();
    case 2:
        return probe_command_queue();
    case 3:
        return probe_swapchain_present();
    case 4:
        return probe_rtv_clear();
    case 5:
        return probe_compute_dispatch();
    case 6:
        return probe_root_signature();
    case 7:
        return probe_descriptors();
    case 8:
        return probe_graphics_pso();
    case 9:
        return probe_geometry_shader_pso();
    case 10:
        return probe_mesh_shader_pso();
    case 11:
        return probe_texture_sample();
    case 12:
        return probe_subnautica_geometry_dxil_replay();
    case 13:
        return probe_dxil_texture_color_output();
    case 14:
        return probe_compute_first_use_dispatch();
    case 15:
        return probe_dxr_acceleration_structures();
    default:
        return {false, E_INVALIDARG, "unknown mini probe case", ""};
    }
}

int main() {
    ProbeResult result = run_probe();
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.mini-probe.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"probe\": \"%s\",\n", MINI_PROBE_NAME);
    std::printf("  \"ok\": %s,\n", result.ok ? "true" : "false");
    std::printf("  \"hr\": \"%s\",\n", hr_hex(result.hr).c_str());
    std::printf("  \"detail\": \"%s\"", json_escape(result.detail).c_str());
    if (!result.extra.empty())
        std::printf(",\n  %s", result.extra.c_str());
    std::printf("\n}\n");
    return 0;
}
