#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

static const GUID IID_D3D12DeviceProbe = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID IID_D3D12Device2Probe = {
    0x30baa41e, 0xb15b, 0x475c,
    {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};

using D3D12CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                                REFIID, void **);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI *)(
    const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
    ID3DBlob **);
using D3DCompileFn = HRESULT(WINAPI *)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
    LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

template <typename T> static void safe_release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T fn = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(fn) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

static std::string hr_hex(HRESULT hr) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return text;
}

static HRESULT compile_shader(const char *source, const char *entry,
                              const char *target, ID3DBlob **blob,
                              std::string &errors) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompileFn compile = load_proc<D3DCompileFn>(compiler, "D3DCompile");
    if (!compile)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3DBlob *error_blob = nullptr;
    HRESULT hr = compile(source, std::strlen(source),
                         "probe_rasterization_breadth.hlsl", nullptr, nullptr,
                         entry, target, 0, 0, blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char *>(error_blob->GetBufferPointer()),
                      error_blob->GetBufferSize());
        error_blob->Release();
    }
    return hr;
}

static HRESULT create_root_signature(ID3D12Device *device,
                                     ID3D12RootSignature **root,
                                     std::string &errors) {
    *root = nullptr;
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    D3D12SerializeRootSignatureFn serialize =
        load_proc<D3D12SerializeRootSignatureFn>(
            d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr;
    ID3DBlob *error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                           &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char *>(error_blob->GetBufferPointer()),
                      error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr) && blob) {
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    }
    safe_release(blob);
    return hr;
}

static HRESULT execute_and_wait(ID3D12Device *device, ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
    HRESULT hr = list ? list->Close() : E_FAIL;
    if (FAILED(hr))
        return hr;
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence *fence = nullptr;
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr))
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr))
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr))
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && !event_handle)
        hr = HRESULT_FROM_WIN32(GetLastError());
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

static D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return desc;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

static bool is_red_pixel(const uint8_t *pixel) {
    return pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 &&
           pixel[3] == 255;
}

struct LegacyTopologyResult {
    HRESULT pso_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    uint32_t red_pixels = 0;
    uint32_t red_rows = 0;
    uint32_t red_columns = 0;
    uint32_t covered_pixels = 0;
    uint32_t coverage_units = 0;
    bool nonzero = false;
    bool exact_shape = false;
};

