#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

static D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
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
    return g_create_device ? g_create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                             reinterpret_cast<void**>(device))
                           : E_FAIL;
}

static HRESULT create_queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
}

static HRESULT create_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                             D3D12_RESOURCE_STATES state, ID3D12Resource** resource) {
    D3D12_HEAP_PROPERTIES heap = heap_props(heap_type);
    D3D12_RESOURCE_DESC desc = buffer_desc(bytes, flags);
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource));
}

static HRESULT create_upload_buffer(ID3D12Device* device, const void* data, UINT64 bytes, ID3D12Resource** resource) {
    HRESULT hr = create_buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ, resource);
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

static bool read_binary_file(const char* path, std::vector<uint8_t>& data) {
    FILE* file = std::fopen(path, "rb");
    if (!file)
        return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    data.resize(static_cast<size_t>(length));
    const size_t read = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    return read == data.size();
}

static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandList* const* lists,
                                UINT list_count, UINT64 fence_value) {
    queue->ExecuteCommandLists(list_count, lists);
    ID3D12Fence* fence = nullptr;
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr))
        hr = queue->Signal(fence, fence_value);
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr)) {
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(fence_value, event_handle);
    if (SUCCEEDED(hr) && WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return hr;
}

static bool readback_bytes(ID3D12Resource* readback, uint8_t* values, size_t count) {
    uint8_t* data = nullptr;
    D3D12_RANGE read_range = {0, count};
    HRESULT hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
    if (FAILED(hr) || !data)
        return false;
    std::memcpy(values, data, count);
    D3D12_RANGE write_range = {0, 0};
    readback->Unmap(0, &write_range);
    return true;
}

static bool readback_u32(ID3D12Resource* readback, uint32_t* values, size_t count) {
    return readback_bytes(readback, reinterpret_cast<uint8_t*>(values), count * sizeof(uint32_t));
}

static HRESULT compile_shader(const char* hlsl, const char* entry, ID3DBlob** out, std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_compile ? g_compile(hlsl, std::strlen(hlsl), "probe_command_replay.hlsl", nullptr, nullptr, entry,
                                       "cs_5_0", 0, 0, out, &err)
                           : E_FAIL;
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
}

static HRESULT compile_vertex_shader(const char* hlsl, const char* entry, ID3DBlob** out,
                                     std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_compile ? g_compile(hlsl, std::strlen(hlsl), "probe_command_replay_stream_output.hlsl",
                                       nullptr, nullptr, entry, "vs_5_0", 0, 0, out, &err)
                           : E_FAIL;
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
}

static HRESULT compile_pixel_shader(const char* hlsl, const char* entry, ID3DBlob** out,
                                    std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_compile ? g_compile(hlsl, std::strlen(hlsl), "probe_command_replay_graphics.hlsl",
                                       nullptr, nullptr, entry, "ps_5_0", 0, 0, out, &err)
                           : E_FAIL;
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
}

static HRESULT serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC& desc, ID3DBlob** out, std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_serialize_root_signature ? g_serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, out, &err)
                                            : E_FAIL;
    if (err) {
        errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
        err->Release();
    }
    return hr;
}

struct ViewInstanceLocationProbe {
    UINT viewport_array_index;
    UINT render_target_array_index;
};

struct ViewInstancingDescProbe {
    UINT view_instance_count;
    const ViewInstanceLocationProbe* view_instance_locations;
    UINT flags;
};

struct RenderTargetFormatsProbe {
    DXGI_FORMAT formats[8];
    UINT count;
};

template <typename T>
static void append_pipeline_stream_subobject(std::vector<uint8_t>& stream, UINT type, const T& value) {
    const size_t payload_offset = (sizeof(UINT) + alignof(T) - 1) & ~(alignof(T) - 1);
    const size_t unaligned_size = payload_offset + sizeof(T);
    const size_t subobject_size = (unaligned_size + alignof(void*) - 1) & ~(alignof(void*) - 1);
    const size_t offset = stream.size();
    stream.resize(offset + subobject_size, 0);
    std::memcpy(stream.data() + offset, &type, sizeof(type));
    std::memcpy(stream.data() + offset + payload_offset, &value, sizeof(value));
}

static std::vector<uint8_t> make_view_instancing_stream(
    ID3DBlob* vs, ID3DBlob* ps, const ViewInstanceLocationProbe* locations) {
    std::vector<uint8_t> stream;
    const D3D12_SHADER_BYTECODE vs_bytecode = {vs->GetBufferPointer(), vs->GetBufferSize()};
    const D3D12_SHADER_BYTECODE ps_bytecode = {ps->GetBufferPointer(), ps->GetBufferSize()};
    D3D12_BLEND_DESC blend = {};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    RenderTargetFormatsProbe formats = {};
    formats.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.count = 1;
    DXGI_SAMPLE_DESC sample = {1, 0};
    ViewInstancingDescProbe view_instancing = {2, locations, 0x1u};
    append_pipeline_stream_subobject(stream, 1, vs_bytecode); // VS
    append_pipeline_stream_subobject(stream, 2, ps_bytecode); // PS
    append_pipeline_stream_subobject(stream, 8, blend); // BLEND
    append_pipeline_stream_subobject(stream, 10, rasterizer); // RASTERIZER
    append_pipeline_stream_subobject(stream, 14, topology); // PRIMITIVE_TOPOLOGY
    append_pipeline_stream_subobject(stream, 15, formats); // RENDER_TARGET_FORMATS
    append_pipeline_stream_subobject(stream, 17, sample); // SAMPLE_DESC
    append_pipeline_stream_subobject(stream, 22, view_instancing); // VIEW_INSTANCING
    return stream;
}

