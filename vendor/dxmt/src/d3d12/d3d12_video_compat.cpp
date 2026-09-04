#include "d3d12_video_compat.hpp"

#include "d3d12_device.hpp"

#include <d3d12video.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <vector>

namespace dxmt {
namespace {

class VideoObjectBase {
protected:
  explicit VideoObjectBase(MTLD3D12Device *device) : device_(device) {
    if (device_)
      device_->AddRef();
  }
  ~VideoObjectBase() {
    if (device_)
      device_->Release();
  }
  HRESULT GetDeviceImpl(REFIID riid, void **device) {
    if (!device)
      return E_POINTER;
    *device = nullptr;
    return device_ ? device_->QueryInterface(riid, device)
                   : DXGI_ERROR_INVALID_CALL;
  }
  HRESULT GetPrivateDataImpl(REFGUID guid, UINT *size, void *data) {
    return private_data_.getData(guid, size, data);
  }
  HRESULT SetPrivateDataImpl(REFGUID guid, UINT size, const void *data) {
    return private_data_.setData(guid, size, data);
  }
  HRESULT SetPrivateDataInterfaceImpl(REFGUID guid, const IUnknown *data) {
    return private_data_.setInterface(guid, data);
  }
  HRESULT SetNameImpl(LPCWSTR name) { return private_data_.setName(name); }

