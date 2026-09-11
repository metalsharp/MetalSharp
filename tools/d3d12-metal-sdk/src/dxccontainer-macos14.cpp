#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "dxc/DxilContainer/DxilContainer.h"
#include "dxc/DxilContainer/DxilRuntimeReflection.inl"
#include <dxcapi.h>

// This is the ABI consumed by GPTK's D3DMetal.framework. The helper is a
// deliberately small, open-source replacement for the GPTK-only container
// adapter; the compiler and DXBC converter remain separate dylibs.
struct ShaderBytecode {
    void* data;
    uint32_t size;
    uint8_t kind;
    uint8_t owned;
    uint16_t reserved;
};

static void clear_bytecode(ShaderBytecode& out) {
    if (out.owned && out.data)
        std::free(out.data);
    out.data = nullptr;
    out.size = 0;
    out.kind = 0;
    out.owned = 0;
    out.reserved = 0;
}

static bool copy_blob(ShaderBytecode& out, const void* data, size_t size, uint8_t kind) {
    if (!data || size > UINT32_MAX)
        return false;
    void* copy = std::malloc(size ? size : 1);
    if (!copy)
        return false;
    if (size)
        std::memcpy(copy, data, size);
    clear_bytecode(out);
    out.data = copy;
    out.size = static_cast<uint32_t>(size);
    out.kind = kind;
    out.owned = 1;
    out.reserved = 0;
    return true;
}

struct IDxbcConverter : IUnknown {
    virtual HRESULT Convert(const void*, uint32_t, const wchar_t*, void**, uint32_t*, wchar_t**) = 0;
};
using DxcCreateInstanceFn = HRESULT (*)(const CLSID&, const IID&, void**);

static bool convert_dxbc(ShaderBytecode& in, ShaderBytecode& out) {
    void* handle = dlopen("@loader_path/libdxilconv.dylib", RTLD_LOCAL | RTLD_NOW);
    if (!handle)
        return false;
    auto create = reinterpret_cast<DxcCreateInstanceFn>(dlsym(handle, "DxcCreateInstance"));
    if (!create) {
        dlclose(handle);
        return false;
    }
    const CLSID clsid = {0x4900391e, 0xb752, 0x4edd, {0xa8, 0x85, 0x6f, 0xb7, 0x6e, 0x25, 0xad, 0xdb}};
    const IID iid = {0x5f956ed5, 0x78d1, 0x4b15, {0x82, 0x47, 0xf7, 0x18, 0x76, 0x14, 0xa0, 0x41}};
    IDxbcConverter* converter = nullptr;
    HRESULT hr = create(clsid, iid, reinterpret_cast<void**>(&converter));
    if (FAILED(hr) || !converter) {
        dlclose(handle);
        return false;
    }
    void* data = nullptr;
    uint32_t size = 0;
    wchar_t* errors = nullptr;
    hr = converter->Convert(in.data, in.size, nullptr, &data, &size, &errors);
    // The GPTK converter owns its optional diagnostic buffer; the ABI caller
    // intentionally ignores it, matching the original adapter.
    converter->Release();
    dlclose(handle);
    if (FAILED(hr) || !data || !size)
        return false;
    clear_bytecode(out);
    out.data = data;
    out.size = size;
    out.kind = 2;
    out.owned = 1;
    out.reserved = 0;
    return true;
}

static bool compile_hlsl(const std::string& source, const char* profile, const char* entry,
                         const std::vector<std::vector<wchar_t>>& extra,
                         const std::vector<std::vector<wchar_t>>& extra2, ShaderBytecode& out) {
    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    HRESULT utils_hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    HRESULT compiler_hr = utils_hr == S_OK ? DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)) : E_FAIL;
    if (FAILED(utils_hr) || FAILED(compiler_hr)) {
        if (utils)
            utils->Release();
        return false;
    }
    DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    std::vector<std::wstring> storage;
    std::vector<LPCWSTR> args;
    storage.reserve(4 + extra.size() + extra2.size());
    args.reserve(4 + extra.size() + extra2.size());
    storage.emplace_back(L"-E");
    args.push_back(storage.back().c_str());
    auto widen = [](const char* text) {
        std::wstring result;
        if (!text)
            return result;
        while (*text)
            result.push_back(static_cast<unsigned char>(*text++));
        return result;
    };
    storage.emplace_back(widen(entry ? entry : "main"));
    args.push_back(storage.back().c_str());
    storage.emplace_back(L"-T");
    args.push_back(storage.back().c_str());
    storage.emplace_back(widen(profile ? profile : "ps_6_0"));
    args.push_back(storage.back().c_str());
    auto append = [&](const std::vector<std::vector<wchar_t>>& v) {
        for (const auto& arg : v) {
            storage.emplace_back(arg.empty() ? L"" : arg.data());
            args.push_back(storage.back().c_str());
        }
    };
    append(extra);
    append(extra2);
    IDxcResult* result = nullptr;
    HRESULT hr =
        compiler->Compile(&buffer, args.data(), static_cast<uint32_t>(args.size()), nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr) || !result) {
        compiler->Release();
        utils->Release();
        return false;
    }
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    IDxcBlob* blob = nullptr;
    if (SUCCEEDED(status))
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr);
    bool ok = blob && copy_blob(out, blob->GetBufferPointer(), blob->GetBufferSize(), 2);
    if (blob)
        blob->Release();
    result->Release();
    compiler->Release();
    utils->Release();
    return ok;
}

