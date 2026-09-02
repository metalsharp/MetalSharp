#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static const GUID kDeviceIID = {
    0x189819f1,
    0x1db6,
    0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

template <typename T> static void safe_release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(proc),
                  "function pointer size mismatch");
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

static std::vector<uint8_t> read_binary_file(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}

static std::string getenv_string(const char *key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (!needed)
        return {};
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (!written)
        return {};
    value.resize(written);
    return value;
}

static std::string json_escape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    return escaped;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static HRESULT serialize_root_signature(ID3DBlob **blob) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using SerializeFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    auto serialize = load_proc<SerializeFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &errors);
    safe_release(errors);
    return hr;
}

static HRESULT execute_and_wait(ID3D12Device *device,
                                ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence *fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr)) {
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        hr = event_handle ? queue->Signal(fence, 1)
                          : HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr))
        hr = WaitForSingleObject(event_handle, 15000) == WAIT_OBJECT_0
                 ? S_OK
                 : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return hr;
}

int main(int argc, char **argv) {
    if (argc != 1 && argc != 3) {
        std::fprintf(stderr,
                     "usage: probe_barycentrics [<vs.cso> <ps.cso>]\n");
        return 2;
    }
    const char *vs_path = argc == 3 ? argv[1] : "probe_barycentrics_vs.cso";
    const char *ps_path = argc == 3 ? argv[2] : "probe_barycentrics_ps.cso";
    const auto vertex_shader = read_binary_file(vs_path);
    const auto pixel_shader = read_binary_file(ps_path);

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                              REFIID, void **);
    auto create_device = load_proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            kDeviceIID,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3DBlob *root_blob = nullptr;
    HRESULT root_hr = serialize_root_signature(&root_blob);
    ID3D12RootSignature *root = nullptr;
    if (SUCCEEDED(create_hr) && SUCCEEDED(root_hr))
        root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root));

    ID3D12PipelineState *pso = nullptr;
    HRESULT pso_hr = E_FAIL;
    if (SUCCEEDED(create_hr) && SUCCEEDED(root_hr) && !vertex_shader.empty() &&
        !pixel_shader.empty()) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = {vertex_shader.data(), vertex_shader.size()};
        desc.PS = {pixel_shader.data(), pixel_shader.size()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        desc.RasterizerState.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        pso_hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *readback = nullptr;
    HRESULT execute_hr = pso_hr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    if (SUCCEEDED(execute_hr)) {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        execute_hr = device->CreateCommandQueue(&queue_desc,
                                                 IID_PPV_ARGS(&queue));
    }
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            IID_PPV_ARGS(&list));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateDescriptorHeap(&rtv_desc,
                                                   IID_PPV_ARGS(&rtv_heap));

    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
    if (SUCCEEDED(execute_hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, &rows,
                                      &row_size, &total_bytes);
        if (!total_bytes)
            execute_hr = E_FAIL;
    }
    D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    if (SUCCEEDED(execute_hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        const float clear_color[4] = {};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(root);
        D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
        D3D12_RECT scissor = {0, 0, 1, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        execute_hr = execute_and_wait(device, queue, list);
    }

    uint8_t rgba[4] = {};
    bool readback_ok = false;
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, total_bytes};
        readback_ok = SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped;
        if (readback_ok) {
            std::memcpy(rgba, mapped, sizeof(rgba));
            readback->Unmap(0, nullptr);
        }
    }
    const bool exact = readback_ok && rgba[0] == 128 && rgba[1] == 64 &&
                       rgba[2] == 64 && rgba[3] == 255;
    const std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
    std::printf(
        "{\n"
        "  \"schema\": \"metalsharp.d3d12-metal.barycentrics.v1\",\n"
        "  \"profile\": \"%s\",\n"
        "  \"create_device\": \"%s\",\n"
        "  \"root_signature\": \"%s\",\n"
        "  \"pso\": \"%s\",\n"
        "  \"execute\": \"%s\",\n"
        "  \"readback\": %s,\n"
        "  \"rgba\": [%u, %u, %u, %u],\n"
        "  \"expected_rgba\": [128, 64, 64, 255],\n"
        "  \"exact\": %s,\n"
        "  \"bounded_scope\": \"default perspective SV_Barycentrics at one full-screen triangle pixel\"\n"
        "}\n",
        json_escape(profile).c_str(), hr_hex(create_hr).c_str(),
        hr_hex(root_hr).c_str(), hr_hex(pso_hr).c_str(),
        hr_hex(execute_hr).c_str(), readback_ok ? "true" : "false", rgba[0],
        rgba[1], rgba[2], rgba[3], exact ? "true" : "false");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), exact ? 0u : 1u);
    safe_release(readback);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return exact ? 0 : 1;
}
