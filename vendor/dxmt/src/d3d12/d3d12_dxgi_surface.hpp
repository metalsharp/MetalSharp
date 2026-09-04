#pragma once

#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "dxgi1_2.h"
#include <atomic>
#include <new>
#include <vector>

struct IMTLDXGIDevice;

namespace dxmt {

class MTLD3D12Resource;

class MTLD3D12DXGISurface final : public IDXGISurface2 {
public:
  MTLD3D12DXGISurface(IMTLDXGIDevice *device, MTLD3D12Resource *resource,
                      UINT subresource, const DXGI_SURFACE_DESC &desc);
  ~MTLD3D12DXGISurface();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                            void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                            const void *data) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                     const IUnknown *data) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                            void *data) override;
  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override;
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override;
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SURFACE_DESC *desc) override;
  HRESULT STDMETHODCALLTYPE Map(DXGI_MAPPED_RECT *mapped_rect,
                                UINT flags) override;
  HRESULT STDMETHODCALLTYPE Unmap() override;
  HRESULT STDMETHODCALLTYPE GetDC(WINBOOL discard, HDC *hdc) override;
  HRESULT STDMETHODCALLTYPE ReleaseDC(RECT *dirty_rect) override;
  HRESULT STDMETHODCALLTYPE GetResource(REFIID iid, void **resource,
                                         UINT *subresource) override;

private:
  UINT BytesPerPixel() const;
  HRESULT CopyResourceToShadow();
  HRESULT CopyShadowToResource();

  std::atomic<ULONG> m_ref_count = {1};
  IMTLDXGIDevice *m_device = nullptr;
  MTLD3D12Resource *m_resource = nullptr;
  UINT m_subresource = 0;
  DXGI_SURFACE_DESC m_desc = {};
  UINT m_row_pitch = 0;
  UINT m_map_flags = 0;
  std::vector<uint8_t> m_shadow;
  ComPrivateData m_private_data;
};

IDXGISurface *CreateD3D12DXGISurface(IMTLDXGIDevice *device,
                                     MTLD3D12Resource *resource,
                                     UINT subresource,
                                     const DXGI_SURFACE_DESC &desc);

} // namespace dxmt
