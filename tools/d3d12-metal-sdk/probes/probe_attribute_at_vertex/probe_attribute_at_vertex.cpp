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

static D3D12_RESOURCE_DESC buffer_desc(
    UINT64 bytes,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

static D3D12_BLEND_DESC blend_desc() {
    D3D12_BLEND_DESC desc = {};
    for (auto &target : desc.RenderTarget) {
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return desc;
}

static D3D12_RASTERIZER_DESC rasterizer_desc() {
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.DepthClipEnable = TRUE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    return desc;
}

static D3D12_DEPTH_STENCIL_DESC depth_stencil_desc() {
    D3D12_DEPTH_STENCIL_DESC desc = {};
    desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BackFace = desc.FrontFace;
    return desc;
}

static HRESULT serialize_root_signature(ID3DBlob **blob) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using SerializeRootSignatureFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    auto serialize = load_proc<SerializeRootSignatureFn>(
        d3d12, "D3D12SerializeRootSignature");
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
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &errors);
    safe_release(errors);
    return hr;
}

static HRESULT execute_and_wait(ID3D12Device *device, ID3D12CommandQueue *queue,
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
                     "usage: probe_attribute_at_vertex [<vs.cso> <ps.cso>]\n");
        return 2;
    }

    const char *vertex_path = argc == 3 ? argv[1] : "probe_attribute_at_vertex_vs.cso";
    const char *pixel_path = argc == 3 ? argv[2] : "probe_attribute_at_vertex_ps.cso";
    const std::vector<uint8_t> vertex_shader = read_binary_file(vertex_path);
    const std::vector<uint8_t> pixel_shader = read_binary_file(pixel_path);
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
        desc.BlendState = blend_desc();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = rasterizer_desc();
        desc.DepthStencilState = depth_stencil_desc();
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        pso_hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    }

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *uav_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12Resource *target = nullptr;
    HRESULT execute_hr = pso_hr;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            IID_PPV_ARGS(&list));

    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc = {};
    uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_desc.NumDescriptors = 1;
    uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateDescriptorHeap(
            &uav_heap_desc, IID_PPV_ARGS(&uav_heap));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateDescriptorHeap(
            &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));

    const D3D12_RESOURCE_DESC output_desc =
        buffer_desc(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(64);
    D3D12_HEAP_PROPERTIES default_heap =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 4;
    target_desc.Height = 4;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (SUCCEEDED(execute_hr))
        execute_hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));

    if (SUCCEEDED(execute_hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = 16;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(
            output, nullptr, &uav, uav_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(
            target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(root);
        ID3D12DescriptorHeap *heaps[] = {uav_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(
            0, uav_heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_VIEWPORT viewport = {0, 0, 4, 4, 0, 1};
        D3D12_RECT scissor = {0, 0, 4, 4};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback, 0, output, 0, 64);
        execute_hr = execute_and_wait(device, queue, list);
    }

    uint32_t values[3] = {};
    bool readback_ok = false;
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(values)};
        readback_ok = SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped;
        if (readback_ok) {
            std::memcpy(values, mapped, sizeof(values));
            readback->Unmap(0, nullptr);
        }
    }
    const bool exact = readback_ok && values[0] == 0x3e000000u &&
                       values[1] == 0x3f000000u && values[2] == 0x3f600000u;
    std::printf(
        "{\n"
        "  \"schema\": \"metalsharp.d3d12-metal.attribute-at-vertex.v1\",\n"
        "  \"provider\": \"gpu_vertex_capture_buffer\",\n"
        "  \"create_device\": \"%s\",\n"
        "  \"root_signature\": \"%s\",\n"
        "  \"pso\": \"%s\",\n"
        "  \"execute\": \"%s\",\n"
        "  \"readback\": %s,\n"
        "  \"values\": [%u, %u, %u],\n"
        "  \"expected\": [1040187392, 1056964608, 1063256064],\n"
        "  \"exact\": %s,\n"
        "  \"bounded_draw\": {\"triangle_list\": true, \"vertices\": 3, \"instances\": 1}\n"
        "}\n",
        hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str(), hr_hex(pso_hr).c_str(),
        hr_hex(execute_hr).c_str(), readback_ok ? "true" : "false", values[0],
        values[1], values[2], exact ? "true" : "false");

    safe_release(target);
    safe_release(readback);
    safe_release(output);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return exact ? 0 : 1;
}
