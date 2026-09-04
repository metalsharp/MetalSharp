#include "d3d12_dxgi_surface.hpp"

#include "d3d12_dxgi_device.hpp"
#include "d3d12_device.hpp"
#include "d3d12_resource.hpp"
#include "util_string.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt {
namespace {

static UINT SurfaceBytesPerPixel(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8_UNORM:
  case DXGI_FORMAT_R8_UINT:
  case DXGI_FORMAT_R8_SINT:
  case DXGI_FORMAT_R8_TYPELESS:
  case DXGI_FORMAT_A8_UNORM:
    return 1;
  case DXGI_FORMAT_R8G8_UNORM:
  case DXGI_FORMAT_R8G8_UINT:
  case DXGI_FORMAT_R8G8_SINT:
  case DXGI_FORMAT_R8G8_TYPELESS:
  case DXGI_FORMAT_R16_FLOAT:
  case DXGI_FORMAT_R16_UINT:
  case DXGI_FORMAT_R16_SINT:
  case DXGI_FORMAT_R16_UNORM:
  case DXGI_FORMAT_R16_TYPELESS:
    return 2;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UINT:
  case DXGI_FORMAT_R8G8B8A8_SINT:
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R10G10B10A2_UINT:
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
  case DXGI_FORMAT_R11G11B10_FLOAT:
  case DXGI_FORMAT_R32_FLOAT:
  case DXGI_FORMAT_R32_UINT:
  case DXGI_FORMAT_R32_SINT:
  case DXGI_FORMAT_R32_TYPELESS:
    return 4;
  case DXGI_FORMAT_R16G16_FLOAT:
  case DXGI_FORMAT_R16G16_UINT:
  case DXGI_FORMAT_R16G16_SINT:
  case DXGI_FORMAT_R16G16_UNORM:
  case DXGI_FORMAT_R16G16_TYPELESS:
  case DXGI_FORMAT_R32G32_FLOAT:
  case DXGI_FORMAT_R32G32_UINT:
  case DXGI_FORMAT_R32G32_SINT:
  case DXGI_FORMAT_R32G32_TYPELESS:
    return 8;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
  case DXGI_FORMAT_R16G16B16A16_UINT:
  case DXGI_FORMAT_R16G16B16A16_SINT:
  case DXGI_FORMAT_R16G16B16A16_UNORM:
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
  case DXGI_FORMAT_R32G32B32A32_UINT:
  case DXGI_FORMAT_R32G32B32A32_SINT:
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    return 16;
  default:
    return 0;
  }
}

static UINT Align256(UINT value) {
  return (value + 255u) & ~255u;
}

} // namespace

MTLD3D12DXGISurface::MTLD3D12DXGISurface(
    IMTLDXGIDevice *device, MTLD3D12Resource *resource, UINT subresource,
    const DXGI_SURFACE_DESC &desc)
    : m_device(device), m_resource(resource), m_subresource(subresource),
      m_desc(desc) {
  if (m_device)
    m_device->AddRef();
  if (m_resource)
    m_resource->AddRef();
  const UINT bpp = BytesPerPixel();
  if (bpp && m_desc.Width <= UINT_MAX / bpp)
    m_row_pitch = Align256(m_desc.Width * bpp);
}

