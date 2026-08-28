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

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                  ID3DBlob**, ID3DBlob**);

struct ColorVertex {
    float position[4];
    float color[4];
};

struct TexVertex {
    float position[4];
    float uv[2];
};

struct Pixel {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

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

static D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment, UINT index) {
    start.ptr += static_cast<SIZE_T>(increment) * index;
    return start;
}

static D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu(D3D12_GPU_DESCRIPTOR_HANDLE start, UINT increment, UINT index) {
    start.ptr += static_cast<UINT64>(increment) * index;
    return start;
}

static HRESULT compile_shader(D3DCompileFn compile, const char* source, const char* entry, const char* target,
                              ID3DBlob** blob, ID3DBlob** errors) {
    return compile ? compile(source, strlen(source), "probe_render_headless.hlsl", nullptr, nullptr, entry, target, 0,
                             0, blob, errors)
                   : E_FAIL;
}

static HRESULT serialize_root_signature(SerializeRootSignatureFn serialize, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                        ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = serialize ? serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &errors) : E_FAIL;
    if (errors)
        errors->Release();
    return hr;
}

static uint32_t fnv1a32(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t clear_checksum(UINT width, UINT height, UINT row_pitch) {
    std::vector<uint8_t> clear(static_cast<size_t>(row_pitch) * height, 0);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x)
            clear[static_cast<size_t>(y) * row_pitch + x * 4 + 3] = 0xff;
    }
    return fnv1a32(clear.data(), clear.size());
}

static Pixel read_pixel(const uint8_t* data, UINT row_pitch, UINT x, UINT y) {
    Pixel pixel = {};
    const size_t offset = static_cast<size_t>(y) * row_pitch + x * 4;
    pixel.r = data[offset + 0];
    pixel.g = data[offset + 1];
    pixel.b = data[offset + 2];
    pixel.a = data[offset + 3];
    return pixel;
}

static bool pixel_equals(const Pixel& pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return pixel.r == r && pixel.g == g && pixel.b == b && pixel.a == a;
}

static bool pixel_not_clear(const Pixel& pixel) {
    return pixel.r != 0 || pixel.g != 0 || pixel.b != 0 || pixel.a != 0xff;
}

static bool write_binary_file(const char* path, const uint8_t* data, size_t size) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

