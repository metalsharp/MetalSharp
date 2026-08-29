#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

#include <algorithm>
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

static D3D12CreateDeviceFn g_create_device = nullptr;
static D3D12SerializeRootSignatureFn g_serialize_root_signature = nullptr;
static D3DCompileFn g_compile = nullptr;

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

static UINT64 align_to(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
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

static D3D12_RESOURCE_DESC texture1d_desc(UINT64 width, UINT16 array_size, UINT16 mip_levels, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    desc.Width = width;
    desc.Height = 1;
    desc.DepthOrArraySize = array_size;
    desc.MipLevels = mip_levels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

static D3D12_RESOURCE_DESC texture2d_desc(UINT64 width, UINT height, UINT16 array_size, UINT16 mip_levels,
                                          DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, UINT sample_count = 1) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = array_size;
    desc.MipLevels = mip_levels;
    desc.Format = format;
    desc.SampleDesc.Count = sample_count;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

static D3D12_RESOURCE_DESC texture3d_desc(UINT64 width, UINT height, UINT16 depth, UINT16 mip_levels,
                                          DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = depth;
    desc.MipLevels = mip_levels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
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

struct CaseResult {
    std::string name;
    bool pass = false;
    HRESULT hr = E_FAIL;
    std::string detail;
    std::string extra;
};

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

static HRESULT create_device(ID3D12Device** device) {
    return g_create_device ? g_create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                             reinterpret_cast<void**>(device))
                           : E_FAIL;
}

static HRESULT create_queue(ID3D12Device* device, ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
}

static HRESULT create_command_list(ID3D12Device* device, ID3D12CommandAllocator** allocator,
                                   ID3D12GraphicsCommandList** list) {
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, *allocator, nullptr, IID_PPV_ARGS(list));
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
    if (SUCCEEDED(hr))
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
        hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return hr;
}

static HRESULT create_committed_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap_type, UINT64 bytes,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                       ID3D12Resource** resource) {
    D3D12_HEAP_PROPERTIES heap = heap_props(heap_type);
    D3D12_RESOURCE_DESC desc = buffer_desc(bytes, flags);
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(resource));
}

static HRESULT create_committed_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                                        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                        ID3D12Resource** resource) {
    D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(resource));
}

static HRESULT compile_shader(const char* hlsl, const char* entry, ID3DBlob** out, std::string& errors) {
    ID3DBlob* err = nullptr;
    HRESULT hr = g_compile ? g_compile(hlsl, std::strlen(hlsl), "probe_resource_views_formats.hlsl", nullptr, nullptr,
                                       entry, "cs_5_0", 0, 0, out, &err)
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

static bool readback_pixel(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                           Pixel* pixel) {
    uint8_t* data = nullptr;
    D3D12_RANGE read_range = {0, static_cast<SIZE_T>(footprint.Footprint.RowPitch * footprint.Footprint.Height)};
    HRESULT hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&data));
    if (FAILED(hr) || !data)
        return false;
    pixel->r = data[footprint.Offset + 0];
    pixel->g = data[footprint.Offset + 1];
    pixel->b = data[footprint.Offset + 2];
    pixel->a = data[footprint.Offset + 3];
    D3D12_RANGE write_range = {0, 0};
    readback->Unmap(0, &write_range);
    return true;
}

static bool pixel_eq(const Pixel& pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return pixel.r == r && pixel.g == g && pixel.b == b && pixel.a == a;
}