namespace DXCompiler {
bool DXBCToDXILE(ShaderBytecode& input, ShaderBytecode& output) {
    if (!input.data || !input.size)
        return false;
    if (input.kind == 2)
        return copy_blob(output, input.data, input.size, 2);
    return convert_dxbc(input, output);
}

bool HLSLToDXILE(const std::string& source, const char* profile, const char* entry,
                 const std::vector<std::vector<wchar_t>>& extra, const std::vector<std::vector<wchar_t>>& extra2,
                 ShaderBytecode& output) {
    return compile_hlsl(source, profile, entry, extra, extra2, output);
}

const hlsl::DxilPartHeader* GetDxilPartByType(const hlsl::DxilContainerHeader* header, unsigned type) {
    return hlsl::GetDxilPartByType(header, static_cast<hlsl::DxilFourCC>(type));
}
hlsl::DxilPartHeader* GetDxilPartByType(hlsl::DxilContainerHeader* header, unsigned type) {
    return hlsl::GetDxilPartByType(header, static_cast<hlsl::DxilFourCC>(type));
}
bool IsValidDxilContainer(const void* data, size_t size) {
    const auto* header = hlsl::IsDxilContainerLike(data, size);
    return header && hlsl::IsValidDxilContainer(header, size);
}
bool GetGlobalRootSignature(const char* data, const unsigned char** out, unsigned* size) {
    if (!out || !size || !data)
        return false;
    auto* header = reinterpret_cast<const hlsl::DxilContainerHeader*>(data);
    auto* part = hlsl::GetDxilPartByType(header, hlsl::DFCC_RootSignature);
    if (!part) {
        *out = nullptr;
        *size = 0;
        return false;
    }
    *out = reinterpret_cast<const unsigned char*>(hlsl::GetDxilPartData(part));
    *size = part->PartSize;
    return true;
}
} // namespace DXCompiler

// GPTK's adapter was built against an older libc++ ABI spelling. Keep the
// exact loader-facing names as aliases while using the implementation above.
extern "C" bool dxccontainer_hlsl_to_dxil(
    const std::string& source, const char* profile, const char* entry, const std::vector<std::vector<wchar_t>>& extra,
    const std::vector<std::vector<wchar_t>>& extra2,
    ShaderBytecode&
        output) __asm__("__ZN10DXCompiler10HLSLToDXILERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_"
                        "9allocatorIcEEEEPKcSA_RKNS0_6vectorINS1_IwNS2_IwEENS4_ISE_EEEESI_R14ShaderBytecode");
extern "C" bool dxccontainer_hlsl_to_dxil(const std::string& source, const char* profile, const char* entry,
                                          const std::vector<std::vector<wchar_t>>& extra,
                                          const std::vector<std::vector<wchar_t>>& extra2, ShaderBytecode& output) {
    return DXCompiler::HLSLToDXILE(source, profile, entry, extra, extra2, output);
}

extern "C" bool
dxccontainer_dxbc_to_dxil(ShaderBytecode& input,
                          ShaderBytecode& output) __asm__("__ZN10DXCompiler10DXBCToDXILE14ShaderBytecodeRS0_");
extern "C" bool dxccontainer_dxbc_to_dxil(ShaderBytecode& input, ShaderBytecode& output) {
    return DXCompiler::DXBCToDXILE(input, output);
}

extern "C" bool dxccontainer_validate(const char* data,
                                      size_t size) __asm__("__ZN10DXCompiler20IsValidDxilContainerEPKcm");
extern "C" bool dxccontainer_validate(const char* data, size_t size) {
    return DXCompiler::IsValidDxilContainer(data, size);
}
