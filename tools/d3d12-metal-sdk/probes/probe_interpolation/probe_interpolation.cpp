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

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

static std::string env_string(const char *name) {
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (!needed)
        return {};
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (!written)
        return {};
    value.resize(written);
    return value;
}

static std::string json_escape(const std::string &value) {
    std::string result;
    for (char c : value) {
        if (c == '\\' || c == '"')
            result += '\\';
        result += c;
    }
    return result;
}

static std::vector<uint8_t> read_rgba(ID3D12Device *device,
                                      ID3D12CommandQueue *queue,
                                      ID3D12GraphicsCommandList *list,
                                      ID3D12Resource *readback,
                                      UINT64 total_bytes, HRESULT &hr) {
    ID3D12Fence *fence = nullptr;
    HANDLE event_handle = nullptr;
    std::vector<uint8_t> rgba(4, 0);
    hr = list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence));
    }
    if (SUCCEEDED(hr))
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr)) {
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event_handle)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event_handle);
    if (SUCCEEDED(hr) &&
        WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (SUCCEEDED(hr)) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, total_bytes};
        hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(rgba.data(), mapped, rgba.size());
            readback->Unmap(0, nullptr);
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
    }
    if (event_handle)
        CloseHandle(event_handle);
    safe_release(fence);
    return rgba;
}

struct CaseResult {
    const char *name = "";
    uint8_t expected_rgba[4] = {0, 0, 0, 255};
    HRESULT pso_hr = E_FAIL;
    HRESULT execute_hr = E_FAIL;
    uint8_t rgba[4] = {};
    bool readback = false;
    bool exact = false;
};

struct InvalidCaseResult {
    HRESULT pso_hr = E_FAIL;
    bool object_null = true;
    bool exact = false;
};

static CaseResult run_case(ID3D12Device *device, ID3D12RootSignature *root,
                           const std::vector<uint8_t> &vs,
                           const std::vector<uint8_t> &ps, const char *name) {
    CaseResult result;
    result.name = name;
    if (std::strcmp(name, "evaluation") == 0) {
        result.expected_rgba[0] = 96;
        result.expected_rgba[1] = 96;
        result.expected_rgba[2] = 96;
    } else {
        result.expected_rgba[0] = std::strcmp(name, "noperspective") == 0 ? 128
                                  : std::strcmp(name, "nointerpolation") == 0 ? 0
                                                                              : 96;
    }
    if (!device || !root || vs.empty() || ps.empty())
        return result;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = root;
    pipeline.VS = {vs.data(), vs.size()};
    pipeline.PS = {ps.data(), ps.size()};
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.FrontCounterClockwise = FALSE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.SampleDesc.Count = 1;

    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device->CreateGraphicsPipelineState(
        &pipeline, IID_PPV_ARGS(&pso));
    if (FAILED(result.pso_hr))
        return result;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    HRESULT hr = S_OK;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        allocator, nullptr,
                                        IID_PPV_ARGS(&list));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    if (SUCCEEDED(hr))
        hr = device->CreateDescriptorHeap(&heap_desc,
                                          IID_PPV_ARGS(&rtv_heap));

    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 1;
    target_desc.Height = 1;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;
    D3D12_CLEAR_VALUE clear_value = {};
    clear_value.Format = target_desc.Format;
    clear_value.Color[3] = 1.0f;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr)) {
        device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint,
                                      &rows, &row_size, &total_bytes);
        if (!total_bytes)
            hr = E_FAIL;
    }
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = total_bytes;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback));

    if (SUCCEEDED(hr)) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        const float clear[4] = {};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(root);
        D3D12_VIEWPORT viewport = {0, 0, 1, 1, 0, 1};
        D3D12_RECT scissor = {0, 0, 1, 1};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        // DXMT may compile a graphics PSO on its worker after creation.  Give
        // that bounded operation time to finish before closing the list; this
        // also makes a genuine Metal source error observable as a failed
        // execute rather than a race with process teardown.
        Sleep(1000);
        const auto rgba = read_rgba(device, queue, list, readback, total_bytes,
                                    result.execute_hr);
        if (SUCCEEDED(result.execute_hr)) {
            std::memcpy(result.rgba, rgba.data(), sizeof(result.rgba));
            result.readback = true;
            result.exact = std::memcmp(result.rgba, result.expected_rgba,
                                       sizeof(result.rgba)) == 0;
        }
    } else {
        result.execute_hr = hr;
    }

    safe_release(readback);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    return result;
}

static InvalidCaseResult run_invalid_shader_bytecode(
    ID3D12Device *device, ID3D12RootSignature *root,
    const std::vector<uint8_t> &vs, const std::vector<uint8_t> &ps) {
    InvalidCaseResult result;
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
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1;
    pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipeline.SampleDesc.Count = 1;

    ID3D12PipelineState *pso = nullptr;
    result.pso_hr = device->CreateGraphicsPipelineState(
        &pipeline, IID_PPV_ARGS(&pso));
    result.object_null = pso == nullptr;
    result.exact = result.pso_hr == E_FAIL && result.object_null;
    safe_release(pso);
    return result;
}

