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
    if (needed == 0)
        return {};
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (written == 0)
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

static HRESULT create_graphics_pso(
    ID3D12Device *device, ID3D12RootSignature *root,
    const std::vector<uint8_t> &vertex_shader,
    const std::vector<uint8_t> &pixel_shader, ID3D12PipelineState **pso,
    bool independent_logic = false, bool depth_state = false,
    bool stencil_state = false) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vertex_shader.data(), vertex_shader.size()};
    desc.PS = {pixel_shader.data(), pixel_shader.size()};
    desc.BlendState = blend_desc();
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = rasterizer_desc();
    desc.DepthStencilState = depth_stencil_desc();
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = independent_logic ? 2 : 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (independent_logic) {
        desc.RTVFormats[1] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.BlendState.IndependentBlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
        desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
        desc.BlendState.RenderTarget[1].LogicOpEnable = TRUE;
        desc.BlendState.RenderTarget[1].LogicOp = D3D12_LOGIC_OP_AND;
    }
    desc.SampleDesc.Count = 1;
    if (depth_state) {
        desc.DSVFormat = stencil_state ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                       : DXGI_FORMAT_D32_FLOAT;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        if (stencil_state) {
            desc.DepthStencilState.StencilEnable = TRUE;
            desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
            desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
            desc.DepthStencilState.FrontFace.StencilFunc =
                D3D12_COMPARISON_FUNC_ALWAYS;
            desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
        }
    }
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
}

static HRESULT create_compute_pso(ID3D12Device *device,
                                 ID3D12RootSignature *root,
                                 const std::vector<uint8_t> &shader,
                                 ID3D12PipelineState **pso) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.CS = {shader.data(), shader.size()};
    return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso));
}

struct CaseResult {
    const char *name = "";
    HRESULT pso_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    bool readback = false;
    uint32_t value = 0;
    uint32_t expected_value = 3;
    bool object_null = true;
    bool exact = false;
};

static void create_common_objects(ID3D12Device *device,
                                  ID3D12CommandQueue **queue,
                                  ID3D12CommandAllocator **allocator,
                                  ID3D12GraphicsCommandList **list,
                                  HRESULT &hr) {
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    if (SUCCEEDED(hr))
        hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, *allocator, nullptr,
            IID_PPV_ARGS(list));
}

