#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
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
    for (char c : input) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
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

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes,
                                       D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
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

enum class TextureShape {
    Texture1D,
    Texture1DArray,
    Texture2D,
    Texture2DArray,
    Texture3D,
    TextureCube,
    TextureCubeArray,
    Texture2DMS,
    Texture2DMSArray,
};

enum class TypedData {
    None,
    R32UInt,
    R32SInt,
    R16UInt,
    R16SInt,
    RG16UInt,
    RGBA8UInt,
    RGBA8SInt,
    R16Float,
    R16UNorm,
    R16SNorm,
    RGBA8SNorm,
    R10G10B10A2UInt,
    R10G10B10A2UNorm,
    R11G11B10Float,
    R64UInt,
    R64SInt,
};

enum class SamplerAddress {
    Clamp,
    Wrap,
    Mirror,
    Border,
    BorderOpaqueBlack,
    BorderTransparentBlack,
    BorderUnsupported,
    MirrorOnce,
};

enum class SamplerFilter {
    MinMagMipPoint,
    MinMagPointMipLinear,
    MinPointMagLinearMipPoint,
    MinPointMagMipLinear,
    MinLinearMagMipPoint,
    MinLinearMagPointMipLinear,
    MinMagLinearMipPoint,
    MinMagMipLinear,
    Anisotropic,
    Minimum,
    Maximum,
};

struct ShapeInfo {
    const char* name;
    const char* shader;
    TextureShape shape;
    bool multisample;
    bool array;
    uint32_t expected;
    uint32_t dimensions_expected;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TypedData typed_data = TypedData::None;
    uint32_t expected_high = 0;
    uint16_t mip_levels = 1;
    bool sample_pattern = false;
    SamplerAddress sampler_address = SamplerAddress::Clamp;
    bool expect_rejection = false;
    SamplerFilter sampler_filter = SamplerFilter::MinMagMipPoint;
};

