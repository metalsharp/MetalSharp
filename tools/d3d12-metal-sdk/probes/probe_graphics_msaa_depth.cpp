#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <iterator>
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
    T function = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
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
                          const std::vector<uint8_t> &vs,
                          const std::vector<uint8_t> &ps, UINT samples,
                          DXGI_FORMAT dsv_format, bool depth_enable,
                          bool stencil_enable, ID3D12PipelineState **pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = depth_enable ? TRUE : FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = stencil_enable ? TRUE : FALSE;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.DepthStencilState.FrontFace.StencilFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace.StencilFunc =
        D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = dsv_format;
    desc.SampleDesc.Count = samples;
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
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

struct CaseResult {
    const char *name;
    UINT sample_count;
    DXGI_FORMAT dsv_format;
    bool depth_should_pass;
    HRESULT target_hr = E_FAIL;
    HRESULT depth_hr = E_FAIL;
    HRESULT resolve_hr = E_FAIL;
    HRESULT pso_hr = E_FAIL;
    bool exact = false;
    uint8_t rgba[4] = {};
};

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: probe_graphics_msaa_depth <vs.dxil> <ps.dxil>\n");
        return 2;
    }
    const auto vs = read_binary_file(argv[1]);
    const auto ps = read_binary_file(argv[2]);
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
    const CaseResult definitions[] = {
        {"2x_depth_pass", 2, DXGI_FORMAT_D32_FLOAT, true},
        {"4x_depth_fail", 4, DXGI_FORMAT_D32_FLOAT, false},
        {"4x_stencil_pass", 4, DXGI_FORMAT_D24_UNORM_S8_UINT, true},
    };
    CaseResult cases[3] = {definitions[0], definitions[1], definitions[2]};
    ID3D12PipelineState *psos[3] = {};
    HRESULT pso_hr[3] = {E_FAIL, E_FAIL, E_FAIL};
    for (UINT i = 0; i < 3; ++i) {
        pso_hr[i] = (device && root && !vs.empty() && !ps.empty())
                        ? create_pso(device, root, vs, ps,
                                     cases[i].sample_count, cases[i].dsv_format,
                                     true, i == 2, &psos[i])
                        : E_FAIL;
        cases[i].pso_hr = pso_hr[i];
    }

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *targets[3] = {};
    ID3D12Resource *resolves[3] = {};
    ID3D12Resource *depths[3] = {};
    ID3D12Resource *readback = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12DescriptorHeap *dsv_heap = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT rtv_heap_hr = E_FAIL;
    HRESULT dsv_heap_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    if (device && root && psos[0] && psos[1] && psos[2]) {
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
        for (UINT i = 0; i < 3 && SUCCEEDED(list_hr); ++i) {
            D3D12_RESOURCE_DESC target_desc = {};
            target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            target_desc.Width = 1;
            target_desc.Height = 1;
            target_desc.DepthOrArraySize = 1;
            target_desc.MipLevels = 1;
            target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            target_desc.SampleDesc.Count = cases[i].sample_count;
            target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE color_clear = {};
            color_clear.Format = target_desc.Format;
            color_clear.Color[3] = 0.0f;
            cases[i].target_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &color_clear,
                IID_PPV_ARGS(&targets[i]));
            D3D12_RESOURCE_DESC depth_desc = target_desc;
            depth_desc.Format = cases[i].dsv_format;
            depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE depth_clear = {};
            depth_clear.Format = cases[i].dsv_format;
            depth_clear.DepthStencil.Depth = cases[i].depth_should_pass ? 1.0f : 0.0f;
            depth_clear.DepthStencil.Stencil = 0;
            cases[i].depth_hr = SUCCEEDED(cases[i].target_hr)
                                    ? device->CreateCommittedResource(
                                          &default_heap, D3D12_HEAP_FLAG_NONE,
                                          &depth_desc,
                                          D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                          &depth_clear, IID_PPV_ARGS(&depths[i]))
                                    : E_FAIL;
            D3D12_RESOURCE_DESC resolve_desc = target_desc;
            resolve_desc.SampleDesc.Count = 1;
            resolve_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            cases[i].resolve_hr = SUCCEEDED(cases[i].depth_hr)
                                       ? device->CreateCommittedResource(
                                             &default_heap, D3D12_HEAP_FLAG_NONE,
                                             &resolve_desc,
                                             D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                             nullptr, IID_PPV_ARGS(&resolves[i]))
                                       : E_FAIL;
        }
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = 3 * 256;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback_hr = SUCCEEDED(cases[2].resolve_hr)
                          ? device->CreateCommittedResource(
                                &readback_heap, D3D12_HEAP_FLAG_NONE,
                                &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                nullptr, IID_PPV_ARGS(&readback))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = 3;
        rtv_heap_hr = SUCCEEDED(readback_hr)
                          ? device->CreateDescriptorHeap(&rtv_desc,
                                                        IID_PPV_ARGS(&rtv_heap))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC dsv_desc = {};
        dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsv_desc.NumDescriptors = 3;
        dsv_heap_hr = SUCCEEDED(rtv_heap_hr)
                          ? device->CreateDescriptorHeap(&dsv_desc,
                                                        IID_PPV_ARGS(&dsv_heap))
                          : E_FAIL;
        if (SUCCEEDED(dsv_heap_hr)) {
            const UINT rtv_stride = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            const UINT dsv_stride = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            const FLOAT clear_color[4] = {0, 0, 0, 0};
            const D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
            const D3D12_RECT scissor = {0, 0, 1, 1};
            for (UINT i = 0; i < 3; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                    rtv_heap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += static_cast<SIZE_T>(i) * rtv_stride;
                D3D12_CPU_DESCRIPTOR_HANDLE dsv =
                    dsv_heap->GetCPUDescriptorHandleForHeapStart();
                dsv.ptr += static_cast<SIZE_T>(i) * dsv_stride;
                device->CreateRenderTargetView(targets[i], nullptr, rtv);
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view = {};
                dsv_view.Format = cases[i].dsv_format;
                dsv_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
                device->CreateDepthStencilView(depths[i], &dsv_view, dsv);
                list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
                list->ClearDepthStencilView(
                    dsv, static_cast<D3D12_CLEAR_FLAGS>(
                             D3D12_CLEAR_FLAG_DEPTH |
                             (i == 2 ? D3D12_CLEAR_FLAG_STENCIL : 0)),
                    cases[i].depth_should_pass ? 1.0f : 0.0f, 0, 0, nullptr);
                list->SetPipelineState(psos[i]);
                list->SetGraphicsRootSignature(root);
                list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(3, 1, 0, 0);
                D3D12_RESOURCE_BARRIER source_barrier = {};
                source_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                source_barrier.Transition.pResource = targets[i];
                source_barrier.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                source_barrier.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RENDER_TARGET;
                source_barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
                list->ResourceBarrier(1, &source_barrier);
                list->ResolveSubresource(resolves[i], 0, targets[i], 0,
                                         DXGI_FORMAT_R8G8B8A8_UNORM);
                D3D12_RESOURCE_BARRIER resolve_barrier = {};
                resolve_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                resolve_barrier.Transition.pResource = resolves[i];
                resolve_barrier.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                resolve_barrier.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RESOLVE_DEST;
                resolve_barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &resolve_barrier);
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = resolves[i];
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

    if (SUCCEEDED(execute_hr) && readback) {
        uint8_t bytes[3 * 256] = {};
        void *mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(bytes)};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            std::memcpy(bytes, mapped, sizeof(bytes));
            readback->Unmap(0, nullptr);
            for (UINT i = 0; i < 3; ++i) {
                std::memcpy(cases[i].rgba, bytes + i * 256,
                            sizeof(cases[i].rgba));
                const bool red = cases[i].rgba[0] == 255 &&
                                 cases[i].rgba[1] == 0 && cases[i].rgba[2] == 0 &&
                                 cases[i].rgba[3] == 255;
                const bool black = cases[i].rgba[0] == 0 &&
                                   cases[i].rgba[1] == 0 && cases[i].rgba[2] == 0 &&
                                   cases[i].rgba[3] == 0;
                cases[i].exact = cases[i].depth_should_pass ? red : black;
            }
        }
    }
    bool cases_exact = SUCCEEDED(map_hr);
    for (const auto &result : cases)
        cases_exact = cases_exact && result.exact;
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(pso_hr[0]) && SUCCEEDED(pso_hr[1]) &&
                      SUCCEEDED(pso_hr[2]) && SUCCEEDED(queue_hr) &&
                      SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(rtv_heap_hr) &&
                      SUCCEEDED(dsv_heap_hr) && SUCCEEDED(execute_hr) &&
                      cases_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-graphics-msaa-depth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str());
    std::printf("  \"cases\": [\n");
    for (UINT i = 0; i < 3; ++i) {
        const auto &result = cases[i];
        std::printf("    {\"name\": \"%s\", \"sample_count\": %u, \"pso_hr\": \"%s\", \"target_hr\": \"%s\", \"depth_hr\": \"%s\", \"resolve_hr\": \"%s\", \"rgba\": [%u, %u, %u, %u], \"depth_should_pass\": %s, \"exact\": %s}%s\n",
                    result.name, result.sample_count,
                    hr_hex(result.pso_hr).c_str(), hr_hex(result.target_hr).c_str(),
                    hr_hex(result.depth_hr).c_str(), hr_hex(result.resolve_hr).c_str(),
                    result.rgba[0], result.rgba[1], result.rgba[2], result.rgba[3],
                    result.depth_should_pass ? "true" : "false",
                    result.exact ? "true" : "false", i == 2 ? "" : ",");
    }
    std::printf("  ],\n  \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n"
                "  \"pass\": %s,\n  \"provider\": \"native_msaa_depth_stencil_output_merger\"\n}\n",
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str(),
                pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(readback);
    for (auto &resolve : resolves)
        safe_release(resolve);
    for (auto &depth : depths)
        safe_release(depth);
    for (auto &target : targets)
        safe_release(target);
    safe_release(dsv_heap);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    for (auto &pso : psos)
        safe_release(pso);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