static void record_draw(
    ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso,
    ID3D12RootSignature *root, ID3D12DescriptorHeap *uav_heap,
    ID3D12DescriptorHeap *rtv_heap, ID3D12Resource *target,
    ID3D12DescriptorHeap *dsv_heap = nullptr, ID3D12Resource *depth = nullptr,
    bool stencil_state = false) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const float clear_color[4] = {};
    list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if (dsv_heap && depth) {
        dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CLEAR_FLAGS flags = stencil_state
                                            ? D3D12_CLEAR_FLAG_DEPTH |
                                                  D3D12_CLEAR_FLAG_STENCIL
                                            : D3D12_CLEAR_FLAG_DEPTH;
        list->ClearDepthStencilView(dsv, flags, 1.0f, 0, 0, nullptr);
    }
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(root);
    ID3D12DescriptorHeap *heaps[] = {uav_heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootDescriptorTable(
        0, uav_heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
    D3D12_RECT scissor = {0, 0, 1, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->OMSetRenderTargets(1, &rtv, FALSE,
                             dsv_heap && depth ? &dsv : nullptr);
    // Three identical overlapping primitives exercise an ordered
    // load/increment/store at one pixel.
    list->DrawInstanced(9, 1, 0, 0);
    (void)target;
}

static CaseResult run_buffer_case(
    ID3D12Device *device, ID3D12RootSignature *root,
    const std::vector<uint8_t> &vertex_shader,
    const std::vector<uint8_t> &pixel_shader, const char *name, bool structured,
    bool typed, bool depth_state = false, bool stencil_state = false) {
    CaseResult result = {};
    result.name = name;
    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = create_graphics_pso(
        device, root, vertex_shader, pixel_shader, &pso, false, depth_state,
        stencil_state);
    if (FAILED(result.pso_hr) || !pso)
        return result;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *uav_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *depth = nullptr;
    ID3D12DescriptorHeap *dsv_heap = nullptr;
    HRESULT hr = S_OK;
    create_common_objects(device, &queue, &allocator, &list, hr);

    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc = {};
    uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_desc.NumDescriptors = 1;
    uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(
            &uav_heap_desc, IID_PPV_ARGS(&uav_heap));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(
            &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (depth_state && SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_heap_desc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&dsv_heap_desc,
                                          IID_PPV_ARGS(&dsv_heap));
    }

    const D3D12_RESOURCE_DESC output_desc =
        buffer_desc(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(64);
    D3D12_HEAP_PROPERTIES default_heap =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));
    if (depth_state && SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC depth_desc = target_desc;
        depth_desc.Format = stencil_state ? DXGI_FORMAT_D24_UNORM_S8_UINT
                                          : DXGI_FORMAT_D32_FLOAT;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = depth_desc.Format;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth));
    }

    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = structured ? 1 : 16;
        if (structured) {
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.StructureByteStride = sizeof(uint32_t);
        } else if (typed) {
            uav.Format = DXGI_FORMAT_R32_UINT;
        } else {
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        device->CreateUnorderedAccessView(
            output, nullptr, &uav,
            uav_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(
            target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        if (depth_state && depth)
            device->CreateDepthStencilView(
                depth, nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
        const UINT clear_uav[4] = {};
        list->SetDescriptorHeaps(1, &uav_heap);
        list->ClearUnorderedAccessViewUint(
            uav_heap->GetGPUDescriptorHandleForHeapStart(),
            uav_heap->GetCPUDescriptorHandleForHeapStart(), output, clear_uav, 0,
            nullptr);
        record_draw(list, pso, root, uav_heap, rtv_heap, target, dsv_heap,
                    depth, stencil_state);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = output;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback, 0, output, 0, sizeof(uint32_t));
        result.execute_hr = execute_and_wait(device, queue, list);
    } else {
        result.execute_hr = hr;
    }

    if (SUCCEEDED(result.execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(result.value)};
        result.readback = SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped;
        if (result.readback) {
            std::memcpy(&result.value, mapped, sizeof(result.value));
            readback->Unmap(0, nullptr);
        }
    }
    result.exact = result.readback && result.value == 3u;

    safe_release(dsv_heap);
    safe_release(depth);
    safe_release(target);
    safe_release(readback);
    safe_release(output);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    return result;
}

static CaseResult run_texture_case(
    ID3D12Device *device, ID3D12RootSignature *root,
    const std::vector<uint8_t> &vertex_shader,
    const std::vector<uint8_t> &pixel_shader, const char *name,
    DXGI_FORMAT uav_format, bool array_view, uint32_t expected_value) {
    CaseResult result = {};
    result.name = name;
    result.expected_value = expected_value;
    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = create_graphics_pso(
        device, root, vertex_shader, pixel_shader, &pso);
    if (FAILED(result.pso_hr) || !pso)
        return result;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *uav_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12Resource *target = nullptr;
    HRESULT hr = S_OK;
    create_common_objects(device, &queue, &allocator, &list, hr);

    D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc = {};
    uav_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uav_heap_desc.NumDescriptors = 1;
    uav_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(
            &uav_heap_desc, IID_PPV_ARGS(&uav_heap));
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(
            &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));

    D3D12_RESOURCE_DESC output_desc = {};
    output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_desc.Width = 1;
    output_desc.Height = 1;
    output_desc.DepthOrArraySize = array_view ? 2 : 1;
    output_desc.MipLevels = 1;
    output_desc.Format = uav_format;
    output_desc.SampleDesc.Count = 1;
    output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&output));
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&output_desc, 0, 1, 0, &footprint,
                                      &rows, &row_size, &total_bytes);
        if (total_bytes == 0)
            hr = E_FAIL;
    }
    const D3D12_RESOURCE_DESC readback_desc = buffer_desc(total_bytes);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target));

    if (SUCCEEDED(hr)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = uav_format;
        uav.ViewDimension = array_view
                                  ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
                                  : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (array_view) {
            uav.Texture2DArray.MipSlice = 0;
            uav.Texture2DArray.FirstArraySlice = 0;
            uav.Texture2DArray.ArraySize = 2;
            uav.Texture2DArray.PlaneSlice = 0;
        } else {
            uav.Texture2D.MipSlice = 0;
            uav.Texture2D.PlaneSlice = 0;
        }
        device->CreateUnorderedAccessView(
            output, nullptr, &uav,
            uav_heap->GetCPUDescriptorHandleForHeapStart());
        device->CreateRenderTargetView(
            target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        const UINT clear_uav[4] = {};
        list->SetDescriptorHeaps(1, &uav_heap);
        list->ClearUnorderedAccessViewUint(
            uav_heap->GetGPUDescriptorHandleForHeapStart(),
            uav_heap->GetCPUDescriptorHandleForHeapStart(), output, clear_uav, 0,
            nullptr);
        record_draw(list, pso, root, uav_heap, rtv_heap, target);

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
        source.SubresourceIndex = array_view ? 1 : 0;
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
        result.readback = SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped;
        if (result.readback) {
            std::memcpy(&result.value, mapped, sizeof(result.value));
            readback->Unmap(0, nullptr);
        }
    }
    result.exact = result.readback && result.value == result.expected_value;

    safe_release(target);
    safe_release(readback);
    safe_release(output);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    return result;
}

