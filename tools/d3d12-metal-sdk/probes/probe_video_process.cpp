#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12video.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr GUID kIIDVideoDevice = {
    0x1f052807, 0x0b46, 0x4acc,
    {0x8a, 0x89, 0x36, 0x4f, 0x79, 0x37, 0x18, 0xa4}};
static constexpr GUID kIIDVideoProcessor = {
    0x304fdb32, 0xbede, 0x410a,
    {0x85, 0x45, 0x94, 0x3a, 0xc6, 0xa4, 0x61, 0x38}};

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

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
template <typename T> static void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device* device = nullptr;
    ID3D12VideoDevice* video = nullptr;
    ID3D12VideoProcessor* processor = nullptr;
    ID3D12VideoProcessor* nv12_processor = nullptr;
    ID3D12VideoProcessor* rgba_to_nv12_processor = nullptr;
    ID3D12VideoProcessor* p010_processor = nullptr;
    ID3D12VideoProcessor* p010_output_processor = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12VideoProcessCommandList* command_list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    ID3D12Resource* input = nullptr;
    ID3D12Resource* nv12_input = nullptr;
    ID3D12Resource* rgba_gray_input = nullptr;
    ID3D12Resource* nv12_output = nullptr;
    ID3D12Resource* p010_input = nullptr;
    ID3D12Resource* output = nullptr;

    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    HRESULT video_hr = create_hr == S_OK
                           ? device->QueryInterface(kIIDVideoDevice,
                                                    reinterpret_cast<void**>(&video))
                           : create_hr;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC output_desc = {};
    output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC input_desc = {};
    input_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    HRESULT processor_hr = video_hr == S_OK
                               ? video->CreateVideoProcessor(1, &output_desc, 1,
                                                             &input_desc,
                                                             kIIDVideoProcessor,
                                                             reinterpret_cast<void**>(&processor))
                               : video_hr;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC nv12_input_desc = input_desc;
    nv12_input_desc.Format = DXGI_FORMAT_NV12;
    HRESULT nv12_processor_hr = video_hr == S_OK
                                    ? video->CreateVideoProcessor(
                                          1, &output_desc, 1,
                                          &nv12_input_desc, kIIDVideoProcessor,
                                          reinterpret_cast<void**>(&nv12_processor))
                                    : video_hr;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC nv12_output_desc = output_desc;
    nv12_output_desc.Format = DXGI_FORMAT_NV12;
    HRESULT rgba_to_nv12_processor_hr = video_hr == S_OK
                                             ? video->CreateVideoProcessor(
                                                   1, &nv12_output_desc, 1,
                                                   &input_desc,
                                                   kIIDVideoProcessor,
                                                   reinterpret_cast<void**>(
                                                       &rgba_to_nv12_processor))
                                             : video_hr;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_DESC p010_input_desc = input_desc;
    p010_input_desc.Format = DXGI_FORMAT_P010;
    HRESULT p010_processor_hr = video_hr == S_OK
                                    ? video->CreateVideoProcessor(
                                          1, &output_desc, 1, &p010_input_desc,
                                          kIIDVideoProcessor,
                                          reinterpret_cast<void**>(&p010_processor))
                                    : video_hr;
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_DESC p010_output_desc = output_desc;
    p010_output_desc.Format = DXGI_FORMAT_P010;
    HRESULT p010_output_reject_hr = video_hr == S_OK
                                         ? video->CreateVideoProcessor(
                                               1, &p010_output_desc, 1,
                                               &input_desc, kIIDVideoProcessor,
                                               reinterpret_cast<void**>(
                                                   &p010_output_processor))
                                         : video_hr;
    const bool p010_output_rejected =
        p010_output_reject_hr == E_INVALIDARG && !p010_output_processor;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS;
    HRESULT queue_hr = device && processor
                           ? device->CreateCommandQueue(&queue_desc,
                                                        IID_PPV_ARGS(&queue))
                           : E_FAIL;
    HRESULT allocator_hr = device && queue
                               ? device->CreateCommandAllocator(
                                     D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                                     IID_PPV_ARGS(&allocator))
                               : E_FAIL;
    HRESULT list_hr = device && allocator
                          ? device->CreateCommandList(
                                0, D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS,
                                allocator, nullptr, IID_PPV_ARGS(&command_list))
                          : E_FAIL;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC input_resource_desc = {};
    input_resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    input_resource_desc.Width = 2;
    input_resource_desc.Height = 2;
    input_resource_desc.DepthOrArraySize = 1;
    input_resource_desc.MipLevels = 1;
    input_resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    input_resource_desc.SampleDesc.Count = 1;
    input_resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_DESC output_resource_desc = input_resource_desc;
    output_resource_desc.Width = 4;
    output_resource_desc.Height = 4;
    output_resource_desc.DepthOrArraySize = 4;
    HRESULT input_resource_hr = device
                                    ? device->CreateCommittedResource(
                                          &heap, D3D12_HEAP_FLAG_NONE,
                                          &input_resource_desc,
                                          D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&input))
                                    : E_FAIL;
    HRESULT output_resource_hr = device
                                     ? device->CreateCommittedResource(
                                           &heap, D3D12_HEAP_FLAG_NONE,
                                           &output_resource_desc,
                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                           IID_PPV_ARGS(&output))
                                     : E_FAIL;
    D3D12_RESOURCE_DESC nv12_resource_desc = input_resource_desc;
    nv12_resource_desc.Format = DXGI_FORMAT_NV12;
    HRESULT nv12_resource_hr = device
                                   ? device->CreateCommittedResource(
                                         &heap, D3D12_HEAP_FLAG_NONE,
                                         &nv12_resource_desc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&nv12_input))
                                   : E_FAIL;
    D3D12_RESOURCE_DESC gray_input_desc = input_resource_desc;
    HRESULT gray_input_resource_hr = device
                                         ? device->CreateCommittedResource(
                                               &heap, D3D12_HEAP_FLAG_NONE,
                                               &gray_input_desc,
                                               D3D12_RESOURCE_STATE_COMMON,
                                               nullptr,
                                               IID_PPV_ARGS(&rgba_gray_input))
                                         : E_FAIL;
    D3D12_RESOURCE_DESC nv12_output_resource_desc = nv12_resource_desc;
    HRESULT nv12_output_resource_hr = device
                                          ? device->CreateCommittedResource(
                                                &heap, D3D12_HEAP_FLAG_NONE,
                                                &nv12_output_resource_desc,
                                                D3D12_RESOURCE_STATE_COMMON,
                                                nullptr,
                                                IID_PPV_ARGS(&nv12_output))
                                          : E_FAIL;
    D3D12_RESOURCE_DESC p010_resource_desc = nv12_resource_desc;
    p010_resource_desc.Format = DXGI_FORMAT_P010;
    HRESULT p010_resource_hr = device
                                   ? device->CreateCommittedResource(
                                         &heap, D3D12_HEAP_FLAG_NONE,
                                         &p010_resource_desc,
                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                         IID_PPV_ARGS(&p010_input))
                                   : E_FAIL;
    const uint32_t input_pixels[4] = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    HRESULT input_write_hr = input
                                 ? input->WriteToSubresource(
                                       0, nullptr, input_pixels, 2 * 4, 2 * 2 * 4)
                                 : E_FAIL;
    const uint8_t nv12_y[4] = {16, 235, 81, 145};
    const uint8_t nv12_uv[2] = {128, 128};
    const uint16_t p010_y[4] = {static_cast<uint16_t>(64u << 6),
                                static_cast<uint16_t>(940u << 6),
                                static_cast<uint16_t>(324u << 6),
                                static_cast<uint16_t>(580u << 6)};
    const uint16_t p010_uv[2] = {static_cast<uint16_t>(512u << 6),
                                 static_cast<uint16_t>(512u << 6)};
    const uint32_t gray_pixels[4] = {
        0xff000000u, 0xffffffffu, 0xff4c4c4cu, 0xff969696u};
    HRESULT nv12_y_write_hr = nv12_input
                                  ? nv12_input->WriteToSubresource(
                                        0, nullptr, nv12_y, 2, 4)
                                  : E_FAIL;
    HRESULT nv12_uv_write_hr = nv12_input
                                   ? nv12_input->WriteToSubresource(
                                         1, nullptr, nv12_uv, 2, 2)
                                   : E_FAIL;
    HRESULT gray_input_write_hr = rgba_gray_input
                                      ? rgba_gray_input->WriteToSubresource(
                                            0, nullptr, gray_pixels, 2 * 4,
                                            2 * 2 * 4)
                                      : E_FAIL;
    HRESULT p010_y_write_hr = p010_input
                                  ? p010_input->WriteToSubresource(
                                        0, nullptr, p010_y, 2 * 2, 2 * 2 * 2)
                                  : E_FAIL;
    HRESULT p010_uv_write_hr = p010_input
                                   ? p010_input->WriteToSubresource(
                                         1, nullptr, p010_uv, 1 * 2 * 2,
                                         1 * 2 * 2)
                                   : E_FAIL;

    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS output_arguments = {};
    output_arguments.OutputStream[0].pTexture2D = output;
    output_arguments.OutputStream[0].Subresource = 0;
    output_arguments.TargetRectangle = {0, 0, 4, 4};
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS input_arguments = {};
    input_arguments.InputStream[0].pTexture2D = input;
    input_arguments.InputStream[0].Subresource = 0;
    input_arguments.Transform.SourceRectangle = {0, 0, 2, 2};
    input_arguments.Transform.DestinationRectangle = {0, 0, 4, 4};
    if (command_list && processor && SUCCEEDED(input_write_hr))
        command_list->ProcessFrames(
            processor, &output_arguments, 1, &input_arguments);
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS rotated_output_arguments =
        output_arguments;
    rotated_output_arguments.OutputStream[0].Subresource = 1;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS rotated_input_arguments =
        input_arguments;
    rotated_input_arguments.Transform.Orientation =
        D3D12_VIDEO_PROCESS_ORIENTATION_CLOCKWISE_180;
    if (command_list && processor && SUCCEEDED(input_write_hr))
        command_list->ProcessFrames(
            processor, &rotated_output_arguments, 1,
            &rotated_input_arguments);
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS nv12_output_arguments =
        output_arguments;
    nv12_output_arguments.OutputStream[0].Subresource = 2;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS nv12_input_arguments = {};
    nv12_input_arguments.InputStream[0].pTexture2D = nv12_input;
    nv12_input_arguments.InputStream[0].Subresource = 0;
    nv12_input_arguments.Transform.SourceRectangle = {0, 0, 2, 2};
    nv12_input_arguments.Transform.DestinationRectangle = {0, 0, 4, 4};
    if (command_list && nv12_processor && SUCCEEDED(nv12_y_write_hr) &&
        SUCCEEDED(nv12_uv_write_hr))
        command_list->ProcessFrames(nv12_processor, &nv12_output_arguments, 1,
                                     &nv12_input_arguments);
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS rgba_to_nv12_output_arguments = {};
    rgba_to_nv12_output_arguments.OutputStream[0].pTexture2D = nv12_output;
    rgba_to_nv12_output_arguments.OutputStream[0].Subresource = 0;
    rgba_to_nv12_output_arguments.TargetRectangle = {0, 0, 2, 2};
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS rgba_to_nv12_input_arguments = {};
    rgba_to_nv12_input_arguments.InputStream[0].pTexture2D = rgba_gray_input;
    rgba_to_nv12_input_arguments.InputStream[0].Subresource = 0;
    rgba_to_nv12_input_arguments.Transform.SourceRectangle = {0, 0, 2, 2};
    rgba_to_nv12_input_arguments.Transform.DestinationRectangle = {0, 0, 2, 2};
    if (command_list && rgba_to_nv12_processor &&
        SUCCEEDED(gray_input_write_hr))
        command_list->ProcessFrames(rgba_to_nv12_processor,
                                     &rgba_to_nv12_output_arguments, 1,
                                     &rgba_to_nv12_input_arguments);
    D3D12_VIDEO_PROCESS_OUTPUT_STREAM_ARGUMENTS p010_output_arguments =
        output_arguments;
    p010_output_arguments.OutputStream[0].Subresource = 3;
    D3D12_VIDEO_PROCESS_INPUT_STREAM_ARGUMENTS p010_input_arguments = {};
    p010_input_arguments.InputStream[0].pTexture2D = p010_input;
    p010_input_arguments.InputStream[0].Subresource = 0;
    p010_input_arguments.Transform.SourceRectangle = {0, 0, 2, 2};
    p010_input_arguments.Transform.DestinationRectangle = {0, 0, 4, 4};
    if (command_list && p010_processor && SUCCEEDED(p010_y_write_hr) &&
        SUCCEEDED(p010_uv_write_hr))
        command_list->ProcessFrames(p010_processor, &p010_output_arguments, 1,
                                     &p010_input_arguments);
    HRESULT close_hr = command_list ? command_list->Close() : E_FAIL;
    if (queue && command_list && SUCCEEDED(close_hr)) {
        ID3D12CommandList* lists[] = {
            static_cast<ID3D12CommandList*>(command_list)};
        queue->ExecuteCommandLists(1, lists);
    }
    HRESULT fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                      IID_PPV_ARGS(&fence)) : E_FAIL;
    HRESULT signal_hr = queue && fence ? queue->Signal(fence, 1) : E_FAIL;
    event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HRESULT event_hr = fence && event ? fence->SetEventOnCompletion(1, event) : E_FAIL;
    DWORD wait_result = event && SUCCEEDED(event_hr)
                            ? WaitForSingleObject(event, 5000)
                            : WAIT_FAILED;
    uint32_t output_pixels[16] = {};
    uint32_t rotated_pixels[16] = {};
    uint32_t nv12_pixels[16] = {};
    uint8_t rgba_to_nv12_y[4] = {};
    uint8_t rgba_to_nv12_uv[2] = {};
    uint32_t p010_pixels[16] = {};
    HRESULT output_read_hr = output
                                 ? output->ReadFromSubresource(
                                       output_pixels, 4 * 4, 4 * 4 * 4, 0, nullptr)
                                 : E_FAIL;
    HRESULT rotated_read_hr = output
                                  ? output->ReadFromSubresource(
                                        rotated_pixels, 4 * 4, 4 * 4 * 4, 1,
                                        nullptr)
                                  : E_FAIL;
    HRESULT nv12_read_hr = output
                               ? output->ReadFromSubresource(
                                     nv12_pixels, 4 * 4, 4 * 4 * 4, 2, nullptr)
                               : E_FAIL;
    HRESULT rgba_to_nv12_y_read_hr = nv12_output
                                         ? nv12_output->ReadFromSubresource(
                                               rgba_to_nv12_y, 2, 4, 0,
                                               nullptr)
                                         : E_FAIL;
    HRESULT rgba_to_nv12_uv_read_hr = nv12_output
                                          ? nv12_output->ReadFromSubresource(
                                                rgba_to_nv12_uv, 2, 2, 1,
                                                nullptr)
                                          : E_FAIL;
    HRESULT p010_read_hr = output
                               ? output->ReadFromSubresource(
                                     p010_pixels, 4 * 4, 4 * 4 * 4, 3, nullptr)
                               : E_FAIL;
    const bool scaled = SUCCEEDED(output_read_hr) &&
                        output_pixels[0] == input_pixels[0] &&
                        output_pixels[1] == input_pixels[0] &&
                        output_pixels[2] == input_pixels[1] &&
                        output_pixels[5] == input_pixels[0] &&
                        output_pixels[8] == input_pixels[2] &&
                        output_pixels[10] == input_pixels[3] &&
                        output_pixels[15] == input_pixels[3];
    const bool rotated = SUCCEEDED(rotated_read_hr) &&
                         rotated_pixels[0] == input_pixels[3] &&
                         rotated_pixels[1] == input_pixels[3] &&
                         rotated_pixels[2] == input_pixels[2] &&
                         rotated_pixels[3] == input_pixels[2] &&
                         rotated_pixels[12] == input_pixels[1] &&
                         rotated_pixels[15] == input_pixels[0];
    const bool nv12_converted = SUCCEEDED(nv12_read_hr) &&
                                nv12_pixels[0] == 0xff000000u &&
                                nv12_pixels[2] == 0xffffffffu &&
                                nv12_pixels[8] == 0xff4c4c4cu &&
                                nv12_pixels[10] == 0xff969696u &&
                                nv12_pixels[15] == 0xff969696u;
    const bool rgba_to_nv12_converted =
        SUCCEEDED(rgba_to_nv12_y_read_hr) &&
        SUCCEEDED(rgba_to_nv12_uv_read_hr) &&
        std::memcmp(rgba_to_nv12_y, nv12_y, sizeof(nv12_y)) == 0 &&
        std::memcmp(rgba_to_nv12_uv, nv12_uv, sizeof(nv12_uv)) == 0;
    const bool p010_converted =
        SUCCEEDED(p010_read_hr) && p010_pixels[0] == 0xff000000u &&
        p010_pixels[2] == 0xffffffffu && p010_pixels[8] == 0xff4c4c4cu &&
        p010_pixels[10] == 0xff969696u && p010_pixels[15] == 0xff969696u;
    const bool passed = create_hr == S_OK && video_hr == S_OK &&
                        processor_hr == S_OK && nv12_processor_hr == S_OK &&
                        rgba_to_nv12_processor_hr == S_OK &&
                        p010_processor_hr == S_OK && p010_output_rejected &&
                        queue_hr == S_OK &&
                        allocator_hr == S_OK && list_hr == S_OK &&
                        input_resource_hr == S_OK && output_resource_hr == S_OK &&
                        input_write_hr == S_OK && nv12_resource_hr == S_OK &&
                        nv12_y_write_hr == S_OK && nv12_uv_write_hr == S_OK &&
                        gray_input_resource_hr == S_OK &&
                        nv12_output_resource_hr == S_OK &&
                        gray_input_write_hr == S_OK && p010_resource_hr == S_OK &&
                        p010_y_write_hr == S_OK && p010_uv_write_hr == S_OK &&
                        close_hr == S_OK && rgba_to_nv12_y_read_hr == S_OK &&
                        rgba_to_nv12_uv_read_hr == S_OK &&
                        signal_hr == S_OK && event_hr == S_OK &&
                        wait_result == WAIT_OBJECT_0 && output_read_hr == S_OK &&
                        rotated_read_hr == S_OK && nv12_read_hr == S_OK &&
                        scaled && rotated && nv12_converted &&
                        rgba_to_nv12_converted && p010_converted;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.video-process.v1\",\n");
    std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
    std::printf("  \"create_device\": \"0x%08lx\",\n", hr_value(create_hr));
    std::printf("  \"query_video_device\": \"0x%08lx\",\n", hr_value(video_hr));
    std::printf("  \"create_processor\": \"0x%08lx\",\n", hr_value(processor_hr));
    std::printf("  \"create_nv12_processor\": \"0x%08lx\",\n",
                hr_value(nv12_processor_hr));
    std::printf("  \"create_rgba_to_nv12_processor\": \"0x%08lx\",\n",
                hr_value(rgba_to_nv12_processor_hr));
    std::printf("  \"create_p010_processor\": \"0x%08lx\",\n",
                hr_value(p010_processor_hr));
    std::printf("  \"p010_output_reject\": \"0x%08lx\",\n",
                hr_value(p010_output_reject_hr));
    std::printf("  \"p010_output_rejected\": %s,\n",
                p010_output_rejected ? "true" : "false");
    std::printf("  \"create_video_queue\": \"0x%08lx\",\n", hr_value(queue_hr));
    std::printf("  \"create_video_command_list\": \"0x%08lx\",\n", hr_value(list_hr));
    std::printf("  \"close\": \"0x%08lx\",\n", hr_value(close_hr));
    std::printf("  \"create_input\": \"0x%08lx\",\n", hr_value(input_resource_hr));
    std::printf("  \"create_output\": \"0x%08lx\",\n", hr_value(output_resource_hr));
    std::printf("  \"create_nv12_input\": \"0x%08lx\",\n",
                hr_value(nv12_resource_hr));
    std::printf("  \"input_write\": \"0x%08lx\",\n", hr_value(input_write_hr));
    std::printf("  \"nv12_y_write\": \"0x%08lx\",\n",
                hr_value(nv12_y_write_hr));
    std::printf("  \"nv12_uv_write\": \"0x%08lx\",\n",
                hr_value(nv12_uv_write_hr));
    std::printf("  \"create_gray_input\": \"0x%08lx\",\n",
                hr_value(gray_input_resource_hr));
    std::printf("  \"create_nv12_output\": \"0x%08lx\",\n",
                hr_value(nv12_output_resource_hr));
    std::printf("  \"create_p010_input\": \"0x%08lx\",\n",
                hr_value(p010_resource_hr));
    std::printf("  \"gray_input_write\": \"0x%08lx\",\n",
                hr_value(gray_input_write_hr));
    std::printf("  \"p010_y_write\": \"0x%08lx\",\n",
                hr_value(p010_y_write_hr));
    std::printf("  \"p010_uv_write\": \"0x%08lx\",\n",
                hr_value(p010_uv_write_hr));
    std::printf("  \"create_fence\": \"0x%08lx\",\n", hr_value(fence_hr));
    std::printf("  \"signal\": \"0x%08lx\",\n", hr_value(signal_hr));
    std::printf("  \"event_completion\": \"0x%08lx\",\n", hr_value(event_hr));
    std::printf("  \"wait_result\": %lu,\n", static_cast<unsigned long>(wait_result));
    std::printf("  \"output_readback\": \"0x%08lx\",\n", hr_value(output_read_hr));
    std::printf("  \"output_scaled_exact\": %s,\n", scaled ? "true" : "false");
    std::printf("  \"output_rotated_readback\": \"0x%08lx\",\n",
                hr_value(rotated_read_hr));
    std::printf("  \"output_rotated_exact\": %s,\n",
                rotated ? "true" : "false");
    std::printf("  \"output_nv12_readback\": \"0x%08lx\",\n",
                hr_value(nv12_read_hr));
    std::printf("  \"output_nv12_exact\": %s,\n",
                nv12_converted ? "true" : "false");
    std::printf("  \"rgba_to_nv12_y_readback\": \"0x%08lx\",\n",
                hr_value(rgba_to_nv12_y_read_hr));
    std::printf("  \"rgba_to_nv12_uv_readback\": \"0x%08lx\",\n",
                hr_value(rgba_to_nv12_uv_read_hr));
    std::printf("  \"rgba_to_nv12_exact\": %s,\n",
                rgba_to_nv12_converted ? "true" : "false");
    std::printf("  \"output_p010_readback\": \"0x%08lx\",\n",
                hr_value(p010_read_hr));
    std::printf("  \"output_p010_exact\": %s,\n",
                p010_converted ? "true" : "false");
    std::printf("  \"output_pixels\": [");
    for (size_t i = 0; i < 16; ++i)
        std::printf("\"0x%08x\"%s", output_pixels[i],
                    i + 1 == 16 ? "" : ",");
    std::printf("],\n");
    std::printf("  \"output_rotated_pixels\": [");
    for (size_t i = 0; i < 16; ++i)
        std::printf("\"0x%08x\"%s", rotated_pixels[i],
                    i + 1 == 16 ? "" : ",");
    std::printf("],\n");
    std::printf("  \"output_nv12_pixels\": [");
    for (size_t i = 0; i < 16; ++i)
        std::printf("\"0x%08x\"%s", nv12_pixels[i],
                    i + 1 == 16 ? "" : ",");
    std::printf("],\n");
    std::printf("  \"output_p010_pixels\": [");
    for (size_t i = 0; i < 16; ++i)
        std::printf("\"0x%08x\"%s", p010_pixels[i],
                    i + 1 == 16 ? "" : ",");
    std::printf("],\n");
    std::printf("  \"rgba_to_nv12_y\": [%u, %u, %u, %u],\n",
                rgba_to_nv12_y[0], rgba_to_nv12_y[1], rgba_to_nv12_y[2],
                rgba_to_nv12_y[3]);
    std::printf("  \"rgba_to_nv12_uv\": [%u, %u],\n",
                rgba_to_nv12_uv[0], rgba_to_nv12_uv[1]);
    std::printf("  \"gpu_execution\": false,\n");
    std::printf("  \"provider_scope\": \"CPU-reference-video-process\"\n");
    std::printf("}\n");

    if (event)
        CloseHandle(event);
    release(p010_output_processor);
    release(p010_input);
    release(nv12_output);
    release(rgba_gray_input);
    release(output);
    release(nv12_input);
    release(input);
    release(fence);
    release(command_list);
    release(allocator);
    release(queue);
    release(p010_processor);
    release(rgba_to_nv12_processor);
    release(nv12_processor);
    release(processor);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