static const ShapeInfo kReadCases[] = {
    {"texture1d", "cs_texture_1d.cso", TextureShape::Texture1D, false, false, 64, 4},
    {"texture1d_array", "cs_texture_1d_array.cso", TextureShape::Texture1DArray, false, true, 96, 131076},
    {"texture1d_mip", "cs_texture_1d_mip.cso", TextureShape::Texture1D, false, false,
     96, 131074, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2},
    {"texture1d_array_mip", "cs_texture_1d_array_mip.cso", TextureShape::Texture1DArray, false, true,
     96, 131586, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2},
    {"texture1d_advanced", "cs_texture_1d_advanced.cso", TextureShape::Texture1D, false, false,
     0x14323232, 131074, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true},
    {"texture1d_filter_point", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x1e3c1e, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinMagMipPoint},
    {"texture1d_filter_point_mip_linear", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x263c1e, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinMagPointMipLinear},
    {"texture1d_filter_min_point_mag_linear_mip_point", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x1e3c19, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinPointMagLinearMipPoint},
    {"texture1d_filter_min_point_mag_linear_mip_linear", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x263c19, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinPointMagMipLinear},
    {"texture1d_filter_min_linear_mag_point_mip_point", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x19371e, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinLinearMagMipPoint},
    {"texture1d_filter_min_linear_mag_point_mip_linear", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x21371e, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinLinearMagPointMipLinear},
    {"texture1d_filter_linear_mip_point", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x193719, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinMagLinearMipPoint},
    {"texture1d_filter_linear_mip_linear", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x213719, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::MinMagMipLinear},
    {"texture1d_filter_anisotropic", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0x211919, 131076, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, false, SamplerFilter::Anisotropic},
    {"texture1d_filter_minimum_rejected", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0xdeadbeef, 0xdeadbeef, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, true, SamplerFilter::Minimum},
    {"texture1d_filter_maximum_rejected", "cs_texture_1d_filter.cso", TextureShape::Texture1D, false, false,
     0xdeadbeef, 0xdeadbeef, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 2, true,
     SamplerAddress::Clamp, true, SamplerFilter::Maximum},
    {"texture1d_address_clamp", "cs_texture_1d_address.cso", TextureShape::Texture1D, false, false,
     0x0a28280a, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::Clamp},
    {"texture1d_address_wrap", "cs_texture_1d_address.cso", TextureShape::Texture1D, false, false,
     0x280a0a28, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::Wrap},
    {"texture1d_address_mirror", "cs_texture_1d_address.cso", TextureShape::Texture1D, false, false,
     0x0a0a280a, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::Mirror},
    {"texture1d_address_border", "cs_texture_1d_address.cso", TextureShape::Texture1D, false, false,
     0xffffffff, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::Border},
    {"texture1d_border_opaque_white", "cs_texture_1d_border.cso", TextureShape::Texture1D, false, false,
     0xffffffff, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::Border},
    {"texture1d_border_opaque_black", "cs_texture_1d_border.cso", TextureShape::Texture1D, false, false,
     0xff000000, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::BorderOpaqueBlack},
    {"texture1d_border_transparent_black", "cs_texture_1d_border.cso", TextureShape::Texture1D, false, false,
     0x00000000, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::BorderTransparentBlack},
    {"texture1d_border_unsupported", "cs_texture_1d_border.cso", TextureShape::Texture1D, false, false,
     0xdeadbeef, 0xdeadbeef, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true,
     SamplerAddress::BorderUnsupported, true},
    {"texture1d_address_mirror_once", "cs_texture_1d_address.cso", TextureShape::Texture1D, false, false,
     0x2828280a, 4, DXGI_FORMAT_R8G8B8A8_UNORM, TypedData::None, 0, 1, true, SamplerAddress::MirrorOnce},
    {"texture2d", "cs_texture_2d.cso", TextureShape::Texture2D, false, false, 64, 1028},
    {"texture2d_array", "cs_texture_2d_array.cso", TextureShape::Texture2DArray, false, true, 96, 132100},
    {"texture3d", "cs_texture_3d.cso", TextureShape::Texture3D, false, false, 96, 263172},
    {"texturecube", "cs_texture_cube.cso", TextureShape::TextureCube, false, true, 96, 1028},
    {"texturecube_array", "cs_texture_cube_array.cso", TextureShape::TextureCubeArray, false, true, 96, 132100},
    {"texture2d_ms", "cs_texture_2d_ms.cso", TextureShape::Texture2DMS, true, false, 64, 33555460},
    {"texture2d_ms_array", "cs_texture_2d_ms_array.cso", TextureShape::Texture2DMSArray, true, true, 96, 33686532},
    {"texture_typed_uint", "cs_texture_typed_uint.cso", TextureShape::Texture2D, false, false,
     0x281e140a, 1028, DXGI_FORMAT_R32_UINT, TypedData::R32UInt},
    {"texture_typed_sint", "cs_texture_typed_sint.cso", TextureShape::Texture2D, false, false,
     0x281e140a, 1028, DXGI_FORMAT_R32_SINT, TypedData::R32SInt},
    {"texture_typed_r16_uint", "cs_texture_typed_uint.cso", TextureShape::Texture2D, false, false,
     0x1234, 1028, DXGI_FORMAT_R16_UINT, TypedData::R16UInt},
    {"texture_typed_r16_sint", "cs_texture_typed_sint.cso", TextureShape::Texture2D, false, false,
     0xfffffffe, 1028, DXGI_FORMAT_R16_SINT, TypedData::R16SInt},
    {"texture_typed_rg16_uint", "cs_texture_typed_uint2.cso", TextureShape::Texture2D, false, false,
     0x56781234, 1028, DXGI_FORMAT_R16G16_UINT, TypedData::RG16UInt},
    {"texture_typed_rgba8_uint", "cs_texture_typed_uint4.cso", TextureShape::Texture2D, false, false,
     0x281e140a, 1028, DXGI_FORMAT_R8G8B8A8_UINT, TypedData::RGBA8UInt},
    {"texture_typed_rgba8_sint", "cs_texture_typed_sint4.cso", TextureShape::Texture2D, false, false,
     0xfcfdfeff, 1028, DXGI_FORMAT_R8G8B8A8_SINT, TypedData::RGBA8SInt},
    {"texture_typed_r16_float", "cs_texture_typed_float16.cso", TextureShape::Texture2D, false, false,
     0x3400, 1028, DXGI_FORMAT_R16_FLOAT, TypedData::R16Float},
    {"texture_normalized_r16_unorm", "cs_texture_2d.cso", TextureShape::Texture2D, false, false,
     64, 1028, DXGI_FORMAT_R16_UNORM, TypedData::R16UNorm},
    {"texture_normalized_r16_snorm", "cs_texture_2d.cso", TextureShape::Texture2D, false, false,
     64, 1028, DXGI_FORMAT_R16_SNORM, TypedData::R16SNorm},
    {"texture_normalized_rgba8_snorm", "cs_texture_2d.cso", TextureShape::Texture2D, false, false,
     64, 1028, DXGI_FORMAT_R8G8B8A8_SNORM, TypedData::RGBA8SNorm},
    {"texture_packed_r10g10b10a2_uint", "cs_texture_typed_uint4.cso", TextureShape::Texture2D, false, false,
     0x031e140a, 1028, DXGI_FORMAT_R10G10B10A2_UINT, TypedData::R10G10B10A2UInt},
    {"texture_packed_r10g10b10a2_unorm", "cs_texture_2d.cso", TextureShape::Texture2D, false, false,
     64, 1028, DXGI_FORMAT_R10G10B10A2_UNORM, TypedData::R10G10B10A2UNorm},
    {"texture_packed_r11g11b10_float", "cs_texture_2d.cso", TextureShape::Texture2D, false, false,
     64, 1028, DXGI_FORMAT_R11G11B10_FLOAT, TypedData::R11G11B10Float},
    {"texture_typed_r64_uint", "cs_texture_typed_uint64.cso", TextureShape::Texture2D, false, false,
     0x89abcdef, 1028, DXGI_FORMAT_R32G32_UINT, TypedData::R64UInt, 0x01234567},
    {"texture_typed_r64_sint", "cs_texture_typed_sint64.cso", TextureShape::Texture2D, false, false,
     0x12345678, 1028, DXGI_FORMAT_R32G32_UINT, TypedData::R64SInt, 0xffffffff},
};

