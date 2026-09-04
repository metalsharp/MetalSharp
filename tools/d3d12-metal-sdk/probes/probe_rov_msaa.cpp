#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 3;
    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
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

static HRESULT create_graphics_pso(ID3D12Device *device,
                                   ID3D12RootSignature *root,
                                   const std::vector<uint8_t> &vs,
                                   const std::vector<uint8_t> &ps,
                                   UINT sample_count,
                                   ID3D12PipelineState **pso) {
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
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = std::min<UINT>(sample_count, 4u);
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
}

static HRESULT create_compute_pso(ID3D12Device *device,
                                  ID3D12RootSignature *root,
                                  const std::vector<uint8_t> &cs,
                                  ID3D12PipelineState **pso) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.CS = {cs.data(), cs.size()};
    return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso));
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
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr,
                     "usage: probe_rov_msaa <vs.dxil> <ps.dxil> <cs.dxil> [2|4|8]\n");
        return 2;
    }
    const UINT sample_count = argc == 5
                                  ? static_cast<UINT>(std::strtoul(argv[4], nullptr, 10))
                                  : 4u;
    if (sample_count != 2 && sample_count != 4 && sample_count != 8)
        return 2;
    const auto vs = read_binary_file(argv[1]);
    const auto ps = read_binary_file(argv[2]);
    const auto cs = read_binary_file(argv[3]);
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
    ID3D12PipelineState *graphics_pso = nullptr;
    ID3D12PipelineState *compute_pso = nullptr;
    HRESULT graphics_pso_hr = (device && root && !vs.empty() && !ps.empty())
                                  ? create_graphics_pso(device, root, vs, ps,
                                                        sample_count,
                                                        &graphics_pso)
                                  : E_FAIL;
    HRESULT compute_pso_hr = (device && root && !cs.empty())
                                 ? create_compute_pso(device, root, cs,
                                                      &compute_pso)
                                 : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *uav_heap = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *target_array = nullptr;
    ID3D12Resource *render_target = nullptr;
    ID3D12Resource *output = nullptr;
    ID3D12Resource *readback = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT uav_heap_hr = E_FAIL;
    HRESULT rtv_heap_hr = E_FAIL;
    HRESULT target_hr = E_FAIL;
    HRESULT target_array_hr = E_FAIL;
    HRESULT render_target_hr = E_FAIL;
    HRESULT output_hr = E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT map_hr = E_FAIL;
    if (device && root && graphics_pso && compute_pso) {
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
        target_desc.Format = DXGI_FORMAT_R32_UINT;
        target_desc.SampleDesc.Count = sample_count;
        target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        target_hr = SUCCEEDED(list_hr)
                        ? device->CreateCommittedResource(
                              &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                              IID_PPV_ARGS(&target))
                        : E_FAIL;
        D3D12_RESOURCE_DESC target_array_desc = target_desc;
        target_array_desc.DepthOrArraySize = 2;
        target_array_hr = SUCCEEDED(target_hr)
                              ? device->CreateCommittedResource(
                                    &default_heap, D3D12_HEAP_FLAG_NONE,
                                    &target_array_desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    nullptr, IID_PPV_ARGS(&target_array))
                              : E_FAIL;
        D3D12_RESOURCE_DESC render_desc = target_desc;
        render_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        render_desc.SampleDesc.Count = std::min<UINT>(sample_count, 4u);
        render_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = render_desc.Format;
        clear.Color[3] = 1.0f;
        render_target_hr = SUCCEEDED(target_hr)
                               ? device->CreateCommittedResource(
                                     &default_heap, D3D12_HEAP_FLAG_NONE,
                                     &render_desc,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                     IID_PPV_ARGS(&render_target))
                               : E_FAIL;
        D3D12_RESOURCE_DESC output_desc = {};
        output_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        output_desc.Width = static_cast<UINT64>(sample_count) * 2u * 4u;
        output_desc.Height = 1;
        output_desc.DepthOrArraySize = 1;
        output_desc.MipLevels = 1;
        output_desc.SampleDesc.Count = 1;
        output_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        output_hr = SUCCEEDED(target_array_hr)
                        ? device->CreateCommittedResource(
                              &default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                              IID_PPV_ARGS(&output))
                        : E_FAIL;
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = output_desc;
        readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        readback_hr = SUCCEEDED(output_hr)
                          ? device->CreateCommittedResource(
                                &readback_heap, D3D12_HEAP_FLAG_NONE,
                                &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                nullptr, IID_PPV_ARGS(&readback))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC uav_desc = {};
        uav_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uav_desc.NumDescriptors = 3;
        uav_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        uav_heap_hr = SUCCEEDED(readback_hr)
                          ? device->CreateDescriptorHeap(&uav_desc,
                                                        IID_PPV_ARGS(&uav_heap))
                          : E_FAIL;
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = 1;
        rtv_heap_hr = SUCCEEDED(uav_heap_hr)
                          ? device->CreateDescriptorHeap(&rtv_desc,
                                                        IID_PPV_ARGS(&rtv_heap))
                          : E_FAIL;
        if (SUCCEEDED(rtv_heap_hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(render_target, nullptr, rtv);
            D3D12_CPU_DESCRIPTOR_HANDLE uav =
                uav_heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_UNORDERED_ACCESS_VIEW_DESC target_view = {};
            target_view.Format = DXGI_FORMAT_R32_UINT;
            target_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(target, nullptr, &target_view,
                                              uav);
            uav.ptr += device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_UNORDERED_ACCESS_VIEW_DESC output_view = {};
            output_view.Format = DXGI_FORMAT_R32_TYPELESS;
            output_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            output_view.Buffer.NumElements = sample_count * 2u;
            output_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device->CreateUnorderedAccessView(output, nullptr, &output_view,
                                              uav);
            uav.ptr += device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_UNORDERED_ACCESS_VIEW_DESC target_array_view = {};
            target_array_view.Format = DXGI_FORMAT_R32_UINT;
            target_array_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            target_array_view.Texture2DArray.ArraySize = 2;
            device->CreateUnorderedAccessView(target_array, nullptr,
                                              &target_array_view, uav);

            ID3D12DescriptorHeap *heaps[] = {uav_heap};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootSignature(root);
            list->SetGraphicsRootDescriptorTable(
                0, uav_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetPipelineState(graphics_pso);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
            const FLOAT clear_color[4] = {0, 0, 0, 1};
            list->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
            const D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
            const D3D12_RECT scissor = {0, 0, 1, 1};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT draw = 0; draw < 3; ++draw) {
                list->DrawInstanced(3, 1, 0, 0);
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = target;
                list->ResourceBarrier(1, &barrier);
                barrier.UAV.pResource = target_array;
                list->ResourceBarrier(1, &barrier);
            }
            list->SetComputeRootSignature(root);
            list->SetComputeRootDescriptorTable(
                0, uav_heap->GetGPUDescriptorHandleForHeapStart());
            list->SetPipelineState(compute_pso);
            list->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER output_barrier = {};
            output_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            output_barrier.Transition.pResource = output;
            output_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            output_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            output_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &output_barrier);
            list->CopyResource(readback, output);
            execute_hr = wait_for_queue(device, queue, list);
        }
    }

    std::vector<uint32_t> values(sample_count * 2u, 0u);
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, values.size() * sizeof(uint32_t)};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            std::memcpy(values.data(), mapped,
                        values.size() * sizeof(uint32_t));
            readback->Unmap(0, nullptr);
        }
    }
    bool values_exact = SUCCEEDED(map_hr);
    for (uint32_t value : values)
        values_exact = values_exact && value == 3u;
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(graphics_pso_hr) && SUCCEEDED(compute_pso_hr) &&
                      SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) &&
                      SUCCEEDED(list_hr) && SUCCEEDED(target_hr) &&
                      SUCCEEDED(target_array_hr) && SUCCEEDED(render_target_hr) &&
                      SUCCEEDED(output_hr) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(uav_heap_hr) &&
                      SUCCEEDED(rtv_heap_hr) && SUCCEEDED(execute_hr) &&
                      values_exact;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-rov-msaa.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str());
    std::printf("  \"graphics_pso_hr\": \"%s\", \"compute_pso_hr\": \"%s\",\n",
                hr_hex(graphics_pso_hr).c_str(), hr_hex(compute_pso_hr).c_str());
    std::printf("  \"target_hr\": \"%s\", \"target_array_hr\": \"%s\", \"render_target_hr\": \"%s\", \"output_hr\": \"%s\", \"readback_hr\": \"%s\",\n",
                hr_hex(target_hr).c_str(), hr_hex(target_array_hr).c_str(),
                hr_hex(render_target_hr).c_str(), hr_hex(output_hr).c_str(),
                hr_hex(readback_hr).c_str());
    std::printf("  \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n",
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str());
    std::printf("  \"draw_count\": 3, \"sample_count\": %u, \"values\": [",
                sample_count);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            std::printf(", ");
        std::printf("%u", values[i]);
    }
    std::printf("],\n  \"values_expected\": [");
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            std::printf(", ");
        std::printf("3");
    }
    std::printf("], \"values_exact\": %s,\n"
                "  \"pass\": %s,\n"
                "  \"provider\": \"flattened_msaa_raster_order_group\"\n}\n",
                values_exact ? "true" : "false", pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(readback);
    safe_release(output);
    safe_release(render_target);
    safe_release(target_array);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(uav_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(compute_pso);
    safe_release(graphics_pso);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
