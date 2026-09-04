#pragma once

#include "com/com_private_data.hpp"
#include "d3d12.h"
#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#define DXMT_VIDEO_COMPAT_UNDEF_WIDL_AGGREGATE_RETURNS
#endif
#include <d3d12video.h>
#ifdef DXMT_VIDEO_COMPAT_UNDEF_WIDL_AGGREGATE_RETURNS
#undef WIDL_EXPLICIT_AGGREGATE_RETURNS
#undef DXMT_VIDEO_COMPAT_UNDEF_WIDL_AGGREGATE_RETURNS
#endif
#include <atomic>
#include <cstdint>

namespace dxmt {

class MTLD3D12Device;

inline constexpr GUID kIID_ID3D12VideoDeviceCompat = {
    0x1f052807, 0x0b46, 0x4acc,
    {0x8a, 0x89, 0x36, 0x4f, 0x79, 0x37, 0x18, 0xa4}};
inline constexpr GUID kIID_ID3D12VideoDecoderCompat = {
    0xc59b6bdc, 0x7720, 0x4074,
    {0xa1, 0x36, 0x17, 0xa1, 0x56, 0x03, 0x74, 0x70}};
inline constexpr GUID kIID_ID3D12VideoDecoderHeapCompat = {
    0x0946b7c9, 0xebf6, 0x4047,
    {0xbb, 0x73, 0x86, 0x83, 0xe2, 0x7d, 0xbb, 0x1f}};
inline constexpr GUID kIID_ID3D12VideoProcessorCompat = {
    0x304fdb32, 0xbede, 0x410a,
    {0x85, 0x45, 0x94, 0x3a, 0xc6, 0xa4, 0x61, 0x38}};
inline constexpr GUID kIID_ID3D12VideoProcessCommandListCompat = {
    0xa9a5f0cc, 0x4f38, 0x4a7b,
    {0x9e, 0x62, 0x6d, 0x0e, 0x2e, 0xc5, 0x5a, 0x91}};

struct ID3D12VideoDeviceCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(
      D3D12_FEATURE_VIDEO feature, void *data, UINT data_size) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVideoDecoder(
      const D3D12_VIDEO_DECODER_DESC *desc, REFIID riid, void **decoder) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVideoDecoderHeap(
      const D3D12_VIDEO_DECODER_HEAP_DESC *desc, REFIID riid, void **heap) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVideoProcessor(
      UINT node_mask,
      const D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *output_desc,
      UINT input_count,
      const D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *input_descs, REFIID riid,
      void **processor) = 0;
};

// The object interfaces below use the exact ID3D12Pageable vtable prefix and
// the Windows hidden-return-pointer ABI for descriptor getters.  Descriptor
// payloads are retained as fixed byte arrays so this compatibility layer does
// not need to import the optional 1.619.5 video header into the PE build.
struct ID3D12VideoObjectCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
                                                             const IUnknown *) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void **) = 0;
};

struct ID3D12VideoDecoderCompat : public ID3D12VideoObjectCompat {
  virtual D3D12_VIDEO_DECODER_DESC *STDMETHODCALLTYPE GetDesc(
      D3D12_VIDEO_DECODER_DESC *ret) = 0;
};
struct ID3D12VideoDecoderHeapCompat : public ID3D12VideoObjectCompat {
  virtual D3D12_VIDEO_DECODER_HEAP_DESC *STDMETHODCALLTYPE GetDesc(
      D3D12_VIDEO_DECODER_HEAP_DESC *ret) = 0;
};
struct ID3D12VideoProcessorCompat : public ID3D12VideoObjectCompat {
  virtual UINT STDMETHODCALLTYPE GetNodeMask() = 0;
  virtual UINT STDMETHODCALLTYPE GetNumInputStreamDescs() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetInputStreamDescs(
      UINT count, D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC *descs) = 0;
  virtual D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *STDMETHODCALLTYPE
  GetOutputStreamDesc(D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC *ret) = 0;
};

// This private extension lets the video-process command-list adapter hand
// recorded CPU-provider operations to the video queue without teaching the
// graphics replay stream to reinterpret a video vtable.
struct ID3D12VideoProcessCommandListCompat : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE ExecuteVideoOperations() = 0;
};

HRESULT CreateD3D12VideoDevice(MTLD3D12Device *device, REFIID riid,
                               void **video_device);
HRESULT CreateD3D12VideoProcessCommandList(
    MTLD3D12Device *device, ID3D12CommandAllocator *allocator, REFIID riid,
    void **command_list);

} // namespace dxmt