MTLD3D12DXGISurface::~MTLD3D12DXGISurface() {
  if (m_resource)
    m_resource->Release();
  if (m_device)
    m_device->Release();
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::QueryInterface(REFIID riid,
                                                               void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDXGIObject) ||
      riid == __uuidof(IDXGIDeviceSubObject) ||
      riid == __uuidof(IDXGISurface) || riid == __uuidof(IDXGISurface1) ||
      riid == __uuidof(IDXGISurface2)) {
    *object = static_cast<IDXGISurface2 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12DXGISurface::AddRef() { return ++m_ref_count; }

ULONG STDMETHODCALLTYPE MTLD3D12DXGISurface::Release() {
  const ULONG ref = --m_ref_count;
  if (!ref)
    delete this;
  return ref;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::SetPrivateData(
    REFGUID guid, UINT size, const void *data) {
  return m_private_data.setData(guid, size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::SetPrivateDataInterface(
    REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetPrivateData(
    REFGUID guid, UINT *size, void *data) {
  return m_private_data.getData(guid, size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetParent(REFIID riid,
                                                          void **parent) {
  if (!parent)
    return E_POINTER;
  *parent = nullptr;
  return m_device ? m_device->QueryInterface(riid, parent)
                  : DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetDevice(REFIID riid,
                                                           void **device) {
  return GetParent(riid, device);
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetDesc(DXGI_SURFACE_DESC *desc) {
  if (!desc)
    return DXGI_ERROR_INVALID_CALL;
  *desc = m_desc;
  return S_OK;
}

UINT MTLD3D12DXGISurface::BytesPerPixel() const {
  return SurfaceBytesPerPixel(m_desc.Format);
}

HRESULT MTLD3D12DXGISurface::CopyResourceToShadow() {
  if (!m_resource || !m_row_pitch || !m_desc.Height)
    return DXGI_ERROR_UNSUPPORTED;
  const uint64_t bytes = uint64_t(m_row_pitch) * m_desc.Height;
  if (bytes > SIZE_MAX)
    return E_OUTOFMEMORY;
  m_shadow.assign(static_cast<size_t>(bytes), 0);
  return m_resource->ReadFromSubresource(m_shadow.data(), m_row_pitch,
                                         m_row_pitch * m_desc.Height,
                                         m_subresource, nullptr);
}

HRESULT MTLD3D12DXGISurface::CopyShadowToResource() {
  if (!m_resource || m_shadow.empty())
    return DXGI_ERROR_UNSUPPORTED;
  return m_resource->WriteToSubresource(
      m_subresource, nullptr, m_shadow.data(), m_row_pitch,
      m_row_pitch * m_desc.Height);
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::Map(
    DXGI_MAPPED_RECT *mapped_rect, UINT flags) {
  if (!mapped_rect)
    return DXGI_ERROR_INVALID_CALL;
  mapped_rect->Pitch = 0;
  mapped_rect->pBits = nullptr;
  constexpr UINT known_flags = DXGI_MAP_READ | DXGI_MAP_WRITE |
                               DXGI_MAP_DISCARD;
  if (flags & ~known_flags)
    return DXGI_ERROR_INVALID_CALL;
  if (m_map_flags || !m_resource || !m_row_pitch || !m_desc.Height)
    return m_map_flags ? DXGI_ERROR_WAS_STILL_DRAWING
                       : DXGI_ERROR_UNSUPPORTED;
  if ((flags & DXGI_MAP_DISCARD) && !(flags & DXGI_MAP_WRITE))
    return DXGI_ERROR_INVALID_CALL;
  if (!(flags & DXGI_MAP_DISCARD)) {
    HRESULT hr = CopyResourceToShadow();
    if (FAILED(hr))
      return hr;
  } else {
    const uint64_t bytes = uint64_t(m_row_pitch) * m_desc.Height;
    if (bytes > SIZE_MAX)
      return E_OUTOFMEMORY;
    m_shadow.assign(static_cast<size_t>(bytes), 0);
  }
  m_map_flags = flags;
  mapped_rect->Pitch = static_cast<INT>(m_row_pitch);
  mapped_rect->pBits = m_shadow.data();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::Unmap() {
  if (!m_map_flags)
    return DXGI_ERROR_INVALID_CALL;
  HRESULT hr = S_OK;
  if (m_map_flags & DXGI_MAP_WRITE)
    hr = CopyShadowToResource();
  m_shadow.clear();
  m_map_flags = 0;
  return hr;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetDC(WINBOOL, HDC *hdc) {
  if (!hdc)
    return E_POINTER;
  *hdc = nullptr;
  return DXGI_ERROR_UNSUPPORTED;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::ReleaseDC(RECT *) {
  return DXGI_ERROR_INVALID_CALL;
}

HRESULT STDMETHODCALLTYPE MTLD3D12DXGISurface::GetResource(
    REFIID iid, void **resource, UINT *subresource) {
  if (!resource || !subresource)
    return E_POINTER;
  *resource = nullptr;
  *subresource = m_subresource;
  return m_resource ? m_resource->QueryInterface(iid, resource)
                    : DXGI_ERROR_INVALID_CALL;
}

IDXGISurface *CreateD3D12DXGISurface(IMTLDXGIDevice *device,
                                     MTLD3D12Resource *resource,
                                     UINT subresource,
                                     const DXGI_SURFACE_DESC &desc) {
  if (!device || !resource || !SurfaceBytesPerPixel(desc.Format) ||
      !desc.Width || !desc.Height ||
      desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0)
    return nullptr;
  return new (std::nothrow)
      MTLD3D12DXGISurface(device, resource, subresource, desc);
}

IDXGISurface2 *CreateD3D12DXGISurfaceFromResource(IUnknown *resource,
                                                  UINT subresource) {
  if (!resource)
    return nullptr;
  ID3D12Resource *d3d_resource = nullptr;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&d3d_resource))))
    return nullptr;
  D3D12_RESOURCE_DESC resource_desc = {};
  d3d_resource->GetDesc(&resource_desc);
  if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
      resource_desc.SampleDesc.Count != 1 || resource_desc.SampleDesc.Quality != 0) {
    d3d_resource->Release();
    return nullptr;
  }
  const UINT mip_levels = std::max<UINT>(resource_desc.MipLevels, 1);
  const UINT array_size = resource_desc.Dimension ==
                                  D3D12_RESOURCE_DIMENSION_TEXTURE3D
                              ? 1
                              : std::max<UINT>(resource_desc.DepthOrArraySize, 1);
  if (subresource >= mip_levels * array_size) {
    d3d_resource->Release();
    return nullptr;
  }
  const UINT mip = subresource % mip_levels;
  DXGI_SURFACE_DESC surface_desc = {};
  surface_desc.Width = static_cast<UINT>(
      std::max<UINT64>(1, resource_desc.Width >> mip));
  surface_desc.Height = static_cast<UINT>(
      resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
          ? 1
          : std::max<UINT>(1, resource_desc.Height >> mip));
  surface_desc.Format = resource_desc.Format;
  surface_desc.SampleDesc = resource_desc.SampleDesc;

  ID3D12Device *d3d_device = nullptr;
  if (FAILED(d3d_resource->GetDevice(IID_PPV_ARGS(&d3d_device)))) {
    d3d_resource->Release();
    return nullptr;
  }
  auto *dxmt_device = static_cast<MTLD3D12Device *>(d3d_device);
  auto *dxgi_device = dxmt_device ? dxmt_device->GetDXGIDevice() : nullptr;
  IDXGISurface2 *surface = nullptr;
  if (dxgi_device)
    surface = static_cast<IDXGISurface2 *>(new (std::nothrow)
        MTLD3D12DXGISurface(dxgi_device,
                            static_cast<MTLD3D12Resource *>(d3d_resource),
                            subresource, surface_desc));
  d3d_device->Release();
  d3d_resource->Release();
  return surface;
}

} // namespace dxmt
