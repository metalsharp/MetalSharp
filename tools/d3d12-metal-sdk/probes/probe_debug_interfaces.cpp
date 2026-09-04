#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static constexpr GUID kIIDDebug = {0x344488b7, 0x6846, 0x474b,
                                   {0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0}};
static constexpr GUID kIIDDebug1 = {0xaffaa4ca, 0x63fe, 0x4d8e,
                                    {0xb8, 0xad, 0x15, 0x90, 0x00, 0xaf, 0x43, 0x04}};
static constexpr GUID kIIDDebug2 = {0x93a665c4, 0xa3b2, 0x4e5d,
                                    {0xb6, 0x92, 0xa2, 0x6a, 0xe1, 0x4e, 0x33, 0x74}};
static constexpr GUID kIIDDebug3 = {0x5cf4e58f, 0xf671, 0x4ff1,
                                    {0xa5, 0x42, 0x36, 0x86, 0xe3, 0xd1, 0x53, 0xd1}};
static constexpr GUID kIIDDebug4 = {0x014b816e, 0x9ec5, 0x4a2f,
                                    {0xa8, 0x45, 0xff, 0xbe, 0x44, 0x1c, 0xe1, 0x3a}};
static constexpr GUID kIIDDebug5 = {0x548d6b12, 0x09fa, 0x40e0,
                                    {0x90, 0x69, 0x5d, 0xcd, 0x58, 0x9a, 0x52, 0xc9}};
static constexpr GUID kIIDDebug6 = {0x82a816d6, 0x5d01, 0x4157,
                                    {0x97, 0xd0, 0x49, 0x75, 0x46, 0x3f, 0xd1, 0xed}};
static constexpr GUID kIIDDebugState = {0x3b80b40e, 0xd3b1, 0x4a27,
                                        {0x91, 0x3d, 0x9a, 0xf5, 0x9d, 0x74, 0xa8, 0x22}};
static constexpr GUID kIIDDebugDevice2 = {0x60eccbc1, 0x378d, 0x4df1,
                                          {0x89, 0x4c, 0xf8, 0xac, 0x5c, 0xe4, 0xd7, 0xdd}};
static constexpr GUID kIIDDebugCommandQueue1 = {0x16be35a2, 0xbfd6, 0x49f2,
                                                {0xbc, 0xae, 0xea, 0xae, 0x4a, 0xff, 0x86, 0x2d}};
static constexpr GUID kIIDDebugCommandQueueState = {0x2cf5d0a1, 0x7f2e, 0x43e5,
                                                    {0x9c, 0xc0, 0x6f, 0x54, 0x27, 0x89, 0xab, 0x90}};
struct DebugCommandQueue1Compat : public IUnknown {
    virtual BOOL STDMETHODCALLTYPE AssertResourceState(ID3D12Resource*, UINT, UINT) = 0;
    virtual void STDMETHODCALLTYPE AssertResourceAccess(ID3D12Resource*, UINT, UINT) = 0;
    virtual void STDMETHODCALLTYPE AssertTextureLayout(ID3D12Resource*, UINT, UINT) = 0;
};
struct DebugCommandQueueStateCompat : public IUnknown {
    virtual BOOL STDMETHODCALLTYPE GetLastAssertResult() = 0;
};
static constexpr GUID kIIDDebugCommandList3 = {0x197d5e15, 0x4d37, 0x4d34,
                                              {0xaf, 0x78, 0x72, 0x4c, 0xd7, 0x0f, 0xdb, 0x1f}};
static constexpr GUID kIIDDebugCommandListState = {0xf0a6a8d1, 0x61b0, 0x4ef6,
                                                   {0x92, 0x17, 0x78, 0x3c, 0x1d, 0x8e, 0x42, 0x99}};
