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
    ID3D12Resource *texture = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
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
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT resource_hr = device
                              ? device->CreateCommittedResource(
                                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&texture))
                              : E_FAIL;
    D3D12_RANGE read_range = {0, 16};
    void *mapped = nullptr;
    HRESULT map_hr = texture ? texture->Map(0, &read_range, &mapped) : E_FAIL;
    const uint32_t expected[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u,
                                  0xffffffffu};
    if (mapped)
        std::memcpy(mapped, expected, sizeof(expected));
    D3D12_RANGE written = {0, sizeof(expected)};
    if (texture && SUCCEEDED(map_hr))
        texture->Unmap(0, &written);
    uint32_t actual[4] = {};
    HRESULT read_hr = texture
                          ? texture->ReadFromSubresource(
                                actual, 2 * sizeof(uint32_t), sizeof(actual), 0,
                                nullptr)
                          : E_FAIL;
    const bool exact = SUCCEEDED(read_hr) &&
                       std::memcmp(actual, expected, sizeof(actual)) == 0;
    const bool pass = device_hr == S_OK && resource_hr == S_OK &&
                      map_hr == S_OK && mapped != nullptr && read_hr == S_OK && exact;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.cpu-texture-map.v1\",\n");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"create_resource\": \"0x%08lx\",\n", hr_value(resource_hr));
    std::printf("  \"map\": \"0x%08lx\",\n", hr_value(map_hr));
    std::printf("  \"readback\": \"0x%08lx\",\n", hr_value(read_hr));
    std::printf("  \"exact\": %s\n", exact ? "true" : "false");
    std::printf("}\n");
    release(texture);
    release(device);
    if (module)
        FreeLibrary(module);
    return pass ? 0 : 1;
}
