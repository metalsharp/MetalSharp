#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const GUID kDeviceIID = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID kDevice2IID = {
    0x30baa41e, 0xb15b, 0x475c,
    {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};

template <typename T> static void safe_release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T function = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(function) == sizeof(proc),
                  "function pointer size mismatch");
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

static std::string hr_hex(HRESULT hr) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return text;
}

struct ViewInstanceLocationProbe {
    UINT ViewportArrayIndex;
    UINT RenderTargetArrayIndex;
};

struct ViewInstancingDescProbe {
    UINT ViewInstanceCount;
    const ViewInstanceLocationProbe *pViewInstanceLocations;
    UINT Flags;
};

struct RTFormatArrayProbe {
    DXGI_FORMAT Formats[8];
    UINT NumRenderTargets;
};

struct StreamBuilder {
    std::vector<uint8_t> bytes;

    template <typename T> void append(UINT type, const T &value) {
        const size_t payload_offset =
            (sizeof(UINT) + alignof(T) - 1u) & ~(alignof(T) - 1u);
        const size_t subobject_size =
            (payload_offset + sizeof(T) + alignof(void *) - 1u) &
            ~(alignof(void *) - 1u);
        const size_t offset = bytes.size();
        bytes.resize(offset + subobject_size, 0);
        std::memcpy(bytes.data() + offset, &type, sizeof(type));
        std::memcpy(bytes.data() + offset + payload_offset, &value,
                    sizeof(value));
    }
};

struct Pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

static HRESULT compile_shader(const char *source, const char *entry,
                              const char *target, ID3DBlob **blob) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR,
                                        const D3D_SHADER_MACRO *, ID3DInclude *,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);
    CompileFn compile = load_proc<CompileFn>(compiler, "D3DCompile");
    if (!compile)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3DBlob *errors = nullptr;
    HRESULT hr = compile(source, std::strlen(source), "view_breadth.hlsl",
                         nullptr, nullptr, entry, target, 0, 0, blob, &errors);
    safe_release(errors);
    return hr;
}

static HRESULT create_root_signature(ID3D12Device *device,
                                     ID3D12RootSignature **root) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using SerializeFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    SerializeFn serialize =
        load_proc<SerializeFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                           &errors);
    safe_release(errors);
    if (SUCCEEDED(hr) && blob)
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

static StreamBuilder make_stream(ID3D12RootSignature *root, ID3DBlob *vs,
                                 ID3DBlob *ps,
                                 const ViewInstanceLocationProbe *locations,
                                 UINT view_count, UINT sample_count) {
    StreamBuilder stream;
    const D3D12_SHADER_BYTECODE vs_bytecode = {vs->GetBufferPointer(),
                                               vs->GetBufferSize()};
    const D3D12_SHADER_BYTECODE ps_bytecode = {ps->GetBufferPointer(),
                                               ps->GetBufferSize()};
    D3D12_BLEND_DESC blend = {};
    for (auto &target : blend.RenderTarget)
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = sample_count > 1 ? TRUE : FALSE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    D3D12_INPUT_LAYOUT_DESC input = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    RTFormatArrayProbe formats = {};
    formats.Formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.NumRenderTargets = 1;
    DXGI_FORMAT dsv_format = sample_count > 1 ? DXGI_FORMAT_D32_FLOAT
                                               : DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample = {sample_count, 0};
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ViewInstancingDescProbe view = {view_count, locations, 0x1u};

    stream.append(0, root);
    stream.append(1, vs_bytecode);
    stream.append(2, ps_bytecode);
    stream.append(8, blend);
    stream.append(10, rasterizer);
    stream.append(11, depth);
    stream.append(12, input);
    stream.append(14, topology);
    stream.append(15, formats);
    stream.append(16, dsv_format);
    stream.append(17, sample);
    stream.append(22, view);
    stream.append(20, flags);
    return stream;
}

