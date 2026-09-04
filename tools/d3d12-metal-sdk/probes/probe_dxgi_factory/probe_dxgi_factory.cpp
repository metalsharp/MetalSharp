#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <dxgi1_6.h>
#include <d3d12.h>

struct InterfaceProbe {
    const char* name;
    GUID iid;
    HRESULT hr = E_FAIL;
    bool supported = false;
};

static const GUID IID_DXGIAdapterProbe = {0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};
static const GUID IID_DXGIAdapter1Probe = {
    0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
static const GUID IID_DXGIFactoryProbe = {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
static const GUID IID_DXGIFactory1Probe = {
    0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID IID_DXGIFactory2Probe = {
    0x50c83a1c, 0xe072, 0x4c48, {0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0}};
static const GUID IID_DXGIFactory3Probe = {
    0x25483823, 0xcd46, 0x4c7d, {0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd}};
static const GUID IID_DXGIFactory4Probe = {
    0x1bc6ea02, 0xef36, 0x464f, {0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a}};
static const GUID IID_DXGIFactory5Probe = {
    0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
static const GUID IID_DXGIFactory6Probe = {
    0xc1b6694f, 0xff09, 0x44a9, {0xb0, 0x3c, 0x77, 0x90, 0x02, 0xa4, 0x7b, 0x07}};
static const GUID IID_DXGIFactory7Probe = {
    0xa4966eed, 0x76db, 0x44da, {0x84, 0xc1, 0xee, 0x9a, 0x7a, 0xfb, 0x20, 0xa8}};
static const GUID IID_DXGIUnknownProbe = {0xd312d312, 0x2026, 0x0523, {0xaa, 0xbb, 0xcc, 0xdd, 0x12, 0x34, 0x56, 0x78}};

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

static std::string wide_to_utf8(const WCHAR* value) {
    if (!value || !value[0])
        return "";
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return "";
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static void print_hr(const char* key, HRESULT hr, bool comma = true) {
    std::printf("    \"%s\": \"0x%08lx\"%s\n", key, static_cast<unsigned long>(static_cast<uint32_t>(hr)),
                comma ? "," : "");
}

static void print_interface_json(const InterfaceProbe& probe, bool last) {
    std::printf("    \"%s\": {\n", probe.name);
    std::printf("      \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(probe.hr)));
    std::printf("      \"supported\": %s\n", probe.supported ? "true" : "false");
    std::printf("    }%s\n", last ? "" : ",");
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_factory = reinterpret_cast<CreateFactoryFn>(
        reinterpret_cast<void*>(dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory") : nullptr));
    auto create_factory1 = reinterpret_cast<CreateFactoryFn>(
        reinterpret_cast<void*>(dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr));
    auto create_factory2 = reinterpret_cast<CreateFactory2Fn>(
        reinterpret_cast<void*>(dxgi ? GetProcAddress(dxgi, "CreateDXGIFactory2") : nullptr));

    IDXGIFactory* factory0 = nullptr;
    IDXGIFactory1* factory1 = nullptr;
    IDXGIFactory2* factory2 = nullptr;
    HRESULT create_hr =
        create_factory ? create_factory(IID_DXGIFactoryProbe, reinterpret_cast<void**>(&factory0)) : E_FAIL;
    HRESULT create1_hr =
        create_factory1 ? create_factory1(IID_DXGIFactory1Probe, reinterpret_cast<void**>(&factory1)) : E_FAIL;
    HRESULT create2_hr =
        create_factory2 ? create_factory2(0, IID_DXGIFactory2Probe, reinterpret_cast<void**>(&factory2)) : E_FAIL;

    IUnknown* factory_unknown = factory1 ? static_cast<IUnknown*>(factory1) : static_cast<IUnknown*>(factory0);
    std::vector<InterfaceProbe> interfaces = {
        {"IDXGIFactory", IID_DXGIFactoryProbe, E_FAIL, false},
        {"IDXGIFactory1", IID_DXGIFactory1Probe, E_FAIL, false},
        {"IDXGIFactory2", IID_DXGIFactory2Probe, E_FAIL, false},
        {"IDXGIFactory3", IID_DXGIFactory3Probe, E_FAIL, false},
        {"IDXGIFactory4", IID_DXGIFactory4Probe, E_FAIL, false},
        {"IDXGIFactory5", IID_DXGIFactory5Probe, E_FAIL, false},
        {"IDXGIFactory6", IID_DXGIFactory6Probe, E_FAIL, false},
        {"IDXGIFactory7", IID_DXGIFactory7Probe, E_FAIL, false},
    };
    for (auto& probe : interfaces) {
        if (!factory_unknown)
            break;
        void* queried = nullptr;
        probe.hr = factory_unknown->QueryInterface(probe.iid, &queried);
        probe.supported = SUCCEEDED(probe.hr) && queried != nullptr;
    }

    IDXGIAdapter* adapter0 = nullptr;
    IDXGIAdapter1* adapter1 = nullptr;
    IDXGIAdapter1* adapter1_second = nullptr;
    IDXGIAdapter1* gpu_preference_adapter = nullptr;
    IDXGIAdapter1* luid_adapter = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    HRESULT enum_adapter_hr = factory0 ? factory0->EnumAdapters(0, &adapter0) : E_FAIL;
    HRESULT enum_adapter1_hr = factory1 ? factory1->EnumAdapters1(0, &adapter1) : E_FAIL;
    HRESULT enum_adapter1_second_hr = factory1 ? factory1->EnumAdapters1(0, &adapter1_second) : E_FAIL;
    HRESULT adapter3_qi_hr = adapter1 ? adapter1->QueryInterface(IID_PPV_ARGS(&adapter3)) : E_FAIL;
    IDXGIAdapter1* adapter_end = nullptr;
    HRESULT enum_adapter_end_hr = factory1 ? factory1->EnumAdapters1(32, &adapter_end) : E_FAIL;

    IDXGIFactory4* factory4 = nullptr;
    IDXGIFactory6* factory6 = nullptr;
    IDXGIFactory7* factory7 = nullptr;
    if (factory_unknown) {
        factory_unknown->QueryInterface(IID_DXGIFactory4Probe, reinterpret_cast<void**>(&factory4));
        factory_unknown->QueryInterface(IID_DXGIFactory6Probe, reinterpret_cast<void**>(&factory6));
        factory_unknown->QueryInterface(IID_DXGIFactory7Probe, reinterpret_cast<void**>(&factory7));
    }

    HRESULT gpu_preference_hr =
        factory6 ? factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_DXGIAdapter1Probe,
                                                        reinterpret_cast<void**>(&gpu_preference_adapter))
                 : E_NOINTERFACE;

    DXGI_ADAPTER_DESC1 desc = {};
    if (adapter1)
        adapter1->GetDesc1(&desc);
    DXGI_ADAPTER_DESC1 desc_second = {};
    if (adapter1_second)
        adapter1_second->GetDesc1(&desc_second);
    LUID luid = desc.AdapterLuid;
    HRESULT enum_luid_hr =
        factory4 ? factory4->EnumAdapterByLuid(luid, IID_DXGIAdapter1Probe, reinterpret_cast<void**>(&luid_adapter))
                 : E_NOINTERFACE;
    DXGI_ADAPTER_DESC1 desc_luid = {};
    if (luid_adapter)
        luid_adapter->GetDesc1(&desc_luid);
    DXGI_ADAPTER_DESC1 desc_preference = {};
    if (gpu_preference_adapter)
        gpu_preference_adapter->GetDesc1(&desc_preference);

    IDXGIOutput* output = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutput5* output5 = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    IDXGIResource* duplicated_resource = nullptr;
    DXGI_OUTDUPL_DESC duplication_desc = {};
    DXGI_OUTDUPL_FRAME_INFO duplication_frame = {};
    HRESULT enum_output_hr = adapter0 ? adapter0->EnumOutputs(0, &output) : E_FAIL;
    HRESULT output1_qi_hr = output ? output->QueryInterface(IID_PPV_ARGS(&output1)) : E_FAIL;
    HRESULT output5_qi_hr = output ? output->QueryInterface(IID_PPV_ARGS(&output5)) : E_FAIL;
    HRESULT duplicate_output_hr = E_FAIL;
    HRESULT duplicate_desc_hr = E_FAIL;
    HRESULT acquire_frame_hr = E_FAIL;
    HRESULT map_desktop_hr = E_FAIL;
    HRESULT unmap_desktop_hr = E_FAIL;
    UINT desktop_map_pixel = 0xffffffffu;
    UINT dirty_rect_bytes = 0;
    HRESULT dirty_rect_query_hr = E_FAIL;
    RECT dirty_rect = {};
    HRESULT dirty_rect_hr = E_FAIL;
    HRESULT release_frame_hr = E_FAIL;
    HRESULT acquire_frame_again_hr = E_FAIL;
    DXGI_OUTDUPL_FRAME_INFO duplicate_frame_again = {};
    IDXGIResource* duplicated_resource_again = nullptr;

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    CreateDeviceFn create_device = nullptr;
    FARPROC create_device_proc =
        d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    static_assert(sizeof(create_device) == sizeof(create_device_proc));
    std::memcpy(&create_device, &create_device_proc, sizeof(create_device));
    ID3D12Device* d3d12_device = nullptr;
    IDXGIDevice* dxgi_device = nullptr;
    IDXGISurface* display_source = nullptr;
    IDXGISurface1* display_source1 = nullptr;
    IDXGISurface* display_copy = nullptr;
    IDXGISwapChain1* composition_swapchain = nullptr;
    IDXGIResource* composition_buffer = nullptr;
    DXGI_SWAP_CHAIN_DESC1 composition_desc = {};
    DXGI_SWAP_CHAIN_DESC1 composition_desc_copy = {};
    HRESULT create_composition_hr = E_FAIL;
    HRESULT composition_desc_hr = E_FAIL;
    HRESULT composition_buffer_hr = E_FAIL;
    DXGI_SURFACE_DESC display_surface_desc = {4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}};
    HRESULT create_d3d12_hr = create_device
                                  ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                                  IID_PPV_ARGS(&d3d12_device))
                                  : E_FAIL;
    HRESULT query_dxgi_device_hr = d3d12_device
                                       ? d3d12_device->QueryInterface(
                                             IID_PPV_ARGS(&dxgi_device))
                                       : E_FAIL;
    composition_desc.Width = 8;
    composition_desc.Height = 8;
    composition_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    composition_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    composition_desc.BufferCount = 2;
    composition_desc.SampleDesc.Count = 1;
    composition_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    create_composition_hr = factory2 && d3d12_device
                                ? factory2->CreateSwapChainForComposition(
                                      d3d12_device, &composition_desc, nullptr,
                                      &composition_swapchain)
                                : E_NOINTERFACE;
    if (composition_swapchain) {
        composition_desc_hr = composition_swapchain->GetDesc1(
            &composition_desc_copy);
        composition_buffer_hr = composition_swapchain->GetBuffer(
            0, __uuidof(IDXGIResource),
            reinterpret_cast<void**>(&composition_buffer));
    }
    duplicate_output_hr = output1 && d3d12_device
                              ? output1->DuplicateOutput(d3d12_device,
                                                         &duplication)
                              : E_NOINTERFACE;
    if (duplication) {
        duplication->GetDesc(&duplication_desc);
        duplicate_desc_hr = S_OK;
        acquire_frame_hr = duplication->AcquireNextFrame(
            0, &duplication_frame, &duplicated_resource);
        if (SUCCEEDED(acquire_frame_hr)) {
            DXGI_MAPPED_RECT desktop_map = {};
            map_desktop_hr = duplication->MapDesktopSurface(&desktop_map);
            if (SUCCEEDED(map_desktop_hr) && desktop_map.pBits)
                std::memcpy(&desktop_map_pixel, desktop_map.pBits,
                            sizeof(desktop_map_pixel));
            if (SUCCEEDED(map_desktop_hr))
                unmap_desktop_hr = duplication->UnMapDesktopSurface();
        }
        dirty_rect_query_hr = duplication->GetFrameDirtyRects(
            0, nullptr, &dirty_rect_bytes);
        if (dirty_rect_bytes <= sizeof(dirty_rect))
            dirty_rect_hr = duplication->GetFrameDirtyRects(
                sizeof(dirty_rect), &dirty_rect, &dirty_rect_bytes);
        if (SUCCEEDED(acquire_frame_hr))
            release_frame_hr = duplication->ReleaseFrame();
        if (SUCCEEDED(release_frame_hr))
            acquire_frame_again_hr = duplication->AcquireNextFrame(
                0, &duplicate_frame_again, &duplicated_resource_again);
        if (duplicated_resource_again)
            duplicated_resource_again->Release();
    }
    HRESULT create_surface_hr = dxgi_device
                                    ? dxgi_device->CreateSurface(
                                          &display_surface_desc, 1,
                                          DXGI_USAGE_SHADER_INPUT, nullptr,
                                          &display_source)
                                    : E_FAIL;
    HRESULT create_copy_surface_hr = dxgi_device
                                         ? dxgi_device->CreateSurface(
                                               &display_surface_desc, 1,
                                               DXGI_USAGE_SHADER_INPUT, nullptr,
                                               &display_copy)
                                         : E_FAIL;
    HRESULT source_map_hr = E_FAIL;
    HRESULT source_unmap_hr = E_FAIL;
    HRESULT surface_get_dc_hr = E_FAIL;
    HRESULT surface_release_dc_hr = E_FAIL;
    HDC surface_dc = nullptr;
    if (display_source) {
        DXGI_MAPPED_RECT mapped = {};
        source_map_hr = display_source->Map(&mapped, DXGI_MAP_WRITE | DXGI_MAP_DISCARD);
        if (SUCCEEDED(source_map_hr) && mapped.pBits) {
            for (UINT y = 0; y < display_surface_desc.Height; ++y)
                for (UINT x = 0; x < display_surface_desc.Width; ++x) {
                    auto* pixel = mapped.pBits + y * mapped.Pitch + x * 4;
                    pixel[0] = 0x11;
                    pixel[1] = 0x22;
                    pixel[2] = 0x33;
                    pixel[3] = 0x44;
                }
            source_unmap_hr = display_source->Unmap();
        }
        if (SUCCEEDED(source_unmap_hr)) {
            HRESULT source1_qi_hr = display_source->QueryInterface(
                __uuidof(IDXGISurface1),
                reinterpret_cast<void**>(&display_source1));
            if (SUCCEEDED(source1_qi_hr))
                surface_get_dc_hr = display_source1->GetDC(FALSE, &surface_dc);
        }
        if (SUCCEEDED(surface_get_dc_hr))
            surface_release_dc_hr = display_source1->ReleaseDC(nullptr);
    }
    HRESULT take_ownership_hr = output && d3d12_device
                                     ? output->TakeOwnership(d3d12_device, TRUE)
                                     : E_FAIL;
    HRESULT set_display_surface_hr = output && display_source
                                          ? output->SetDisplaySurface(display_source)
                                          : E_FAIL;
    HRESULT get_display_surface_data_hr = output && display_copy
                                              ? output->GetDisplaySurfaceData(display_copy)
                                              : E_FAIL;
    DXGI_MAPPED_RECT copy_map = {};
    HRESULT copy_map_hr = display_copy ? display_copy->Map(&copy_map, DXGI_MAP_READ) : E_FAIL;
    uint32_t display_copy_pixel = 0;
    if (SUCCEEDED(copy_map_hr) && copy_map.pBits) {
        std::memcpy(&display_copy_pixel, copy_map.pBits, sizeof(display_copy_pixel));
        display_copy->Unmap();
    }
    DXGI_FRAME_STATISTICS output_stats = {};
    HRESULT output_stats_hr = output ? output->GetFrameStatistics(&output_stats) : E_FAIL;
    HRESULT wait_vblank_hr = output ? output->WaitForVBlank() : E_FAIL;
    if (output)
        output->ReleaseOwnership();
    IDXGISurface2* subresource_surface = nullptr;
    HRESULT subresource_surface_hr = E_FAIL;
    if (d3d12_device) {
        D3D12_HEAP_PROPERTIES default_heap = {};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        default_heap.CreationNodeMask = 1;
        default_heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Width = 4;
        resource_desc.Height = 4;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels = 1;
        resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource* subresource_resource = nullptr;
        if (SUCCEEDED(d3d12_device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&subresource_resource)))) {
            IDXGIResource1* dxgi_resource = nullptr;
            if (SUCCEEDED(subresource_resource->QueryInterface(
                    IID_PPV_ARGS(&dxgi_resource)))) {
                subresource_surface_hr = dxgi_resource->CreateSubresourceSurface(
                    0, &subresource_surface);
                dxgi_resource->Release();
            }
            subresource_resource->Release();
        }
    }
    void* unknown_factory = nullptr;
    HRESULT unknown_qi_hr =
        factory_unknown ? factory_unknown->QueryInterface(IID_DXGIUnknownProbe, &unknown_factory) : E_FAIL;
    DWORD adapters_changed_cookie = 0;
    HANDLE adapters_changed_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HRESULT register_adapters_changed_hr =
        factory7 && adapters_changed_event
            ? factory7->RegisterAdaptersChangedEvent(adapters_changed_event, &adapters_changed_cookie)
            : E_NOINTERFACE;
    DWORD adapters_changed_wait = adapters_changed_event ? WaitForSingleObject(adapters_changed_event, 0) : WAIT_FAILED;
    HRESULT unregister_adapters_changed_hr = factory7 && adapters_changed_cookie != 0
                                                 ? factory7->UnregisterAdaptersChangedEvent(adapters_changed_cookie)
                                                 : E_NOINTERFACE;
    HRESULT unregister_unknown_adapter_cookie_hr =
        factory7 ? factory7->UnregisterAdaptersChangedEvent(adapters_changed_cookie + 0x10000u) : E_NOINTERFACE;
    HANDLE budget_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    DWORD budget_cookie = 0;
    HRESULT register_budget_hr =
        adapter3 && budget_event
            ? adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(budget_event, &budget_cookie)
            : E_NOINTERFACE;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory_info = {};
    HRESULT query_budget_hr =
        adapter3 ? adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory_info) : E_NOINTERFACE;
    DWORD budget_event_wait = budget_event ? WaitForSingleObject(budget_event, 0) : WAIT_FAILED;
    if (adapter3 && budget_cookie)
        adapter3->UnregisterVideoMemoryBudgetChangeNotification(budget_cookie);
    DWORD protection_cookie = 0xffffffffu;
    HRESULT register_protection_hr =
        adapter3 && budget_event
            ? adapter3->RegisterHardwareContentProtectionTeardownStatusEvent(budget_event, &protection_cookie)
            : E_NOINTERFACE;

    bool factory_versions_supported = true;
    for (const auto& probe : interfaces)
        factory_versions_supported = factory_versions_supported && probe.supported;
    bool adapter_stable = SUCCEEDED(enum_adapter1_hr) && SUCCEEDED(enum_adapter1_second_hr) &&
                          SUCCEEDED(gpu_preference_hr) && SUCCEEDED(enum_luid_hr) &&
                          desc.AdapterLuid.HighPart == desc_second.AdapterLuid.HighPart &&
                          desc.AdapterLuid.LowPart == desc_second.AdapterLuid.LowPart &&
                          desc.AdapterLuid.HighPart == desc_preference.AdapterLuid.HighPart &&
                          desc.AdapterLuid.LowPart == desc_preference.AdapterLuid.LowPart &&
                          desc.AdapterLuid.HighPart == desc_luid.AdapterLuid.HighPart &&
                          desc.AdapterLuid.LowPart == desc_luid.AdapterLuid.LowPart;

    bool pass =
        dxgi && create_factory && create_factory1 && create_factory2 && SUCCEEDED(create_hr) && SUCCEEDED(create1_hr) &&
        SUCCEEDED(create2_hr) && SUCCEEDED(enum_adapter_hr) && SUCCEEDED(enum_adapter1_hr) && desc.VendorId != 0 &&
        desc.DedicatedVideoMemory + desc.SharedSystemMemory > 0 && unknown_qi_hr == E_NOINTERFACE &&
        factory_versions_supported && adapter_stable && enum_adapter_end_hr == DXGI_ERROR_NOT_FOUND &&
        SUCCEEDED(register_adapters_changed_hr) && adapters_changed_cookie != 0 &&
        adapters_changed_wait == WAIT_TIMEOUT && SUCCEEDED(unregister_adapters_changed_hr) &&
        unregister_unknown_adapter_cookie_hr == DXGI_ERROR_INVALID_CALL && SUCCEEDED(adapter3_qi_hr) &&
        SUCCEEDED(register_budget_hr) && budget_cookie != 0 && SUCCEEDED(query_budget_hr) && memory_info.Budget > 0 &&
        budget_event_wait == WAIT_TIMEOUT && register_protection_hr == DXGI_ERROR_UNSUPPORTED && protection_cookie == 0 &&
        SUCCEEDED(create_d3d12_hr) && SUCCEEDED(query_dxgi_device_hr) &&
        SUCCEEDED(create_composition_hr) && SUCCEEDED(composition_desc_hr) &&
        SUCCEEDED(composition_buffer_hr) && composition_buffer != nullptr &&
        composition_desc_copy.Width == composition_desc.Width &&
        composition_desc_copy.Height == composition_desc.Height &&
        SUCCEEDED(create_surface_hr) &&
        SUCCEEDED(create_copy_surface_hr) && SUCCEEDED(source_map_hr) && SUCCEEDED(source_unmap_hr) &&
        SUCCEEDED(surface_get_dc_hr) && surface_dc != nullptr &&
        SUCCEEDED(surface_release_dc_hr) &&
        SUCCEEDED(map_desktop_hr) && SUCCEEDED(unmap_desktop_hr) &&
        acquire_frame_again_hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT) &&
        SUCCEEDED(take_ownership_hr) &&
        SUCCEEDED(set_display_surface_hr) &&
        SUCCEEDED(get_display_surface_data_hr) && SUCCEEDED(copy_map_hr) && display_copy_pixel == 0x44332211u &&
        SUCCEEDED(output_stats_hr) && SUCCEEDED(subresource_surface_hr) && subresource_surface != nullptr;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-dxgi-factory.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"dxgi_loaded\": %s,\n", dxgi ? "true" : "false");
    std::printf("    \"CreateDXGIFactory\": %s,\n", create_factory ? "true" : "false");
    std::printf("    \"CreateDXGIFactory1\": %s,\n", create_factory1 ? "true" : "false");
    std::printf("    \"CreateDXGIFactory2\": %s\n", create_factory2 ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"factory_creation\": {\n");
    print_hr("CreateDXGIFactory", create_hr);
    print_hr("CreateDXGIFactory1", create1_hr);
    print_hr("CreateDXGIFactory2", create2_hr, false);
    std::printf("  },\n");
    std::printf("  \"factory_interfaces\": {\n");
    for (size_t i = 0; i < interfaces.size(); ++i)
        print_interface_json(interfaces[i], i + 1 == interfaces.size());
    std::printf("  },\n");
    std::printf("  \"adapter_enumeration\": {\n");
    print_hr("EnumAdapters", enum_adapter_hr);
    print_hr("EnumAdapters1", enum_adapter1_hr);
    print_hr("EnumAdapters1_repeat", enum_adapter1_second_hr);
    print_hr("EnumAdapters1_end", enum_adapter_end_hr);
    print_hr("EnumAdapterByGpuPreference", gpu_preference_hr);
    print_hr("EnumAdapterByLuid", enum_luid_hr);
    print_hr("EnumOutputs", enum_output_hr);
    print_hr("QueryInterface_IDXGIOutput1", output1_qi_hr);
    print_hr("QueryInterface_IDXGIOutput5", output5_qi_hr);
    print_hr("DuplicateOutput", duplicate_output_hr);
    print_hr("DuplicateOutput_GetDesc", duplicate_desc_hr);
    print_hr("DuplicateOutput_AcquireNextFrame", acquire_frame_hr);
    print_hr("DuplicateOutput_GetFrameDirtyRects_query", dirty_rect_query_hr);
    print_hr("DuplicateOutput_GetFrameDirtyRects", dirty_rect_hr);
    print_hr("DuplicateOutput_ReleaseFrame", release_frame_hr);
    print_hr("DuplicateOutput_AcquireNextFrame_again", acquire_frame_again_hr);
    print_hr("DuplicateOutput_MapDesktopSurface", map_desktop_hr);
    print_hr("DuplicateOutput_UnMapDesktopSurface", unmap_desktop_hr);
    std::printf("    \"duplicate_desktop_map_pixel\": \"0x%08x\",\n",
                desktop_map_pixel);
    std::printf("    \"factory_versions_supported\": %s,\n", factory_versions_supported ? "true" : "false");
    std::printf("    \"adapter_stable\": %s,\n", adapter_stable ? "true" : "false");
    std::printf("    \"description\": \"%s\",\n", json_escape(wide_to_utf8(desc.Description)).c_str());
    std::printf("    \"vendor_id\": %u,\n", desc.VendorId);
    std::printf("    \"device_id\": %u,\n", desc.DeviceId);
    std::printf("    \"dedicated_video_memory\": %llu,\n", static_cast<unsigned long long>(desc.DedicatedVideoMemory));
    std::printf("    \"shared_system_memory\": %llu,\n", static_cast<unsigned long long>(desc.SharedSystemMemory));
    std::printf("    \"flags\": %u\n", desc.Flags);
    std::printf("  },\n");
    std::printf("  \"edge_cases\": {\n");
    print_hr("unknown_factory_qi", unknown_qi_hr);
    print_hr("RegisterAdaptersChangedEvent", register_adapters_changed_hr);
    print_hr("UnregisterAdaptersChangedEvent", unregister_adapters_changed_hr);
    print_hr("UnregisterAdaptersChangedEvent_unknown_cookie", unregister_unknown_adapter_cookie_hr);
    print_hr("QueryInterface_IDXGIAdapter3", adapter3_qi_hr);
    print_hr("RegisterVideoMemoryBudgetChangeNotificationEvent", register_budget_hr);
    print_hr("QueryVideoMemoryInfo", query_budget_hr);
    print_hr("RegisterHardwareContentProtectionTeardownStatusEvent", register_protection_hr);
    print_hr("CreateD3D12Device", create_d3d12_hr);
    print_hr("QueryInterface_IDXGIDevice", query_dxgi_device_hr);
    print_hr("CreateSwapChainForComposition", create_composition_hr);
    print_hr("Composition_GetDesc1", composition_desc_hr);
    print_hr("Composition_GetBuffer", composition_buffer_hr);
    print_hr("CreateSurface", create_surface_hr);
    print_hr("CreateSurface_copy", create_copy_surface_hr);
    print_hr("Surface_Map_write_discard", source_map_hr);
    print_hr("Surface_Unmap_write", source_unmap_hr);
    print_hr("Surface_GetDC", surface_get_dc_hr);
    print_hr("Surface_ReleaseDC", surface_release_dc_hr);
    print_hr("TakeOwnership", take_ownership_hr);
    print_hr("SetDisplaySurface", set_display_surface_hr);
    print_hr("GetDisplaySurfaceData", get_display_surface_data_hr);
    print_hr("Surface_Map_read_copy", copy_map_hr);
    print_hr("GetFrameStatistics", output_stats_hr);
    print_hr("WaitForVBlank", wait_vblank_hr);
    print_hr("CreateSubresourceSurface", subresource_surface_hr);
    std::printf("    \"display_copy_pixel\": \"0x%08x\",\n", display_copy_pixel);
    std::printf("    \"frame_present_count\": %u,\n", output_stats.PresentCount);
    std::printf("    \"video_memory_budget\": %llu,\n", static_cast<unsigned long long>(memory_info.Budget));
    std::printf("    \"budget_cookie_nonzero\": %s,\n", budget_cookie ? "true" : "false");
    std::printf("    \"budget_event_initially_unsignaled\": %s,\n",
                budget_event_wait == WAIT_TIMEOUT ? "true" : "false");
    std::printf("    \"protection_cookie_zero_on_rejection\": %s,\n", protection_cookie == 0 ? "true" : "false");
    std::printf("    \"adapters_changed_cookie_nonzero\": %s,\n", adapters_changed_cookie ? "true" : "false");
    std::printf("    \"adapters_changed_event_initially_unsignaled\": %s,\n",
                adapters_changed_wait == WAIT_TIMEOUT ? "true" : "false");
    std::printf("    \"register_adapters_changed_decision\": \"%s\"\n",
                SUCCEEDED(register_adapters_changed_hr) ? "safe_success_observed" : "safe_rejection_observed");
    std::printf("  }\n");
    std::printf("}\n");

    if (duplicated_resource)
        duplicated_resource->Release();
    if (duplication)
        duplication->Release();
    if (composition_buffer)
        composition_buffer->Release();
    if (composition_swapchain)
        composition_swapchain->Release();
    if (subresource_surface)
        subresource_surface->Release();
    if (display_copy)
        display_copy->Release();
    if (display_source1)
        display_source1->Release();
    if (display_source)
        display_source->Release();
    if (dxgi_device)
        dxgi_device->Release();
    if (d3d12_device)
        d3d12_device->Release();
    if (d3d12)
        FreeLibrary(d3d12);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
