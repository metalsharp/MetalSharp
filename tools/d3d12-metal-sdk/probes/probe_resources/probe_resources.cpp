#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_3.h>
#include <dxgiformat.h>

struct FormatProbe {
    const char* name;
    DXGI_FORMAT format;
    HRESULT hr = E_FAIL;
    HRESULT format_info_hr = E_FAIL;
    UINT support1 = 0;
    UINT support2 = 0;
    UINT plane_count = 0;
    UINT expected_plane_count = 1;
};

struct ResourceShapeProbe {
    const char* name;
    D3D12_RESOURCE_DESC requested = {};
    HRESULT hr = E_FAIL;
    D3D12_RESOURCE_DESC created = {};
};

struct TightAlignmentFeatureProbe {
    UINT SupportTier = 0;
};

struct SparseFormatProbe {
    const char* name;
    DXGI_FORMAT format;
    UINT width;
    UINT height;
    UINT expected_tile_width;
    UINT expected_tile_height;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* readback = nullptr;
    HRESULT texture_hr = E_FAIL;
    HRESULT tiling_hr = E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT readback_map_hr = E_FAIL;
    UINT total_tiles = 0;
    D3D12_TILE_SHAPE tile_shape = {};
    bool copy_ok = false;
};

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

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

static void print_hr(const char* key, HRESULT hr, bool comma = true) {
    std::printf("    \"%s\": \"0x%08lx\"%s\n", key, static_cast<unsigned long>(static_cast<uint32_t>(hr)),
                comma ? "," : "");
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

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

static void print_format_json(const FormatProbe& probe, bool last) {
    std::printf("    \"%s\": {\n", probe.name);
    std::printf("      \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(probe.hr)));
    std::printf("      \"format_info_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(probe.format_info_hr)));
    std::printf("      \"plane_count\": %u,\n", probe.plane_count);
    std::printf("      \"expected_plane_count\": %u,\n", probe.expected_plane_count);
    std::printf("      \"support1\": %u,\n", probe.support1);
    std::printf("      \"support2\": %u,\n", probe.support2);
    std::printf("      \"render_target\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ? "true" : "false");
    std::printf("      \"depth_stencil\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) ? "true" : "false");
    std::printf("      \"shader_sample\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ? "true" : "false");
    std::printf("      \"typed_uav_load\": %s\n",
                (probe.support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ? "true" : "false");
    std::printf("    }%s\n", last ? "" : ",");
}

static int run_shared_child() {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));
    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                            reinterpret_cast<void**>(&device))
                            : E_NOINTERFACE;
    HANDLE shared_handle = nullptr;
    ID3D12Resource* resource = nullptr;
    HANDLE fence_handle = nullptr;
    ID3D12Fence* shared_fence = nullptr;
    HANDLE heap_handle = nullptr;
    ID3D12Heap* shared_heap = nullptr;
    HRESULT open_name_hr = device ? device->OpenSharedHandleByName(L"metalsharp-probe-buffer", GENERIC_ALL,
                                                                    &shared_handle)
                                  : E_FAIL;
    HRESULT open_hr = SUCCEEDED(open_name_hr) && device
                          ? device->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&resource))
                          : E_FAIL;
    uint32_t before = 0;
    void* mapped = nullptr;
    HRESULT fence_name_hr = device ? device->OpenSharedHandleByName(L"metalsharp-probe-fence", GENERIC_ALL,
                                                                      &fence_handle)
                                    : E_FAIL;
    HRESULT fence_open_hr = SUCCEEDED(fence_name_hr) && device
                                ? device->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&shared_fence))
                                : E_FAIL;
    const HRESULT heap_name_hr = device ? device->OpenSharedHandleByName(L"metalsharp-probe-upload-heap",
                                                                          GENERIC_ALL, &heap_handle)
                                        : E_FAIL;
    const HRESULT heap_open_hr = SUCCEEDED(heap_name_hr) && device
                                     ? device->OpenSharedHandle(heap_handle, IID_PPV_ARGS(&shared_heap))
                                     : E_FAIL;
    D3D12_HEAP_DESC heap_desc = {};
    if (shared_heap)
        shared_heap->GetDesc(&heap_desc);
    const bool heap_verified = SUCCEEDED(heap_name_hr) && SUCCEEDED(heap_open_hr) && shared_heap &&
                               heap_desc.SizeInBytes == 64 * 1024 &&
                               heap_desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD;
    const uint64_t fence_value = shared_fence ? shared_fence->GetCompletedValue() : 0;
    const bool fence_verified = SUCCEEDED(fence_name_hr) && SUCCEEDED(fence_open_hr) && fence_value == 7;
    HRESULT map_hr = resource ? resource->Map(0, nullptr, &mapped) : E_FAIL;
    if (SUCCEEDED(map_hr) && mapped) {
        std::memcpy(&before, mapped, sizeof(before));
        const uint32_t after = 0xdecafbad;
        std::memcpy(mapped, &after, sizeof(after));
        resource->Unmap(0, nullptr);
    }
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(open_name_hr) && SUCCEEDED(open_hr) &&
                      SUCCEEDED(fence_name_hr) && SUCCEEDED(fence_open_hr) && fence_verified &&
                      fence_value == 7 && heap_verified && SUCCEEDED(map_hr) && before == 0x1234abcdu;
    // The parent owns the machine-readable probe stream; keep child output
    // silent so it cannot corrupt the parent's JSON document.
    if (resource)
        resource->Release();
    if (shared_handle)
        CloseHandle(shared_handle);
    if (fence_handle)
        CloseHandle(fence_handle);
    if (heap_handle)
        CloseHandle(heap_handle);
    if (shared_fence)
        shared_fence->Release();
    if (shared_heap)
        shared_heap->Release();
    if (device)
        device->Release();
    // Do not unload d3d12.dll explicitly: DXMT owns worker-thread state and
    // Wine's loader lock can otherwise outlive the child device teardown.
    return pass ? 0 : 1;
}

