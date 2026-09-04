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

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

static D3D12_RESOURCE_DESC texture_desc(UINT samples) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.SampleDesc.Count = samples;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return desc;
}

static std::vector<uint8_t> read_binary_file(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
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

struct SampleResult {
    UINT samples = 0;
    UINT sample_mask = 0;
    float values[4] = {};
    float expected[4] = {};
    bool exact = false;
};

static bool exact_float4(const float *actual, const float *expected) {
    return std::memcmp(actual, expected, sizeof(float) * 4) == 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: probe_graphics_msaa_breadth <vs.dxil> <ps.dxil> <alpha-ps.dxil>\n");
        return 2;
    }
    const auto vs_bytes = read_binary_file(argv[1]);
    const auto ps_bytes = read_binary_file(argv[2]);
    const auto alpha_ps_bytes = read_binary_file(argv[3]);
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
    HRESULT vs_hr = vs_bytes.empty() ? E_FAIL : S_OK;
    HRESULT ps_hr = ps_bytes.empty() ? E_FAIL : S_OK;

    ID3D12PipelineState *pso2 = nullptr;
    ID3D12PipelineState *pso4 = nullptr;
    ID3D12PipelineState *pso4_mask = nullptr;
    ID3D12PipelineState *pso4_alpha = nullptr;
    D3D12_SHADER_BYTECODE vs_bytecode = {vs_bytes.data(), vs_bytes.size()};
    D3D12_SHADER_BYTECODE ps_bytecode = {ps_bytes.data(), ps_bytes.size()};
    auto create_pso_from_bytes = [&](UINT samples, UINT sample_mask,
                                     D3D12_SHADER_BYTECODE pixel,
                                     bool alpha_to_coverage,
                                     ID3D12PipelineState **pso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = vs_bytecode;
        desc.PS = pixel;
        desc.SampleMask = sample_mask;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.BlendState.AlphaToCoverageEnable = alpha_to_coverage;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = samples;
        return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
    };
    HRESULT pso2_hr = (device && root && !vs_bytes.empty() && !ps_bytes.empty())
                          ? create_pso_from_bytes(2, UINT_MAX, ps_bytecode,
                                                  false, &pso2)
                          : E_FAIL;
    HRESULT pso4_hr = (device && root && !vs_bytes.empty() && !ps_bytes.empty())
                          ? create_pso_from_bytes(4, UINT_MAX, ps_bytecode,
                                                  false, &pso4)
                          : E_FAIL;
    HRESULT pso4_mask_hr = (device && root && !vs_bytes.empty() && !ps_bytes.empty())
                               ? create_pso_from_bytes(4, 0x5u, ps_bytecode,
                                                       false, &pso4_mask)
                               : E_FAIL;
    D3D12_SHADER_BYTECODE alpha_ps_bytecode = {alpha_ps_bytes.data(),
                                                alpha_ps_bytes.size()};
    HRESULT pso4_alpha_hr =
        (device && root && !vs_bytes.empty() && !alpha_ps_bytes.empty())
            ? create_pso_from_bytes(4, UINT_MAX, alpha_ps_bytecode, true,
                                    &pso4_alpha)
            : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *targets[4] = {};
    ID3D12Resource *resolves[4] = {};
    ID3D12Resource *readback = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT target_hr[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    HRESULT resolve_hr[4] = {E_FAIL, E_FAIL, E_FAIL, E_FAIL};
    HRESULT readback_hr = E_FAIL;
    HRESULT rtv_heap_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    if (device && root && pso2 && pso4 && pso4_mask && pso4_alpha) {
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
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        for (UINT i = 0; i < 4 && SUCCEEDED(list_hr); ++i) {
            const UINT samples = i == 0 ? 2 : 4;
            D3D12_RESOURCE_DESC source_desc = texture_desc(samples);
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = source_desc.Format;
            clear.Color[3] = 1.0f;
            target_hr[i] = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&targets[i]));
            D3D12_RESOURCE_DESC destination_desc = texture_desc(1);
            destination_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            resolve_hr[i] = SUCCEEDED(target_hr[i])
                                ? device->CreateCommittedResource(
                                      &default_heap, D3D12_HEAP_FLAG_NONE,
                                      &destination_desc,
                                      D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                      IID_PPV_ARGS(&resolves[i]))
                                : E_FAIL;
        }
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = 4 * 256;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback_hr = SUCCEEDED(resolve_hr[3])
                          ? device->CreateCommittedResource(
                                &readback_heap, D3D12_HEAP_FLAG_NONE,
                                &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                nullptr, IID_PPV_ARGS(&readback))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 4;
        rtv_heap_hr = SUCCEEDED(readback_hr)
                          ? device->CreateDescriptorHeap(&heap_desc,
                                                        IID_PPV_ARGS(&rtv_heap))
                          : E_FAIL;
        if (SUCCEEDED(rtv_heap_hr)) {
            const FLOAT clear_color[4] = {0, 0, 0, 1};
            const D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
            const D3D12_RECT scissor = {0, 0, 1, 1};
            const UINT descriptor_stride = device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            for (UINT i = 0; i < 4; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                    rtv_heap->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += static_cast<SIZE_T>(i) * descriptor_stride;
                device->CreateRenderTargetView(targets[i], nullptr, rtv);
                const FLOAT alpha_clear[4] = {0, 0, 0, 0};
                list->ClearRenderTargetView(
                    rtv, i == 3 ? alpha_clear : clear_color, 0, nullptr);
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);
                list->SetGraphicsRootSignature(root);
                list->SetPipelineState(i == 0 ? pso2 :
                                       (i == 1 ? pso4 :
                                        (i == 2 ? pso4_mask : pso4_alpha)));
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawInstanced(6, 1, 0, 0);
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
                                         DXGI_FORMAT_R32G32B32A32_FLOAT);
                D3D12_RESOURCE_BARRIER destination_barrier = {};
                destination_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                destination_barrier.Transition.pResource = resolves[i];
                destination_barrier.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                destination_barrier.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_RESOLVE_DEST;
                destination_barrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &destination_barrier);
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = resolves[i];
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = readback;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint.Offset = i * 256;
                destination.PlacedFootprint.Footprint.Format =
                    DXGI_FORMAT_R32G32B32A32_FLOAT;
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

    float actual[4][4] = {};
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, 4 * 256};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            const auto *bytes = static_cast<const uint8_t *>(mapped);
            for (UINT i = 0; i < 4; ++i)
                std::memcpy(actual[i], bytes + i * 256, sizeof(actual[i]));
            readback->Unmap(0, nullptr);
        }
    }
    SampleResult results[4] = {};
    results[0].samples = 2;
    results[0].sample_mask = UINT_MAX;
    results[0].expected[0] = 1.5f;
    results[0].expected[1] = 0.0f;
    results[0].expected[2] = 0.5f;
    results[0].expected[3] = 1.0f;
    results[1].samples = 4;
    results[1].sample_mask = UINT_MAX;
    results[1].expected[0] = 2.5f;
    results[1].expected[1] = 0.0f;
    results[1].expected[2] = 1.5f;
    results[1].expected[3] = 1.0f;
    results[2].samples = 4;
    results[2].sample_mask = 5;
    results[2].expected[0] = 1.0f;
    results[2].expected[1] = 0.0f;
    results[2].expected[2] = 0.5f;
    results[2].expected[3] = 1.0f;
    results[3].samples = 4;
    results[3].sample_mask = UINT_MAX;
    results[3].expected[0] = 0.0f;
    results[3].expected[1] = 0.0f;
    results[3].expected[2] = 0.0f;
    results[3].expected[3] = 0.0f;
    bool sample_exact = SUCCEEDED(map_hr);
    for (UINT i = 0; i < 4; ++i) {
        std::memcpy(results[i].values, actual[i], sizeof(actual[i]));
        results[i].exact = exact_float4(actual[i], results[i].expected);
        sample_exact = sample_exact && results[i].exact;
    }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality[3] = {};
    HRESULT quality_hr[3] = {E_FAIL, E_FAIL, E_FAIL};
    bool quality_exact = false;
    if (device) {
        const UINT counts[3] = {1, 2, 4};
        for (UINT i = 0; i < 3; ++i) {
            quality[i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            quality[i].SampleCount = counts[i];
            quality_hr[i] = device->CheckFeatureSupport(
                D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &quality[i],
                sizeof(quality[i]));
        }
        quality_exact = quality_hr[0] == S_OK && quality_hr[1] == S_OK &&
                        quality_hr[2] == S_OK &&
                        quality[0].NumQualityLevels == 1 &&
                        quality[1].NumQualityLevels == 1 &&
                        quality[2].NumQualityLevels == 1;
    }
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(vs_hr) && SUCCEEDED(ps_hr) &&
                      SUCCEEDED(pso2_hr) && SUCCEEDED(pso4_hr) &&
                      SUCCEEDED(pso4_mask_hr) && SUCCEEDED(pso4_alpha_hr) &&
                      SUCCEEDED(queue_hr) &&
                      SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                      SUCCEEDED(target_hr[0]) && SUCCEEDED(target_hr[1]) &&
                      SUCCEEDED(target_hr[2]) && SUCCEEDED(target_hr[3]) &&
                      SUCCEEDED(resolve_hr[0]) && SUCCEEDED(resolve_hr[1]) &&
                      SUCCEEDED(resolve_hr[2]) && SUCCEEDED(resolve_hr[3]) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(rtv_heap_hr) &&
                      SUCCEEDED(execute_hr) && sample_exact && quality_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-graphics-msaa-breadth.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\", \"vs_hr\": \"%s\", \"ps_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str(),
                hr_hex(vs_hr).c_str(), hr_hex(ps_hr).c_str());
    std::printf("  \"pso_hr\": [\"%s\", \"%s\", \"%s\", \"%s\"],\n",
                hr_hex(pso2_hr).c_str(), hr_hex(pso4_hr).c_str(),
                hr_hex(pso4_mask_hr).c_str(), hr_hex(pso4_alpha_hr).c_str());
    std::printf("  \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n",
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str());
    std::printf("  \"samples\": [\n");
    for (UINT i = 0; i < 4; ++i) {
        std::printf("    {\"count\": %u, \"sample_mask\": %u, \"alpha_to_coverage\": %s, \"values\": [%g, %g, %g, %g], \"expected\": [%g, %g, %g, %g], \"exact\": %s}%s\n",
                    results[i].samples, results[i].sample_mask,
                    i == 3 ? "true" : "false", results[i].values[0], results[i].values[1],
                    results[i].values[2], results[i].values[3],
                    results[i].expected[0], results[i].expected[1],
                    results[i].expected[2], results[i].expected[3],
                    results[i].exact ? "true" : "false", i == 3 ? "" : ",");
    }
    std::printf("  ],\n  \"quality\": [\n");
    const UINT quality_counts[3] = {1, 2, 4};
    for (UINT i = 0; i < 3; ++i)
        std::printf("    {\"count\": %u, \"hr\": \"%s\", \"num_quality_levels\": %u, \"exact\": %s}%s\n",
                    quality_counts[i], hr_hex(quality_hr[i]).c_str(),
                    quality[i].NumQualityLevels,
                    (quality_hr[i] == S_OK && quality[i].NumQualityLevels == 1)
                        ? "true" : "false",
                    i == 2 ? "" : ",");
    std::printf("  ],\n  \"quality_exact\": %s,\n  \"pass\": %s,\n"
                "  \"provider\": \"native_render_target_msaa_sample_frequency_and_mask\"\n}\n",
                quality_exact ? "true" : "false", pass ? "true" : "false");
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
    safe_release(pso4_alpha);
    safe_release(pso4_mask);
    safe_release(pso4);
    safe_release(pso2);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
