#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID IID_IDXGIFactory2Probe = {
    0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};

struct ExportProbe {
    const char* name;
    const char* symbol;
    int ordinal;
    FARPROC symbol_proc = nullptr;
    FARPROC ordinal_proc = nullptr;
};

struct ModuleProbe {
    const char* name;
    HMODULE handle = nullptr;
    std::string path;
    std::vector<ExportProbe> exports;
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
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            } else {
                out += c;
            }
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

static std::string module_path(HMODULE module) {
    char buffer[4096];
    DWORD written = GetModuleFileNameA(module, buffer, sizeof(buffer));
    if (written == 0)
        return "";
    if (written >= sizeof(buffer))
        written = sizeof(buffer) - 1;
    return std::string(buffer, written);
}

static std::string wide_to_utf8(const WCHAR* text) {
    if (!text || !text[0])
        return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return "";
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static const char* bool_json(bool value) {
    return value ? "true" : "false";
}

static std::string hr_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return std::string(buf);
}

static uintptr_t rva(HMODULE module, FARPROC proc) {
    if (!module || !proc)
        return 0;
    return reinterpret_cast<uintptr_t>(proc) - reinterpret_cast<uintptr_t>(module);
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

template <typename T> static void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

struct AbiSemanticReport {
    HRESULT create_queue_hr = E_FAIL;
    HRESULT create_allocator_hr = E_FAIL;
    HRESULT create_list_hr = E_FAIL;
    HRESULT create_fence_hr = E_FAIL;
    HRESULT query_factory_self_hr = E_FAIL;
    HRESULT query_adapter_self_hr = E_FAIL;
    HRESULT query_device_self_hr = E_FAIL;
    HRESULT query_device_iunknown_hr = E_FAIL;
    HRESULT query_queue_self_hr = E_FAIL;
    HRESULT query_allocator_self_hr = E_FAIL;
    HRESULT query_list_self_hr = E_FAIL;
    HRESULT query_fence_self_hr = E_FAIL;
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
    bool guid_constants_ok = false;
    bool create_objects_ok = false;
    bool query_interface_ok = false;
    bool vtable_layout_ok = false;
    bool device_child_get_device_ok = false;
    bool device_child_identity_ok = false;
    bool private_data_roundtrip_ok = false;
    bool ok = false;
};

static AbiSemanticReport exercise_abi_semantics(IDXGIFactory2* factory, IDXGIAdapter1* adapter, ID3D12Device* device) {
    AbiSemanticReport report;
    const GUID expected_device = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    const GUID expected_factory2 = {0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
    const GUID expected_adapter1 = {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
    const GUID expected_queue = {0x0ec870a6, 0x5d7e, 0x4c22, {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
    const GUID expected_allocator = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
    const GUID expected_list = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
    const GUID expected_fence = {0x0a753dcf, 0xc4d8, 0x4b91, {0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76}};
    report.guid_constants_ok = guid_equal(__uuidof(ID3D12Device), expected_device) &&
                               guid_equal(__uuidof(IDXGIFactory2), expected_factory2) &&
                               guid_equal(__uuidof(IDXGIAdapter1), expected_adapter1) &&
                               guid_equal(__uuidof(ID3D12CommandQueue), expected_queue) &&
                               guid_equal(__uuidof(ID3D12CommandAllocator), expected_allocator) &&
                               guid_equal(__uuidof(ID3D12GraphicsCommandList), expected_list) &&
                               guid_equal(__uuidof(ID3D12Fence), expected_fence);
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    report.create_queue_hr =
        device ? device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))
               : E_FAIL;
    report.create_allocator_hr =
        device ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                                reinterpret_cast<void**>(&allocator))
               : E_FAIL;
    report.create_list_hr =
        device ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                           __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))
               : E_FAIL;
    if (list)
        list->Close();
    report.create_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence))
               : E_FAIL;
    report.create_objects_ok = queue && allocator && list && fence && SUCCEEDED(report.create_queue_hr) &&
                               SUCCEEDED(report.create_allocator_hr) && SUCCEEDED(report.create_list_hr) &&
                               SUCCEEDED(report.create_fence_hr);

    IDXGIFactory2* factory_self = nullptr;
    IDXGIAdapter1* adapter_self = nullptr;
    ID3D12Device* device_self = nullptr;
    IUnknown* device_unknown = nullptr;
    ID3D12CommandQueue* queue_self = nullptr;
    ID3D12CommandAllocator* allocator_self = nullptr;
    ID3D12GraphicsCommandList* list_self = nullptr;
    ID3D12Fence* fence_self = nullptr;
    ID3D12Device* queue_device = nullptr;
    ID3D12Device* allocator_device = nullptr;
    ID3D12Device* list_device = nullptr;
    ID3D12Device* fence_device = nullptr;
    IUnknown* queue_device_unknown = nullptr;
    IUnknown* allocator_device_unknown = nullptr;
    IUnknown* list_device_unknown = nullptr;
    IUnknown* fence_device_unknown = nullptr;
    report.query_factory_self_hr =
        factory ? factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory_self)) : E_FAIL;
    report.query_adapter_self_hr =
        adapter ? adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter_self)) : E_FAIL;
    report.query_device_self_hr =
        device ? device->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device_self)) : E_FAIL;
    report.query_device_iunknown_hr =
        device ? device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&device_unknown)) : E_FAIL;
    report.query_queue_self_hr =
        queue ? queue->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue_self)) : E_FAIL;
    report.query_allocator_self_hr = allocator ? allocator->QueryInterface(__uuidof(ID3D12CommandAllocator),
                                                                           reinterpret_cast<void**>(&allocator_self))
                                               : E_FAIL;
    report.query_list_self_hr =
        list ? list->QueryInterface(__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list_self)) : E_FAIL;
    report.query_fence_self_hr =
        fence ? fence->QueryInterface(__uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence_self)) : E_FAIL;
    report.queue_get_device_hr =
        queue ? queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&queue_device)) : E_FAIL;
    report.allocator_get_device_hr =
        allocator ? allocator->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&allocator_device)) : E_FAIL;
    report.list_get_device_hr =
        list ? list->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&list_device)) : E_FAIL;
    report.fence_get_device_hr =
        fence ? fence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&fence_device)) : E_FAIL;
    report.queue_device_iunknown_hr =
        queue_device ? queue_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&queue_device_unknown))
                     : E_FAIL;
    report.allocator_device_iunknown_hr =
        allocator_device
            ? allocator_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&allocator_device_unknown))
            : E_FAIL;
    report.list_device_iunknown_hr =
        list_device ? list_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&list_device_unknown))
                    : E_FAIL;
    report.fence_device_iunknown_hr =
        fence_device ? fence_device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&fence_device_unknown))
                     : E_FAIL;

    const GUID private_data_guid = {0x8840934a, 0x7a59, 0x4c73, {0xb2, 0x35, 0xec, 0x9a, 0x71, 0x65, 0x83, 0x12}};
    uint8_t payload[16] = {0x46, 0x52, 0x45, 0x53, 0x48, 0x2d, 0x41, 0x42,
                           0x49, 0x2d, 0x47, 0x55, 0x49, 0x44, 0x21, 0x00};
    uint8_t expected_payload[16] = {};
    std::memcpy(expected_payload, payload, sizeof(payload));
    uint8_t readback[16] = {};
    report.private_data_set_hr = device ? device->SetPrivateData(private_data_guid, sizeof(payload), payload) : E_FAIL;
    std::memset(payload, 0xa5, sizeof(payload));
    report.private_data_size = sizeof(readback);
    report.private_data_get_hr =
        device ? device->GetPrivateData(private_data_guid, &report.private_data_size, readback) : E_FAIL;
    report.private_data_roundtrip_ok = SUCCEEDED(report.private_data_set_hr) && SUCCEEDED(report.private_data_get_hr) &&
                                       report.private_data_size == sizeof(expected_payload) &&
                                       std::memcmp(expected_payload, readback, sizeof(expected_payload)) == 0;
    report.query_interface_ok = SUCCEEDED(report.query_factory_self_hr) && SUCCEEDED(report.query_adapter_self_hr) &&
                                SUCCEEDED(report.query_device_self_hr) && SUCCEEDED(report.query_device_iunknown_hr) &&
                                SUCCEEDED(report.query_queue_self_hr) && SUCCEEDED(report.query_allocator_self_hr) &&
                                SUCCEEDED(report.query_list_self_hr) && SUCCEEDED(report.query_fence_self_hr) &&
                                factory_self && adapter_self && device_self && device_unknown && queue_self &&
                                allocator_self && list_self && fence_self;
    report.vtable_layout_ok = com_vtable_nonnull(factory) && com_vtable_nonnull(adapter) &&
                              com_vtable_nonnull(device) && com_vtable_nonnull(queue) &&
                              com_vtable_nonnull(allocator) && com_vtable_nonnull(list) && com_vtable_nonnull(fence);
    report.device_child_get_device_ok = SUCCEEDED(report.queue_get_device_hr) &&
                                        SUCCEEDED(report.allocator_get_device_hr) &&
                                        SUCCEEDED(report.list_get_device_hr) && SUCCEEDED(report.fence_get_device_hr) &&
                                        queue_device && allocator_device && list_device && fence_device;
    report.device_child_identity_ok = report.device_child_get_device_ok && device_unknown && queue_device_unknown &&
                                      allocator_device_unknown && list_device_unknown && fence_device_unknown &&
                                      queue_device_unknown == device_unknown &&
                                      allocator_device_unknown == device_unknown &&
                                      list_device_unknown == device_unknown && fence_device_unknown == device_unknown;
    report.ok = report.guid_constants_ok && report.create_objects_ok && report.query_interface_ok &&
                report.vtable_layout_ok && report.device_child_get_device_ok && report.device_child_identity_ok &&
                report.private_data_roundtrip_ok;
    safe_release(factory_self);
    safe_release(adapter_self);
    safe_release(device_self);
    safe_release(device_unknown);
    safe_release(queue_self);
    safe_release(allocator_self);
    safe_release(list_self);
    safe_release(fence_self);
    safe_release(queue_device_unknown);
    safe_release(allocator_device_unknown);
    safe_release(list_device_unknown);
    safe_release(fence_device_unknown);
    safe_release(queue_device);
    safe_release(allocator_device);
    safe_release(list_device);
    safe_release(fence_device);
    safe_release(fence);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    return report;
}