static bool launch_shared_child() {
    char module_path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, module_path, ARRAYSIZE(module_path)))
        return false;
    std::string command = std::string("\"") + module_path + "\" --shared-child";
    std::vector<char> command_line(command.begin(), command.end());
    command_line.push_back('\0');
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
        return false;
    const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--shared-child") == 0)
        return run_shared_child();

    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : E_FAIL;
    LUID device_luid = {};
    bool device_luid_ok = false;
    if (device)
        device_luid_ok = device->GetAdapterLuid(&device_luid) != nullptr &&
                         (device_luid.HighPart != 0 || device_luid.LowPart != 0);

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* sparse_mapping_queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Fence* sparse_mapping_fence = nullptr;
    ID3D12Fence* residency_fence = nullptr;
    HRESULT residency_fence_hr = E_FAIL;
    HRESULT enqueue_make_resident_hr = E_FAIL;
    HRESULT invalid_enqueue_flags_hr = E_FAIL;
    UINT64 enqueue_fence_completed = 0;
    HRESULT queue_hr = device ? device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)) : E_FAIL;
    HRESULT sparse_mapping_queue_hr =
        device ? device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&sparse_mapping_queue)) : E_FAIL;
    HRESULT allocator_hr =
        device ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) : E_FAIL;
    HRESULT list_hr =
        device ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))
               : E_FAIL;
    HRESULT list1_hr = list ? list->QueryInterface(IID_PPV_ARGS(&list1)) : E_FAIL;
    HRESULT fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) : E_FAIL;
    HRESULT sparse_mapping_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sparse_mapping_fence)) : E_FAIL;
    ID3D12DescriptorHeap *residency_descriptor_heap = nullptr;
    ID3D12QueryHeap *residency_query_heap = nullptr;
    HRESULT residency_descriptor_heap_create_hr = E_FAIL;
    HRESULT residency_query_heap_create_hr = E_FAIL;
    HRESULT residency_descriptor_heap_evict_hr = E_FAIL;
    HRESULT residency_descriptor_heap_make_hr = E_FAIL;
    HRESULT residency_query_heap_evict_hr = E_FAIL;
    HRESULT residency_query_heap_make_hr = E_FAIL;
    HRESULT residency_pageable_priority_hr = E_FAIL;
    ID3D12Fence* shared_fence = nullptr;
    ID3D12Fence* unnamed_shared_fence = nullptr;
    ID3D12Fence* unnamed_shared_fence_open = nullptr;
    HANDLE shared_fence_handle = nullptr;
    HANDLE unnamed_shared_fence_handle = nullptr;
    HRESULT shared_fence_create_hr = device
                                         ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                               IID_PPV_ARGS(&shared_fence))
                                         : E_FAIL;
    HRESULT shared_fence_handle_hr =
        shared_fence ? device->CreateSharedHandle(shared_fence, nullptr, GENERIC_ALL,
                                                   L"metalsharp-probe-fence", &shared_fence_handle)
                     : E_FAIL;
    HRESULT shared_fence_signal_hr =
        shared_fence && queue ? queue->Signal(shared_fence, 7) : E_FAIL;
    HRESULT shared_fence_mapping_signal_hr = E_FAIL;
    HRESULT shared_fence_wait_hr = E_FAIL;
    HRESULT unnamed_shared_fence_create_hr = E_FAIL;
    HRESULT unnamed_shared_fence_handle_hr = E_FAIL;
    HRESULT unnamed_shared_fence_open_hr = E_FAIL;
    HRESULT unnamed_shared_fence_signal_hr = E_FAIL;
    bool unnamed_shared_fence_roundtrip_ok = false;
    if (SUCCEEDED(shared_fence_signal_hr) && shared_fence_handle && device) {
        ID3D12Fence *mapped_fence_probe = nullptr;
        if (SUCCEEDED(device->OpenSharedHandle(
                shared_fence_handle, IID_PPV_ARGS(&mapped_fence_probe))) &&
            mapped_fence_probe) {
            for (UINT attempt = 0; attempt < 5000; ++attempt) {
                if (mapped_fence_probe->GetCompletedValue() >= 7) {
                    shared_fence_mapping_signal_hr = S_OK;
                    break;
                }
                Sleep(1);
            }
            shared_fence_wait_hr =
                sparse_mapping_queue
                    ? sparse_mapping_queue->Wait(mapped_fence_probe, 7)
                    : E_FAIL;
            mapped_fence_probe->Release();
        }
    }
    unnamed_shared_fence_create_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&unnamed_shared_fence))
               : E_FAIL;
    unnamed_shared_fence_handle_hr =
        unnamed_shared_fence
            ? device->CreateSharedHandle(unnamed_shared_fence, nullptr,
                                         GENERIC_ALL, nullptr,
                                         &unnamed_shared_fence_handle)
            : E_FAIL;
    unnamed_shared_fence_open_hr =
        SUCCEEDED(unnamed_shared_fence_handle_hr)
            ? device->OpenSharedHandle(unnamed_shared_fence_handle,
                                       IID_PPV_ARGS(&unnamed_shared_fence_open))
            : E_FAIL;
    unnamed_shared_fence_signal_hr =
        unnamed_shared_fence && queue ? queue->Signal(unnamed_shared_fence, 13)
                                      : E_FAIL;
    bool unnamed_shared_fence_signaled = false;
    if (SUCCEEDED(unnamed_shared_fence_signal_hr) &&
        unnamed_shared_fence_open) {
        for (UINT attempt = 0; attempt < 5000; ++attempt) {
            if (unnamed_shared_fence_open->GetCompletedValue() >= 13) {
                unnamed_shared_fence_signaled = true;
                break;
            }
            Sleep(1);
        }
    }
    unnamed_shared_fence_roundtrip_ok =
        SUCCEEDED(unnamed_shared_fence_create_hr) &&
        SUCCEEDED(unnamed_shared_fence_handle_hr) &&
        SUCCEEDED(unnamed_shared_fence_open_hr) &&
        SUCCEEDED(unnamed_shared_fence_signal_hr) &&
        unnamed_shared_fence_signaled;

    const UINT64 buffer_bytes = 4096;
    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC buffer = buffer_desc(buffer_bytes);
    ID3D12Resource* upload_buffer = nullptr;
    IDXGIDevice3* dxgi_device = nullptr;
    IDXGIResource* offered_dxgi_resource = nullptr;
    ID3D12Resource* default_buffer = nullptr;
    ID3D12Resource* readback_buffer = nullptr;
    ID3D12Resource* shared_open_buffer = nullptr;
    ID3D12Resource* shared_named_open_buffer = nullptr;
    ID3D12Resource* unnamed_shared_source = nullptr;
    ID3D12Resource* unnamed_shared_open = nullptr;
    HANDLE unnamed_shared_handle = nullptr;
    HRESULT unnamed_shared_source_hr = E_FAIL;
    HRESULT unnamed_shared_create_hr = E_FAIL;
    HRESULT unnamed_shared_open_hr = E_FAIL;
    HRESULT unnamed_shared_map_hr = E_FAIL;
    bool unnamed_shared_roundtrip_ok = false;
    ID3D12Resource* unknown_open_buffer = nullptr;
    ID3D12Heap* address_heap = nullptr;
    ID3D12Resource* address_resource = nullptr;
    ID3D12Resource* address_alias_resource = nullptr;
    ID3D12Heap* shared_heap_source = nullptr;
    ID3D12Heap* shared_heap_open = nullptr;
    ID3D12Heap* shared_heap_file_open = nullptr;
    ID3D12Heap* unnamed_shared_heap_source = nullptr;
    ID3D12Heap* unnamed_shared_heap_open = nullptr;
    HANDLE unnamed_shared_heap_handle = nullptr;
    std::vector<ResourceShapeProbe> resource_shapes;
    ID3D12Heap *placed_1d_array_heap = nullptr;
    ID3D12Resource *placed_1d_array_resource = nullptr;
    HRESULT placed_1d_array_heap_hr = E_FAIL;
    HRESULT placed_1d_array_resource_hr = E_FAIL;
    HRESULT placed_1d_array_write_hr[2] = {E_FAIL, E_FAIL};
    HRESULT placed_1d_array_read_hr[2] = {E_FAIL, E_FAIL};
    bool placed_1d_array_io_ok = false;
    HRESULT invalid_zero_width_hr = E_FAIL;
    HRESULT oversized_1d_hr = E_FAIL;
    HRESULT oversized_2d_hr = E_FAIL;
    HRESULT oversized_3d_hr = E_FAIL;
    HRESULT invalid_committed_heap_flags_hr = E_FAIL;
    HRESULT invalid_heap_properties_hr = E_FAIL;
    HRESULT invalid_node_mask_hr = E_FAIL;
    HRESULT invalid_upload_state_hr = E_FAIL;
    HRESULT invalid_readback_state_hr = E_FAIL;
    HRESULT invalid_buffer_resource_flags_hr = E_FAIL;
    HRESULT invalid_texture_resource_flags_hr = E_FAIL;
    HRESULT invalid_depth_format_flags_hr = E_FAIL;
    HRESULT invalid_clear_without_flag_hr = E_FAIL;
    HRESULT invalid_clear_format_hr = E_FAIL;
    HRESULT null_heap_properties_hr = E_FAIL;
    UINT64 invalid_zero_width_allocation_size = 0;
    UINT64 invalid_zero_width_allocation_alignment = 0;
    UINT64 allocation_overflow_size = UINT64_MAX;
    UINT64 allocation_overflow_alignment = UINT64_MAX;
    UINT64 allocation_batch_size = 0;
    UINT64 allocation_batch_alignment = 0;
    D3D12_RESOURCE_ALLOCATION_INFO1 allocation_batch_sideband[2] = {};
    bool allocation_batch_ok = false;
    HRESULT invalid_msaa_mips_hr = E_FAIL;
    UINT64 invalid_msaa_mips_allocation_size = 0;
    UINT64 invalid_msaa_mips_allocation_alignment = 0;
    UINT64 volume_allocation_size = 0;
    UINT64 volume_allocation_alignment = 0;
    UINT64 null_allocation_size = UINT64_MAX;
    UINT64 null_allocation_alignment = UINT64_MAX;
    HRESULT null_sideband_query_hr = E_FAIL;
    UINT64 null_sideband_size = UINT64_MAX;
    UINT64 null_sideband_alignment = UINT64_MAX;
    UINT64 null_sideband_offset = UINT64_MAX;
    UINT64 invalid_footprint_total = 0;
    UINT64 footprint_overflow_total = 0;
    UINT64 planar_footprint_total = 0;
    bool planar_footprint_ok = false;
    UINT64 nv12_footprint_total = 0;
    bool nv12_footprint_ok = false;
    HRESULT invalid_nv12_height_hr = E_FAIL;
    HRESULT tight_feature_hr = E_FAIL;
    UINT tight_feature_tier = 0;
    HRESULT tight_committed_hr = E_FAIL;
    HRESULT tight_allocation_info_hr = E_FAIL;
    UINT64 tight_allocation_size = 0;
    UINT64 tight_allocation_alignment = 0;
    HRESULT tight_heap_hr = E_FAIL;
    HRESULT tight_placed_hr = E_FAIL;
    HRESULT tight_invalid_alignment_hr = E_FAIL;
    HRESULT tight_overaligned_placed_hr = E_FAIL;
    HRESULT tight_reserved_hr = E_FAIL;
    HRESULT full_mip_create_hr = E_FAIL;
    UINT full_mip_count = 0;
    bool full_mip_footprint_ok = false;
    HRESULT not_resident_committed_hr = E_FAIL;
    HRESULT not_resident_initial_map_hr = E_FAIL;
    HRESULT not_resident_make_resident_hr = E_FAIL;
    HRESULT not_resident_remade_map_hr = E_FAIL;
    HRESULT not_resident_heap_hr = E_FAIL;
    HRESULT not_resident_placed_hr = E_FAIL;
    bool not_resident_roundtrip_ok = false;
    bool null_heap_desc_ok = false;
    bool tight_placed_roundtrip_ok = false;
    HANDLE shared_heap_handle = nullptr;
    HRESULT shared_heap_create_hr = E_FAIL;
    HRESULT shared_heap_open_hr = E_FAIL;
    HRESULT shared_heap_file_open_hr = E_FAIL;
    bool shared_heap_roundtrip_ok = false;
    HRESULT unnamed_shared_heap_create_hr = E_FAIL;
    HRESULT unnamed_shared_heap_handle_hr = E_FAIL;
    HRESULT unnamed_shared_heap_open_hr = E_FAIL;
    bool unnamed_shared_heap_roundtrip_ok = false;
    HANDLE shared_handle = nullptr;
    HANDLE shared_named_handle = nullptr;
    HRESULT shared_create_hr = E_FAIL;
    HRESULT shared_open_hr = E_FAIL;
    HRESULT shared_open_named_hr = E_FAIL;
    HRESULT shared_unknown_hr = E_FAIL;
    HRESULT shared_missing_name_hr = E_FAIL;
    HRESULT shared_invalid_create_access_hr = E_FAIL;
    HRESULT shared_invalid_open_access_hr = E_FAIL;
    bool shared_independent_object_ok = false;
    D3D12_RESOURCE_DESC default_buffer_desc = {};
    D3D12_GPU_VIRTUAL_ADDRESS upload_gpu_va = 0;
    D3D12_GPU_VIRTUAL_ADDRESS default_gpu_va = 0;
    D3D12_GPU_VIRTUAL_ADDRESS texture_gpu_va = 0;
    bool command_resource_lifetime_ok = false;
    bool atomic_copy_ok = false;
    bool atomic64_copy_ok = false;
    bool discard_ok = false;
    bool cross_process_shared_ok = false;
    bool shared_heap_cross_process_ok = false;
    HRESULT default_write_subresource_hr = E_FAIL;
    HRESULT default_read_subresource_hr = E_FAIL;
    bool default_cpu_io_verified = false;
    HRESULT residency_make_hr = E_FAIL;
    HRESULT residency_priority_hr = E_FAIL;
    HRESULT residency_evict_hr = E_FAIL;
    HRESULT residency_first_evict_map_hr = E_FAIL;
    HRESULT residency_second_evict_hr = E_FAIL;
    HRESULT residency_evicted_map_hr = E_FAIL;
    HRESULT residency_remake_hr = E_FAIL;
    HRESULT residency_remade_map_hr = E_FAIL;
    HRESULT invalid_map_range_hr = E_FAIL;
    bool invalid_map_range_ok = false;
    HRESULT dxgi_device_qi_hr = E_FAIL;
    HRESULT offered_dxgi_resource_qi_hr = E_FAIL;
    HRESULT offered_resource_priority_set_hr = E_FAIL;
    HRESULT offered_resource_priority_get_hr = E_FAIL;
    UINT offered_resource_priority = 0;
    HRESULT offer_resources_hr = E_FAIL;
    HRESULT trim_residency_hr = E_FAIL;
    HRESULT query_trim_residency_hr = E_FAIL;
    HRESULT reclaim_resources_hr = E_FAIL;
    HRESULT query_reclaim_residency_hr = E_FAIL;
    WINBOOL offered_resource_discarded = FALSE;
    bool dxgi_offer_reclaim_ok = false;
    bool residency_state_ok = false;
    HRESULT address_heap_hr = E_FAIL;
    HRESULT address_resource_hr = E_FAIL;
    HRESULT address_alias_resource_hr = E_FAIL;
    HRESULT address_open_hr = E_FAIL;
    HRESULT invalid_heap_alignment_hr = E_FAIL;
    HRESULT invalid_heap_flags_hr = E_FAIL;
    HRESULT invalid_heap_size_hr = E_FAIL;
    HRESULT misaligned_placement_hr = E_FAIL;
    bool address_heap_open_ok = false;
    bool heap_aliasing_ok = false;
    HRESULT address_heap_evict_hr = E_FAIL;
    HRESULT address_heap_map_hr = E_FAIL;
    HRESULT address_heap_make_resident_hr = E_FAIL;
    HRESULT address_heap_remap_hr = E_FAIL;
    bool heap_residency_ok = false;
    ID3D12Resource* tight_committed = nullptr;
    ID3D12Heap* tight_heap = nullptr;
    ID3D12Resource* tight_placed = nullptr;
    ID3D12Resource* not_resident_committed = nullptr;
    ID3D12Heap* not_resident_heap = nullptr;
    ID3D12Resource* not_resident_placed = nullptr;
    auto probe_resource_shape = [&](const char* name, D3D12_RESOURCE_DESC desc,
                                    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
        ResourceShapeProbe probe = {};
        probe.name = name;
        probe.requested = desc;
        ID3D12Resource* resource = nullptr;
        probe.hr = device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state,
                                                            nullptr, IID_PPV_ARGS(&resource))
                          : E_FAIL;
        if (resource) {
            probe.created = resource->GetDesc();
            resource->Release();
        }
        resource_shapes.push_back(probe);
    };
    if (device) {
        D3D12_RESOURCE_DESC shape = texture_desc(17, 1, DXGI_FORMAT_R8_UNORM);
        shape.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        probe_resource_shape("texture1d", shape);
        shape.DepthOrArraySize = 3;
        probe_resource_shape("texture1d_array", shape);
        shape = texture_desc(19, 11, DXGI_FORMAT_R8G8B8A8_UNORM);
        shape.DepthOrArraySize = 4;
        probe_resource_shape("texture2d_array", shape);
        shape.DepthOrArraySize = 6;
        probe_resource_shape("texture2d_cube", shape);
        shape = texture_desc(33, 17, DXGI_FORMAT_R8G8B8A8_UNORM);
        shape.MipLevels = 6;
        probe_resource_shape("texture2d_mips", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
        shape.SampleDesc.Count = 4;
        probe_resource_shape("texture2d_msaa4", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_D32_FLOAT);
        shape.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        probe_resource_shape("texture2d_depth", shape, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        shape = texture_desc(8, 8, DXGI_FORMAT_R32_TYPELESS);
        probe_resource_shape("texture2d_typeless", shape);
        shape = texture_desc(7, 5, DXGI_FORMAT_BC1_UNORM);
        probe_resource_shape("texture2d_bc1_unaligned", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_B8G8R8X8_UNORM);
        probe_resource_shape("texture2d_b8g8r8x8", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_B5G6R5_UNORM);
        probe_resource_shape("texture2d_b5g6r5", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_B5G5R5A1_UNORM);
        probe_resource_shape("texture2d_b5g5r5a1", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_B4G4R4A4_UNORM);
        probe_resource_shape("texture2d_b4g4r4a4", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
        probe_resource_shape("texture2d_r9g9b9e5", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_R8G8_B8G8_UNORM);
        probe_resource_shape("texture2d_r8g8_b8g8", shape);
        shape = texture_desc(8, 8, DXGI_FORMAT_G8R8_G8B8_UNORM);
        probe_resource_shape("texture2d_g8r8_g8b8", shape);
        shape = texture_desc(7, 5, DXGI_FORMAT_R8_UNORM);
        shape.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        shape.DepthOrArraySize = 4;
        probe_resource_shape("texture3d", shape);
        D3D12_RESOURCE_DESC placed_1d_array_desc =
            texture_desc(17, 1, DXGI_FORMAT_R8_UNORM);
        placed_1d_array_desc.Dimension =
            D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        placed_1d_array_desc.DepthOrArraySize = 2;
        D3D12_RESOURCE_ALLOCATION_INFO placed_1d_array_info =
            device->GetResourceAllocationInfo(0, 1, &placed_1d_array_desc);
        D3D12_HEAP_DESC placed_1d_array_heap_desc = {};
        placed_1d_array_heap_desc.SizeInBytes =
            placed_1d_array_info.SizeInBytes;
        placed_1d_array_heap_desc.Alignment = placed_1d_array_info.Alignment;
        placed_1d_array_heap_desc.Properties = default_heap;
        placed_1d_array_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
        placed_1d_array_heap_hr = device->CreateHeap(
            &placed_1d_array_heap_desc, IID_PPV_ARGS(&placed_1d_array_heap));
        placed_1d_array_resource_hr =
            placed_1d_array_heap
                ? device->CreatePlacedResource(
                      placed_1d_array_heap, 0, &placed_1d_array_desc,
                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                      IID_PPV_ARGS(&placed_1d_array_resource))
                : E_FAIL;
        if (placed_1d_array_resource) {
            uint8_t first_slice[17] = {};
            uint8_t second_slice[17] = {};
            for (UINT i = 0; i < 17; ++i) {
                first_slice[i] = static_cast<uint8_t>(0x20u + i);
                second_slice[i] = static_cast<uint8_t>(0x80u + i);
            }
            placed_1d_array_write_hr[0] =
                placed_1d_array_resource->WriteToSubresource(
                    0, nullptr, first_slice, 17, 17);
            placed_1d_array_write_hr[1] =
                placed_1d_array_resource->WriteToSubresource(
                    1, nullptr, second_slice, 17, 17);
            uint8_t first_readback[17] = {};
            uint8_t second_readback[17] = {};
            placed_1d_array_read_hr[0] =
                placed_1d_array_resource->ReadFromSubresource(
                    first_readback, 17, 17, 0, nullptr);
            placed_1d_array_read_hr[1] =
                placed_1d_array_resource->ReadFromSubresource(
                    second_readback, 17, 17, 1, nullptr);
            placed_1d_array_io_ok =
                SUCCEEDED(placed_1d_array_write_hr[0]) &&
                SUCCEEDED(placed_1d_array_write_hr[1]) &&
                SUCCEEDED(placed_1d_array_read_hr[0]) &&
                SUCCEEDED(placed_1d_array_read_hr[1]) &&
                std::memcmp(first_slice, first_readback, sizeof(first_slice)) == 0 &&
                std::memcmp(second_slice, second_readback,
                            sizeof(second_slice)) == 0;
        }
        ID3D12Resource* invalid_resource = nullptr;
        D3D12_RESOURCE_DESC invalid = texture_desc(0, 1, DXGI_FORMAT_R8_UNORM);
        invalid_zero_width_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &invalid,
                                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                 IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        D3D12_RESOURCE_ALLOCATION_INFO invalid_zero_width_info =
            device->GetResourceAllocationInfo(0, 1, &invalid);
        invalid_zero_width_allocation_size = invalid_zero_width_info.SizeInBytes;
        invalid_zero_width_allocation_alignment = invalid_zero_width_info.Alignment;
        D3D12_RESOURCE_DESC oversized_1d =
            texture_desc(D3D12_REQ_TEXTURE1D_U_DIMENSION + 1, 1,
                         DXGI_FORMAT_R8_UNORM);
        oversized_1d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        oversized_1d_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &oversized_1d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC oversized_2d =
            texture_desc(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1,
                         D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION + 1,
                         DXGI_FORMAT_R8_UNORM);
        oversized_2d_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &oversized_2d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC oversized_3d =
            texture_desc(D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION + 1,
                         D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION + 1,
                         DXGI_FORMAT_R8_UNORM);
        oversized_3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        oversized_3d.DepthOrArraySize =
            D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION + 1;
        oversized_3d_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &oversized_3d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_HEAP_PROPERTIES invalid_properties = default_heap;
        invalid_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        invalid_heap_properties_hr = device->CreateCommittedResource(
            &invalid_properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        invalid_properties = default_heap;
        invalid_properties.CreationNodeMask = 3;
        invalid_properties.VisibleNodeMask = 3;
        invalid_node_mask_hr = device->CreateCommittedResource(
            &invalid_properties, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        invalid_committed_heap_flags_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_DENY_BUFFERS, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        invalid_upload_state_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        invalid_readback_state_hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC invalid_buffer_flags = buffer;
        invalid_buffer_flags.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        invalid_buffer_resource_flags_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &invalid_buffer_flags,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC invalid_texture_flags =
            texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
        invalid_texture_flags.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                      D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        invalid_texture_resource_flags_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &invalid_texture_flags,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC invalid_depth_format =
            texture_desc(8, 8, DXGI_FORMAT_R8_UNORM);
        invalid_depth_format.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        invalid_depth_format_flags_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &invalid_depth_format,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_CLEAR_VALUE invalid_color_clear = {};
        invalid_color_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        invalid_clear_without_flag_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, &invalid_color_clear,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        D3D12_RESOURCE_DESC color_rt_desc =
            texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
        color_rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE invalid_depth_clear = {};
        invalid_depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
        invalid_clear_format_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &color_rt_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &invalid_depth_clear,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        null_heap_properties_hr = device->CreateCommittedResource(
            nullptr, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        invalid_resource = nullptr;
        invalid = texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
        invalid.SampleDesc.Count = 4;
        invalid.MipLevels = 2;
        invalid_msaa_mips_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &invalid,
                                                                D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                IID_PPV_ARGS(&invalid_resource));
        if (invalid_resource)
            invalid_resource->Release();
        D3D12_RESOURCE_ALLOCATION_INFO invalid_msaa_mips_info =
            device->GetResourceAllocationInfo(0, 1, &invalid);
        invalid_msaa_mips_allocation_size = invalid_msaa_mips_info.SizeInBytes;
        invalid_msaa_mips_allocation_alignment = invalid_msaa_mips_info.Alignment;

        // DepthOrArraySize is texel depth for a 3D resource, not an outer
        // array count. Counting it twice would report 256 KiB instead of the
        // expected 64 KiB for this one-mip R8 volume.
        D3D12_RESOURCE_DESC volume_allocation_desc =
            texture_desc(64, 64, DXGI_FORMAT_R8_UNORM);
        volume_allocation_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        volume_allocation_desc.DepthOrArraySize = 4;
        D3D12_RESOURCE_ALLOCATION_INFO volume_info =
            device->GetResourceAllocationInfo(0, 1, &volume_allocation_desc);
        volume_allocation_size = volume_info.SizeInBytes;
        volume_allocation_alignment = volume_info.Alignment;
        D3D12_RESOURCE_ALLOCATION_INFO null_info =
            device->GetResourceAllocationInfo(0, 1, nullptr);
        null_allocation_size = null_info.SizeInBytes;
        null_allocation_alignment = null_info.Alignment;
        D3D12_RESOURCE_DESC allocation_overflow_desc = buffer_desc(UINT64_MAX);
        D3D12_RESOURCE_ALLOCATION_INFO allocation_overflow_info =
            device->GetResourceAllocationInfo(0, 1,
                                               &allocation_overflow_desc);
        allocation_overflow_size = allocation_overflow_info.SizeInBytes;
        allocation_overflow_alignment = allocation_overflow_info.Alignment;
        D3D12_RESOURCE_DESC allocation_batch_descs[2] = {
            buffer_desc(1000), buffer_desc(2000)};
        ID3D12Device4 *allocation_device4 = nullptr;
        if (SUCCEEDED(device->QueryInterface(
                IID_PPV_ARGS(&allocation_device4)))) {
            D3D12_RESOURCE_ALLOCATION_INFO allocation_batch_info =
                allocation_device4->GetResourceAllocationInfo1(
                    0, 2, allocation_batch_descs,
                    allocation_batch_sideband);
            allocation_batch_size = allocation_batch_info.SizeInBytes;
            allocation_batch_alignment = allocation_batch_info.Alignment;
            allocation_batch_ok =
                allocation_batch_size == 2 * 65536 &&
                allocation_batch_alignment == 65536 &&
                allocation_batch_sideband[0].Offset == 0 &&
                allocation_batch_sideband[0].SizeInBytes == 65536 &&
                allocation_batch_sideband[1].Offset == 65536 &&
                allocation_batch_sideband[1].SizeInBytes == 65536;
            allocation_device4->Release();
        }
        ID3D12Device4 *device4 = nullptr;
        null_sideband_query_hr = device->QueryInterface(
            IID_PPV_ARGS(&device4));
        if (device4) {
            D3D12_RESOURCE_ALLOCATION_INFO sideband_info = {};
            D3D12_RESOURCE_ALLOCATION_INFO1 sideband = {};
            sideband_info = device4->GetResourceAllocationInfo1(0, 1, nullptr, &sideband);
            null_sideband_size = sideband_info.SizeInBytes;
            null_sideband_alignment = sideband_info.Alignment;
            null_sideband_offset = sideband.Offset;
            device4->Release();
        }
        D3D12_RESOURCE_DESC invalid_footprint_desc =
            texture_desc(0, 8, DXGI_FORMAT_R8_UNORM);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT invalid_footprint_layout = {};
        UINT invalid_footprint_rows = 0;
        UINT64 invalid_footprint_row_size = 0;
        device->GetCopyableFootprints(
            &invalid_footprint_desc, 0, 1, 0, &invalid_footprint_layout,
            &invalid_footprint_rows, &invalid_footprint_row_size,
            &invalid_footprint_total);
        D3D12_RESOURCE_DESC overflow_desc =
            texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT overflow_footprint = {};
        UINT overflow_rows = 0;
        UINT64 overflow_row_size = 0;
        device->GetCopyableFootprints(
            &overflow_desc, 0, 1,
            UINT64_MAX - 1, &overflow_footprint, &overflow_rows,
            &overflow_row_size, &footprint_overflow_total);
        D3D12_RESOURCE_DESC planar_desc =
            texture_desc(13, 7, DXGI_FORMAT_R24G8_TYPELESS);
        planar_desc.DepthOrArraySize = 2;
        planar_desc.MipLevels = 2;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT planar_layouts[8] = {};
        UINT planar_rows[8] = {};
        UINT64 planar_row_sizes[8] = {};
        device->GetCopyableFootprints(
            &planar_desc, 0, 8, 0, planar_layouts, planar_rows,
            planar_row_sizes, &planar_footprint_total);
        planar_footprint_ok = planar_footprint_total != 0;
        for (UINT subresource = 0; subresource < 8 && planar_footprint_ok;
             ++subresource) {
            const UINT mip = subresource % 2;
            const UINT plane = (subresource / 2) / 2;
            const UINT width = std::max<UINT>(1, 13 >> mip);
            const UINT height = std::max<UINT>(1, 7 >> mip);
            const UINT expected_bytes = plane ? 1 : 4;
            planar_footprint_ok =
                planar_layouts[subresource].Footprint.Format ==
                    (plane ? DXGI_FORMAT_R8_TYPELESS : DXGI_FORMAT_R32_TYPELESS) &&
                planar_layouts[subresource].Footprint.Width == width &&
                planar_layouts[subresource].Footprint.Height == height &&
                planar_layouts[subresource].Footprint.Depth == 1 &&
                planar_layouts[subresource].Footprint.RowPitch >= 256 &&
                planar_rows[subresource] == height &&
                planar_row_sizes[subresource] == UINT64(width) * expected_bytes &&
                (!subresource || planar_layouts[subresource].Offset >
                                     planar_layouts[subresource - 1].Offset);
        }
        D3D12_RESOURCE_DESC nv12_desc =
            texture_desc(13, 8, DXGI_FORMAT_NV12);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT nv12_layouts[2] = {};
        UINT nv12_rows[2] = {};
        UINT64 nv12_row_sizes[2] = {};
        device->GetCopyableFootprints(
            &nv12_desc, 0, 2, 0, nv12_layouts, nv12_rows,
            nv12_row_sizes, &nv12_footprint_total);
        nv12_footprint_ok =
            nv12_layouts[0].Footprint.Format == DXGI_FORMAT_R8_UNORM &&
            nv12_layouts[0].Footprint.Width == 13 &&
            nv12_layouts[0].Footprint.Height == 8 && nv12_rows[0] == 8 &&
            nv12_row_sizes[0] == 13 &&
            nv12_layouts[1].Footprint.Format == DXGI_FORMAT_R8G8_UNORM &&
            nv12_layouts[1].Footprint.Width == 7 &&
            nv12_layouts[1].Footprint.Height == 4 && nv12_rows[1] == 4 &&
            nv12_row_sizes[1] == 14 && nv12_footprint_total == 3072 &&
            nv12_layouts[1].Offset > nv12_layouts[0].Offset;
        D3D12_RESOURCE_DESC invalid_nv12_desc = nv12_desc;
        invalid_nv12_desc.Height = 7;
        ID3D12Resource *invalid_nv12_resource = nullptr;
        invalid_nv12_height_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &invalid_nv12_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&invalid_nv12_resource));
        if (invalid_nv12_resource)
            invalid_nv12_resource->Release();

        const D3D12_RESOURCE_FLAGS tight_flag =
            static_cast<D3D12_RESOURCE_FLAGS>(0x400);
        TightAlignmentFeatureProbe tight_feature = {};
        tight_feature_hr = device->CheckFeatureSupport(
            static_cast<D3D12_FEATURE>(54), &tight_feature, sizeof(tight_feature));
        tight_feature_tier = tight_feature.SupportTier;
        D3D12_RESOURCE_DESC tight_desc = buffer_desc(1000);
        tight_desc.Flags = tight_flag;
        tight_committed_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &tight_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tight_committed));
        D3D12_RESOURCE_ALLOCATION_INFO tight_info =
            device->GetResourceAllocationInfo(0, 1, &tight_desc);
        tight_allocation_info_hr = S_OK;
        tight_allocation_size = tight_info.SizeInBytes;
        tight_allocation_alignment = tight_info.Alignment;
        D3D12_HEAP_DESC tight_heap_desc = {};
        tight_heap_desc.SizeInBytes = 64 * 1024;
        tight_heap_desc.Properties = upload_heap;
        tight_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        tight_heap_hr = device->CreateHeap(&tight_heap_desc, IID_PPV_ARGS(&tight_heap));
        null_heap_desc_ok = tight_heap && tight_heap->GetDesc(nullptr) == nullptr;
        tight_placed_hr = tight_heap
                             ? device->CreatePlacedResource(
                                   tight_heap, 256, &tight_desc,
                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                   IID_PPV_ARGS(&tight_placed))
                             : E_FAIL;
        if (tight_placed) {
            uint8_t *tight_data = nullptr;
            if (SUCCEEDED(tight_placed->Map(
                    0, nullptr, reinterpret_cast<void **>(&tight_data))) &&
                tight_data) {
                for (UINT i = 0; i < 1000; ++i)
                    tight_data[i] = static_cast<uint8_t>((i * 13u + 5u) & 0xffu);
                D3D12_RANGE written = {0, 1000};
                tight_placed->Unmap(0, &written);
                uint8_t tight_readback[1000] = {};
                tight_placed_roundtrip_ok = SUCCEEDED(
                    tight_placed->ReadFromSubresource(tight_readback, 1000, 1000, 0, nullptr));
                for (UINT i = 0; i < 1000 && tight_placed_roundtrip_ok; ++i)
                    tight_placed_roundtrip_ok =
                        tight_readback[i] == static_cast<uint8_t>((i * 13u + 5u) & 0xffu);
            }
        }
        D3D12_RESOURCE_DESC invalid_tight_desc = tight_desc;
        invalid_tight_desc.Alignment = 3;
        ID3D12Resource *invalid_tight_resource = nullptr;
        tight_invalid_alignment_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &invalid_tight_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&invalid_tight_resource));
        if (invalid_tight_resource)
            invalid_tight_resource->Release();
        D3D12_RESOURCE_DESC overaligned_tight_desc = tight_desc;
        overaligned_tight_desc.Alignment = 512;
        ID3D12Resource *overaligned_tight_resource = nullptr;
        tight_overaligned_placed_hr = tight_heap
                                          ? device->CreatePlacedResource(
                                                tight_heap, 256,
                                                &overaligned_tight_desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr,
                                                IID_PPV_ARGS(&overaligned_tight_resource))
                                          : E_FAIL;
        if (overaligned_tight_resource)
            overaligned_tight_resource->Release();
        ID3D12Resource *tight_reserved_resource = nullptr;
        tight_reserved_hr = device->CreateReservedResource(
            &tight_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&tight_reserved_resource));
        if (tight_reserved_resource)
            tight_reserved_resource->Release();

        D3D12_RESOURCE_DESC full_mip_desc =
            texture_desc(32, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
        full_mip_desc.MipLevels = 0;
        ID3D12Resource *full_mip_resource = nullptr;
        full_mip_create_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &full_mip_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&full_mip_resource));
        if (full_mip_resource) {
            D3D12_RESOURCE_DESC created_full_mip = {};
            full_mip_resource->GetDesc(&created_full_mip);
            full_mip_count = created_full_mip.MipLevels;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT full_mip_layouts[6] = {};
            UINT full_mip_rows[6] = {};
            UINT64 full_mip_row_bytes[6] = {};
            UINT64 full_mip_total = 0;
            device->GetCopyableFootprints(
                &full_mip_desc, 0, 6, 0, full_mip_layouts, full_mip_rows,
                full_mip_row_bytes, &full_mip_total);
            full_mip_footprint_ok = full_mip_total != 0;
            for (UINT mip = 0; mip < 6 && full_mip_footprint_ok; ++mip) {
                const UINT width = std::max<UINT>(1, full_mip_desc.Width >> mip);
                const UINT height = std::max<UINT>(1, full_mip_desc.Height >> mip);
                full_mip_footprint_ok =
                    full_mip_layouts[mip].Footprint.Width == width &&
                    full_mip_layouts[mip].Footprint.Height == height &&
                    full_mip_rows[mip] == height &&
                    full_mip_row_bytes[mip] == UINT64(width) * 4 &&
                    (!mip || full_mip_layouts[mip].Offset >
                                  full_mip_layouts[mip - 1].Offset);
            }
            full_mip_resource->Release();
        }

        const D3D12_HEAP_FLAGS not_resident_flag =
            static_cast<D3D12_HEAP_FLAGS>(0x800);
        D3D12_RESOURCE_DESC not_resident_desc = buffer_desc(1024);
        not_resident_committed_hr = device->CreateCommittedResource(
            &upload_heap, not_resident_flag, &not_resident_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&not_resident_committed));
        void *not_resident_data = nullptr;
        not_resident_initial_map_hr = not_resident_committed
                                          ? not_resident_committed->Map(
                                                0, nullptr, &not_resident_data)
                                          : E_FAIL;
        ID3D12Pageable *not_resident_pageable = not_resident_committed;
        not_resident_make_resident_hr =
            not_resident_committed
                ? device->MakeResident(1, &not_resident_pageable)
                : E_FAIL;
        not_resident_data = nullptr;
        not_resident_remade_map_hr = not_resident_committed
                                          ? not_resident_committed->Map(
                                                0, nullptr, &not_resident_data)
                                          : E_FAIL;
        if (SUCCEEDED(not_resident_remade_map_hr) && not_resident_data) {
            auto *bytes = static_cast<uint8_t *>(not_resident_data);
            for (UINT i = 0; i < 1024; ++i)
                bytes[i] = static_cast<uint8_t>((i * 7u + 19u) & 0xffu);
            D3D12_RANGE written = {0, 1024};
            not_resident_committed->Unmap(0, &written);
            not_resident_roundtrip_ok = true;
            uint8_t readback[1024] = {};
            not_resident_roundtrip_ok = SUCCEEDED(
                not_resident_committed->ReadFromSubresource(readback, 1024, 1024, 0, nullptr));
            for (UINT i = 0; i < 1024 && not_resident_roundtrip_ok; ++i)
                not_resident_roundtrip_ok =
                    readback[i] == static_cast<uint8_t>((i * 7u + 19u) & 0xffu);
        }
        D3D12_HEAP_DESC not_resident_heap_desc = {};
        not_resident_heap_desc.SizeInBytes = 64 * 1024;
        not_resident_heap_desc.Properties = upload_heap;
        not_resident_heap_desc.Flags = not_resident_flag | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        not_resident_heap_hr = device->CreateHeap(
            &not_resident_heap_desc, IID_PPV_ARGS(&not_resident_heap));
        not_resident_placed_hr = not_resident_heap
                                     ? device->CreatePlacedResource(
                                           not_resident_heap, 0,
                                           &not_resident_desc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                           nullptr,
                                           IID_PPV_ARGS(&not_resident_placed))
                                     : E_FAIL;
        if (not_resident_placed) {
            void *placed_data = nullptr;
            HRESULT initial_placed_map =
                not_resident_placed->Map(0, nullptr, &placed_data);
            ID3D12Pageable *placed_pageable = not_resident_placed;
            HRESULT placed_make = device->MakeResident(1, &placed_pageable);
            placed_data = nullptr;
            HRESULT remade_placed_map =
                not_resident_placed->Map(0, nullptr, &placed_data);
            not_resident_roundtrip_ok =
                not_resident_roundtrip_ok &&
                initial_placed_map == DXGI_ERROR_INVALID_CALL &&
                SUCCEEDED(placed_make) && SUCCEEDED(remade_placed_map) &&
                placed_data != nullptr;
            if (SUCCEEDED(remade_placed_map))
                not_resident_placed->Unmap(0, nullptr);
        }
        residency_fence_hr = device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&residency_fence));
        ID3D12Device3 *device3_residency = nullptr;
        HRESULT device3_residency_hr = device->QueryInterface(
            IID_PPV_ARGS(&device3_residency));
        if (residency_fence && device3_residency && not_resident_committed) {
            ID3D12Pageable *pageable = not_resident_committed;
            device->Evict(1, &pageable);
            enqueue_make_resident_hr = device3_residency->EnqueueMakeResident(
                D3D12_RESIDENCY_FLAG_NONE, 1, &pageable, residency_fence, 9);
            const D3D12_RESIDENCY_FLAGS invalid_flags =
                static_cast<D3D12_RESIDENCY_FLAGS>(2);
            invalid_enqueue_flags_hr = device3_residency->EnqueueMakeResident(
                invalid_flags, 1, &pageable, residency_fence, 10);
            enqueue_fence_completed = residency_fence->GetCompletedValue();
        } else {
            enqueue_make_resident_hr = device3_residency_hr;
            invalid_enqueue_flags_hr = device3_residency_hr;
        }
        if (device3_residency)
            device3_residency->Release();
    }
    auto same_resource_desc = [](const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
        return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
               a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize && a.MipLevels == b.MipLevels &&
               a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count &&
               a.SampleDesc.Quality == b.SampleDesc.Quality && a.Layout == b.Layout && a.Flags == b.Flags;
    };
    bool resource_shapes_ok = !resource_shapes.empty();
    for (const auto& shape : resource_shapes)
        resource_shapes_ok = resource_shapes_ok && SUCCEEDED(shape.hr) && same_resource_desc(shape.created, shape.requested);
    auto verify_footprints = [&](const D3D12_RESOURCE_DESC& desc, UINT count, UINT expected_width,
                                 UINT expected_height, UINT expected_depth, UINT64 expected_row_bytes,
                                 UINT expected_rows) {
        if (!device || !count || count > 16)
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[16] = {};
        UINT rows[16] = {};
        UINT64 row_bytes[16] = {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, layouts, rows, row_bytes, &total);
        if (!total)
            return false;
        for (UINT i = 0; i < count; ++i) {
            if (layouts[i].Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                layouts[i].Footprint.Width != expected_width ||
                layouts[i].Footprint.Height != expected_height ||
                layouts[i].Footprint.Depth != expected_depth ||
                layouts[i].Footprint.RowPitch < D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ||
                row_bytes[i] != expected_row_bytes || rows[i] != expected_rows)
                return false;
            if (i && layouts[i].Offset <= layouts[i - 1].Offset)
                return false;
        }
        return true;
    };
    D3D12_RESOURCE_DESC footprint_1d = texture_desc(17, 1, DXGI_FORMAT_R8_UNORM);
    footprint_1d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    D3D12_RESOURCE_DESC footprint_array = texture_desc(19, 11, DXGI_FORMAT_R8G8B8A8_UNORM);
    footprint_array.DepthOrArraySize = 4;
    D3D12_RESOURCE_DESC footprint_mips = texture_desc(33, 17, DXGI_FORMAT_R8G8B8A8_UNORM);
    footprint_mips.MipLevels = 6;
    D3D12_RESOURCE_DESC footprint_3d = texture_desc(7, 5, DXGI_FORMAT_R8_UNORM);
    footprint_3d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    footprint_3d.DepthOrArraySize = 4;
    D3D12_RESOURCE_DESC footprint_bc = texture_desc(7, 5, DXGI_FORMAT_BC1_UNORM);
    D3D12_RESOURCE_DESC footprint_b5 = texture_desc(8, 4, DXGI_FORMAT_B5G6R5_UNORM);
    D3D12_RESOURCE_DESC footprint_r9 = texture_desc(8, 4, DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
    auto verify_mip_footprints = [&](const D3D12_RESOURCE_DESC& desc, UINT count) {
        if (!device || !count || count > 16)
            return false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[16] = {};
        UINT rows[16] = {};
        UINT64 row_bytes[16] = {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, layouts, rows, row_bytes, &total);
        if (!total)
            return false;
        for (UINT mip = 0; mip < count; ++mip) {
            const UINT width = std::max<UINT>(1, desc.Width >> mip);
            const UINT height = std::max<UINT>(1, desc.Height >> mip);
            if (layouts[mip].Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT ||
                layouts[mip].Footprint.Width != width || layouts[mip].Footprint.Height != height ||
                layouts[mip].Footprint.Depth != 1 || row_bytes[mip] != UINT64(width) * 4 ||
                rows[mip] != height || (mip && layouts[mip].Offset <= layouts[mip - 1].Offset))
                return false;
        }
        return true;
    };
    const bool footprint_matrix_ok =
        verify_footprints(footprint_1d, 1, 17, 1, 1, 17, 1) &&
        verify_footprints(footprint_array, 4, 19, 11, 1, 19 * 4, 11) &&
        verify_mip_footprints(footprint_mips, 6) &&
        verify_footprints(footprint_3d, 1, 7, 5, 4, 7, 5) &&
        verify_footprints(footprint_bc, 1, 7, 5, 1, 16, 2) &&
        verify_footprints(footprint_b5, 1, 8, 4, 1, 16, 4) &&
        verify_footprints(footprint_r9, 1, 8, 4, 1, 32, 4);
    resource_shapes_ok = resource_shapes_ok && footprint_matrix_ok;
    HRESULT upload_buffer_hr = device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&upload_buffer))
                                      : E_FAIL;
    HRESULT default_buffer_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&default_buffer))
               : E_FAIL;
    HRESULT readback_buffer_hr = device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                          IID_PPV_ARGS(&readback_buffer))
                                        : E_FAIL;
    if (device) {
        D3D12_HEAP_DESC address_heap_desc = {};
        address_heap_desc.SizeInBytes = 64 * 1024;
        address_heap_desc.Properties = upload_heap;
        address_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        address_heap_hr = device->CreateHeap(&address_heap_desc, IID_PPV_ARGS(&address_heap));
        D3D12_HEAP_DESC invalid_heap_desc = address_heap_desc;
        invalid_heap_desc.Alignment = 123;
        ID3D12Heap* invalid_heap = nullptr;
        invalid_heap_alignment_hr = device->CreateHeap(&invalid_heap_desc, IID_PPV_ARGS(&invalid_heap));
        if (invalid_heap)
            invalid_heap->Release();
        invalid_heap_desc = address_heap_desc;
        invalid_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | D3D12_HEAP_FLAG_DENY_BUFFERS;
        invalid_heap = nullptr;
        invalid_heap_flags_hr = device->CreateHeap(&invalid_heap_desc, IID_PPV_ARGS(&invalid_heap));
        if (invalid_heap)
            invalid_heap->Release();
        invalid_heap_desc = address_heap_desc;
        invalid_heap_desc.SizeInBytes = UINT64_MAX;
        invalid_heap_size_hr = device->CreateHeap(&invalid_heap_desc, IID_PPV_ARGS(&invalid_heap));
        if (invalid_heap)
            invalid_heap->Release();
        D3D12_RESOURCE_DESC address_desc = buffer_desc(4096);
        address_resource_hr = address_heap
                                  ? device->CreatePlacedResource(address_heap, 0, &address_desc,
                                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                 IID_PPV_ARGS(&address_resource))
                                  : E_FAIL;
        address_alias_resource_hr = address_heap
                                        ? device->CreatePlacedResource(address_heap, 0, &address_desc,
                                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                       IID_PPV_ARGS(&address_alias_resource))
                                        : E_FAIL;
        if (address_resource && address_alias_resource && SUCCEEDED(address_resource_hr) &&
            SUCCEEDED(address_alias_resource_hr)) {
            void* first_alias = nullptr;
            if (SUCCEEDED(address_resource->Map(0, nullptr, &first_alias)) && first_alias) {
                std::memset(first_alias, 0x5a, 4096);
                address_resource->Unmap(0, nullptr);
                void* second_alias = nullptr;
                if (SUCCEEDED(address_alias_resource->Map(0, nullptr, &second_alias)) && second_alias) {
                    heap_aliasing_ok = static_cast<uint8_t*>(second_alias)[0] == 0x5a &&
                                       static_cast<uint8_t*>(second_alias)[4095] == 0x5a;
                    std::memset(second_alias, 0xa6, 4096);
                    address_alias_resource->Unmap(0, nullptr);
                    void* first_again = nullptr;
                    if (SUCCEEDED(address_resource->Map(0, nullptr, &first_again)) && first_again) {
                        heap_aliasing_ok = heap_aliasing_ok &&
                                           static_cast<uint8_t*>(first_again)[0] == 0xa6 &&
                                           static_cast<uint8_t*>(first_again)[4095] == 0xa6;
                        address_resource->Unmap(0, nullptr);
                    } else {
                        heap_aliasing_ok = false;
                    }
                }
            }
        }
        ID3D12Resource* misaligned_resource = nullptr;
        misaligned_placement_hr = address_heap
                                      ? device->CreatePlacedResource(address_heap, 1, &address_desc,
                                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                     IID_PPV_ARGS(&misaligned_resource))
                                      : E_FAIL;
        if (misaligned_resource)
            misaligned_resource->Release();
        void* address = nullptr;
        if (address_resource && SUCCEEDED(address_resource->Map(0, nullptr, &address))) {
            ID3D12Device3* device3 = nullptr;
            HRESULT device3_hr = device->QueryInterface(IID_PPV_ARGS(&device3));
            ID3D12Heap* reopened_heap = nullptr;
            address_open_hr = device3
                                  ? device3->OpenExistingHeapFromAddress(address, IID_PPV_ARGS(&reopened_heap))
                                  : device3_hr;
            address_heap_open_ok = SUCCEEDED(address_open_hr) && reopened_heap == address_heap;
            if (reopened_heap)
                reopened_heap->Release();
            if (device3)
                device3->Release();
            std::memset(address, 0x7c, 4096);
            address_resource->Unmap(0, nullptr);
        }

        D3D12_HEAP_DESC shared_heap_desc = {};
        shared_heap_desc.SizeInBytes = 64 * 1024;
        shared_heap_desc.Properties = upload_heap;
        shared_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        HRESULT shared_heap_source_hr =
            device->CreateHeap(&shared_heap_desc, IID_PPV_ARGS(&shared_heap_source));
        if (SUCCEEDED(shared_heap_source_hr) && shared_heap_source) {
            shared_heap_create_hr = device->CreateSharedHandle(
                shared_heap_source, nullptr, GENERIC_ALL,
                L"metalsharp-probe-upload-heap", &shared_heap_handle);
            if (SUCCEEDED(shared_heap_create_hr))
                shared_heap_open_hr = device->OpenSharedHandle(
                    shared_heap_handle, IID_PPV_ARGS(&shared_heap_open));
            ID3D12Device3* device3 = nullptr;
            HRESULT device3_hr = device->QueryInterface(IID_PPV_ARGS(&device3));
            shared_heap_file_open_hr =
                device3 && shared_heap_handle
                    ? device3->OpenExistingHeapFromFileMapping(
                          shared_heap_handle, IID_PPV_ARGS(&shared_heap_file_open))
                    : device3_hr;
            if (device3)
                device3->Release();
            shared_heap_roundtrip_ok =
                SUCCEEDED(shared_heap_create_hr) && SUCCEEDED(shared_heap_open_hr) &&
                SUCCEEDED(shared_heap_file_open_hr) && shared_heap_open &&
                shared_heap_file_open;
        }
        if (device) {
            D3D12_HEAP_DESC unnamed_shared_heap_desc = {};
            unnamed_shared_heap_desc.SizeInBytes = 64 * 1024;
            unnamed_shared_heap_desc.Properties = upload_heap;
            unnamed_shared_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            unnamed_shared_heap_create_hr = device->CreateHeap(
                &unnamed_shared_heap_desc,
                IID_PPV_ARGS(&unnamed_shared_heap_source));
            unnamed_shared_heap_handle_hr =
                unnamed_shared_heap_source
                    ? device->CreateSharedHandle(
                          unnamed_shared_heap_source, nullptr, GENERIC_ALL,
                          nullptr, &unnamed_shared_heap_handle)
                    : E_FAIL;
            unnamed_shared_heap_open_hr =
                SUCCEEDED(unnamed_shared_heap_handle_hr)
                    ? device->OpenSharedHandle(
                          unnamed_shared_heap_handle,
                          IID_PPV_ARGS(&unnamed_shared_heap_open))
                    : E_FAIL;
            D3D12_HEAP_DESC opened_desc = {};
            if (unnamed_shared_heap_open)
                unnamed_shared_heap_open->GetDesc(&opened_desc);
            unnamed_shared_heap_roundtrip_ok =
                SUCCEEDED(unnamed_shared_heap_create_hr) &&
                SUCCEEDED(unnamed_shared_heap_handle_hr) &&
                SUCCEEDED(unnamed_shared_heap_open_hr) &&
                unnamed_shared_heap_open &&
                opened_desc.SizeInBytes == unnamed_shared_heap_desc.SizeInBytes &&
                opened_desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD;
        }
        D3D12_DESCRIPTOR_HEAP_DESC residency_descriptor_desc = {};
        residency_descriptor_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        residency_descriptor_desc.NumDescriptors = 1;
        residency_descriptor_heap_create_hr = device->CreateDescriptorHeap(
            &residency_descriptor_desc, IID_PPV_ARGS(&residency_descriptor_heap));
        D3D12_QUERY_HEAP_DESC residency_query_desc = {};
        residency_query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        residency_query_desc.Count = 1;
        residency_query_heap_create_hr = device->CreateQueryHeap(
            &residency_query_desc, IID_PPV_ARGS(&residency_query_heap));
        if (residency_descriptor_heap) {
            ID3D12Pageable *pageable = residency_descriptor_heap;
            residency_descriptor_heap_evict_hr = device->Evict(1, &pageable);
            residency_descriptor_heap_make_hr = device->MakeResident(1, &pageable);
        }
        if (residency_query_heap) {
            ID3D12Pageable *pageable = residency_query_heap;
            residency_query_heap_evict_hr = device->Evict(1, &pageable);
            residency_query_heap_make_hr = device->MakeResident(1, &pageable);
        }
        if (residency_descriptor_heap && residency_query_heap) {
            ID3D12Pageable *pageables[] = {residency_descriptor_heap,
                                           residency_query_heap};
            D3D12_RESIDENCY_PRIORITY priorities[] = {
                D3D12_RESIDENCY_PRIORITY_HIGH, D3D12_RESIDENCY_PRIORITY_LOW};
            ID3D12Device1 *device1 = nullptr;
            if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device1))) ) {
                residency_pageable_priority_hr = device1->SetResidencyPriority(
                    2, pageables, priorities);
                device1->Release();
            }
        }
        if (address_heap && address_resource) {
            ID3D12Pageable *heap_pageable = address_heap;
            address_heap_evict_hr = device->Evict(1, &heap_pageable);
            void *evicted_address = nullptr;
            address_heap_map_hr = address_resource->Map(
                0, nullptr, &evicted_address);
            address_heap_make_resident_hr =
                device->MakeResident(1, &heap_pageable);
            void *remapped_address = nullptr;
            address_heap_remap_hr = address_resource->Map(
                0, nullptr, &remapped_address);
            heap_residency_ok = SUCCEEDED(address_heap_evict_hr) &&
                                address_heap_map_hr == DXGI_ERROR_INVALID_CALL &&
                                SUCCEEDED(address_heap_make_resident_hr) &&
                                SUCCEEDED(address_heap_remap_hr) &&
                                remapped_address != nullptr;
            if (SUCCEEDED(address_heap_remap_hr))
                address_resource->Unmap(0, nullptr);
        }
    }

    if (device && upload_buffer) {
        ID3D12Pageable* pageable = upload_buffer;
        residency_make_hr = device->MakeResident(1, &pageable);
        D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_HIGH;
        ID3D12Device1* residency_device1 = nullptr;
        HRESULT residency_device1_hr = device->QueryInterface(IID_PPV_ARGS(&residency_device1));
        residency_priority_hr = residency_device1
                                    ? residency_device1->SetResidencyPriority(1, &pageable, &priority)
                                    : residency_device1_hr;
        if (residency_device1)
            residency_device1->Release();
        residency_evict_hr = device->Evict(1, &pageable);
        void *first_evict_map = nullptr;
        residency_first_evict_map_hr =
            upload_buffer->Map(0, nullptr, &first_evict_map);
        if (SUCCEEDED(residency_first_evict_map_hr))
            upload_buffer->Unmap(0, nullptr);
        residency_second_evict_hr = device->Evict(1, &pageable);
        void* evicted_map = nullptr;
        residency_evicted_map_hr = upload_buffer->Map(0, nullptr, &evicted_map);
        residency_remake_hr = device->MakeResident(1, &pageable);
        void* remade_map = nullptr;
        residency_remade_map_hr = upload_buffer->Map(0, nullptr, &remade_map);
        if (SUCCEEDED(residency_remade_map_hr))
            upload_buffer->Unmap(0, nullptr);
        D3D12_RANGE invalid_map_range = {0, buffer_bytes + 1};
        void *invalid_map_data = nullptr;
        invalid_map_range_hr = upload_buffer->Map(
            0, &invalid_map_range, &invalid_map_data);
        invalid_map_range_ok = invalid_map_range_hr == E_INVALIDARG &&
                              invalid_map_data == nullptr;
        residency_state_ok = SUCCEEDED(residency_make_hr) &&
                             SUCCEEDED(residency_priority_hr) &&
                             SUCCEEDED(residency_evict_hr) &&
                             SUCCEEDED(residency_first_evict_map_hr) &&
                             SUCCEEDED(residency_second_evict_hr) &&
                             residency_evicted_map_hr == DXGI_ERROR_INVALID_CALL &&
                             SUCCEEDED(residency_remake_hr) &&
                             SUCCEEDED(residency_remade_map_hr) && remade_map;
        dxgi_device_qi_hr = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        offered_dxgi_resource_qi_hr =
            upload_buffer ? upload_buffer->QueryInterface(
                                IID_PPV_ARGS(&offered_dxgi_resource))
                          : E_FAIL;
        if (dxgi_device && offered_dxgi_resource) {
            IDXGIResource *offered[] = {offered_dxgi_resource};
            offered_resource_priority_set_hr =
                offered_dxgi_resource->SetEvictionPriority(
                    DXGI_RESOURCE_PRIORITY_HIGH);
            offered_resource_priority_get_hr =
                offered_dxgi_resource->GetEvictionPriority(
                    &offered_resource_priority);
            offer_resources_hr = dxgi_device->OfferResources(
                1, offered, DXGI_OFFER_RESOURCE_PRIORITY_LOW);
            dxgi_device->Trim();
            trim_residency_hr = S_OK;
            IUnknown *offered_unknown = offered_dxgi_resource;
            DXGI_RESIDENCY trim_residency = DXGI_RESIDENCY_FULLY_RESIDENT;
            query_trim_residency_hr = dxgi_device->QueryResourceResidency(
                &offered_unknown, &trim_residency, 1);
            reclaim_resources_hr = dxgi_device->ReclaimResources(
                1, offered, &offered_resource_discarded);
            DXGI_RESIDENCY reclaim_residency = DXGI_RESIDENCY_EVICTED_TO_DISK;
            query_reclaim_residency_hr = dxgi_device->QueryResourceResidency(
                &offered_unknown, &reclaim_residency, 1);
            dxgi_offer_reclaim_ok =
                SUCCEEDED(offered_resource_priority_set_hr) &&
                SUCCEEDED(offered_resource_priority_get_hr) &&
                offered_resource_priority == DXGI_RESOURCE_PRIORITY_HIGH &&
                SUCCEEDED(offer_resources_hr) && trim_residency_hr == S_OK &&
                SUCCEEDED(query_trim_residency_hr) &&
                trim_residency == DXGI_RESIDENCY_EVICTED_TO_DISK &&
                SUCCEEDED(reclaim_resources_hr) &&
                offered_resource_discarded == TRUE &&
                SUCCEEDED(query_reclaim_residency_hr) &&
                reclaim_residency == DXGI_RESIDENCY_FULLY_RESIDENT;
        }
    }

    if (device && default_buffer) {
        D3D12_RESOURCE_DESC unnamed_shared_desc = buffer_desc(256);
        unnamed_shared_source_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &unnamed_shared_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&unnamed_shared_source));
        if (unnamed_shared_source) {
            void *unnamed_source_data = nullptr;
            if (SUCCEEDED(unnamed_shared_source->Map(
                    0, nullptr, &unnamed_source_data)) &&
                unnamed_source_data) {
                for (UINT i = 0; i < 256; ++i)
                    static_cast<uint8_t *>(unnamed_source_data)[i] =
                        static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
                unnamed_shared_source->Unmap(0, nullptr);
                unnamed_shared_create_hr = device->CreateSharedHandle(
                    unnamed_shared_source, nullptr, GENERIC_ALL, nullptr,
                    &unnamed_shared_handle);
            }
            if (SUCCEEDED(unnamed_shared_create_hr))
                unnamed_shared_open_hr = device->OpenSharedHandle(
                    unnamed_shared_handle, IID_PPV_ARGS(&unnamed_shared_open));
            if (unnamed_shared_open) {
                void *unnamed_open_data = nullptr;
                unnamed_shared_map_hr = unnamed_shared_open->Map(
                    0, nullptr, &unnamed_open_data);
                unnamed_shared_roundtrip_ok =
                    SUCCEEDED(unnamed_shared_map_hr) && unnamed_open_data;
                for (UINT i = 0; i < 256 && unnamed_shared_roundtrip_ok;
                     ++i)
                    unnamed_shared_roundtrip_ok =
                        static_cast<uint8_t *>(unnamed_open_data)[i] ==
                        static_cast<uint8_t>((i * 37u + 11u) & 0xffu);
                if (SUCCEEDED(unnamed_shared_map_hr))
                    unnamed_shared_open->Unmap(0, nullptr);
            }
        }
        uint8_t cpu_io_scratch[64] = {};
        const uint32_t cpu_io_value = 0x12345678u;
        default_write_subresource_hr =
            default_buffer->WriteToSubresource(0, nullptr, &cpu_io_value, sizeof(cpu_io_value), sizeof(cpu_io_value));
        default_read_subresource_hr = default_buffer->ReadFromSubresource(cpu_io_scratch, sizeof(cpu_io_scratch),
                                                                          sizeof(cpu_io_scratch), 0, nullptr);
        default_cpu_io_verified = SUCCEEDED(default_write_subresource_hr) &&
                                  SUCCEEDED(default_read_subresource_hr) &&
                                  std::memcmp(cpu_io_scratch, &cpu_io_value, sizeof(cpu_io_value)) == 0;
        shared_create_hr = device->CreateSharedHandle(default_buffer, nullptr, GENERIC_ALL, L"metalsharp-probe-buffer",
                                                      &shared_handle);
        if (SUCCEEDED(shared_create_hr))
            shared_open_hr = device->OpenSharedHandle(shared_handle, IID_PPV_ARGS(&shared_open_buffer));
        shared_open_named_hr =
            device->OpenSharedHandleByName(L"metalsharp-probe-buffer", GENERIC_ALL, &shared_named_handle);
        if (SUCCEEDED(shared_open_named_hr))
            shared_open_named_hr =
                device->OpenSharedHandle(shared_named_handle, IID_PPV_ARGS(&shared_named_open_buffer));
        shared_independent_object_ok =
            shared_open_buffer && shared_named_open_buffer &&
            shared_open_buffer != default_buffer &&
            shared_named_open_buffer != default_buffer &&
            shared_open_buffer != shared_named_open_buffer;
        void* shared_parent_data = nullptr;
        if (SUCCEEDED(shared_create_hr) && SUCCEEDED(default_buffer->Map(0, nullptr, &shared_parent_data)) &&
            shared_parent_data) {
            const uint32_t shared_parent_value = 0x1234abcdu;
            std::memcpy(shared_parent_data, &shared_parent_value, sizeof(shared_parent_value));
            default_buffer->Unmap(0, nullptr);
            shared_heap_cross_process_ok = launch_shared_child();
            cross_process_shared_ok = shared_heap_cross_process_ok;
            shared_parent_data = nullptr;
            if (cross_process_shared_ok && SUCCEEDED(default_buffer->Map(0, nullptr, &shared_parent_data)) &&
                shared_parent_data) {
                uint32_t shared_child_value = 0;
                std::memcpy(&shared_child_value, shared_parent_data, sizeof(shared_child_value));
                cross_process_shared_ok = shared_child_value == 0xdecafbad;
                default_buffer->Unmap(0, nullptr);
            } else {
                cross_process_shared_ok = false;
            }
        }
        HANDLE unknown_shared_handle = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (unknown_shared_handle) {
            shared_unknown_hr = device->OpenSharedHandle(unknown_shared_handle, IID_PPV_ARGS(&unknown_open_buffer));
            CloseHandle(unknown_shared_handle);
        }
        HANDLE missing_name_handle = nullptr;
        shared_missing_name_hr =
            device->OpenSharedHandleByName(L"metalsharp-probe-missing", GENERIC_ALL, &missing_name_handle);
        if (missing_name_handle)
            CloseHandle(missing_name_handle);
        HANDLE invalid_access_handle = nullptr;
        shared_invalid_create_access_hr = device->CreateSharedHandle(
            default_buffer, nullptr, 0, L"metalsharp-probe-invalid-access",
            &invalid_access_handle);
        if (invalid_access_handle)
            CloseHandle(invalid_access_handle);
        shared_invalid_open_access_hr = device->OpenSharedHandleByName(
            L"metalsharp-probe-buffer", 0, &invalid_access_handle);
        if (invalid_access_handle)
            CloseHandle(invalid_access_handle);
    }

    uint8_t* upload_ptr = nullptr;
    HRESULT map_upload_hr =
        upload_buffer ? upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr)) : E_FAIL;
    if (SUCCEEDED(map_upload_hr) && upload_ptr) {
        for (UINT64 i = 0; i < buffer_bytes; ++i)
            upload_ptr[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
        const uint32_t atomic_value = 0xa5c0ffeeu;
        const uint64_t atomic64_value = 0x1122334455667788ull;
        std::memcpy(upload_ptr + 128, &atomic_value, sizeof(atomic_value));
        std::memcpy(upload_ptr + 256, &atomic64_value, sizeof(atomic64_value));
        upload_buffer->Unmap(0, nullptr);
    }

    if (list && upload_buffer && default_buffer && readback_buffer) {
        list->CopyBufferRegion(default_buffer, 0, upload_buffer, 0, buffer_bytes);
        if (list1) {
            list1->AtomicCopyBufferUINT(default_buffer, 128, upload_buffer, 128, 0, nullptr, nullptr);
            list1->AtomicCopyBufferUINT64(default_buffer, 256, upload_buffer, 256, 0, nullptr, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier =
            transition_barrier(default_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback_buffer, 0, default_buffer, 0, buffer_bytes);
        if (address_resource)
            list->DiscardResource(address_resource, nullptr);
        default_buffer_desc = default_buffer->GetDesc();
        upload_gpu_va = upload_buffer->GetGPUVirtualAddress();
        default_gpu_va = default_buffer->GetGPUVirtualAddress();
        // The command list owns the recorded resources. Releasing the caller's
        // references here makes the first copy a real lifetime test rather
        // than relying on cleanup after execution.
        upload_buffer->Release();
        upload_buffer = nullptr;
        default_buffer->Release();
        default_buffer = nullptr;
        command_resource_lifetime_ok = true;
    }

    ID3D12Resource* texture = nullptr;
    ID3D12Resource* texture_upload = nullptr;
    ID3D12Resource* texture_readback = nullptr;
    ID3D12Resource* direct_io_texture = nullptr;
    ID3D12Resource* direct_io_volume = nullptr;
    ID3D12Resource* direct_io_d32s8 = nullptr;
    ID3D12Resource* direct_io_nv12 = nullptr;
    ID3D12Resource* direct_io_p010 = nullptr;
    ID3D12Resource* direct_io_nv12_upload = nullptr;
    ID3D12Resource* direct_io_nv12_readback = nullptr;
    ID3D12Resource* direct_io_nv12_copy_destination = nullptr;
    ID3D12Resource* unsupported_texture = nullptr;
    ID3D12Heap* sparse_heap = nullptr;
    ID3D12Heap* reuse_heap = nullptr;
    ID3D12Heap* array_alias_heap = nullptr;
    ID3D12Heap* volume_heap = nullptr;
    ID3D12Heap* copy_mapping_heap = nullptr;
    ID3D12Resource* reserved_texture = nullptr;
    ID3D12Resource* reserved_1d_texture = nullptr;
    ID3D12Resource* reserved_1d_alias_texture = nullptr;
    ID3D12Resource* reserved_1d_readback = nullptr;
    ID3D12Resource* placement_alias_texture = nullptr;
    ID3D12Resource* array_alias_texture = nullptr;
    ID3D12Resource* array_alias_readback = nullptr;
    ID3D12Resource* volume_texture = nullptr;
    ID3D12Resource* volume_alias_texture = nullptr;
    ID3D12Resource* sparse_upload = nullptr;
    ID3D12Resource* sparse_readback = nullptr;
    ID3D12Resource* sparse_unmapped_readback = nullptr;
    ID3D12Resource* volume_readback = nullptr;
    ID3D12Resource* volume_alias_readback = nullptr;
    ID3D12Resource* reserved_buffer = nullptr;
    ID3D12Resource* reuse_buffer = nullptr;
    ID3D12Resource* reserved_buffer_readback = nullptr;
    ID3D12Resource* reserved_buffer_reuse_readback = nullptr;
    ID3D12Resource* reserved_buffer_unmapped_readback = nullptr;
    ID3D12Resource* placement_alias_readback = nullptr;
    ID3D12Resource* mapping_copy_source = nullptr;
    ID3D12Resource* mapping_copy_destination = nullptr;
    ID3D12Resource* mapping_copy_readback = nullptr;
    ID3D12Resource* mipped_reserved_texture = nullptr;
    ID3D12Resource* mipped_reserved_readback = nullptr;
    ID3D12Resource* packed_tail_reserved_texture = nullptr;
    ID3D12Resource* packed_tail_readback = nullptr;
    ID3D12Resource* r8_reserved_texture = nullptr;
    ID3D12Resource* r8_reserved_readback = nullptr;
    ID3D12Heap* r8_mipped_heap = nullptr;
    ID3D12Resource* r8_mipped_texture = nullptr;
    ID3D12Resource* r8_mipped_readback = nullptr;
    ID3D12Resource* r8_partial_texture = nullptr;
    ID3D12Resource* r8_partial_upload = nullptr;
    ID3D12Resource* r8_partial_readback = nullptr;
    HRESULT sparse_heap_hr = E_FAIL;
    HRESULT reuse_heap_hr = E_FAIL;
    HRESULT reuse_buffer_hr = E_FAIL;
    HRESULT array_alias_heap_hr = E_FAIL;
    HRESULT volume_heap_hr = E_FAIL;
    HRESULT copy_mapping_heap_hr = E_FAIL;
    HRESULT reserved_texture_hr = E_FAIL;
    HRESULT reserved_1d_texture_hr = E_FAIL;
    HRESULT reserved_1d_alias_texture_hr = E_FAIL;
    HRESULT reserved_1d_tiling_hr = E_FAIL;
    HRESULT reserved_1d_readback_hr = E_FAIL;
    HRESULT reserved_1d_readback_map_hr = E_FAIL;
    UINT reserved_1d_total_tiles = 0;
    D3D12_TILE_SHAPE reserved_1d_tile_shape = {};
    D3D12_SUBRESOURCE_TILING reserved_1d_tilings[2] = {};
    UINT reserved_1d_tiling_count = 2;
    bool reserved_1d_copy_ok = false;
    bool reserved_1d_mapping_copy_ok = false;
    HRESULT placement_alias_texture_hr = E_FAIL;
    HRESULT array_alias_texture_hr = E_FAIL;
    HRESULT array_alias_readback_hr = E_FAIL;
    HRESULT array_mapping_signal_hr = E_FAIL;
    HRESULT array_mapping_wait_hr = E_FAIL;
    HRESULT array_mapping_close_hr = E_FAIL;
    HRESULT array_mapping_execute_hr = E_FAIL;
    HRESULT array_mapping_signal_fence_hr = E_FAIL;
    HRESULT array_mapping_wait_fence_hr = E_FAIL;
    HRESULT array_upload_map_hr = E_FAIL;
    HRESULT array_upload_restore_hr = E_FAIL;
    HRESULT array_alias_readback_map_hr = E_FAIL;
    UINT8 array_alias_first = 0;
    UINT8 array_alias_second = 0;
    UINT64 array_alias_mismatch = UINT64_MAX;
    UINT8 array_alias_mismatch_actual = 0;
    UINT8 array_alias_mismatch_expected = 0;
    bool array_alias_copy_ok = false;
    HRESULT volume_texture_hr = E_FAIL;
    HRESULT volume_alias_texture_hr = E_FAIL;
    HRESULT sparse_upload_hr = E_FAIL;
    HRESULT sparse_readback_hr = E_FAIL;
    HRESULT sparse_unmapped_readback_hr = E_FAIL;
    HRESULT sparse_unmap_close_hr = E_FAIL;
    HRESULT sparse_unmap_execute_hr = E_FAIL;
    HRESULT sparse_unmap_signal_hr = E_FAIL;
    HRESULT sparse_unmap_wait_hr = E_FAIL;
    HRESULT sparse_tiling_hr = E_FAIL;
    HRESULT reserved_buffer_hr = E_FAIL;
    HRESULT reserved_buffer_tiling_hr = E_FAIL;
    HRESULT reserved_buffer_readback_hr = E_FAIL;
    HRESULT reserved_buffer_reuse_readback_hr = E_FAIL;
    HRESULT reserved_buffer_reuse_readback_map_hr = E_FAIL;
    HRESULT reserved_buffer_unmapped_readback_hr = E_FAIL;
    HRESULT placement_alias_readback_hr = E_FAIL;
    HRESULT volume_readback_hr = E_FAIL;
    HRESULT volume_alias_readback_hr = E_FAIL;
    HRESULT volume_tiling_hr = E_FAIL;
    HRESULT volume_alias_tiling_hr = E_FAIL;
    HRESULT volume_readback_map_hr = E_FAIL;
    HRESULT volume_alias_readback_map_hr = E_FAIL;
    UINT volume_total_tiles = 0;
    UINT volume_alias_total_tiles = 0;
    UINT volume_tiling_count = 1;
    UINT volume_alias_tiling_count = 1;
    D3D12_TILE_SHAPE volume_tile_shape = {};
    D3D12_TILE_SHAPE volume_alias_tile_shape = {};
    D3D12_SUBRESOURCE_TILING volume_tiling = {};
    D3D12_SUBRESOURCE_TILING volume_alias_tiling = {};
    bool volume_copy_ok = false;
    bool volume_alias_copy_ok = false;
    bool volume_physical_page_ownership_ok = false;
    HRESULT mapping_copy_source_hr = E_FAIL;
    HRESULT mapping_copy_destination_hr = E_FAIL;
    HRESULT mapping_copy_readback_hr = E_FAIL;
    HRESULT mapping_copy_readback_map_hr = E_FAIL;
    UINT reserved_buffer_total_tiles = 0;
    D3D12_TILE_SHAPE reserved_buffer_tile_shape = {};
    D3D12_SUBRESOURCE_TILING reserved_buffer_tiling = {};
    UINT reserved_buffer_tiling_count = 1;
    bool reserved_buffer_copy_ok = false;
    bool reserved_buffer_reuse_skip_ok = false;
    UINT8 reserved_buffer_reuse_first = 0;
    UINT8 reserved_buffer_reuse_last = 0;
    UINT64 reserved_buffer_reuse_mismatch = UINT64_MAX;
    UINT8 reserved_buffer_reuse_actual = 0;
    UINT8 reserved_buffer_reuse_expected = 0;
    bool reserved_buffer_unmapped_zero_ok = false;
    HRESULT reuse_skip_close_hr = E_FAIL;
    HRESULT reuse_skip_execute_hr = E_FAIL;
    HRESULT reuse_skip_signal_hr = E_FAIL;
    HRESULT reuse_skip_wait_hr = E_FAIL;
    HRESULT reuse_mapping_signal_hr = E_FAIL;
    HRESULT reuse_mapping_wait_hr = E_FAIL;
    bool placement_alias_copy_ok = false;
    HRESULT placement_alias_readback_map_hr = E_FAIL;
    uint8_t placement_alias_first = 0;
    uint64_t placement_alias_first_mismatch = UINT64_MAX;
    bool mapping_copy_ok = false;
    HRESULT mipped_reserved_texture_hr = E_FAIL;
    HRESULT mipped_reserved_tiling_hr = E_FAIL;
    HRESULT packed_tail_reserved_texture_hr = E_FAIL;
    HRESULT packed_tail_reserved_tiling_hr = E_FAIL;
    HRESULT packed_tail_readback_hr = E_FAIL;
    HRESULT packed_tail_readback_map_hr = E_FAIL;
    UINT packed_tail_total_tiles = 0;
    UINT packed_tail_tiling_count = 4;
    D3D12_PACKED_MIP_INFO packed_tail_info = {};
    D3D12_TILE_SHAPE packed_tail_shape = {};
    D3D12_SUBRESOURCE_TILING packed_tail_tilings[4] = {};
    UINT packed_tail_partial_count = 4;
    D3D12_SUBRESOURCE_TILING packed_tail_partial_tilings[2] = {};
    bool packed_tail_partial_query_ok = false;
    HRESULT r8_reserved_texture_hr = E_FAIL;
    HRESULT r8_reserved_tiling_hr = E_FAIL;
    HRESULT r8_reserved_readback_hr = E_FAIL;
    HRESULT r8_reserved_readback_map_hr = E_FAIL;
    HRESULT r8_mipped_heap_hr = E_FAIL;
    HRESULT r8_mipped_texture_hr = E_FAIL;
    HRESULT r8_mipped_tiling_hr = E_FAIL;
    HRESULT r8_mipped_readback_hr = E_FAIL;
    HRESULT r8_mipped_readback_map_hr = E_FAIL;
    HRESULT r8_partial_texture_hr = E_FAIL;
    HRESULT r8_partial_upload_hr = E_FAIL;
    HRESULT r8_partial_readback_hr = E_FAIL;
    HRESULT r8_partial_upload_map_hr = E_FAIL;
    HRESULT r8_partial_readback_map_hr = E_FAIL;
    HRESULT r8_partial_tiling_hr = E_FAIL;
    HRESULT mipped_reserved_readback_hr = E_FAIL;
    HRESULT mipped_reserved_readback_map_hr = E_FAIL;
    UINT mipped_reserved_total_tiles = 0;
    UINT mipped_reserved_tiling_count = 2;
    D3D12_TILE_SHAPE mipped_reserved_tile_shape = {};
    D3D12_SUBRESOURCE_TILING mipped_reserved_tilings[2] = {};
    bool mipped_reserved_copy_ok = false;
    bool packed_tail_footprint_ok = false;
    bool packed_tail_copy_ok = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT packed_tail_footprint = {};
    UINT packed_tail_rows = 0;
    UINT64 packed_tail_row_bytes = 0;
    UINT64 packed_tail_copy_bytes = 0;
    UINT r8_reserved_total_tiles = 0;
    UINT r8_reserved_tiling_count = 1;
    D3D12_TILE_SHAPE r8_reserved_tile_shape = {};
    D3D12_SUBRESOURCE_TILING r8_reserved_tiling = {};
    bool r8_reserved_copy_ok = false;
    UINT r8_mipped_total_tiles = 0;
    UINT r8_mipped_tiling_count = 2;
    D3D12_TILE_SHAPE r8_mipped_tile_shape = {};
    D3D12_SUBRESOURCE_TILING r8_mipped_tilings[2] = {};
    bool r8_mipped_copy_ok = false;
    bool r8_partial_copy_ok = false;
    UINT r8_partial_total_tiles = 0;
    UINT r8_partial_tiling_count = 2;
    D3D12_PACKED_MIP_INFO r8_partial_packed_mips = {};
    D3D12_TILE_SHAPE r8_partial_tile_shape = {};
    D3D12_SUBRESOURCE_TILING r8_partial_tilings[2] = {};
    UINT64 r8_partial_upload_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT r8_partial_upload_footprint = {};
    UINT r8_partial_rows = 0;
    UINT64 r8_partial_row_bytes = 0;
    UINT sparse_total_tiles = 0;
    D3D12_PACKED_MIP_INFO sparse_packed_mips = {};
    D3D12_TILE_SHAPE sparse_tile_shape = {};
    D3D12_SUBRESOURCE_TILING sparse_tiling[2] = {};
    UINT sparse_tiling_count = 2;
    const UINT64 sparse_tile_size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    const UINT64 sparse_tile_bytes = 2 * sparse_tile_size;
    D3D12_RESOURCE_DESC tex_desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    UINT64 texture_upload_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT texture_footprint = {};
    UINT texture_rows = 0;
    UINT64 texture_row_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &texture_footprint, &texture_rows, &texture_row_bytes,
                                      &texture_upload_bytes);
    D3D12_RESOURCE_DESC texture_staging_desc = buffer_desc(texture_upload_bytes);
    HRESULT texture_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))
               : E_FAIL;
    HRESULT texture_upload_hr =
        device
            ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &texture_staging_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture_upload))
            : E_FAIL;
    HRESULT texture_readback_hr =
        device
            ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &texture_staging_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture_readback))
            : E_FAIL;
    HRESULT direct_io_texture_create_hr = E_FAIL;
    HRESULT direct_io_texture_write_hr = E_FAIL;
    HRESULT direct_io_texture_read_hr = E_FAIL;
    HRESULT direct_io_volume_create_hr = E_FAIL;
    HRESULT direct_io_volume_write_hr = E_FAIL;
    HRESULT direct_io_volume_read_hr = E_FAIL;
    HRESULT direct_io_volume_box_write_hr = E_FAIL;
    HRESULT direct_io_volume_box_read_hr = E_FAIL;
    HRESULT direct_io_d32s8_create_hr = E_FAIL;
    HRESULT direct_io_d32s8_write_hr[2] = {E_FAIL, E_FAIL};
    HRESULT direct_io_d32s8_read_hr[2] = {E_FAIL, E_FAIL};
    bool direct_io_d32s8_ok = false;
    HRESULT direct_io_nv12_create_hr = E_FAIL;
    HRESULT direct_io_nv12_write_hr[2] = {E_FAIL, E_FAIL};
    HRESULT direct_io_nv12_read_hr[2] = {E_FAIL, E_FAIL};
    HRESULT direct_io_p010_create_hr = E_FAIL;
    HRESULT direct_io_p010_write_hr[2] = {E_FAIL, E_FAIL};
    HRESULT direct_io_p010_read_hr[2] = {E_FAIL, E_FAIL};
    HRESULT direct_io_nv12_upload_hr = E_FAIL;
    HRESULT direct_io_nv12_readback_hr = E_FAIL;
    HRESULT direct_io_nv12_upload_map_hr = E_FAIL;
    HRESULT direct_io_nv12_readback_map_hr = E_FAIL;
    HRESULT direct_io_nv12_copy_destination_hr = E_FAIL;
    HRESULT direct_io_nv12_copy_y_read_hr = E_FAIL;
    HRESULT direct_io_nv12_copy_uv_read_hr = E_FAIL;
    bool direct_io_texture_ok = false;
    bool direct_io_volume_ok = false;
    bool direct_io_volume_box_ok = false;
    bool direct_io_nv12_ok = false;
    bool direct_io_nv12_copy_ok = false;
    bool direct_io_nv12_resource_copy_ok = false;
    bool direct_io_p010_ok = false;
    HRESULT direct_io_variant_create_hr[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    HRESULT direct_io_variant_write_hr[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    HRESULT direct_io_variant_read_hr[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    bool direct_io_variant_ok = false;
    D3D12_RESOURCE_DESC direct_io_texture_desc =
        texture_desc(8, 8, DXGI_FORMAT_R8G8B8A8_UNORM);
    direct_io_texture_desc.DepthOrArraySize = 2;
    direct_io_texture_desc.MipLevels = 2;
    direct_io_texture_create_hr = device
                                      ? device->CreateCommittedResource(
                                            &default_heap, D3D12_HEAP_FLAG_NONE,
                                            &direct_io_texture_desc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&direct_io_texture))
                                      : E_FAIL;
    if (direct_io_texture) {
        const D3D12_BOX io_box = {1, 1, 0, 3, 3, 1};
        uint8_t io_source[16] = {};
        for (UINT i = 0; i < 16; ++i)
            io_source[i] = static_cast<uint8_t>((i * 11u + 9u) & 0xffu);
        direct_io_texture_write_hr = direct_io_texture->WriteToSubresource(
            1, &io_box, io_source, 8, 16);
        uint8_t io_destination[32] = {};
        direct_io_texture_read_hr = direct_io_texture->ReadFromSubresource(
            io_destination, 16, 32, 1, &io_box);
        direct_io_texture_ok = SUCCEEDED(direct_io_texture_write_hr) &&
                               SUCCEEDED(direct_io_texture_read_hr);
        for (UINT row = 0; row < 2 && direct_io_texture_ok; ++row)
            for (UINT byte = 0; byte < 8; ++byte)
                direct_io_texture_ok =
                    io_destination[row * 16 + byte] == io_source[row * 8 + byte];
    }
    D3D12_RESOURCE_DESC direct_io_volume_desc =
        texture_desc(4, 4, DXGI_FORMAT_R8_UNORM);
    direct_io_volume_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    direct_io_volume_desc.DepthOrArraySize = 2;
    direct_io_volume_create_hr = device
                                    ? device->CreateCommittedResource(
                                          &default_heap, D3D12_HEAP_FLAG_NONE,
                                          &direct_io_volume_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&direct_io_volume))
                                    : E_FAIL;
    if (direct_io_volume) {
        uint8_t volume_source[32] = {};
        for (UINT i = 0; i < 32; ++i)
            volume_source[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
        direct_io_volume_write_hr = direct_io_volume->WriteToSubresource(
            0, nullptr, volume_source, 4, 16);
        uint8_t volume_destination[32] = {};
        direct_io_volume_read_hr = direct_io_volume->ReadFromSubresource(
            volume_destination, 4, 16, 0, nullptr);
        direct_io_volume_ok =
            SUCCEEDED(direct_io_volume_write_hr) &&
            SUCCEEDED(direct_io_volume_read_hr) &&
            std::memcmp(volume_source, volume_destination, sizeof(volume_source)) == 0;
        const D3D12_BOX volume_box = {1, 1, 1, 3, 3, 2};
        const uint8_t volume_box_source[4] = {0x19, 0x2a, 0x3b, 0x4c};
        uint8_t volume_box_destination[4] = {};
        direct_io_volume_box_write_hr = direct_io_volume->WriteToSubresource(
            0, &volume_box, volume_box_source, 2, 4);
        direct_io_volume_box_read_hr = direct_io_volume->ReadFromSubresource(
            volume_box_destination, 2, 4, 0, &volume_box);
        direct_io_volume_box_ok =
            SUCCEEDED(direct_io_volume_box_write_hr) &&
            SUCCEEDED(direct_io_volume_box_read_hr) &&
            std::memcmp(volume_box_source, volume_box_destination,
                        sizeof(volume_box_source)) == 0;
    }
    D3D12_RESOURCE_DESC direct_io_d32s8_desc =
        texture_desc(4, 4, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    direct_io_d32s8_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE direct_io_d32s8_clear = {};
    direct_io_d32s8_clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    direct_io_d32s8_clear.DepthStencil.Depth = 1.0f;
    direct_io_d32s8_create_hr =
        device ? device->CreateCommittedResource(
                     &default_heap, D3D12_HEAP_FLAG_NONE,
                     &direct_io_d32s8_desc,
                     D3D12_RESOURCE_STATE_DEPTH_WRITE,
                     &direct_io_d32s8_clear,
                     IID_PPV_ARGS(&direct_io_d32s8))
               : E_FAIL;
    if (direct_io_d32s8) {
        uint32_t depth_source[16] = {};
        uint8_t stencil_source[16] = {};
        for (UINT i = 0; i < 16; ++i) {
            depth_source[i] = 0x3e800000u + i * 0x00100000u;
            stencil_source[i] = static_cast<uint8_t>(0x40u + i);
        }
        direct_io_d32s8_write_hr[0] =
            direct_io_d32s8->WriteToSubresource(
                0, nullptr, depth_source, 16, sizeof(depth_source));
        direct_io_d32s8_write_hr[1] =
            direct_io_d32s8->WriteToSubresource(
                1, nullptr, stencil_source, 4, sizeof(stencil_source));
        uint32_t depth_destination[16] = {};
        uint8_t stencil_destination[16] = {};
        direct_io_d32s8_read_hr[0] =
            direct_io_d32s8->ReadFromSubresource(
                depth_destination, 16, sizeof(depth_destination), 0, nullptr);
        direct_io_d32s8_read_hr[1] =
            direct_io_d32s8->ReadFromSubresource(
                stencil_destination, 4, sizeof(stencil_destination), 1, nullptr);
        direct_io_d32s8_ok =
            SUCCEEDED(direct_io_d32s8_write_hr[0]) &&
            SUCCEEDED(direct_io_d32s8_write_hr[1]) &&
            SUCCEEDED(direct_io_d32s8_read_hr[0]) &&
            SUCCEEDED(direct_io_d32s8_read_hr[1]) &&
            std::memcmp(depth_source, depth_destination,
                        sizeof(depth_source)) == 0 &&
            std::memcmp(stencil_source, stencil_destination,
                        sizeof(stencil_source)) == 0;
    }
    D3D12_RESOURCE_DESC direct_io_nv12_desc =
        texture_desc(13, 8, DXGI_FORMAT_NV12);
    direct_io_nv12_create_hr =
        device ? device->CreateCommittedResource(
                     &default_heap, D3D12_HEAP_FLAG_NONE, &direct_io_nv12_desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                     IID_PPV_ARGS(&direct_io_nv12))
               : E_FAIL;
    if (direct_io_nv12) {
        uint8_t y_source[13 * 8] = {};
        uint8_t uv_source[7 * 4 * 2] = {};
        for (UINT i = 0; i < sizeof(y_source); ++i)
            y_source[i] = static_cast<uint8_t>((i * 5u + 1u) & 0xffu);
        for (UINT i = 0; i < sizeof(uv_source); ++i)
            uv_source[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xffu);
        direct_io_nv12_write_hr[0] = direct_io_nv12->WriteToSubresource(
            0, nullptr, y_source, 13, sizeof(y_source));
        direct_io_nv12_write_hr[1] = direct_io_nv12->WriteToSubresource(
            1, nullptr, uv_source, 14, sizeof(uv_source));
        uint8_t y_destination[sizeof(y_source)] = {};
        uint8_t uv_destination[sizeof(uv_source)] = {};
        direct_io_nv12_read_hr[0] = direct_io_nv12->ReadFromSubresource(
            y_destination, 13, sizeof(y_destination), 0, nullptr);
        direct_io_nv12_read_hr[1] = direct_io_nv12->ReadFromSubresource(
            uv_destination, 14, sizeof(uv_destination), 1, nullptr);
        direct_io_nv12_ok =
            SUCCEEDED(direct_io_nv12_write_hr[0]) &&
            SUCCEEDED(direct_io_nv12_write_hr[1]) &&
            SUCCEEDED(direct_io_nv12_read_hr[0]) &&
            SUCCEEDED(direct_io_nv12_read_hr[1]) &&
            std::memcmp(y_source, y_destination, sizeof(y_source)) == 0 &&
            std::memcmp(uv_source, uv_destination, sizeof(uv_source)) == 0;
    }
    direct_io_nv12_copy_destination_hr =
        device ? device->CreateCommittedResource(
                     &default_heap, D3D12_HEAP_FLAG_NONE,
                     &direct_io_nv12_desc, D3D12_RESOURCE_STATE_COMMON,
                     nullptr, IID_PPV_ARGS(&direct_io_nv12_copy_destination))
               : E_FAIL;
    D3D12_RESOURCE_DESC direct_io_p010_desc =
        texture_desc(8, 8, DXGI_FORMAT_P010);
    direct_io_p010_create_hr =
        device ? device->CreateCommittedResource(
                     &default_heap, D3D12_HEAP_FLAG_NONE, &direct_io_p010_desc,
                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                     IID_PPV_ARGS(&direct_io_p010))
               : E_FAIL;
    if (direct_io_p010) {
        uint8_t y_source[8 * 8 * 2] = {};
        uint8_t uv_source[4 * 4 * 4] = {};
        for (UINT i = 0; i < sizeof(y_source); ++i)
            y_source[i] = static_cast<uint8_t>((i * 11u + 5u) & 0xffu);
        for (UINT i = 0; i < sizeof(uv_source); ++i)
            uv_source[i] = static_cast<uint8_t>((i * 13u + 9u) & 0xffu);
        direct_io_p010_write_hr[0] = direct_io_p010->WriteToSubresource(
            0, nullptr, y_source, 16, sizeof(y_source));
        direct_io_p010_write_hr[1] = direct_io_p010->WriteToSubresource(
            1, nullptr, uv_source, 16, sizeof(uv_source));
        uint8_t y_destination[sizeof(y_source)] = {};
        uint8_t uv_destination[sizeof(uv_source)] = {};
        direct_io_p010_read_hr[0] = direct_io_p010->ReadFromSubresource(
            y_destination, 16, sizeof(y_destination), 0, nullptr);
        direct_io_p010_read_hr[1] = direct_io_p010->ReadFromSubresource(
            uv_destination, 16, sizeof(uv_destination), 1, nullptr);
        direct_io_p010_ok =
            SUCCEEDED(direct_io_p010_write_hr[0]) &&
            SUCCEEDED(direct_io_p010_write_hr[1]) &&
            SUCCEEDED(direct_io_p010_read_hr[0]) &&
            SUCCEEDED(direct_io_p010_read_hr[1]) &&
            std::memcmp(y_source, y_destination, sizeof(y_source)) == 0 &&
            std::memcmp(uv_source, uv_destination, sizeof(uv_source)) == 0;
    }
    const DXGI_FORMAT direct_io_variant_formats[4] = {
        DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM,
        DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_R9G9B9E5_SHAREDEXP};
    const UINT direct_io_variant_bytes_per_pixel[4] = {2, 2, 2, 4};
    direct_io_variant_ok = true;
    for (UINT variant = 0; variant < 4; ++variant) {
        D3D12_RESOURCE_DESC variant_desc = texture_desc(
            8, 4, direct_io_variant_formats[variant]);
        ID3D12Resource *variant_resource = nullptr;
        direct_io_variant_create_hr[variant] =
            device ? device->CreateCommittedResource(
                         &default_heap, D3D12_HEAP_FLAG_NONE, &variant_desc,
                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                         IID_PPV_ARGS(&variant_resource))
                   : E_FAIL;
        const UINT row_pitch = 8 * direct_io_variant_bytes_per_pixel[variant];
        const UINT slice_pitch = row_pitch * 4;
        std::vector<uint8_t> source(slice_pitch);
        std::vector<uint8_t> destination(slice_pitch);
        for (UINT i = 0; i < slice_pitch; ++i)
            source[i] = static_cast<uint8_t>((i * 19u + variant * 23u + 1u) & 0xffu);
        if (variant_resource) {
            direct_io_variant_write_hr[variant] =
                variant_resource->WriteToSubresource(
                    0, nullptr, source.data(), row_pitch, slice_pitch);
            direct_io_variant_read_hr[variant] =
                variant_resource->ReadFromSubresource(
                    destination.data(), row_pitch, slice_pitch, 0, nullptr);
            direct_io_variant_ok =
                direct_io_variant_ok &&
                SUCCEEDED(direct_io_variant_write_hr[variant]) &&
                SUCCEEDED(direct_io_variant_read_hr[variant]) &&
                source == destination;
            variant_resource->Release();
        } else {
            direct_io_variant_ok = false;
        }
    }
    D3D12_RESOURCE_DESC planar_copy_buffer_desc = buffer_desc(2048);
    direct_io_nv12_upload_hr =
        device ? device->CreateCommittedResource(
                     &upload_heap, D3D12_HEAP_FLAG_NONE,
                     &planar_copy_buffer_desc,
                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                     IID_PPV_ARGS(&direct_io_nv12_upload))
               : E_FAIL;
    direct_io_nv12_readback_hr =
        device ? device->CreateCommittedResource(
                     &readback_heap, D3D12_HEAP_FLAG_NONE,
                     &planar_copy_buffer_desc,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&direct_io_nv12_readback))
               : E_FAIL;
    if (direct_io_nv12_upload) {
        uint8_t *planar_upload_data = nullptr;
        direct_io_nv12_upload_map_hr = direct_io_nv12_upload->Map(
            0, nullptr, reinterpret_cast<void **>(&planar_upload_data));
        if (SUCCEEDED(direct_io_nv12_upload_map_hr) && planar_upload_data) {
            std::memset(planar_upload_data, 0, 2048);
            for (UINT y = 0; y < 8; ++y)
                for (UINT x = 0; x < 13; ++x)
                    planar_upload_data[y * 256 + x] =
                        static_cast<uint8_t>((x * 17u + y * 31u + 5u) & 0xffu);
            direct_io_nv12_upload->Unmap(0, nullptr);
        }
    }
    if (list && direct_io_nv12 && direct_io_nv12_upload &&
        direct_io_nv12_readback && SUCCEEDED(direct_io_nv12_upload_map_hr)) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT planar_copy_footprint = {};
        planar_copy_footprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        planar_copy_footprint.Footprint.Width = 13;
        planar_copy_footprint.Footprint.Height = 8;
        planar_copy_footprint.Footprint.Depth = 1;
        planar_copy_footprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION planar_upload_location = {};
        planar_upload_location.pResource = direct_io_nv12_upload;
        planar_upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        planar_upload_location.PlacedFootprint = planar_copy_footprint;
        D3D12_TEXTURE_COPY_LOCATION planar_texture_location = {};
        planar_texture_location.pResource = direct_io_nv12;
        planar_texture_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        planar_texture_location.SubresourceIndex = 0;
        list->CopyTextureRegion(&planar_texture_location, 0, 0, 0,
                                &planar_upload_location, nullptr);
        D3D12_RESOURCE_BARRIER planar_copy_barrier = transition_barrier(
            direct_io_nv12, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &planar_copy_barrier);
        D3D12_TEXTURE_COPY_LOCATION planar_readback_location = {};
        planar_readback_location.pResource = direct_io_nv12_readback;
        planar_readback_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        planar_readback_location.PlacedFootprint = planar_copy_footprint;
        D3D12_TEXTURE_COPY_LOCATION planar_source_location = {};
        planar_source_location.pResource = direct_io_nv12;
        planar_source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        planar_source_location.SubresourceIndex = 0;
        list->CopyTextureRegion(&planar_readback_location, 0, 0, 0,
                                &planar_source_location, nullptr);
        if (direct_io_nv12_copy_destination) {
            list->CopyResource(direct_io_nv12_copy_destination, direct_io_nv12);
        }
    }
    D3D12_RESOURCE_DESC unsupported_texture_desc = texture_desc(4, 4, DXGI_FORMAT_R1_UNORM);
    HRESULT unsupported_texture_hr =
        device
            ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &unsupported_texture_desc,
                                              D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&unsupported_texture))
            : E_FAIL;
    const bool unsupported_texture_rejected = unsupported_texture_hr == E_INVALIDARG && unsupported_texture == nullptr;
    uint8_t* texture_upload_ptr = nullptr;
    HRESULT texture_map_hr =
        texture_upload ? texture_upload->Map(0, nullptr, reinterpret_cast<void**>(&texture_upload_ptr)) : E_FAIL;
    if (SUCCEEDED(texture_map_hr) && texture_upload_ptr) {
        std::memset(texture_upload_ptr, 0, static_cast<size_t>(texture_upload_bytes));
        for (UINT y = 0; y < 4; ++y) {
            for (UINT x = 0; x < 4; ++x) {
                size_t offset =
                    static_cast<size_t>(texture_footprint.Offset + y * texture_footprint.Footprint.RowPitch + x * 4);
                texture_upload_ptr[offset + 0] = static_cast<uint8_t>(x * 40);
                texture_upload_ptr[offset + 1] = static_cast<uint8_t>(y * 40);
                texture_upload_ptr[offset + 2] = 0xa5;
                texture_upload_ptr[offset + 3] = 0xff;
            }
        }
        texture_upload->Unmap(0, nullptr);
    }
    if (list && texture && texture_upload && texture_readback) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = texture_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = texture_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER texture_barrier =
            transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &texture_barrier);
        D3D12_TEXTURE_COPY_LOCATION readback_dst = {};
        readback_dst.pResource = texture_readback;
        readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readback_dst.PlacedFootprint = texture_footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = texture;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        texture_src.SubresourceIndex = 0;
        list->CopyTextureRegion(&readback_dst, 0, 0, 0, &texture_src, nullptr);
    }

    D3D12_HEAP_DESC sparse_heap_desc = {};
    sparse_heap_desc.SizeInBytes = sparse_tile_bytes;
    sparse_heap_desc.Properties = default_heap;
    sparse_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    sparse_heap_hr = device ? device->CreateHeap(&sparse_heap_desc, IID_PPV_ARGS(&sparse_heap)) : E_FAIL;
    D3D12_HEAP_DESC reuse_heap_desc = {};
    reuse_heap_desc.SizeInBytes = sparse_tile_size;
    reuse_heap_desc.Properties = default_heap;
    reuse_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    reuse_heap_hr = device ? device->CreateHeap(&reuse_heap_desc, IID_PPV_ARGS(&reuse_heap)) : E_FAIL;
    D3D12_HEAP_DESC array_alias_heap_desc = reuse_heap_desc;
    array_alias_heap_desc.SizeInBytes = sparse_tile_bytes;
    array_alias_heap_hr = device ? device->CreateHeap(&array_alias_heap_desc, IID_PPV_ARGS(&array_alias_heap)) : E_FAIL;
    D3D12_HEAP_DESC copy_mapping_heap_desc = {};
    copy_mapping_heap_desc.SizeInBytes = sparse_tile_size;
    copy_mapping_heap_desc.Properties = default_heap;
    copy_mapping_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    copy_mapping_heap_hr =
        device ? device->CreateHeap(&copy_mapping_heap_desc, IID_PPV_ARGS(&copy_mapping_heap)) : E_FAIL;
    D3D12_RESOURCE_DESC reserved_desc = texture_desc(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM);
    reserved_desc.DepthOrArraySize = 2;
    reserved_texture_hr = device ? device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                  nullptr, IID_PPV_ARGS(&reserved_texture))
                                 : E_FAIL;
    if (device && reserved_texture) {
        sparse_tiling_hr = S_OK;
        device->GetResourceTiling(reserved_texture, &sparse_total_tiles, &sparse_packed_mips, &sparse_tile_shape,
                                  &sparse_tiling_count, 0, sparse_tiling);
    }
    D3D12_RESOURCE_DESC reserved_1d_desc = texture_desc(16384, 1, DXGI_FORMAT_R32_FLOAT);
    reserved_1d_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    reserved_1d_desc.DepthOrArraySize = 2;
    reserved_1d_texture_hr =
        device ? device->CreateReservedResource(&reserved_1d_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr,
                                                IID_PPV_ARGS(&reserved_1d_texture))
               : E_FAIL;
    if (device && reserved_1d_texture) {
        reserved_1d_tiling_hr = S_OK;
        device->GetResourceTiling(reserved_1d_texture,
                                  &reserved_1d_total_tiles, nullptr,
                                  &reserved_1d_tile_shape,
                                  &reserved_1d_tiling_count, 0,
                                  reserved_1d_tilings);
    }
    reserved_1d_alias_texture_hr =
        device ? device->CreateReservedResource(
                     &reserved_1d_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                     nullptr, IID_PPV_ARGS(&reserved_1d_alias_texture))
               : E_FAIL;
    D3D12_RESOURCE_DESC placement_alias_desc = reserved_desc;
    placement_alias_desc.DepthOrArraySize = 1;
    placement_alias_texture_hr =
        device ? device->CreateReservedResource(&placement_alias_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&placement_alias_texture))
               : E_FAIL;
    array_alias_texture_hr =
        device ? device->CreateReservedResource(&reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&array_alias_texture))
               : E_FAIL;
    D3D12_RESOURCE_DESC volume_desc = reserved_desc;
    volume_desc.Width = 32;
    volume_desc.Height = 32;
    volume_desc.DepthOrArraySize = 32;
    volume_desc.MipLevels = 1;
    volume_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume_texture_hr = device ? device->CreateReservedResource(&volume_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&volume_texture))
                               : E_FAIL;
    D3D12_RESOURCE_DESC volume_alias_desc = volume_desc;
    volume_alias_texture_hr = device
                                  ? device->CreateReservedResource(&volume_alias_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   nullptr, IID_PPV_ARGS(&volume_alias_texture))
                                  : E_FAIL;
    if (device && volume_texture) {
        volume_tiling_hr = S_OK;
        device->GetResourceTiling(volume_texture, &volume_total_tiles, nullptr, &volume_tile_shape,
                                  &volume_tiling_count, 0, &volume_tiling);
    }
    if (device && volume_alias_texture) {
        volume_alias_tiling_hr = S_OK;
        device->GetResourceTiling(volume_alias_texture, &volume_alias_total_tiles, nullptr, &volume_alias_tile_shape,
                                  &volume_alias_tiling_count, 0, &volume_alias_tiling);
    }
    D3D12_HEAP_DESC volume_heap_desc = sparse_heap_desc;
    // Metal's 16 KiB placement pages use a 64x64 RGBA8 XY tile for a
    // volume. One D3D12 32x32x16 tile therefore consumes four 64 KiB heap
    // tiles; reserve two such logical tiles for the two depth regions.
    volume_heap_desc.SizeInBytes = sparse_tile_bytes * 4;
    volume_heap_hr = device ? device->CreateHeap(&volume_heap_desc, IID_PPV_ARGS(&volume_heap)) : E_FAIL;
    D3D12_RESOURCE_DESC reserved_buffer_desc = buffer_desc(sparse_tile_bytes);
    reserved_buffer_hr = device ? device->CreateReservedResource(&reserved_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                 nullptr, IID_PPV_ARGS(&reserved_buffer))
                                : E_FAIL;
    reuse_buffer_hr = device ? device->CreateReservedResource(&reserved_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                              nullptr, IID_PPV_ARGS(&reuse_buffer))
                             : E_FAIL;
    if (device && reserved_buffer) {
        reserved_buffer_tiling_hr = S_OK;
        device->GetResourceTiling(reserved_buffer, &reserved_buffer_total_tiles, nullptr, &reserved_buffer_tile_shape,
                                  &reserved_buffer_tiling_count, 0, &reserved_buffer_tiling);
    }
    D3D12_RESOURCE_DESC sparse_buffer_desc = buffer_desc(sparse_tile_bytes);
    sparse_upload_hr = device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &sparse_buffer_desc,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                IID_PPV_ARGS(&sparse_upload))
                              : E_FAIL;
    sparse_readback_hr = device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                                  &sparse_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                  nullptr, IID_PPV_ARGS(&sparse_readback))
                                : E_FAIL;
    D3D12_RESOURCE_DESC reserved_1d_readback_desc = buffer_desc(sparse_tile_bytes);
    reserved_1d_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                  &reserved_1d_readback_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr,
                                                  IID_PPV_ARGS(&reserved_1d_readback))
               : E_FAIL;
    sparse_unmapped_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &sparse_buffer_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&sparse_unmapped_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC reserved_buffer_readback_desc = buffer_desc(sparse_tile_bytes);
    reserved_buffer_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&reserved_buffer_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC reserved_buffer_reuse_readback_desc = buffer_desc(sparse_tile_size);
    reserved_buffer_reuse_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                 &reserved_buffer_reuse_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&reserved_buffer_reuse_readback))
               : E_FAIL;
    reserved_buffer_unmapped_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&reserved_buffer_unmapped_readback))
               : E_FAIL;
    placement_alias_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&placement_alias_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC array_alias_readback_desc = buffer_desc(sparse_tile_bytes);
    array_alias_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                 &array_alias_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&array_alias_readback))
               : E_FAIL;
    volume_readback_hr =
        device
            ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&volume_readback))
            : E_FAIL;
    volume_alias_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&volume_alias_readback))
               : E_FAIL;
    mapping_copy_source_hr = device
                                 ? device->CreateReservedResource(&reserved_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                  nullptr, IID_PPV_ARGS(&mapping_copy_source))
                                 : E_FAIL;
    mapping_copy_destination_hr =
        device ? device->CreateReservedResource(&reserved_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&mapping_copy_destination))
               : E_FAIL;
    mapping_copy_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &reserved_buffer_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&mapping_copy_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC mipped_reserved_desc = texture_desc(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM);
    mipped_reserved_desc.MipLevels = 2;
    mipped_reserved_texture_hr =
        device ? device->CreateReservedResource(&mipped_reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&mipped_reserved_texture))
               : E_FAIL;
    if (device && mipped_reserved_texture) {
        mipped_reserved_tiling_hr = S_OK;
        device->GetResourceTiling(mipped_reserved_texture, &mipped_reserved_total_tiles, nullptr,
                                  &mipped_reserved_tile_shape, &mipped_reserved_tiling_count, 0,
                                  mipped_reserved_tilings);
    }
    D3D12_RESOURCE_DESC packed_tail_desc = texture_desc(512, 512, DXGI_FORMAT_R8G8B8A8_UNORM);
    packed_tail_desc.MipLevels = 4;
    packed_tail_reserved_texture_hr =
        device ? device->CreateReservedResource(&packed_tail_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr,
                                                IID_PPV_ARGS(&packed_tail_reserved_texture))
               : E_FAIL;
    if (device && packed_tail_reserved_texture) {
        packed_tail_reserved_tiling_hr = S_OK;
        device->GetResourceTiling(packed_tail_reserved_texture, &packed_tail_total_tiles,
                                  &packed_tail_info, &packed_tail_shape,
                                  &packed_tail_tiling_count, 0,
                                  packed_tail_tilings);
        device->GetResourceTiling(packed_tail_reserved_texture, nullptr, nullptr,
                                  nullptr, &packed_tail_partial_count, 2,
                                  packed_tail_partial_tilings);
        packed_tail_partial_query_ok =
            packed_tail_partial_count == 2 &&
            packed_tail_partial_tilings[0].WidthInTiles == 1 &&
            packed_tail_partial_tilings[0].StartTileIndexInOverallResource == 20 &&
            packed_tail_partial_tilings[1].WidthInTiles == 0 &&
            packed_tail_partial_tilings[1].StartTileIndexInOverallResource == D3D12_PACKED_TILE;
    }
    D3D12_RESOURCE_DESC mipped_reserved_readback_desc = buffer_desc(sparse_tile_size);
    mipped_reserved_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &mipped_reserved_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&mipped_reserved_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC packed_tail_readback_desc = buffer_desc(sparse_tile_size);
    packed_tail_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &packed_tail_readback_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr,
                                                  IID_PPV_ARGS(&packed_tail_readback))
               : E_FAIL;
    D3D12_RESOURCE_DESC r8_reserved_desc = texture_desc(256, 256, DXGI_FORMAT_R8_UNORM);
    r8_reserved_texture_hr = device ? device->CreateReservedResource(&r8_reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                     nullptr, IID_PPV_ARGS(&r8_reserved_texture))
                                    : E_FAIL;
    if (device && r8_reserved_texture) {
        r8_reserved_tiling_hr = S_OK;
        device->GetResourceTiling(r8_reserved_texture, &r8_reserved_total_tiles, nullptr, &r8_reserved_tile_shape,
                                  &r8_reserved_tiling_count, 0, &r8_reserved_tiling);
    }
    D3D12_RESOURCE_DESC r8_reserved_readback_desc = buffer_desc(sparse_tile_size);
    r8_reserved_readback_hr = device ? device->CreateCommittedResource(
                                           &readback_heap, D3D12_HEAP_FLAG_NONE, &r8_reserved_readback_desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r8_reserved_readback))
                                     : E_FAIL;
    D3D12_HEAP_DESC r8_mipped_heap_desc = {};
    r8_mipped_heap_desc.SizeInBytes = 5 * sparse_tile_size;
    r8_mipped_heap_desc.Properties = default_heap;
    r8_mipped_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    r8_mipped_heap_hr = device ? device->CreateHeap(&r8_mipped_heap_desc, IID_PPV_ARGS(&r8_mipped_heap)) : E_FAIL;
    D3D12_RESOURCE_DESC r8_mipped_desc = texture_desc(512, 512, DXGI_FORMAT_R8_UNORM);
    r8_mipped_desc.MipLevels = 2;
    r8_mipped_texture_hr = device ? device->CreateReservedResource(&r8_mipped_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   nullptr, IID_PPV_ARGS(&r8_mipped_texture))
                                  : E_FAIL;
    if (device && r8_mipped_texture) {
        r8_mipped_tiling_hr = S_OK;
        device->GetResourceTiling(r8_mipped_texture, &r8_mipped_total_tiles, nullptr, &r8_mipped_tile_shape,
                                  &r8_mipped_tiling_count, 0, r8_mipped_tilings);
    }
    r8_mipped_readback_hr = device ? device->CreateCommittedResource(
                                         &readback_heap, D3D12_HEAP_FLAG_NONE, &r8_reserved_readback_desc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r8_mipped_readback))
                                   : E_FAIL;
    D3D12_RESOURCE_DESC r8_partial_desc = texture_desc(256, 256, DXGI_FORMAT_R8_UNORM);
    r8_partial_desc.MipLevels = 2;
    r8_partial_texture_hr = device ? device->CreateReservedResource(&r8_partial_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    nullptr, IID_PPV_ARGS(&r8_partial_texture))
                                   : E_FAIL;
    if (device && r8_partial_texture) {
        r8_partial_tiling_hr = S_OK;
        device->GetResourceTiling(r8_partial_texture, &r8_partial_total_tiles, &r8_partial_packed_mips,
                                  &r8_partial_tile_shape, &r8_partial_tiling_count, 0, r8_partial_tilings);
    }
    if (device)
        device->GetCopyableFootprints(&r8_partial_desc, 1, 1, 0, &r8_partial_upload_footprint, &r8_partial_rows,
                                      &r8_partial_row_bytes, &r8_partial_upload_bytes);
    D3D12_RESOURCE_DESC r8_partial_staging_desc = buffer_desc(r8_partial_upload_bytes);
    r8_partial_upload_hr = device ? device->CreateCommittedResource(
                                        &upload_heap, D3D12_HEAP_FLAG_NONE, &r8_partial_staging_desc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r8_partial_upload))
                                  : E_FAIL;
    r8_partial_readback_hr = device ? device->CreateCommittedResource(
                                          &readback_heap, D3D12_HEAP_FLAG_NONE, &r8_partial_staging_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r8_partial_readback))
                                    : E_FAIL;
    uint8_t* r8_partial_upload_ptr = nullptr;
    r8_partial_upload_map_hr =
        r8_partial_upload ? r8_partial_upload->Map(0, nullptr, reinterpret_cast<void**>(&r8_partial_upload_ptr))
                          : E_FAIL;
    if (SUCCEEDED(r8_partial_upload_map_hr) && r8_partial_upload_ptr) {
        std::memset(r8_partial_upload_ptr, 0, static_cast<size_t>(r8_partial_upload_bytes));
        for (UINT y = 0; y < 128; ++y) {
            for (UINT x = 0; x < 128; ++x) {
                r8_partial_upload_ptr[r8_partial_upload_footprint.Offset +
                                      y * r8_partial_upload_footprint.Footprint.RowPitch + x] =
                    static_cast<uint8_t>(((y * 128u + x) * 29u + 7u) & 0xffu);
            }
        }
        r8_partial_upload->Unmap(0, nullptr);
    }
    std::vector<SparseFormatProbe> sparse_format_probes = {
        {"R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, 256, 128, 256, 128},
        {"R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, 128, 128, 128, 128},
        {"R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT, 128, 128, 128, 128},
        {"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM, 128, 64, 128, 64},
        {"R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, 64, 64, 64, 64},
        {"R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT, 128, 128, 128, 128},
        {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT, 128, 128, 128, 128},
        {"BC1_UNORM", DXGI_FORMAT_BC1_UNORM, 512, 256, 512, 256},
        {"BC4_UNORM", DXGI_FORMAT_BC4_UNORM, 512, 256, 512, 256},
        {"BC7_UNORM", DXGI_FORMAT_BC7_UNORM, 256, 256, 256, 256},
    };
    for (auto& sparse_format : sparse_format_probes) {
        D3D12_RESOURCE_DESC sparse_format_desc =
            texture_desc(sparse_format.width, sparse_format.height, sparse_format.format);
        sparse_format.texture_hr =
            device ? device->CreateReservedResource(&sparse_format_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&sparse_format.texture))
                   : E_FAIL;
        if (device && sparse_format.texture) {
            sparse_format.tiling_hr = S_OK;
            UINT tiling_count = 1;
            device->GetResourceTiling(sparse_format.texture, &sparse_format.total_tiles, nullptr,
                                      &sparse_format.tile_shape, &tiling_count, 0, nullptr);
            if (tiling_count != 1)
                sparse_format.tiling_hr = E_FAIL;
        }
        D3D12_RESOURCE_DESC sparse_format_readback_desc = buffer_desc(sparse_tile_size);
        sparse_format.readback_hr =
            device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &sparse_format_readback_desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&sparse_format.readback))
                   : E_FAIL;
    }
    uint8_t* sparse_upload_ptr = nullptr;
    HRESULT sparse_upload_map_hr =
        sparse_upload ? sparse_upload->Map(0, nullptr, reinterpret_cast<void**>(&sparse_upload_ptr)) : E_FAIL;
    bool sparse_copy_ok = false;
    if (SUCCEEDED(sparse_upload_map_hr) && sparse_upload_ptr) {
        for (UINT64 i = 0; i < sparse_tile_bytes; i++)
            sparse_upload_ptr[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
        sparse_upload->Unmap(0, nullptr);
    }
    if (queue && sparse_heap && reserved_texture && placement_alias_texture && sparse_upload && sparse_readback &&
        placement_alias_readback && sparse_total_tiles == 2 && sparse_tiling_count == 2 &&
        sparse_tile_shape.WidthInTexels == 128 && sparse_tile_shape.HeightInTexels == 128) {
        D3D12_TILED_RESOURCE_COORDINATE coordinates[2] = {};
        coordinates[1].Subresource = 1;
        D3D12_TILE_REGION_SIZE region_sizes[2] = {};
        region_sizes[0].NumTiles = 1;
        region_sizes[1].NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS range_flags[2] = {D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NONE};
        UINT heap_offsets[2] = {0, 1};
        UINT range_tile_counts[2] = {1, 1};
        queue->UpdateTileMappings(reserved_texture, 2, coordinates, region_sizes, sparse_heap, 2, range_flags,
                                  heap_offsets, range_tile_counts, D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TILED_RESOURCE_COORDINATE alias_coordinate = {};
        D3D12_TILE_REGION_SIZE alias_region = {};
        alias_region.NumTiles = 1;
        queue->CopyTileMappings(placement_alias_texture, &alias_coordinate, reserved_texture, &coordinates[0],
                                &alias_region, D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(reserved_texture, &coordinates[0], &region_sizes[0], sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        list->CopyTiles(reserved_texture, &coordinates[1], &region_sizes[1], sparse_upload, sparse_tile_size,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER sparse_barrier =
            transition_barrier(reserved_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &sparse_barrier);
        D3D12_RESOURCE_BARRIER aliasing_barrier = {};
        aliasing_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        aliasing_barrier.Aliasing.pResourceBefore = reserved_texture;
        aliasing_barrier.Aliasing.pResourceAfter = placement_alias_texture;
        list->ResourceBarrier(1, &aliasing_barrier);
        D3D12_RESOURCE_BARRIER alias_barrier = transition_barrier(
            placement_alias_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &alias_barrier);
        list->CopyTiles(placement_alias_texture, &alias_coordinate, &alias_region, placement_alias_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        list->CopyTiles(reserved_texture, &coordinates[0], &region_sizes[0], sparse_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        list->CopyTiles(reserved_texture, &coordinates[1], &region_sizes[1], sparse_readback, sparse_tile_size,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (queue && volume_heap && volume_texture && volume_alias_texture && sparse_upload && volume_readback &&
        volume_alias_readback && volume_total_tiles == 2 && volume_alias_total_tiles == 2 && volume_tiling_count == 1 &&
        volume_alias_tiling_count == 1 && volume_tile_shape.WidthInTexels == 32 &&
        volume_tile_shape.HeightInTexels == 32 && volume_tile_shape.DepthInTexels == 16) {
        D3D12_TILED_RESOURCE_COORDINATE volume_coordinate = {};
        D3D12_TILE_REGION_SIZE volume_region = {};
        volume_region.UseBox = TRUE;
        volume_region.Width = 1;
        volume_region.Height = 1;
        volume_region.Depth = 2;
        volume_region.NumTiles = 2;
        D3D12_TILE_RANGE_FLAGS volume_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT volume_heap_offset = 0;
        UINT volume_range_count = 1;
        UINT volume_range_tiles = 2;
        queue->UpdateTileMappings(volume_texture, 1, &volume_coordinate, &volume_region, volume_heap,
                                  volume_range_count, &volume_range_flag, &volume_heap_offset, &volume_range_tiles,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        queue->CopyTileMappings(volume_alias_texture, &volume_coordinate, volume_texture, &volume_coordinate,
                                &volume_region, D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(volume_texture, &volume_coordinate, &volume_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER volume_barrier =
            transition_barrier(volume_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &volume_barrier);
        D3D12_RESOURCE_BARRIER volume_alias_barrier =
            transition_barrier(volume_alias_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &volume_alias_barrier);
        list->CopyTiles(volume_texture, &volume_coordinate, &volume_region, volume_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        list->CopyTiles(volume_alias_texture, &volume_coordinate, &volume_region, volume_alias_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && reserved_buffer && sparse_heap && sparse_upload && reserved_buffer_readback &&
        reserved_buffer_total_tiles == 2 && reserved_buffer_tiling_count == 1 &&
        reserved_buffer_tile_shape.WidthInTexels == sparse_tile_size) {
        D3D12_TILED_RESOURCE_COORDINATE buffer_coordinate = {};
        D3D12_TILE_REGION_SIZE buffer_region = {};
        buffer_region.NumTiles = 2;
        D3D12_TILE_RANGE_FLAGS buffer_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT buffer_heap_offset = 0;
        UINT buffer_range_count = 2;
        queue->UpdateTileMappings(reserved_buffer, 1, &buffer_coordinate, &buffer_region, sparse_heap, 1,
                                  &buffer_range_flag, &buffer_heap_offset, &buffer_range_count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(reserved_buffer, &buffer_coordinate, &buffer_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER reserved_buffer_barrier =
            transition_barrier(reserved_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &reserved_buffer_barrier);
        list->CopyTiles(reserved_buffer, &buffer_coordinate, &buffer_region, reserved_buffer_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && mipped_reserved_texture && sparse_heap && sparse_upload && mipped_reserved_readback &&
        mipped_reserved_total_tiles == 5 && mipped_reserved_tiling_count == 2 &&
        mipped_reserved_tile_shape.WidthInTexels == 128 && mipped_reserved_tilings[0].WidthInTiles == 2 &&
        mipped_reserved_tilings[0].HeightInTiles == 2 && mipped_reserved_tilings[1].WidthInTiles == 1 &&
        mipped_reserved_tilings[1].HeightInTiles == 1 &&
        mipped_reserved_tilings[1].StartTileIndexInOverallResource == 4) {
        D3D12_TILED_RESOURCE_COORDINATE mip_coordinate = {};
        mip_coordinate.Subresource = 1;
        D3D12_TILE_REGION_SIZE mip_region = {};
        mip_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS mip_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT mip_heap_offset = 0;
        UINT mip_range_count = 1;
        queue->UpdateTileMappings(mipped_reserved_texture, 1, &mip_coordinate, &mip_region, sparse_heap, 1,
                                  &mip_range_flag, &mip_heap_offset, &mip_range_count, D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(mipped_reserved_texture, &mip_coordinate, &mip_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER mip_barrier = transition_barrier(mipped_reserved_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &mip_barrier);
        list->CopyTiles(mipped_reserved_texture, &mip_coordinate, &mip_region, mipped_reserved_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && copy_mapping_heap && mapping_copy_source && mapping_copy_destination &&
        mapping_copy_readback && sparse_upload && SUCCEEDED(copy_mapping_heap_hr) &&
        SUCCEEDED(mapping_copy_source_hr) && SUCCEEDED(mapping_copy_destination_hr) &&
        SUCCEEDED(mapping_copy_readback_hr)) {
        D3D12_TILED_RESOURCE_COORDINATE source_coordinate = {};
        D3D12_TILED_RESOURCE_COORDINATE destination_coordinate = {};
        D3D12_TILE_REGION_SIZE mapping_region = {};
        mapping_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS mapping_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT mapping_heap_offset = 0;
        UINT mapping_range_count = 1;
        queue->UpdateTileMappings(mapping_copy_source, 1, &source_coordinate, &mapping_region, copy_mapping_heap, 1,
                                  &mapping_range_flag, &mapping_heap_offset, &mapping_range_count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        queue->CopyTileMappings(mapping_copy_destination, &destination_coordinate, mapping_copy_source,
                                &source_coordinate, &mapping_region, D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(mapping_copy_source, &source_coordinate, &mapping_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER source_barrier =
            transition_barrier(mapping_copy_source, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &source_barrier);
        D3D12_RESOURCE_BARRIER destination_barrier = transition_barrier(
            mapping_copy_destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &destination_barrier);
        list->CopyTiles(mapping_copy_destination, &destination_coordinate, &mapping_region, mapping_copy_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && sparse_heap && r8_reserved_texture && r8_reserved_readback &&
        SUCCEEDED(r8_reserved_texture_hr) && SUCCEEDED(r8_reserved_tiling_hr) && SUCCEEDED(r8_reserved_readback_hr) &&
        r8_reserved_total_tiles == 1 && r8_reserved_tiling_count == 1 && r8_reserved_tile_shape.WidthInTexels == 256 &&
        r8_reserved_tile_shape.HeightInTexels == 256) {
        D3D12_TILED_RESOURCE_COORDINATE r8_coordinate = {};
        D3D12_TILE_REGION_SIZE r8_region = {};
        r8_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS r8_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT r8_heap_offset = 0;
        UINT r8_range_count = 1;
        queue->UpdateTileMappings(r8_reserved_texture, 1, &r8_coordinate, &r8_region, sparse_heap, 1, &r8_range_flag,
                                  &r8_heap_offset, &r8_range_count, D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(r8_reserved_texture, &r8_coordinate, &r8_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER r8_barrier =
            transition_barrier(r8_reserved_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &r8_barrier);
        list->CopyTiles(r8_reserved_texture, &r8_coordinate, &r8_region, r8_reserved_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && r8_mipped_heap && r8_mipped_texture && r8_mipped_readback && SUCCEEDED(r8_mipped_heap_hr) &&
        SUCCEEDED(r8_mipped_texture_hr) && SUCCEEDED(r8_mipped_tiling_hr) && SUCCEEDED(r8_mipped_readback_hr) &&
        r8_mipped_total_tiles == 5 && r8_mipped_tiling_count == 2 && r8_mipped_tile_shape.WidthInTexels == 256 &&
        r8_mipped_tile_shape.HeightInTexels == 256 && r8_mipped_tilings[0].WidthInTiles == 2 &&
        r8_mipped_tilings[0].HeightInTiles == 2 && r8_mipped_tilings[1].WidthInTiles == 1 &&
        r8_mipped_tilings[1].HeightInTiles == 1 && r8_mipped_tilings[1].StartTileIndexInOverallResource == 4) {
        D3D12_TILED_RESOURCE_COORDINATE r8_mip_coordinate = {};
        r8_mip_coordinate.Subresource = 1;
        D3D12_TILE_REGION_SIZE r8_mip_region = {};
        r8_mip_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS r8_mip_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT r8_mip_heap_offset = 0;
        UINT r8_mip_range_count = 1;
        queue->UpdateTileMappings(r8_mipped_texture, 1, &r8_mip_coordinate, &r8_mip_region, r8_mipped_heap, 1,
                                  &r8_mip_range_flag, &r8_mip_heap_offset, &r8_mip_range_count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(r8_mipped_texture, &r8_mip_coordinate, &r8_mip_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER r8_mip_barrier =
            transition_barrier(r8_mipped_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &r8_mip_barrier);
        list->CopyTiles(r8_mipped_texture, &r8_mip_coordinate, &r8_mip_region, r8_mipped_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && sparse_heap && r8_partial_texture && r8_partial_upload && r8_partial_readback &&
        SUCCEEDED(r8_partial_texture_hr) && SUCCEEDED(r8_partial_tiling_hr) && SUCCEEDED(r8_partial_upload_hr) &&
        SUCCEEDED(r8_partial_readback_hr) && SUCCEEDED(r8_partial_upload_map_hr) && r8_partial_total_tiles == 2 &&
        r8_partial_tiling_count == 2 && r8_partial_packed_mips.NumStandardMips == 1 &&
        r8_partial_packed_mips.NumPackedMips == 1 && r8_partial_packed_mips.NumTilesForPackedMips == 1 &&
        r8_partial_packed_mips.StartTileIndexInOverallResource == 1 && r8_partial_tilings[0].WidthInTiles == 1 &&
        r8_partial_tilings[0].HeightInTiles == 1 && r8_partial_tilings[0].StartTileIndexInOverallResource == 0 &&
        r8_partial_tilings[1].WidthInTiles == 0 && r8_partial_tilings[1].HeightInTiles == 0 &&
        r8_partial_tilings[1].DepthInTiles == 0 &&
        r8_partial_tilings[1].StartTileIndexInOverallResource == D3D12_PACKED_TILE) {
        D3D12_TILED_RESOURCE_COORDINATE partial_coordinate = {};
        partial_coordinate.Subresource = 1;
        D3D12_TILE_REGION_SIZE partial_region = {};
        partial_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS partial_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT partial_heap_offset = 1;
        UINT partial_range_count = 1;
        queue->UpdateTileMappings(r8_partial_texture, 1, &partial_coordinate, &partial_region, sparse_heap, 1,
                                  &partial_range_flag, &partial_heap_offset, &partial_range_count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TEXTURE_COPY_LOCATION partial_src = {};
        partial_src.pResource = r8_partial_upload;
        partial_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        partial_src.PlacedFootprint = r8_partial_upload_footprint;
        D3D12_TEXTURE_COPY_LOCATION partial_dst = {};
        partial_dst.pResource = r8_partial_texture;
        partial_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        partial_dst.SubresourceIndex = 1;
        list->CopyTextureRegion(&partial_dst, 0, 0, 0, &partial_src, nullptr);
        D3D12_RESOURCE_BARRIER partial_barrier =
            transition_barrier(r8_partial_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &partial_barrier);
        D3D12_TEXTURE_COPY_LOCATION partial_readback_dst = {};
        partial_readback_dst.pResource = r8_partial_readback;
        partial_readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        partial_readback_dst.PlacedFootprint = r8_partial_upload_footprint;
        D3D12_TEXTURE_COPY_LOCATION partial_src_texture = {};
        partial_src_texture.pResource = r8_partial_texture;
        partial_src_texture.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        partial_src_texture.SubresourceIndex = 1;
        list->CopyTextureRegion(&partial_readback_dst, 0, 0, 0, &partial_src_texture, nullptr);
    }
    if (list && queue && packed_tail_reserved_texture && sparse_heap && sparse_upload &&
        packed_tail_readback && SUCCEEDED(packed_tail_reserved_texture_hr) &&
        SUCCEEDED(packed_tail_reserved_tiling_hr) &&
        packed_tail_total_tiles == 22 && packed_tail_info.NumStandardMips == 3 &&
        packed_tail_info.NumPackedMips == 1) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT packed_tail_layout_guard[2] = {};
        UINT packed_tail_rows_guard[2] = {};
        UINT64 packed_tail_row_bytes_guard[2] = {};
        packed_tail_layout_guard[1].Offset = UINT64_C(0xdeadbeef);
        packed_tail_rows_guard[1] = 0xdeadbeef;
        packed_tail_row_bytes_guard[1] = UINT64_C(0xfeedface);
        device->GetCopyableFootprints(
            &packed_tail_desc, 3, 1, 0, packed_tail_layout_guard,
            packed_tail_rows_guard, packed_tail_row_bytes_guard,
            &packed_tail_copy_bytes);
        packed_tail_footprint = packed_tail_layout_guard[0];
        packed_tail_rows = packed_tail_rows_guard[0];
        packed_tail_row_bytes = packed_tail_row_bytes_guard[0];
        packed_tail_footprint_ok =
            packed_tail_layout_guard[1].Offset == UINT64_C(0xdeadbeef) &&
            packed_tail_rows_guard[1] == 0xdeadbeef &&
            packed_tail_row_bytes_guard[1] == UINT64_C(0xfeedface) &&
            packed_tail_footprint.Offset == 0 &&
            packed_tail_footprint.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            packed_tail_footprint.Footprint.Width == 64 &&
            packed_tail_footprint.Footprint.Height == 64 &&
            packed_tail_footprint.Footprint.Depth == 1 &&
            packed_tail_footprint.Footprint.RowPitch == 256 &&
            packed_tail_rows == 64 && packed_tail_row_bytes == 256 &&
            packed_tail_copy_bytes == 16384;
        D3D12_TILED_RESOURCE_COORDINATE packed_tail_coordinate = {};
        packed_tail_coordinate.Subresource = 3;
        D3D12_TILE_REGION_SIZE packed_tail_region = {};
        packed_tail_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS packed_tail_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT packed_tail_heap_offset = 0;
        UINT packed_tail_range_count = 1;
        queue->UpdateTileMappings(
            packed_tail_reserved_texture, 1, &packed_tail_coordinate,
            &packed_tail_region, sparse_heap, 1, &packed_tail_range_flag,
            &packed_tail_heap_offset, &packed_tail_range_count,
            D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TEXTURE_COPY_LOCATION packed_tail_upload_location = {};
        packed_tail_upload_location.pResource = sparse_upload;
        packed_tail_upload_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        packed_tail_upload_location.PlacedFootprint = packed_tail_footprint;
        D3D12_TEXTURE_COPY_LOCATION packed_tail_texture_location = {};
        packed_tail_texture_location.pResource = packed_tail_reserved_texture;
        packed_tail_texture_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        packed_tail_texture_location.SubresourceIndex = 3;
        list->CopyTextureRegion(&packed_tail_texture_location, 0, 0, 0,
                                &packed_tail_upload_location, nullptr);
        D3D12_RESOURCE_BARRIER packed_tail_barrier = transition_barrier(
            packed_tail_reserved_texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &packed_tail_barrier);
        D3D12_TEXTURE_COPY_LOCATION packed_tail_readback_location = {};
        packed_tail_readback_location.pResource = packed_tail_readback;
        packed_tail_readback_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        packed_tail_readback_location.PlacedFootprint = packed_tail_footprint;
        D3D12_TEXTURE_COPY_LOCATION packed_tail_source_location = {};
        packed_tail_source_location.pResource = packed_tail_reserved_texture;
        packed_tail_source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        packed_tail_source_location.SubresourceIndex = 3;
        list->CopyTextureRegion(&packed_tail_readback_location, 0, 0, 0,
                                &packed_tail_source_location, nullptr);
    }
    for (auto& sparse_format : sparse_format_probes) {
        if (!list || !queue || !sparse_heap || !sparse_upload || !sparse_format.texture || !sparse_format.readback ||
            FAILED(sparse_format.texture_hr) || FAILED(sparse_format.tiling_hr) || FAILED(sparse_format.readback_hr) ||
            sparse_format.total_tiles != 1 ||
            sparse_format.tile_shape.WidthInTexels != sparse_format.expected_tile_width ||
            sparse_format.tile_shape.HeightInTexels != sparse_format.expected_tile_height)
            continue;
        D3D12_TILED_RESOURCE_COORDINATE format_coordinate = {};
        D3D12_TILE_REGION_SIZE format_region = {};
        format_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS format_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT format_heap_offset = 0;
        UINT format_range_count = 1;
        queue->UpdateTileMappings(sparse_format.texture, 1, &format_coordinate, &format_region, sparse_heap, 1,
                                  &format_range_flag, &format_heap_offset, &format_range_count,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(sparse_format.texture, &format_coordinate, &format_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER format_barrier =
            transition_barrier(sparse_format.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &format_barrier);
        list->CopyTiles(sparse_format.texture, &format_coordinate, &format_region, sparse_format.readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }
    if (list && queue && sparse_heap && sparse_upload && reserved_1d_texture &&
        reserved_1d_alias_texture && reserved_1d_readback &&
        SUCCEEDED(reserved_1d_texture_hr) &&
        SUCCEEDED(reserved_1d_alias_texture_hr) && reserved_1d_total_tiles == 2 &&
        reserved_1d_tiling_count == 2 && reserved_1d_tile_shape.WidthInTexels == 16384 &&
        reserved_1d_tile_shape.HeightInTexels == 1) {
        D3D12_TILED_RESOURCE_COORDINATE reserved_1d_coordinate = {};
        D3D12_TILE_REGION_SIZE reserved_1d_region = {};
        reserved_1d_region.NumTiles = 2;
        D3D12_TILE_RANGE_FLAGS reserved_1d_range_flag = D3D12_TILE_RANGE_FLAG_NONE;
        UINT reserved_1d_heap_offset = 0;
        UINT reserved_1d_range_tiles = 2;
        queue->UpdateTileMappings(reserved_1d_texture, 1, &reserved_1d_coordinate,
                                  &reserved_1d_region, sparse_heap, 1,
                                  &reserved_1d_range_flag, &reserved_1d_heap_offset,
                                  &reserved_1d_range_tiles,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        queue->CopyTileMappings(reserved_1d_alias_texture,
                                &reserved_1d_coordinate, reserved_1d_texture,
                                &reserved_1d_coordinate, &reserved_1d_region,
                                D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(reserved_1d_texture, &reserved_1d_coordinate,
                        &reserved_1d_region, sparse_upload, 0,
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER reserved_1d_barrier =
            transition_barrier(reserved_1d_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &reserved_1d_barrier);
        D3D12_RESOURCE_BARRIER reserved_1d_alias_barrier = transition_barrier(
            reserved_1d_alias_texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &reserved_1d_alias_barrier);
        list->CopyTiles(reserved_1d_alias_texture, &reserved_1d_coordinate,
                        &reserved_1d_region, reserved_1d_readback, 0,
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }

    ID3D12Resource* bc_texture = nullptr;
    ID3D12Resource* bc_upload = nullptr;
    ID3D12Resource* bc_readback = nullptr;
    D3D12_RESOURCE_DESC bc_desc = texture_desc(7, 5, DXGI_FORMAT_BC1_UNORM);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT bc_footprint = {};
    UINT bc_rows = 0;
    UINT64 bc_row_bytes = 0;
    UINT64 bc_total_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&bc_desc, 0, 1, 0, &bc_footprint, &bc_rows, &bc_row_bytes, &bc_total_bytes);
    D3D12_RESOURCE_DESC bc_staging_desc = buffer_desc(bc_total_bytes);
    HRESULT bc_texture_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &bc_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bc_texture))
               : E_FAIL;
    HRESULT bc_upload_hr =
        device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bc_staging_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bc_upload))
               : E_FAIL;
    HRESULT bc_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &bc_staging_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bc_readback))
               : E_FAIL;
    HRESULT bc_direct_write_hr = E_FAIL;
    HRESULT bc_direct_read_hr = E_FAIL;
    bool bc_direct_io_ok = false;
    uint8_t bc_direct_data[32] = {};
    for (UINT row = 0; row < 2; ++row)
        for (UINT64 byte = 0; byte < 16; ++byte)
            bc_direct_data[row * 16 + byte] = static_cast<uint8_t>(0x31u + row * 17u + byte);
    if (bc_texture) {
        bc_direct_write_hr = bc_texture->WriteToSubresource(
            0, nullptr, bc_direct_data, 16, 32);
        uint8_t bc_direct_readback[32] = {};
        bc_direct_read_hr = bc_texture->ReadFromSubresource(
            bc_direct_readback, 16, 32, 0, nullptr);
        bc_direct_io_ok = SUCCEEDED(bc_direct_write_hr) &&
                          SUCCEEDED(bc_direct_read_hr) &&
                          std::memcmp(bc_direct_data, bc_direct_readback,
                                      sizeof(bc_direct_data)) == 0;
    }
    uint8_t* bc_upload_ptr = nullptr;
    HRESULT bc_upload_map_hr =
        bc_upload ? bc_upload->Map(0, nullptr, reinterpret_cast<void**>(&bc_upload_ptr)) : E_FAIL;
    if (SUCCEEDED(bc_upload_map_hr) && bc_upload_ptr) {
        std::memset(bc_upload_ptr, 0, static_cast<size_t>(bc_total_bytes));
        for (UINT row = 0; row < bc_rows; ++row) {
            for (UINT64 byte = 0; byte < bc_row_bytes; ++byte) {
                size_t offset = static_cast<size_t>(bc_footprint.Offset + row * bc_footprint.Footprint.RowPitch + byte);
                bc_upload_ptr[offset] = static_cast<uint8_t>(0x31u + row * 17u + byte);
            }
        }
        bc_upload->Unmap(0, nullptr);
    }
    if (list && bc_texture && bc_upload && bc_readback) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = bc_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = bc_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = bc_texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier =
            transition_barrier(bc_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION readback_dst = {};
        readback_dst.pResource = bc_readback;
        readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readback_dst.PlacedFootprint = bc_footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = bc_texture;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&readback_dst, 0, 0, 0, &texture_src, nullptr);
    }

    HRESULT close_hr = list ? list->Close() : E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT wait_hr = E_FAIL;
    if (queue && list && fence && SUCCEEDED(close_hr)) {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        execute_hr = S_OK;
        signal_hr = queue->Signal(fence, 1);
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (event_handle && SUCCEEDED(signal_hr)) {
            wait_hr = fence->SetEventOnCompletion(1, event_handle);
            if (SUCCEEDED(wait_hr))
                WaitForSingleObject(event_handle, 15000);
        }
    }
    bool sparse_unmapped_zero_ok = false;
    if (queue && sparse_mapping_queue && list && allocator && fence && sparse_mapping_fence &&
        reserved_texture && array_alias_texture && array_alias_heap && array_alias_readback &&
        SUCCEEDED(wait_hr)) {
        uint8_t *array_upload_ptr = nullptr;
        array_upload_map_hr =
            sparse_upload->Map(0, nullptr,
                               reinterpret_cast<void **>(&array_upload_ptr));
        if (SUCCEEDED(array_upload_map_hr) && array_upload_ptr) {
            for (UINT64 i = 0; i < sparse_tile_size; ++i)
                array_upload_ptr[sparse_tile_size + i] =
                    static_cast<uint8_t>((i * 53u + 11u) & 0xffu);
            sparse_upload->Unmap(0, nullptr);
        }
        D3D12_TILED_RESOURCE_COORDINATE array_source_coordinates[2] = {};
        array_source_coordinates[1].Subresource = 1;
        D3D12_TILE_REGION_SIZE array_source_regions[2] = {};
        array_source_regions[0].NumTiles = 1;
        array_source_regions[1].NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS array_source_flags[2] = {
            D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NONE};
        UINT array_source_heap_offsets[2] = {0, 1};
        UINT array_source_range_counts[2] = {1, 1};
        queue->UpdateTileMappings(reserved_texture, 2,
                                  array_source_coordinates,
                                  array_source_regions, array_alias_heap, 2,
                                  array_source_flags, array_source_heap_offsets,
                                  array_source_range_counts,
                                  D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TILED_RESOURCE_COORDINATE array_destination = {};
        D3D12_TILED_RESOURCE_COORDINATE array_source = {};
        D3D12_TILE_REGION_SIZE array_region = {};
        array_region.NumTiles = 2;
        queue->CopyTileMappings(array_alias_texture, &array_destination,
                                reserved_texture, &array_source, &array_region,
                                D3D12_TILE_MAPPING_FLAG_NONE);
        array_mapping_signal_hr = queue->Signal(sparse_mapping_fence, 1);
        array_mapping_wait_hr = sparse_mapping_queue->Wait(
            sparse_mapping_fence, 1);
        array_mapping_close_hr =
            (SUCCEEDED(array_mapping_signal_hr) && SUCCEEDED(array_mapping_wait_hr))
                ? list->Reset(allocator, nullptr)
                : E_FAIL;
        if (SUCCEEDED(array_mapping_close_hr)) {
            D3D12_RESOURCE_BARRIER source_to_copy =
                transition_barrier(reserved_texture,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &source_to_copy);
            list->CopyTiles(reserved_texture, &array_source_coordinates[0],
                            &array_source_regions[0], sparse_upload, 0,
                            D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
            list->CopyTiles(reserved_texture, &array_source_coordinates[1],
                            &array_source_regions[1], sparse_upload,
                            sparse_tile_size,
                            D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
            D3D12_RESOURCE_BARRIER source_to_read =
                transition_barrier(reserved_texture,
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &source_to_read);
            D3D12_RESOURCE_BARRIER aliasing_barrier = {};
            aliasing_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            aliasing_barrier.Aliasing.pResourceBefore = reserved_texture;
            aliasing_barrier.Aliasing.pResourceAfter = array_alias_texture;
            list->ResourceBarrier(1, &aliasing_barrier);
            D3D12_RESOURCE_BARRIER array_barrier =
                transition_barrier(array_alias_texture,
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &array_barrier);
            D3D12_TILED_RESOURCE_COORDINATE array_read_coordinates[2] = {};
            array_read_coordinates[1].Subresource = 1;
            D3D12_TILE_REGION_SIZE array_read_region = {};
            array_read_region.NumTiles = 1;
            list->CopyTiles(array_alias_texture, &array_read_coordinates[0],
                            &array_read_region, array_alias_readback, 0,
                            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            list->CopyTiles(array_alias_texture, &array_read_coordinates[1],
                            &array_read_region, array_alias_readback,
                            sparse_tile_size,
                            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            array_mapping_close_hr = list->Close();
        }
        if (SUCCEEDED(array_mapping_close_hr)) {
            ID3D12CommandList *lists[] = {list};
            sparse_mapping_queue->ExecuteCommandLists(1, lists);
            array_mapping_execute_hr = S_OK;
            array_mapping_signal_fence_hr =
                sparse_mapping_queue->Signal(fence, 2);
            HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (event_handle && SUCCEEDED(array_mapping_signal_fence_hr)) {
                array_mapping_wait_fence_hr =
                    fence->SetEventOnCompletion(2, event_handle);
                if (SUCCEEDED(array_mapping_wait_fence_hr) &&
                    WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
                    array_mapping_wait_fence_hr = E_FAIL;
                CloseHandle(event_handle);
            }
            uint8_t *array_upload_restore_ptr = nullptr;
            array_upload_restore_hr =
                sparse_upload->Map(
                    0, nullptr,
                    reinterpret_cast<void **>(&array_upload_restore_ptr));
            if (SUCCEEDED(array_upload_restore_hr) && array_upload_restore_ptr) {
                for (UINT64 i = 0; i < sparse_tile_size; ++i)
                    array_upload_restore_ptr[sparse_tile_size + i] =
                        static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
                sparse_upload->Unmap(0, nullptr);
            }
        }
    }
    if (queue && sparse_mapping_queue && list && allocator && fence && sparse_mapping_fence && reuse_buffer &&
        reuse_heap && reserved_buffer_reuse_readback && SUCCEEDED(wait_hr)) {
        // Map both logical tiles to one physical tile, then issue SKIP for
        // the second logical tile. A write to tile 0 must be visible through
        // tile 1, proving both REUSE_SINGLE_TILE and SKIP have provider-
        // visible semantics.
        D3D12_TILED_RESOURCE_COORDINATE reuse_coordinate = {};
        reuse_coordinate.X = 1;
        D3D12_TILE_REGION_SIZE reuse_region = {};
        reuse_region.NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS reuse_flag = D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
        UINT reuse_heap_offset = 0;
        // Omit the coordinate, region size, and range count arrays in the
        // first call to exercise D3D12's single-region/default-range forms.
        queue->UpdateTileMappings(reuse_buffer, 1, nullptr, nullptr, reuse_heap, 1,
                                  &reuse_flag, &reuse_heap_offset, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
        D3D12_TILE_RANGE_FLAGS skip_flag = D3D12_TILE_RANGE_FLAG_SKIP;
        queue->UpdateTileMappings(reuse_buffer, 1, &reuse_coordinate, &reuse_region, nullptr, 1,
                                  &skip_flag, nullptr, nullptr, D3D12_TILE_MAPPING_FLAG_NONE);
        reuse_mapping_signal_hr = queue->Signal(sparse_mapping_fence, 2);
        reuse_mapping_wait_hr = sparse_mapping_queue->Wait(sparse_mapping_fence, 2);
        reuse_skip_close_hr = (SUCCEEDED(reuse_mapping_signal_hr) && SUCCEEDED(reuse_mapping_wait_hr))
                                  ? list->Reset(allocator, nullptr)
                                  : E_FAIL;
        if (SUCCEEDED(reuse_skip_close_hr)) {
            D3D12_TILED_RESOURCE_COORDINATE source_coordinate = {};
            D3D12_TILE_REGION_SIZE one_tile = {};
            one_tile.NumTiles = 1;
            list->CopyTiles(reuse_buffer, &source_coordinate, &one_tile, sparse_upload, 0,
                            D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
            D3D12_RESOURCE_BARRIER reuse_barrier =
                transition_barrier(reuse_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &reuse_barrier);
            list->CopyTiles(reuse_buffer, &reuse_coordinate, &reuse_region, reserved_buffer_reuse_readback, 0,
                            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            reuse_skip_close_hr = list->Close();
        }
        if (SUCCEEDED(reuse_skip_close_hr)) {
            ID3D12CommandList* lists[] = {list};
            sparse_mapping_queue->ExecuteCommandLists(1, lists);
            reuse_skip_execute_hr = S_OK;
            reuse_skip_signal_hr = sparse_mapping_queue->Signal(fence, 3);
            HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (event_handle && SUCCEEDED(reuse_skip_signal_hr)) {
                reuse_skip_wait_hr = fence->SetEventOnCompletion(3, event_handle);
                if (SUCCEEDED(reuse_skip_wait_hr) && WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
                    reuse_skip_wait_hr = E_FAIL;
                CloseHandle(event_handle);
            }
        }
    }
    if (queue && list && allocator && fence && reserved_texture && sparse_unmapped_readback && SUCCEEDED(wait_hr)) {
        D3D12_TILED_RESOURCE_COORDINATE coordinates[2] = {};
        coordinates[1].Subresource = 1;
        D3D12_TILE_REGION_SIZE region_sizes[2] = {};
        region_sizes[0].NumTiles = 1;
        region_sizes[1].NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS range_flags[2] = {D3D12_TILE_RANGE_FLAG_NULL, D3D12_TILE_RANGE_FLAG_NULL};
        UINT range_tile_counts[2] = {1, 1};
        queue->UpdateTileMappings(reserved_texture, 2, coordinates, region_sizes, nullptr, 2, range_flags, nullptr,
                                  range_tile_counts, D3D12_TILE_MAPPING_FLAG_NONE);
        if (reserved_buffer && reserved_buffer_unmapped_readback) {
            D3D12_TILED_RESOURCE_COORDINATE buffer_coordinate = {};
            D3D12_TILE_REGION_SIZE buffer_region = {};
            buffer_region.NumTiles = 2;
            D3D12_TILE_RANGE_FLAGS buffer_range_flag = D3D12_TILE_RANGE_FLAG_NULL;
            UINT buffer_range_count = 2;
            queue->UpdateTileMappings(reserved_buffer, 1, &buffer_coordinate, &buffer_region, nullptr, 1,
                                      &buffer_range_flag, nullptr, &buffer_range_count, D3D12_TILE_MAPPING_FLAG_NONE);
        }
        sparse_unmap_close_hr = list->Reset(allocator, nullptr);
        if (SUCCEEDED(sparse_unmap_close_hr)) {
            list->CopyTiles(reserved_texture, &coordinates[0], &region_sizes[0], sparse_unmapped_readback, 0,
                            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            list->CopyTiles(reserved_texture, &coordinates[1], &region_sizes[1], sparse_unmapped_readback,
                            sparse_tile_size, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            if (reserved_buffer && reserved_buffer_unmapped_readback) {
                D3D12_TILED_RESOURCE_COORDINATE buffer_coordinate = {};
                D3D12_TILE_REGION_SIZE buffer_region = {};
                buffer_region.NumTiles = 2;
                list->CopyTiles(reserved_buffer, &buffer_coordinate, &buffer_region, reserved_buffer_unmapped_readback,
                                0, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            }
            sparse_unmap_close_hr = list->Close();
        }
        if (SUCCEEDED(sparse_unmap_close_hr)) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            sparse_unmap_execute_hr = S_OK;
            sparse_unmap_signal_hr = queue->Signal(fence, 4);
            HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (event_handle && SUCCEEDED(sparse_unmap_signal_hr)) {
                sparse_unmap_wait_hr = fence->SetEventOnCompletion(4, event_handle);
                if (SUCCEEDED(sparse_unmap_wait_hr) && WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
                    sparse_unmap_wait_hr = E_FAIL;
            }
        }
    }

    if (address_resource) {
        void* discarded = nullptr;
        if (SUCCEEDED(address_resource->Map(0, nullptr, &discarded)) && discarded) {
            discard_ok = true;
            for (UINT i = 0; i < 4096; ++i) {
                if (static_cast<uint8_t*>(discarded)[i] != 0) {
                    discard_ok = false;
                    break;
                }
            }
            address_resource->Unmap(0, nullptr);
        }
    }

    uint8_t* readback_ptr = nullptr;
    HRESULT map_readback_hr =
        readback_buffer ? readback_buffer->Map(0, nullptr, reinterpret_cast<void**>(&readback_ptr)) : E_FAIL;
    bool buffer_copy_ok = SUCCEEDED(map_readback_hr) && readback_ptr;
    if (buffer_copy_ok) {
        for (UINT64 i = 0; i < buffer_bytes; ++i) {
            if ((i >= 128 && i < 128 + sizeof(uint32_t)) ||
                (i >= 256 && i < 256 + sizeof(uint64_t)))
                continue;
            if (readback_ptr[i] != static_cast<uint8_t>((i * 17u + 3u) & 0xffu)) {
                buffer_copy_ok = false;
                break;
            }
        }
        uint32_t atomic_readback = 0;
        std::memcpy(&atomic_readback, readback_ptr + 128, sizeof(atomic_readback));
        atomic_copy_ok = buffer_copy_ok && atomic_readback == 0xa5c0ffeeu;
        uint64_t atomic64_readback = 0;
        std::memcpy(&atomic64_readback, readback_ptr + 256, sizeof(atomic64_readback));
        atomic64_copy_ok = buffer_copy_ok && atomic64_readback == 0x1122334455667788ull;
        readback_buffer->Unmap(0, nullptr);
    }

    uint8_t* texture_readback_ptr = nullptr;
    HRESULT texture_readback_map_hr =
        texture_readback ? texture_readback->Map(0, nullptr, reinterpret_cast<void**>(&texture_readback_ptr)) : E_FAIL;
    bool texture_copy_ok = SUCCEEDED(texture_readback_map_hr) && texture_readback_ptr;
    if (texture_copy_ok) {
        for (UINT y = 0; y < 4; ++y) {
            for (UINT x = 0; x < 4; ++x) {
                size_t offset =
                    static_cast<size_t>(texture_footprint.Offset + y * texture_footprint.Footprint.RowPitch + x * 4);
                if (texture_readback_ptr[offset + 0] != static_cast<uint8_t>(x * 40) ||
                    texture_readback_ptr[offset + 1] != static_cast<uint8_t>(y * 40) ||
                    texture_readback_ptr[offset + 2] != 0xa5 || texture_readback_ptr[offset + 3] != 0xff) {
                    texture_copy_ok = false;
                    break;
                }
            }
        }
        texture_readback->Unmap(0, nullptr);
    }

    uint8_t *direct_io_nv12_copy_readback_ptr = nullptr;
    direct_io_nv12_readback_map_hr =
        direct_io_nv12_readback
            ? direct_io_nv12_readback->Map(
                  0, nullptr,
                  reinterpret_cast<void **>(&direct_io_nv12_copy_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(direct_io_nv12_readback_map_hr) &&
        direct_io_nv12_copy_readback_ptr) {
        direct_io_nv12_copy_ok = true;
        for (UINT y = 0; y < 8 && direct_io_nv12_copy_ok; ++y) {
            for (UINT x = 0; x < 13; ++x) {
                const uint8_t expected =
                    static_cast<uint8_t>((x * 17u + y * 31u + 5u) & 0xffu);
                if (direct_io_nv12_copy_readback_ptr[y * 256 + x] !=
                    expected) {
                    direct_io_nv12_copy_ok = false;
                    break;
                }
            }
        }
        direct_io_nv12_readback->Unmap(0, nullptr);
    }
    if (direct_io_nv12_copy_destination) {
        uint8_t copied_y[13 * 8] = {};
        uint8_t copied_uv[7 * 4 * 2] = {};
        direct_io_nv12_copy_y_read_hr =
            direct_io_nv12_copy_destination->ReadFromSubresource(
                copied_y, 13, sizeof(copied_y), 0, nullptr);
        direct_io_nv12_copy_uv_read_hr =
            direct_io_nv12_copy_destination->ReadFromSubresource(
                copied_uv, 14, sizeof(copied_uv), 1, nullptr);
        direct_io_nv12_resource_copy_ok =
            SUCCEEDED(direct_io_nv12_copy_y_read_hr) &&
            SUCCEEDED(direct_io_nv12_copy_uv_read_hr);
        for (UINT y = 0; y < 8 && direct_io_nv12_resource_copy_ok; ++y) {
            for (UINT x = 0; x < 13; ++x) {
                const uint8_t expected =
                    static_cast<uint8_t>((x * 17u + y * 31u + 5u) & 0xffu);
                if (copied_y[y * 13 + x] != expected) {
                    direct_io_nv12_resource_copy_ok = false;
                    break;
                }
            }
        }
        for (UINT i = 0; i < sizeof(copied_uv) && direct_io_nv12_resource_copy_ok;
             ++i) {
            const uint8_t expected = static_cast<uint8_t>((i * 7u + 3u) & 0xffu);
            if (copied_uv[i] != expected)
                direct_io_nv12_resource_copy_ok = false;
        }
    }

    uint8_t* bc_readback_ptr = nullptr;
    HRESULT bc_readback_map_hr =
        bc_readback ? bc_readback->Map(0, nullptr, reinterpret_cast<void**>(&bc_readback_ptr)) : E_FAIL;
    bool bc_copy_ok = SUCCEEDED(bc_readback_map_hr) && bc_readback_ptr && bc_rows == 2 && bc_row_bytes == 16;
    if (SUCCEEDED(bc_readback_map_hr) && bc_readback_ptr) {
        if (bc_copy_ok) {
            for (UINT row = 0; row < bc_rows; ++row) {
                for (UINT64 byte = 0; byte < bc_row_bytes; ++byte) {
                    size_t offset =
                        static_cast<size_t>(bc_footprint.Offset + row * bc_footprint.Footprint.RowPitch + byte);
                    uint8_t expected = static_cast<uint8_t>(0x31u + row * 17u + byte);
                    if (bc_readback_ptr[offset] != expected) {
                        bc_copy_ok = false;
                        break;
                    }
                }
            }
        }
        D3D12_RANGE written = {0, 0};
        bc_readback->Unmap(0, &written);
    }

    uint8_t* sparse_readback_ptr = nullptr;
    HRESULT sparse_readback_map_hr =
        sparse_readback ? sparse_readback->Map(0, nullptr, reinterpret_cast<void**>(&sparse_readback_ptr)) : E_FAIL;
    if (SUCCEEDED(sparse_readback_map_hr) && sparse_readback_ptr) {
        sparse_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (sparse_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                sparse_copy_ok = false;
                break;
            }
        }
        sparse_readback->Unmap(0, nullptr);
    }
    uint8_t* reserved_1d_readback_ptr = nullptr;
    reserved_1d_readback_map_hr =
        reserved_1d_readback
            ? reserved_1d_readback->Map(0, nullptr,
                                         reinterpret_cast<void**>(&reserved_1d_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(reserved_1d_readback_map_hr) && reserved_1d_readback_ptr) {
        reserved_1d_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; ++i) {
            if (reserved_1d_readback_ptr[i] !=
                static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                reserved_1d_copy_ok = false;
                break;
            }
        }
        reserved_1d_mapping_copy_ok = reserved_1d_copy_ok;
        reserved_1d_readback->Unmap(0, nullptr);
    }
    uint8_t* sparse_unmapped_ptr = nullptr;
    HRESULT sparse_unmapped_map_hr =
        sparse_unmapped_readback
            ? sparse_unmapped_readback->Map(0, nullptr, reinterpret_cast<void**>(&sparse_unmapped_ptr))
            : E_FAIL;
    if (SUCCEEDED(sparse_unmapped_map_hr) && sparse_unmapped_ptr) {
        sparse_unmapped_zero_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (sparse_unmapped_ptr[i] != 0) {
                sparse_unmapped_zero_ok = false;
                break;
            }
        }
        sparse_unmapped_readback->Unmap(0, nullptr);
    }
    uint8_t* reserved_buffer_reuse_readback_ptr = nullptr;
    reserved_buffer_reuse_readback_map_hr =
        reserved_buffer_reuse_readback
            ? reserved_buffer_reuse_readback->Map(0, nullptr,
                                                  reinterpret_cast<void**>(&reserved_buffer_reuse_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(reserved_buffer_reuse_readback_map_hr) && reserved_buffer_reuse_readback_ptr) {
        reserved_buffer_reuse_first = reserved_buffer_reuse_readback_ptr[0];
        reserved_buffer_reuse_last = reserved_buffer_reuse_readback_ptr[sparse_tile_size - 1];
        reserved_buffer_reuse_skip_ok = SUCCEEDED(reuse_skip_wait_hr);
        for (UINT64 i = 0; i < sparse_tile_size && reserved_buffer_reuse_skip_ok; i++) {
            if (reserved_buffer_reuse_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                reserved_buffer_reuse_skip_ok = false;
                reserved_buffer_reuse_mismatch = i;
                reserved_buffer_reuse_actual = reserved_buffer_reuse_readback_ptr[i];
                reserved_buffer_reuse_expected = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
            }
        }
        reserved_buffer_reuse_readback->Unmap(0, nullptr);
    }
    uint8_t* reserved_buffer_readback_ptr = nullptr;
    HRESULT reserved_buffer_readback_map_hr =
        reserved_buffer_readback
            ? reserved_buffer_readback->Map(0, nullptr, reinterpret_cast<void**>(&reserved_buffer_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(reserved_buffer_readback_map_hr) && reserved_buffer_readback_ptr) {
        reserved_buffer_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (reserved_buffer_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                reserved_buffer_copy_ok = false;
                break;
            }
        }
        reserved_buffer_readback->Unmap(0, nullptr);
    }
    uint8_t* reserved_buffer_unmapped_ptr = nullptr;
    HRESULT reserved_buffer_unmapped_map_hr =
        reserved_buffer_unmapped_readback ? reserved_buffer_unmapped_readback->Map(
                                                0, nullptr, reinterpret_cast<void**>(&reserved_buffer_unmapped_ptr))
                                          : E_FAIL;
    if (SUCCEEDED(reserved_buffer_unmapped_map_hr) && reserved_buffer_unmapped_ptr) {
        reserved_buffer_unmapped_zero_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (reserved_buffer_unmapped_ptr[i] != 0) {
                reserved_buffer_unmapped_zero_ok = false;
                break;
            }
        }
        reserved_buffer_unmapped_readback->Unmap(0, nullptr);
    }
    uint8_t* mipped_reserved_readback_ptr = nullptr;
    mipped_reserved_readback_map_hr =
        mipped_reserved_readback
            ? mipped_reserved_readback->Map(0, nullptr, reinterpret_cast<void**>(&mipped_reserved_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(mipped_reserved_readback_map_hr) && mipped_reserved_readback_ptr) {
        mipped_reserved_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_size; i++) {
            if (mipped_reserved_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                mipped_reserved_copy_ok = false;
                break;
            }
        }
        mipped_reserved_readback->Unmap(0, nullptr);
    }
    uint8_t *packed_tail_readback_ptr = nullptr;
    packed_tail_readback_map_hr =
        packed_tail_readback
            ? packed_tail_readback->Map(
                  0, nullptr,
                  reinterpret_cast<void **>(&packed_tail_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(packed_tail_readback_map_hr) && packed_tail_readback_ptr) {
        packed_tail_copy_ok = true;
        for (UINT64 i = 0; i < 64 * 64 * 4; ++i) {
            if (packed_tail_readback_ptr[i] !=
                static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                packed_tail_copy_ok = false;
                break;
            }
        }
        packed_tail_readback->Unmap(0, nullptr);
    }
    uint8_t* placement_alias_readback_ptr = nullptr;
    placement_alias_readback_map_hr =
        placement_alias_readback
            ? placement_alias_readback->Map(0, nullptr, reinterpret_cast<void**>(&placement_alias_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(placement_alias_readback_map_hr) && placement_alias_readback_ptr) {
        placement_alias_copy_ok = true;
        placement_alias_first = placement_alias_readback_ptr[0];
        for (UINT64 i = 0; i < sparse_tile_size; ++i) {
            if (placement_alias_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                placement_alias_copy_ok = false;
                placement_alias_first_mismatch = i;
                break;
            }
        }
        placement_alias_readback->Unmap(0, nullptr);
    }
    uint8_t *array_alias_readback_ptr = nullptr;
    array_alias_readback_map_hr =
        array_alias_readback
            ? array_alias_readback->Map(
                  0, nullptr,
                  reinterpret_cast<void **>(&array_alias_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(array_alias_readback_map_hr) && array_alias_readback_ptr) {
        array_alias_first = array_alias_readback_ptr[0];
        array_alias_second = array_alias_readback_ptr[sparse_tile_size];
        array_alias_copy_ok = SUCCEEDED(array_mapping_wait_fence_hr);
        for (UINT tile = 0; tile < 2 && array_alias_copy_ok; ++tile) {
            for (UINT64 i = 0; i < sparse_tile_size; ++i) {
                const UINT64 offset = UINT64(tile) * sparse_tile_size + i;
                const uint8_t expected =
                    tile == 0
                        ? static_cast<uint8_t>((i * 29u + 7u) & 0xffu)
                        : static_cast<uint8_t>((i * 53u + 11u) & 0xffu);
                if (array_alias_readback_ptr[offset] != expected) {
                    array_alias_mismatch = offset;
                    array_alias_mismatch_actual = array_alias_readback_ptr[offset];
                    array_alias_mismatch_expected = expected;
                    array_alias_copy_ok = false;
                    break;
                }
            }
        }
        array_alias_readback->Unmap(0, nullptr);
    }
    uint8_t* volume_readback_ptr = nullptr;
    volume_readback_map_hr =
        volume_readback ? volume_readback->Map(0, nullptr, reinterpret_cast<void**>(&volume_readback_ptr)) : E_FAIL;
    if (SUCCEEDED(volume_readback_map_hr) && volume_readback_ptr) {
        volume_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; ++i) {
            if (volume_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                volume_copy_ok = false;
                break;
            }
        }
        volume_readback->Unmap(0, nullptr);
    }
    uint8_t* volume_alias_readback_ptr = nullptr;
    volume_alias_readback_map_hr =
        volume_alias_readback
            ? volume_alias_readback->Map(0, nullptr, reinterpret_cast<void**>(&volume_alias_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(volume_alias_readback_map_hr) && volume_alias_readback_ptr) {
        volume_alias_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; ++i) {
            if (volume_alias_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                volume_alias_copy_ok = false;
                break;
            }
        }
        volume_alias_readback->Unmap(0, nullptr);
    }
    volume_physical_page_ownership_ok =
        volume_copy_ok && volume_alias_copy_ok && volume_total_tiles == 2 && volume_tile_shape.DepthInTexels == 16;
    uint8_t* mapping_copy_readback_ptr = nullptr;
    mapping_copy_readback_map_hr =
        mapping_copy_readback
            ? mapping_copy_readback->Map(0, nullptr, reinterpret_cast<void**>(&mapping_copy_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(mapping_copy_readback_map_hr) && mapping_copy_readback_ptr) {
        mapping_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_size; i++) {
            if (mapping_copy_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                mapping_copy_ok = false;
                break;
            }
        }
        mapping_copy_readback->Unmap(0, nullptr);
    }
    uint8_t* r8_reserved_readback_ptr = nullptr;
    r8_reserved_readback_map_hr =
        r8_reserved_readback
            ? r8_reserved_readback->Map(0, nullptr, reinterpret_cast<void**>(&r8_reserved_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(r8_reserved_readback_map_hr) && r8_reserved_readback_ptr) {
        r8_reserved_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_size; i++) {
            if (r8_reserved_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                r8_reserved_copy_ok = false;
                break;
            }
        }
        r8_reserved_readback->Unmap(0, nullptr);
    }
    uint8_t* r8_mipped_readback_ptr = nullptr;
    r8_mipped_readback_map_hr =
        r8_mipped_readback ? r8_mipped_readback->Map(0, nullptr, reinterpret_cast<void**>(&r8_mipped_readback_ptr))
                           : E_FAIL;
    if (SUCCEEDED(r8_mipped_readback_map_hr) && r8_mipped_readback_ptr) {
        r8_mipped_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_size; i++) {
            if (r8_mipped_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                r8_mipped_copy_ok = false;
                break;
            }
        }
        r8_mipped_readback->Unmap(0, nullptr);
    }
    uint8_t* r8_partial_readback_ptr = nullptr;
    r8_partial_readback_map_hr =
        r8_partial_readback ? r8_partial_readback->Map(0, nullptr, reinterpret_cast<void**>(&r8_partial_readback_ptr))
                            : E_FAIL;
    if (SUCCEEDED(r8_partial_readback_map_hr) && r8_partial_readback_ptr) {
        r8_partial_copy_ok = true;
        for (UINT y = 0; y < 128; ++y) {
            for (UINT x = 0; x < 128; ++x) {
                const UINT8 expected = static_cast<UINT8>(((y * 128u + x) * 29u + 7u) & 0xffu);
                const UINT8 actual = r8_partial_readback_ptr[r8_partial_upload_footprint.Offset +
                                                             y * r8_partial_upload_footprint.Footprint.RowPitch + x];
                if (actual != expected) {
                    r8_partial_copy_ok = false;
                    break;
                }
            }
            if (!r8_partial_copy_ok)
                break;
        }
        r8_partial_readback->Unmap(0, nullptr);
    }
    for (auto& sparse_format : sparse_format_probes) {
        uint8_t* format_readback_ptr = nullptr;
        sparse_format.readback_map_hr =
            sparse_format.readback
                ? sparse_format.readback->Map(0, nullptr, reinterpret_cast<void**>(&format_readback_ptr))
                : E_FAIL;
        if (SUCCEEDED(sparse_format.readback_map_hr) && format_readback_ptr) {
            sparse_format.copy_ok = true;
            for (UINT64 i = 0; i < sparse_tile_size; i++) {
                if (format_readback_ptr[i] != static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                    sparse_format.copy_ok = false;
                    break;
                }
            }
            sparse_format.readback->Unmap(0, nullptr);
        }
    }

    D3D12_RESOURCE_DESC texture_roundtrip_desc = texture ? texture->GetDesc() : D3D12_RESOURCE_DESC{};
    texture_gpu_va = texture ? texture->GetGPUVirtualAddress() : 1;

    std::vector<FormatProbe> formats = {
        {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
        {"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM},
        {"B8G8R8X8_UNORM", DXGI_FORMAT_B8G8R8X8_UNORM},
        {"B5G6R5_UNORM", DXGI_FORMAT_B5G6R5_UNORM},
        {"B5G5R5A1_UNORM", DXGI_FORMAT_B5G5R5A1_UNORM},
        {"B4G4R4A4_UNORM", DXGI_FORMAT_B4G4R4A4_UNORM},
        {"R9G9B9E5_SHAREDEXP", DXGI_FORMAT_R9G9B9E5_SHAREDEXP},
        {"R8G8_B8G8_UNORM", DXGI_FORMAT_R8G8_B8G8_UNORM},
        {"G8R8_G8B8_UNORM", DXGI_FORMAT_G8R8_G8B8_UNORM},
        {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
        {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT, E_FAIL, E_FAIL, 0, 0, 0, 2},
        {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
        {"R32_UINT", DXGI_FORMAT_R32_UINT},
    };
    for (auto& format : formats) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
        support.Format = format.format;
        format.hr =
            device ? device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)) : E_FAIL;
        format.support1 = support.Support1;
        format.support2 = support.Support2;
        D3D12_FEATURE_DATA_FORMAT_INFO format_info = {};
        format_info.Format = format.format;
        format.format_info_hr = device ? device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &format_info,
                                                                       sizeof(format_info))
                                       : E_FAIL;
        format.plane_count = format_info.PlaneCount;
    }

    bool format_support_ok = true;
    for (const auto& format : formats) {
        if (FAILED(format.hr) || FAILED(format.format_info_hr) ||
            format.plane_count != format.expected_plane_count)
            format_support_ok = false;
    }
    bool sparse_format_matrix_ok = !sparse_format_probes.empty();
    for (const auto& sparse_format : sparse_format_probes) {
        sparse_format_matrix_ok = sparse_format_matrix_ok && SUCCEEDED(sparse_format.texture_hr) &&
                                  SUCCEEDED(sparse_format.tiling_hr) && SUCCEEDED(sparse_format.readback_hr) &&
                                  SUCCEEDED(sparse_format.readback_map_hr) && sparse_format.total_tiles == 1 &&
                                  sparse_format.copy_ok;
    }

    const bool default_cpu_io_ok = default_cpu_io_verified;
    D3D12_RESOURCE_DESC shared_open_desc = {};
    D3D12_RESOURCE_DESC shared_named_open_desc = {};
    if (shared_open_buffer)
        shared_open_buffer->GetDesc(&shared_open_desc);
    if (shared_named_open_buffer)
        shared_named_open_buffer->GetDesc(&shared_named_open_desc);
    const bool shared_handle_roundtrip =
        SUCCEEDED(shared_create_hr) && SUCCEEDED(shared_open_hr) && SUCCEEDED(shared_open_named_hr) && shared_handle &&
        shared_named_handle && shared_open_buffer && shared_named_open_buffer &&
        shared_open_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        shared_open_desc.Width == buffer_bytes &&
        shared_named_open_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        shared_named_open_desc.Width == buffer_bytes && shared_independent_object_ok &&
        shared_unknown_hr == DXGI_ERROR_INVALID_CALL && shared_missing_name_hr == DXGI_ERROR_NOT_FOUND &&
        shared_invalid_create_access_hr == E_INVALIDARG &&
        shared_invalid_open_access_hr == E_INVALIDARG;
    if (shared_handle)
        CloseHandle(shared_handle);
    if (shared_named_handle)
        CloseHandle(shared_named_handle);

    const bool resource_validation_ok =
        FAILED(invalid_zero_width_hr) &&
        oversized_1d_hr == E_INVALIDARG && oversized_2d_hr == E_INVALIDARG &&
        oversized_3d_hr == E_INVALIDARG &&
        invalid_committed_heap_flags_hr == E_INVALIDARG &&
        invalid_heap_properties_hr == E_INVALIDARG &&
        invalid_node_mask_hr == E_INVALIDARG &&
        invalid_upload_state_hr == E_INVALIDARG &&
        invalid_readback_state_hr == E_INVALIDARG &&
        invalid_buffer_resource_flags_hr == E_INVALIDARG &&
        invalid_texture_resource_flags_hr == E_INVALIDARG &&
        invalid_depth_format_flags_hr == E_INVALIDARG &&
        invalid_clear_without_flag_hr == E_INVALIDARG &&
        invalid_clear_format_hr == E_INVALIDARG &&
        null_heap_properties_hr == E_INVALIDARG &&
        FAILED(invalid_msaa_mips_hr) &&
        invalid_zero_width_allocation_size == 0 &&
        invalid_zero_width_allocation_alignment == 0 &&
        invalid_map_range_ok &&
        allocation_overflow_size == 0 && allocation_overflow_alignment == 0 &&
        allocation_batch_ok && invalid_msaa_mips_allocation_size == 0 &&
        invalid_msaa_mips_allocation_alignment == 0 &&
        volume_allocation_size == 64 * 1024 &&
        volume_allocation_alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        null_allocation_size == 0 && null_allocation_alignment == 0 &&
        invalid_footprint_total == UINT64_MAX &&
        footprint_overflow_total == UINT64_MAX && planar_footprint_ok &&
        planar_footprint_total != 0 && nv12_footprint_ok &&
        invalid_nv12_height_hr == E_INVALIDARG &&
        SUCCEEDED(null_sideband_query_hr) && null_sideband_size == 0 &&
        null_sideband_alignment == 0 && null_sideband_offset == 0 &&
        SUCCEEDED(tight_feature_hr) && tight_feature_tier == 1 &&
        SUCCEEDED(tight_committed_hr) && SUCCEEDED(tight_allocation_info_hr) &&
        tight_allocation_size == 1024 && tight_allocation_alignment == 256 &&
        SUCCEEDED(tight_heap_hr) && null_heap_desc_ok &&
        SUCCEEDED(tight_placed_hr) && tight_placed_roundtrip_ok &&
        tight_invalid_alignment_hr == E_INVALIDARG &&
        tight_overaligned_placed_hr == E_INVALIDARG &&
        tight_reserved_hr == E_INVALIDARG && full_mip_create_hr == S_OK &&
        full_mip_count == 6 && full_mip_footprint_ok &&
        SUCCEEDED(not_resident_committed_hr) &&
        not_resident_initial_map_hr == DXGI_ERROR_INVALID_CALL &&
        SUCCEEDED(not_resident_make_resident_hr) &&
        SUCCEEDED(not_resident_remade_map_hr) && SUCCEEDED(not_resident_heap_hr) &&
        SUCCEEDED(not_resident_placed_hr) && not_resident_roundtrip_ok &&
        SUCCEEDED(residency_fence_hr) && SUCCEEDED(enqueue_make_resident_hr) &&
        invalid_enqueue_flags_hr == E_INVALIDARG && enqueue_fence_completed >= 9 &&
        FAILED(invalid_heap_alignment_hr) && FAILED(invalid_heap_flags_hr) &&
        invalid_heap_size_hr == E_INVALIDARG && FAILED(misaligned_placement_hr);

    bool pass =
        SUCCEEDED(create_hr) && device_luid_ok && SUCCEEDED(queue_hr) && SUCCEEDED(sparse_mapping_queue_hr) &&
        SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) && SUCCEEDED(fence_hr) &&
        SUCCEEDED(sparse_mapping_fence_hr) && SUCCEEDED(upload_buffer_hr) && SUCCEEDED(default_buffer_hr) &&
        SUCCEEDED(readback_buffer_hr) && SUCCEEDED(map_upload_hr) && SUCCEEDED(close_hr) && SUCCEEDED(execute_hr) &&
        SUCCEEDED(signal_hr) && SUCCEEDED(wait_hr) && SUCCEEDED(map_readback_hr) && buffer_copy_ok &&
        SUCCEEDED(texture_hr) && SUCCEEDED(texture_upload_hr) && SUCCEEDED(texture_readback_hr) &&
        SUCCEEDED(texture_map_hr) && SUCCEEDED(texture_readback_map_hr) && texture_copy_ok &&
        SUCCEEDED(direct_io_texture_create_hr) && SUCCEEDED(direct_io_texture_write_hr) &&
        SUCCEEDED(direct_io_texture_read_hr) && direct_io_texture_ok &&
        SUCCEEDED(direct_io_volume_create_hr) && SUCCEEDED(direct_io_volume_write_hr) &&
        SUCCEEDED(direct_io_volume_read_hr) && direct_io_volume_ok &&
        SUCCEEDED(direct_io_volume_box_write_hr) &&
        SUCCEEDED(direct_io_volume_box_read_hr) && direct_io_volume_box_ok &&
        SUCCEEDED(direct_io_d32s8_create_hr) &&
        SUCCEEDED(direct_io_d32s8_write_hr[0]) &&
        SUCCEEDED(direct_io_d32s8_write_hr[1]) &&
        SUCCEEDED(direct_io_d32s8_read_hr[0]) &&
        SUCCEEDED(direct_io_d32s8_read_hr[1]) && direct_io_d32s8_ok &&
        SUCCEEDED(direct_io_nv12_create_hr) && direct_io_nv12_ok &&
        SUCCEEDED(direct_io_nv12_upload_hr) &&
        SUCCEEDED(direct_io_nv12_readback_hr) &&
        SUCCEEDED(direct_io_nv12_upload_map_hr) &&
        SUCCEEDED(direct_io_nv12_readback_map_hr) && direct_io_nv12_copy_ok &&
        SUCCEEDED(direct_io_nv12_copy_destination_hr) &&
        SUCCEEDED(direct_io_nv12_copy_y_read_hr) &&
        SUCCEEDED(direct_io_nv12_copy_uv_read_hr) &&
        direct_io_nv12_resource_copy_ok &&
        SUCCEEDED(direct_io_p010_create_hr) && direct_io_p010_ok &&
        direct_io_variant_ok &&
        SUCCEEDED(direct_io_variant_create_hr[0]) &&
        SUCCEEDED(direct_io_variant_create_hr[1]) &&
        SUCCEEDED(direct_io_variant_create_hr[2]) &&
        SUCCEEDED(direct_io_variant_create_hr[3]) &&
        SUCCEEDED(direct_io_variant_write_hr[0]) &&
        SUCCEEDED(direct_io_variant_write_hr[1]) &&
        SUCCEEDED(direct_io_variant_write_hr[2]) &&
        SUCCEEDED(direct_io_variant_write_hr[3]) &&
        SUCCEEDED(direct_io_variant_read_hr[0]) &&
        SUCCEEDED(direct_io_variant_read_hr[1]) &&
        SUCCEEDED(direct_io_variant_read_hr[2]) &&
        SUCCEEDED(direct_io_variant_read_hr[3]) &&
        SUCCEEDED(bc_texture_hr) && SUCCEEDED(bc_upload_hr) && SUCCEEDED(bc_readback_hr) &&
        SUCCEEDED(bc_upload_map_hr) && SUCCEEDED(bc_readback_map_hr) && bc_copy_ok &&
        SUCCEEDED(bc_direct_write_hr) && SUCCEEDED(bc_direct_read_hr) && bc_direct_io_ok &&
        SUCCEEDED(sparse_heap_hr) &&
        SUCCEEDED(reserved_texture_hr) && SUCCEEDED(reserved_1d_texture_hr) &&
        SUCCEEDED(reserved_1d_tiling_hr) && reserved_1d_total_tiles == 2 &&
        reserved_1d_tiling_count == 2 && reserved_1d_tile_shape.WidthInTexels == 16384 &&
        reserved_1d_tile_shape.HeightInTexels == 1 &&
        SUCCEEDED(reserved_1d_alias_texture_hr) &&
        reserved_1d_tilings[0].WidthInTiles == 1 &&
        reserved_1d_tilings[0].StartTileIndexInOverallResource == 0 &&
        reserved_1d_tilings[1].WidthInTiles == 1 &&
        reserved_1d_tilings[1].StartTileIndexInOverallResource == 1 &&
        SUCCEEDED(reserved_1d_readback_hr) &&
        SUCCEEDED(reserved_1d_readback_map_hr) && reserved_1d_copy_ok &&
        reserved_1d_mapping_copy_ok &&
        SUCCEEDED(placement_alias_texture_hr) &&
        SUCCEEDED(array_alias_heap_hr) && SUCCEEDED(array_alias_texture_hr) &&
        SUCCEEDED(array_alias_readback_hr) && SUCCEEDED(array_alias_readback_map_hr) &&
        SUCCEEDED(array_upload_map_hr) && SUCCEEDED(array_upload_restore_hr) &&
        array_alias_copy_ok && SUCCEEDED(array_mapping_signal_hr) &&
        SUCCEEDED(array_mapping_wait_hr) && SUCCEEDED(array_mapping_close_hr) &&
        SUCCEEDED(array_mapping_execute_hr) && SUCCEEDED(array_mapping_signal_fence_hr) &&
        SUCCEEDED(array_mapping_wait_fence_hr) &&
        SUCCEEDED(placement_alias_readback_hr) && SUCCEEDED(placement_alias_readback_map_hr) &&
        placement_alias_copy_ok && SUCCEEDED(volume_heap_hr) && SUCCEEDED(volume_texture_hr) &&
        SUCCEEDED(volume_alias_texture_hr) && SUCCEEDED(volume_tiling_hr) && SUCCEEDED(volume_alias_tiling_hr) &&
        SUCCEEDED(volume_readback_hr) && SUCCEEDED(volume_alias_readback_hr) && SUCCEEDED(volume_readback_map_hr) &&
        SUCCEEDED(volume_alias_readback_map_hr) && volume_physical_page_ownership_ok && SUCCEEDED(sparse_tiling_hr) &&
        SUCCEEDED(sparse_upload_hr) && SUCCEEDED(sparse_readback_hr) && SUCCEEDED(sparse_upload_map_hr) &&
        SUCCEEDED(sparse_readback_map_hr) && sparse_copy_ok && SUCCEEDED(sparse_unmapped_readback_hr) &&
        SUCCEEDED(reserved_buffer_hr) && SUCCEEDED(reserved_buffer_tiling_hr) &&
        SUCCEEDED(reuse_heap_hr) && SUCCEEDED(reuse_buffer_hr) &&
        SUCCEEDED(reuse_mapping_signal_hr) && SUCCEEDED(reuse_mapping_wait_hr) &&
        SUCCEEDED(reserved_buffer_readback_hr) && SUCCEEDED(reserved_buffer_reuse_readback_hr) &&
        SUCCEEDED(reserved_buffer_reuse_readback_map_hr) && SUCCEEDED(reserved_buffer_unmapped_readback_hr) &&
        SUCCEEDED(reserved_buffer_readback_map_hr) && SUCCEEDED(reserved_buffer_unmapped_map_hr) &&
        reserved_buffer_copy_ok && reserved_buffer_reuse_skip_ok && reserved_buffer_unmapped_zero_ok &&
        SUCCEEDED(copy_mapping_heap_hr) &&
        SUCCEEDED(mapping_copy_source_hr) && SUCCEEDED(mapping_copy_destination_hr) &&
        SUCCEEDED(mapping_copy_readback_hr) && SUCCEEDED(mapping_copy_readback_map_hr) && mapping_copy_ok &&
        SUCCEEDED(r8_reserved_texture_hr) && SUCCEEDED(r8_reserved_tiling_hr) && SUCCEEDED(r8_reserved_readback_hr) &&
        SUCCEEDED(r8_reserved_readback_map_hr) && r8_reserved_copy_ok && r8_reserved_total_tiles == 1 &&
        r8_reserved_tiling_count == 1 && r8_reserved_tile_shape.WidthInTexels == 256 &&
        r8_reserved_tile_shape.HeightInTexels == 256 && SUCCEEDED(r8_mipped_heap_hr) &&
        SUCCEEDED(r8_mipped_texture_hr) && SUCCEEDED(r8_mipped_tiling_hr) && SUCCEEDED(r8_mipped_readback_hr) &&
        SUCCEEDED(r8_mipped_readback_map_hr) && r8_mipped_copy_ok && r8_mipped_total_tiles == 5 &&
        r8_mipped_tiling_count == 2 && r8_mipped_tile_shape.WidthInTexels == 256 &&
        r8_mipped_tile_shape.HeightInTexels == 256 && r8_mipped_tilings[0].WidthInTiles == 2 &&
        r8_mipped_tilings[0].HeightInTiles == 2 && r8_mipped_tilings[1].WidthInTiles == 1 &&
        r8_mipped_tilings[1].HeightInTiles == 1 && r8_mipped_tilings[1].StartTileIndexInOverallResource == 4 &&
        SUCCEEDED(r8_partial_texture_hr) && SUCCEEDED(r8_partial_upload_hr) && SUCCEEDED(r8_partial_readback_hr) &&
        SUCCEEDED(r8_partial_upload_map_hr) && SUCCEEDED(r8_partial_tiling_hr) &&
        SUCCEEDED(r8_partial_readback_map_hr) && r8_partial_copy_ok && r8_partial_total_tiles == 2 &&
        r8_partial_tiling_count == 2 && r8_partial_packed_mips.NumStandardMips == 1 &&
        r8_partial_packed_mips.NumPackedMips == 1 && r8_partial_packed_mips.NumTilesForPackedMips == 1 &&
        r8_partial_packed_mips.StartTileIndexInOverallResource == 1 && r8_partial_tilings[0].WidthInTiles == 1 &&
        r8_partial_tilings[1].WidthInTiles == 0 &&
        r8_partial_tilings[1].StartTileIndexInOverallResource == D3D12_PACKED_TILE &&
        reserved_buffer_total_tiles == 2 && reserved_buffer_tiling_count == 1 &&
        reserved_buffer_tile_shape.WidthInTexels == sparse_tile_size && SUCCEEDED(mipped_reserved_texture_hr) &&
        SUCCEEDED(mipped_reserved_tiling_hr) && SUCCEEDED(mipped_reserved_readback_hr) &&
        SUCCEEDED(mipped_reserved_readback_map_hr) && mipped_reserved_copy_ok && mipped_reserved_total_tiles == 5 &&
        mipped_reserved_tiling_count == 2 && SUCCEEDED(packed_tail_reserved_texture_hr) &&
        SUCCEEDED(packed_tail_reserved_tiling_hr) && packed_tail_total_tiles == 22 &&
        packed_tail_tiling_count == 4 && packed_tail_info.NumStandardMips == 3 &&
        packed_tail_info.NumPackedMips == 1 && packed_tail_info.NumTilesForPackedMips == 1 &&
        packed_tail_info.StartTileIndexInOverallResource == 21 &&
        packed_tail_shape.WidthInTexels == 128 && packed_tail_shape.HeightInTexels == 128 &&
        SUCCEEDED(packed_tail_readback_hr) &&
        SUCCEEDED(packed_tail_readback_map_hr) && packed_tail_footprint_ok &&
        packed_tail_copy_ok &&
        packed_tail_tilings[0].WidthInTiles == 4 && packed_tail_tilings[1].WidthInTiles == 2 &&
        packed_tail_tilings[2].WidthInTiles == 1 && packed_tail_tilings[3].WidthInTiles == 0 &&
        packed_tail_tilings[3].StartTileIndexInOverallResource == D3D12_PACKED_TILE &&
        packed_tail_partial_query_ok && SUCCEEDED(sparse_unmap_close_hr) &&
        SUCCEEDED(sparse_unmap_signal_hr) && SUCCEEDED(sparse_unmap_wait_hr) && SUCCEEDED(sparse_unmapped_map_hr) &&
        sparse_unmapped_zero_ok && command_resource_lifetime_ok &&
        SUCCEEDED(residency_descriptor_heap_create_hr) &&
        SUCCEEDED(residency_query_heap_create_hr) &&
        SUCCEEDED(residency_descriptor_heap_evict_hr) &&
        SUCCEEDED(residency_descriptor_heap_make_hr) &&
        SUCCEEDED(residency_query_heap_evict_hr) &&
        SUCCEEDED(residency_query_heap_make_hr) &&
        SUCCEEDED(residency_pageable_priority_hr) &&
        default_cpu_io_ok && residency_state_ok && dxgi_offer_reclaim_ok &&
        heap_residency_ok && address_heap_open_ok && heap_aliasing_ok &&
        atomic_copy_ok && atomic64_copy_ok && discard_ok &&
        resource_shapes_ok && resource_validation_ok &&
        SUCCEEDED(placed_1d_array_heap_hr) &&
        SUCCEEDED(placed_1d_array_resource_hr) &&
        SUCCEEDED(placed_1d_array_write_hr[0]) &&
        SUCCEEDED(placed_1d_array_write_hr[1]) &&
        SUCCEEDED(placed_1d_array_read_hr[0]) &&
        SUCCEEDED(placed_1d_array_read_hr[1]) &&
        placed_1d_array_io_ok &&
        sparse_total_tiles == 2 && sparse_tiling_count == 2 &&
        sparse_tile_shape.WidthInTexels == 128 && sparse_tile_shape.HeightInTexels == 128 &&
        sparse_tiling[0].WidthInTiles == 1 && sparse_tiling[0].HeightInTiles == 1 &&
        sparse_tiling[1].WidthInTiles == 1 && sparse_tiling[1].HeightInTiles == 1 &&
        default_buffer_desc.Width == buffer_bytes && texture_roundtrip_desc.Width == 4 &&
        texture_roundtrip_desc.Height == 4 && upload_gpu_va != 0 && default_gpu_va != 0 && texture_gpu_va == 0 && shared_handle_roundtrip &&
        format_support_ok && sparse_format_matrix_ok && unsupported_texture_rejected && cross_process_shared_ok &&
        shared_heap_roundtrip_ok && unnamed_shared_heap_roundtrip_ok &&
        unnamed_shared_roundtrip_ok && SUCCEEDED(unnamed_shared_source_hr) &&
        SUCCEEDED(unnamed_shared_create_hr) &&
        SUCCEEDED(unnamed_shared_open_hr) &&
        SUCCEEDED(unnamed_shared_map_hr) &&
        SUCCEEDED(shared_fence_create_hr) &&
        SUCCEEDED(shared_fence_handle_hr) && SUCCEEDED(shared_fence_signal_hr) &&
        SUCCEEDED(shared_fence_mapping_signal_hr) &&
        SUCCEEDED(shared_fence_wait_hr) &&
        unnamed_shared_fence_roundtrip_ok && shared_heap_cross_process_ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-resources.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"device_create\": {\n");
    print_hr("hr", create_hr);
    std::printf("    \"adapter_luid\": [%ld, %lu],\n", static_cast<long>(device_luid.HighPart),
                static_cast<unsigned long>(device_luid.LowPart));
    std::printf("    \"adapter_luid_verified\": %s\n", device_luid_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"command_execution\": {\n");
    print_hr("queue", queue_hr);
    print_hr("sparse_mapping_queue", sparse_mapping_queue_hr);
    print_hr("allocator", allocator_hr);
    print_hr("list", list_hr);
    print_hr("fence", fence_hr);
    print_hr("sparse_mapping_fence", sparse_mapping_fence_hr);
    print_hr("close", close_hr);
    print_hr("execute", execute_hr);
    print_hr("signal", signal_hr);
    print_hr("wait", wait_hr, false);
    std::printf("  },\n");
    std::printf("  \"buffers\": {\n");
    print_hr("upload_create", upload_buffer_hr);
    print_hr("default_create", default_buffer_hr);
    print_hr("readback_create", readback_buffer_hr);
    print_hr("upload_map", map_upload_hr);
    print_hr("readback_map", map_readback_hr);
    print_hr("default_write_to_subresource", default_write_subresource_hr);
    print_hr("default_read_from_subresource", default_read_subresource_hr);
    std::printf("    \"copy_verified\": %s,\n", buffer_copy_ok ? "true" : "false");
    std::printf("    \"atomic_copy_verified\": %s,\n", atomic_copy_ok ? "true" : "false");
    std::printf("    \"atomic64_copy_verified\": %s,\n", atomic64_copy_ok ? "true" : "false");
    std::printf("    \"discard_verified\": %s,\n", discard_ok ? "true" : "false");
    std::printf("    \"default_desc_width\": %llu,\n", static_cast<unsigned long long>(default_buffer_desc.Width));
    std::printf("    \"upload_gpu_va_nonzero\": %s,\n", upload_gpu_va != 0 ? "true" : "false");
    std::printf("    \"default_gpu_va_nonzero\": %s,\n", default_gpu_va != 0 ? "true" : "false");
    std::printf("    \"texture_gpu_va_zero\": %s,\n", texture_gpu_va == 0 ? "true" : "false");
    std::printf("    \"command_resource_lifetime_verified\": %s,\n", command_resource_lifetime_ok ? "true" : "false");
    print_hr("list1_query", list1_hr);
    std::printf("    \"default_cpu_io_verified\": %s,\n", default_cpu_io_verified ? "true" : "false");
    print_hr("residency_make", residency_make_hr);
    print_hr("residency_priority", residency_priority_hr);
    print_hr("residency_evict", residency_evict_hr);
    print_hr("residency_first_evict_map", residency_first_evict_map_hr);
    print_hr("residency_second_evict", residency_second_evict_hr);
    print_hr("residency_evicted_map", residency_evicted_map_hr);
    print_hr("residency_remake", residency_remake_hr);
    print_hr("residency_remade_map", residency_remade_map_hr);
    print_hr("invalid_map_range", invalid_map_range_hr);
    std::printf("    \"invalid_map_range_verified\": %s,\n",
                invalid_map_range_ok ? "true" : "false");
    std::printf("    \"residency_state_verified\": %s,\n", residency_state_ok ? "true" : "false");
    print_hr("dxgi_device_query", dxgi_device_qi_hr);
    print_hr("offered_dxgi_resource_query", offered_dxgi_resource_qi_hr);
    print_hr("offered_resource_priority_set", offered_resource_priority_set_hr);
    print_hr("offered_resource_priority_get", offered_resource_priority_get_hr);
    std::printf("    \"offered_resource_priority\": %u,\n",
                offered_resource_priority);
    print_hr("offer_resources", offer_resources_hr);
    print_hr("trim_residency", trim_residency_hr);
    print_hr("query_trim_residency", query_trim_residency_hr);
    print_hr("reclaim_resources", reclaim_resources_hr);
    print_hr("query_reclaim_residency", query_reclaim_residency_hr);
    std::printf("    \"offered_resource_discarded\": %s,\n",
                offered_resource_discarded ? "true" : "false");
    std::printf("    \"dxgi_offer_reclaim_verified\": %s,\n",
                dxgi_offer_reclaim_ok ? "true" : "false");
    print_hr("address_heap_create", address_heap_hr);
    print_hr("address_resource_create", address_resource_hr);
    print_hr("address_alias_resource_create", address_alias_resource_hr);
    print_hr("address_open", address_open_hr);
    std::printf("    \"address_heap_open_verified\": %s,\n", address_heap_open_ok ? "true" : "false");
    std::printf("    \"heap_aliasing_verified\": %s,\n", heap_aliasing_ok ? "true" : "false");
    print_hr("heap_evict", address_heap_evict_hr);
    print_hr("heap_evicted_resource_map", address_heap_map_hr);
    print_hr("heap_make_resident", address_heap_make_resident_hr);
    print_hr("heap_remapped_resource_map", address_heap_remap_hr);
    std::printf("    \"heap_residency_verified\": %s\n", heap_residency_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"resource_shapes\": {\n");
    std::printf("    \"all_created_and_roundtripped\": %s,\n", resource_shapes_ok ? "true" : "false");
    std::printf("    \"validation_matrix_verified\": %s,\n", resource_validation_ok ? "true" : "false");
    std::printf("    \"footprint_matrix_verified\": %s,\n", footprint_matrix_ok ? "true" : "false");
    print_hr("placed_1d_array_heap_create", placed_1d_array_heap_hr);
    print_hr("placed_1d_array_resource_create", placed_1d_array_resource_hr);
    print_hr("placed_1d_array_first_write", placed_1d_array_write_hr[0]);
    print_hr("placed_1d_array_second_write", placed_1d_array_write_hr[1]);
    print_hr("placed_1d_array_first_read", placed_1d_array_read_hr[0]);
    print_hr("placed_1d_array_second_read", placed_1d_array_read_hr[1]);
    std::printf("    \"placed_1d_array_io_verified\": %s,\n",
                placed_1d_array_io_ok ? "true" : "false");
    print_hr("invalid_zero_width", invalid_zero_width_hr);
    print_hr("oversized_1d", oversized_1d_hr);
    print_hr("oversized_2d", oversized_2d_hr);
    print_hr("oversized_3d", oversized_3d_hr);
    print_hr("invalid_committed_heap_flags", invalid_committed_heap_flags_hr);
    print_hr("invalid_heap_properties", invalid_heap_properties_hr);
    print_hr("invalid_node_mask", invalid_node_mask_hr);
    print_hr("invalid_upload_state", invalid_upload_state_hr);
    print_hr("invalid_readback_state", invalid_readback_state_hr);
    print_hr("invalid_buffer_resource_flags", invalid_buffer_resource_flags_hr);
    print_hr("invalid_texture_resource_flags", invalid_texture_resource_flags_hr);
    print_hr("invalid_depth_format_flags", invalid_depth_format_flags_hr);
    print_hr("invalid_clear_without_flag", invalid_clear_without_flag_hr);
    print_hr("invalid_clear_format", invalid_clear_format_hr);
    print_hr("null_heap_properties", null_heap_properties_hr);
    std::printf("    \"invalid_zero_width_allocation\": [%llu,%llu],\n",
                static_cast<unsigned long long>(invalid_zero_width_allocation_size),
                static_cast<unsigned long long>(invalid_zero_width_allocation_alignment));
    std::printf("    \"allocation_overflow\": [%llu,%llu],\n",
                static_cast<unsigned long long>(allocation_overflow_size),
                static_cast<unsigned long long>(allocation_overflow_alignment));
    std::printf("    \"allocation_batch\": [%llu,%llu,%llu,%llu,%llu],\n",
                static_cast<unsigned long long>(allocation_batch_size),
                static_cast<unsigned long long>(allocation_batch_alignment),
                static_cast<unsigned long long>(allocation_batch_sideband[0].Offset),
                static_cast<unsigned long long>(allocation_batch_sideband[1].Offset),
                static_cast<unsigned long long>(allocation_batch_sideband[1].SizeInBytes));
    std::printf("    \"allocation_batch_verified\": %s,\n",
                allocation_batch_ok ? "true" : "false");
    print_hr("invalid_msaa_mips", invalid_msaa_mips_hr);
    std::printf("    \"invalid_msaa_mips_allocation\": [%llu,%llu],\n",
                static_cast<unsigned long long>(invalid_msaa_mips_allocation_size),
                static_cast<unsigned long long>(invalid_msaa_mips_allocation_alignment));
    std::printf("    \"volume_allocation\": [%llu,%llu],\n",
                static_cast<unsigned long long>(volume_allocation_size),
                static_cast<unsigned long long>(volume_allocation_alignment));
    std::printf("    \"null_allocation\": [%llu,%llu],\n",
                static_cast<unsigned long long>(null_allocation_size),
                static_cast<unsigned long long>(null_allocation_alignment));
    std::printf("    \"invalid_footprint_total\": %llu,\n",
                static_cast<unsigned long long>(invalid_footprint_total));
    std::printf("    \"footprint_overflow_total\": %llu,\n",
                static_cast<unsigned long long>(footprint_overflow_total));
    std::printf("    \"planar_footprint_total\": %llu,\n",
                static_cast<unsigned long long>(planar_footprint_total));
    std::printf("    \"planar_footprint_verified\": %s,\n",
                planar_footprint_ok ? "true" : "false");
    std::printf("    \"nv12_footprint_total\": %llu,\n",
                static_cast<unsigned long long>(nv12_footprint_total));
    std::printf("    \"nv12_footprint_verified\": %s,\n",
                nv12_footprint_ok ? "true" : "false");
    print_hr("invalid_nv12_height", invalid_nv12_height_hr);
    print_hr("null_sideband_query", null_sideband_query_hr);
    std::printf("    \"null_sideband\": [%llu,%llu,%llu],\n",
                static_cast<unsigned long long>(null_sideband_size),
                static_cast<unsigned long long>(null_sideband_alignment),
                static_cast<unsigned long long>(null_sideband_offset));
    print_hr("tight_feature", tight_feature_hr);
    std::printf("    \"tight_feature_tier\": %u,\n", tight_feature_tier);
    print_hr("tight_committed", tight_committed_hr);
    print_hr("tight_allocation_info", tight_allocation_info_hr);
    std::printf("    \"tight_allocation\": [%llu,%llu],\n",
                static_cast<unsigned long long>(tight_allocation_size),
                static_cast<unsigned long long>(tight_allocation_alignment));
    print_hr("tight_heap", tight_heap_hr);
    std::printf("    \"null_heap_desc_verified\": %s,\n", null_heap_desc_ok ? "true" : "false");
    print_hr("tight_placed", tight_placed_hr);
    std::printf("    \"tight_placed_roundtrip_verified\": %s,\n",
                tight_placed_roundtrip_ok ? "true" : "false");
    print_hr("tight_invalid_alignment", tight_invalid_alignment_hr);
    print_hr("tight_overaligned_placed", tight_overaligned_placed_hr);
    print_hr("tight_reserved", tight_reserved_hr);
    print_hr("full_mip_create", full_mip_create_hr);
    std::printf("    \"full_mip_count\": %u,\n", full_mip_count);
    std::printf("    \"full_mip_footprint_verified\": %s,\n",
                full_mip_footprint_ok ? "true" : "false");
    print_hr("not_resident_committed", not_resident_committed_hr);
    print_hr("not_resident_initial_map", not_resident_initial_map_hr);
    print_hr("not_resident_make_resident", not_resident_make_resident_hr);
    print_hr("not_resident_remade_map", not_resident_remade_map_hr);
    print_hr("not_resident_heap", not_resident_heap_hr);
    print_hr("not_resident_placed", not_resident_placed_hr);
    std::printf("    \"not_resident_roundtrip_verified\": %s,\n",
                not_resident_roundtrip_ok ? "true" : "false");
    print_hr("residency_fence", residency_fence_hr);
    print_hr("enqueue_make_resident", enqueue_make_resident_hr);
    print_hr("invalid_enqueue_flags", invalid_enqueue_flags_hr);
    print_hr("descriptor_heap_create", residency_descriptor_heap_create_hr);
    print_hr("descriptor_heap_evict", residency_descriptor_heap_evict_hr);
    print_hr("descriptor_heap_make_resident", residency_descriptor_heap_make_hr);
    print_hr("query_heap_create", residency_query_heap_create_hr);
    print_hr("query_heap_evict", residency_query_heap_evict_hr);
    print_hr("query_heap_make_resident", residency_query_heap_make_hr);
    print_hr("pageable_priority", residency_pageable_priority_hr);
    std::printf("    \"descriptor_heap_residency_verified\": %s,\n",
                (SUCCEEDED(residency_descriptor_heap_evict_hr) &&
                 SUCCEEDED(residency_descriptor_heap_make_hr))
                    ? "true"
                    : "false");
    std::printf("    \"query_heap_residency_verified\": %s,\n",
                (SUCCEEDED(residency_query_heap_evict_hr) &&
                 SUCCEEDED(residency_query_heap_make_hr))
                    ? "true"
                    : "false");
    std::printf("    \"enqueue_fence_completed\": %llu,\n",
                static_cast<unsigned long long>(enqueue_fence_completed));
    print_hr("misaligned_placement", misaligned_placement_hr);
    print_hr("invalid_heap_alignment", invalid_heap_alignment_hr);
    print_hr("invalid_heap_flags", invalid_heap_flags_hr);
    print_hr("invalid_heap_size", invalid_heap_size_hr);
    std::printf("    \"cases\": [\n");
    for (size_t i = 0; i < resource_shapes.size(); ++i) {
        const auto& shape = resource_shapes[i];
        std::printf("      {\"name\":\"%s\",\"hr\":\"0x%08lx\",\"dimension\":%u,"
                    "\"width\":%llu,\"height\":%u,\"depth_or_array\":%u,\"mips\":%u,"
                    "\"format\":%u,\"samples\":%u,\"roundtrip\":%s}%s\n",
                    shape.name, static_cast<unsigned long>(static_cast<uint32_t>(shape.hr)),
                    static_cast<unsigned>(shape.requested.Dimension),
                    static_cast<unsigned long long>(shape.requested.Width), shape.requested.Height,
                    shape.requested.DepthOrArraySize, shape.requested.MipLevels,
                    static_cast<unsigned>(shape.requested.Format), shape.requested.SampleDesc.Count,
                    SUCCEEDED(shape.hr) && same_resource_desc(shape.created, shape.requested) ? "true" : "false",
                    i + 1 == resource_shapes.size() ? "" : ",");
    }
    std::printf("    ]\n");
    std::printf("  },\n");
    std::printf("  \"shared_handles\": {\n");
    print_hr("create", shared_create_hr);
    print_hr("open", shared_open_hr);
    print_hr("open_by_name", shared_open_named_hr);
    std::printf("    \"independent_objects_verified\": %s,\n", shared_independent_object_ok ? "true" : "false");
    print_hr("unnamed_source_create", unnamed_shared_source_hr);
    print_hr("unnamed_handle_create", unnamed_shared_create_hr);
    print_hr("unnamed_open", unnamed_shared_open_hr);
    print_hr("unnamed_map", unnamed_shared_map_hr);
    std::printf("    \"unnamed_roundtrip_verified\": %s,\n",
                unnamed_shared_roundtrip_ok ? "true" : "false");
    print_hr("unknown_handle", shared_unknown_hr);
    print_hr("missing_name", shared_missing_name_hr);
    print_hr("invalid_create_access", shared_invalid_create_access_hr);
    print_hr("invalid_open_access", shared_invalid_open_access_hr);
    std::printf("    \"roundtrip_verified\": %s,\n", shared_handle_roundtrip ? "true" : "false");
    std::printf("    \"cross_process_verified\": %s,\n", cross_process_shared_ok ? "true" : "false");
    print_hr("heap_create", shared_heap_create_hr);
    print_hr("heap_open", shared_heap_open_hr);
    print_hr("heap_file_open", shared_heap_file_open_hr);
    std::printf("    \"heap_roundtrip_verified\": %s,\n", shared_heap_roundtrip_ok ? "true" : "false");
    std::printf("    \"heap_cross_process_verified\": %s,\n", shared_heap_cross_process_ok ? "true" : "false");
    print_hr("unnamed_heap_create", unnamed_shared_heap_create_hr);
    print_hr("unnamed_heap_handle_create", unnamed_shared_heap_handle_hr);
    print_hr("unnamed_heap_open", unnamed_shared_heap_open_hr);
    std::printf("    \"unnamed_heap_roundtrip_verified\": %s,\n",
                unnamed_shared_heap_roundtrip_ok ? "true" : "false");
    print_hr("fence_create", shared_fence_create_hr);
    print_hr("fence_handle_create", shared_fence_handle_hr);
    print_hr("fence_signal", shared_fence_signal_hr);
    print_hr("fence_mapping_signal", shared_fence_mapping_signal_hr);
    print_hr("fence_wait_mapping", shared_fence_wait_hr);
    print_hr("unnamed_fence_create", unnamed_shared_fence_create_hr);
    print_hr("unnamed_fence_handle_create", unnamed_shared_fence_handle_hr);
    print_hr("unnamed_fence_open", unnamed_shared_fence_open_hr);
    print_hr("unnamed_fence_signal", unnamed_shared_fence_signal_hr);
    std::printf("    \"unnamed_fence_roundtrip_verified\": %s,\n",
                unnamed_shared_fence_roundtrip_ok ? "true" : "false");
    std::printf("    \"fence_cross_process_verified\": %s\n", shared_heap_cross_process_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"textures\": {\n");
    print_hr("texture_create", texture_hr);
    print_hr("upload_create", texture_upload_hr);
    print_hr("readback_create", texture_readback_hr);
    print_hr("upload_map", texture_map_hr);
    print_hr("readback_map", texture_readback_map_hr);
    std::printf("    \"copy_verified\": %s,\n", texture_copy_ok ? "true" : "false");
    print_hr("direct_io_texture_create", direct_io_texture_create_hr);
    print_hr("direct_io_texture_write", direct_io_texture_write_hr);
    print_hr("direct_io_texture_read", direct_io_texture_read_hr);
    std::printf("    \"direct_io_texture_verified\": %s,\n",
                direct_io_texture_ok ? "true" : "false");
    print_hr("direct_io_volume_create", direct_io_volume_create_hr);
    print_hr("direct_io_volume_write", direct_io_volume_write_hr);
    print_hr("direct_io_volume_read", direct_io_volume_read_hr);
    print_hr("direct_io_volume_box_write", direct_io_volume_box_write_hr);
    print_hr("direct_io_volume_box_read", direct_io_volume_box_read_hr);
    std::printf("    \"direct_io_volume_verified\": %s,\n",
                direct_io_volume_ok ? "true" : "false");
    std::printf("    \"direct_io_volume_box_verified\": %s,\n",
                direct_io_volume_box_ok ? "true" : "false");
    print_hr("direct_io_d32s8_create", direct_io_d32s8_create_hr);
    print_hr("direct_io_d32s8_write_depth", direct_io_d32s8_write_hr[0]);
    print_hr("direct_io_d32s8_write_stencil", direct_io_d32s8_write_hr[1]);
    print_hr("direct_io_d32s8_read_depth", direct_io_d32s8_read_hr[0]);
    print_hr("direct_io_d32s8_read_stencil", direct_io_d32s8_read_hr[1]);
    std::printf("    \"direct_io_d32s8_verified\": %s,\n",
                direct_io_d32s8_ok ? "true" : "false");
    print_hr("direct_io_nv12_create", direct_io_nv12_create_hr);
    print_hr("direct_io_nv12_y_write", direct_io_nv12_write_hr[0]);
    print_hr("direct_io_nv12_uv_write", direct_io_nv12_write_hr[1]);
    print_hr("direct_io_nv12_y_read", direct_io_nv12_read_hr[0]);
    print_hr("direct_io_nv12_uv_read", direct_io_nv12_read_hr[1]);
    std::printf("    \"direct_io_nv12_verified\": %s,\n",
                direct_io_nv12_ok ? "true" : "false");
    print_hr("direct_io_nv12_upload_create", direct_io_nv12_upload_hr);
    print_hr("direct_io_nv12_readback_create", direct_io_nv12_readback_hr);
    print_hr("direct_io_nv12_upload_map", direct_io_nv12_upload_map_hr);
    print_hr("direct_io_nv12_readback_map", direct_io_nv12_readback_map_hr);
    std::printf("    \"direct_io_nv12_copy_verified\": %s,\n",
                direct_io_nv12_copy_ok ? "true" : "false");
    print_hr("direct_io_nv12_copy_destination_create",
              direct_io_nv12_copy_destination_hr);
    print_hr("direct_io_nv12_copy_y_read", direct_io_nv12_copy_y_read_hr);
    print_hr("direct_io_nv12_copy_uv_read", direct_io_nv12_copy_uv_read_hr);
    std::printf("    \"direct_io_nv12_resource_copy_verified\": %s,\n",
                direct_io_nv12_resource_copy_ok ? "true" : "false");
    print_hr("direct_io_p010_create", direct_io_p010_create_hr);
    print_hr("direct_io_p010_y_write", direct_io_p010_write_hr[0]);
    print_hr("direct_io_p010_uv_write", direct_io_p010_write_hr[1]);
    print_hr("direct_io_p010_y_read", direct_io_p010_read_hr[0]);
    print_hr("direct_io_p010_uv_read", direct_io_p010_read_hr[1]);
    std::printf("    \"direct_io_p010_verified\": %s,\n",
                direct_io_p010_ok ? "true" : "false");
    const char *variant_names[4] = {"b5g6r5", "b5g5r5a1", "b4g4r4a4", "r9g9b9e5"};
    for (UINT variant = 0; variant < 4; ++variant) {
        char key[64] = {};
        std::snprintf(key, sizeof(key), "direct_io_%s_create", variant_names[variant]);
        print_hr(key, direct_io_variant_create_hr[variant]);
        std::snprintf(key, sizeof(key), "direct_io_%s_write", variant_names[variant]);
        print_hr(key, direct_io_variant_write_hr[variant]);
        std::snprintf(key, sizeof(key), "direct_io_%s_read", variant_names[variant]);
        print_hr(key, direct_io_variant_read_hr[variant]);
    }
    std::printf("    \"direct_io_variant_verified\": %s,\n",
                direct_io_variant_ok ? "true" : "false");
    std::printf("    \"width\": %llu,\n", static_cast<unsigned long long>(texture_roundtrip_desc.Width));
    std::printf("    \"height\": %u,\n", texture_roundtrip_desc.Height);
    std::printf("    \"row_pitch\": %u,\n", texture_footprint.Footprint.RowPitch);
    std::printf("    \"upload_bytes\": %llu,\n", static_cast<unsigned long long>(texture_upload_bytes));
    print_hr("unsupported_r1_texture_create", unsupported_texture_hr);
    std::printf("    \"unsupported_r1_texture_rejected\": %s,\n", unsupported_texture_rejected ? "true" : "false");
    std::printf("    \"unaligned_bc1_create_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_texture_hr)));
    std::printf("    \"unaligned_bc1_upload_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_upload_hr)));
    std::printf("    \"unaligned_bc1_readback_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_readback_hr)));
    print_hr("unaligned_bc1_direct_write", bc_direct_write_hr);
    print_hr("unaligned_bc1_direct_read", bc_direct_read_hr);
    std::printf("    \"unaligned_bc1_direct_io_verified\": %s,\n", bc_direct_io_ok ? "true" : "false");
    std::printf("    \"unaligned_bc1_copy_verified\": %s,\n", bc_copy_ok ? "true" : "false");
    std::printf("    \"unaligned_bc1_width\": 7,\n");
    std::printf("    \"unaligned_bc1_height\": 5,\n");
    std::printf("    \"unaligned_bc1_rows\": %u,\n", bc_rows);
    std::printf("    \"unaligned_bc1_row_bytes\": %llu\n", static_cast<unsigned long long>(bc_row_bytes));
    std::printf("  },\n");
    std::printf("  \"sparse\": {\n");
    print_hr("heap_create", sparse_heap_hr);
    print_hr("reserved_resource_create", reserved_texture_hr);
    print_hr("reserved_1d_resource_create", reserved_1d_texture_hr);
    print_hr("reserved_1d_alias_texture_create", reserved_1d_alias_texture_hr);
    print_hr("reserved_1d_tiling", reserved_1d_tiling_hr);
    print_hr("reserved_1d_readback_create", reserved_1d_readback_hr);
    print_hr("reserved_1d_readback_map", reserved_1d_readback_map_hr);
    std::printf("    \"reserved_1d_total_tiles\": %u,\n", reserved_1d_total_tiles);
    std::printf("    \"reserved_1d_tiling_count\": %u,\n", reserved_1d_tiling_count);
    std::printf("    \"reserved_1d_tile_shape\": [%u,%u,%u],\n",
                reserved_1d_tile_shape.WidthInTexels,
                reserved_1d_tile_shape.HeightInTexels,
                reserved_1d_tile_shape.DepthInTexels);
    std::printf("    \"reserved_1d_tilings\": [[%u,%u],[%u,%u]],\n",
                reserved_1d_tilings[0].WidthInTiles,
                reserved_1d_tilings[0].StartTileIndexInOverallResource,
                reserved_1d_tilings[1].WidthInTiles,
                reserved_1d_tilings[1].StartTileIndexInOverallResource);
    std::printf("    \"reserved_1d_copy_verified\": %s,\n",
                reserved_1d_copy_ok ? "true" : "false");
    std::printf("    \"reserved_1d_mapping_copy_verified\": %s,\n",
                reserved_1d_mapping_copy_ok ? "true" : "false");
    print_hr("upload_create", sparse_upload_hr);
    print_hr("readback_create", sparse_readback_hr);
    print_hr("unmapped_readback_create", sparse_unmapped_readback_hr);
    print_hr("upload_map", sparse_upload_map_hr);
    print_hr("readback_map", sparse_readback_map_hr);
    print_hr("unmap_close", sparse_unmap_close_hr);
    print_hr("unmap_execute", sparse_unmap_execute_hr);
    print_hr("unmap_signal", sparse_unmap_signal_hr);
    print_hr("unmap_wait", sparse_unmap_wait_hr);
    print_hr("unmapped_readback_map", sparse_unmapped_map_hr);
    std::printf("    \"total_tiles\": %u,\n", sparse_total_tiles);
    std::printf("    \"tiling_count\": %u,\n", sparse_tiling_count);
    std::printf("    \"packed_mips\": [%u, %u, %u, %u],\n", sparse_packed_mips.NumStandardMips,
                sparse_packed_mips.NumPackedMips, sparse_packed_mips.NumTilesForPackedMips,
                sparse_packed_mips.StartTileIndexInOverallResource);
    std::printf("    \"tile_shape\": [%u, %u, %u],\n", sparse_tile_shape.WidthInTexels,
                sparse_tile_shape.HeightInTexels, sparse_tile_shape.DepthInTexels);
    std::printf("    \"subresource_tiling\": [[%u, %u, %u, %u], [%u, %u, %u, %u]],\n", sparse_tiling[0].WidthInTiles,
                sparse_tiling[0].HeightInTiles, sparse_tiling[0].DepthInTiles,
                sparse_tiling[0].StartTileIndexInOverallResource, sparse_tiling[1].WidthInTiles,
                sparse_tiling[1].HeightInTiles, sparse_tiling[1].DepthInTiles,
                sparse_tiling[1].StartTileIndexInOverallResource);
    std::printf("    \"copy_verified\": %s,\n", sparse_copy_ok ? "true" : "false");
    std::printf("    \"unmapped_zero_verified\": %s,\n", sparse_unmapped_zero_ok ? "true" : "false");
    std::printf("    \"cross_queue_mapping_wait_verified\": %s,\n",
                (SUCCEEDED(reuse_mapping_signal_hr) && SUCCEEDED(reuse_mapping_wait_hr))
                    ? "true"
                    : "false");
    print_hr("placement_alias_texture_create", placement_alias_texture_hr);
    print_hr("array_alias_heap_create", array_alias_heap_hr);
    print_hr("array_alias_texture_create", array_alias_texture_hr);
    print_hr("array_alias_readback_create", array_alias_readback_hr);
    print_hr("array_alias_readback_map", array_alias_readback_map_hr);
    print_hr("array_upload_map", array_upload_map_hr);
    print_hr("array_upload_restore", array_upload_restore_hr);
    print_hr("array_mapping_signal", array_mapping_signal_hr);
    print_hr("array_mapping_wait", array_mapping_wait_hr);
    print_hr("array_mapping_close", array_mapping_close_hr);
    print_hr("array_mapping_execute", array_mapping_execute_hr);
    print_hr("array_mapping_signal_fence", array_mapping_signal_fence_hr);
    print_hr("array_mapping_wait_fence", array_mapping_wait_fence_hr);
    std::printf("    \"array_alias_mapping_verified\": %s,\n", array_alias_copy_ok ? "true" : "false");
    std::printf("    \"array_alias_first_byte\": %u,\n", array_alias_first);
    std::printf("    \"array_alias_second_byte\": %u,\n", array_alias_second);
    std::printf("    \"array_alias_mismatch\": %s,\n",
                array_alias_mismatch == UINT64_MAX
                    ? "null"
                    : std::to_string(array_alias_mismatch).c_str());
    std::printf("    \"array_alias_mismatch_actual_expected\": [%u, %u],\n",
                array_alias_mismatch_actual, array_alias_mismatch_expected);
    print_hr("placement_alias_readback_create", placement_alias_readback_hr);
    print_hr("placement_alias_readback_map", placement_alias_readback_map_hr);
    std::printf("    \"placement_alias_copy_verified\": %s,\n", placement_alias_copy_ok ? "true" : "false");
    std::printf("    \"placement_alias_first_byte\": %u,\n", placement_alias_first);
    std::printf("    \"placement_alias_first_mismatch\": %s,\n",
                placement_alias_first_mismatch == UINT64_MAX ? "null"
                                                             : std::to_string(placement_alias_first_mismatch).c_str());
    std::printf("    \"reserved_buffer\": {\n");
    print_hr("create", reserved_buffer_hr);
    print_hr("reuse_heap_create", reuse_heap_hr);
    print_hr("reuse_buffer_create", reuse_buffer_hr);
    print_hr("tiling", reserved_buffer_tiling_hr);
    print_hr("readback_create", reserved_buffer_readback_hr);
    print_hr("reuse_readback_create", reserved_buffer_reuse_readback_hr);
    print_hr("reuse_readback_map", reserved_buffer_reuse_readback_map_hr);
    print_hr("unmapped_readback_create", reserved_buffer_unmapped_readback_hr);
    print_hr("readback_map", reserved_buffer_readback_map_hr);
    print_hr("unmapped_readback_map", reserved_buffer_unmapped_map_hr);
    std::printf("      \"total_tiles\": %u,\n", reserved_buffer_total_tiles);
    std::printf("      \"tiling_count\": %u,\n", reserved_buffer_tiling_count);
    std::printf("      \"tile_shape_width\": %u,\n", reserved_buffer_tile_shape.WidthInTexels);
    std::printf("      \"subresource_tiling\": [%u, %u, %u, %u],\n", reserved_buffer_tiling.WidthInTiles,
                reserved_buffer_tiling.HeightInTiles, reserved_buffer_tiling.DepthInTiles,
                reserved_buffer_tiling.StartTileIndexInOverallResource);
    std::printf("      \"copy_verified\": %s,\n", reserved_buffer_copy_ok ? "true" : "false");
    print_hr("reuse_skip_close", reuse_skip_close_hr);
    print_hr("reuse_skip_execute", reuse_skip_execute_hr);
    print_hr("reuse_skip_signal", reuse_skip_signal_hr);
    print_hr("reuse_skip_wait", reuse_skip_wait_hr);
    print_hr("reuse_mapping_signal", reuse_mapping_signal_hr);
    print_hr("reuse_mapping_wait", reuse_mapping_wait_hr);
    std::printf("      \"reuse_single_tile_skip_verified\": %s,\n",
                reserved_buffer_reuse_skip_ok ? "true" : "false");
    std::printf("      \"reuse_first_last\": [%u, %u],\n", reserved_buffer_reuse_first,
                reserved_buffer_reuse_last);
    std::printf("      \"reuse_mismatch\": %s,\n",
                reserved_buffer_reuse_mismatch == UINT64_MAX
                    ? "null"
                    : std::to_string(reserved_buffer_reuse_mismatch).c_str());
    std::printf("      \"reuse_actual_expected\": [%u, %u],\n", reserved_buffer_reuse_actual,
                reserved_buffer_reuse_expected);
    std::printf("      \"unmapped_zero_verified\": %s,\n", reserved_buffer_unmapped_zero_ok ? "true" : "false");
    print_hr("mapping_copy_heap_create", copy_mapping_heap_hr);
    print_hr("mapping_copy_source_create", mapping_copy_source_hr);
    print_hr("mapping_copy_destination_create", mapping_copy_destination_hr);
    print_hr("mapping_copy_readback_create", mapping_copy_readback_hr);
    print_hr("mapping_copy_readback_map", mapping_copy_readback_map_hr);
    std::printf("      \"mapping_copy_verified\": %s,\n", mapping_copy_ok ? "true" : "false");
    print_hr("r8_texture_create", r8_reserved_texture_hr);
    print_hr("r8_tiling", r8_reserved_tiling_hr);
    print_hr("r8_readback_create", r8_reserved_readback_hr);
    print_hr("r8_readback_map", r8_reserved_readback_map_hr);
    std::printf("      \"r8_total_tiles\": %u,\n", r8_reserved_total_tiles);
    std::printf("      \"r8_tile_shape\": [%u, %u, %u],\n", r8_reserved_tile_shape.WidthInTexels,
                r8_reserved_tile_shape.HeightInTexels, r8_reserved_tile_shape.DepthInTexels);
    std::printf("      \"r8_copy_verified\": %s,\n", r8_reserved_copy_ok ? "true" : "false");
    print_hr("r8_mipped_heap_create", r8_mipped_heap_hr);
    print_hr("r8_mipped_texture_create", r8_mipped_texture_hr);
    print_hr("r8_mipped_tiling", r8_mipped_tiling_hr);
    print_hr("r8_mipped_readback_create", r8_mipped_readback_hr);
    print_hr("r8_mipped_readback_map", r8_mipped_readback_map_hr);
    std::printf("      \"r8_mipped_total_tiles\": %u,\n", r8_mipped_total_tiles);
    std::printf("      \"r8_mipped_tiling_count\": %u,\n", r8_mipped_tiling_count);
    std::printf("      \"r8_mipped_copy_verified\": %s,\n", r8_mipped_copy_ok ? "true" : "false");
    print_hr("r8_partial_texture_create", r8_partial_texture_hr);
    print_hr("r8_partial_upload_create", r8_partial_upload_hr);
    print_hr("r8_partial_readback_create", r8_partial_readback_hr);
    print_hr("r8_partial_upload_map", r8_partial_upload_map_hr);
    print_hr("r8_partial_tiling", r8_partial_tiling_hr);
    print_hr("r8_partial_readback_map", r8_partial_readback_map_hr);
    std::printf("      \"r8_partial_total_tiles\": %u,\n", r8_partial_total_tiles);
    std::printf("      \"r8_partial_packed_mips\": [%u, %u, %u, %u],\n", r8_partial_packed_mips.NumStandardMips,
                r8_partial_packed_mips.NumPackedMips, r8_partial_packed_mips.NumTilesForPackedMips,
                r8_partial_packed_mips.StartTileIndexInOverallResource);
    std::printf("      \"r8_partial_copy_verified\": %s\n", r8_partial_copy_ok ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"format_matrix\": [\n");
    for (size_t i = 0; i < sparse_format_probes.size(); ++i) {
        const auto& sparse_format = sparse_format_probes[i];
        std::printf("      {\"format\": \"%s\", \"texture_create\": \"0x%08lx\", \"tiling\": \"0x%08lx\", "
                    "\"readback_create\": \"0x%08lx\", \"readback_map\": \"0x%08lx\", \"total_tiles\": %u, "
                    "\"tile_shape\": [%u, %u, %u], \"copy_verified\": %s}%s\n",
                    sparse_format.name, static_cast<unsigned long>(static_cast<uint32_t>(sparse_format.texture_hr)),
                    static_cast<unsigned long>(static_cast<uint32_t>(sparse_format.tiling_hr)),
                    static_cast<unsigned long>(static_cast<uint32_t>(sparse_format.readback_hr)),
                    static_cast<unsigned long>(static_cast<uint32_t>(sparse_format.readback_map_hr)),
                    sparse_format.total_tiles, sparse_format.tile_shape.WidthInTexels,
                    sparse_format.tile_shape.HeightInTexels, sparse_format.tile_shape.DepthInTexels,
                    sparse_format.copy_ok ? "true" : "false", i + 1 == sparse_format_probes.size() ? "" : ",");
    }
    std::printf("    ],\n");
    std::printf("    \"packed_tail_reserved\": {\n");
    print_hr("create", packed_tail_reserved_texture_hr);
    print_hr("tiling", packed_tail_reserved_tiling_hr);
    std::printf("      \"total_tiles\": %u,\n", packed_tail_total_tiles);
    std::printf("      \"tiling_count\": %u,\n", packed_tail_tiling_count);
    std::printf("      \"partial_query_count\": %u,\n", packed_tail_partial_count);
    std::printf("      \"partial_query_verified\": %s,\n", packed_tail_partial_query_ok ? "true" : "false");
    print_hr("readback_create", packed_tail_readback_hr);
    print_hr("readback_map", packed_tail_readback_map_hr);
    std::printf("      \"footprint_verified\": %s,\n",
                packed_tail_footprint_ok ? "true" : "false");
    std::printf("      \"footprint\": [%llu,%u,%u,%u,%u,%u,%llu,%llu],\n",
                static_cast<unsigned long long>(packed_tail_footprint.Offset),
                packed_tail_footprint.Footprint.Width,
                packed_tail_footprint.Footprint.Height,
                packed_tail_footprint.Footprint.Depth,
                packed_tail_footprint.Footprint.RowPitch,
                packed_tail_rows,
                static_cast<unsigned long long>(packed_tail_row_bytes),
                static_cast<unsigned long long>(packed_tail_copy_bytes));
    std::printf("      \"copy_verified\": %s,\n",
                packed_tail_copy_ok ? "true" : "false");
    std::printf("      \"packed_mips\": [%u, %u, %u, %u],\n", packed_tail_info.NumStandardMips,
                packed_tail_info.NumPackedMips, packed_tail_info.NumTilesForPackedMips,
                packed_tail_info.StartTileIndexInOverallResource);
    std::printf("      \"tile_shape\": [%u, %u, %u],\n", packed_tail_shape.WidthInTexels,
                packed_tail_shape.HeightInTexels, packed_tail_shape.DepthInTexels);
    std::printf("      \"tilings\": [[%u,%u,%u,%u],[%u,%u,%u,%u],[%u,%u,%u,%u],[%u,%u,%u,%u]]\n",
                packed_tail_tilings[0].WidthInTiles, packed_tail_tilings[0].HeightInTiles,
                packed_tail_tilings[0].DepthInTiles, packed_tail_tilings[0].StartTileIndexInOverallResource,
                packed_tail_tilings[1].WidthInTiles, packed_tail_tilings[1].HeightInTiles,
                packed_tail_tilings[1].DepthInTiles, packed_tail_tilings[1].StartTileIndexInOverallResource,
                packed_tail_tilings[2].WidthInTiles, packed_tail_tilings[2].HeightInTiles,
                packed_tail_tilings[2].DepthInTiles, packed_tail_tilings[2].StartTileIndexInOverallResource,
                packed_tail_tilings[3].WidthInTiles, packed_tail_tilings[3].HeightInTiles,
                packed_tail_tilings[3].DepthInTiles, packed_tail_tilings[3].StartTileIndexInOverallResource);
    std::printf("    },\n");
    std::printf("    \"mipped_texture\": {\n");
    print_hr("create", mipped_reserved_texture_hr);
    print_hr("tiling", mipped_reserved_tiling_hr);
    print_hr("readback_create", mipped_reserved_readback_hr);
    print_hr("readback_map", mipped_reserved_readback_map_hr);
    std::printf("      \"total_tiles\": %u,\n", mipped_reserved_total_tiles);
    std::printf("      \"tiling_count\": %u,\n", mipped_reserved_tiling_count);
    std::printf("      \"copy_verified\": %s\n", mipped_reserved_copy_ok ? "true" : "false");
    std::printf("    },\n");
    print_hr("volume_heap_create", volume_heap_hr);
    print_hr("volume_texture_create", volume_texture_hr);
    print_hr("volume_alias_texture_create", volume_alias_texture_hr);
    print_hr("volume_tiling", volume_tiling_hr);
    print_hr("volume_alias_tiling", volume_alias_tiling_hr);
    print_hr("volume_readback_create", volume_readback_hr);
    print_hr("volume_alias_readback_create", volume_alias_readback_hr);
    print_hr("volume_readback_map", volume_readback_map_hr);
    print_hr("volume_alias_readback_map", volume_alias_readback_map_hr);
    std::printf("    \"volume_total_tiles\": %u,\n", volume_total_tiles);
    std::printf("    \"volume_tile_shape\": [%u, %u, %u],\n", volume_tile_shape.WidthInTexels,
                volume_tile_shape.HeightInTexels, volume_tile_shape.DepthInTexels);
    std::printf("    \"volume_copy_verified\": %s,\n", volume_copy_ok ? "true" : "false");
    std::printf("    \"volume_alias_copy_verified\": %s,\n", volume_alias_copy_ok ? "true" : "false");
    std::printf("    \"tier3_physical_page_ownership_verified\": %s\n",
                volume_physical_page_ownership_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"formats\": {\n");
    for (size_t i = 0; i < formats.size(); ++i)
        print_format_json(formats[i], i + 1 == formats.size());
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
