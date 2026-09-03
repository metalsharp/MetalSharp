
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d11.h"
#include "log/log.hpp"
#include <cstring>
#include <cstdlib>

namespace dxmt {
Logger Logger::s_instance("d3d10core.log");

using D3D11CoreCreateDeviceFn = HRESULT(WINAPI *)(
    IDXGIFactory *, IDXGIAdapter *, UINT, const D3D_FEATURE_LEVEL *, UINT,
    ID3D11Device **);

static D3D11CoreCreateDeviceFn LoadD3D11CoreCreateDevice() {
  const char *probe_alias = std::getenv("DXMT_PROBE_D3D11_DLL");
  HMODULE module = LoadLibraryA(probe_alias && probe_alias[0]
                                    ? probe_alias
                                    : "d3d11.dll");
  if (!module)
    return nullptr;
  FARPROC procedure = GetProcAddress(module, "D3D11CoreCreateDevice");
  D3D11CoreCreateDeviceFn function = nullptr;
  static_assert(sizeof(function) == sizeof(procedure),
                "function pointer size mismatch");
  std::memcpy(&function, &procedure, sizeof(function));
  return function;
}

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreCreateDevice(
    IDXGIFactory *pFactory, IDXGIAdapter *pAdapter, UINT Flags, D3D_FEATURE_LEVEL FeatureLevel, ID3D10Device **ppDevice
) {
  InitReturnPtr(ppDevice);

  Com<ID3D11Device> d3d11_device;

  HRESULT hr = pAdapter->CheckInterfaceSupport(__uuidof(ID3D10Device), nullptr);

  if (FAILED(hr))
    return hr;

  const auto create_d3d11 = LoadD3D11CoreCreateDevice();
  if (!create_d3d11)
    return E_NOINTERFACE;
  hr = create_d3d11(pFactory, pAdapter,
                    Flags | 0x80000000 /* DXMT_D3D10_DEVICE */,
                    &FeatureLevel, 1, &d3d11_device);

  if (FAILED(hr))
    return hr;

  Com<ID3D10Multithread> multithread;
  d3d11_device->QueryInterface(IID_PPV_ARGS(&multithread));
  multithread->SetMultithreadProtected(!(Flags & D3D10_CREATE_DEVICE_SINGLETHREADED));

  return d3d11_device->QueryInterface(IID_PPV_ARGS(ppDevice));
}

extern "C" HRESULT STDMETHODCALLTYPE
D3D10CoreRegisterLayers() {
  ERR("D3D10CoreRegisterLayers: stub");
  return E_NOTIMPL;
}
} // namespace dxmt