static void print_case(const CaseResult &result, bool last) {
    std::printf(
        "    {\"name\":\"%s\",\"pso\":\"%s\",\"execute\":\"%s\","
        "\"readback\":%s,\"uav_value\":%u,\"expected_uav_value\":%u,"
        "\"exact\":%s}%s\n",
        result.name, hr_hex(result.pso_hr).c_str(),
        hr_hex(result.execute_hr).c_str(), result.readback ? "true" : "false",
        result.value, result.expected_value, result.exact ? "true" : "false",
        last ? "" : ",");
}

static void print_rejection(const CaseResult &result, bool last) {
    std::printf(
        "    {\"name\":\"%s\",\"pso\":\"%s\","
        "\"expected_reject\":true,\"rejected\":%s,\"object_null\":%s}%s\n",
        result.name, hr_hex(result.pso_hr).c_str(),
        result.exact ? "true" : "false", result.object_null ? "true" : "false",
        last ? "" : ",");
}

int main(int argc, char **argv) {
    if (argc != 1 && argc != 6 && argc != 8 && argc != 10) {
        std::fprintf(stderr,
                     "usage: probe_rov [<vs.cso> <raw-ps.cso> "
                     "<texture-ps.cso> <structured-ps.cso> "
                     "<typed-ps.cso> [<array-ps.cso> <float-ps.cso> "
                     "<vertex-rov.cso> <compute-rov.cso>]]\n");
        return 2;
    }

    const char *vertex_path = argc >= 6 ? argv[1] : "probe_rov_vs.cso";
    const char *raw_path = argc >= 6 ? argv[2] : "probe_rov_raw_ps.cso";
    const char *texture_path = argc >= 6 ? argv[3] : "probe_rov_texture_ps.cso";
    const char *structured_path =
        argc >= 6 ? argv[4] : "probe_rov_structured_ps.cso";
    const char *typed_path =
        argc >= 6 ? argv[5] : "probe_rov_typed_ps.cso";
    const char *array_path = argc >= 8 ? argv[6] : "probe_rov_array_ps.cso";
    const char *float_path = argc >= 8 ? argv[7] : "probe_rov_float_ps.cso";
    const char *vertex_rov_path = argc == 10 ? argv[8] : "probe_rov_vertex.cso";
    const char *compute_rov_path = argc == 10 ? argv[9] : "probe_rov_compute.cso";
    const bool extended_texture_matrix = argc >= 8;
    const bool stage_rejection_matrix = argc == 10;
    const auto vertex_shader = read_binary_file(vertex_path);
    const auto raw_shader = read_binary_file(raw_path);
    const auto texture_shader = read_binary_file(texture_path);
    const auto structured_shader = read_binary_file(structured_path);
    const auto typed_shader = read_binary_file(typed_path);
    const auto array_shader = read_binary_file(array_path);
    const auto float_shader = read_binary_file(float_path);
    const auto vertex_rov_shader = read_binary_file(vertex_rov_path);
    const auto compute_rov_shader = read_binary_file(compute_rov_path);

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

    CaseResult raw = {};
    raw.name = "byte_address_buffer";
    CaseResult texture = {};
    texture.name = "typed_texture2d_uint";
    CaseResult structured = {};
    structured.name = "structured_buffer";
    CaseResult typed = {};
    typed.name = "typed_buffer_uint";
    CaseResult array = {};
    array.name = "typed_texture2d_array_uint";
    CaseResult floating = {};
    floating.name = "typed_texture2d_float";
    floating.expected_value = 0x40400000u;
    CaseResult depth_state = {};
    depth_state.name = "byte_address_buffer_depth_state";
    CaseResult stencil_state = {};
    stencil_state.name = "byte_address_buffer_stencil_state";
    CaseResult independent_logic_rejected = {};
    independent_logic_rejected.name = "independent_logic_uav_rejected";
    CaseResult vertex_rov_rejected = {};
    vertex_rov_rejected.name = "vertex_rov_rejected";
    CaseResult compute_rov_rejected = {};
    compute_rov_rejected.name = "compute_rov_rejected";
    if (SUCCEEDED(create_hr) && SUCCEEDED(root_hr)) {
        if (!vertex_shader.empty() && !raw_shader.empty())
            raw = run_buffer_case(device, root, vertex_shader, raw_shader,
                                  "byte_address_buffer", false, false);
        if (!vertex_shader.empty() && !texture_shader.empty())
            texture = run_texture_case(device, root, vertex_shader,
                                       texture_shader, "typed_texture2d_uint",
                                       DXGI_FORMAT_R32_UINT, false, 3u);
        if (!vertex_shader.empty() && !structured_shader.empty())
            structured = run_buffer_case(device, root, vertex_shader,
                                         structured_shader, "structured_buffer",
                                         true, false);
        if (!vertex_shader.empty() && !typed_shader.empty())
            typed = run_buffer_case(device, root, vertex_shader, typed_shader,
                                    "typed_buffer_uint", false, true);
        if (stage_rejection_matrix && !vertex_shader.empty() &&
            !raw_shader.empty()) {
            depth_state = run_buffer_case(
                device, root, vertex_shader, raw_shader,
                "byte_address_buffer_depth_state", false, false, true);
            stencil_state = run_buffer_case(
                device, root, vertex_shader, raw_shader,
                "byte_address_buffer_stencil_state", false, false, true, true);
        }
        if (extended_texture_matrix && !vertex_shader.empty() &&
            !array_shader.empty())
            array = run_texture_case(device, root, vertex_shader, array_shader,
                                     "typed_texture2d_array_uint",
                                     DXGI_FORMAT_R32_UINT, true, 3u);
        if (extended_texture_matrix && !vertex_shader.empty() &&
            !float_shader.empty())
            floating = run_texture_case(device, root, vertex_shader,
                                        float_shader, "typed_texture2d_float",
                                        DXGI_FORMAT_R32_FLOAT, false,
                                        0x40400000u);
        if (stage_rejection_matrix && !vertex_rov_shader.empty() &&
            !raw_shader.empty()) {
            ID3D12PipelineState *rejected_pso = nullptr;
            vertex_rov_rejected.pso_hr = create_graphics_pso(
                device, root, vertex_rov_shader, raw_shader, &rejected_pso);
            vertex_rov_rejected.object_null = rejected_pso == nullptr;
            vertex_rov_rejected.exact =
                (static_cast<uint32_t>(vertex_rov_rejected.pso_hr) &
                 0x80000000u) != 0 && vertex_rov_rejected.object_null;
            safe_release(rejected_pso);
        }
        if (stage_rejection_matrix && !compute_rov_shader.empty()) {
            ID3D12PipelineState *rejected_pso = nullptr;
            compute_rov_rejected.pso_hr = create_compute_pso(
                device, root, compute_rov_shader, &rejected_pso);
            compute_rov_rejected.object_null = rejected_pso == nullptr;
            compute_rov_rejected.exact =
                (static_cast<uint32_t>(compute_rov_rejected.pso_hr) &
                 0x80000000u) != 0 && compute_rov_rejected.object_null;
            safe_release(rejected_pso);
        }
        if (!vertex_shader.empty() && !raw_shader.empty()) {
            ID3D12PipelineState *rejected_pso = nullptr;
            independent_logic_rejected.pso_hr = create_graphics_pso(
                device, root, vertex_shader, raw_shader, &rejected_pso, true);
            independent_logic_rejected.object_null = rejected_pso == nullptr;
            independent_logic_rejected.exact =
                (static_cast<uint32_t>(independent_logic_rejected.pso_hr) &
                 0x80000000u) != 0 && independent_logic_rejected.object_null;
            safe_release(rejected_pso);
        }
    }

    const bool all_exact = raw.exact && texture.exact && structured.exact &&
                           typed.exact &&
                           (!stage_rejection_matrix ||
                            (depth_state.exact && stencil_state.exact)) &&
                           (!extended_texture_matrix ||
                            (array.exact && floating.exact)) &&
                           (!stage_rejection_matrix ||
                            (vertex_rov_rejected.exact &&
                             compute_rov_rejected.exact)) &&
                           independent_logic_rejected.exact;
    const std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
    std::printf(
        "{\n"
        "  \"schema\": \"metalsharp.d3d12-metal.rov.v2\",\n"
        "  \"profile\": \"%s\",\n"
        "  \"provider\": \"metal_raster_order_group\",\n"
        "  \"create_device\": \"%s\",\n"
        "  \"root_signature\": \"%s\",\n"
        "  \"overlap_triangles\": 3,\n"
        "  \"cases\": [\n",
        json_escape(profile).c_str(), hr_hex(create_hr).c_str(),
        hr_hex(root_hr).c_str());
    print_case(raw, false);
    print_case(texture, false);
    print_case(structured, false);
    print_case(typed, false);
    if (extended_texture_matrix) {
        print_case(array, false);
        print_case(floating, false);
    }
    if (stage_rejection_matrix) {
        print_case(depth_state, false);
        print_case(stencil_state, false);
    }
    if (stage_rejection_matrix) {
        print_rejection(vertex_rov_rejected, false);
        print_rejection(compute_rov_rejected, false);
    }
    print_rejection(independent_logic_rejected, true);
    std::printf(
        "  ],\n"
        "  \"exact\": %s,\n"
        "  \"bounded_scope\": \"pixel ROV raw, typed, structured buffer, typed 2D/2D-array texture resources; D32/D24S8 state; one-pixel three-primitive ordered load/store; vertex/compute and independent-logic rejection\"\n"
        "}\n",
        all_exact ? "true" : "false");
    std::fflush(stdout);

    // Avoid late CRT teardown after the Wine Metal bridge has completed its
    // process-scoped worker and command-buffer cleanup.
    TerminateProcess(GetCurrentProcess(), all_exact ? 0u : 1u);
    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return all_exact ? 0 : 1;
}