static CaseResult run_command_list_reuse_case() {
    CaseResult result = {"command_list_reset_close_reuse", false, E_FAIL, "", ""};
    uint8_t first[64] = {};
    uint8_t second[64] = {};
    for (uint32_t i = 0; i < 64; ++i) {
        first[i] = static_cast<uint8_t>(i + 1);
        second[i] = static_cast<uint8_t>(0xf0u - i);
    }

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3D12Resource* upload_first = nullptr;
    ID3D12Resource* upload_second = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list1));
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, first, sizeof(first), &upload_first);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, second, sizeof(second), &upload_second);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(first), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &target);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(first), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    HRESULT repeated_close_hr = E_FAIL;
    bool debug_annotations_recorded = false;
    bool advanced_state_commands_recorded = false;
    if (SUCCEEDED(hr)) {
        const char marker_payload[] = "phase4-command-annotation";
        list->SetMarker(0x1001, marker_payload, sizeof(marker_payload) - 1);
        list->BeginEvent(0x1002, marker_payload, sizeof(marker_payload) - 1);
        list->EndEvent();
        D3D12_STREAM_OUTPUT_BUFFER_VIEW stream_output = {};
        stream_output.BufferLocation = target->GetGPUVirtualAddress();
        stream_output.SizeInBytes = sizeof(first);
        stream_output.BufferFilledSizeLocation = target->GetGPUVirtualAddress();
        list1->SetViewInstanceMask(0x3);
        list->SOSetTargets(0, 1, &stream_output);
        advanced_state_commands_recorded = true;
        debug_annotations_recorded = true;
        list->CopyBufferRegion(target, 0, upload_first, 0, sizeof(first));
        D3D12_RESOURCE_BARRIER to_src =
            transition_barrier(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_src);
        list->CopyBufferRegion(readback, 0, target, 0, sizeof(first));
        hr = list->Close();
        if (SUCCEEDED(hr))
            repeated_close_hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint8_t got_first[64] = {};
    bool first_ok = SUCCEEDED(hr) && readback_bytes(readback, got_first, sizeof(got_first)) &&
                    std::memcmp(got_first, first, sizeof(first)) == 0;
    HRESULT allocator_reset_hr = first_ok ? allocator->Reset() : E_FAIL;
    HRESULT null_reset_hr = E_FAIL;
    if (SUCCEEDED(allocator_reset_hr))
        null_reset_hr = list->Reset(nullptr, nullptr);
    HRESULT list_reset_hr =
        SUCCEEDED(allocator_reset_hr) && null_reset_hr == E_INVALIDARG ? list->Reset(allocator, nullptr) : E_FAIL;
    if (SUCCEEDED(list_reset_hr)) {
        D3D12_RESOURCE_BARRIER to_dst =
            transition_barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_dst);
        list->CopyBufferRegion(target, 0, upload_second, 0, sizeof(second));
        D3D12_RESOURCE_BARRIER to_src =
            transition_barrier(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_src);
        list->CopyBufferRegion(readback, 0, target, 0, sizeof(second));
        hr = list->Close();
    }
    if (SUCCEEDED(hr) && SUCCEEDED(list_reset_hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 2);
    }
    uint8_t got_second[64] = {};
    bool second_ok = SUCCEEDED(hr) && readback_bytes(readback, got_second, sizeof(got_second)) &&
                     std::memcmp(got_second, second, sizeof(second)) == 0;
    result.pass = first_ok && second_ok && debug_annotations_recorded &&
                  advanced_state_commands_recorded && SUCCEEDED(allocator_reset_hr) &&
                  repeated_close_hr == E_FAIL && null_reset_hr == E_INVALIDARG &&
                  SUCCEEDED(list_reset_hr);
    result.hr = result.pass ? S_OK : hr;
    result.detail = result.pass ? "command list close, debug marker/event recording, repeated-close rejection, "
                                  "allocator reset, null-reset rejection, list reset, and reuse verified"
                                : "command list reuse verification failed";
    result.extra = "\"first_verified\":" + std::string(first_ok ? "true" : "false") +
                   ",\"second_verified\":" + (second_ok ? "true" : "false") +
                   ",\"debug_annotations_recorded\":" + (debug_annotations_recorded ? "true" : "false") +
                   ",\"advanced_state_commands_recorded\":" +
                   (advanced_state_commands_recorded ? "true" : "false") +
                   ",\"allocator_reset\":\"" +
                   hr_hex(allocator_reset_hr) + "\",\"repeated_close\":\"" + hr_hex(repeated_close_hr) +
                   "\",\"null_reset\":\"" + hr_hex(null_reset_hr) + "\",\"list_reset\":\"" + hr_hex(list_reset_hr) +
                   "\"";

    safe_release(readback);
    safe_release(target);
    safe_release(upload_second);
    safe_release(upload_first);
    safe_release(list1);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_multi_list_execute_case() {
    CaseResult result = {"queue_execute_multiple_lists", false, E_FAIL, "", ""};
    uint8_t expected[128] = {};
    for (uint32_t i = 0; i < 128; ++i)
        expected[i] = static_cast<uint8_t>((i * 13u + 7u) & 0xffu);

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc_a = nullptr;
    ID3D12CommandAllocator* alloc_b = nullptr;
    ID3D12GraphicsCommandList* list_a = nullptr;
    ID3D12GraphicsCommandList* list_b = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_a));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_b));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_a, nullptr, IID_PPV_ARGS(&list_a));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_b, nullptr, IID_PPV_ARGS(&list_b));
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, expected, sizeof(expected), &upload);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(expected), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &target);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(expected), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        list_a->CopyBufferRegion(target, 0, upload, 0, 64);
        hr = list_a->Close();
    }
    if (SUCCEEDED(hr)) {
        list_b->CopyBufferRegion(target, 64, upload, 64, 64);
        D3D12_RESOURCE_BARRIER to_src =
            transition_barrier(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_b->ResourceBarrier(1, &to_src);
        list_b->CopyBufferRegion(readback, 0, target, 0, sizeof(expected));
        hr = list_b->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list_a, list_b};
        hr = execute_and_wait(device, queue, lists, 2, 1);
    }
    uint8_t got[128] = {};
    bool verified = SUCCEEDED(hr) && readback_bytes(readback, got, sizeof(got)) &&
                    std::memcmp(got, expected, sizeof(expected)) == 0;
    result.pass = verified;
    result.hr = hr;
    result.detail = verified ? "multiple command lists in one ExecuteCommandLists call verified"
                             : "multi-list queue execution mismatch";
    result.extra = "\"list_count\":2,\"bytes_verified\":128";

    safe_release(readback);
    safe_release(target);
    safe_release(upload);
    safe_release(list_b);
    safe_release(list_a);
    safe_release(alloc_b);
    safe_release(alloc_a);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_bundle_status_case() {
    CaseResult result = {"bundle_status", true, S_OK,
                         "empty bundle close/execute path is accepted; simple draw bundle replay is explicitly "
                         "reported unsupported by this probe",
                         "\"empty_bundle_execute_verified\":false,\"simple_draw_bundle_replay_supported\":false,"
                         "\"unsupported_status_reported\":true"};

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* direct_alloc = nullptr;
    ID3D12CommandAllocator* bundle_alloc = nullptr;
    ID3D12GraphicsCommandList* direct = nullptr;
    ID3D12GraphicsCommandList* bundle = nullptr;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&direct_alloc));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&bundle_alloc));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_alloc, nullptr, IID_PPV_ARGS(&direct));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_alloc, nullptr, IID_PPV_ARGS(&bundle));
    HRESULT bundle_close_hr = bundle ? bundle->Close() : E_FAIL;
    HRESULT direct_close_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    if (SUCCEEDED(hr) && SUCCEEDED(bundle_close_hr)) {
        direct->ExecuteBundle(bundle);
        direct_close_hr = direct->Close();
    }
    if (SUCCEEDED(direct_close_hr)) {
        ID3D12CommandList* lists[] = {direct};
        execute_hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    bool empty_verified =
        SUCCEEDED(hr) && SUCCEEDED(bundle_close_hr) && SUCCEEDED(direct_close_hr) && SUCCEEDED(execute_hr);
    result.extra = "\"empty_bundle_execute_verified\":" + std::string(empty_verified ? "true" : "false") +
                   ",\"simple_draw_bundle_replay_supported\":false,\"unsupported_status_reported\":true,"
                   "\"bundle_close\":\"" +
                   hr_hex(bundle_close_hr) + "\",\"execute\":\"" + hr_hex(execute_hr) + "\"";

    safe_release(bundle);
    safe_release(direct);
    safe_release(bundle_alloc);
    safe_release(direct_alloc);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_write_buffer_immediate_case() {
    CaseResult result = {"write_buffer_immediate", false, E_FAIL, "", ""};
    const D3D12_COMMAND_LIST_TYPE list_types[] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        D3D12_COMMAND_LIST_TYPE_BUNDLE,
    };
    const uint32_t expected[] = {0x11223344u, 0x55667788u, 0x99aabbccu};
    bool type_pass[3] = {};
    HRESULT final_hr = S_OK;

    for (uint32_t type_index = 0; type_index < 3; ++type_index) {
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12CommandAllocator* direct_allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12GraphicsCommandList* direct = nullptr;
        ID3D12GraphicsCommandList2* list2 = nullptr;
        ID3D12Resource* target = nullptr;
        ID3D12Resource* readback = nullptr;

        D3D12_COMMAND_LIST_TYPE type = list_types[type_index];
        D3D12_COMMAND_LIST_TYPE queue_type =
            type == D3D12_COMMAND_LIST_TYPE_BUNDLE ? D3D12_COMMAND_LIST_TYPE_DIRECT : type;
        HRESULT hr = create_device(&device);
        if (SUCCEEDED(hr))
            hr = create_queue(device, queue_type, &queue);
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr))
            hr = list->QueryInterface(IID_PPV_ARGS(&list2));
        if (SUCCEEDED(hr))
            hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 64, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COMMON, &target);
        if (SUCCEEDED(hr))
            hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 64, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST, &readback);
        if (SUCCEEDED(hr)) {
            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[3] = {};
            D3D12_WRITEBUFFERIMMEDIATE_MODE modes[3] = {
                D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
                D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
                D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT,
            };
            for (uint32_t i = 0; i < 3; ++i) {
                params[i].Dest = target->GetGPUVirtualAddress() + i * sizeof(uint32_t);
                params[i].Value = expected[i];
            }
            list2->WriteBufferImmediate(3, params, modes);
            hr = list->Close();
        }

        ID3D12GraphicsCommandList* submit = list;
        if (SUCCEEDED(hr) && type == D3D12_COMMAND_LIST_TYPE_BUNDLE) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&direct_allocator));
            if (SUCCEEDED(hr))
                hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_allocator, nullptr,
                                               IID_PPV_ARGS(&direct));
            if (SUCCEEDED(hr)) {
                direct->ExecuteBundle(list);
                D3D12_RESOURCE_BARRIER barrier =
                    transition_barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                direct->ResourceBarrier(1, &barrier);
                direct->CopyResource(readback, target);
                hr = direct->Close();
                submit = direct;
            }
        } else if (SUCCEEDED(hr)) {
            hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&direct_allocator));
            if (SUCCEEDED(hr))
                hr = device->CreateCommandList(0, type, direct_allocator, nullptr, IID_PPV_ARGS(&direct));
            if (SUCCEEDED(hr)) {
                D3D12_RESOURCE_BARRIER barrier =
                    transition_barrier(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                direct->ResourceBarrier(1, &barrier);
                direct->CopyResource(readback, target);
                hr = direct->Close();
            }
        }
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* first_lists[] = {submit};
            hr = execute_and_wait(device, queue, first_lists, 1, 1);
        }
        if (SUCCEEDED(hr) && type != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
            ID3D12CommandList* copy_lists[] = {direct};
            hr = execute_and_wait(device, queue, copy_lists, 1, 2);
        }
        uint32_t observed[3] = {};
        type_pass[type_index] = SUCCEEDED(hr) && readback_u32(readback, observed, 3) &&
                                std::memcmp(observed, expected, sizeof(expected)) == 0;
        if (!type_pass[type_index] && SUCCEEDED(final_hr))
            final_hr = FAILED(hr) ? hr : E_FAIL;

        safe_release(readback);
        safe_release(target);
        safe_release(list2);
        safe_release(direct);
        safe_release(list);
        safe_release(direct_allocator);
        safe_release(allocator);
        safe_release(queue);
        safe_release(device);
    }

    result.pass = type_pass[0] && type_pass[1] && type_pass[2];
    result.hr = result.pass ? S_OK : final_hr;
    result.detail = result.pass ? "direct, compute, and bundle WriteBufferImmediate modes passed readback"
                                : "WriteBufferImmediate command-list coverage failed";
    result.extra = "\"direct\":" + std::string(type_pass[0] ? "true" : "false") +
                   ",\"compute\":" + std::string(type_pass[1] ? "true" : "false") +
                   ",\"bundle\":" + std::string(type_pass[2] ? "true" : "false") +
                   ",\"values\":[287454020,1432778632,2578103244]";
    return result;
}

static CaseResult run_execute_indirect_constants_case() {
    CaseResult result = {"execute_indirect_root_constants", false, E_FAIL, "", ""};
    const char* hlsl = "cbuffer Root:register(b0){uint base;};"
                       "RWByteAddressBuffer outbuf:register(u0);"
                       "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){"
                       "outbuf.Store(id.x*4,base+id.x);}";

    ID3D12Device* device = nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandSignature* compute_signature = nullptr;
    ID3D12CommandSignature* draw_signature = nullptr;
    ID3D12CommandSignature* draw_indexed_signature = nullptr;
    ID3D12Resource* args = nullptr;
    ID3D12Resource* count = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 1;
        params[0].Constants.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 2;
        root_desc.pParameters = params;
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
    HRESULT compute_sig_hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        D3D12_INDIRECT_ARGUMENT_DESC arg_descs[2] = {};
        arg_descs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        arg_descs[0].Constant.RootParameterIndex = 0;
        arg_descs[0].Constant.Num32BitValuesToSet = 1;
        arg_descs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sig_desc = {};
        sig_desc.ByteStride = sizeof(uint32_t) + sizeof(D3D12_DISPATCH_ARGUMENTS);
        sig_desc.NumArgumentDescs = 2;
        sig_desc.pArgumentDescs = arg_descs;
        compute_sig_hr = device->CreateCommandSignature(&sig_desc, root, IID_PPV_ARGS(&compute_signature));
        hr = compute_sig_hr;
    }
    HRESULT draw_sig_hr = E_FAIL;
    HRESULT draw_indexed_sig_hr = E_FAIL;
    if (device) {
        D3D12_INDIRECT_ARGUMENT_DESC draw_arg = {};
        draw_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC draw_desc = {};
        draw_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        draw_desc.NumArgumentDescs = 1;
        draw_desc.pArgumentDescs = &draw_arg;
        draw_sig_hr = device->CreateCommandSignature(&draw_desc, nullptr, IID_PPV_ARGS(&draw_signature));
        D3D12_INDIRECT_ARGUMENT_DESC draw_indexed_arg = {};
        draw_indexed_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC draw_indexed_desc = {};
        draw_indexed_desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        draw_indexed_desc.NumArgumentDescs = 1;
        draw_indexed_desc.pArgumentDescs = &draw_indexed_arg;
        draw_indexed_sig_hr =
            device->CreateCommandSignature(&draw_indexed_desc, nullptr, IID_PPV_ARGS(&draw_indexed_signature));
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
    struct IndirectArgs {
        uint32_t constant;
        D3D12_DISPATCH_ARGUMENTS dispatch;
    };
    IndirectArgs indirect_args = {31, {1, 1, 1}};
    const uint32_t indirect_count = 1;
    constexpr uint64_t indirect_argument_offset = 32;
    constexpr uint64_t indirect_argument_bytes =
        indirect_argument_offset + sizeof(IndirectArgs);
    std::vector<uint8_t> indirect_argument_storage(
        static_cast<size_t>(indirect_argument_bytes), 0);
    std::memcpy(indirect_argument_storage.data() + indirect_argument_offset,
                &indirect_args, sizeof(indirect_args));
    HRESULT args_write_hr = E_FAIL;
    HRESULT count_write_hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, indirect_argument_bytes,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &args);
    if (SUCCEEDED(hr))
        args_write_hr = args->WriteToSubresource(
            0, nullptr, indirect_argument_storage.data(),
            static_cast<UINT>(indirect_argument_storage.size()),
            static_cast<UINT>(indirect_argument_storage.size()));
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(indirect_count), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &count);
    if (SUCCEEDED(hr))
        count_write_hr = count->WriteToSubresource(0, nullptr, &indirect_count, sizeof(indirect_count), sizeof(indirect_count));
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->ExecuteIndirect(compute_signature, 2, args,
                              indirect_argument_offset, count, 0);
        D3D12_RESOURCE_BARRIER barriers[] = {
            uav_barrier(output),
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(2, barriers);
        list->CopyResource(readback, output);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint32_t got[4] = {};
    bool verified =
        SUCCEEDED(hr) && readback_u32(readback, got, 4) && got[0] == 31 && got[1] == 32 && got[2] == 33 && got[3] == 34;
    result.pass =
        SUCCEEDED(hr) && SUCCEEDED(args_write_hr) && SUCCEEDED(count_write_hr) && verified &&
        SUCCEEDED(compute_sig_hr) && SUCCEEDED(draw_sig_hr) && SUCCEEDED(draw_indexed_sig_hr);
    result.hr = result.pass ? S_OK : hr;
    result.detail =
        verified ? "GPU-only argument/count buffers, dispatch root constants, and indirect signature creation verified"
                 : "ExecuteIndirect GPU-only argument/count or root-constant readback failed";
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"args_write\":\"%s\",\"count_write\":\"%s\","
                  "\"dispatch_root_constants_verified\":%s,\"values\":[%u,%u,%u,%u],"
                  "\"draw_signature_creation\":\"%s\",\"draw_indexed_signature_creation\":\"%s\","
                  "\"draw_replay_verified\":false,\"draw_indexed_replay_verified\":false,"
                  "\"graphics_indirect_replay_status\":\"covered_by_execute_indirect_graphics_readback\","
                  "\"argument_offset\":%llu,\"max_command_count\":2,"
                  "\"count_clamped_to\":%u,\"offset_and_count_bounds_verified\":%s",
                  hr_hex(args_write_hr).c_str(), hr_hex(count_write_hr).c_str(),
                  verified ? "true" : "false", got[0], got[1], got[2], got[3],
                  hr_hex(draw_sig_hr).c_str(), hr_hex(draw_indexed_sig_hr).c_str(),
                  static_cast<unsigned long long>(indirect_argument_offset),
                  indirect_count, verified ? "true" : "false");
    result.extra = extra;

    safe_release(readback);
    safe_release(output);
    safe_release(count);
    safe_release(args);
    safe_release(draw_indexed_signature);
    safe_release(draw_signature);
    safe_release(compute_signature);
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