static LegacyTopologyResult run_legacy_topology(
    ID3D12Device *device, ID3D12RootSignature *root, ID3DBlob *vs,
    ID3DBlob *ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type,
    D3D12_PRIMITIVE_TOPOLOGY topology, UINT vertex_count) {
    constexpr UINT width = 16;
    constexpr UINT height = 16;
    constexpr UINT row_pitch = 256;
    LegacyTopologyResult result;
    if (!device || !root || !vs || !ps)
        return result;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    for (auto &rt : desc.BlendState.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.InputLayout = {nullptr, 0};
    desc.PrimitiveTopologyType = topology_type;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&pso));
    if (FAILED(result.pso_hr))
        return result;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    UINT descriptor_size = 0;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_HEAP_PROPERTIES default_heap =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap =
        heap_properties(D3D12_HEAP_TYPE_READBACK);
    HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        allocator, nullptr,
                                        IID_PPV_ARGS(&list));
    D3D12_RESOURCE_DESC target_desc = texture_desc(width, height);
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = target_desc.Format;
    clear_value.Color[0] = 0.0f;
    clear_value.Color[1] = 0.0f;
    clear_value.Color[2] = 0.0f;
    clear_value.Color[3] = 1.0f;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr))
        descriptor_size = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    if (rtv_heap) {
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
    }
    UINT64 total_bytes = 0;
    UINT64 row_size = 0;
    UINT num_rows = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    if (SUCCEEDED(hr))
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint,
                                      &num_rows, &row_size, &total_bytes);
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = std::max<UINT64>(total_bytes, row_pitch * height);
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr)) {
        const FLOAT clear_color[4] = {0, 0, 0, 1};
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
        D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width),
                                   static_cast<float>(height), 0, 1};
        D3D12_RECT scissor = {0, 0, static_cast<LONG>(width),
                              static_cast<LONG>(height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root);
        list->SetPipelineState(pso);
        list->IASetPrimitiveTopology(topology);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(vertex_count, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        result.execute_hr = execute_and_wait(device, queue, list);
    } else {
        result.execute_hr = hr;
    }

    if (SUCCEEDED(result.execute_hr)) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(readback_desc.Width)};
        result.map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(result.map_hr) && mapped) {
            const auto *bytes = static_cast<const uint8_t *>(mapped);
            for (UINT y = 0; y < height; ++y) {
                bool row_red = false;
                for (UINT x = 0; x < width; ++x) {
                    if (is_red_pixel(bytes + y * footprint.Footprint.RowPitch +
                                     x * 4)) {
                        ++result.red_pixels;
                        row_red = true;
                        result.nonzero = true;
                    }
                }
                result.red_rows += row_red ? 1u : 0u;
            }
            for (UINT x = 0; x < width; ++x) {
                for (UINT y = 0; y < height; ++y) {
                    if (is_red_pixel(bytes + y * footprint.Footprint.RowPitch +
                                     x * 4)) {
                        ++result.red_columns;
                        break;
                    }
                }
            }
            readback->Unmap(0, nullptr);
        }
    }
    // This fixture deliberately uses a 16x16 target and endpoints at +/-0.9.
    // The exact native Metal readback is one point pixel and a 14-pixel
    // horizontal line on the pinned Apple M4 rasterizer.  Keep the expected
    // shape strict so a successful PSO with a dropped/changed primitive cannot
    // masquerade as provider evidence.
    result.exact_shape =
        (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE &&
         result.red_rows == 1 && result.red_columns == 14 &&
         result.red_pixels == 14) ||
        (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT &&
         result.red_pixels == 1 && result.red_rows == 1 &&
         result.red_columns == 1);

    safe_release(readback);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    (void)descriptor_size;
    (void)num_rows;
    (void)row_size;
    return result;
}

// The Agility 1.619.5 header has Rasterizer2 but the older MinGW D3D12 header
// used to build this probe does not. The wire values and fields are stable.
struct RasterizerDesc2Probe {
    D3D12_FILL_MODE FillMode;
    D3D12_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    FLOAT DepthBias;
    FLOAT DepthBiasClamp;
    FLOAT SlopeScaledDepthBias;
    BOOL DepthClipEnable;
    UINT LineRasterizationMode;
    UINT ForcedSampleCount;
    D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

struct PipelineStreamBuilder {
    std::vector<uint8_t> bytes;

