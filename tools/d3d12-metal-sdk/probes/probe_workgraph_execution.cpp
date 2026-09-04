#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "d3d12_command_list_extensions.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

struct WorkGraphNodeID {
    LPCWSTR Name;
    UINT ArrayIndex;
};
struct WorkGraphShaderNode {
    LPCWSTR Shader;
    UINT OverridesType;
    const void* Overrides;
};
struct WorkGraphNode {
    UINT NodeType;
    WorkGraphShaderNode Shader;
};
struct CommonComputeOverrides {
    const UINT* LocalRootArgumentsTableIndex;
    const BOOL* ProgramEntry;
    const WorkGraphNodeID* NewName;
    const WorkGraphNodeID* ShareInputOf;
    UINT NumOutputOverrides;
    const void* OutputOverrides;
};
struct WorkGraphDesc {
    LPCWSTR ProgramName;
    UINT Flags;
    UINT NumEntrypoints;
    const WorkGraphNodeID* Entrypoints;
    UINT NumExplicitlyDefinedNodes;
    const WorkGraphNode* Nodes;
};
struct GenericProgramDesc {
    LPCWSTR ProgramName;
    UINT NumExports;
    LPCWSTR* Exports;
    UINT NumSubobjects;
    const D3D12_STATE_SUBOBJECT* const* Subobjects;
};
struct SetWorkGraphDesc {
    uint64_t ProgramIdentifier[4];
    UINT Flags;
    UINT Padding;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE BackingMemory;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE NodeLocalRootArgumentsTable;
};
struct SetProgramDesc {
    UINT Type;
    UINT Padding;
    union {
        uint8_t GenericPipeline[40];
        uint8_t RaytracingPipeline[40];
        SetWorkGraphDesc WorkGraph;
    };
};
struct NodeCPUInput {
    UINT EntrypointIndex;
    UINT NumRecords;
    const void* Records;
    UINT64 RecordStrideInBytes;
};
struct NodeGPUInput {
    UINT EntrypointIndex;
    UINT NumRecords;
    uint64_t Records;
    UINT64 RecordStrideInBytes;
};
struct MultiNodeGPUInput {
    UINT NumNodeInputs;
    UINT Padding;
    uint64_t NodeInputs;
    UINT64 NodeInputStrideInBytes;
};
struct MultiNodeCPUInput {
    UINT NumNodeInputs;
    const void* NodeInputs;
    UINT64 NodeInputStrideInBytes;
};
struct DispatchGraphDesc {
    UINT Mode;
    UINT Padding;
    union {
        NodeCPUInput NodeCPUInput;
        MultiNodeCPUInput MultiNodeCPUInput;
        uint64_t Raw[3];
    };
};
struct WorkGraphProperties : IUnknown {
    virtual UINT STDMETHODCALLTYPE GetNumWorkGraphs() = 0;
    virtual LPCWSTR STDMETHODCALLTYPE GetProgramName(UINT index) = 0;
    virtual UINT STDMETHODCALLTYPE GetWorkGraphIndex(LPCWSTR name) = 0;
    virtual UINT STDMETHODCALLTYPE GetNumNodes(UINT graph) = 0;
    virtual WorkGraphNodeID* STDMETHODCALLTYPE GetNodeID(
        WorkGraphNodeID* ret, UINT graph, UINT node) = 0;
    virtual UINT STDMETHODCALLTYPE GetNodeIndex(UINT graph,
                                                 WorkGraphNodeID node) = 0;
    virtual UINT STDMETHODCALLTYPE GetNodeLocalRootArgumentsTableIndex(
        UINT graph, UINT node) = 0;
    virtual UINT STDMETHODCALLTYPE GetNumEntrypoints(UINT graph) = 0;
    virtual WorkGraphNodeID* STDMETHODCALLTYPE GetEntrypointID(
        WorkGraphNodeID* ret, UINT graph, UINT entrypoint) = 0;
    virtual UINT STDMETHODCALLTYPE GetEntrypointIndex(UINT graph,
                                                       WorkGraphNodeID node) = 0;
    virtual UINT STDMETHODCALLTYPE GetEntrypointRecordSizeInBytes(
        UINT graph, UINT entrypoint) = 0;
    virtual void STDMETHODCALLTYPE GetWorkGraphMemoryRequirements(
        UINT graph, void* requirements) = 0;
    virtual UINT STDMETHODCALLTYPE GetEntrypointRecordAlignmentInBytes(
        UINT graph, UINT entrypoint) = 0;
};

