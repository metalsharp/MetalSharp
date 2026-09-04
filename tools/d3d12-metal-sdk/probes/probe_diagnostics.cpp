#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static constexpr GUID kIIDDeviceStatistics = {
    0x3d5ca1a8, 0xa39e, 0x4619,
    {0x95, 0xe0, 0xf9, 0xb0, 0xa4, 0x03, 0x40, 0xf5}};
static constexpr GUID kIIDDeviceTools1 = {
    0xe30e9fc7, 0xe641, 0x4d6e,
    {0x8a, 0x81, 0x9d, 0xd9, 0x20, 0x6e, 0xc4, 0x7a}};
static constexpr GUID kIIDDeviceTools = {
    0x2ea68e9c, 0x19c3, 0x4e47,
    {0xa1, 0x09, 0x6c, 0xda, 0xdf, 0xf0, 0xac, 0xa9}};
static constexpr GUID kCLSIDDsrFactory = {
    0xbb6dd27e, 0x94a9, 0x41a6,
    {0x9f, 0x1b, 0x13, 0x37, 0x72, 0x17, 0x24, 0x28}};
static constexpr GUID kIIDDsrFactory = {
    0xf343d1a0, 0xafe3, 0x439f,
    {0xb1, 0x3d, 0xcd, 0x87, 0xa4, 0x3b, 0x70, 0xca}};
static constexpr GUID kCLSIDDred = {
    0x4a75bbc4, 0x9ff4, 0x4ad8,
    {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};
static constexpr GUID kIIDDredSettings2 = {
    0x61552388, 0x01ab, 0x4008,
    {0xa4, 0x36, 0x83, 0xdb, 0x18, 0x95, 0x66, 0xea}};
static constexpr GUID kUnknown = {
    0x2f9d3b4a, 0x62c1, 0x4e9a,
    {0x84, 0x31, 0x10, 0x21, 0x54, 0x87, 0xa9, 0x65}};

enum DredEnablement : UINT { DredDefault = 0, DredEnable = 1, DredDisable = 2 };
enum BlobStatus : UINT {
    BlobUnknown = 1,
    BlobUsed = 2,
    BlobIgnored = 3,
    BlobNotSpecified = 4,
};
struct StateStatistics {
    BOOL DefaultPSDBRegistered;
    struct {
        UINT NumCreated;
        UINT NumPSDBCacheMissed;
        UINT NumTotalCacheMissed;
        UINT NumCacheUnknown;
    } PipelineStateObjectStatistics;
    struct {
        UINT NumCreated;
        UINT NumPSDBCacheMissed;
        UINT NumTotalCacheMissed;
        UINT NumCacheUnknown;
    } StateObjectStatistics;
};
struct DeviceStatistics : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetStateObjectStatistics(StateStatistics*) = 0;
};
struct DeviceTools : IUnknown {
    virtual void STDMETHODCALLTYPE SetNextAllocationAddress(
        D3D12_GPU_VIRTUAL_ADDRESS) = 0;
};
struct DeviceTools1 : DeviceTools {
    virtual HRESULT STDMETHODCALLTYPE GetApplicationSpecificDriverState(
        ID3DBlob**) = 0;
    virtual BlobStatus STDMETHODCALLTYPE GetApplicationSpecificDriverBlobStatus() = 0;
};
struct DsrFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDSRDevice(ID3D12Device*, UINT,
                                                       REFIID, void**) = 0;
};
struct DredSettings : IUnknown {
    virtual void STDMETHODCALLTYPE SetAutoBreadcrumbsEnablement(DredEnablement) = 0;
    virtual void STDMETHODCALLTYPE SetPageFaultEnablement(DredEnablement) = 0;
    virtual void STDMETHODCALLTYPE SetWatsonDumpEnablement(DredEnablement) = 0;
    virtual void STDMETHODCALLTYPE SetBreadcrumbContextEnablement(DredEnablement) = 0;
    virtual void STDMETHODCALLTYPE UseMarkersOnlyAutoBreadcrumbs(BOOL) = 0;
};
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using GetInterfaceFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);