static CaseResult run_resource_shapes_case() {
    CaseResult result = {"resource_shapes_heaps_and_map", false, E_FAIL, "", ""};
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* def = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* tex1d = nullptr;
    ID3D12Resource* tex2d_array_mips = nullptr;
    ID3D12Resource* tex3d = nullptr;
    ID3D12Resource* cube = nullptr;
    ID3D12Resource* msaa = nullptr;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* placed_buffer = nullptr;
    ID3D12Resource* placed_texture = nullptr;

    const UINT64 bytes = 4096;
    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = create_queue(device, &queue);
    if (SUCCEEDED(hr))
        hr = create_command_list(device, &allocator, &list);
    HRESULT upload_hr = SUCCEEDED(hr)
                            ? create_committed_buffer(device, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, &upload)
                            : E_FAIL;
    HRESULT default_hr = SUCCEEDED(hr)
                             ? create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_NONE,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, &def)
                             : E_FAIL;
    HRESULT readback_hr =
        SUCCEEDED(hr) ? create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE,
                                                D3D12_RESOURCE_STATE_COPY_DEST, &readback)
                      : E_FAIL;

    uint8_t* upload_ptr = nullptr;
    HRESULT upload_map_hr = upload ? upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr)) : E_FAIL;
    if (SUCCEEDED(upload_map_hr) && upload_ptr) {
        for (UINT64 i = 0; i < bytes; ++i)
            upload_ptr[i] = static_cast<uint8_t>((i * 23u + 5u) & 0xffu);
        upload->Unmap(0, nullptr);
    }
    void* default_ptr = nullptr;
    HRESULT default_map_hr = def ? def->Map(0, nullptr, &default_ptr) : E_FAIL;
    if (SUCCEEDED(default_map_hr) && def)
        def->Unmap(0, nullptr);

    if (SUCCEEDED(hr) && upload && def && readback) {
        list->CopyBufferRegion(def, 0, upload, 0, bytes);
        D3D12_RESOURCE_BARRIER to_copy =
            transition_barrier(def, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        list->CopyBufferRegion(readback, 0, def, 0, bytes);
        hr = execute_and_wait(device, queue, list);
    }

    uint8_t* readback_ptr = nullptr;
    HRESULT readback_map_hr = readback ? readback->Map(0, nullptr, reinterpret_cast<void**>(&readback_ptr)) : E_FAIL;
    bool buffer_copy_ok = SUCCEEDED(hr) && SUCCEEDED(readback_map_hr) && readback_ptr;
    if (buffer_copy_ok) {
        for (UINT64 i = 0; i < bytes; ++i) {
            if (readback_ptr[i] != static_cast<uint8_t>((i * 23u + 5u) & 0xffu)) {
                buffer_copy_ok = false;
                break;
            }
        }
        readback->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_DESC tex1d_desc_v = texture1d_desc(16, 1, 1, DXGI_FORMAT_R8_UNORM);
    D3D12_RESOURCE_DESC tex2d_array_mips_desc_v =
        texture2d_desc(8, 8, 2, 3, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC tex3d_desc_v = texture3d_desc(4, 4, 4, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_DESC cube_desc_v =
        texture2d_desc(4, 4, 6, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC msaa_desc_v =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, 4);
    D3D12_CLEAR_VALUE msaa_clear = {};
    msaa_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaa_clear.Color[3] = 1.0f;
    HRESULT tex1d_hr =
        device ? create_committed_texture(device, tex1d_desc_v, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &tex1d)
               : E_FAIL;
    HRESULT tex2d_array_mips_hr =
        device ? create_committed_texture(device, tex2d_array_mips_desc_v, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          &tex2d_array_mips)
               : E_FAIL;
    HRESULT tex3d_hr =
        device ? create_committed_texture(device, tex3d_desc_v, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &tex3d)
               : E_FAIL;
    HRESULT cube_hr =
        device ? create_committed_texture(device, cube_desc_v, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &cube)
               : E_FAIL;
    bool cube_face_io_ok = false;
    if (SUCCEEDED(cube_hr) && cube) {
        uint8_t source[64] = {};
        uint8_t destination[64] = {};
        cube_face_io_ok = true;
        for (UINT face = 0; face < 6 && cube_face_io_ok; ++face) {
            for (UINT i = 0; i < sizeof(source); ++i)
                source[i] = static_cast<uint8_t>((face * 41u + i * 13u + 3u) & 0xffu);
            HRESULT write_hr = cube->WriteToSubresource(face, nullptr, source, 16, sizeof(source));
            HRESULT read_hr = cube->ReadFromSubresource(destination, 16, sizeof(destination), face, nullptr);
            cube_face_io_ok = SUCCEEDED(write_hr) && SUCCEEDED(read_hr) &&
                              std::memcmp(source, destination, sizeof(source)) == 0;
        }
    }
    HRESULT msaa_hr =
        device ? create_committed_texture(device, msaa_desc_v, D3D12_RESOURCE_STATE_RENDER_TARGET, &msaa_clear, &msaa)
               : E_FAIL;

    D3D12_RESOURCE_DESC placed_buffer_desc = buffer_desc(8192);
    D3D12_RESOURCE_DESC placed_texture_desc =
        texture2d_desc(16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC alloc_descs[] = {placed_buffer_desc, placed_texture_desc};
    D3D12_RESOURCE_ALLOCATION_INFO allocs[2] = {};
    D3D12_RESOURCE_ALLOCATION_INFO combined_alloc = {};
    if (device)
        combined_alloc = device->GetResourceAllocationInfo(0, 2, alloc_descs);
    if (device)
        allocs[0] = device->GetResourceAllocationInfo(0, 1, &placed_buffer_desc);
    if (device)
        allocs[1] = device->GetResourceAllocationInfo(0, 1, &placed_texture_desc);
    UINT64 placed_texture_offset =
        allocs[0].Alignment ? align_to(allocs[0].SizeInBytes, allocs[1].Alignment ? allocs[1].Alignment : 65536) : 0;
    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = combined_alloc.SizeInBytes;
    heap_desc.Properties = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    heap_desc.Alignment = combined_alloc.Alignment;
    heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    HRESULT heap_hr = device ? device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)) : E_FAIL;
    HRESULT placed_buffer_hr =
        heap ? device->CreatePlacedResource(heap, 0, &placed_buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&placed_buffer))
             : E_FAIL;
    HRESULT placed_texture_hr =
        heap ? device->CreatePlacedResource(heap, placed_texture_offset, &placed_texture_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&placed_texture))
             : E_FAIL;

    D3D12_RESOURCE_DESC tex1d_roundtrip = tex1d ? tex1d->GetDesc() : D3D12_RESOURCE_DESC{};
    D3D12_RESOURCE_DESC tex2d_roundtrip = tex2d_array_mips ? tex2d_array_mips->GetDesc() : D3D12_RESOURCE_DESC{};
    D3D12_RESOURCE_DESC tex3d_roundtrip = tex3d ? tex3d->GetDesc() : D3D12_RESOURCE_DESC{};
    D3D12_RESOURCE_DESC cube_roundtrip = cube ? cube->GetDesc() : D3D12_RESOURCE_DESC{};
    D3D12_RESOURCE_DESC msaa_roundtrip = msaa ? msaa->GetDesc() : D3D12_RESOURCE_DESC{};
    bool shapes_ok =
        tex1d_roundtrip.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D && tex1d_roundtrip.Width == 16 &&
        tex2d_roundtrip.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && tex2d_roundtrip.DepthOrArraySize == 2 &&
        tex2d_roundtrip.MipLevels == 3 && tex3d_roundtrip.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
        tex3d_roundtrip.DepthOrArraySize == 4 && tex3d_roundtrip.MipLevels == 2 &&
        cube_roundtrip.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        cube_roundtrip.DepthOrArraySize == 6 && msaa_roundtrip.SampleDesc.Count == 4;
    bool map_behavior_ok = SUCCEEDED(upload_map_hr) && SUCCEEDED(readback_map_hr) && FAILED(default_map_hr);
    bool gpu_va_ok = upload && def && placed_buffer && upload->GetGPUVirtualAddress() != 0 &&
                     def->GetGPUVirtualAddress() != 0 && placed_buffer->GetGPUVirtualAddress() != 0 &&
                     upload->GetGPUVirtualAddress() != def->GetGPUVirtualAddress();
    bool allocation_ok = combined_alloc.SizeInBytes != 0 && combined_alloc.Alignment != 0 &&
                         allocs[0].SizeInBytes != 0 && allocs[1].SizeInBytes != 0 &&
                         placed_texture_offset < combined_alloc.SizeInBytes;

    result.pass = SUCCEEDED(hr) && SUCCEEDED(upload_hr) && SUCCEEDED(default_hr) && SUCCEEDED(readback_hr) &&
                  buffer_copy_ok && map_behavior_ok && gpu_va_ok && SUCCEEDED(tex1d_hr) &&
                  SUCCEEDED(tex2d_array_mips_hr) && SUCCEEDED(tex3d_hr) && SUCCEEDED(cube_hr) &&
                  cube_face_io_ok && SUCCEEDED(msaa_hr) && shapes_ok &&
                  allocation_ok && SUCCEEDED(heap_hr) && SUCCEEDED(placed_buffer_hr) && SUCCEEDED(placed_texture_hr);
    result.hr = result.pass ? S_OK : hr;
    result.detail =
        result.pass ? "committed, placed, upload, readback, default, 1D, 2D array/mip, cube, 3D, and MSAA resources verified"
                    : "resource shape, heap, map, or copy verification failed";
    char extra[1024] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"upload_create\":\"%s\",\"default_create\":\"%s\",\"readback_create\":\"%s\","
                  "\"upload_map\":\"%s\",\"readback_map\":\"%s\",\"default_map_rejected\":%s,"
                  "\"texture1d_create\":\"%s\",\"texture2d_array_mips_create\":\"%s\","
                  "\"texture3d_create\":\"%s\",\"cube_create\":\"%s\",\"cube_six_slice_array\":%s,"
                  "\"cube_face_io_verified\":%s,\"msaa4x_create\":\"%s\",\"heap_create\":\"%s\","
                  "\"placed_buffer_create\":\"%s\",\"placed_texture_create\":\"%s\","
                  "\"combined_allocation_size\":%llu,\"combined_allocation_alignment\":%llu,"
                  "\"gpu_virtual_addresses_nonzero_unique\":%s,\"buffer_copy_verified\":%s",
                  hr_hex(upload_hr).c_str(), hr_hex(default_hr).c_str(), hr_hex(readback_hr).c_str(),
                  hr_hex(upload_map_hr).c_str(), hr_hex(readback_map_hr).c_str(),
                  FAILED(default_map_hr) ? "true" : "false", hr_hex(tex1d_hr).c_str(),
                  hr_hex(tex2d_array_mips_hr).c_str(), hr_hex(tex3d_hr).c_str(), hr_hex(cube_hr).c_str(),
                  cube_roundtrip.DepthOrArraySize == 6 ? "true" : "false",
                  cube_face_io_ok ? "true" : "false", hr_hex(msaa_hr).c_str(), hr_hex(heap_hr).c_str(),
                  hr_hex(placed_buffer_hr).c_str(), hr_hex(placed_texture_hr).c_str(),
                  static_cast<unsigned long long>(combined_alloc.SizeInBytes),
                  static_cast<unsigned long long>(combined_alloc.Alignment), gpu_va_ok ? "true" : "false",
                  buffer_copy_ok ? "true" : "false");
    result.extra = extra;

    safe_release(placed_texture);
    safe_release(placed_buffer);
    safe_release(heap);
    safe_release(msaa);
    safe_release(cube);
    safe_release(tex3d);
    safe_release(tex2d_array_mips);
    safe_release(tex1d);
    safe_release(readback);
    safe_release(def);
    safe_release(upload);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(device);
    return result;
}

