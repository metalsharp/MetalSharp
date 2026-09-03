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
    0x189819f1, 0x1db6, 0x4b57,
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

static std::string env_string(const char *key) {
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
    for (char c : value) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    return escaped;
}

static std::string hr_hex(HRESULT hr) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return text;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static HRESULT serialize_root_signature(ID3DBlob **blob) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using SerializeFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    auto serialize = load_proc<SerializeFn>(d3d12,
                                            "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC description = {};
    description.NumParameters = 1;
    description.pParameters = &parameter;
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&description, D3D_ROOT_SIGNATURE_VERSION_1_0, blob,
                           &errors);
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
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        hr = event_handle ? queue->Signal(fence, 1)
                          : HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr) &&
        WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return hr;
}

struct CaseResult {
    const char *name = "";
    HRESULT pso_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    uint32_t value = 0;
    bool readback = false;
    bool exact = false;
};

enum class Shape { Texture1D, Texture1DArray, Texture3D };

static CaseResult run_case(ID3D12Device *device, ID3D12RootSignature *root,
                           const std::vector<uint8_t> &vs,
                           const std::vector<uint8_t> &ps, const char *name,
                           Shape shape) {
    CaseResult result;
    result.name = name;
    if (!device || !root || vs.empty() || ps.empty())
        return result;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = root;
    pipeline.VS = {vs.data(), vs.size()};
    pipeline.PS = {ps.data(), ps.size()};
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pipeline.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device->CreateGraphicsPipelineState(
        &pipeline, IID_PPV_ARGS(&pso));
    if (FAILED(result.pso_hr) || !pso)
        return result;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *uav_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    HRESULT hr = S_OK;
    D3D12_COMMAND_QUEUE_DESC queue_description = {};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        allocator, nullptr,
                                        IID_PPV_ARGS(&list));
    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_description = {};
    uav_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_description.NumDescriptors = 1;
    uav_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&uav_heap_description,
                                          IID_PPV_ARGS(&uav_heap));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description = {};
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&rtv_heap_description,
                                          IID_PPV_ARGS(&rtv_heap));

    D3D12_RESOURCE_DESC output_description = {};
    output_description.Dimension = shape == Shape::Texture3D
                                       ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                       : D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    output_description.Width = 1;
    output_description.Height = 1;
    output_description.DepthOrArraySize = shape == Shape::Texture1DArray ? 2 : 1;
    output_description.MipLevels = 1;
    output_description.Format = DXGI_FORMAT_R32_UINT;
    output_description.SampleDesc.Count = 1;
    output_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    output_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC target_description = {};
    target_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_description.Width = 1;
    target_description.Height = 1;
    target_description.DepthOrArraySize = 1;
    target_description.MipLevels = 1;
    target_description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_description.SampleDesc.Count = 1;
    target_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    const UINT subresource = shape == Shape::Texture1DArray ? 1 : 0;
    if (SUCCEEDED(hr))
        device->GetCopyableFootprints(&output_description, subresource, 1, 0,
                                      &footprint, &rows, &row_size,
                                      &total_bytes);
    if (SUCCEEDED(hr) && total_bytes == 0)
        hr = E_FAIL;
    D3D12_RESOURCE_DESC readback_description = {};
    readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_description.Width = total_bytes;
    readback_description.Height = 1;
    readback_description.DepthOrArraySize = 1;
    readback_description.MipLevels = 1;
    readback_description.SampleDesc.Count = 1;
    readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(&target));

    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_UINT;
        if (shape == Shape::Texture3D) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            uav.Texture3D.MipSlice = 0;
            uav.Texture3D.FirstWSlice = 0;
            uav.Texture3D.WSize = 1;
        } else if (shape == Shape::Texture1DArray) {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            uav.Texture1DArray.MipSlice = 0;
            uav.Texture1DArray.FirstArraySlice = 0;
            uav.Texture1DArray.ArraySize = 2;
        } else {
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            uav.Texture1D.MipSlice = 0;
        }
        device->CreateUnorderedAccessView(
            output, nullptr, &uav,
            uav_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(
            target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        const UINT clear_values[4] = {};
        list->SetDescriptorHeaps(1, &uav_heap);
        list->ClearUnorderedAccessViewUint(
            uav_heap->GetGPUDescriptorHandleForHeapStart(),
            uav_heap->GetCPUDescriptorHandleForHeapStart(), output,
            clear_values, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const FLOAT clear_color[4] = {};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(root);
        list->SetGraphicsRootDescriptorTable(
            0, uav_heap->GetGPUDescriptorHandleForHeapStart());
        const D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
        const D3D12_RECT scissor = {0, 0, 1, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        // Three separate full-screen draws are used instead of one nine-vertex
        // draw so the source-owned VS does not need a modulo/array lowering
        // special case.  The ROV value must still observe all three ordered
        // increments.
        for (UINT draw = 0; draw < 3; ++draw)
            list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = output;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        result.execute_hr = execute_and_wait(device, queue, list);
    } else {
        result.execute_hr = hr;
    }
    if (SUCCEEDED(result.execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, total_bytes};
        result.map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(result.map_hr) && mapped) {
            std::memcpy(&result.value, mapped, sizeof(result.value));
            readback->Unmap(0, nullptr);
            result.readback = true;
        }
    }
    result.exact = result.pso_hr == S_OK && result.execute_hr == S_OK &&
                   result.map_hr == S_OK && result.readback && result.value == 3;
    safe_release(readback);
    safe_release(output);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: probe_rov_dimensions <vs> <1d-ps> <1d-array-ps> <3d-ps>\n");
        return 2;
    }
    const auto vs = read_binary_file(argv[1]);
    const auto ps_1d = read_binary_file(argv[2]);
    const auto ps_1d_array = read_binary_file(argv[3]);
    const auto ps_3d = read_binary_file(argv[4]);
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                              REFIID, void **);
    auto create_device = load_proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            kDeviceIID,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3DBlob *root_blob = nullptr;
    HRESULT root_hr = serialize_root_signature(&root_blob);
    ID3D12RootSignature *root = nullptr;
    if (SUCCEEDED(device_hr) && SUCCEEDED(root_hr))
        root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root));
    std::vector<CaseResult> cases;
    if (SUCCEEDED(device_hr) && SUCCEEDED(root_hr)) {
        cases.push_back(run_case(device, root, vs, ps_1d, "texture1d", Shape::Texture1D));
        cases.push_back(run_case(device, root, vs, ps_1d_array,
                                 "texture1d_array", Shape::Texture1DArray));
        cases.push_back(run_case(device, root, vs, ps_3d, "texture3d", Shape::Texture3D));
    }
    bool exact = cases.size() == 3;
    for (const auto &item : cases)
        exact = exact && item.exact;
    std::printf("{\n  \"schema\": \"metalsharp.d3d12.rov-dimensions.v1\",\n"
                "  \"profile\": \"%s\",\n  \"provider\": \"metal_raster_order_group\",\n"
                "  \"create_device\": \"%s\",\n  \"root_signature\": \"%s\",\n"
                "  \"cases\": [\n",
                json_escape(env_string("D3D12_METAL_SDK_PROFILE")).c_str(),
                hr_hex(device_hr).c_str(), hr_hex(root_hr).c_str());
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto &item = cases[i];
        std::printf("    {\"name\":\"%s\",\"pso\":\"%s\",\"execute\":\"%s\",\"map\":\"%s\",\"readback\":%s,\"value\":%u,\"expected\":3,\"exact\":%s}%s\n",
                    item.name, hr_hex(item.pso_hr).c_str(),
                    hr_hex(item.execute_hr).c_str(), hr_hex(item.map_hr).c_str(),
                    item.readback ? "true" : "false", item.value,
                    item.exact ? "true" : "false",
                    i + 1 == cases.size() ? "" : ",");
    }
    std::printf("  ],\n  \"exact\": %s,\n  \"bounded_wait_ms\": 15000\n}\n",
                exact ? "true" : "false");
    std::fflush(stdout);
    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return exact ? 0 : 1;
}
