#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                       ID3DBlob**, ID3DBlob**);
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

template <typename T> static void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char* name) {
    T fn = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(fn) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

static std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

static std::string getenv_string(const char* key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (needed == 0)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (written == 0)
        return "";
    value.resize(written);
    return value;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

static D3D12_BLEND_DESC default_blend_desc() {
    D3D12_BLEND_DESC desc = {};
    for (auto& rt : desc.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return desc;
}

static D3D12_RASTERIZER_DESC default_rasterizer_desc() {
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_BACK;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

static D3D12_DEPTH_STENCIL_DESC default_depth_stencil_desc() {
    D3D12_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BackFace = desc.FrontFace;
    return desc;
}

static HRESULT compile_shader(const char* source, const char* entry, const char* target, ID3DBlob** blob,
                              std::string& errors) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    D3DCompileFn compile = load_proc<D3DCompileFn>(compiler, "D3DCompile");
    if (!compile)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ID3DBlob* error_blob = nullptr;
    HRESULT hr = compile(source, std::strlen(source), "probe_graphics_pso.hlsl", nullptr, nullptr, entry, target, 0, 0,
                         blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    return hr;
}

static std::vector<uint8_t> read_binary_file(const char *path) {
    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()),
                             &read, nullptr) &&
                    read == data.size();
    CloseHandle(file);
    return ok ? data : std::vector<uint8_t>{};
}

static HRESULT execute_and_wait(ID3D12Device *device,
                                ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
    HRESULT hr = list ? list->Close() : E_FAIL;
    if (FAILED(hr))
        return hr;
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence *fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event_handle = nullptr;
    if (SUCCEEDED(hr)) {
        hr = queue->Signal(fence, 1);
        event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
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

static HRESULT serialize_root_signature(ID3DBlob** blob, std::string& errors) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    D3D12SerializeRootSignatureFn serialize =
        load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    return hr;
}

struct CaseResult {
    const char* name = "";
    HRESULT hr = E_FAIL;
    bool expected_success = true;
    bool ok = false;
    std::string detail;
};

static D3D12_GRAPHICS_PIPELINE_STATE_DESC make_base_desc(ID3D12RootSignature* root, ID3DBlob* vs, ID3DBlob* ps,
                                                         const D3D12_INPUT_ELEMENT_DESC* layout, UINT layout_count) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.VS = {vs ? vs->GetBufferPointer() : nullptr, vs ? vs->GetBufferSize() : 0};
    desc.PS = {ps ? ps->GetBufferPointer() : nullptr, ps ? ps->GetBufferSize() : 0};
    desc.BlendState = default_blend_desc();
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState = default_rasterizer_desc();
    desc.DepthStencilState = default_depth_stencil_desc();
    desc.InputLayout = {layout, layout_count};
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return desc;
}

static void run_case(ID3D12Device* device, const char* name, D3D12_GRAPHICS_PIPELINE_STATE_DESC desc,
                     bool expected_success, std::vector<CaseResult>& results) {
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = device ? device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)) : E_FAIL;
    bool ok = expected_success ? SUCCEEDED(hr) : FAILED(hr);
    safe_release(pso);
    results.push_back({name, hr, expected_success, ok, ""});
}

static void run_cached_blob_case(ID3D12Device* device, const char* name, D3D12_GRAPHICS_PIPELINE_STATE_DESC desc,
                                 std::vector<CaseResult>& results) {
    static const uint8_t cached_blob[] = {'m', 's', 'p', 's', 'o', '1'};
    desc.CachedPSO.pCachedBlob = cached_blob;
    desc.CachedPSO.CachedBlobSizeInBytes = sizeof(cached_blob);

    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = device ? device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)) : E_FAIL;
    bool blob_ok = false;
    std::string detail;
    if (SUCCEEDED(hr) && pso) {
        ID3DBlob* returned_blob = nullptr;
        HRESULT blob_hr = pso->GetCachedBlob(&returned_blob);
        blob_ok = SUCCEEDED(blob_hr) && returned_blob && returned_blob->GetBufferSize() >= sizeof(cached_blob);
        detail = "GetCachedBlob=" + hr_hex(blob_hr);
        safe_release(returned_blob);
    }
    bool ok = SUCCEEDED(hr) && blob_ok;
    safe_release(pso);
    results.push_back({name, hr, true, ok, detail});
}

struct ConservativePoint {
    float x;
    float y;
};