static const ShapeInfo kStoreCases[] = {
    {"texture1d", "cs_store_1d.cso", TextureShape::Texture1D, false, false, 64, 0},
    {"texture1d_array", "cs_store_1d_array.cso", TextureShape::Texture1DArray, false, true, 64, 0},
    {"texture2d", "cs_store_2d.cso", TextureShape::Texture2D, false, false, 64, 0},
    {"texture2d_array", "cs_store_2d_array.cso", TextureShape::Texture2DArray, false, true, 64, 0},
    {"texture3d", "cs_store_3d.cso", TextureShape::Texture3D, false, false, 64, 0},
    {"texture_typed_uint", "cs_store_typed_uint.cso", TextureShape::Texture2D, false, false,
     0x12345678, 0, DXGI_FORMAT_R32_UINT, TypedData::R32UInt},
    {"texture_typed_sint", "cs_store_typed_sint.cso", TextureShape::Texture2D, false, false,
     0xffed2979, 0, DXGI_FORMAT_R32_SINT, TypedData::R32SInt},
    {"texture_typed_rgba8_uint", "cs_store_typed_uint4.cso", TextureShape::Texture2D, false, false,
     0x281e140a, 0, DXGI_FORMAT_R8G8B8A8_UINT, TypedData::RGBA8UInt},
    {"texture_typed_rgba8_sint", "cs_store_typed_sint4.cso", TextureShape::Texture2D, false, false,
     0xfcfdfeff, 0, DXGI_FORMAT_R8G8B8A8_SINT, TypedData::RGBA8SInt},
    {"texture_normalized_r16_unorm", "cs_store_2d.cso", TextureShape::Texture2D, false, false,
     0x00004000, 0, DXGI_FORMAT_R16_UNORM, TypedData::R16UNorm},
    {"texture_normalized_r16_snorm", "cs_store_2d.cso", TextureShape::Texture2D, false, false,
     0x00002000, 0, DXGI_FORMAT_R16_SNORM, TypedData::R16SNorm},
    {"texture_normalized_rgba8_snorm", "cs_store_2d.cso", TextureShape::Texture2D, false, false,
     0x7f000020, 0, DXGI_FORMAT_R8G8B8A8_SNORM, TypedData::RGBA8SNorm},
    {"texture_packed_r10g10b10a2_uint", "cs_store_typed_uint4.cso", TextureShape::Texture2D, false, false,
     0xc1e0500a, 0, DXGI_FORMAT_R10G10B10A2_UINT, TypedData::R10G10B10A2UInt},
    {"texture_packed_r10g10b10a2_unorm", "cs_store_2d.cso", TextureShape::Texture2D, false, false,
     0xc0000100, 0, DXGI_FORMAT_R10G10B10A2_UNORM, TypedData::R10G10B10A2UNorm},
    {"texture_packed_r11g11b10_float", "cs_store_2d.cso", TextureShape::Texture2D, false, false,
     0x00000340, 0, DXGI_FORMAT_R11G11B10_FLOAT, TypedData::R11G11B10Float},
    {"texture_typed_r64_uint", "cs_store_typed_uint64.cso", TextureShape::Texture2D, false, false,
     0x89abcdef, 0, DXGI_FORMAT_R32G32_UINT, TypedData::R64UInt, 0x01234567},
    {"texture_typed_r64_sint", "cs_store_typed_sint64.cso", TextureShape::Texture2D, false, false,
     0x12345678, 0, DXGI_FORMAT_R32G32_UINT, TypedData::R64SInt, 0xffffffff},
};

static D3D12_RESOURCE_DESC texture_desc(TextureShape shape, bool writable) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = shape == TextureShape::Texture1D ||
                     shape == TextureShape::Texture1DArray
                         ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                         : shape == TextureShape::Texture3D
                               ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : 4;
    desc.DepthOrArraySize = 1;
    if (shape == TextureShape::Texture1DArray ||
        shape == TextureShape::Texture2DArray ||
        shape == TextureShape::Texture2DMSArray)
        desc.DepthOrArraySize = 2;
    else if (shape == TextureShape::Texture3D)
        desc.DepthOrArraySize = 4;
    else if (shape == TextureShape::TextureCube)
        desc.DepthOrArraySize = 6;
    else if (shape == TextureShape::TextureCubeArray)
        desc.DepthOrArraySize = 12;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = (shape == TextureShape::Texture2DMS ||
                             shape == TextureShape::Texture2DMSArray)
                                ? 2
                                : 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = writable ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                          : D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES before,
                                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

static D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu(D3D12_CPU_DESCRIPTOR_HANDLE start,
                                               UINT increment, UINT index) {
    start.ptr += static_cast<SIZE_T>(increment) * index;
    return start;
}

static D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu(D3D12_GPU_DESCRIPTOR_HANDLE start,
                                               UINT increment, UINT index) {
    start.ptr += static_cast<UINT64>(increment) * index;
    return start;
}

static std::string g_binary_file_error;

static bool read_binary_file(const char* path, std::vector<uint8_t>& out) {
    std::string resolved = path ? path : "";
    if (!resolved.empty() && resolved.find(':') == std::string::npos &&
        resolved.front() != '/' && resolved.front() != '\\') {
        char module_path[MAX_PATH] = {};
        DWORD length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (length && length < MAX_PATH) {
            char* slash = std::strrchr(module_path, '\\');
            if (slash) {
                slash[1] = '\0';
                resolved = std::string(module_path) + resolved;
            }
        }
    }
    FILE* file = std::fopen(resolved.c_str(), "rb");
    if (!file) {
        g_binary_file_error = resolved + " (win32=" +
                              std::to_string(GetLastError()) + ")";
        return false;
    }
    g_binary_file_error.clear();
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        g_binary_file_error = resolved + " (empty)";
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    size_t count = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return count == out.size();
}

