#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d10.h>
#include <d3d11.h>
#include <windows.h>

namespace {

template <typename Function>
Function load_function(HMODULE module, const char* name) {
    if (!module || !name)
        return nullptr;
    FARPROC procedure = GetProcAddress(module, name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(procedure), "function pointer size mismatch");
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
}

using d3d11_create_device_fn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using dxgi_create_factory1_fn = HRESULT(WINAPI*)(REFIID, void**);
using d3d10_core_create_device_fn = HRESULT(WINAPI*)(
    IDXGIFactory*, IDXGIAdapter*, UINT, D3D_FEATURE_LEVEL, ID3D10Device**);

template <typename T> void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void print_hr(HRESULT hr) {
    std::printf("0x%08x", static_cast<unsigned>(hr));
}

bool validate_rgba(const uint8_t* data, UINT row_pitch, UINT width, UINT height, uint8_t r, uint8_t g, uint8_t b,
                   uint8_t a) {
    if (!data || row_pitch < width * 4)
        return false;
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * row_pitch;
        for (UINT x = 0; x < width; ++x) {
            const uint8_t* pixel = row + x * 4;
            if (pixel[0] != r || pixel[1] != g || pixel[2] != b || pixel[3] != a)
                return false;
        }
    }
    return true;
}

HRESULT probe_d3d11(bool& readback_ok) {
    constexpr UINT width = 4;
    constexpr UINT height = 4;
    readback_ok = false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    HMODULE d3d11_module = LoadLibraryA("d3d11.dll");
    D3D11_TEXTURE2D_DESC desc = {};
    D3D11_TEXTURE2D_DESC staging_desc = {};
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
    const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const auto create_device = load_function<d3d11_create_device_fn>(
        d3d11_module, "D3D11CreateDevice");
    HRESULT hr = create_device
                     ? create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                     feature_levels, ARRAYSIZE(feature_levels),
                                     D3D11_SDK_VERSION, &device, &feature_level,
                                     &context)
                     : E_NOINTERFACE;

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&desc, nullptr, &target);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(target, nullptr, &rtv);
    if (SUCCEEDED(hr)) {
        const FLOAT color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(rtv, color);
    }

    staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (SUCCEEDED(hr)) {
        context->CopyResource(staging, target);
        context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
            readback_ok = validate_rgba(bytes, mapped.RowPitch, width, height, 0, 255, 0, 255);
            context->Unmap(staging, 0);
            if (!readback_ok)
                hr = E_FAIL;
        }
    }

    release(staging);
    release(rtv);
    release(target);
    release(context);
    release(device);
    if (d3d11_module)
        FreeLibrary(d3d11_module);
    return hr;
}

HRESULT probe_d3d10(bool& readback_ok) {
    constexpr UINT width = 4;
    constexpr UINT height = 4;
    readback_ok = false;

    ID3D10Device* device = nullptr;
    ID3D10Texture2D* target = nullptr;
    ID3D10RenderTargetView* rtv = nullptr;
    ID3D10Texture2D* staging = nullptr;
    HMODULE dxgi_module = LoadLibraryA("dxgi.dll");
    HMODULE d3d10core_module = LoadLibraryA("d3d10core.dll");
    const auto create_factory = load_function<dxgi_create_factory1_fn>(
        dxgi_module, "CreateDXGIFactory1");
    const auto create_device = load_function<d3d10_core_create_device_fn>(
        d3d10core_module, "D3D10CoreCreateDevice");
    D3D10_TEXTURE2D_DESC desc = {};
    D3D10_TEXTURE2D_DESC staging_desc = {};
    IDXGIFactory* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = create_factory
                     ? create_factory(__uuidof(IDXGIFactory1), (void**)&factory)
                     : E_NOINTERFACE;
    if (SUCCEEDED(hr))
        hr = factory->EnumAdapters(0, &adapter);
    if (SUCCEEDED(hr))
        hr = create_device
                 ? create_device(factory, adapter, 0, D3D_FEATURE_LEVEL_10_0,
                                 &device)
                 : E_NOINTERFACE;

    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_RENDER_TARGET;

    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&desc, nullptr, &target);
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(target, nullptr, &rtv);
    if (SUCCEEDED(hr)) {
        const FLOAT color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        device->ClearRenderTargetView(rtv, color);
    }

    staging_desc = desc;
    staging_desc.Usage = D3D10_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (SUCCEEDED(hr))
        hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (SUCCEEDED(hr)) {
        device->CopyResource(staging, target);
        device->Flush();
        D3D10_MAPPED_TEXTURE2D mapped = {};
        hr = staging->Map(0, D3D10_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
            readback_ok = validate_rgba(bytes, mapped.RowPitch, width, height, 0, 0, 255, 255);
            staging->Unmap(0);
            if (!readback_ok)
                hr = E_FAIL;
        }
    }

    release(staging);
    release(rtv);
    release(target);
    release(device);
    release(adapter);
    release(factory);
    if (d3d10core_module)
        FreeLibrary(d3d10core_module);
    if (dxgi_module)
        FreeLibrary(dxgi_module);
    return hr;
}

} // namespace

int main() {
    bool d3d11_readback = false;
    bool d3d10_readback = false;
    const HRESULT d3d11_hr = probe_d3d11(d3d11_readback);
    const HRESULT d3d10_hr = probe_d3d10(d3d10_readback);
    const bool pass = SUCCEEDED(d3d11_hr) && d3d11_readback && SUCCEEDED(d3d10_hr) && d3d10_readback;

    std::printf("{\"pass\":%s,\"d3d11_hr\":\"", pass ? "true" : "false");
    print_hr(d3d11_hr);
    std::printf("\",\"d3d11_readback\":%s,\"d3d10_hr\":\"", d3d11_readback ? "true" : "false");
    print_hr(d3d10_hr);
    std::printf("\",\"d3d10_readback\":%s}\n", d3d10_readback ? "true" : "false");
    return pass ? 0 : 1;
}
