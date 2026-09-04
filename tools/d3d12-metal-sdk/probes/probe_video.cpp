#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static constexpr GUID kIIDVideoDevice = {
    0x1f052807, 0x0b46, 0x4acc,
    {0x8a, 0x89, 0x36, 0x4f, 0x79, 0x37, 0x18, 0xa4}};
static constexpr GUID kIIDVideoDecoder = {
    0xc59b6bdc, 0x7720, 0x4074,
    {0xa1, 0x36, 0x17, 0xa1, 0x56, 0x03, 0x74, 0x70}};
static constexpr GUID kIIDVideoDecoderHeap = {
    0x0946b7c9, 0xebf6, 0x4047,
    {0xbb, 0x73, 0x86, 0x83, 0xe2, 0x7d, 0xbb, 0x1f}};
static constexpr GUID kIIDVideoProcessor = {
    0x304fdb32, 0xbede, 0x410a,
    {0x85, 0x45, 0x94, 0x3a, 0xc6, 0xa4, 0x61, 0x38}};
static constexpr GUID kIIDUnknown = {
    0x00000000, 0x0000, 0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct VideoDeviceCompat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CheckFeatureSupport(UINT, void*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVideoDecoder(const void*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVideoDecoderHeap(const void*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateVideoProcessor(UINT, const void*, UINT,
                                                            const void*, REFIID, void**) = 0;
};
struct VideoObjectCompat : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) = 0;
};
struct VideoDecoderCompat : public VideoObjectCompat {
    virtual void* STDMETHODCALLTYPE GetDesc(void*) = 0;
};
struct VideoDecoderHeapCompat : public VideoObjectCompat {
    virtual void* STDMETHODCALLTYPE GetDesc(void*) = 0;
};
struct VideoProcessorCompat : public VideoObjectCompat {
    virtual UINT STDMETHODCALLTYPE GetNodeMask() = 0;
    virtual UINT STDMETHODCALLTYPE GetNumInputStreamDescs() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInputStreamDescs(UINT, void*) = 0;
    virtual void* STDMETHODCALLTYPE GetOutputStreamDesc(void*) = 0;
};

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

template <typename T> static void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
template <typename T> static T load_proc(HMODULE module, const char* name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static unsigned long hr_value(HRESULT hr) {
    return static_cast<unsigned long>(static_cast<uint32_t>(hr));
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device* device = nullptr;
    VideoDeviceCompat* video = nullptr;
    VideoDecoderCompat* decoder = nullptr;
    VideoDecoderHeapCompat* decoder_heap = nullptr;
    VideoProcessorCompat* processor = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT video_hr = create_hr == S_OK
                           ? device->QueryInterface(kIIDVideoDevice,
                                                    reinterpret_cast<void**>(&video))
                           : create_hr;
    UINT max_inputs = 0;
    HRESULT feature_hr = video_hr == S_OK
                             ? video->CheckFeatureSupport(6u, &max_inputs,
                                                          sizeof(max_inputs))
                             : video_hr;
    UINT invalid_value = 0;
    HRESULT invalid_feature_hr = video_hr == S_OK
                                     ? video->CheckFeatureSupport(0u, &invalid_value,
                                                                  sizeof(invalid_value))
                                     : video_hr;

    D3D12_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.NodeMask = 1;
    HRESULT decoder_hr = video_hr == S_OK
                             ? video->CreateVideoDecoder(&decoder_desc,
                                                         kIIDVideoDecoder,
                                                         reinterpret_cast<void**>(&decoder))
                             : video_hr;
    D3D12_VIDEO_DECODER_DESC decoder_copy = {};
    if (decoder_hr == S_OK)
        decoder->GetDesc(&decoder_copy);

    D3D12_VIDEO_DECODER_HEAP_DESC heap_desc = {};
    heap_desc.NodeMask = 1;
    heap_desc.DecodeWidth = 64;
    heap_desc.DecodeHeight = 64;
    heap_desc.Format = DXGI_FORMAT_NV12;
    HRESULT heap_hr = video_hr == S_OK
                          ? video->CreateVideoDecoderHeap(&heap_desc,
                                                          kIIDVideoDecoderHeap,
                                                          reinterpret_cast<void**>(&decoder_heap))
                          : video_hr;
    D3D12_VIDEO_DECODER_HEAP_DESC heap_copy = {};
    if (heap_hr == S_OK)
        decoder_heap->GetDesc(&heap_copy);

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc = {};
    output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_desc = {};
    input_desc.Format = DXGI_FORMAT_NV12;
    HRESULT processor_hr = video_hr == S_OK
                               ? video->CreateVideoProcessor(1, &output_desc, 1,
                                                             &input_desc,
                                                             kIIDVideoProcessor,
                                                             reinterpret_cast<void**>(&processor))
                               : video_hr;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_copy = {};
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_copy = {};
    HRESULT input_desc_hr = processor_hr == S_OK
                                ? processor->GetInputStreamDescs(1, &input_copy)
                                : processor_hr;
    if (processor_hr == S_OK)
        processor->GetOutputStreamDesc(&output_copy);

    HRESULT decoder_unknown_hr = video_hr == S_OK
                                     ? video->CreateVideoDecoder(&decoder_desc,
                                                                 kIIDUnknown, nullptr)
                                     : video_hr;
    const bool descriptor_roundtrip =
        std::memcmp(&decoder_desc, &decoder_copy, sizeof(decoder_desc)) == 0 &&
        std::memcmp(&heap_desc, &heap_copy, sizeof(heap_desc)) == 0 &&
        std::memcmp(&output_desc, &output_copy, sizeof(output_desc)) == 0 &&
        std::memcmp(&input_desc, &input_copy, sizeof(input_desc)) == 0;
    const bool passed = create_hr == S_OK && video_hr == S_OK && feature_hr == S_OK &&
                        max_inputs == 1 && invalid_feature_hr == E_INVALIDARG &&
                        decoder_hr == S_OK && heap_hr == S_OK && processor_hr == S_OK &&
                        input_desc_hr == S_OK && processor->GetNumInputStreamDescs() == 1 &&
                        descriptor_roundtrip;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.video.v1\",\n");
    std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
    std::printf("  \"create_device\": \"0x%08lx\",\n", hr_value(create_hr));
    std::printf("  \"query_video_device\": \"0x%08lx\",\n", hr_value(video_hr));
    std::printf("  \"check_feature_max_input_streams\": \"0x%08lx\",\n",
                hr_value(feature_hr));
    std::printf("  \"max_input_streams\": %u,\n", max_inputs);
    std::printf("  \"invalid_feature\": \"0x%08lx\",\n", hr_value(invalid_feature_hr));
    std::printf("  \"create_decoder\": \"0x%08lx\",\n", hr_value(decoder_hr));
    std::printf("  \"create_decoder_heap\": \"0x%08lx\",\n", hr_value(heap_hr));
    std::printf("  \"create_processor\": \"0x%08lx\",\n", hr_value(processor_hr));
    std::printf("  \"input_descs\": \"0x%08lx\",\n", hr_value(input_desc_hr));
    std::printf("  \"unknown_output_null_guard\": \"0x%08lx\",\n",
                hr_value(decoder_unknown_hr));
    std::printf("  \"descriptor_roundtrip\": %s,\n",
                descriptor_roundtrip ? "true" : "false");
    std::printf("  \"gpu_execution\": false,\n");
    std::printf("  \"software_provider_scope\": \"object-and-feature-contract-only\"\n");
    std::printf("}\n");

    release(processor);
    release(decoder_heap);
    release(decoder);
    release(video);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
