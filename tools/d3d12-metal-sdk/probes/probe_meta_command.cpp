#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr GUID kMetaCommandId = {
    0x3b9b0a12, 0x6c42, 0x4c11,
    {0x9d, 0x1b, 0x5a, 0x2e, 0x13, 0x77, 0x42, 0x90}};
struct ExecutionData {
    uint64_t destination_gpu_address;
    uint32_t value;
    uint32_t byte_count;
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
static D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES result = {};
    result.Type = D3D12_HEAP_TYPE_UPLOAD;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}
static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC result = {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}
static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
                               ID3D12GraphicsCommandList4* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && event)
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        hr = E_FAIL;
    if (event)
        CloseHandle(event);
    release(fence);
    return hr;
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device5* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList4* list = nullptr;
    ID3D12MetaCommand* meta = nullptr;
    ID3D12Resource* output = nullptr;
    HRESULT hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&device))
                               : E_FAIL;
    UINT command_count = 0;
    D3D12_META_COMMAND_DESC command_desc = {};
    UINT parameter_count = 0;
    UINT parameter_size = 0;
    bool parameter_shape_ok = false;
    bool readback_exact = false;
    uint8_t bytes[16] = {};
    if (SUCCEEDED(hr))
        hr = device->EnumerateMetaCommands(&command_count, nullptr);
    if (SUCCEEDED(hr) && command_count)
        hr = device->EnumerateMetaCommands(&command_count, &command_desc);
    if (SUCCEEDED(hr))
        hr = device->EnumerateMetaCommandParameters(
            kMetaCommandId, D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,
            &parameter_size, &parameter_count, nullptr);
    D3D12_META_COMMAND_PARAMETER_DESC parameters[3] = {};
    if (SUCCEEDED(hr)) {
        parameter_count = 3;
        hr = device->EnumerateMetaCommandParameters(
            kMetaCommandId, D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,
            &parameter_size, &parameter_count, parameters);
        parameter_shape_ok = SUCCEEDED(hr) && parameter_count == 3 &&
                             parameter_size == sizeof(ExecutionData);
    }
    if (SUCCEEDED(hr))
        hr = device->CreateMetaCommand(kMetaCommandId, 0, nullptr, 0,
                                       IID_PPV_ARGS(&meta));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    if (SUCCEEDED(hr))
        hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&list));
    D3D12_HEAP_PROPERTIES heap = upload_heap();
    D3D12_RESOURCE_DESC desc = buffer_desc(64);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&output));
    ExecutionData execution = {output ? output->GetGPUVirtualAddress() : 0,
                               0x7f7f7f7f, sizeof(bytes)};
    if (SUCCEEDED(hr)) {
        list->InitializeMetaCommand(meta, nullptr, 0);
        list->ExecuteMetaCommand(meta, &execution, sizeof(execution));
        hr = execute_and_wait(device, queue, list);
    }
    if (SUCCEEDED(hr) && output) {
        void* mapped = nullptr;
        hr = output->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(bytes, mapped, sizeof(bytes));
            output->Unmap(0, nullptr);
            readback_exact = true;
            for (uint8_t value : bytes)
                readback_exact = readback_exact && value == 0x7f;
        }
    }
    const bool passed = SUCCEEDED(hr) && command_count == 1 &&
                        command_desc.Id.Data1 == kMetaCommandId.Data1 && meta &&
                        parameter_shape_ok && readback_exact;
    std::printf("{\n  \"schema\": \"metalsharp.d3d12-metal.meta-command.v1\",\n");
    std::printf("  \"pass\": %s,\n  \"hr\": \"0x%08lx\",\n",
                passed ? "true" : "false",
                static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"enumerated_count\": %u,\n  \"parameter_size\": %u,\n",
                command_count, parameter_size);
    std::printf("  \"parameter_shape_ok\": %s,\n",
                parameter_shape_ok ? "true" : "false");
    std::printf("  \"gpu_execution_provider\": true,\n  \"readback_exact\": %s,\n  \"bytes\": [",
                readback_exact ? "true" : "false");
    for (size_t i = 0; i < sizeof(bytes); ++i)
        std::printf("%s%u", i ? "," : "", bytes[i]);
    std::printf("]\n}\n");
    release(output);
    release(list);
    release(allocator);
    release(queue);
    release(meta);
    release(device);
    if (module)
        FreeLibrary(module);
    return passed ? 0 : 1;
}