static bool bytes_per_pixel(DXGI_FORMAT format, UINT* bytes) {
    switch (format) {
    case DXGI_FORMAT_R8_UNORM:
        *bytes = 1;
        return true;
    case DXGI_FORMAT_R16_UINT:
        *bytes = 2;
        return true;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        *bytes = 4;
        return true;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        *bytes = 8;
        return true;
    default:
        return false;
    }
}

static bool compare_linear_footprints(const D3D12_RESOURCE_DESC& desc, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fps,
                                      const UINT* rows, const UINT64* row_bytes, UINT subresources, UINT64 base_offset,
                                      std::string* mismatch) {
    UINT bpp = 0;
    if (!bytes_per_pixel(desc.Format, &bpp)) {
        *mismatch = "unsupported bytes-per-pixel expectation";
        return false;
    }
    UINT mip_levels = desc.MipLevels;
    UINT64 next_offset = base_offset;
    for (UINT sub = 0; sub < subresources; ++sub) {
        UINT mip = sub % mip_levels;
        UINT64 width = std::max<UINT64>(1, desc.Width >> mip);
        UINT height = std::max<UINT>(1, desc.Height >> mip);
        UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                         ? std::max<UINT>(1, static_cast<UINT>(desc.DepthOrArraySize) >> mip)
                         : 1;
        UINT64 expected_row_bytes = width * bpp;
        UINT expected_row_pitch = static_cast<UINT>(align_to(expected_row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        UINT64 expected_offset = align_to(next_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (fps[sub].Offset != expected_offset || fps[sub].Footprint.Width != width ||
            fps[sub].Footprint.Height != height || fps[sub].Footprint.Depth != depth ||
            fps[sub].Footprint.RowPitch != expected_row_pitch || rows[sub] != height ||
            row_bytes[sub] != expected_row_bytes) {
            char buffer[256] = {};
            std::snprintf(buffer, sizeof(buffer),
                          "subresource %u mismatch offset=%llu/%llu rowPitch=%u/%u rows=%u/%u rowBytes=%llu/%llu", sub,
                          static_cast<unsigned long long>(fps[sub].Offset),
                          static_cast<unsigned long long>(expected_offset), fps[sub].Footprint.RowPitch,
                          expected_row_pitch, rows[sub], height, static_cast<unsigned long long>(row_bytes[sub]),
                          static_cast<unsigned long long>(expected_row_bytes));
            *mismatch = buffer;
            return false;
        }
        next_offset = expected_offset + static_cast<UINT64>(expected_row_pitch) * height * depth;
    }
    return true;
}

static CaseResult run_copyable_footprints_case() {
    CaseResult result = {"copyable_footprints", false, E_FAIL, "", ""};
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device(&device);
    D3D12_RESOURCE_DESC tex2d = texture2d_desc(7, 5, 2, 3, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    constexpr UINT tex2d_subresources = 6;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps2d[tex2d_subresources] = {};
    UINT rows2d[tex2d_subresources] = {};
    UINT64 row_bytes2d[tex2d_subresources] = {};
    UINT64 total2d = 0;
    if (SUCCEEDED(hr))
        device->GetCopyableFootprints(&tex2d, 0, tex2d_subresources, 0, fps2d, rows2d, row_bytes2d, &total2d);

    D3D12_RESOURCE_DESC tex3d = texture3d_desc(5, 4, 3, 2, DXGI_FORMAT_R16_UINT);
    constexpr UINT tex3d_subresources = 2;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fps3d[tex3d_subresources] = {};
    UINT rows3d[tex3d_subresources] = {};
    UINT64 row_bytes3d[tex3d_subresources] = {};
    UINT64 total3d = 0;
    if (SUCCEEDED(hr))
        device->GetCopyableFootprints(&tex3d, 0, tex3d_subresources, 128, fps3d, rows3d, row_bytes3d, &total3d);

    std::string mismatch;
    bool tex2d_ok =
        SUCCEEDED(hr) && compare_linear_footprints(tex2d, fps2d, rows2d, row_bytes2d, tex2d_subresources, 0, &mismatch);
    bool tex3d_ok = SUCCEEDED(hr) &&
                    compare_linear_footprints(tex3d, fps3d, rows3d, row_bytes3d, tex3d_subresources, 128, &mismatch);
    bool totals_ok = total2d >= fps2d[tex2d_subresources - 1].Offset && total3d >= fps3d[tex3d_subresources - 1].Offset;
    result.pass = SUCCEEDED(hr) && tex2d_ok && tex3d_ok && totals_ok;
    result.hr = hr;
    result.detail = result.pass ? "GetCopyableFootprints row pitch, row bytes, offsets, mips, arrays, and 3D slices "
                                  "match D3D12 alignment rules"
                                : (mismatch.empty() ? "copyable footprint validation failed" : mismatch);
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"tex2d_array_mip_total_bytes\":%llu,\"tex2d_first_row_pitch\":%u,"
                  "\"tex2d_last_offset\":%llu,\"tex3d_total_bytes_with_base_128\":%llu,"
                  "\"tex3d_first_row_pitch\":%u,\"tex3d_first_depth\":%u",
                  static_cast<unsigned long long>(total2d), fps2d[0].Footprint.RowPitch,
                  static_cast<unsigned long long>(fps2d[tex2d_subresources - 1].Offset),
                  static_cast<unsigned long long>(total3d), fps3d[0].Footprint.RowPitch, fps3d[0].Footprint.Depth);
    result.extra = extra;
    safe_release(device);
    return result;
}

static CaseResult run_view_binding_case() {
    CaseResult result = {"view_creation_and_binding", false, E_FAIL, "", ""};
    const char* hlsl = "cbuffer Constants:register(b0){uint addend; uint multiplier; uint pad0; uint pad1;};"
                       "StructuredBuffer<uint> input_values:register(t0);"
                       "RWStructuredBuffer<uint> output_values:register(u0);"
                       "[numthreads(4,1,1)] void main(uint3 id:SV_DispatchThreadID){"
                       "output_values[id.x]=input_values[id.x]*multiplier+addend;}";

    ID3D12Device* device = nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* table_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12Resource* constants = nullptr;
    ID3D12Resource* input_upload = nullptr;
    ID3D12Resource* input_default = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* output_readback = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* color_readback = nullptr;
    ID3D12Resource* depth = nullptr;
    std::string shader_errors;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, shader_errors);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 0;
        ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 3;
        param.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        hr = serialize_root_signature(root_desc, &root_blob, shader_errors);
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
        hr = create_queue(device, &queue);
    if (SUCCEEDED(hr))
        hr = create_command_list(device, &allocator, &list);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 3;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&table_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv_heap));
    }
    if (SUCCEEDED(hr)) {
        uint32_t cbuf[64] = {};
        cbuf[0] = 7;
        cbuf[1] = 2;
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, &constants);
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = constants->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, cbuf, sizeof(cbuf));
            constants->Unmap(0, nullptr);
        }
    }
    uint32_t input_values[4] = {3, 4, 5, 6};
    if (SUCCEEDED(hr)) {
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_UPLOAD, sizeof(input_values), D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, &input_upload);
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = input_upload->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, input_values, sizeof(input_values));
            input_upload->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &input_default);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &output_readback);

    D3D12_RESOURCE_DESC color_desc =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_RESOURCE_DESC depth_desc =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE color_clear = {};
    color_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_clear.Color[3] = 1.0f;
    D3D12_CLEAR_VALUE depth_clear = {};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_footprint = {};
    UINT color_rows = 0;
    UINT64 color_row_bytes = 0;
    UINT64 color_readback_bytes = 0;
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_footprint, &color_rows, &color_row_bytes,
                                      &color_readback_bytes);
        hr = create_committed_texture(device, color_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &color_clear, &color);
    }
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, color_readback_bytes, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &color_readback);
    if (SUCCEEDED(hr))
        hr = create_committed_texture(device, depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &depth);

    if (SUCCEEDED(hr)) {
        UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = table_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
        cbv.BufferLocation = constants->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        device->CreateConstantBufferView(&cbv, offset_cpu(cpu, increment, 0));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = 4;
        srv.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateShaderResourceView(input_default, &srv, offset_cpu(cpu, increment, 1));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.StructureByteStride = sizeof(uint32_t);
        device->CreateUnorderedAccessView(output, nullptr, &uav, offset_cpu(cpu, increment, 2));
        device->CreateRenderTargetView(color, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth, &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());

        list->CopyBufferRegion(input_default, 0, input_upload, 0, sizeof(input_values));
        D3D12_RESOURCE_BARRIER input_to_srv = transition_barrier(input_default, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &input_to_srv);
        ID3D12DescriptorHeap* heaps[] = {table_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, table_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER output_barriers[] = {
            uav_barrier(output),
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(2, output_barriers);
        list->CopyResource(output_readback, output);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv_handle);
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        list->ClearRenderTargetView(rtv, red, 0, nullptr);
        list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 0, nullptr);
        D3D12_RESOURCE_BARRIER color_to_copy =
            transition_barrier(color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &color_to_copy);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = color_readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = color_footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = color;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = execute_and_wait(device, queue, list);
    }

    uint32_t got[4] = {};
    Pixel pixel = {};
    bool compute_verified = SUCCEEDED(hr) && readback_u32(output_readback, got, 4) && got[0] == 13 && got[1] == 15 &&
                            got[2] == 17 && got[3] == 19;
    bool rtv_verified =
        SUCCEEDED(hr) && readback_pixel(color_readback, color_footprint, &pixel) && pixel_eq(pixel, 255, 0, 0, 255);
    result.pass = SUCCEEDED(hr) && compute_verified && rtv_verified;
    result.hr = hr;
    result.detail =
        result.pass ? "CBV/SRV/UAV descriptor table plus RTV/DSV binding produced verified compute and render results"
                    : (shader_errors.empty() ? "view creation or binding verification failed" : shader_errors);
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"compute_values\":[%u,%u,%u,%u],\"rtv_pixel\":[%u,%u,%u,%u],"
                  "\"cbv_srv_uav_bound\":%s,\"rtv_bound\":%s,\"dsv_bound\":%s",
                  got[0], got[1], got[2], got[3], pixel.r, pixel.g, pixel.b, pixel.a,
                  compute_verified ? "true" : "false", rtv_verified ? "true" : "false",
                  SUCCEEDED(hr) ? "true" : "false");
    result.extra = extra;

    safe_release(depth);
    safe_release(color_readback);
    safe_release(color);
    safe_release(output_readback);
    safe_release(output);
    safe_release(input_default);
    safe_release(input_upload);
    safe_release(constants);
    safe_release(dsv_heap);
    safe_release(rtv_heap);
    safe_release(table_heap);
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