template <typename T> static void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
template <typename T> static T load_proc(HMODULE module, const char* name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static unsigned long hr_value(HRESULT hr) {
    return static_cast<unsigned long>(static_cast<uint32_t>(hr));
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    auto get_interface = load_proc<GetInterfaceFn>(module, "D3D12GetInterface");
    ID3D12Device* device = nullptr;
    DeviceStatistics* statistics = nullptr;
    DeviceTools1* tools = nullptr;
    DsrFactory* dsr_factory = nullptr;
    DredSettings* dred = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT stats_qi_hr = device ? device->QueryInterface(
                                       kIIDDeviceStatistics,
                                       reinterpret_cast<void**>(&statistics))
                                 : create_hr;
    StateStatistics state_statistics = {};
    HRESULT stats_hr = statistics
                           ? statistics->GetStateObjectStatistics(&state_statistics)
                           : stats_qi_hr;
    HRESULT tools_qi_hr = device ? device->QueryInterface(
                                       kIIDDeviceTools1,
                                       reinterpret_cast<void**>(&tools))
                                 : create_hr;
    HRESULT tools_blob_hr = E_FAIL;
    ID3DBlob* tools_blob = nullptr;
    BlobStatus blob_status = BlobUnknown;
    if (tools) {
        tools->SetNextAllocationAddress(0x100000000ull);
        tools_blob_hr = tools->GetApplicationSpecificDriverState(&tools_blob);
        blob_status = tools->GetApplicationSpecificDriverBlobStatus();
    }
    const bool tools_blob_ok = tools_blob_hr == S_OK && tools_blob &&
                               tools_blob->GetBufferSize() == sizeof(uint64_t);

    HRESULT dsr_qi_hr = get_interface
                            ? get_interface(kCLSIDDsrFactory, kIIDDsrFactory,
                                           reinterpret_cast<void**>(&dsr_factory))
                            : E_FAIL;
    void* dsr_device = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    HRESULT dsr_create_hr = dsr_factory
                                ? dsr_factory->CreateDSRDevice(
                                      device, 1, kUnknown, &dsr_device)
                                : dsr_qi_hr;
    const bool dsr_null_on_rejection = dsr_create_hr == E_NOINTERFACE &&
                                       dsr_device == nullptr;

    HRESULT dred_qi_hr = get_interface
                             ? get_interface(kCLSIDDred, kIIDDredSettings2,
                                             reinterpret_cast<void**>(&dred))
                             : E_FAIL;
    if (dred) {
        dred->SetAutoBreadcrumbsEnablement(DredEnable);
        dred->SetPageFaultEnablement(DredEnable);
        dred->SetWatsonDumpEnablement(DredDisable);
        dred->SetBreadcrumbContextEnablement(DredEnable);
        dred->UseMarkersOnlyAutoBreadcrumbs(TRUE);
    }
    const bool passed = create_hr == S_OK && stats_qi_hr == S_OK &&
                        stats_hr == S_OK && tools_qi_hr == S_OK &&
                        tools_blob_ok && blob_status == BlobNotSpecified &&
                        dsr_qi_hr == S_OK && dsr_null_on_rejection &&
                        dred_qi_hr == S_OK && dred;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.diagnostics.v1\",\n");
    std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
    std::printf("  \"create_device\": \"0x%08lx\",\n", hr_value(create_hr));
    std::printf("  \"statistics_qi\": \"0x%08lx\",\n", hr_value(stats_qi_hr));
    std::printf("  \"statistics\": \"0x%08lx\",\n", hr_value(stats_hr));
    std::printf("  \"state_object_count\": %u,\n",
                state_statistics.StateObjectStatistics.NumCreated);
    std::printf("  \"tools_qi\": \"0x%08lx\",\n", hr_value(tools_qi_hr));
    std::printf("  \"tools_blob\": \"0x%08lx\",\n", hr_value(tools_blob_hr));
    std::printf("  \"tools_blob_size\": %zu,\n",
                tools_blob ? tools_blob->GetBufferSize() : 0u);
    std::printf("  \"tools_blob_status\": %u,\n", static_cast<unsigned>(blob_status));
    std::printf("  \"dsr_factory_qi\": \"0x%08lx\",\n", hr_value(dsr_qi_hr));
    std::printf("  \"dsr_device_rejection\": \"0x%08lx\",\n",
                hr_value(dsr_create_hr));
    std::printf("  \"dsr_null_on_rejection\": %s,\n",
                dsr_null_on_rejection ? "true" : "false");
    std::printf("  \"dred_settings_qi\": \"0x%08lx\",\n", hr_value(dred_qi_hr));
    std::printf("  \"settings_state_mutated\": %s\n", dred ? "true" : "false");
    std::printf("}\n");

    release(dred);
    release(dsr_factory);
    release(tools_blob);
    release(tools);
    release(statistics);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