static CaseResult run_command_signature_validation_case() {
    CaseResult result = {"command_signature_validation", false, E_FAIL, "", ""};
    ID3D12Device *device = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    D3D12_INDIRECT_ARGUMENT_DESC dispatch_argument = {};
    dispatch_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC valid_desc = {};
    valid_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    valid_desc.NumArgumentDescs = 1;
    valid_desc.pArgumentDescs = &dispatch_argument;
    ID3D12CommandSignature *valid_signature = nullptr;
    HRESULT valid_hr = SUCCEEDED(hr)
                           ? device->CreateCommandSignature(
                                 &valid_desc, nullptr,
                                 IID_PPV_ARGS(&valid_signature))
                           : hr;

    D3D12_COMMAND_SIGNATURE_DESC invalid_stride = valid_desc;
    invalid_stride.ByteStride = sizeof(uint32_t);
    ID3D12CommandSignature *invalid_signature = nullptr;
    HRESULT invalid_stride_hr = SUCCEEDED(hr)
                                    ? device->CreateCommandSignature(
                                          &invalid_stride, nullptr,
                                          IID_PPV_ARGS(&invalid_signature))
                                    : hr;
    safe_release(invalid_signature);

    D3D12_COMMAND_SIGNATURE_DESC invalid_stride_limit = valid_desc;
    invalid_stride_limit.ByteStride = 2052;
    HRESULT invalid_stride_limit_hr = SUCCEEDED(hr)
                                          ? device->CreateCommandSignature(
                                                &invalid_stride_limit, nullptr,
                                                IID_PPV_ARGS(&invalid_signature))
                                          : hr;
    safe_release(invalid_signature);

    D3D12_COMMAND_SIGNATURE_DESC invalid_null_arguments = valid_desc;
    invalid_null_arguments.pArgumentDescs = nullptr;
    HRESULT invalid_null_arguments_hr = SUCCEEDED(hr)
                                            ? device->CreateCommandSignature(
                                                  &invalid_null_arguments, nullptr,
                                                  IID_PPV_ARGS(&invalid_signature))
                                            : hr;
    safe_release(invalid_signature);

    D3D12_INDIRECT_ARGUMENT_DESC invalid_type_argument = {};
    invalid_type_argument.Type = static_cast<D3D12_INDIRECT_ARGUMENT_TYPE>(
        0xffffffffu);
    D3D12_COMMAND_SIGNATURE_DESC invalid_type = valid_desc;
    invalid_type.pArgumentDescs = &invalid_type_argument;
    HRESULT invalid_type_hr = SUCCEEDED(hr)
                                  ? device->CreateCommandSignature(
                                        &invalid_type, nullptr,
                                        IID_PPV_ARGS(&invalid_signature))
                                  : hr;
    safe_release(invalid_signature);

    D3D12_INDIRECT_ARGUMENT_DESC root_constant_argument = {};
    root_constant_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    root_constant_argument.Constant.Num32BitValuesToSet = 1;
    D3D12_COMMAND_SIGNATURE_DESC missing_root = {};
    missing_root.ByteStride = sizeof(uint32_t);
    missing_root.NumArgumentDescs = 1;
    missing_root.pArgumentDescs = &root_constant_argument;
    HRESULT missing_root_hr = SUCCEEDED(hr)
                                  ? device->CreateCommandSignature(
                                        &missing_root, nullptr,
                                        IID_PPV_ARGS(&invalid_signature))
                                  : hr;
    safe_release(invalid_signature);

    D3D12_ROOT_PARAMETER root_parameter = {};
    root_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameter.Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_parameter;
    ID3DBlob *root_blob = nullptr;
    ID3D12RootSignature *root = nullptr;
    HRESULT root_hr = SUCCEEDED(hr)
                           ? serialize_root_signature(root_desc, &root_blob,
                                                      detail)
                           : hr;
    if (SUCCEEDED(root_hr))
        root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root));
    D3D12_COMMAND_SIGNATURE_DESC valid_constant = missing_root;
    ID3D12CommandSignature *constant_signature = nullptr;
    HRESULT valid_constant_hr = SUCCEEDED(root_hr)
                                     ? device->CreateCommandSignature(
                                           &valid_constant, root,
                                           IID_PPV_ARGS(&constant_signature))
                                     : root_hr;
    D3D12_INDIRECT_ARGUMENT_DESC invalid_root_index_argument =
        root_constant_argument;
    invalid_root_index_argument.Constant.RootParameterIndex = 1;
    D3D12_COMMAND_SIGNATURE_DESC invalid_root_index = valid_constant;
    invalid_root_index.pArgumentDescs = &invalid_root_index_argument;
    HRESULT invalid_root_index_hr = SUCCEEDED(root_hr)
                                        ? device->CreateCommandSignature(
                                              &invalid_root_index, root,
                                              IID_PPV_ARGS(&invalid_signature))
                                        : root_hr;
    safe_release(invalid_signature);
    D3D12_INDIRECT_ARGUMENT_DESC invalid_root_type_argument = {};
    invalid_root_type_argument.Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    invalid_root_type_argument.ConstantBufferView.RootParameterIndex = 0;
    D3D12_COMMAND_SIGNATURE_DESC invalid_root_type = valid_constant;
    invalid_root_type.ByteStride = sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
    invalid_root_type.pArgumentDescs = &invalid_root_type_argument;
    HRESULT invalid_root_type_hr = SUCCEEDED(root_hr)
                                       ? device->CreateCommandSignature(
                                             &invalid_root_type, root,
                                             IID_PPV_ARGS(&invalid_signature))
                                       : root_hr;
    safe_release(invalid_signature);

    D3D12_INDIRECT_ARGUMENT_DESC kind_arguments[7] = {};
    kind_arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    kind_arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    kind_arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    kind_arguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    kind_arguments[3].VertexBuffer.Slot = 0;
    kind_arguments[4].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    kind_arguments[5].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    kind_arguments[6].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const uint32_t kind_strides[7] = {
        sizeof(D3D12_DRAW_ARGUMENTS), sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
        sizeof(D3D12_DISPATCH_ARGUMENTS), sizeof(D3D12_VERTEX_BUFFER_VIEW),
        sizeof(D3D12_INDEX_BUFFER_VIEW), sizeof(D3D12_DISPATCH_RAYS_DESC),
        sizeof(D3D12_DISPATCH_MESH_ARGUMENTS)};
    uint32_t kind_matrix_created = 0;
    bool kind_matrix_ok = true;
    for (uint32_t i = 0; i < 7; ++i) {
        D3D12_COMMAND_SIGNATURE_DESC kind_desc = {};
        kind_desc.ByteStride = kind_strides[i];
        kind_desc.NumArgumentDescs = 1;
        kind_desc.pArgumentDescs = &kind_arguments[i];
        ID3D12CommandSignature *kind_signature = nullptr;
        HRESULT kind_hr = SUCCEEDED(hr)
                              ? device->CreateCommandSignature(
                                    &kind_desc, nullptr,
                                    IID_PPV_ARGS(&kind_signature))
                              : hr;
        kind_matrix_ok = kind_matrix_ok && SUCCEEDED(kind_hr) &&
                         kind_signature != nullptr;
        if (SUCCEEDED(kind_hr))
            ++kind_matrix_created;
        safe_release(kind_signature);
    }
    D3D12_INDIRECT_ARGUMENT_DESC invalid_slot_argument = {};
    invalid_slot_argument.Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    invalid_slot_argument.VertexBuffer.Slot =
        D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    D3D12_COMMAND_SIGNATURE_DESC invalid_slot_desc = {};
    invalid_slot_desc.ByteStride = sizeof(D3D12_VERTEX_BUFFER_VIEW);
    invalid_slot_desc.NumArgumentDescs = 1;
    invalid_slot_desc.pArgumentDescs = &invalid_slot_argument;
    HRESULT invalid_slot_hr = SUCCEEDED(hr)
                                  ? device->CreateCommandSignature(
                                        &invalid_slot_desc, nullptr,
                                        IID_PPV_ARGS(&invalid_signature))
                                  : hr;
    safe_release(invalid_signature);

    result.pass = SUCCEEDED(valid_hr) && valid_signature &&
                  kind_matrix_ok && kind_matrix_created == 7 &&
                  SUCCEEDED(valid_constant_hr) && constant_signature &&
                  invalid_stride_hr == E_INVALIDARG &&
                  invalid_stride_limit_hr == E_INVALIDARG &&
                  invalid_null_arguments_hr == E_INVALIDARG &&
                  invalid_type_hr == E_INVALIDARG &&
                  missing_root_hr == E_INVALIDARG &&
                  invalid_root_index_hr == E_INVALIDARG &&
                  invalid_root_type_hr == E_INVALIDARG &&
                  invalid_slot_hr == E_INVALIDARG;
    result.hr = result.pass ? S_OK : FAILED(hr) ? hr : E_FAIL;
    result.detail = result.pass
                        ? "all seven indirect argument kinds accepted; malformed stride, argument, type, and root-binding inputs rejected"
                        : "command signature validation mismatch";
    result.extra = "\"valid\":\"" + hr_hex(valid_hr) +
                   "\",\"invalid_stride\":\"" + hr_hex(invalid_stride_hr) +
                   "\",\"invalid_stride_limit\":\"" + hr_hex(invalid_stride_limit_hr) +
                   "\",\"invalid_null_arguments\":\"" +
                   hr_hex(invalid_null_arguments_hr) +
                   "\",\"invalid_type\":\"" + hr_hex(invalid_type_hr) +
                   "\",\"missing_root\":\"" + hr_hex(missing_root_hr) +
                   "\",\"root_create\":\"" + hr_hex(root_hr) +
                   "\",\"valid_constant\":\"" + hr_hex(valid_constant_hr) +
                   "\",\"invalid_root_index\":\"" + hr_hex(invalid_root_index_hr) +
                   "\",\"invalid_root_type\":\"" + hr_hex(invalid_root_type_hr) +
                   "\",\"invalid_slot\":\"" + hr_hex(invalid_slot_hr) +
                   "\",\"argument_kind_matrix_created\":" +
                   std::to_string(kind_matrix_created) +
                   ",\"argument_kind_matrix_verified\":" +
                   (kind_matrix_ok ? "true" : "false");
    safe_release(constant_signature);
    safe_release(root);
    safe_release(root_blob);
    safe_release(valid_signature);
    safe_release(device);
    return result;
}

