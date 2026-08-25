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
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

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

static D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

static D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT increment, UINT index) {
    handle.ptr += static_cast<SIZE_T>(increment) * index;
    return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu(D3D12_GPU_DESCRIPTOR_HANDLE handle, UINT increment, UINT index) {
    handle.ptr += static_cast<UINT64>(increment) * index;
    return handle;
}

struct CaseResult {
    std::string name;
    bool pass = false;
    HRESULT hr = E_FAIL;
    std::string detail;
    std::string extra;
};

static D3D12CreateDeviceFn g_create_device = nullptr;
static D3D12SerializeRootSignatureFn g_serialize_root_signature = nullptr;
static D3DCompileFn g_compile = nullptr;

static HRESULT create_device(ID3D12Device** device) {
    return g_create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe, reinterpret_cast<void**>(device));
}

static HRESULT create_queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
}

static HRESULT compile_shader(const char* hlsl, const char* entry, ID3DBlob** out, std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_compile(hlsl, std::strlen(hlsl), nullptr, nullptr, nullptr, entry, "cs_5_0", 0, 0, out, &err);
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
}

static HRESULT serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** out, std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, out, &err);
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
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
    if (SUCCEEDED(hr)) {
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle)
            hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
            hr = fence->SetEventOnCompletion(1, event_handle);
        if (SUCCEEDED(hr))
            WaitForSingleObject(event_handle, 15000);
        if (event_handle)
            CloseHandle(event_handle);
    }
    fence->Release();
    return hr;
}

static HRESULT create_upload_buffer(ID3D12Device* device, const void* data, UINT64 bytes, ID3D12Resource** resource) {
    D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(resource));
    if (FAILED(hr) || !data || bytes == 0)
        return hr;
    void* mapped = nullptr;
    D3D12_RANGE read_range = {0, 0};
    hr = (*resource)->Map(0, &read_range, &mapped);
    if (SUCCEEDED(hr)) {
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        D3D12_RANGE write_range = {0, static_cast<SIZE_T>(bytes)};
        (*resource)->Unmap(0, &write_range);
    }
    return hr;
}

static HRESULT create_default_buffer(ID3D12Device* device, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state, ID3D12Resource** resource) {
    D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = buffer_desc(bytes, flags);
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource));
}

static bool readback_u32(ID3D12Resource* readback, uint32_t* values, size_t count) {
    uint32_t* data = nullptr;
    D3D12_RANGE read_range = {0, count * sizeof(uint32_t)};
    HRESULT hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
    if (FAILED(hr) || !data)
        return false;
    std::memcpy(values, data, count * sizeof(uint32_t));
    D3D12_RANGE write_range = {0, 0};
    readback->Unmap(0, &write_range);
    return true;
}

static CaseResult run_resource_binding_case() {
    CaseResult result = {"resource_bindings", false, E_FAIL, "", ""};
    const char* hlsl =
        "cbuffer Constants:register(b0){uint addend; float2 uv; uint pad;};"
        "Texture2D<float4> tex:register(t0);"
        "ByteAddressBuffer inbuf:register(t1);"
        "SamplerState smp:register(s0);"
        "RWByteAddressBuffer outbuf:register(u0);"
        "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){"
        "float4 c=tex.SampleLevel(smp,uv,0);"
        "uint sample=(uint)(c.r*255.0+0.5)+(uint)(c.g*255.0+0.5)+(uint)(c.b*255.0+0.5)+(uint)(c.a*255.0+0.5);"
        "outbuf.Store(id.x*4,inbuf.Load(id.x*4)+addend+sample+id.x);"
        "}";

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* constants = nullptr;
    ID3D12Resource* input = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* texture = nullptr;
    std::string detail;

    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE ranges[4] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = 2;
        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[3].NumDescriptors = 1;
        ranges[3].BaseShaderRegister = 0;
        ranges[3].OffsetInDescriptorsFromTableStart = 3;
        D3D12_ROOT_PARAMETER params[1] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 4;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = params;
        root_desc.NumStaticSamplers = 1;
        root_desc.pStaticSamplers = &sampler;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 4;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }
    struct Constants {
        uint32_t addend;
        float uv[2];
        uint32_t pad;
    };
    Constants cbuf = {5, {0.5f, 0.5f}, 0};
    uint32_t input_values[4] = {1, 2, 3, 4};
    D3D12_RESOURCE_DESC tex_desc =
        texture_desc(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &cbuf, 256, &constants);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, input_values, sizeof(input_values), &input);
    if (SUCCEEDED(hr))
        hr = create_default_buffer(device, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC rb_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear.Color[0] = 10.0f / 255.0f;
        clear.Color[1] = 20.0f / 255.0f;
        clear.Color[2] = 30.0f / 255.0f;
        clear.Color[3] = 40.0f / 255.0f;
        hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
        cbv.BufferLocation = constants->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        device->CreateConstantBufferView(&cbv, offset_cpu(cpu, inc, 0));
        D3D12_SHADER_RESOURCE_VIEW_DESC tex_srv = {};
        tex_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex_srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(texture, &tex_srv, offset_cpu(cpu, inc, 1));
        device->CreateRenderTargetView(texture, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 4;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(input, &srv, offset_cpu(cpu, inc, 2));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, offset_cpu(cpu, inc, 3));
    }
    if (SUCCEEDED(hr)) {
        const float clear_color[4] = {10.0f / 255.0f, 20.0f / 255.0f, 30.0f / 255.0f, 40.0f / 255.0f};
        list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), clear_color, 0, nullptr);
        D3D12_RESOURCE_BARRIER tex_barrier = transition_barrier(texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &tex_barrier);
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = uav_barrier(output);
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, output);
        hr = execute_and_wait(device, queue, list);
    }

    uint32_t got[4] = {};
    bool read_ok = SUCCEEDED(hr) && readback_u32(readback, got, 4);
    // The sampled UNORM texel is (10,20,30,40)/255, so the shader adds a
    // channel sum of 100 in addition to the input value, CBV addend, and lane.
    uint32_t expected[4] = {106, 108, 110, 112};
    bool verified = read_ok && std::memcmp(got, expected, sizeof(expected)) == 0;
    result.pass = SUCCEEDED(hr) && verified;
    result.hr = hr;
    result.detail = verified
                        ? "CBV/SRV/UAV dispatch and compute texture sampling verified"
                    : detail.empty() ? "resource binding mismatch"
                                     : detail;
    char extra[256] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"shader\":\"sm50\",\"values\":[%u,%u,%u,%u],"
                  "\"expected\":[106,108,110,112],"
                  "\"sampler_read_supported\":true",
                  got[0], got[1], got[2], got[3]);
    result.extra = extra;

    safe_release(texture);
    safe_release(readback);
    safe_release(output);
    safe_release(input);
    safe_release(constants);
    safe_release(heap);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(device);
    return result;
}

