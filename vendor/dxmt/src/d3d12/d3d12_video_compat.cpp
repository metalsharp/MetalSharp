#include "d3d12_video_compat.hpp"

#include "d3d12_device.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_resource.hpp"

#include <d3d12video.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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
                                   public ID3D12VideoDecoder,
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
        riid == kIID_ID3D12VideoDecoderCompat || riid == IID_ID3D12VideoDecoder)
      *object = static_cast<ID3D12VideoDecoder *>(this);
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
  D3D12_VIDEO_DECODER_DESC *STDMETHODCALLTYPE GetDesc(
      D3D12_VIDEO_DECODER_DESC *ret) override {
    if (!ret)
      return nullptr;
    std::memcpy(ret, desc_.data(), desc_.size());
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  std::array<uint8_t, sizeof(D3D12_VIDEO_DECODER_DESC)> desc_ = {};
};

class MTLD3D12VideoDecoderHeap final : public ID3D12VideoDecoderHeapCompat,
                                        public ID3D12VideoDecoderHeap,
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
      *object = static_cast<ID3D12VideoDecoderHeap *>(this);
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
  D3D12_VIDEO_DECODER_HEAP_DESC *STDMETHODCALLTYPE GetDesc(
      D3D12_VIDEO_DECODER_HEAP_DESC *ret) override {
    if (!ret)
      return nullptr;
    std::memcpy(ret, desc_.data(), desc_.size());
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  std::array<uint8_t, sizeof(D3D12_VIDEO_DECODER_HEAP_DESC)> desc_ = {};
};

class MTLD3D12VideoProcessor final : public ID3D12VideoProcessorCompat,
                                     public ID3D12VideoProcessor,
                                     private VideoObjectBase {
public:
  MTLD3D12VideoProcessor(
      MTLD3D12Device *device,
      const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *output_desc,
      UINT input_count,
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *input_descs)
      : VideoObjectBase(device), input_count_(input_count) {
    if (output_desc)
      output_desc_ = *output_desc;
    if (input_descs)
      input_descs_.assign(input_descs, input_descs + input_count_);
    else
      input_descs_.resize(input_count_);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == kIID_ID3D12VideoProcessorCompat ||
        riid == IID_ID3D12VideoProcessor)
      *object = static_cast<ID3D12VideoProcessor *>(this);
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
  HRESULT STDMETHODCALLTYPE GetInputStreamDescs(
      UINT count, D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *descs) override {
    if (count != input_count_ || (count && !descs))
      return E_INVALIDARG;
    for (UINT i = 0; i < count; ++i)
      descs[i] = input_descs_[i];
    return S_OK;
  }
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *STDMETHODCALLTYPE
  GetOutputStreamDesc(D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *ret) override {
    if (!ret)
      return nullptr;
    *ret = output_desc_;
    return ret;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  UINT input_count_ = 0;
  D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc_ = {};
  std::vector<D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC> input_descs_;
};

struct VideoProcessOperation {
  Com<IUnknown> processor;
  Com<ID3D12Resource> input;
  Com<ID3D12Resource> output;
  UINT input_subresource = 0;
  UINT output_subresource = 0;
  D3D12_VIDEO_PROCESS_TRANSFORM transform = {};
  D3D12_RECT target = {};
};

static bool VideoFormatBytesPerPixel(DXGI_FORMAT format, UINT *bytes) {
  if (!bytes)
    return false;
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R10G10B10A2_UNORM:
  case DXGI_FORMAT_R11G11B10_FLOAT:
    *bytes = 4;
    return true;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    *bytes = 8;
    return true;
  default:
    return false;
  }
}