static ModuleProbe inspect_module(const char* name, std::vector<ExportProbe> exports) {
    ModuleProbe probe{name, nullptr, "", exports};
    probe.handle = LoadLibraryA(name);
    if (!probe.handle)
        return probe;
    probe.path = module_path(probe.handle);
    for (auto& item : probe.exports) {
        if (item.symbol)
            item.symbol_proc = GetProcAddress(probe.handle, item.symbol);
        if (item.ordinal > 0)
            item.ordinal_proc = GetProcAddress(probe.handle, MAKEINTRESOURCEA(item.ordinal));
    }
    return probe;
}

static void print_export_json(const ModuleProbe& module, const ExportProbe& probe, bool last) {
    std::printf("        {\n");
    std::printf("          \"name\": \"%s\",\n", json_escape(probe.name).c_str());
    if (probe.symbol) {
        std::printf("          \"symbol\": \"%s\",\n", json_escape(probe.symbol).c_str());
        std::printf("          \"has_symbol\": %s,\n", bool_json(probe.symbol_proc != nullptr));
        std::printf("          \"symbol_rva\": \"0x%llx\",\n",
                    static_cast<unsigned long long>(rva(module.handle, probe.symbol_proc)));
    } else {
        std::printf("          \"symbol\": null,\n");
        std::printf("          \"has_symbol\": null,\n");
        std::printf("          \"symbol_rva\": null,\n");
    }
    if (probe.ordinal > 0) {
        std::printf("          \"ordinal\": %d,\n", probe.ordinal);
        std::printf("          \"has_ordinal\": %s,\n", bool_json(probe.ordinal_proc != nullptr));
        std::printf("          \"ordinal_rva\": \"0x%llx\"\n",
                    static_cast<unsigned long long>(rva(module.handle, probe.ordinal_proc)));
    } else {
        std::printf("          \"ordinal\": null,\n");
        std::printf("          \"has_ordinal\": null,\n");
        std::printf("          \"ordinal_rva\": null\n");
    }
    std::printf("        }%s\n", last ? "" : ",");
}

