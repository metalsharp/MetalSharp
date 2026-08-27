#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>

namespace {

template <typename T> void release(T *&object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

void print_hr(HRESULT hr) {
  std::printf("0x%08x", static_cast<unsigned>(hr));
}

bool validate_rgba(const uint8_t *data, UINT row_pitch, UINT width, UINT height,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!data || row_pitch < width * 4)
    return false;
  for (UINT y = 0; y < height; ++y) {
    const uint8_t *row = data + static_cast<size_t>(y) * row_pitch;
    for (UINT x = 0; x < width; ++x) {
      const uint8_t *pixel = row + x * 4;
      if (pixel[0] != r || pixel[1] != g || pixel[2] != b || pixel[3] != a)
        return false;
    }
  }
  return true;
}

HRESULT probe_d3d11(bool &readback_ok) {
  constexpr UINT width = 4;
  constexpr UINT height = 4;
  readback_ok = false;

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  ID3D11Texture2D *target = nullptr;
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11Texture2D *staging = nullptr;
  D3D11_TEXTURE2D_DESC desc = {};
  D3D11_TEXTURE2D_DESC staging_desc = {};
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
  const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_0,
                                               D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, feature_levels,
      ARRAYSIZE(feature_levels), D3D11_SDK_VERSION, &device, &feature_level,
      &context);

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
      const auto *bytes = static_cast<const uint8_t *>(mapped.pData);
      readback_ok = validate_rgba(bytes, mapped.RowPitch, width, height, 0,
                                  255, 0, 255);
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
  return hr;
}

HRESULT probe_d3d10(bool &readback_ok) {
  constexpr UINT width = 4;
  constexpr UINT height = 4;
  readback_ok = false;

  ID3D10Device *device = nullptr;
  ID3D10Texture2D *target = nullptr;
  ID3D10RenderTargetView *rtv = nullptr;
  ID3D10Texture2D *staging = nullptr;
  D3D10_TEXTURE2D_DESC desc = {};
  D3D10_TEXTURE2D_DESC staging_desc = {};
  HRESULT hr = D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr,
                                 0, D3D10_SDK_VERSION, &device);

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
      const auto *bytes = static_cast<const uint8_t *>(mapped.pData);
      readback_ok = validate_rgba(bytes, mapped.RowPitch, width, height, 0,
                                  0, 255, 255);
      staging->Unmap(0);
      if (!readback_ok)
        hr = E_FAIL;
    }
  }

  release(staging);
  release(rtv);
  release(target);
  release(device);
  return hr;
}

} // namespace

int main() {
  bool d3d11_readback = false;
  bool d3d10_readback = false;
  const HRESULT d3d11_hr = probe_d3d11(d3d11_readback);
  const HRESULT d3d10_hr = probe_d3d10(d3d10_readback);
  const bool pass = SUCCEEDED(d3d11_hr) && d3d11_readback &&
                    SUCCEEDED(d3d10_hr) && d3d10_readback;

  std::printf("{\"pass\":%s,\"d3d11_hr\":\"",
              pass ? "true" : "false");
  print_hr(d3d11_hr);
  std::printf("\",\"d3d11_readback\":%s,\"d3d10_hr\":\"",
              d3d11_readback ? "true" : "false");
  print_hr(d3d10_hr);
  std::printf("\",\"d3d10_readback\":%s}\n",
              d3d10_readback ? "true" : "false");
  return pass ? 0 : 1;
}
