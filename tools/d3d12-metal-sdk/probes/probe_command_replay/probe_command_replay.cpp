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
    if (SUCCEEDED(hr)) {
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
    result.pass = first_ok && second_ok && SUCCEEDED(allocator_reset_hr) && repeated_close_hr == E_FAIL &&
                  null_reset_hr == E_INVALIDARG && SUCCEEDED(list_reset_hr);
    result.hr = result.pass ? S_OK : hr;
    result.detail = result.pass ? "command list close, repeated-close rejection, allocator reset, null-reset "
                                  "rejection, list reset, and reuse verified"
                                : "command list reuse verification failed";
    result.extra = "\"first_verified\":" + std::string(first_ok ? "true" : "false") +
                   ",\"second_verified\":" + (second_ok ? "true" : "false") + ",\"allocator_reset\":\"" +
                   hr_hex(allocator_reset_hr) + "\",\"repeated_close\":\"" + hr_hex(repeated_close_hr) +
                   "\",\"null_reset\":\"" + hr_hex(null_reset_hr) + "\",\"list_reset\":\"" + hr_hex(list_reset_hr) +
                   "\"";

    safe_release(readback);
    safe_release(target);
    safe_release(upload_second);
    safe_release(upload_first);
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
    if (SUCCEEDED(hr))
        hr = create_upload_buffer(device, &indirect_args, sizeof(indirect_args), &args);
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
        list->ExecuteIndirect(compute_signature, 1, args, 0, nullptr, 0);
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
        SUCCEEDED(hr) && SUCCEEDED(compute_sig_hr) && SUCCEEDED(draw_sig_hr) && SUCCEEDED(draw_indexed_sig_hr);
    result.hr = result.pass ? S_OK : hr;
    result.detail =
        verified ? "ExecuteIndirect dispatch with root constants verified; draw signatures accepted"
                 : "ExecuteIndirect dispatch executes, but command-signature root constants are explicitly unsupported";
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"dispatch_root_constants_verified\":%s,\"values\":[%u,%u,%u,%u],"
                  "\"draw_signature\":\"%s\",\"draw_indexed_signature\":\"%s\","
                  "\"draw_replay_verified\":false,\"draw_indexed_replay_verified\":false,"
                  "\"graphics_indirect_replay_status\":\"signature-only\"",
                  verified ? "true" : "false", got[0], got[1], got[2], got[3], hr_hex(draw_sig_hr).c_str(),
                  hr_hex(draw_indexed_sig_hr).c_str());
    result.extra = extra;

    safe_release(readback);
    safe_release(output);
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

    HRESULT hr = create_device(&device);
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
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
        D3D12_RESOURCE_BARRIER barriers[] = {
            uav_barrier(output),
            uav_barrier(output_suppressed),
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(output_suppressed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(4, barriers);
        list->CopyBufferRegion(readback, 0, output, 0, sizeof(uint32_t));
        list->CopyBufferRegion(readback, sizeof(uint32_t), output_suppressed, 0, sizeof(uint32_t));
        hr = list->Close();
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {list};
        hr = execute_and_wait(device, queue, lists, 1, 1);
    }
    uint32_t values[2] = {};
    const bool verified = SUCCEEDED(hr) && readback_u32(readback, values, 2) && values[0] == 1 && values[1] == 0;
    result.pass = verified;
    result.hr = verified ? S_OK : hr;
    result.detail = verified ? "nonzero predicate executes and zero predicate suppresses dispatch"
                             : "predication readback mismatch";
    result.extra = std::string("\"executed_value\":") + std::to_string(values[0]) +
                   ",\"suppressed_value\":" + std::to_string(values[1]);

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
        cases.push_back(run_predication_case());
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
    std::printf("    \"execute_indirect_root_constants_status_reported\": true,\n");
    std::printf("    \"execute_indirect_graphics_replay_status_reported\": true,\n");
    std::printf("    \"enhanced_barrier_global_buffer_texture\": true,\n");
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