static HRESULT create_device(ID3D12Device** device) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    if (!create)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                  reinterpret_cast<void**>(device));
}

static HRESULT create_root_signature(ID3D12Device* device, bool read,
                                     ID3D12RootSignature** root,
                                     std::string& errors) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(
        d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = read ? 1 : 0;
    params[1].DescriptorTable.pDescriptorRanges = read ? &ranges[1] : nullptr;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = read ? 1 : 0;
    params[2].DescriptorTable.pDescriptorRanges = read ? &ranges[2] : nullptr;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = read ? 3 : 1;
    desc.pParameters = params;

    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob,
                           &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()),
                      error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

static HRESULT create_graphics_texture_root_signature(
    ID3D12Device* device, ID3D12RootSignature** root, std::string& errors) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(
        d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[2] = {};
    for (UINT i = 0; i < 2; ++i) {
        params[i].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob,
                           &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()),
                      error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

static HRESULT create_queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                            ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
}

static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
                                ID3D12GraphicsCommandList* list) {
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
    if (SUCCEEDED(hr))
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && event_handle)
        hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr) && event_handle)
        hr = WaitForSingleObject(event_handle, 15000) == WAIT_OBJECT_0
                 ? S_OK
                 : E_FAIL;
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return hr;
}

static void make_srv_desc(TextureShape shape, D3D12_SHADER_RESOURCE_VIEW_DESC& srv,
                          DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM,
                          UINT mip_levels = 1) {
    srv = {};
    srv.Format = format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (shape) {
    case TextureShape::Texture1D:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        srv.Texture1D.MipLevels = mip_levels;
        break;
    case TextureShape::Texture1DArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        srv.Texture1DArray.MipLevels = mip_levels;
        srv.Texture1DArray.ArraySize = 2;
        break;
    case TextureShape::Texture2D:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = mip_levels;
        break;
    case TextureShape::Texture2DArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = mip_levels;
        srv.Texture2DArray.ArraySize = 2;
        break;
    case TextureShape::Texture3D:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = mip_levels;
        break;
    case TextureShape::TextureCube:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srv.TextureCube.MipLevels = mip_levels;
        break;
    case TextureShape::TextureCubeArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        srv.TextureCubeArray.MipLevels = mip_levels;
        srv.TextureCubeArray.NumCubes = 2;
        break;
    case TextureShape::Texture2DMS:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
    case TextureShape::Texture2DMSArray:
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        srv.Texture2DMSArray.ArraySize = 2;
        break;
    }
}

static void make_uav_desc(TextureShape shape, D3D12_UNORDERED_ACCESS_VIEW_DESC& uav,
                          DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    uav = {};
    uav.Format = format;
    switch (shape) {
    case TextureShape::Texture1D:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        break;
    case TextureShape::Texture1DArray:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        uav.Texture1DArray.ArraySize = 2;
        break;
    case TextureShape::Texture2D:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        break;
    case TextureShape::Texture2DArray:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uav.Texture2DArray.ArraySize = 2;
        break;
    case TextureShape::Texture3D:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = 4;
        break;
    default:
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        break;
    }
}

static bool fill_upload(ID3D12Resource* upload,
                        const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& footprints,
                        const std::vector<UINT>& rows, const D3D12_RESOURCE_DESC& desc,
                        uint8_t r) {
    uint8_t* mapped = nullptr;
    D3D12_RANGE read_range = {0, 0};
    if (FAILED(upload->Map(0, &read_range, reinterpret_cast<void**>(&mapped))))
        return false;
    const UINT subresources = static_cast<UINT>(footprints.size());
    const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                           ? desc.DepthOrArraySize
                           : 1;
    for (UINT subresource = 0; subresource < subresources; ++subresource) {
        const auto& footprint = footprints[subresource];
        for (UINT z = 0; z < depth; ++z) {
            const UINT logical_slice = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                           ? z
                                           : subresource;
            const uint8_t slice_r = logical_slice > 0 ? static_cast<uint8_t>(r + 32) : r;
            for (UINT y = 0; y < rows[subresource]; ++y) {
                uint8_t* row = mapped + footprint.Offset +
                               (static_cast<size_t>(z) * rows[subresource] + y) *
                                   footprint.Footprint.RowPitch;
                for (UINT x = 0; x < footprint.Footprint.Width; ++x) {
                    row[x * 4 + 0] = slice_r;
                    row[x * 4 + 1] = 0;
                    row[x * 4 + 2] = 0;
                    row[x * 4 + 3] = 255;
                }
            }
        }
    }
    D3D12_RANGE write_range = {0, static_cast<SIZE_T>(upload->GetDesc().Width)};
    upload->Unmap(0, &write_range);
    return true;
}

struct CaseResult {
    std::string name;
    std::string operation;
    bool pass = false;
    HRESULT hr = E_FAIL;
    HRESULT pso_hr = E_FAIL;
    uint32_t expected = 64;
    uint32_t actual = 0;
    uint32_t expected_high = 0;
    uint32_t actual_high = 0;
    uint32_t dimensions_expected = 0;
    uint32_t dimensions_actual = 0;
    std::string detail;
};

