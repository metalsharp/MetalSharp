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

static constexpr GUID kIIDProtectedResourceSession = {
    0x6cd696f4, 0xf289, 0x40cc,
    {0x80, 0x91, 0x5a, 0x6c, 0x0a, 0x09, 0x9c, 0x3d}};
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
static constexpr GUID kIIDDredData = {
    0x98931d33, 0x5ae8, 0x4791,
    {0xaa, 0x3c, 0x1a, 0x73, 0xa2, 0x93, 0x4e, 0x71}};
static constexpr GUID kIIDDredData1 = {
    0x9727a022, 0xcf1d, 0x4dda,
    {0x9e, 0xba, 0xef, 0xfa, 0x65, 0x3f, 0xc5, 0x06}};
static constexpr GUID kIIDDredData2 = {
    0x67fc5816, 0xe4ca, 0x4915,
    {0xbf, 0x18, 0x42, 0x54, 0x12, 0x72, 0xda, 0x54}};
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
struct DredAutoOutput {
    const void *head;
};
struct DredPageFaultOutput {
    UINT64 page_fault_va;
    const void *existing;
    const void *recently_freed;
};
struct DredPageFaultOutput2 : DredPageFaultOutput {
    UINT flags;
    UINT reserved;
};
struct DredData : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput(void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput(void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAutoBreadcrumbsOutput1(void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput1(void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageFaultAllocationOutput2(void *) = 0;
    virtual UINT STDMETHODCALLTYPE GetDeviceState() = 0;
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
    ID3D12Resource* resource = nullptr;
    ID3D12Resource1* resource1 = nullptr;
    ID3D12Resource2* resource2 = nullptr;
    DeviceStatistics* statistics = nullptr;
    DeviceTools1* tools = nullptr;
    DsrFactory* dsr_factory = nullptr;
    DredSettings* dred = nullptr;
    DredData* dred_data = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload_heap.CreationNodeMask = 1;
    upload_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = 256;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT resource_hr = device
                              ? device->CreateCommittedResource(
                                    &upload_heap, D3D12_HEAP_FLAG_NONE,
                                    &resource_desc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&resource))
                              : create_hr;
    HRESULT resource1_qi_hr = resource
                                  ? resource->QueryInterface(
                                        __uuidof(ID3D12Resource1),
                                        reinterpret_cast<void**>(&resource1))
                                  : resource_hr;
    void* protected_from_resource = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    HRESULT resource1_protected_hr = resource1
                                         ? resource1->GetProtectedResourceSession(
                                               kIIDProtectedResourceSession,
                                               &protected_from_resource)
                                         : resource1_qi_hr;
    const bool resource1_boundary_ok =
        resource1_qi_hr == S_OK && resource1_protected_hr == E_NOINTERFACE &&
        protected_from_resource == nullptr;
    HRESULT resource2_qi_hr = resource
                                  ? resource->QueryInterface(
                                        __uuidof(ID3D12Resource2),
                                        reinterpret_cast<void**>(&resource2))
                                  : resource_hr;
    D3D12_RESOURCE_DESC1 resource2_desc = {};
    bool resource2_desc_ok = false;
    if (resource2_qi_hr == S_OK && resource2) {
        resource2_desc = resource2->GetDesc1();
        resource2_desc_ok = resource2_desc.Width == resource_desc.Width &&
                            resource2_desc.Dimension == resource_desc.Dimension;
    }
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
    HRESULT dred_data_hr = get_interface
                               ? get_interface(kCLSIDDred, kIIDDredData,
                                              reinterpret_cast<void **>(&dred_data))
                               : E_FAIL;
    DredAutoOutput auto_output = {reinterpret_cast<const void *>(1)};
    DredPageFaultOutput page_output = {1, reinterpret_cast<const void *>(1),
                                       reinterpret_cast<const void *>(1)};
    DredAutoOutput auto_output1 = {reinterpret_cast<const void *>(1)};
    DredPageFaultOutput2 page_output2 = {};
    HRESULT dred_auto_hr = dred_data
                               ? dred_data->GetAutoBreadcrumbsOutput(&auto_output)
                               : E_FAIL;
    HRESULT dred_page_hr = dred_data
                               ? dred_data->GetPageFaultAllocationOutput(&page_output)
                               : E_FAIL;
    HRESULT dred_auto1_hr = dred_data
                                ? dred_data->GetAutoBreadcrumbsOutput1(&auto_output1)
                                : E_FAIL;
    HRESULT dred_page2_hr = dred_data
                                ? dred_data->GetPageFaultAllocationOutput2(&page_output2)
                                : E_FAIL;
    const bool dred_data_ok = dred_data_hr == S_OK && dred_auto_hr == S_OK &&
                              dred_page_hr == S_OK && dred_auto1_hr == S_OK &&
                              dred_page2_hr == S_OK && auto_output.head == nullptr &&
                              page_output.page_fault_va == 0 &&
                              page_output.existing == nullptr &&
                              page_output.recently_freed == nullptr &&
                              auto_output1.head == nullptr &&
                              page_output2.page_fault_va == 0 &&
                              page_output2.existing == nullptr &&
                              page_output2.recently_freed == nullptr &&
                              dred_data->GetDeviceState() == 0;
    DredData* dred_data1 = nullptr;
    DredData* dred_data2 = nullptr;
    HRESULT dred_data1_hr = get_interface
                                ? get_interface(kCLSIDDred, kIIDDredData1,
                                               reinterpret_cast<void **>(&dred_data1))
                                : E_FAIL;
    HRESULT dred_data2_hr = get_interface
                                ? get_interface(kCLSIDDred, kIIDDredData2,
                                               reinterpret_cast<void **>(&dred_data2))
                                : E_FAIL;
    const bool passed = create_hr == S_OK && resource_hr == S_OK &&
                        resource1_boundary_ok && resource2_qi_hr == S_OK &&
                        resource2_desc_ok && stats_qi_hr == S_OK &&
                        stats_hr == S_OK && tools_qi_hr == S_OK &&
                        tools_blob_ok && blob_status == BlobNotSpecified &&
                        dsr_qi_hr == S_OK && dsr_null_on_rejection &&
                        dred_qi_hr == S_OK && dred && dred_data_ok &&
                        dred_data1_hr == S_OK && dred_data2_hr == S_OK;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.diagnostics.v1\",\n");
    std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
    std::printf("  \"create_device\": \"0x%08lx\",\n", hr_value(create_hr));
    std::printf("  \"resource\": \"0x%08lx\",\n", hr_value(resource_hr));
    std::printf("  \"resource1_qi\": \"0x%08lx\",\n", hr_value(resource1_qi_hr));
    std::printf("  \"resource1_protected\": \"0x%08lx\",\n",
                hr_value(resource1_protected_hr));
    std::printf("  \"resource1_boundary_ok\": %s,\n",
                resource1_boundary_ok ? "true" : "false");
    std::printf("  \"resource2_qi\": \"0x%08lx\",\n", hr_value(resource2_qi_hr));
    std::printf("  \"resource2_desc_ok\": %s,\n",
                resource2_desc_ok ? "true" : "false");
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
    std::printf("  \"settings_state_mutated\": %s,\n", dred ? "true" : "false");
    std::printf("  \"dred_data\": \"0x%08lx\",\n", hr_value(dred_data_hr));
    std::printf("  \"dred_data_ok\": %s,\n", dred_data_ok ? "true" : "false");
    std::printf("  \"dred_data1\": \"0x%08lx\",\n", hr_value(dred_data1_hr));
    std::printf("  \"dred_data2\": \"0x%08lx\"\n", hr_value(dred_data2_hr));
    std::printf("}\n");

    release(dred_data2);
    release(dred_data1);
    release(dred_data);
    release(dred);
    release(resource1);
    release(resource);
    release(dsr_factory);
    release(tools_blob);
    release(tools);
    release(statistics);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
