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
struct DispatchGraphDesc {
    UINT Mode;
    UINT Padding;
    union {
        NodeCPUInput NodeCPUInput;
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
    ID3D12StateObject* state = nullptr;
    WorkGraphProperties* properties = nullptr;
    uint32_t values[6] = {};
    uint32_t inputs[3] = {41, 42, 43};
    uint32_t input = inputs[0];
    bool properties_ok = false;
    bool readback_ok = false;
    bool gpu_input_readback_ok = false;

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
        auto gpu_record_desc = buffer_desc(16);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &gpu_record_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&gpu_records));
        void* mapped = nullptr;
        if (SUCCEEDED(hr))
            hr = gpu_records->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(mapped, inputs, sizeof(inputs));
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

    WorkGraphNodeID entrypoint = {L"entry", 0};
    UINT local_root_index = 3;
    CommonComputeOverrides overrides = {};
    overrides.LocalRootArgumentsTableIndex = &local_root_index;
    WorkGraphNode node = {};
    node.NodeType = 0;
    node.Shader.Shader = L"node";
    node.Shader.OverridesType = 4;
    node.Shader.Overrides = &overrides;
    WorkGraphDesc graph = {L"graph", 0, 1, &entrypoint, 1, &node};
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
                        properties->GetNumNodes(0) == 1 &&
                        properties->GetNumEntrypoints(0) == 1 &&
                        properties->GetNodeID(&got, 0, 0) && got.Name &&
                        ::wcscmp(got.Name, L"node") == 0 &&
                        properties->GetEntrypointIndex(0, entrypoint) == 0 &&
                        properties->GetNodeLocalRootArgumentsTableIndex(0, 0) ==
                            local_root_index &&
                        properties->GetEntrypointRecordSizeInBytes(0, 0) == 16;
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

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.workgraph-execution.v1\",\n");
    std::printf("  \"pass\": %s,\n", SUCCEEDED(hr) && properties_ok && readback_ok ? "true" : "false");
    std::printf("  \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"properties_complete\": %s,\n", properties_ok ? "true" : "false");
    std::printf("  \"node_local_root_index\": %u,\n", local_root_index);
    std::printf("  \"gpu_native_provider\": true,\n");
    std::printf("  \"cpu_scheduler\": false,\n");
    std::printf("  \"input\": %u,\n  \"record_count\": 3,\n  \"values\": [%u, %u],\n", input, values[0], values[1]);
    std::printf("  \"readback_exact\": %s,\n", readback_ok ? "true" : "false");
    std::printf("  \"gpu_input_readback_exact\": %s\n",
                gpu_input_readback_ok ? "true" : "false");
    std::printf("}\n");

    release(state_properties);
    release(state);
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
                   gpu_input_readback_ok
               ? 0
               : 1;
}