int main() {
    const UINT render_width = 16;
    const UINT render_height = 16;
    const UINT uav_buffer_bytes = 16;
    const char* raw_readback_path = "Z:\\tmp\\dxmt_render_headless_rgba.bin";

    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));
    auto compile = reinterpret_cast<D3DCompileFn>(
        reinterpret_cast<void*>(d3dcompiler ? GetProcAddress(d3dcompiler, "D3DCompile") : nullptr));
    auto serialize = reinterpret_cast<SerializeRootSignatureFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr));

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : E_FAIL;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT queue_hr = device ? device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)) : E_FAIL;
    HRESULT allocator_hr =
        device ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) : E_FAIL;
    HRESULT list_hr =
        device ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))
               : E_FAIL;
    HRESULT fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) : E_FAIL;

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);

    D3D12_RESOURCE_DESC render_desc =
        texture_desc(render_width, render_height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_RESOURCE_DESC depth_desc =
        texture_desc(render_width, render_height, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_RESOURCE_DESC sample_desc = texture_desc(2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_RESOURCE_DESC uav_desc = buffer_desc(uav_buffer_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_CLEAR_VALUE render_clear = {};
    render_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    render_clear.Color[0] = 0.0f;
    render_clear.Color[1] = 0.0f;
    render_clear.Color[2] = 0.0f;
    render_clear.Color[3] = 1.0f;
    D3D12_CLEAR_VALUE depth_clear = {};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil.Depth = 1.0f;

    ID3D12Resource* render_target = nullptr;
    ID3D12Resource* depth_target = nullptr;
    ID3D12Resource* sample_texture = nullptr;
    ID3D12Resource* sample_upload = nullptr;
    ID3D12Resource* render_readback = nullptr;
    ID3D12Resource* logic_readback = nullptr;
    ID3D12Resource* uav_buffer = nullptr;
    ID3D12Resource* uav_readback = nullptr;
    ID3D12Resource* uav_zero_upload = nullptr;
    ID3D12Resource* triangle_vb = nullptr;
    ID3D12Resource* textured_vb = nullptr;
    ID3D12Resource* indexed_vb = nullptr;
    ID3D12Resource* quad_ib = nullptr;
    ID3D12Resource* depth_far_vb = nullptr;
    ID3D12Resource* depth_near_vb = nullptr;
    HRESULT render_target_hr = device
                                   ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &render_desc,
                                                                     D3D12_RESOURCE_STATE_RENDER_TARGET, &render_clear,
                                                                     IID_PPV_ARGS(&render_target))
                                   : E_FAIL;
    HRESULT depth_target_hr = device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
                                                                       IID_PPV_ARGS(&depth_target))
                                     : E_FAIL;
    HRESULT sample_texture_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &sample_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&sample_texture))
               : E_FAIL;
    HRESULT uav_buffer_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&uav_buffer))
               : E_FAIL;

    UINT64 sample_upload_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT sample_footprint = {};
    UINT sample_rows = 0;
    UINT64 sample_row_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&sample_desc, 0, 1, 0, &sample_footprint, &sample_rows, &sample_row_bytes,
                                      &sample_upload_bytes);
    D3D12_RESOURCE_DESC sample_upload_desc = buffer_desc(sample_upload_bytes);
    HRESULT sample_upload_hr =
        device
            ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &sample_upload_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&sample_upload))
            : E_FAIL;

    UINT64 render_readback_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT render_footprint = {};
    UINT render_rows = 0;
    UINT64 render_row_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&render_desc, 0, 1, 0, &render_footprint, &render_rows, &render_row_bytes,
                                      &render_readback_bytes);
    D3D12_RESOURCE_DESC render_readback_desc = buffer_desc(render_readback_bytes);
    HRESULT render_readback_hr =
        device
            ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &render_readback_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&render_readback))
            : E_FAIL;
    HRESULT logic_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &render_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&logic_readback))
               : E_FAIL;

    D3D12_RESOURCE_DESC uav_readback_desc = buffer_desc(uav_buffer_bytes);
    HRESULT uav_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &uav_readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&uav_readback))
               : E_FAIL;
    HRESULT uav_zero_upload_hr =
        device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &uav_readback_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&uav_zero_upload))
               : E_FAIL;

    const ColorVertex triangle_vertices[] = {
        {{-1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}},
    };
    const TexVertex textured_vertices[] = {
        {{0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}}, {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}}, {{1.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}}, {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    const ColorVertex indexed_vertices[] = {
        {{-1.0f, -0.5f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
        {{-0.5f, -0.5f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
        {{-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f}},
    };
    const ColorVertex depth_far_vertices[] = {
        {{0.0f, 0.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},  {{1.0f, 0.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.0f, -1.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}, {{1.0f, 0.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{1.0f, -1.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}, {{0.0f, -1.0f, 0.8f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    };
    const ColorVertex depth_near_vertices[] = {
        {{0.25f, -0.25f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.75f, -0.25f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.25f, -0.75f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.75f, -0.25f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.75f, -0.75f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{0.25f, -0.75f, 0.2f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
    };
    const uint32_t quad_indices[] = {0, 1, 2};

    const auto create_upload_buffer = [&](const void* src, UINT64 bytes, ID3D12Resource** resource) -> HRESULT {
        if (!device)
            return E_FAIL;
        D3D12_RESOURCE_DESC desc = buffer_desc(bytes);
        HRESULT hr =
            device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(resource));
        if (FAILED(hr))
            return hr;
        uint8_t* mapped = nullptr;
        hr = (*resource)->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, src, static_cast<size_t>(bytes));
            (*resource)->Unmap(0, nullptr);
        }
        return hr;
    };

    HRESULT triangle_vb_hr = create_upload_buffer(triangle_vertices, sizeof(triangle_vertices), &triangle_vb);
    HRESULT textured_vb_hr = create_upload_buffer(textured_vertices, sizeof(textured_vertices), &textured_vb);
    HRESULT indexed_vb_hr = create_upload_buffer(indexed_vertices, sizeof(indexed_vertices), &indexed_vb);
    HRESULT depth_far_vb_hr = create_upload_buffer(depth_far_vertices, sizeof(depth_far_vertices), &depth_far_vb);
    HRESULT depth_near_vb_hr = create_upload_buffer(depth_near_vertices, sizeof(depth_near_vertices), &depth_near_vb);
    HRESULT quad_ib_hr = create_upload_buffer(quad_indices, sizeof(quad_indices), &quad_ib);

    uint8_t* sample_upload_ptr = nullptr;
    HRESULT sample_map_hr =
        sample_upload ? sample_upload->Map(0, nullptr, reinterpret_cast<void**>(&sample_upload_ptr)) : E_FAIL;
    if (SUCCEEDED(sample_map_hr) && sample_upload_ptr) {
        std::memset(sample_upload_ptr, 0, static_cast<size_t>(sample_upload_bytes));
        const uint8_t texels[4][4] = {
            {255, 255, 0, 255},
            {0, 255, 255, 255},
            {255, 0, 255, 255},
            {255, 255, 255, 255},
        };
        for (UINT y = 0; y < 2; ++y) {
            for (UINT x = 0; x < 2; ++x) {
                const size_t offset =
                    static_cast<size_t>(sample_footprint.Offset + y * sample_footprint.Footprint.RowPitch + x * 4);
                const uint8_t* texel = texels[y * 2 + x];
                sample_upload_ptr[offset + 0] = texel[0];
                sample_upload_ptr[offset + 1] = texel[1];
                sample_upload_ptr[offset + 2] = texel[2];
                sample_upload_ptr[offset + 3] = texel[3];
            }
        }
        sample_upload->Unmap(0, nullptr);
    }

    uint8_t* uav_zero_ptr = nullptr;
    HRESULT uav_zero_map_hr =
        uav_zero_upload ? uav_zero_upload->Map(0, nullptr, reinterpret_cast<void**>(&uav_zero_ptr)) : E_FAIL;
    if (SUCCEEDED(uav_zero_map_hr) && uav_zero_ptr) {
        std::memset(uav_zero_ptr, 0, uav_buffer_bytes);
        uav_zero_upload->Unmap(0, nullptr);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.NumDescriptors = 1;
    D3D12_DESCRIPTOR_HEAP_DESC srv_uav_heap_desc = {};
    srv_uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_uav_heap_desc.NumDescriptors = 2;
    srv_uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12DescriptorHeap* srv_uav_heap = nullptr;
    HRESULT rtv_heap_hr = device ? device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)) : E_FAIL;
    HRESULT dsv_heap_hr = device ? device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap)) : E_FAIL;
    HRESULT srv_uav_heap_hr =
        device ? device->CreateDescriptorHeap(&srv_uav_heap_desc, IID_PPV_ARGS(&srv_uav_heap)) : E_FAIL;
    HRESULT sampler_heap_hr = S_OK;

    UINT srv_uav_increment =
        device ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        rtv_heap ? rtv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle =
        dsv_heap ? dsv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_uav_cpu =
        srv_uav_heap ? srv_uav_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_uav_gpu =
        srv_uav_heap ? srv_uav_heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};

    if (device && render_target) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(render_target, &rtv_desc, rtv_handle);
    }
    if (device && depth_target) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth_target, &dsv_desc, dsv_handle);
    }
    if (device && sample_texture && srv_uav_heap && srv_uav_increment != 0) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(sample_texture, &srv_desc, offset_cpu(srv_uav_cpu, srv_uav_increment, 0));
    }
    if (device && uav_buffer && srv_uav_heap && srv_uav_increment != 0) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view = {};
        uav_view.Format = DXGI_FORMAT_R32_TYPELESS;
        uav_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_view.Buffer.FirstElement = 0;
        uav_view.Buffer.NumElements = uav_buffer_bytes / 4;
        uav_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(uav_buffer, nullptr, &uav_view,
                                          offset_cpu(srv_uav_cpu, srv_uav_increment, 1));
    }
    const char* shader_source = R"(
struct VSColorIn {
  float4 position : POSITION;
  float4 color : COLOR0;
};
struct VSTexIn {
  float4 position : POSITION;
  float2 uv : TEXCOORD0;
};
struct PSColorIn {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};
struct PSTexIn {
  float4 position : SV_POSITION;
  float2 uv : TEXCOORD0;
};

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
RWByteAddressBuffer outbuf : register(u0);

PSColorIn vs_color(VSColorIn input) {
  PSColorIn output;
  output.position = input.position;
  output.color = input.color;
  return output;
}

PSTexIn vs_tex(VSTexIn input) {
  PSTexIn output;
  output.position = input.position;
  output.uv = input.uv;
  return output;
}

float4 ps_color(PSColorIn input) : SV_Target {
  return input.color;
}

float4 ps_logic(PSColorIn input) : SV_Target {
  return float4(240.0 / 255.0, 204.0 / 255.0, 170.0 / 255.0, 85.0 / 255.0);
}

float4 ps_tex(PSTexIn input) : SV_Target {
  return tex0.Sample(samp0, input.uv);
}

float4 ps_uav(PSColorIn input) : SV_Target {
  outbuf.Store(0, 0x11223344u);
  return input.color;
}

[numthreads(1, 1, 1)]
void cs_write(uint3 tid : SV_DispatchThreadID) {
  if (tid.x == 0 && tid.y == 0 && tid.z == 0)
    outbuf.Store(4, 0x55667788u);
}
)";

    ID3DBlob* vs_color_blob = nullptr;
    ID3DBlob* ps_color_blob = nullptr;
    ID3DBlob* ps_logic_blob = nullptr;
    ID3DBlob* vs_tex_blob = nullptr;
    ID3DBlob* ps_tex_blob = nullptr;
    ID3DBlob* ps_uav_blob = nullptr;
    ID3DBlob* cs_blob = nullptr;
    ID3DBlob* compile_errors = nullptr;
    HRESULT vs_color_hr = compile_shader(compile, shader_source, "vs_color", "vs_5_0", &vs_color_blob, &compile_errors);
    HRESULT ps_color_hr = compile_shader(compile, shader_source, "ps_color", "ps_5_0", &ps_color_blob, &compile_errors);
    HRESULT ps_logic_hr = compile_shader(compile, shader_source, "ps_logic", "ps_5_0", &ps_logic_blob, &compile_errors);
    HRESULT vs_tex_hr = compile_shader(compile, shader_source, "vs_tex", "vs_5_0", &vs_tex_blob, &compile_errors);
    HRESULT ps_tex_hr = compile_shader(compile, shader_source, "ps_tex", "ps_5_0", &ps_tex_blob, &compile_errors);
    HRESULT ps_uav_hr = compile_shader(compile, shader_source, "ps_uav", "ps_5_1", &ps_uav_blob, &compile_errors);
    HRESULT cs_hr = compile_shader(compile, shader_source, "cs_write", "cs_5_0", &cs_blob, &compile_errors);

    D3D12_ROOT_SIGNATURE_DESC empty_root_desc = {};
    empty_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_DESCRIPTOR_RANGE texture_range = {};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 1;
    texture_range.BaseShaderRegister = 0;
    texture_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER texture_param = {};
    texture_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    texture_param.DescriptorTable.NumDescriptorRanges = 1;
    texture_param.DescriptorTable.pDescriptorRanges = &texture_range;
    texture_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC static_sampler = {};
    static_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    static_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    static_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    static_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    static_sampler.MinLOD = 0.0f;
    static_sampler.MaxLOD = 3.402823466e+38F;
    static_sampler.ShaderRegister = 0;
    static_sampler.RegisterSpace = 0;
    static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC texture_root_desc = {};
    texture_root_desc.NumParameters = 1;
    texture_root_desc.pParameters = &texture_param;
    texture_root_desc.NumStaticSamplers = 1;
    texture_root_desc.pStaticSamplers = &static_sampler;
    texture_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_DESCRIPTOR_RANGE uav_range = {};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = 1;
    uav_range.BaseShaderRegister = 0;
    uav_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER uav_param = {};
    uav_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    uav_param.DescriptorTable.NumDescriptorRanges = 1;
    uav_param.DescriptorTable.pDescriptorRanges = &uav_range;
    uav_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC uav_root_desc = {};
    uav_root_desc.NumParameters = 1;
    uav_root_desc.pParameters = &uav_param;
    uav_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* empty_root_blob = nullptr;
    ID3DBlob* texture_root_blob = nullptr;
    ID3DBlob* uav_root_blob = nullptr;
    HRESULT empty_root_blob_hr = serialize_root_signature(serialize, empty_root_desc, &empty_root_blob);
    HRESULT texture_root_blob_hr = serialize_root_signature(serialize, texture_root_desc, &texture_root_blob);
    HRESULT uav_root_blob_hr = serialize_root_signature(serialize, uav_root_desc, &uav_root_blob);

    ID3D12RootSignature* empty_root = nullptr;
    ID3D12RootSignature* texture_root = nullptr;
    ID3D12RootSignature* uav_root = nullptr;
    HRESULT empty_root_hr =
        (device && empty_root_blob)
            ? device->CreateRootSignature(0, empty_root_blob->GetBufferPointer(), empty_root_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&empty_root))
            : E_FAIL;
    HRESULT texture_root_hr =
        (device && texture_root_blob)
            ? device->CreateRootSignature(0, texture_root_blob->GetBufferPointer(), texture_root_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&texture_root))
            : E_FAIL;
    HRESULT uav_root_hr = (device && uav_root_blob)
                              ? device->CreateRootSignature(0, uav_root_blob->GetBufferPointer(),
                                                            uav_root_blob->GetBufferSize(), IID_PPV_ARGS(&uav_root))
                              : E_FAIL;

    D3D12_BLEND_DESC blend_desc = {};
    for (auto& rt : blend_desc.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    D3D12_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D12_FILL_MODE_SOLID;
    raster_desc.CullMode = D3D12_CULL_MODE_NONE;
    raster_desc.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC no_depth_desc = {};
    no_depth_desc.DepthEnable = FALSE;
    no_depth_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    no_depth_desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_DEPTH_STENCIL_DESC depth_enabled_desc = {};
    depth_enabled_desc.DepthEnable = TRUE;
    depth_enabled_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth_enabled_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    const D3D12_INPUT_ELEMENT_DESC color_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    const D3D12_INPUT_ELEMENT_DESC tex_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    auto make_graphics_desc = [&](ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps,
                                  const D3D12_INPUT_ELEMENT_DESC* layout, UINT layout_count,
                                  const D3D12_DEPTH_STENCIL_DESC& depth_state) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = {vs ? vs->GetBufferPointer() : nullptr, vs ? vs->GetBufferSize() : 0};
        desc.PS = {ps ? ps->GetBufferPointer() : nullptr, ps ? ps->GetBufferSize() : 0};
        desc.BlendState = blend_desc;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = raster_desc;
        desc.DepthStencilState = depth_state;
        desc.InputLayout = {layout, layout_count};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = depth_state.DepthEnable ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        return desc;
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC color_pso_desc =
        make_graphics_desc(empty_root, vs_color_blob, ps_color_blob, color_layout, 2, no_depth_desc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC texture_pso_desc =
        make_graphics_desc(texture_root, vs_tex_blob, ps_tex_blob, tex_layout, 2, no_depth_desc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC depth_pso_desc =
        make_graphics_desc(empty_root, vs_color_blob, ps_color_blob, color_layout, 2, depth_enabled_desc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC uav_pso_desc =
        make_graphics_desc(uav_root, vs_color_blob, ps_uav_blob, color_layout, 2, no_depth_desc);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC logic_pso_desc =
        make_graphics_desc(empty_root, vs_color_blob, ps_logic_blob, color_layout, 2, no_depth_desc);
    logic_pso_desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    logic_pso_desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;

    ID3D12PipelineState* color_pso = nullptr;
    ID3D12PipelineState* texture_pso = nullptr;
    ID3D12PipelineState* depth_pso = nullptr;
    ID3D12PipelineState* graphics_uav_pso = nullptr;
    ID3D12PipelineState* logic_pso = nullptr;
    HRESULT color_pso_hr =
        device ? device->CreateGraphicsPipelineState(&color_pso_desc, IID_PPV_ARGS(&color_pso)) : E_FAIL;
    HRESULT texture_pso_hr =
        device ? device->CreateGraphicsPipelineState(&texture_pso_desc, IID_PPV_ARGS(&texture_pso)) : E_FAIL;
    HRESULT depth_pso_hr =
        device ? device->CreateGraphicsPipelineState(&depth_pso_desc, IID_PPV_ARGS(&depth_pso)) : E_FAIL;
    HRESULT graphics_uav_pso_hr =
        device ? device->CreateGraphicsPipelineState(&uav_pso_desc, IID_PPV_ARGS(&graphics_uav_pso)) : E_FAIL;
    HRESULT logic_pso_hr =
        device ? device->CreateGraphicsPipelineState(&logic_pso_desc, IID_PPV_ARGS(&logic_pso)) : E_FAIL;

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute_pso_desc = {};
    compute_pso_desc.pRootSignature = uav_root;
    compute_pso_desc.CS = {cs_blob ? cs_blob->GetBufferPointer() : nullptr, cs_blob ? cs_blob->GetBufferSize() : 0};
    ID3D12PipelineState* compute_pso = nullptr;
    HRESULT compute_pso_hr =
        device ? device->CreateComputePipelineState(&compute_pso_desc, IID_PPV_ARGS(&compute_pso)) : E_FAIL;

    D3D12_VERTEX_BUFFER_VIEW triangle_view = {};
    triangle_view.BufferLocation = triangle_vb ? triangle_vb->GetGPUVirtualAddress() : 0;
    triangle_view.SizeInBytes = sizeof(triangle_vertices);
    triangle_view.StrideInBytes = sizeof(ColorVertex);
    D3D12_VERTEX_BUFFER_VIEW textured_view = {};
    textured_view.BufferLocation = textured_vb ? textured_vb->GetGPUVirtualAddress() : 0;
    textured_view.SizeInBytes = sizeof(textured_vertices);
    textured_view.StrideInBytes = sizeof(TexVertex);
    D3D12_VERTEX_BUFFER_VIEW indexed_view = {};
    indexed_view.BufferLocation = indexed_vb ? indexed_vb->GetGPUVirtualAddress() : 0;
    indexed_view.SizeInBytes = sizeof(indexed_vertices);
    indexed_view.StrideInBytes = sizeof(ColorVertex);
    D3D12_VERTEX_BUFFER_VIEW depth_far_view = {};
    depth_far_view.BufferLocation = depth_far_vb ? depth_far_vb->GetGPUVirtualAddress() : 0;
    depth_far_view.SizeInBytes = sizeof(depth_far_vertices);
    depth_far_view.StrideInBytes = sizeof(ColorVertex);
    D3D12_VERTEX_BUFFER_VIEW depth_near_view = {};
    depth_near_view.BufferLocation = depth_near_vb ? depth_near_vb->GetGPUVirtualAddress() : 0;
    depth_near_view.SizeInBytes = sizeof(depth_near_vertices);
    depth_near_view.StrideInBytes = sizeof(ColorVertex);
    D3D12_INDEX_BUFFER_VIEW quad_index_view = {};
    quad_index_view.BufferLocation = quad_ib ? quad_ib->GetGPUVirtualAddress() : 0;
    quad_index_view.SizeInBytes = sizeof(quad_indices);
    quad_index_view.Format = DXGI_FORMAT_R32_UINT;

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(render_width);
    viewport.Height = static_cast<float>(render_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(render_width), static_cast<LONG>(render_height)};

    bool command_recorded = false;
    if (list && sample_texture && sample_upload && uav_buffer && uav_zero_upload && render_target && render_readback &&
        uav_readback) {
        D3D12_TEXTURE_COPY_LOCATION sample_src = {};
        sample_src.pResource = sample_upload;
        sample_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sample_src.PlacedFootprint = sample_footprint;
        D3D12_TEXTURE_COPY_LOCATION sample_dst = {};
        sample_dst.pResource = sample_texture;
        sample_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sample_dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&sample_dst, 0, 0, 0, &sample_src, nullptr);
        list->CopyBufferRegion(uav_buffer, 0, uav_zero_upload, 0, uav_buffer_bytes);

        D3D12_RESOURCE_BARRIER pre_draw_barriers[] = {
            transition_barrier(sample_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(uav_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list->ResourceBarrier(2, pre_draw_barriers);

        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv_handle, FALSE, &dsv_handle);
        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
        list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        if (color_pso && empty_root && triangle_vb) {
            list->SetGraphicsRootSignature(empty_root);
            list->SetPipelineState(color_pso);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &triangle_view);
            list->DrawInstanced(3, 1, 0, 0);
        }

        if (texture_pso && texture_root && textured_vb && srv_uav_heap) {
            ID3D12DescriptorHeap* heaps[] = {srv_uav_heap};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootSignature(texture_root);
            list->SetPipelineState(texture_pso);
            list->SetGraphicsRootDescriptorTable(0, srv_uav_gpu);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &textured_view);
            list->DrawInstanced(6, 1, 0, 0);
        }

        if (depth_pso && empty_root && depth_far_vb && depth_near_vb) {
            list->SetGraphicsRootSignature(empty_root);
            list->SetPipelineState(depth_pso);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &depth_far_view);
            list->DrawInstanced(6, 1, 0, 0);
            list->IASetVertexBuffers(0, 1, &depth_near_view);
            list->DrawInstanced(6, 1, 0, 0);
        }

        if (color_pso && empty_root && indexed_vb && quad_ib) {
            list->SetGraphicsRootSignature(empty_root);
            list->SetPipelineState(color_pso);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &indexed_view);
            list->IASetIndexBuffer(&quad_index_view);
            list->DrawIndexedInstanced(3, 1, 0, 0, 0);
        }

        if (SUCCEEDED(ps_uav_hr) && graphics_uav_pso && uav_root && triangle_vb && srv_uav_heap) {
            ID3D12DescriptorHeap* heaps[] = {srv_uav_heap};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootSignature(uav_root);
            list->SetPipelineState(graphics_uav_pso);
            list->SetGraphicsRootDescriptorTable(0, offset_gpu(srv_uav_gpu, srv_uav_increment, 1));
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &triangle_view);
            list->DrawInstanced(3, 1, 0, 0);
        }

        if (compute_pso && uav_root && srv_uav_heap) {
            ID3D12DescriptorHeap* heaps[] = {srv_uav_heap};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(uav_root);
            list->SetPipelineState(compute_pso);
            list->SetComputeRootDescriptorTable(0, offset_gpu(srv_uav_gpu, srv_uav_increment, 1));
            list->Dispatch(1, 1, 1);
        }

        D3D12_RESOURCE_BARRIER post_draw_barriers[] = {
            uav_barrier(uav_buffer),
            transition_barrier(render_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(uav_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        list->ResourceBarrier(3, post_draw_barriers);

        D3D12_TEXTURE_COPY_LOCATION render_src = {};
        render_src.pResource = render_target;
        render_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        render_src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION render_dst = {};
        render_dst.pResource = render_readback;
        render_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        render_dst.PlacedFootprint = render_footprint;
        list->CopyTextureRegion(&render_dst, 0, 0, 0, &render_src, nullptr);
        list->CopyBufferRegion(uav_readback, 0, uav_buffer, 0, uav_buffer_bytes);
        command_recorded = true;
    }

    HRESULT close_hr = list ? list->Close() : E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT wait_hr = E_FAIL;
    if (queue && list && fence && SUCCEEDED(close_hr) && command_recorded) {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        execute_hr = S_OK;
        signal_hr = queue->Signal(fence, 1);
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (event_handle && SUCCEEDED(signal_hr)) {
            wait_hr = fence->SetEventOnCompletion(1, event_handle);
            if (SUCCEEDED(wait_hr))
                WaitForSingleObject(event_handle, 15000);
            CloseHandle(event_handle);
        }
    }

    uint8_t* render_ptr = nullptr;
    HRESULT render_map_hr =
        render_readback ? render_readback->Map(0, nullptr, reinterpret_cast<void**>(&render_ptr)) : E_FAIL;
    Pixel background = {};
    Pixel triangle_pixel = {};
    Pixel indexed_pixel = {};
    Pixel textured_tl = {};
    Pixel textured_tr = {};
    Pixel textured_bl = {};
    Pixel depth_edge = {};
    Pixel depth_center = {};
    uint32_t render_checksum = 0;
    uint32_t baseline_checksum = clear_checksum(render_width, render_height, render_footprint.Footprint.RowPitch);
    bool background_ok = false;
    bool triangle_ok = false;
    bool indexed_ok = false;
    bool textured_ok = false;
    bool depth_ok = false;
    bool render_changed = false;
    if (SUCCEEDED(render_map_hr) && render_ptr) {
        render_checksum = fnv1a32(render_ptr, static_cast<size_t>(render_readback_bytes));
        write_binary_file(raw_readback_path, render_ptr, static_cast<size_t>(render_readback_bytes));
        background = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 6, 13);
        triangle_pixel = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 2, 2);
        indexed_pixel = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 1, 13);
        textured_tl = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 9, 1);
        textured_tr = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 14, 1);
        textured_bl = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 9, 6);
        depth_edge = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 9, 9);
        depth_center = read_pixel(render_ptr, render_footprint.Footprint.RowPitch, 12, 12);
        background_ok = pixel_equals(background, 0, 0, 0, 255);
        triangle_ok = pixel_not_clear(triangle_pixel);
        indexed_ok = pixel_equals(indexed_pixel, 255, 255, 0, 255);
        textured_ok = pixel_equals(textured_tl, 255, 255, 0, 255) && pixel_equals(textured_tr, 0, 255, 255, 255) &&
                      pixel_equals(textured_bl, 255, 0, 255, 255);
        depth_ok = pixel_equals(depth_edge, 255, 0, 0, 255) && pixel_equals(depth_center, 0, 255, 0, 255);
        render_changed = render_checksum != baseline_checksum;
        render_readback->Unmap(0, nullptr);
    }

    uint8_t* uav_ptr = nullptr;
    HRESULT uav_map_hr = uav_readback ? uav_readback->Map(0, nullptr, reinterpret_cast<void**>(&uav_ptr)) : E_FAIL;
    uint32_t graphics_uav_value = 0;
    uint32_t compute_uav_value = 0;
    bool graphics_uav_ok = false;
    bool compute_uav_ok = false;
    if (SUCCEEDED(uav_map_hr) && uav_ptr) {
        std::memcpy(&graphics_uav_value, uav_ptr + 0, sizeof(uint32_t));
        std::memcpy(&compute_uav_value, uav_ptr + 4, sizeof(uint32_t));
        graphics_uav_ok = graphics_uav_value == 0x11223344u;
        compute_uav_ok = compute_uav_value == 0x55667788u;
        uav_readback->Unmap(0, nullptr);
    }

    HRESULT logic_reset_hr = SUCCEEDED(wait_hr) ? allocator->Reset() : E_FAIL;
    if (SUCCEEDED(logic_reset_hr))
        logic_reset_hr = list->Reset(allocator, nullptr);
    bool logic_command_recorded = false;
    HRESULT logic_close_hr = E_FAIL;
    HRESULT logic_execute_hr = E_FAIL;
    HRESULT logic_signal_hr = E_FAIL;
    HRESULT logic_wait_hr = E_FAIL;
    if (SUCCEEDED(logic_reset_hr) && logic_pso && logic_readback && empty_root && triangle_vb) {
        D3D12_RESOURCE_BARRIER restore_barrier =
            transition_barrier(render_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &restore_barrier);
        list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        const float logic_clear[4] = {15.0f / 255.0f, 51.0f / 255.0f, 85.0f / 255.0f, 170.0f / 255.0f};
        list->ClearRenderTargetView(rtv_handle, logic_clear, 0, nullptr);
        list->SetGraphicsRootSignature(empty_root);
        list->SetPipelineState(logic_pso);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &triangle_view);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER copy_barrier =
            transition_barrier(render_target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &copy_barrier);
        D3D12_TEXTURE_COPY_LOCATION logic_src = {};
        logic_src.pResource = render_target;
        logic_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION logic_dst = {};
        logic_dst.pResource = logic_readback;
        logic_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        logic_dst.PlacedFootprint = render_footprint;
        list->CopyTextureRegion(&logic_dst, 0, 0, 0, &logic_src, nullptr);
        logic_command_recorded = true;
    }
    if (logic_command_recorded)
        logic_close_hr = list->Close();
    if (logic_command_recorded && SUCCEEDED(logic_close_hr) && queue && fence) {
        ID3D12CommandList* logic_lists[] = {list};
        queue->ExecuteCommandLists(1, logic_lists);
        logic_execute_hr = S_OK;
        logic_signal_hr = queue->Signal(fence, 2);
        HANDLE logic_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (logic_event && SUCCEEDED(logic_signal_hr)) {
            logic_wait_hr = fence->SetEventOnCompletion(2, logic_event);
            if (SUCCEEDED(logic_wait_hr))
                WaitForSingleObject(logic_event, 15000);
            CloseHandle(logic_event);
        }
    }
    Pixel logic_pixel = {};
    HRESULT logic_map_hr = E_FAIL;
    bool logic_operation_verified = false;
    uint8_t* logic_ptr = nullptr;
    if (SUCCEEDED(logic_wait_hr) && logic_readback)
        logic_map_hr = logic_readback->Map(0, nullptr, reinterpret_cast<void**>(&logic_ptr));
    if (SUCCEEDED(logic_map_hr) && logic_ptr) {
        logic_pixel = read_pixel(logic_ptr, render_footprint.Footprint.RowPitch, 2, 2);
        logic_operation_verified = pixel_equals(logic_pixel, 0xff, 0xff, 0xff, 0xff);
        logic_readback->Unmap(0, nullptr);
    }

    const bool entrypoints_valid = d3d12 && d3dcompiler && create_device && compile && serialize;
    const bool resources_valid =
        SUCCEEDED(render_target_hr) && SUCCEEDED(depth_target_hr) && SUCCEEDED(sample_texture_hr) &&
        SUCCEEDED(sample_upload_hr) && SUCCEEDED(render_readback_hr) && SUCCEEDED(logic_readback_hr) &&
        SUCCEEDED(uav_buffer_hr) && SUCCEEDED(uav_readback_hr) && SUCCEEDED(uav_zero_upload_hr) &&
        SUCCEEDED(triangle_vb_hr) && SUCCEEDED(textured_vb_hr) && SUCCEEDED(indexed_vb_hr) &&
        SUCCEEDED(depth_far_vb_hr) && SUCCEEDED(depth_near_vb_hr) && SUCCEEDED(quad_ib_hr) &&
        SUCCEEDED(sample_map_hr) && SUCCEEDED(uav_zero_map_hr);
    const bool heap_valid = SUCCEEDED(rtv_heap_hr) && SUCCEEDED(dsv_heap_hr) && SUCCEEDED(srv_uav_heap_hr) &&
                            SUCCEEDED(sampler_heap_hr) && srv_uav_increment != 0;
    const bool graphics_uav_supported = SUCCEEDED(ps_uav_hr) && SUCCEEDED(graphics_uav_pso_hr);
    const bool compile_valid = SUCCEEDED(vs_color_hr) && SUCCEEDED(ps_color_hr) && SUCCEEDED(ps_logic_hr) &&
                               SUCCEEDED(vs_tex_hr) && SUCCEEDED(ps_tex_hr) && SUCCEEDED(cs_hr);
    const bool roots_valid = SUCCEEDED(empty_root_blob_hr) && SUCCEEDED(texture_root_blob_hr) &&
                             SUCCEEDED(uav_root_blob_hr) && SUCCEEDED(empty_root_hr) && SUCCEEDED(texture_root_hr) &&
                             SUCCEEDED(uav_root_hr);
    const bool pipelines_valid = SUCCEEDED(color_pso_hr) && SUCCEEDED(texture_pso_hr) && SUCCEEDED(depth_pso_hr) &&
                                 SUCCEEDED(logic_pso_hr) && SUCCEEDED(compute_pso_hr) &&
                                 (!graphics_uav_supported || SUCCEEDED(graphics_uav_pso_hr));
    const bool queue_valid = SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                             SUCCEEDED(fence_hr) && SUCCEEDED(close_hr) && SUCCEEDED(execute_hr) &&
                             SUCCEEDED(signal_hr) && SUCCEEDED(wait_hr) && logic_command_recorded &&
                             SUCCEEDED(logic_close_hr) && SUCCEEDED(logic_execute_hr) && SUCCEEDED(logic_signal_hr) &&
                             SUCCEEDED(logic_wait_hr);
    const bool readback_valid = SUCCEEDED(render_map_hr) && SUCCEEDED(uav_map_hr);
    const bool draw_valid = triangle_ok && indexed_ok && textured_ok && depth_ok && background_ok;
    const bool uav_valid = compute_uav_ok && (!graphics_uav_supported || graphics_uav_ok);
    const bool pass = entrypoints_valid && SUCCEEDED(create_hr) && resources_valid && heap_valid && compile_valid &&
                      roots_valid && pipelines_valid && command_recorded && queue_valid && readback_valid &&
                      draw_valid && uav_valid && render_changed && logic_operation_verified;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-render-headless.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"d3dcompiler_47_loaded\": %s,\n", d3dcompiler ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", create_device ? "true" : "false");
    std::printf("    \"D3DCompile\": %s,\n", compile ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s\n", serialize ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"device\": {\n");
    print_hr("D3D12CreateDevice", create_hr, false);
    std::printf("  },\n");
    std::printf("  \"command_execution\": {\n");
    print_hr("queue", queue_hr);
    print_hr("allocator", allocator_hr);
    print_hr("list", list_hr);
    print_hr("fence", fence_hr);
    print_hr("close", close_hr);
    print_hr("execute", execute_hr);
    print_hr("signal", signal_hr);
    print_hr("wait", wait_hr, false);
    std::printf("  },\n");
    std::printf("  \"compile\": {\n");
    print_hr("vs_color", vs_color_hr);
    print_hr("ps_color", ps_color_hr);
    print_hr("ps_logic", ps_logic_hr);
    print_hr("vs_tex", vs_tex_hr);
    print_hr("ps_tex", ps_tex_hr);
    print_hr("ps_uav", ps_uav_hr);
    print_hr("cs_write", cs_hr, false);
    std::printf("  },\n");
    std::printf("  \"root_signatures\": {\n");
    print_hr("empty", empty_root_hr);
    print_hr("texture", texture_root_hr);
    print_hr("uav", uav_root_hr, false);
    std::printf("  },\n");
    std::printf("  \"pipeline_state\": {\n");
    print_hr("color", color_pso_hr);
    print_hr("texture", texture_pso_hr);
    print_hr("depth", depth_pso_hr);
    print_hr("logic_op", logic_pso_hr);
    print_hr("graphics_uav", graphics_uav_pso_hr);
    print_hr("compute", compute_pso_hr, false);
    std::printf("  },\n");
    std::printf("  \"render_targets\": {\n");
    print_hr("color_create", render_target_hr);
    print_hr("depth_create", depth_target_hr);
    print_hr("sample_texture_create", sample_texture_hr);
    print_hr("sample_upload_map", sample_map_hr);
    print_hr("logic_readback_create", logic_readback_hr);
    print_hr("render_readback_map", render_map_hr, false);
    std::printf("  },\n");
    std::printf("  \"draw_checks\": {\n");
    std::printf("    \"background_preserved\": %s,\n", background_ok ? "true" : "false");
    std::printf("    \"triangle_changed_pixels\": %s,\n", triangle_ok ? "true" : "false");
    std::printf("    \"indexed_geometry_verified\": %s,\n", indexed_ok ? "true" : "false");
    std::printf("    \"indexed_texture_verified\": %s,\n", textured_ok ? "true" : "false");
    std::printf("    \"depth_verified\": %s,\n", depth_ok ? "true" : "false");
    std::printf("    \"render_changed_from_clear\": %s\n", render_changed ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"logic_op\": {\n");
    print_hr("reset", logic_reset_hr);
    print_hr("close", logic_close_hr);
    print_hr("execute", logic_execute_hr);
    print_hr("signal", logic_signal_hr);
    print_hr("wait", logic_wait_hr);
    print_hr("readback_map", logic_map_hr);
    std::printf("    \"recorded\": %s,\n", logic_command_recorded ? "true" : "false");
    std::printf("    \"pixel\": [%u, %u, %u, %u],\n", logic_pixel.r, logic_pixel.g, logic_pixel.b, logic_pixel.a);
    std::printf("    \"verified\": %s\n", logic_operation_verified ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"uav_checks\": {\n");
    std::printf("    \"graphics_supported\": %s,\n", graphics_uav_supported ? "true" : "false");
    std::printf("    \"graphics_value\": %u,\n", graphics_uav_value);
    std::printf("    \"graphics_verified\": %s,\n", graphics_uav_ok ? "true" : "false");
    std::printf("    \"compute_value\": %u,\n", compute_uav_value);
    std::printf("    \"compute_verified\": %s,\n", compute_uav_ok ? "true" : "false");
    print_hr("uav_readback_map", uav_map_hr, false);
    std::printf("  },\n");
    std::printf("  \"readback\": {\n");
    std::printf("    \"row_pitch\": %u,\n", render_footprint.Footprint.RowPitch);
    std::printf("    \"checksum\": %u,\n", render_checksum);
    std::printf("    \"clear_checksum\": %u,\n", baseline_checksum);
    std::printf("    \"background\": [%u, %u, %u, %u],\n", background.r, background.g, background.b, background.a);
    std::printf("    \"triangle\": [%u, %u, %u, %u],\n", triangle_pixel.r, triangle_pixel.g, triangle_pixel.b,
                triangle_pixel.a);
    std::printf("    \"indexed\": [%u, %u, %u, %u],\n", indexed_pixel.r, indexed_pixel.g, indexed_pixel.b,
                indexed_pixel.a);
    std::printf("    \"textured_tl\": [%u, %u, %u, %u],\n", textured_tl.r, textured_tl.g, textured_tl.b, textured_tl.a);
    std::printf("    \"textured_tr\": [%u, %u, %u, %u],\n", textured_tr.r, textured_tr.g, textured_tr.b, textured_tr.a);
    std::printf("    \"textured_bl\": [%u, %u, %u, %u],\n", textured_bl.r, textured_bl.g, textured_bl.b, textured_bl.a);
    std::printf("    \"depth_edge\": [%u, %u, %u, %u],\n", depth_edge.r, depth_edge.g, depth_edge.b, depth_edge.a);
    std::printf("    \"depth_center\": [%u, %u, %u, %u]\n", depth_center.r, depth_center.g, depth_center.b,
                depth_center.a);
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
