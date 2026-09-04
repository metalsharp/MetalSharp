#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cmath>
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

struct Point {
    float x;
    float y;
};

static float cross(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

static bool point_in_triangle(Point p, Point a, Point b, Point c) {
    constexpr float epsilon = 1.0e-5f;
    const float area = cross(a, b, c);
    if (std::abs(area) <= epsilon)
        return false;
    const float e0 = cross(a, b, p);
    const float e1 = cross(b, c, p);
    const float e2 = cross(c, a, p);
    return (e0 >= -epsilon && e1 >= -epsilon && e2 >= -epsilon) ||
           (e0 <= epsilon && e1 <= epsilon && e2 <= epsilon);
}

static bool point_in_box(Point p, Point lo, Point hi) {
    constexpr float epsilon = 1.0e-5f;
    return p.x >= lo.x - epsilon && p.x <= hi.x + epsilon &&
           p.y >= lo.y - epsilon && p.y <= hi.y + epsilon;
}

static bool segment_intersects(Point a, Point b, Point c, Point d) {
    constexpr float epsilon = 1.0e-5f;
    const float ab_c = cross(a, b, c);
    const float ab_d = cross(a, b, d);
    const float cd_a = cross(c, d, a);
    const float cd_b = cross(c, d, b);
    return ((ab_c >= -epsilon && ab_d <= epsilon) ||
            (ab_c <= epsilon && ab_d >= -epsilon)) &&
           ((cd_a >= -epsilon && cd_b <= epsilon) ||
            (cd_a <= epsilon && cd_b >= -epsilon));
}

static bool conservative_pixel(Point a, Point b, Point c, Point lo) {
    const Point hi = {lo.x + 1.0f, lo.y + 1.0f};
    const Point q[] = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y},
                       {lo.x, hi.y}};
    if (point_in_box(a, lo, hi) || point_in_box(b, lo, hi) ||
        point_in_box(c, lo, hi))
        return true;
    for (const Point corner : q) {
        if (point_in_triangle(corner, a, b, c))
            return true;
    }
    const Point edges[][2] = {{q[0], q[1]}, {q[1], q[2]}, {q[2], q[3]},
                              {q[3], q[0]}};
    const Point sides[][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto &side : sides)
        for (const auto &edge : edges)
            if (segment_intersects(side[0], side[1], edge[0], edge[1]))
                return true;
    return false;
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
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr,
                     "usage: probe_conservative_msaa <vs.dxil> <ps.dxil> [2|4]\n");
        return 2;
    }
    const UINT sample_count = argc == 4
                                  ? static_cast<UINT>(std::strtoul(argv[3], nullptr, 10))
                                  : 4u;
    if (sample_count != 2 && sample_count != 4)
        return 2;
    constexpr UINT width = 8;
    constexpr UINT height = 8;
    constexpr UINT row_pitch = 256;
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
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = root;
    pso_desc.VS = {vs.data(), vs.size()};
    pso_desc.PS = {ps.data(), ps.size()};
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.ConservativeRaster =
        D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    D3D12_INPUT_ELEMENT_DESC input = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    pso_desc.InputLayout = {&input, 1};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = sample_count;
    ID3D12PipelineState *pso = nullptr;
    HRESULT pso_hr = (device && root && !vs.empty() && !ps.empty())
                         ? device->CreateGraphicsPipelineState(
                               &pso_desc, IID_PPV_ARGS(&pso))
                         : E_FAIL;

    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Resource *target = nullptr;
    ID3D12Resource *resolved = nullptr;
    ID3D12Resource *readback = nullptr;
    ID3D12Resource *vertex_buffer = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    HRESULT queue_hr = E_FAIL;
    HRESULT allocator_hr = E_FAIL;
    HRESULT list_hr = E_FAIL;
    HRESULT target_hr = E_FAIL;
    HRESULT resolved_hr = E_FAIL;
    HRESULT readback_hr = E_FAIL;
    HRESULT vertex_hr = E_FAIL;
    HRESULT rtv_hr = E_FAIL;
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
        D3D12_RESOURCE_DESC source_desc = {};
        source_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        source_desc.Width = width;
        source_desc.Height = height;
        source_desc.DepthOrArraySize = 1;
        source_desc.MipLevels = 1;
        source_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        source_desc.SampleDesc.Count = sample_count;
        source_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        source_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = source_desc.Format;
        clear.Color[3] = 0.0f;
        D3D12_HEAP_PROPERTIES default_heap =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        if (SUCCEEDED(list_hr))
            target_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(&target));
        D3D12_RESOURCE_DESC destination_desc = source_desc;
        destination_desc.SampleDesc.Count = 1;
        if (SUCCEEDED(target_hr))
            resolved_hr = device->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &destination_desc,
                D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                IID_PPV_ARGS(&resolved));
        D3D12_HEAP_PROPERTIES upload_heap =
            heap_properties(D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RESOURCE_DESC vertex_desc = {};
        vertex_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vertex_desc.Width = 36;
        vertex_desc.Height = 1;
        vertex_desc.DepthOrArraySize = 1;
        vertex_desc.MipLevels = 1;
        vertex_desc.SampleDesc.Count = 1;
        vertex_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(resolved_hr))
            vertex_hr = device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &vertex_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&vertex_buffer));
        if (SUCCEEDED(vertex_hr)) {
            const float vertices[9] = {
                -0.90f, -0.90f, 0.0f, 0.10f, 0.90f, 0.0f, 0.90f, -0.20f,
                0.0f};
            void *mapped = nullptr;
            vertex_hr = vertex_buffer->Map(0, nullptr, &mapped);
            if (SUCCEEDED(vertex_hr) && mapped) {
                std::memcpy(mapped, vertices, sizeof(vertices));
                vertex_buffer->Unmap(0, nullptr);
            }
        }
        D3D12_HEAP_PROPERTIES readback_heap =
            heap_properties(D3D12_HEAP_TYPE_READBACK);
        D3D12_RESOURCE_DESC readback_desc = {};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width = row_pitch * height;
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(vertex_hr))
            readback_hr = device->CreateCommittedResource(
                &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&readback));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 1;
        if (SUCCEEDED(readback_hr))
            rtv_hr = device->CreateDescriptorHeap(&heap_desc,
                                                  IID_PPV_ARGS(&rtv_heap));
        if (SUCCEEDED(rtv_hr)) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(target, nullptr, rtv);
            const FLOAT clear_color[4] = {0, 0, 0, 0};
            list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
            const D3D12_VIEWPORT viewport = {0, 0, (float)width, (float)height,
                                             0, 1};
            const D3D12_RECT scissor = {0, 0, (LONG)width, (LONG)height};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->SetGraphicsRootSignature(root);
            list->SetPipelineState(pso);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vbv = {};
            vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = 36;
            vbv.StrideInBytes = 12;
            list->IASetVertexBuffers(0, 1, &vbv);
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            list->DrawInstanced(3, 1, 0, 0);
            D3D12_RESOURCE_BARRIER source_barrier = {};
            source_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            source_barrier.Transition.pResource = target;
            source_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            source_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            source_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            list->ResourceBarrier(1, &source_barrier);
            list->ResolveSubresource(resolved, 0, target, 0,
                                     DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_RESOURCE_BARRIER destination_barrier = {};
            destination_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            destination_barrier.Transition.pResource = resolved;
            destination_barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            destination_barrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RESOLVE_DEST;
            destination_barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &destination_barrier);
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = resolved;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = readback;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Footprint.Format =
                DXGI_FORMAT_R8G8B8A8_UNORM;
            destination.PlacedFootprint.Footprint.Width = width;
            destination.PlacedFootprint.Footprint.Height = height;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = row_pitch;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            execute_hr = wait_for_queue(device, queue, list);
        }
    }

    uint8_t pixels[row_pitch * height] = {};
    if (SUCCEEDED(execute_hr) && readback) {
        void *mapped = nullptr;
        D3D12_RANGE range = {0, sizeof(pixels)};
        map_hr = readback->Map(0, &range, &mapped);
        if (SUCCEEDED(map_hr) && mapped) {
            std::memcpy(pixels, mapped, sizeof(pixels));
            readback->Unmap(0, nullptr);
        }
    }
    const Point p0 = {(-0.90f * 0.5f + 0.5f) * width,
                      (0.5f - (-0.90f * 0.5f)) * height};
    const Point p1 = {(0.10f * 0.5f + 0.5f) * width,
                      (0.5f - (0.90f * 0.5f)) * height};
    const Point p2 = {(0.90f * 0.5f + 0.5f) * width,
                      (0.5f - (-0.20f * 0.5f)) * height};
    uint32_t red_pixels = 0;
    bool pixels_exact = SUCCEEDED(map_hr);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const bool expected =
                conservative_pixel(p0, p1, p2, {(float)x, (float)y});
            const uint8_t *pixel = pixels + y * row_pitch + x * 4;
            const bool actual = pixel[0] == 255 && pixel[1] == 0 &&
                                pixel[2] == 0 && pixel[3] == 255;
            if (actual)
                ++red_pixels;
            pixels_exact = pixels_exact && actual == expected;
        }
    }
    const bool pass = SUCCEEDED(create_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(pso_hr) && SUCCEEDED(queue_hr) &&
                      SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                      SUCCEEDED(target_hr) && SUCCEEDED(resolved_hr) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(vertex_hr) &&
                      SUCCEEDED(rtv_hr) && SUCCEEDED(execute_hr) &&
                      pixels_exact;
    uint32_t expected_red_pixels = 0;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
            expected_red_pixels += conservative_pixel(
                p0, p1, p2, {(float)x, (float)y}) ? 1u : 0u;

    std::printf("{\n  \"schema\": \"metalsharp.d3d12.phase6-conservative-msaa.v1\",\n");
    std::printf("  \"create_hr\": \"%s\", \"root_hr\": \"%s\", \"pso_hr\": \"%s\",\n",
                hr_hex(create_hr).c_str(), hr_hex(root_hr).c_str(),
                hr_hex(pso_hr).c_str());
    std::printf("  \"target_hr\": \"%s\", \"resolved_hr\": \"%s\", \"readback_hr\": \"%s\",\n",
                hr_hex(target_hr).c_str(), hr_hex(resolved_hr).c_str(),
                hr_hex(readback_hr).c_str());
    std::printf("  \"execute_hr\": \"%s\", \"map_hr\": \"%s\",\n",
                hr_hex(execute_hr).c_str(), hr_hex(map_hr).c_str());
    std::printf("  \"sample_count\": %u, \"red_pixels\": %u, \"expected_red_pixels\": %u,\n",
                sample_count, red_pixels, expected_red_pixels);
    std::printf("  \"pixels_exact\": %s, \"pass\": %s,\n"
                "  \"provider\": \"gpu_reference_conservative_raster_msaa\"\n}\n",
                pixels_exact ? "true" : "false", pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(readback);
    safe_release(vertex_buffer);
    safe_release(resolved);
    safe_release(target);
    safe_release(rtv_heap);
    safe_release(list);
    safe_release(allocator);
    safe_release(queue);
    safe_release(pso);
    safe_release(root);
    safe_release(device);
    return pass ? 0 : 1;
}