static CaseResult run_read_case(ID3D12Device* device, const ShapeInfo& info) {
    CaseResult result;
    result.name = info.name;
    result.operation = "read";
    result.expected = info.expected;
    result.expected_high = info.expected_high;
    result.dimensions_expected = info.dimensions_expected;
    std::vector<uint8_t> shader;
    if (!read_binary_file(info.shader, shader)) {
        result.detail = "compiled DXIL blob missing: " + g_binary_file_error;
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return result;
    }

    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string errors;
    HRESULT hr = create_root_signature(device, true, &root, errors);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS.pShaderBytecode = shader.data();
        desc.CS.BytecodeLength = shader.size();
        result.pso_hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        hr = result.pso_hr;
    }
    D3D12_RESOURCE_DESC tex_desc = texture_desc(info.shape, false);
    tex_desc.Format = info.format;
    tex_desc.MipLevels = info.mip_levels;
    const UINT array_size = info.shape == TextureShape::Texture3D
                                ? 1
                                : tex_desc.DepthOrArraySize;
    const UINT subresources = array_size * tex_desc.MipLevels;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> row_bytes(subresources);
    UINT64 upload_bytes = 0;
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 2;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&resource_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&sampler_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&tex_desc, 0, subresources, 0,
                                      footprints.data(), rows.data(), row_bytes.data(),
                                      &upload_bytes);
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC desc = buffer_desc(upload_bytes);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload));
    }
    if (SUCCEEDED(hr) && info.typed_data == TypedData::None &&
        !fill_upload(upload, footprints, rows, tex_desc, 64))
        hr = E_FAIL;
    if (SUCCEEDED(hr) && info.sample_pattern) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = upload->Map(0, &read_range,
                         reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            for (UINT subresource = 0; subresource < subresources;
                 ++subresource) {
                const uint8_t base = subresource == 0 ? 10 : 50;
                for (UINT y = 0; y < rows[subresource]; ++y) {
                    uint8_t* row = mapped + footprints[subresource].Offset +
                                   static_cast<size_t>(y) *
                                       footprints[subresource].Footprint.RowPitch;
                    for (UINT x = 0;
                         x < footprints[subresource].Footprint.Width; ++x) {
                        row[x * 4 + 0] = static_cast<uint8_t>(base + x * 10);
                        row[x * 4 + 1] = 0;
                        row[x * 4 + 2] = 0;
                        row[x * 4 + 3] = 255;
                    }
                }
            }
            D3D12_RANGE write_range = {
                0, static_cast<SIZE_T>(upload->GetDesc().Width)};
            upload->Unmap(0, &write_range);
        }
    }
    if (SUCCEEDED(hr) && info.typed_data != TypedData::None) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        hr = upload->Map(0, &read_range,
                         reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            std::memset(mapped, 0, static_cast<size_t>(upload->GetDesc().Width));
            uint8_t* pixel = mapped + footprints[0].Offset;
            switch (info.typed_data) {
            case TypedData::R32UInt:
            case TypedData::R32SInt:
            case TypedData::RGBA8UInt:
                pixel[0] = 10; pixel[1] = 20; pixel[2] = 30; pixel[3] = 40;
                break;
            case TypedData::R16UInt:
                pixel[0] = 0x34; pixel[1] = 0x12;
                break;
            case TypedData::R16SInt:
                pixel[0] = 0xfe; pixel[1] = 0xff;
                break;
            case TypedData::RG16UInt:
                pixel[0] = 0x34; pixel[1] = 0x12;
                pixel[2] = 0x78; pixel[3] = 0x56;
                break;
            case TypedData::RGBA8SInt:
                pixel[0] = 0xff; pixel[1] = 0xfe; pixel[2] = 0xfd; pixel[3] = 0xfc;
                break;
            case TypedData::R16Float:
                pixel[0] = 0x00; pixel[1] = 0x34;
                break;
            case TypedData::R16UNorm:
                pixel[0] = 0x00; pixel[1] = 0x40;
                break;
            case TypedData::R16SNorm:
                pixel[0] = 0x00; pixel[1] = 0x20;
                break;
            case TypedData::RGBA8SNorm:
                pixel[0] = 0x20; pixel[1] = 0x00; pixel[2] = 0x00; pixel[3] = 0x7f;
                break;
            case TypedData::R10G10B10A2UInt:
                pixel[0] = 0x0a; pixel[1] = 0x50; pixel[2] = 0xe0; pixel[3] = 0xc1;
                break;
            case TypedData::R10G10B10A2UNorm:
                pixel[0] = 0x00; pixel[1] = 0x01; pixel[2] = 0x00; pixel[3] = 0xc0;
                break;
            case TypedData::R11G11B10Float:
                pixel[0] = 0x40; pixel[1] = 0x03; pixel[2] = 0x00; pixel[3] = 0x00;
                break;
            case TypedData::R64UInt:
                pixel[0] = 0xef; pixel[1] = 0xcd; pixel[2] = 0xab; pixel[3] = 0x89;
                pixel[4] = 0x67; pixel[5] = 0x45; pixel[6] = 0x23; pixel[7] = 0x01;
                break;
            case TypedData::R64SInt:
                pixel[0] = 0x78; pixel[1] = 0x56; pixel[2] = 0x34; pixel[3] = 0x12;
                pixel[4] = 0xff; pixel[5] = 0xff; pixel[6] = 0xff; pixel[7] = 0xff;
                break;
            case TypedData::None:
                break;
            }
            const UINT bytes_per_pixel =
                info.typed_data == TypedData::R64UInt ||
                        info.typed_data == TypedData::R64SInt
                    ? 8
                    : info.typed_data == TypedData::R16UInt ||
                        info.typed_data == TypedData::R16SInt ||
                        info.typed_data == TypedData::R16Float ||
                        info.typed_data == TypedData::R16UNorm ||
                        info.typed_data == TypedData::R16SNorm
                    ? 2
                    : 4;
            uint8_t pattern[8] = {};
            std::memcpy(pattern, pixel, bytes_per_pixel);
            for (UINT y = 0; y < rows[0]; ++y) {
                uint8_t* row = mapped + footprints[0].Offset +
                               static_cast<size_t>(y) *
                                   footprints[0].Footprint.RowPitch;
                for (UINT x = 0; x < footprints[0].Footprint.Width; ++x)
                    std::memcpy(row + x * bytes_per_pixel, pattern,
                                bytes_per_pixel);
            }
            D3D12_RANGE write_range = {0, static_cast<SIZE_T>(upload->GetDesc().Width)};
            upload->Unmap(0, &write_range);
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&output));
        if (SUCCEEDED(hr) && info.expect_rejection) {
            const uint32_t sentinel[2] = {0xdeadbeef, 0xdeadbeef};
            hr = output->WriteToSubresource(0, nullptr, sentinel,
                                            sizeof(sentinel), sizeof(sentinel));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES heap = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC desc = buffer_desc(256);
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 64;
        uav.Buffer.StructureByteStride = 4;
        device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        make_srv_desc(info.shape, srv, info.format, info.mip_levels);
        device->CreateShaderResourceView(texture, &srv, offset_cpu(cpu, inc, 1));
        D3D12_SAMPLER_DESC sampler = {};
        switch (info.sampler_filter) {
        case SamplerFilter::MinMagPointMipLinear:
            sampler.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            break;
        case SamplerFilter::MinPointMagLinearMipPoint:
            sampler.Filter = D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
            break;
        case SamplerFilter::MinPointMagMipLinear:
            sampler.Filter = D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
            break;
        case SamplerFilter::MinLinearMagMipPoint:
            sampler.Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
            break;
        case SamplerFilter::MinLinearMagPointMipLinear:
            sampler.Filter = D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
            break;
        case SamplerFilter::MinMagLinearMipPoint:
            sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            break;
        case SamplerFilter::MinMagMipLinear:
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            break;
        case SamplerFilter::Anisotropic:
            sampler.Filter = D3D12_FILTER_ANISOTROPIC;
            break;
        case SamplerFilter::Minimum:
            sampler.Filter = D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
            break;
        case SamplerFilter::Maximum:
            sampler.Filter = D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT;
            break;
        case SamplerFilter::MinMagMipPoint:
        default:
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            break;
        }
        switch (info.sampler_address) {
        case SamplerAddress::Wrap:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            break;
        case SamplerAddress::Mirror:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
            break;
        case SamplerAddress::Border:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.BorderColor[0] = 1.0f;
            sampler.BorderColor[1] = 1.0f;
            sampler.BorderColor[2] = 1.0f;
            sampler.BorderColor[3] = 1.0f;
            break;
        case SamplerAddress::BorderOpaqueBlack:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.BorderColor[3] = 1.0f;
            break;
        case SamplerAddress::BorderTransparentBlack:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            break;
        case SamplerAddress::BorderUnsupported:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler.BorderColor[0] = 1.0f;
            sampler.BorderColor[3] = 1.0f;
            break;
        case SamplerAddress::MirrorOnce:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
            break;
        case SamplerAddress::Clamp:
        default:
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            break;
        }
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy =
            info.sampler_filter == SamplerFilter::Anisotropic ? 16 : 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
        for (UINT subresource = 0; subresource < subresources; ++subresource) {
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = upload;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[subresource];
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = texture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = subresource;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER texture_barrier = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &texture_barrier);
        ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
        list->SetDescriptorHeaps(2, heaps);
        list->SetComputeRootSignature(root);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRootDescriptorTable(1, offset_gpu(gpu, inc, 1));
        list->SetComputeRootDescriptorTable(2, sampler_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource = output;
        barriers[1] = transition_barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(2, barriers);
        list->CopyResource(readback, output);
        hr = execute_and_wait(device, queue, list);
    }
    if (SUCCEEDED(hr)) {
        uint32_t* mapped = nullptr;
        const bool typed_64 = info.typed_data == TypedData::R64UInt ||
                              info.typed_data == TypedData::R64SInt;
        D3D12_RANGE range = {0, (typed_64 ? 3u : 2u) * sizeof(uint32_t)};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            result.actual = mapped[0];
            if (typed_64) {
                result.actual_high = mapped[1];
                result.dimensions_actual = mapped[2];
            } else {
                result.dimensions_actual = mapped[1];
            }
            readback->Unmap(0, nullptr);
        }
    }
    result.hr = hr;
    result.pass = SUCCEEDED(hr) && result.actual == result.expected &&
                  result.actual_high == result.expected_high &&
                  result.dimensions_actual == result.dimensions_expected;
    result.detail = result.pass
                        ? info.expect_rejection
                              ? "unsupported sampler remained fail-closed with the exact output sentinel"
                              : "dimension-aware DXIL texture read and GetDimensions matched exact readback"
                        : errors.empty() ? "texture dimension readback failed" : errors;
    safe_release(readback); safe_release(output); safe_release(upload); safe_release(texture);
    safe_release(sampler_heap); safe_release(resource_heap); safe_release(list);
    safe_release(allocator); safe_release(queue); safe_release(pso); safe_release(root);
    return result;
}