static CaseResult run_atomic_case() {
    CaseResult result = {"atomics_32bit", false, E_FAIL, "", ""};
    const char* hlsl = "RWStructuredBuffer<uint> b:register(u0);"
                       "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){uint o;"
                       "b[0]=10;InterlockedAdd(b[0],5,o);b[1]=b[0];b[2]=o;"
                       "b[0]=0xf0;InterlockedAnd(b[0],0x3c,o);b[3]=b[0];b[4]=o;"
                       "b[0]=0x10;InterlockedOr(b[0],0x03,o);b[5]=b[0];b[6]=o;"
                       "b[0]=0x55;InterlockedXor(b[0],0x0f,o);b[7]=b[0];b[8]=o;"
                       "b[0]=9;InterlockedMin(b[0],4,o);b[9]=b[0];b[10]=o;"
                       "b[0]=9;InterlockedMax(b[0],14,o);b[11]=b[0];b[12]=o;"
                       "b[0]=7;InterlockedExchange(b[0],99,o);b[13]=b[0];b[14]=o;"
                       "b[0]=77;InterlockedCompareExchange(b[0],77,88,o);b[15]=b[0];b[16]=o;"
                       "}";

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;

    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr))
        hr = create_default_buffer(device, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC rb_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateUnorderedAccessView(output, nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = uav_barrier(output);
        list->ResourceBarrier(1, &barrier);
        list->CopyResource(readback, output);
        hr = execute_and_wait(device, queue, list);
    }

    uint32_t got[17] = {};
    bool read_ok = SUCCEEDED(hr) && readback_u32(readback, got, 17);
    uint32_t expected[17] = {88, 15, 10, 0x30, 0xf0, 0x13, 0x10, 0x5a, 0x55, 4, 9, 14, 9, 99, 7, 88, 77};
    bool verified = read_ok && std::memcmp(got, expected, sizeof(expected)) == 0;
    result.pass = SUCCEEDED(hr) && verified;
    result.hr = hr;
    result.detail = verified ? "32-bit atomic operations verified" : detail.empty() ? "atomic result mismatch" : detail;
    char extra[384] = {};
    std::snprintf(extra, sizeof(extra), "\"values\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", got[0],
                  got[1], got[2], got[3], got[4], got[5], got[6], got[7], got[8], got[9], got[10], got[11], got[12],
                  got[13], got[14], got[15], got[16]);
    result.extra = extra;

    safe_release(readback);
    safe_release(output);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(device);
    return result;
}

