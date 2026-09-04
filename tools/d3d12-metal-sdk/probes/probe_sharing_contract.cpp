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

static constexpr GUID kIIDSharingContract = {
    0x0adf7d52, 0x929c, 0x4e61,
    {0xad, 0xdb, 0xff, 0xed, 0x30, 0xde, 0x66, 0xef}};
struct SharingContractCompat : public IUnknown {
    virtual void STDMETHODCALLTYPE Present(ID3D12Resource *, UINT, HWND) = 0;
    virtual void STDMETHODCALLTYPE SharedFenceSignal(ID3D12Fence *, UINT64) = 0;
    virtual void STDMETHODCALLTYPE BeginCapturableWork(REFGUID) = 0;
    virtual void STDMETHODCALLTYPE EndCapturableWork(REFGUID) = 0;
    virtual UINT64 STDMETHODCALLTYPE GetPresentCount() = 0;
    virtual UINT64 STDMETHODCALLTYPE GetLastPresentChecksum() = 0;
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
    SharingContractCompat *sharing = nullptr;
    ID3D12Resource *texture = nullptr;
    ID3D12Fence *fence = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT sharing_hr = device ? device->QueryInterface(
                                      kIIDSharingContract,
                                      reinterpret_cast<void **>(&sharing))
                                : E_FAIL;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2;
    desc.Height = 2;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT resource_hr = device
                              ? device->CreateCommittedResource(
                                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&texture))
                              : E_FAIL;
    const uint32_t pixels[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u,
                                0xffffffffu};
    HRESULT write_hr = texture
                           ? texture->WriteToSubresource(0, nullptr, pixels,
                                                         2 * 4, sizeof(pixels))
                           : E_FAIL;
    HRESULT fence_hr = device ? device->CreateFence(
                                    0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence)) : E_FAIL;
    const GUID capture_guid = {0x12345678, 0x9abc, 0x4def,
                               {0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde}};
    if (sharing && SUCCEEDED(resource_hr) && SUCCEEDED(write_hr)) {
        sharing->BeginCapturableWork(capture_guid);
        sharing->Present(texture, 0, nullptr);
        sharing->EndCapturableWork(capture_guid);
    }
    if (sharing && fence && SUCCEEDED(fence_hr))
        sharing->SharedFenceSignal(fence, 9);
    const UINT64 completed = fence ? fence->GetCompletedValue() : 0;
    const UINT64 present_count = sharing ? sharing->GetPresentCount() : 0;
    const UINT64 checksum = sharing ? sharing->GetLastPresentChecksum() : 0;
    const bool pass = device_hr == S_OK && sharing_hr == S_OK &&
                      resource_hr == S_OK && write_hr == S_OK && fence_hr == S_OK &&
                      completed == 9 && present_count == 1 && checksum != 0;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.sharing-contract.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"query\": \"0x%08lx\",\n", hr_value(sharing_hr));
    std::printf("  \"fence_completed\": %llu,\n",
                static_cast<unsigned long long>(completed));
    std::printf("  \"present_count\": %llu,\n",
                static_cast<unsigned long long>(present_count));
    std::printf("  \"present_checksum\": \"0x%016llx\"\n",
                static_cast<unsigned long long>(checksum));
    std::printf("}\n");
    release(fence);
    release(texture);
    release(sharing);
    release(device);
    if (module)
        FreeLibrary(module);
    return pass ? 0 : 1;
}
