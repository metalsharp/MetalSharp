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

static constexpr GUID kIIDInfoQueue1 = {
    0x2852dd88, 0xb484, 0x4c0c,
    {0xb6, 0xb1, 0x67, 0x16, 0x85, 0x00, 0xe6, 0x00}};
using MessageCallback = void(STDMETHODCALLTYPE *)(
    D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY, D3D12_MESSAGE_ID,
    LPCSTR, void *);
struct InfoQueue1Compat : public ID3D12InfoQueue {
    virtual HRESULT STDMETHODCALLTYPE RegisterMessageCallback(
        MessageCallback, UINT, void *, DWORD *) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterMessageCallback(DWORD) = 0;
};
using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID,
                                          void **);

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static unsigned long hr_value(HRESULT hr) {
    return static_cast<unsigned long>(static_cast<uint32_t>(hr));
}

struct CallbackState {
    UINT count = 0;
    D3D12_MESSAGE_CATEGORY category = D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED;
    D3D12_MESSAGE_SEVERITY severity = D3D12_MESSAGE_SEVERITY_CORRUPTION;
    D3D12_MESSAGE_ID id = D3D12_MESSAGE_ID_UNKNOWN;
    void *context = nullptr;
    char description[128] = {};
};
static void STDMETHODCALLTYPE callback(
    D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id, LPCSTR description, void *context) {
    auto *state = static_cast<CallbackState *>(context);
    if (!state)
        return;
    ++state->count;
    state->category = category;
    state->severity = severity;
    state->id = id;
    state->context = context;
    if (description)
        std::strncpy(state->description, description,
                     sizeof(state->description) - 1);
}

template <typename T> static void release(T *&value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    ID3D12InfoQueue *queue = nullptr;
    InfoQueue1Compat *queue1 = nullptr;
    CallbackState state;
    CallbackState ignored_state;
    DWORD callback_cookie = 0;
    DWORD ignored_cookie = 0;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT queue_hr = device ? device->QueryInterface(IID_PPV_ARGS(&queue)) : E_FAIL;
    HRESULT queue1_hr = queue ? queue->QueryInterface(
                                    kIIDInfoQueue1,
                                    reinterpret_cast<void **>(&queue1))
                              : E_FAIL;
    HRESULT register_hr = queue1
                              ? queue1->RegisterMessageCallback(
                                    callback, 0u, &state, &callback_cookie)
                              : E_FAIL;
    HRESULT message_hr = queue && SUCCEEDED(register_hr)
                             ? queue->AddApplicationMessage(
                                   D3D12_MESSAGE_SEVERITY_WARNING,
                                   "infoqueue callback message")
                             : E_FAIL;

    D3D12_INFO_QUEUE_FILTER filter = {};
    D3D12_MESSAGE_SEVERITY denied_severity = D3D12_MESSAGE_SEVERITY_WARNING;
    filter.DenyList.NumSeverities = 1;
    filter.DenyList.pSeverityList = &denied_severity;
    HRESULT filter_hr = queue ? queue->AddStorageFilterEntries(&filter) : E_FAIL;
    HRESULT filtered_message_hr = queue && SUCCEEDED(filter_hr)
                                      ? queue->AddApplicationMessage(
                                            D3D12_MESSAGE_SEVERITY_WARNING,
                                            "filtered callback message")
                                      : E_FAIL;
    HRESULT ignored_register_hr = queue1
                                      ? queue1->RegisterMessageCallback(
                                            callback,
                                            1u, &ignored_state, &ignored_cookie)
                                      : E_FAIL;
    HRESULT ignored_message_hr = queue && SUCCEEDED(ignored_register_hr)
                                     ? queue->AddApplicationMessage(
                                           D3D12_MESSAGE_SEVERITY_WARNING,
                                           "ignored-filter callback message")
                                     : E_FAIL;
    HRESULT unregister_hr = queue1 && callback_cookie
                                ? queue1->UnregisterMessageCallback(callback_cookie)
                                : E_FAIL;
    HRESULT unregister_ignored_hr = queue1 && ignored_cookie
                                        ? queue1->UnregisterMessageCallback(ignored_cookie)
                                        : E_FAIL;
    const bool pass = create_hr == S_OK && queue_hr == S_OK && queue1_hr == S_OK &&
                      register_hr == S_OK && callback_cookie != 0 &&
                      message_hr == S_OK && state.count == 1 &&
                      state.context == &state &&
                      std::strcmp(state.description, "infoqueue callback message") == 0 &&
                      filter_hr == S_OK && filtered_message_hr == S_OK &&
                      ignored_register_hr == S_OK && ignored_cookie != 0 &&
                      ignored_message_hr == S_OK && ignored_state.count == 1 &&
                      ignored_state.context == &ignored_state &&
                      unregister_hr == S_OK && unregister_ignored_hr == S_OK;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.infoqueue-callback.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"create_device\": \"0x%08lx\",\n", hr_value(create_hr));
    std::printf("  \"query_infoqueue\": \"0x%08lx\",\n", hr_value(queue_hr));
    std::printf("  \"query_infoqueue1\": \"0x%08lx\",\n", hr_value(queue1_hr));
    std::printf("  \"register\": \"0x%08lx\",\n", hr_value(register_hr));
    std::printf("  \"message\": \"0x%08lx\",\n", hr_value(message_hr));
    std::printf("  \"callback_count\": %u,\n", state.count);
    std::printf("  \"filter\": \"0x%08lx\",\n", hr_value(filter_hr));
    std::printf("  \"filtered_message\": \"0x%08lx\",\n",
                hr_value(filtered_message_hr));
    std::printf("  \"ignored_callback_count\": %u,\n", ignored_state.count);
    std::printf("  \"unregister\": \"0x%08lx\",\n", hr_value(unregister_hr));
    std::printf("  \"unregister_ignored\": \"0x%08lx\"\n",
                hr_value(unregister_ignored_hr));
    std::printf("}\n");

    release(queue1);
    release(queue);
    release(device);
    if (module)
        FreeLibrary(module);
    return pass ? 0 : 1;
}
