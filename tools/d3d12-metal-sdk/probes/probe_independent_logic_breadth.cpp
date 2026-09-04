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
    HRESULT hr = compile(source, std::strlen(source), "logic8.hlsl", nullptr,
                         nullptr, entry, target, 0, 0, blob, &errors);
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
                     "usage: probe_independent_logic_breadth [1|2|4]\n");
        return 2;
    }
    const UINT sample_count = argc == 2
                                  ? static_cast<UINT>(std::strtoul(argv[1], nullptr, 10))
                                  : 1u;
    if (sample_count != 1 && sample_count != 2 && sample_count != 4)
        return 2;
    const char *hlsl = R"HLSL(
struct VSOut { float4 position : SV_Position; };
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  o.position = id == 0 ? float4(-3.0, -3.0, 0.0, 1.0) :
               (id == 1 ? float4(9.0, -3.0, 0.0, 1.0) :
                          float4(-3.0, 9.0, 0.0, 1.0));
  return o;
}
struct PSOut {
  float4 c0 : SV_Target0; float4 c1 : SV_Target1;
  float4 c2 : SV_Target2; float4 c3 : SV_Target3;
  float4 c4 : SV_Target4; float4 c5 : SV_Target5;
  float4 c6 : SV_Target6; float4 c7 : SV_Target7;
};
PSOut ps_main(VSOut input) {
  PSOut o;
  o.c0 = float4(15.0 / 255.0, 15.0 / 255.0, 15.0 / 255.0, 1.0);
  o.c1 = float4(240.0 / 255.0, 240.0 / 255.0, 240.0 / 255.0, 1.0);
  o.c2 = float4(51.0 / 255.0, 51.0 / 255.0, 51.0 / 255.0, 1.0);
  o.c3 = float4(204.0 / 255.0, 204.0 / 255.0, 204.0 / 255.0, 1.0);
  o.c4 = float4(85.0 / 255.0, 85.0 / 255.0, 85.0 / 255.0, 1.0);
  o.c5 = float4(15.0 / 255.0, 15.0 / 255.0, 15.0 / 255.0, 1.0);
  o.c6 = float4(1.0, 1.0, 1.0, 1.0);
  o.c7 = float4(0.0, 0.0, 0.0, 1.0);
  return o;
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
    ID3DBlob *vs = nullptr;
    ID3DBlob *ps = nullptr;
    HRESULT vs_hr = compile_shader(hlsl, "vs_main", "vs_5_0", &vs);
    HRESULT ps_hr = compile_shader(hlsl, "ps_main", "ps_5_0", &ps);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    if (vs)
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    if (ps)
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.IndependentBlendEnable = TRUE;
    for (UINT i = 0; i < 8; ++i) {
        auto &blend = desc.BlendState.RenderTarget[i];
        blend.LogicOpEnable = TRUE;
        blend.LogicOp = (i & 1) ? D3D12_LOGIC_OP_AND : D3D12_LOGIC_OP_XOR;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 8;
    desc.SampleDesc.Count = sample_count;
    ID3D12PipelineState *pso = nullptr;
    HRESULT pso_hr = (device && root && vs && ps)
                         ? device->CreateGraphicsPipelineState(&desc,
                                                               IID_PPV_ARGS(&pso))
                         : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *targets[8] = {};
    ID3D12Resource *resolves[8] = {};
    ID3D12Resource *readback = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT rtv_heap_hr = E_FAIL;
    HRESULT target_hr[8] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL,
                            E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    HRESULT resolve_hr[8] = {S_OK, S_OK, S_OK, S_OK,
                             S_OK, S_OK, S_OK, S_OK};
    HRESULT readback_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    if (device && root && pso) {
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
        D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC target_desc = {};
        target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        target_desc.Width = 1;
        target_desc.Height = 1;
        target_desc.DepthOrArraySize = 1;
        target_desc.MipLevels = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = sample_count;
        target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = target_desc.Format;
        clear.Color[0] = 170.0f / 255.0f;
        clear.Color[1] = 170.0f / 255.0f;
        clear.Color[2] = 170.0f / 255.0f;
        clear.Color[3] = 1.0f;
        for (UINT i = 0; i < 8 && SUCCEEDED(list_hr); ++i) {
            target_hr[i] = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&targets[i]));
            if (sample_count > 1) {
                D3D12_RESOURCE_DESC resolve_desc = target_desc;
                resolve_desc.SampleDesc.Count = 1;
                resolve_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
                resolve_hr[i] = SUCCEEDED(target_hr[i])
                                    ? device->CreateCommittedResource(
                                          &default_heap, D3D12_HEAP_FLAG_NONE,
                                          &resolve_desc,
                                          D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                          nullptr, IID_PPV_ARGS(&resolves[i]))
                                    : E_FAIL;
            }
        }
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = 8 * 256;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback_hr = SUCCEEDED(target_hr[7]) && SUCCEEDED(resolve_hr[7])
                          ? device->CreateCommittedResource(
                                &readback_heap, D3D12_HEAP_FLAG_NONE,
                                &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                nullptr, IID_PPV_ARGS(&readback))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 8;
        rtv_heap_hr = SUCCEEDED(readback_hr)
                          ? device->CreateDescriptorHeap(&heap_desc,
                                                        IID_PPV_ARGS(&rtv_heap))
                          : E_FAIL;
        if (SUCCEEDED(rtv_heap_hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[8] = {};
            const UINT stride = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const FLOAT clear_color[4] = {170.0f / 255.0f,
                                          170.0f / 255.0f,
                                          170.0f / 255.0f, 1.0f};
            for (UINT i = 0; i < 8; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvs;
                handle.ptr += static_cast<SIZE_T>(i) * stride;
                rtv_handles[i] = handle;
                device->CreateRenderTargetView(targets[i], nullptr, handle);
                list->ClearRenderTargetView(handle, clear_color, 0, nullptr);
            }
            const D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
            const D3D12_RECT scissor = {0, 0, 1, 1};
            list->SetGraphicsRootSignature(root);
            list->SetPipelineState(pso);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->OMSetRenderTargets(8, rtv_handles, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER barriers[8] = {};
            for (UINT i = 0; i < 8; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.pResource = targets[i];
                barriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RENDER_TARGET;
                barriers[i].Transition.StateAfter = sample_count > 1
                                                        ? D3D12_RESOURCE_STATE_RESOLVE_SOURCE
                                                        : D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            list->ResourceBarrier(8, barriers);
            if (sample_count > 1) {
                for (UINT i = 0; i < 8; ++i)
                    list->ResolveSubresource(resolves[i], 0, targets[i], 0,
                                             DXGI_FORMAT_R8G8B8A8_UNORM);
                D3D12_RESOURCE_BARRIER resolve_barriers[8] = {};
                for (UINT i = 0; i < 8; ++i) {
                    resolve_barriers[i].Type =
                        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    resolve_barriers[i].Transition.pResource = resolves[i];
                    resolve_barriers[i].Transition.Subresource =
                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    resolve_barriers[i].Transition.StateBefore =
                        D3D12_RESOURCE_STATE_RESOLVE_DEST;
                    resolve_barriers[i].Transition.StateAfter =
                        D3D12_RESOURCE_STATE_COPY_SOURCE;
                }
                list->ResourceBarrier(8, resolve_barriers);
            }
            for (UINT i = 0; i < 8; ++i) {
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = sample_count > 1 ? resolves[i] : targets[i];
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = readback;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset = i * 256;
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

    const uint8_t sources[8] = {15, 240, 51, 204, 85, 15, 255, 0};
    uint8_t actual[8][4] = {};
    uint8_t expected[8][4] = {};
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, 8 * 256};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            const auto *bytes = static_cast<const uint8_t *>(mapped);
            for (UINT i = 0; i < 8; ++i)
                std::memcpy(actual[i], bytes + i * 256, 4);
            readback->Unmap(0, nullptr);
        }
    }
    bool values_exact = SUCCEEDED(map_hr);
    for (UINT i = 0; i < 8; ++i) {
        const uint8_t rgb = (i & 1) ? (uint8_t)(170u & sources[i])
                                    : (uint8_t)(170u ^ sources[i]);
        const uint8_t alpha = (i & 1) ? 255u : 0u;
        expected[i][0] = expected[i][1] = expected[i][2] = rgb;
        expected[i][3] = alpha;
        values_exact = values_exact &&
                       std::memcmp(actual[i], expected[i], 4) == 0;
    }
    bool resources_exact = true;
    for (UINT i = 0; i < 8; ++i)
        resources_exact = resources_exact && target_hr[i] == S_OK &&
                          resolve_hr[i] == S_OK;
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(vs_hr) && SUCCEEDED(ps_hr) && SUCCEEDED(pso_hr) &&
                      SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) &&
                      SUCCEEDED(list_hr) && resources_exact &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(rtv_heap_hr) &&
                      SUCCEEDED(execute_hr) && values_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-independent-logic-breadth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\", \"vs_hr\": \"%s\", \"ps_hr\": \"%s\", \"pso_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str(),
                hr_hex(vs_hr).c_str(), hr_hex(ps_hr).c_str(),
                hr_hex(pso_hr).c_str());
    std::printf("  \"execute_hr\": \"%s\", \"map_hr\": \"%s\", \"target_count\": 8, \"sample_count\": %u,\n",
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str(),
                sample_count);
    std::printf("  \"targets\": [\n");
    for (UINT i = 0; i < 8; ++i) {
        std::printf("    {\"index\": %u, \"operation\": \"%s\", \"rgba\": [%u, %u, %u, %u], \"expected\": [%u, %u, %u, %u], \"exact\": %s}%s\n",
                    i, (i & 1) ? "and" : "xor", actual[i][0], actual[i][1],
                    actual[i][2], actual[i][3], expected[i][0], expected[i][1],
                    expected[i][2], expected[i][3],
                    std::memcmp(actual[i], expected[i], 4) == 0 ? "true"
                                                                : "false",
                    i == 7 ? "" : ",");
    }
    std::printf("  ],\n  \"values_exact\": %s, \"pass\": %s,\n"
                "  \"provider\": \"per_target_logic_pipeline_replay\"\n}\n",
                values_exact ? "true" : "false", pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(readback);
    for (auto &resolve : resolves)
        safe_release(resolve);
    for (auto &target : targets)
        safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(ps);
    safe_release(vs);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