static HRESULT CopyVideoProcessFrame(const VideoProcessOperation &operation) {
  if (!operation.processor || !operation.input || !operation.output)
    return E_INVALIDARG;
  if (operation.transform.Orientation != D3D12_VIDEO_PROCESS_ORIENTATION_DEFAULT)
    return E_NOTIMPL;
  auto *input = static_cast<MTLD3D12Resource *>(operation.input.ptr());
  auto *output = static_cast<MTLD3D12Resource *>(operation.output.ptr());
  if (!input || !output)
    return E_INVALIDARG;
  D3D12_RESOURCE_DESC input_desc = {};
  D3D12_RESOURCE_DESC output_desc = {};
  input->GetDesc(&input_desc);
  output->GetDesc(&output_desc);
  if (input_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      input_desc.SampleDesc.Count != 1 || output_desc.SampleDesc.Count != 1)
    return E_NOTIMPL;
  UINT input_bpp = 0;
  UINT output_bpp = 0;
  if (!VideoFormatBytesPerPixel(input_desc.Format, &input_bpp) ||
      !VideoFormatBytesPerPixel(output_desc.Format, &output_bpp) ||
      input_bpp != output_bpp)
    return E_NOTIMPL;
  const UINT input_mips = input_desc.MipLevels ? input_desc.MipLevels : 1;
  const UINT output_mips = output_desc.MipLevels ? output_desc.MipLevels : 1;
  const UINT input_arrays = input_desc.DepthOrArraySize ? input_desc.DepthOrArraySize : 1;
  const UINT output_arrays = output_desc.DepthOrArraySize ? output_desc.DepthOrArraySize : 1;
  if (operation.input_subresource >= input_mips * input_arrays ||
      operation.output_subresource >= output_mips * output_arrays)
    return E_INVALIDARG;
  const UINT input_mip = operation.input_subresource % input_mips;
  const UINT output_mip = operation.output_subresource % output_mips;
  const UINT input_width = std::max<UINT>(1, input_desc.Width >> input_mip);
  const UINT input_height = std::max<UINT>(1, input_desc.Height >> input_mip);
  const UINT output_width = std::max<UINT>(1, output_desc.Width >> output_mip);
  const UINT output_height = std::max<UINT>(1, output_desc.Height >> output_mip);
  const uint64_t input_row64 = uint64_t(input_width) * input_bpp;
  const uint64_t output_row64 = uint64_t(output_width) * output_bpp;
  if (input_row64 > UINT32_MAX || output_row64 > UINT32_MAX)
    return E_OUTOFMEMORY;
  const UINT input_row = static_cast<UINT>(input_row64);
  const UINT output_row = static_cast<UINT>(output_row64);
  const uint64_t input_size = input_row64 * input_height;
  const uint64_t output_size = output_row64 * output_height;
  if (input_size > SIZE_MAX || output_size > SIZE_MAX)
    return E_OUTOFMEMORY;
  std::vector<uint8_t> input_pixels;
  std::vector<uint8_t> output_pixels;
  try {
    input_pixels.resize(static_cast<size_t>(input_size));
    output_pixels.resize(static_cast<size_t>(output_size));
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  HRESULT hr = input->ReadFromSubresource(
      input_pixels.data(), input_row, input_row * input_height,
      operation.input_subresource, nullptr);
  if (FAILED(hr))
    return hr;
  hr = output->ReadFromSubresource(
      output_pixels.data(), output_row, output_row * output_height,
      operation.output_subresource, nullptr);
  if (FAILED(hr))
    return hr;

  RECT source = {0, 0, static_cast<LONG>(input_width),
                 static_cast<LONG>(input_height)};
  RECT destination = {0, 0, static_cast<LONG>(output_width),
                      static_cast<LONG>(output_height)};
  const auto &transform = operation.transform;
  if (transform.SourceRectangle.right > transform.SourceRectangle.left &&
      transform.SourceRectangle.bottom > transform.SourceRectangle.top)
    source = transform.SourceRectangle;
  if (transform.DestinationRectangle.right > transform.DestinationRectangle.left &&
      transform.DestinationRectangle.bottom > transform.DestinationRectangle.top)
    destination = transform.DestinationRectangle;
  if (operation.target.right > operation.target.left &&
      operation.target.bottom > operation.target.top)
    destination = operation.target;
  if (source.left < 0 || source.top < 0 || source.right > static_cast<LONG>(input_width) ||
      source.bottom > static_cast<LONG>(input_height) || destination.left < 0 ||
      destination.top < 0 || destination.right > static_cast<LONG>(output_width) ||
      destination.bottom > static_cast<LONG>(output_height))
    return E_INVALIDARG;
  const UINT source_width = static_cast<UINT>(source.right - source.left);
  const UINT source_height = static_cast<UINT>(source.bottom - source.top);
  const UINT destination_width = static_cast<UINT>(destination.right - destination.left);
  const UINT destination_height = static_cast<UINT>(destination.bottom - destination.top);
  if (!source_width || !source_height || !destination_width || !destination_height)
    return E_INVALIDARG;
  for (UINT y = 0; y < destination_height; ++y) {
    const UINT source_y = static_cast<UINT>(source.top) +
                          (uint64_t(y) * source_height) / destination_height;
    for (UINT x = 0; x < destination_width; ++x) {
      const UINT source_x = static_cast<UINT>(source.left) +
                            (uint64_t(x) * source_width) / destination_width;
      const auto *src = input_pixels.data() + uint64_t(source_y) * input_row +
                        uint64_t(source_x) * input_bpp;
      auto *dst = output_pixels.data() +
                  uint64_t(destination.top + y) * output_row +
                  uint64_t(destination.left + x) * output_bpp;
      std::memcpy(dst, src, output_bpp);
    }
  }
  return output->WriteToSubresource(operation.output_subresource, nullptr,
                                    output_pixels.data(), output_row,
                                    output_row * output_height);
}

class MTLD3D12VideoProcessCommandList final
    : public ID3D12VideoProcessCommandList,
      public ID3D12VideoProcessCommandListCompat {
public:
  MTLD3D12VideoProcessCommandList(MTLD3D12Device *device,
                                  ID3D12CommandAllocator *allocator)
      : graphics_(new (std::nothrow) MTLD3D12GraphicsCommandList(
            device, static_cast<MTLD3D12CommandAllocator *>(allocator),
            D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, nullptr)) {}
  ~MTLD3D12VideoProcessCommandList() {
    if (graphics_)
      graphics_->Release();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12CommandList ||
        riid == IID_ID3D12VideoProcessCommandList)
      *object = static_cast<ID3D12VideoProcessCommandList *>(this);
    else if (riid == kIID_ID3D12VideoProcessCommandListCompat)
      *object = static_cast<ID3D12VideoProcessCommandListCompat *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_count_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG ref = --ref_count_;
    if (!ref)
      delete this;
    return ref;
  }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size,
                                            void *data) override {
    return graphics_ ? graphics_->GetPrivateData(guid, size, data) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size,
                                            const void *data) override {
    return graphics_ ? graphics_->SetPrivateData(guid, size, data) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                     const IUnknown *data) override {
    return graphics_ ? graphics_->SetPrivateDataInterface(guid, data) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
    return graphics_ ? graphics_->SetName(name) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return graphics_ ? graphics_->GetDevice(riid, device) : E_FAIL;
  }
  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
    return D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
  }
  HRESULT STDMETHODCALLTYPE Close() override {
    return graphics_ ? graphics_->Close() : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator) override {
    if (!graphics_)
      return E_FAIL;
    operations_.clear();
    return graphics_->Reset(static_cast<MTLD3D12CommandAllocator *>(allocator),
                            nullptr);
  }
  void STDMETHODCALLTYPE ClearState() override {
    if (graphics_)
      graphics_->ClearState(nullptr);
  }
  void STDMETHODCALLTYPE ResourceBarrier(UINT count,
                                         const D3D12_RESOURCE_BARRIER *barriers) override {
    if (graphics_)
      graphics_->ResourceBarrier(count, barriers);
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *resource,
                                          const D3D12_DISCARD_REGION *region) override {
    if (graphics_)
      graphics_->DiscardResource(resource, region);
  }
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type,
                                    UINT index) override {
    if (graphics_)
      graphics_->BeginQuery(heap, type, index);
  }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type,
                                  UINT index) override {
    if (graphics_)
      graphics_->EndQuery(heap, type, index);
  }
  void STDMETHODCALLTYPE ResolveQueryData(
      ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT start_index,
      UINT count, ID3D12Resource *destination, UINT64 offset) override {
    if (graphics_)
      graphics_->ResolveQueryData(heap, type, start_index, count, destination,
                                  offset);
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *buffer, UINT64 offset,
                                        D3D12_PREDICATION_OP operation) override {
    if (graphics_)
      graphics_->SetPredication(buffer, offset, operation);
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data,
                                   UINT size) override {
    if (graphics_)
      graphics_->SetMarker(metadata, data, size);
  }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data,
                                    UINT size) override {
    if (graphics_)
      graphics_->BeginEvent(metadata, data, size);
  }
  void STDMETHODCALLTYPE EndEvent() override {
    if (graphics_)
      graphics_->EndEvent();
  }
  void STDMETHODCALLTYPE ProcessFrames(
      ID3D12VideoProcessor *processor,
      const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS *output_arguments,
      UINT input_count,
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS *input_arguments) override {
    if (!graphics_ || graphics_->IsClosed() || !processor || !output_arguments ||
        !input_arguments || input_count != 1)
      return;
    const auto &input = input_arguments[0].InputStream[0];
    const auto &output = output_arguments->OutputStream[0];
    if (!input.pTexture2D || !output.pTexture2D)
      return;
    VideoProcessOperation operation;
    operation.processor = static_cast<IUnknown *>(processor);
    operation.input = input.pTexture2D;
    operation.output = output.pTexture2D;
    operation.input_subresource = input.Subresource;
    operation.output_subresource = output.Subresource;
    operation.transform = input_arguments[0].Transform;
    operation.target = output_arguments->TargetRectangle;
    try {
      operations_.push_back(std::move(operation));
    } catch (const std::bad_alloc &) {
      // A void command-list method has no HRESULT channel.  Leave the
      // operation absent; queue execution reports a provider failure only
      // for operations that were successfully recorded.
    }
  }
  void STDMETHODCALLTYPE WriteBufferImmediate(
      UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
      const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override {
    if (graphics_)
      graphics_->WriteBufferImmediate(count, parameters, modes);
  }
  HRESULT STDMETHODCALLTYPE ExecuteVideoOperations() override {
    if (!graphics_ || !graphics_->IsClosed())
      return E_INVALIDARG;
    for (const auto &operation : operations_) {
      HRESULT hr = CopyVideoProcessFrame(operation);
      if (FAILED(hr))
        return hr;
    }
    return S_OK;
  }

private:
  std::atomic<ULONG> ref_count_ = {1};
  MTLD3D12GraphicsCommandList *graphics_ = nullptr;
  std::vector<VideoProcessOperation> operations_;
};

