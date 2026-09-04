#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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
    HRESULT hr = compile(source, std::strlen(source), "sample_positions.hlsl",
                         nullptr, nullptr, entry, target, 0, 0, blob, &errors);
    if (FAILED(hr) && errors)
        std::fprintf(stderr, "%s compile error: %.*s\n", entry,
                     static_cast<int>(errors->GetBufferSize()),
                     static_cast<const char *>(errors->GetBufferPointer()));
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
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                           &errors);
    safe_release(errors);
    if (SUCCEEDED(hr) && blob)
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

static HRESULT create_pso(ID3D12Device *device, ID3D12RootSignature *root,
                          ID3DBlob *vs, ID3DBlob *ps,
                          ID3D12PipelineState **pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 4;
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
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

static bool pixel_is(const uint8_t *pixel, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t a) {
    return pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == a;
}

int main() {
    const char *hlsl = R"HLSL(
struct VSOut { float4 position : SV_Position; };
VSOut vs_pattern(uint id : SV_VertexID) {
  VSOut o;
  float2 p = id == 0 ? float2(1.0, 1.0) :
             (id == 1 ? float2(1.0, 0.5) : float2(0.75, 1.0));
  o.position = float4(p, 0.0, 1.0);
  return o;
}
VSOut vs_full(uint id : SV_VertexID) {
  VSOut o;
  float2 p = id == 0 ? float2(-3.0, -3.0) :
             (id == 1 ? float2(3.0, -3.0) :
              (id == 2 ? float2(3.0, 3.0) :
               (id == 3 ? float2(-3.0, -3.0) :
                (id == 4 ? float2(3.0, 3.0) : float2(-3.0, 3.0)))));
  o.position = float4(p, 0.0, 1.0);
  return o;
}
float4 ps_main(VSOut input) : SV_Target0 {
  return float4(1.0, 0.0, 0.0, 1.0);
}
)HLSL";
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
    ID3D12RootSignature *root = nullptr;
    HRESULT root_hr = SUCCEEDED(create_hr)
                          ? create_root_signature(device, &root)
                          : E_FAIL;
    ID3DBlob *vs_pattern = nullptr;
    ID3DBlob *vs_full = nullptr;
    ID3DBlob *ps = nullptr;
    HRESULT vs_pattern_hr = compile_shader(hlsl, "vs_pattern", "vs_5_0",
                                           &vs_pattern);
    HRESULT vs_full_hr = compile_shader(hlsl, "vs_full", "vs_5_0", &vs_full);
    HRESULT ps_hr = compile_shader(hlsl, "ps_main", "ps_5_0", &ps);
    ID3D12PipelineState *pattern_pso = nullptr;
    ID3D12PipelineState *full_pso = nullptr;
    HRESULT pattern_pso_hr = (device && root && vs_pattern && ps)
                                 ? create_pso(device, root, vs_pattern, ps,
                                              &pattern_pso)
                                 : E_FAIL;
    HRESULT full_pso_hr = (device && root && vs_full && ps)
                              ? create_pso(device, root, vs_full, ps, &full_pso)
                              : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12GraphicsCommandList1 *list1 = nullptr;
    ID3D12Resource *pattern_target = nullptr;
    ID3D12Resource *pattern_resolve = nullptr;
    ID3D12Resource *default_target = nullptr;
    ID3D12Resource *default_resolve = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT list1_hr = E_FAIL;
    HRESULT pattern_target_hr = E_FAIL;
    HRESULT pattern_resolve_hr = E_FAIL;
    HRESULT default_target_hr = E_FAIL;
    HRESULT default_resolve_hr = E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT rtv_heap_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    bool pattern_recorded = false;
    bool reset_recorded = false;
    if (device && root && pattern_pso && full_pso) {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
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

        D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC source_desc = {};
        source_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source_desc.Width = 2;
        source_desc.Height = 2;
        source_desc.DepthOrArraySize = 1;
        source_desc.MipLevels = 1;
        source_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source_desc.SampleDesc.Count = 4;
        source_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = source_desc.Format;
        clear.Color[3] = 1.0f;
        if (SUCCEEDED(list1_hr))
            pattern_target_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&pattern_target));
        if (SUCCEEDED(pattern_target_hr))
            default_target_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&default_target));
        D3D12_RESOURCE_DESC resolve_desc = source_desc;
        resolve_desc.SampleDesc.Count = 1;
        if (SUCCEEDED(default_target_hr))
            pattern_resolve_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &resolve_desc,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&pattern_resolve));
        if (SUCCEEDED(pattern_resolve_hr))
            default_resolve_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &resolve_desc,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&default_resolve));
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = 2 * 512;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(default_resolve_hr))
            readback_hr = device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 2;
        if (SUCCEEDED(readback_hr))
            rtv_heap_hr = device->CreateDescriptorHeap(
                &heap_desc, IID_PPV_ARGS(&rtv_heap));
        if (SUCCEEDED(rtv_heap_hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE pattern_rtv =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE default_rtv = pattern_rtv;
            default_rtv.ptr += device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            device->CreateRenderTargetView(pattern_target, nullptr, pattern_rtv);
            device->CreateRenderTargetView(default_target, nullptr, default_rtv);
            const FLOAT clear_color[4] = {0, 0, 0, 1};
            const D3D12_VIEWPORT viewport = {0, 0, 2, 2, 0, 1};
            const D3D12_RECT scissor = {0, 0, 2, 2};
            list->ClearRenderTargetView(pattern_rtv, clear_color, 0, nullptr);
            list->SetPipelineState(pattern_pso);
            list->SetGraphicsRootSignature(root);
            list->OMSetRenderTargets(1, &pattern_rtv, FALSE, nullptr);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_SAMPLE_POSITION positions[16] = {
                {-7, -7}, {-6, -7}, {-7, -6}, {-6, -6},
                {7, -7},  {6, -7},  {7, -6},  {6, -6},
                {-7, 7},  {-6, 7},  {-7, 6},  {-6, 6},
                {7, 7},   {6, 7},   {7, 6},   {6, 6}};
            if (list1) {
                list1->SetSamplePositions(4, 4,
                                          const_cast<D3D12_SAMPLE_POSITION *>(positions));
                pattern_recorded = true;
            }
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER pattern_barrier = {};
            pattern_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pattern_barrier.Transition.pResource = pattern_target;
            pattern_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pattern_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            pattern_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            list->ResourceBarrier(1, &pattern_barrier);
            list->ResolveSubresource(pattern_resolve, 0, pattern_target, 0,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_RESOURCE_BARRIER pattern_resolve_barrier = {};
            pattern_resolve_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pattern_resolve_barrier.Transition.pResource = pattern_resolve;
            pattern_resolve_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            pattern_resolve_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RESOLVE_DEST;
            pattern_resolve_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &pattern_resolve_barrier);
            D3D12_TEXTURE_COPY_LOCATION pattern_source = {};
            pattern_source.pResource = pattern_resolve;
            pattern_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION pattern_destination = {};
            pattern_destination.pResource = readback;
            pattern_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            pattern_destination.PlacedFootprint.Footprint.Format =
                DXGI_FORMAT_R8G8B8A8_UNORM;
            pattern_destination.PlacedFootprint.Footprint.Width = 2;
            pattern_destination.PlacedFootprint.Footprint.Height = 2;
            pattern_destination.PlacedFootprint.Footprint.Depth = 1;
            pattern_destination.PlacedFootprint.Footprint.RowPitch = 256;
            list->CopyTextureRegion(&pattern_destination, 0, 0, 0,
                                    &pattern_source, nullptr);

            list1->SetSamplePositions(0, 0, nullptr);
            reset_recorded = true;
            list->ClearRenderTargetView(default_rtv, clear_color, 0, nullptr);
            list->SetPipelineState(full_pso);
            list->OMSetRenderTargets(1, &default_rtv, FALSE, nullptr);
            list->DrawInstanced(6, 1, 0, 0);
            D3D12_RESOURCE_BARRIER default_barrier = {};
            default_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            default_barrier.Transition.pResource = default_target;
            default_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            default_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            default_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            list->ResourceBarrier(1, &default_barrier);
            list->ResolveSubresource(default_resolve, 0, default_target, 0,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_RESOURCE_BARRIER default_resolve_barrier = {};
            default_resolve_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            default_resolve_barrier.Transition.pResource = default_resolve;
            default_resolve_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            default_resolve_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RESOLVE_DEST;
            default_resolve_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &default_resolve_barrier);
            D3D12_TEXTURE_COPY_LOCATION default_source = {};
            default_source.pResource = default_resolve;
            default_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION default_destination = {};
            default_destination.pResource = readback;
            default_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            default_destination.PlacedFootprint.Offset = 512;
            default_destination.PlacedFootprint.Footprint.Format =
                DXGI_FORMAT_R8G8B8A8_UNORM;
            default_destination.PlacedFootprint.Footprint.Width = 2;
            default_destination.PlacedFootprint.Footprint.Height = 2;
            default_destination.PlacedFootprint.Footprint.Depth = 1;
            default_destination.PlacedFootprint.Footprint.RowPitch = 256;
            list->CopyTextureRegion(&default_destination, 0, 0, 0,
                                    &default_source, nullptr);
            execute_hr = wait_for_queue(device, queue, list);
        }
    }

    uint8_t pixels[2 * 512] = {};
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(pixels)};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            std::memcpy(pixels, mapped, sizeof(pixels));
            readback->Unmap(0, nullptr);
        }
    }
    const bool pattern_exact =
        pixel_is(pixels + 0, 0, 0, 0, 255) &&
        pixel_is(pixels + 4, 255, 0, 0, 255) &&
        pixel_is(pixels + 256, 0, 0, 0, 255) &&
        pixel_is(pixels + 260, 0, 0, 0, 255);
    bool default_exact = true;
    for (UINT y = 0; y < 2; ++y)
        for (UINT x = 0; x < 2; ++x)
            default_exact = default_exact &&
                            pixel_is(pixels + 512 + y * 256 + x * 4, 255, 0,
                                     0, 255);
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(vs_pattern_hr) && SUCCEEDED(vs_full_hr) &&
                      SUCCEEDED(ps_hr) && SUCCEEDED(pattern_pso_hr) &&
                      SUCCEEDED(full_pso_hr) && SUCCEEDED(queue_hr) &&
                      SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                      SUCCEEDED(list1_hr) && SUCCEEDED(pattern_target_hr) &&
                      SUCCEEDED(pattern_resolve_hr) &&
                      SUCCEEDED(default_target_hr) &&
                      SUCCEEDED(default_resolve_hr) && SUCCEEDED(readback_hr) &&
                      SUCCEEDED(rtv_heap_hr) && SUCCEEDED(execute_hr) &&
                      SUCCEEDED(map_hr) && pattern_recorded && reset_recorded &&
                      pattern_exact && default_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-sample-positions-breadth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\", \"vs_pattern_hr\": \"%s\", \"vs_full_hr\": \"%s\", \"ps_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str(),
                hr_hex(vs_pattern_hr).c_str(), hr_hex(vs_full_hr).c_str(),
                hr_hex(ps_hr).c_str());
    std::printf("  \"pso_hr\": [\"%s\", \"%s\"], \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n",
                hr_hex(pattern_pso_hr).c_str(), hr_hex(full_pso_hr).c_str(),
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str());
    std::printf("  \"pattern\": {\"sample_count\": 4, \"pixel_count\": 4, \"recorded\": %s, \"exact\": %s},\n",
                pattern_recorded ? "true" : "false",
                pattern_exact ? "true" : "false");
    std::printf("  \"reset\": {\"recorded\": %s, \"default_quad_exact\": %s},\n",
                reset_recorded ? "true" : "false",
                default_exact ? "true" : "false");
    std::printf("  \"pass\": %s,\n  \"provider\": \"command_replay_sample_positions\"\n}\n",
                pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(readback);
    safe_release(default_resolve);
    safe_release(default_target);
    safe_release(pattern_resolve);
    safe_release(pattern_target);
    safe_release(rtv_heap);
    safe_release(list1);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(full_pso);
    safe_release(pattern_pso);
    safe_release(ps);
    safe_release(vs_full);
    safe_release(vs_pattern);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