static void print_module_json(const ModuleProbe& module, bool last) {
    std::printf("    \"%s\": {\n", json_escape(module.name).c_str());
    std::printf("      \"loaded\": %s,\n", bool_json(module.handle != nullptr));
    std::printf("      \"path\": \"%s\",\n", json_escape(module.path).c_str());
    std::printf("      \"base\": \"0x%llx\",\n",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module.handle)));
    std::printf("      \"exports\": [\n");
    for (size_t i = 0; i < module.exports.size(); ++i)
        print_export_json(module, module.exports[i], i + 1 == module.exports.size());
    std::printf("      ]\n");
    std::printf("    }%s\n", last ? "" : ",");
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    auto d3d12 = inspect_module("d3d12.dll", {{"D3D12CreateDevice", "D3D12CreateDevice", 101},
                                              {"D3D12GetInterface", "D3D12GetInterface", 0},
                                              {"D3D12SerializeRootSignature", "D3D12SerializeRootSignature", 0}});
    auto dxgi = inspect_module("dxgi.dll", {{"CreateDXGIFactory", "CreateDXGIFactory", 9},
                                            {"CreateDXGIFactory1", "CreateDXGIFactory1", 10},
                                            {"CreateDXGIFactory2", "CreateDXGIFactory2", 11}});
    auto dxgi_dxmt = inspect_module("dxgi_dxmt.dll", {{"CreateDXGIFactory", "CreateDXGIFactory", 9},
                                                      {"CreateDXGIFactory1", "CreateDXGIFactory1", 10},
                                                      {"CreateDXGIFactory2", "CreateDXGIFactory2", 11}});
    auto winemetal = inspect_module(
        "winemetal.dll", {{"WMTBootstrapRegister", "WMTBootstrapRegister", 0},
                          {"WMTBootstrapLookUp", "WMTBootstrapLookUp", 0},
                          {"WMTSetMetalShaderCachePath", "WMTSetMetalShaderCachePath", 0},
                          {"MTLLibrary_newFunctionWithDescriptor", "MTLLibrary_newFunctionWithDescriptor", 0}});

    using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

    auto create_device = reinterpret_cast<D3D12CreateDeviceFn>(reinterpret_cast<void*>(d3d12.exports[0].symbol_proc));
    auto create_factory2 = reinterpret_cast<CreateDXGIFactory2Fn>(reinterpret_cast<void*>(dxgi.exports[2].symbol_proc));

    IDXGIFactory2* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 adapter_desc = {};
    HRESULT create_factory_hr =
        create_factory2 ? create_factory2(0, IID_IDXGIFactory2Probe, reinterpret_cast<void**>(&factory)) : E_FAIL;
    HRESULT enum_adapter_hr = factory ? factory->EnumAdapters1(0, &adapter) : E_FAIL;
    HRESULT adapter_desc_hr = adapter ? adapter->GetDesc1(&adapter_desc) : E_FAIL;

    ID3D12Device* device = nullptr;
    HRESULT create_device_hr = create_device ? create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                             reinterpret_cast<void**>(&device))
                                             : E_FAIL;
    if (FAILED(create_device_hr) && create_device) {
        create_device_hr =
            create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe, reinterpret_cast<void**>(&device));
    }

    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
    D3D_FEATURE_LEVEL requested_levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    levels.NumFeatureLevels = static_cast<UINT>(sizeof(requested_levels) / sizeof(requested_levels[0]));
    levels.pFeatureLevelsRequested = requested_levels;
    levels.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT feature_levels_hr =
        device ? device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels)) : E_FAIL;
    AbiSemanticReport abi_semantics = exercise_abi_semantics(factory, adapter, device);

    bool module_exports_ok = d3d12.handle && dxgi.handle && dxgi_dxmt.handle && winemetal.handle &&
                             d3d12.exports[0].symbol_proc && d3d12.exports[0].ordinal_proc &&
                             dxgi.exports[2].symbol_proc && dxgi_dxmt.exports[2].symbol_proc &&
                             winemetal.exports[0].symbol_proc && winemetal.exports[1].symbol_proc;
    bool adapter_ok = SUCCEEDED(create_factory_hr) && SUCCEEDED(enum_adapter_hr) && SUCCEEDED(adapter_desc_hr) &&
                      adapter_desc.VendorId != 0;
    bool device_ok = SUCCEEDED(create_device_hr) && device != nullptr && SUCCEEDED(feature_levels_hr);
    bool pass = module_exports_ok && adapter_ok && device_ok && abi_semantics.ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.vkd3d.fresh.runtime-identity.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", bool_json(pass));
    std::printf("  \"environment\": {\n");
    std::printf("    \"WINEDLLOVERRIDES\": \"%s\",\n", json_escape(getenv_string("WINEDLLOVERRIDES")).c_str());
    std::printf("    \"WINEDLLPATH\": \"%s\",\n", json_escape(getenv_string("WINEDLLPATH")).c_str());
    std::printf("    \"DXMT_WINEMETAL_UNIXLIB\": \"%s\"\n",
                json_escape(getenv_string("DXMT_WINEMETAL_UNIXLIB")).c_str());
    std::printf("  },\n");
    std::printf("  \"modules\": {\n");
    print_module_json(d3d12, false);
    print_module_json(dxgi, false);
    print_module_json(dxgi_dxmt, false);
    print_module_json(winemetal, true);
    std::printf("  },\n");
    std::printf("  \"dxgi\": {\n");
    std::printf("    \"CreateDXGIFactory2\": \"%s\",\n", hr_hex(create_factory_hr).c_str());
    std::printf("    \"EnumAdapters1\": \"%s\",\n", hr_hex(enum_adapter_hr).c_str());
    std::printf("    \"GetDesc1\": \"%s\",\n", hr_hex(adapter_desc_hr).c_str());
    std::printf("    \"adapter_description\": \"%s\",\n", json_escape(wide_to_utf8(adapter_desc.Description)).c_str());
    std::printf("    \"vendor_id\": %u,\n", adapter_desc.VendorId);
    std::printf("    \"device_id\": %u,\n", adapter_desc.DeviceId);
    std::printf("    \"dedicated_video_memory\": %llu,\n",
                static_cast<unsigned long long>(adapter_desc.DedicatedVideoMemory));
    std::printf("    \"shared_system_memory\": %llu\n",
                static_cast<unsigned long long>(adapter_desc.SharedSystemMemory));
    std::printf("  },\n");
    std::printf("  \"abi_semantics\": {\n");
    std::printf("    \"proof_scope\": \"runtime_guid_com_abi_queryinterface_private_data_bootstrap\",\n");
    std::printf("    \"IID_ID3D12Device\": \"%s\",\n", guid_string(__uuidof(ID3D12Device)).c_str());
    std::printf("    \"IID_IDXGIFactory2\": \"%s\",\n", guid_string(__uuidof(IDXGIFactory2)).c_str());
    std::printf("    \"IID_IDXGIAdapter1\": \"%s\",\n", guid_string(__uuidof(IDXGIAdapter1)).c_str());
    std::printf("    \"IID_ID3D12CommandQueue\": \"%s\",\n", guid_string(__uuidof(ID3D12CommandQueue)).c_str());
    std::printf("    \"IID_ID3D12CommandAllocator\": \"%s\",\n", guid_string(__uuidof(ID3D12CommandAllocator)).c_str());
    std::printf("    \"IID_ID3D12GraphicsCommandList\": \"%s\",\n",
                guid_string(__uuidof(ID3D12GraphicsCommandList)).c_str());
    std::printf("    \"IID_ID3D12Fence\": \"%s\",\n", guid_string(__uuidof(ID3D12Fence)).c_str());
    std::printf("    \"CreateCommandQueue\": \"%s\",\n", hr_hex(abi_semantics.create_queue_hr).c_str());
    std::printf("    \"CreateCommandAllocator\": \"%s\",\n", hr_hex(abi_semantics.create_allocator_hr).c_str());
    std::printf("    \"CreateCommandList\": \"%s\",\n", hr_hex(abi_semantics.create_list_hr).c_str());
    std::printf("    \"CreateFence\": \"%s\",\n", hr_hex(abi_semantics.create_fence_hr).c_str());
    std::printf("    \"QueryInterface_IDXGIFactory2\": \"%s\",\n", hr_hex(abi_semantics.query_factory_self_hr).c_str());
    std::printf("    \"QueryInterface_IDXGIAdapter1\": \"%s\",\n", hr_hex(abi_semantics.query_adapter_self_hr).c_str());
    std::printf("    \"QueryInterface_ID3D12Device\": \"%s\",\n", hr_hex(abi_semantics.query_device_self_hr).c_str());
    std::printf("    \"QueryInterface_IUnknown_device\": \"%s\",\n",
                hr_hex(abi_semantics.query_device_iunknown_hr).c_str());
    std::printf("    \"QueryInterface_ID3D12CommandQueue\": \"%s\",\n",
                hr_hex(abi_semantics.query_queue_self_hr).c_str());
    std::printf("    \"QueryInterface_ID3D12CommandAllocator\": \"%s\",\n",
                hr_hex(abi_semantics.query_allocator_self_hr).c_str());
    std::printf("    \"QueryInterface_ID3D12GraphicsCommandList\": \"%s\",\n",
                hr_hex(abi_semantics.query_list_self_hr).c_str());
    std::printf("    \"QueryInterface_ID3D12Fence\": \"%s\",\n", hr_hex(abi_semantics.query_fence_self_hr).c_str());
    std::printf("    \"GetDevice_from_queue\": \"%s\",\n", hr_hex(abi_semantics.queue_get_device_hr).c_str());
    std::printf("    \"GetDevice_from_allocator\": \"%s\",\n", hr_hex(abi_semantics.allocator_get_device_hr).c_str());
    std::printf("    \"GetDevice_from_list\": \"%s\",\n", hr_hex(abi_semantics.list_get_device_hr).c_str());
    std::printf("    \"GetDevice_from_fence\": \"%s\",\n", hr_hex(abi_semantics.fence_get_device_hr).c_str());
    std::printf("    \"QueryInterface_IUnknown_queue_device\": \"%s\",\n",
                hr_hex(abi_semantics.queue_device_iunknown_hr).c_str());
    std::printf("    \"QueryInterface_IUnknown_allocator_device\": \"%s\",\n",
                hr_hex(abi_semantics.allocator_device_iunknown_hr).c_str());
    std::printf("    \"QueryInterface_IUnknown_list_device\": \"%s\",\n",
                hr_hex(abi_semantics.list_device_iunknown_hr).c_str());
    std::printf("    \"QueryInterface_IUnknown_fence_device\": \"%s\",\n",
                hr_hex(abi_semantics.fence_device_iunknown_hr).c_str());
    std::printf("    \"SetPrivateData\": \"%s\",\n", hr_hex(abi_semantics.private_data_set_hr).c_str());
    std::printf("    \"GetPrivateData\": \"%s\",\n", hr_hex(abi_semantics.private_data_get_hr).c_str());
    std::printf("    \"private_data_size\": %u,\n", abi_semantics.private_data_size);
    std::printf("    \"guid_constants_ok\": %s,\n", bool_json(abi_semantics.guid_constants_ok));
    std::printf("    \"create_objects_ok\": %s,\n", bool_json(abi_semantics.create_objects_ok));
    std::printf("    \"query_interface_ok\": %s,\n", bool_json(abi_semantics.query_interface_ok));
    std::printf("    \"vtable_layout_ok\": %s,\n", bool_json(abi_semantics.vtable_layout_ok));
    std::printf("    \"device_child_get_device_ok\": %s,\n", bool_json(abi_semantics.device_child_get_device_ok));
    std::printf("    \"device_child_identity_ok\": %s,\n", bool_json(abi_semantics.device_child_identity_ok));
    std::printf("    \"private_data_copy_semantics\": "
                "\"caller_buffer_mutated_after_SetPrivateData_before_GetPrivateData\",\n");
    std::printf("    \"private_data_roundtrip_ok\": %s,\n", bool_json(abi_semantics.private_data_roundtrip_ok));
    std::printf("    \"ok\": %s\n", bool_json(abi_semantics.ok));
    std::printf("  },\n");
    std::printf("  \"d3d12\": {\n");
    std::printf("    \"D3D12CreateDevice\": \"%s\",\n", hr_hex(create_device_hr).c_str());
    std::printf("    \"CheckFeatureSupport_FEATURE_LEVELS\": \"%s\",\n", hr_hex(feature_levels_hr).c_str());
    std::printf("    \"max_feature_level\": \"0x%x\"\n", static_cast<unsigned>(levels.MaxSupportedFeatureLevel));
    std::printf("  }\n");
    std::printf("}\n");

    if (device)
        device->Release();
    if (adapter)
        adapter->Release();
    if (factory)
        factory->Release();
    // This gate proves runtime identity and bootstrap, not process teardown.
    // Avoid CRT/static destruction after the JSON is flushed because the VKD3D
    // runtime owns process-global worker state whose orderly shutdown is covered
    // by later lifecycle gates.
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    return pass ? 0 : 1;
}