class MTLD3D12VideoDevice final : public ID3D12VideoDeviceCompat,
                                  public ID3D12VideoDevice {
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
    if (riid == IID_IUnknown || riid == kIID_ID3D12VideoDeviceCompat ||
        riid == IID_ID3D12VideoDevice) {
      *object = static_cast<ID3D12VideoDevice *>(this);
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
  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE_VIDEO feature,
                                                void *data,
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
  HRESULT STDMETHODCALLTYPE CreateVideoDecoder(
      const D3D12_VIDEO_DECODER_DESC *desc, REFIID riid,
      void **decoder) override {
    if (!decoder)
      return E_POINTER;
    *decoder = nullptr;
    if (!desc || desc->NodeMask != 1)
      return E_INVALIDARG;
    auto *created = new (std::nothrow) MTLD3D12VideoDecoder(device_, desc);
    if (!created)
      return E_OUTOFMEMORY;
    HRESULT hr = created->QueryInterface(riid, decoder);
    created->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoDecoderHeap(
      const D3D12_VIDEO_DECODER_HEAP_DESC *desc, REFIID riid,
      void **heap) override {
    if (!heap)
      return E_POINTER;
    *heap = nullptr;
    if (!desc || desc->NodeMask != 1 || !desc->DecodeWidth ||
        !desc->DecodeHeight)
      return E_INVALIDARG;
    auto *created = new (std::nothrow) MTLD3D12VideoDecoderHeap(device_, desc);
    if (!created)
      return E_OUTOFMEMORY;
    HRESULT hr = created->QueryInterface(riid, heap);
    created->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE CreateVideoProcessor(
      UINT node_mask,
      const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *output_desc,
      UINT input_count,
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *input_descs, REFIID riid,
      void **processor) override {
    if (!processor)
      return E_POINTER;
    *processor = nullptr;
    UINT output_bytes = 0;
    if (node_mask != 1 || !output_desc || !input_count ||
        (input_count && !input_descs) || input_count > 2 ||
        !VideoFormatBytesPerPixel(output_desc->Format, &output_bytes))
      return E_INVALIDARG;
    for (UINT i = 0; i < input_count; ++i) {
      UINT input_bytes = 0;
      const bool known_input =
          VideoFormatBytesPerPixel(input_descs[i].Format, &input_bytes) ||
          input_descs[i].Format == DXGI_FORMAT_NV12 ||
          input_descs[i].Format == DXGI_FORMAT_P010;
      if (!known_input)
        return E_INVALIDARG;
    }
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

HRESULT CreateD3D12VideoProcessCommandList(
    MTLD3D12Device *device, ID3D12CommandAllocator *allocator, REFIID riid,
    void **command_list) {
  if (!command_list)
    return E_POINTER;
  *command_list = nullptr;
  if (!device)
    return E_INVALIDARG;
  auto *created = new (std::nothrow)
      MTLD3D12VideoProcessCommandList(device, allocator);
  if (!created)
    return E_OUTOFMEMORY;
  HRESULT hr = created->QueryInterface(riid, command_list);
  created->Release();
  return hr;
}

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