static HRESULT create_view_pso(ID3D12Device2 *device,
                               ID3D12RootSignature *root, ID3DBlob *vs,
                               ID3DBlob *ps,
                               const ViewInstanceLocationProbe *locations,
                               UINT count, UINT sample_count,
                               ID3D12PipelineState **pso) {
    StreamBuilder stream =
        make_stream(root, vs, ps, locations, count, sample_count);
    D3D12_PIPELINE_STATE_STREAM_DESC desc = {stream.bytes.size(),
                                             stream.bytes.data()};
    return device->CreatePipelineState(&desc, IID_PPV_ARGS(pso));
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static HRESULT wait_for_queue(ID3D12Device *device, ID3D12CommandQueue *queue,
                              ID3D12GraphicsCommandList *list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence *fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event_handle = nullptr;
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

int main(int argc, char **argv) {
    if (argc > 2) {
        std::fprintf(stderr,
                     "usage: probe_view_instancing_breadth [1|4]\n");
        return 2;
    }
    const UINT sample_count = argc == 2
                                  ? static_cast<UINT>(std::strtoul(argv[1], nullptr, 10))
                                  : 1u;
    if (sample_count != 1 && sample_count != 4)
        return 2;
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<HRESULT(WINAPI *)(IUnknown *,
                                                       D3D_FEATURE_LEVEL,
                                                       REFIID, void **)>(
        d3d12, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            kDeviceIID,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3D12Device2 *device2 = nullptr;
    HRESULT device2_hr = SUCCEEDED(create_hr)
                             ? device->QueryInterface(
                                   kDevice2IID,
                                   reinterpret_cast<void **>(&device2))
                             : E_FAIL;
    ID3D12RootSignature *root = nullptr;
    HRESULT root_hr = SUCCEEDED(create_hr)
                          ? create_root_signature(device, &root)
                          : E_FAIL;
    const char *vs_source = R"HLSL(
struct VSOut { float4 position : SV_Position; };
VSOut main(uint id : SV_VertexID) {
  VSOut o;
  o.position = id == 0 ? float4(-1.0, -1.0, 0.0, 1.0) :
               (id == 1 ? float4(3.0, -1.0, 0.0, 1.0) :
                          float4(-1.0, 3.0, 0.0, 1.0));
  return o;
}
)HLSL";
    const char *pixel_sources[] = {
        "float4 main() : SV_Target0 { return float4(1,0,0,1); }",
        "float4 main() : SV_Target0 { return float4(0,1,0,1); }",
        "float4 main() : SV_Target0 { return float4(0,0,1,1); }",
        "float4 main() : SV_Target0 { return float4(1,1,0,1); }",
    };
    const Pixel expected[] = {{255, 0, 0, 255}, {0, 255, 0, 255},
                              {0, 0, 255, 255}, {255, 255, 0, 255}};
    ID3DBlob *vs = nullptr;
    HRESULT vs_hr = compile_shader(vs_source, "main", "vs_5_0", &vs);
    ID3DBlob *ps[4] = {};
    HRESULT ps_hr[4] = {};
    for (size_t i = 0; i < 4; ++i)
        ps_hr[i] = compile_shader(pixel_sources[i], "main", "ps_5_0",
                                   &ps[i]);

    ViewInstanceLocationProbe locations[4] = {{0, 0}, {1, 1}, {2, 2}, {3, 3}};
    ID3D12PipelineState *psos[4] = {};
    HRESULT pso_hr[4] = {};
    for (size_t i = 0; i < 4; ++i)
        pso_hr[i] = (device2 && root && vs && ps[i])
                        ? create_view_pso(device2, root, vs, ps[i], locations,
                                          4, sample_count, &psos[i])
                        : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12GraphicsCommandList1 *list1 = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *resolved = nullptr;
    ID3D12Resource *depth = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12DescriptorHeap *dsv_heap = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT list1_hr = E_FAIL;
    HRESULT target_hr = E_FAIL;
    HRESULT resolved_hr = sample_count == 1 ? S_OK : E_FAIL;
    HRESULT depth_hr = sample_count == 1 ? S_OK : E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT dsv_heap_hr = sample_count == 1 ? S_OK : E_FAIL;
    HRESULT execute_hr = E_FAIL;
    if (device && root && vs && psos[0] && psos[1] && psos[2] && psos[3]) {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(queue_hr))
            allocator_hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(allocator_hr))
            list_hr = device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                IID_PPV_ARGS(&list));
        if (SUCCEEDED(list_hr))
            list1_hr = list->QueryInterface(IID_PPV_ARGS(&list1));

        D3D12_RESOURCE_DESC target_desc = {};
        target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        target_desc.Width = 1;
        target_desc.Height = 1;
        target_desc.DepthOrArraySize = 4;
        target_desc.MipLevels = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = sample_count;
        target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        clear.Color[3] = 1.0f;
        D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        if (SUCCEEDED(list1_hr))
            target_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&target));
        if (SUCCEEDED(target_hr) && sample_count > 1) {
            D3D12_RESOURCE_DESC resolved_desc = target_desc;
            resolved_desc.SampleDesc.Count = 1;
            resolved_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &resolved_desc,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&resolved));
            D3D12_RESOURCE_DESC depth_desc = target_desc;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE depth_clear = {};
            depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
            depth_clear.DepthStencil.Depth = 1.0f;
            depth_hr = SUCCEEDED(resolved_hr)
                           ? device->CreateCommittedResource(
                                 &default_heap, D3D12_HEAP_FLAG_NONE,
                                 &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                 &depth_clear, IID_PPV_ARGS(&depth))
                           : E_FAIL;
        }
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = 4 * 256;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(target_hr) && SUCCEEDED(resolved_hr) &&
            SUCCEEDED(depth_hr))
            readback_hr = device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        if (SUCCEEDED(readback_hr))
            readback_hr = device->CreateDescriptorHeap(
                &heap_desc, IID_PPV_ARGS(&rtv_heap));
        if (SUCCEEDED(readback_hr) && sample_count > 1) {
            D3D12_DESCRIPTOR_HEAP_DESC dsv_desc = {};
            dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            dsv_desc.NumDescriptors = 1;
            dsv_heap_hr = device->CreateDescriptorHeap(
                &dsv_desc, IID_PPV_ARGS(&dsv_heap));
        }
        if (SUCCEEDED(readback_hr) && SUCCEEDED(dsv_heap_hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
            rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (sample_count > 1) {
                rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                rtv_desc.Texture2DMSArray.ArraySize = 4;
            } else {
                rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv_desc.Texture2DArray.ArraySize = 4;
            }
            device->CreateRenderTargetView(target, &rtv_desc, rtv);
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
            if (sample_count > 1) {
                dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
                dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
                dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
                dsv_desc.Texture2DMSArray.ArraySize = 4;
                device->CreateDepthStencilView(depth, &dsv_desc, dsv);
            }
            const FLOAT clear_color[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
            if (sample_count > 1)
                list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f,
                                            0, 0, nullptr);
            const D3D12_VIEWPORT viewports[4] = {
                {0, 0, 1, 1, 0, 1}, {0, 0, 1, 1, 0, 1},
                {0, 0, 1, 1, 0, 1}, {0, 0, 1, 1, 0, 1}};
            const D3D12_RECT scissors[4] = {{0, 0, 1, 1}, {0, 0, 1, 1},
                                            {0, 0, 1, 1}, {0, 0, 1, 1}};
            list->RSSetViewports(4, viewports);
            list->RSSetScissorRects(4, scissors);
            list->SetGraphicsRootSignature(root);
            list->OMSetRenderTargets(1, &rtv, FALSE,
                                     sample_count > 1 ? &dsv : nullptr);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT i = 0; i < 4; ++i) {
                list->SetPipelineState(psos[i]);
                list1->SetViewInstanceMask(1u << i);
                list->DrawInstanced(3, 1, 0, 0);
            }
            // A zero mask must not fall back to one uninstanced draw.  Use the
            // blue PSO so an accidental fallback would overwrite slice zero.
            list->SetPipelineState(psos[2]);
            list1->SetViewInstanceMask(0);
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = sample_count > 1
                                                ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                                : D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
            if (sample_count > 1) {
                for (UINT slice = 0; slice < 4; ++slice)
                    list->ResolveSubresource(resolved, slice, target, slice,
                                             DXGI_FORMAT_R8G8B8A8_UNORM);
                D3D12_RESOURCE_BARRIER resolved_barrier = {};
                resolved_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                resolved_barrier.Transition.pResource = resolved;
                resolved_barrier.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                resolved_barrier.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RESOLVE_DEST;
                resolved_barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &resolved_barrier);
            }
            // This 1x1 RGBA8 array has one 256-byte readback row per slice.
            // Keep the footprint local to each copy; GetCopyableFootprints
            // would otherwise write one structure per subresource.
            for (UINT slice = 0; slice < 4; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = sample_count > 1 ? resolved : target;
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = slice;
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = readback;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset = slice * 256;
                destination.PlacedFootprint.Footprint.Format =
                    DXGI_FORMAT_R8G8B8A8_UNORM;
                destination.PlacedFootprint.Footprint.Width = 1;
                destination.PlacedFootprint.Footprint.Height = 1;
                destination.PlacedFootprint.Footprint.Depth = 1;
                destination.PlacedFootprint.Footprint.RowPitch = 256;
                list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                        nullptr);
            }
            execute_hr = wait_for_queue(device, queue, list);
        }
    }

    Pixel actual[4] = {};
    HRESULT map_hr = E_FAIL;
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, 4 * 256};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            const auto *bytes = static_cast<const uint8_t *>(mapped);
            for (UINT i = 0; i < 4; ++i)
                std::memcpy(&actual[i], bytes + i * 256, sizeof(Pixel));
            readback->Unmap(0, nullptr);
        }
    }
    bool shader_exact = true;
    for (HRESULT hr : ps_hr)
        shader_exact = shader_exact && hr == S_OK;
    bool pso_exact = true;
    for (HRESULT hr : pso_hr)
        pso_exact = pso_exact && hr == S_OK;
    bool readback_exact = SUCCEEDED(map_hr);
    for (UINT i = 0; i < 4; ++i)
        readback_exact = readback_exact && std::memcmp(&actual[i], &expected[i],
                                                       sizeof(Pixel)) == 0;
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(device2_hr) &&
                      SUCCEEDED(root_hr) && SUCCEEDED(vs_hr) && shader_exact &&
                      pso_exact &&
                      SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) &&
                      SUCCEEDED(list_hr) && SUCCEEDED(list1_hr) &&
                      SUCCEEDED(target_hr) && SUCCEEDED(resolved_hr) &&
                      SUCCEEDED(depth_hr) && SUCCEEDED(dsv_heap_hr) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(execute_hr) &&
                      readback_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-view-instancing-breadth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"device2_hr\": \"%s\", \"root_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(device2_hr).c_str(),
                hr_hex(root_hr).c_str());
    std::printf("  \"vs_hr\": \"%s\", \"shader_exact\": %s, \"pso_exact\": %s,\n",
                hr_hex(vs_hr).c_str(), shader_exact ? "true" : "false",
                pso_exact ? "true" : "false");
    std::printf("  \"pso_hr\": [");
    for (size_t i = 0; i < 4; ++i) {
        if (i)
            std::printf(", ");
        std::printf("\"%s\"", hr_hex(pso_hr[i]).c_str());
    }
    std::printf("],\n  \"queue_hr\": \"%s\", \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n",
                hr_hex(queue_hr).c_str(), hr_hex(execute_hr).c_str(),
                hr_hex(map_hr).c_str());
    std::printf("  \"view_count\": 4, \"sample_count\": %u, \"depth_enabled\": %s, \"masks\": [1, 2, 4, 8, 0],\n",
                sample_count, sample_count > 1 ? "true" : "false");
    std::printf("  \"slices\": [\n");
    for (size_t i = 0; i < 4; ++i) {
        std::printf("    {\"slice\": %zu, \"rgba\": [%u, %u, %u, %u], \"expected\": [%u, %u, %u, %u], \"exact\": %s}%s\n",
                    i, actual[i].r, actual[i].g, actual[i].b, actual[i].a,
                    expected[i].r, expected[i].g, expected[i].b, expected[i].a,
                    std::memcmp(&actual[i], &expected[i], sizeof(Pixel)) == 0
                        ? "true" : "false",
                    i == 3 ? "" : ",");
    }
    std::printf("  ],\n  \"zero_mask_preserved\": %s,\n  \"pass\": %s,\n"
                "  \"provider\": \"draw_level_view_replay\"\n}\n",
                (std::memcmp(&actual[0], &expected[0], sizeof(Pixel)) == 0)
                    ? "true" : "false",
                pass ? "true" : "false");
    std::fflush(stdout);

    for (auto &blob : ps)
        safe_release(blob);
    safe_release(vs);
    safe_release(readback);
    safe_release(depth);
    safe_release(resolved);
    safe_release(target);
    safe_release(dsv_heap);
    safe_release(rtv_heap);
    safe_release(list1);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    for (auto &pso : psos)
        safe_release(pso);
    safe_release(root);
    safe_release(device2);
    safe_release(device);
    return pass ? 0 : 1;
}