static CaseResult run_castable_format_case() {
    CaseResult result = {"relaxed_castable_formats", false, E_FAIL, "", ""};
    const char* hlsl = "Texture2D<uint> input_texture:register(t0);"
                       "RWByteAddressBuffer output:register(u0);"
                       "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){"
                       "output.Store(0,input_texture.Load(int3(0,0,0)));}";
    const char* rgba_hlsl = "Texture2D<uint4> input_texture:register(t0);"
                            "RWByteAddressBuffer output:register(u0);"
                            "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){"
                            "output.Store4(0,input_texture.Load(int3(0,0,0)));}";

    ID3D12Device* device = nullptr;
    ID3D12Device10* device10 = nullptr;
    ID3DBlob* cs = nullptr;
    ID3DBlob* rgba_cs = nullptr;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12PipelineState* rgba_pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Heap* cast_heap = nullptr;
    ID3D12Resource* cast_texture = nullptr;
    ID3D12Resource* placed_cast_texture = nullptr;
    ID3D12Resource* alias_texture = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* alias_upload = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Resource* rejected_output = nullptr;
    ID3D12Resource* rejected_readback = nullptr;
    ID3D12Resource* placed_output = nullptr;
    ID3D12Resource* placed_readback = nullptr;
    ID3D12Resource* alias_output = nullptr;
    ID3D12Resource* alias_readback = nullptr;
    ID3D12Resource* rgba_output = nullptr;
    ID3D12Resource* rgba_readback = nullptr;
    std::string shader_errors;

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr))
        hr = device->QueryInterface(IID_PPV_ARGS(&device10));
    if (SUCCEEDED(hr))
        hr = compile_shader(hlsl, "main", &cs, shader_errors);
    if (SUCCEEDED(hr))
        hr = compile_shader(rgba_hlsl, "main", &rgba_cs, shader_errors);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &param;
        hr = serialize_root_signature(root_desc, &root_blob, shader_errors);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (SUCCEEDED(hr)) {
            desc.CS = {rgba_cs->GetBufferPointer(), rgba_cs->GetBufferSize()};
            hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&rgba_pso));
        }
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, &queue);
    if (SUCCEEDED(hr))
        hr = create_command_list(device, &allocator, &list);
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 10;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }

    D3D12_RESOURCE_DESC texture_desc = texture2d_desc(1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC1 texture_desc1 = {};
    std::memcpy(&texture_desc1, &texture_desc, sizeof(texture_desc));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 upload_bytes = 0;
    DXGI_FORMAT castable_formats[] = {DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8G8B8A8_UINT};
    HRESULT invalid_list_hr = E_FAIL;
    bool invalid_list_rejected = false;
    if (SUCCEEDED(hr)) {
        const D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        DXGI_FORMAT invalid_format = DXGI_FORMAT_R16_UINT;
        ID3D12Resource* invalid_resource = nullptr;
        invalid_list_hr = device10->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &texture_desc1,
                                                             D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 1,
                                                             &invalid_format, IID_PPV_ARGS(&invalid_resource));
        invalid_list_rejected = invalid_list_hr == E_INVALIDARG && invalid_resource == nullptr;
        safe_release(invalid_resource);
        if (!invalid_list_rejected)
            hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        const D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device10->CreateCommittedResource3(&props, D3D12_HEAP_FLAG_NONE, &texture_desc1,
                                                D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 2, castable_formats,
                                                IID_PPV_ARGS(&cast_texture));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_ALLOCATION_INFO info = {};
        device->GetResourceAllocationInfo(&info, 0, 1, &texture_desc);
        D3D12_HEAP_DESC heap_desc = {};
        heap_desc.SizeInBytes = info.SizeInBytes;
        heap_desc.Alignment = info.Alignment;
        heap_desc.Properties = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&cast_heap));
    }
    if (SUCCEEDED(hr))
        hr = device10->CreatePlacedResource2(cast_heap, 0, &texture_desc1, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, 2,
                                             castable_formats, IID_PPV_ARGS(&placed_cast_texture));
    if (SUCCEEDED(hr))
        hr = device10->CreatePlacedResource2(cast_heap, 0, &texture_desc1, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, 2,
                                             castable_formats, IID_PPV_ARGS(&alias_texture));
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &upload_bytes);
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
        uint8_t* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr)) {
            const uint32_t one_float_bits = 0x3f800000u;
            std::memcpy(mapped + footprint.Offset, &one_float_bits, sizeof(one_float_bits));
            upload->Unmap(0, nullptr);
        }
        if (SUCCEEDED(hr))
            hr = create_committed_buffer(device, D3D12_HEAP_TYPE_UPLOAD, upload_bytes, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, &alias_upload);
        if (SUCCEEDED(hr)) {
            uint8_t* alias_mapped = nullptr;
            hr = alias_upload->Map(0, nullptr, reinterpret_cast<void**>(&alias_mapped));
            if (SUCCEEDED(hr) && alias_mapped) {
                const uint32_t two_float_bits = 0x40000000u;
                std::memcpy(alias_mapped + footprint.Offset, &two_float_bits, sizeof(two_float_bits));
                alias_upload->Unmap(0, nullptr);
            }
        }
    }
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &rejected_output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &rejected_readback);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &placed_output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &placed_readback);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &alias_output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &alias_readback);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &rgba_output);
    if (SUCCEEDED(hr))
        hr = create_committed_buffer(device, D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COPY_DEST, &rgba_readback);

    if (SUCCEEDED(hr)) {
        const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_UINT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(cast_texture, &srv, cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(output, nullptr, &uav, offset_cpu(cpu, increment, 1));
        srv.Format = DXGI_FORMAT_R32_SINT;
        device->CreateShaderResourceView(cast_texture, &srv, offset_cpu(cpu, increment, 2));
        device->CreateUnorderedAccessView(rejected_output, nullptr, &uav, offset_cpu(cpu, increment, 3));
        srv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(placed_cast_texture, &srv, offset_cpu(cpu, increment, 4));
        device->CreateUnorderedAccessView(placed_output, nullptr, &uav, offset_cpu(cpu, increment, 5));
        srv.Format = DXGI_FORMAT_R8G8B8A8_UINT;
        device->CreateShaderResourceView(cast_texture, &srv, offset_cpu(cpu, increment, 6));
        device->CreateUnorderedAccessView(rgba_output, nullptr, &uav, offset_cpu(cpu, increment, 7));
        srv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(placed_cast_texture, &srv, offset_cpu(cpu, increment, 8));
        device->CreateUnorderedAccessView(alias_output, nullptr, &uav, offset_cpu(cpu, increment, 9));

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = cast_texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = placed_cast_texture;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER texture_barrier = transition_barrier(cast_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_BARRIER placed_texture_barrier = transition_barrier(
            placed_cast_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_RESOURCE_BARRIER texture_barriers[] = {texture_barrier, placed_texture_barrier};
        list->ResourceBarrier(2, texture_barriers);
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        D3D12_GPU_DESCRIPTOR_HANDLE output_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        output_gpu.ptr += increment;
        const UINT clear[4] = {};
        list->ClearUnorderedAccessViewUint(output_gpu, offset_cpu(cpu, increment, 1), output, clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER clear_barrier = uav_barrier(output);
        list->ResourceBarrier(1, &clear_barrier);
        D3D12_GPU_DESCRIPTOR_HANDLE rejected_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        rejected_gpu.ptr += 3ull * increment;
        list->ClearUnorderedAccessViewUint(rejected_gpu, offset_cpu(cpu, increment, 3), rejected_output, clear, 0,
                                           nullptr);
        D3D12_RESOURCE_BARRIER rejected_clear_barrier = uav_barrier(rejected_output);
        list->ResourceBarrier(1, &rejected_clear_barrier);
        D3D12_GPU_DESCRIPTOR_HANDLE placed_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        placed_gpu.ptr += 5ull * increment;
        list->ClearUnorderedAccessViewUint(placed_gpu, offset_cpu(cpu, increment, 5), placed_output, clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER placed_clear_barrier = uav_barrier(placed_output);
        list->ResourceBarrier(1, &placed_clear_barrier);
        D3D12_GPU_DESCRIPTOR_HANDLE rgba_gpu = heap->GetGPUDescriptorHandleForHeapStart();
        rgba_gpu.ptr += 7ull * increment;
        list->ClearUnorderedAccessViewUint(rgba_gpu, offset_cpu(cpu, increment, 7), rgba_output, clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER rgba_clear_barrier = uav_barrier(rgba_output);
        list->ResourceBarrier(1, &rgba_clear_barrier);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_GPU_DESCRIPTOR_HANDLE invalid_table = heap->GetGPUDescriptorHandleForHeapStart();
        invalid_table.ptr += 2ull * increment;
        list->SetComputeRootDescriptorTable(0, invalid_table);
        list->Dispatch(1, 1, 1);
        D3D12_GPU_DESCRIPTOR_HANDLE placed_table = heap->GetGPUDescriptorHandleForHeapStart();
        placed_table.ptr += 4ull * increment;
        list->SetComputeRootDescriptorTable(0, placed_table);
        list->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER alias_to_texture = {};
        alias_to_texture.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        alias_to_texture.Aliasing.pResourceBefore = placed_cast_texture;
        alias_to_texture.Aliasing.pResourceAfter = alias_texture;
        list->ResourceBarrier(1, &alias_to_texture);
        D3D12_TEXTURE_COPY_LOCATION alias_src = {};
        alias_src.pResource = alias_upload;
        alias_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        alias_src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION alias_dst = {};
        alias_dst.pResource = alias_texture;
        alias_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&alias_dst, 0, 0, 0, &alias_src, nullptr);
        D3D12_RESOURCE_BARRIER alias_texture_source =
            transition_barrier(alias_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &alias_texture_source);
        D3D12_RESOURCE_BARRIER texture_to_alias = {};
        texture_to_alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        texture_to_alias.Aliasing.pResourceBefore = alias_texture;
        texture_to_alias.Aliasing.pResourceAfter = placed_cast_texture;
        list->ResourceBarrier(1, &texture_to_alias);
        D3D12_GPU_DESCRIPTOR_HANDLE alias_table = heap->GetGPUDescriptorHandleForHeapStart();
        alias_table.ptr += 8ull * increment;
        const D3D12_CPU_DESCRIPTOR_HANDLE alias_uav_cpu = offset_cpu(cpu, increment, 9);
        D3D12_GPU_DESCRIPTOR_HANDLE alias_uav_gpu = alias_table;
        alias_uav_gpu.ptr += increment;
        list->ClearUnorderedAccessViewUint(alias_uav_gpu, alias_uav_cpu, alias_output, clear, 0, nullptr);
        list->SetComputeRootDescriptorTable(0, alias_table);
        list->Dispatch(1, 1, 1);

        D3D12_GPU_DESCRIPTOR_HANDLE rgba_table = heap->GetGPUDescriptorHandleForHeapStart();
        rgba_table.ptr += 6ull * increment;
        list->SetComputeRootDescriptorTable(0, rgba_table);
        list->SetPipelineState(rgba_pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER output_barriers[] = {
            uav_barrier(output),
            transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            uav_barrier(rejected_output),
            transition_barrier(rejected_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            uav_barrier(placed_output),
            transition_barrier(placed_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            uav_barrier(alias_output),
            transition_barrier(alias_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            uav_barrier(rgba_output),
            transition_barrier(rgba_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(10, output_barriers);
        list->CopyResource(readback, output);
        list->CopyResource(rejected_readback, rejected_output);
        list->CopyResource(placed_readback, placed_output);
        list->CopyResource(alias_readback, alias_output);
        list->CopyResource(rgba_readback, rgba_output);
        hr = execute_and_wait(device, queue, list);
    }

    uint32_t observed = 0;
    uint32_t rejected_value = 0;
    uint32_t placed_observed = 0;
    uint32_t alias_observed = 0;
    uint32_t rgba_observed[4] = {};
    const bool readback_ok =
        SUCCEEDED(hr) && readback_u32(readback, &observed, 1) && readback_u32(rejected_readback, &rejected_value, 1) &&
        readback_u32(placed_readback, &placed_observed, 1) && readback_u32(alias_readback, &alias_observed, 1) &&
        readback_u32(rgba_readback, rgba_observed, 4);
    const bool overlapping_alias_verified =
        readback_ok && placed_observed == 0x3f800000u && alias_observed == 0x40000000u;
    result.pass = invalid_list_rejected && readback_ok && observed == 0x3f800000u && rejected_value == 0 &&
                  overlapping_alias_verified;
    result.pass = result.pass && rgba_observed[0] == 0 && rgba_observed[1] == 0 && rgba_observed[2] == 128 &&
                  rgba_observed[3] == 63;
    result.hr = result.pass ? S_OK : hr;
    result.detail =
        result.pass
            ? "committed and placed R32_FLOAT resources passed declared R32_UINT/R8G8B8A8_UINT reads, an overlapping "
              "placed alias switched from 1.0 to 2.0, mismatched lists failed, and undeclared R32_SINT remained null"
            : (shader_errors.empty() ? "declared castable-format readback failed" : shader_errors);
    char extra[512] = {};
    std::snprintf(extra, sizeof(extra),
                  "\"device10_available\":%s,\"resource_format\":\"R32_FLOAT\","
                  "\"declared_view_format\":\"R32_UINT\",\"rejected_view_format\":\"R32_SINT\","
                  "\"observed_bits\":%u,\"rejected_view_value\":%u,"
                  "\"placed_observed_bits\":%u,\"alias_observed_bits\":%u,\"overlapping_alias_verified\":%s,\"rgba8_"
                  "uint_values\":[%u,%u,%u,%u],"
                  "\"mismatched_unit_size_hr\":\"%s\",\"mismatched_unit_size_rejected\":%s",
                  device10 ? "true" : "false", observed, rejected_value, placed_observed, alias_observed,
                  overlapping_alias_verified ? "true" : "false", rgba_observed[0], rgba_observed[1], rgba_observed[2],
                  rgba_observed[3], hr_hex(invalid_list_hr).c_str(), invalid_list_rejected ? "true" : "false");
    result.extra = extra;

    safe_release(rgba_readback);
    safe_release(rgba_output);
    safe_release(alias_readback);
    safe_release(alias_output);
    safe_release(placed_readback);
    safe_release(placed_output);
    safe_release(rejected_readback);
    safe_release(rejected_output);
    safe_release(readback);
    safe_release(output);
    safe_release(alias_upload);
    safe_release(upload);
    safe_release(alias_texture);
    safe_release(placed_cast_texture);
    safe_release(cast_texture);
    safe_release(cast_heap);
    safe_release(heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(rgba_pso);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(rgba_cs);
    safe_release(cs);
    safe_release(device10);
    safe_release(device);
    return result;
}

struct FormatCase {
    const char* name;
    DXGI_FORMAT format;
    UINT required_support1;
    UINT required_support2;
    HRESULT hr = E_FAIL;
    UINT support1 = 0;
    UINT support2 = 0;
    bool required_bits = false;
};

static CaseResult run_format_cases() {
    CaseResult result = {"formats_and_typeless_views", false, E_FAIL, "", ""};
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* cbv_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12Resource* typeless = nullptr;
    ID3D12Resource* uint_texture = nullptr;
    ID3D12Resource* depth = nullptr;

    std::vector<FormatCase> formats = {
        {"color_unorm", DXGI_FORMAT_R8G8B8A8_UNORM,
         D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
         0},
        {"color_bgra", DXGI_FORMAT_B8G8R8A8_UNORM,
         D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
         0},
        {"color_float", DXGI_FORMAT_R16G16B16A16_FLOAT,
         D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
         0},
        {"integer_uav", DXGI_FORMAT_R32_UINT,
         D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW,
         D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE},
        {"vertex_float", DXGI_FORMAT_R32_FLOAT, D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER, 0},
        {"index_uint16", DXGI_FORMAT_R16_UINT, D3D12_FORMAT_SUPPORT1_IA_INDEX_BUFFER, 0},
        {"srgb", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE,
         0},
        {"depth", DXGI_FORMAT_D32_FLOAT, D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, 0},
        {"depth_stencil", DXGI_FORMAT_D24_UNORM_S8_UINT,
         D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL, 0},
        {"typeless", DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_FORMAT_SUPPORT1_TEXTURE2D, 0},
    };

    HRESULT hr = create_device(&device);
    if (SUCCEEDED(hr)) {
        for (auto& format : formats) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
            support.Format = format.format;
            format.hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
            format.support1 = support.Support1;
            format.support2 = support.Support2;
            format.required_bits = SUCCEEDED(format.hr) &&
                                   (support.Support1 & format.required_support1) == format.required_support1 &&
                                   (support.Support2 & format.required_support2) == format.required_support2;
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 4;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cbv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv_heap));
    }

    D3D12_CLEAR_VALUE typeless_clear = {};
    typeless_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    typeless_clear.Color[3] = 1.0f;
    D3D12_RESOURCE_DESC typeless_desc =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    HRESULT typeless_hr = SUCCEEDED(hr)
                              ? create_committed_texture(device, typeless_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         &typeless_clear, &typeless)
                              : E_FAIL;
    D3D12_RESOURCE_DESC uint_desc =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT uint_hr = SUCCEEDED(hr) ? create_committed_texture(device, uint_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               nullptr, &uint_texture)
                                    : E_FAIL;
    D3D12_CLEAR_VALUE depth_clear = {};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;
    D3D12_RESOURCE_DESC depth_desc =
        texture2d_desc(4, 4, 1, 1, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    HRESULT depth_hr = SUCCEEDED(hr) ? create_committed_texture(device, depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                                &depth_clear, &depth)
                                     : E_FAIL;

    bool typeless_views_created = false;
    bool uav_view_created = false;
    bool dsv_view_created = false;
    if (SUCCEEDED(hr) && cbv_heap && rtv_heap && dsv_heap && typeless && uint_texture && depth) {
        UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cbv_start = cbv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(typeless, &rtv, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_unorm = {};
        srv_unorm.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_unorm.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_unorm.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_unorm.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(typeless, &srv_unorm, offset_cpu(cbv_start, increment, 0));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_srgb = srv_unorm;
        srv_srgb.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        device->CreateShaderResourceView(typeless, &srv_srgb, offset_cpu(cbv_start, increment, 1));
        typeless_views_created = true;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_UINT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(uint_texture, nullptr, &uav, offset_cpu(cbv_start, increment, 2));
        uav_view_created = true;

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth, &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());
        dsv_view_created = true;
    }

    bool all_format_support = true;
    std::string missing;
    for (const auto& format : formats) {
        if (!format.required_bits) {
            all_format_support = false;
            if (missing.empty())
                missing = format.name;
        }
    }

    result.pass = SUCCEEDED(hr) && all_format_support && SUCCEEDED(typeless_hr) && SUCCEEDED(uint_hr) &&
                  SUCCEEDED(depth_hr) && typeless_views_created && uav_view_created && dsv_view_created;
    result.hr = result.pass ? S_OK : hr;
    result.detail = result.pass ? "common color, depth/stencil, integer, normalized, sRGB, vertex/index, UAV, and "
                                  "typeless view formats validated"
                                : ("format support or view creation failed first_missing=" + missing);
    std::string extra = "\"typeless_resource_create\":\"" + hr_hex(typeless_hr) +
                        "\",\"typed_uav_resource_create\":\"" + hr_hex(uint_hr) + "\",\"depth_resource_create\":\"" +
                        hr_hex(depth_hr) +
                        "\",\"typeless_rtv_srv_srgb_views_created\":" + (typeless_views_created ? "true" : "false") +
                        ",\"typed_uav_view_created\":" + (uav_view_created ? "true" : "false") +
                        ",\"dsv_view_created\":" + (dsv_view_created ? "true" : "false") + ",\"formats\":[";
    for (size_t i = 0; i < formats.size(); ++i) {
        const auto& format = formats[i];
        char buffer[256] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "%s{\"name\":\"%s\",\"hr\":\"%s\",\"support1\":%u,\"support2\":%u,\"required_bits\":%s}",
                      i == 0 ? "" : ",", format.name, hr_hex(format.hr).c_str(), format.support1, format.support2,
                      format.required_bits ? "true" : "false");
        extra += buffer;
    }
    extra += "]";
    result.extra = extra;

    safe_release(depth);
    safe_release(uint_texture);
    safe_release(typeless);
    safe_release(dsv_heap);
    safe_release(rtv_heap);
    safe_release(cbv_heap);
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
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    if (!d3dcompiler)
        d3dcompiler = LoadLibraryA("d3dcompiler.dll");
    g_create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    g_serialize_root_signature = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    g_compile = load_proc<D3DCompileFn>(d3dcompiler, "D3DCompile");

    std::vector<CaseResult> cases;
    cases.push_back(run_resource_shapes_case());
    cases.push_back(run_copyable_footprints_case());
    cases.push_back(run_view_binding_case());
    cases.push_back(run_castable_format_case());
    cases.push_back(run_format_cases());

    bool pass = d3d12 && d3dcompiler && g_create_device && g_serialize_root_signature && g_compile;
    for (const auto& c : cases)
        pass = pass && c.pass;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-resource-views-formats.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"d3dcompiler_loaded\": %s,\n", d3dcompiler ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", g_create_device ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s,\n", g_serialize_root_signature ? "true" : "false");
    std::printf("    \"D3DCompile\": %s\n", g_compile ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"coverage\": {\n");
    std::printf("    \"committed_resources\": true,\n");
    std::printf("    \"placed_heap_resources\": true,\n");
    std::printf("    \"heap_type_map_unmap_behavior\": true,\n");
    std::printf("    \"gpu_virtual_address_allocation\": true,\n");
    std::printf("    \"texture_1d_2d_3d_array_mip_msaa\": true,\n");
    std::printf("    \"cbv_srv_uav_rtv_dsv_creation_and_binding\": true,\n");
    std::printf("    \"copyable_footprints\": true,\n");
    std::printf("    \"format_support_and_typeless_views\": true,\n");
    std::printf("    \"device10_declared_castable_format_readback\": true,\n");
    std::printf("    \"reserved_sparse_resources_status_reported\": true\n");
    std::printf("  },\n");
    std::printf("  \"unsupported\": {\n");
    std::printf("    \"reserved_sparse_resources\": \"not claimed by this probe; keep feature-gated until backed by "
                "Metal sparse APIs\"\n");
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < cases.size(); ++i)
        print_case(cases[i], i + 1 == cases.size());
    std::printf("  ]\n");
    std::printf("}\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