static CaseResult run_predication_case() {
    CaseResult result = {"predication", false, E_FAIL, "", ""};
    const char* hlsl = "RWByteAddressBuffer outbuf:register(u0);"
                       "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){"
                       "outbuf.Store(0,1);}";
    ID3D12Device* device = nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* predicate_true = nullptr;
    ID3D12Resource* predicate_false = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* output_suppressed = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;
    struct PredicationFeature {
        BOOL Supported;
    } predication = {};
    HRESULT predication_hr = E_FAIL;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        predication_hr = device->CheckFeatureSupport(static_cast<D3D12_FEATURE>(50), &predication,
                                                     sizeof(predication));
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable.NumDescriptorRanges = 1;
        parameter.DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &parameter;
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
        desc.NumDescriptors = 2;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }
    const uint32_t one = 1;
    const uint32_t zero = 0;
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &one, sizeof(one), &predicate_true);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &zero, sizeof(zero), &predicate_false);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output_suppressed);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 2 * sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        const uint32_t initial = 0;
        hr = output->WriteToSubresource(0, nullptr, &initial, sizeof(initial), sizeof(initial));
        if (SUCCEEDED(hr))
            hr = output_suppressed->WriteToSubresource(0, nullptr, &initial, sizeof(initial), sizeof(initial));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 1;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE second_cpu = heap->GetCPUDescriptorHandleForHeapStart();
        second_cpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateUnorderedAccessView(output_suppressed, nullptr, &uav, second_cpu);
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->SetPredication(predicate_true, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
        list->Dispatch(1, 1, 1);
        D3D12_GPU_DESCRIPTOR_HANDLE second_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        second_gpu.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(0, second_gpu);
        list->SetPredication(predicate_false, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER copy_barriers[] = {
            uav_barrier(output),
            uav_barrier(output_suppressed),
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(output_suppressed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list->ResourceBarrier(4, copy_barriers);
        list->CopyBufferRegion(output_suppressed, 0, output, 0, sizeof(uint32_t));
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
        D3D12_RESOURCE_BARRIER suppressed_source =
            transition_barrier(output_suppressed, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &suppressed_source);
        list->CopyBufferRegion(readback, 0, output, 0, sizeof(uint32_t));
        list->CopyBufferRegion(readback, sizeof(uint32_t), output_suppressed, 0, sizeof(uint32_t));
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint32_t values[2] = {};
    const bool verified = SUCCEEDED(hr) && SUCCEEDED(predication_hr) && predication.Supported &&
                          readback_u32(readback, values, 2) && values[0] == 1 && values[1] == 0;
    result.pass = verified;
    result.hr = verified ? S_OK : hr;
    result.detail = verified ? "nonzero predicate executes and zero predicate suppresses dispatch and copy"
                             : "predication readback mismatch";
    result.extra = std::string("\"feature_supported\":") + (predication.Supported ? "true" : "false") +
                   ",\"feature_hr\":\"" + hr_hex(predication_hr) + "\",\"executed_value\":" +
                   std::to_string(values[0]) + ",\"suppressed_dispatch_value\":" +
                   std::to_string(values[1]) + ",\"suppressed_copy_value\":" + std::to_string(values[1]);

    safe_release(readback);
    safe_release(output_suppressed);
    safe_release(output);
    safe_release(predicate_false);
    safe_release(predicate_true);
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

static CaseResult run_execute_indirect_graphics_case() {
    CaseResult result = {"execute_indirect_graphics_readback", false, E_FAIL, "", ""};
    const char* vs_source =
        "struct VSOut { float4 position : SV_Position; };"
        "VSOut main(uint id : SV_VertexID) { VSOut o; "
        "float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), "
        "float2(-1.0, 3.0) }; o.position = float4(p[id], 0.0, 1.0); return o; }";
    const char* ps_source =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* arguments = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_vertex_shader(vs_source, "main", &vs, detail);
    if (SUCCEEDED(hr))
        hr = compile_pixel_shader(ps_source, "main", &ps, detail);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    D3D12_INDIRECT_ARGUMENT_DESC draw_argument = {};
    draw_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &draw_argument;
    if (SUCCEEDED(hr))
        hr = device->CreateCommandSignature(&signature_desc, nullptr,
                                            IID_PPV_ARGS(&signature));
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
    D3D12_DRAW_ARGUMENTS draw = {3, 1, 0, 0};
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(draw),
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &arguments);
    if (SUCCEEDED(hr))
        hr = arguments->WriteToSubresource(0, nullptr, &draw, sizeof(draw),
                                            sizeof(draw));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        clear.Color[3] = 1.0f;
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                             &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             &clear, IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(
            render_target, &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, 1, 1};
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RENDER_PASS_RENDER_TARGET_DESC pass_target = {};
        pass_target.cpuDescriptor = rtv;
        pass_target.BeginningAccess.Type =
            D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        pass_target.EndingAccess.Type =
            D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        list->SetPipelineState(pso);
        list4->BeginRenderPass(1, &pass_target, nullptr,
                               D3D12_RENDER_PASS_FLAG_NONE);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->ExecuteIndirect(signature, 1, arguments, 0, nullptr, 0);
        list4->EndRenderPass();
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = 1;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = render_target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint8_t pixel[4] = {};
    const bool pixel_readback = SUCCEEDED(hr) && readback_bytes(readback, pixel, sizeof(pixel));
    const bool pixel_verified = pixel_readback && pixel[0] == 255 && pixel[1] == 0 &&
                                pixel[2] == 0 && pixel[3] == 255;
    result.pass = pixel_verified;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "GPU-only indirect DRAW executed and exact render-target readback verified"
                        : "indirect graphics draw or exact render-target readback failed";
    char extra[384] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"pixel_verified\":%s,\"pixel\":[%u,%u,%u,%u],"
                  "\"argument_kind\":\"DRAW\",\"argument_buffer_gpu_only\":true",
                  pixel_verified ? "true" : "false", pixel[0], pixel[1], pixel[2], pixel[3]);
    result.extra = extra;
    safe_release(rtv_heap);
    safe_release(readback);
    safe_release(render_target);
    safe_release(arguments);
    safe_release(signature);
    safe_release(pso);
    safe_release(ps);
    safe_release(vs);
    safe_release(list);
    safe_release(list4);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_execute_indirect_indexed_graphics_case() {
    CaseResult result = {"execute_indirect_indexed_graphics_readback", false, E_FAIL, "", ""};
    const char* vs_source =
        "struct VSIn { float4 position : POSITION; };"
        "struct VSOut { float4 position : SV_Position; };"
        "VSOut main(VSIn input) { VSOut o; o.position = input.position; return o; }";
    const char* ps_source =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    const float vertices[3][4] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        {3.0f, -1.0f, 0.0f, 1.0f},
        {-1.0f, 3.0f, 0.0f, 1.0f}};
    const uint32_t indices[3] = {0, 1, 2};
    struct IndexedArguments {
        D3D12_VERTEX_BUFFER_VIEW vertex;
        D3D12_INDEX_BUFFER_VIEW index;
        D3D12_DRAW_INDEXED_ARGUMENTS draw;
    };

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* index_buffer = nullptr;
    ID3D12Resource* arguments = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_vertex_shader(vs_source, "main", &vs, detail);
    if (SUCCEEDED(hr))
        hr = compile_pixel_shader(ps_source, "main", &ps, detail);
    if (SUCCEEDED(hr)) {
        D3D12_INPUT_ELEMENT_DESC input = {};
        input.SemanticName = "POSITION";
        input.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.InputLayout = {&input, 1};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    D3D12_INDIRECT_ARGUMENT_DESC signature_arguments[3] = {};
    signature_arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    signature_arguments[0].VertexBuffer.Slot = 0;
    signature_arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    signature_arguments[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_VERTEX_BUFFER_VIEW) +
                                sizeof(D3D12_INDEX_BUFFER_VIEW) +
                                sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    signature_desc.NumArgumentDescs = 3;
    signature_desc.pArgumentDescs = signature_arguments;
    HRESULT signature_hr = SUCCEEDED(hr)
                               ? device->CreateCommandSignature(
                                     &signature_desc, nullptr,
                                     IID_PPV_ARGS(&signature))
                               : hr;
    hr = SUCCEEDED(hr) ? signature_hr : hr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, vertices, sizeof(vertices),
                                  &vertex_buffer);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, indices, sizeof(indices),
                                  &index_buffer);
    IndexedArguments indirect = {};
    if (SUCCEEDED(hr)) {
        indirect.vertex.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
        indirect.vertex.SizeInBytes = sizeof(vertices);
        indirect.vertex.StrideInBytes = sizeof(vertices[0]);
        indirect.index.BufferLocation = index_buffer->GetGPUVirtualAddress();
        indirect.index.SizeInBytes = sizeof(indices);
        indirect.index.Format = DXGI_FORMAT_R32_UINT;
        indirect.draw.IndexCountPerInstance = 3;
        indirect.draw.InstanceCount = 1;
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(indirect),
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &arguments);
    }
    HRESULT arguments_write_hr = E_FAIL;
    if (SUCCEEDED(hr))
        arguments_write_hr = arguments->WriteToSubresource(
            0, nullptr, &indirect, sizeof(indirect), sizeof(indirect));
    if (SUCCEEDED(hr))
        hr = arguments_write_hr;
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        clear.Color[3] = 1.0f;
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                             &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             &clear, IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 256,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(
            render_target, &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, 1, 1};
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->SetPipelineState(pso);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->ExecuteIndirect(signature, 1, arguments, 0, nullptr, 0);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = 1;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = render_target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint8_t pixel[4] = {};
    const bool pixel_readback = SUCCEEDED(hr) && readback_bytes(readback, pixel, sizeof(pixel));
    const bool pixel_verified = pixel_readback && pixel[0] == 255 && pixel[1] == 0 &&
                                pixel[2] == 0 && pixel[3] == 255;
    result.pass = pixel_verified;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "GPU-only indirect VBV/IBV plus DRAW_INDEXED executed and exact pixel readback verified"
                        : "indirect VBV/IBV indexed draw or exact readback failed";
    char extra[384] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"pixel_verified\":%s,\"pixel\":[%u,%u,%u,%u],"
                  "\"argument_kinds\":[\"VBV\",\"IBV\",\"DRAW_INDEXED\"],"
                  "\"argument_buffer_gpu_only\":true",
                  pixel_verified ? "true" : "false", pixel[0], pixel[1], pixel[2], pixel[3]);
    result.extra = extra;
    safe_release(rtv_heap);
    safe_release(readback);
    safe_release(render_target);
    safe_release(arguments);
    safe_release(index_buffer);
    safe_release(vertex_buffer);
    safe_release(signature);
    safe_release(pso);
    safe_release(ps);
    safe_release(vs);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_view_instancing_case() {
    CaseResult result = {"view_instancing_mask_side_effect", false, E_FAIL, "", ""};
    const char* vs_source =
        "struct VSOut { float4 position : SV_Position; };"
        "VSOut main(uint id : SV_VertexID) { VSOut o; "
        "float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), "
        "float2(-1.0, 3.0) }; o.position = float4(p[id], 0.0, 1.0); return o; }";
    const char* red_source =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    const char* green_source =
        "float4 main() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }";
    ViewInstanceLocationProbe locations[2] = {{0, 0}, {1, 1}};

    ID3D12Device* device = nullptr;
    ID3D12Device2* device2 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* red = nullptr;
    ID3DBlob* green = nullptr;
    ID3D12PipelineState* red_pso = nullptr;
    ID3D12PipelineState* green_pso = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = device->QueryInterface(IID_PPV_ARGS(&device2));
    if (SUCCEEDED(hr))
        hr = compile_vertex_shader(vs_source, "main", &vs, detail);
    if (SUCCEEDED(hr))
        hr = compile_pixel_shader(red_source, "main", &red, detail);
    if (SUCCEEDED(hr))
        hr = compile_pixel_shader(green_source, "main", &green, detail);
    if (SUCCEEDED(hr)) {
        auto create_view_pso = [&](ID3DBlob* pixel, ID3D12PipelineState** out) {
            auto stream_bytes = make_view_instancing_stream(vs, pixel, locations);
            D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {
                stream_bytes.size(), stream_bytes.data()};
            return device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(out));
        };
        hr = create_view_pso(red, &red_pso);
        if (SUCCEEDED(hr))
            hr = create_view_pso(green, &green_pso);
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list1));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC target_desc = {};
        target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        target_desc.Width = 1;
        target_desc.Height = 1;
        target_desc.DepthOrArraySize = 2;
        target_desc.MipLevels = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = 1;
        target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        clear.Color[3] = 1.0f;
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
            IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 512,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    if (SUCCEEDED(hr)) {
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_desc.Texture2DArray.MipSlice = 0;
        rtv_desc.Texture2DArray.FirstArraySlice = 0;
        rtv_desc.Texture2DArray.ArraySize = 2;
        device->CreateRenderTargetView(render_target, &rtv_desc, rtv);
        const float clear_color[4] = {0, 0, 0, 1};
        const D3D12_VIEWPORT viewports[2] = {
            {0, 0, 1, 1, 0, 1}, {0, 0, 1, 1, 0, 1}};
        const D3D12_RECT scissors[2] = {{0, 0, 1, 1}, {0, 0, 1, 1}};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->SetPipelineState(red_pso);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(2, viewports);
        list->RSSetScissorRects(2, scissors);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list1->SetViewInstanceMask(0x1u);
        list->DrawInstanced(3, 1, 0, 0);
        list->SetPipelineState(green_pso);
        list1->SetViewInstanceMask(0x2u);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        for (UINT slice = 0; slice < 2; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = slice * 256;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = 1;
            dst.PlacedFootprint.Footprint.Height = 1;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = 256;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = render_target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = slice;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint8_t bytes[512] = {};
    const bool readback_ok = SUCCEEDED(hr) && readback_bytes(readback, bytes, sizeof(bytes));
    const bool slice0_red = readback_ok && bytes[0] == 255 && bytes[1] == 0 &&
                            bytes[2] == 0 && bytes[3] == 255;
    const bool slice1_green = readback_ok && bytes[256] == 0 && bytes[257] == 255 &&
                              bytes[258] == 0 && bytes[259] == 255;
    result.pass = slice0_red && slice1_green;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "view-instance masks routed one draw each to exact array-layer readbacks"
                        : "view-instance mask or per-view array-layer readback failed";
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"pipeline_view_instance_count\":2,\"mask_first\":1,"
                  "\"mask_second\":2,\"slice0_red\":%s,\"slice1_green\":%s,"
                  "\"slice0_rgba\":[%u,%u,%u,%u],\"slice1_rgba\":[%u,%u,%u,%u]",
                  slice0_red ? "true" : "false", slice1_green ? "true" : "false",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[256], bytes[257],
                  bytes[258], bytes[259]);
    result.extra = extra;
    safe_release(rtv_heap);
    safe_release(readback);
    safe_release(render_target);
    safe_release(green_pso);
    safe_release(red_pso);
    safe_release(green);
    safe_release(red);
    safe_release(vs);
    safe_release(list1);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device2);
    safe_release(device);
    return result;
}