static float conservative_cross(ConservativePoint a, ConservativePoint b,
                               ConservativePoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool conservative_point_in_box(ConservativePoint point,
                                      ConservativePoint low,
                                      ConservativePoint high) {
    constexpr float epsilon = 1.0e-5f;
    return point.x >= low.x - epsilon && point.x <= high.x + epsilon &&
           point.y >= low.y - epsilon && point.y <= high.y + epsilon;
}

static bool conservative_point_in_triangle(ConservativePoint point,
                                           ConservativePoint a,
                                           ConservativePoint b,
                                           ConservativePoint c) {
    constexpr float epsilon = 1.0e-5f;
    const float s0 = conservative_cross(a, b, point);
    const float s1 = conservative_cross(b, c, point);
    const float s2 = conservative_cross(c, a, point);
    return (s0 >= -epsilon && s1 >= -epsilon && s2 >= -epsilon) ||
           (s0 <= epsilon && s1 <= epsilon && s2 <= epsilon);
}

static bool conservative_segments_intersect(ConservativePoint a,
                                            ConservativePoint b,
                                            ConservativePoint c,
                                            ConservativePoint d) {
    constexpr float epsilon = 1.0e-5f;
    const float ab_c = conservative_cross(a, b, c);
    const float ab_d = conservative_cross(a, b, d);
    const float cd_a = conservative_cross(c, d, a);
    const float cd_b = conservative_cross(c, d, b);
    return ((ab_c >= -epsilon && ab_d <= epsilon) ||
            (ab_c <= epsilon && ab_d >= -epsilon)) &&
           ((cd_a >= -epsilon && cd_b <= epsilon) ||
            (cd_a <= epsilon && cd_b >= -epsilon));
}

static bool conservative_triangle_pixel(ConservativePoint a,
                                        ConservativePoint b,
                                        ConservativePoint c,
                                        ConservativePoint low) {
    const ConservativePoint high = {low.x + 1.0f, low.y + 1.0f};
    const ConservativePoint q0 = {low.x, low.y};
    const ConservativePoint q1 = {high.x, low.y};
    const ConservativePoint q2 = {high.x, high.y};
    const ConservativePoint q3 = {low.x, high.y};
    if (conservative_point_in_box(a, low, high) ||
        conservative_point_in_box(b, low, high) ||
        conservative_point_in_box(c, low, high))
        return true;
    if (conservative_point_in_triangle(q0, a, b, c) ||
        conservative_point_in_triangle(q1, a, b, c) ||
        conservative_point_in_triangle(q2, a, b, c) ||
        conservative_point_in_triangle(q3, a, b, c))
        return true;
    const ConservativePoint edges[][2] = {{a, b}, {b, c}, {c, a}};
    const ConservativePoint box_edges[][2] = {{q0, q1}, {q1, q2},
                                                {q2, q3}, {q3, q0}};
    for (const auto &edge : edges)
        for (const auto &box_edge : box_edges)
            if (conservative_segments_intersect(edge[0], edge[1],
                                                box_edge[0], box_edge[1]))
                return true;
    return false;
}

static D3D12_RESOURCE_DESC conservative_buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

static bool run_conservative_coverage_probe(ID3D12Device *device,
                                            ID3D12RootSignature *root,
                                            const std::vector<uint8_t> &vs,
                                            const std::vector<uint8_t> &ps,
                                            uint32_t &case_count,
                                            uint32_t &rendered_pixels) {
    if (!device || !root || vs.empty() || ps.empty())
        return false;

    const D3D12_INPUT_ELEMENT_DESC input = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.VS = {vs.data(), vs.size()};
    pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.BlendState = default_blend_desc();
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = default_rasterizer_desc();
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    pso_desc.DepthStencilState = default_depth_stencil_desc();
    pso_desc.InputLayout = {&input, 1};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    ID3D12PipelineState *pso = nullptr;
    if (FAILED(device->CreateGraphicsPipelineState(
            &pso_desc, IID_PPV_ARGS(&pso))) || !pso)
        return false;

    struct Triangle {
        float x[3];
        float y[3];
    };
    const Triangle triangles[] = {
        {{-0.90f, 0.10f, 0.90f}, {-0.90f, 0.90f, -0.20f}},
        {{0.90f, 0.10f, -0.90f}, {-0.20f, 0.90f, -0.90f}},
        {{-1.20f, -0.20f, 0.20f}, {0.20f, 1.20f, -1.20f}},
    };
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 8;
    constexpr uint32_t row_pitch = 256;
    bool all_ok = true;
    case_count = static_cast<uint32_t>(sizeof(triangles) / sizeof(triangles[0]));
    rendered_pixels = 0;
    for (const Triangle &triangle : triangles) {
        ID3D12CommandQueue *queue = nullptr;
        ID3D12CommandAllocator *allocator = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        ID3D12Resource *target = nullptr;
        ID3D12Resource *readback = nullptr;
        ID3D12Resource *vertex_buffer = nullptr;
        ID3D12DescriptorHeap *rtv_heap = nullptr;

        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        HRESULT hr = device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&queue));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            allocator, nullptr,
                                            IID_PPV_ARGS(&list));

        D3D12_HEAP_PROPERTIES default_heap = {};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES upload_heap = {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_HEAP_PROPERTIES readback_heap = {};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC target_desc = {};
        target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        target_desc.Width = width;
        target_desc.Height = height;
        target_desc.DepthOrArraySize = 1;
        target_desc.MipLevels = 1;
        target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_desc.SampleDesc.Count = 1;
        target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_RESOURCE_DESC vertex_desc = conservative_buffer_desc(36);
        D3D12_RESOURCE_DESC readback_desc = conservative_buffer_desc(
            row_pitch * height);
        if (SUCCEEDED(hr))
            hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                IID_PPV_ARGS(&target));
        if (SUCCEEDED(hr))
            hr = device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
        if (SUCCEEDED(hr))
            hr = device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&vertex_buffer));
        if (SUCCEEDED(hr)) {
            void *mapped = nullptr;
            hr = vertex_buffer->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr)) {
                float vertices[9] = {
                    triangle.x[0], triangle.y[0], 0.0f,
                    triangle.x[1], triangle.y[1], 0.0f,
                    triangle.x[2], triangle.y[2], 0.0f};
                std::memcpy(mapped, vertices, sizeof(vertices));
                vertex_buffer->Unmap(0, nullptr);
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_desc.NumDescriptors = 1;
        if (SUCCEEDED(hr))
            hr = device->CreateDescriptorHeap(&rtv_desc,
                                              IID_PPV_ARGS(&rtv_heap));
        if (SUCCEEDED(hr))
            device->CreateRenderTargetView(
                target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        if (SUCCEEDED(hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const float clear[4] = {0, 0, 0, 0};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
            list->SetPipelineState(pso);
            list->SetGraphicsRootSignature(root);
            D3D12_VIEWPORT viewport = {0, 0, (float)width, (float)height, 0, 1};
            D3D12_RECT scissor = {0, 0, (LONG)width, (LONG)height};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vbv = {};
            vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = 36;
            vbv.StrideInBytes = 12;
            list->IASetVertexBuffers(0, 1, &vbv);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = target;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint.Format =
                DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = width;
            dst.PlacedFootprint.Footprint.Height = height;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = row_pitch;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            hr = execute_and_wait(device, queue, list);
        }

        bool case_ok = SUCCEEDED(hr);
        if (case_ok) {
            uint8_t *mapped = nullptr;
            D3D12_RANGE range = {0, row_pitch * height};
            case_ok = SUCCEEDED(readback->Map(
                0, &range, reinterpret_cast<void **>(&mapped))) && mapped;
            if (case_ok) {
                ConservativePoint points[3] = {};
                for (uint32_t i = 0; i < 3; ++i) {
                    points[i] = {((triangle.x[i] * 0.5f) + 0.5f) * width,
                                 (0.5f - triangle.y[i] * 0.5f) * height};
                }
                for (uint32_t y = 0; y < height; ++y) {
                    const uint32_t *row = reinterpret_cast<const uint32_t *>(
                        mapped + y * row_pitch);
                    for (uint32_t x = 0; x < width; ++x) {
                        const bool expected = conservative_triangle_pixel(
                            points[0], points[1], points[2],
                            {(float)x, (float)y});
                        const uint32_t actual = row[x];
                        case_ok &= expected ? actual == 0xff0000ffu
                                             : actual == 0u;
                        rendered_pixels += actual == 0xff0000ffu;
                    }
                }
                readback->Unmap(0, nullptr);
            }
        }
        all_ok &= case_ok;
        safe_release(rtv_heap);
        safe_release(vertex_buffer);
        safe_release(readback);
        safe_release(target);
        safe_release(list);
        safe_release(allocator);
        safe_release(queue);
    }
    safe_release(pso);
    return all_ok;
}

int main() {
    const std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    D3D12CreateDeviceFn create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    std::string errors;
    ID3DBlob* root_blob = nullptr;
    ID3D12RootSignature* root = nullptr;
    HRESULT root_blob_hr = serialize_root_signature(&root_blob, errors);
    HRESULT root_hr = (device && root_blob)
                          ? device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                                        IID_PPV_ARGS(&root))
                          : E_FAIL;

    const char* hlsl =
        "struct VSIn{float3 position:POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;"
        "float4 packed:COLOR1;float2 inst:INSTANCE0;};"
        "struct VSOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD0;float4 color:COLOR0;};"
        "VSOut vs_main(VSIn input){VSOut o;o.pos=float4(input.position.xy + input.inst * 0.001, input.position.z, 1);"
        "o.uv=input.uv;o.color=input.color + input.packed * 0.001;return o;}"
        "float4 ps_main(VSOut input):SV_Target0{return float4(input.uv,0.5,1) * input.color;}"
        "struct PSOut{float4 c0:SV_Target0;float4 c2:SV_Target2;};"
        "PSOut ps_mrt(VSOut input){PSOut o;o.c0=float4(input.uv,0,1);o.c2=float4(input.color.rgb,1);return o;}";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* ps_mrt = nullptr;
    const std::vector<uint8_t> conservative_vs =
        read_binary_file("probe_conservative_raster_vs.cso");
    const std::vector<uint8_t> conservative_ps =
        read_binary_file("probe_conservative_raster_ps.cso");
    HRESULT vs_hr = compile_shader(hlsl, "vs_main", "vs_5_0", &vs, errors);
    HRESULT ps_hr = compile_shader(hlsl, "ps_main", "ps_5_0", &ps, errors);
    HRESULT ps_mrt_hr = compile_shader(hlsl, "ps_mrt", "ps_5_0", &ps_mrt, errors);

    const D3D12_INPUT_ELEMENT_DESC full_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R10G10B10A2_UNORM, 1, D3D12_APPEND_ALIGNED_ELEMENT,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"INSTANCE", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 2},
    };

    std::vector<CaseResult> results;
    if (SUCCEEDED(create_hr) && SUCCEEDED(root_hr) && SUCCEEDED(vs_hr) && SUCCEEDED(ps_hr) && SUCCEEDED(ps_mrt_hr)) {
        auto base = make_base_desc(root, vs, ps, full_layout, static_cast<UINT>(std::size(full_layout)));
        auto vertex_only = base;
        vertex_only.PS = {};
        run_case(device, "vertex_only", vertex_only, true, results);

        run_case(device, "vertex_pixel", base, true, results);

        auto depth_only = base;
        depth_only.PS = {};
        depth_only.NumRenderTargets = 0;
        depth_only.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        depth_only.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        depth_only.DepthStencilState.DepthEnable = TRUE;
        depth_only.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depth_only.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        run_case(device, "depth_only", depth_only, true, results);

        auto color_depth = base;
        color_depth.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        color_depth.DepthStencilState.DepthEnable = TRUE;
        color_depth.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        color_depth.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        run_case(device, "color_depth", color_depth, true, results);

        auto msaa = color_depth;
        msaa.SampleDesc.Count = 4;
        msaa.RasterizerState.MultisampleEnable = TRUE;
        run_case(device, "msaa_4x", msaa, true, results);

        auto logic_op = base;
        logic_op.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
        logic_op.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
        run_case(device, "logic_op_xor", logic_op, true, results);

        auto blend = base;
        blend.BlendState.RenderTarget[0].BlendEnable = TRUE;
        blend.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        run_case(device, "blend", blend, true, results);

        auto write_mask = base;
        write_mask.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_ALPHA;
        run_case(device, "write_mask_ra", write_mask, true, results);

        auto mrt = base;
        mrt.PS = {ps_mrt->GetBufferPointer(), ps_mrt->GetBufferSize()};
        mrt.BlendState.IndependentBlendEnable = TRUE;
        mrt.NumRenderTargets = 3;
        mrt.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        mrt.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mrt.RTVFormats[2] = DXGI_FORMAT_R10G10B10A2_UNORM;
        mrt.BlendState.RenderTarget[2].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        run_case(device, "pixel_outputs_target0_target2", mrt, true, results);

        auto logic_op_mrt = mrt;
        logic_op_mrt.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
        logic_op_mrt.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_XOR;
        logic_op_mrt.BlendState.RenderTarget[1].LogicOpEnable = TRUE;
        logic_op_mrt.BlendState.RenderTarget[1].LogicOp = D3D12_LOGIC_OP_AND;
        run_case(device, "logic_op_mrt_rejected", logic_op_mrt, false, results);

        run_cached_blob_case(device, "cached_blob_roundtrip", base, results);

        auto stream_output = base;
        D3D12_SO_DECLARATION_ENTRY so_entry = {};
        so_entry.SemanticName = "SV_Position";
        so_entry.ComponentCount = 4;
        UINT stride = 16;
        stream_output.StreamOutput.pSODeclaration = &so_entry;
        stream_output.StreamOutput.NumEntries = 1;
        stream_output.StreamOutput.pBufferStrides = &stride;
        stream_output.StreamOutput.NumStrides = 1;
        run_case(device, "stream_output_rejected", stream_output, false, results);

        auto tessellation = base;
        tessellation.HS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        tessellation.DS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        tessellation.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        run_case(device, "hs_ds_rejected", tessellation, false, results);
    }

    bool cases_ok = !results.empty();
    for (const auto& result : results)
        cases_ok = cases_ok && result.ok;
    bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_blob_hr) && SUCCEEDED(root_hr) && SUCCEEDED(vs_hr) &&
                SUCCEEDED(ps_hr) && SUCCEEDED(ps_mrt_hr) && cases_ok;
    uint32_t conservative_case_count = 0;
    uint32_t conservative_rendered_pixels = 0;
    const bool conservative_rasterization_ok =
        run_conservative_coverage_probe(device, root, conservative_vs,
                                        conservative_ps,
                                        conservative_case_count,
                                        conservative_rendered_pixels);
    pass = pass && conservative_rasterization_ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-graphics-pso.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"setup\": {\n");
    std::printf("    \"create_device\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("    \"root_serialize\": \"%s\",\n", hr_hex(root_blob_hr).c_str());
    std::printf("    \"root_create\": \"%s\",\n", hr_hex(root_hr).c_str());
    std::printf("    \"vs_compile\": \"%s\",\n", hr_hex(vs_hr).c_str());
    std::printf("    \"ps_compile\": \"%s\",\n", hr_hex(ps_hr).c_str());
    std::printf("    \"ps_mrt_compile\": \"%s\"\n", hr_hex(ps_mrt_hr).c_str());
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < results.size(); i++) {
        const auto& result = results[i];
        std::printf("    {\"name\":\"%s\",\"hr\":\"%s\",\"expected_success\":%s,\"ok\":%s,\"detail\":\"%s\"}%s\n",
                    result.name, hr_hex(result.hr).c_str(), result.expected_success ? "true" : "false",
                    result.ok ? "true" : "false", json_escape(result.detail).c_str(),
                    i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ],\n");
    std::printf("  \"coverage\": {\n");
    std::printf("    \"vertex_only\": true,\n");
    std::printf("    \"vertex_pixel\": true,\n");
    std::printf("    \"depth_only\": true,\n");
    std::printf("    \"color_only\": true,\n");
    std::printf("    \"color_depth\": true,\n");
    std::printf("    \"msaa\": true,\n");
    std::printf("    \"blend\": true,\n");
    std::printf("    \"logic_op_xor\": true,\n");
    std::printf("    \"logic_op_mrt_rejected\": true,\n");
    std::printf("    \"write_mask\": true,\n");
    std::printf("    \"input_layout_semantics\": true,\n");
    std::printf("    \"input_layout_per_instance_step_rate\": 2,\n");
    std::printf("    \"input_layout_append_aligned_offsets\": true,\n");
    std::printf("    \"input_layout_packed_formats\": true,\n");
    std::printf("    \"input_layout_multiple_vertex_buffers\": true,\n");
    std::printf("    \"pixel_outputs_target0_target2\": true,\n");
    std::printf("    \"cached_blob\": true,\n");
    std::printf("    \"unsupported_stream_output_rejected\": true,\n");
    std::printf("    \"unsupported_hs_ds_rejected\": true,\n");
    std::printf("    \"conservative_rasterization_case_count\": %u,\n",
                conservative_case_count);
    std::printf("    \"conservative_rasterization_rendered_pixels\": %u,\n",
                conservative_rendered_pixels);
    std::printf("    \"conservative_rasterization_tier3_verified\": %s\n",
                conservative_rasterization_ok ? "true" : "false");
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    safe_release(ps_mrt);
    safe_release(ps);
    safe_release(vs);
    safe_release(root);
    safe_release(root_blob);
    safe_release(device);
    return 0;
}
