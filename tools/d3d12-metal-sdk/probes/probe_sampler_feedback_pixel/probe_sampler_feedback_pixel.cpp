#define WIN32_LEAN_AND_MEAN
#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE
#define DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE ((DXGI_FORMAT)189)
#endif

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                  ID3DBlob**, ID3DBlob**);

template <typename T> static void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}
template <typename T> static T proc(HMODULE module, const char* name) {
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    T result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result = {};
    result.Type = type;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}
static D3D12_RESOURCE_DESC texture_desc(UINT64 width, UINT height, DXGI_FORMAT format, UINT16 mips,
                                        D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC result = {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = width;
    result.Height = height;
    result.DepthOrArraySize = 1;
    result.MipLevels = mips;
    result.Format = format;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    result.Flags = flags;
    return result;
}
static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC result = {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}
static bool write_file(const char* path, const char* text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    DWORD size = static_cast<DWORD>(std::strlen(text));
    bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}
static std::vector<uint8_t> read_file(const char* path) {
    HANDLE file =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    data.resize(read);
    return data;
}
static DWORD run_process(const char* command_line) {
    std::vector<char> command(command_line, command_line + std::strlen(command_line) + 1);
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process))
        return UINT32_MAX;
    WaitForSingleObject(process.hProcess, 30000);
    DWORD result = UINT32_MAX;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}
static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr))
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr)) {
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (event)
        CloseHandle(event);
    release(fence);
    return hr;
}