struct DebugCommandList3Compat : public IUnknown {
    virtual BOOL STDMETHODCALLTYPE AssertResourceState(ID3D12Resource*, UINT, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFeatureMask(UINT) = 0;
    virtual UINT STDMETHODCALLTYPE GetFeatureMask() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDebugParameter(UINT, const void*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDebugParameter(UINT, void*, UINT) = 0;
    virtual void STDMETHODCALLTYPE AssertResourceAccess(ID3D12Resource*, UINT, UINT) = 0;
    virtual void STDMETHODCALLTYPE AssertTextureLayout(ID3D12Resource*, UINT, UINT) = 0;
};
struct DebugCommandListStateCompat : public IUnknown {
    virtual BOOL STDMETHODCALLTYPE GetLastAssertResult() = 0;
};
struct DebugDevice2Compat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetFeatureMask(UINT) = 0;
    virtual UINT STDMETHODCALLTYPE GetFeatureMask() = 0;
    virtual HRESULT STDMETHODCALLTYPE ReportLiveDeviceObjects(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDebugParameter(UINT, const void*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDebugParameter(UINT, void*, UINT) = 0;
};
struct Debug1Compat : public IUnknown {
    virtual void STDMETHODCALLTYPE EnableDebugLayer() = 0;
    virtual void STDMETHODCALLTYPE SetEnableGPUBasedValidation(BOOL) = 0;
    virtual void STDMETHODCALLTYPE SetEnableSynchronizedCommandQueueValidation(BOOL) = 0;
};
struct Debug2Compat : public IUnknown {
    virtual void STDMETHODCALLTYPE SetGPUBasedValidationFlags(UINT) = 0;
};
struct Debug4Compat : public IUnknown {
    virtual void STDMETHODCALLTYPE EnableDebugLayer() = 0;
    virtual void STDMETHODCALLTYPE SetEnableGPUBasedValidation(BOOL) = 0;
    virtual void STDMETHODCALLTYPE SetEnableSynchronizedCommandQueueValidation(BOOL) = 0;
    virtual void STDMETHODCALLTYPE SetGPUBasedValidationFlags(UINT) = 0;
    virtual void STDMETHODCALLTYPE DisableDebugLayer() = 0;
};
struct Debug5Compat : public Debug4Compat {
    virtual void STDMETHODCALLTYPE SetEnableAutoName(BOOL) = 0;
};
struct Debug6Compat : public Debug5Compat {
    virtual void STDMETHODCALLTYPE SetForceLegacyBarrierValidation(BOOL) = 0;
};
struct DebugStateCompat : public IUnknown {
    virtual BOOL STDMETHODCALLTYPE IsDebugLayerEnabled() = 0;
    virtual BOOL STDMETHODCALLTYPE IsGPUBasedValidationEnabled() = 0;
    virtual BOOL STDMETHODCALLTYPE IsSynchronizedValidationEnabled() = 0;
    virtual UINT STDMETHODCALLTYPE GetGPUBasedValidationFlags() = 0;
    virtual BOOL STDMETHODCALLTYPE IsAutoNameEnabled() = 0;
    virtual BOOL STDMETHODCALLTYPE IsLegacyBarrierValidationEnabled() = 0;
};
using GetDebugFn = HRESULT(WINAPI *)(REFIID, void **);
static unsigned long hr_value(HRESULT hr) {
    return static_cast<unsigned long>(static_cast<uint32_t>(hr));
}
template <typename T> static void release(T *&value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
template <typename T> static T load_proc(HMODULE module, const char *name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto get_debug = load_proc<GetDebugFn>(module, "D3D12GetDebugInterface");
    Debug6Compat *debug6 = nullptr;
    DebugStateCompat *state = nullptr;
    ID3D12Device *device = nullptr;
    DebugDevice2Compat *debug_device = nullptr;
    ID3D12CommandQueue *command_queue = nullptr;
    DebugCommandQueue1Compat *debug_queue = nullptr;
    DebugCommandQueueStateCompat *debug_queue_state = nullptr;
    ID3D12CommandAllocator *debug_allocator = nullptr;
    ID3D12GraphicsCommandList *debug_list = nullptr;
    DebugCommandList3Compat *debug_list_iface = nullptr;
    DebugCommandListStateCompat *debug_list_state = nullptr;
    ID3D12Resource *debug_resource = nullptr;
    HRESULT null_hr = get_debug ? get_debug(kIIDDebug, nullptr) : E_FAIL;
    HRESULT debug_hr = get_debug
                           ? get_debug(kIIDDebug6,
                                       reinterpret_cast<void **>(&debug6))
                           : E_FAIL;
    HRESULT state_hr = debug6 ? debug6->QueryInterface(
                                   kIIDDebugState,
                                   reinterpret_cast<void **>(&state))
                             : E_FAIL;
    if (debug6) {
        debug6->EnableDebugLayer();
        debug6->SetEnableGPUBasedValidation(TRUE);
        debug6->SetEnableSynchronizedCommandQueueValidation(TRUE);
        debug6->SetGPUBasedValidationFlags(7);
        debug6->SetEnableAutoName(TRUE);
        debug6->SetForceLegacyBarrierValidation(TRUE);
    }
    const bool state_ok = state && state->IsDebugLayerEnabled() &&
                          state->IsGPUBasedValidationEnabled() &&
                          state->IsSynchronizedValidationEnabled() &&
                          state->GetGPUBasedValidationFlags() == 7 &&
                          state->IsAutoNameEnabled() &&
                          state->IsLegacyBarrierValidationEnabled();
    if (debug6)
        debug6->DisableDebugLayer();
    const bool disabled = state && !state->IsDebugLayerEnabled();
    Debug1Compat *debug1 = nullptr;
    HRESULT debug1_hr = get_debug
                            ? get_debug(kIIDDebug1,
                                        reinterpret_cast<void **>(&debug1))
                            : E_FAIL;
    Debug2Compat *debug2 = nullptr;
    HRESULT debug2_hr = get_debug
                            ? get_debug(kIIDDebug2,
                                        reinterpret_cast<void **>(&debug2))
                            : E_FAIL;
    using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                              REFIID, void **);
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT debug_device_hr = device
                                  ? device->QueryInterface(
                                        kIIDDebugDevice2,
                                        reinterpret_cast<void **>(&debug_device))
                                  : E_FAIL;
    uint8_t parameter[4] = {1, 2, 3, 4};
    uint8_t parameter_copy[4] = {};
    HRESULT set_parameter_hr = debug_device
                                   ? debug_device->SetFeatureMask(3)
                                   : E_FAIL;
    if (debug_device)
        set_parameter_hr = debug_device->SetDebugParameter(0, parameter,
                                                            sizeof(parameter));
    HRESULT get_parameter_hr = debug_device
                                    ? debug_device->GetDebugParameter(
                                          0, parameter_copy,
                                          sizeof(parameter_copy))
                                    : E_FAIL;
    HRESULT report_live_hr = debug_device
                                 ? debug_device->ReportLiveDeviceObjects(0)
                                 : E_FAIL;
    D3D12_COMMAND_QUEUE_DESC debug_queue_desc = {};
    HRESULT debug_queue_create_hr = device
                                         ? device->CreateCommandQueue(
                                               &debug_queue_desc,
                                               IID_PPV_ARGS(&command_queue))
                                         : E_FAIL;
    HRESULT debug_queue_hr = command_queue
                                 ? command_queue->QueryInterface(
                                       kIIDDebugCommandQueue1,
                                       reinterpret_cast<void **>(&debug_queue))
                                 : E_FAIL;
    HRESULT debug_queue_state_hr = debug_queue
                                       ? debug_queue->QueryInterface(
                                             kIIDDebugCommandQueueState,
                                             reinterpret_cast<void **>(&debug_queue_state))
                                       : E_FAIL;
    D3D12_HEAP_PROPERTIES debug_heap = {};
    debug_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    debug_heap.CreationNodeMask = 1;
    debug_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC debug_desc = {};
    debug_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    debug_desc.Width = 256;
    debug_desc.Height = 1;
    debug_desc.DepthOrArraySize = 1;
    debug_desc.MipLevels = 1;
    debug_desc.Format = DXGI_FORMAT_UNKNOWN;
    debug_desc.SampleDesc.Count = 1;
    debug_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT debug_resource_hr = device
                                    ? device->CreateCommittedResource(
                                          &debug_heap, D3D12_HEAP_FLAG_NONE,
                                          &debug_desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&debug_resource))
                                    : E_FAIL;
    HRESULT debug_allocator_hr = device
                                     ? device->CreateCommandAllocator(
                                           D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&debug_allocator))
                                     : E_FAIL;
    HRESULT debug_list_hr = device && debug_allocator
                                ? device->CreateCommandList(
                                      0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      debug_allocator, nullptr,
                                      IID_PPV_ARGS(&debug_list))
                                : E_FAIL;
    HRESULT debug_list_iface_hr = debug_list
                                      ? debug_list->QueryInterface(
                                            kIIDDebugCommandList3,
                                            reinterpret_cast<void **>(&debug_list_iface))
                                      : E_FAIL;
    HRESULT debug_list_state_hr = debug_list_iface
                                      ? debug_list_iface->QueryInterface(
                                            kIIDDebugCommandListState,
                                            reinterpret_cast<void **>(&debug_list_state))
                                      : E_FAIL;
    BOOL debug_list_state_true = debug_list_iface && debug_resource
                                     ? debug_list_iface->AssertResourceState(
                                           debug_resource, 0,
                                           D3D12_RESOURCE_STATE_GENERIC_READ)
                                     : FALSE;
    BOOL debug_list_state_false = debug_list_iface && debug_resource
                                      ? debug_list_iface->AssertResourceState(
                                            debug_resource, 0,
                                            D3D12_RESOURCE_STATE_COPY_DEST)
                                      : TRUE;
    BOOL debug_state_true = debug_queue && debug_resource
                                ? debug_queue->AssertResourceState(
                                      debug_resource, 0,
                                      D3D12_RESOURCE_STATE_GENERIC_READ)
                                : FALSE;
    BOOL debug_state_false = debug_queue && debug_resource
                                 ? debug_queue->AssertResourceState(
                                       debug_resource, 0,
                                       D3D12_RESOURCE_STATE_COPY_DEST)
                                 : TRUE;
    const bool pass = get_debug && null_hr == E_POINTER && debug_hr == S_OK &&
                      state_hr == S_OK && state_ok && disabled &&
                      debug1_hr == S_OK && debug2_hr == S_OK &&
                      device_hr == S_OK && debug_device_hr == S_OK &&
                      debug_device->GetFeatureMask() == 3 &&
                      set_parameter_hr == S_OK && get_parameter_hr == S_OK &&
                      report_live_hr == S_OK &&
                      std::memcmp(parameter, parameter_copy, sizeof(parameter)) == 0 &&
                      debug_queue_create_hr == S_OK && debug_queue_hr == S_OK &&
                      debug_queue_state_hr == S_OK && debug_resource_hr == S_OK &&
                      debug_state_true == TRUE && debug_state_false == FALSE &&
                      debug_queue_state->GetLastAssertResult() == FALSE &&
                      debug_allocator_hr == S_OK && debug_list_hr == S_OK &&
                      debug_list_iface_hr == S_OK && debug_list_state_hr == S_OK &&
                      debug_list_state_true == TRUE && debug_list_state_false == FALSE &&
                      debug_list_state->GetLastAssertResult() == FALSE;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.debug-interfaces.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"null_output\": \"0x%08lx\",\n", hr_value(null_hr));
    std::printf("  \"debug6\": \"0x%08lx\",\n", hr_value(debug_hr));
    std::printf("  \"state\": \"0x%08lx\",\n", hr_value(state_hr));
    std::printf("  \"state_roundtrip\": %s,\n", state_ok ? "true" : "false");
    std::printf("  \"disabled_roundtrip\": %s,\n", disabled ? "true" : "false");
    std::printf("  \"debug1\": \"0x%08lx\",\n", hr_value(debug1_hr));
    std::printf("  \"debug2\": \"0x%08lx\",\n", hr_value(debug2_hr));
    std::printf("  \"debug_device\": \"0x%08lx\",\n", hr_value(debug_device_hr));
    std::printf("  \"debug_parameter\": \"0x%08lx\",\n", hr_value(get_parameter_hr));
    std::printf("  \"debug_parameter_exact\": %s,\n",
                std::memcmp(parameter, parameter_copy, sizeof(parameter)) == 0
                    ? "true" : "false");
    std::printf("  \"debug_command_queue\": \"0x%08lx\",\n",
                hr_value(debug_queue_hr));
    std::printf("  \"debug_resource_state\": %s,\n",
                debug_state_true && !debug_state_false ? "true" : "false");
    std::printf("  \"debug_command_list\": \"0x%08lx\",\n",
                hr_value(debug_list_iface_hr));
    std::printf("  \"debug_command_list_state\": %s\n",
                debug_list_state_true && !debug_list_state_false ? "true" : "false");
    std::printf("}\n");
    release(debug2);
    release(debug1);
    release(state);
    release(debug6);
    release(debug_device);
    release(debug_queue_state);
    release(debug_queue);
    release(debug_list_state);
    release(debug_list_iface);
    release(debug_list);
    release(debug_allocator);
    release(debug_resource);
    release(command_queue);
    release(device);
    if (d3d12)
        FreeLibrary(d3d12);
    if (module)
        FreeLibrary(module);
    return pass ? 0 : 1;
}
