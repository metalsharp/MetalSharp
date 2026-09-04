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

static constexpr GUID kIIDManualWriteTrackingResource = {
    0x86ca3b85, 0x49ad, 0x4b6e,
    {0xae, 0xd5, 0xed, 0xdb, 0x18, 0x54, 0x0f, 0x41}};
struct ManualWriteTrackingResourceCompat : public IUnknown {
    virtual void STDMETHODCALLTYPE TrackWrite(UINT, const D3D12_RANGE *) = 0;
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
    ID3D12Resource *resource = nullptr;
    ManualWriteTrackingResourceCompat *tracking = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT resource_hr = device
                              ? device->CreateCommittedResource(
                                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&resource))
                              : E_FAIL;
    struct Options17 {
        BOOL non_normalized_samplers;
        BOOL manual_write_tracking;
    } options17 = {};
    HRESULT options_hr = device
                             ? device->CheckFeatureSupport(
                                   static_cast<D3D12_FEATURE>(46), &options17,
                                   sizeof(options17))
                             : E_FAIL;
    HRESULT query_hr = resource
                           ? resource->QueryInterface(
                                 kIIDManualWriteTrackingResource,
                                 reinterpret_cast<void **>(&tracking))
                           : E_FAIL;
    void *mapped = nullptr;
    HRESULT map_hr = resource ? resource->Map(0, nullptr, &mapped) : E_FAIL;
    uint32_t expected = 0x11223344u;
    if (mapped)
        std::memcpy(mapped, &expected, sizeof(expected));
    D3D12_RANGE written = {0, sizeof(expected)};
    if (tracking && SUCCEEDED(map_hr))
        tracking->TrackWrite(0, &written);
    if (resource && SUCCEEDED(map_hr))
        resource->Unmap(0, &written);
    uint32_t actual = 0;
    HRESULT read_hr = resource
                          ? resource->ReadFromSubresource(
                                &actual, sizeof(actual), sizeof(actual), 0,
                                nullptr)
                          : E_FAIL;
    const bool pass = device_hr == S_OK && resource_hr == S_OK &&
                      options_hr == S_OK && !options17.non_normalized_samplers &&
                      options17.manual_write_tracking && query_hr == S_OK &&
                      tracking != nullptr && map_hr == S_OK &&
                      read_hr == S_OK && actual == expected;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.manual-write-tracking.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"options17\": \"0x%08lx\",\n", hr_value(options_hr));
    std::printf("  \"manual_write_tracking_supported\": %s,\n",
                options17.manual_write_tracking ? "true" : "false");
    std::printf("  \"query\": \"0x%08lx\",\n", hr_value(query_hr));
    std::printf("  \"map\": \"0x%08lx\",\n", hr_value(map_hr));
    std::printf("  \"readback\": \"0x%08lx\",\n", hr_value(read_hr));
    std::printf("  \"exact_value\": \"0x%08x\"\n", actual);
    std::printf("}\n");
    release(tracking);
    release(resource);
    release(device);
    if (module)
        FreeLibrary(module);
    return pass ? 0 : 1;
}
