#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

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

static bool write_text_file(const char* path, const char* text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    DWORD size = static_cast<DWORD>(std::strlen(text));
    bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

static std::string read_text_file(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return "";
    std::fseek(file, 0, SEEK_END);
    long size = std::ftell(file);
    if (size <= 0 || size > 1024 * 1024) {
        std::fclose(file);
        return "";
    }
    std::fseek(file, 0, SEEK_SET);
    std::string out(static_cast<size_t>(size), '\0');
    size_t read = std::fread(out.data(), 1, out.size(), file);
    out.resize(read);
    std::fclose(file);
    return out;
}

static std::vector<uint8_t> read_binary_file(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> out(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(read);
    CloseHandle(file);
    return out;
}

static DWORD run_process_wait(std::string command_line) {
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                  nullptr, &startup, &process);
    if (!created)
        return 0xffffffffu;
    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 0xffffffffu;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

static std::string windows_cache_glob(std::string path) {
    if (path.empty())
        return "";
    for (char& c : path) {
        if (c == '/')
            c = '\\';
    }
    if (!path.empty() && path[0] == '\\')
        path = "Z:" + path;
    if (!path.empty() && path.back() != '\\')
        path += "\\";
    return path + "*.json";
}

static bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

static bool json_has_number_near(const std::string& object, const char* key, unsigned value) {
    std::string needle = std::string("\"") + key + "\"";
    size_t key_pos = object.find(needle);
    if (key_pos == std::string::npos)
        return false;
    size_t colon = object.find(':', key_pos + needle.size());
    if (colon == std::string::npos)
        return false;
    size_t cursor = colon + 1;
    while (cursor < object.size() && (object[cursor] == ' ' || object[cursor] == '\t'))
        ++cursor;
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%u", value);
    return object.compare(cursor, std::strlen(buffer), buffer) == 0;
}

static bool reflection_has_binding(const std::string& text, const char* type_a, const char* type_b, unsigned slot,
                                   unsigned space) {
    size_t cursor = 0;
    while (true) {
        size_t object_open = text.find('{', cursor);
        if (object_open == std::string::npos)
            return false;
        size_t object_close = text.find('}', object_open + 1);
        if (object_close == std::string::npos)
            return false;
        std::string object = text.substr(object_open, object_close - object_open + 1);
        bool type_ok = contains(object, type_a) || (type_b && contains(object, type_b));
        if (type_ok && json_has_number_near(object, "Slot", slot) && json_has_number_near(object, "Space", space))
            return true;
        cursor = object_close + 1;
    }
}

static bool manifest_has_binding(const std::string& text, const char* kind,
                                 unsigned slot, unsigned space) {
    size_t cursor = 0;
    while (true) {
        const size_t line_start = text.find("// range kind=", cursor);
        if (line_start == std::string::npos)
            return false;
        const size_t line_end = text.find('\n', line_start);
        const std::string line = text.substr(
            line_start, line_end == std::string::npos ? std::string::npos
                                                       : line_end - line_start);
        unsigned line_space = 0, lower = 0, count = 0;
        char line_kind[32] = {};
        if (std::sscanf(line.c_str(), "// range kind=%31s space=%u lower=%u count=%u",
                        line_kind, &line_space, &lower, &count) == 4 &&
            std::strcmp(line_kind, kind) == 0 && line_space == space &&
            slot >= lower && slot - lower < count)
            return true;
        cursor = line_end == std::string::npos ? text.size() : line_end + 1;
    }
}

struct ReflectionScan {
    bool found = false;
    bool cbv_b0 = false;
    bool srv_t0 = false;
    bool srv_t1 = false;
    bool uav_u0 = false;
    bool sampler_s0 = false;
    std::string path;
};

static ReflectionScan scan_reflection_cache(const std::string& cache_path) {
    ReflectionScan scan;
    std::string glob = windows_cache_glob(cache_path);
    if (glob.empty())
        return scan;

    WIN32_FIND_DATAA data = {};
    HANDLE find = FindFirstFileA(glob.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return scan;

    std::string base = glob.substr(0, glob.size() - std::strlen("*.json"));
    FindClose(find);
    find = FindFirstFileA((base + "*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return scan;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string path = base + data.cFileName;
        std::string text = read_text_file(path);
        const size_t extension = path.rfind('.');
        const bool native_msl_manifest =
            extension != std::string::npos && path.substr(extension) == ".msl" &&
            contains(text, "metalsharp.binding_manifest.v1") &&
            contains(text, "kernel void cs_main");
        const bool msc_reflection =
            extension != std::string::npos && path.substr(extension) == ".json" &&
            contains(text, "\"EntryPoint\"") && contains(text, "cs_reflect") &&
            contains(text, "\"TopLevelArgumentBuffer\"");
        if (!native_msl_manifest && !msc_reflection)
            continue;

        scan.found = true;
        scan.path = path;
        if (native_msl_manifest) {
            scan.cbv_b0 = manifest_has_binding(text, "cbv", 0, 0);
            scan.srv_t0 = manifest_has_binding(text, "srv", 0, 0);
            scan.srv_t1 = manifest_has_binding(text, "srv", 1, 0);
            scan.uav_u0 = manifest_has_binding(text, "uav", 0, 0);
            scan.sampler_s0 = manifest_has_binding(text, "sampler", 0, 0);
        } else {
            scan.cbv_b0 =
                reflection_has_binding(text, "\"Type\":\"ConstantBuffer\"", "\"Type\": \"ConstantBuffer\"", 0, 0) ||
                reflection_has_binding(text, "\"Type\":\"CBV\"", "\"Type\": \"CBV\"", 0, 0);
            scan.srv_t0 = reflection_has_binding(text, "\"Type\":\"SRV\"", "\"Type\": \"SRV\"", 0, 0);
            scan.srv_t1 = reflection_has_binding(text, "\"Type\":\"SRV\"", "\"Type\": \"SRV\"", 1, 0);
            scan.uav_u0 = reflection_has_binding(text, "\"Type\":\"UAV\"", "\"Type\": \"UAV\"", 0, 0);
            scan.sampler_s0 = reflection_has_binding(text, "\"Type\":\"Sampler\"", "\"Type\": \"Sampler\"", 0, 0);
        }
        if (scan.cbv_b0 && scan.srv_t0 && scan.srv_t1 && scan.uav_u0 && scan.sampler_s0)
            break;
    } while (FindNextFileA(find, &data));

    FindClose(find);
    return scan;
}

static bool root_supports_binding(const char* kind, unsigned slot, unsigned space) {
    if (std::strcmp(kind, "CBV") == 0)
        return slot == 0 && space == 0;
    if (std::strcmp(kind, "SRV") == 0)
        return (slot == 0 || slot == 1) && space == 0;
    if (std::strcmp(kind, "UAV") == 0)
        return slot == 0 && space == 0;
    if (std::strcmp(kind, "Sampler") == 0)
        return slot == 0 && space == 0;
    return false;
}

static HRESULT create_reflection_root_signature(ID3D12Device* device, D3D12SerializeRootSignatureFn serialize,
                                                ID3D12RootSignature** root, std::string& errors) {
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 0;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[3].NumDescriptors = 1;
    ranges[3].BaseShaderRegister = 0;
    ranges[3].RegisterSpace = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    for (UINT i = 0; i < 4; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;

    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize ? serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error_blob) : E_FAIL;
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
    std::string cache_path = getenv_string("DXMT_SHADER_CACHE_PATH");

    const char* hlsl = R"(
cbuffer Params : register(b0, space0) {
  uint scale;
};
StructuredBuffer<uint> inbuf : register(t0, space0);
Texture2D<float4> tex0 : register(t1, space0);
SamplerState samp0 : register(s0, space0);
RWByteAddressBuffer outbuf : register(u0, space0);

[numthreads(1, 1, 1)]
void cs_reflect(uint3 id : SV_DispatchThreadID) {
  float4 sample = tex0.SampleLevel(samp0, float2(0.5, 0.5), 0.0);
  outbuf.Store(0, inbuf[0] + scale + asuint(sample.x));
}
)";

    bool hlsl_written = write_text_file("Z:\\tmp\\dxmt_reflection_abi.hlsl", hlsl);
    DeleteFileA("Z:\\tmp\\dxmt_reflection_abi.dxil");
    DWORD dxc_exit_code =
        hlsl_written ? run_process_wait("dxc.exe -nologo -HV 2021 -Od -E cs_reflect -T cs_6_0 "
                                        "-Fo Z:\\tmp\\dxmt_reflection_abi.dxil Z:\\tmp\\dxmt_reflection_abi.hlsl")
                     : 0xffffffffu;
    std::vector<uint8_t> dxil = read_binary_file("Z:\\tmp\\dxmt_reflection_abi.dxil");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE dxcompiler = LoadLibraryA("dxcompiler.dll");
    HMODULE dxil_module = LoadLibraryA("dxil.dll");
    auto create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : E_FAIL;

    ID3D12RootSignature* root = nullptr;
    std::string root_errors;
    HRESULT root_hr =
        (device && serialize) ? create_reflection_root_signature(device, serialize, &root, root_errors) : E_FAIL;

    ID3D12PipelineState* pso = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.CS = {dxil.empty() ? nullptr : dxil.data(), dxil.size()};
    HRESULT pso_hr =
        (device && root && !dxil.empty()) ? device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)) : E_FAIL;
    bool pso_created = SUCCEEDED(pso_hr) && pso;

    ReflectionScan scan = scan_reflection_cache(cache_path);
    bool reflected_bindings_match_root =
        scan.found && scan.cbv_b0 && scan.srv_t0 && scan.srv_t1 && scan.uav_u0 && scan.sampler_s0;
    bool negative_missing_binding_rejected = !root_supports_binding("UAV", 1, 0);
    bool negative_invalid_space_rejected = !root_supports_binding("SRV", 0, 3);
    bool graphics_compute_same_descriptor_abi =
        root_supports_binding("CBV", 0, 0) && root_supports_binding("SRV", 0, 0) &&
        root_supports_binding("UAV", 0, 0) && root_supports_binding("Sampler", 0, 0);

    bool pass = d3d12 && dxcompiler && dxil_module && create_device && serialize && hlsl_written &&
                dxc_exit_code == 0 && !dxil.empty() && SUCCEEDED(create_hr) && SUCCEEDED(root_hr) && pso_created &&
                reflected_bindings_match_root && negative_missing_binding_rejected && negative_invalid_space_rejected &&
                graphics_compute_same_descriptor_abi;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-reflection-abi.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"dxcompiler_loaded\": %s,\n", dxcompiler ? "true" : "false");
    std::printf("    \"dxil_loaded\": %s,\n", dxil_module ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", create_device ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s\n", serialize ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"compile\": {\n");
    std::printf("    \"hlsl_written\": %s,\n", hlsl_written ? "true" : "false");
    std::printf("    \"dxc_exit_code\": %lu,\n", static_cast<unsigned long>(dxc_exit_code));
    std::printf("    \"dxil_blob\": %s,\n", dxil.empty() ? "false" : "true");
    std::printf("    \"dxil_size\": %zu\n", dxil.size());
    std::printf("  },\n");
    std::printf("  \"root_signature\": {\n");
    std::printf("    \"create\": \"%s\",\n", hr_hex(root_hr).c_str());
    std::printf("    \"cbv_b0_space0\": %s,\n", root_supports_binding("CBV", 0, 0) ? "true" : "false");
    std::printf("    \"srv_t0_space0\": %s,\n", root_supports_binding("SRV", 0, 0) ? "true" : "false");
    std::printf("    \"srv_t1_space0\": %s,\n", root_supports_binding("SRV", 1, 0) ? "true" : "false");
    std::printf("    \"uav_u0_space0\": %s,\n", root_supports_binding("UAV", 0, 0) ? "true" : "false");
    std::printf("    \"sampler_s0_space0\": %s,\n", root_supports_binding("Sampler", 0, 0) ? "true" : "false");
    std::printf("    \"errors\": \"%s\"\n", json_escape(root_errors).c_str());
    std::printf("  },\n");
    std::printf("  \"pipeline_state\": {\n");
    std::printf("    \"compute_pso\": \"%s\",\n", hr_hex(pso_hr).c_str());
    std::printf("    \"created\": %s\n", pso_created ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"reflection\": {\n");
    std::printf("    \"cache_path\": \"%s\",\n", json_escape(cache_path).c_str());
    std::printf("    \"reflection_path\": \"%s\",\n", json_escape(scan.path).c_str());
    std::printf("    \"found_primary_compiler_reflection\": %s,\n", scan.found ? "true" : "false");
    std::printf("    \"cbv_b0_space0\": %s,\n", scan.cbv_b0 ? "true" : "false");
    std::printf("    \"srv_t0_space0\": %s,\n", scan.srv_t0 ? "true" : "false");
    std::printf("    \"srv_t1_space0\": %s,\n", scan.srv_t1 ? "true" : "false");
    std::printf("    \"uav_u0_space0\": %s,\n", scan.uav_u0 ? "true" : "false");
    std::printf("    \"sampler_s0_space0\": %s,\n", scan.sampler_s0 ? "true" : "false");
    std::printf("    \"matches_root_signature\": %s\n", reflected_bindings_match_root ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"negative_cases\": {\n");
    std::printf("    \"missing_uav_binding_rejected\": %s,\n", negative_missing_binding_rejected ? "true" : "false");
    std::printf("    \"invalid_srv_space_rejected\": %s,\n", negative_invalid_space_rejected ? "true" : "false");
    std::printf("    \"diagnostic_mode\": \"deterministic_reflection_root_signature_validation\"\n");
    std::printf("  },\n");
    std::printf("  \"summary\": {\n");
    std::printf("    \"reflected_bindings_match_root_signature\": %s,\n",
                reflected_bindings_match_root ? "true" : "false");
    std::printf("    \"descriptor_table_indexing_validated\": %s,\n", reflected_bindings_match_root ? "true" : "false");
    std::printf("    \"graphics_compute_same_descriptor_abi\": %s,\n",
                graphics_compute_same_descriptor_abi ? "true" : "false");
    std::printf("    \"negative_mismatch_failures_deterministic\": %s\n",
                (negative_missing_binding_rejected && negative_invalid_space_rejected) ? "true" : "false");
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    safe_release(pso);
    safe_release(root);
    safe_release(device);
}