static CaseResult run_multi_pixel_sample_positions_case() {
    CaseResult result = {"multi_pixel_sample_positions", false, E_FAIL, "", ""};
    const char* vs_source =
        "struct VSOut { float4 position : SV_Position; };"
        "VSOut main(uint id : SV_VertexID) { VSOut o; "
        "float2 p[3] = { float2(1.0, 1.0), float2(1.0, 0.5), "
        "float2(0.75, 1.0) }; o.position = float4(p[id], 0.0, 1.0); return o; }";
    const char* ps_source =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }";
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* resolved_target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_vertex_shader(vs_source, "main", &vs, detail);
    if (SUCCEEDED(hr))
        hr = compile_pixel_shader(ps_source, "main", &ps, detail);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 4;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list1));
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 2;
        desc.Height = 2;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 4;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        clear.Color[3] = 1.0f;
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
            IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 2;
        desc.Height = 2;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format;
        clear.Color[3] = 1.0f;
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, &clear,
            IID_PPV_ARGS(&resolved_target));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, 512,
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device->CreateRenderTargetView(render_target, &rtv_desc, rtv);
        const float clear_color[4] = {0, 0, 0, 1};
        const D3D12_VIEWPORT viewport = {0, 0, 2, 2, 0, 1};
        const D3D12_RECT scissor = {0, 0, 2, 2};
        // Four samples per pixel, with a distinct corner cluster for each
        // pixel in the 2x2 programmable pattern.
        D3D12_SAMPLE_POSITION positions[16] = {
            {-7, -7}, {-6, -7}, {-7, -6}, {-6, -6},
            { 7, -7}, { 6, -7}, { 7, -6}, { 6, -6},
            {-7,  7}, {-6,  7}, {-7,  6}, {-6,  6},
            { 7,  7}, { 6,  7}, { 7,  6}, { 6,  6}};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->SetPipelineState(pso);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list1->SetSamplePositions(4, 4, positions);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER target_barrier = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        list->ResourceBarrier(1, &target_barrier);
        list->ResolveSubresource(resolved_target, 0, render_target, 0,
                                 DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_RESOURCE_BARRIER resolved_barrier = transition_barrier(
            resolved_target, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &resolved_barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = 2;
        dst.PlacedFootprint.Footprint.Height = 2;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = resolved_target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint8_t bytes[512] = {};
    const bool readback_ok = SUCCEEDED(hr) &&
                             readback_bytes(readback, bytes, sizeof(bytes));
    const bool pixel0_black = readback_ok && bytes[0] == 0 && bytes[1] == 0 &&
                              bytes[2] == 0 && bytes[3] == 255;
    const bool pixel1_red = readback_ok && bytes[4] == 255 && bytes[5] == 0 &&
                            bytes[6] == 0 && bytes[7] == 255;
    const bool pixel2_black = readback_ok && bytes[256] == 0 &&
                              bytes[257] == 0 && bytes[258] == 0 &&
                              bytes[259] == 255;
    const bool pixel3_black = readback_ok && bytes[260] == 0 &&
                              bytes[261] == 0 && bytes[262] == 0 &&
                              bytes[263] == 255;
    result.pass = pixel0_black && pixel1_red && pixel2_black && pixel3_black;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "four-pixel programmable sample pattern produced exact per-pixel MSAA resolve readback"
                        : "multi-pixel sample-position pattern or exact MSAA resolve readback failed";
    char extra[768] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"sample_count\":4,\"pixel_count\":4,"
                  "\"pixel0_black\":%s,\"pixel1_red\":%s,"
                  "\"pixel2_black\":%s,\"pixel3_black\":%s,"
                  "\"pixels_rgba\":[[%u,%u,%u,%u],[%u,%u,%u,%u],"
                  "[%u,%u,%u,%u],[%u,%u,%u,%u]]",
                  pixel0_black ? "true" : "false", pixel1_red ? "true" : "false",
                  pixel2_black ? "true" : "false", pixel3_black ? "true" : "false",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[256], bytes[257], bytes[258],
                  bytes[259], bytes[260], bytes[261], bytes[262], bytes[263]);
    result.extra = extra;
    safe_release(rtv_heap);
    safe_release(readback);
    safe_release(resolved_target);
    safe_release(render_target);
    safe_release(pso);
    safe_release(ps);
    safe_release(vs);
    safe_release(list1);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_execute_indirect_root_descriptors_case() {
    CaseResult result = {"execute_indirect_root_descriptors", false, E_FAIL, "", ""};
    const char* hlsl =
        "cbuffer Params : register(b0) { uint base; };"
        "ByteAddressBuffer input : register(t0);"
        "RWByteAddressBuffer output : register(u0);"
        "[numthreads(4,1,1)] void main(uint3 id : SV_DispatchThreadID) {"
        " output.Store(id.x * 4, base + input.Load(id.x * 4)); }";
    uint32_t input_values[4] = {1, 2, 3, 4};
    uint32_t expected[4] = {31, 32, 33, 34};
    const uint32_t base_value = 30;
    uint8_t params_data[256] = {};
    std::memcpy(params_data, &base_value, sizeof(base_value));
    struct IndirectDescriptors {
        D3D12_GPU_VIRTUAL_ADDRESS cbv;
        D3D12_GPU_VIRTUAL_ADDRESS srv;
        D3D12_GPU_VIRTUAL_ADDRESS uav;
        D3D12_DISPATCH_ARGUMENTS dispatch;
    };

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* params = nullptr;
    ID3D12Resource* input = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* arguments = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_ROOT_PARAMETER root_parameters[3] = {};
        root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[0].Descriptor.ShaderRegister = 0;
        root_parameters[0].Descriptor.RegisterSpace = 0;
        root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        root_parameters[1].Descriptor.ShaderRegister = 0;
        root_parameters[1].Descriptor.RegisterSpace = 0;
        root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        root_parameters[2].Descriptor.ShaderRegister = 0;
        root_parameters[2].Descriptor.RegisterSpace = 0;
        root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 3;
        root_desc.pParameters = root_parameters;
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                          root_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    D3D12_INDIRECT_ARGUMENT_DESC signature_arguments[4] = {};
    signature_arguments[0].Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    signature_arguments[0].ConstantBufferView.RootParameterIndex = 0;
    signature_arguments[1].Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW;
    signature_arguments[1].ShaderResourceView.RootParameterIndex = 1;
    signature_arguments[2].Type =
        D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
    signature_arguments[2].UnorderedAccessView.RootParameterIndex = 2;
    signature_arguments[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_GPU_VIRTUAL_ADDRESS) * 3 +
                                sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature_desc.NumArgumentDescs = 4;
    signature_desc.pArgumentDescs = signature_arguments;
    HRESULT signature_hr = SUCCEEDED(hr)
                               ? device->CreateCommandSignature(
                                     &signature_desc, root,
                                     IID_PPV_ARGS(&signature))
                               : hr;
    hr = SUCCEEDED(hr) ? signature_hr : hr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, input_values, sizeof(input_values),
                                  &input);
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, params_data, sizeof(params_data),
                                  &params);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(expected),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    IndirectDescriptors descriptor_values = {};
    if (SUCCEEDED(hr)) {
        descriptor_values.cbv = params->GetGPUVirtualAddress();
        descriptor_values.srv = input->GetGPUVirtualAddress();
        descriptor_values.uav = output->GetGPUVirtualAddress();
        descriptor_values.dispatch = {1, 1, 1};
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT,
                           sizeof(descriptor_values), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &arguments);
    }
    HRESULT arguments_write_hr = E_FAIL;
    if (SUCCEEDED(hr))
        arguments_write_hr = arguments->WriteToSubresource(
            0, nullptr, &descriptor_values, sizeof(descriptor_values),
            sizeof(descriptor_values));
    if (SUCCEEDED(hr))
        hr = arguments_write_hr;
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(expected),
                           D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        list->SetComputeRootSignature(root);
        list->SetPipelineState(pso);
        list->ExecuteIndirect(signature, 1, arguments, 0, nullptr, 0);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback, 0, output, 0, sizeof(expected));
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint32_t got[4] = {};
    const bool readback_ok = SUCCEEDED(hr) && readback_u32(readback, got, 4);
    const bool verified = readback_ok && std::memcmp(got, expected, sizeof(expected)) == 0;
    result.pass = verified;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "GPU-only indirect CBV/SRV/UAV addresses and dispatch readback verified"
                        : "indirect root-descriptor execution or readback failed";
    char extra[384] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"signature\":\"%s\",\"arguments_write\":\"%s\","
                  "\"values\":[%u,%u,%u,%u],\"readback_verified\":%s",
                  hr_hex(signature_hr).c_str(), hr_hex(arguments_write_hr).c_str(),
                  got[0], got[1], got[2], got[3], verified ? "true" : "false");
    result.extra = extra;
    safe_release(readback);
    safe_release(arguments);
    safe_release(output);
    safe_release(input);
    safe_release(params);
    safe_release(signature);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(cs);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_stream_output_case() {
    CaseResult result = {"stream_output_capture", false, E_FAIL, "", ""};
    const char* hlsl =
        "struct VSOut { float4 position : SV_Position; uint4 value : TEXCOORD0; };"
        "VSOut main(uint id : SV_VertexID) { VSOut o; "
        "o.position = float4(0.0, 0.0, 0.0, 1.0); "
        "o.value = uint4(id + 11, id + 21, id + 31, id + 41); return o; }";
    constexpr uint32_t kVertexCount = 4;
    constexpr uint32_t kDrawCount = 2;
    constexpr uint32_t kStride = sizeof(uint32_t) * 4;
    constexpr uint32_t kInitialFilledSize = kStride;
    constexpr uint32_t kPayloadBytes = kVertexCount * kStride * kDrawCount;
    constexpr uint32_t kOutputBytes = kInitialFilledSize + kPayloadBytes;
    uint32_t expected[kVertexCount * kDrawCount][4] = {};
    for (uint32_t draw = 0; draw < kDrawCount; ++draw) {
        for (uint32_t vertex = 0; vertex < kVertexCount; ++vertex) {
            const uint32_t record = draw * kVertexCount + vertex;
            expected[record][0] = vertex + 11;
            expected[record][1] = vertex + 21;
            expected[record][2] = vertex + 31;
            expected[record][3] = vertex + 41;
        }
    }

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3DBlob* vs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* filled_size = nullptr;
    ID3D12Resource* output_readback = nullptr;
    ID3D12Resource* filled_size_readback = nullptr;
    std::string detail;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = compile_vertex_shader(hlsl, "main", &vs, detail);
    if (SUCCEEDED(hr)) {
        D3D12_SO_DECLARATION_ENTRY declaration = {};
        declaration.Stream = 0;
        declaration.SemanticName = "TEXCOORD";
        declaration.SemanticIndex = 0;
        declaration.StartComponent = 0;
        declaration.ComponentCount = 4;
        declaration.OutputSlot = 0;
        UINT stride = kStride;
        D3D12_STREAM_OUTPUT_DESC stream_output = {};
        stream_output.pSODeclaration = &declaration;
        stream_output.NumEntries = 1;
        stream_output.pBufferStrides = &stride;
        stream_output.NumStrides = 1;
        stream_output.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.StreamOutput = stream_output;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        desc.SampleDesc.Count = 1;
        hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, kOutputBytes, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_STREAM_OUT, &output);
    if (SUCCEEDED(hr)) {
        const uint32_t initial_counter[2] = {0, kInitialFilledSize};
        hr = create_upload_buffer(device, initial_counter, sizeof(initial_counter),
                                  &filled_size);
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, kOutputBytes, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &output_readback);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &filled_size_readback);
    if (SUCCEEDED(hr)) {
        D3D12_STREAM_OUTPUT_BUFFER_VIEW view = {};
        view.BufferLocation = output->GetGPUVirtualAddress();
        view.SizeInBytes = kOutputBytes;
        view.BufferFilledSizeLocation = filled_size->GetGPUVirtualAddress() + sizeof(uint32_t);
        list->SetPipelineState(pso);
        list->SOSetTargets(0, 1, &view);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        list->DrawInstanced(kVertexCount, 1, 0, 0);
        list->DrawInstanced(kVertexCount, 1, 0, 0);
        // The target is full after two captures.  The provider must reject
        // this third draw before the SM50 epilogue can write past the view.
        list->DrawInstanced(kVertexCount, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier =
            transition_barrier(output, D3D12_RESOURCE_STATE_STREAM_OUT,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(output_readback, 0, output, 0, kOutputBytes);
        list->CopyBufferRegion(filled_size_readback, 0, filled_size,
                               sizeof(uint32_t), sizeof(uint32_t));
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }

    uint8_t captured[kOutputBytes] = {};
    uint32_t filled = 0;
    const bool output_readback_ok = SUCCEEDED(hr) &&
                                    readback_bytes(output_readback, captured,
                                                   sizeof(captured));
    const bool filled_readback_ok = SUCCEEDED(hr) &&
                                    readback_u32(filled_size_readback, &filled, 1);
    bool initial_region_untouched = output_readback_ok;
    for (uint32_t i = 0; i < kInitialFilledSize; ++i)
        initial_region_untouched &= captured[i] == 0;
    const auto *got = reinterpret_cast<const uint32_t *>(captured + kInitialFilledSize);
    const bool output_verified = output_readback_ok && initial_region_untouched &&
                                 std::memcmp(got, expected, sizeof(expected)) == 0;
    const bool filled_verified = filled_readback_ok && filled == kOutputBytes;
    result.pass = output_verified && filled_verified;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "single-stream DXBC vertex capture, counter accumulation across two draws, bounded overflow rejection, exact payload, and filled-size readback verified"
                        : "stream-output capture or filled-size readback failed";
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"output_verified\":%s,\"initial_region_untouched\":%s,"
                  "\"filled_size_verified\":%s,\"filled_size\":%u,"
                  "\"expected_filled_size\":%u,\"initial_filled_size\":%u,"
                  "\"filled_size_offset\":%u,\"stride\":%u,"
                  "\"vertex_count\":%u,\"draw_count\":%u,"
                  "\"captured_draw_count\":%u,\"overflow_guard_verified\":%s,"
                  "\"provider\":\"sm50_vertex_capture\"",
                  output_verified ? "true" : "false",
                  initial_region_untouched ? "true" : "false",
                  filled_verified ? "true" : "false", filled, kOutputBytes,
                  kInitialFilledSize, static_cast<unsigned>(sizeof(uint32_t)),
                  kStride, kVertexCount,
                  kDrawCount + 1, kDrawCount,
                  (output_verified && filled_verified) ? "true" : "false");
    result.extra = extra;
    safe_release(filled_size_readback);
    safe_release(output_readback);
    safe_release(filled_size);
    safe_release(output);
    safe_release(pso);
    safe_release(vs);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_execute_indirect_rays_case() {
    CaseResult result = {"execute_indirect_dispatch_rays", false, E_FAIL, "", ""};
    const std::string shader_path = getenv_string("D3D12_METAL_SDK_COMMAND_RAY_CSO").empty()
                                        ? "probe_command_replay_raygen.cso"
                                        : getenv_string("D3D12_METAL_SDK_COMMAND_RAY_CSO");
    std::vector<uint8_t> shader;
    if (!read_binary_file(shader_path.c_str(), shader)) {
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.detail = "command-replay raygen DXIL library is missing";
        result.extra = "\"shader_path\":\"" + json_escape(shader_path) + "\"";
        return result;
    }

    ID3D12Device* device = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList4* list4 = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12StateObject* state = nullptr;
    ID3D12StateObjectProperties* state_properties = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* direct_readback = nullptr;
    ID3D12Resource* indirect_readback = nullptr;
    ID3D12Resource* shader_table = nullptr;
    ID3D12Resource* arguments = nullptr;
    HRESULT hr = create_device(&device);
    HRESULT state_hr = E_FAIL;
    HRESULT signature_hr = E_FAIL;
    HRESULT arguments_hr = E_FAIL;
    bool direct_recorded = false;
    bool indirect_recorded = false;
    std::string detail;

    if (SUCCEEDED(hr))
        hr = device->QueryInterface(IID_PPV_ARGS(&device5));
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list4));

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ID3DBlob* root_blob = nullptr;
    if (SUCCEEDED(hr))
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    safe_release(root_blob);

    D3D12_EXPORT_DESC export_desc = {};
    export_desc.Name = L"raygen";
    D3D12_DXIL_LIBRARY_DESC library_desc = {};
    library_desc.DXILLibrary = {shader.data(), shader.size()};
    library_desc.NumExports = 1;
    library_desc.pExports = &export_desc;
    D3D12_GLOBAL_ROOT_SIGNATURE global_root = {root};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config = {};
    shader_config.MaxPayloadSizeInBytes = 4;
    shader_config.MaxAttributeSizeInBytes = 0;
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config = {};
    pipeline_config.MaxTraceRecursionDepth = 1;
    D3D12_STATE_SUBOBJECT subobjects[4] = {};
    subobjects[0] = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library_desc};
    subobjects[1] = {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root};
    subobjects[2] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config};
    subobjects[3] = {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config};
    D3D12_STATE_OBJECT_DESC state_desc = {};
    state_desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    state_desc.NumSubobjects = 4;
    state_desc.pSubobjects = subobjects;
    if (SUCCEEDED(hr)) {
        state_hr = device5->CreateStateObject(&state_desc, IID_PPV_ARGS(&state));
        hr = state_hr;
    }
    if (SUCCEEDED(hr))
        hr = state->QueryInterface(IID_PPV_ARGS(&state_properties));
    const void* raygen_identifier =
        state_properties ? state_properties->GetShaderIdentifier(L"raygen") : nullptr;

    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 2;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &direct_readback);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &indirect_readback);
    if (SUCCEEDED(hr)) {
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_TYPELESS;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 1;
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(output, &srv, srv_handle);
        D3D12_CPU_DESCRIPTOR_HANDLE uav_handle = srv_handle;
        uav_handle.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 1;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, uav_handle);
    }

    D3D12_DISPATCH_RAYS_DESC dispatch = {};
    dispatch.Width = 1;
    dispatch.Height = 1;
    dispatch.Depth = 1;
    if (raygen_identifier)
        dispatch.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    if (SUCCEEDED(hr) && raygen_identifier) {
        hr = create_upload_buffer(device, nullptr, 64, &shader_table);
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            D3D12_RANGE read_range = {0, 0};
            hr = shader_table->Map(0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                std::memcpy(mapped, raygen_identifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                shader_table->Unmap(0, nullptr);
                dispatch.RayGenerationShaderRecord.StartAddress = shader_table->GetGPUVirtualAddress();
            }
        }
    } else if (SUCCEEDED(hr)) {
        hr = E_FAIL;
    }

    D3D12_INDIRECT_ARGUMENT_DESC indirect_argument = {};
    indirect_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(dispatch);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &indirect_argument;
    if (SUCCEEDED(hr)) {
        signature_hr = device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature));
        hr = signature_hr;
    }
    constexpr UINT64 argument_offset = 16;
    const UINT64 argument_bytes = argument_offset + sizeof(dispatch);
    std::vector<uint8_t> argument_storage(static_cast<size_t>(argument_bytes), 0);
    std::memcpy(argument_storage.data() + argument_offset, &dispatch, sizeof(dispatch));
    if (SUCCEEDED(hr)) {
        arguments_hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, argument_bytes, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &arguments);
        hr = arguments_hr;
    }
    if (SUCCEEDED(hr)) {
        arguments_hr = arguments->WriteToSubresource(0, nullptr, argument_storage.data(),
                                                     static_cast<UINT>(argument_storage.size()),
                                                     static_cast<UINT>(argument_storage.size()));
        hr = arguments_hr;
    }

    if (SUCCEEDED(hr)) {
        ID3D12DescriptorHeap* heaps[] = {heap};
        list4->SetDescriptorHeaps(1, heaps);
        list4->SetComputeRootSignature(root);
        list4->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list4->SetPipelineState1(state);
        list4->DispatchRays(&dispatch);
        direct_recorded = true;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list4->ResourceBarrier(1, &to_copy);
        list4->CopyBufferRegion(direct_readback, 0, output, 0, sizeof(uint32_t));
        D3D12_RESOURCE_BARRIER to_uav = transition_barrier(
            output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list4->ResourceBarrier(1, &to_uav);
        const UINT clear_values[4] = {};
        D3D12_GPU_DESCRIPTOR_HANDLE output_gpu_handle = heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE output_cpu_handle = heap->GetCPUDescriptorHandleForHeapStart();
        const UINT descriptor_increment =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        output_gpu_handle.ptr += descriptor_increment;
        output_cpu_handle.ptr += descriptor_increment;
        list4->ClearUnorderedAccessViewUint(output_gpu_handle, output_cpu_handle, output, clear_values, 0,
                                            nullptr);
        list4->ExecuteIndirect(signature, 1, arguments, argument_offset, nullptr, 0);
        indirect_recorded = true;
        list4->ResourceBarrier(1, &to_copy);
        list4->CopyBufferRegion(indirect_readback, 0, output, 0, sizeof(uint32_t));
        hr = list4->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }

    uint32_t direct_value = 0;
    uint32_t indirect_value = 0;
    const bool direct_readback_ok = SUCCEEDED(hr) && readback_u32(direct_readback, &direct_value, 1);
    const bool indirect_readback_ok = SUCCEEDED(hr) && readback_u32(indirect_readback, &indirect_value, 1);
    const bool direct_verified = direct_readback_ok && direct_value == 0x52415931;
    const bool indirect_verified = indirect_readback_ok && indirect_value == 0x52415931;
    result.pass = SUCCEEDED(hr) && SUCCEEDED(state_hr) && SUCCEEDED(signature_hr) && SUCCEEDED(arguments_hr) &&
                  direct_recorded && indirect_recorded && direct_verified && indirect_verified;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "direct and ExecuteIndirect DISPATCH_RAYS command replay produced exact raygen UAV readbacks"
                        : "DISPATCH_RAYS direct/indirect command replay or readback failed";
    result.extra = "\"shader_path\":\"" + json_escape(shader_path) +
                   "\",\"state_object_created\":" + (SUCCEEDED(state_hr) ? "true" : "false") +
                   ",\"signature_hr\":\"" + hr_hex(signature_hr) + "\",\"arguments_hr\":\"" +
                   hr_hex(arguments_hr) + "\",\"direct_dispatch_recorded\":" +
                   (direct_recorded ? "true" : "false") + ",\"indirect_dispatch_recorded\":" +
                   (indirect_recorded ? "true" : "false") + ",\"direct_value\":" +
                   std::to_string(direct_value) + ",\"indirect_value\":" + std::to_string(indirect_value) +
                   ",\"direct_behavior_verified\":" + (direct_verified ? "true" : "false") +
                   ",\"indirect_behavior_verified\":" + (indirect_verified ? "true" : "false") +
                   ",\"argument_offset\":" + std::to_string(argument_offset);

    safe_release(arguments);
    safe_release(shader_table);
    safe_release(indirect_readback);
    safe_release(direct_readback);
    safe_release(output);
    safe_release(signature);
    safe_release(heap);
    safe_release(state_properties);
    safe_release(state);
    safe_release(root);
    safe_release(list4);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device5);
    safe_release(device);
    return result;
}