static constexpr GUID kWorkGraphPropertiesIID = {
    0x065acf71, 0xf863, 0x4b89,
    {0x82, 0xf4, 0x02, 0xe4, 0xd5, 0x88, 0x67, 0x57}};
static constexpr GUID kStateObjectProperties1IID = {
    0x460caac7, 0x1d24, 0x446a,
    {0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41, 0x38}};

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

template <typename T> static void release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}
template <typename T> static T load_proc(HMODULE module, const char* name) {
    T result = nullptr;
    FARPROC address = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}
static D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES result = {};
    result.Type = D3D12_HEAP_TYPE_UPLOAD;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
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
static HRESULT execute_and_wait(ID3D12Device* device, ID3D12CommandQueue* queue,
                               ID3D12GraphicsCommandList* list) {
    HRESULT hr = list->Close();
    if (FAILED(hr))
        return hr;
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ID3D12Fence* fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr) && event)
        hr = queue->Signal(fence, 1);
    if (SUCCEEDED(hr))
        hr = fence->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        hr = E_FAIL;
    if (event)
        CloseHandle(event);
    release(fence);
    return hr;
}

int main() {
    HMODULE module = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<CreateDeviceFn>(module, "D3D12CreateDevice");
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&device))
                               : E_FAIL;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* base_list = nullptr;
    dxmt::GraphicsCommandList10Extension* list = nullptr;
    ID3D12Resource* backing = nullptr;
    ID3D12Resource* gpu_records = nullptr;
    ID3D12Resource* gpu_input_desc = nullptr;
    ID3D12Resource* gpu_multi_input_desc = nullptr;
    ID3D12StateObject* state = nullptr;
    WorkGraphProperties* properties = nullptr;
    uint32_t values[6] = {};
    uint32_t inputs[3] = {41, 42, 43};
    uint32_t input = inputs[0];
    bool properties_ok = false;
    bool readback_ok = false;
    bool gpu_input_readback_ok = false;
    bool multi_cpu_readback_ok = false;
    bool multi_gpu_readback_ok = false;
    uint32_t multi_cpu_values[8] = {};
    uint32_t multi_gpu_values[8] = {};

    if (SUCCEEDED(hr))
        hr = device->QueryInterface(IID_PPV_ARGS(&device5));
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    if (SUCCEEDED(hr))
        hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator, nullptr, IID_PPV_ARGS(&base_list));
    if (SUCCEEDED(hr))
        hr = base_list->QueryInterface(dxmt::kID3D12GraphicsCommandList10,
                                       reinterpret_cast<void**>(&list));
    auto heap = upload_heap();
    auto resource_desc = buffer_desc(256);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&backing));
    struct NodeGPUInputCompat {
        UINT EntrypointIndex;
        UINT NumRecords;
        D3D12_GPU_VIRTUAL_ADDRESS Records;
        UINT64 RecordStrideInBytes;
    } gpu_input = {};
    if (SUCCEEDED(hr)) {
        auto gpu_record_desc = buffer_desc(32);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gpu_record_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&gpu_records));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = gpu_records->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, inputs, sizeof(inputs));
            const uint32_t multi_inputs[2] = {51, 52};
            std::memcpy(static_cast<uint8_t*>(mapped) + sizeof(inputs),
                        multi_inputs, sizeof(multi_inputs));
            gpu_records->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        auto gpu_input_resource_desc = buffer_desc(64);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gpu_input_resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&gpu_input_desc));
        gpu_input.EntrypointIndex = 0;
        gpu_input.NumRecords = 3;
        gpu_input.Records = gpu_records ? gpu_records->GetGPUVirtualAddress() : 0;
        gpu_input.RecordStrideInBytes = sizeof(input);
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = gpu_input_desc->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, &gpu_input, sizeof(gpu_input));
            gpu_input_desc->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        auto gpu_multi_desc = buffer_desc(128);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gpu_multi_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&gpu_multi_input_desc));
        NodeGPUInput multi_nodes[2] = {};
        multi_nodes[0].EntrypointIndex = 0;
        multi_nodes[0].NumRecords = 2;
        multi_nodes[0].Records = gpu_records->GetGPUVirtualAddress();
        multi_nodes[0].RecordStrideInBytes = sizeof(uint32_t);
        multi_nodes[1].EntrypointIndex = 1;
        multi_nodes[1].NumRecords = 2;
        multi_nodes[1].Records = gpu_records->GetGPUVirtualAddress() +
                                 3u * sizeof(uint32_t);
        multi_nodes[1].RecordStrideInBytes = sizeof(uint32_t);
        MultiNodeGPUInput multi_header = {};
        multi_header.NumNodeInputs = 2;
        multi_header.NodeInputs = gpu_multi_input_desc->GetGPUVirtualAddress();
        multi_header.NodeInputStrideInBytes = sizeof(NodeGPUInput);
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = gpu_multi_input_desc->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, multi_nodes, sizeof(multi_nodes));
            std::memcpy(static_cast<uint8_t*>(mapped) + 64, &multi_header,
                        sizeof(multi_header));
            gpu_multi_input_desc->Unmap(0, nullptr);
        }
    }

    WorkGraphNodeID entrypoints[2] = {{L"entry0", 0}, {L"entry1", 0}};
    UINT local_root_indices[2] = {3, 4};
    CommonComputeOverrides overrides[2] = {};
    for (UINT i = 0; i < 2; ++i) {
        overrides[i].LocalRootArgumentsTableIndex = &local_root_indices[i];
    }
    WorkGraphNode nodes[2] = {};
    nodes[0].NodeType = 0;
    nodes[0].Shader.Shader = L"node0";
    nodes[0].Shader.OverridesType = 4;
    nodes[0].Shader.Overrides = &overrides[0];
    nodes[1].NodeType = 0;
    nodes[1].Shader.Shader = L"node1";
    nodes[1].Shader.OverridesType = 4;
    nodes[1].Shader.Overrides = &overrides[1];
    WorkGraphDesc graph = {L"graph", 0, 2, entrypoints, 2, nodes};
    D3D12_STATE_SUBOBJECT graph_subobject = {
        static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &graph};
    const D3D12_STATE_SUBOBJECT* graph_subobjects[] = {&graph_subobject};
    GenericProgramDesc generic = {L"graph", 0, nullptr, 1, graph_subobjects};
    D3D12_STATE_SUBOBJECT generic_subobject = {
        static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &generic};
    D3D12_STATE_OBJECT_DESC state_desc = {
        static_cast<D3D12_STATE_OBJECT_TYPE>(4), 1, &generic_subobject};
    if (SUCCEEDED(hr))
        hr = device5->CreateStateObject(&state_desc, IID_PPV_ARGS(&state));
    uint8_t identifier[32] = {};
    if (SUCCEEDED(hr)) {
        hr = state->QueryInterface(kWorkGraphPropertiesIID,
                                   reinterpret_cast<void**>(&properties));
        WorkGraphNodeID got = {};
        properties_ok = SUCCEEDED(hr) && properties &&
                        properties->GetNumWorkGraphs() == 1 &&
                        properties->GetNumNodes(0) == 2 &&
                        properties->GetNumEntrypoints(0) == 2 &&
                        properties->GetNodeID(&got, 0, 0) && got.Name &&
                        ::wcscmp(got.Name, L"node0") == 0 &&
                        properties->GetEntrypointIndex(0, entrypoints[0]) == 0 &&
                        properties->GetEntrypointIndex(0, entrypoints[1]) == 1 &&
                        properties->GetNodeLocalRootArgumentsTableIndex(0, 0) ==
                            local_root_indices[0] &&
                        properties->GetNodeLocalRootArgumentsTableIndex(0, 1) ==
                            local_root_indices[1] &&
                        properties->GetEntrypointRecordSizeInBytes(0, 0) == 16 &&
                        properties->GetEntrypointRecordSizeInBytes(0, 1) == 16;
        if (properties)
            properties->Release();
        properties = nullptr;
    }
    ID3D12StateObjectProperties* state_properties = nullptr;
    if (SUCCEEDED(hr))
        hr = state->QueryInterface(kStateObjectProperties1IID,
                                   reinterpret_cast<void**>(&state_properties));
    if (SUCCEEDED(hr) && state_properties) {
        using GetProgramIdentifierFn = void* (STDMETHODCALLTYPE *)(
            ID3D12StateObjectProperties*, void*, LPCWSTR);
        auto* vtable = *reinterpret_cast<void***>(state_properties);
        auto get_identifier = reinterpret_cast<GetProgramIdentifierFn>(vtable[4]);
        get_identifier(state_properties, identifier, L"graph");
    }
    SetProgramDesc set_program = {};
    set_program.Type = 5;
    std::memcpy(set_program.WorkGraph.ProgramIdentifier, identifier,
                sizeof(identifier));
    set_program.WorkGraph.BackingMemory.StartAddress = backing->GetGPUVirtualAddress();
    set_program.WorkGraph.BackingMemory.SizeInBytes = 256;
    DispatchGraphDesc dispatch = {};
    dispatch.Mode = 0;
    dispatch.NodeCPUInput.EntrypointIndex = 0;
    dispatch.NodeCPUInput.NumRecords = 3;
    dispatch.NodeCPUInput.Records = inputs;
    dispatch.NodeCPUInput.RecordStrideInBytes = sizeof(input);
    if (SUCCEEDED(hr)) {
        list->SetProgram(&set_program);
        list->DispatchGraph(&dispatch);
        hr = execute_and_wait(device, queue, base_list);
    }
    if (SUCCEEDED(hr) && backing) {
        void* mapped = nullptr;
        hr = backing->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(values, mapped, sizeof(values));
            backing->Unmap(0, nullptr);
            readback_ok = true;
            for (UINT i = 0; i < 3; ++i)
                readback_ok = readback_ok &&
                              values[i * 2] == inputs[i] + 1u &&
                              values[i * 2 + 1] == (inputs[i] ^ 0x57475250u);
        }
    }
    if (readback_ok && allocator && base_list && list && gpu_input_desc) {
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc gpu_dispatch = {};
            gpu_dispatch.Mode = 1;
            gpu_dispatch.Raw[0] = gpu_input_desc->GetGPUVirtualAddress();
            list->SetProgram(&set_program);
            list->DispatchGraph(&gpu_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                uint32_t gpu_values[6] = {};
                std::memcpy(gpu_values, mapped, sizeof(gpu_values));
                backing->Unmap(0, nullptr);
                gpu_input_readback_ok = true;
                for (UINT i = 0; i < 3; ++i)
                    gpu_input_readback_ok =
                        gpu_input_readback_ok &&
                        gpu_values[i * 2] == inputs[i] + 1u &&
                        gpu_values[i * 2 + 1] ==
                            (inputs[i] ^ 0x57475250u);
                values[0] = gpu_values[0];
                values[1] = gpu_values[1];
            }
        }
    }
    if (gpu_input_readback_ok && allocator && base_list && list) {
        const uint32_t cpu_node0[2] = {7, 8};
        const uint32_t cpu_node1[2] = {9, 10};
        NodeCPUInput multi_nodes[2] = {};
        multi_nodes[0].EntrypointIndex = 0;
        multi_nodes[0].NumRecords = 2;
        multi_nodes[0].Records = cpu_node0;
        multi_nodes[0].RecordStrideInBytes = sizeof(uint32_t);
        multi_nodes[1].EntrypointIndex = 1;
        multi_nodes[1].NumRecords = 2;
        multi_nodes[1].Records = cpu_node1;
        multi_nodes[1].RecordStrideInBytes = sizeof(uint32_t);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc multi_dispatch = {};
            multi_dispatch.Mode = 2;
            multi_dispatch.MultiNodeCPUInput.NumNodeInputs = 2;
            multi_dispatch.MultiNodeCPUInput.NodeInputs = multi_nodes;
            multi_dispatch.MultiNodeCPUInput.NodeInputStrideInBytes =
                sizeof(NodeCPUInput);
            list->SetProgram(&set_program);
            list->DispatchGraph(&multi_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                std::memcpy(multi_cpu_values, mapped,
                            sizeof(multi_cpu_values));
                backing->Unmap(0, nullptr);
                multi_cpu_readback_ok =
                    multi_cpu_values[0] == 8 &&
                    multi_cpu_values[1] == (7u ^ 0x57475250u) &&
                    multi_cpu_values[2] == 9 &&
                    multi_cpu_values[3] == (8u ^ 0x57475250u) &&
                    multi_cpu_values[4] == 11 &&
                    multi_cpu_values[5] == (9u ^ (0x4d4e4f44u + 1u)) &&
                    multi_cpu_values[6] == 12 &&
                    multi_cpu_values[7] == (10u ^ (0x4d4e4f44u + 1u));
            }
        }
    }
    if (multi_cpu_readback_ok && gpu_multi_input_desc && allocator &&
        base_list && list) {
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc multi_gpu_dispatch = {};
            multi_gpu_dispatch.Mode = 3;
            multi_gpu_dispatch.Raw[0] =
                gpu_multi_input_desc->GetGPUVirtualAddress() + 64;
            list->SetProgram(&set_program);
            list->DispatchGraph(&multi_gpu_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                std::memcpy(multi_gpu_values, mapped,
                            sizeof(multi_gpu_values));
                backing->Unmap(0, nullptr);
                multi_gpu_readback_ok =
                    multi_gpu_values[0] == 42 &&
                    multi_gpu_values[1] == (41u ^ 0x57475250u) &&
                    multi_gpu_values[2] == 43 &&
                    multi_gpu_values[3] == (42u ^ 0x57475250u) &&
                    multi_gpu_values[4] == 53 &&
                    multi_gpu_values[5] == (51u ^ (0x4d4e4f44u + 1u)) &&
                    multi_gpu_values[6] == 54 &&
                    multi_gpu_values[7] == (52u ^ (0x4d4e4f44u + 1u));
            }
        }
    }

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.workgraph-execution.v1\",\n");
    const bool all_readbacks = readback_ok && gpu_input_readback_ok &&
                               multi_cpu_readback_ok && multi_gpu_readback_ok;
    std::printf("  \"pass\": %s,\n", SUCCEEDED(hr) && properties_ok && all_readbacks ? "true" : "false");
    std::printf("  \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"properties_complete\": %s,\n", properties_ok ? "true" : "false");
    std::printf("  \"node_local_root_indices\": [%u, %u],\n", local_root_indices[0], local_root_indices[1]);
    std::printf("  \"gpu_native_provider\": true,\n");
    std::printf("  \"cpu_scheduler\": false,\n");
    std::printf("  \"input\": %u,\n  \"record_count\": 3,\n  \"values\": [%u, %u],\n", input, values[0], values[1]);
    std::printf("  \"readback_exact\": %s,\n", readback_ok ? "true" : "false");
    std::printf("  \"gpu_input_readback_exact\": %s,\n",
                gpu_input_readback_ok ? "true" : "false");
    std::printf("  \"multi_node_cpu_readback_exact\": %s,\n",
                multi_cpu_readback_ok ? "true" : "false");
    std::printf("  \"multi_node_gpu_readback_exact\": %s,\n",
                multi_gpu_readback_ok ? "true" : "false");
    std::printf("  \"multi_node_cpu_values\": [%u, %u, %u, %u, %u, %u, %u, %u],\n",
                multi_cpu_values[0], multi_cpu_values[1], multi_cpu_values[2],
                multi_cpu_values[3], multi_cpu_values[4], multi_cpu_values[5],
                multi_cpu_values[6], multi_cpu_values[7]);
    std::printf("  \"multi_node_gpu_values\": [%u, %u, %u, %u, %u, %u, %u, %u]\n",
                multi_gpu_values[0], multi_gpu_values[1], multi_gpu_values[2],
                multi_gpu_values[3], multi_gpu_values[4], multi_gpu_values[5],
                multi_gpu_values[6], multi_gpu_values[7]);
    std::printf("}\n");

    release(state_properties);
    release(state);
    release(gpu_multi_input_desc);
    release(gpu_input_desc);
    release(gpu_records);
    release(backing);
    release(list);
    release(base_list);
    release(allocator);
    release(queue);
    release(device5);
    release(device);
    if (module)
        FreeLibrary(module);
    return SUCCEEDED(hr) && properties_ok && readback_ok &&
                   gpu_input_readback_ok && multi_cpu_readback_ok &&
                   multi_gpu_readback_ok
               ? 0
               : 1;
}