static CaseResult run_store_case(ID3D12Device* device, const ShapeInfo& info) {
    CaseResult result;
    result.name = info.name;
    result.operation = "store";
    result.expected = info.expected;
    result.expected_high = info.expected_high;
    std::vector<uint8_t> shader;
    if (!read_binary_file(info.shader, shader)) {
        result.detail = "compiled DXIL blob missing: " + g_binary_file_error;
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return result;
    }
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string errors;
    HRESULT hr = create_root_signature(device, false, &root, errors);
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS.pShaderBytecode = shader.data();
        desc.CS.BytecodeLength = shader.size();
        result.pso_hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        hr = result.pso_hr;
    }
    D3D12_RESOURCE_DESC tex_desc = texture_desc(info.shape, true);
    tex_desc.Format = info.format;
    const UINT subresources = info.shape == TextureShape::Texture3D ? 1 : tex_desc.DepthOrArraySize;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
    std::vector<UINT> rows(subresources);
    std::vector<UINT64> row_bytes(subresources);
    UINT64 readback_bytes = 0;
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
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                             IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&tex_desc, 0, subresources, 0,
                                      footprints.data(), rows.data(), row_bytes.data(),
                                      &readback_bytes);
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC desc = buffer_desc(readback_bytes);
        hr = device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        make_uav_desc(info.shape, uav, info.format);
        device->CreateUnorderedAccessView(texture, nullptr, &uav,
                                          heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap* heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->Dispatch(1, 1, 1);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        for (UINT subresource = 0; subresource < subresources; ++subresource) {
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = texture;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = footprints[subresource];
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        hr = execute_and_wait(device, queue, list);
    }
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            UINT64 target_offset = footprints[0].Offset;
            if (info.shape == TextureShape::Texture3D)
                target_offset += static_cast<UINT64>(rows[0]) *
                                 footprints[0].Footprint.RowPitch;
            else if (info.array)
                target_offset = footprints[1].Offset;
            if (info.typed_data != TypedData::None) {
                std::memcpy(&result.actual, mapped + target_offset,
                            sizeof(result.actual));
                if (info.typed_data == TypedData::R64UInt ||
                    info.typed_data == TypedData::R64SInt)
                    std::memcpy(&result.actual_high,
                                mapped + target_offset + sizeof(uint32_t),
                                sizeof(result.actual_high));
            } else {
                result.actual = mapped[target_offset];
            }
            readback->Unmap(0, nullptr);
        }
    }
    result.hr = hr;
    result.pass = SUCCEEDED(hr) && result.actual == result.expected &&
                  result.actual_high == result.expected_high;
    result.detail = result.pass ? "dimension-aware DXIL texture store matched exact readback"
                                : errors.empty() ? "texture dimension store readback failed"
                                                 : errors;
    safe_release(readback); safe_release(texture); safe_release(heap); safe_release(list);
    safe_release(allocator); safe_release(queue); safe_release(pso); safe_release(root);
    return result;
}