    template <typename T>
    void append(uint32_t type, const T &value) {
        const size_t aligned = (bytes.size() + 7u) & ~size_t(7u);
        const size_t payload_offset =
            (sizeof(uint32_t) + alignof(T) - 1u) & ~(alignof(T) - 1u);
        const size_t end = aligned + payload_offset + sizeof(T);
        bytes.resize((end + 7u) & ~size_t(7u));
        std::memcpy(bytes.data() + aligned, &type, sizeof(type));
        std::memcpy(bytes.data() + aligned + payload_offset, &value,
                    sizeof(value));
    }
};

struct RTFormatArrayProbe {
    DXGI_FORMAT formats[8];
    UINT count;
};

static LegacyTopologyResult run_rasterizer2_line(
    ID3D12Device2 *device2, ID3D12RootSignature *root, ID3DBlob *vs,
    ID3DBlob *ps, UINT line_mode, UINT sample_count = 1,
    D3D12_PRIMITIVE_TOPOLOGY draw_topology =
        D3D_PRIMITIVE_TOPOLOGY_LINELIST) {
    LegacyTopologyResult result;
    if (!device2 || !root || !vs || !ps)
        return result;
    D3D12_BLEND_DESC blend = {};
    for (auto &rt : blend.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    UINT sample_mask = UINT_MAX;
    RasterizerDesc2Probe rasterizer = {};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.LineRasterizationMode = line_mode;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_INPUT_ELEMENT_DESC position_element = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_INPUT_LAYOUT_DESC input = {&position_element, 1};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    RTFormatArrayProbe formats = {};
    formats.formats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    formats.count = 1;
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample = {sample_count, 0};
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    PipelineStreamBuilder stream;
    stream.append(0, root);
    D3D12_SHADER_BYTECODE vs_bytecode = {vs->GetBufferPointer(),
                                         vs->GetBufferSize()};
    D3D12_SHADER_BYTECODE ps_bytecode = {ps->GetBufferPointer(),
                                         ps->GetBufferSize()};
    stream.append(1, vs_bytecode);
    stream.append(2, ps_bytecode);
    stream.append(8, blend);
    stream.append(9, sample_mask);
    stream.append(28, rasterizer);
    stream.append(11, depth);
    stream.append(12, input);
    stream.append(14, topology);
    stream.append(15, formats);
    stream.append(16, dsv_format);
    stream.append(17, sample);
    stream.append(20, flags);

    D3D12_PIPELINE_STATE_STREAM_DESC desc = {};
    desc.SizeInBytes = stream.bytes.size();
    desc.pPipelineStateSubobjectStream = stream.bytes.data();
    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    if (SUCCEEDED(result.pso_hr)) {
        // Reuse the normal legacy draw helper by executing through a temporary
        // legacy descriptor is not possible; the stream result is validated by
        // creation here and the raster output is performed below.
        constexpr UINT width = 16;
        constexpr UINT height = 16;
        D3D12_RESOURCE_DESC target_desc = texture_desc(width, height);
        target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        target_desc.SampleDesc.Count = sample_count;
        D3D12_RESOURCE_DESC resolve_desc = texture_desc(width, height);
        resolve_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        clear.Color[3] = 1.0f;
        ID3D12CommandQueue *queue = nullptr;
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        ID3D12Resource *target = nullptr;
        ID3D12Resource *resolve_target = nullptr;
        ID3D12Resource *vertex_buffer = nullptr;
        ID3D12Resource *readback = nullptr;
        ID3D12DescriptorHeap *rtv_heap = nullptr;
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_HEAP_PROPERTIES upload_heap =
            heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        HRESULT hr = device2->CreateCommandQueue(&queue_desc,
                                                   IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr))
            hr = device2->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device2->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator, pso,
                                             IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr))
            hr = device2->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&target));
        if (SUCCEEDED(hr) && sample_count > 1)
            hr = device2->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &resolve_desc,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&resolve_target));
        D3D12_RESOURCE_DESC vertex_desc = {};
        vertex_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vertex_desc.Width = 9 * sizeof(float);
        vertex_desc.Height = 1;
        vertex_desc.DepthOrArraySize = 1;
        vertex_desc.MipLevels = 1;
        vertex_desc.SampleDesc.Count = 1;
        vertex_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(hr))
            hr = device2->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&vertex_buffer));
        if (SUCCEEDED(hr)) {
            float vertices[9] = {-0.9f, 0.0f, 0.0f,
                                  0.9f, 0.0f, 0.0f,
                                  0.9f, 0.0f, 0.0f};
            if (draw_topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) {
                vertices[3] = 0.0f;
                vertices[6] = 0.9f;
            }
            void *mapped = nullptr;
            D3D12_RANGE empty = {0, 0};
            hr = vertex_buffer->Map(0, &empty, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                std::memcpy(mapped, vertices, sizeof(vertices));
                vertex_buffer->Unmap(0, nullptr);
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        if (SUCCEEDED(hr))
            hr = device2->CreateDescriptorHeap(&heap_desc,
                                               IID_PPV_ARGS(&rtv_heap));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        if (rtv_heap) {
            rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            device2->CreateRenderTargetView(target, nullptr, rtv);
        }
        UINT64 total_bytes = 0;
        UINT64 row_size = 0;
        UINT rows = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        if (SUCCEEDED(hr))
            device2->GetCopyableFootprints(&resolve_desc, 0, 1, 0, &footprint,
                                           &rows, &row_size, &total_bytes);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = total_bytes;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(hr))
            hr = device2->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr)) {
            const FLOAT clear_color[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
            D3D12_VIEWPORT viewport = {0, 0, static_cast<float>(width),
                                       static_cast<float>(height), 0, 1};
            D3D12_RECT scissor = {0, 0, static_cast<LONG>(width),
                                  static_cast<LONG>(height)};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(root);
            list->SetPipelineState(pso);
            D3D12_VERTEX_BUFFER_VIEW vbv = {};
            vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = 9 * sizeof(float);
            vbv.StrideInBytes = 3 * sizeof(float);
            list->IASetVertexBuffers(0, 1, &vbv);
            list->IASetPrimitiveTopology(draw_topology);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->DrawInstanced(
                draw_topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP ? 3 : 2,
                1, 0, 0);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target;
            barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter =
                sample_count > 1 ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                 : D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);
            if (sample_count > 1) {
                list->ResolveSubresource(resolve_target, 0, target, 0,
                                         DXGI_FORMAT_R32G32B32A32_FLOAT);
                barrier.Transition.pResource = resolve_target;
                barrier.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RESOLVE_DEST;
                barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &barrier);
            }
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = sample_count > 1 ? resolve_target : target;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = readback;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            result.execute_hr = execute_and_wait(device2, queue, list);
        } else {
            result.execute_hr = hr;
        }
        if (SUCCEEDED(result.execute_hr)) {
            void *mapped = nullptr;
            D3D12_RANGE range = {0, static_cast<SIZE_T>(readback_desc.Width)};
            result.map_hr = readback->Map(0, &range, &mapped);
            if (SUCCEEDED(result.map_hr) && mapped) {
                const auto *bytes = static_cast<const uint8_t *>(mapped);
                for (UINT y = 0; y < height; ++y) {
                    bool row_has_red = false;
                    for (UINT x = 0; x < width; ++x) {
                        const auto *pixel = reinterpret_cast<const float *>(
                            bytes + y * footprint.Footprint.RowPitch + x * 16);
                        if (pixel[0] > 0.0f && pixel[1] == 0.0f &&
                            pixel[2] == 0.0f && pixel[3] == 1.0f) {
                            ++result.covered_pixels;
                            result.coverage_units += static_cast<uint32_t>(
                                pixel[0] * sample_count + 0.5f);
                            result.nonzero = true;
                            row_has_red = true;
                            if (pixel[0] == 1.0f)
                                ++result.red_pixels;
                        }
                    }
                    result.red_rows += row_has_red ? 1u : 0u;
                }
                readback->Unmap(0, nullptr);
            }
        }
        const UINT expected_rows = line_mode == 2 ? 2u : 1u;
        const UINT expected_pixels = 14u * expected_rows;
        if (sample_count == 1) {
            result.exact_shape = result.red_pixels == expected_pixels &&
                                 result.red_rows == expected_rows;
        } else if (sample_count == 2) {
            result.exact_shape = result.red_pixels == 0 &&
                                 result.red_rows == 2 &&
                                 result.covered_pixels == 28 &&
                                 result.coverage_units == 28;
        } else if (sample_count == 4 && line_mode == 2) {
            result.exact_shape = result.red_pixels == 0 &&
                                 result.red_rows == 2 &&
                                 result.covered_pixels == 32 &&
                                 result.coverage_units == 88;
        } else if (sample_count == 4 && line_mode == 3) {
            result.exact_shape = result.red_pixels == 0 &&
                                 result.red_rows == 2 &&
                                 result.covered_pixels == 30 &&
                                 result.coverage_units == 58;
        }
        safe_release(readback);
        safe_release(resolve_target);
        safe_release(vertex_buffer);
        safe_release(target);
        safe_release(rtv_heap);
        safe_release(list);
        safe_release(allocator);
        safe_release(queue);
    }
    safe_release(pso);
    return result;
}