  MTLD3D12Device *device_ = nullptr;
  ComPrivateData private_data_;
};

class MTLD3D12VideoDecoder final : public ID3D12VideoDecoderCompat,
                                   private VideoObjectBase {
public:
  MTLD3D12VideoDecoder(MTLD3D12Device *device, const void *desc)
      : VideoObjectBase(device) {
    std::memcpy(desc_.data(), desc, sizeof(desc_));
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == kIID_ID3D12VideoDecoderCompat)
      *object = static_cast<ID3D12VideoDecoderCompat *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override {
    return GetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override {
    return SetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,
                                                     const IUnknown *d) override {
    return SetPrivateDataInterfaceImpl(g, d);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return SetNameImpl(n); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID i, void **d) override {
    return GetDeviceImpl(i, d);
  }
  void *STDMETHODCALLTYPE GetDesc(void *ret) override {
    if (!ret)
      return nullptr;
    std::memcpy(ret, desc_.data(), desc_.size());
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  std::array<uint8_t, 32> desc_ = {};
};

class MTLD3D12VideoDecoderHeap final : public ID3D12VideoDecoderHeapCompat,
                                        private VideoObjectBase {
public:
  MTLD3D12VideoDecoderHeap(MTLD3D12Device *device, const void *desc)
      : VideoObjectBase(device) {
    std::memcpy(desc_.data(), desc, desc_.size());
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == kIID_ID3D12VideoDecoderHeapCompat)
      *object = static_cast<ID3D12VideoDecoderHeapCompat *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override {
    return GetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override {
    return SetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,
                                                     const IUnknown *d) override {
    return SetPrivateDataInterfaceImpl(g, d);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return SetNameImpl(n); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID i, void **d) override {
    return GetDeviceImpl(i, d);
  }
  void *STDMETHODCALLTYPE GetDesc(void *ret) override {
    if (!ret)
      return nullptr;
    std::memcpy(ret, desc_.data(), desc_.size());
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  std::array<uint8_t, 64> desc_ = {};
};

class MTLD3D12VideoProcessor final : public ID3D12VideoProcessorCompat,
                                     private VideoObjectBase {
public:
  MTLD3D12VideoProcessor(MTLD3D12Device *device, const void *output_desc,
                         UINT input_count, const void *input_descs)
      : VideoObjectBase(device), input_count_(input_count) {
    if (output_desc)
      std::memcpy(output_desc_.data(), output_desc, output_desc_.size());
    const auto *bytes = static_cast<const uint8_t *>(input_descs);
    for (UINT i = 0; i < input_count_; ++i) {
      std::array<uint8_t, sizeof(D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC)> value = {};
      if (bytes)
        std::memcpy(value.data(), bytes + size_t(i) * value.size(), value.size());
      input_descs_.push_back(value);
    }
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == kIID_ID3D12VideoProcessorCompat)
      *object = static_cast<ID3D12VideoProcessorCompat *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT *s, void *d) override {
    return GetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void *d) override {
    return SetPrivateDataImpl(g, s, d);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,
                                                     const IUnknown *d) override {
    return SetPrivateDataInterfaceImpl(g, d);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override { return SetNameImpl(n); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID i, void **d) override {
    return GetDeviceImpl(i, d);
  }
  UINT STDMETHODCALLTYPE GetNodeMask() override { return 1; }
  UINT STDMETHODCALLTYPE GetNumInputStreamDescs() override { return input_count_; }
  HRESULT STDMETHODCALLTYPE GetInputStreamDescs(UINT count, void *descs) override {
    if (count != input_count_ || (count && !descs))
      return E_INVALIDARG;
    auto *bytes = static_cast<uint8_t *>(descs);
    for (UINT i = 0; i < count; ++i)
      std::memcpy(bytes + size_t(i) * input_descs_[i].size(),
                  input_descs_[i].data(), input_descs_[i].size());
    return S_OK;
  }
  void *STDMETHODCALLTYPE GetOutputStreamDesc(void *ret) override {
    if (!ret)
      return nullptr;
    std::memcpy(ret, output_desc_.data(), output_desc_.size());
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  UINT input_count_ = 0;
  std::array<uint8_t, sizeof(D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC)> output_desc_ = {};
  std::vector<std::array<uint8_t, sizeof(D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC)>>
      input_descs_;
};

class MTLD3D12VideoDevice final : public ID3D12VideoDeviceCompat {
public:
  explicit MTLD3D12VideoDevice(MTLD3D12Device *device) : device_(device) {
    if (device_)
      device_->AddRef();
  }
  ~MTLD3D12VideoDevice() {
    if (device_)
      device_->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == kIID_ID3D12VideoDeviceCompat) {
      *object = static_cast<ID3D12VideoDeviceCompat *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(UINT feature, void *data,
                                                UINT data_size) override {
    if (!data || !data_size)
      return E_INVALIDARG;
    // The software provider exposes the one-input processing shape. Codec
    // profiles and decode/encode bitstream support remain separate providers.
    if (feature == 6u && data_size >= sizeof(UINT)) {
      *static_cast<UINT *>(data) = 1;
      return S_OK;
    }
    return E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoDecoder(const void *desc, REFIID riid,
                                               void **decoder) override {
    if (!decoder)
      return E_POINTER;
    *decoder = nullptr;
    if (!desc)
      return E_INVALIDARG;
    auto *created = new (std::nothrow) MTLD3D12VideoDecoder(device_, desc);
    if (!created)
      return E_OUTOFMEMORY;
    HRESULT hr = created->QueryInterface(riid, decoder);
    created->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoDecoderHeap(const void *desc,
                                                   REFIID riid,
                                                   void **heap) override {
    if (!heap)
      return E_POINTER;
    *heap = nullptr;
    if (!desc)
      return E_INVALIDARG;
    auto *created = new (std::nothrow) MTLD3D12VideoDecoderHeap(device_, desc);
    if (!created)
      return E_OUTOFMEMORY;
    HRESULT hr = created->QueryInterface(riid, heap);
    created->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessor(
      UINT node_mask, const void *output_desc, UINT input_count,
      const void *input_descs, REFIID riid, void **processor) override {
    if (!processor)
      return E_POINTER;
    *processor = nullptr;
    if ((node_mask != 0 && node_mask != 1) || !output_desc ||
        (input_count && !input_descs) || input_count > 32)
      return E_INVALIDARG;
    auto *created = new (std::nothrow)
        MTLD3D12VideoProcessor(device_, output_desc, input_count, input_descs);
    if (!created)
      return E_OUTOFMEMORY;
    HRESULT hr = created->QueryInterface(riid, processor);
    created->Release();
    return hr;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  MTLD3D12Device *device_ = nullptr;
};

} // namespace

HRESULT CreateD3D12VideoDevice(MTLD3D12Device *device, REFIID riid,
                               void **video_device) {
  if (!video_device)
    return E_POINTER;
  *video_device = nullptr;
  if (!device)
    return E_INVALIDARG;
  auto *created = new (std::nothrow) MTLD3D12VideoDevice(device);
  if (!created)
    return E_OUTOFMEMORY;
  HRESULT hr = created->QueryInterface(riid, video_device);
  created->Release();
  return hr;
}

} // namespace dxmt