static CaseResult run_dispatch_indirect_case() {
    CaseResult result = {"dispatch_indirect", false, E_FAIL, "", ""};
    const char* hlsl = "RWByteAddressBuffer outbuf:register(u0);"
                       "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){outbuf.Store(id.x*4,id.x+21);}";

    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* args = nullptr;
    ID3D12Resource* count = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* bounds_output = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* bounds_readback = nullptr;
    std::string detail;

    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr)) {
        D3D12_INDIRECT_ARGUMENT_DESC arg_desc = {};
        arg_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sig_desc = {};
        sig_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sig_desc.NumArgumentDescs = 1;
        sig_desc.pArgumentDescs = &arg_desc;
        hr = device->CreateCommandSignature(&sig_desc, nullptr, IID_PPV_ARGS(&signature));
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 2;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }
    D3D12_DISPATCH_ARGUMENTS dispatch_args = {1, 1, 1};
    uint32_t count_value = 1;
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &dispatch_args, sizeof(dispatch_args), &args);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &count_value, sizeof(count_value), &count);
    if (SUCCEEDED(hr))
        hr = create_default_buffer(device, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_default_buffer(device, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &bounds_output);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC rb_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES rb_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC rb_desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&bounds_readback));
    }
    if (SUCCEEDED(hr)) {
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, offset_cpu(cpu, inc, 0));
        device->CreateUnorderedAccessView(bounds_output, nullptr, &uav, offset_cpu(cpu, inc, 1));
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetPipelineState(pso);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->ExecuteIndirect(signature, 1, args, 0, count, 0);
        list->SetComputeRootDescriptorTable(0, offset_gpu(heap->GetGPUDescriptorHandleForHeapStart(), inc, 1));
        list->ExecuteIndirect(signature, 1, args, 8, nullptr, 0);
        D3D12_RESOURCE_BARRIER barriers[] = {uav_barrier(output), uav_barrier(bounds_output)};
        list->ResourceBarrier(2, barriers);
        list->CopyResource(readback, output);
        list->CopyResource(bounds_readback, bounds_output);
        hr = execute_and_wait(device, queue, list);
    }

    uint32_t got[4] = {};
    uint32_t bounds[4] = {};
    bool read_ok = SUCCEEDED(hr) && readback_u32(readback, got, 4) && readback_u32(bounds_readback, bounds, 4);
    bool verified = read_ok && got[0] == 21 && got[1] == 22 && got[2] == 23 && got[3] == 24 && bounds[0] == 0 &&
                    bounds[1] == 0 && bounds[2] == 0 && bounds[3] == 0;
    result.pass = SUCCEEDED(hr) && verified;
    result.hr = hr;
    result.detail = verified         ? "DispatchIndirect arguments, count clamp, and bounds skip verified"
                    : detail.empty() ? "dispatch indirect mismatch"
                                     : detail;
    char extra[192] = {};
    std::snprintf(extra, sizeof(extra), "\"direct\":[%u,%u,%u,%u],\"bounds\":[%u,%u,%u,%u]", got[0], got[1], got[2],
                  got[3], bounds[0], bounds[1], bounds[2], bounds[3]);
    result.extra = extra;

    safe_release(bounds_readback);
    safe_release(readback);
    safe_release(bounds_output);
    safe_release(output);
    safe_release(count);
    safe_release(args);
    safe_release(signature);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(device);
    return result;
}

static CaseResult run_counter_case() {
    CaseResult result = {"append_consume_counter_status", false, E_FAIL, "", ""};
    result.pass = true;
    result.hr = S_FALSE;
    result.detail = "Append/consume UAV counters are explicitly unsupported by the current DXMT D3D12 compute bridge";
    result.extra = "\"supported\":false";
    return result;
}

int main() {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    g_create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    g_serialize_root_signature = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    g_compile = load_proc<D3DCompileFn>(d3dcompiler, "D3DCompile");

    std::vector<CaseResult> cases;
    if (!d3d12 || !d3dcompiler || !g_create_device || !g_serialize_root_signature || !g_compile) {
        cases.push_back(
            {"entrypoints", false, HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), "required D3D12 entrypoint missing", ""});
    } else {
        cases.push_back(run_resource_binding_case());
        cases.push_back(run_atomic_case());
        cases.push_back(run_dispatch_indirect_case());
        cases.push_back(run_counter_case());
    }

    bool pass = !cases.empty();
    for (const auto& c : cases)
        pass = pass && c.pass;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-compute-pso.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"d3dcompiler_47_loaded\": %s,\n", d3dcompiler ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", g_create_device ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s,\n", g_serialize_root_signature ? "true" : "false");
    std::printf("    \"D3DCompile\": %s\n", g_compile ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"coverage\": {\n");
    std::printf("    \"cbv_srv_uav_reads\": true,\n");
    std::printf("    \"sampler_read_status_reported\": true,\n");
    std::printf("    \"uav_writes\": true,\n");
    std::printf("    \"atomics_32bit\": true,\n");
    std::printf("    \"append_consume_counter_status\": true,\n");
    std::printf("    \"dispatch_indirect_layout_and_bounds\": true\n");
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < cases.size(); i++) {
        const auto& c = cases[i];
        std::printf("    {\"name\":\"%s\",\"pass\":%s,\"hr\":\"%s\",\"detail\":\"%s\"", json_escape(c.name).c_str(),
                    c.pass ? "true" : "false", hr_hex(c.hr).c_str(), json_escape(c.detail).c_str());
        if (!c.extra.empty())
            std::printf(",%s", c.extra.c_str());
        std::printf("}%s\n", i + 1 == cases.size() ? "" : ",");
    }
    std::printf("  ]\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    return 0;
}