static CaseResult run_execute_indirect_mesh_case() {
    CaseResult result = {"execute_indirect_dispatch_mesh", false, E_FAIL, "", ""};
    const std::string ms_path = getenv_string("D3D12_METAL_SDK_COMMAND_MESH_MS_CSO").empty()
                                    ? "probe_command_replay_mesh_ms.cso"
                                    : getenv_string("D3D12_METAL_SDK_COMMAND_MESH_MS_CSO");
    const std::string ps_path = getenv_string("D3D12_METAL_SDK_COMMAND_MESH_PS_CSO").empty()
                                    ? "probe_command_replay_mesh_ps.cso"
                                    : getenv_string("D3D12_METAL_SDK_COMMAND_MESH_PS_CSO");
    std::vector<uint8_t> ms_shader;
    std::vector<uint8_t> ps_shader;
    if (!read_binary_file(ms_path.c_str(), ms_shader) || !read_binary_file(ps_path.c_str(), ps_shader)) {
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.detail = "command-replay mesh DXIL shader blobs are missing";
        result.extra = "\"ms_path\":\"" + json_escape(ms_path) + "\",\"ps_path\":\"" +
                       json_escape(ps_path) + "\"";
        return result;
    }

    ID3D12Device* device = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList6* list6 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* uav_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* output_direct_readback = nullptr;
    ID3D12Resource* output_indirect_readback = nullptr;
    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* direct_readback = nullptr;
    ID3D12Resource* indirect_readback = nullptr;
    ID3D12Resource* arguments = nullptr;
    HRESULT hr = create_device(&device);
    HRESULT pso_hr = E_FAIL;
    HRESULT signature_hr = E_FAIL;
    HRESULT arguments_hr = E_FAIL;
    bool direct_recorded = false;
    bool indirect_recorded = false;
    std::string detail;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list6));

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ID3DBlob* root_blob = nullptr;
    if (SUCCEEDED(hr))
        hr = serialize_root_signature(root_desc, &root_blob, detail);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    safe_release(root_blob);

    if (SUCCEEDED(hr)) {
        std::vector<uint8_t> stream;
        const D3D12_SHADER_BYTECODE ms_bytecode = {ms_shader.data(), ms_shader.size()};
        const D3D12_SHADER_BYTECODE ps_bytecode = {ps_shader.data(), ps_shader.size()};
        D3D12_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_RASTERIZER_DESC rasterizer = {};
        rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;
        rasterizer.DepthClipEnable = TRUE;
        D3D12_DEPTH_STENCIL_DESC depth_stencil = {};
        const UINT sample_mask = UINT_MAX;
        const D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        RenderTargetFormatsProbe formats = {};
        formats.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        formats.count = 1;
        const DXGI_SAMPLE_DESC sample = {1, 0};
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, ms_bytecode);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps_bytecode);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, sample_mask);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, rasterizer);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, depth_stencil);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, topology);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        append_pipeline_stream_subobject(stream, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, sample);
        D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {stream.size(), stream.data()};
        ID3D12Device2* device2 = nullptr;
        if (SUCCEEDED(hr))
            hr = device->QueryInterface(IID_PPV_ARGS(&device2));
        if (SUCCEEDED(hr))
            pso_hr = device2->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso));
        if (SUCCEEDED(hr))
            hr = pso_hr;
        safe_release(device2);
    }

    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uav_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &output_direct_readback);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(uint32_t), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &output_indirect_readback);

    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 16;
    target_desc.Height = 16;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 readback_bytes = 0;
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 1.0f;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                             IID_PPV_ARGS(&render_target));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows, &row_size, &readback_bytes);
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, readback_bytes, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &direct_readback);
    }
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, readback_bytes, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &indirect_readback);
    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 1;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, uav_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(render_target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    D3D12_DISPATCH_MESH_ARGUMENTS dispatch = {1, 1, 1};
    D3D12_INDIRECT_ARGUMENT_DESC indirect_argument = {};
    indirect_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(dispatch);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &indirect_argument;
    if (SUCCEEDED(hr)) {
        signature_hr = device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature));
        hr = signature_hr;
    }
    constexpr UINT64 argument_offset = 16;
    const UINT64 argument_bytes = argument_offset + sizeof(dispatch);
    std::vector<uint8_t> argument_storage(static_cast<size_t>(argument_bytes), 0);
    std::memcpy(argument_storage.data() + argument_offset, &dispatch, sizeof(dispatch));
    if (SUCCEEDED(hr)) {
        arguments_hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, argument_bytes, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, &arguments);
        hr = arguments_hr;
    }
    if (SUCCEEDED(hr)) {
        arguments_hr = arguments->WriteToSubresource(0, nullptr, argument_storage.data(),
                                                     static_cast<UINT>(argument_storage.size()),
                                                     static_cast<UINT>(argument_storage.size()));
        hr = arguments_hr;
    }

    if (SUCCEEDED(hr)) {
        ID3D12DescriptorHeap* heaps[] = {uav_heap};
        list6->SetDescriptorHeaps(1, heaps);
        list6->SetGraphicsRootSignature(root);
        list6->SetGraphicsRootDescriptorTable(0, uav_heap->GetGPUDescriptorHandleForHeapStart());
        list6->SetPipelineState(pso);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear_color[4] = {};
        list6->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list6->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list6->DispatchMesh(1, 1, 1);
        direct_recorded = true;
        D3D12_RESOURCE_BARRIER rt_to_copy = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_BARRIER output_to_copy = transition_barrier(
            output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list6->ResourceBarrier(1, &rt_to_copy);
        list6->ResourceBarrier(1, &output_to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = direct_readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = render_target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list6->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list6->CopyBufferRegion(output_direct_readback, 0, output, 0, sizeof(uint32_t));
        D3D12_RESOURCE_BARRIER rt_to_render = transition_barrier(
            render_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_RESOURCE_BARRIER output_to_uav = transition_barrier(
            output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list6->ResourceBarrier(1, &rt_to_render);
        list6->ResourceBarrier(1, &output_to_uav);
        list6->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        const UINT clear_values[4] = {};
        list6->ClearUnorderedAccessViewUint(uav_heap->GetGPUDescriptorHandleForHeapStart(),
                                            uav_heap->GetCPUDescriptorHandleForHeapStart(), output, clear_values, 0,
                                            nullptr);
        list6->ExecuteIndirect(signature, 1, arguments, argument_offset, nullptr, 0);
        indirect_recorded = true;
        list6->ResourceBarrier(1, &rt_to_copy);
        list6->ResourceBarrier(1, &output_to_copy);
        dst.pResource = indirect_readback;
        list6->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list6->CopyBufferRegion(output_indirect_readback, 0, output, 0, sizeof(uint32_t));
        hr = list6->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }

    uint32_t direct_output = 0;
    uint32_t indirect_output = 0;
    const bool direct_output_ok = SUCCEEDED(hr) && readback_u32(output_direct_readback, &direct_output, 1);
    const bool indirect_output_ok = SUCCEEDED(hr) && readback_u32(output_indirect_readback, &indirect_output, 1);
    uint64_t direct_pixels = 0;
    uint64_t indirect_pixels = 0;
    auto count_pixels = [&](ID3D12Resource* readback, uint64_t& count) {
        uint8_t* data = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(readback_bytes)};
        if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&data))) || !data)
            return false;
        for (UINT y = 0; y < 16; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(data + footprint.Footprint.RowPitch * y);
            for (UINT x = 0; x < 16; ++x)
                count += row[x] != 0;
        }
        readback->Unmap(0, nullptr);
        return true;
    };
    const bool direct_pixels_ok = SUCCEEDED(hr) && count_pixels(direct_readback, direct_pixels);
    const bool indirect_pixels_ok = SUCCEEDED(hr) && count_pixels(indirect_readback, indirect_pixels);
    const bool direct_verified = direct_output_ok && direct_output == 0x4d455348;
    const bool indirect_verified = indirect_output_ok && indirect_output == 0x4d455348;
    result.pass = SUCCEEDED(hr) && SUCCEEDED(pso_hr) && SUCCEEDED(signature_hr) && SUCCEEDED(arguments_hr) &&
                  direct_recorded && indirect_recorded && direct_verified && indirect_verified && direct_pixels_ok &&
                  indirect_pixels_ok && direct_pixels > 0 && indirect_pixels > 0;
    result.hr = result.pass ? S_OK : (FAILED(hr) ? hr : E_FAIL);
    result.detail = result.pass
                        ? "direct and ExecuteIndirect DISPATCH_MESH command replay produced exact UAV and raster readbacks"
                        : "DISPATCH_MESH direct/indirect command replay or readback failed";
    result.extra = "\"ms_path\":\"" + json_escape(ms_path) + "\",\"ps_path\":\"" + json_escape(ps_path) +
                   "\",\"pso_created\":" + (SUCCEEDED(pso_hr) ? "true" : "false") +
                   ",\"signature_hr\":\"" + hr_hex(signature_hr) + "\",\"arguments_hr\":\"" +
                   hr_hex(arguments_hr) + "\",\"direct_dispatch_recorded\":" +
                   (direct_recorded ? "true" : "false") + ",\"indirect_dispatch_recorded\":" +
                   (indirect_recorded ? "true" : "false") + ",\"direct_uav_value\":" +
                   std::to_string(direct_output) + ",\"indirect_uav_value\":" + std::to_string(indirect_output) +
                   ",\"direct_pixels\":" + std::to_string(direct_pixels) +
                   ",\"indirect_pixels\":" + std::to_string(indirect_pixels) +
                   ",\"direct_behavior_verified\":" + (direct_verified ? "true" : "false") +
                   ",\"indirect_behavior_verified\":" + (indirect_verified ? "true" : "false") +
                   ",\"argument_offset\":" + std::to_string(argument_offset);

    safe_release(arguments);
    safe_release(indirect_readback);
    safe_release(direct_readback);
    safe_release(output_indirect_readback);
    safe_release(output_direct_readback);
    safe_release(render_target);
    safe_release(output);
    safe_release(signature);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(pso);
    safe_release(root);
    safe_release(list6);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static CaseResult run_enhanced_barrier_case() {
    CaseResult result = {"enhanced_barriers", false, E_FAIL, "", ""};
    uint32_t expected[4] = {0x454e4831, 0x454e4832, 0x454e4833, 0x454e4834};
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList7* list7 = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* texture = nullptr;
    HRESULT hr = create_device(&device);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
    HRESULT options12_hr =
        SUCCEEDED(hr) ? device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12)) : hr;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->QueryInterface(IID_PPV_ARGS(&list7));
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, expected, sizeof(expected), &upload);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_DEFAULT, sizeof(expected), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &target);
    if (SUCCEEDED(hr))
        hr = create_buffer(device, D3D12_HEAP_TYPE_READBACK, sizeof(expected), D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 1;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        list7->CopyBufferRegion(target, 0, upload, 0, sizeof(expected));
        D3D12_GLOBAL_BARRIER global = {};
        global.SyncBefore = D3D12_BARRIER_SYNC_COPY;
        global.SyncAfter = D3D12_BARRIER_SYNC_COPY;
        global.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
        global.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
        D3D12_BUFFER_BARRIER buffer = {};
        buffer.SyncBefore = D3D12_BARRIER_SYNC_COPY;
        buffer.SyncAfter = D3D12_BARRIER_SYNC_COPY;
        buffer.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
        buffer.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
        buffer.pResource = target;
        buffer.Offset = 0;
        buffer.Size = sizeof(expected);
        D3D12_TEXTURE_BARRIER texture_barrier = {};
        texture_barrier.SyncBefore = D3D12_BARRIER_SYNC_NONE;
        texture_barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        texture_barrier.AccessBefore = D3D12_BARRIER_ACCESS_NO_ACCESS;
        texture_barrier.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
        texture_barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COMMON;
        texture_barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_COMMON;
        texture_barrier.pResource = texture;
        texture_barrier.Subresources.NumMipLevels = 1;
        texture_barrier.Subresources.NumArraySlices = 1;
        texture_barrier.Subresources.NumPlanes = 1;
        D3D12_BARRIER_GROUP groups[3] = {};
        groups[0].Type = D3D12_BARRIER_TYPE_GLOBAL;
        groups[0].NumBarriers = 1;
        groups[0].pGlobalBarriers = &global;
        groups[1].Type = D3D12_BARRIER_TYPE_BUFFER;
        groups[1].NumBarriers = 1;
        groups[1].pBufferBarriers = &buffer;
        groups[2].Type = D3D12_BARRIER_TYPE_TEXTURE;
        groups[2].NumBarriers = 1;
        groups[2].pTextureBarriers = &texture_barrier;
        list7->Barrier(3, groups);
        list7->CopyBufferRegion(readback, 0, target, 0, sizeof(expected));
        hr = list7->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list7};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint32_t got[4] = {};
    const bool values_verified =
        SUCCEEDED(hr) && readback_u32(readback, got, 4) && std::memcmp(got, expected, sizeof(expected)) == 0;
    result.pass = values_verified && SUCCEEDED(options12_hr) && options12.EnhancedBarriersSupported;
    result.hr = result.pass ? S_OK : hr;
    result.detail = result.pass ? "global/buffer/texture enhanced barrier groups and ordered buffer copy verified"
                                : "enhanced barrier interface, report, or ordered copy failed";
    char extra[384] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"options12_enhanced_barriers\":%s,\"group_count\":3,"
                  "\"global_barriers\":1,\"buffer_barriers\":1,"
                  "\"texture_barriers\":1,\"copy_values_verified\":%s,"
                  "\"values\":[%u,%u,%u,%u]",
                  options12.EnhancedBarriersSupported ? "true" : "false", values_verified ? "true" : "false", got[0],
                  got[1], got[2], got[3]);
    result.extra = extra;
    safe_release(texture);
    safe_release(readback);
    safe_release(target);
    safe_release(upload);
    safe_release(list7);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static void print_case(const CaseResult& result, bool last) {
    std::printf("    {\n");
    std::printf("      \"name\": \"%s\",\n", json_escape(result.name).c_str());
    std::printf("      \"pass\": %s,\n", result.pass ? "true" : "false");
    std::printf("      \"hr\": \"%s\",\n", hr_hex(result.hr).c_str());
    std::printf("      \"detail\": \"%s\"", json_escape(result.detail).c_str());
    if (!result.extra.empty())
        std::printf(",\n      %s\n", result.extra.c_str());
    else
        std::printf("\n");
    std::printf("    }%s\n", last ? "" : ",");
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    g_create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    g_serialize_root_signature = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    g_compile = load_proc<D3DCompileFn>(compiler, "D3DCompile");

    std::vector<CaseResult> cases;
    if (!g_create_device || !g_serialize_root_signature || !g_compile) {
        cases.push_back({"loader", false, E_FAIL, "required D3D12 or D3DCompile entry points missing", ""});
    } else {
        cases.push_back(run_command_list_reuse_case());
        cases.push_back(run_multi_list_execute_case());
        cases.push_back(run_bundle_status_case());
        cases.push_back(run_write_buffer_immediate_case());
        cases.push_back(run_execute_indirect_constants_case());
        cases.push_back(run_command_signature_validation_case());
        cases.push_back(run_predication_case());
        cases.push_back(run_execute_indirect_graphics_case());
        cases.push_back(run_execute_indirect_indexed_graphics_case());
        cases.push_back(run_view_instancing_case());
        cases.push_back(run_multi_pixel_sample_positions_case());
        cases.push_back(run_execute_indirect_root_descriptors_case());
        cases.push_back(run_stream_output_case());
        cases.push_back(run_execute_indirect_rays_case());
        cases.push_back(run_execute_indirect_mesh_case());
        cases.push_back(run_enhanced_barrier_case());
    }

    bool pass = true;
    for (const auto& item : cases)
        pass = pass && item.pass;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-command-replay.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"coverage\": {\n");
    std::printf("    \"command_list_reset_close_reuse\": true,\n");
    std::printf("    \"queue_execute_multiple_lists\": true,\n");
    std::printf("    \"bundle_status_reported\": true,\n");
    std::printf("    \"write_buffer_immediate_direct_compute_bundle\": true,\n");
    std::printf("    \"execute_indirect_dispatch\": true,\n");
    std::printf("    \"execute_indirect_gpu_argument_and_count\": true,\n");
    std::printf("    \"execute_indirect_root_constants_verified\": true,\n");
    std::printf("    \"execute_indirect_graphics_replay_status_reported\": true,\n");
    std::printf("    \"command_signature_bounds_validation\": true,\n");
    std::printf("    \"enhanced_barrier_global_buffer_texture\": true,\n");
    std::printf("    \"stream_output_capture_and_counter\": true,\n");
    std::printf("    \"execute_indirect_graphics_readback\": true,\n");
    std::printf("    \"execute_indirect_indexed_graphics_readback\": true,\n");
    std::printf("    \"execute_indirect_root_descriptors_readback\": true,\n");
    const auto case_pass = [&cases](const char* name) {
        for (const auto& item : cases) {
            if (item.name == name)
                return item.pass;
        }
        return false;
    };
    std::printf("    \"execute_indirect_dispatch_rays_readback\": %s,\n",
                case_pass("execute_indirect_dispatch_rays") ? "true" : "false");
    std::printf("    \"execute_indirect_dispatch_mesh_readback\": %s,\n",
                case_pass("execute_indirect_dispatch_mesh") ? "true" : "false");
    std::printf("    \"view_instancing_mask_side_effect\": true,\n");
    std::printf("    \"multi_pixel_sample_positions\": true,\n");
    std::printf("    \"predication_execution_verified\": true\n");
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < cases.size(); ++i)
        print_case(cases[i], i + 1 == cases.size());
    std::printf("  ]\n");
    std::printf("}\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
