#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <initguid.h>

static const GUID IID_D3D12DeviceFresh = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                  ID3DBlob**, ID3DBlob**);

struct ColorVertex {
    float position[3];
    float color[4];
};

struct TexVertex {
    float position[3];
    float uv[2];
};

struct Tex3DVertex {
    float position[3];
    float texface[4];
};

constexpr UINT kBackbufferWidth = 960;
constexpr UINT kBackbufferHeight = 540;
constexpr UINT kFreshTextureWidth = 16;
constexpr UINT kFreshTextureHeight = 16;
constexpr UINT kTextureStampX = 24;
constexpr UINT kTextureStampY = 24;
constexpr UINT kHeapAliasStampX = 56;
constexpr UINT kHeapAliasStampY = 24;
constexpr UINT kUavBarrierStampX = 88;
constexpr UINT kUavBarrierStampY = 24;
constexpr UINT kRtvFormatStampX = 120;
constexpr UINT kRtvFormatStampY = 24;
constexpr UINT kRenderPassStampX = 152;
constexpr UINT kRenderPassStampY = 24;
constexpr UINT kCorpusShaderStampX = 184;
constexpr UINT kCorpusShaderStampY = 24;
constexpr UINT kSrvSampleStampX = 216;
constexpr UINT kSrvSampleStampY = 24;
constexpr UINT kCbvStampX = 248;
constexpr UINT kCbvStampY = 24;
constexpr UINT kIndexedStampX = 280;
constexpr UINT kIndexedStampY = 24;
constexpr UINT kIndexedR32StampX = 280;
constexpr UINT kIndexedR32StampY = 56;
constexpr UINT kIndexedNegativeBaseStampX = 280;
constexpr UINT kIndexedNegativeBaseStampY = 88;
constexpr UINT kIndexedDynamicStrideStampX = 280;
constexpr UINT kIndexedDynamicStrideStampY = 120;
constexpr UINT kIndexedExecuteIndirectStampX = 280;
constexpr UINT kIndexedExecuteIndirectStampY = 152;
constexpr UINT kMultiSlotInstanceRootStampAX = 312;
constexpr UINT kMultiSlotInstanceRootStampAY = 56;
constexpr UINT kMultiSlotInstanceRootStampBX = 312;
constexpr UINT kMultiSlotInstanceRootStampBY = 88;
constexpr UINT kIndirectStampX = 312;
constexpr UINT kIndirectStampY = 24;
constexpr UINT kWaveOpsStampX = 344;
constexpr UINT kWaveOpsStampY = 24;
constexpr UINT kNaniteClusterStampX = 376;
constexpr UINT kNaniteClusterStampY = 24;
constexpr UINT kSubresourceViewStampX = 408;
constexpr UINT kSubresourceViewStampY = 24;
constexpr UINT kTextureArraySrvStampX = 440;
constexpr UINT kTextureArraySrvStampY = 24;
constexpr UINT kTessellationFallbackStampX = 472;
constexpr UINT kTessellationFallbackStampY = 24;
constexpr UINT kTessellationIndexedStampX = 504;
constexpr UINT kTessellationIndexedStampY = 24;
constexpr UINT kTessellationFallbackStaticVertexCount = 42;
constexpr UINT kTessellationProgressiveBackgroundVertexCount = 6;
constexpr UINT kTessellationProgressivePatchVertexCount = 3;
constexpr UINT kTessellationNonIndexedVertexCount = kTessellationFallbackStaticVertexCount +
                                                    kTessellationProgressiveBackgroundVertexCount +
                                                    kTessellationProgressivePatchVertexCount;
constexpr UINT kTessellationIndexedVertexCount = 42;
constexpr UINT kTessellationFallbackVertexCount = kTessellationNonIndexedVertexCount + kTessellationIndexedVertexCount;
constexpr UINT kTessellationProgressiveRegionX = 472;
constexpr UINT kTessellationProgressiveRegionY = 56;
constexpr UINT kTessellationProgressiveRegionWidth = 96;
constexpr UINT kTessellationProgressiveRegionHeight = 80;
constexpr UINT kTessellationProgressiveTopX = 520;
constexpr UINT kTessellationProgressiveTopY = 60;
constexpr UINT kTessellationProgressiveLeftX = 480;
constexpr UINT kTessellationProgressiveLeftY = 132;
constexpr UINT kTessellationProgressiveRightX = 560;
constexpr UINT kTessellationProgressiveRightY = 132;
constexpr UINT kTextured3DFaceCount = 3;
constexpr UINT kTextured3DFullRotationFrames = 30;
constexpr UINT kTextured3DVertexCount = 33;
constexpr UINT kTextured3DDepthSampleX = 704;
constexpr UINT kTextured3DDepthSampleY = 156;
constexpr UINT kSm5StampX = 96;
constexpr UINT kSm5StampY = 96;
constexpr UINT kProgressiveVertexTriangleRegionX = 752;
constexpr UINT kProgressiveVertexTriangleRegionY = 384;
constexpr UINT kProgressiveVertexTriangleRegionWidth = 144;
constexpr UINT kProgressiveVertexTriangleRegionHeight = 112;
constexpr UINT kProgressiveVertexTriangleTopX = 824;
constexpr UINT kProgressiveVertexTriangleTopY = 390;
constexpr UINT kProgressiveVertexTriangleLeftX = 760;
constexpr UINT kProgressiveVertexTriangleLeftY = 486;
constexpr UINT kProgressiveVertexTriangleRightX = 888;
constexpr UINT kProgressiveVertexTriangleRightY = 486;
constexpr UINT kProgressiveVertexTriangleFinalCoverageMin = 3000;
constexpr size_t kFreshTexturePayloadBytes = kFreshTextureWidth * kFreshTextureHeight * 4;

struct TexturePayload {
    uint64_t fnv1a64 = 1469598103934665603ull;
    uint64_t declared_size = 0;
    uint32_t bytes_from_file = 0;
    std::string family;
    std::string label;
    std::string extension;
    std::string destination;
    std::string source_path;
    std::string sha256;
    std::array<uint8_t, kFreshTexturePayloadBytes> rgba = {};
};

static bool fill_texture_upload(ID3D12Resource* upload, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                const TexturePayload& payload, UINT width, UINT height);

template <typename T> static void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

static std::string narrow_wide(const wchar_t* input) {
    std::string out;
    if (!input)
        return out;
    for (const wchar_t* cursor = input; *cursor; ++cursor)
        out.push_back((*cursor >= 32 && *cursor <= 126) ? static_cast<char>(*cursor) : '?');
    return out;
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
    if (!needed)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (!written)
        return "";
    value.resize(written);
    return value;
}

static uint32_t getenv_u32(const char* key, uint32_t fallback) {
    std::string value = getenv_string(key);
    if (value.empty())
        return fallback;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > 36000)
        return fallback;
    return static_cast<uint32_t>(parsed);
}

static std::string wine_path(std::string path) {
    if (!path.empty() && path[0] == '/')
        path = "Z:" + path;
    for (char& c : path) {
        if (c == '/')
            c = '\\';
    }
    return path;
}

static uint64_t fnv1a_update(uint64_t hash, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool read_file_bytes(const std::string& path, std::vector<uint8_t>& bytes, uint64_t& file_hash) {
    std::ifstream file(wine_path(path), std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size <= 0)
        return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (file.gcount() != size)
        return false;
    file_hash = fnv1a_update(1469598103934665603ull, bytes.data(), bytes.size());
    return true;
}

static bool read_file_hash(const std::string& path, uint64_t& aggregate_hash, uint64_t& bytes, uint64_t& file_hash,
                           TexturePayload* payload = nullptr) {
    std::ifstream file(wine_path(path), std::ios::binary);
    if (!file)
        return false;
    file_hash = 1469598103934665603ull;
    if (payload) {
        payload->fnv1a64 = file_hash;
        payload->bytes_from_file = 0;
        payload->rgba.fill(0);
    }
    std::array<uint8_t, 64 * 1024> buffer{};
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        std::streamsize count = file.gcount();
        if (count > 0) {
            aggregate_hash = fnv1a_update(aggregate_hash, buffer.data(), static_cast<size_t>(count));
            file_hash = fnv1a_update(file_hash, buffer.data(), static_cast<size_t>(count));
            bytes += static_cast<uint64_t>(count);
            if (payload && payload->bytes_from_file < payload->rgba.size()) {
                const size_t dst_offset = payload->bytes_from_file;
                const size_t copy_bytes =
                    std::min<size_t>(static_cast<size_t>(count), payload->rgba.size() - dst_offset);
                std::memcpy(payload->rgba.data() + dst_offset, buffer.data(), copy_bytes);
                payload->bytes_from_file += static_cast<uint32_t>(copy_bytes);
            }
        }
    }
    if (payload)
        payload->fnv1a64 = file_hash;
    return true;
}

struct CorpusStats {
    std::string tsv_path;
    uint32_t rows = 0;
    uint32_t shaders_seen = 0;
    uint32_t textures_seen = 0;
    uint32_t shader_files_loaded = 0;
    uint32_t texture_files_loaded = 0;
    uint32_t failed_loads = 0;
    uint64_t bytes_loaded = 0;
    uint64_t texture_payload_bytes_from_files = 0;
    uint64_t fnv1a64 = 1469598103934665603ull;
    std::vector<uint64_t> shader_hashes;
    std::vector<uint64_t> texture_hashes;
    std::vector<TexturePayload> texture_payloads;
    std::string position_color_vs_path;
    std::string position_color_ps_path;
    uint64_t position_color_vs_fnv1a64 = 0;
    uint64_t position_color_ps_fnv1a64 = 0;
};

static std::vector<std::string> split_tsv(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    std::stringstream ss(line);
    while (std::getline(ss, field, '\t'))
        fields.push_back(field);
    return fields;
}

static CorpusStats load_corpus_tsv(const std::string& path, uint32_t target_shaders, uint32_t target_textures) {
    CorpusStats stats;
    stats.tsv_path = path;
    if (path.empty())
        return stats;

    const auto has_texture_family = [&stats](const std::string& family) {
        for (const TexturePayload& payload : stats.texture_payloads) {
            if (payload.family == family)
                return true;
        }
        return false;
    };

    std::ifstream file(wine_path(path));
    std::string line;
    bool header = true;
    while (std::getline(file, line)) {
        if (header) {
            header = false;
            continue;
        }
        auto fields = split_tsv(line);
        if (fields.size() < 8)
            continue;
        const std::string& category = fields[2];
        const std::string& destination = fields[6];
        stats.rows++;
        if (category == "shader") {
            stats.shaders_seen++;
            const bool is_position_color_vs = destination.find("D3D12StateObjectDatabase") != std::string::npos &&
                                              destination.find("PositionColorVS.hlsl") != std::string::npos;
            const bool is_position_color_ps = destination.find("D3D12StateObjectDatabase") != std::string::npos &&
                                              destination.find("PositionColorPS.hlsl") != std::string::npos;
            if ((is_position_color_vs && stats.position_color_vs_path.empty()) ||
                (is_position_color_ps && stats.position_color_ps_path.empty())) {
                uint64_t candidate_aggregate = 1469598103934665603ull;
                uint64_t candidate_bytes = 0;
                uint64_t candidate_hash = 0;
                if (read_file_hash(destination, candidate_aggregate, candidate_bytes, candidate_hash)) {
                    if (is_position_color_vs) {
                        stats.position_color_vs_path = destination;
                        stats.position_color_vs_fnv1a64 = candidate_hash;
                    }
                    if (is_position_color_ps) {
                        stats.position_color_ps_path = destination;
                        stats.position_color_ps_fnv1a64 = candidate_hash;
                    }
                }
            }
            if (stats.shader_files_loaded >= target_shaders)
                continue;
            uint64_t file_hash = 0;
            if (read_file_hash(destination, stats.fnv1a64, stats.bytes_loaded, file_hash)) {
                stats.shader_files_loaded++;
                stats.shader_hashes.push_back(file_hash);
                if (is_position_color_vs) {
                    stats.position_color_vs_path = destination;
                    stats.position_color_vs_fnv1a64 = file_hash;
                }
                if (is_position_color_ps) {
                    stats.position_color_ps_path = destination;
                    stats.position_color_ps_fnv1a64 = file_hash;
                }
            } else {
                stats.failed_loads++;
            }
        } else if (category == "texture") {
            stats.textures_seen++;
            uint64_t file_hash = 0;
            TexturePayload payload;
            if (read_file_hash(destination, stats.fnv1a64, stats.bytes_loaded, file_hash, &payload)) {
                payload.family = fields[0];
                payload.label = fields[1];
                payload.extension = fields[3];
                payload.declared_size = std::strtoull(fields[4].c_str(), nullptr, 10);
                payload.sha256 = fields[5];
                payload.destination = destination;
                payload.source_path = fields[7];
                stats.texture_files_loaded++;
                stats.texture_hashes.push_back(file_hash);
                stats.texture_payloads.push_back(payload);
                stats.texture_payload_bytes_from_files += payload.bytes_from_file;
            } else {
                stats.failed_loads++;
            }
        }
        const bool textured_3d_families_ready =
            has_texture_family("unreal") && has_texture_family("unity-sdk") && has_texture_family("microsoft-sdk");
        if (stats.shader_files_loaded >= target_shaders && stats.texture_files_loaded >= target_textures &&
            textured_3d_families_ready && !stats.position_color_vs_path.empty() &&
            !stats.position_color_ps_path.empty())
            break;
    }
    return stats;
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
    desc.Alignment = 0;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
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
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

static D3D12_RESOURCE_BARRIER aliasing_barrier(ID3D12Resource* before, ID3D12Resource* after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Aliasing.pResourceBefore = before;
    barrier.Aliasing.pResourceAfter = after;
    return barrier;
}

static D3D12_RESOURCE_BARRIER uav_resource_barrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

static D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment, UINT index) {
    start.ptr += static_cast<SIZE_T>(increment) * index;
    return start;
}

static LRESULT CALLBACK game_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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

static void pump_messages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static bool wait_for_fence(ID3D12Fence* fence, UINT64 value, HANDLE event_handle) {
    if (fence->GetCompletedValue() >= value)
        return true;
    if (FAILED(fence->SetEventOnCompletion(value, event_handle)))
        return false;
    return WaitForSingleObject(event_handle, 5000) == WAIT_OBJECT_0;
}

static bool guid_equal(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

static std::string guid_string(const GUID& guid) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(guid.Data1), static_cast<unsigned>(guid.Data2),
                  static_cast<unsigned>(guid.Data3), static_cast<unsigned>(guid.Data4[0]),
                  static_cast<unsigned>(guid.Data4[1]), static_cast<unsigned>(guid.Data4[2]),
                  static_cast<unsigned>(guid.Data4[3]), static_cast<unsigned>(guid.Data4[4]),
                  static_cast<unsigned>(guid.Data4[5]), static_cast<unsigned>(guid.Data4[6]),
                  static_cast<unsigned>(guid.Data4[7]));
    return std::string(buf);
}

template <typename T> static bool com_vtable_nonnull(T* object) {
    if (!object)
        return false;
    void** vtbl = *reinterpret_cast<void***>(object);
    return vtbl && vtbl[0] && vtbl[1] && vtbl[2];
}

struct AbiSemanticStats {
    HRESULT query_device_iunknown_hr = E_FAIL;
    HRESULT query_device_self_hr = E_FAIL;
    HRESULT query_factory_iunknown_hr = E_FAIL;
    HRESULT query_adapter_iunknown_hr = E_FAIL;
    HRESULT query_queue_self_hr = E_FAIL;
    HRESULT query_queue_iunknown_hr = E_FAIL;
    HRESULT query_allocator_self_hr = E_FAIL;
    HRESULT query_list_self_hr = E_FAIL;
    HRESULT query_fence_self_hr = E_FAIL;
    HRESULT query_swapchain3_self_hr = E_FAIL;
    HRESULT queue_get_device_hr = E_FAIL;
    HRESULT allocator_get_device_hr = E_FAIL;
    HRESULT list_get_device_hr = E_FAIL;
    HRESULT fence_get_device_hr = E_FAIL;
    HRESULT queue_device_iunknown_hr = E_FAIL;
    HRESULT allocator_device_iunknown_hr = E_FAIL;
    HRESULT list_device_iunknown_hr = E_FAIL;
    HRESULT fence_device_iunknown_hr = E_FAIL;
    HRESULT private_data_set_hr = E_FAIL;
    HRESULT private_data_get_hr = E_FAIL;
    UINT private_data_size = 0;
    uint32_t validated_frames = 0;
    bool guid_constants_ok = false;
    bool query_interface_ok = false;
    bool vtable_layout_ok = false;
    bool device_child_get_device_ok = false;
    bool device_child_identity_ok = false;
    bool private_data_roundtrip_ok = false;
    bool present_tie_ok = false;
    bool pass = false;
};

static AbiSemanticStats exercise_abi_semantics(IDXGIFactory4* factory, IDXGIAdapter1* adapter, ID3D12Device* device,
                                               ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator,
                                               ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                                               IDXGISwapChain3* swapchain) {
    AbiSemanticStats stats;
    const GUID expected_device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    const GUID expected_factory4 = {0x1bc6ea02, 0xef36, 0x464f, {0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a}};
    const GUID expected_adapter1 = {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
    const GUID expected_queue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
    const GUID expected_allocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
    const GUID expected_list = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
    const GUID expected_fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
    const GUID expected_swapchain3 = {0x94d99bdb, 0xf1f8, 0x4ab0, {0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1}};
    stats.guid_constants_ok =
        guid_equal(__uuidof(ID3D12Device), expected_device) && guid_equal(__uuidof(IDXGIFactory4), expected_factory4) &&
        guid_equal(__uuidof(IDXGIAdapter1), expected_adapter1) &&
        guid_equal(__uuidof(ID3D12CommandQueue), expected_queue) &&
        guid_equal(__uuidof(ID3D12CommandAllocator), expected_allocator) &&
        guid_equal(__uuidof(ID3D12GraphicsCommandList), expected_list) &&
        guid_equal(__uuidof(ID3D12Fence), expected_fence) && guid_equal(__uuidof(IDXGISwapChain3), expected_swapchain3);

    IUnknown* device_unknown = nullptr;
    ID3D12Device* device_self = nullptr;
    IUnknown* factory_unknown = nullptr;
    IUnknown* adapter_unknown = nullptr;
    ID3D12CommandQueue* queue_self = nullptr;
    IUnknown* queue_unknown = nullptr;
    ID3D12CommandAllocator* allocator_self = nullptr;
    ID3D12GraphicsCommandList* list_self = nullptr;
    ID3D12Fence* fence_self = nullptr;
    IDXGISwapChain3* swapchain_self = nullptr;
    ID3D12Device* queue_device = nullptr;
    ID3D12Device* allocator_device = nullptr;
    ID3D12Device* list_device = nullptr;
    ID3D12Device* fence_device = nullptr;
    IUnknown* queue_device_unknown = nullptr;
    IUnknown* allocator_device_unknown = nullptr;
    IUnknown* list_device_unknown = nullptr;
    IUnknown* fence_device_unknown = nullptr;

    stats.query_device_iunknown_hr =
        device ? device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&device_unknown)) : E_FAIL;
    stats.query_device_self_hr =
        device ? device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device_self)) : E_FAIL;
    stats.query_factory_iunknown_hr =
        factory ? factory->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&factory_unknown)) : E_FAIL;
    stats.query_adapter_iunknown_hr =
        adapter ? adapter->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&adapter_unknown)) : E_FAIL;
    stats.query_queue_self_hr =
        queue ? queue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue_self)) : E_FAIL;
    stats.query_queue_iunknown_hr =
        queue ? queue->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&queue_unknown)) : E_FAIL;
    stats.query_allocator_self_hr = allocator ? allocator->QueryInterface(__uuidof(ID3D12CommandAllocator),
                                                                          reinterpret_cast<void**>(&allocator_self))
                                              : E_FAIL;
    stats.query_list_self_hr =
        list ? list->QueryInterface(__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list_self)) : E_FAIL;
    stats.query_fence_self_hr =
        fence ? fence->QueryInterface(__uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence_self)) : E_FAIL;
    stats.query_swapchain3_self_hr =
        swapchain ? swapchain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapchain_self))
                  : E_FAIL;
    stats.queue_get_device_hr =
        queue ? queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&queue_device)) : E_FAIL;
    stats.allocator_get_device_hr =
        allocator ? allocator->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&allocator_device)) : E_FAIL;
    stats.list_get_device_hr =
        list ? list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&list_device)) : E_FAIL;
    stats.fence_get_device_hr =
        fence ? fence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&fence_device)) : E_FAIL;
    stats.queue_device_iunknown_hr =
        queue_device ? queue_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&queue_device_unknown))
                     : E_FAIL;
    stats.allocator_device_iunknown_hr =
        allocator_device
            ? allocator_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&allocator_device_unknown))
            : E_FAIL;
    stats.list_device_iunknown_hr =
        list_device ? list_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&list_device_unknown))
                    : E_FAIL;
    stats.fence_device_iunknown_hr =
        fence_device ? fence_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&fence_device_unknown))
                     : E_FAIL;

    const GUID private_data_guid = {0x5ed3f4a0, 0x4f8f, 0x4e7a, {0x93, 0xa0, 0x9b, 0x51, 0xd3, 0x12, 0x60, 0x06}};
    uint8_t payload[16] = {0x4d, 0x31, 0x32, 0x2d, 0x41, 0x42, 0x49, 0x2d,
                           0x53, 0x45, 0x4d, 0x41, 0x4e, 0x54, 0x49, 0x43};
    uint8_t expected_payload[16] = {};
    std::memcpy(expected_payload, payload, sizeof(payload));
    uint8_t readback[16] = {};
    stats.private_data_set_hr = device ? device->SetPrivateData(private_data_guid, sizeof(payload), payload) : E_FAIL;
    std::memset(payload, 0xa5, sizeof(payload));
    stats.private_data_size = sizeof(readback);
    stats.private_data_get_hr =
        device ? device->GetPrivateData(private_data_guid, &stats.private_data_size, readback) : E_FAIL;
    stats.private_data_roundtrip_ok = SUCCEEDED(stats.private_data_set_hr) && SUCCEEDED(stats.private_data_get_hr) &&
                                      stats.private_data_size == sizeof(expected_payload) &&
                                      std::memcmp(expected_payload, readback, sizeof(expected_payload)) == 0;

    stats.query_interface_ok = SUCCEEDED(stats.query_device_iunknown_hr) && SUCCEEDED(stats.query_device_self_hr) &&
                               SUCCEEDED(stats.query_factory_iunknown_hr) &&
                               SUCCEEDED(stats.query_adapter_iunknown_hr) && SUCCEEDED(stats.query_queue_self_hr) &&
                               SUCCEEDED(stats.query_queue_iunknown_hr) && SUCCEEDED(stats.query_allocator_self_hr) &&
                               SUCCEEDED(stats.query_list_self_hr) && SUCCEEDED(stats.query_fence_self_hr) &&
                               SUCCEEDED(stats.query_swapchain3_self_hr) && device_unknown && device_self &&
                               factory_unknown && adapter_unknown && queue_self && queue_unknown && allocator_self &&
                               list_self && fence_self && swapchain_self;
    stats.vtable_layout_ok = com_vtable_nonnull(device) && com_vtable_nonnull(factory) && com_vtable_nonnull(adapter) &&
                             com_vtable_nonnull(queue) && com_vtable_nonnull(allocator) && com_vtable_nonnull(list) &&
                             com_vtable_nonnull(fence) && com_vtable_nonnull(swapchain);
    stats.device_child_get_device_ok = SUCCEEDED(stats.queue_get_device_hr) &&
                                       SUCCEEDED(stats.allocator_get_device_hr) &&
                                       SUCCEEDED(stats.list_get_device_hr) && SUCCEEDED(stats.fence_get_device_hr) &&
                                       queue_device && allocator_device && list_device && fence_device;
    stats.device_child_identity_ok = stats.device_child_get_device_ok && device_unknown && queue_device_unknown &&
                                     allocator_device_unknown && list_device_unknown && fence_device_unknown &&
                                     queue_device_unknown == device_unknown &&
                                     allocator_device_unknown == device_unknown &&
                                     list_device_unknown == device_unknown && fence_device_unknown == device_unknown;

    safe_release(device_unknown);
    safe_release(device_self);
    safe_release(factory_unknown);
    safe_release(adapter_unknown);
    safe_release(queue_self);
    safe_release(queue_unknown);
    safe_release(allocator_self);
    safe_release(list_self);
    safe_release(fence_self);
    safe_release(swapchain_self);
    safe_release(queue_device_unknown);
    safe_release(allocator_device_unknown);
    safe_release(list_device_unknown);
    safe_release(fence_device_unknown);
    safe_release(queue_device);
    safe_release(allocator_device);
    safe_release(list_device);
    safe_release(fence_device);
    return stats;
}

static HRESULT compile_shader(D3DCompileFn compile, const char* source, const char* entry, const char* target,
                              ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile ? compile(source, std::strlen(source), "vkd3d_fresh_game_loading.hlsl", nullptr, nullptr, entry,
                                   target, 0, 0, blob, &errors)
                         : E_FAIL;
    if (errors)
        errors->Release();
    return hr;
}

static HRESULT compile_shader_bytes(D3DCompileFn compile, const std::vector<uint8_t>& source, const char* source_name,
                                    const char* entry, const char* target, ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = compile ? compile(source.data(), source.size(), source_name, nullptr, nullptr, entry, target, 0, 0,
                                   blob, &errors)
                         : E_FAIL;
    if (errors)
        errors->Release();
    return hr;
}

static HRESULT serialize_root_signature(SerializeRootSignatureFn serialize, const D3D12_ROOT_SIGNATURE_DESC& desc,
                                        ID3DBlob** blob) {
    ID3DBlob* errors = nullptr;
    HRESULT hr = serialize ? serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &errors) : E_FAIL;
    if (errors)
        errors->Release();
    return hr;
}

struct VisibleSceneStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    uint32_t target_frames = 0;
    uint32_t draw_calls = 0;
    uint32_t quads_per_frame = 0;
    uint32_t vertices_per_frame = 0;
    uint32_t sm5_stamp_quads_per_frame = 0;
    uint32_t sm5_stamp_samples_checked = 0;
    uint32_t sm5_stamp_matches = 0;
    uint8_t sm5_stamp_expected_rgba[4] = {0, 0, 0, 0};
    uint8_t sm5_stamp_rgba[4] = {0, 0, 0, 0};
    bool sm5_stamp_present_pass = false;
    uint32_t progressive_triangle_vertices_per_frame = 0;
    uint32_t progressive_triangle_draw_calls = 0;
    uint32_t progressive_triangle_frames_validated = 0;
    uint32_t progressive_triangle_samples_checked = 0;
    uint32_t progressive_triangle_seed_pixels = 0;
    uint32_t progressive_triangle_final_pixels = 0;
    uint32_t progressive_triangle_peak_pixels = 0;
    uint32_t progressive_triangle_final_red_dominant_pixels = 0;
    uint32_t progressive_triangle_final_green_dominant_pixels = 0;
    uint32_t progressive_triangle_final_blue_dominant_pixels = 0;
    bool progressive_triangle_coverage_monotonic = true;
    bool progressive_triangle_seed_point_pass = false;
    bool progressive_triangle_full_pass = false;
    bool progressive_triangle_rgb_channels_present = false;
    bool progressive_triangle_present_pass = false;
    std::vector<uint32_t> progressive_triangle_coverage_counts;
    bool pass = false;
};

struct VisibleSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ColorVertex* mapped_vertices = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    D3D12_VERTEX_BUFFER_VIEW progressive_triangle_vertex_view = {};
    uint32_t max_quads = 96;
    VisibleSceneStats stats;
};

static void sm5_expected_stamp_rgba(uint32_t frame, uint8_t out[4]) {
    switch (frame % 3u) {
    case 0:
        out[0] = 0;
        out[1] = 255;
        out[2] = 255;
        out[3] = 255;
        break;
    case 1:
        out[0] = 255;
        out[1] = 255;
        out[2] = 0;
        out[3] = 255;
        break;
    default:
        out[0] = 255;
        out[1] = 0;
        out[2] = 64;
        out[3] = 255;
        break;
    }
}

static const char* kVisibleLoadingHlsl = R"(
struct VSIn {
  float3 position : POSITION;
  float4 color : COLOR0;
};
struct PSIn {
  float4 position : SV_POSITION;
  float4 color : COLOR0;
};
PSIn VSMain(VSIn input) {
  PSIn output;
  output.position = float4(input.position, 1.0);
  output.color = input.color;
  return output;
}
float4 PSMain(PSIn input) : SV_TARGET {
  return input.color;
}
)";

static void write_quad(ColorVertex* dst, float x0, float y0, float x1, float y1, float z, float r, float g, float b,
                       float a) {
    const ColorVertex quad[6] = {
        {{x0, y0, z}, {r, g, b, a}}, {{x0, y1, z}, {r, g, b, a}}, {{x1, y0, z}, {r, g, b, a}},
        {{x1, y0, z}, {r, g, b, a}}, {{x0, y1, z}, {r, g, b, a}}, {{x1, y1, z}, {r, g, b, a}},
    };
    std::memcpy(dst, quad, sizeof(quad));
}

static float progressive_rgb_triangle_scale(uint32_t frame, uint32_t target_frames) {
    constexpr float seed_scale = 0.050f;
    if (target_frames <= 1u)
        return 1.0f;
    const float t = std::min<float>(1.0f, static_cast<float>(frame) / static_cast<float>(target_frames - 1u));
    return seed_scale + (1.0f - seed_scale) * t;
}

static ColorVertex progressive_rgb_triangle_vertex(float pixel_x, float pixel_y, float scale, float r, float g,
                                                   float b) {
    constexpr float center_x =
        (static_cast<float>(kProgressiveVertexTriangleTopX) + static_cast<float>(kProgressiveVertexTriangleLeftX) +
         static_cast<float>(kProgressiveVertexTriangleRightX)) /
        3.0f;
    constexpr float center_y =
        (static_cast<float>(kProgressiveVertexTriangleTopY) + static_cast<float>(kProgressiveVertexTriangleLeftY) +
         static_cast<float>(kProgressiveVertexTriangleRightY)) /
        3.0f;
    const float scaled_x = center_x + (pixel_x - center_x) * scale;
    const float scaled_y = center_y + (pixel_y - center_y) * scale;
    const float ndc_x = scaled_x * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f;
    const float ndc_y = 1.0f - scaled_y * 2.0f / static_cast<float>(kBackbufferHeight);
    return {{ndc_x, ndc_y, 0.06f}, {r, g, b, 1.0f}};
}

static void write_progressive_rgb_triangle(ColorVertex* dst, uint32_t frame, uint32_t target_frames) {
    const float region_x0 = static_cast<float>(kProgressiveVertexTriangleRegionX);
    const float region_y0 = static_cast<float>(kProgressiveVertexTriangleRegionY);
    const float region_x1 =
        static_cast<float>(kProgressiveVertexTriangleRegionX + kProgressiveVertexTriangleRegionWidth);
    const float region_y1 =
        static_cast<float>(kProgressiveVertexTriangleRegionY + kProgressiveVertexTriangleRegionHeight);
    write_quad(dst, region_x0 * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f,
               1.0f - region_y1 * 2.0f / static_cast<float>(kBackbufferHeight),
               region_x1 * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f,
               1.0f - region_y0 * 2.0f / static_cast<float>(kBackbufferHeight), 0.055f, 0.01f, 0.02f, 0.05f, 1.0f);

    const float scale = progressive_rgb_triangle_scale(frame, target_frames);
    for (uint32_t repeat = 0; repeat < 3u; ++repeat) {
        ColorVertex* tri = dst + 6u + repeat * 3u;
        tri[0] = progressive_rgb_triangle_vertex(static_cast<float>(kProgressiveVertexTriangleTopX),
                                                 static_cast<float>(kProgressiveVertexTriangleTopY), scale, 1.0f, 0.0f,
                                                 0.0f);
        tri[1] = progressive_rgb_triangle_vertex(static_cast<float>(kProgressiveVertexTriangleLeftX),
                                                 static_cast<float>(kProgressiveVertexTriangleLeftY), scale, 0.0f, 1.0f,
                                                 0.0f);
        tri[2] = progressive_rgb_triangle_vertex(static_cast<float>(kProgressiveVertexTriangleRightX),
                                                 static_cast<float>(kProgressiveVertexTriangleRightY), scale, 0.0f,
                                                 0.0f, 1.0f);
    }
}

static uint32_t populate_visible_loading_vertices(VisibleSceneResources& scene, uint32_t frame,
                                                  const CorpusStats& corpus) {
    if (!scene.mapped_vertices)
        return 0;
    ColorVertex* out = scene.mapped_vertices;
    uint32_t quads = 0;
    const auto push = [&](float x0, float y0, float x1, float y1, float z, float r, float g, float b, float a) {
        if (quads >= scene.max_quads)
            return;
        write_quad(out + quads * 6, x0, y0, x1, y1, z, r, g, b, a);
        quads++;
    };

    const float progress = static_cast<float>((frame % 240u) + 1u) / 240.0f;
    const uint64_t tint = corpus.fnv1a64;
    const float tint_r = 0.20f + static_cast<float>((tint >> 0) & 0xffu) / 510.0f;
    const float tint_g = 0.20f + static_cast<float>((tint >> 16) & 0xffu) / 510.0f;
    const float tint_b = 0.35f + static_cast<float>((tint >> 32) & 0xffu) / 420.0f;

    push(-0.94f, -0.90f, 0.94f, 0.90f, 0.50f, 0.015f, 0.025f, 0.055f, 1.0f);
    push(-0.88f, 0.62f, 0.88f, 0.76f, 0.45f, tint_r, tint_g, tint_b, 1.0f);
    push(-0.86f, -0.76f, 0.86f, -0.66f, 0.35f, 0.05f, 0.08f, 0.12f, 1.0f);
    push(-0.84f, -0.74f, -0.84f + 1.68f * progress, -0.68f, 0.25f, 0.20f, 0.80f, 1.0f, 1.0f);

    const float center_x = -0.72f;
    const float center_y = -0.42f;
    const float cell_w = 0.10f;
    const float cell_h = 0.10f;
    uint32_t tile_index = 0;
    for (uint32_t y = 0; y < 5; ++y) {
        for (uint32_t x = 0; x < 12; ++x) {
            uint64_t seed = corpus.texture_hashes.empty()
                                ? (corpus.fnv1a64 + tile_index * 0x9e3779b97f4a7c15ull)
                                : corpus.texture_hashes[tile_index % corpus.texture_hashes.size()];
            const bool hot = ((tile_index + frame / 4u) % 17u) == 0u;
            const float r = (static_cast<float>((seed >> 8) & 0xffu) / 255.0f) * (hot ? 1.0f : 0.55f);
            const float g = (static_cast<float>((seed >> 24) & 0xffu) / 255.0f) * (hot ? 1.0f : 0.55f);
            const float b = (static_cast<float>((seed >> 40) & 0xffu) / 255.0f) * (hot ? 1.0f : 0.70f);
            const float x0 = center_x + x * cell_w;
            const float y0 = center_y + y * cell_h;
            push(x0, y0, x0 + cell_w * 0.72f, y0 + cell_h * 0.72f, 0.18f, 0.15f + r, 0.15f + g, 0.20f + b, 1.0f);
            tile_index++;
        }
    }

    const float pulse = static_cast<float>((frame % 90u) < 45u ? (frame % 45u) : (90u - (frame % 90u))) / 45.0f;
    push(-0.16f, 0.18f, 0.16f, 0.50f, 0.12f, 0.90f, 0.96f, 1.0f, 1.0f);
    push(-0.28f, 0.05f, -0.04f, 0.29f, 0.10f, 0.20f + pulse, 0.50f, 1.0f, 1.0f);
    push(0.04f, 0.05f, 0.28f, 0.29f, 0.10f, 1.0f, 0.45f + pulse * 0.35f, 0.25f, 1.0f);

    uint8_t sm5_expected[4] = {};
    sm5_expected_stamp_rgba(frame, sm5_expected);
    const float sm5_x0 = -0.86f;
    const float sm5_y0 = 0.54f;
    const float sm5_x1 = -0.70f;
    const float sm5_y1 = 0.74f;
    push(sm5_x0, sm5_y0, sm5_x1, sm5_y1, 0.08f, static_cast<float>(sm5_expected[0] ^ 0xffu) / 255.0f,
         static_cast<float>(sm5_expected[1] ^ 0xffu) / 255.0f, static_cast<float>(sm5_expected[2] ^ 0xffu) / 255.0f,
         static_cast<float>(sm5_expected[3] ^ 0xffu) / 255.0f);
    push(sm5_x0, sm5_y0, sm5_x1, sm5_y1, 0.07f, static_cast<float>(sm5_expected[0]) / 255.0f,
         static_cast<float>(sm5_expected[1]) / 255.0f, static_cast<float>(sm5_expected[2]) / 255.0f,
         static_cast<float>(sm5_expected[3]) / 255.0f);
    scene.stats.sm5_stamp_quads_per_frame = 2;

    const uint32_t progressive_triangle_vertex_offset = quads * 6;
    write_progressive_rgb_triangle(out + progressive_triangle_vertex_offset, frame, scene.stats.target_frames);
    scene.stats.progressive_triangle_vertices_per_frame = 15;
    scene.progressive_triangle_vertex_view.BufferLocation =
        scene.vertex_view.BufferLocation + progressive_triangle_vertex_offset * sizeof(ColorVertex);
    scene.progressive_triangle_vertex_view.SizeInBytes =
        scene.stats.progressive_triangle_vertices_per_frame * sizeof(ColorVertex);
    scene.progressive_triangle_vertex_view.StrideInBytes = sizeof(ColorVertex);

    scene.stats.quads_per_frame = quads;
    scene.stats.vertices_per_frame = quads * 6;
    return scene.stats.vertices_per_frame;
}

static VisibleSceneResources create_visible_scene(ID3D12Device* device, D3DCompileFn compile,
                                                  SerializeRootSignatureFn serialize, uint32_t target_frames) {
    VisibleSceneResources scene;
    scene.stats.target_frames = target_frames;
    scene.stats.d3dcompiler_loaded = compile != nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* root_blob = nullptr;
    scene.stats.compile_vs_hr = compile_shader(compile, kVisibleLoadingHlsl, "VSMain", "vs_5_0", &vs);
    scene.stats.compile_ps_hr = compile_shader(compile, kVisibleLoadingHlsl, "PSMain", "ps_5_0", &ps);

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    scene.stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        scene.stats.create_root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        scene.stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        const UINT64 vb_bytes = static_cast<UINT64>(scene.max_quads) * 6u * sizeof(ColorVertex);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(vb_bytes);
        scene.stats.create_vertex_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&scene.mapped_vertices));
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = static_cast<UINT>(vb_bytes);
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
        }
    }

    if (root_blob)
        root_blob->Release();
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
    scene.stats.pass = scene.stats.d3dcompiler_loaded && SUCCEEDED(scene.stats.compile_vs_hr) &&
                       SUCCEEDED(scene.stats.compile_ps_hr) && SUCCEEDED(scene.stats.serialize_root_hr) &&
                       SUCCEEDED(scene.stats.create_root_hr) && SUCCEEDED(scene.stats.create_pso_hr) &&
                       SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.mapped_vertices != nullptr;
    return scene;
}

static void destroy_visible_scene(VisibleSceneResources& scene) {
    if (scene.vertex_buffer && scene.mapped_vertices) {
        scene.vertex_buffer->Unmap(0, nullptr);
        scene.mapped_vertices = nullptr;
    }
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct CorpusShaderStats {
    std::string vs_path;
    std::string ps_path;
    bool vs_loaded = false;
    bool ps_loaded = false;
    uint64_t vs_bytes = 0;
    uint64_t ps_bytes = 0;
    uint64_t vs_fnv1a64 = 0;
    uint64_t ps_fnv1a64 = 0;
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    uint32_t draw_calls = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {64, 192, 32, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool present_pass = false;
    bool pass = false;
};

struct CorpusShaderSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    CorpusShaderStats stats;
};

static void corpus_shader_expected_rgba(uint8_t out[4]) {
    out[0] = 64;
    out[1] = 192;
    out[2] = 32;
    out[3] = 255;
}

static float ndc_x_from_pixel(float x, float width) {
    return x * 2.0f / width - 1.0f;
}

static float ndc_y_from_pixel(float y, float height) {
    return 1.0f - y * 2.0f / height;
}

static CorpusShaderSceneResources create_corpus_shader_scene(ID3D12Device* device, D3DCompileFn compile,
                                                             SerializeRootSignatureFn serialize,
                                                             const CorpusStats& corpus, UINT backbuffer_width,
                                                             UINT backbuffer_height) {
    CorpusShaderSceneResources scene;
    scene.stats.vs_path = corpus.position_color_vs_path;
    scene.stats.ps_path = corpus.position_color_ps_path;
    scene.stats.d3dcompiler_loaded = compile != nullptr;
    corpus_shader_expected_rgba(scene.stats.expected_rgba);

    std::vector<uint8_t> vs_source;
    std::vector<uint8_t> ps_source;
    scene.stats.vs_loaded = read_file_bytes(scene.stats.vs_path, vs_source, scene.stats.vs_fnv1a64);
    scene.stats.ps_loaded = read_file_bytes(scene.stats.ps_path, ps_source, scene.stats.ps_fnv1a64);
    scene.stats.vs_bytes = static_cast<uint64_t>(vs_source.size());
    scene.stats.ps_bytes = static_cast<uint64_t>(ps_source.size());

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    if (scene.stats.vs_loaded)
        scene.stats.compile_vs_hr = compile_shader_bytes(compile, vs_source, "fresh_corpus_PositionColorVS.hlsl",
                                                         "PositionColorVS", "vs_5_0", &vs);
    if (scene.stats.ps_loaded)
        scene.stats.compile_ps_hr = compile_shader_bytes(compile, ps_source, "fresh_corpus_PositionColorPS.hlsl",
                                                         "PositionColorPS", "ps_5_0", &ps);

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    scene.stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        scene.stats.create_root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        scene.stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(6u * sizeof(ColorVertex));
        scene.stats.create_vertex_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            ColorVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                const float draw_x0 = static_cast<float>(kCorpusShaderStampX) - 2.0f;
                const float draw_y0 = static_cast<float>(kCorpusShaderStampY) - 2.0f;
                const float draw_x1 = static_cast<float>(kCorpusShaderStampX + kFreshTextureWidth) + 2.0f;
                const float draw_y1 = static_cast<float>(kCorpusShaderStampY + kFreshTextureHeight) + 2.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                const float r = static_cast<float>(scene.stats.expected_rgba[0]) / 255.0f;
                const float g = static_cast<float>(scene.stats.expected_rgba[1]) / 255.0f;
                const float b = static_cast<float>(scene.stats.expected_rgba[2]) / 255.0f;
                const float a = static_cast<float>(scene.stats.expected_rgba[3]) / 255.0f;
                write_quad(mapped, x0, y0, x1, y1, 0.04f, r, g, b, a);
                D3D12_RANGE written = {0, 6u * sizeof(ColorVertex)};
                scene.vertex_buffer->Unmap(0, &written);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = 6u * sizeof(ColorVertex);
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
            scene.stats.vertices_per_draw = 6;
        }
    }

    scene.stats.pass = scene.stats.d3dcompiler_loaded && scene.stats.vs_loaded && scene.stats.ps_loaded &&
                       corpus.position_color_vs_fnv1a64 == scene.stats.vs_fnv1a64 &&
                       corpus.position_color_ps_fnv1a64 == scene.stats.ps_fnv1a64 && scene.stats.vs_bytes > 0 &&
                       scene.stats.ps_bytes > 0 && SUCCEEDED(scene.stats.compile_vs_hr) &&
                       SUCCEEDED(scene.stats.compile_ps_hr) && SUCCEEDED(scene.stats.serialize_root_hr) &&
                       SUCCEEDED(scene.stats.create_root_hr) && SUCCEEDED(scene.stats.create_pso_hr) &&
                       SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.vertex_buffer != nullptr &&
                       scene.stats.vertices_per_draw == 6;
    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_corpus_shader_scene(CorpusShaderSceneResources& scene) {
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct SrvSampleStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_texture_hr = E_FAIL;
    HRESULT create_upload_hr = E_FAIL;
    HRESULT create_descriptor_heap_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    uint32_t srv_descriptors_created = 0;
    uint32_t copy_texture_region_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t draw_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {16, 144, 224, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool upload_filled = false;
    bool fence_wait_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct SrvSampleSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    SrvSampleStats stats;
};

static void srv_sample_expected_rgba(uint8_t out[4]) {
    out[0] = 16;
    out[1] = 144;
    out[2] = 224;
    out[3] = 255;
}

static void write_tex_quad(TexVertex* dst, float x0, float y0, float x1, float y1, float z) {
    const TexVertex quad[6] = {
        {{x0, y1, z}, {0.0f, 0.0f}}, {{x1, y1, z}, {1.0f, 0.0f}}, {{x0, y0, z}, {0.0f, 1.0f}},
        {{x0, y0, z}, {0.0f, 1.0f}}, {{x1, y1, z}, {1.0f, 0.0f}}, {{x1, y0, z}, {1.0f, 1.0f}},
    };
    std::memcpy(dst, quad, sizeof(quad));
}

static SrvSampleSceneResources create_srv_sample_scene(ID3D12Device* device, D3DCompileFn compile,
                                                       SerializeRootSignatureFn serialize, ID3D12CommandQueue* queue,
                                                       ID3D12CommandAllocator* allocator,
                                                       ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                                                       HANDLE fence_event, UINT64& fence_value, UINT backbuffer_width,
                                                       UINT backbuffer_height) {
    SrvSampleSceneResources scene;
    SrvSampleStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    srv_sample_expected_rgba(stats.expected_rgba);

    static const char* kSrvSampleHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D gTexture : register(t0);
SamplerState gSampler : register(s0);
PSIn vs_main(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.uv = input.uv;
    return output;
}
float4 ps_main(PSIn input) : SV_Target {
    return gTexture.Sample(gSampler, input.uv);
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kSrvSampleHlsl, "vs_main", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kSrvSampleHlsl, "ps_main", "ps_5_0", &ps);

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_param.DescriptorTable.NumDescriptorRanges = 1;
    root_param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_param;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(12u * sizeof(TexVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            TexVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                const float draw_x0 = static_cast<float>(kSrvSampleStampX) - 1.0f;
                const float draw_y0 = static_cast<float>(kSrvSampleStampY) - 1.0f;
                const float draw_x1 = static_cast<float>(kSrvSampleStampX + kFreshTextureWidth) + 1.0f;
                const float draw_y1 = static_cast<float>(kSrvSampleStampY + kFreshTextureHeight) + 1.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                write_tex_quad(mapped, x0, y0, x1, y1, 0.035f);
                write_tex_quad(mapped + 6, x0, y0, x1, y1, 0.035f);
                D3D12_RANGE written = {0, 12u * sizeof(TexVertex)};
                scene.vertex_buffer->Unmap(0, &written);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = 12u * sizeof(TexVertex);
            scene.vertex_view.StrideInBytes = sizeof(TexVertex);
            stats.vertices_per_draw = 12;
        }
    }

    if (device) {
        D3D12_RESOURCE_DESC tex_desc =
            texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        stats.create_texture_hr =
            device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&scene.texture));
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 upload_bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &upload_bytes);
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_bytes);
        stats.create_upload_hr =
            device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&scene.upload));
        if (SUCCEEDED(stats.create_upload_hr) && scene.upload) {
            TexturePayload payload;
            for (size_t i = 0; i < payload.rgba.size(); i += 4) {
                payload.rgba[i + 0] = stats.expected_rgba[0];
                payload.rgba[i + 1] = stats.expected_rgba[1];
                payload.rgba[i + 2] = stats.expected_rgba[2];
                payload.rgba[i + 3] = stats.expected_rgba[3];
            }
            stats.upload_filled =
                fill_texture_upload(scene.upload, footprint, payload, kFreshTextureWidth, kFreshTextureHeight);
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&scene.srv_heap));
        if (SUCCEEDED(stats.create_descriptor_heap_hr) && scene.srv_heap && scene.texture) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(scene.texture, &srv_desc,
                                             scene.srv_heap->GetCPUDescriptorHandleForHeapStart());
            stats.srv_descriptors_created = 1;
        }

        if (scene.texture && scene.upload && stats.upload_filled && allocator && list && queue && fence &&
            fence_event && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr))) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = scene.texture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = scene.upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprint;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            stats.copy_texture_region_commands = 1;
            D3D12_RESOURCE_BARRIER barrier = transition_barrier(scene.texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &barrier);
            stats.transition_barriers = 1;
            stats.close_hr = list->Close();
            if (SUCCEEDED(stats.close_hr)) {
                ID3D12CommandList* base = list;
                queue->ExecuteCommandLists(1, &base);
                fence_value++;
                stats.signal_hr = queue->Signal(fence, fence_value);
                stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
            }
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_texture_hr) && SUCCEEDED(stats.create_upload_hr) &&
                 SUCCEEDED(stats.create_descriptor_heap_hr) && stats.srv_descriptors_created == 1 &&
                 stats.upload_filled && stats.copy_texture_region_commands == 1 && stats.transition_barriers == 1 &&
                 SUCCEEDED(stats.close_hr) && SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok &&
                 scene.root_signature && scene.pipeline_state && scene.vertex_buffer && scene.texture &&
                 scene.srv_heap && stats.vertices_per_draw == 12;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_srv_sample_scene(SrvSampleSceneResources& scene) {
    safe_release(scene.srv_heap);
    safe_release(scene.upload);
    safe_release(scene.texture);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct Textured3DFaceStats {
    std::string family;
    std::string label;
    std::string extension;
    std::string destination;
    std::string source_path;
    std::string sha256;
    uint64_t declared_size = 0;
    uint64_t file_fnv1a64 = 0;
    uint64_t upload_fnv1a64 = 1469598103934665603ull;
    uint32_t bytes_from_file = 0;
    UINT sample_x = 0;
    UINT sample_y = 0;
    uint8_t expected_rgba[4] = {0, 0, 0, 0};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint32_t samples_checked = 0;
    uint32_t sample_matches = 0;
};

struct Textured3DStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_depth_texture_hr = E_FAIL;
    HRESULT create_dsv_heap_hr = E_FAIL;
    std::array<HRESULT, kTextured3DFaceCount> create_texture_hr = {E_FAIL, E_FAIL, E_FAIL};
    std::array<HRESULT, kTextured3DFaceCount> create_upload_hr = {E_FAIL, E_FAIL, E_FAIL};
    HRESULT create_descriptor_heap_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    uint32_t face_count = 0;
    uint32_t unique_families = 0;
    uint32_t textures_created = 0;
    uint32_t upload_buffers_created = 0;
    uint32_t uploads_filled = 0;
    uint32_t srv_descriptors_created = 0;
    uint32_t dsv_descriptors_created = 0;
    uint32_t clear_depth_commands = 0;
    uint32_t copy_texture_region_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t vertices_created = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t vertex_buffer_updates = 0;
    uint32_t root_constant_sets = 0;
    uint32_t draw_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t front_face = 0;
    UINT depth_overlap_sample_x = kTextured3DDepthSampleX;
    UINT depth_overlap_sample_y = kTextured3DDepthSampleY;
    uint8_t depth_overlap_expected_rgba[4] = {48, 224, 96, 255};
    uint8_t depth_overlap_present_rgba[4] = {0, 0, 0, 0};
    uint32_t depth_overlap_samples_checked = 0;
    uint32_t depth_overlap_sample_matches = 0;
    bool fence_wait_ok = false;
    bool present_pass = false;
    bool pass = false;
    std::array<Textured3DFaceStats, kTextured3DFaceCount> faces;
};

struct Textured3DSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* depth_texture = nullptr;
    std::array<ID3D12Resource*, kTextured3DFaceCount> textures = {nullptr, nullptr, nullptr};
    std::array<ID3D12Resource*, kTextured3DFaceCount> uploads = {nullptr, nullptr, nullptr};
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    Textured3DStats stats;
};

static void textured3d_expected_rgba(UINT face, uint8_t out[4]) {
    static const uint8_t kColors[kTextured3DFaceCount][4] = {
        {232, 48, 56, 255},
        {48, 224, 96, 255},
        {64, 120, 240, 255},
    };
    const UINT index = face < kTextured3DFaceCount ? face : 0;
    std::memcpy(out, kColors[index], 4);
}

static const TexturePayload* find_texture_family(const std::vector<TexturePayload>& payloads, const char* family) {
    for (const TexturePayload& payload : payloads) {
        if (payload.family == family)
            return &payload;
    }
    return nullptr;
}

static TexturePayload make_textured3d_upload_payload(const TexturePayload& source, UINT face, uint64_t& upload_hash) {
    TexturePayload upload = source;
    uint8_t base[4] = {};
    textured3d_expected_rgba(face, base);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const size_t pixel = (static_cast<size_t>(y) * kFreshTextureWidth + x) * 4;
            const bool center_proof_patch = x >= 3 && x <= 12 && y >= 3 && y <= 13;
            if (center_proof_patch) {
                upload.rgba[pixel + 0] = base[0];
                upload.rgba[pixel + 1] = base[1];
                upload.rgba[pixel + 2] = base[2];
                upload.rgba[pixel + 3] = base[3];
                continue;
            }
            const uint8_t source_byte = source.rgba[(pixel + face * 37u) % source.rgba.size()];
            const uint8_t accent = static_cast<uint8_t>(source_byte & 31u);
            upload.rgba[pixel + 0] = static_cast<uint8_t>(std::min<int>(255, base[0] / 2 + accent));
            upload.rgba[pixel + 1] = static_cast<uint8_t>(std::min<int>(255, base[1] / 2 + accent));
            upload.rgba[pixel + 2] = static_cast<uint8_t>(std::min<int>(255, base[2] / 2 + accent));
            upload.rgba[pixel + 3] = 255;
        }
    }
    upload_hash = fnv1a_update(1469598103934665603ull, upload.rgba.data(), upload.rgba.size());
    return upload;
}

struct ProjectedPoint {
    float ndc_x = 0.0f;
    float ndc_y = 0.0f;
    float view_z = 0.0f;
};

static ProjectedPoint project_textured3d_prism_point(float local_x, float local_y, float local_z, float angle) {
    const float c = static_cast<float>(std::cos(angle));
    const float s = static_cast<float>(std::sin(angle));
    const float x = local_x * c + local_z * s;
    const float z = -local_x * s + local_z * c;
    const float perspective = 1.0f / (1.95f - z * 0.55f);
    ProjectedPoint out;
    out.ndc_x = x * perspective;
    out.ndc_y = 0.42f + local_y * perspective;
    out.view_z = z;
    return out;
}

static UINT clamp_pixel_from_ndc_x(float ndc_x, UINT width) {
    const float px = (ndc_x + 1.0f) * 0.5f * static_cast<float>(width);
    return static_cast<UINT>(std::max<float>(0.0f, std::min<float>(static_cast<float>(width - 1u), px + 0.5f)));
}

static UINT clamp_pixel_from_ndc_y(float ndc_y, UINT height) {
    const float py = (1.0f - ndc_y) * 0.5f * static_cast<float>(height);
    return static_cast<UINT>(std::max<float>(0.0f, std::min<float>(static_cast<float>(height - 1u), py + 0.5f)));
}

static float depth_from_view_z(float view_z) {
    return std::max<float>(0.05f, std::min<float>(0.95f, 0.48f - view_z * 0.34f));
}

static void write_textured3d_triangle(Tex3DVertex* dst, UINT face, const ProjectedPoint& apex,
                                      const ProjectedPoint& base_a, const ProjectedPoint& base_b) {
    const float f = static_cast<float>(face);
    const Tex3DVertex tri[3] = {
        {{apex.ndc_x, apex.ndc_y, depth_from_view_z(apex.view_z)}, {0.5f, 0.0f, f, 0.0f}},
        {{base_a.ndc_x, base_a.ndc_y, depth_from_view_z(base_a.view_z)}, {0.0f, 1.0f, f, 0.0f}},
        {{base_b.ndc_x, base_b.ndc_y, depth_from_view_z(base_b.view_z)}, {1.0f, 1.0f, f, 0.0f}},
    };
    std::memcpy(dst, tri, sizeof(tri));
}

static void write_textured3d_depth_overlap(Tex3DVertex* dst) {
    const float cx =
        ndc_x_from_pixel(static_cast<float>(kTextured3DDepthSampleX), static_cast<float>(kBackbufferWidth));
    const float cy =
        ndc_y_from_pixel(static_cast<float>(kTextured3DDepthSampleY), static_cast<float>(kBackbufferHeight));
    constexpr float sx = 0.060f;
    constexpr float sy = 0.085f;
    const Tex3DVertex front_unity[3] = {
        {{cx, cy + sy, 0.15f}, {0.5f, 0.0f, 1.0f, 0.0f}},
        {{cx - sx, cy - sy, 0.15f}, {0.0f, 1.0f, 1.0f, 0.0f}},
        {{cx + sx, cy - sy, 0.15f}, {1.0f, 1.0f, 1.0f, 0.0f}},
    };
    const Tex3DVertex back_unreal[3] = {
        {{cx, cy + sy, 0.85f}, {0.5f, 0.0f, 0.0f, 0.0f}},
        {{cx - sx, cy - sy, 0.85f}, {0.0f, 1.0f, 0.0f, 0.0f}},
        {{cx + sx, cy - sy, 0.85f}, {1.0f, 1.0f, 0.0f, 0.0f}},
    };
    std::memcpy(dst, front_unity, sizeof(front_unity));
    std::memcpy(dst + 3, back_unreal, sizeof(back_unreal));
}

static bool populate_textured3d_prism_vertices(Textured3DSceneResources& scene, Textured3DStats& stats, uint32_t frame,
                                               UINT backbuffer_width, UINT backbuffer_height) {
    if (!scene.vertex_buffer)
        return false;
    Tex3DVertex* mapped = nullptr;
    if (FAILED(scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;

    constexpr float kRadius = 0.52f;
    constexpr float kApexY = 0.48f;
    constexpr float kBaseY = -0.27f;
    const float angle = static_cast<float>(frame) * 0.20943951f;
    const float vertex_angle[3] = {1.570796327f, 3.665191429f, 5.759586532f};
    const ProjectedPoint apex = project_textured3d_prism_point(0.0f, kApexY, 0.0f, angle);
    ProjectedPoint base[3] = {};
    for (UINT i = 0; i < 3; ++i) {
        const float x = kRadius * static_cast<float>(std::cos(vertex_angle[i]));
        const float z = kRadius * static_cast<float>(std::sin(vertex_angle[i]));
        base[i] = project_textured3d_prism_point(x, kBaseY, z, angle);
    }

    static const UINT kFaceA[kTextured3DFaceCount] = {0, 1, 2};
    static const UINT kFaceB[kTextured3DFaceCount] = {1, 2, 0};
    std::array<float, kTextured3DFaceCount> face_depth = {};
    for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
        const UINT a = kFaceA[face];
        const UINT b = kFaceB[face];
        face_depth[face] = (apex.view_z + base[a].view_z + base[b].view_z) / 3.0f;
        const float centroid_x = (apex.ndc_x + base[a].ndc_x + base[b].ndc_x) / 3.0f;
        const float centroid_y = (apex.ndc_y + base[a].ndc_y + base[b].ndc_y) / 3.0f;
        stats.faces[face].sample_x = clamp_pixel_from_ndc_x(centroid_x, backbuffer_width);
        stats.faces[face].sample_y = clamp_pixel_from_ndc_y(centroid_y, backbuffer_height);
    }
    stats.front_face = 0;
    for (UINT face = 1; face < kTextured3DFaceCount; ++face) {
        if (face_depth[face] > face_depth[stats.front_face])
            stats.front_face = face;
    }

    UINT cursor = 0;
    for (UINT repeat = 0; repeat < 3; ++repeat) {
        for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
            const UINT a = kFaceA[face];
            const UINT b = kFaceB[face];
            write_textured3d_triangle(mapped + cursor, face, apex, base[a], base[b]);
            cursor += 3;
        }
    }

    write_textured3d_depth_overlap(mapped + cursor);
    cursor += 6;

    D3D12_RANGE written = {0, kTextured3DVertexCount * sizeof(Tex3DVertex)};
    scene.vertex_buffer->Unmap(0, &written);
    stats.vertex_buffer_updates++;
    return cursor == kTextured3DVertexCount;
}

static Textured3DSceneResources
create_textured3d_scene(ID3D12Device* device, D3DCompileFn compile, SerializeRootSignatureFn serialize,
                        ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
                        ID3D12Fence* fence, HANDLE fence_event, UINT64& fence_value, const CorpusStats& corpus,
                        UINT backbuffer_width, UINT backbuffer_height) {
    Textured3DSceneResources scene;
    Textured3DStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;

    static const char* kTextured3DHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float4 texface : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float face : TEXCOORD1; };
Texture2D gUnrealTexture : register(t0);
Texture2D gUnityTexture : register(t1);
Texture2D gMicrosoftTexture : register(t2);
SamplerState gSampler : register(s0);
PSIn vs_main(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.uv = input.texface.xy;
    output.face = input.texface.z;
    return output;
}
float4 ps_main(PSIn input) : SV_Target {
    if (input.face < 0.5f)
        return gUnrealTexture.Sample(gSampler, input.uv);
    if (input.face < 1.5f)
        return gUnityTexture.Sample(gSampler, input.uv);
    return gMicrosoftTexture.Sample(gSampler, input.uv);
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kTextured3DHlsl, "vs_main", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kTextured3DHlsl, "ps_main", "ps_5_0", &ps);

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTextured3DFaceCount;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 1> root_params = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    root_params[0].DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = static_cast<UINT>(root_params.size());
    root_desc.pParameters = root_params.data();
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = TRUE;
        pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    const char* required_families[kTextured3DFaceCount] = {"unreal", "unity-sdk", "microsoft-sdk"};
    for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
        const TexturePayload* selected = find_texture_family(corpus.texture_payloads, required_families[face]);
        if (!selected)
            continue;
        Textured3DFaceStats& face_stats = stats.faces[face];
        face_stats.family = selected->family;
        face_stats.label = selected->label;
        face_stats.extension = selected->extension;
        face_stats.destination = selected->destination;
        face_stats.source_path = selected->source_path;
        face_stats.sha256 = selected->sha256;
        face_stats.declared_size = selected->declared_size;
        face_stats.file_fnv1a64 = selected->fnv1a64;
        face_stats.bytes_from_file = selected->bytes_from_file;
        face_stats.sample_x = 0;
        face_stats.sample_y = 0;
        textured3d_expected_rgba(face, face_stats.expected_rgba);
        stats.face_count++;
    }
    for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
        bool first = !stats.faces[face].family.empty();
        for (UINT prior = 0; prior < face && first; ++prior)
            first = stats.faces[face].family != stats.faces[prior].family;
        if (first)
            stats.unique_families++;
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(kTextured3DVertexCount * sizeof(Tex3DVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            Tex3DVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                std::memset(mapped, 0, kTextured3DVertexCount * sizeof(Tex3DVertex));
                D3D12_RANGE written = {0, kTextured3DVertexCount * sizeof(Tex3DVertex)};
                scene.vertex_buffer->Unmap(0, &written);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = kTextured3DVertexCount * sizeof(Tex3DVertex);
            scene.vertex_view.StrideInBytes = sizeof(Tex3DVertex);
            stats.vertices_created = kTextured3DVertexCount;
            stats.vertices_per_draw = kTextured3DVertexCount;
        }
    }

    if (device) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = kTextured3DFaceCount;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&scene.srv_heap));
        const UINT descriptor_increment =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu =
            scene.srv_heap ? scene.srv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_RESOURCE_DESC tex_desc =
            texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
            if (stats.faces[face].family.empty())
                continue;
            stats.create_texture_hr[face] = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&scene.textures[face]));
            if (SUCCEEDED(stats.create_texture_hr[face]))
                stats.textures_created++;
            UINT rows = 0;
            UINT64 row_bytes = 0;
            UINT64 upload_bytes = 0;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &upload_bytes);
            D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_bytes);
            stats.create_upload_hr[face] = device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&scene.uploads[face]));
            if (SUCCEEDED(stats.create_upload_hr[face]))
                stats.upload_buffers_created++;
            uint64_t upload_hash = 0;
            TexturePayload source = corpus.texture_payloads.empty() ? TexturePayload{} : corpus.texture_payloads[0];
            for (const TexturePayload& payload : corpus.texture_payloads) {
                if (payload.destination == stats.faces[face].destination) {
                    source = payload;
                    break;
                }
            }
            TexturePayload upload_payload = make_textured3d_upload_payload(source, face, upload_hash);
            stats.faces[face].upload_fnv1a64 = upload_hash;
            if (scene.uploads[face] && fill_texture_upload(scene.uploads[face], footprint, upload_payload,
                                                           kFreshTextureWidth, kFreshTextureHeight)) {
                stats.uploads_filled++;
            }
            if (scene.srv_heap && scene.textures[face]) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv_desc.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(scene.textures[face], &srv_desc,
                                                 offset_cpu(srv_cpu, descriptor_increment, face));
                stats.srv_descriptors_created++;
            }
        }

        if (allocator && list && queue && fence && fence_event && stats.textures_created == kTextured3DFaceCount &&
            stats.uploads_filled == kTextured3DFaceCount && SUCCEEDED(allocator->Reset()) &&
            SUCCEEDED(list->Reset(allocator, nullptr))) {
            for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
                UINT rows = 0;
                UINT64 row_bytes = 0;
                UINT64 upload_bytes = 0;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
                device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprint, &rows, &row_bytes, &upload_bytes);
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = scene.textures[face];
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = scene.uploads[face];
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprint;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                stats.copy_texture_region_commands++;
                D3D12_RESOURCE_BARRIER barrier = transition_barrier(
                    scene.textures[face], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &barrier);
                stats.transition_barriers++;
            }
            stats.close_hr = list->Close();
            if (SUCCEEDED(stats.close_hr)) {
                ID3D12CommandList* base = list;
                queue->ExecuteCommandLists(1, &base);
                fence_value++;
                stats.signal_hr = queue->Signal(fence, fence_value);
                stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
            }
        }
    }

    if (device) {
        D3D12_RESOURCE_DESC depth_desc = {};
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.Alignment = 0;
        depth_desc.Width = backbuffer_width;
        depth_desc.Height = backbuffer_height;
        depth_desc.DepthOrArraySize = 1;
        depth_desc.MipLevels = 1;
        depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.SampleDesc.Quality = 0;
        depth_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear_value = {};
        clear_value.Format = DXGI_FORMAT_D32_FLOAT;
        clear_value.DepthStencil.Depth = 1.0f;
        clear_value.DepthStencil.Stencil = 0;
        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        stats.create_depth_texture_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
            IID_PPV_ARGS(&scene.depth_texture));
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_heap_desc.NumDescriptors = 1;
        stats.create_dsv_heap_hr = device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&scene.dsv_heap));
        if (scene.depth_texture && scene.dsv_heap) {
            scene.dsv_handle = scene.dsv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
            dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(scene.depth_texture, &dsv_desc, scene.dsv_handle);
            stats.dsv_descriptors_created = 1;
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_depth_texture_hr) && SUCCEEDED(stats.create_dsv_heap_hr) &&
                 stats.dsv_descriptors_created == 1 && stats.face_count == kTextured3DFaceCount &&
                 stats.unique_families == kTextured3DFaceCount && stats.textures_created == kTextured3DFaceCount &&
                 stats.upload_buffers_created == kTextured3DFaceCount && stats.uploads_filled == kTextured3DFaceCount &&
                 stats.srv_descriptors_created == kTextured3DFaceCount &&
                 stats.copy_texture_region_commands == kTextured3DFaceCount &&
                 stats.transition_barriers == kTextured3DFaceCount && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && scene.root_signature && scene.pipeline_state &&
                 scene.vertex_buffer && scene.srv_heap && stats.vertices_per_draw == kTextured3DVertexCount;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_textured3d_scene(Textured3DSceneResources& scene) {
    safe_release(scene.dsv_heap);
    safe_release(scene.depth_texture);
    safe_release(scene.srv_heap);
    for (ID3D12Resource*& upload : scene.uploads)
        safe_release(upload);
    for (ID3D12Resource*& texture : scene.textures)
        safe_release(texture);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct CbvSampleStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_constant_buffer_hr = E_FAIL;
    HRESULT create_descriptor_heap_hr = E_FAIL;
    uint32_t cbv_descriptors_created = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t draw_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {208, 48, 160, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool constant_buffer_filled = false;
    bool present_pass = false;
    bool pass = false;
};

struct CbvSampleSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* constant_buffer = nullptr;
    ID3D12DescriptorHeap* cbv_heap = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    CbvSampleStats stats;
};

static void cbv_sample_expected_rgba(uint8_t out[4]) {
    out[0] = 208;
    out[1] = 48;
    out[2] = 160;
    out[3] = 255;
}

static CbvSampleSceneResources create_cbv_sample_scene(ID3D12Device* device, D3DCompileFn compile,
                                                       SerializeRootSignatureFn serialize, UINT backbuffer_width,
                                                       UINT backbuffer_height) {
    CbvSampleSceneResources scene;
    CbvSampleStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    cbv_sample_expected_rgba(stats.expected_rgba);

    static const char* kCbvSampleHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
cbuffer ColorConstants : register(b0) { float4 gColor; };
PSIn vs_main(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.uv = input.uv;
    return output;
}
float4 ps_main(PSIn input) : SV_Target {
    return gColor;
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kCbvSampleHlsl, "vs_main", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kCbvSampleHlsl, "ps_main", "ps_5_0", &ps);

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_param.DescriptorTable.NumDescriptorRanges = 1;
    root_param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_param;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(18u * sizeof(TexVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            TexVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                const float draw_x0 = static_cast<float>(kCbvStampX) - 1.0f;
                const float draw_y0 = static_cast<float>(kCbvStampY) - 1.0f;
                const float draw_x1 = static_cast<float>(kCbvStampX + kFreshTextureWidth) + 1.0f;
                const float draw_y1 = static_cast<float>(kCbvStampY + kFreshTextureHeight) + 1.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                write_tex_quad(mapped, x0, y0, x1, y1, 0.03f);
                write_tex_quad(mapped + 6, x0, y0, x1, y1, 0.03f);
                write_tex_quad(mapped + 12, x0, y0, x1, y1, 0.03f);
                D3D12_RANGE written = {0, 18u * sizeof(TexVertex)};
                scene.vertex_buffer->Unmap(0, &written);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = 18u * sizeof(TexVertex);
            scene.vertex_view.StrideInBytes = sizeof(TexVertex);
            stats.vertices_per_draw = 18;
        }

        D3D12_RESOURCE_DESC cb_desc = buffer_desc(256u);
        stats.create_constant_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
                                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                          IID_PPV_ARGS(&scene.constant_buffer));
        if (SUCCEEDED(stats.create_constant_buffer_hr) && scene.constant_buffer) {
            float* mapped = nullptr;
            if (SUCCEEDED(scene.constant_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                mapped[0] = static_cast<float>(stats.expected_rgba[0]) / 255.0f;
                mapped[1] = static_cast<float>(stats.expected_rgba[1]) / 255.0f;
                mapped[2] = static_cast<float>(stats.expected_rgba[2]) / 255.0f;
                mapped[3] = static_cast<float>(stats.expected_rgba[3]) / 255.0f;
                D3D12_RANGE written = {0, 4u * sizeof(float)};
                scene.constant_buffer->Unmap(0, &written);
                stats.constant_buffer_filled = true;
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&scene.cbv_heap));
        if (SUCCEEDED(stats.create_descriptor_heap_hr) && scene.cbv_heap && scene.constant_buffer) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc = {};
            cbv_desc.BufferLocation = scene.constant_buffer->GetGPUVirtualAddress();
            cbv_desc.SizeInBytes = 256u;
            device->CreateConstantBufferView(&cbv_desc, scene.cbv_heap->GetCPUDescriptorHandleForHeapStart());
            stats.cbv_descriptors_created = 1;
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_constant_buffer_hr) && SUCCEEDED(stats.create_descriptor_heap_hr) &&
                 stats.constant_buffer_filled && stats.cbv_descriptors_created == 1 && scene.root_signature &&
                 scene.pipeline_state && scene.vertex_buffer && scene.constant_buffer && scene.cbv_heap &&
                 stats.vertices_per_draw == 18;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_cbv_sample_scene(CbvSampleSceneResources& scene) {
    safe_release(scene.cbv_heap);
    safe_release(scene.constant_buffer);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct IndexedDrawStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_dynamic_stride_vertex_buffer_hr = E_FAIL;
    HRESULT create_index_buffer_hr = E_FAIL;
    HRESULT create_index_buffer_r32_hr = E_FAIL;
    HRESULT create_index_buffer_negative_base_hr = E_FAIL;
    HRESULT create_index_buffer_execute_indirect_hr = E_FAIL;
    HRESULT create_execute_indirect_indexed_argument_buffer_hr = E_FAIL;
    HRESULT create_execute_indirect_indexed_command_signature_hr = E_FAIL;
    bool append_aligned_element = true;
    uint32_t append_aligned_color_expected_offset = 12;
    uint32_t vertices_created = 0;
    uint32_t vertex_buffer_size = 0;
    uint32_t vertex_view_byte_offset = 0;
    uint32_t dynamic_stride_vertices_created = 0;
    uint32_t dynamic_stride_vertex_buffer_size = 0;
    uint32_t dynamic_stride = 32;
    uint32_t indices_created = 0;
    uint32_t index_format = DXGI_FORMAT_R16_UINT;
    uint32_t index_buffer_size = 0;
    uint32_t index_view_byte_offset = 0;
    uint32_t start_index_location = 2;
    uint32_t r32_indices_created = 0;
    uint32_t r32_index_format = DXGI_FORMAT_R32_UINT;
    uint32_t r32_index_buffer_size = 0;
    uint32_t r32_index_view_byte_offset = 0;
    uint32_t r32_start_index_location = 2;
    INT r32_base_vertex_location = 4;
    uint32_t negative_base_indices_created = 0;
    uint32_t negative_base_index_format = DXGI_FORMAT_R32_UINT;
    uint32_t negative_base_index_buffer_size = 0;
    uint32_t negative_base_start_index_location = 0;
    INT negative_base_vertex_location = -4;
    uint32_t execute_indirect_indexed_indices_created = 0;
    uint32_t execute_indirect_indexed_index_format = DXGI_FORMAT_R32_UINT;
    uint32_t execute_indirect_indexed_index_buffer_size = 0;
    uint32_t execute_indirect_indexed_argument_byte_stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    uint32_t execute_indirect_indexed_command_signature_arguments = 1;
    uint32_t execute_indirect_indexed_max_command_count = 1;
    uint32_t execute_indirect_indexed_index_count = 6;
    uint32_t execute_indirect_indexed_instance_count = 1;
    uint32_t execute_indirect_indexed_start_index_location = 2;
    INT execute_indirect_indexed_base_vertex_location = -6;
    uint32_t execute_indirect_indexed_start_instance_location = 0;
    uint32_t draw_indexed_calls = 0;
    uint32_t draw_indexed_r32_calls = 0;
    uint32_t draw_indexed_negative_base_calls = 0;
    uint32_t draw_indexed_dynamic_stride_calls = 0;
    uint32_t execute_indirect_indexed_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {240, 200, 48, 255};
    uint8_t expected_r32_rgba[4] = {64, 220, 240, 255};
    uint8_t expected_negative_base_rgba[4] = {200, 80, 240, 255};
    uint8_t expected_dynamic_stride_rgba[4] = {96, 240, 120, 255};
    uint8_t expected_execute_indirect_indexed_rgba[4] = {240, 144, 72, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_r32_rgba[4] = {0, 0, 0, 0};
    uint8_t present_r32_last_rgba[4] = {0, 0, 0, 0};
    uint32_t present_r32_samples_checked = 0;
    uint32_t present_r32_sample_matches = 0;
    uint32_t present_r32_pixels_checked = 0;
    uint32_t present_r32_pixel_matches = 0;
    uint8_t present_negative_base_rgba[4] = {0, 0, 0, 0};
    uint8_t present_negative_base_last_rgba[4] = {0, 0, 0, 0};
    uint32_t present_negative_base_samples_checked = 0;
    uint32_t present_negative_base_sample_matches = 0;
    uint32_t present_negative_base_pixels_checked = 0;
    uint32_t present_negative_base_pixel_matches = 0;
    uint8_t present_dynamic_stride_rgba[4] = {0, 0, 0, 0};
    uint8_t present_dynamic_stride_last_rgba[4] = {0, 0, 0, 0};
    uint32_t present_dynamic_stride_samples_checked = 0;
    uint32_t present_dynamic_stride_sample_matches = 0;
    uint32_t present_dynamic_stride_pixels_checked = 0;
    uint32_t present_dynamic_stride_pixel_matches = 0;
    uint8_t present_execute_indirect_indexed_rgba[4] = {0, 0, 0, 0};
    uint8_t present_execute_indirect_indexed_last_rgba[4] = {0, 0, 0, 0};
    uint32_t present_execute_indirect_indexed_samples_checked = 0;
    uint32_t present_execute_indirect_indexed_sample_matches = 0;
    uint32_t present_execute_indirect_indexed_pixels_checked = 0;
    uint32_t present_execute_indirect_indexed_pixel_matches = 0;
    bool present_pass = false;
    bool pass = false;
};

struct IndexedDrawSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* vertex_buffer_dynamic_stride = nullptr;
    ID3D12Resource* index_buffer = nullptr;
    ID3D12Resource* index_buffer_r32 = nullptr;
    ID3D12Resource* index_buffer_negative_base = nullptr;
    ID3D12Resource* index_buffer_execute_indirect = nullptr;
    ID3D12Resource* execute_indirect_indexed_argument_buffer = nullptr;
    ID3D12CommandSignature* execute_indirect_indexed_command_signature = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    D3D12_VERTEX_BUFFER_VIEW vertex_view_dynamic_stride = {};
    D3D12_VERTEX_BUFFER_VIEW vertex_view_execute_indirect = {};
    D3D12_INDEX_BUFFER_VIEW index_view = {};
    D3D12_INDEX_BUFFER_VIEW index_view_r32 = {};
    D3D12_INDEX_BUFFER_VIEW index_view_negative_base = {};
    D3D12_INDEX_BUFFER_VIEW index_view_execute_indirect = {};
    IndexedDrawStats stats;
};

static void indexed_draw_expected_rgba(uint8_t out[4]) {
    out[0] = 240;
    out[1] = 200;
    out[2] = 48;
    out[3] = 255;
}

static IndexedDrawSceneResources create_indexed_draw_scene(ID3D12Device* device, D3DCompileFn compile,
                                                           SerializeRootSignatureFn serialize, UINT backbuffer_width,
                                                           UINT backbuffer_height) {
    IndexedDrawSceneResources scene;
    IndexedDrawStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    indexed_draw_expected_rgba(stats.expected_rgba);

    static const char* kIndexedHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float4 color : COLOR0; };
struct PSIn { float4 pos : SV_Position; float4 color : COLOR0; };
PSIn indexed_vs(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
float4 indexed_ps(PSIn input) : SV_Target {
    return saturate(input.color * float4(1.0f, 1.0f, 1.0f, 1.0f));
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kIndexedHlsl, "indexed_vs", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kIndexedHlsl, "indexed_ps", "ps_5_0", &ps);

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(17u * sizeof(ColorVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            ColorVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                auto fill_quad = [&](ColorVertex* out, UINT stamp_x, UINT stamp_y, const uint8_t rgba[4]) {
                    const float draw_x0 = static_cast<float>(stamp_x) - 1.0f;
                    const float draw_y0 = static_cast<float>(stamp_y) - 1.0f;
                    const float draw_x1 = static_cast<float>(stamp_x + kFreshTextureWidth) + 1.0f;
                    const float draw_y1 = static_cast<float>(stamp_y + kFreshTextureHeight) + 1.0f;
                    const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                    const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                    const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                    const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                    const float r = static_cast<float>(rgba[0]) / 255.0f;
                    const float g = static_cast<float>(rgba[1]) / 255.0f;
                    const float b = static_cast<float>(rgba[2]) / 255.0f;
                    const float a = static_cast<float>(rgba[3]) / 255.0f;
                    out[0] = {{x0, y1, 0.025f}, {r, g, b, a}};
                    out[1] = {{x1, y1, 0.025f}, {r, g, b, a}};
                    out[2] = {{x0, y0, 0.025f}, {r, g, b, a}};
                    out[3] = {{x1, y0, 0.025f}, {r, g, b, a}};
                };
                const ColorVertex poison = {{-1.0f, -1.0f, 0.025f}, {0.0f, 0.0f, 0.0f, 0.0f}};
                mapped[0] = poison;
                fill_quad(mapped + 1, kIndexedStampX, kIndexedStampY, stats.expected_rgba);
                fill_quad(mapped + 5, kIndexedR32StampX, kIndexedR32StampY, stats.expected_r32_rgba);
                fill_quad(mapped + 9, kIndexedNegativeBaseStampX, kIndexedNegativeBaseStampY,
                          stats.expected_negative_base_rgba);
                fill_quad(mapped + 13, kIndexedExecuteIndirectStampX, kIndexedExecuteIndirectStampY,
                          stats.expected_execute_indirect_indexed_rgba);
                D3D12_RANGE written = {0, 17u * sizeof(ColorVertex)};
                scene.vertex_buffer->Unmap(0, &written);
                stats.vertices_created = 12;
                stats.vertex_view_byte_offset = sizeof(ColorVertex);
                stats.vertex_buffer_size = 12u * sizeof(ColorVertex);
            }
            scene.vertex_view.BufferLocation =
                scene.vertex_buffer->GetGPUVirtualAddress() + stats.vertex_view_byte_offset;
            scene.vertex_view.SizeInBytes = stats.vertex_buffer_size;
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
            scene.vertex_view_execute_indirect.BufferLocation = scene.vertex_view.BufferLocation;
            scene.vertex_view_execute_indirect.SizeInBytes = 16u * sizeof(ColorVertex);
            scene.vertex_view_execute_indirect.StrideInBytes = sizeof(ColorVertex);
        }

        struct DynamicStrideVertex {
            float position[3];
            float color[4];
            float padding;
        };
        D3D12_RESOURCE_DESC dynamic_vb_desc = buffer_desc(4u * sizeof(DynamicStrideVertex));
        stats.create_dynamic_stride_vertex_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &dynamic_vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.vertex_buffer_dynamic_stride));
        if (SUCCEEDED(stats.create_dynamic_stride_vertex_buffer_hr) && scene.vertex_buffer_dynamic_stride) {
            DynamicStrideVertex* mapped = nullptr;
            if (SUCCEEDED(scene.vertex_buffer_dynamic_stride->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) &&
                mapped) {
                const float draw_x0 = static_cast<float>(kIndexedDynamicStrideStampX) - 1.0f;
                const float draw_y0 = static_cast<float>(kIndexedDynamicStrideStampY) - 1.0f;
                const float draw_x1 = static_cast<float>(kIndexedDynamicStrideStampX + kFreshTextureWidth) + 1.0f;
                const float draw_y1 = static_cast<float>(kIndexedDynamicStrideStampY + kFreshTextureHeight) + 1.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                const float r = static_cast<float>(stats.expected_dynamic_stride_rgba[0]) / 255.0f;
                const float g = static_cast<float>(stats.expected_dynamic_stride_rgba[1]) / 255.0f;
                const float b = static_cast<float>(stats.expected_dynamic_stride_rgba[2]) / 255.0f;
                const float a = static_cast<float>(stats.expected_dynamic_stride_rgba[3]) / 255.0f;
                mapped[0] = {{x0, y1, 0.025f}, {r, g, b, a}, 0.0f};
                mapped[1] = {{x1, y1, 0.025f}, {r, g, b, a}, 0.0f};
                mapped[2] = {{x0, y0, 0.025f}, {r, g, b, a}, 0.0f};
                mapped[3] = {{x1, y0, 0.025f}, {r, g, b, a}, 0.0f};
                D3D12_RANGE written = {0, 4u * sizeof(DynamicStrideVertex)};
                scene.vertex_buffer_dynamic_stride->Unmap(0, &written);
                stats.dynamic_stride_vertices_created = 4;
                stats.dynamic_stride_vertex_buffer_size = 4u * sizeof(DynamicStrideVertex);
                stats.dynamic_stride = sizeof(DynamicStrideVertex);
            }
            scene.vertex_view_dynamic_stride.BufferLocation =
                scene.vertex_buffer_dynamic_stride->GetGPUVirtualAddress();
            scene.vertex_view_dynamic_stride.SizeInBytes = stats.dynamic_stride_vertex_buffer_size;
            scene.vertex_view_dynamic_stride.StrideInBytes = stats.dynamic_stride;
        }

        D3D12_RESOURCE_DESC ib_desc = buffer_desc(10u * sizeof(uint16_t));
        stats.create_index_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                       IID_PPV_ARGS(&scene.index_buffer));
        if (SUCCEEDED(stats.create_index_buffer_hr) && scene.index_buffer) {
            uint16_t* mapped = nullptr;
            if (SUCCEEDED(scene.index_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                const uint16_t indices[10] = {65535, 65535, 65535, 65535, 0, 1, 2, 2, 1, 3};
                std::memcpy(mapped, indices, sizeof(indices));
                D3D12_RANGE written = {0, sizeof(indices)};
                scene.index_buffer->Unmap(0, &written);
                stats.indices_created = 6;
                stats.index_view_byte_offset = 2u * sizeof(uint16_t);
                stats.index_buffer_size = 8u * sizeof(uint16_t);
            }
            scene.index_view.BufferLocation = scene.index_buffer->GetGPUVirtualAddress() + stats.index_view_byte_offset;
            scene.index_view.SizeInBytes = stats.index_buffer_size;
            scene.index_view.Format = DXGI_FORMAT_R16_UINT;
            stats.index_format = DXGI_FORMAT_R16_UINT;
        }

        D3D12_RESOURCE_DESC ib_r32_desc = buffer_desc(10u * sizeof(uint32_t));
        stats.create_index_buffer_r32_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_r32_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.index_buffer_r32));
        if (SUCCEEDED(stats.create_index_buffer_r32_hr) && scene.index_buffer_r32) {
            uint32_t* mapped = nullptr;
            if (SUCCEEDED(scene.index_buffer_r32->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                const uint32_t indices[10] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0, 1, 2, 2, 1, 3};
                std::memcpy(mapped, indices, sizeof(indices));
                D3D12_RANGE written = {0, sizeof(indices)};
                scene.index_buffer_r32->Unmap(0, &written);
                stats.r32_indices_created = 6;
                stats.r32_index_view_byte_offset = 2u * sizeof(uint32_t);
                stats.r32_index_buffer_size = 8u * sizeof(uint32_t);
            }
            scene.index_view_r32.BufferLocation =
                scene.index_buffer_r32->GetGPUVirtualAddress() + stats.r32_index_view_byte_offset;
            scene.index_view_r32.SizeInBytes = stats.r32_index_buffer_size;
            scene.index_view_r32.Format = DXGI_FORMAT_R32_UINT;
            stats.r32_index_format = DXGI_FORMAT_R32_UINT;
        }

        D3D12_RESOURCE_DESC ib_negative_desc = buffer_desc(6u * sizeof(uint32_t));
        stats.create_index_buffer_negative_base_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_negative_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.index_buffer_negative_base));
        if (SUCCEEDED(stats.create_index_buffer_negative_base_hr) && scene.index_buffer_negative_base) {
            uint32_t* mapped = nullptr;
            if (SUCCEEDED(scene.index_buffer_negative_base->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) &&
                mapped) {
                const uint32_t indices[6] = {12, 13, 14, 14, 13, 15};
                std::memcpy(mapped, indices, sizeof(indices));
                D3D12_RANGE written = {0, sizeof(indices)};
                scene.index_buffer_negative_base->Unmap(0, &written);
                stats.negative_base_indices_created = 6;
                stats.negative_base_index_buffer_size = sizeof(indices);
            }
            scene.index_view_negative_base.BufferLocation = scene.index_buffer_negative_base->GetGPUVirtualAddress();
            scene.index_view_negative_base.SizeInBytes = stats.negative_base_index_buffer_size;
            scene.index_view_negative_base.Format = DXGI_FORMAT_R32_UINT;
            stats.negative_base_index_format = DXGI_FORMAT_R32_UINT;
        }

        D3D12_RESOURCE_DESC ib_execute_desc = buffer_desc(8u * sizeof(uint32_t));
        stats.create_index_buffer_execute_indirect_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &ib_execute_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.index_buffer_execute_indirect));
        if (SUCCEEDED(stats.create_index_buffer_execute_indirect_hr) && scene.index_buffer_execute_indirect) {
            uint32_t* mapped = nullptr;
            if (SUCCEEDED(scene.index_buffer_execute_indirect->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) &&
                mapped) {
                const uint32_t indices[8] = {0xffffffffu, 0xffffffffu, 18u, 19u, 20u, 20u, 19u, 21u};
                std::memcpy(mapped, indices, sizeof(indices));
                D3D12_RANGE written = {0, sizeof(indices)};
                scene.index_buffer_execute_indirect->Unmap(0, &written);
                stats.execute_indirect_indexed_indices_created = 6;
                stats.execute_indirect_indexed_index_buffer_size = sizeof(indices);
            }
            scene.index_view_execute_indirect.BufferLocation =
                scene.index_buffer_execute_indirect->GetGPUVirtualAddress();
            scene.index_view_execute_indirect.SizeInBytes = stats.execute_indirect_indexed_index_buffer_size;
            scene.index_view_execute_indirect.Format = DXGI_FORMAT_R32_UINT;
            stats.execute_indirect_indexed_index_format = DXGI_FORMAT_R32_UINT;
        }

        D3D12_RESOURCE_DESC arg_desc = buffer_desc(sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
        stats.create_execute_indirect_indexed_argument_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &arg_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.execute_indirect_indexed_argument_buffer));
        if (SUCCEEDED(stats.create_execute_indirect_indexed_argument_buffer_hr) &&
            scene.execute_indirect_indexed_argument_buffer) {
            D3D12_DRAW_INDEXED_ARGUMENTS* mapped = nullptr;
            if (SUCCEEDED(scene.execute_indirect_indexed_argument_buffer->Map(0, nullptr,
                                                                              reinterpret_cast<void**>(&mapped))) &&
                mapped) {
                mapped->IndexCountPerInstance = stats.execute_indirect_indexed_index_count;
                mapped->InstanceCount = stats.execute_indirect_indexed_instance_count;
                mapped->StartIndexLocation = stats.execute_indirect_indexed_start_index_location;
                mapped->BaseVertexLocation = stats.execute_indirect_indexed_base_vertex_location;
                mapped->StartInstanceLocation = stats.execute_indirect_indexed_start_instance_location;
                D3D12_RANGE written = {0, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)};
                scene.execute_indirect_indexed_argument_buffer->Unmap(0, &written);
            }
        }

        D3D12_INDIRECT_ARGUMENT_DESC execute_indirect_arg = {};
        execute_indirect_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC execute_signature_desc = {};
        execute_signature_desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        execute_signature_desc.NumArgumentDescs = 1;
        execute_signature_desc.pArgumentDescs = &execute_indirect_arg;
        stats.create_execute_indirect_indexed_command_signature_hr = device->CreateCommandSignature(
            &execute_signature_desc, nullptr, IID_PPV_ARGS(&scene.execute_indirect_indexed_command_signature));
    }

    stats.pass =
        stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
        SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) && SUCCEEDED(stats.create_pso_hr) &&
        SUCCEEDED(stats.create_vertex_buffer_hr) && SUCCEEDED(stats.create_dynamic_stride_vertex_buffer_hr) &&
        SUCCEEDED(stats.create_index_buffer_hr) && SUCCEEDED(stats.create_index_buffer_r32_hr) &&
        SUCCEEDED(stats.create_index_buffer_negative_base_hr) &&
        SUCCEEDED(stats.create_index_buffer_execute_indirect_hr) &&
        SUCCEEDED(stats.create_execute_indirect_indexed_argument_buffer_hr) &&
        SUCCEEDED(stats.create_execute_indirect_indexed_command_signature_hr) && stats.append_aligned_element &&
        stats.append_aligned_color_expected_offset == 12 && stats.vertices_created == 12 &&
        stats.vertex_buffer_size == 336 && stats.dynamic_stride_vertices_created == 4 &&
        stats.dynamic_stride_vertex_buffer_size == 128 && stats.dynamic_stride == 32 &&
        stats.vertex_view_byte_offset == 28 && stats.indices_created == 6 &&
        stats.index_format == DXGI_FORMAT_R16_UINT && stats.index_buffer_size == 16 &&
        stats.index_view_byte_offset == 4 && stats.start_index_location == 2 && stats.r32_indices_created == 6 &&
        stats.r32_index_format == DXGI_FORMAT_R32_UINT && stats.r32_index_buffer_size == 32 &&
        stats.r32_index_view_byte_offset == 8 && stats.r32_start_index_location == 2 &&
        stats.r32_base_vertex_location == 4 && stats.negative_base_indices_created == 6 &&
        stats.negative_base_index_format == DXGI_FORMAT_R32_UINT && stats.negative_base_index_buffer_size == 24 &&
        stats.negative_base_start_index_location == 0 && stats.negative_base_vertex_location == -4 &&
        stats.execute_indirect_indexed_indices_created == 6 &&
        stats.execute_indirect_indexed_index_format == DXGI_FORMAT_R32_UINT &&
        stats.execute_indirect_indexed_index_buffer_size == 32 &&
        stats.execute_indirect_indexed_argument_byte_stride == sizeof(D3D12_DRAW_INDEXED_ARGUMENTS) &&
        stats.execute_indirect_indexed_command_signature_arguments == 1 &&
        stats.execute_indirect_indexed_max_command_count == 1 && stats.execute_indirect_indexed_index_count == 6 &&
        stats.execute_indirect_indexed_instance_count == 1 &&
        stats.execute_indirect_indexed_start_index_location == 2 &&
        stats.execute_indirect_indexed_base_vertex_location == -6 &&
        stats.execute_indirect_indexed_start_instance_location == 0 && scene.root_signature && scene.pipeline_state &&
        scene.vertex_buffer && scene.vertex_buffer_dynamic_stride && scene.index_buffer && scene.index_buffer_r32 &&
        scene.index_buffer_negative_base && scene.index_buffer_execute_indirect &&
        scene.execute_indirect_indexed_argument_buffer && scene.execute_indirect_indexed_command_signature &&
        scene.vertex_view.BufferLocation != 0 && scene.vertex_view_dynamic_stride.BufferLocation != 0 &&
        scene.vertex_view_execute_indirect.BufferLocation != 0 && scene.index_view.BufferLocation != 0 &&
        scene.index_view_r32.BufferLocation != 0 && scene.index_view_negative_base.BufferLocation != 0 &&
        scene.index_view_execute_indirect.BufferLocation != 0;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_indexed_draw_scene(IndexedDrawSceneResources& scene) {
    safe_release(scene.execute_indirect_indexed_command_signature);
    safe_release(scene.execute_indirect_indexed_argument_buffer);
    safe_release(scene.index_buffer_execute_indirect);
    safe_release(scene.index_buffer_negative_base);
    safe_release(scene.index_buffer_r32);
    safe_release(scene.index_buffer);
    safe_release(scene.vertex_buffer_dynamic_stride);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct MultiSlotPositionVertex {
    float pos[3];
};

struct MultiSlotInstanceVertex {
    float color[4];
    float offset[2];
};

struct MultiSlotInstanceRootStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_position_buffer_hr = E_FAIL;
    HRESULT create_instance_buffer_hr = E_FAIL;
    uint32_t input_slots = 2;
    uint32_t position_vertices_created = 0;
    uint32_t instance_records_created = 0;
    uint32_t selected_instance_record = 5;
    uint32_t per_instance_step_rate = 2;
    uint32_t start_instance_location = 4;
    uint32_t vertex_count_per_draw = 6;
    uint32_t instance_count_per_draw = 3;
    uint32_t slot0_stride = sizeof(MultiSlotPositionVertex);
    uint32_t slot1_stride = sizeof(MultiSlotInstanceVertex);
    uint32_t root_constant_float_count = 8;
    uint32_t root_constant_sets = 0;
    uint32_t draw_calls = 0;
    uint32_t first_draw_calls = 0;
    uint32_t second_draw_calls = 0;
    uint8_t root_first_rgba[4] = {208, 96, 40, 255};
    uint8_t root_second_rgba[4] = {72, 208, 240, 255};
    uint8_t expected_first_rgba[4] = {104, 96, 20, 255};
    uint8_t expected_second_rgba[4] = {36, 208, 120, 255};
    uint8_t present_first_rgba[4] = {0, 0, 0, 0};
    uint8_t present_first_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_second_rgba[4] = {0, 0, 0, 0};
    uint8_t present_second_last_rgba[4] = {0, 0, 0, 0};
    uint32_t present_first_samples_checked = 0;
    uint32_t present_first_sample_matches = 0;
    uint32_t present_first_pixels_checked = 0;
    uint32_t present_first_pixel_matches = 0;
    uint32_t present_second_samples_checked = 0;
    uint32_t present_second_sample_matches = 0;
    uint32_t present_second_pixels_checked = 0;
    uint32_t present_second_pixel_matches = 0;
    bool present_pass = false;
    bool pass = false;
};

struct MultiSlotInstanceRootSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* position_buffer = nullptr;
    ID3D12Resource* instance_buffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_views[2] = {};
    D3D12_VERTEX_BUFFER_VIEW second_vertex_views[2] = {};
    float root_constants_first[8] = {};
    float root_constants_second[8] = {};
    MultiSlotInstanceRootStats stats;
};

static void fill_multi_slot_root_constants(float constants[8], const uint8_t rgba[4]) {
    constants[0] = static_cast<float>(rgba[0]) / 255.0f;
    constants[1] = static_cast<float>(rgba[1]) / 255.0f;
    constants[2] = static_cast<float>(rgba[2]) / 255.0f;
    constants[3] = static_cast<float>(rgba[3]) / 255.0f;
    constants[4] = 0.0f;
    constants[5] = 0.0f;
    constants[6] = 0.0f;
    constants[7] = 0.0f;
}

static void fill_multi_slot_position_quad(MultiSlotPositionVertex* out, UINT stamp_x, UINT stamp_y,
                                          UINT backbuffer_width, UINT backbuffer_height) {
    const float center_x = ndc_x_from_pixel(static_cast<float>(stamp_x) + static_cast<float>(kFreshTextureWidth) * 0.5f,
                                            static_cast<float>(backbuffer_width));
    const float center_y =
        ndc_y_from_pixel(static_cast<float>(stamp_y) + static_cast<float>(kFreshTextureHeight) * 0.5f,
                         static_cast<float>(backbuffer_height));
    const float half_x =
        (static_cast<float>(kFreshTextureWidth) * 0.5f + 1.0f) * 2.0f / static_cast<float>(backbuffer_width);
    const float half_y =
        (static_cast<float>(kFreshTextureHeight) * 0.5f + 1.0f) * 2.0f / static_cast<float>(backbuffer_height);
    const MultiSlotPositionVertex quad[6] = {
        {{center_x - half_x, center_y + half_y, 0.02f}}, {{center_x + half_x, center_y + half_y, 0.02f}},
        {{center_x - half_x, center_y - half_y, 0.02f}}, {{center_x - half_x, center_y - half_y, 0.02f}},
        {{center_x + half_x, center_y + half_y, 0.02f}}, {{center_x + half_x, center_y - half_y, 0.02f}}};
    std::memcpy(out, quad, sizeof(quad));
}

static MultiSlotInstanceRootSceneResources
create_multi_slot_instance_root_scene(ID3D12Device* device, D3DCompileFn compile, SerializeRootSignatureFn serialize,
                                      UINT backbuffer_width, UINT backbuffer_height) {
    MultiSlotInstanceRootSceneResources scene;
    MultiSlotInstanceRootStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    fill_multi_slot_root_constants(scene.root_constants_first, stats.root_first_rgba);
    fill_multi_slot_root_constants(scene.root_constants_second, stats.root_second_rgba);

    static const char* kMultiSlotHlsl = R"HLSL(
cbuffer RootConstants : register(b0) {
    float4 rootColor;
    float4 rootUnused;
};
struct VSIn {
    float3 pos : POSITION;
    float4 instColor : COLOR0;
    float2 instOffset : TEXCOORD0;
};
struct PSIn { float4 pos : SV_Position; float4 color : COLOR0; };
PSIn multi_slot_vs(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos.xy + input.instOffset, input.pos.z, 1.0f);
    output.color = saturate(input.instColor);
    return output;
}
float4 multi_slot_ps(PSIn input) : SV_Target {
    return saturate(input.color * rootColor);
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kMultiSlotHlsl, "multi_slot_vs", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kMultiSlotHlsl, "multi_slot_ps", "ps_5_0", &ps);

    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_param.Constants.ShaderRegister = 0;
    root_param.Constants.RegisterSpace = 0;
    root_param.Constants.Num32BitValues = stats.root_constant_float_count;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_param;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
             stats.per_instance_step_rate},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
             stats.per_instance_step_rate},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 3};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        const UINT position_vertex_count = stats.vertex_count_per_draw * 2u;
        D3D12_RESOURCE_DESC pos_desc = buffer_desc(position_vertex_count * sizeof(MultiSlotPositionVertex));
        stats.create_position_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &pos_desc,
                                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                          IID_PPV_ARGS(&scene.position_buffer));
        if (SUCCEEDED(stats.create_position_buffer_hr) && scene.position_buffer) {
            MultiSlotPositionVertex* mapped = nullptr;
            if (SUCCEEDED(scene.position_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                fill_multi_slot_position_quad(mapped, kMultiSlotInstanceRootStampAX, kMultiSlotInstanceRootStampAY,
                                              backbuffer_width, backbuffer_height);
                fill_multi_slot_position_quad(mapped + stats.vertex_count_per_draw, kMultiSlotInstanceRootStampBX,
                                              kMultiSlotInstanceRootStampBY, backbuffer_width, backbuffer_height);
                D3D12_RANGE written = {0, position_vertex_count * sizeof(MultiSlotPositionVertex)};
                scene.position_buffer->Unmap(0, &written);
                stats.position_vertices_created = position_vertex_count;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS base = scene.position_buffer->GetGPUVirtualAddress();
            scene.vertex_views[0].BufferLocation = base;
            scene.vertex_views[0].SizeInBytes = stats.vertex_count_per_draw * sizeof(MultiSlotPositionVertex);
            scene.vertex_views[0].StrideInBytes = sizeof(MultiSlotPositionVertex);
            scene.second_vertex_views[0].BufferLocation =
                base + stats.vertex_count_per_draw * sizeof(MultiSlotPositionVertex);
            scene.second_vertex_views[0].SizeInBytes = stats.vertex_count_per_draw * sizeof(MultiSlotPositionVertex);
            scene.second_vertex_views[0].StrideInBytes = sizeof(MultiSlotPositionVertex);
        }

        D3D12_RESOURCE_DESC instance_desc = buffer_desc(6u * sizeof(MultiSlotInstanceVertex));
        stats.create_instance_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &instance_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.instance_buffer));
        if (SUCCEEDED(stats.create_instance_buffer_hr) && scene.instance_buffer) {
            MultiSlotInstanceVertex* mapped = nullptr;
            if (SUCCEEDED(scene.instance_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                const MultiSlotInstanceVertex records[6] = {
                    {{0.0f, 0.0f, 1.0f, 1.0f}, {2.5f, 0.0f}}, {{1.0f, 0.0f, 1.0f, 1.0f}, {2.5f, 0.0f}},
                    {{0.5f, 0.5f, 0.5f, 1.0f}, {2.5f, 0.0f}}, {{0.0f, 1.0f, 0.0f, 1.0f}, {2.5f, 0.0f}},
                    {{1.0f, 0.0f, 0.0f, 1.0f}, {2.5f, 0.0f}}, {{0.5f, 1.0f, 0.5f, 1.0f}, {0.0f, 0.0f}},
                };
                std::memcpy(mapped, records, sizeof(records));
                D3D12_RANGE written = {0, sizeof(records)};
                scene.instance_buffer->Unmap(0, &written);
                stats.instance_records_created = 6;
            }
            scene.vertex_views[1].BufferLocation = scene.instance_buffer->GetGPUVirtualAddress();
            scene.vertex_views[1].SizeInBytes = 6u * sizeof(MultiSlotInstanceVertex);
            scene.vertex_views[1].StrideInBytes = sizeof(MultiSlotInstanceVertex);
            scene.second_vertex_views[1] = scene.vertex_views[1];
        }
    }

    stats.pass =
        stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
        SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) && SUCCEEDED(stats.create_pso_hr) &&
        SUCCEEDED(stats.create_position_buffer_hr) && SUCCEEDED(stats.create_instance_buffer_hr) &&
        stats.input_slots == 2 && stats.position_vertices_created == stats.vertex_count_per_draw * 2u &&
        stats.instance_records_created == 6 &&
        stats.selected_instance_record ==
            stats.start_instance_location + ((stats.instance_count_per_draw - 1u) / stats.per_instance_step_rate) &&
        stats.per_instance_step_rate == 2 && stats.start_instance_location == 4 && stats.vertex_count_per_draw == 6 &&
        stats.instance_count_per_draw == 3 && stats.slot0_stride == 12 && stats.slot1_stride == 24 &&
        stats.root_constant_float_count == 8 && scene.root_signature && scene.pipeline_state && scene.position_buffer &&
        scene.instance_buffer && scene.vertex_views[0].BufferLocation != 0 &&
        scene.vertex_views[1].BufferLocation != 0 && scene.second_vertex_views[0].BufferLocation != 0 &&
        scene.second_vertex_views[1].BufferLocation != 0;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_multi_slot_instance_root_scene(MultiSlotInstanceRootSceneResources& scene) {
    safe_release(scene.instance_buffer);
    safe_release(scene.position_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct TessellationFallbackStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_hs_hr = E_FAIL;
    HRESULT compile_ds_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_index_buffer_hr = E_FAIL;
    uint32_t patch_control_points = 3;
    uint32_t vertices_created = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t indices_created = 0;
    uint32_t indices_per_draw = 0;
    uint32_t draw_calls = 0;
    uint32_t draw_indexed_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {248, 96, 176, 255};
    uint8_t indexed_expected_rgba[4] = {32, 224, 216, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    uint8_t indexed_present_rgba[4] = {0, 0, 0, 0};
    uint8_t indexed_present_last_rgba[4] = {0, 0, 0, 0};
    uint32_t indexed_present_samples_checked = 0;
    uint32_t indexed_present_sample_matches = 0;
    uint32_t indexed_present_pixels_checked = 0;
    uint32_t indexed_present_pixel_matches = 0;
    bool indexed_present_pass = false;
    uint32_t progressive_triangle_target_frames = 0;
    uint32_t progressive_triangle_frames_validated = 0;
    uint32_t progressive_triangle_samples_checked = 0;
    uint32_t progressive_triangle_seed_pixels = 0;
    uint32_t progressive_triangle_final_pixels = 0;
    uint32_t progressive_triangle_peak_pixels = 0;
    uint32_t progressive_triangle_final_red_dominant_pixels = 0;
    uint32_t progressive_triangle_final_green_dominant_pixels = 0;
    uint32_t progressive_triangle_final_blue_dominant_pixels = 0;
    bool progressive_triangle_coverage_monotonic = true;
    bool progressive_triangle_seed_point_pass = false;
    bool progressive_triangle_full_pass = false;
    bool progressive_triangle_rgb_channels_present = false;
    bool progressive_triangle_ok = false;
    uint32_t tessellation_factor_half_edge = 0x3c00u;
    uint32_t tessellation_factor_half_inside = 0x3c00u;
    float tessellation_factor_float = 1.0f;
    uint64_t motion_color_hash = 1469598103934665603ull;
    float last_control_points[9] = {};
    std::vector<uint32_t> progressive_triangle_coverage_counts;
    std::vector<uint32_t> tessellation_factor_history;
    bool native_tessellation_required = false;
    bool native_tessellation_resolved = false;
    bool native_draw_encoded = false;
    bool native_indexed_draw_encoded = false;
    bool fallback_draw_encoded = false;
    bool blocked_expected = false;
    bool present_pass = false;
    bool pass = false;
};

struct TessellationFallbackSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* index_buffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    D3D12_INDEX_BUFFER_VIEW index_view = {};
    TessellationFallbackStats stats;
};

static void tessellation_fallback_expected_rgba(uint8_t out[4]) {
    out[0] = 248;
    out[1] = 96;
    out[2] = 176;
    out[3] = 255;
}

static float tessellation_progressive_triangle_scale(uint32_t frame, uint32_t target_frames) {
    constexpr float seed_scale = 0.050f;
    if (target_frames <= 1u)
        return 1.0f;
    const float t = std::min<float>(1.0f, static_cast<float>(frame) / static_cast<float>(target_frames - 1u));
    return seed_scale + (1.0f - seed_scale) * t;
}

static ColorVertex tessellation_progressive_triangle_vertex(float pixel_x, float pixel_y, float scale, float r, float g,
                                                            float b) {
    constexpr float center_x =
        (static_cast<float>(kTessellationProgressiveTopX) + static_cast<float>(kTessellationProgressiveLeftX) +
         static_cast<float>(kTessellationProgressiveRightX)) /
        3.0f;
    constexpr float center_y =
        (static_cast<float>(kTessellationProgressiveTopY) + static_cast<float>(kTessellationProgressiveLeftY) +
         static_cast<float>(kTessellationProgressiveRightY)) /
        3.0f;
    const float scaled_x = center_x + (pixel_x - center_x) * scale;
    const float scaled_y = center_y + (pixel_y - center_y) * scale;
    const float ndc_x = scaled_x * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f;
    const float ndc_y = 1.0f - scaled_y * 2.0f / static_cast<float>(kBackbufferHeight);
    return {{ndc_x, ndc_y, 0.021f}, {r, g, b, 1.0f}};
}

static bool populate_tessellation_native_vertices(TessellationFallbackSceneResources& scene,
                                                  TessellationFallbackStats& stats, uint32_t frame,
                                                  uint32_t target_frames, UINT backbuffer_width,
                                                  UINT backbuffer_height) {
    if (!scene.vertex_buffer)
        return false;
    ColorVertex* mapped = nullptr;
    if (FAILED(scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;

    const float draw_x0 = static_cast<float>(kTessellationFallbackStampX) - 1.0f;
    const float draw_y0 = static_cast<float>(kTessellationFallbackStampY) - 1.0f;
    const float draw_x1 = static_cast<float>(kTessellationFallbackStampX + kFreshTextureWidth) + 1.0f;
    const float draw_y1 = static_cast<float>(kTessellationFallbackStampY + kFreshTextureHeight) + 1.0f;
    const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
    const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
    const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
    const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
    const float r = static_cast<float>(stats.expected_rgba[0]) / 255.0f;
    const float g = static_cast<float>(stats.expected_rgba[1]) / 255.0f;
    const float b = static_cast<float>(stats.expected_rgba[2]) / 255.0f;
    const float a = static_cast<float>(stats.expected_rgba[3]) / 255.0f;
    for (UINT quad = 0; quad < kTessellationFallbackStaticVertexCount / 6u; ++quad)
        write_quad(mapped + quad * 6u, x0, y0, x1, y1, 0.022f, r, g, b, a);

    const float region_x0 = static_cast<float>(kTessellationProgressiveRegionX);
    const float region_y0 = static_cast<float>(kTessellationProgressiveRegionY);
    const float region_x1 = static_cast<float>(kTessellationProgressiveRegionX + kTessellationProgressiveRegionWidth);
    const float region_y1 = static_cast<float>(kTessellationProgressiveRegionY + kTessellationProgressiveRegionHeight);
    ColorVertex* background = mapped + kTessellationFallbackStaticVertexCount;
    write_quad(background, region_x0 * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f,
               1.0f - region_y1 * 2.0f / static_cast<float>(kBackbufferHeight),
               region_x1 * 2.0f / static_cast<float>(kBackbufferWidth) - 1.0f,
               1.0f - region_y0 * 2.0f / static_cast<float>(kBackbufferHeight), 0.020f, 0.01f, 0.02f, 0.05f, 1.0f);

    const float scale = tessellation_progressive_triangle_scale(frame, target_frames);
    ColorVertex* tri = background + kTessellationProgressiveBackgroundVertexCount;
    tri[0] = tessellation_progressive_triangle_vertex(static_cast<float>(kTessellationProgressiveTopX),
                                                      static_cast<float>(kTessellationProgressiveTopY), scale, 1.0f,
                                                      0.0f, 0.0f);
    tri[1] = tessellation_progressive_triangle_vertex(static_cast<float>(kTessellationProgressiveLeftX),
                                                      static_cast<float>(kTessellationProgressiveLeftY), scale, 0.0f,
                                                      1.0f, 0.0f);
    tri[2] = tessellation_progressive_triangle_vertex(static_cast<float>(kTessellationProgressiveRightX),
                                                      static_cast<float>(kTessellationProgressiveRightY), scale, 0.0f,
                                                      0.0f, 1.0f);
    for (UINT i = 0; i < 3; ++i) {
        stats.last_control_points[i * 3u + 0u] = tri[i].position[0];
        stats.last_control_points[i * 3u + 1u] = tri[i].position[1];
        stats.last_control_points[i * 3u + 2u] = tri[i].position[2];
    }

    const float indexed_draw_x0 = static_cast<float>(kTessellationIndexedStampX) - 1.0f;
    const float indexed_draw_y0 = static_cast<float>(kTessellationIndexedStampY) - 1.0f;
    const float indexed_draw_x1 = static_cast<float>(kTessellationIndexedStampX + kFreshTextureWidth) + 1.0f;
    const float indexed_draw_y1 = static_cast<float>(kTessellationIndexedStampY + kFreshTextureHeight) + 1.0f;
    const float ix0 = ndc_x_from_pixel(indexed_draw_x0, static_cast<float>(backbuffer_width));
    const float ix1 = ndc_x_from_pixel(indexed_draw_x1, static_cast<float>(backbuffer_width));
    const float iy0 = ndc_y_from_pixel(indexed_draw_y1, static_cast<float>(backbuffer_height));
    const float iy1 = ndc_y_from_pixel(indexed_draw_y0, static_cast<float>(backbuffer_height));
    const float ir = static_cast<float>(stats.indexed_expected_rgba[0]) / 255.0f;
    const float ig = static_cast<float>(stats.indexed_expected_rgba[1]) / 255.0f;
    const float ib = static_cast<float>(stats.indexed_expected_rgba[2]) / 255.0f;
    const float ia = static_cast<float>(stats.indexed_expected_rgba[3]) / 255.0f;
    ColorVertex* indexed = mapped + kTessellationNonIndexedVertexCount;
    for (UINT quad = 0; quad < kTessellationIndexedVertexCount / 6u; ++quad)
        write_quad(indexed + quad * 6u, ix0, iy0, ix1, iy1, 0.023f, ir, ig, ib, ia);
    stats.tessellation_factor_history.push_back(stats.tessellation_factor_half_edge);
    stats.motion_color_hash =
        fnv1a_update(stats.motion_color_hash, reinterpret_cast<const uint8_t*>(&frame), sizeof(frame));
    stats.motion_color_hash = fnv1a_update(stats.motion_color_hash, reinterpret_cast<const uint8_t*>(tri),
                                           sizeof(ColorVertex) * kTessellationProgressivePatchVertexCount);

    D3D12_RANGE written = {0, kTessellationFallbackVertexCount * sizeof(ColorVertex)};
    scene.vertex_buffer->Unmap(0, &written);
    return true;
}

static TessellationFallbackSceneResources create_tessellation_fallback_scene(ID3D12Device* device, D3DCompileFn compile,
                                                                             SerializeRootSignatureFn serialize,
                                                                             UINT backbuffer_width,
                                                                             UINT backbuffer_height) {
    TessellationFallbackSceneResources scene;
    TessellationFallbackStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    tessellation_fallback_expected_rgba(stats.expected_rgba);

    static const char* kTessellationFallbackHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float4 color : COLOR0; };
struct TessCP { float3 world : POSITION; float4 pos : SV_Position; float4 color : COLOR0; };
struct HSConst { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
TessCP tess_vs(VSIn input) {
    TessCP output;
    output.world = input.pos;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
HSConst tess_constants(InputPatch<TessCP, 3> patch, uint patch_id : SV_PrimitiveID) {
    HSConst output;
    output.edge[0] = 1.0f;
    output.edge[1] = 1.0f;
    output.edge[2] = 1.0f;
    output.inside = 1.0f;
    return output;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("tess_constants")]
TessCP tess_hs(InputPatch<TessCP, 3> patch, uint point_id : SV_OutputControlPointID,
               uint patch_id : SV_PrimitiveID) {
    return patch[point_id];
}
[domain("tri")]
TessCP tess_ds(HSConst factors, const OutputPatch<TessCP, 3> patch, float3 bary : SV_DomainLocation) {
    TessCP output;
    output.world = patch[0].world * bary.x + patch[1].world * bary.y + patch[2].world * bary.z;
    output.pos = float4(output.world, 1.0f);
    output.color = patch[0].color * bary.x + patch[1].color * bary.y + patch[2].color * bary.z;
    return output;
}
float4 tess_ps(TessCP input) : SV_Target {
    return saturate(input.color);
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* hs = nullptr;
    ID3DBlob* ds = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kTessellationFallbackHlsl, "tess_vs", "vs_5_0", &vs);
    stats.compile_hs_hr = compile_shader(compile, kTessellationFallbackHlsl, "tess_hs", "hs_5_0", &hs);
    stats.compile_ds_hr = compile_shader(compile, kTessellationFallbackHlsl, "tess_ds", "ds_5_0", &ds);
    stats.compile_ps_hr = compile_shader(compile, kTessellationFallbackHlsl, "tess_ps", "ps_5_0", &ps);

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && hs && ds && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
        pso_desc.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
        stats.native_tessellation_required = true;
        stats.native_tessellation_resolved = SUCCEEDED(stats.create_pso_hr) && scene.pipeline_state != nullptr;
        stats.blocked_expected = FAILED(stats.create_pso_hr) && scene.pipeline_state == nullptr;
        stats.fallback_draw_encoded = false;
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(kTessellationFallbackVertexCount * sizeof(ColorVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            stats.vertices_created = kTessellationFallbackVertexCount;
            stats.vertices_per_draw = kTessellationNonIndexedVertexCount;
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = kTessellationFallbackVertexCount * sizeof(ColorVertex);
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
            populate_tessellation_native_vertices(scene, scene.stats, 0, 1, backbuffer_width, backbuffer_height);
        }

        D3D12_RESOURCE_DESC ib_desc = buffer_desc(kTessellationFallbackVertexCount * sizeof(uint16_t));
        stats.create_index_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ib_desc,
                                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                       IID_PPV_ARGS(&scene.index_buffer));
        if (SUCCEEDED(stats.create_index_buffer_hr) && scene.index_buffer) {
            uint16_t* indices = nullptr;
            if (SUCCEEDED(scene.index_buffer->Map(0, nullptr, reinterpret_cast<void**>(&indices))) && indices) {
                for (UINT i = 0; i < kTessellationFallbackVertexCount; ++i)
                    indices[i] = static_cast<uint16_t>(i);
                D3D12_RANGE written = {0, kTessellationFallbackVertexCount * sizeof(uint16_t)};
                scene.index_buffer->Unmap(0, &written);
                stats.indices_created = kTessellationIndexedVertexCount;
                stats.indices_per_draw = kTessellationIndexedVertexCount;
            }
            scene.index_view.BufferLocation = scene.index_buffer->GetGPUVirtualAddress();
            scene.index_view.SizeInBytes = kTessellationFallbackVertexCount * sizeof(uint16_t);
            scene.index_view.Format = DXGI_FORMAT_R16_UINT;
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_hs_hr) &&
                 SUCCEEDED(stats.compile_ds_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_vertex_buffer_hr) && SUCCEEDED(stats.create_index_buffer_hr) &&
                 stats.native_tessellation_required && stats.native_tessellation_resolved && !stats.blocked_expected &&
                 !stats.fallback_draw_encoded && stats.vertices_created == kTessellationFallbackVertexCount &&
                 stats.vertices_per_draw == kTessellationNonIndexedVertexCount &&
                 stats.indices_created == kTessellationIndexedVertexCount &&
                 stats.indices_per_draw == kTessellationIndexedVertexCount;

    safe_release(vs);
    safe_release(hs);
    safe_release(ds);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_tessellation_fallback_scene(TessellationFallbackSceneResources& scene) {
    safe_release(scene.index_buffer);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct IndirectDrawStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_argument_buffer_hr = E_FAIL;
    HRESULT create_command_signature_hr = E_FAIL;
    uint32_t vertices_created = 0;
    uint32_t argument_byte_stride = sizeof(D3D12_DRAW_ARGUMENTS);
    uint32_t command_signature_arguments = 1;
    uint32_t max_command_count = 1;
    uint32_t argument_vertex_count = 24;
    uint32_t argument_instance_count = 1;
    uint32_t argument_start_vertex = 0;
    uint32_t argument_start_instance = 0;
    uint32_t execute_indirect_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {80, 224, 240, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool present_pass = false;
    bool pass = false;
};

struct IndirectDrawSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* argument_buffer = nullptr;
    ID3D12CommandSignature* command_signature = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    IndirectDrawStats stats;
};

static void indirect_draw_expected_rgba(uint8_t out[4]) {
    out[0] = 80;
    out[1] = 224;
    out[2] = 240;
    out[3] = 255;
}

static IndirectDrawSceneResources create_indirect_draw_scene(ID3D12Device* device, D3DCompileFn compile,
                                                             SerializeRootSignatureFn serialize, UINT backbuffer_width,
                                                             UINT backbuffer_height) {
    IndirectDrawSceneResources scene;
    IndirectDrawStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    indirect_draw_expected_rgba(stats.expected_rgba);

    static const char* kIndirectHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float4 color : COLOR0; };
struct PSIn { float4 pos : SV_Position; float4 color : COLOR0; };
PSIn indirect_vs(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
float4 indirect_ps(PSIn input) : SV_Target {
    return saturate(float4(input.color.z, input.color.x, input.color.y, input.color.w));
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kIndirectHlsl, "indirect_vs", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kIndirectHlsl, "indirect_ps", "ps_5_0", &ps);

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(stats.argument_vertex_count * sizeof(ColorVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            ColorVertex* mapped = nullptr;
            if (SUCCEEDED(scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                const float draw_x0 = static_cast<float>(kIndirectStampX) - 1.0f;
                const float draw_y0 = static_cast<float>(kIndirectStampY) - 1.0f;
                const float draw_x1 = static_cast<float>(kIndirectStampX + kFreshTextureWidth) + 1.0f;
                const float draw_y1 = static_cast<float>(kIndirectStampY + kFreshTextureHeight) + 1.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                const float src_r = static_cast<float>(stats.expected_rgba[1]) / 255.0f;
                const float src_g = static_cast<float>(stats.expected_rgba[2]) / 255.0f;
                const float src_b = static_cast<float>(stats.expected_rgba[0]) / 255.0f;
                const float a = static_cast<float>(stats.expected_rgba[3]) / 255.0f;
                const ColorVertex quad[6] = {
                    {{x0, y1, 0.03f}, {src_r, src_g, src_b, a}}, {{x1, y1, 0.03f}, {src_r, src_g, src_b, a}},
                    {{x0, y0, 0.03f}, {src_r, src_g, src_b, a}}, {{x0, y0, 0.03f}, {src_r, src_g, src_b, a}},
                    {{x1, y1, 0.03f}, {src_r, src_g, src_b, a}}, {{x1, y0, 0.03f}, {src_r, src_g, src_b, a}}};
                for (uint32_t i = 0; i < stats.argument_vertex_count; i += 6)
                    std::memcpy(mapped + i, quad, sizeof(quad));
                D3D12_RANGE written = {0, stats.argument_vertex_count * sizeof(ColorVertex)};
                scene.vertex_buffer->Unmap(0, &written);
                stats.vertices_created = stats.argument_vertex_count;
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = stats.argument_vertex_count * sizeof(ColorVertex);
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
        }

        D3D12_RESOURCE_DESC arg_desc = buffer_desc(sizeof(D3D12_DRAW_ARGUMENTS));
        stats.create_argument_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &arg_desc,
                                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                          IID_PPV_ARGS(&scene.argument_buffer));
        if (SUCCEEDED(stats.create_argument_buffer_hr) && scene.argument_buffer) {
            D3D12_DRAW_ARGUMENTS* mapped = nullptr;
            if (SUCCEEDED(scene.argument_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
                mapped->VertexCountPerInstance = stats.argument_vertex_count;
                mapped->InstanceCount = stats.argument_instance_count;
                mapped->StartVertexLocation = stats.argument_start_vertex;
                mapped->StartInstanceLocation = stats.argument_start_instance;
                D3D12_RANGE written = {0, sizeof(D3D12_DRAW_ARGUMENTS)};
                scene.argument_buffer->Unmap(0, &written);
            }
        }

        D3D12_INDIRECT_ARGUMENT_DESC indirect_arg = {};
        indirect_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
        signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        signature_desc.NumArgumentDescs = 1;
        signature_desc.pArgumentDescs = &indirect_arg;
        stats.create_command_signature_hr =
            device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&scene.command_signature));
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_argument_buffer_hr) && SUCCEEDED(stats.create_command_signature_hr) &&
                 stats.vertices_created == stats.argument_vertex_count && stats.argument_byte_stride == 16 &&
                 stats.command_signature_arguments == 1 && stats.max_command_count == 1 && scene.root_signature &&
                 scene.pipeline_state && scene.vertex_buffer && scene.argument_buffer && scene.command_signature &&
                 scene.vertex_view.BufferLocation != 0;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_indirect_draw_scene(IndirectDrawSceneResources& scene) {
    safe_release(scene.command_signature);
    safe_release(scene.argument_buffer);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct NaniteClusterStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_cs_hr = E_FAIL;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_compute_root_hr = E_FAIL;
    HRESULT serialize_graphics_root_hr = E_FAIL;
    HRESULT create_compute_root_hr = E_FAIL;
    HRESULT create_graphics_root_hr = E_FAIL;
    HRESULT create_compute_pso_hr = E_FAIL;
    HRESULT create_graphics_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_argument_buffer_hr = E_FAIL;
    HRESULT create_argument_readback_hr = E_FAIL;
    HRESULT create_indirect_argument_upload_hr = E_FAIL;
    HRESULT create_command_signature_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    D3D12_GPU_VIRTUAL_ADDRESS argument_gpu_virtual_address = 0;
    D3D12_GPU_VIRTUAL_ADDRESS indirect_argument_gpu_virtual_address = 0;
    uint32_t vertices_created = 0;
    uint32_t argument_byte_stride = sizeof(D3D12_DRAW_ARGUMENTS);
    uint32_t command_signature_arguments = 1;
    uint32_t max_command_count = 1;
    uint32_t dispatch_commands = 0;
    uint32_t uav_barriers = 0;
    uint32_t copy_argument_readback_commands = 0;
    uint32_t transition_to_copy_barriers = 0;
    uint32_t transition_to_indirect_barriers = 0;
    uint32_t computed_vertex_count = 0;
    uint32_t computed_instance_count = 0;
    uint32_t computed_start_vertex = 0xffffffffu;
    uint32_t computed_start_instance = 0xffffffffu;
    uint32_t execute_indirect_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {176, 112, 232, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool argument_readback_ok = false;
    bool indirect_argument_upload_filled = false;
    bool fence_wait_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct NaniteClusterSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* argument_buffer = nullptr;
    ID3D12Resource* indirect_argument_buffer = nullptr;
    ID3D12CommandSignature* command_signature = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    NaniteClusterStats stats;
};

static void nanite_cluster_expected_rgba(uint8_t out[4]) {
    out[0] = 176;
    out[1] = 112;
    out[2] = 232;
    out[3] = 255;
}

static NaniteClusterSceneResources
create_nanite_cluster_scene(ID3D12Device* device, D3DCompileFn compile, SerializeRootSignatureFn serialize,
                            ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator,
                            ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fence_event,
                            UINT64& fence_value, UINT backbuffer_width, UINT backbuffer_height) {
    NaniteClusterSceneResources scene;
    NaniteClusterStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    nanite_cluster_expected_rgba(stats.expected_rgba);
    if (!device || !compile || !serialize || !queue || !allocator || !list || !fence || !fence_event)
        return scene;

    static const char* kNaniteClusterHlsl = R"HLSL(
RWStructuredBuffer<uint> DrawArgs : register(u0);
[numthreads(1, 1, 1)]
void nanite_cull_cs(uint3 tid : SV_DispatchThreadID) {
    DrawArgs[0] = 6u;
    DrawArgs[1] = 1u;
    DrawArgs[2] = 0u;
    DrawArgs[3] = 0u;
}
struct VSIn { float3 pos : POSITION; float4 color : COLOR0; };
struct PSIn { float4 pos : SV_Position; float4 color : COLOR0; };
PSIn nanite_vs(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.color = input.color;
    return output;
}
float4 nanite_ps(PSIn input) : SV_Target {
    float guard = input.pos.w < 0.0f ? (1.0f / 255.0f) : 0.0f;
    return saturate(input.color + float4(guard, 0.0f, 0.0f, 0.0f));
}
)HLSL";

    ID3DBlob* cs = nullptr;
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_cs_hr = compile_shader(compile, kNaniteClusterHlsl, "nanite_cull_cs", "cs_5_0", &cs);
    stats.compile_vs_hr = compile_shader(compile, kNaniteClusterHlsl, "nanite_vs", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kNaniteClusterHlsl, "nanite_ps", "ps_5_0", &ps);

    D3D12_ROOT_PARAMETER compute_root_params[1] = {};
    compute_root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    compute_root_params[0].Descriptor.ShaderRegister = 0;
    compute_root_params[0].Descriptor.RegisterSpace = 0;
    compute_root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC compute_root_desc = {};
    compute_root_desc.NumParameters = 1;
    compute_root_desc.pParameters = compute_root_params;
    compute_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob* compute_root_blob = nullptr;
    stats.serialize_compute_root_hr = serialize_root_signature(serialize, compute_root_desc, &compute_root_blob);

    D3D12_ROOT_SIGNATURE_DESC graphics_root_desc = {};
    graphics_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* graphics_root_blob = nullptr;
    stats.serialize_graphics_root_hr = serialize_root_signature(serialize, graphics_root_desc, &graphics_root_blob);

    ID3D12RootSignature* compute_root_signature = nullptr;
    ID3D12PipelineState* compute_pipeline_state = nullptr;
    if (compute_root_blob) {
        stats.create_compute_root_hr =
            device->CreateRootSignature(0, compute_root_blob->GetBufferPointer(), compute_root_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&compute_root_signature));
    }
    if (graphics_root_blob) {
        stats.create_graphics_root_hr =
            device->CreateRootSignature(0, graphics_root_blob->GetBufferPointer(), graphics_root_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&scene.root_signature));
    }
    if (compute_root_signature && cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = compute_root_signature;
        pso_desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        stats.create_compute_pso_hr =
            device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&compute_pipeline_state));
    }
    if (scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_graphics_pso_hr =
            device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC vb_desc = buffer_desc(6u * sizeof(ColorVertex));
    stats.create_vertex_buffer_hr =
        device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                        nullptr, IID_PPV_ARGS(&scene.vertex_buffer));
    if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
        ColorVertex* mapped = nullptr;
        if (SUCCEEDED(scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
            const float draw_x0 = static_cast<float>(kNaniteClusterStampX) - 1.0f;
            const float draw_y0 = static_cast<float>(kNaniteClusterStampY) - 1.0f;
            const float draw_x1 = static_cast<float>(kNaniteClusterStampX + kFreshTextureWidth) + 1.0f;
            const float draw_y1 = static_cast<float>(kNaniteClusterStampY + kFreshTextureHeight) + 1.0f;
            const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
            const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
            const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
            const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
            const float r = static_cast<float>(stats.expected_rgba[0]) / 255.0f;
            const float g = static_cast<float>(stats.expected_rgba[1]) / 255.0f;
            const float b = static_cast<float>(stats.expected_rgba[2]) / 255.0f;
            const float a = static_cast<float>(stats.expected_rgba[3]) / 255.0f;
            const ColorVertex quad[6] = {{{x0, y1, 0.02f}, {r, g, b, a}}, {{x1, y1, 0.02f}, {r, g, b, a}},
                                         {{x0, y0, 0.02f}, {r, g, b, a}}, {{x0, y0, 0.02f}, {r, g, b, a}},
                                         {{x1, y1, 0.02f}, {r, g, b, a}}, {{x1, y0, 0.02f}, {r, g, b, a}}};
            std::memcpy(mapped, quad, sizeof(quad));
            D3D12_RANGE written = {0, sizeof(quad)};
            scene.vertex_buffer->Unmap(0, &written);
            stats.vertices_created = 6;
        }
        scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
        scene.vertex_view.SizeInBytes = 6u * sizeof(ColorVertex);
        scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
    }

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC arg_desc = buffer_desc(256u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(256u);
    D3D12_RESOURCE_DESC indirect_upload_desc = buffer_desc(sizeof(D3D12_DRAW_ARGUMENTS));
    ID3D12Resource* argument_readback = nullptr;
    stats.create_argument_buffer_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &arg_desc,
                                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                      IID_PPV_ARGS(&scene.argument_buffer));
    stats.create_argument_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&argument_readback));
    stats.create_indirect_argument_upload_hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &indirect_upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&scene.indirect_argument_buffer));
    if (scene.argument_buffer)
        stats.argument_gpu_virtual_address = scene.argument_buffer->GetGPUVirtualAddress();
    if (scene.indirect_argument_buffer)
        stats.indirect_argument_gpu_virtual_address = scene.indirect_argument_buffer->GetGPUVirtualAddress();

    D3D12_INDIRECT_ARGUMENT_DESC indirect_arg = {};
    indirect_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &indirect_arg;
    stats.create_command_signature_hr =
        device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&scene.command_signature));

    bool submitted_work = false;
    if (compute_pipeline_state && compute_root_signature && scene.argument_buffer && argument_readback &&
        SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr))) {
        list->SetComputeRootSignature(compute_root_signature);
        list->SetPipelineState(compute_pipeline_state);
        list->SetComputeRootUnorderedAccessView(0, scene.argument_buffer->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
        stats.dispatch_commands++;
        D3D12_RESOURCE_BARRIER uav = uav_resource_barrier(scene.argument_buffer);
        list->ResourceBarrier(1, &uav);
        stats.uav_barriers++;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            scene.argument_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        stats.transition_to_copy_barriers++;
        list->CopyBufferRegion(argument_readback, 0, scene.argument_buffer, 0, sizeof(D3D12_DRAW_ARGUMENTS));
        stats.copy_argument_readback_commands++;
        D3D12_RESOURCE_BARRIER to_indirect = transition_barrier(scene.argument_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        list->ResourceBarrier(1, &to_indirect);
        stats.transition_to_indirect_barriers++;
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (argument_readback && stats.fence_wait_ok) {
        uint32_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, sizeof(D3D12_DRAW_ARGUMENTS)};
        stats.map_readback_hr = argument_readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            stats.computed_vertex_count = mapped[0];
            stats.computed_instance_count = mapped[1];
            stats.computed_start_vertex = mapped[2];
            stats.computed_start_instance = mapped[3];
            stats.argument_readback_ok = stats.computed_vertex_count == 6u && stats.computed_instance_count == 1u &&
                                         stats.computed_start_vertex == 0u && stats.computed_start_instance == 0u;
            D3D12_RANGE written = {0, 0};
            argument_readback->Unmap(0, &written);
        }
    }
    if (stats.argument_readback_ok && scene.indirect_argument_buffer) {
        D3D12_DRAW_ARGUMENTS* mapped = nullptr;
        if (SUCCEEDED(scene.indirect_argument_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) && mapped) {
            mapped->VertexCountPerInstance = stats.computed_vertex_count;
            mapped->InstanceCount = stats.computed_instance_count;
            mapped->StartVertexLocation = stats.computed_start_vertex;
            mapped->StartInstanceLocation = stats.computed_start_instance;
            D3D12_RANGE written = {0, sizeof(D3D12_DRAW_ARGUMENTS)};
            scene.indirect_argument_buffer->Unmap(0, &written);
            stats.indirect_argument_upload_filled = true;
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_cs_hr) && SUCCEEDED(stats.compile_vs_hr) &&
                 SUCCEEDED(stats.compile_ps_hr) && SUCCEEDED(stats.serialize_compute_root_hr) &&
                 SUCCEEDED(stats.serialize_graphics_root_hr) && SUCCEEDED(stats.create_compute_root_hr) &&
                 SUCCEEDED(stats.create_graphics_root_hr) && SUCCEEDED(stats.create_compute_pso_hr) &&
                 SUCCEEDED(stats.create_graphics_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_argument_buffer_hr) && SUCCEEDED(stats.create_argument_readback_hr) &&
                 SUCCEEDED(stats.create_indirect_argument_upload_hr) && SUCCEEDED(stats.create_command_signature_hr) &&
                 stats.argument_gpu_virtual_address != 0 && stats.indirect_argument_gpu_virtual_address != 0 &&
                 stats.vertices_created == 6 && stats.argument_byte_stride == 16 &&
                 stats.command_signature_arguments == 1 && stats.max_command_count == 1 &&
                 stats.dispatch_commands == 1 && stats.uav_barriers == 1 &&
                 stats.copy_argument_readback_commands == 1 && stats.transition_to_copy_barriers == 1 &&
                 stats.transition_to_indirect_barriers == 1 && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && SUCCEEDED(stats.map_readback_hr) &&
                 stats.argument_readback_ok && stats.indirect_argument_upload_filled && scene.root_signature &&
                 scene.pipeline_state && scene.vertex_buffer && scene.argument_buffer &&
                 scene.indirect_argument_buffer && scene.command_signature && scene.vertex_view.BufferLocation != 0;
    if (!stats.pass) {
        safe_release(scene.indirect_argument_buffer);
        safe_release(scene.argument_buffer);
    }
    safe_release(argument_readback);
    safe_release(compute_pipeline_state);
    safe_release(compute_root_signature);
    safe_release(cs);
    safe_release(vs);
    safe_release(ps);
    safe_release(compute_root_blob);
    safe_release(graphics_root_blob);
    return scene;
}

static void destroy_nanite_cluster_scene(NaniteClusterSceneResources& scene) {
    safe_release(scene.command_signature);
    safe_release(scene.indirect_argument_buffer);
    safe_release(scene.argument_buffer);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct DxilSceneStats {
    std::string vs_path;
    std::string ps_path;
    bool vs_loaded = false;
    bool ps_loaded = false;
    uint64_t vs_bytes = 0;
    uint64_t ps_bytes = 0;
    uint64_t vs_fnv1a64 = 1469598103934665603ull;
    uint64_t ps_fnv1a64 = 1469598103934665603ull;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    uint32_t draw_calls = 0;
    uint32_t vertices_per_draw = 0;
    bool pass = false;
};

struct DxilSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    DxilSceneStats stats;
};

static DxilSceneResources create_dxil_scene(ID3D12Device* device, SerializeRootSignatureFn serialize,
                                            const std::string& vs_path, const std::string& ps_path) {
    DxilSceneResources scene;
    scene.stats.vs_path = vs_path;
    scene.stats.ps_path = ps_path;
    std::vector<uint8_t> vs;
    std::vector<uint8_t> ps;
    scene.stats.vs_loaded = read_file_bytes(vs_path, vs, scene.stats.vs_fnv1a64);
    scene.stats.ps_loaded = read_file_bytes(ps_path, ps, scene.stats.ps_fnv1a64);
    scene.stats.vs_bytes = static_cast<uint64_t>(vs.size());
    scene.stats.ps_bytes = static_cast<uint64_t>(ps.size());

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    scene.stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        scene.stats.create_root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && scene.stats.vs_loaded && scene.stats.ps_loaded) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs.data(), vs.size()};
        pso_desc.PS = {ps.data(), ps.size()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        scene.stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        const ColorVertex dxil_overlay_triangle[3] = {
            {{-0.88f, -0.84f, 0.0f}, {0.25f, 0.75f, 0.50f, 1.0f}},
            {{0.00f, 0.88f, 0.0f}, {0.25f, 0.75f, 0.50f, 1.0f}},
            {{0.88f, -0.84f, 0.0f}, {0.25f, 0.75f, 0.50f, 1.0f}},
        };
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(sizeof(dxil_overlay_triangle));
        scene.stats.create_vertex_buffer_hr = device->CreateCommittedResource(
            &upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            void* mapped = nullptr;
            if (SUCCEEDED(scene.vertex_buffer->Map(0, nullptr, &mapped)) && mapped) {
                std::memcpy(mapped, dxil_overlay_triangle, sizeof(dxil_overlay_triangle));
                scene.vertex_buffer->Unmap(0, nullptr);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = sizeof(dxil_overlay_triangle);
            scene.vertex_view.StrideInBytes = sizeof(ColorVertex);
            scene.stats.vertices_per_draw = 3;
        }
    }

    if (root_blob)
        root_blob->Release();
    scene.stats.pass = scene.stats.vs_loaded && scene.stats.ps_loaded && scene.stats.vs_bytes > 0 &&
                       scene.stats.ps_bytes > 0 && SUCCEEDED(scene.stats.serialize_root_hr) &&
                       SUCCEEDED(scene.stats.create_root_hr) && SUCCEEDED(scene.stats.create_pso_hr) &&
                       SUCCEEDED(scene.stats.create_vertex_buffer_hr) && scene.vertex_buffer != nullptr &&
                       scene.stats.vertices_per_draw == 3;
    return scene;
}

static void destroy_dxil_scene(DxilSceneResources& scene) {
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct DxilHazardReplayEntryStats {
    std::string ps_path;
    bool ps_loaded = false;
    uint64_t ps_bytes = 0;
    uint64_t ps_fnv1a64 = 1469598103934665603ull;
    HRESULT create_pso_hr = E_FAIL;
    bool ok = false;
};

struct DxilHazardReplayStats {
    std::string proof_scope = "exact_elden_dxil_pixel_shader_pso_replay";
    std::string vs_path;
    bool vs_loaded = false;
    uint64_t vs_bytes = 0;
    uint64_t vs_fnv1a64 = 1469598103934665603ull;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    uint32_t requested_count = 0;
    uint32_t replay_count = 0;
    uint32_t success_count = 0;
    std::vector<DxilHazardReplayEntryStats> entries;
    bool pass = true;
};

static DxilHazardReplayStats create_dxil_hazard_replay(ID3D12Device* device, SerializeRootSignatureFn serialize,
                                                       const std::string& vs_path,
                                                       const std::vector<std::string>& ps_paths) {
    DxilHazardReplayStats stats;
    stats.vs_path = vs_path;
    std::vector<uint8_t> vs;
    stats.vs_loaded = read_file_bytes(vs_path, vs, stats.vs_fnv1a64);
    stats.vs_bytes = static_cast<uint64_t>(vs.size());

    std::array<D3D12_DESCRIPTOR_RANGE, 3> descriptor_ranges = {};
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    descriptor_ranges[0].NumDescriptors = 32;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].RegisterSpace = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[1].NumDescriptors = 128;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].RegisterSpace = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[2].NumDescriptors = 32;
    descriptor_ranges[2].BaseShaderRegister = 0;
    descriptor_ranges[2].RegisterSpace = 0;
    descriptor_ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    std::array<D3D12_ROOT_PARAMETER, 3> root_params = {};
    for (size_t range_index = 0; range_index < root_params.size(); ++range_index) {
        root_params[range_index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[range_index].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_params[range_index].DescriptorTable.NumDescriptorRanges = 1;
        root_params[range_index].DescriptorTable.pDescriptorRanges = &descriptor_ranges[range_index];
    }
    std::array<D3D12_STATIC_SAMPLER_DESC, 16> static_samplers = {};
    for (size_t sampler_index = 0; sampler_index < static_samplers.size(); ++sampler_index) {
        D3D12_STATIC_SAMPLER_DESC& sampler = static_samplers[sampler_index];
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = static_cast<UINT>(sampler_index);
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = static_cast<UINT>(root_params.size());
    root_desc.pParameters = root_params.data();
    root_desc.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
    root_desc.pStaticSamplers = static_samplers.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    ID3D12RootSignature* root_signature = nullptr;
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature));
    }

    stats.entries.resize(ps_paths.size());
    for (size_t i = 0; i < ps_paths.size(); ++i) {
        DxilHazardReplayEntryStats& entry = stats.entries[i];
        entry.ps_path = ps_paths[i];
        if (entry.ps_path.empty())
            continue;
        stats.requested_count++;
        std::vector<uint8_t> ps;
        entry.ps_loaded = read_file_bytes(entry.ps_path, ps, entry.ps_fnv1a64);
        entry.ps_bytes = static_cast<uint64_t>(ps.size());
        if (!(device && root_signature && stats.vs_loaded && entry.ps_loaded && !vs.empty() && !ps.empty()))
            continue;

        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root_signature;
        pso_desc.VS = {vs.data(), vs.size()};
        pso_desc.PS = {ps.data(), ps.size()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        ID3D12PipelineState* pso = nullptr;
        entry.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
        entry.ok = SUCCEEDED(entry.create_pso_hr) && pso != nullptr;
        stats.replay_count++;
        if (entry.ok)
            stats.success_count++;
        safe_release(pso);
    }

    if (root_blob)
        root_blob->Release();
    safe_release(root_signature);
    stats.pass = stats.requested_count == 0 ||
                 (stats.vs_loaded && SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                  stats.replay_count == stats.requested_count && stats.success_count == stats.requested_count);
    return stats;
}

struct DxilReadbackStats {
    HRESULT create_readback_hr = E_FAIL;
    uint32_t copy_commands = 0;
    uint32_t sentinel_writes = 0;
    uint32_t samples_checked = 0;
    uint32_t semantic_samples = 0;
    uint8_t expected_rgba[4] = {143, 128, 191, 191};
    uint8_t center_rgba[4] = {0, 0, 0, 0};
    bool pass = false;
};

struct DxilReadbackResources {
    ID3D12Resource* buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    DxilReadbackStats stats;
};

static DxilReadbackResources create_dxil_readback(ID3D12Device* device, UINT width, UINT height) {
    DxilReadbackResources readback;
    if (!device)
        return readback;
    D3D12_RESOURCE_DESC backbuffer_desc = texture_desc(width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    device->GetCopyableFootprints(&backbuffer_desc, 0, 1, 0, &readback.footprint, &readback.rows, &readback.row_bytes,
                                  &readback.total_bytes);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC buffer = buffer_desc(readback.total_bytes);
    readback.stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&readback.buffer));
    return readback;
}

static uint8_t* readback_pixel(DxilReadbackResources& readback, uint8_t* mapped, UINT x, UINT y) {
    return mapped + static_cast<size_t>(readback.footprint.Offset) +
           static_cast<size_t>(y) * readback.footprint.Footprint.RowPitch + static_cast<size_t>(x) * 4u;
}

static uint8_t* readback_center_pixel(DxilReadbackResources& readback, uint8_t* mapped, UINT width, UINT height) {
    return readback_pixel(readback, mapped, width / 2, height / 2);
}

static void heap_alias_expected_rgba(uint8_t out[4]);
static void uav_barrier_expected_rgba(UINT x, UINT y, uint8_t out[4]);
static void nanite_cluster_expected_rgba(uint8_t out[4]);
static void subresource_view_expected_rgba(UINT slice, uint8_t out[4]);
static void texture_array_srv_expected_rgba(UINT slice, uint8_t out[4]);
static void rtv_format_expected_rgba(uint8_t out[4]);
static void render_pass_expected_rgba(uint8_t out[4]);
static void corpus_shader_expected_rgba(uint8_t out[4]);
static void srv_sample_expected_rgba(uint8_t out[4]);
static void cbv_sample_expected_rgba(uint8_t out[4]);
static void indexed_draw_expected_rgba(uint8_t out[4]);
static void indirect_draw_expected_rgba(uint8_t out[4]);
static void wave_ops_expected_rgba(UINT x, UINT y, uint8_t out[4]);

static bool seed_dxil_readback_sentinel(DxilReadbackResources& readback, UINT width, UINT height,
                                        const uint8_t* texture_expected_rgba, uint32_t frame,
                                        const Textured3DStats* textured_3d_stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE read_range = {0, 0};
    if (FAILED(readback.buffer->Map(0, &read_range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    uint8_t* center = readback_center_pixel(readback, mapped, width, height);
    center[0] = 0;
    center[1] = 255;
    center[2] = 0;
    center[3] = 255;
    uint8_t* heap_alias_stamp = readback_pixel(readback, mapped, kHeapAliasStampX, kHeapAliasStampY);
    uint8_t heap_alias_expected[4] = {};
    heap_alias_expected_rgba(heap_alias_expected);
    heap_alias_stamp[0] = static_cast<uint8_t>(heap_alias_expected[0] ^ 0xffu);
    heap_alias_stamp[1] = static_cast<uint8_t>(heap_alias_expected[1] ^ 0xffu);
    heap_alias_stamp[2] = static_cast<uint8_t>(heap_alias_expected[2] ^ 0xffu);
    heap_alias_stamp[3] = static_cast<uint8_t>(heap_alias_expected[3] ^ 0xffu);
    uint8_t* uav_barrier_stamp = readback_pixel(readback, mapped, kUavBarrierStampX, kUavBarrierStampY);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* uav_pixel = readback_pixel(readback, mapped, kUavBarrierStampX + x, kUavBarrierStampY + y);
            uint8_t uav_barrier_expected[4] = {};
            uav_barrier_expected_rgba(x, y, uav_barrier_expected);
            uav_pixel[0] = static_cast<uint8_t>(uav_barrier_expected[0] ^ 0xffu);
            uav_pixel[1] = static_cast<uint8_t>(uav_barrier_expected[1] ^ 0xffu);
            uav_pixel[2] = static_cast<uint8_t>(uav_barrier_expected[2] ^ 0xffu);
            uav_pixel[3] = static_cast<uint8_t>(uav_barrier_expected[3] ^ 0xffu);
        }
    }
    uint8_t* wave_ops_stamp = readback_pixel(readback, mapped, kWaveOpsStampX, kWaveOpsStampY);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* wave_pixel = readback_pixel(readback, mapped, kWaveOpsStampX + x, kWaveOpsStampY + y);
            uint8_t wave_expected[4] = {};
            wave_ops_expected_rgba(x, y, wave_expected);
            wave_pixel[0] = static_cast<uint8_t>(wave_expected[0] ^ 0xffu);
            wave_pixel[1] = static_cast<uint8_t>(wave_expected[1] ^ 0xffu);
            wave_pixel[2] = static_cast<uint8_t>(wave_expected[2] ^ 0xffu);
            wave_pixel[3] = static_cast<uint8_t>(wave_expected[3] ^ 0xffu);
        }
    }
    uint8_t* rtv_format_stamp = readback_pixel(readback, mapped, kRtvFormatStampX, kRtvFormatStampY);
    uint8_t rtv_format_expected[4] = {};
    rtv_format_expected_rgba(rtv_format_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* rtv_pixel = readback_pixel(readback, mapped, kRtvFormatStampX + x, kRtvFormatStampY + y);
            rtv_pixel[0] = static_cast<uint8_t>(rtv_format_expected[0] ^ 0xffu);
            rtv_pixel[1] = static_cast<uint8_t>(rtv_format_expected[1] ^ 0xffu);
            rtv_pixel[2] = static_cast<uint8_t>(rtv_format_expected[2] ^ 0xffu);
            rtv_pixel[3] = static_cast<uint8_t>(rtv_format_expected[3] ^ 0xffu);
        }
    }
    uint8_t* render_pass_stamp = readback_pixel(readback, mapped, kRenderPassStampX, kRenderPassStampY);
    uint8_t render_pass_expected[4] = {};
    render_pass_expected_rgba(render_pass_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* render_pass_pixel = readback_pixel(readback, mapped, kRenderPassStampX + x, kRenderPassStampY + y);
            render_pass_pixel[0] = static_cast<uint8_t>(render_pass_expected[0] ^ 0xffu);
            render_pass_pixel[1] = static_cast<uint8_t>(render_pass_expected[1] ^ 0xffu);
            render_pass_pixel[2] = static_cast<uint8_t>(render_pass_expected[2] ^ 0xffu);
            render_pass_pixel[3] = static_cast<uint8_t>(render_pass_expected[3] ^ 0xffu);
        }
    }
    uint8_t* corpus_shader_stamp = readback_pixel(readback, mapped, kCorpusShaderStampX, kCorpusShaderStampY);
    uint8_t corpus_shader_expected[4] = {};
    corpus_shader_expected_rgba(corpus_shader_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* corpus_pixel = readback_pixel(readback, mapped, kCorpusShaderStampX + x, kCorpusShaderStampY + y);
            corpus_pixel[0] = static_cast<uint8_t>(corpus_shader_expected[0] ^ 0xffu);
            corpus_pixel[1] = static_cast<uint8_t>(corpus_shader_expected[1] ^ 0xffu);
            corpus_pixel[2] = static_cast<uint8_t>(corpus_shader_expected[2] ^ 0xffu);
            corpus_pixel[3] = static_cast<uint8_t>(corpus_shader_expected[3] ^ 0xffu);
        }
    }
    uint8_t* srv_sample_stamp = readback_pixel(readback, mapped, kSrvSampleStampX, kSrvSampleStampY);
    uint8_t srv_sample_expected[4] = {};
    srv_sample_expected_rgba(srv_sample_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* srv_pixel = readback_pixel(readback, mapped, kSrvSampleStampX + x, kSrvSampleStampY + y);
            srv_pixel[0] = static_cast<uint8_t>(srv_sample_expected[0] ^ 0xffu);
            srv_pixel[1] = static_cast<uint8_t>(srv_sample_expected[1] ^ 0xffu);
            srv_pixel[2] = static_cast<uint8_t>(srv_sample_expected[2] ^ 0xffu);
            srv_pixel[3] = static_cast<uint8_t>(srv_sample_expected[3] ^ 0xffu);
        }
    }
    uint8_t* cbv_stamp = readback_pixel(readback, mapped, kCbvStampX, kCbvStampY);
    uint8_t cbv_expected[4] = {};
    cbv_sample_expected_rgba(cbv_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* cbv_pixel = readback_pixel(readback, mapped, kCbvStampX + x, kCbvStampY + y);
            cbv_pixel[0] = static_cast<uint8_t>(cbv_expected[0] ^ 0xffu);
            cbv_pixel[1] = static_cast<uint8_t>(cbv_expected[1] ^ 0xffu);
            cbv_pixel[2] = static_cast<uint8_t>(cbv_expected[2] ^ 0xffu);
            cbv_pixel[3] = static_cast<uint8_t>(cbv_expected[3] ^ 0xffu);
        }
    }
    uint8_t* indexed_stamp = readback_pixel(readback, mapped, kIndexedStampX, kIndexedStampY);
    uint8_t indexed_expected[4] = {};
    indexed_draw_expected_rgba(indexed_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* indexed_pixel = readback_pixel(readback, mapped, kIndexedStampX + x, kIndexedStampY + y);
            indexed_pixel[0] = static_cast<uint8_t>(indexed_expected[0] ^ 0xffu);
            indexed_pixel[1] = static_cast<uint8_t>(indexed_expected[1] ^ 0xffu);
            indexed_pixel[2] = static_cast<uint8_t>(indexed_expected[2] ^ 0xffu);
            indexed_pixel[3] = static_cast<uint8_t>(indexed_expected[3] ^ 0xffu);
        }
    }
    uint8_t* indirect_stamp = readback_pixel(readback, mapped, kIndirectStampX, kIndirectStampY);
    uint8_t indirect_expected[4] = {};
    indirect_draw_expected_rgba(indirect_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* indirect_pixel = readback_pixel(readback, mapped, kIndirectStampX + x, kIndirectStampY + y);
            indirect_pixel[0] = static_cast<uint8_t>(indirect_expected[0] ^ 0xffu);
            indirect_pixel[1] = static_cast<uint8_t>(indirect_expected[1] ^ 0xffu);
            indirect_pixel[2] = static_cast<uint8_t>(indirect_expected[2] ^ 0xffu);
            indirect_pixel[3] = static_cast<uint8_t>(indirect_expected[3] ^ 0xffu);
        }
    }
    uint8_t* nanite_stamp = readback_pixel(readback, mapped, kNaniteClusterStampX, kNaniteClusterStampY);
    uint8_t nanite_expected[4] = {};
    nanite_cluster_expected_rgba(nanite_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* nanite_pixel =
                readback_pixel(readback, mapped, kNaniteClusterStampX + x, kNaniteClusterStampY + y);
            nanite_pixel[0] = static_cast<uint8_t>(nanite_expected[0] ^ 0xffu);
            nanite_pixel[1] = static_cast<uint8_t>(nanite_expected[1] ^ 0xffu);
            nanite_pixel[2] = static_cast<uint8_t>(nanite_expected[2] ^ 0xffu);
            nanite_pixel[3] = static_cast<uint8_t>(nanite_expected[3] ^ 0xffu);
        }
    }
    uint8_t* subresource_view_stamp = readback_pixel(readback, mapped, kSubresourceViewStampX, kSubresourceViewStampY);
    uint8_t subresource_view_expected[4] = {};
    subresource_view_expected_rgba(1, subresource_view_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* subresource_pixel =
                readback_pixel(readback, mapped, kSubresourceViewStampX + x, kSubresourceViewStampY + y);
            subresource_pixel[0] = static_cast<uint8_t>(subresource_view_expected[0] ^ 0xffu);
            subresource_pixel[1] = static_cast<uint8_t>(subresource_view_expected[1] ^ 0xffu);
            subresource_pixel[2] = static_cast<uint8_t>(subresource_view_expected[2] ^ 0xffu);
            subresource_pixel[3] = static_cast<uint8_t>(subresource_view_expected[3] ^ 0xffu);
        }
    }
    uint8_t* texture_array_srv_stamp = readback_pixel(readback, mapped, kTextureArraySrvStampX, kTextureArraySrvStampY);
    uint8_t texture_array_srv_expected[4] = {};
    texture_array_srv_expected_rgba(1, texture_array_srv_expected);
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            uint8_t* texture_array_pixel =
                readback_pixel(readback, mapped, kTextureArraySrvStampX + x, kTextureArraySrvStampY + y);
            texture_array_pixel[0] = static_cast<uint8_t>(texture_array_srv_expected[0] ^ 0xffu);
            texture_array_pixel[1] = static_cast<uint8_t>(texture_array_srv_expected[1] ^ 0xffu);
            texture_array_pixel[2] = static_cast<uint8_t>(texture_array_srv_expected[2] ^ 0xffu);
            texture_array_pixel[3] = static_cast<uint8_t>(texture_array_srv_expected[3] ^ 0xffu);
        }
    }
    uint8_t* sm5_stamp = readback_pixel(readback, mapped, kSm5StampX, kSm5StampY);
    uint8_t sm5_expected[4] = {};
    sm5_expected_stamp_rgba(frame, sm5_expected);
    sm5_stamp[0] = static_cast<uint8_t>(sm5_expected[0] ^ 0xffu);
    sm5_stamp[1] = static_cast<uint8_t>(sm5_expected[1] ^ 0xffu);
    sm5_stamp[2] = static_cast<uint8_t>(sm5_expected[2] ^ 0xffu);
    sm5_stamp[3] = static_cast<uint8_t>(sm5_expected[3] ^ 0xffu);
    uint8_t* stamp = readback_pixel(readback, mapped, kTextureStampX, kTextureStampY);
    if (texture_expected_rgba) {
        stamp[0] = static_cast<uint8_t>(texture_expected_rgba[0] ^ 0xffu);
        stamp[1] = static_cast<uint8_t>(texture_expected_rgba[1] ^ 0xffu);
        stamp[2] = static_cast<uint8_t>(texture_expected_rgba[2] ^ 0xffu);
        stamp[3] = static_cast<uint8_t>(texture_expected_rgba[3] ^ 0xffu);
    } else {
        stamp[0] = 0xaa;
        stamp[1] = 0x55;
        stamp[2] = 0x00;
        stamp[3] = 0xff;
    }
    uint8_t* textured_3d_face_stamps[kTextured3DFaceCount] = {};
    bool textured_3d_face_stamp_valid[kTextured3DFaceCount] = {};
    if (textured_3d_stats && textured_3d_stats->face_count == kTextured3DFaceCount) {
        for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
            const Textured3DFaceStats& face_stats = textured_3d_stats->faces[face];
            textured_3d_face_stamps[face] = readback_pixel(readback, mapped, face_stats.sample_x, face_stats.sample_y);
            textured_3d_face_stamps[face][0] = static_cast<uint8_t>(face_stats.expected_rgba[0] ^ 0xffu);
            textured_3d_face_stamps[face][1] = static_cast<uint8_t>(face_stats.expected_rgba[1] ^ 0xffu);
            textured_3d_face_stamps[face][2] = static_cast<uint8_t>(face_stats.expected_rgba[2] ^ 0xffu);
            textured_3d_face_stamps[face][3] = static_cast<uint8_t>(face_stats.expected_rgba[3] ^ 0xffu);
            textured_3d_face_stamp_valid[face] = true;
        }
    }
    uint8_t* textured_3d_depth_overlap_stamp = nullptr;
    if (textured_3d_stats) {
        textured_3d_depth_overlap_stamp = readback_pixel(readback, mapped, textured_3d_stats->depth_overlap_sample_x,
                                                         textured_3d_stats->depth_overlap_sample_y);
        textured_3d_depth_overlap_stamp[0] =
            static_cast<uint8_t>(textured_3d_stats->depth_overlap_expected_rgba[0] ^ 0xffu);
        textured_3d_depth_overlap_stamp[1] =
            static_cast<uint8_t>(textured_3d_stats->depth_overlap_expected_rgba[1] ^ 0xffu);
        textured_3d_depth_overlap_stamp[2] =
            static_cast<uint8_t>(textured_3d_stats->depth_overlap_expected_rgba[2] ^ 0xffu);
        textured_3d_depth_overlap_stamp[3] =
            static_cast<uint8_t>(textured_3d_stats->depth_overlap_expected_rgba[3] ^ 0xffu);
    }
    const size_t center_offset = static_cast<size_t>(center - mapped);
    const size_t heap_alias_stamp_offset = static_cast<size_t>(heap_alias_stamp - mapped);
    const size_t uav_barrier_stamp_offset = static_cast<size_t>(uav_barrier_stamp - mapped);
    const uint8_t* uav_barrier_stamp_last = readback_pixel(
        readback, mapped, kUavBarrierStampX + kFreshTextureWidth - 1u, kUavBarrierStampY + kFreshTextureHeight - 1u);
    const size_t uav_barrier_stamp_last_offset = static_cast<size_t>(uav_barrier_stamp_last - mapped);
    const size_t wave_ops_stamp_offset = static_cast<size_t>(wave_ops_stamp - mapped);
    const uint8_t* wave_ops_stamp_last = readback_pixel(readback, mapped, kWaveOpsStampX + kFreshTextureWidth - 1u,
                                                        kWaveOpsStampY + kFreshTextureHeight - 1u);
    const size_t wave_ops_stamp_last_offset = static_cast<size_t>(wave_ops_stamp_last - mapped);
    const size_t rtv_format_stamp_offset = static_cast<size_t>(rtv_format_stamp - mapped);
    const uint8_t* rtv_format_stamp_last = readback_pixel(readback, mapped, kRtvFormatStampX + kFreshTextureWidth - 1u,
                                                          kRtvFormatStampY + kFreshTextureHeight - 1u);
    const size_t rtv_format_stamp_last_offset = static_cast<size_t>(rtv_format_stamp_last - mapped);
    const size_t render_pass_stamp_offset = static_cast<size_t>(render_pass_stamp - mapped);
    const uint8_t* render_pass_stamp_last = readback_pixel(
        readback, mapped, kRenderPassStampX + kFreshTextureWidth - 1u, kRenderPassStampY + kFreshTextureHeight - 1u);
    const size_t render_pass_stamp_last_offset = static_cast<size_t>(render_pass_stamp_last - mapped);
    const size_t corpus_shader_stamp_offset = static_cast<size_t>(corpus_shader_stamp - mapped);
    const uint8_t* corpus_shader_stamp_last =
        readback_pixel(readback, mapped, kCorpusShaderStampX + kFreshTextureWidth - 1u,
                       kCorpusShaderStampY + kFreshTextureHeight - 1u);
    const size_t corpus_shader_stamp_last_offset = static_cast<size_t>(corpus_shader_stamp_last - mapped);
    const size_t srv_sample_stamp_offset = static_cast<size_t>(srv_sample_stamp - mapped);
    const uint8_t* srv_sample_stamp_last = readback_pixel(readback, mapped, kSrvSampleStampX + kFreshTextureWidth - 1u,
                                                          kSrvSampleStampY + kFreshTextureHeight - 1u);
    const size_t srv_sample_stamp_last_offset = static_cast<size_t>(srv_sample_stamp_last - mapped);
    const size_t cbv_stamp_offset = static_cast<size_t>(cbv_stamp - mapped);
    const uint8_t* cbv_stamp_last =
        readback_pixel(readback, mapped, kCbvStampX + kFreshTextureWidth - 1u, kCbvStampY + kFreshTextureHeight - 1u);
    const size_t cbv_stamp_last_offset = static_cast<size_t>(cbv_stamp_last - mapped);
    const size_t indexed_stamp_offset = static_cast<size_t>(indexed_stamp - mapped);
    const uint8_t* indexed_stamp_last = readback_pixel(readback, mapped, kIndexedStampX + kFreshTextureWidth - 1u,
                                                       kIndexedStampY + kFreshTextureHeight - 1u);
    const size_t indexed_stamp_last_offset = static_cast<size_t>(indexed_stamp_last - mapped);
    const size_t indirect_stamp_offset = static_cast<size_t>(indirect_stamp - mapped);
    const uint8_t* indirect_stamp_last = readback_pixel(readback, mapped, kIndirectStampX + kFreshTextureWidth - 1u,
                                                        kIndirectStampY + kFreshTextureHeight - 1u);
    const size_t indirect_stamp_last_offset = static_cast<size_t>(indirect_stamp_last - mapped);
    const size_t nanite_stamp_offset = static_cast<size_t>(nanite_stamp - mapped);
    const uint8_t* nanite_stamp_last = readback_pixel(readback, mapped, kNaniteClusterStampX + kFreshTextureWidth - 1u,
                                                      kNaniteClusterStampY + kFreshTextureHeight - 1u);
    const size_t nanite_stamp_last_offset = static_cast<size_t>(nanite_stamp_last - mapped);
    const size_t subresource_view_stamp_offset = static_cast<size_t>(subresource_view_stamp - mapped);
    const uint8_t* subresource_view_stamp_last =
        readback_pixel(readback, mapped, kSubresourceViewStampX + kFreshTextureWidth - 1u,
                       kSubresourceViewStampY + kFreshTextureHeight - 1u);
    const size_t subresource_view_stamp_last_offset = static_cast<size_t>(subresource_view_stamp_last - mapped);
    const size_t texture_array_srv_stamp_offset = static_cast<size_t>(texture_array_srv_stamp - mapped);
    const uint8_t* texture_array_srv_stamp_last =
        readback_pixel(readback, mapped, kTextureArraySrvStampX + kFreshTextureWidth - 1u,
                       kTextureArraySrvStampY + kFreshTextureHeight - 1u);
    const size_t texture_array_srv_stamp_last_offset = static_cast<size_t>(texture_array_srv_stamp_last - mapped);
    const size_t sm5_stamp_offset = static_cast<size_t>(sm5_stamp - mapped);
    const size_t stamp_offset = static_cast<size_t>(stamp - mapped);
    size_t min_written_offset = std::min({center_offset,
                                          heap_alias_stamp_offset,
                                          uav_barrier_stamp_offset,
                                          uav_barrier_stamp_last_offset,
                                          wave_ops_stamp_offset,
                                          wave_ops_stamp_last_offset,
                                          rtv_format_stamp_offset,
                                          rtv_format_stamp_last_offset,
                                          render_pass_stamp_offset,
                                          render_pass_stamp_last_offset,
                                          corpus_shader_stamp_offset,
                                          corpus_shader_stamp_last_offset,
                                          srv_sample_stamp_offset,
                                          srv_sample_stamp_last_offset,
                                          cbv_stamp_offset,
                                          cbv_stamp_last_offset,
                                          indexed_stamp_offset,
                                          indexed_stamp_last_offset,
                                          indirect_stamp_offset,
                                          indirect_stamp_last_offset,
                                          nanite_stamp_offset,
                                          nanite_stamp_last_offset,
                                          subresource_view_stamp_offset,
                                          subresource_view_stamp_last_offset,
                                          texture_array_srv_stamp_offset,
                                          texture_array_srv_stamp_last_offset,
                                          sm5_stamp_offset,
                                          stamp_offset});
    size_t max_written_offset = std::max({center_offset,
                                          heap_alias_stamp_offset,
                                          uav_barrier_stamp_offset,
                                          uav_barrier_stamp_last_offset,
                                          wave_ops_stamp_offset,
                                          wave_ops_stamp_last_offset,
                                          rtv_format_stamp_offset,
                                          rtv_format_stamp_last_offset,
                                          render_pass_stamp_offset,
                                          render_pass_stamp_last_offset,
                                          corpus_shader_stamp_offset,
                                          corpus_shader_stamp_last_offset,
                                          srv_sample_stamp_offset,
                                          srv_sample_stamp_last_offset,
                                          cbv_stamp_offset,
                                          cbv_stamp_last_offset,
                                          indexed_stamp_offset,
                                          indexed_stamp_last_offset,
                                          indirect_stamp_offset,
                                          indirect_stamp_last_offset,
                                          nanite_stamp_offset,
                                          nanite_stamp_last_offset,
                                          subresource_view_stamp_offset,
                                          subresource_view_stamp_last_offset,
                                          texture_array_srv_stamp_offset,
                                          texture_array_srv_stamp_last_offset,
                                          sm5_stamp_offset,
                                          stamp_offset});
    for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
        if (!textured_3d_face_stamp_valid[face])
            continue;
        const size_t face_offset = static_cast<size_t>(textured_3d_face_stamps[face] - mapped);
        min_written_offset = std::min(min_written_offset, face_offset);
        max_written_offset = std::max(max_written_offset, face_offset);
    }
    if (textured_3d_depth_overlap_stamp) {
        const size_t depth_overlap_offset = static_cast<size_t>(textured_3d_depth_overlap_stamp - mapped);
        min_written_offset = std::min(min_written_offset, depth_overlap_offset);
        max_written_offset = std::max(max_written_offset, depth_overlap_offset);
    }
    D3D12_RANGE written = {min_written_offset, max_written_offset + 4u};
    readback.buffer->Unmap(0, &written);
    readback.stats.sentinel_writes++;
    return true;
}

static bool inspect_dxil_scalar_vector_center(DxilReadbackResources& readback, UINT width, UINT height) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* pixel = readback_center_pixel(readback, mapped, width, height);
    readback.stats.center_rgba[0] = pixel[0];
    readback.stats.center_rgba[1] = pixel[1];
    readback.stats.center_rgba[2] = pixel[2];
    readback.stats.center_rgba[3] = pixel[3];
    readback.stats.samples_checked++;
    bool semantic_match = true;
    for (UINT channel = 0; channel < 4; ++channel) {
        const int delta = static_cast<int>(pixel[channel]) - static_cast<int>(readback.stats.expected_rgba[channel]);
        if (delta < -2 || delta > 2)
            semantic_match = false;
    }
    if (semantic_match)
        readback.stats.semantic_samples++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return semantic_match;
}

static void destroy_dxil_readback(DxilReadbackResources& readback) {
    safe_release(readback.buffer);
}

struct GpuTextureStats {
    HRESULT create_descriptor_heap_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    uint32_t textures_requested = 0;
    uint32_t textures_created = 0;
    uint32_t upload_buffers_created = 0;
    uint32_t srv_descriptors_created = 0;
    uint32_t copy_texture_region_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t texture_payloads_uploaded = 0;
    uint32_t present_backbuffer_sentinel_copies = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint8_t present_expected_rgba[4] = {0, 0, 0, 0};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint64_t upload_bytes = 0;
    uint64_t texture_payload_bytes_from_files = 0;
    uint64_t upload_payload_fnv1a64 = 1469598103934665603ull;
    bool fence_wait_ok = false;
    bool present_pass = false;
    bool pass = false;
};

static bool fill_texture_upload(ID3D12Resource* upload, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                const TexturePayload& payload, UINT width, UINT height) {
    uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const size_t row_pitch = static_cast<size_t>(footprint.Footprint.RowPitch);
    const size_t tight_row_pitch = static_cast<size_t>(width) * 4u;
    uint8_t* base = mapped + static_cast<size_t>(footprint.Offset);
    for (UINT y = 0; y < height; ++y) {
        uint8_t* row = base + static_cast<size_t>(y) * row_pitch;
        const uint8_t* source = payload.rgba.data() + static_cast<size_t>(y) * tight_row_pitch;
        std::memcpy(row, source, tight_row_pitch);
    }
    upload->Unmap(0, nullptr);
    return true;
}

struct GpuTextureExercise {
    GpuTextureStats stats;
    ID3D12Resource* present_texture = nullptr;
    ID3D12Resource* present_sentinel_upload = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT present_sentinel_footprint = {};
};

struct HeapAliasStats {
    HRESULT create_heap_hr = E_FAIL;
    HRESULT create_placed_a_hr = E_FAIL;
    HRESULT create_placed_b_hr = E_FAIL;
    HRESULT create_upload_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT map_upload_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 heap_size = 0;
    UINT64 alias_bytes = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address_a = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_virtual_address_b = 0;
    uint32_t copy_before_alias_commands = 0;
    uint32_t aliasing_barriers = 0;
    uint32_t copy_alias_overlap_commands = 0;
    uint32_t copy_after_alias_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint8_t present_expected_rgba[4] = {64, 240, 128, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    bool fence_wait_ok = false;
    bool gpu_virtual_addresses_match = false;
    bool readback_before_alias_ok = false;
    bool readback_alias_overlap_ok = false;
    bool readback_after_alias_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct HeapAliasExercise {
    HeapAliasStats stats;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* present_buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
};

static void heap_alias_expected_rgba(uint8_t out[4]) {
    out[0] = 64;
    out[1] = 240;
    out[2] = 128;
    out[3] = 255;
}

static void fill_alias_upload_footprint(uint8_t* base, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                        const uint8_t rgba[4]) {
    for (UINT y = 0; y < footprint.Footprint.Height; ++y) {
        uint8_t* row = base + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < footprint.Footprint.Width; ++x) {
            uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
            pixel[0] = rgba[0];
            pixel[1] = rgba[1];
            pixel[2] = rgba[2];
            pixel[3] = rgba[3];
        }
    }
}

static bool verify_alias_footprint(const uint8_t* base, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                   const uint8_t rgba[4]) {
    for (UINT y = 0; y < footprint.Footprint.Height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < footprint.Footprint.Width; ++x) {
            const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
            if (pixel[0] != rgba[0] || pixel[1] != rgba[1] || pixel[2] != rgba[2] || pixel[3] != rgba[3])
                return false;
        }
    }
    return true;
}

static HeapAliasExercise exercise_heap_alias_stamp(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                   ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
                                                   ID3D12Fence* fence, HANDLE fence_event, UINT64& fence_value) {
    HeapAliasExercise exercise;
    HeapAliasStats& stats = exercise.stats;
    if (!device || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    D3D12_RESOURCE_DESC stamp_desc = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 alias_bytes = 0;
    device->GetCopyableFootprints(&stamp_desc, 0, 1, 0, &exercise.footprint, &rows, &row_bytes, &alias_bytes);
    stats.alias_bytes = alias_bytes;
    D3D12_RESOURCE_DESC buffer = buffer_desc(alias_bytes);
    D3D12_RESOURCE_ALLOCATION_INFO alloc_info = device->GetResourceAllocationInfo(0, 1, &buffer);

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = alloc_info.SizeInBytes ? alloc_info.SizeInBytes : alias_bytes;
    heap_desc.Properties = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    heap_desc.Alignment = alloc_info.Alignment;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    stats.heap_size = heap_desc.SizeInBytes;
    stats.create_heap_hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&exercise.heap));
    ID3D12Resource* placed_a = nullptr;
    ID3D12Resource* placed_b = nullptr;
    stats.create_placed_a_hr =
        exercise.heap ? device->CreatePlacedResource(exercise.heap, 0, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&placed_a))
                      : E_FAIL;
    stats.create_placed_b_hr =
        exercise.heap ? device->CreatePlacedResource(exercise.heap, 0, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&placed_b))
                      : E_FAIL;

    if (placed_a && placed_b) {
        stats.gpu_virtual_address_a = placed_a->GetGPUVirtualAddress();
        stats.gpu_virtual_address_b = placed_b->GetGPUVirtualAddress();
        stats.gpu_virtual_addresses_match =
            stats.gpu_virtual_address_a != 0 && stats.gpu_virtual_address_a == stats.gpu_virtual_address_b;
    }

    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC upload_desc = buffer_desc(alias_bytes * 2u);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(alias_bytes * 3u);
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
    stats.create_upload_hr =
        device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    uint8_t expected_a[4] = {12, 34, 56, 255};
    uint8_t expected_b[4] = {};
    heap_alias_expected_rgba(expected_b);
    uint8_t* upload_ptr = nullptr;
    stats.map_upload_hr = upload ? upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr)) : E_FAIL;
    if (SUCCEEDED(stats.map_upload_hr) && upload_ptr) {
        fill_alias_upload_footprint(upload_ptr, exercise.footprint, expected_a);
        fill_alias_upload_footprint(upload_ptr + static_cast<size_t>(alias_bytes), exercise.footprint, expected_b);
        upload->Unmap(0, nullptr);
    }

    if (placed_a && placed_b && upload && readback && SUCCEEDED(allocator->Reset()) &&
        SUCCEEDED(list->Reset(allocator, nullptr))) {
        list->CopyBufferRegion(placed_a, 0, upload, 0, alias_bytes);
        stats.copy_before_alias_commands++;
        D3D12_RESOURCE_BARRIER a_to_copy_source =
            transition_barrier(placed_a, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &a_to_copy_source);
        stats.transition_barriers++;
        list->CopyBufferRegion(readback, 0, placed_a, 0, alias_bytes);

        D3D12_RESOURCE_BARRIER alias = aliasing_barrier(placed_a, placed_b);
        list->ResourceBarrier(1, &alias);
        stats.aliasing_barriers++;

        D3D12_RESOURCE_BARRIER b_overlap_to_copy_source =
            transition_barrier(placed_b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b_overlap_to_copy_source);
        stats.transition_barriers++;
        list->CopyBufferRegion(readback, alias_bytes, placed_b, 0, alias_bytes);
        stats.copy_alias_overlap_commands++;
        D3D12_RESOURCE_BARRIER b_overlap_to_copy_dest =
            transition_barrier(placed_b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b_overlap_to_copy_dest);
        stats.transition_barriers++;

        list->CopyBufferRegion(placed_b, 0, upload, alias_bytes, alias_bytes);
        stats.copy_after_alias_commands++;
        D3D12_RESOURCE_BARRIER b_to_copy_source =
            transition_barrier(placed_b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &b_to_copy_source);
        stats.transition_barriers++;
        list->CopyBufferRegion(readback, alias_bytes * 2u, placed_b, 0, alias_bytes);
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }

    if (readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(alias_bytes * 3u)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            stats.readback_before_alias_ok = verify_alias_footprint(mapped, exercise.footprint, expected_a);
            stats.readback_alias_overlap_ok =
                verify_alias_footprint(mapped + static_cast<size_t>(alias_bytes), exercise.footprint, expected_a);
            stats.readback_after_alias_ok =
                verify_alias_footprint(mapped + static_cast<size_t>(alias_bytes * 2u), exercise.footprint, expected_b);
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = SUCCEEDED(stats.create_heap_hr) && SUCCEEDED(stats.create_placed_a_hr) &&
                 SUCCEEDED(stats.create_placed_b_hr) && SUCCEEDED(stats.create_upload_hr) &&
                 SUCCEEDED(stats.create_readback_hr) && SUCCEEDED(stats.map_upload_hr) && stats.alias_bytes > 0 &&
                 stats.gpu_virtual_addresses_match && stats.copy_before_alias_commands == 1 &&
                 stats.aliasing_barriers == 1 && stats.copy_alias_overlap_commands == 1 &&
                 stats.copy_after_alias_commands == 1 && stats.transition_barriers == 4 && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && SUCCEEDED(stats.map_readback_hr) &&
                 stats.readback_before_alias_ok && stats.readback_alias_overlap_ok && stats.readback_after_alias_ok;
    if (stats.pass) {
        exercise.present_buffer = placed_b;
        placed_b = nullptr;
    }
    safe_release(placed_a);
    safe_release(placed_b);
    safe_release(upload);
    safe_release(readback);
    return exercise;
}

static void destroy_heap_alias_exercise(HeapAliasExercise& exercise) {
    safe_release(exercise.present_buffer);
    safe_release(exercise.heap);
}

struct WaveOpsStats {
    std::string cs_path;
    bool cs_loaded = false;
    uint64_t cs_bytes = 0;
    uint64_t cs_fnv1a64 = 1469598103934665603ull;
    HRESULT options1_hr = E_FAIL;
    bool wave_ops_reported = false;
    UINT wave_lane_count_min = 0;
    UINT wave_lane_count_max = 0;
    UINT total_lane_count = 0;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_uav_buffer_hr = E_FAIL;
    HRESULT create_uav_descriptor_heap_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT row_pitch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS uav_gpu_virtual_address = 0;
    UINT64 uav_descriptor_gpu_handle = 0;
    bool fixed_footprint_ok = false;
    uint32_t root_uav_sets = 0;
    uint32_t uav_descriptors_created = 0;
    uint32_t dispatch_commands = 0;
    uint32_t dispatch_groups_x = 0;
    uint32_t dispatch_groups_y = 0;
    uint32_t dispatch_groups_z = 0;
    uint32_t uav_barriers = 0;
    uint32_t transition_barriers = 0;
    uint32_t compute_pixels_checked = 0;
    uint32_t compute_pixel_matches = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t compute_first_rgba[4] = {0, 0, 0, 0};
    uint8_t compute_center_rgba[4] = {0, 0, 0, 0};
    uint8_t compute_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_first_rgba[4] = {0, 0, 0, 0};
    uint8_t present_center_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fence_wait_ok = false;
    bool compute_readback_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct WaveOpsExercise {
    WaveOpsStats stats;
    ID3D12Resource* present_buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
};

static void wave_ops_expected_rgba(UINT x, UINT y, uint8_t out[4]) {
    const UINT lane = ((y & 1u) * kFreshTextureWidth) + x;
    out[0] = static_cast<uint8_t>(lane); // WaveGetLaneIndex
    out[1] = 32;                         // WaveGetLaneCount
    out[2] = 5;                          // WaveReadLaneFirst(WaveGetLaneIndex + 5)
    out[3] = 14;                         // WaveReadLaneAt(lane + 5, 7) + AnyTrue(first lane) + false/true AllTrue cases
}

static WaveOpsExercise exercise_wave_ops_stamp(ID3D12Device* device, SerializeRootSignatureFn serialize,
                                               ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator,
                                               ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fence_event,
                                               UINT64& fence_value, const std::string& wave_cs_path) {
    WaveOpsExercise exercise;
    WaveOpsStats& stats = exercise.stats;
    stats.cs_path = wave_cs_path;
    if (!device || !serialize || !queue || !allocator || !list || !fence || !fence_event || wave_cs_path.empty())
        return exercise;

    std::vector<uint8_t> cs;
    stats.cs_loaded = read_file_bytes(wave_cs_path, cs, stats.cs_fnv1a64);
    stats.cs_bytes = static_cast<uint64_t>(cs.size());

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
    stats.options1_hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1));
    if (SUCCEEDED(stats.options1_hr)) {
        stats.wave_ops_reported = options1.WaveOps;
        stats.wave_lane_count_min = options1.WaveLaneCountMin;
        stats.wave_lane_count_max = options1.WaveLaneCountMax;
        stats.total_lane_count = options1.TotalLaneCount;
    }

    D3D12_RESOURCE_DESC stamp_desc = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&stamp_desc, 0, 1, 0, &exercise.footprint, &rows, &row_bytes, &total_bytes);
    stats.footprint_bytes = total_bytes;
    stats.row_pitch = exercise.footprint.Footprint.RowPitch;
    stats.fixed_footprint_ok =
        stats.row_pitch == kFreshTextureWidth * 16u && total_bytes >= kFreshTextureHeight * stats.row_pitch;
    stats.dispatch_groups_x = 8;
    stats.dispatch_groups_y = 1;
    stats.dispatch_groups_z = 1;

    D3D12_DESCRIPTOR_RANGE uav_range = {};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = 1;
    uav_range.BaseShaderRegister = 0;
    uav_range.RegisterSpace = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER root_params[1] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_params[0].DescriptorTable.NumDescriptorRanges = 1;
    root_params[0].DescriptorTable.pDescriptorRanges = &uav_range;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = root_params;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);

    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    if (root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature));
    }
    if (root_signature && stats.cs_loaded && !cs.empty()) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root_signature;
        pso_desc.CS = {cs.data(), cs.size()};
        stats.create_pso_hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state));
    }

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC uav_desc = stamp_desc;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* uav_heap = nullptr;
    stats.create_uav_buffer_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                 IID_PPV_ARGS(&exercise.present_buffer));
    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc = {};
    uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_desc.NumDescriptors = 1;
    uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    stats.create_uav_descriptor_heap_hr = device->CreateDescriptorHeap(&uav_heap_desc, IID_PPV_ARGS(&uav_heap));
    if (exercise.present_buffer && uav_heap) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view = {};
        uav_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav_view.Texture2D.MipSlice = 0;
        uav_view.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(exercise.present_buffer, nullptr, &uav_view,
                                          uav_heap->GetCPUDescriptorHandleForHeapStart());
        stats.uav_descriptors_created++;
    }
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (uav_heap)
        stats.uav_descriptor_gpu_handle = uav_heap->GetGPUDescriptorHandleForHeapStart().ptr;

    bool submitted_work = false;
    if (stats.fixed_footprint_ok && pipeline_state && root_signature && exercise.present_buffer && uav_heap &&
        readback && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr))) {
        list->SetPipelineState(pipeline_state);
        list->SetComputeRootSignature(root_signature);
        ID3D12DescriptorHeap* heaps[] = {uav_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootDescriptorTable(0, uav_heap->GetGPUDescriptorHandleForHeapStart());
        stats.root_uav_sets++;
        list->Dispatch(stats.dispatch_groups_x, stats.dispatch_groups_y, stats.dispatch_groups_z);
        stats.dispatch_commands++;
        D3D12_RESOURCE_BARRIER uav = uav_resource_barrier(exercise.present_buffer);
        list->ResourceBarrier(1, &uav);
        stats.uav_barriers++;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            exercise.present_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        stats.transition_barriers++;
        D3D12_TEXTURE_COPY_LOCATION readback_dst = {};
        readback_dst.pResource = readback;
        readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readback_dst.PlacedFootprint = exercise.footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = exercise.present_buffer;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        texture_src.SubresourceIndex = 0;
        list->CopyTextureRegion(&readback_dst, 0, 0, 0, &texture_src, nullptr);
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(total_bytes)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            const uint8_t* base = mapped + static_cast<size_t>(exercise.footprint.Offset);
            const uint8_t* first = base;
            std::memcpy(stats.compute_first_rgba, first, sizeof(stats.compute_first_rgba));
            const uint8_t* center =
                base + static_cast<size_t>(kFreshTextureHeight / 2u) * exercise.footprint.Footprint.RowPitch +
                static_cast<size_t>(kFreshTextureWidth / 2u) * 4u;
            std::memcpy(stats.compute_center_rgba, center, sizeof(stats.compute_center_rgba));
            const uint8_t* last =
                base + static_cast<size_t>(kFreshTextureHeight - 1u) * exercise.footprint.Footprint.RowPitch +
                static_cast<size_t>(kFreshTextureWidth - 1u) * 4u;
            std::memcpy(stats.compute_last_rgba, last, sizeof(stats.compute_last_rgba));
            for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                const uint8_t* row = base + static_cast<size_t>(y) * exercise.footprint.Footprint.RowPitch;
                for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                    const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                    uint8_t expected[4] = {};
                    wave_ops_expected_rgba(x, y, expected);
                    stats.compute_pixels_checked++;
                    if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                        pixel[3] == expected[3]) {
                        stats.compute_pixel_matches++;
                    }
                }
            }
            stats.compute_readback_ok = stats.compute_pixels_checked == kFreshTextureWidth * kFreshTextureHeight &&
                                        stats.compute_pixel_matches == stats.compute_pixels_checked;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = stats.cs_loaded && stats.cs_bytes > 0 && SUCCEEDED(stats.options1_hr) && stats.wave_ops_reported &&
                 stats.wave_lane_count_min == 32 && stats.wave_lane_count_max == 32 &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_uav_buffer_hr) &&
                 SUCCEEDED(stats.create_uav_descriptor_heap_hr) && stats.uav_descriptors_created == 1 &&
                 SUCCEEDED(stats.create_readback_hr) && stats.footprint_bytes > 0 && stats.fixed_footprint_ok &&
                 stats.uav_descriptor_gpu_handle != 0 && stats.root_uav_sets == 1 && stats.dispatch_commands == 1 &&
                 stats.dispatch_groups_x == 8 && stats.dispatch_groups_y == 1 && stats.dispatch_groups_z == 1 &&
                 stats.uav_barriers == 1 && stats.transition_barriers == 1 && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && SUCCEEDED(stats.map_readback_hr) &&
                 stats.compute_readback_ok;
    if (!stats.pass)
        safe_release(exercise.present_buffer);
    if (root_blob)
        root_blob->Release();
    safe_release(pipeline_state);
    safe_release(root_signature);
    safe_release(uav_heap);
    safe_release(readback);
    return exercise;
}

static void destroy_wave_ops_exercise(WaveOpsExercise& exercise) {
    safe_release(exercise.present_buffer);
}

struct UavBarrierStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_cs_hr = E_FAIL;
    HRESULT compile_read_cs_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_read_pso_hr = E_FAIL;
    HRESULT create_uav_buffer_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT row_pitch = 0;
    D3D12_GPU_VIRTUAL_ADDRESS uav_gpu_virtual_address = 0;
    bool fixed_footprint_ok = false;
    uint32_t root_uav_sets = 0;
    uint32_t root_constant_sets = 0;
    uint32_t dispatch_commands = 0;
    uint32_t dispatch_write_commands = 0;
    uint32_t dispatch_read_transform_commands = 0;
    uint32_t dispatch_x = 0;
    uint32_t dispatch_y = 0;
    uint32_t uav_barriers = 0;
    uint32_t transition_barriers = 0;
    uint32_t compute_pixels_checked = 0;
    uint32_t compute_pixel_matches = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t compute_first_rgba[4] = {0, 0, 0, 0};
    uint8_t compute_center_rgba[4] = {0, 0, 0, 0};
    uint8_t compute_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_expected_rgba[4] = {48, 96, 160, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_center_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fence_wait_ok = false;
    bool compute_readback_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct UavBarrierExercise {
    UavBarrierStats stats;
    ID3D12Resource* present_buffer = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
};

static void uav_barrier_expected_rgba(UINT x, UINT y, uint8_t out[4]) {
    out[0] = static_cast<uint8_t>(48u + x);
    out[1] = static_cast<uint8_t>(96u + y);
    out[2] = static_cast<uint8_t>(160u + (x ^ y));
    out[3] = 255;
}

static const char* kUavBarrierHlsl = R"(
RWStructuredBuffer<uint> OutBuffer : register(u0);
[numthreads(1, 1, 1)]
void CSWrite(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= 16u || tid.y >= 16u) {
    return;
  }
  uint index = tid.y * 64u + tid.x;
  uint packed = tid.x | (tid.y << 8u) | ((tid.x ^ tid.y) << 16u) | (1u << 24u);
  OutBuffer[index] = packed;
}
[numthreads(1, 1, 1)]
void CSTransform(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= 16u || tid.y >= 16u) {
    return;
  }
  uint index = tid.y * 64u + tid.x;
  uint intermediate = OutBuffer[index];
  uint r = (intermediate & 0xffu) + 48u;
  uint g = ((intermediate >> 8u) & 0xffu) + 96u;
  uint b = ((intermediate >> 16u) & 0xffu) + 160u;
  OutBuffer[index] = r | (g << 8u) | (b << 16u) | 0xff000000u;
}
)";

static UavBarrierExercise exercise_uav_barrier_stamp(ID3D12Device* device, D3DCompileFn compile,
                                                     SerializeRootSignatureFn serialize, ID3D12CommandQueue* queue,
                                                     ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
                                                     ID3D12Fence* fence, HANDLE fence_event, UINT64& fence_value) {
    UavBarrierExercise exercise;
    UavBarrierStats& stats = exercise.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    if (!device || !compile || !serialize || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    D3D12_RESOURCE_DESC stamp_desc = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&stamp_desc, 0, 1, 0, &exercise.footprint, &rows, &row_bytes, &total_bytes);
    stats.footprint_bytes = total_bytes;
    stats.row_pitch = exercise.footprint.Footprint.RowPitch;
    stats.fixed_footprint_ok =
        stats.row_pitch == kFreshTextureWidth * 16u && total_bytes >= kFreshTextureHeight * stats.row_pitch;
    stats.dispatch_x = kFreshTextureWidth;
    stats.dispatch_y = kFreshTextureHeight;

    ID3DBlob* cs = nullptr;
    ID3DBlob* cs_read = nullptr;
    ID3DBlob* root_blob = nullptr;
    stats.compile_cs_hr = compile_shader(compile, kUavBarrierHlsl, "CSWrite", "cs_5_0", &cs);
    stats.compile_read_cs_hr = compile_shader(compile, kUavBarrierHlsl, "CSTransform", "cs_5_0", &cs_read);

    D3D12_ROOT_PARAMETER root_params[1] = {};
    root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    root_params[0].Descriptor.ShaderRegister = 0;
    root_params[0].Descriptor.RegisterSpace = 0;
    root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = root_params;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);

    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12PipelineState* read_pipeline_state = nullptr;
    if (root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature));
    }
    if (root_signature && cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root_signature;
        pso_desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        stats.create_pso_hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state));
    }
    if (root_signature && cs_read) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = root_signature;
        pso_desc.CS = {cs_read->GetBufferPointer(), cs_read->GetBufferSize()};
        stats.create_read_pso_hr = device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&read_pipeline_state));
    }

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC uav_desc = buffer_desc(total_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    ID3D12Resource* readback = nullptr;
    stats.create_uav_buffer_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
                                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                                 IID_PPV_ARGS(&exercise.present_buffer));
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (exercise.present_buffer)
        stats.uav_gpu_virtual_address = exercise.present_buffer->GetGPUVirtualAddress();

    bool submitted_work = false;
    if (stats.fixed_footprint_ok && pipeline_state && read_pipeline_state && root_signature &&
        exercise.present_buffer && readback && SUCCEEDED(allocator->Reset()) &&
        SUCCEEDED(list->Reset(allocator, nullptr))) {
        list->SetPipelineState(pipeline_state);
        list->SetComputeRootSignature(root_signature);
        list->SetComputeRootUnorderedAccessView(0, exercise.present_buffer->GetGPUVirtualAddress());
        stats.root_uav_sets++;
        list->Dispatch(kFreshTextureWidth, kFreshTextureHeight, 1);
        stats.dispatch_commands++;
        stats.dispatch_write_commands++;
        D3D12_RESOURCE_BARRIER uav = uav_resource_barrier(exercise.present_buffer);
        list->ResourceBarrier(1, &uav);
        stats.uav_barriers++;
        list->SetPipelineState(read_pipeline_state);
        list->SetComputeRootUnorderedAccessView(0, exercise.present_buffer->GetGPUVirtualAddress());
        stats.root_uav_sets++;
        list->Dispatch(kFreshTextureWidth, kFreshTextureHeight, 1);
        stats.dispatch_commands++;
        stats.dispatch_read_transform_commands++;
        D3D12_RESOURCE_BARRIER uav_after_transform = uav_resource_barrier(exercise.present_buffer);
        list->ResourceBarrier(1, &uav_after_transform);
        stats.uav_barriers++;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            exercise.present_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        stats.transition_barriers++;
        list->CopyBufferRegion(readback, 0, exercise.present_buffer, 0, total_bytes);
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(total_bytes)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            const uint8_t* base = mapped + static_cast<size_t>(exercise.footprint.Offset);
            const uint8_t* first = base;
            std::memcpy(stats.compute_first_rgba, first, sizeof(stats.compute_first_rgba));
            const uint8_t* center =
                base + static_cast<size_t>(kFreshTextureHeight / 2u) * exercise.footprint.Footprint.RowPitch +
                static_cast<size_t>(kFreshTextureWidth / 2u) * 4u;
            std::memcpy(stats.compute_center_rgba, center, sizeof(stats.compute_center_rgba));
            const uint8_t* last =
                base + static_cast<size_t>(kFreshTextureHeight - 1u) * exercise.footprint.Footprint.RowPitch +
                static_cast<size_t>(kFreshTextureWidth - 1u) * 4u;
            std::memcpy(stats.compute_last_rgba, last, sizeof(stats.compute_last_rgba));
            for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                const uint8_t* row = base + static_cast<size_t>(y) * exercise.footprint.Footprint.RowPitch;
                for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                    const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                    uint8_t expected[4] = {};
                    uav_barrier_expected_rgba(x, y, expected);
                    stats.compute_pixels_checked++;
                    if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                        pixel[3] == expected[3]) {
                        stats.compute_pixel_matches++;
                    }
                }
            }
            stats.compute_readback_ok = stats.compute_pixels_checked == kFreshTextureWidth * kFreshTextureHeight &&
                                        stats.compute_pixel_matches == stats.compute_pixels_checked;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_cs_hr) && SUCCEEDED(stats.compile_read_cs_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_read_pso_hr) &&
                 SUCCEEDED(stats.create_uav_buffer_hr) && SUCCEEDED(stats.create_readback_hr) &&
                 stats.footprint_bytes > 0 && stats.fixed_footprint_ok && stats.uav_gpu_virtual_address != 0 &&
                 stats.root_uav_sets == 2 && stats.root_constant_sets == 0 && stats.dispatch_commands == 2 &&
                 stats.dispatch_write_commands == 1 && stats.dispatch_read_transform_commands == 1 &&
                 stats.dispatch_x == kFreshTextureWidth && stats.dispatch_y == kFreshTextureHeight &&
                 stats.uav_barriers == 2 && stats.transition_barriers == 1 && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && SUCCEEDED(stats.map_readback_hr) &&
                 stats.compute_readback_ok;
    if (!stats.pass)
        safe_release(exercise.present_buffer);
    if (root_blob)
        root_blob->Release();
    if (cs)
        cs->Release();
    if (cs_read)
        cs_read->Release();
    safe_release(read_pipeline_state);
    safe_release(pipeline_state);
    safe_release(root_signature);
    safe_release(readback);
    return exercise;
}

static void destroy_uav_barrier_exercise(UavBarrierExercise& exercise) {
    safe_release(exercise.present_buffer);
}

struct RtvFormatStats {
    HRESULT create_texture_hr = E_FAIL;
    HRESULT create_rtv_heap_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT row_pitch = 0;
    uint32_t create_rtv_descriptors = 0;
    uint32_t clear_rtv_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t offscreen_pixels_checked = 0;
    uint32_t offscreen_pixel_matches = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {32, 192, 96, 255};
    uint8_t offscreen_first_rgba[4] = {0, 0, 0, 0};
    uint8_t offscreen_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fixed_footprint_ok = false;
    bool fence_wait_ok = false;
    bool offscreen_readback_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct RtvFormatExercise {
    RtvFormatStats stats;
    ID3D12Resource* present_texture = nullptr;
};

static void rtv_format_expected_rgba(uint8_t out[4]) {
    out[0] = 32;
    out[1] = 192;
    out[2] = 96;
    out[3] = 255;
}

static RtvFormatExercise exercise_rtv_format_stamp(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                   ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
                                                   ID3D12Fence* fence, HANDLE fence_event, UINT64& fence_value) {
    RtvFormatExercise exercise;
    RtvFormatStats& stats = exercise.stats;
    rtv_format_expected_rgba(stats.expected_rgba);
    if (!device || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    D3D12_RESOURCE_DESC texture = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    stats.create_texture_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                              IID_PPV_ARGS(&exercise.present_texture));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    stats.create_rtv_heap_hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        rtv_heap ? rtv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (exercise.present_texture && rtv_heap) {
        device->CreateRenderTargetView(exercise.present_texture, nullptr, rtv_handle);
        stats.create_rtv_descriptors++;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
    stats.footprint_bytes = total_bytes;
    stats.row_pitch = footprint.Footprint.RowPitch;
    stats.fixed_footprint_ok = footprint.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                               footprint.Footprint.Width == kFreshTextureWidth &&
                               footprint.Footprint.Height == kFreshTextureHeight && footprint.Footprint.Depth == 1 &&
                               footprint.Footprint.RowPitch == 256 &&
                               total_bytes >= static_cast<UINT64>(kFreshTextureHeight) * footprint.Footprint.RowPitch;
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    ID3D12Resource* readback = nullptr;
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    bool submitted_work = false;
    if (stats.fixed_footprint_ok && exercise.present_texture && rtv_heap && readback && SUCCEEDED(allocator->Reset()) &&
        SUCCEEDED(list->Reset(allocator, nullptr))) {
        const FLOAT color[4] = {32.0f / 255.0f, 192.0f / 255.0f, 96.0f / 255.0f, 1.0f};
        list->ClearRenderTargetView(rtv_handle, color, 0, nullptr);
        stats.clear_rtv_commands++;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            exercise.present_texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        stats.transition_barriers++;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = exercise.present_texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (stats.fixed_footprint_ok && readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(total_bytes)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            const uint8_t* base = mapped + static_cast<size_t>(footprint.Offset);
            std::memcpy(stats.offscreen_first_rgba, base, sizeof(stats.offscreen_first_rgba));
            const uint8_t* last = base + static_cast<size_t>(kFreshTextureHeight - 1u) * footprint.Footprint.RowPitch +
                                  static_cast<size_t>(kFreshTextureWidth - 1u) * 4u;
            std::memcpy(stats.offscreen_last_rgba, last, sizeof(stats.offscreen_last_rgba));
            for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                const uint8_t* row = base + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                    const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                    stats.offscreen_pixels_checked++;
                    if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                        pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                        stats.offscreen_pixel_matches++;
                    }
                }
            }
            stats.offscreen_readback_ok = stats.offscreen_pixels_checked == kFreshTextureWidth * kFreshTextureHeight &&
                                          stats.offscreen_pixel_matches == stats.offscreen_pixels_checked;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = SUCCEEDED(stats.create_texture_hr) && SUCCEEDED(stats.create_rtv_heap_hr) &&
                 SUCCEEDED(stats.create_readback_hr) && stats.create_rtv_descriptors == 1 && stats.fixed_footprint_ok &&
                 stats.clear_rtv_commands == 1 && stats.transition_barriers == 1 && stats.fence_wait_ok &&
                 stats.offscreen_readback_ok;
    if (!stats.pass)
        safe_release(exercise.present_texture);
    safe_release(readback);
    safe_release(rtv_heap);
    return exercise;
}

static void destroy_rtv_format_exercise(RtvFormatExercise& exercise) {
    safe_release(exercise.present_texture);
}

struct SubresourceViewStats {
    HRESULT create_texture_array_hr = E_FAIL;
    HRESULT create_upload_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT create_descriptor_heap_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_upload_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT64 slice_offsets[2] = {0, 0};
    UINT row_pitch[2] = {0, 0};
    uint32_t slice_count = 2;
    uint32_t srv_descriptors_created = 0;
    uint32_t upload_subresources_filled = 0;
    uint32_t readback_sentinel_fills = 0;
    uint32_t copy_upload_subresources = 0;
    uint32_t copy_readback_subresources = 0;
    uint32_t transition_barriers = 0;
    uint32_t subresource_pixels_checked = 0;
    uint32_t subresource_pixel_matches = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_slice0_rgba[4] = {64, 48, 208, 255};
    uint8_t expected_slice1_rgba[4] = {208, 144, 64, 255};
    uint8_t slice0_first_rgba[4] = {0, 0, 0, 0};
    uint8_t slice0_last_rgba[4] = {0, 0, 0, 0};
    uint8_t slice1_first_rgba[4] = {0, 0, 0, 0};
    uint8_t slice1_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fixed_footprints_ok = false;
    bool fence_wait_ok = false;
    bool subresource_readback_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct SubresourceViewExercise {
    SubresourceViewStats stats;
    ID3D12Resource* present_texture = nullptr;
};

static void subresource_view_expected_rgba(UINT slice, uint8_t out[4]) {
    if (slice == 0) {
        out[0] = 64;
        out[1] = 48;
        out[2] = 208;
        out[3] = 255;
    } else {
        out[0] = 208;
        out[1] = 144;
        out[2] = 64;
        out[3] = 255;
    }
}

static TexturePayload make_subresource_view_payload(UINT slice) {
    TexturePayload payload;
    uint8_t rgba[4] = {};
    subresource_view_expected_rgba(slice, rgba);
    for (size_t i = 0; i < payload.rgba.size(); i += 4) {
        payload.rgba[i + 0] = rgba[0];
        payload.rgba[i + 1] = rgba[1];
        payload.rgba[i + 2] = rgba[2];
        payload.rgba[i + 3] = rgba[3];
    }
    payload.family = "generated-subresource-view";
    payload.label = slice == 0 ? "texture2darray-slice0" : "texture2darray-slice1";
    payload.bytes_from_file = static_cast<uint32_t>(payload.rgba.size());
    payload.declared_size = payload.rgba.size();
    return payload;
}

static SubresourceViewExercise exercise_subresource_view_stamp(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                               ID3D12CommandAllocator* allocator,
                                                               ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                                                               HANDLE fence_event, UINT64& fence_value) {
    SubresourceViewExercise exercise;
    SubresourceViewStats& stats = exercise.stats;
    subresource_view_expected_rgba(0, stats.expected_slice0_rgba);
    subresource_view_expected_rgba(1, stats.expected_slice1_rgba);
    if (!device || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    D3D12_RESOURCE_DESC texture = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    texture.DepthOrArraySize = 2;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 row_bytes[2] = {};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 2, 0, footprints, rows, row_bytes, &total_bytes);
    stats.footprint_bytes = total_bytes;
    stats.slice_offsets[0] = footprints[0].Offset;
    stats.slice_offsets[1] = footprints[1].Offset;
    stats.row_pitch[0] = footprints[0].Footprint.RowPitch;
    stats.row_pitch[1] = footprints[1].Footprint.RowPitch;
    stats.fixed_footprints_ok =
        footprints[0].Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        footprints[1].Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
        footprints[0].Footprint.Width == kFreshTextureWidth && footprints[1].Footprint.Width == kFreshTextureWidth &&
        footprints[0].Footprint.Height == kFreshTextureHeight &&
        footprints[1].Footprint.Height == kFreshTextureHeight && footprints[0].Footprint.Depth == 1 &&
        footprints[1].Footprint.Depth == 1 && footprints[0].Footprint.RowPitch == 256 &&
        footprints[1].Footprint.RowPitch == 256 && rows[0] == kFreshTextureHeight && rows[1] == kFreshTextureHeight &&
        footprints[1].Offset >=
            footprints[0].Offset + static_cast<UINT64>(kFreshTextureHeight) * footprints[0].Footprint.RowPitch &&
        total_bytes >=
            footprints[1].Offset + static_cast<UINT64>(kFreshTextureHeight) * footprints[1].Footprint.RowPitch;

    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    stats.create_texture_array_hr =
        device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&exercise.present_texture));

    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC upload_desc = buffer_desc(total_bytes);
    ID3D12Resource* upload = nullptr;
    stats.create_upload_hr =
        device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (upload && stats.fixed_footprints_ok) {
        for (UINT slice = 0; slice < 2; ++slice) {
            TexturePayload payload = make_subresource_view_payload(slice);
            if (fill_texture_upload(upload, footprints[slice], payload, kFreshTextureWidth, kFreshTextureHeight))
                stats.upload_subresources_filled++;
        }
        stats.map_upload_hr = stats.upload_subresources_filled == 2 ? S_OK : E_FAIL;
    }

    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    ID3D12Resource* readback = nullptr;
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (readback && stats.fixed_footprints_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        if (SUCCEEDED(readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped))) && mapped) {
            for (UINT slice = 0; slice < 2; ++slice) {
                uint8_t expected[4] = {};
                subresource_view_expected_rgba(slice, expected);
                uint8_t* base = mapped + static_cast<size_t>(footprints[slice].Offset);
                for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                    uint8_t* row = base + static_cast<size_t>(y) * footprints[slice].Footprint.RowPitch;
                    for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                        uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                        pixel[0] = static_cast<uint8_t>(expected[0] ^ 0xffu);
                        pixel[1] = static_cast<uint8_t>(expected[1] ^ 0xffu);
                        pixel[2] = static_cast<uint8_t>(expected[2] ^ 0xffu);
                        pixel[3] = static_cast<uint8_t>(expected[3] ^ 0xffu);
                    }
                }
                stats.readback_sentinel_fills++;
            }
            D3D12_RANGE written = {0, static_cast<SIZE_T>(total_bytes)};
            readback->Unmap(0, &written);
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 2;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap));
    if (srv_heap && exercise.present_texture) {
        UINT descriptor_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC array_srv = {};
        array_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        array_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        array_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        array_srv.Texture2DArray.MostDetailedMip = 0;
        array_srv.Texture2DArray.MipLevels = 1;
        array_srv.Texture2DArray.FirstArraySlice = 0;
        array_srv.Texture2DArray.ArraySize = 2;
        device->CreateShaderResourceView(exercise.present_texture, &array_srv,
                                         offset_cpu(srv_cpu, descriptor_increment, 0));
        stats.srv_descriptors_created++;
        D3D12_SHADER_RESOURCE_VIEW_DESC slice_srv = array_srv;
        slice_srv.Texture2DArray.FirstArraySlice = 1;
        slice_srv.Texture2DArray.ArraySize = 1;
        device->CreateShaderResourceView(exercise.present_texture, &slice_srv,
                                         offset_cpu(srv_cpu, descriptor_increment, 1));
        stats.srv_descriptors_created++;
    }

    bool submitted_work = false;
    if (stats.fixed_footprints_ok && exercise.present_texture && upload && readback &&
        stats.upload_subresources_filled == 2 && SUCCEEDED(allocator->Reset()) &&
        SUCCEEDED(list->Reset(allocator, nullptr))) {
        for (UINT slice = 0; slice < 2; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = exercise.present_texture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[slice];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            stats.copy_upload_subresources++;
        }
        D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            exercise.present_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy_source);
        stats.transition_barriers++;
        for (UINT slice = 0; slice < 2; ++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprints[slice];
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = exercise.present_texture;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = slice;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            stats.copy_readback_subresources++;
        }
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (stats.fixed_footprints_ok && readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(total_bytes)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            for (UINT slice = 0; slice < 2; ++slice) {
                uint8_t expected[4] = {};
                subresource_view_expected_rgba(slice, expected);
                const uint8_t* base = mapped + static_cast<size_t>(footprints[slice].Offset);
                const uint8_t* last =
                    base + static_cast<size_t>(kFreshTextureHeight - 1u) * footprints[slice].Footprint.RowPitch +
                    static_cast<size_t>(kFreshTextureWidth - 1u) * 4u;
                if (slice == 0) {
                    std::memcpy(stats.slice0_first_rgba, base, sizeof(stats.slice0_first_rgba));
                    std::memcpy(stats.slice0_last_rgba, last, sizeof(stats.slice0_last_rgba));
                } else {
                    std::memcpy(stats.slice1_first_rgba, base, sizeof(stats.slice1_first_rgba));
                    std::memcpy(stats.slice1_last_rgba, last, sizeof(stats.slice1_last_rgba));
                }
                for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                    const uint8_t* row = base + static_cast<size_t>(y) * footprints[slice].Footprint.RowPitch;
                    for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                        const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                        stats.subresource_pixels_checked++;
                        if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                            pixel[3] == expected[3]) {
                            stats.subresource_pixel_matches++;
                        }
                    }
                }
            }
            stats.subresource_readback_ok =
                stats.subresource_pixels_checked == 2u * kFreshTextureWidth * kFreshTextureHeight &&
                stats.subresource_pixel_matches == stats.subresource_pixels_checked;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = SUCCEEDED(stats.create_texture_array_hr) && SUCCEEDED(stats.create_upload_hr) &&
                 SUCCEEDED(stats.create_readback_hr) && SUCCEEDED(stats.create_descriptor_heap_hr) &&
                 stats.fixed_footprints_ok && stats.srv_descriptors_created == 2 &&
                 stats.upload_subresources_filled == 2 && stats.readback_sentinel_fills == 2 &&
                 stats.copy_upload_subresources == 2 && stats.copy_readback_subresources == 2 &&
                 stats.transition_barriers == 1 && stats.fence_wait_ok && stats.subresource_readback_ok;
    if (!stats.pass)
        safe_release(exercise.present_texture);
    safe_release(srv_heap);
    safe_release(readback);
    safe_release(upload);
    return exercise;
}

static void destroy_subresource_view_exercise(SubresourceViewExercise& exercise) {
    safe_release(exercise.present_texture);
}

struct TextureArraySrvSampleStats {
    bool d3dcompiler_loaded = false;
    HRESULT compile_vs_hr = E_FAIL;
    HRESULT compile_ps_hr = E_FAIL;
    HRESULT serialize_root_hr = E_FAIL;
    HRESULT create_root_hr = E_FAIL;
    HRESULT create_pso_hr = E_FAIL;
    HRESULT create_vertex_buffer_hr = E_FAIL;
    HRESULT create_texture_array_hr = E_FAIL;
    HRESULT create_upload_hr = E_FAIL;
    HRESULT create_descriptor_heap_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT64 slice_offsets[2] = {0, 0};
    UINT row_pitch[2] = {0, 0};
    uint32_t slice_count = 2;
    uint32_t srv_descriptors_created = 0;
    uint32_t upload_subresources_filled = 0;
    uint32_t copy_upload_subresources = 0;
    uint32_t transition_barriers = 0;
    uint32_t vertices_per_draw = 0;
    uint32_t draw_calls = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_slice0_rgba[4] = {24, 96, 216, 255};
    uint8_t expected_slice1_rgba[4] = {216, 88, 40, 255};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fixed_footprints_ok = false;
    bool fence_wait_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct TextureArraySrvSampleSceneResources {
    ID3D12RootSignature* root_signature = nullptr;
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* texture_array = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    TextureArraySrvSampleStats stats;
};

static void texture_array_srv_expected_rgba(UINT slice, uint8_t out[4]) {
    if (slice == 0) {
        out[0] = 24;
        out[1] = 96;
        out[2] = 216;
        out[3] = 255;
    } else {
        out[0] = 216;
        out[1] = 88;
        out[2] = 40;
        out[3] = 255;
    }
}

static TexturePayload make_texture_array_srv_payload(UINT slice) {
    TexturePayload payload;
    uint8_t rgba[4] = {};
    texture_array_srv_expected_rgba(slice, rgba);
    for (size_t i = 0; i < payload.rgba.size(); i += 4) {
        payload.rgba[i + 0] = rgba[0];
        payload.rgba[i + 1] = rgba[1];
        payload.rgba[i + 2] = rgba[2];
        payload.rgba[i + 3] = rgba[3];
    }
    payload.family = "generated-texture-array-srv";
    payload.label = slice == 0 ? "texture2darray-srv-slice0" : "texture2darray-srv-slice1";
    payload.bytes_from_file = static_cast<uint32_t>(payload.rgba.size());
    payload.declared_size = payload.rgba.size();
    return payload;
}

static TextureArraySrvSampleSceneResources
create_texture_array_srv_sample_scene(ID3D12Device* device, D3DCompileFn compile, SerializeRootSignatureFn serialize,
                                      ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator,
                                      ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE fence_event,
                                      UINT64& fence_value, UINT backbuffer_width, UINT backbuffer_height) {
    TextureArraySrvSampleSceneResources scene;
    TextureArraySrvSampleStats& stats = scene.stats;
    stats.d3dcompiler_loaded = compile != nullptr;
    texture_array_srv_expected_rgba(0, stats.expected_slice0_rgba);
    texture_array_srv_expected_rgba(1, stats.expected_slice1_rgba);

    static const char* kTextureArraySrvHlsl = R"HLSL(
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2DArray gTextureArray : register(t0);
SamplerState gSampler : register(s0);
PSIn vs_main(VSIn input) {
    PSIn output;
    output.pos = float4(input.pos, 1.0f);
    output.uv = input.uv;
    return output;
}
float4 ps_main(PSIn input) : SV_Target {
    return gTextureArray.Sample(gSampler, float3(input.uv, 1.0f));
}
)HLSL";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    stats.compile_vs_hr = compile_shader(compile, kTextureArraySrvHlsl, "vs_main", "vs_5_0", &vs);
    stats.compile_ps_hr = compile_shader(compile, kTextureArraySrvHlsl, "ps_main", "ps_5_0", &ps);

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    root_param.DescriptorTable.NumDescriptorRanges = 1;
    root_param.DescriptorTable.pDescriptorRanges = &range;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &root_param;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* root_blob = nullptr;
    stats.serialize_root_hr = serialize_root_signature(serialize, root_desc, &root_blob);
    if (device && root_blob) {
        stats.create_root_hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                           IID_PPV_ARGS(&scene.root_signature));
    }

    if (device && scene.root_signature && vs && ps) {
        D3D12_INPUT_ELEMENT_DESC input_elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.pRootSignature = scene.root_signature;
        pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso_desc.InputLayout = {input_elements, 2};
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        stats.create_pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&scene.pipeline_state));
    }

    if (device) {
        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vb_desc = buffer_desc(30u * sizeof(TexVertex));
        stats.create_vertex_buffer_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&scene.vertex_buffer));
        if (SUCCEEDED(stats.create_vertex_buffer_hr) && scene.vertex_buffer) {
            TexVertex* mapped = nullptr;
            scene.vertex_buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            if (mapped) {
                const float draw_x0 = static_cast<float>(kTextureArraySrvStampX) - 1.0f;
                const float draw_y0 = static_cast<float>(kTextureArraySrvStampY) - 1.0f;
                const float draw_x1 = static_cast<float>(kTextureArraySrvStampX + kFreshTextureWidth) + 1.0f;
                const float draw_y1 = static_cast<float>(kTextureArraySrvStampY + kFreshTextureHeight) + 1.0f;
                const float x0 = ndc_x_from_pixel(draw_x0, static_cast<float>(backbuffer_width));
                const float x1 = ndc_x_from_pixel(draw_x1, static_cast<float>(backbuffer_width));
                const float y0 = ndc_y_from_pixel(draw_y1, static_cast<float>(backbuffer_height));
                const float y1 = ndc_y_from_pixel(draw_y0, static_cast<float>(backbuffer_height));
                for (UINT quad = 0; quad < 5; ++quad)
                    write_tex_quad(mapped + quad * 6u, x0, y0, x1, y1, 0.038f);
                D3D12_RANGE written = {0, 30u * sizeof(TexVertex)};
                scene.vertex_buffer->Unmap(0, &written);
            }
            scene.vertex_view.BufferLocation = scene.vertex_buffer->GetGPUVirtualAddress();
            scene.vertex_view.SizeInBytes = 30u * sizeof(TexVertex);
            scene.vertex_view.StrideInBytes = sizeof(TexVertex);
            stats.vertices_per_draw = 30;
        }
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 row_bytes[2] = {};
    UINT64 total_bytes = 0;
    if (device) {
        D3D12_RESOURCE_DESC tex_desc =
            texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
        tex_desc.DepthOrArraySize = 2;
        device->GetCopyableFootprints(&tex_desc, 0, 2, 0, footprints, rows, row_bytes, &total_bytes);
        stats.footprint_bytes = total_bytes;
        stats.slice_offsets[0] = footprints[0].Offset;
        stats.slice_offsets[1] = footprints[1].Offset;
        stats.row_pitch[0] = footprints[0].Footprint.RowPitch;
        stats.row_pitch[1] = footprints[1].Footprint.RowPitch;
        stats.fixed_footprints_ok =
            footprints[0].Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            footprints[1].Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
            footprints[0].Footprint.Width == kFreshTextureWidth &&
            footprints[1].Footprint.Width == kFreshTextureWidth &&
            footprints[0].Footprint.Height == kFreshTextureHeight &&
            footprints[1].Footprint.Height == kFreshTextureHeight && footprints[0].Footprint.Depth == 1 &&
            footprints[1].Footprint.Depth == 1 && footprints[0].Footprint.RowPitch == 256 &&
            footprints[1].Footprint.RowPitch == 256 && rows[0] == kFreshTextureHeight &&
            rows[1] == kFreshTextureHeight &&
            footprints[1].Offset >=
                footprints[0].Offset + static_cast<UINT64>(kFreshTextureHeight) * footprints[0].Footprint.RowPitch &&
            total_bytes >=
                footprints[1].Offset + static_cast<UINT64>(kFreshTextureHeight) * footprints[1].Footprint.RowPitch;

        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        stats.create_texture_array_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                        IID_PPV_ARGS(&scene.texture_array));

        D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(total_bytes);
        stats.create_upload_hr =
            device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&scene.upload));
        if (scene.upload && stats.fixed_footprints_ok) {
            for (UINT slice = 0; slice < 2; ++slice) {
                TexturePayload payload = make_texture_array_srv_payload(slice);
                if (fill_texture_upload(scene.upload, footprints[slice], payload, kFreshTextureWidth,
                                        kFreshTextureHeight))
                    stats.upload_subresources_filled++;
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&scene.srv_heap));
        if (SUCCEEDED(stats.create_descriptor_heap_hr) && scene.srv_heap && scene.texture_array) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
            srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2DArray.MostDetailedMip = 0;
            srv_desc.Texture2DArray.MipLevels = 1;
            srv_desc.Texture2DArray.FirstArraySlice = 0;
            srv_desc.Texture2DArray.ArraySize = 2;
            device->CreateShaderResourceView(scene.texture_array, &srv_desc,
                                             scene.srv_heap->GetCPUDescriptorHandleForHeapStart());
            stats.srv_descriptors_created = 1;
        }

        if (stats.fixed_footprints_ok && scene.texture_array && scene.upload && stats.upload_subresources_filled == 2 &&
            allocator && list && queue && fence && fence_event && SUCCEEDED(allocator->Reset()) &&
            SUCCEEDED(list->Reset(allocator, nullptr))) {
            for (UINT slice = 0; slice < 2; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = scene.texture_array;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = slice;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = scene.upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprints[slice];
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                stats.copy_upload_subresources++;
            }
            D3D12_RESOURCE_BARRIER to_srv = transition_barrier(scene.texture_array, D3D12_RESOURCE_STATE_COPY_DEST,
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &to_srv);
            stats.transition_barriers = 1;
            stats.close_hr = list->Close();
            if (SUCCEEDED(stats.close_hr)) {
                ID3D12CommandList* base = list;
                queue->ExecuteCommandLists(1, &base);
                fence_value++;
                stats.signal_hr = queue->Signal(fence, fence_value);
                stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
            }
        }
    }

    stats.pass = stats.d3dcompiler_loaded && SUCCEEDED(stats.compile_vs_hr) && SUCCEEDED(stats.compile_ps_hr) &&
                 SUCCEEDED(stats.serialize_root_hr) && SUCCEEDED(stats.create_root_hr) &&
                 SUCCEEDED(stats.create_pso_hr) && SUCCEEDED(stats.create_vertex_buffer_hr) &&
                 SUCCEEDED(stats.create_texture_array_hr) && SUCCEEDED(stats.create_upload_hr) &&
                 SUCCEEDED(stats.create_descriptor_heap_hr) && stats.fixed_footprints_ok &&
                 stats.srv_descriptors_created == 1 && stats.upload_subresources_filled == 2 &&
                 stats.copy_upload_subresources == 2 && stats.transition_barriers == 1 && SUCCEEDED(stats.close_hr) &&
                 SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok && scene.root_signature && scene.pipeline_state &&
                 scene.vertex_buffer && scene.texture_array && scene.srv_heap && stats.vertices_per_draw == 30;

    safe_release(vs);
    safe_release(ps);
    safe_release(root_blob);
    return scene;
}

static void destroy_texture_array_srv_sample_scene(TextureArraySrvSampleSceneResources& scene) {
    safe_release(scene.srv_heap);
    safe_release(scene.upload);
    safe_release(scene.texture_array);
    safe_release(scene.vertex_buffer);
    safe_release(scene.pipeline_state);
    safe_release(scene.root_signature);
}

struct RenderPassStats {
    HRESULT query_list4_hr = E_FAIL;
    HRESULT create_texture_hr = E_FAIL;
    HRESULT create_rtv_heap_hr = E_FAIL;
    HRESULT create_readback_hr = E_FAIL;
    HRESULT close_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT map_readback_hr = E_FAIL;
    UINT64 footprint_bytes = 0;
    UINT row_pitch = 0;
    uint32_t create_rtv_descriptors = 0;
    uint32_t begin_render_pass_commands = 0;
    uint32_t end_render_pass_commands = 0;
    uint32_t transition_barriers = 0;
    uint32_t offscreen_pixels_checked = 0;
    uint32_t offscreen_pixel_matches = 0;
    uint32_t present_copy_commands = 0;
    uint32_t present_samples_checked = 0;
    uint32_t present_sample_matches = 0;
    uint32_t present_pixels_checked = 0;
    uint32_t present_pixel_matches = 0;
    uint8_t expected_rgba[4] = {224, 80, 176, 255};
    uint8_t offscreen_first_rgba[4] = {0, 0, 0, 0};
    uint8_t offscreen_last_rgba[4] = {0, 0, 0, 0};
    uint8_t present_rgba[4] = {0, 0, 0, 0};
    uint8_t present_last_rgba[4] = {0, 0, 0, 0};
    bool fixed_footprint_ok = false;
    bool fence_wait_ok = false;
    bool offscreen_readback_ok = false;
    bool present_pass = false;
    bool pass = false;
};

struct RenderPassExercise {
    RenderPassStats stats;
    ID3D12Resource* present_texture = nullptr;
};

static void render_pass_expected_rgba(uint8_t out[4]) {
    out[0] = 224;
    out[1] = 80;
    out[2] = 176;
    out[3] = 255;
}

static RenderPassExercise exercise_render_pass_stamp(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                     ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list,
                                                     ID3D12Fence* fence, HANDLE fence_event, UINT64& fence_value) {
    RenderPassExercise exercise;
    RenderPassStats& stats = exercise.stats;
    render_pass_expected_rgba(stats.expected_rgba);
    if (!device || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    ID3D12GraphicsCommandList4* list4 = nullptr;
    stats.query_list4_hr = list->QueryInterface(IID_PPV_ARGS(&list4));

    D3D12_RESOURCE_DESC texture = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    stats.create_texture_hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                              D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                              IID_PPV_ARGS(&exercise.present_texture));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    stats.create_rtv_heap_hr = device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        rtv_heap ? rtv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    if (exercise.present_texture && rtv_heap) {
        device->CreateRenderTargetView(exercise.present_texture, nullptr, rtv_handle);
        stats.create_rtv_descriptors++;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_bytes, &total_bytes);
    stats.footprint_bytes = total_bytes;
    stats.row_pitch = footprint.Footprint.RowPitch;
    stats.fixed_footprint_ok = footprint.Footprint.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                               footprint.Footprint.Width == kFreshTextureWidth &&
                               footprint.Footprint.Height == kFreshTextureHeight && footprint.Footprint.Depth == 1 &&
                               footprint.Footprint.RowPitch == 256 &&
                               total_bytes >= static_cast<UINT64>(kFreshTextureHeight) * footprint.Footprint.RowPitch;
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    ID3D12Resource* readback = nullptr;
    stats.create_readback_hr =
        device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    bool submitted_work = false;
    if (SUCCEEDED(stats.query_list4_hr) && stats.fixed_footprint_ok && exercise.present_texture && rtv_heap &&
        readback && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr))) {
        D3D12_RENDER_PASS_RENDER_TARGET_DESC render_target = {};
        render_target.cpuDescriptor = rtv_handle;
        render_target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        render_target.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        render_target.BeginningAccess.Clear.ClearValue.Color[0] = 224.0f / 255.0f;
        render_target.BeginningAccess.Clear.ClearValue.Color[1] = 80.0f / 255.0f;
        render_target.BeginningAccess.Clear.ClearValue.Color[2] = 176.0f / 255.0f;
        render_target.BeginningAccess.Clear.ClearValue.Color[3] = 1.0f;
        render_target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        list4->BeginRenderPass(1, &render_target, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
        stats.begin_render_pass_commands++;
        list4->EndRenderPass();
        stats.end_render_pass_commands++;
        D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            exercise.present_texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_copy);
        stats.transition_barriers++;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = exercise.present_texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            submitted_work = true;
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }
    if (submitted_work && !stats.fence_wait_ok) {
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2u);
    }

    if (stats.fixed_footprint_ok && readback && stats.fence_wait_ok) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, static_cast<SIZE_T>(total_bytes)};
        stats.map_readback_hr = readback->Map(0, &read_range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(stats.map_readback_hr) && mapped) {
            const uint8_t* base = mapped + static_cast<size_t>(footprint.Offset);
            std::memcpy(stats.offscreen_first_rgba, base, sizeof(stats.offscreen_first_rgba));
            const uint8_t* last = base + static_cast<size_t>(kFreshTextureHeight - 1u) * footprint.Footprint.RowPitch +
                                  static_cast<size_t>(kFreshTextureWidth - 1u) * 4u;
            std::memcpy(stats.offscreen_last_rgba, last, sizeof(stats.offscreen_last_rgba));
            for (UINT y = 0; y < kFreshTextureHeight; ++y) {
                const uint8_t* row = base + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                    const uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
                    stats.offscreen_pixels_checked++;
                    if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                        pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                        stats.offscreen_pixel_matches++;
                    }
                }
            }
            stats.offscreen_readback_ok = stats.offscreen_pixels_checked == kFreshTextureWidth * kFreshTextureHeight &&
                                          stats.offscreen_pixel_matches == stats.offscreen_pixels_checked;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    stats.pass = SUCCEEDED(stats.query_list4_hr) && SUCCEEDED(stats.create_texture_hr) &&
                 SUCCEEDED(stats.create_rtv_heap_hr) && SUCCEEDED(stats.create_readback_hr) &&
                 stats.create_rtv_descriptors == 1 && stats.fixed_footprint_ok &&
                 stats.begin_render_pass_commands == 1 && stats.end_render_pass_commands == 1 &&
                 stats.transition_barriers == 1 && stats.fence_wait_ok && stats.offscreen_readback_ok;
    if (!stats.pass)
        safe_release(exercise.present_texture);
    safe_release(readback);
    safe_release(rtv_heap);
    safe_release(list4);
    return exercise;
}

static void destroy_render_pass_exercise(RenderPassExercise& exercise) {
    safe_release(exercise.present_texture);
}

static bool inspect_texture_stamp(DxilReadbackResources& readback, GpuTextureStats& texture_stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* pixel = readback_pixel(readback, mapped, kTextureStampX, kTextureStampY);
    std::memcpy(texture_stats.present_rgba, pixel, sizeof(texture_stats.present_rgba));
    texture_stats.present_samples_checked++;
    const bool matches = std::memcmp(texture_stats.present_rgba, texture_stats.present_expected_rgba,
                                     sizeof(texture_stats.present_rgba)) == 0;
    if (matches)
        texture_stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_heap_alias_stamp(DxilReadbackResources& readback, HeapAliasStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* pixel = readback_pixel(readback, mapped, kHeapAliasStampX, kHeapAliasStampY);
    std::memcpy(stats.present_rgba, pixel, sizeof(stats.present_rgba));
    stats.present_samples_checked++;
    const bool matches = std::memcmp(stats.present_rgba, stats.present_expected_rgba, sizeof(stats.present_rgba)) == 0;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_uav_barrier_stamp(DxilReadbackResources& readback, UavBarrierStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kUavBarrierStampX, kUavBarrierStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* center = readback_pixel(readback, mapped, kUavBarrierStampX + kFreshTextureWidth / 2u,
                                           kUavBarrierStampY + kFreshTextureHeight / 2u);
    std::memcpy(stats.present_center_rgba, center, sizeof(stats.present_center_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kUavBarrierStampX + kFreshTextureWidth - 1u,
                                         kUavBarrierStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kUavBarrierStampX + x, kUavBarrierStampY + y);
            uint8_t expected[4] = {};
            uav_barrier_expected_rgba(x, y, expected);
            stats.present_pixels_checked++;
            if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                pixel[3] == expected[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_wave_ops_stamp(DxilReadbackResources& readback, WaveOpsStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kWaveOpsStampX, kWaveOpsStampY);
    std::memcpy(stats.present_first_rgba, first, sizeof(stats.present_first_rgba));
    const uint8_t* center = readback_pixel(readback, mapped, kWaveOpsStampX + kFreshTextureWidth / 2u,
                                           kWaveOpsStampY + kFreshTextureHeight / 2u);
    std::memcpy(stats.present_center_rgba, center, sizeof(stats.present_center_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kWaveOpsStampX + kFreshTextureWidth - 1u,
                                         kWaveOpsStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kWaveOpsStampX + x, kWaveOpsStampY + y);
            uint8_t expected[4] = {};
            wave_ops_expected_rgba(x, y, expected);
            if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                pixel[3] == expected[3]) {
                frame_matches++;
            }
        }
    }
    stats.present_pixels_checked += kFreshTextureWidth * kFreshTextureHeight;
    stats.present_pixel_matches += frame_matches;
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    stats.present_samples_checked++;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_nanite_cluster_stamp(DxilReadbackResources& readback, NaniteClusterStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kNaniteClusterStampX, kNaniteClusterStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kNaniteClusterStampX + kFreshTextureWidth - 1u,
                                         kNaniteClusterStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kNaniteClusterStampX + x, kNaniteClusterStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_rtv_format_stamp(DxilReadbackResources& readback, RtvFormatStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kRtvFormatStampX, kRtvFormatStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kRtvFormatStampX + kFreshTextureWidth - 1u,
                                         kRtvFormatStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kRtvFormatStampX + x, kRtvFormatStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_subresource_view_stamp(DxilReadbackResources& readback, SubresourceViewStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kSubresourceViewStampX, kSubresourceViewStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kSubresourceViewStampX + kFreshTextureWidth - 1u,
                                         kSubresourceViewStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel =
                readback_pixel(readback, mapped, kSubresourceViewStampX + x, kSubresourceViewStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_slice1_rgba[0] && pixel[1] == stats.expected_slice1_rgba[1] &&
                pixel[2] == stats.expected_slice1_rgba[2] && pixel[3] == stats.expected_slice1_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_render_pass_stamp(DxilReadbackResources& readback, RenderPassStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kRenderPassStampX, kRenderPassStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kRenderPassStampX + kFreshTextureWidth - 1u,
                                         kRenderPassStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kRenderPassStampX + x, kRenderPassStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_corpus_shader_stamp(DxilReadbackResources& readback, CorpusShaderStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kCorpusShaderStampX, kCorpusShaderStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kCorpusShaderStampX + kFreshTextureWidth - 1u,
                                         kCorpusShaderStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kCorpusShaderStampX + x, kCorpusShaderStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_srv_sample_stamp(DxilReadbackResources& readback, SrvSampleStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kSrvSampleStampX, kSrvSampleStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kSrvSampleStampX + kFreshTextureWidth - 1u,
                                         kSrvSampleStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kSrvSampleStampX + x, kSrvSampleStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_texture_array_srv_sample_stamp(DxilReadbackResources& readback, TextureArraySrvSampleStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kTextureArraySrvStampX, kTextureArraySrvStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kTextureArraySrvStampX + kFreshTextureWidth - 1u,
                                         kTextureArraySrvStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel =
                readback_pixel(readback, mapped, kTextureArraySrvStampX + x, kTextureArraySrvStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_slice1_rgba[0] && pixel[1] == stats.expected_slice1_rgba[1] &&
                pixel[2] == stats.expected_slice1_rgba[2] && pixel[3] == stats.expected_slice1_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_textured3d_faces(DxilReadbackResources& readback, Textured3DStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const UINT face = stats.front_face < kTextured3DFaceCount ? stats.front_face : 0;
    Textured3DFaceStats& face_stats = stats.faces[face];
    const uint8_t* pixel = readback_pixel(readback, mapped, face_stats.sample_x, face_stats.sample_y);
    std::memcpy(face_stats.present_rgba, pixel, sizeof(face_stats.present_rgba));
    face_stats.samples_checked++;
    stats.present_samples_checked++;
    const bool face_match = pixel[0] == face_stats.expected_rgba[0] && pixel[1] == face_stats.expected_rgba[1] &&
                            pixel[2] == face_stats.expected_rgba[2] && pixel[3] == face_stats.expected_rgba[3];
    if (face_match) {
        face_stats.sample_matches++;
        stats.present_sample_matches++;
    }
    const uint8_t* depth_pixel =
        readback_pixel(readback, mapped, stats.depth_overlap_sample_x, stats.depth_overlap_sample_y);
    std::memcpy(stats.depth_overlap_present_rgba, depth_pixel, sizeof(stats.depth_overlap_present_rgba));
    stats.depth_overlap_samples_checked++;
    const bool depth_match = depth_pixel[0] == stats.depth_overlap_expected_rgba[0] &&
                             depth_pixel[1] == stats.depth_overlap_expected_rgba[1] &&
                             depth_pixel[2] == stats.depth_overlap_expected_rgba[2] &&
                             depth_pixel[3] == stats.depth_overlap_expected_rgba[3];
    if (depth_match)
        stats.depth_overlap_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return face_match && depth_match;
}

static bool inspect_cbv_sample_stamp(DxilReadbackResources& readback, CbvSampleStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kCbvStampX, kCbvStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last =
        readback_pixel(readback, mapped, kCbvStampX + kFreshTextureWidth - 1u, kCbvStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kCbvStampX + x, kCbvStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_indexed_draw_stamp(DxilReadbackResources& readback, IndexedDrawStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;

    auto inspect_stamp = [&](UINT stamp_x, UINT stamp_y, const uint8_t expected[4], uint8_t first_rgba[4],
                             uint8_t last_rgba[4], uint32_t& samples_checked, uint32_t& sample_matches,
                             uint32_t& pixels_checked, uint32_t& pixel_matches) {
        const uint8_t* first = readback_pixel(readback, mapped, stamp_x, stamp_y);
        std::memcpy(first_rgba, first, 4);
        const uint8_t* last =
            readback_pixel(readback, mapped, stamp_x + kFreshTextureWidth - 1u, stamp_y + kFreshTextureHeight - 1u);
        std::memcpy(last_rgba, last, 4);
        samples_checked++;
        uint32_t frame_matches = 0;
        for (UINT y = 0; y < kFreshTextureHeight; ++y) {
            for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                const uint8_t* pixel = readback_pixel(readback, mapped, stamp_x + x, stamp_y + y);
                pixels_checked++;
                if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                    pixel[3] == expected[3]) {
                    pixel_matches++;
                    frame_matches++;
                }
            }
        }
        const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
        if (matches)
            sample_matches++;
        return matches;
    };

    const bool r16_matches =
        inspect_stamp(kIndexedStampX, kIndexedStampY, stats.expected_rgba, stats.present_rgba, stats.present_last_rgba,
                      stats.present_samples_checked, stats.present_sample_matches, stats.present_pixels_checked,
                      stats.present_pixel_matches);
    const bool r32_matches =
        inspect_stamp(kIndexedR32StampX, kIndexedR32StampY, stats.expected_r32_rgba, stats.present_r32_rgba,
                      stats.present_r32_last_rgba, stats.present_r32_samples_checked, stats.present_r32_sample_matches,
                      stats.present_r32_pixels_checked, stats.present_r32_pixel_matches);
    const bool negative_base_matches =
        inspect_stamp(kIndexedNegativeBaseStampX, kIndexedNegativeBaseStampY, stats.expected_negative_base_rgba,
                      stats.present_negative_base_rgba, stats.present_negative_base_last_rgba,
                      stats.present_negative_base_samples_checked, stats.present_negative_base_sample_matches,
                      stats.present_negative_base_pixels_checked, stats.present_negative_base_pixel_matches);
    const bool dynamic_stride_matches =
        inspect_stamp(kIndexedDynamicStrideStampX, kIndexedDynamicStrideStampY, stats.expected_dynamic_stride_rgba,
                      stats.present_dynamic_stride_rgba, stats.present_dynamic_stride_last_rgba,
                      stats.present_dynamic_stride_samples_checked, stats.present_dynamic_stride_sample_matches,
                      stats.present_dynamic_stride_pixels_checked, stats.present_dynamic_stride_pixel_matches);
    const bool execute_indirect_indexed_matches = inspect_stamp(
        kIndexedExecuteIndirectStampX, kIndexedExecuteIndirectStampY, stats.expected_execute_indirect_indexed_rgba,
        stats.present_execute_indirect_indexed_rgba, stats.present_execute_indirect_indexed_last_rgba,
        stats.present_execute_indirect_indexed_samples_checked, stats.present_execute_indirect_indexed_sample_matches,
        stats.present_execute_indirect_indexed_pixels_checked, stats.present_execute_indirect_indexed_pixel_matches);

    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return r16_matches && r32_matches && negative_base_matches && dynamic_stride_matches &&
           execute_indirect_indexed_matches;
}

static bool inspect_multi_slot_instance_root_stamp(DxilReadbackResources& readback, MultiSlotInstanceRootStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;

    auto inspect_stamp = [&](UINT stamp_x, UINT stamp_y, const uint8_t expected[4], uint8_t first_rgba[4],
                             uint8_t last_rgba[4], uint32_t& samples_checked, uint32_t& sample_matches,
                             uint32_t& pixels_checked, uint32_t& pixel_matches) {
        const uint8_t* first = readback_pixel(readback, mapped, stamp_x, stamp_y);
        std::memcpy(first_rgba, first, 4);
        const uint8_t* last =
            readback_pixel(readback, mapped, stamp_x + kFreshTextureWidth - 1u, stamp_y + kFreshTextureHeight - 1u);
        std::memcpy(last_rgba, last, 4);
        samples_checked++;
        uint32_t frame_matches = 0;
        for (UINT y = 0; y < kFreshTextureHeight; ++y) {
            for (UINT x = 0; x < kFreshTextureWidth; ++x) {
                const uint8_t* pixel = readback_pixel(readback, mapped, stamp_x + x, stamp_y + y);
                pixels_checked++;
                if (pixel[0] == expected[0] && pixel[1] == expected[1] && pixel[2] == expected[2] &&
                    pixel[3] == expected[3]) {
                    pixel_matches++;
                    frame_matches++;
                }
            }
        }
        const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
        if (matches)
            sample_matches++;
        return matches;
    };

    const bool first_matches = inspect_stamp(
        kMultiSlotInstanceRootStampAX, kMultiSlotInstanceRootStampAY, stats.expected_first_rgba,
        stats.present_first_rgba, stats.present_first_last_rgba, stats.present_first_samples_checked,
        stats.present_first_sample_matches, stats.present_first_pixels_checked, stats.present_first_pixel_matches);
    const bool second_matches = inspect_stamp(
        kMultiSlotInstanceRootStampBX, kMultiSlotInstanceRootStampBY, stats.expected_second_rgba,
        stats.present_second_rgba, stats.present_second_last_rgba, stats.present_second_samples_checked,
        stats.present_second_sample_matches, stats.present_second_pixels_checked, stats.present_second_pixel_matches);

    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return first_matches && second_matches;
}

static bool inspect_tessellation_fallback_stamp(DxilReadbackResources& readback, TessellationFallbackStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kTessellationFallbackStampX, kTessellationFallbackStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kTessellationFallbackStampX + kFreshTextureWidth - 1u,
                                         kTessellationFallbackStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel =
                readback_pixel(readback, mapped, kTessellationFallbackStampX + x, kTessellationFallbackStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;

    const uint8_t* indexed_first =
        readback_pixel(readback, mapped, kTessellationIndexedStampX, kTessellationIndexedStampY);
    std::memcpy(stats.indexed_present_rgba, indexed_first, sizeof(stats.indexed_present_rgba));
    const uint8_t* indexed_last = readback_pixel(readback, mapped, kTessellationIndexedStampX + kFreshTextureWidth - 1u,
                                                 kTessellationIndexedStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.indexed_present_last_rgba, indexed_last, sizeof(stats.indexed_present_last_rgba));
    stats.indexed_present_samples_checked++;
    uint32_t indexed_frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel =
                readback_pixel(readback, mapped, kTessellationIndexedStampX + x, kTessellationIndexedStampY + y);
            stats.indexed_present_pixels_checked++;
            if (pixel[0] == stats.indexed_expected_rgba[0] && pixel[1] == stats.indexed_expected_rgba[1] &&
                pixel[2] == stats.indexed_expected_rgba[2] && pixel[3] == stats.indexed_expected_rgba[3]) {
                stats.indexed_present_pixel_matches++;
                indexed_frame_matches++;
            }
        }
    }
    const bool indexed_matches = indexed_frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (indexed_matches)
        stats.indexed_present_sample_matches++;

    uint32_t covered_pixels = 0;
    uint32_t red_dominant = 0;
    uint32_t green_dominant = 0;
    uint32_t blue_dominant = 0;
    for (UINT y = 0; y < kTessellationProgressiveRegionHeight; ++y) {
        for (UINT x = 0; x < kTessellationProgressiveRegionWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kTessellationProgressiveRegionX + x,
                                                  kTessellationProgressiveRegionY + y);
            stats.progressive_triangle_samples_checked++;
            const uint32_t color_sum =
                static_cast<uint32_t>(pixel[0]) + static_cast<uint32_t>(pixel[1]) + static_cast<uint32_t>(pixel[2]);
            const bool covered =
                pixel[3] >= 240u && color_sum >= 96u && (pixel[0] >= 32u || pixel[1] >= 32u || pixel[2] >= 32u);
            if (!covered)
                continue;
            covered_pixels++;
            if (pixel[0] > pixel[1] + 24u && pixel[0] > pixel[2] + 24u)
                red_dominant++;
            if (pixel[1] > pixel[0] + 24u && pixel[1] > pixel[2] + 24u)
                green_dominant++;
            if (pixel[2] > pixel[0] + 24u && pixel[2] > pixel[1] + 24u)
                blue_dominant++;
        }
    }
    stats.progressive_triangle_coverage_counts.push_back(covered_pixels);
    if (stats.progressive_triangle_frames_validated == 0u) {
        stats.progressive_triangle_seed_pixels = covered_pixels;
    } else if (covered_pixels <
               stats.progressive_triangle_coverage_counts[stats.progressive_triangle_coverage_counts.size() - 2u]) {
        stats.progressive_triangle_coverage_monotonic = false;
    }
    stats.progressive_triangle_peak_pixels = std::max(stats.progressive_triangle_peak_pixels, covered_pixels);
    stats.progressive_triangle_frames_validated++;
    if (stats.progressive_triangle_target_frames != 0u &&
        stats.progressive_triangle_frames_validated == stats.progressive_triangle_target_frames) {
        stats.progressive_triangle_final_pixels = covered_pixels;
        stats.progressive_triangle_final_red_dominant_pixels = red_dominant;
        stats.progressive_triangle_final_green_dominant_pixels = green_dominant;
        stats.progressive_triangle_final_blue_dominant_pixels = blue_dominant;
    }

    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches && indexed_matches && covered_pixels > 0u && stats.progressive_triangle_coverage_monotonic;
}

static bool inspect_indirect_draw_stamp(DxilReadbackResources& readback, IndirectDrawStats& stats) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* first = readback_pixel(readback, mapped, kIndirectStampX, kIndirectStampY);
    std::memcpy(stats.present_rgba, first, sizeof(stats.present_rgba));
    const uint8_t* last = readback_pixel(readback, mapped, kIndirectStampX + kFreshTextureWidth - 1u,
                                         kIndirectStampY + kFreshTextureHeight - 1u);
    std::memcpy(stats.present_last_rgba, last, sizeof(stats.present_last_rgba));
    stats.present_samples_checked++;
    uint32_t frame_matches = 0;
    for (UINT y = 0; y < kFreshTextureHeight; ++y) {
        for (UINT x = 0; x < kFreshTextureWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kIndirectStampX + x, kIndirectStampY + y);
            stats.present_pixels_checked++;
            if (pixel[0] == stats.expected_rgba[0] && pixel[1] == stats.expected_rgba[1] &&
                pixel[2] == stats.expected_rgba[2] && pixel[3] == stats.expected_rgba[3]) {
                stats.present_pixel_matches++;
                frame_matches++;
            }
        }
    }
    const bool matches = frame_matches == kFreshTextureWidth * kFreshTextureHeight;
    if (matches)
        stats.present_sample_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static bool inspect_progressive_rgb_triangle(DxilReadbackResources& readback, VisibleSceneStats& visible_stats,
                                             uint32_t frame) {
    if (!readback.buffer)
        return false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;

    uint32_t covered_pixels = 0;
    uint32_t red_dominant = 0;
    uint32_t green_dominant = 0;
    uint32_t blue_dominant = 0;
    for (UINT y = 0; y < kProgressiveVertexTriangleRegionHeight; ++y) {
        for (UINT x = 0; x < kProgressiveVertexTriangleRegionWidth; ++x) {
            const uint8_t* pixel = readback_pixel(readback, mapped, kProgressiveVertexTriangleRegionX + x,
                                                  kProgressiveVertexTriangleRegionY + y);
            visible_stats.progressive_triangle_samples_checked++;
            const uint32_t color_sum =
                static_cast<uint32_t>(pixel[0]) + static_cast<uint32_t>(pixel[1]) + static_cast<uint32_t>(pixel[2]);
            const bool covered =
                pixel[3] >= 240u && color_sum >= 96u && (pixel[0] >= 32u || pixel[1] >= 32u || pixel[2] >= 32u);
            if (!covered)
                continue;
            covered_pixels++;
            if (pixel[0] > pixel[1] + 24u && pixel[0] > pixel[2] + 24u)
                red_dominant++;
            if (pixel[1] > pixel[0] + 24u && pixel[1] > pixel[2] + 24u)
                green_dominant++;
            if (pixel[2] > pixel[0] + 24u && pixel[2] > pixel[1] + 24u)
                blue_dominant++;
        }
    }

    visible_stats.progressive_triangle_coverage_counts.push_back(covered_pixels);
    if (visible_stats.progressive_triangle_frames_validated == 0u) {
        visible_stats.progressive_triangle_seed_pixels = covered_pixels;
    } else if (covered_pixels <
               visible_stats
                   .progressive_triangle_coverage_counts[visible_stats.progressive_triangle_coverage_counts.size() -
                                                         2u]) {
        visible_stats.progressive_triangle_coverage_monotonic = false;
    }
    visible_stats.progressive_triangle_peak_pixels =
        std::max(visible_stats.progressive_triangle_peak_pixels, covered_pixels);
    visible_stats.progressive_triangle_frames_validated++;
    if (visible_stats.target_frames != 0u && frame + 1u == visible_stats.target_frames) {
        visible_stats.progressive_triangle_final_pixels = covered_pixels;
        visible_stats.progressive_triangle_final_red_dominant_pixels = red_dominant;
        visible_stats.progressive_triangle_final_green_dominant_pixels = green_dominant;
        visible_stats.progressive_triangle_final_blue_dominant_pixels = blue_dominant;
    }

    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return covered_pixels > 0u && visible_stats.progressive_triangle_coverage_monotonic;
}

static bool channel_matches_expected(uint8_t actual, uint8_t expected) {
    return expected >= 128 ? actual >= 180 : actual <= 80;
}

static bool inspect_sm5_stamp(DxilReadbackResources& readback, VisibleSceneStats& visible_stats, uint32_t frame) {
    if (!readback.buffer)
        return false;
    uint8_t expected[4] = {};
    sm5_expected_stamp_rgba(frame, expected);
    uint8_t* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(readback.total_bytes)};
    if (FAILED(readback.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped)
        return false;
    const uint8_t* pixel = readback_pixel(readback, mapped, kSm5StampX, kSm5StampY);
    std::memcpy(visible_stats.sm5_stamp_expected_rgba, expected, sizeof(visible_stats.sm5_stamp_expected_rgba));
    std::memcpy(visible_stats.sm5_stamp_rgba, pixel, sizeof(visible_stats.sm5_stamp_rgba));
    visible_stats.sm5_stamp_samples_checked++;
    const bool matches =
        channel_matches_expected(pixel[0], expected[0]) && channel_matches_expected(pixel[1], expected[1]) &&
        channel_matches_expected(pixel[2], expected[2]) && channel_matches_expected(pixel[3], expected[3]);
    if (matches)
        visible_stats.sm5_stamp_matches++;
    D3D12_RANGE written = {0, 0};
    readback.buffer->Unmap(0, &written);
    return matches;
}

static GpuTextureExercise exercise_corpus_gpu_textures(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                       ID3D12CommandAllocator* allocator,
                                                       ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                                                       HANDLE fence_event, UINT64& fence_value,
                                                       const std::vector<TexturePayload>& texture_payloads) {
    GpuTextureExercise exercise;
    GpuTextureStats& stats = exercise.stats;
    if (!device || !queue || !allocator || !list || !fence || !fence_event)
        return exercise;

    constexpr uint32_t max_textures = 300;
    const uint32_t texture_count = std::min<uint32_t>(max_textures, static_cast<uint32_t>(texture_payloads.size()));
    stats.textures_requested = texture_count;
    if (!texture_count)
        return exercise;
    std::memcpy(stats.present_expected_rgba, texture_payloads[0].rgba.data(), sizeof(stats.present_expected_rgba));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = texture_count;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    stats.create_descriptor_heap_hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap));
    if (FAILED(stats.create_descriptor_heap_hr))
        return exercise;

    const UINT descriptor_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_DESC tex_desc = texture_desc(kFreshTextureWidth, kFreshTextureHeight, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);

    std::vector<ID3D12Resource*> textures(texture_count, nullptr);
    std::vector<ID3D12Resource*> uploads(texture_count, nullptr);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(texture_count);

    UINT sentinel_rows = 0;
    UINT64 sentinel_row_bytes = 0;
    UINT64 sentinel_upload_bytes = 0;
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &exercise.present_sentinel_footprint, &sentinel_rows,
                                  &sentinel_row_bytes, &sentinel_upload_bytes);
    D3D12_RESOURCE_DESC sentinel_upload_desc = buffer_desc(sentinel_upload_bytes);
    HRESULT sentinel_hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &sentinel_upload_desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&exercise.present_sentinel_upload));
    if (SUCCEEDED(sentinel_hr) && exercise.present_sentinel_upload) {
        TexturePayload sentinel_payload;
        for (size_t i = 0; i < sentinel_payload.rgba.size(); i += 4) {
            sentinel_payload.rgba[i + 0] = static_cast<uint8_t>(stats.present_expected_rgba[0] ^ 0xffu);
            sentinel_payload.rgba[i + 1] = static_cast<uint8_t>(stats.present_expected_rgba[1] ^ 0xffu);
            sentinel_payload.rgba[i + 2] = static_cast<uint8_t>(stats.present_expected_rgba[2] ^ 0xffu);
            sentinel_payload.rgba[i + 3] = static_cast<uint8_t>(stats.present_expected_rgba[3] ^ 0xffu);
        }
        fill_texture_upload(exercise.present_sentinel_upload, exercise.present_sentinel_footprint, sentinel_payload,
                            kFreshTextureWidth, kFreshTextureHeight);
    }

    bool creation_ok = SUCCEEDED(sentinel_hr) && exercise.present_sentinel_upload != nullptr;
    for (uint32_t i = 0; i < texture_count; ++i) {
        HRESULT hr =
            device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textures[i]));
        if (FAILED(hr)) {
            creation_ok = false;
            break;
        }
        stats.textures_created++;

        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 upload_bytes = 0;
        device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &footprints[i], &rows, &row_bytes, &upload_bytes);
        D3D12_RESOURCE_DESC upload_desc = buffer_desc(upload_bytes);
        hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploads[i]));
        if (FAILED(hr)) {
            creation_ok = false;
            break;
        }
        stats.upload_buffers_created++;
        stats.upload_bytes += upload_bytes;
        if (fill_texture_upload(uploads[i], footprints[i], texture_payloads[i], kFreshTextureWidth,
                                kFreshTextureHeight)) {
            stats.texture_payloads_uploaded++;
            stats.texture_payload_bytes_from_files += texture_payloads[i].bytes_from_file;
            stats.upload_payload_fnv1a64 = fnv1a_update(stats.upload_payload_fnv1a64, texture_payloads[i].rgba.data(),
                                                        texture_payloads[i].rgba.size());
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(textures[i], &srv_desc, offset_cpu(srv_cpu, descriptor_increment, i));
        stats.srv_descriptors_created++;
    }

    if (creation_ok && SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator, nullptr))) {
        for (uint32_t i = 0; i < texture_count; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = textures[i];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = uploads[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[i];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            stats.copy_texture_region_commands++;
            D3D12_RESOURCE_BARRIER barrier = transition_barrier(textures[i], D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &barrier);
            stats.transition_barriers++;
        }
        stats.close_hr = list->Close();
        if (SUCCEEDED(stats.close_hr)) {
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            fence_value++;
            stats.signal_hr = queue->Signal(fence, fence_value);
            stats.fence_wait_ok = SUCCEEDED(stats.signal_hr) && wait_for_fence(fence, fence_value, fence_event);
        }
    }

    stats.pass =
        creation_ok && stats.textures_created == texture_count && stats.upload_buffers_created == texture_count &&
        stats.srv_descriptors_created == texture_count && stats.texture_payloads_uploaded == texture_count &&
        stats.texture_payload_bytes_from_files >= static_cast<uint64_t>(texture_count) * kFreshTexturePayloadBytes &&
        stats.copy_texture_region_commands == texture_count && stats.transition_barriers == texture_count &&
        SUCCEEDED(stats.close_hr) && SUCCEEDED(stats.signal_hr) && stats.fence_wait_ok;

    if (stats.pass && !textures.empty()) {
        exercise.present_texture = textures[0];
        textures[0] = nullptr;
    }

    for (auto*& upload : uploads)
        safe_release(upload);
    for (auto*& texture : textures)
        safe_release(texture);
    safe_release(srv_heap);
    return exercise;
}

struct D3DRunStats {
    HRESULT create_factory_hr = E_FAIL;
    HRESULT enum_adapter_hr = E_FAIL;
    HRESULT adapter_desc_hr = E_FAIL;
    HRESULT create_device_hr = E_FAIL;
    HRESULT create_queue_hr = E_FAIL;
    HRESULT create_swapchain_hr = E_FAIL;
    HRESULT create_rtv_heap_hr = E_FAIL;
    HRESULT create_allocator_hr = E_FAIL;
    HRESULT create_list_hr = E_FAIL;
    HRESULT create_fence_hr = E_FAIL;
    HRESULT present_hr = E_FAIL;
    GpuTextureStats gpu_textures;
    HeapAliasStats heap_alias;
    UavBarrierStats uav_barrier;
    WaveOpsStats wave_ops;
    RtvFormatStats rtv_format;
    SubresourceViewStats subresource_views;
    TextureArraySrvSampleStats texture_array_srv_sample;
    RenderPassStats render_pass;
    CorpusShaderStats corpus_shader;
    SrvSampleStats srv_sample;
    Textured3DStats textured_3d;
    CbvSampleStats cbv_sample;
    IndexedDrawStats indexed_draw;
    MultiSlotInstanceRootStats multi_slot_instance_root;
    TessellationFallbackStats tessellation_fallback;
    IndirectDrawStats indirect_draw;
    NaniteClusterStats nanite_cluster;
    VisibleSceneStats visible_scene;
    DxilSceneStats dxil_scene;
    DxilHazardReplayStats dxil_hazard_replay;
    DxilReadbackStats dxil_readback;
    AbiSemanticStats abi_semantics;
    uint32_t frames_presented = 0;
    uint32_t adapter_vendor_id = 0;
    uint32_t adapter_device_id = 0;
    uint64_t adapter_dedicated_video_memory = 0;
    uint64_t adapter_shared_system_memory = 0;
    LUID adapter_luid = {};
    LUID device_luid = {};
    std::string adapter_description;
    bool adapter_luid_nonzero = false;
    bool device_luid_nonzero = false;
    bool adapter_luid_matches_device = false;
    bool adapter_report_pass = false;
    bool hwnd_created = false;
    bool pass = false;
};

static D3DRunStats run_d3d_window(const CorpusStats& corpus) {
    D3DRunStats stats;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"MetalSharpVKD3DFreshGameWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = game_window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = class_name;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, class_name, L"MetalSharp VKD3D Fresh Proof Game", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 540, nullptr, nullptr, instance, nullptr);
    stats.hwnd_created = hwnd != nullptr;
    if (!hwnd)
        return stats;
    ShowWindow(hwnd, SW_SHOW);
    pump_messages();

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    auto create_device = reinterpret_cast<D3D12CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));
    auto create_factory2 = reinterpret_cast<CreateDXGIFactory2Fn>(
        reinterpret_cast<void*>(dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory2") : nullptr));
    auto compile = reinterpret_cast<D3DCompileFn>(
        reinterpret_cast<void*>(d3dcompiler ? GetProcAddress(d3dcompiler, "D3DCompile") : nullptr));
    auto serialize = reinterpret_cast<SerializeRootSignatureFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr));

    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* swapchain1 = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;

    stats.create_factory_hr = create_factory2 ? create_factory2(0, IID_PPV_ARGS(&factory)) : E_FAIL;
    stats.enum_adapter_hr = factory ? factory->EnumAdapters1(0, &adapter) : E_FAIL;
    DXGI_ADAPTER_DESC1 adapter_desc = {};
    stats.adapter_desc_hr = adapter ? adapter->GetDesc1(&adapter_desc) : E_FAIL;
    if (SUCCEEDED(stats.adapter_desc_hr)) {
        stats.adapter_vendor_id = adapter_desc.VendorId;
        stats.adapter_device_id = adapter_desc.DeviceId;
        stats.adapter_dedicated_video_memory = adapter_desc.DedicatedVideoMemory;
        stats.adapter_shared_system_memory = adapter_desc.SharedSystemMemory;
        stats.adapter_luid = adapter_desc.AdapterLuid;
        stats.adapter_description = narrow_wide(adapter_desc.Description);
    }
    stats.create_device_hr = create_device ? create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceFresh,
                                                           reinterpret_cast<void**>(&device))
                                           : E_FAIL;
    stats.adapter_luid_nonzero = stats.adapter_luid.HighPart != 0 || stats.adapter_luid.LowPart != 0;
    if (device) {
        stats.device_luid = device->GetAdapterLuid();
        stats.device_luid_nonzero = stats.device_luid.HighPart != 0 || stats.device_luid.LowPart != 0;
        stats.adapter_luid_matches_device = stats.adapter_luid.HighPart == stats.device_luid.HighPart &&
                                            stats.adapter_luid.LowPart == stats.device_luid.LowPart;
    }
    stats.adapter_report_pass =
        SUCCEEDED(stats.enum_adapter_hr) && SUCCEEDED(stats.adapter_desc_hr) && stats.adapter_vendor_id != 0 &&
        (stats.adapter_dedicated_video_memory + stats.adapter_shared_system_memory) > 0 && stats.adapter_luid_nonzero &&
        stats.device_luid_nonzero && stats.adapter_luid_matches_device;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    stats.create_queue_hr = device ? device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)) : E_FAIL;

    DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
    swap_desc.Width = 960;
    swap_desc.Height = 540;
    swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    stats.create_swapchain_hr =
        (factory && queue) ? factory->CreateSwapChainForHwnd(queue, hwnd, &swap_desc, nullptr, nullptr, &swapchain1)
                           : E_FAIL;
    if (SUCCEEDED(stats.create_swapchain_hr) && swapchain1)
        swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain));

    constexpr UINT backbuffer_width = kBackbufferWidth;
    constexpr UINT backbuffer_height = kBackbufferHeight;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 2;
    stats.create_rtv_heap_hr = device ? device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)) : E_FAIL;
    UINT rtv_increment = device ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
        rtv_heap ? rtv_heap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};

    std::array<ID3D12Resource*, 2> buffers = {nullptr, nullptr};
    if (swapchain && rtv_heap) {
        for (UINT i = 0; i < 2; ++i) {
            if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]))))
                device->CreateRenderTargetView(buffers[i], nullptr, offset_cpu(rtv_start, rtv_increment, i));
        }
    }

    stats.create_allocator_hr =
        device ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) : E_FAIL;
    stats.create_list_hr =
        device ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))
               : E_FAIL;
    if (SUCCEEDED(stats.create_list_hr) && list)
        list->Close();
    stats.create_fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) : E_FAIL;
    stats.abi_semantics = exercise_abi_semantics(factory, adapter, device, queue, allocator, list, fence, swapchain);

    const uint32_t visible_frame_target = getenv_u32("VKD3D_FRESH_VISIBLE_FRAMES", 600);
    VisibleSceneResources visible_scene = create_visible_scene(device, compile, serialize, visible_frame_target);
    stats.visible_scene = visible_scene.stats;
    const std::string dxil_vs_path = getenv_string("VKD3D_FRESH_DXIL_VS");
    DxilSceneResources dxil_scene =
        create_dxil_scene(device, serialize, dxil_vs_path, getenv_string("VKD3D_FRESH_DXIL_PS"));
    stats.dxil_scene = dxil_scene.stats;
    std::vector<std::string> dxil_hazard_ps_paths;
    for (uint32_t hazard_index = 0; hazard_index < 64; ++hazard_index) {
        std::string path = getenv_string(("VKD3D_FRESH_DXIL_HAZARD_PS" + std::to_string(hazard_index)).c_str());
        if (!path.empty())
            dxil_hazard_ps_paths.push_back(path);
    }
    stats.dxil_hazard_replay = create_dxil_hazard_replay(device, serialize, dxil_vs_path, dxil_hazard_ps_paths);
    CorpusShaderSceneResources corpus_shader =
        create_corpus_shader_scene(device, compile, serialize, corpus, backbuffer_width, backbuffer_height);
    stats.corpus_shader = corpus_shader.stats;
    DxilReadbackResources dxil_readback = create_dxil_readback(device, backbuffer_width, backbuffer_height);
    stats.dxil_readback = dxil_readback.stats;

    GpuTextureExercise gpu_textures = exercise_corpus_gpu_textures(device, queue, allocator, list, fence, fence_event,
                                                                   fence_value, corpus.texture_payloads);
    stats.gpu_textures = gpu_textures.stats;
    HeapAliasExercise heap_alias =
        exercise_heap_alias_stamp(device, queue, allocator, list, fence, fence_event, fence_value);
    stats.heap_alias = heap_alias.stats;
    UavBarrierExercise uav_barrier =
        exercise_uav_barrier_stamp(device, compile, serialize, queue, allocator, list, fence, fence_event, fence_value);
    stats.uav_barrier = uav_barrier.stats;
    WaveOpsExercise wave_ops = exercise_wave_ops_stamp(device, serialize, queue, allocator, list, fence, fence_event,
                                                       fence_value, getenv_string("VKD3D_FRESH_WAVEOPS_CS"));
    stats.wave_ops = wave_ops.stats;
    RtvFormatExercise rtv_format =
        exercise_rtv_format_stamp(device, queue, allocator, list, fence, fence_event, fence_value);
    stats.rtv_format = rtv_format.stats;
    SubresourceViewExercise subresource_views =
        exercise_subresource_view_stamp(device, queue, allocator, list, fence, fence_event, fence_value);
    stats.subresource_views = subresource_views.stats;
    TextureArraySrvSampleSceneResources texture_array_srv_sample =
        create_texture_array_srv_sample_scene(device, compile, serialize, queue, allocator, list, fence, fence_event,
                                              fence_value, backbuffer_width, backbuffer_height);
    stats.texture_array_srv_sample = texture_array_srv_sample.stats;
    RenderPassExercise render_pass =
        exercise_render_pass_stamp(device, queue, allocator, list, fence, fence_event, fence_value);
    stats.render_pass = render_pass.stats;
    SrvSampleSceneResources srv_sample =
        create_srv_sample_scene(device, compile, serialize, queue, allocator, list, fence, fence_event, fence_value,
                                backbuffer_width, backbuffer_height);
    stats.srv_sample = srv_sample.stats;
    Textured3DSceneResources textured_3d =
        create_textured3d_scene(device, compile, serialize, queue, allocator, list, fence, fence_event, fence_value,
                                corpus, backbuffer_width, backbuffer_height);
    stats.textured_3d = textured_3d.stats;
    CbvSampleSceneResources cbv_sample =
        create_cbv_sample_scene(device, compile, serialize, backbuffer_width, backbuffer_height);
    stats.cbv_sample = cbv_sample.stats;
    IndexedDrawSceneResources indexed_draw =
        create_indexed_draw_scene(device, compile, serialize, backbuffer_width, backbuffer_height);
    stats.indexed_draw = indexed_draw.stats;
    MultiSlotInstanceRootSceneResources multi_slot_instance_root =
        create_multi_slot_instance_root_scene(device, compile, serialize, backbuffer_width, backbuffer_height);
    stats.multi_slot_instance_root = multi_slot_instance_root.stats;
    TessellationFallbackSceneResources tessellation_fallback =
        create_tessellation_fallback_scene(device, compile, serialize, backbuffer_width, backbuffer_height);
    stats.tessellation_fallback = tessellation_fallback.stats;
    stats.tessellation_fallback.progressive_triangle_target_frames = visible_frame_target;
    IndirectDrawSceneResources indirect_draw =
        create_indirect_draw_scene(device, compile, serialize, backbuffer_width, backbuffer_height);
    stats.indirect_draw = indirect_draw.stats;
    NaniteClusterSceneResources nanite_cluster =
        create_nanite_cluster_scene(device, compile, serialize, queue, allocator, list, fence, fence_event, fence_value,
                                    backbuffer_width, backbuffer_height);
    stats.nanite_cluster = nanite_cluster.stats;

    const float colors[3][4] = {{0.01f, 0.02f, 0.05f, 1.0f}, {0.01f, 0.02f, 0.05f, 1.0f}, {0.01f, 0.02f, 0.05f, 1.0f}};
    if (swapchain && allocator && list && queue && fence && fence_event && visible_scene.stats.pass &&
        dxil_scene.stats.pass && stats.dxil_hazard_replay.pass && corpus_shader.stats.pass && srv_sample.stats.pass &&
        texture_array_srv_sample.stats.pass && textured_3d.stats.pass && cbv_sample.stats.pass &&
        indexed_draw.stats.pass && multi_slot_instance_root.stats.pass && tessellation_fallback.stats.pass &&
        indirect_draw.stats.pass && nanite_cluster.stats.pass && wave_ops.stats.pass) {
        for (UINT frame = 0; frame < visible_frame_target; ++frame) {
            pump_messages();
            UINT index = swapchain->GetCurrentBackBufferIndex();
            ID3D12Resource* buffer = buffers[index];
            if (!buffer)
                break;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr)))
                break;
            auto to_rtv = transition_barrier(buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &to_rtv);
            auto rtv = offset_cpu(rtv_start, rtv_increment, index);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->ClearRenderTargetView(rtv, colors[frame % 3], 0, nullptr);
            D3D12_VIEWPORT viewport = {
                0.0f, 0.0f, static_cast<float>(backbuffer_width), static_cast<float>(backbuffer_height), 0.0f, 1.0f};
            D3D12_RECT scissor = {0, 0, static_cast<LONG>(backbuffer_width), static_cast<LONG>(backbuffer_height)};
            const uint32_t visible_vertices = populate_visible_loading_vertices(visible_scene, frame, corpus);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(visible_scene.root_signature);
            list->SetPipelineState(visible_scene.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &visible_scene.vertex_view);
            list->DrawInstanced(visible_vertices, 1, 0, 0);
            stats.visible_scene.draw_calls++;
            list->SetGraphicsRootSignature(dxil_scene.root_signature);
            list->SetPipelineState(dxil_scene.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &dxil_scene.vertex_view);
            list->DrawInstanced(3, 1, 0, 0);
            stats.dxil_scene.draw_calls++;
            list->SetGraphicsRootSignature(corpus_shader.root_signature);
            list->SetPipelineState(corpus_shader.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &corpus_shader.vertex_view);
            list->DrawInstanced(corpus_shader.stats.vertices_per_draw, 1, 0, 0);
            stats.corpus_shader.draw_calls++;
            ID3D12DescriptorHeap* srv_heaps[] = {srv_sample.srv_heap};
            list->SetDescriptorHeaps(1, srv_heaps);
            list->SetGraphicsRootSignature(srv_sample.root_signature);
            list->SetPipelineState(srv_sample.pipeline_state);
            list->SetGraphicsRootDescriptorTable(0, srv_sample.srv_heap->GetGPUDescriptorHandleForHeapStart());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &srv_sample.vertex_view);
            list->DrawInstanced(srv_sample.stats.vertices_per_draw, 1, 0, 0);
            stats.srv_sample.draw_calls++;
            ID3D12DescriptorHeap* texture_array_srv_heaps[] = {texture_array_srv_sample.srv_heap};
            list->SetDescriptorHeaps(1, texture_array_srv_heaps);
            list->SetGraphicsRootSignature(texture_array_srv_sample.root_signature);
            list->SetPipelineState(texture_array_srv_sample.pipeline_state);
            list->SetGraphicsRootDescriptorTable(
                0, texture_array_srv_sample.srv_heap->GetGPUDescriptorHandleForHeapStart());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &texture_array_srv_sample.vertex_view);
            list->DrawInstanced(texture_array_srv_sample.stats.vertices_per_draw, 1, 0, 0);
            stats.texture_array_srv_sample.draw_calls++;
            ID3D12DescriptorHeap* textured_3d_heaps[] = {textured_3d.srv_heap};
            list->SetDescriptorHeaps(1, textured_3d_heaps);
            list->OMSetRenderTargets(1, &rtv, FALSE, &textured_3d.dsv_handle);
            list->ClearDepthStencilView(textured_3d.dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            stats.textured_3d.clear_depth_commands++;
            list->SetGraphicsRootSignature(textured_3d.root_signature);
            list->SetPipelineState(textured_3d.pipeline_state);
            list->SetGraphicsRootDescriptorTable(0, textured_3d.srv_heap->GetGPUDescriptorHandleForHeapStart());
            if (!populate_textured3d_prism_vertices(textured_3d, stats.textured_3d, frame, backbuffer_width,
                                                    backbuffer_height))
                break;
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &textured_3d.vertex_view);
            list->DrawInstanced(textured_3d.stats.vertices_per_draw, 1, 0, 0);
            stats.textured_3d.draw_calls++;
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            ID3D12DescriptorHeap* cbv_heaps[] = {cbv_sample.cbv_heap};
            list->SetDescriptorHeaps(1, cbv_heaps);
            list->SetGraphicsRootSignature(cbv_sample.root_signature);
            list->SetPipelineState(cbv_sample.pipeline_state);
            list->SetGraphicsRootDescriptorTable(0, cbv_sample.cbv_heap->GetGPUDescriptorHandleForHeapStart());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &cbv_sample.vertex_view);
            list->DrawInstanced(cbv_sample.stats.vertices_per_draw, 1, 0, 0);
            stats.cbv_sample.draw_calls++;
            list->SetGraphicsRootSignature(indexed_draw.root_signature);
            list->SetPipelineState(indexed_draw.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &indexed_draw.vertex_view);
            list->IASetIndexBuffer(&indexed_draw.index_view);
            list->DrawIndexedInstanced(indexed_draw.stats.indices_created, 1, indexed_draw.stats.start_index_location,
                                       0, 0);
            stats.indexed_draw.draw_indexed_calls++;
            list->IASetIndexBuffer(&indexed_draw.index_view_r32);
            list->DrawIndexedInstanced(indexed_draw.stats.r32_indices_created, 1,
                                       indexed_draw.stats.r32_start_index_location,
                                       indexed_draw.stats.r32_base_vertex_location, 0);
            stats.indexed_draw.draw_indexed_r32_calls++;
            list->IASetIndexBuffer(&indexed_draw.index_view_negative_base);
            list->DrawIndexedInstanced(indexed_draw.stats.negative_base_indices_created, 1,
                                       indexed_draw.stats.negative_base_start_index_location,
                                       indexed_draw.stats.negative_base_vertex_location, 0);
            stats.indexed_draw.draw_indexed_negative_base_calls++;
            list->IASetVertexBuffers(0, 1, &indexed_draw.vertex_view_dynamic_stride);
            list->IASetIndexBuffer(&indexed_draw.index_view);
            list->DrawIndexedInstanced(indexed_draw.stats.indices_created, 1, indexed_draw.stats.start_index_location,
                                       0, 0);
            stats.indexed_draw.draw_indexed_dynamic_stride_calls++;
            list->IASetVertexBuffers(0, 1, &indexed_draw.vertex_view_execute_indirect);
            list->IASetIndexBuffer(&indexed_draw.index_view_execute_indirect);
            list->ExecuteIndirect(indexed_draw.execute_indirect_indexed_command_signature,
                                  indexed_draw.stats.execute_indirect_indexed_max_command_count,
                                  indexed_draw.execute_indirect_indexed_argument_buffer, 0, nullptr, 0);
            stats.indexed_draw.execute_indirect_indexed_calls++;
            list->SetGraphicsRootSignature(multi_slot_instance_root.root_signature);
            list->SetPipelineState(multi_slot_instance_root.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 2, multi_slot_instance_root.vertex_views);
            list->SetGraphicsRoot32BitConstants(0, multi_slot_instance_root.stats.root_constant_float_count,
                                                multi_slot_instance_root.root_constants_first, 0);
            stats.multi_slot_instance_root.root_constant_sets++;
            list->DrawInstanced(multi_slot_instance_root.stats.vertex_count_per_draw,
                                multi_slot_instance_root.stats.instance_count_per_draw, 0,
                                multi_slot_instance_root.stats.start_instance_location);
            stats.multi_slot_instance_root.draw_calls++;
            stats.multi_slot_instance_root.first_draw_calls++;
            list->SetGraphicsRoot32BitConstants(0, multi_slot_instance_root.stats.root_constant_float_count,
                                                multi_slot_instance_root.root_constants_second, 0);
            stats.multi_slot_instance_root.root_constant_sets++;
            list->IASetVertexBuffers(0, 2, multi_slot_instance_root.second_vertex_views);
            list->DrawInstanced(multi_slot_instance_root.stats.vertex_count_per_draw,
                                multi_slot_instance_root.stats.instance_count_per_draw, 0,
                                multi_slot_instance_root.stats.start_instance_location);
            stats.multi_slot_instance_root.draw_calls++;
            stats.multi_slot_instance_root.second_draw_calls++;
            if (tessellation_fallback.pipeline_state &&
                populate_tessellation_native_vertices(tessellation_fallback, stats.tessellation_fallback, frame,
                                                      visible_frame_target, backbuffer_width, backbuffer_height)) {
                list->SetGraphicsRootSignature(tessellation_fallback.root_signature);
                list->SetPipelineState(tessellation_fallback.pipeline_state);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
                list->IASetVertexBuffers(0, 1, &tessellation_fallback.vertex_view);
                list->DrawInstanced(stats.tessellation_fallback.vertices_per_draw, 1, 0, 0);
                stats.tessellation_fallback.draw_calls++;
                stats.tessellation_fallback.native_tessellation_resolved = true;
                stats.tessellation_fallback.native_draw_encoded = true;
                stats.tessellation_fallback.fallback_draw_encoded = false;
                list->IASetIndexBuffer(&tessellation_fallback.index_view);
                list->DrawIndexedInstanced(stats.tessellation_fallback.indices_per_draw, 1,
                                           kTessellationNonIndexedVertexCount, 0, 0);
                stats.tessellation_fallback.draw_indexed_calls++;
                stats.tessellation_fallback.native_indexed_draw_encoded = true;
            } else {
                stats.tessellation_fallback.native_tessellation_required = true;
                stats.tessellation_fallback.blocked_expected = true;
                stats.tessellation_fallback.fallback_draw_encoded = false;
            }
            list->SetGraphicsRootSignature(indirect_draw.root_signature);
            list->SetPipelineState(indirect_draw.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &indirect_draw.vertex_view);
            list->ExecuteIndirect(indirect_draw.command_signature, indirect_draw.stats.max_command_count,
                                  indirect_draw.argument_buffer, 0, nullptr, 0);
            stats.indirect_draw.execute_indirect_calls++;
            list->SetGraphicsRootSignature(nanite_cluster.root_signature);
            list->SetPipelineState(nanite_cluster.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &nanite_cluster.vertex_view);
            list->ExecuteIndirect(nanite_cluster.command_signature, nanite_cluster.stats.max_command_count,
                                  nanite_cluster.indirect_argument_buffer, 0, nullptr, 0);
            stats.nanite_cluster.execute_indirect_calls++;
            list->SetGraphicsRootSignature(visible_scene.root_signature);
            list->SetPipelineState(visible_scene.pipeline_state);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->IASetVertexBuffers(0, 1, &visible_scene.progressive_triangle_vertex_view);
            list->DrawInstanced(visible_scene.stats.progressive_triangle_vertices_per_frame, 1, 0, 0);
            stats.visible_scene.progressive_triangle_draw_calls++;
            D3D12_RESOURCE_STATES backbuffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            if (gpu_textures.present_texture && gpu_textures.present_sentinel_upload) {
                auto backbuffer_to_copy_dest =
                    transition_barrier(buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                D3D12_TEXTURE_COPY_LOCATION stamp_dst = {};
                stamp_dst.pResource = buffer;
                stamp_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                stamp_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION sentinel_src = {};
                sentinel_src.pResource = gpu_textures.present_sentinel_upload;
                sentinel_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sentinel_src.PlacedFootprint = gpu_textures.present_sentinel_footprint;
                D3D12_BOX stamp_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&stamp_dst, kTextureStampX, kTextureStampY, 0, &sentinel_src, &stamp_box);
                stats.gpu_textures.present_backbuffer_sentinel_copies++;
                auto texture_to_copy =
                    transition_barrier(gpu_textures.present_texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1, &texture_to_copy);
                D3D12_TEXTURE_COPY_LOCATION stamp_src = {};
                stamp_src.pResource = gpu_textures.present_texture;
                stamp_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                stamp_src.SubresourceIndex = 0;
                list->CopyTextureRegion(&stamp_dst, kTextureStampX, kTextureStampY, 0, &stamp_src, &stamp_box);
                stats.gpu_textures.present_copy_commands++;
                auto texture_to_srv = transition_barrier(gpu_textures.present_texture, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                list->ResourceBarrier(1, &texture_to_srv);
                backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            if (heap_alias.present_buffer) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION heap_dst = {};
                heap_dst.pResource = buffer;
                heap_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                heap_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION heap_src = {};
                heap_src.pResource = heap_alias.present_buffer;
                heap_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                heap_src.PlacedFootprint = heap_alias.footprint;
                D3D12_BOX heap_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&heap_dst, kHeapAliasStampX, kHeapAliasStampY, 0, &heap_src, &heap_box);
                stats.heap_alias.present_copy_commands++;
            }
            if (uav_barrier.present_buffer) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION uav_dst = {};
                uav_dst.pResource = buffer;
                uav_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                uav_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION uav_src = {};
                uav_src.pResource = uav_barrier.present_buffer;
                uav_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                uav_src.PlacedFootprint = uav_barrier.footprint;
                D3D12_BOX uav_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&uav_dst, kUavBarrierStampX, kUavBarrierStampY, 0, &uav_src, &uav_box);
                stats.uav_barrier.present_copy_commands++;
            }
            if (wave_ops.present_buffer) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION wave_dst = {};
                wave_dst.pResource = buffer;
                wave_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                wave_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION wave_src = {};
                wave_src.pResource = wave_ops.present_buffer;
                wave_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                wave_src.SubresourceIndex = 0;
                D3D12_BOX wave_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&wave_dst, kWaveOpsStampX, kWaveOpsStampY, 0, &wave_src, &wave_box);
                stats.wave_ops.present_copy_commands++;
            }
            if (stats.rtv_format.pass && rtv_format.present_texture) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION rtv_dst = {};
                rtv_dst.pResource = buffer;
                rtv_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                rtv_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION rtv_src = {};
                rtv_src.pResource = rtv_format.present_texture;
                rtv_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                rtv_src.SubresourceIndex = 0;
                D3D12_BOX rtv_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&rtv_dst, kRtvFormatStampX, kRtvFormatStampY, 0, &rtv_src, &rtv_box);
                stats.rtv_format.present_copy_commands++;
            }
            if (stats.subresource_views.pass && subresource_views.present_texture) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION subresource_dst = {};
                subresource_dst.pResource = buffer;
                subresource_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                subresource_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION subresource_src = {};
                subresource_src.pResource = subresource_views.present_texture;
                subresource_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                subresource_src.SubresourceIndex = 1;
                D3D12_BOX subresource_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&subresource_dst, kSubresourceViewStampX, kSubresourceViewStampY, 0,
                                        &subresource_src, &subresource_box);
                stats.subresource_views.present_copy_commands++;
            }
            if (stats.render_pass.pass && render_pass.present_texture) {
                if (backbuffer_state != D3D12_RESOURCE_STATE_COPY_DEST) {
                    auto backbuffer_to_copy_dest =
                        transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &backbuffer_to_copy_dest);
                    backbuffer_state = D3D12_RESOURCE_STATE_COPY_DEST;
                }
                D3D12_TEXTURE_COPY_LOCATION render_pass_dst = {};
                render_pass_dst.pResource = buffer;
                render_pass_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                render_pass_dst.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION render_pass_src = {};
                render_pass_src.pResource = render_pass.present_texture;
                render_pass_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                render_pass_src.SubresourceIndex = 0;
                D3D12_BOX render_pass_box = {0, 0, 0, kFreshTextureWidth, kFreshTextureHeight, 1};
                list->CopyTextureRegion(&render_pass_dst, kRenderPassStampX, kRenderPassStampY, 0, &render_pass_src,
                                        &render_pass_box);
                stats.render_pass.present_copy_commands++;
            }
            auto to_copy = transition_barrier(buffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            if (dxil_readback.buffer) {
                if (!seed_dxil_readback_sentinel(dxil_readback, backbuffer_width, backbuffer_height,
                                                 stats.gpu_textures.present_expected_rgba, frame, &stats.textured_3d))
                    break;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = dxil_readback.buffer;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = dxil_readback.footprint;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = buffer;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                dxil_readback.stats.copy_commands++;
            }
            auto to_present =
                transition_barrier(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            list->ResourceBarrier(1, &to_present);
            if (FAILED(list->Close()))
                break;
            ID3D12CommandList* base = list;
            queue->ExecuteCommandLists(1, &base);
            fence_value++;
            if (FAILED(queue->Signal(fence, fence_value)) || !wait_for_fence(fence, fence_value, fence_event))
                break;
            if (!inspect_dxil_scalar_vector_center(dxil_readback, backbuffer_width, backbuffer_height))
                break;
            if (gpu_textures.present_texture && !inspect_texture_stamp(dxil_readback, stats.gpu_textures))
                break;
            if (heap_alias.present_buffer && !inspect_heap_alias_stamp(dxil_readback, stats.heap_alias))
                break;
            if (uav_barrier.present_buffer && !inspect_uav_barrier_stamp(dxil_readback, stats.uav_barrier))
                break;
            if (wave_ops.present_buffer && !inspect_wave_ops_stamp(dxil_readback, stats.wave_ops))
                break;
            if (stats.rtv_format.pass && rtv_format.present_texture &&
                !inspect_rtv_format_stamp(dxil_readback, stats.rtv_format))
                break;
            if (stats.subresource_views.pass && subresource_views.present_texture &&
                !inspect_subresource_view_stamp(dxil_readback, stats.subresource_views))
                break;
            if (stats.render_pass.pass && render_pass.present_texture &&
                !inspect_render_pass_stamp(dxil_readback, stats.render_pass))
                break;
            if (!inspect_corpus_shader_stamp(dxil_readback, stats.corpus_shader))
                break;
            if (!inspect_srv_sample_stamp(dxil_readback, stats.srv_sample))
                break;
            if (!inspect_texture_array_srv_sample_stamp(dxil_readback, stats.texture_array_srv_sample))
                break;
            if (!inspect_textured3d_faces(dxil_readback, stats.textured_3d))
                break;
            if (!inspect_cbv_sample_stamp(dxil_readback, stats.cbv_sample))
                break;
            if (!inspect_indexed_draw_stamp(dxil_readback, stats.indexed_draw))
                break;
            if (!inspect_multi_slot_instance_root_stamp(dxil_readback, stats.multi_slot_instance_root))
                break;
            if (stats.tessellation_fallback.native_draw_encoded &&
                !inspect_tessellation_fallback_stamp(dxil_readback, stats.tessellation_fallback))
                break;
            if (!inspect_indirect_draw_stamp(dxil_readback, stats.indirect_draw))
                break;
            if (!inspect_nanite_cluster_stamp(dxil_readback, stats.nanite_cluster))
                break;
            if (!inspect_progressive_rgb_triangle(dxil_readback, stats.visible_scene, frame))
                break;
            if (!inspect_sm5_stamp(dxil_readback, stats.visible_scene, frame))
                break;
            stats.present_hr = swapchain->Present(0, 0);
            if (FAILED(stats.present_hr))
                break;
            stats.frames_presented++;
            Sleep(16);
        }
    }

    stats.visible_scene.quads_per_frame = visible_scene.stats.quads_per_frame;
    stats.visible_scene.vertices_per_frame = visible_scene.stats.vertices_per_frame;
    stats.visible_scene.progressive_triangle_vertices_per_frame =
        visible_scene.stats.progressive_triangle_vertices_per_frame;
    stats.visible_scene.sm5_stamp_quads_per_frame = visible_scene.stats.sm5_stamp_quads_per_frame;
    stats.visible_scene.sm5_stamp_present_pass =
        stats.visible_scene.sm5_stamp_quads_per_frame == 2 &&
        stats.visible_scene.sm5_stamp_samples_checked == visible_frame_target &&
        stats.visible_scene.sm5_stamp_matches == visible_frame_target;
    stats.visible_scene.progressive_triangle_seed_point_pass =
        stats.visible_scene.progressive_triangle_seed_pixels > 0u &&
        stats.visible_scene.progressive_triangle_seed_pixels <= 48u;
    stats.visible_scene.progressive_triangle_full_pass =
        stats.visible_scene.progressive_triangle_final_pixels >= kProgressiveVertexTriangleFinalCoverageMin &&
        stats.visible_scene.progressive_triangle_final_pixels >
            stats.visible_scene.progressive_triangle_seed_pixels * 100u;
    stats.visible_scene.progressive_triangle_rgb_channels_present =
        stats.visible_scene.progressive_triangle_final_red_dominant_pixels >= 64u &&
        stats.visible_scene.progressive_triangle_final_green_dominant_pixels >= 64u &&
        stats.visible_scene.progressive_triangle_final_blue_dominant_pixels >= 64u;
    stats.visible_scene.progressive_triangle_present_pass =
        stats.visible_scene.progressive_triangle_vertices_per_frame == 15u &&
        stats.visible_scene.progressive_triangle_draw_calls == visible_frame_target &&
        stats.visible_scene.progressive_triangle_frames_validated == visible_frame_target &&
        stats.visible_scene.progressive_triangle_coverage_counts.size() == visible_frame_target &&
        stats.visible_scene.progressive_triangle_coverage_monotonic &&
        stats.visible_scene.progressive_triangle_seed_point_pass &&
        stats.visible_scene.progressive_triangle_full_pass &&
        stats.visible_scene.progressive_triangle_rgb_channels_present;
    dxil_readback.stats.pass = SUCCEEDED(dxil_readback.stats.create_readback_hr) &&
                               dxil_readback.stats.copy_commands == visible_frame_target &&
                               dxil_readback.stats.sentinel_writes == visible_frame_target &&
                               dxil_readback.stats.samples_checked == visible_frame_target &&
                               dxil_readback.stats.semantic_samples == visible_frame_target;
    stats.dxil_readback = dxil_readback.stats;
    stats.gpu_textures.present_pass = gpu_textures.present_texture && gpu_textures.present_sentinel_upload &&
                                      stats.gpu_textures.present_backbuffer_sentinel_copies == visible_frame_target &&
                                      stats.gpu_textures.present_copy_commands == visible_frame_target &&
                                      stats.gpu_textures.present_samples_checked == visible_frame_target &&
                                      stats.gpu_textures.present_sample_matches == visible_frame_target;
    stats.heap_alias.present_pass = heap_alias.present_buffer &&
                                    stats.heap_alias.present_copy_commands == visible_frame_target &&
                                    stats.heap_alias.present_samples_checked == visible_frame_target &&
                                    stats.heap_alias.present_sample_matches == visible_frame_target;
    stats.uav_barrier.present_pass =
        uav_barrier.present_buffer && stats.uav_barrier.present_copy_commands == visible_frame_target &&
        stats.uav_barrier.present_samples_checked == visible_frame_target &&
        stats.uav_barrier.present_sample_matches == visible_frame_target &&
        stats.uav_barrier.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.uav_barrier.present_pixel_matches == stats.uav_barrier.present_pixels_checked;
    stats.wave_ops.present_pass =
        wave_ops.present_buffer && stats.wave_ops.present_copy_commands == visible_frame_target &&
        stats.wave_ops.present_samples_checked == visible_frame_target &&
        stats.wave_ops.present_sample_matches == visible_frame_target &&
        stats.wave_ops.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.wave_ops.present_pixel_matches == stats.wave_ops.present_pixels_checked;
    stats.rtv_format.present_pass =
        rtv_format.present_texture && stats.rtv_format.present_copy_commands == visible_frame_target &&
        stats.rtv_format.present_samples_checked == visible_frame_target &&
        stats.rtv_format.present_sample_matches == visible_frame_target &&
        stats.rtv_format.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.rtv_format.present_pixel_matches == stats.rtv_format.present_pixels_checked;
    stats.subresource_views.present_pass =
        subresource_views.present_texture && stats.subresource_views.present_copy_commands == visible_frame_target &&
        stats.subresource_views.present_samples_checked == visible_frame_target &&
        stats.subresource_views.present_sample_matches == visible_frame_target &&
        stats.subresource_views.present_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.subresource_views.present_pixel_matches == stats.subresource_views.present_pixels_checked;
    stats.render_pass.present_pass =
        render_pass.present_texture && stats.render_pass.present_copy_commands == visible_frame_target &&
        stats.render_pass.present_samples_checked == visible_frame_target &&
        stats.render_pass.present_sample_matches == visible_frame_target &&
        stats.render_pass.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.render_pass.present_pixel_matches == stats.render_pass.present_pixels_checked;
    stats.corpus_shader.present_pass =
        stats.corpus_shader.present_samples_checked == visible_frame_target &&
        stats.corpus_shader.present_sample_matches == visible_frame_target &&
        stats.corpus_shader.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.corpus_shader.present_pixel_matches == stats.corpus_shader.present_pixels_checked;
    stats.srv_sample.present_pass =
        stats.srv_sample.present_samples_checked == visible_frame_target &&
        stats.srv_sample.present_sample_matches == visible_frame_target &&
        stats.srv_sample.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.srv_sample.present_pixel_matches == stats.srv_sample.present_pixels_checked;
    stats.texture_array_srv_sample.present_pass =
        stats.texture_array_srv_sample.present_samples_checked == visible_frame_target &&
        stats.texture_array_srv_sample.present_sample_matches == visible_frame_target &&
        stats.texture_array_srv_sample.present_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.texture_array_srv_sample.present_pixel_matches == stats.texture_array_srv_sample.present_pixels_checked;
    bool textured_3d_all_faces_sampled = true;
    if (visible_frame_target >= kTextured3DFullRotationFrames) {
        for (UINT face = 0; face < kTextured3DFaceCount; ++face)
            textured_3d_all_faces_sampled =
                textured_3d_all_faces_sampled && stats.textured_3d.faces[face].sample_matches > 0;
    }
    stats.textured_3d.present_pass =
        stats.textured_3d.present_samples_checked == visible_frame_target &&
        stats.textured_3d.present_sample_matches == stats.textured_3d.present_samples_checked &&
        stats.textured_3d.depth_overlap_samples_checked == visible_frame_target &&
        stats.textured_3d.depth_overlap_sample_matches == visible_frame_target && textured_3d_all_faces_sampled;
    stats.cbv_sample.present_pass =
        stats.cbv_sample.present_samples_checked == visible_frame_target &&
        stats.cbv_sample.present_sample_matches == visible_frame_target &&
        stats.cbv_sample.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.cbv_sample.present_pixel_matches == stats.cbv_sample.present_pixels_checked;
    stats.indexed_draw.present_pass =
        stats.indexed_draw.present_samples_checked == visible_frame_target &&
        stats.indexed_draw.present_sample_matches == visible_frame_target &&
        stats.indexed_draw.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indexed_draw.present_pixel_matches == stats.indexed_draw.present_pixels_checked &&
        stats.indexed_draw.present_r32_samples_checked == visible_frame_target &&
        stats.indexed_draw.present_r32_sample_matches == visible_frame_target &&
        stats.indexed_draw.present_r32_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indexed_draw.present_r32_pixel_matches == stats.indexed_draw.present_r32_pixels_checked &&
        stats.indexed_draw.present_negative_base_samples_checked == visible_frame_target &&
        stats.indexed_draw.present_negative_base_sample_matches == visible_frame_target &&
        stats.indexed_draw.present_negative_base_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indexed_draw.present_negative_base_pixel_matches ==
            stats.indexed_draw.present_negative_base_pixels_checked &&
        stats.indexed_draw.present_dynamic_stride_samples_checked == visible_frame_target &&
        stats.indexed_draw.present_dynamic_stride_sample_matches == visible_frame_target &&
        stats.indexed_draw.present_dynamic_stride_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indexed_draw.present_dynamic_stride_pixel_matches ==
            stats.indexed_draw.present_dynamic_stride_pixels_checked &&
        stats.indexed_draw.present_execute_indirect_indexed_samples_checked == visible_frame_target &&
        stats.indexed_draw.present_execute_indirect_indexed_sample_matches == visible_frame_target &&
        stats.indexed_draw.present_execute_indirect_indexed_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indexed_draw.present_execute_indirect_indexed_pixel_matches ==
            stats.indexed_draw.present_execute_indirect_indexed_pixels_checked;
    stats.multi_slot_instance_root.present_pass =
        stats.multi_slot_instance_root.present_first_samples_checked == visible_frame_target &&
        stats.multi_slot_instance_root.present_first_sample_matches == visible_frame_target &&
        stats.multi_slot_instance_root.present_first_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.multi_slot_instance_root.present_first_pixel_matches ==
            stats.multi_slot_instance_root.present_first_pixels_checked &&
        stats.multi_slot_instance_root.present_second_samples_checked == visible_frame_target &&
        stats.multi_slot_instance_root.present_second_sample_matches == visible_frame_target &&
        stats.multi_slot_instance_root.present_second_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.multi_slot_instance_root.present_second_pixel_matches ==
            stats.multi_slot_instance_root.present_second_pixels_checked;
    stats.tessellation_fallback.progressive_triangle_seed_point_pass =
        stats.tessellation_fallback.progressive_triangle_seed_pixels > 0u &&
        stats.tessellation_fallback.progressive_triangle_seed_pixels < 96u;
    stats.tessellation_fallback.progressive_triangle_full_pass =
        stats.tessellation_fallback.progressive_triangle_final_pixels >= 1800u &&
        stats.tessellation_fallback.progressive_triangle_final_pixels >
            stats.tessellation_fallback.progressive_triangle_seed_pixels * 8u;
    stats.tessellation_fallback.progressive_triangle_rgb_channels_present =
        stats.tessellation_fallback.progressive_triangle_final_red_dominant_pixels > 16u &&
        stats.tessellation_fallback.progressive_triangle_final_green_dominant_pixels > 16u &&
        stats.tessellation_fallback.progressive_triangle_final_blue_dominant_pixels > 16u;
    stats.tessellation_fallback.progressive_triangle_ok =
        stats.tessellation_fallback.progressive_triangle_frames_validated == visible_frame_target &&
        stats.tessellation_fallback.progressive_triangle_coverage_counts.size() == visible_frame_target &&
        stats.tessellation_fallback.progressive_triangle_coverage_monotonic &&
        stats.tessellation_fallback.progressive_triangle_seed_point_pass &&
        stats.tessellation_fallback.progressive_triangle_full_pass &&
        stats.tessellation_fallback.progressive_triangle_rgb_channels_present;
    stats.tessellation_fallback.indexed_present_pass =
        stats.tessellation_fallback.indexed_present_samples_checked == visible_frame_target &&
        stats.tessellation_fallback.indexed_present_sample_matches == visible_frame_target &&
        stats.tessellation_fallback.indexed_present_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.tessellation_fallback.indexed_present_pixel_matches ==
            stats.tessellation_fallback.indexed_present_pixels_checked;
    stats.tessellation_fallback.present_pass =
        stats.tessellation_fallback.blocked_expected && !stats.tessellation_fallback.fallback_draw_encoded
            ? stats.tessellation_fallback.draw_calls == 0
            : (stats.tessellation_fallback.present_samples_checked == visible_frame_target &&
               stats.tessellation_fallback.present_sample_matches == visible_frame_target &&
               stats.tessellation_fallback.present_pixels_checked ==
                   visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
               stats.tessellation_fallback.present_pixel_matches ==
                   stats.tessellation_fallback.present_pixels_checked &&
               stats.tessellation_fallback.indexed_present_pass && stats.tessellation_fallback.progressive_triangle_ok);
    stats.indirect_draw.present_pass =
        stats.indirect_draw.present_samples_checked == visible_frame_target &&
        stats.indirect_draw.present_sample_matches == visible_frame_target &&
        stats.indirect_draw.present_pixels_checked == visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.indirect_draw.present_pixel_matches == stats.indirect_draw.present_pixels_checked;
    stats.nanite_cluster.present_pass =
        stats.nanite_cluster.present_samples_checked == visible_frame_target &&
        stats.nanite_cluster.present_sample_matches == visible_frame_target &&
        stats.nanite_cluster.present_pixels_checked ==
            visible_frame_target * kFreshTextureWidth * kFreshTextureHeight &&
        stats.nanite_cluster.present_pixel_matches == stats.nanite_cluster.present_pixels_checked;
    stats.abi_semantics.validated_frames = stats.frames_presented;
    stats.abi_semantics.present_tie_ok = stats.frames_presented == visible_frame_target;
    stats.abi_semantics.pass = stats.abi_semantics.guid_constants_ok && stats.abi_semantics.query_interface_ok &&
                               stats.abi_semantics.vtable_layout_ok && stats.abi_semantics.device_child_get_device_ok &&
                               stats.abi_semantics.device_child_identity_ok &&
                               stats.abi_semantics.private_data_roundtrip_ok && stats.abi_semantics.present_tie_ok;

    stats.pass =
        stats.hwnd_created && stats.adapter_report_pass && stats.abi_semantics.pass &&
        SUCCEEDED(stats.create_factory_hr) && SUCCEEDED(stats.create_device_hr) && SUCCEEDED(stats.create_queue_hr) &&
        SUCCEEDED(stats.create_swapchain_hr) && SUCCEEDED(stats.create_rtv_heap_hr) &&
        SUCCEEDED(stats.create_allocator_hr) && SUCCEEDED(stats.create_list_hr) && SUCCEEDED(stats.create_fence_hr) &&
        stats.gpu_textures.pass && stats.gpu_textures.present_pass && stats.heap_alias.pass &&
        stats.heap_alias.present_pass && stats.uav_barrier.pass && stats.uav_barrier.present_pass &&
        stats.wave_ops.pass && stats.wave_ops.present_pass && stats.rtv_format.pass && stats.rtv_format.present_pass &&
        stats.subresource_views.pass && stats.subresource_views.present_pass && stats.render_pass.pass &&
        stats.render_pass.present_pass && stats.corpus_shader.pass && stats.corpus_shader.present_pass &&
        stats.corpus_shader.draw_calls == visible_frame_target && stats.srv_sample.pass &&
        stats.srv_sample.present_pass && stats.srv_sample.draw_calls == visible_frame_target &&
        stats.texture_array_srv_sample.pass && stats.texture_array_srv_sample.present_pass &&
        stats.texture_array_srv_sample.draw_calls == visible_frame_target && stats.textured_3d.pass &&
        stats.textured_3d.present_pass && stats.textured_3d.draw_calls == visible_frame_target &&
        stats.textured_3d.vertex_buffer_updates == visible_frame_target &&
        stats.textured_3d.clear_depth_commands == visible_frame_target && stats.cbv_sample.pass &&
        stats.cbv_sample.present_pass && stats.cbv_sample.draw_calls == visible_frame_target &&
        stats.indexed_draw.pass && stats.indexed_draw.present_pass &&
        stats.indexed_draw.draw_indexed_calls == visible_frame_target &&
        stats.indexed_draw.draw_indexed_r32_calls == visible_frame_target &&
        stats.indexed_draw.draw_indexed_negative_base_calls == visible_frame_target &&
        stats.indexed_draw.draw_indexed_dynamic_stride_calls == visible_frame_target &&
        stats.indexed_draw.execute_indirect_indexed_calls == visible_frame_target &&
        stats.multi_slot_instance_root.pass && stats.multi_slot_instance_root.present_pass &&
        stats.multi_slot_instance_root.root_constant_sets == visible_frame_target * 2 &&
        stats.multi_slot_instance_root.draw_calls == visible_frame_target * 2 &&
        stats.multi_slot_instance_root.first_draw_calls == visible_frame_target &&
        stats.multi_slot_instance_root.second_draw_calls == visible_frame_target && stats.tessellation_fallback.pass &&
        stats.tessellation_fallback.present_pass && stats.tessellation_fallback.native_tessellation_resolved &&
        stats.tessellation_fallback.native_draw_encoded && stats.tessellation_fallback.native_indexed_draw_encoded &&
        !stats.tessellation_fallback.blocked_expected && !stats.tessellation_fallback.fallback_draw_encoded &&
        stats.tessellation_fallback.draw_calls == visible_frame_target &&
        stats.tessellation_fallback.draw_indexed_calls == visible_frame_target && stats.indirect_draw.pass &&
        stats.indirect_draw.present_pass && stats.indirect_draw.execute_indirect_calls == visible_frame_target &&
        stats.nanite_cluster.pass && stats.nanite_cluster.present_pass &&
        stats.nanite_cluster.execute_indirect_calls == visible_frame_target && stats.visible_scene.pass &&
        stats.visible_scene.sm5_stamp_present_pass && stats.visible_scene.progressive_triangle_present_pass &&
        stats.visible_scene.draw_calls == visible_frame_target && stats.dxil_scene.pass &&
        stats.dxil_scene.draw_calls == visible_frame_target && stats.dxil_hazard_replay.pass &&
        stats.dxil_readback.pass && SUCCEEDED(stats.present_hr) && stats.frames_presented == visible_frame_target;

    safe_release(gpu_textures.present_texture);
    safe_release(gpu_textures.present_sentinel_upload);
    destroy_heap_alias_exercise(heap_alias);
    destroy_wave_ops_exercise(wave_ops);
    destroy_uav_barrier_exercise(uav_barrier);
    destroy_rtv_format_exercise(rtv_format);
    destroy_subresource_view_exercise(subresource_views);
    destroy_texture_array_srv_sample_scene(texture_array_srv_sample);
    destroy_render_pass_exercise(render_pass);
    destroy_srv_sample_scene(srv_sample);
    destroy_textured3d_scene(textured_3d);
    destroy_cbv_sample_scene(cbv_sample);
    destroy_indexed_draw_scene(indexed_draw);
    destroy_multi_slot_instance_root_scene(multi_slot_instance_root);
    destroy_tessellation_fallback_scene(tessellation_fallback);
    destroy_indirect_draw_scene(indirect_draw);
    destroy_nanite_cluster_scene(nanite_cluster);
    destroy_dxil_readback(dxil_readback);
    destroy_corpus_shader_scene(corpus_shader);
    destroy_dxil_scene(dxil_scene);
    for (auto*& buffer : buffers)
        safe_release(buffer);
    safe_release(fence);
    safe_release(list);
    safe_release(allocator);
    safe_release(rtv_heap);
    safe_release(swapchain);
    safe_release(swapchain1);
    safe_release(queue);
    destroy_visible_scene(visible_scene);
    safe_release(device);
    safe_release(adapter);
    safe_release(factory);
    if (fence_event)
        CloseHandle(fence_event);
    DestroyWindow(hwnd);
    pump_messages();
    return stats;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

int main() {
    std::string corpus_tsv = getenv_string("VKD3D_FRESH_CORPUS_TSV");
    uint32_t target_shaders = 300;
    uint32_t target_textures = 300;
    CorpusStats corpus = load_corpus_tsv(corpus_tsv, target_shaders, target_textures);
    D3DRunStats d3d = run_d3d_window(corpus);
    bool corpus_ok =
        corpus.shader_files_loaded >= target_shaders && corpus.texture_files_loaded >= target_textures &&
        corpus.texture_payloads.size() >= target_textures &&
        corpus.texture_payload_bytes_from_files >= static_cast<uint64_t>(target_textures) * kFreshTexturePayloadBytes &&
        corpus.failed_loads == 0;
    bool pass = corpus_ok && d3d.pass;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.vkd3d.fresh.game.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"corpus\": {\n");
    std::printf("    \"tsv\": \"%s\",\n", json_escape(corpus.tsv_path).c_str());
    std::printf("    \"rows_seen\": %u,\n", corpus.rows);
    std::printf("    \"shaders_seen\": %u,\n", corpus.shaders_seen);
    std::printf("    \"textures_seen\": %u,\n", corpus.textures_seen);
    std::printf("    \"shader_files_loaded\": %u,\n", corpus.shader_files_loaded);
    std::printf("    \"texture_files_loaded\": %u,\n", corpus.texture_files_loaded);
    std::printf("    \"target_shaders\": %u,\n", target_shaders);
    std::printf("    \"target_textures\": %u,\n", target_textures);
    std::printf("    \"failed_loads\": %u,\n", corpus.failed_loads);
    std::printf("    \"bytes_loaded\": %llu,\n", static_cast<unsigned long long>(corpus.bytes_loaded));
    std::printf("    \"texture_payload_bytes_from_files\": %llu,\n",
                static_cast<unsigned long long>(corpus.texture_payload_bytes_from_files));
    std::printf("    \"texture_payloads_captured\": %zu,\n", corpus.texture_payloads.size());
    std::printf("    \"texture_payload_bytes_per_file\": %zu,\n", kFreshTexturePayloadBytes);
    std::printf("    \"fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(corpus.fnv1a64));
    std::printf("    \"ok\": %s\n", corpus_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"d3d12_window\": {\n");
    std::printf("    \"hwnd_created\": %s,\n", d3d.hwnd_created ? "true" : "false");
    std::printf("    \"CreateDXGIFactory2\": \"%s\",\n", hr_hex(d3d.create_factory_hr).c_str());
    std::printf("    \"adapter_report\": {\n");
    std::printf("      \"EnumAdapters1\": \"%s\",\n", hr_hex(d3d.enum_adapter_hr).c_str());
    std::printf("      \"GetDesc1\": \"%s\",\n", hr_hex(d3d.adapter_desc_hr).c_str());
    std::printf("      \"description\": \"%s\",\n", json_escape(d3d.adapter_description).c_str());
    std::printf("      \"vendor_id\": %u,\n", d3d.adapter_vendor_id);
    std::printf("      \"device_id\": %u,\n", d3d.adapter_device_id);
    std::printf("      \"dedicated_video_memory\": %llu,\n",
                static_cast<unsigned long long>(d3d.adapter_dedicated_video_memory));
    std::printf("      \"shared_system_memory\": %llu,\n",
                static_cast<unsigned long long>(d3d.adapter_shared_system_memory));
    std::printf("      \"adapter_luid_high\": %ld,\n", static_cast<long>(d3d.adapter_luid.HighPart));
    std::printf("      \"adapter_luid_low\": %lu,\n", static_cast<unsigned long>(d3d.adapter_luid.LowPart));
    std::printf("      \"device_luid_high\": %ld,\n", static_cast<long>(d3d.device_luid.HighPart));
    std::printf("      \"device_luid_low\": %lu,\n", static_cast<unsigned long>(d3d.device_luid.LowPart));
    std::printf("      \"adapter_luid_nonzero\": %s,\n", d3d.adapter_luid_nonzero ? "true" : "false");
    std::printf("      \"device_luid_nonzero\": %s,\n", d3d.device_luid_nonzero ? "true" : "false");
    std::printf("      \"luid_matches_device\": %s,\n", d3d.adapter_luid_matches_device ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.adapter_report_pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"D3D12CreateDevice\": \"%s\",\n", hr_hex(d3d.create_device_hr).c_str());
    std::printf("    \"CreateCommandQueue\": \"%s\",\n", hr_hex(d3d.create_queue_hr).c_str());
    std::printf("    \"CreateSwapChainForHwnd\": \"%s\",\n", hr_hex(d3d.create_swapchain_hr).c_str());
    std::printf("    \"CreateDescriptorHeap_RTV\": \"%s\",\n", hr_hex(d3d.create_rtv_heap_hr).c_str());
    std::printf("    \"CreateCommandAllocator\": \"%s\",\n", hr_hex(d3d.create_allocator_hr).c_str());
    std::printf("    \"CreateCommandList\": \"%s\",\n", hr_hex(d3d.create_list_hr).c_str());
    std::printf("    \"CreateFence\": \"%s\",\n", hr_hex(d3d.create_fence_hr).c_str());
    std::printf("    \"abi_semantics\": {\n");
    std::printf(
        "      \"proof_scope\": \"guid_com_abi_queryinterface_private_data_tied_to_presented_swapchain_run\",\n");
    std::printf("      \"IID_ID3D12Device\": \"%s\",\n", guid_string(__uuidof(ID3D12Device)).c_str());
    std::printf("      \"IID_IDXGIFactory4\": \"%s\",\n", guid_string(__uuidof(IDXGIFactory4)).c_str());
    std::printf("      \"IID_IDXGIAdapter1\": \"%s\",\n", guid_string(__uuidof(IDXGIAdapter1)).c_str());
    std::printf("      \"IID_ID3D12CommandQueue\": \"%s\",\n", guid_string(__uuidof(ID3D12CommandQueue)).c_str());
    std::printf("      \"IID_ID3D12CommandAllocator\": \"%s\",\n",
                guid_string(__uuidof(ID3D12CommandAllocator)).c_str());
    std::printf("      \"IID_ID3D12GraphicsCommandList\": \"%s\",\n",
                guid_string(__uuidof(ID3D12GraphicsCommandList)).c_str());
    std::printf("      \"IID_ID3D12Fence\": \"%s\",\n", guid_string(__uuidof(ID3D12Fence)).c_str());
    std::printf("      \"IID_IDXGISwapChain3\": \"%s\",\n", guid_string(__uuidof(IDXGISwapChain3)).c_str());
    std::printf("      \"guid_constants_ok\": %s,\n", d3d.abi_semantics.guid_constants_ok ? "true" : "false");
    std::printf("      \"QueryInterface_IUnknown_device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_device_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_ID3D12Device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_device_self_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_factory\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_factory_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_adapter\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_adapter_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_ID3D12CommandQueue\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_queue_self_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_queue\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_queue_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_ID3D12CommandAllocator\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_allocator_self_hr).c_str());
    std::printf("      \"QueryInterface_ID3D12GraphicsCommandList\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_list_self_hr).c_str());
    std::printf("      \"QueryInterface_ID3D12Fence\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_fence_self_hr).c_str());
    std::printf("      \"QueryInterface_IDXGISwapChain3\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.query_swapchain3_self_hr).c_str());
    std::printf("      \"GetDevice_from_queue\": \"%s\",\n", hr_hex(d3d.abi_semantics.queue_get_device_hr).c_str());
    std::printf("      \"GetDevice_from_allocator\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.allocator_get_device_hr).c_str());
    std::printf("      \"GetDevice_from_list\": \"%s\",\n", hr_hex(d3d.abi_semantics.list_get_device_hr).c_str());
    std::printf("      \"GetDevice_from_fence\": \"%s\",\n", hr_hex(d3d.abi_semantics.fence_get_device_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_queue_device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.queue_device_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_allocator_device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.allocator_device_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_list_device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.list_device_iunknown_hr).c_str());
    std::printf("      \"QueryInterface_IUnknown_fence_device\": \"%s\",\n",
                hr_hex(d3d.abi_semantics.fence_device_iunknown_hr).c_str());
    std::printf("      \"SetPrivateData\": \"%s\",\n", hr_hex(d3d.abi_semantics.private_data_set_hr).c_str());
    std::printf("      \"GetPrivateData\": \"%s\",\n", hr_hex(d3d.abi_semantics.private_data_get_hr).c_str());
    std::printf("      \"private_data_size\": %u,\n", d3d.abi_semantics.private_data_size);
    std::printf("      \"query_interface_ok\": %s,\n", d3d.abi_semantics.query_interface_ok ? "true" : "false");
    std::printf("      \"vtable_layout_ok\": %s,\n", d3d.abi_semantics.vtable_layout_ok ? "true" : "false");
    std::printf("      \"device_child_get_device_ok\": %s,\n",
                d3d.abi_semantics.device_child_get_device_ok ? "true" : "false");
    std::printf("      \"device_child_identity_ok\": %s,\n",
                d3d.abi_semantics.device_child_identity_ok ? "true" : "false");
    std::printf("      \"private_data_copy_semantics\": "
                "\"caller_buffer_mutated_after_SetPrivateData_before_GetPrivateData\",\n");
    std::printf("      \"private_data_roundtrip_ok\": %s,\n",
                d3d.abi_semantics.private_data_roundtrip_ok ? "true" : "false");
    std::printf("      \"validated_frames\": %u,\n", d3d.abi_semantics.validated_frames);
    std::printf("      \"present_tie_ok\": %s,\n", d3d.abi_semantics.present_tie_ok ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.abi_semantics.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"gpu_textures\": {\n");
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV\": \"%s\",\n",
                hr_hex(d3d.gpu_textures.create_descriptor_heap_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.gpu_textures.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.gpu_textures.signal_hr).c_str());
    std::printf("      \"textures_requested\": %u,\n", d3d.gpu_textures.textures_requested);
    std::printf("      \"textures_created\": %u,\n", d3d.gpu_textures.textures_created);
    std::printf("      \"upload_buffers_created\": %u,\n", d3d.gpu_textures.upload_buffers_created);
    std::printf("      \"srv_descriptors_created\": %u,\n", d3d.gpu_textures.srv_descriptors_created);
    std::printf("      \"copy_texture_region_commands\": %u,\n", d3d.gpu_textures.copy_texture_region_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.gpu_textures.transition_barriers);
    std::printf("      \"texture_payloads_uploaded\": %u,\n", d3d.gpu_textures.texture_payloads_uploaded);
    std::printf("      \"present_backbuffer_sentinel_copies\": %u,\n",
                d3d.gpu_textures.present_backbuffer_sentinel_copies);
    std::printf("      \"present_copy_commands\": %u,\n", d3d.gpu_textures.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.gpu_textures.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.gpu_textures.present_sample_matches);
    std::printf("      \"present_expected_rgba\": [%u, %u, %u, %u],\n", d3d.gpu_textures.present_expected_rgba[0],
                d3d.gpu_textures.present_expected_rgba[1], d3d.gpu_textures.present_expected_rgba[2],
                d3d.gpu_textures.present_expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.gpu_textures.present_rgba[0],
                d3d.gpu_textures.present_rgba[1], d3d.gpu_textures.present_rgba[2], d3d.gpu_textures.present_rgba[3]);
    std::printf("      \"texture_payload_bytes_from_files\": %llu,\n",
                static_cast<unsigned long long>(d3d.gpu_textures.texture_payload_bytes_from_files));
    std::printf("      \"upload_payload_fnv1a64\": \"%016llx\",\n",
                static_cast<unsigned long long>(d3d.gpu_textures.upload_payload_fnv1a64));
    std::printf("      \"upload_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.gpu_textures.upload_bytes));
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.gpu_textures.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.gpu_textures.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.gpu_textures.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"heap_alias\": {\n");
    std::printf("      \"CreateHeap\": \"%s\",\n", hr_hex(d3d.heap_alias.create_heap_hr).c_str());
    std::printf("      \"CreatePlacedResourceA\": \"%s\",\n", hr_hex(d3d.heap_alias.create_placed_a_hr).c_str());
    std::printf("      \"CreatePlacedResourceB\": \"%s\",\n", hr_hex(d3d.heap_alias.create_placed_b_hr).c_str());
    std::printf("      \"CreateUpload\": \"%s\",\n", hr_hex(d3d.heap_alias.create_upload_hr).c_str());
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.heap_alias.create_readback_hr).c_str());
    std::printf("      \"MapUpload\": \"%s\",\n", hr_hex(d3d.heap_alias.map_upload_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.heap_alias.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.heap_alias.signal_hr).c_str());
    std::printf("      \"MapReadback\": \"%s\",\n", hr_hex(d3d.heap_alias.map_readback_hr).c_str());
    std::printf("      \"heap_size\": %llu,\n", static_cast<unsigned long long>(d3d.heap_alias.heap_size));
    std::printf("      \"alias_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.heap_alias.alias_bytes));
    std::printf("      \"gpu_virtual_address_a\": %llu,\n",
                static_cast<unsigned long long>(d3d.heap_alias.gpu_virtual_address_a));
    std::printf("      \"gpu_virtual_address_b\": %llu,\n",
                static_cast<unsigned long long>(d3d.heap_alias.gpu_virtual_address_b));
    std::printf("      \"gpu_virtual_addresses_match\": %s,\n",
                d3d.heap_alias.gpu_virtual_addresses_match ? "true" : "false");
    std::printf("      \"copy_before_alias_commands\": %u,\n", d3d.heap_alias.copy_before_alias_commands);
    std::printf("      \"aliasing_barriers\": %u,\n", d3d.heap_alias.aliasing_barriers);
    std::printf("      \"copy_alias_overlap_commands\": %u,\n", d3d.heap_alias.copy_alias_overlap_commands);
    std::printf("      \"copy_after_alias_commands\": %u,\n", d3d.heap_alias.copy_after_alias_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.heap_alias.transition_barriers);
    std::printf("      \"readback_before_alias_ok\": %s,\n",
                d3d.heap_alias.readback_before_alias_ok ? "true" : "false");
    std::printf("      \"readback_alias_overlap_ok\": %s,\n",
                d3d.heap_alias.readback_alias_overlap_ok ? "true" : "false");
    std::printf("      \"readback_after_alias_ok\": %s,\n", d3d.heap_alias.readback_after_alias_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.heap_alias.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.heap_alias.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.heap_alias.present_sample_matches);
    std::printf("      \"present_expected_rgba\": [%u, %u, %u, %u],\n", d3d.heap_alias.present_expected_rgba[0],
                d3d.heap_alias.present_expected_rgba[1], d3d.heap_alias.present_expected_rgba[2],
                d3d.heap_alias.present_expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.heap_alias.present_rgba[0],
                d3d.heap_alias.present_rgba[1], d3d.heap_alias.present_rgba[2], d3d.heap_alias.present_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.heap_alias.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.heap_alias.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.heap_alias.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"uav_barrier\": {\n");
    std::printf("      \"proof_scope\": \"dependent_uav_dispatch_visibility_with_explicit_uav_barriers\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.uav_barrier.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"CSWrite_cs_5_0\": \"%s\",\n", hr_hex(d3d.uav_barrier.compile_cs_hr).c_str());
    std::printf("      \"CSTransform_cs_5_0\": \"%s\",\n", hr_hex(d3d.uav_barrier.compile_read_cs_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n", hr_hex(d3d.uav_barrier.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.uav_barrier.create_root_hr).c_str());
    std::printf("      \"CreateComputePipelineStateWrite\": \"%s\",\n", hr_hex(d3d.uav_barrier.create_pso_hr).c_str());
    std::printf("      \"CreateComputePipelineStateTransform\": \"%s\",\n",
                hr_hex(d3d.uav_barrier.create_read_pso_hr).c_str());
    std::printf("      \"CreateUavBuffer\": \"%s\",\n", hr_hex(d3d.uav_barrier.create_uav_buffer_hr).c_str());
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.uav_barrier.create_readback_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.uav_barrier.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.uav_barrier.signal_hr).c_str());
    std::printf("      \"MapReadback\": \"%s\",\n", hr_hex(d3d.uav_barrier.map_readback_hr).c_str());
    std::printf("      \"footprint_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.uav_barrier.footprint_bytes));
    std::printf("      \"row_pitch\": %u,\n", d3d.uav_barrier.row_pitch);
    std::printf("      \"fixed_footprint_ok\": %s,\n", d3d.uav_barrier.fixed_footprint_ok ? "true" : "false");
    std::printf("      \"uav_gpu_virtual_address\": %llu,\n",
                static_cast<unsigned long long>(d3d.uav_barrier.uav_gpu_virtual_address));
    std::printf("      \"root_uav_sets\": %u,\n", d3d.uav_barrier.root_uav_sets);
    std::printf("      \"root_constant_sets\": %u,\n", d3d.uav_barrier.root_constant_sets);
    std::printf("      \"dispatch_commands\": %u,\n", d3d.uav_barrier.dispatch_commands);
    std::printf("      \"dispatch_write_commands\": %u,\n", d3d.uav_barrier.dispatch_write_commands);
    std::printf("      \"dispatch_read_transform_commands\": %u,\n", d3d.uav_barrier.dispatch_read_transform_commands);
    std::printf("      \"dispatch_x\": %u,\n", d3d.uav_barrier.dispatch_x);
    std::printf("      \"dispatch_y\": %u,\n", d3d.uav_barrier.dispatch_y);
    std::printf("      \"uav_barriers\": %u,\n", d3d.uav_barrier.uav_barriers);
    std::printf("      \"transition_barriers\": %u,\n", d3d.uav_barrier.transition_barriers);
    std::printf("      \"compute_pixels_checked\": %u,\n", d3d.uav_barrier.compute_pixels_checked);
    std::printf("      \"compute_pixel_matches\": %u,\n", d3d.uav_barrier.compute_pixel_matches);
    std::printf("      \"compute_first_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.compute_first_rgba[0],
                d3d.uav_barrier.compute_first_rgba[1], d3d.uav_barrier.compute_first_rgba[2],
                d3d.uav_barrier.compute_first_rgba[3]);
    std::printf("      \"compute_center_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.compute_center_rgba[0],
                d3d.uav_barrier.compute_center_rgba[1], d3d.uav_barrier.compute_center_rgba[2],
                d3d.uav_barrier.compute_center_rgba[3]);
    std::printf("      \"compute_last_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.compute_last_rgba[0],
                d3d.uav_barrier.compute_last_rgba[1], d3d.uav_barrier.compute_last_rgba[2],
                d3d.uav_barrier.compute_last_rgba[3]);
    std::printf("      \"compute_readback_ok\": %s,\n", d3d.uav_barrier.compute_readback_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.uav_barrier.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.uav_barrier.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.uav_barrier.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.uav_barrier.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.uav_barrier.present_pixel_matches);
    std::printf("      \"present_expected_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.present_expected_rgba[0],
                d3d.uav_barrier.present_expected_rgba[1], d3d.uav_barrier.present_expected_rgba[2],
                d3d.uav_barrier.present_expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.present_rgba[0],
                d3d.uav_barrier.present_rgba[1], d3d.uav_barrier.present_rgba[2], d3d.uav_barrier.present_rgba[3]);
    std::printf("      \"present_center_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.present_center_rgba[0],
                d3d.uav_barrier.present_center_rgba[1], d3d.uav_barrier.present_center_rgba[2],
                d3d.uav_barrier.present_center_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.uav_barrier.present_last_rgba[0],
                d3d.uav_barrier.present_last_rgba[1], d3d.uav_barrier.present_last_rgba[2],
                d3d.uav_barrier.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.uav_barrier.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.uav_barrier.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.uav_barrier.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"wave_ops\": {\n");
    std::printf("      \"proof_scope\": \"sm6_dxil_waveops_compute_uav_presented_readback\",\n");
    std::printf("      \"cs_path\": \"%s\",\n", json_escape(d3d.wave_ops.cs_path).c_str());
    std::printf("      \"cs_loaded\": %s,\n", d3d.wave_ops.cs_loaded ? "true" : "false");
    std::printf("      \"cs_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.wave_ops.cs_bytes));
    std::printf("      \"cs_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(d3d.wave_ops.cs_fnv1a64));
    std::printf("      \"CheckFeatureSupport_OPTIONS1\": \"%s\",\n", hr_hex(d3d.wave_ops.options1_hr).c_str());
    std::printf("      \"wave_ops_reported\": %s,\n", d3d.wave_ops.wave_ops_reported ? "true" : "false");
    std::printf("      \"wave_lane_count_min\": %u,\n", d3d.wave_ops.wave_lane_count_min);
    std::printf("      \"wave_lane_count_max\": %u,\n", d3d.wave_ops.wave_lane_count_max);
    std::printf("      \"total_lane_count\": %u,\n", d3d.wave_ops.total_lane_count);
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n", hr_hex(d3d.wave_ops.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.wave_ops.create_root_hr).c_str());
    std::printf("      \"CreateComputePipelineStateWaveOps\": \"%s\",\n", hr_hex(d3d.wave_ops.create_pso_hr).c_str());
    std::printf("      \"CreateUavBuffer\": \"%s\",\n", hr_hex(d3d.wave_ops.create_uav_buffer_hr).c_str());
    std::printf("      \"CreateUavDescriptorHeap\": \"%s\",\n",
                hr_hex(d3d.wave_ops.create_uav_descriptor_heap_hr).c_str());
    std::printf("      \"CreateUnorderedAccessView_descriptors\": %u,\n", d3d.wave_ops.uav_descriptors_created);
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.wave_ops.create_readback_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.wave_ops.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.wave_ops.signal_hr).c_str());
    std::printf("      \"MapReadback\": \"%s\",\n", hr_hex(d3d.wave_ops.map_readback_hr).c_str());
    std::printf("      \"footprint_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.wave_ops.footprint_bytes));
    std::printf("      \"row_pitch\": %u,\n", d3d.wave_ops.row_pitch);
    std::printf("      \"fixed_footprint_ok\": %s,\n", d3d.wave_ops.fixed_footprint_ok ? "true" : "false");
    std::printf("      \"uav_gpu_virtual_address\": %llu,\n",
                static_cast<unsigned long long>(d3d.wave_ops.uav_gpu_virtual_address));
    std::printf("      \"uav_descriptor_gpu_handle\": %llu,\n",
                static_cast<unsigned long long>(d3d.wave_ops.uav_descriptor_gpu_handle));
    std::printf("      \"root_uav_sets\": %u,\n", d3d.wave_ops.root_uav_sets);
    std::printf("      \"dispatch_commands\": %u,\n", d3d.wave_ops.dispatch_commands);
    std::printf("      \"dispatch_groups\": [%u, %u, %u],\n", d3d.wave_ops.dispatch_groups_x,
                d3d.wave_ops.dispatch_groups_y, d3d.wave_ops.dispatch_groups_z);
    std::printf("      \"uav_barriers\": %u,\n", d3d.wave_ops.uav_barriers);
    std::printf("      \"transition_barriers\": %u,\n", d3d.wave_ops.transition_barriers);
    std::printf("      \"compute_pixels_checked\": %u,\n", d3d.wave_ops.compute_pixels_checked);
    std::printf("      \"compute_pixel_matches\": %u,\n", d3d.wave_ops.compute_pixel_matches);
    std::printf("      \"compute_first_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.compute_first_rgba[0],
                d3d.wave_ops.compute_first_rgba[1], d3d.wave_ops.compute_first_rgba[2],
                d3d.wave_ops.compute_first_rgba[3]);
    std::printf("      \"compute_center_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.compute_center_rgba[0],
                d3d.wave_ops.compute_center_rgba[1], d3d.wave_ops.compute_center_rgba[2],
                d3d.wave_ops.compute_center_rgba[3]);
    std::printf("      \"compute_last_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.compute_last_rgba[0],
                d3d.wave_ops.compute_last_rgba[1], d3d.wave_ops.compute_last_rgba[2],
                d3d.wave_ops.compute_last_rgba[3]);
    std::printf("      \"compute_readback_ok\": %s,\n", d3d.wave_ops.compute_readback_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.wave_ops.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.wave_ops.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.wave_ops.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.wave_ops.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.wave_ops.present_pixel_matches);
    std::printf("      \"present_first_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.present_first_rgba[0],
                d3d.wave_ops.present_first_rgba[1], d3d.wave_ops.present_first_rgba[2],
                d3d.wave_ops.present_first_rgba[3]);
    std::printf("      \"present_center_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.present_center_rgba[0],
                d3d.wave_ops.present_center_rgba[1], d3d.wave_ops.present_center_rgba[2],
                d3d.wave_ops.present_center_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.wave_ops.present_last_rgba[0],
                d3d.wave_ops.present_last_rgba[1], d3d.wave_ops.present_last_rgba[2],
                d3d.wave_ops.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.wave_ops.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.wave_ops.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.wave_ops.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"rtv_format\": {\n");
    std::printf("      \"proof_scope\": \"offscreen_r8g8b8a8_unorm_rtv_clear_presented_copy_readback\",\n");
    std::printf("      \"CreateTexture2D_R8G8B8A8_UNORM_ALLOW_RENDER_TARGET\": \"%s\",\n",
                hr_hex(d3d.rtv_format.create_texture_hr).c_str());
    std::printf("      \"CreateRtvDescriptorHeap\": \"%s\",\n", hr_hex(d3d.rtv_format.create_rtv_heap_hr).c_str());
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.rtv_format.create_readback_hr).c_str());
    std::printf("      \"CreateRenderTargetView_descriptors\": %u,\n", d3d.rtv_format.create_rtv_descriptors);
    std::printf("      \"ClearRenderTargetView_commands\": %u,\n", d3d.rtv_format.clear_rtv_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.rtv_format.transition_barriers);
    std::printf("      \"fixed_footprint_ok\": %s,\n", d3d.rtv_format.fixed_footprint_ok ? "true" : "false");
    std::printf("      \"row_pitch\": %u,\n", d3d.rtv_format.row_pitch);
    std::printf("      \"footprint_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.rtv_format.footprint_bytes));
    std::printf("      \"offscreen_pixels_checked\": %u,\n", d3d.rtv_format.offscreen_pixels_checked);
    std::printf("      \"offscreen_pixel_matches\": %u,\n", d3d.rtv_format.offscreen_pixel_matches);
    std::printf("      \"offscreen_first_rgba\": [%u, %u, %u, %u],\n", d3d.rtv_format.offscreen_first_rgba[0],
                d3d.rtv_format.offscreen_first_rgba[1], d3d.rtv_format.offscreen_first_rgba[2],
                d3d.rtv_format.offscreen_first_rgba[3]);
    std::printf("      \"offscreen_last_rgba\": [%u, %u, %u, %u],\n", d3d.rtv_format.offscreen_last_rgba[0],
                d3d.rtv_format.offscreen_last_rgba[1], d3d.rtv_format.offscreen_last_rgba[2],
                d3d.rtv_format.offscreen_last_rgba[3]);
    std::printf("      \"offscreen_readback_ok\": %s,\n", d3d.rtv_format.offscreen_readback_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.rtv_format.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.rtv_format.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.rtv_format.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.rtv_format.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.rtv_format.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.rtv_format.expected_rgba[0],
                d3d.rtv_format.expected_rgba[1], d3d.rtv_format.expected_rgba[2], d3d.rtv_format.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.rtv_format.present_rgba[0],
                d3d.rtv_format.present_rgba[1], d3d.rtv_format.present_rgba[2], d3d.rtv_format.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.rtv_format.present_last_rgba[0],
                d3d.rtv_format.present_last_rgba[1], d3d.rtv_format.present_last_rgba[2],
                d3d.rtv_format.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.rtv_format.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.rtv_format.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.rtv_format.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"texture_array_subresources\": {\n");
    std::printf("      \"proof_scope\": \"texture2darray_subresource_upload_readback_slice1_presented_copy\",\n");
    std::printf("      \"CreateTexture2DArray_R8G8B8A8_UNORM\": \"%s\",\n",
                hr_hex(d3d.subresource_views.create_texture_array_hr).c_str());
    std::printf("      \"CreateUploadBuffer\": \"%s\",\n", hr_hex(d3d.subresource_views.create_upload_hr).c_str());
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.subresource_views.create_readback_hr).c_str());
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV\": \"%s\",\n",
                hr_hex(d3d.subresource_views.create_descriptor_heap_hr).c_str());
    std::printf("      \"MapUpload\": \"%s\",\n", hr_hex(d3d.subresource_views.map_upload_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.subresource_views.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.subresource_views.signal_hr).c_str());
    std::printf("      \"MapReadback\": \"%s\",\n", hr_hex(d3d.subresource_views.map_readback_hr).c_str());
    std::printf("      \"slice_count\": %u,\n", d3d.subresource_views.slice_count);
    std::printf("      \"footprint_bytes\": %llu,\n",
                static_cast<unsigned long long>(d3d.subresource_views.footprint_bytes));
    std::printf("      \"slice_offsets\": [%llu, %llu],\n",
                static_cast<unsigned long long>(d3d.subresource_views.slice_offsets[0]),
                static_cast<unsigned long long>(d3d.subresource_views.slice_offsets[1]));
    std::printf("      \"row_pitch\": [%u, %u],\n", d3d.subresource_views.row_pitch[0],
                d3d.subresource_views.row_pitch[1]);
    std::printf("      \"fixed_footprints_ok\": %s,\n", d3d.subresource_views.fixed_footprints_ok ? "true" : "false");
    std::printf("      \"CreateShaderResourceView_descriptors_inventory_only\": %u,\n",
                d3d.subresource_views.srv_descriptors_created);
    std::printf("      \"upload_subresources_filled\": %u,\n", d3d.subresource_views.upload_subresources_filled);
    std::printf("      \"readback_sentinel_fills\": %u,\n", d3d.subresource_views.readback_sentinel_fills);
    std::printf("      \"copy_upload_subresources\": %u,\n", d3d.subresource_views.copy_upload_subresources);
    std::printf("      \"copy_readback_subresources\": %u,\n", d3d.subresource_views.copy_readback_subresources);
    std::printf("      \"transition_barriers\": %u,\n", d3d.subresource_views.transition_barriers);
    std::printf("      \"subresource_pixels_checked\": %u,\n", d3d.subresource_views.subresource_pixels_checked);
    std::printf("      \"subresource_pixel_matches\": %u,\n", d3d.subresource_views.subresource_pixel_matches);
    std::printf("      \"expected_slice0_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.expected_slice0_rgba[0],
                d3d.subresource_views.expected_slice0_rgba[1], d3d.subresource_views.expected_slice0_rgba[2],
                d3d.subresource_views.expected_slice0_rgba[3]);
    std::printf("      \"expected_slice1_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.expected_slice1_rgba[0],
                d3d.subresource_views.expected_slice1_rgba[1], d3d.subresource_views.expected_slice1_rgba[2],
                d3d.subresource_views.expected_slice1_rgba[3]);
    std::printf("      \"slice0_first_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.slice0_first_rgba[0],
                d3d.subresource_views.slice0_first_rgba[1], d3d.subresource_views.slice0_first_rgba[2],
                d3d.subresource_views.slice0_first_rgba[3]);
    std::printf("      \"slice0_last_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.slice0_last_rgba[0],
                d3d.subresource_views.slice0_last_rgba[1], d3d.subresource_views.slice0_last_rgba[2],
                d3d.subresource_views.slice0_last_rgba[3]);
    std::printf("      \"slice1_first_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.slice1_first_rgba[0],
                d3d.subresource_views.slice1_first_rgba[1], d3d.subresource_views.slice1_first_rgba[2],
                d3d.subresource_views.slice1_first_rgba[3]);
    std::printf("      \"slice1_last_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.slice1_last_rgba[0],
                d3d.subresource_views.slice1_last_rgba[1], d3d.subresource_views.slice1_last_rgba[2],
                d3d.subresource_views.slice1_last_rgba[3]);
    std::printf("      \"subresource_readback_ok\": %s,\n",
                d3d.subresource_views.subresource_readback_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.subresource_views.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.subresource_views.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.subresource_views.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.subresource_views.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.subresource_views.present_pixel_matches);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.present_rgba[0],
                d3d.subresource_views.present_rgba[1], d3d.subresource_views.present_rgba[2],
                d3d.subresource_views.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.subresource_views.present_last_rgba[0],
                d3d.subresource_views.present_last_rgba[1], d3d.subresource_views.present_last_rgba[2],
                d3d.subresource_views.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.subresource_views.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.subresource_views.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.subresource_views.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"texture_array_srv_sample\": {\n");
    std::printf("      \"proof_scope\": \"texture2darray_srv_descriptor_table_slice1_sample_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n",
                d3d.texture_array_srv_sample.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"vs_5_0\": \"%s\",\n", hr_hex(d3d.texture_array_srv_sample.compile_vs_hr).c_str());
    std::printf("      \"ps_5_0_Texture2DArray_Sample\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_descriptor_table_static_sampler\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateTexture2DArray_R8G8B8A8_UNORM\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_texture_array_hr).c_str());
    std::printf("      \"CreateUploadBuffer\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_upload_hr).c_str());
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE\": \"%s\",\n",
                hr_hex(d3d.texture_array_srv_sample.create_descriptor_heap_hr).c_str());
    std::printf("      \"slice_count\": %u,\n", d3d.texture_array_srv_sample.slice_count);
    std::printf("      \"footprint_bytes\": %llu,\n",
                static_cast<unsigned long long>(d3d.texture_array_srv_sample.footprint_bytes));
    std::printf("      \"slice_offsets\": [%llu, %llu],\n",
                static_cast<unsigned long long>(d3d.texture_array_srv_sample.slice_offsets[0]),
                static_cast<unsigned long long>(d3d.texture_array_srv_sample.slice_offsets[1]));
    std::printf("      \"row_pitch\": [%u, %u],\n", d3d.texture_array_srv_sample.row_pitch[0],
                d3d.texture_array_srv_sample.row_pitch[1]);
    std::printf("      \"fixed_footprints_ok\": %s,\n",
                d3d.texture_array_srv_sample.fixed_footprints_ok ? "true" : "false");
    std::printf("      \"CreateShaderResourceView_Texture2DArray_descriptors\": %u,\n",
                d3d.texture_array_srv_sample.srv_descriptors_created);
    std::printf("      \"upload_subresources_filled\": %u,\n", d3d.texture_array_srv_sample.upload_subresources_filled);
    std::printf("      \"CopyTextureRegion_commands\": %u,\n", d3d.texture_array_srv_sample.copy_upload_subresources);
    std::printf("      \"transition_barriers\": %u,\n", d3d.texture_array_srv_sample.transition_barriers);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.texture_array_srv_sample.fence_wait_ok ? "true" : "false");
    std::printf("      \"draw_calls\": %u,\n", d3d.texture_array_srv_sample.draw_calls);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.texture_array_srv_sample.vertices_per_draw);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.texture_array_srv_sample.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.texture_array_srv_sample.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.texture_array_srv_sample.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.texture_array_srv_sample.present_pixel_matches);
    std::printf(
        "      \"expected_slice0_rgba\": [%u, %u, %u, %u],\n", d3d.texture_array_srv_sample.expected_slice0_rgba[0],
        d3d.texture_array_srv_sample.expected_slice0_rgba[1], d3d.texture_array_srv_sample.expected_slice0_rgba[2],
        d3d.texture_array_srv_sample.expected_slice0_rgba[3]);
    std::printf(
        "      \"expected_slice1_rgba\": [%u, %u, %u, %u],\n", d3d.texture_array_srv_sample.expected_slice1_rgba[0],
        d3d.texture_array_srv_sample.expected_slice1_rgba[1], d3d.texture_array_srv_sample.expected_slice1_rgba[2],
        d3d.texture_array_srv_sample.expected_slice1_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.texture_array_srv_sample.present_rgba[0],
                d3d.texture_array_srv_sample.present_rgba[1], d3d.texture_array_srv_sample.present_rgba[2],
                d3d.texture_array_srv_sample.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.texture_array_srv_sample.present_last_rgba[0],
                d3d.texture_array_srv_sample.present_last_rgba[1], d3d.texture_array_srv_sample.present_last_rgba[2],
                d3d.texture_array_srv_sample.present_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.texture_array_srv_sample.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.texture_array_srv_sample.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"render_pass\": {\n");
    std::printf("      \"proof_scope\": \"offscreen_render_pass_clear_presented_copy_readback\",\n");
    std::printf("      \"QueryInterface_ID3D12GraphicsCommandList4\": \"%s\",\n",
                hr_hex(d3d.render_pass.query_list4_hr).c_str());
    std::printf("      \"CreateTexture2D_R8G8B8A8_UNORM_ALLOW_RENDER_TARGET\": \"%s\",\n",
                hr_hex(d3d.render_pass.create_texture_hr).c_str());
    std::printf("      \"CreateRtvDescriptorHeap\": \"%s\",\n", hr_hex(d3d.render_pass.create_rtv_heap_hr).c_str());
    std::printf("      \"CreateReadback\": \"%s\",\n", hr_hex(d3d.render_pass.create_readback_hr).c_str());
    std::printf("      \"CreateRenderTargetView_descriptors\": %u,\n", d3d.render_pass.create_rtv_descriptors);
    std::printf("      \"BeginRenderPass_commands\": %u,\n", d3d.render_pass.begin_render_pass_commands);
    std::printf("      \"EndRenderPass_commands\": %u,\n", d3d.render_pass.end_render_pass_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.render_pass.transition_barriers);
    std::printf("      \"fixed_footprint_ok\": %s,\n", d3d.render_pass.fixed_footprint_ok ? "true" : "false");
    std::printf("      \"row_pitch\": %u,\n", d3d.render_pass.row_pitch);
    std::printf("      \"footprint_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.render_pass.footprint_bytes));
    std::printf("      \"offscreen_pixels_checked\": %u,\n", d3d.render_pass.offscreen_pixels_checked);
    std::printf("      \"offscreen_pixel_matches\": %u,\n", d3d.render_pass.offscreen_pixel_matches);
    std::printf("      \"offscreen_first_rgba\": [%u, %u, %u, %u],\n", d3d.render_pass.offscreen_first_rgba[0],
                d3d.render_pass.offscreen_first_rgba[1], d3d.render_pass.offscreen_first_rgba[2],
                d3d.render_pass.offscreen_first_rgba[3]);
    std::printf("      \"offscreen_last_rgba\": [%u, %u, %u, %u],\n", d3d.render_pass.offscreen_last_rgba[0],
                d3d.render_pass.offscreen_last_rgba[1], d3d.render_pass.offscreen_last_rgba[2],
                d3d.render_pass.offscreen_last_rgba[3]);
    std::printf("      \"offscreen_readback_ok\": %s,\n", d3d.render_pass.offscreen_readback_ok ? "true" : "false");
    std::printf("      \"present_copy_commands\": %u,\n", d3d.render_pass.present_copy_commands);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.render_pass.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.render_pass.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.render_pass.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.render_pass.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.render_pass.expected_rgba[0],
                d3d.render_pass.expected_rgba[1], d3d.render_pass.expected_rgba[2], d3d.render_pass.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.render_pass.present_rgba[0],
                d3d.render_pass.present_rgba[1], d3d.render_pass.present_rgba[2], d3d.render_pass.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.render_pass.present_last_rgba[0],
                d3d.render_pass.present_last_rgba[1], d3d.render_pass.present_last_rgba[2],
                d3d.render_pass.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.render_pass.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.render_pass.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.render_pass.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"corpus_shader\": {\n");
    std::printf("      \"proof_scope\": \"fresh_corpus_position_color_hlsl_graphics_pso_presented_readback\",\n");
    std::printf("      \"vs_path\": \"%s\",\n", json_escape(d3d.corpus_shader.vs_path).c_str());
    std::printf("      \"ps_path\": \"%s\",\n", json_escape(d3d.corpus_shader.ps_path).c_str());
    std::printf("      \"vs_loaded\": %s,\n", d3d.corpus_shader.vs_loaded ? "true" : "false");
    std::printf("      \"ps_loaded\": %s,\n", d3d.corpus_shader.ps_loaded ? "true" : "false");
    std::printf("      \"vs_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.corpus_shader.vs_bytes));
    std::printf("      \"ps_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.corpus_shader.ps_bytes));
    std::printf("      \"vs_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(d3d.corpus_shader.vs_fnv1a64));
    std::printf("      \"ps_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(d3d.corpus_shader.ps_fnv1a64));
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.corpus_shader.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"PositionColorVS_vs_5_0\": \"%s\",\n", hr_hex(d3d.corpus_shader.compile_vs_hr).c_str());
    std::printf("      \"PositionColorPS_ps_5_0\": \"%s\",\n", hr_hex(d3d.corpus_shader.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n",
                hr_hex(d3d.corpus_shader.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.corpus_shader.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.corpus_shader.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.corpus_shader.create_vertex_buffer_hr).c_str());
    std::printf("      \"draw_calls\": %u,\n", d3d.corpus_shader.draw_calls);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.corpus_shader.vertices_per_draw);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.corpus_shader.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.corpus_shader.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.corpus_shader.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.corpus_shader.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.corpus_shader.expected_rgba[0],
                d3d.corpus_shader.expected_rgba[1], d3d.corpus_shader.expected_rgba[2],
                d3d.corpus_shader.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.corpus_shader.present_rgba[0],
                d3d.corpus_shader.present_rgba[1], d3d.corpus_shader.present_rgba[2],
                d3d.corpus_shader.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.corpus_shader.present_last_rgba[0],
                d3d.corpus_shader.present_last_rgba[1], d3d.corpus_shader.present_last_rgba[2],
                d3d.corpus_shader.present_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.corpus_shader.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.corpus_shader.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"srv_sample\": {\n");
    std::printf(
        "      \"proof_scope\": \"shader_visible_srv_descriptor_table_texture2d_sample_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.srv_sample.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"vs_5_0\": \"%s\",\n", hr_hex(d3d.srv_sample.compile_vs_hr).c_str());
    std::printf("      \"ps_5_0_Texture2D_Sample\": \"%s\",\n", hr_hex(d3d.srv_sample.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_descriptor_table_static_sampler\": \"%s\",\n",
                hr_hex(d3d.srv_sample.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.srv_sample.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.srv_sample.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.srv_sample.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateTexture2D_R8G8B8A8_UNORM\": \"%s\",\n",
                hr_hex(d3d.srv_sample.create_texture_hr).c_str());
    std::printf("      \"CreateUploadBuffer\": \"%s\",\n", hr_hex(d3d.srv_sample.create_upload_hr).c_str());
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE\": \"%s\",\n",
                hr_hex(d3d.srv_sample.create_descriptor_heap_hr).c_str());
    std::printf("      \"CreateShaderResourceView_descriptors\": %u,\n", d3d.srv_sample.srv_descriptors_created);
    std::printf("      \"CopyTextureRegion_commands\": %u,\n", d3d.srv_sample.copy_texture_region_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.srv_sample.transition_barriers);
    std::printf("      \"upload_filled\": %s,\n", d3d.srv_sample.upload_filled ? "true" : "false");
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.srv_sample.fence_wait_ok ? "true" : "false");
    std::printf("      \"draw_calls\": %u,\n", d3d.srv_sample.draw_calls);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.srv_sample.vertices_per_draw);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.srv_sample.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.srv_sample.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.srv_sample.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.srv_sample.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.srv_sample.expected_rgba[0],
                d3d.srv_sample.expected_rgba[1], d3d.srv_sample.expected_rgba[2], d3d.srv_sample.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.srv_sample.present_rgba[0],
                d3d.srv_sample.present_rgba[1], d3d.srv_sample.present_rgba[2], d3d.srv_sample.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.srv_sample.present_last_rgba[0],
                d3d.srv_sample.present_last_rgba[1], d3d.srv_sample.present_last_rgba[2],
                d3d.srv_sample.present_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.srv_sample.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.srv_sample.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"textured_3d\": {\n");
    std::printf("      \"proof_scope\": "
                "\"rotating_rgb_triangular_pyramid_three_engine_texture_faces_depth_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.textured_3d.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"vs_5_0_rotating_3d\": \"%s\",\n", hr_hex(d3d.textured_3d.compile_vs_hr).c_str());
    std::printf("      \"ps_5_0_three_Texture2D_Sample\": \"%s\",\n", hr_hex(d3d.textured_3d.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_three_srv_table_static_sampler\": \"%s\",\n",
                hr_hex(d3d.textured_3d.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.textured_3d.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.textured_3d.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.textured_3d.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateDepthTexture_D32_FLOAT_ALLOW_DEPTH_STENCIL\": \"%s\",\n",
                hr_hex(d3d.textured_3d.create_depth_texture_hr).c_str());
    std::printf("      \"CreateDescriptorHeap_DSV\": \"%s\",\n", hr_hex(d3d.textured_3d.create_dsv_heap_hr).c_str());
    std::printf("      \"depth_stencil_policy\": \"D32_FLOAT_depth_only_stencil_invalid_expected\",\n");
    std::printf("      \"CreateTexture2D_R8G8B8A8_UNORM\": [");
    for (UINT face = 0; face < kTextured3DFaceCount; ++face)
        std::printf("%s\"%s\"", face ? ", " : "", hr_hex(d3d.textured_3d.create_texture_hr[face]).c_str());
    std::printf("],\n");
    std::printf("      \"CreateUploadBuffer\": [");
    for (UINT face = 0; face < kTextured3DFaceCount; ++face)
        std::printf("%s\"%s\"", face ? ", " : "", hr_hex(d3d.textured_3d.create_upload_hr[face]).c_str());
    std::printf("],\n");
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE\": \"%s\",\n",
                hr_hex(d3d.textured_3d.create_descriptor_heap_hr).c_str());
    std::printf("      \"face_count\": %u,\n", d3d.textured_3d.face_count);
    std::printf("      \"unique_families\": %u,\n", d3d.textured_3d.unique_families);
    std::printf("      \"textures_created\": %u,\n", d3d.textured_3d.textures_created);
    std::printf("      \"upload_buffers_created\": %u,\n", d3d.textured_3d.upload_buffers_created);
    std::printf("      \"uploads_filled\": %u,\n", d3d.textured_3d.uploads_filled);
    std::printf("      \"CreateShaderResourceView_descriptors\": %u,\n", d3d.textured_3d.srv_descriptors_created);
    std::printf("      \"CreateDepthStencilView_descriptors\": %u,\n", d3d.textured_3d.dsv_descriptors_created);
    std::printf("      \"ClearDepthStencilView_commands\": %u,\n", d3d.textured_3d.clear_depth_commands);
    std::printf(
        "      \"depth_overlap_policy\": \"front_unity_drawn_first_back_unreal_drawn_last_requires_D32_depth\",\n");
    std::printf("      \"CopyTextureRegion_commands\": %u,\n", d3d.textured_3d.copy_texture_region_commands);
    std::printf("      \"transition_barriers\": %u,\n", d3d.textured_3d.transition_barriers);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.textured_3d.fence_wait_ok ? "true" : "false");
    std::printf("      \"vertices_created\": %u,\n", d3d.textured_3d.vertices_created);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.textured_3d.vertices_per_draw);
    std::printf("      \"vertex_buffer_updates\": %u,\n", d3d.textured_3d.vertex_buffer_updates);
    std::printf("      \"root_constant_sets\": %u,\n", d3d.textured_3d.root_constant_sets);
    std::printf("      \"draw_calls\": %u,\n", d3d.textured_3d.draw_calls);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.textured_3d.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.textured_3d.present_sample_matches);
    std::printf("      \"depth_overlap_sample_x\": %u,\n", d3d.textured_3d.depth_overlap_sample_x);
    std::printf("      \"depth_overlap_sample_y\": %u,\n", d3d.textured_3d.depth_overlap_sample_y);
    std::printf("      \"depth_overlap_expected_rgba\": [%u, %u, %u, %u],\n",
                d3d.textured_3d.depth_overlap_expected_rgba[0], d3d.textured_3d.depth_overlap_expected_rgba[1],
                d3d.textured_3d.depth_overlap_expected_rgba[2], d3d.textured_3d.depth_overlap_expected_rgba[3]);
    std::printf("      \"depth_overlap_present_rgba\": [%u, %u, %u, %u],\n",
                d3d.textured_3d.depth_overlap_present_rgba[0], d3d.textured_3d.depth_overlap_present_rgba[1],
                d3d.textured_3d.depth_overlap_present_rgba[2], d3d.textured_3d.depth_overlap_present_rgba[3]);
    std::printf("      \"depth_overlap_samples_checked\": %u,\n", d3d.textured_3d.depth_overlap_samples_checked);
    std::printf("      \"depth_overlap_sample_matches\": %u,\n", d3d.textured_3d.depth_overlap_sample_matches);
    std::printf("      \"faces\": [\n");
    for (UINT face = 0; face < kTextured3DFaceCount; ++face) {
        const Textured3DFaceStats& face_stats = d3d.textured_3d.faces[face];
        std::printf("        {\n");
        std::printf("          \"index\": %u,\n", face);
        std::printf("          \"family\": \"%s\",\n", json_escape(face_stats.family).c_str());
        std::printf("          \"label\": \"%s\",\n", json_escape(face_stats.label).c_str());
        std::printf("          \"extension\": \"%s\",\n", json_escape(face_stats.extension).c_str());
        std::printf("          \"destination\": \"%s\",\n", json_escape(face_stats.destination).c_str());
        std::printf("          \"source_path\": \"%s\",\n", json_escape(face_stats.source_path).c_str());
        std::printf("          \"sha256\": \"%s\",\n", json_escape(face_stats.sha256).c_str());
        std::printf("          \"declared_size\": %llu,\n", static_cast<unsigned long long>(face_stats.declared_size));
        std::printf("          \"file_fnv1a64\": \"%016llx\",\n",
                    static_cast<unsigned long long>(face_stats.file_fnv1a64));
        std::printf("          \"upload_fnv1a64\": \"%016llx\",\n",
                    static_cast<unsigned long long>(face_stats.upload_fnv1a64));
        std::printf("          \"bytes_from_file\": %u,\n", face_stats.bytes_from_file);
        std::printf("          \"sample_x\": %u,\n", face_stats.sample_x);
        std::printf("          \"sample_y\": %u,\n", face_stats.sample_y);
        std::printf("          \"expected_rgba\": [%u, %u, %u, %u],\n", face_stats.expected_rgba[0],
                    face_stats.expected_rgba[1], face_stats.expected_rgba[2], face_stats.expected_rgba[3]);
        std::printf("          \"present_rgba\": [%u, %u, %u, %u],\n", face_stats.present_rgba[0],
                    face_stats.present_rgba[1], face_stats.present_rgba[2], face_stats.present_rgba[3]);
        std::printf("          \"samples_checked\": %u,\n", face_stats.samples_checked);
        std::printf("          \"sample_matches\": %u\n", face_stats.sample_matches);
        std::printf("        }%s\n", face + 1u < kTextured3DFaceCount ? "," : "");
    }
    std::printf("      ],\n");
    std::printf("      \"present_ok\": %s,\n", d3d.textured_3d.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.textured_3d.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"cbv_sample\": {\n");
    std::printf("      \"proof_scope\": \"shader_visible_cbv_descriptor_table_constant_buffer_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.cbv_sample.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"vs_5_0\": \"%s\",\n", hr_hex(d3d.cbv_sample.compile_vs_hr).c_str());
    std::printf("      \"ps_5_0_cbuffer\": \"%s\",\n", hr_hex(d3d.cbv_sample.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_cbv_descriptor_table\": \"%s\",\n",
                hr_hex(d3d.cbv_sample.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.cbv_sample.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.cbv_sample.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.cbv_sample.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateConstantBuffer\": \"%s\",\n", hr_hex(d3d.cbv_sample.create_constant_buffer_hr).c_str());
    std::printf("      \"CreateDescriptorHeap_CBV_SRV_UAV_SHADER_VISIBLE\": \"%s\",\n",
                hr_hex(d3d.cbv_sample.create_descriptor_heap_hr).c_str());
    std::printf("      \"CreateConstantBufferView_descriptors\": %u,\n", d3d.cbv_sample.cbv_descriptors_created);
    std::printf("      \"constant_buffer_filled\": %s,\n", d3d.cbv_sample.constant_buffer_filled ? "true" : "false");
    std::printf("      \"draw_calls\": %u,\n", d3d.cbv_sample.draw_calls);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.cbv_sample.vertices_per_draw);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.cbv_sample.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.cbv_sample.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.cbv_sample.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.cbv_sample.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.cbv_sample.expected_rgba[0],
                d3d.cbv_sample.expected_rgba[1], d3d.cbv_sample.expected_rgba[2], d3d.cbv_sample.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.cbv_sample.present_rgba[0],
                d3d.cbv_sample.present_rgba[1], d3d.cbv_sample.present_rgba[2], d3d.cbv_sample.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.cbv_sample.present_last_rgba[0],
                d3d.cbv_sample.present_last_rgba[1], d3d.cbv_sample.present_last_rgba[2],
                d3d.cbv_sample.present_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.cbv_sample.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.cbv_sample.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"indexed_draw\": {\n");
    std::printf("      \"proof_scope\": "
                "\"r16_r32_subrange_base_append_dynamic_stride_execute_indirect_indexed_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.indexed_draw.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"indexed_vs_vs_5_0\": \"%s\",\n", hr_hex(d3d.indexed_draw.compile_vs_hr).c_str());
    std::printf("      \"indexed_ps_ps_5_0\": \"%s\",\n", hr_hex(d3d.indexed_draw.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n", hr_hex(d3d.indexed_draw.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.indexed_draw.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.indexed_draw.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.indexed_draw.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateDynamicStrideVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_dynamic_stride_vertex_buffer_hr).c_str());
    std::printf("      \"CreateIndexBuffer\": \"%s\",\n", hr_hex(d3d.indexed_draw.create_index_buffer_hr).c_str());
    std::printf("      \"CreateIndexBufferR32\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_index_buffer_r32_hr).c_str());
    std::printf("      \"CreateIndexBufferNegativeBase\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_index_buffer_negative_base_hr).c_str());
    std::printf("      \"CreateIndexBufferExecuteIndirectIndexed\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_index_buffer_execute_indirect_hr).c_str());
    std::printf("      \"CreateExecuteIndirectIndexedArgumentBuffer\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_execute_indirect_indexed_argument_buffer_hr).c_str());
    std::printf("      \"CreateExecuteIndirectIndexedCommandSignature\": \"%s\",\n",
                hr_hex(d3d.indexed_draw.create_execute_indirect_indexed_command_signature_hr).c_str());
    std::printf("      \"append_aligned_element\": %s,\n", d3d.indexed_draw.append_aligned_element ? "true" : "false");
    std::printf("      \"append_aligned_color_expected_offset\": %u,\n",
                d3d.indexed_draw.append_aligned_color_expected_offset);
    std::printf("      \"vertices_created\": %u,\n", d3d.indexed_draw.vertices_created);
    std::printf("      \"vertex_buffer_size\": %u,\n", d3d.indexed_draw.vertex_buffer_size);
    std::printf("      \"vertex_view_byte_offset\": %u,\n", d3d.indexed_draw.vertex_view_byte_offset);
    std::printf("      \"dynamic_stride_vertices_created\": %u,\n", d3d.indexed_draw.dynamic_stride_vertices_created);
    std::printf("      \"dynamic_stride_vertex_buffer_size\": %u,\n",
                d3d.indexed_draw.dynamic_stride_vertex_buffer_size);
    std::printf("      \"dynamic_stride\": %u,\n", d3d.indexed_draw.dynamic_stride);
    std::printf("      \"indices_created\": %u,\n", d3d.indexed_draw.indices_created);
    std::printf("      \"index_format\": %u,\n", d3d.indexed_draw.index_format);
    std::printf("      \"index_buffer_size\": %u,\n", d3d.indexed_draw.index_buffer_size);
    std::printf("      \"index_view_byte_offset\": %u,\n", d3d.indexed_draw.index_view_byte_offset);
    std::printf("      \"start_index_location\": %u,\n", d3d.indexed_draw.start_index_location);
    std::printf("      \"r32_indices_created\": %u,\n", d3d.indexed_draw.r32_indices_created);
    std::printf("      \"r32_index_format\": %u,\n", d3d.indexed_draw.r32_index_format);
    std::printf("      \"r32_index_buffer_size\": %u,\n", d3d.indexed_draw.r32_index_buffer_size);
    std::printf("      \"r32_index_view_byte_offset\": %u,\n", d3d.indexed_draw.r32_index_view_byte_offset);
    std::printf("      \"r32_start_index_location\": %u,\n", d3d.indexed_draw.r32_start_index_location);
    std::printf("      \"r32_base_vertex_location\": %d,\n", d3d.indexed_draw.r32_base_vertex_location);
    std::printf("      \"negative_base_indices_created\": %u,\n", d3d.indexed_draw.negative_base_indices_created);
    std::printf("      \"negative_base_index_format\": %u,\n", d3d.indexed_draw.negative_base_index_format);
    std::printf("      \"negative_base_index_buffer_size\": %u,\n", d3d.indexed_draw.negative_base_index_buffer_size);
    std::printf("      \"negative_base_start_index_location\": %u,\n",
                d3d.indexed_draw.negative_base_start_index_location);
    std::printf("      \"negative_base_vertex_location\": %d,\n", d3d.indexed_draw.negative_base_vertex_location);
    std::printf("      \"execute_indirect_indexed_indices_created\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_indices_created);
    std::printf("      \"execute_indirect_indexed_index_format\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_index_format);
    std::printf("      \"execute_indirect_indexed_index_buffer_size\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_index_buffer_size);
    std::printf("      \"execute_indirect_indexed_argument_byte_stride\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_argument_byte_stride);
    std::printf("      \"execute_indirect_indexed_command_signature_arguments\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_command_signature_arguments);
    std::printf("      \"execute_indirect_indexed_max_command_count\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_max_command_count);
    std::printf("      \"execute_indirect_indexed_index_count\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_index_count);
    std::printf("      \"execute_indirect_indexed_instance_count\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_instance_count);
    std::printf("      \"execute_indirect_indexed_start_index_location\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_start_index_location);
    std::printf("      \"execute_indirect_indexed_base_vertex_location\": %d,\n",
                d3d.indexed_draw.execute_indirect_indexed_base_vertex_location);
    std::printf("      \"execute_indirect_indexed_start_instance_location\": %u,\n",
                d3d.indexed_draw.execute_indirect_indexed_start_instance_location);
    std::printf("      \"draw_indexed_calls\": %u,\n", d3d.indexed_draw.draw_indexed_calls);
    std::printf("      \"draw_indexed_r32_calls\": %u,\n", d3d.indexed_draw.draw_indexed_r32_calls);
    std::printf("      \"draw_indexed_negative_base_calls\": %u,\n", d3d.indexed_draw.draw_indexed_negative_base_calls);
    std::printf("      \"draw_indexed_dynamic_stride_calls\": %u,\n",
                d3d.indexed_draw.draw_indexed_dynamic_stride_calls);
    std::printf("      \"execute_indirect_indexed_calls\": %u,\n", d3d.indexed_draw.execute_indirect_indexed_calls);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.indexed_draw.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.indexed_draw.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.indexed_draw.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.indexed_draw.present_pixel_matches);
    std::printf("      \"present_r32_samples_checked\": %u,\n", d3d.indexed_draw.present_r32_samples_checked);
    std::printf("      \"present_r32_sample_matches\": %u,\n", d3d.indexed_draw.present_r32_sample_matches);
    std::printf("      \"present_r32_pixels_checked\": %u,\n", d3d.indexed_draw.present_r32_pixels_checked);
    std::printf("      \"present_r32_pixel_matches\": %u,\n", d3d.indexed_draw.present_r32_pixel_matches);
    std::printf("      \"present_negative_base_samples_checked\": %u,\n",
                d3d.indexed_draw.present_negative_base_samples_checked);
    std::printf("      \"present_negative_base_sample_matches\": %u,\n",
                d3d.indexed_draw.present_negative_base_sample_matches);
    std::printf("      \"present_negative_base_pixels_checked\": %u,\n",
                d3d.indexed_draw.present_negative_base_pixels_checked);
    std::printf("      \"present_negative_base_pixel_matches\": %u,\n",
                d3d.indexed_draw.present_negative_base_pixel_matches);
    std::printf("      \"present_dynamic_stride_samples_checked\": %u,\n",
                d3d.indexed_draw.present_dynamic_stride_samples_checked);
    std::printf("      \"present_dynamic_stride_sample_matches\": %u,\n",
                d3d.indexed_draw.present_dynamic_stride_sample_matches);
    std::printf("      \"present_dynamic_stride_pixels_checked\": %u,\n",
                d3d.indexed_draw.present_dynamic_stride_pixels_checked);
    std::printf("      \"present_dynamic_stride_pixel_matches\": %u,\n",
                d3d.indexed_draw.present_dynamic_stride_pixel_matches);
    std::printf("      \"present_execute_indirect_indexed_samples_checked\": %u,\n",
                d3d.indexed_draw.present_execute_indirect_indexed_samples_checked);
    std::printf("      \"present_execute_indirect_indexed_sample_matches\": %u,\n",
                d3d.indexed_draw.present_execute_indirect_indexed_sample_matches);
    std::printf("      \"present_execute_indirect_indexed_pixels_checked\": %u,\n",
                d3d.indexed_draw.present_execute_indirect_indexed_pixels_checked);
    std::printf("      \"present_execute_indirect_indexed_pixel_matches\": %u,\n",
                d3d.indexed_draw.present_execute_indirect_indexed_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.expected_rgba[0],
                d3d.indexed_draw.expected_rgba[1], d3d.indexed_draw.expected_rgba[2],
                d3d.indexed_draw.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.present_rgba[0],
                d3d.indexed_draw.present_rgba[1], d3d.indexed_draw.present_rgba[2], d3d.indexed_draw.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.present_last_rgba[0],
                d3d.indexed_draw.present_last_rgba[1], d3d.indexed_draw.present_last_rgba[2],
                d3d.indexed_draw.present_last_rgba[3]);
    std::printf("      \"expected_r32_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.expected_r32_rgba[0],
                d3d.indexed_draw.expected_r32_rgba[1], d3d.indexed_draw.expected_r32_rgba[2],
                d3d.indexed_draw.expected_r32_rgba[3]);
    std::printf("      \"present_r32_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.present_r32_rgba[0],
                d3d.indexed_draw.present_r32_rgba[1], d3d.indexed_draw.present_r32_rgba[2],
                d3d.indexed_draw.present_r32_rgba[3]);
    std::printf("      \"present_r32_last_rgba\": [%u, %u, %u, %u],\n", d3d.indexed_draw.present_r32_last_rgba[0],
                d3d.indexed_draw.present_r32_last_rgba[1], d3d.indexed_draw.present_r32_last_rgba[2],
                d3d.indexed_draw.present_r32_last_rgba[3]);
    std::printf("      \"expected_negative_base_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.expected_negative_base_rgba[0], d3d.indexed_draw.expected_negative_base_rgba[1],
                d3d.indexed_draw.expected_negative_base_rgba[2], d3d.indexed_draw.expected_negative_base_rgba[3]);
    std::printf("      \"present_negative_base_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.present_negative_base_rgba[0], d3d.indexed_draw.present_negative_base_rgba[1],
                d3d.indexed_draw.present_negative_base_rgba[2], d3d.indexed_draw.present_negative_base_rgba[3]);
    std::printf(
        "      \"present_negative_base_last_rgba\": [%u, %u, %u, %u],\n",
        d3d.indexed_draw.present_negative_base_last_rgba[0], d3d.indexed_draw.present_negative_base_last_rgba[1],
        d3d.indexed_draw.present_negative_base_last_rgba[2], d3d.indexed_draw.present_negative_base_last_rgba[3]);
    std::printf("      \"expected_dynamic_stride_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.expected_dynamic_stride_rgba[0], d3d.indexed_draw.expected_dynamic_stride_rgba[1],
                d3d.indexed_draw.expected_dynamic_stride_rgba[2], d3d.indexed_draw.expected_dynamic_stride_rgba[3]);
    std::printf("      \"present_dynamic_stride_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.present_dynamic_stride_rgba[0], d3d.indexed_draw.present_dynamic_stride_rgba[1],
                d3d.indexed_draw.present_dynamic_stride_rgba[2], d3d.indexed_draw.present_dynamic_stride_rgba[3]);
    std::printf(
        "      \"present_dynamic_stride_last_rgba\": [%u, %u, %u, %u],\n",
        d3d.indexed_draw.present_dynamic_stride_last_rgba[0], d3d.indexed_draw.present_dynamic_stride_last_rgba[1],
        d3d.indexed_draw.present_dynamic_stride_last_rgba[2], d3d.indexed_draw.present_dynamic_stride_last_rgba[3]);
    std::printf("      \"expected_execute_indirect_indexed_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.expected_execute_indirect_indexed_rgba[0],
                d3d.indexed_draw.expected_execute_indirect_indexed_rgba[1],
                d3d.indexed_draw.expected_execute_indirect_indexed_rgba[2],
                d3d.indexed_draw.expected_execute_indirect_indexed_rgba[3]);
    std::printf("      \"present_execute_indirect_indexed_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.present_execute_indirect_indexed_rgba[0],
                d3d.indexed_draw.present_execute_indirect_indexed_rgba[1],
                d3d.indexed_draw.present_execute_indirect_indexed_rgba[2],
                d3d.indexed_draw.present_execute_indirect_indexed_rgba[3]);
    std::printf("      \"present_execute_indirect_indexed_last_rgba\": [%u, %u, %u, %u],\n",
                d3d.indexed_draw.present_execute_indirect_indexed_last_rgba[0],
                d3d.indexed_draw.present_execute_indirect_indexed_last_rgba[1],
                d3d.indexed_draw.present_execute_indirect_indexed_last_rgba[2],
                d3d.indexed_draw.present_execute_indirect_indexed_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.indexed_draw.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.indexed_draw.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"multi_slot_instance_root\": {\n");
    std::printf("      \"proof_scope\": "
                "\"two_slot_per_instance_step_rate_start_instance_root_constants_mutation_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n",
                d3d.multi_slot_instance_root.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"multi_slot_vs_vs_5_0\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.compile_vs_hr).c_str());
    std::printf("      \"multi_slot_ps_ps_5_0\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_root_constants\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.create_pso_hr).c_str());
    std::printf("      \"CreatePositionVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.create_position_buffer_hr).c_str());
    std::printf("      \"CreateInstanceVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.multi_slot_instance_root.create_instance_buffer_hr).c_str());
    std::printf("      \"input_slots\": %u,\n", d3d.multi_slot_instance_root.input_slots);
    std::printf("      \"position_vertices_created\": %u,\n", d3d.multi_slot_instance_root.position_vertices_created);
    std::printf("      \"instance_records_created\": %u,\n", d3d.multi_slot_instance_root.instance_records_created);
    std::printf("      \"selected_instance_record\": %u,\n", d3d.multi_slot_instance_root.selected_instance_record);
    std::printf("      \"per_instance_step_rate\": %u,\n", d3d.multi_slot_instance_root.per_instance_step_rate);
    std::printf("      \"start_instance_location\": %u,\n", d3d.multi_slot_instance_root.start_instance_location);
    std::printf("      \"vertex_count_per_draw\": %u,\n", d3d.multi_slot_instance_root.vertex_count_per_draw);
    std::printf("      \"instance_count_per_draw\": %u,\n", d3d.multi_slot_instance_root.instance_count_per_draw);
    std::printf("      \"slot0_stride\": %u,\n", d3d.multi_slot_instance_root.slot0_stride);
    std::printf("      \"slot1_stride\": %u,\n", d3d.multi_slot_instance_root.slot1_stride);
    std::printf("      \"root_constant_float_count\": %u,\n", d3d.multi_slot_instance_root.root_constant_float_count);
    std::printf("      \"root_constant_sets\": %u,\n", d3d.multi_slot_instance_root.root_constant_sets);
    std::printf("      \"draw_calls\": %u,\n", d3d.multi_slot_instance_root.draw_calls);
    std::printf("      \"first_draw_calls\": %u,\n", d3d.multi_slot_instance_root.first_draw_calls);
    std::printf("      \"second_draw_calls\": %u,\n", d3d.multi_slot_instance_root.second_draw_calls);
    std::printf("      \"present_first_samples_checked\": %u,\n",
                d3d.multi_slot_instance_root.present_first_samples_checked);
    std::printf("      \"present_first_sample_matches\": %u,\n",
                d3d.multi_slot_instance_root.present_first_sample_matches);
    std::printf("      \"present_first_pixels_checked\": %u,\n",
                d3d.multi_slot_instance_root.present_first_pixels_checked);
    std::printf("      \"present_first_pixel_matches\": %u,\n",
                d3d.multi_slot_instance_root.present_first_pixel_matches);
    std::printf("      \"present_second_samples_checked\": %u,\n",
                d3d.multi_slot_instance_root.present_second_samples_checked);
    std::printf("      \"present_second_sample_matches\": %u,\n",
                d3d.multi_slot_instance_root.present_second_sample_matches);
    std::printf("      \"present_second_pixels_checked\": %u,\n",
                d3d.multi_slot_instance_root.present_second_pixels_checked);
    std::printf("      \"present_second_pixel_matches\": %u,\n",
                d3d.multi_slot_instance_root.present_second_pixel_matches);
    std::printf(
        "      \"expected_first_rgba\": [%u, %u, %u, %u],\n", d3d.multi_slot_instance_root.expected_first_rgba[0],
        d3d.multi_slot_instance_root.expected_first_rgba[1], d3d.multi_slot_instance_root.expected_first_rgba[2],
        d3d.multi_slot_instance_root.expected_first_rgba[3]);
    std::printf("      \"present_first_rgba\": [%u, %u, %u, %u],\n", d3d.multi_slot_instance_root.present_first_rgba[0],
                d3d.multi_slot_instance_root.present_first_rgba[1], d3d.multi_slot_instance_root.present_first_rgba[2],
                d3d.multi_slot_instance_root.present_first_rgba[3]);
    std::printf("      \"present_first_last_rgba\": [%u, %u, %u, %u],\n",
                d3d.multi_slot_instance_root.present_first_last_rgba[0],
                d3d.multi_slot_instance_root.present_first_last_rgba[1],
                d3d.multi_slot_instance_root.present_first_last_rgba[2],
                d3d.multi_slot_instance_root.present_first_last_rgba[3]);
    std::printf(
        "      \"expected_second_rgba\": [%u, %u, %u, %u],\n", d3d.multi_slot_instance_root.expected_second_rgba[0],
        d3d.multi_slot_instance_root.expected_second_rgba[1], d3d.multi_slot_instance_root.expected_second_rgba[2],
        d3d.multi_slot_instance_root.expected_second_rgba[3]);
    std::printf(
        "      \"present_second_rgba\": [%u, %u, %u, %u],\n", d3d.multi_slot_instance_root.present_second_rgba[0],
        d3d.multi_slot_instance_root.present_second_rgba[1], d3d.multi_slot_instance_root.present_second_rgba[2],
        d3d.multi_slot_instance_root.present_second_rgba[3]);
    std::printf("      \"present_second_last_rgba\": [%u, %u, %u, %u],\n",
                d3d.multi_slot_instance_root.present_second_last_rgba[0],
                d3d.multi_slot_instance_root.present_second_last_rgba[1],
                d3d.multi_slot_instance_root.present_second_last_rgba[2],
                d3d.multi_slot_instance_root.present_second_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.multi_slot_instance_root.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.multi_slot_instance_root.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"tessellation_fallback\": {\n");
    std::printf(
        "      \"proof_scope\": \"hs_ds_patch_topology_d3d12_native_metal_tessellation_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.tessellation_fallback.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"tess_vs_vs_5_0\": \"%s\",\n", hr_hex(d3d.tessellation_fallback.compile_vs_hr).c_str());
    std::printf("      \"tess_hs_hs_5_0\": \"%s\",\n", hr_hex(d3d.tessellation_fallback.compile_hs_hr).c_str());
    std::printf("      \"tess_ds_ds_5_0\": \"%s\",\n", hr_hex(d3d.tessellation_fallback.compile_ds_hr).c_str());
    std::printf("      \"tess_ps_ps_5_0\": \"%s\",\n", hr_hex(d3d.tessellation_fallback.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n",
                hr_hex(d3d.tessellation_fallback.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.tessellation_fallback.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState_PATCH_HS_DS\": \"%s\",\n",
                hr_hex(d3d.tessellation_fallback.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.tessellation_fallback.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateIndexBuffer\": \"%s\",\n",
                hr_hex(d3d.tessellation_fallback.create_index_buffer_hr).c_str());
    std::printf("      \"patch_control_points\": %u,\n", d3d.tessellation_fallback.patch_control_points);
    std::printf("      \"vertices_created\": %u,\n", d3d.tessellation_fallback.vertices_created);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.tessellation_fallback.vertices_per_draw);
    std::printf("      \"indices_created\": %u,\n", d3d.tessellation_fallback.indices_created);
    std::printf("      \"indices_per_draw\": %u,\n", d3d.tessellation_fallback.indices_per_draw);
    std::printf("      \"draw_calls\": %u,\n", d3d.tessellation_fallback.draw_calls);
    std::printf("      \"draw_indexed_calls\": %u,\n", d3d.tessellation_fallback.draw_indexed_calls);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.tessellation_fallback.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.tessellation_fallback.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.tessellation_fallback.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.tessellation_fallback.present_pixel_matches);
    std::printf("      \"native_tessellation_required\": %s,\n",
                d3d.tessellation_fallback.native_tessellation_required ? "true" : "false");
    std::printf("      \"native_tessellation_resolved\": %s,\n",
                d3d.tessellation_fallback.native_tessellation_resolved ? "true" : "false");
    std::printf("      \"native_draw_encoded\": %s,\n",
                d3d.tessellation_fallback.native_draw_encoded ? "true" : "false");
    std::printf("      \"native_indexed_draw_encoded\": %s,\n",
                d3d.tessellation_fallback.native_indexed_draw_encoded ? "true" : "false");
    std::printf("      \"blocked_expected\": %s,\n", d3d.tessellation_fallback.blocked_expected ? "true" : "false");
    std::printf("      \"fallback_draw_encoded\": %s,\n",
                d3d.tessellation_fallback.fallback_draw_encoded ? "true" : "false");
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.tessellation_fallback.expected_rgba[0],
                d3d.tessellation_fallback.expected_rgba[1], d3d.tessellation_fallback.expected_rgba[2],
                d3d.tessellation_fallback.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.tessellation_fallback.present_rgba[0],
                d3d.tessellation_fallback.present_rgba[1], d3d.tessellation_fallback.present_rgba[2],
                d3d.tessellation_fallback.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.tessellation_fallback.present_last_rgba[0],
                d3d.tessellation_fallback.present_last_rgba[1], d3d.tessellation_fallback.present_last_rgba[2],
                d3d.tessellation_fallback.present_last_rgba[3]);
    std::printf("      \"indexed_expected_rgba\": [%u, %u, %u, %u],\n",
                d3d.tessellation_fallback.indexed_expected_rgba[0], d3d.tessellation_fallback.indexed_expected_rgba[1],
                d3d.tessellation_fallback.indexed_expected_rgba[2], d3d.tessellation_fallback.indexed_expected_rgba[3]);
    std::printf("      \"indexed_present_rgba\": [%u, %u, %u, %u],\n",
                d3d.tessellation_fallback.indexed_present_rgba[0], d3d.tessellation_fallback.indexed_present_rgba[1],
                d3d.tessellation_fallback.indexed_present_rgba[2], d3d.tessellation_fallback.indexed_present_rgba[3]);
    std::printf(
        "      \"indexed_present_last_rgba\": [%u, %u, %u, %u],\n",
        d3d.tessellation_fallback.indexed_present_last_rgba[0], d3d.tessellation_fallback.indexed_present_last_rgba[1],
        d3d.tessellation_fallback.indexed_present_last_rgba[2], d3d.tessellation_fallback.indexed_present_last_rgba[3]);
    std::printf("      \"indexed_present_samples_checked\": %u,\n",
                d3d.tessellation_fallback.indexed_present_samples_checked);
    std::printf("      \"indexed_present_sample_matches\": %u,\n",
                d3d.tessellation_fallback.indexed_present_sample_matches);
    std::printf("      \"indexed_present_pixels_checked\": %u,\n",
                d3d.tessellation_fallback.indexed_present_pixels_checked);
    std::printf("      \"indexed_present_pixel_matches\": %u,\n",
                d3d.tessellation_fallback.indexed_present_pixel_matches);
    std::printf("      \"indexed_present_ok\": %s,\n",
                d3d.tessellation_fallback.indexed_present_pass ? "true" : "false");
    std::printf("      \"tessellation_factor_debug_dump\": {\"format\": \"half\", \"edge_half\": \"0x%04x\", "
                "\"inside_half\": \"0x%04x\", \"float_value\": %.1f},\n",
                d3d.tessellation_fallback.tessellation_factor_half_edge,
                d3d.tessellation_fallback.tessellation_factor_half_inside,
                d3d.tessellation_fallback.tessellation_factor_float);
    std::printf("      \"tessellation_factor_history\": [");
    for (size_t i = 0; i < d3d.tessellation_fallback.tessellation_factor_history.size(); ++i)
        std::printf("%s%u", i ? ", " : "", d3d.tessellation_fallback.tessellation_factor_history[i]);
    std::printf("],\n");
    std::printf("      \"progressive_triangle_proof_scope\": "
                "\"progressive_rgb_tessellated_triangle_seed_point_to_full_triangle\",\n");
    std::printf("      \"progressive_triangle_frames_validated\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_frames_validated);
    std::printf("      \"progressive_triangle_samples_checked\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_samples_checked);
    std::printf("      \"progressive_triangle_coverage_counts\": [");
    for (size_t i = 0; i < d3d.tessellation_fallback.progressive_triangle_coverage_counts.size(); ++i)
        std::printf("%s%u", i ? ", " : "", d3d.tessellation_fallback.progressive_triangle_coverage_counts[i]);
    std::printf("],\n");
    std::printf("      \"progressive_triangle_seed_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_seed_pixels);
    std::printf("      \"progressive_triangle_final_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_final_pixels);
    std::printf("      \"progressive_triangle_peak_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_peak_pixels);
    std::printf("      \"progressive_triangle_final_red_dominant_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_final_red_dominant_pixels);
    std::printf("      \"progressive_triangle_final_green_dominant_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_final_green_dominant_pixels);
    std::printf("      \"progressive_triangle_final_blue_dominant_pixels\": %u,\n",
                d3d.tessellation_fallback.progressive_triangle_final_blue_dominant_pixels);
    std::printf("      \"progressive_triangle_coverage_monotonic\": %s,\n",
                d3d.tessellation_fallback.progressive_triangle_coverage_monotonic ? "true" : "false");
    std::printf("      \"progressive_triangle_seed_point_ok\": %s,\n",
                d3d.tessellation_fallback.progressive_triangle_seed_point_pass ? "true" : "false");
    std::printf("      \"progressive_triangle_full_ok\": %s,\n",
                d3d.tessellation_fallback.progressive_triangle_full_pass ? "true" : "false");
    std::printf("      \"progressive_triangle_rgb_channels_present\": %s,\n",
                d3d.tessellation_fallback.progressive_triangle_rgb_channels_present ? "true" : "false");
    std::printf("      \"progressive_triangle_ok\": %s,\n",
                d3d.tessellation_fallback.progressive_triangle_ok ? "true" : "false");
    std::printf("      \"progressive_triangle_motion_color_hash\": \"%016llx\",\n",
                static_cast<unsigned long long>(d3d.tessellation_fallback.motion_color_hash));
    std::printf("      \"last_control_points_ndc\": [");
    for (size_t i = 0; i < 9; ++i)
        std::printf("%s%.6f", i ? ", " : "", d3d.tessellation_fallback.last_control_points[i]);
    std::printf("],\n");
    std::printf("      \"present_ok\": %s,\n", d3d.tessellation_fallback.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.tessellation_fallback.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"indirect_draw\": {\n");
    std::printf("      \"proof_scope\": \"command_signature_execute_indirect_draw_presented_readback\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.indirect_draw.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"indirect_vs_vs_5_0\": \"%s\",\n", hr_hex(d3d.indirect_draw.compile_vs_hr).c_str());
    std::printf("      \"indirect_ps_ps_5_0\": \"%s\",\n", hr_hex(d3d.indirect_draw.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n",
                hr_hex(d3d.indirect_draw.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.indirect_draw.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.indirect_draw.create_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.indirect_draw.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateArgumentBuffer\": \"%s\",\n",
                hr_hex(d3d.indirect_draw.create_argument_buffer_hr).c_str());
    std::printf("      \"CreateCommandSignature\": \"%s\",\n",
                hr_hex(d3d.indirect_draw.create_command_signature_hr).c_str());
    std::printf("      \"vertices_created\": %u,\n", d3d.indirect_draw.vertices_created);
    std::printf("      \"argument_byte_stride\": %u,\n", d3d.indirect_draw.argument_byte_stride);
    std::printf("      \"command_signature_arguments\": %u,\n", d3d.indirect_draw.command_signature_arguments);
    std::printf("      \"max_command_count\": %u,\n", d3d.indirect_draw.max_command_count);
    std::printf("      \"argument_vertex_count\": %u,\n", d3d.indirect_draw.argument_vertex_count);
    std::printf("      \"argument_instance_count\": %u,\n", d3d.indirect_draw.argument_instance_count);
    std::printf("      \"argument_start_vertex\": %u,\n", d3d.indirect_draw.argument_start_vertex);
    std::printf("      \"argument_start_instance\": %u,\n", d3d.indirect_draw.argument_start_instance);
    std::printf("      \"execute_indirect_calls\": %u,\n", d3d.indirect_draw.execute_indirect_calls);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.indirect_draw.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.indirect_draw.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.indirect_draw.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.indirect_draw.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.indirect_draw.expected_rgba[0],
                d3d.indirect_draw.expected_rgba[1], d3d.indirect_draw.expected_rgba[2],
                d3d.indirect_draw.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.indirect_draw.present_rgba[0],
                d3d.indirect_draw.present_rgba[1], d3d.indirect_draw.present_rgba[2],
                d3d.indirect_draw.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.indirect_draw.present_last_rgba[0],
                d3d.indirect_draw.present_last_rgba[1], d3d.indirect_draw.present_last_rgba[2],
                d3d.indirect_draw.present_last_rgba[3]);
    std::printf("      \"present_ok\": %s,\n", d3d.indirect_draw.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.indirect_draw.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"nanite_cluster\": {\n");
    std::printf("      \"proof_scope\": "
                "\"nanite_style_compute_generated_args_readback_mirrored_execute_indirect_presented\",\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.nanite_cluster.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"nanite_cull_cs_cs_5_0\": \"%s\",\n", hr_hex(d3d.nanite_cluster.compile_cs_hr).c_str());
    std::printf("      \"nanite_vs_vs_5_0\": \"%s\",\n", hr_hex(d3d.nanite_cluster.compile_vs_hr).c_str());
    std::printf("      \"nanite_ps_ps_5_0\": \"%s\",\n", hr_hex(d3d.nanite_cluster.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_compute_uav\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.serialize_compute_root_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature_graphics\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.serialize_graphics_root_hr).c_str());
    std::printf("      \"CreateComputeRootSignature\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_compute_root_hr).c_str());
    std::printf("      \"CreateGraphicsRootSignature\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_graphics_root_hr).c_str());
    std::printf("      \"CreateComputePipelineState\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_compute_pso_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_graphics_pso_hr).c_str());
    std::printf("      \"CreateVertexBuffer\": \"%s\",\n", hr_hex(d3d.nanite_cluster.create_vertex_buffer_hr).c_str());
    std::printf("      \"CreateArgumentBuffer_UAV\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_argument_buffer_hr).c_str());
    std::printf("      \"CreateArgumentReadback\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_argument_readback_hr).c_str());
    std::printf("      \"CreateIndirectArgumentUpload\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_indirect_argument_upload_hr).c_str());
    std::printf("      \"CreateCommandSignature\": \"%s\",\n",
                hr_hex(d3d.nanite_cluster.create_command_signature_hr).c_str());
    std::printf("      \"CloseCommandList\": \"%s\",\n", hr_hex(d3d.nanite_cluster.close_hr).c_str());
    std::printf("      \"SignalFence\": \"%s\",\n", hr_hex(d3d.nanite_cluster.signal_hr).c_str());
    std::printf("      \"MapReadback\": \"%s\",\n", hr_hex(d3d.nanite_cluster.map_readback_hr).c_str());
    std::printf("      \"argument_gpu_virtual_address\": %llu,\n",
                static_cast<unsigned long long>(d3d.nanite_cluster.argument_gpu_virtual_address));
    std::printf("      \"indirect_argument_gpu_virtual_address\": %llu,\n",
                static_cast<unsigned long long>(d3d.nanite_cluster.indirect_argument_gpu_virtual_address));
    std::printf("      \"vertices_created\": %u,\n", d3d.nanite_cluster.vertices_created);
    std::printf("      \"argument_byte_stride\": %u,\n", d3d.nanite_cluster.argument_byte_stride);
    std::printf("      \"command_signature_arguments\": %u,\n", d3d.nanite_cluster.command_signature_arguments);
    std::printf("      \"max_command_count\": %u,\n", d3d.nanite_cluster.max_command_count);
    std::printf("      \"dispatch_commands\": %u,\n", d3d.nanite_cluster.dispatch_commands);
    std::printf("      \"uav_barriers\": %u,\n", d3d.nanite_cluster.uav_barriers);
    std::printf("      \"copy_argument_readback_commands\": %u,\n", d3d.nanite_cluster.copy_argument_readback_commands);
    std::printf("      \"transition_to_copy_barriers\": %u,\n", d3d.nanite_cluster.transition_to_copy_barriers);
    std::printf("      \"transition_to_indirect_barriers\": %u,\n", d3d.nanite_cluster.transition_to_indirect_barriers);
    std::printf("      \"computed_draw_args\": [%u, %u, %u, %u],\n", d3d.nanite_cluster.computed_vertex_count,
                d3d.nanite_cluster.computed_instance_count, d3d.nanite_cluster.computed_start_vertex,
                d3d.nanite_cluster.computed_start_instance);
    std::printf("      \"argument_readback_ok\": %s,\n", d3d.nanite_cluster.argument_readback_ok ? "true" : "false");
    std::printf("      \"indirect_argument_upload_filled\": %s,\n",
                d3d.nanite_cluster.indirect_argument_upload_filled ? "true" : "false");
    std::printf("      \"execute_indirect_calls\": %u,\n", d3d.nanite_cluster.execute_indirect_calls);
    std::printf("      \"present_samples_checked\": %u,\n", d3d.nanite_cluster.present_samples_checked);
    std::printf("      \"present_sample_matches\": %u,\n", d3d.nanite_cluster.present_sample_matches);
    std::printf("      \"present_pixels_checked\": %u,\n", d3d.nanite_cluster.present_pixels_checked);
    std::printf("      \"present_pixel_matches\": %u,\n", d3d.nanite_cluster.present_pixel_matches);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.nanite_cluster.expected_rgba[0],
                d3d.nanite_cluster.expected_rgba[1], d3d.nanite_cluster.expected_rgba[2],
                d3d.nanite_cluster.expected_rgba[3]);
    std::printf("      \"present_rgba\": [%u, %u, %u, %u],\n", d3d.nanite_cluster.present_rgba[0],
                d3d.nanite_cluster.present_rgba[1], d3d.nanite_cluster.present_rgba[2],
                d3d.nanite_cluster.present_rgba[3]);
    std::printf("      \"present_last_rgba\": [%u, %u, %u, %u],\n", d3d.nanite_cluster.present_last_rgba[0],
                d3d.nanite_cluster.present_last_rgba[1], d3d.nanite_cluster.present_last_rgba[2],
                d3d.nanite_cluster.present_last_rgba[3]);
    std::printf("      \"fence_wait_ok\": %s,\n", d3d.nanite_cluster.fence_wait_ok ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.nanite_cluster.present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.nanite_cluster.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"visible_scene\": {\n");
    std::printf("      \"D3DCompile_loaded\": %s,\n", d3d.visible_scene.d3dcompiler_loaded ? "true" : "false");
    std::printf("      \"VSMain_vs_5_0\": \"%s\",\n", hr_hex(d3d.visible_scene.compile_vs_hr).c_str());
    std::printf("      \"PSMain_ps_5_0\": \"%s\",\n", hr_hex(d3d.visible_scene.compile_ps_hr).c_str());
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n",
                hr_hex(d3d.visible_scene.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.visible_scene.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(d3d.visible_scene.create_pso_hr).c_str());
    std::printf("      \"CreateVisibleVertexBuffer\": \"%s\",\n",
                hr_hex(d3d.visible_scene.create_vertex_buffer_hr).c_str());
    std::printf("      \"target_frames\": %u,\n", d3d.visible_scene.target_frames);
    std::printf("      \"draw_calls\": %u,\n", d3d.visible_scene.draw_calls);
    std::printf("      \"quads_per_frame\": %u,\n", d3d.visible_scene.quads_per_frame);
    std::printf("      \"vertices_per_frame\": %u,\n", d3d.visible_scene.vertices_per_frame);
    std::printf("      \"sm5_stamp_source\": \"DXBC_SM5_DYNAMIC_STAMP_SENTINEL_OVERWRITE\",\n");
    std::printf("      \"sm5_stamp_quads_per_frame\": %u,\n", d3d.visible_scene.sm5_stamp_quads_per_frame);
    std::printf("      \"sm5_stamp_samples_checked\": %u,\n", d3d.visible_scene.sm5_stamp_samples_checked);
    std::printf("      \"sm5_stamp_matches\": %u,\n", d3d.visible_scene.sm5_stamp_matches);
    std::printf("      \"sm5_stamp_expected_rgba\": [%u, %u, %u, %u],\n", d3d.visible_scene.sm5_stamp_expected_rgba[0],
                d3d.visible_scene.sm5_stamp_expected_rgba[1], d3d.visible_scene.sm5_stamp_expected_rgba[2],
                d3d.visible_scene.sm5_stamp_expected_rgba[3]);
    std::printf("      \"sm5_stamp_rgba\": [%u, %u, %u, %u],\n", d3d.visible_scene.sm5_stamp_rgba[0],
                d3d.visible_scene.sm5_stamp_rgba[1], d3d.visible_scene.sm5_stamp_rgba[2],
                d3d.visible_scene.sm5_stamp_rgba[3]);
    std::printf("      \"progressive_triangle_proof_scope\": "
                "\"progressive_rgb_vertex_triangle_seed_point_to_full_triangle\",\n");
    std::printf("      \"progressive_triangle_region\": [%u, %u, %u, %u],\n", kProgressiveVertexTriangleRegionX,
                kProgressiveVertexTriangleRegionY, kProgressiveVertexTriangleRegionWidth,
                kProgressiveVertexTriangleRegionHeight);
    std::printf("      \"progressive_triangle_vertices_per_frame\": %u,\n",
                d3d.visible_scene.progressive_triangle_vertices_per_frame);
    std::printf("      \"progressive_triangle_draw_calls\": %u,\n", d3d.visible_scene.progressive_triangle_draw_calls);
    std::printf("      \"progressive_triangle_frames_validated\": %u,\n",
                d3d.visible_scene.progressive_triangle_frames_validated);
    std::printf("      \"progressive_triangle_samples_checked\": %u,\n",
                d3d.visible_scene.progressive_triangle_samples_checked);
    std::printf("      \"progressive_triangle_seed_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_seed_pixels);
    std::printf("      \"progressive_triangle_final_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_final_pixels);
    std::printf("      \"progressive_triangle_peak_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_peak_pixels);
    std::printf("      \"progressive_triangle_final_red_dominant_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_final_red_dominant_pixels);
    std::printf("      \"progressive_triangle_final_green_dominant_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_final_green_dominant_pixels);
    std::printf("      \"progressive_triangle_final_blue_dominant_pixels\": %u,\n",
                d3d.visible_scene.progressive_triangle_final_blue_dominant_pixels);
    std::printf("      \"progressive_triangle_coverage_monotonic\": %s,\n",
                d3d.visible_scene.progressive_triangle_coverage_monotonic ? "true" : "false");
    std::printf("      \"progressive_triangle_seed_point_ok\": %s,\n",
                d3d.visible_scene.progressive_triangle_seed_point_pass ? "true" : "false");
    std::printf("      \"progressive_triangle_full_ok\": %s,\n",
                d3d.visible_scene.progressive_triangle_full_pass ? "true" : "false");
    std::printf("      \"progressive_triangle_rgb_channels_present\": %s,\n",
                d3d.visible_scene.progressive_triangle_rgb_channels_present ? "true" : "false");
    std::printf("      \"progressive_triangle_coverage_counts\": [");
    for (size_t i = 0; i < d3d.visible_scene.progressive_triangle_coverage_counts.size(); ++i) {
        std::printf("%s%u", i == 0 ? "" : ", ", d3d.visible_scene.progressive_triangle_coverage_counts[i]);
    }
    std::printf("],\n");
    std::printf("      \"progressive_triangle_ok\": %s,\n",
                d3d.visible_scene.progressive_triangle_present_pass ? "true" : "false");
    std::printf("      \"present_ok\": %s,\n", d3d.visible_scene.sm5_stamp_present_pass ? "true" : "false");
    std::printf("      \"ok\": %s\n", d3d.visible_scene.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"dxil_scene\": {\n");
    std::printf("      \"vs_path\": \"%s\",\n", json_escape(d3d.dxil_scene.vs_path).c_str());
    std::printf("      \"ps_path\": \"%s\",\n", json_escape(d3d.dxil_scene.ps_path).c_str());
    std::printf("      \"vs_loaded\": %s,\n", d3d.dxil_scene.vs_loaded ? "true" : "false");
    std::printf("      \"ps_loaded\": %s,\n", d3d.dxil_scene.ps_loaded ? "true" : "false");
    std::printf("      \"vs_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.dxil_scene.vs_bytes));
    std::printf("      \"ps_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.dxil_scene.ps_bytes));
    std::printf("      \"vs_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(d3d.dxil_scene.vs_fnv1a64));
    std::printf("      \"ps_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(d3d.dxil_scene.ps_fnv1a64));
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n", hr_hex(d3d.dxil_scene.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.dxil_scene.create_root_hr).c_str());
    std::printf("      \"CreateGraphicsPipelineState_SM6_DXIL\": \"%s\",\n",
                hr_hex(d3d.dxil_scene.create_pso_hr).c_str());
    std::printf("      \"CreateDxilVertexBuffer\": \"%s\",\n", hr_hex(d3d.dxil_scene.create_vertex_buffer_hr).c_str());
    std::printf(
        "      \"vertex_source\": \"POSITION_NONDEGENERATE_COLOR_VERTEX_BUFFER_PS_SCALAR_VECTOR_SEMANTICS\",\n");
    std::printf("      \"draw_calls\": %u,\n", d3d.dxil_scene.draw_calls);
    std::printf("      \"vertices_per_draw\": %u,\n", d3d.dxil_scene.vertices_per_draw);
    std::printf("      \"ok\": %s\n", d3d.dxil_scene.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"dxil_hazard_replay\": {\n");
    std::printf("      \"proof_scope\": \"%s\",\n", d3d.dxil_hazard_replay.proof_scope.c_str());
    std::printf("      \"vs_path\": \"%s\",\n", json_escape(d3d.dxil_hazard_replay.vs_path).c_str());
    std::printf("      \"vs_loaded\": %s,\n", d3d.dxil_hazard_replay.vs_loaded ? "true" : "false");
    std::printf("      \"vs_bytes\": %llu,\n", static_cast<unsigned long long>(d3d.dxil_hazard_replay.vs_bytes));
    std::printf("      \"vs_fnv1a64\": \"%016llx\",\n",
                static_cast<unsigned long long>(d3d.dxil_hazard_replay.vs_fnv1a64));
    std::printf("      \"D3D12SerializeRootSignature\": \"%s\",\n",
                hr_hex(d3d.dxil_hazard_replay.serialize_root_hr).c_str());
    std::printf("      \"CreateRootSignature\": \"%s\",\n", hr_hex(d3d.dxil_hazard_replay.create_root_hr).c_str());
    std::printf("      \"requested_count\": %u,\n", d3d.dxil_hazard_replay.requested_count);
    std::printf("      \"replay_count\": %u,\n", d3d.dxil_hazard_replay.replay_count);
    std::printf("      \"success_count\": %u,\n", d3d.dxil_hazard_replay.success_count);
    std::printf("      \"entries\": [\n");
    for (size_t i = 0; i < d3d.dxil_hazard_replay.entries.size(); ++i) {
        const DxilHazardReplayEntryStats& entry = d3d.dxil_hazard_replay.entries[i];
        std::printf("        {\n");
        std::printf("          \"index\": %zu,\n", i);
        std::printf("          \"ps_path\": \"%s\",\n", json_escape(entry.ps_path).c_str());
        std::printf("          \"ps_loaded\": %s,\n", entry.ps_loaded ? "true" : "false");
        std::printf("          \"ps_bytes\": %llu,\n", static_cast<unsigned long long>(entry.ps_bytes));
        std::printf("          \"ps_fnv1a64\": \"%016llx\",\n", static_cast<unsigned long long>(entry.ps_fnv1a64));
        std::printf("          \"CreateGraphicsPipelineState\": \"%s\",\n", hr_hex(entry.create_pso_hr).c_str());
        std::printf("          \"ok\": %s\n", entry.ok ? "true" : "false");
        std::printf("        }%s\n", (i + 1 == d3d.dxil_hazard_replay.entries.size()) ? "" : ",");
    }
    std::printf("      ],\n");
    std::printf("      \"ok\": %s\n", d3d.dxil_hazard_replay.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"dxil_readback\": {\n");
    std::printf("      \"CreateReadbackBuffer\": \"%s\",\n", hr_hex(d3d.dxil_readback.create_readback_hr).c_str());
    std::printf("      \"copy_commands\": %u,\n", d3d.dxil_readback.copy_commands);
    std::printf("      \"sentinel_writes\": %u,\n", d3d.dxil_readback.sentinel_writes);
    std::printf("      \"samples_checked\": %u,\n", d3d.dxil_readback.samples_checked);
    std::printf("      \"semantic_samples\": %u,\n", d3d.dxil_readback.semantic_samples);
    std::printf("      \"expected_rgba\": [%u, %u, %u, %u],\n", d3d.dxil_readback.expected_rgba[0],
                d3d.dxil_readback.expected_rgba[1], d3d.dxil_readback.expected_rgba[2],
                d3d.dxil_readback.expected_rgba[3]);
    std::printf("      \"center_rgba\": [%u, %u, %u, %u],\n", d3d.dxil_readback.center_rgba[0],
                d3d.dxil_readback.center_rgba[1], d3d.dxil_readback.center_rgba[2], d3d.dxil_readback.center_rgba[3]);
    std::printf("      \"ok\": %s\n", d3d.dxil_readback.pass ? "true" : "false");
    std::printf("    },\n");
    std::printf("    \"Present\": \"%s\",\n", hr_hex(d3d.present_hr).c_str());
    std::printf("    \"frames_presented\": %u,\n", d3d.frames_presented);
    std::printf("    \"ok\": %s\n", d3d.pass ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"process_exit_mode\": \"normal_return_after_explicit_d3d_release\"\n");
    std::printf("}\n");
    std::fflush(stdout);
    return pass ? 0 : 1;
}