int main(int argc, char **argv) {
    if (argc != 7 && argc != 8 && argc != 9) {
        std::fprintf(stderr,
                     "usage: probe_interpolation <vs> <linear> <noperspective> "
                     "<centroid> <sample> <flat> [<evaluation> [<invalid>]]\n");
        return 2;
    }
    const auto vs = read_binary_file(argv[1]);
    const auto linear = read_binary_file(argv[2]);
    const auto noperspective = read_binary_file(argv[3]);
    const auto centroid = read_binary_file(argv[4]);
    const auto sample = read_binary_file(argv[5]);
    const auto flat = read_binary_file(argv[6]);
    const auto evaluation = argc >= 8 ? read_binary_file(argv[7])
                                       : std::vector<uint8_t>{};
    const auto invalid = argc == 9 ? read_binary_file(argv[8])
                                   : std::vector<uint8_t>{};

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                              REFIID, void **);
    using SerializeFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    auto create_device = load_proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = load_proc<SerializeFn>(d3d12,
                                            "D3D12SerializeRootSignature");
    ID3D12Device *device = nullptr;
    HRESULT device_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            kDeviceIID,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    ID3DBlob *root_blob = nullptr;
    ID3DBlob *root_errors = nullptr;
    HRESULT root_hr = serialize
                          ? serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &root_blob, &root_errors)
                          : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    safe_release(root_errors);
    ID3D12RootSignature *root = nullptr;
    if (SUCCEEDED(device_hr) && SUCCEEDED(root_hr))
        root_hr = device->CreateRootSignature(
            0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
            IID_PPV_ARGS(&root));

    std::vector<CaseResult> results;
    if (SUCCEEDED(device_hr) && SUCCEEDED(root_hr)) {
        results.push_back(run_case(device, root, vs, linear, "linear"));
        results.push_back(run_case(device, root, vs, noperspective,
                                   "noperspective"));
        results.push_back(run_case(device, root, vs, centroid, "centroid"));
        results.push_back(run_case(device, root, vs, sample, "sample"));
        results.push_back(run_case(device, root, vs, flat, "nointerpolation"));
        if (argc >= 8)
            results.push_back(run_case(device, root, vs, evaluation, "evaluation"));
    }

    InvalidCaseResult invalid_result;
    if (argc == 9 && SUCCEEDED(device_hr) && SUCCEEDED(root_hr)) {
        // Keep the negative lane independent of compiler signature-linkage
        // policy.  A truncated DXIL container is unambiguously invalid and
        // must return E_FAIL with no usable pipeline object.
        auto malformed = invalid;
        if (malformed.size() > 16)
            malformed.resize(malformed.size() - 8);
        invalid_result = run_invalid_shader_bytecode(device, root, vs,
                                                     malformed);
    }

    bool exact = results.size() == (argc >= 8 ? 6u : 5u);
    for (const auto &result : results)
        exact = exact && result.pso_hr == S_OK && result.execute_hr == S_OK &&
                result.readback && result.exact;

    if (argc == 9)
        exact = exact && invalid_result.exact;

    const std::string profile = json_escape(env_string("D3D12_METAL_SDK_PROFILE"));
    std::printf("{\n  \"schema\": \"metalsharp.d3d12.interpolation.v1\",\n"
                "  \"profile\": \"%s\",\n  \"provider\": "
                "\"native_msl_stage_in_qualifiers\",\n"
                "  \"create_device\": \"%s\",\n"
                "  \"root_signature\": \"%s\",\n  \"cases\": [\n",
                profile.c_str(), hr_hex(device_hr).c_str(),
                hr_hex(root_hr).c_str());
    for (size_t i = 0; i < results.size(); ++i) {
        const CaseResult &result = results[i];
        std::printf("    {\"name\": \"%s\", \"pso\": \"%s\", "
                    "\"execute\": \"%s\", \"readback\": %s, "
                    "\"rgba\": [%u, %u, %u, %u], "
                    "\"expected_rgba\": [%u, %u, %u, %u], \"exact\": %s}%s\n",
                    result.name, hr_hex(result.pso_hr).c_str(),
                    hr_hex(result.execute_hr).c_str(),
                    result.readback ? "true" : "false", result.rgba[0],
                    result.rgba[1], result.rgba[2], result.rgba[3],
                    result.expected_rgba[0], result.expected_rgba[1],
                    result.expected_rgba[2], result.expected_rgba[3],
                    result.exact ? "true" : "false",
                    i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ],\n");
    if (argc == 9)
        std::printf("  \"invalid_shader_bytecode\": {\"pso\": \"%s\", "
                    "\"object_null\": %s, \"exact\": %s},\n",
                    hr_hex(invalid_result.pso_hr).c_str(),
                    invalid_result.object_null ? "true" : "false",
                    invalid_result.exact ? "true" : "false");
    std::printf("  \"exact\": %s,\n"
                "  \"bounded_wait_ms\": 15000,\n"
                "  \"scope\": \"one full-coverage pixel; analytic perspective="
                "96, noperspective=128, centroid/sample=96, flat=0; evaluation="
                "[96,96,96,255]\"\n}\n",
                exact ? "true" : "false");
    std::fflush(stdout);

    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return exact ? 0 : 1;
}