int main() {
    const char* shader_path = "Z:\\tmp\\metalsharp_sampler_feedback_pixel.hlsl";
    const char* source = R"HLSL(
Texture2D<float4> paired : register(t0);
SamplerState feedback_sampler : register(s0);
FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> feedback : register(u0);
struct VSIn { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(VSIn input) {
  VSOut result;
  result.position = float4(input.position, 1);
  result.uv = input.uv;
  return result;
}
float4 ps_implicit(VSOut input) : SV_Target0 {
  feedback.WriteSamplerFeedback(paired, feedback_sampler, input.uv);
  return float4(0.25, 0.5, 0.75, 1.0);
}
float4 ps_bias(VSOut input) : SV_Target0 {
  feedback.WriteSamplerFeedbackBias(paired, feedback_sampler, input.uv, -1.0);
  return float4(0.25, 0.5, 0.75, 1.0);
}
float4 ps_grad(VSOut input) : SV_Target0 {
  feedback.WriteSamplerFeedbackGrad(paired, feedback_sampler, input.uv,
                                    float2(1.0 / 64.0, 0),
                                    float2(0, 1.0 / 64.0));
  return float4(0.25, 0.5, 0.75, 1.0);
}
float4 ps_level(VSOut input) : SV_Target0 {
  feedback.WriteSamplerFeedbackLevel(paired, feedback_sampler, input.uv, 2.0);
  return float4(0.25, 0.5, 0.75, 1.0);
}
)HLSL";

    HRESULT hr = E_FAIL;
    DWORD vs_dxc = UINT32_MAX, ps_dxc[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    bool pso_ok = false, values_ok = false, color_ok = false;
    uint8_t observed[4] = {};
    uint32_t color = 0;

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create_device = proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = proc<SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    ID3D12Device* device = nullptr;
    ID3D12Device8* device8 = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso[4] = {};
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12GraphicsCommandList1* list1 = nullptr;
    ID3D12DescriptorHeap* resource_heap = nullptr;
    ID3D12DescriptorHeap* sampler_heap = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* paired = nullptr;
    ID3D12Resource* feedback[4] = {};
    ID3D12Resource* decoded[4] = {};
    ID3D12Resource* color_target = nullptr;
    ID3D12Resource* vertex_buffer = nullptr;
    ID3D12Resource* readback = nullptr;

    if (!write_file(shader_path, source))
        hr = E_FAIL;
    else {
        vs_dxc = run_process("dxc.exe -nologo -T vs_6_0 -E vs_main -HV 2021 -Od "
                             "-Fo Z:\\tmp\\metalsharp_sampler_feedback_vs.cso "
                             "Z:\\tmp\\metalsharp_sampler_feedback_pixel.hlsl");
        const char* ps_entries[4] = {"ps_implicit", "ps_bias", "ps_grad", "ps_level"};
        std::vector<uint8_t> ps[4];
        for (uint32_t i = 0; i < 4; ++i) {
            char command[512] = {};
            std::snprintf(command, sizeof(command),
                          "dxc.exe -nologo -T ps_6_5 -E %s -HV 2021 -Od "
                          "-Fo Z:\\tmp\\metalsharp_sampler_feedback_ps%u.cso "
                          "Z:\\tmp\\metalsharp_sampler_feedback_pixel.hlsl",
                          ps_entries[i], i);
            ps_dxc[i] = run_process(command);
            char path[128] = {};
            std::snprintf(path, sizeof(path), "Z:\\tmp\\metalsharp_sampler_feedback_ps%u.cso", i);
            ps[i] = read_file(path);
        }
        auto vs = read_file("Z:\\tmp\\metalsharp_sampler_feedback_vs.cso");
        bool shaders_ok = vs_dxc == 0 && !vs.empty();
        for (uint32_t i = 0; i < 4; ++i)
            shaders_ok &= ps_dxc[i] == 0 && !ps[i].empty();
        if (!shaders_ok || !create_device || !serialize)
            hr = E_FAIL;
        else
            hr = create_device(nullptr, D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device),
                               reinterpret_cast<void**>(&device));
        if (SUCCEEDED(hr))
            hr = device->QueryInterface(IID_PPV_ARGS(&device8));

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[2].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[3] = {};
        for (uint32_t i = 0; i < 3; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 3;
        root_desc.pParameters = params;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *root_blob = nullptr, *root_error = nullptr;
        if (SUCCEEDED(hr))
            hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_blob, &root_error);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        release(root_error);
        release(root_blob);

        D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
        if (SUCCEEDED(hr)) {
            auto desc = texture_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D12_RESOURCE_FLAG_NONE);
            hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&paired));
        }
        for (uint32_t i = 0; i < 4 && SUCCEEDED(hr); ++i) {
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 64;
            desc.Height = 64;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 4;
            desc.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            desc.SamplerFeedbackMipRegion = {64, 64, 1};
            hr = device8->CreateCommittedResource2(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, nullptr,
                                                   IID_PPV_ARGS(&feedback[i]));
        }
        for (uint32_t i = 0; i < 4 && SUCCEEDED(hr); ++i) {
            auto desc = texture_desc(1, 1, DXGI_FORMAT_R8_UINT, 1, D3D12_RESOURCE_FLAG_NONE);
            hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&decoded[i]));
        }
        if (SUCCEEDED(hr)) {
            auto desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                 IID_PPV_ARGS(&color_target));
        }
        if (SUCCEEDED(hr)) {
            struct Vertex {
                float position[3];
                float uv[2];
            };
            const Vertex vertices[3] = {
                {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
                {{3.0f, 1.0f, 0.0f}, {2.0f, 0.0f}},
                {{-1.0f, -3.0f, 0.0f}, {0.0f, 2.0f}},
            };
            D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = buffer_desc(sizeof(vertices));
            hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&vertex_buffer));
            if (SUCCEEDED(hr)) {
                void* mapped = nullptr;
                D3D12_RANGE read_range = {0, 0};
                hr = vertex_buffer->Map(0, &read_range, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    std::memcpy(mapped, vertices, sizeof(vertices));
                    D3D12_RANGE written = {0, sizeof(vertices)};
                    vertex_buffer->Unmap(0, &written);
                }
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
            auto desc = buffer_desc(2048);
            hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        }

        auto create_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags,
                               ID3D12DescriptorHeap** heap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = type;
            desc.NumDescriptors = count;
            desc.Flags = flags;
            return device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap));
        };
        if (SUCCEEDED(hr))
            hr = create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                             &resource_heap);
        if (SUCCEEDED(hr))
            hr = create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
                             &sampler_heap);
        if (SUCCEEDED(hr))
            hr = create_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &rtv_heap);
        if (SUCCEEDED(hr)) {
            UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
            for (uint32_t i = 0; i < 4; ++i) {
                device8->CreateSamplerFeedbackUnorderedAccessView(paired, feedback[i], cpu);
                cpu.ptr += stride;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 4;
            device->CreateShaderResourceView(paired, &srv, cpu);
            D3D12_SAMPLER_DESC sampler = {};
            sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            device->CreateSampler(&sampler, sampler_heap->GetCPUDescriptorHandleForHeapStart());
            D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
            rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(color_target, &rtv, rtv_heap->GetCPUDescriptorHandleForHeapStart());
        }

        if (SUCCEEDED(hr)) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = root;
            desc.VS = {vs.data(), vs.size()};
            D3D12_INPUT_ELEMENT_DESC input_elements[2] = {};
            input_elements[0] = {
                "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            input_elements[1] = {
                "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
            desc.InputLayout = {input_elements, 2};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT32_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = TRUE;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            pso_ok = true;
            for (uint32_t i = 0; i < 4 && SUCCEEDED(hr); ++i) {
                desc.PS = {ps[i].data(), ps[i].size()};
                hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso[i]));
                pso_ok &= SUCCEEDED(hr);
            }
        }
        if (SUCCEEDED(hr)) {
            D3D12_COMMAND_QUEUE_DESC desc = {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        }
        if (SUCCEEDED(hr))
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pso[0], IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr))
            hr = list->QueryInterface(IID_PPV_ARGS(&list1));

        if (SUCCEEDED(hr)) {
            UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_GPU_DESCRIPTOR_HANDLE gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = resource_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT clear[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
            for (uint32_t i = 0; i < 4; ++i) {
                list->ClearUnorderedAccessViewUint(gpu, cpu, feedback[i], clear, 0, nullptr);
                gpu.ptr += stride;
                cpu.ptr += stride;
            }
            ID3D12DescriptorHeap* heaps[] = {resource_heap, sampler_heap};
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRootSignature(root);
            gpu = resource_heap->GetGPUDescriptorHandleForHeapStart();
            D3D12_GPU_DESCRIPTOR_HANDLE feedback_gpu = gpu;
            gpu.ptr += 4 * stride;
            list->SetGraphicsRootDescriptorTable(1, gpu);
            list->SetGraphicsRootDescriptorTable(2, sampler_heap->GetGPUDescriptorHandleForHeapStart());
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            D3D12_VIEWPORT viewport = {0, 0, 4, 4, 0, 1};
            D3D12_RECT scissor = {0, 0, 4, 4};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vbv = {};
            vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
            vbv.SizeInBytes = 60;
            vbv.StrideInBytes = 20;
            list->IASetVertexBuffers(0, 1, &vbv);
            for (uint32_t i = 0; i < 4; ++i) {
                list->SetGraphicsRootDescriptorTable(0, feedback_gpu);
                list->SetPipelineState(pso[i]);
                list->DrawInstanced(3, 1, 0, 0);
                feedback_gpu.ptr += stride;
            }
            for (uint32_t i = 0; i < 4; ++i) {
                D3D12_RESOURCE_BARRIER barriers[2] = {};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barriers[0].UAV.pResource = feedback[i];
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = feedback[i];
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
                list->ResourceBarrier(2, barriers);
                list1->ResolveSubresourceRegion(decoded[i], 0, 0, 0, feedback[i], 0, nullptr, DXGI_FORMAT_R8_UINT,
                                                D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
                D3D12_RESOURCE_BARRIER decoded_barrier = {};
                decoded_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                decoded_barrier.Transition.pResource = decoded[i];
                decoded_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                decoded_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
                decoded_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &decoded_barrier);
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = decoded[i];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = UINT64(i) * 256;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
                dst.PlacedFootprint.Footprint.Width = 1;
                dst.PlacedFootprint.Footprint.Height = 1;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = 256;
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            D3D12_RESOURCE_BARRIER color_barrier = {};
            color_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            color_barrier.Transition.pResource = color_target;
            color_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            color_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            color_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &color_barrier);
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = color_target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 1024;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = 4;
            dst.PlacedFootprint.Footprint.Height = 4;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = 256;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            hr = execute_and_wait(device, queue, list);
        }
    }

    if (SUCCEEDED(hr) && readback) {
        uint8_t* mapped = nullptr;
        D3D12_RANGE range = {0, 2048};
        hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
        if (SUCCEEDED(hr) && mapped) {
            for (uint32_t i = 0; i < 4; ++i)
                observed[i] = mapped[i * 256];
            std::memcpy(&color, mapped + 1024, sizeof(color));
            values_ok = observed[0] == 3 && observed[1] == 2 && observed[2] == 0 && observed[3] == 2;
            color_ok = color == 0xffbf8040u;
            D3D12_RANGE written = {0, 0};
            readback->Unmap(0, &written);
        }
    }

    bool dxc_ok = vs_dxc == 0;
    for (uint32_t i = 0; i < 4; ++i)
        dxc_ok &= ps_dxc[i] == 0;
    const bool passed = SUCCEEDED(hr) && dxc_ok && pso_ok && values_ok && color_ok;
    std::printf("{\n");
    std::printf("  \"probe\": \"sampler_feedback_pixel\",\n");
    std::printf("  \"passed\": %s,\n", passed ? "true" : "false");
    std::printf("  \"runtime_hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"vs_dxc_exit_code\": %lu,\n", static_cast<unsigned long>(vs_dxc));
    std::printf("  \"ps_dxc_exit_codes\": [%lu, %lu, %lu, %lu],\n", static_cast<unsigned long>(ps_dxc[0]),
                static_cast<unsigned long>(ps_dxc[1]), static_cast<unsigned long>(ps_dxc[2]),
                static_cast<unsigned long>(ps_dxc[3]));
    std::printf("  \"graphics_pso_created\": %s,\n", pso_ok ? "true" : "false");
    std::printf("  \"implicit_bias_grad_level_ok\": %s,\n", values_ok ? "true" : "false");
    std::printf("  \"color_output_ok\": %s,\n", color_ok ? "true" : "false");
    std::printf("  \"observed_feedback\": [%u, %u, %u, %u],\n", observed[0], observed[1], observed[2], observed[3]);
    std::printf("  \"observed_color\": \"0x%08x\"\n", color);
    std::printf("}\n");

    release(readback);
    release(vertex_buffer);
    release(color_target);
    for (uint32_t i = 0; i < 4; ++i) {
        release(decoded[i]);
        release(feedback[i]);
    }
    release(paired);
    release(rtv_heap);
    release(sampler_heap);
    release(resource_heap);
    release(list1);
    release(list);
    release(allocator);
    release(queue);
    for (uint32_t i = 0; i < 4; ++i)
        release(pso[i]);
    release(root);
    release(device8);
    release(device);
    if (d3d12)
        FreeLibrary(d3d12);
    return passed ? 0 : 1;
}