struct InvalidRasterizer2Result {
    HRESULT pso_hr = E_FAIL;
    bool object_null = true;
    bool exact = false;
};

static InvalidRasterizer2Result run_invalid_rasterizer2(
    ID3D12Device2 *device2, ID3D12RootSignature *root, ID3DBlob *vs,
    ID3DBlob *ps) {
    InvalidRasterizer2Result result;
    if (!device2 || !root || !vs || !ps)
        return result;
    D3D12_BLEND_DESC blend = {};
    for (auto &rt : blend.RenderTarget) {
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    UINT sample_mask = UINT_MAX;
    RasterizerDesc2Probe rasterizer = {};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.LineRasterizationMode = 4;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_INPUT_LAYOUT_DESC input = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    RTFormatArrayProbe formats = {};
    formats.formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.count = 1;
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample = {1, 0};
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    PipelineStreamBuilder stream;
    stream.append(0, root);
    D3D12_SHADER_BYTECODE vs_bytecode = {vs->GetBufferPointer(),
                                         vs->GetBufferSize()};
    D3D12_SHADER_BYTECODE ps_bytecode = {ps->GetBufferPointer(),
                                         ps->GetBufferSize()};
    stream.append(1, vs_bytecode);
    stream.append(2, ps_bytecode);
    stream.append(8, blend);
    stream.append(9, sample_mask);
    stream.append(28, rasterizer);
    stream.append(11, depth);
    stream.append(12, input);
    stream.append(14, topology);
    stream.append(15, formats);
    stream.append(16, dsv_format);
    stream.append(17, sample);
    stream.append(20, flags);
    D3D12_PIPELINE_STATE_STREAM_DESC desc = {stream.bytes.size(),
                                             stream.bytes.data()};
    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    result.object_null = pso == nullptr;
    result.exact = result.pso_hr == E_INVALIDARG && result.object_null;
    safe_release(pso);
    return result;
}

int main() {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    D3D12CreateDeviceFn create_device =
        load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            IID_D3D12DeviceProbe,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    std::string errors;
    ID3D12RootSignature *root = nullptr;
    HRESULT root_hr = SUCCEEDED(create_hr)
                          ? create_root_signature(device, &root, errors)
                          : E_FAIL;
    static const char *hlsl = R"HLSL(
struct VSOut { float4 position : SV_POSITION; float4 color : COLOR0; };
VSOut vs(uint id : SV_VertexID) {
  VSOut o;
  float2 positions[4] = {
    float2(-0.9, 0.0), float2(0.9, 0.0),
    float2(0.0, 0.0), float2(0.0, 0.0)
  };
  o.position = float4(positions[id], 0.0, 1.0);
  o.color = float4(1.0, 0.0, 0.0, 1.0);
  return o;
}
float4 ps(VSOut input) : SV_Target0 { return input.color; }
)HLSL";
    static const char *quadrilateral_hlsl = R"HLSL(
struct VSOut { float4 position : SV_POSITION; };
VSOut vs(float3 position : POSITION) {
  VSOut o;
  o.position = float4(position, 1.0);
  return o;
}
float4 ps() : SV_Target0 { return float4(1.0, 0.0, 0.0, 1.0); }
)HLSL";
    ID3DBlob *vs = nullptr;
    ID3DBlob *ps = nullptr;
    ID3DBlob *quadrilateral_vs = nullptr;
    ID3DBlob *quadrilateral_ps = nullptr;
    HRESULT vs_hr = compile_shader(hlsl, "vs", "vs_5_0", &vs, errors);
    HRESULT ps_hr = compile_shader(hlsl, "ps", "ps_5_0", &ps, errors);
    HRESULT quadrilateral_vs_hr = compile_shader(
        quadrilateral_hlsl, "vs", "vs_5_0", &quadrilateral_vs, errors);
    HRESULT quadrilateral_ps_hr = compile_shader(
        quadrilateral_hlsl, "ps", "ps_5_0", &quadrilateral_ps, errors);

    LegacyTopologyResult point = run_legacy_topology(
        device, root, vs, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,
        D3D_PRIMITIVE_TOPOLOGY_POINTLIST, 1);
    LegacyTopologyResult line = run_legacy_topology(
        device, root, vs, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
        D3D_PRIMITIVE_TOPOLOGY_LINELIST, 2);

    ID3D12Device2 *device2 = nullptr;
    HRESULT device2_hr = device
                             ? device->QueryInterface(IID_D3D12Device2Probe,
                                                      reinterpret_cast<void **>(
                                                          &device2))
                             : E_FAIL;
    LegacyTopologyResult rasterizer2[4] = {};
    for (UINT mode = 0; mode < 4; ++mode)
        rasterizer2[mode] = run_rasterizer2_line(
            device2, root, quadrilateral_vs, quadrilateral_ps, mode);
    LegacyTopologyResult quadrilateral_strip[2] = {
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 2, 1,
                             D3D_PRIMITIVE_TOPOLOGY_LINESTRIP),
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 3, 1,
                             D3D_PRIMITIVE_TOPOLOGY_LINESTRIP),
    };
    LegacyTopologyResult quadrilateral_msaa[4] = {
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 2, 2),
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 2, 4),
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 3, 2),
        run_rasterizer2_line(device2, root, quadrilateral_vs,
                             quadrilateral_ps, 3, 4),
    };
    InvalidRasterizer2Result invalid_rasterizer2 =
        run_invalid_rasterizer2(device2, root, vs, ps);

    bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) && SUCCEEDED(vs_hr) &&
                SUCCEEDED(ps_hr) && SUCCEEDED(quadrilateral_vs_hr) &&
                SUCCEEDED(quadrilateral_ps_hr) && point.exact_shape &&
                line.exact_shape;
    bool rasterizer2_created = SUCCEEDED(device2_hr);
    for (const auto &result : rasterizer2)
        rasterizer2_created &= SUCCEEDED(result.pso_hr) && result.exact_shape;
    bool quadrilateral_strip_exact = true;
    for (const auto &result : quadrilateral_strip)
        quadrilateral_strip_exact &=
            SUCCEEDED(result.pso_hr) && result.exact_shape;
    bool quadrilateral_msaa_exact = true;
    for (const auto &result : quadrilateral_msaa)
        quadrilateral_msaa_exact &=
            SUCCEEDED(result.pso_hr) && result.exact_shape;
    pass = pass && rasterizer2_created && quadrilateral_strip_exact &&
           quadrilateral_msaa_exact && invalid_rasterizer2.exact;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12.rasterization-breadth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("  \"root_hr\": \"%s\",\n", hr_hex(root_hr).c_str());
    std::printf("  \"shader_compile_hr\": [\"%s\", \"%s\"],\n",
                hr_hex(vs_hr).c_str(), hr_hex(ps_hr).c_str());
    std::printf("  \"point\": {\"pso_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"red_pixels\": %u, \"exact_shape\": %s},\n",
                hr_hex(point.pso_hr).c_str(), hr_hex(point.execute_hr).c_str(),
                hr_hex(point.map_hr).c_str(), point.red_pixels,
                point.exact_shape ? "true" : "false");
    std::printf("  \"line\": {\"pso_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"red_pixels\": %u, \"red_rows\": %u, \"exact_shape\": %s},\n",
                hr_hex(line.pso_hr).c_str(), hr_hex(line.execute_hr).c_str(),
                hr_hex(line.map_hr).c_str(), line.red_pixels, line.red_rows,
                line.exact_shape ? "true" : "false");
    std::printf("  \"device2_hr\": \"%s\",\n", hr_hex(device2_hr).c_str());
    std::printf("  \"rasterizer2_pso_created\": %s,\n",
                rasterizer2_created ? "true" : "false");
    std::printf("  \"rasterizer2\": [\n");
    for (UINT mode = 0; mode < 4; ++mode) {
        const auto &result = rasterizer2[mode];
        const UINT expected_rows = mode == 2 ? 2u : 1u;
        std::printf("    {\"mode\": %u, \"pso_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"red_pixels\": %u, \"red_rows\": %u, \"expected_pixels\": %u, \"expected_rows\": %u, \"exact_shape\": %s}%s\n",
                    mode, hr_hex(result.pso_hr).c_str(),
                    hr_hex(result.execute_hr).c_str(),
                    hr_hex(result.map_hr).c_str(), result.red_pixels,
                    result.red_rows, 14u * expected_rows, expected_rows,
                    result.exact_shape ? "true" : "false",
                    mode == 3 ? "" : ",");
    }
    std::printf("  ],\n");
    std::printf("  \"quadrilateral_line_strip\": [\n");
    for (UINT i = 0; i < 2; ++i) {
        const UINT mode = i + 2;
        const UINT expected_rows = mode == 2 ? 2u : 1u;
        const auto &result = quadrilateral_strip[i];
        std::printf("    {\"mode\": %u, \"pso_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"red_pixels\": %u, \"red_rows\": %u, \"expected_pixels\": %u, \"expected_rows\": %u, \"exact_shape\": %s}%s\n",
                    mode, hr_hex(result.pso_hr).c_str(),
                    hr_hex(result.execute_hr).c_str(),
                    hr_hex(result.map_hr).c_str(), result.red_pixels,
                    result.red_rows, 14u * expected_rows, expected_rows,
                    result.exact_shape ? "true" : "false", i == 1 ? "" : ",");
    }
    std::printf("  ],\n");
    std::printf("  \"quadrilateral_line_strip_exact\": %s,\n",
                quadrilateral_strip_exact ? "true" : "false");
    std::printf("  \"quadrilateral_msaa\": [\n");
    for (UINT i = 0; i < 4; ++i) {
        const UINT mode = i < 2 ? 2 : 3;
        const UINT samples = (i & 1u) ? 4 : 2;
        const UINT expected_covered =
            samples == 2 ? 28u : (mode == 2 ? 32u : 30u);
        const UINT expected_units =
            samples == 2 ? 28u : (mode == 2 ? 88u : 58u);
        const auto &result = quadrilateral_msaa[i];
        std::printf("    {\"mode\": %u, \"sample_count\": %u, \"pso_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"red_pixels\": %u, \"red_rows\": %u, \"covered_pixels\": %u, \"coverage_units\": %u, \"expected_covered_pixels\": %u, \"expected_coverage_units\": %u, \"expected_rows\": 2, \"exact_shape\": %s}%s\n",
                    mode, samples, hr_hex(result.pso_hr).c_str(),
                    hr_hex(result.execute_hr).c_str(),
                    hr_hex(result.map_hr).c_str(), result.red_pixels,
                    result.red_rows, result.covered_pixels,
                    result.coverage_units, expected_covered, expected_units,
                    result.exact_shape ? "true" : "false", i == 3 ? "" : ",");
    }
    std::printf("  ],\n");
    std::printf("  \"quadrilateral_msaa_exact\": %s,\n",
                quadrilateral_msaa_exact ? "true" : "false");
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"rasterizer2_invalid\": {\"pso_hr\": \"%s\", "
                "\"object_null\": %s, \"exact\": %s},\n",
                hr_hex(invalid_rasterizer2.pso_hr).c_str(),
                invalid_rasterizer2.object_null ? "true" : "false",
                invalid_rasterizer2.exact ? "true" : "false");
    std::printf("  \"rasterizer2_shape_evidence\": %s\n",
                rasterizer2_created ? "true" : "false");
    std::printf("}\n");

    safe_release(device2);
    safe_release(quadrilateral_ps);
    safe_release(quadrilateral_vs);
    safe_release(ps);
    safe_release(vs);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