static CaseResult run_graphics_lod_case(ID3D12Device* device) {
    CaseResult result;
    result.name = "texture1d_graphics_lod";
    result.operation = "graphics";
    float expected_lod = 1.0f;
    std::memcpy(&result.expected, &expected_lod, sizeof(expected_lod));
    result.expected_high = result.expected;
    float expected_comparison_sum = 4.0f;
    std::memcpy(&result.dimensions_expected, &expected_comparison_sum,
                sizeof(expected_comparison_sum));

    std::vector<uint8_t> vertex_shader;
    std::vector<uint8_t> pixel_shader;
    if (!read_binary_file("vs_texture_lod.cso", vertex_shader) ||
        !read_binary_file("ps_texture_lod.cso", pixel_shader)) {
        result.hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.detail = "compiled graphics LOD DXIL blob missing: " +
                        g_binary_file_error;
        return result;
    }

    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* comparison_texture = nullptr;
    ID3D12Resource* comparison_texture1d = nullptr;
    ID3D12Resource* comparison_texture1d_array = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    std::string errors;
    HRESULT hr =
        create_graphics_texture_root_signature(device, &root, errors);
    if (SUCCEEDED(hr)) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = {vertex_shader.data(), vertex_shader.size()};
        desc.PS = {pixel_shader.data(), pixel_shader.size()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = 0xffffffffu;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        result.pso_hr =
            device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        hr = result.pso_hr;
    }
    if (SUCCEEDED(hr))
        hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &queue);
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr,
                                       IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 4;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc,
                                           IID_PPV_ARGS(&resource_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc.NumDescriptors = 2;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = device->CreateDescriptorHeap(&desc,
                                           IID_PPV_ARGS(&sampler_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv_heap));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = texture_desc(TextureShape::Texture1D, false);
        desc.MipLevels = 2;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&texture));
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 4;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&comparison_texture));
        if (SUCCEEDED(hr)) {
            const float depth[] = {0.25f, 0.25f, 0.75f, 0.75f};
            hr = comparison_texture->WriteToSubresource(
                0, nullptr, depth, sizeof(depth), sizeof(depth));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        desc.Width = 4;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&comparison_texture1d));
        if (SUCCEEDED(hr)) {
            const float depth[] = {0.75f, 0.75f, 0.75f, 0.75f};
            hr = comparison_texture1d->WriteToSubresource(
                0, nullptr, depth, sizeof(depth), sizeof(depth));
        }
    }
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        desc.Width = 4;
        desc.Height = 1;
        desc.DepthOrArraySize = 2;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&comparison_texture1d_array));
        const float depth[] = {0.75f, 0.75f, 0.75f, 0.75f};
        for (UINT slice = 0; SUCCEEDED(hr) && slice < 2; ++slice)
            hr = comparison_texture1d_array->WriteToSubresource(
                slice, nullptr, depth, sizeof(depth), sizeof(depth));
    }
    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 4;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(&target));
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 readback_bytes = 0;
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint,
                                      &rows, &row_bytes, &readback_bytes);
        D3D12_HEAP_PROPERTIES props = heap_props(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC desc = buffer_desc(readback_bytes);
        hr = device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
    }
    if (SUCCEEDED(hr)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        make_srv_desc(TextureShape::Texture1D, srv,
                      DXGI_FORMAT_R8G8B8A8_UNORM, 2);
        const UINT resource_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE resource_cpu =
            resource_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateShaderResourceView(texture, &srv, resource_cpu);
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            comparison_texture, &srv,
            offset_cpu(resource_cpu, resource_increment, 1));
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        srv.Texture1D.MipLevels = 1;
        device->CreateShaderResourceView(
            comparison_texture1d, &srv,
            offset_cpu(resource_cpu, resource_increment, 2));
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        srv.Texture1DArray.MipLevels = 1;
        srv.Texture1DArray.ArraySize = 2;
        device->CreateShaderResourceView(
            comparison_texture1d_array, &srv,
            offset_cpu(resource_cpu, resource_increment, 3));
        D3D12_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        const UINT sampler_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu =
            sampler_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateSampler(&sampler, sampler_cpu);
        sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        device->CreateSampler(
            &sampler, offset_cpu(sampler_cpu, sampler_increment, 1));
        device->CreateRenderTargetView(
            target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        D3D12_VIEWPORT viewport = {0.0f, 0.0f, 4.0f, 1.0f, 0.0f, 1.0f};
        D3D12_RECT scissor = {0, 0, 4, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
        list->SetDescriptorHeaps(2, heaps);
        list->SetGraphicsRootSignature(root);
        list->SetGraphicsRootDescriptorTable(
            0, resource_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootDescriptorTable(
            1, sampler_heap->GetGPUDescriptorHandleForHeapStart());
        list->SetPipelineState(pso);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = transition_barrier(
            target, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = execute_and_wait(device, queue, list);
    }
    if (SUCCEEDED(hr)) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(readback_bytes)};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(&result.actual, mapped + footprint.Offset,
                        sizeof(result.actual));
            std::memcpy(&result.actual_high,
                        mapped + footprint.Offset + sizeof(uint32_t),
                        sizeof(result.actual_high));
            std::memcpy(&result.dimensions_actual,
                        mapped + footprint.Offset + 2 * sizeof(uint32_t),
                        sizeof(result.dimensions_actual));
            readback->Unmap(0, nullptr);
        }
    }
    result.hr = hr;
    result.pass = SUCCEEDED(hr) && result.actual == result.expected &&
                  result.actual_high == result.expected_high &&
                  result.dimensions_actual == result.dimensions_expected;
    result.detail = result.pass
                        ? "vertex/pixel LOD and depth comparison sampling matched exact float readback"
                        : errors.empty() ? "pixel-stage texture sampling readback failed"
                                         : errors;
    safe_release(readback);
    safe_release(target);
    safe_release(comparison_texture1d_array);
    safe_release(comparison_texture1d);
    safe_release(comparison_texture);
    safe_release(texture);
    safe_release(rtv_heap);
    safe_release(sampler_heap);
    safe_release(resource_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    return result;
}

