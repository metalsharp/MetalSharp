#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID,
                                          void **);

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static unsigned long hr_value(HRESULT hr) {
    return static_cast<unsigned long>(static_cast<uint32_t>(hr));
}
template <typename T> static void release(T *&value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *texture = nullptr;
    ID3D12DescriptorHeap *descriptor_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))
                            : E_FAIL;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    HRESULT queue_hr = device ? device->CreateCommandQueue(
                                    &queue_desc, IID_PPV_ARGS(&queue)) : E_FAIL;
    HRESULT allocator_hr = device ? device->CreateCommandAllocator(
                                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator)) : E_FAIL;
    HRESULT list_hr = device && allocator
                          ? device->CreateCommandList(
                                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                                nullptr, IID_PPV_ARGS(&list))
                          : E_FAIL;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HRESULT resource_hr = device
                              ? device->CreateCommittedResource(
                                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
                                    IID_PPV_ARGS(&texture))
                              : E_FAIL;
    uint32_t initial[16];
    for (auto &pixel : initial)
        pixel = 0x11223344u;
    HRESULT write_hr = texture
                           ? texture->WriteToSubresource(0, nullptr, initial,
                                                         4 * sizeof(uint32_t),
                                                         sizeof(initial))
                           : E_FAIL;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    HRESULT rtv_heap_hr = device
                              ? device->CreateDescriptorHeap(
                                    &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap))
                              : E_FAIL;
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    if (rtv_heap && device)
        device->CreateRenderTargetView(
            texture, &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_RECT rtv_rect = {0, 3, 4, 4};
    const float rtv_color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    if (list && rtv_heap && SUCCEEDED(rtv_heap_hr))
        list->ClearRenderTargetView(
            rtv_heap->GetCPUDescriptorHandleForHeapStart(), rtv_color, 1,
            &rtv_rect);

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
    descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_heap_desc.NumDescriptors = 1;
    descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT descriptor_heap_hr = device
                                    ? device->CreateDescriptorHeap(
                                          &descriptor_heap_desc,
                                          IID_PPV_ARGS(&descriptor_heap))
                                    : E_FAIL;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;
    HRESULT create_uav_hr = descriptor_heap && device
                                ? (device->CreateUnorderedAccessView(
                                       texture, nullptr, &uav_desc,
                                       descriptor_heap->GetCPUDescriptorHandleForHeapStart()),
                                   S_OK)
                                : E_FAIL;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav = descriptor_heap
                                              ? descriptor_heap->GetGPUDescriptorHandleForHeapStart()
                                              : D3D12_GPU_DESCRIPTOR_HANDLE{};
    const UINT clear_values[4] = {0x55667788u, 0, 0, 0};
    D3D12_RECT clear_rect = {0, 0, 4, 1};
    if (list && descriptor_heap && SUCCEEDED(create_uav_hr))
        list->ClearUnorderedAccessViewUint(
            gpu_uav, descriptor_heap->GetCPUDescriptorHandleForHeapStart(),
            texture, clear_values, 1, &clear_rect);
    RECT rect = {1, 1, 3, 3};
    D3D12_DISCARD_REGION region = {};
    region.FirstSubresource = 0;
    region.NumSubresources = 1;
    region.NumRects = 1;
    region.pRects = &rect;
    if (list && texture && SUCCEEDED(write_hr))
        list->DiscardResource(texture, &region);
    HRESULT close_hr = list ? list->Close() : E_FAIL;
    if (queue && list && SUCCEEDED(close_hr)) {
        ID3D12CommandList *lists[] = {list};
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
    uint32_t actual[16] = {};
    HRESULT read_hr = texture
                          ? texture->ReadFromSubresource(actual,
                                                         4 * sizeof(uint32_t),
                                                         sizeof(actual), 0,
                                                         nullptr)
                          : E_FAIL;
    bool exact = SUCCEEDED(read_hr);
    for (UINT y = 0; y < 4 && exact; ++y)
        for (UINT x = 0; x < 4; ++x) {
            const bool discarded = x >= 1 && x < 3 && y >= 1 && y < 3;
            const bool cleared = y == 0;
            const bool rtv_cleared = y == 3;
            exact = actual[y * 4 + x] ==
                    (discarded ? 0u
                               : (cleared ? clear_values[0]
                                          : (rtv_cleared ? 0xff0000ffu
                                                         : initial[0])));
        }
    const bool passed = device_hr == S_OK && queue_hr == S_OK &&
                        allocator_hr == S_OK && list_hr == S_OK &&
                        resource_hr == S_OK && write_hr == S_OK &&
                        rtv_heap_hr == S_OK && descriptor_heap_hr == S_OK &&
                        create_uav_hr == S_OK &&
                        close_hr == S_OK &&
                        fence_hr == S_OK && signal_hr == S_OK && event_hr == S_OK &&
                        wait_result == WAIT_OBJECT_0 && read_hr == S_OK && exact;
    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.discard-texture.v1\",\n");
    std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
    std::printf("  \"discard_rect\": \"{1,1,3,3}\",\n");
    std::printf("  \"readback\": \"0x%08lx\",\n", hr_value(read_hr));
    std::printf("  \"exact_rect_zeroing\": %s,\n", exact ? "true" : "false");
    std::printf("  \"pixels\": [");
    for (size_t i = 0; i < 16; ++i)
        std::printf("\"0x%08x\"%s", actual[i], i + 1 == 16 ? "" : ",");
    std::printf("],\n");
    std::printf("  \"unmodified_pixel\": \"0x%08x\"\n", actual[4]);
    std::printf("}\n");
    if (event)
        CloseHandle(event);
    release(fence);
    release(rtv_heap);
    release(descriptor_heap);
    release(texture);
    release(list);
    release(allocator);
    release(queue);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