int main() {
    ID3D12Device* device = nullptr;
    HRESULT device_hr = create_device(&device);
    std::vector<CaseResult> results;
    if (SUCCEEDED(device_hr)) {
        for (const auto& info : kReadCases)
            results.push_back(run_read_case(device, info));
        for (const auto& info : kStoreCases)
            results.push_back(run_store_case(device, info));
        results.push_back(run_graphics_lod_case(device));
    }
    bool pass = SUCCEEDED(device_hr) && !results.empty();
    for (const auto& result : results)
        pass = pass && result.pass;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.texture-dimensions.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n",
                json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"device_hr\": \"%s\",\n", hr_hex(device_hr).c_str());
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        std::printf("    {\"name\":\"%s\",\"operation\":\"%s\",\"pass\":%s,\"hr\":\"%s\",\"pso_hr\":\"%s\",\"expected\":%u,\"actual\":%u,\"expected_high\":%u,\"actual_high\":%u,\"dimensions_expected\":%u,\"dimensions_actual\":%u,\"detail\":\"%s\"}%s\n",
                    json_escape(result.name).c_str(), result.operation.c_str(),
                    result.pass ? "true" : "false", hr_hex(result.hr).c_str(),
                    hr_hex(result.pso_hr).c_str(), result.expected, result.actual,
                    result.expected_high, result.actual_high,
                    result.dimensions_expected, result.dimensions_actual,
                    json_escape(result.detail).c_str(),
                    i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ]\n}\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    return 0;
}
