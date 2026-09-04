#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>

#include "d3d12_command_list_extensions.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

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
using SerializeRootSignatureFn = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
    ID3DBlob**);

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
static bool read_binary_file(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    return !bytes.empty();
}
static D3D12_HEAP_PROPERTIES upload_heap() {
    D3D12_HEAP_PROPERTIES result = {};
    result.Type = D3D12_HEAP_TYPE_UPLOAD;
    result.CreationNodeMask = 1;
    result.VisibleNodeMask = 1;
    return result;
}
static D3D12_RESOURCE_DESC buffer_desc(
    UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC result = {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result.Flags = flags;
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
    auto serialize_root_signature =
        load_proc<SerializeRootSignatureFn>(module,
                                            "D3D12SerializeRootSignature");
    ID3D12Device* device = nullptr;
    HRESULT hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&device))
                               : E_FAIL;
    ID3D12Device5* device5 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* compute_queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12CommandAllocator* compute_allocator = nullptr;
    ID3D12GraphicsCommandList* base_list = nullptr;
    ID3D12GraphicsCommandList* compute_base_list = nullptr;
    dxmt::GraphicsCommandList10Extension* list = nullptr;
    dxmt::GraphicsCommandList10Extension* compute_list = nullptr;
    ID3D12Resource* backing = nullptr;
    ID3D12Resource* node_output = nullptr;
    ID3D12RootSignature* node_root = nullptr;
    ID3DBlob* node_root_blob = nullptr;
    ID3D12Resource* node_local_table = nullptr;
    ID3D12Resource* gpu_records = nullptr;
    ID3D12Resource* gpu_input_desc = nullptr;
    ID3D12Resource* gpu_multi_input_desc = nullptr;
    ID3D12StateObject* state = nullptr;
    ID3D12StateObject* include_all_state = nullptr;
    ID3D12StateObject* node_shader_state = nullptr;
    ID3D12StateObject* node_multi_state = nullptr;
    ID3D12StateObjectProperties* node_shader_state_properties = nullptr;
    ID3D12StateObjectProperties* node_multi_state_properties = nullptr;
    WorkGraphProperties* properties = nullptr;
    WorkGraphProperties* include_all_properties = nullptr;
    WorkGraphProperties* node_multi_properties = nullptr;
    uint32_t values[6] = {};
    uint32_t inputs[3] = {41, 42, 43};
    uint32_t input = inputs[0];
    bool properties_ok = false;
    bool include_all_properties_ok = false;
    bool readback_ok = false;
    bool gpu_input_readback_ok = false;
    bool multi_cpu_readback_ok = false;
    bool multi_cpu_pointer_free = false;
    bool multi_gpu_readback_ok = false;
    bool multi_node_negative_unchanged = false;
    bool work_graph_initialize_exact = false;
    bool node_local_table_validation_exact = false;
    bool backing_overflow_unchanged = false;
    bool multi_dispatch_ordering_exact = false;
    bool cross_queue_dispatch_exact = false;
    bool cross_queue_gpu_dependency_exact = false;
    bool cross_queue_repeated_gpu_dependency_exact = false;
    bool cross_queue_multi_gpu_dependency_exact = false;
    uint32_t cross_queue_values[2] = {};
    uint32_t cross_queue_repeated_values[2] = {};
    uint32_t cross_queue_multi_values[2] = {};
    bool dxil_node_shader_readback_exact = false;
    bool dxil_node_shader_uav_binding_exact = false;
    bool node_table_uav_exact = false;
    bool node_table_short_view_unchanged = false;
    bool node_table_null_view_unchanged = false;
    bool dxil_node_shader_gpu_readback_exact = false;
    bool dxil_multi_node_readback_exact = false;
    bool node_multi_cpu_input_exact = false;
    bool node_multi_gpu_input_exact = false;
    bool node_multi_properties_complete = false;
    bool node_input_layouts_exact = false;
    uint8_t input_program_identifier[32] = {};
    ID3D12StateObject* input_program_state = nullptr;
    bool input_program_ready = false;
    bool node_input_binding_exact = false;
    bool node_internal_binding_rejected = false;
    bool node_launch_geometry_exact = false;
    bool node_dynamic_grid_rejected = false;
    uint32_t node_launch_values[8] = {};
    bool node_input_gpu_dependency_exact = false;
    uint32_t node_input_gpu_dependency_values[2] = {};
    uint32_t multi_cpu_values[8] = {};
    uint32_t multi_gpu_values[8] = {};
    uint32_t node_shader_values[2] = {};
    uint32_t dxil_multi_node_values[2] = {};
    uint32_t node_multi_cpu_values[2] = {};
    uint32_t node_multi_gpu_values[2] = {};
    std::vector<uint8_t> node_shader_bytecode;
    std::vector<uint8_t> node_multi_bytecode;
    const bool node_shader_bytecode_loaded =
        read_binary_file("probe_workgraph_node.cso", node_shader_bytecode);
    const bool node_multi_bytecode_loaded =
        read_binary_file("probe_workgraph_node_multi.cso", node_multi_bytecode);

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
    if (SUCCEEDED(hr)) {
        D3D12_COMMAND_QUEUE_DESC compute_queue_desc = {};
        compute_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        hr = device->CreateCommandQueue(&compute_queue_desc,
                                         IID_PPV_ARGS(&compute_queue));
    }
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                            IID_PPV_ARGS(&compute_allocator));
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                       compute_allocator, nullptr,
                                       IID_PPV_ARGS(&compute_base_list));
    if (SUCCEEDED(hr))
        hr = compute_base_list->QueryInterface(
            dxmt::kID3D12GraphicsCommandList10,
            reinterpret_cast<void**>(&compute_list));
    auto heap = upload_heap();
    auto resource_desc = buffer_desc(256);
    if (SUCCEEDED(hr))
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&backing));
    if (SUCCEEDED(hr)) {
        auto node_table_desc = buffer_desc(64);
        hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &node_table_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&node_local_table));
    }
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES node_output_heap = {};
        node_output_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        node_output_heap.CreationNodeMask = 1;
        node_output_heap.VisibleNodeMask = 1;
        auto node_output_desc = buffer_desc(
            64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(
            &node_output_heap, D3D12_HEAP_FLAG_NONE, &node_output_desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&node_output));
    }
    if (SUCCEEDED(hr) && serialize_root_signature && device) {
        D3D12_ROOT_PARAMETER node_root_parameter = {};
        node_root_parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_UAV;
        node_root_parameter.Descriptor.ShaderRegister = 0;
        node_root_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC node_root_desc = {};
        node_root_desc.NumParameters = 1;
        node_root_desc.pParameters = &node_root_parameter;
        hr = serialize_root_signature(&node_root_desc,
                                      D3D_ROOT_SIGNATURE_VERSION_1,
                                      &node_root_blob, nullptr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(
                0, node_root_blob->GetBufferPointer(),
                node_root_blob->GetBufferSize(), IID_PPV_ARGS(&node_root));
    }
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
        NodeGPUInput node_gpu_input = {};
        node_gpu_input.EntrypointIndex = gpu_input.EntrypointIndex;
        node_gpu_input.NumRecords = 1;
        node_gpu_input.Records = gpu_input.Records;
        node_gpu_input.RecordStrideInBytes = gpu_input.RecordStrideInBytes;
        if (SUCCEEDED(hr))
            hr = gpu_input_desc->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr)) {
            std::memcpy(static_cast<uint8_t*>(mapped) + 32,
                        &node_gpu_input, sizeof(node_gpu_input));
            gpu_input_desc->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr)) {
        auto gpu_multi_desc = buffer_desc(256);
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
            NodeGPUInput single_node = multi_nodes[0];
            single_node.NumRecords = 1;
            std::memcpy(static_cast<uint8_t*>(mapped) + 128, &single_node,
                        sizeof(single_node));
            MultiNodeGPUInput single_header = {};
            single_header.NumNodeInputs = 1;
            single_header.NodeInputs =
                gpu_multi_input_desc->GetGPUVirtualAddress() + 128;
            single_header.NodeInputStrideInBytes = sizeof(NodeGPUInput);
            std::memcpy(static_cast<uint8_t*>(mapped) + 160, &single_header,
                        sizeof(single_header));
            gpu_multi_input_desc->Unmap(0, nullptr);
        }
    }

    WorkGraphNodeID entrypoints[2] = {{L"node0", 0}, {L"node1", 0}};
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
                        properties->GetEntrypointRecordSizeInBytes(0, 1) == 16 &&
                        properties->GetEntrypointRecordSizeInBytes(1, 0) == UINT_MAX &&
                        properties->GetEntrypointRecordSizeInBytes(0, 2) == UINT_MAX &&
                        properties->GetEntrypointRecordSizeInBytes(0, UINT_MAX) == UINT_MAX &&
                        properties->GetEntrypointRecordAlignmentInBytes(1, 0) == UINT_MAX &&
                        properties->GetEntrypointRecordAlignmentInBytes(0, 2) == UINT_MAX &&
                        properties->GetEntrypointRecordAlignmentInBytes(UINT_MAX, 0) == UINT_MAX;
        if (properties)
            properties->Release();
        properties = nullptr;
    }
    if (SUCCEEDED(hr)) {
        WorkGraphNodeID include_all_entry = {L"node0", 0};
        WorkGraphDesc include_all_graph = {L"include_all_graph", 1, 1,
                                           &include_all_entry, 0, nullptr};
        D3D12_STATE_SUBOBJECT include_all_graph_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &include_all_graph};
        const D3D12_STATE_SUBOBJECT* include_all_graph_subobjects[] = {
            &include_all_graph_subobject};
        GenericProgramDesc include_all_generic = {
            L"include_all_graph", 0, nullptr, 1,
            include_all_graph_subobjects};
        D3D12_STATE_SUBOBJECT include_all_generic_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29),
            &include_all_generic};
        D3D12_STATE_OBJECT_DESC include_all_state_desc = {
            static_cast<D3D12_STATE_OBJECT_TYPE>(4), 1,
            &include_all_generic_subobject};
        HRESULT include_all_hr = device5->CreateStateObject(
            &include_all_state_desc, IID_PPV_ARGS(&include_all_state));
        if (SUCCEEDED(include_all_hr) && include_all_state) {
            include_all_hr = include_all_state->QueryInterface(
                kWorkGraphPropertiesIID,
                reinterpret_cast<void**>(&include_all_properties));
            WorkGraphNodeID include_all_got = {};
            include_all_properties_ok =
                SUCCEEDED(include_all_hr) && include_all_properties &&
                include_all_properties->GetNumWorkGraphs() == 1 &&
                include_all_properties->GetNumNodes(0) == 1 &&
                include_all_properties->GetNumEntrypoints(0) == 1 &&
                include_all_properties->GetNodeID(&include_all_got, 0, 0) &&
                include_all_got.Name &&
                ::wcscmp(include_all_got.Name, L"node0") == 0 &&
                include_all_properties->GetNodeLocalRootArgumentsTableIndex(
                    0, 0) == UINT_MAX;
        }
        release(include_all_properties);
        include_all_properties = nullptr;
    }
    ID3D12StateObjectProperties* state_properties = nullptr;
    if (SUCCEEDED(hr))
        hr = state->QueryInterface(kStateObjectProperties1IID,
                                   reinterpret_cast<void**>(&state_properties));
    if (SUCCEEDED(hr) && state_properties) {
        using GetProgramIdentifierFn = void* (STDMETHODCALLTYPE *)(
            ID3D12StateObjectProperties*, void*, LPCWSTR);
        auto* vtable = *reinterpret_cast<void***>(state_properties);
        auto get_identifier = reinterpret_cast<GetProgramIdentifierFn>(vtable[7]);
        get_identifier(state_properties, identifier, L"graph");
    }
    // Deliberately reorder entrypoints relative to nodes: property lookup
    // must resolve node identity, not use the entrypoint as a node-array index.
    std::vector<uint8_t> layout_bytecode;
    if (SUCCEEDED(hr) && read_binary_file("probe_workgraph_node_layout.cso", layout_bytecode)) {
        const wchar_t* names[4] = {L"node_main", L"node_vector", L"node_half", L"node_empty"};
        D3D12_EXPORT_DESC exports[4] = {};
        WorkGraphNode layout_nodes[4] = {};
        for (UINT i = 0; i < 4; ++i) {
            exports[i].Name = names[i];
            layout_nodes[i] = {0, {names[i], 0, nullptr}};
        }
        WorkGraphNodeID entries[4] = {{names[1], 0}, {names[3], 0}, {names[0], 0}, {names[2], 0}};
        WorkGraphDesc graph = {L"layout_graph", 0, 4, entries, 4, layout_nodes};
        D3D12_STATE_SUBOBJECT graph_sub = {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &graph};
        const D3D12_STATE_SUBOBJECT* graph_subs[] = {&graph_sub};
        GenericProgramDesc generic = {L"layout_graph", 0, nullptr, 1, graph_subs};
        D3D12_DXIL_LIBRARY_DESC library = {};
        library.DXILLibrary = {layout_bytecode.data(), layout_bytecode.size()};
        library.NumExports = 4;
        library.pExports = exports;
        D3D12_STATE_SUBOBJECT subs[2] = {
            {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
            {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &generic}};
        D3D12_STATE_OBJECT_DESC desc = {static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2, subs};
        ID3D12StateObject* layout_state = nullptr;
        WorkGraphProperties* layout_properties = nullptr;
        HRESULT layout_hr = device5->CreateStateObject(&desc, IID_PPV_ARGS(&layout_state));
        if (SUCCEEDED(layout_hr))
            layout_hr = layout_state->QueryInterface(kWorkGraphPropertiesIID,
                reinterpret_cast<void**>(&layout_properties));
        if (SUCCEEDED(layout_hr) && layout_properties) {
            const UINT sizes[4] = {16, 0, 4, 4};
            const UINT alignments[4] = {4, 0, 4, 4};
            node_input_layouts_exact = true;
            for (UINT i = 0; i < 4; ++i)
                node_input_layouts_exact &=
                    layout_properties->GetEntrypointRecordSizeInBytes(0, i) == sizes[i] &&
                    layout_properties->GetEntrypointRecordAlignmentInBytes(0, i) == alignments[i];
            node_input_layouts_exact &=
                layout_properties->GetEntrypointRecordSizeInBytes(0, 4) == UINT_MAX &&
                layout_properties->GetEntrypointRecordAlignmentInBytes(1, 0) == UINT_MAX;
        }
        ID3D12StateObjectProperties* program_properties = nullptr;
        if (layout_state && SUCCEEDED(layout_state->QueryInterface(kStateObjectProperties1IID,
                reinterpret_cast<void**>(&program_properties)))) {
            using GetIdentifier = void* (STDMETHODCALLTYPE *)(ID3D12StateObjectProperties*, void*, LPCWSTR);
            auto* vtable = *reinterpret_cast<void***>(program_properties);
            reinterpret_cast<GetIdentifier>(vtable[7])(program_properties, input_program_identifier, L"layout_graph");
            input_program_ready = true;
        }
        release(program_properties);
        release(layout_properties);
        input_program_state = layout_state;
        D3D12_EXPORT_DESC dynamic_export = {};
        dynamic_export.Name = L"node_dynamic";
        WorkGraphNodeID dynamic_entry = {L"node_dynamic", 0};
        WorkGraphNode dynamic_node = {0, {L"node_dynamic", 0, nullptr}};
        library.NumExports = 1;
        library.pExports = &dynamic_export;
        graph.NumEntrypoints = 1;
        graph.Entrypoints = &dynamic_entry;
        graph.NumExplicitlyDefinedNodes = 1;
        graph.Nodes = &dynamic_node;
        ID3D12StateObject* rejected = reinterpret_cast<ID3D12StateObject*>(uintptr_t(1));
        const HRESULT rejected_hr = device5->CreateStateObject(&desc, IID_PPV_ARGS(&rejected));
        node_dynamic_grid_rejected = rejected_hr == E_FAIL && rejected == nullptr;
        if (rejected && rejected != reinterpret_cast<ID3D12StateObject*>(uintptr_t(1))) release(rejected);
    }
    uint8_t node_shader_identifier[32] = {};
    HRESULT node_shader_hr = E_FAIL;
    if (SUCCEEDED(hr) && node_shader_bytecode_loaded) {
        D3D12_EXPORT_DESC node_export = {};
        node_export.Name = L"node_main";
        D3D12_DXIL_LIBRARY_DESC node_library = {};
        node_library.DXILLibrary.pShaderBytecode = node_shader_bytecode.data();
        node_library.DXILLibrary.BytecodeLength = node_shader_bytecode.size();
        node_library.NumExports = 1;
        node_library.pExports = &node_export;
        WorkGraphNodeID node_entrypoint = {L"node_main", 0};
        WorkGraphShaderNode node_shader = {L"node_main", 0, nullptr};
        WorkGraphNode node_node = {0, node_shader};
        WorkGraphDesc node_graph = {L"actual_graph", 0, 1,
                                    &node_entrypoint, 1, &node_node};
        D3D12_STATE_SUBOBJECT node_graph_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &node_graph};
        const D3D12_STATE_SUBOBJECT* node_graph_subobjects[] = {
            &node_graph_subobject};
        GenericProgramDesc node_generic = {L"actual_graph", 0, nullptr, 1,
                                           node_graph_subobjects};
        D3D12_STATE_SUBOBJECT node_generic_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &node_generic};
        D3D12_STATE_SUBOBJECT node_state_subobjects[2] = {};
        node_state_subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        node_state_subobjects[0].pDesc = &node_library;
        node_state_subobjects[1] = node_generic_subobject;
        D3D12_STATE_OBJECT_DESC node_state_desc = {
            static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2,
            node_state_subobjects};
        node_shader_hr = device5->CreateStateObject(
            &node_state_desc, IID_PPV_ARGS(&node_shader_state));
        if (SUCCEEDED(node_shader_hr) && node_shader_state) {
            node_shader_hr = node_shader_state->QueryInterface(
                kStateObjectProperties1IID,
                reinterpret_cast<void**>(&node_shader_state_properties));
            if (SUCCEEDED(node_shader_hr) && node_shader_state_properties) {
                using GetProgramIdentifierFn = void* (STDMETHODCALLTYPE *)(
                    ID3D12StateObjectProperties*, void*, LPCWSTR);
                auto* vtable = *reinterpret_cast<void***>(
                    node_shader_state_properties);
                auto get_identifier = reinterpret_cast<GetProgramIdentifierFn>(
                    vtable[7]);
                get_identifier(node_shader_state_properties,
                               node_shader_identifier, L"actual_graph");
                std::vector<uint8_t> collision_bytecode;
                if (read_binary_file("probe_workgraph_node_collision.cso", collision_bytecode)) {
                    node_library.DXILLibrary = {collision_bytecode.data(), collision_bytecode.size()};
                    ID3D12StateObject* rejected = reinterpret_cast<ID3D12StateObject*>(uintptr_t(1));
                    const HRESULT rejected_hr = device5->CreateStateObject(&node_state_desc, IID_PPV_ARGS(&rejected));
                    node_internal_binding_rejected = rejected_hr == E_FAIL && rejected == nullptr;
                    if (rejected && rejected != reinterpret_cast<ID3D12StateObject*>(uintptr_t(1))) release(rejected);
                }
            }
        }
    }

    uint8_t node_multi_identifier[32] = {};
    HRESULT node_multi_hr = E_FAIL;
    if (SUCCEEDED(hr) && node_multi_bytecode_loaded) {
        D3D12_EXPORT_DESC node_exports[3] = {};
        node_exports[0].Name = L"node_a";
        node_exports[1].Name = L"node_b";
        node_exports[2].Name = L"node_c";
        D3D12_DXIL_LIBRARY_DESC node_library = {};
        node_library.DXILLibrary.pShaderBytecode = node_multi_bytecode.data();
        node_library.DXILLibrary.BytecodeLength = node_multi_bytecode.size();
        node_library.NumExports = 3;
        node_library.pExports = node_exports;
        WorkGraphNodeID node_entrypoints[3] = {{L"node_b", 0},
                                               {L"node_c", 0},
                                               {L"node_a", 0}};
        WorkGraphNode node_nodes[3] = {};
        node_nodes[0].NodeType = 0;
        node_nodes[0].Shader.Shader = L"node_a";
        node_nodes[1].NodeType = 0;
        node_nodes[1].Shader.Shader = L"node_b";
        node_nodes[2].NodeType = 0;
        node_nodes[2].Shader.Shader = L"node_c";
        WorkGraphDesc node_graph = {L"actual_multi_graph", 0, 3,
                                    node_entrypoints, 3, node_nodes};
        D3D12_STATE_SUBOBJECT node_graph_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &node_graph};
        const D3D12_STATE_SUBOBJECT* node_graph_subobjects[] = {
            &node_graph_subobject};
        GenericProgramDesc node_generic = {L"actual_multi_graph", 0, nullptr,
                                           1, node_graph_subobjects};
        D3D12_STATE_SUBOBJECT node_generic_subobject = {
            static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &node_generic};
        D3D12_STATE_SUBOBJECT node_state_subobjects[2] = {};
        node_state_subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        node_state_subobjects[0].pDesc = &node_library;
        node_state_subobjects[1] = node_generic_subobject;
        D3D12_STATE_OBJECT_DESC node_state_desc = {
            static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2,
            node_state_subobjects};
        node_multi_hr = device5->CreateStateObject(
            &node_state_desc, IID_PPV_ARGS(&node_multi_state));
        if (SUCCEEDED(node_multi_hr) && node_multi_state) {
            HRESULT node_multi_properties_hr =
                node_multi_state->QueryInterface(
                    kWorkGraphPropertiesIID,
                    reinterpret_cast<void**>(&node_multi_properties));
            WorkGraphNodeID node_multi_got = {};
            WorkGraphNodeID node_multi_entry = {};
            node_multi_entry.Name = L"node_b";
            node_multi_properties_complete =
                SUCCEEDED(node_multi_properties_hr) &&
                node_multi_properties &&
                node_multi_properties->GetProgramName(0) &&
                ::wcscmp(node_multi_properties->GetProgramName(0),
                         L"actual_multi_graph") == 0 &&
                node_multi_properties->GetNumNodes(0) == 3 &&
                node_multi_properties->GetNumEntrypoints(0) == 3 &&
                node_multi_properties->GetNodeID(&node_multi_got, 0, 2) &&
                node_multi_got.Name &&
                ::wcscmp(node_multi_got.Name, L"node_c") == 0 &&
                node_multi_properties->GetNodeIndex(0, node_multi_entry) == 1 &&
                node_multi_properties->GetEntrypointIndex(0,
                                                           node_multi_entry) == 0 &&
                node_multi_properties->GetEntrypointRecordSizeInBytes(0, 2) ==
                    0 &&
                node_multi_properties->GetEntrypointRecordAlignmentInBytes(0,
                                                                              2) ==
                    0;
            release(node_multi_properties);
            node_multi_properties = nullptr;
            node_multi_hr = node_multi_state->QueryInterface(
                kStateObjectProperties1IID,
                reinterpret_cast<void**>(&node_multi_state_properties));
            if (SUCCEEDED(node_multi_hr) && node_multi_state_properties) {
                using GetProgramIdentifierFn = void* (STDMETHODCALLTYPE *)(
                    ID3D12StateObjectProperties*, void*, LPCWSTR);
                auto* vtable = *reinterpret_cast<void***>(
                    node_multi_state_properties);
                auto get_identifier = reinterpret_cast<GetProgramIdentifierFn>(
                    vtable[7]);
                get_identifier(node_multi_state_properties,
                               node_multi_identifier,
                               L"actual_multi_graph");
            }
        }
    }

    SetProgramDesc set_program = {};
    set_program.Type = 5;
    std::memcpy(set_program.WorkGraph.ProgramIdentifier, identifier,
                sizeof(identifier));
    set_program.WorkGraph.BackingMemory.StartAddress = backing->GetGPUVirtualAddress();
    set_program.WorkGraph.BackingMemory.SizeInBytes = 256;
    set_program.WorkGraph.NodeLocalRootArgumentsTable.StartAddress =
        node_local_table ? node_local_table->GetGPUVirtualAddress() : 0;
    set_program.WorkGraph.NodeLocalRootArgumentsTable.SizeInBytes =
        node_local_table ? 64 : 0;
    set_program.WorkGraph.NodeLocalRootArgumentsTable.StrideInBytes =
        node_local_table ? 32 : 0;
    uint32_t initialize_sentinel[64] = {};
    std::fill(std::begin(initialize_sentinel), std::end(initialize_sentinel),
              0xdeadbeefu);
    if (SUCCEEDED(hr) && backing) {
        void* mapped = nullptr;
        hr = backing->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, initialize_sentinel,
                        sizeof(initialize_sentinel));
            backing->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr) && allocator && base_list && list) {
        SetProgramDesc initialize_program = set_program;
        initialize_program.WorkGraph.Flags = 1;
        list->SetProgram(&initialize_program);
        hr = execute_and_wait(device, queue, base_list);
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                work_graph_initialize_exact = true;
                for (UINT i = 0; i < 64; ++i)
                    work_graph_initialize_exact =
                        work_graph_initialize_exact &&
                        static_cast<const uint32_t*>(mapped)[i] == 0;
                backing->Unmap(0, nullptr);
            }
        }
    }
    hr = allocator->Reset();
    if (SUCCEEDED(hr))
        hr = base_list->Reset(allocator, nullptr);
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
        uint32_t cpu_node0[2] = {7, 8};
        uint32_t cpu_node1[2] = {9, 10};
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
            // The command list must own the nested records.  Mutating the
            // caller's arrays after recording makes a stale-pointer replay
            // observable without involving a CPU scheduler.
            cpu_node0[0] = 0xdead0001u;
            cpu_node0[1] = 0xdead0002u;
            cpu_node1[0] = 0xdead0003u;
            cpu_node1[1] = 0xdead0004u;
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
                multi_cpu_pointer_free = multi_cpu_readback_ok;
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
    if (multi_gpu_readback_ok && allocator && base_list && list) {
        uint32_t invalid_record = 0xfeedfaceu;
        NodeCPUInput invalid_node = {};
        invalid_node.EntrypointIndex = 0;
        invalid_node.NumRecords = 1;
        invalid_node.Records = &invalid_record;
        invalid_node.RecordStrideInBytes = 1028;
        MultiNodeCPUInput invalid_multi = {};
        invalid_multi.NumNodeInputs = 1;
        invalid_multi.NodeInputs = &invalid_node;
        invalid_multi.NodeInputStrideInBytes = sizeof(NodeCPUInput);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc invalid_dispatch = {};
            invalid_dispatch.Mode = 2;
            invalid_dispatch.MultiNodeCPUInput.NumNodeInputs =
                invalid_multi.NumNodeInputs;
            invalid_dispatch.MultiNodeCPUInput.NodeInputs =
                invalid_multi.NodeInputs;
            invalid_dispatch.MultiNodeCPUInput.NodeInputStrideInBytes =
                invalid_multi.NodeInputStrideInBytes;
            list->SetProgram(&set_program);
            list->DispatchGraph(&invalid_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                uint32_t after_invalid[8] = {};
                std::memcpy(after_invalid, mapped, sizeof(after_invalid));
                backing->Unmap(0, nullptr);
                multi_node_negative_unchanged =
                    std::memcmp(after_invalid, multi_gpu_values,
                                sizeof(after_invalid)) == 0;
            }
        }
    }

    if (multi_node_negative_unchanged && allocator && base_list && list) {
        const uint32_t valid_record = 73;
        SetProgramDesc invalid_table_program = set_program;
        invalid_table_program.WorkGraph.NodeLocalRootArgumentsTable.StrideInBytes = 0;
        NodeCPUInput valid_input = {};
        valid_input.EntrypointIndex = 0;
        valid_input.NumRecords = 1;
        valid_input.Records = &valid_record;
        valid_input.RecordStrideInBytes = sizeof(valid_record);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc valid_dispatch = {};
            valid_dispatch.Mode = 0;
            valid_dispatch.NodeCPUInput = valid_input;
            list->SetProgram(&invalid_table_program);
            list->DispatchGraph(&valid_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                uint32_t after_invalid_table[8] = {};
                std::memcpy(after_invalid_table, mapped,
                            sizeof(after_invalid_table));
                backing->Unmap(0, nullptr);
                node_local_table_validation_exact =
                    std::memcmp(after_invalid_table, multi_gpu_values,
                                sizeof(after_invalid_table)) == 0;
            }
        }

        const uint32_t overflow_records[2] = {201, 202};
        NodeCPUInput overflow_input = {};
        overflow_input.EntrypointIndex = 0;
        overflow_input.NumRecords = 2;
        overflow_input.Records = overflow_records;
        overflow_input.RecordStrideInBytes = sizeof(overflow_records[0]);
        SetProgramDesc overflow_program = set_program;
        overflow_program.WorkGraph.BackingMemory.SizeInBytes = 8;
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc overflow_dispatch = {};
            overflow_dispatch.Mode = 0;
            overflow_dispatch.NodeCPUInput = overflow_input;
            list->SetProgram(&overflow_program);
            list->DispatchGraph(&overflow_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                uint32_t after_overflow[8] = {};
                std::memcpy(after_overflow, mapped, sizeof(after_overflow));
                backing->Unmap(0, nullptr);
                backing_overflow_unchanged =
                    std::memcmp(after_overflow, multi_gpu_values,
                                sizeof(after_overflow)) == 0;
            }
        }
    }

    if (multi_node_negative_unchanged && backing_overflow_unchanged &&
        allocator && base_list && list) {
        const uint32_t first_ordered_record = 5;
        const uint32_t second_ordered_record = 101;
        NodeCPUInput first_ordered = {};
        first_ordered.EntrypointIndex = 0;
        first_ordered.NumRecords = 1;
        first_ordered.Records = &first_ordered_record;
        first_ordered.RecordStrideInBytes = sizeof(first_ordered_record);
        NodeCPUInput second_ordered = {};
        second_ordered.EntrypointIndex = 1;
        second_ordered.NumRecords = 1;
        second_ordered.Records = &second_ordered_record;
        second_ordered.RecordStrideInBytes = sizeof(second_ordered_record);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc first_dispatch = {};
            first_dispatch.Mode = 0;
            first_dispatch.NodeCPUInput = first_ordered;
            DispatchGraphDesc second_dispatch = {};
            second_dispatch.Mode = 0;
            second_dispatch.NodeCPUInput = second_ordered;
            list->SetProgram(&set_program);
            list->DispatchGraph(&first_dispatch);
            list->DispatchGraph(&second_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                uint32_t ordered_values[2] = {};
                std::memcpy(ordered_values, mapped, sizeof(ordered_values));
                backing->Unmap(0, nullptr);
                multi_dispatch_ordering_exact =
                    ordered_values[0] == second_ordered_record + 2u &&
                    ordered_values[1] ==
                        (second_ordered_record ^ (0x4d4e4f44u + 1u));
            }
        }
    }
    if (SUCCEEDED(node_shader_hr) && node_shader_state_properties &&
        node_root && node_output && allocator && base_list && list) {
        SetProgramDesc node_program = set_program;
        std::memcpy(node_program.WorkGraph.ProgramIdentifier,
                    node_shader_identifier, sizeof(node_shader_identifier));
        const uint32_t node_record = 1;
        NodeCPUInput node_input = {};
        node_input.EntrypointIndex = 0;
        node_input.NumRecords = 1;
        node_input.Records = &node_record;
        node_input.RecordStrideInBytes = sizeof(node_record);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc node_dispatch = {};
            node_dispatch.Mode = 0;
            node_dispatch.NodeCPUInput = node_input;
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(
                0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&node_program);
            list->DispatchGraph(&node_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            hr = node_output->ReadFromSubresource(
                node_shader_values, sizeof(node_shader_values),
                sizeof(node_shader_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                dxil_node_shader_readback_exact =
                    node_shader_values[0] == 0xabcdef01u &&
                    node_shader_values[1] == 0x12345678u;
                dxil_node_shader_uav_binding_exact =
                    dxil_node_shader_readback_exact;
            }
        }
    }
    if (SUCCEEDED(node_shader_hr) && node_shader_state_properties &&
        node_root && node_output && gpu_input_desc && allocator && base_list &&
        list) {
        SetProgramDesc node_gpu_program = set_program;
        std::memcpy(node_gpu_program.WorkGraph.ProgramIdentifier,
                    node_shader_identifier, sizeof(node_shader_identifier));
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc node_gpu_dispatch = {};
            node_gpu_dispatch.Mode = 1;
            node_gpu_dispatch.Raw[0] =
                gpu_input_desc->GetGPUVirtualAddress() + 32;
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(
                0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&node_gpu_program);
            list->DispatchGraph(&node_gpu_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            uint32_t node_gpu_values[2] = {};
            hr = node_output->ReadFromSubresource(
                node_gpu_values, sizeof(node_gpu_values),
                sizeof(node_gpu_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                dxil_node_shader_gpu_readback_exact =
                    node_gpu_values[0] == 0xabcdef01u &&
                    node_gpu_values[1] == 0x12345678u;
            }
        }
    }
    if (SUCCEEDED(node_multi_hr) && node_multi_state_properties &&
        node_root && node_output && allocator && base_list && list) {
        SetProgramDesc node_multi_program = set_program;
        std::memcpy(node_multi_program.WorkGraph.ProgramIdentifier,
                    node_multi_identifier, sizeof(node_multi_identifier));
        const uint32_t node_multi_record_a = 7;
        const uint32_t node_multi_record_b = 8;
        const uint32_t node_multi_record_c = 9;
        NodeCPUInput node_multi_input_a = {};
        node_multi_input_a.EntrypointIndex = 0;
        node_multi_input_a.NumRecords = 1;
        node_multi_input_a.Records = &node_multi_record_a;
        node_multi_input_a.RecordStrideInBytes = sizeof(node_multi_record_a);
        NodeCPUInput node_multi_input_b = {};
        node_multi_input_b.EntrypointIndex = 1;
        node_multi_input_b.NumRecords = 1;
        node_multi_input_b.Records = &node_multi_record_b;
        node_multi_input_b.RecordStrideInBytes = sizeof(node_multi_record_b);
        NodeCPUInput node_multi_input_c = {};
        node_multi_input_c.EntrypointIndex = 2;
        node_multi_input_c.NumRecords = 1;
        node_multi_input_c.Records = &node_multi_record_c;
        node_multi_input_c.RecordStrideInBytes = sizeof(node_multi_record_c);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc node_multi_dispatch_a = {};
            node_multi_dispatch_a.Mode = 0;
            node_multi_dispatch_a.NodeCPUInput = node_multi_input_a;
            DispatchGraphDesc node_multi_dispatch_b = {};
            node_multi_dispatch_b.Mode = 0;
            node_multi_dispatch_b.NodeCPUInput = node_multi_input_b;
            DispatchGraphDesc node_multi_dispatch_c = {};
            node_multi_dispatch_c.Mode = 0;
            node_multi_dispatch_c.NodeCPUInput = node_multi_input_c;
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(
                0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&node_multi_program);
            list->DispatchGraph(&node_multi_dispatch_a);
            list->DispatchGraph(&node_multi_dispatch_b);
            list->DispatchGraph(&node_multi_dispatch_c);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            hr = node_output->ReadFromSubresource(
                dxil_multi_node_values, sizeof(dxil_multi_node_values),
                sizeof(dxil_multi_node_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                dxil_multi_node_readback_exact =
                    dxil_multi_node_values[0] == 0x11111111u &&
                    dxil_multi_node_values[1] == 0xaaaa0001u;
            }
        }
    }
    if (SUCCEEDED(node_multi_hr) && node_multi_state_properties &&
        node_root && node_output && allocator && base_list && list) {
        const uint32_t multi_cpu_record = 10;
        NodeCPUInput multi_cpu_node = {};
        multi_cpu_node.EntrypointIndex = 2;
        multi_cpu_node.NumRecords = 1;
        multi_cpu_node.Records = &multi_cpu_record;
        multi_cpu_node.RecordStrideInBytes = sizeof(multi_cpu_record);
        MultiNodeCPUInput multi_cpu_input = {};
        multi_cpu_input.NumNodeInputs = 1;
        multi_cpu_input.NodeInputs = &multi_cpu_node;
        multi_cpu_input.NodeInputStrideInBytes = sizeof(NodeCPUInput);
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc multi_cpu_dispatch = {};
            multi_cpu_dispatch.Mode = 2;
            multi_cpu_dispatch.MultiNodeCPUInput = multi_cpu_input;
            SetProgramDesc node_multi_program = set_program;
            std::memcpy(node_multi_program.WorkGraph.ProgramIdentifier,
                        node_multi_identifier, sizeof(node_multi_identifier));
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(
                0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&node_multi_program);
            list->DispatchGraph(&multi_cpu_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            hr = node_output->ReadFromSubresource(
                node_multi_cpu_values, sizeof(node_multi_cpu_values),
                sizeof(node_multi_cpu_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                node_multi_cpu_input_exact =
                    node_multi_cpu_values[0] == 0x11111111u &&
                    node_multi_cpu_values[1] == 0xaaaa0001u;
            }
        }
    }
    if (SUCCEEDED(node_multi_hr) && node_multi_state_properties &&
        node_root && node_output && gpu_multi_input_desc && allocator &&
        base_list && list) {
        SetProgramDesc node_multi_program = set_program;
        std::memcpy(node_multi_program.WorkGraph.ProgramIdentifier,
                    node_multi_identifier, sizeof(node_multi_identifier));
        hr = allocator->Reset();
        if (SUCCEEDED(hr))
            hr = base_list->Reset(allocator, nullptr);
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc multi_gpu_dispatch = {};
            multi_gpu_dispatch.Mode = 3;
            multi_gpu_dispatch.Raw[0] =
                gpu_multi_input_desc->GetGPUVirtualAddress() + 160;
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(
                0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&node_multi_program);
            list->DispatchGraph(&multi_gpu_dispatch);
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) {
            hr = node_output->ReadFromSubresource(
                node_multi_gpu_values, sizeof(node_multi_gpu_values),
                sizeof(node_multi_gpu_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                node_multi_gpu_input_exact =
                    node_multi_gpu_values[0] == 0x22222222u &&
                    node_multi_gpu_values[1] == 0xbbbb0002u;
            }
        }
    }

    if (SUCCEEDED(hr) && dxil_node_shader_uav_binding_exact) {
        ID3D12RootSignature* table_root = nullptr;
        ID3DBlob* table_blob = nullptr;
        ID3D12DescriptorHeap* table_heap = nullptr;
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 7;
        parameters[0].Constants.Num32BitValues = 1;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {};
        root_desc.NumParameters = 2;
        root_desc.pParameters = parameters;
        hr = serialize_root_signature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                      &table_blob, nullptr);
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, table_blob->GetBufferPointer(),
                table_blob->GetBufferSize(), IID_PPV_ARGS(&table_root));
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.NumDescriptors = 3;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&table_heap));
        const UINT increment = device->GetDescriptorHandleIncrementSize(heap_desc.Type);
        uint32_t before[16] = {}, after[16] = {};
        for (unsigned test = 0; test < 3 && SUCCEEDED(hr); ++test) {
            hr = node_output->ReadFromSubresource(before, sizeof(before), sizeof(before), 0, nullptr);
            if (FAILED(hr)) break;
            auto cpu = table_heap->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += 2 * increment;
            auto gpu = table_heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += increment;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer.FirstElement = test ? 4 : 2;
            view.Buffer.NumElements = test == 1 ? 1 : 2;
            // node_records declares RWByteAddressBuffer, so its table must
            // expose a raw UAV rather than a structured-buffer view.
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device->CreateUnorderedAccessView(test == 2 ? nullptr : node_output,
                                              nullptr, &view, cpu);
            hr = allocator->Reset();
            if (SUCCEEDED(hr)) hr = base_list->Reset(allocator, nullptr);
            if (SUCCEEDED(hr)) {
                SetProgramDesc program = set_program;
                std::memcpy(program.WorkGraph.ProgramIdentifier,
                            node_shader_identifier, sizeof(node_shader_identifier));
                const uint32_t record = 1;
                DispatchGraphDesc dispatch = {};
                dispatch.Mode = 0;
                dispatch.NodeCPUInput.NumRecords = 1;
                dispatch.NodeCPUInput.Records = &record;
                dispatch.NodeCPUInput.RecordStrideInBytes = sizeof(record);
                ID3D12DescriptorHeap* heaps[] = {table_heap};
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootSignature(table_root);
                list->SetComputeRootDescriptorTable(1, gpu);
                list->SetProgram(&program);
                list->DispatchGraph(&dispatch);
                hr = execute_and_wait(device, queue, base_list);
            }
            if (SUCCEEDED(hr))
                hr = node_output->ReadFromSubresource(after, sizeof(after), sizeof(after), 0, nullptr);
            if (SUCCEEDED(hr)) {
                if (!test) {
                    before[2] = 0xabcdef01u;
                    before[3] = 0x12345678u;
                    node_table_uav_exact = !std::memcmp(before, after, sizeof(after));
                } else if (test == 1) {
                    node_table_short_view_unchanged = !std::memcmp(before, after, sizeof(after));
                } else {
                    node_table_null_view_unchanged = !std::memcmp(before, after, sizeof(after));
                }
            }
        }
        release(table_heap);
        release(table_blob);
        release(table_root);
    }

    if (compute_queue && compute_base_list && compute_list &&
        SUCCEEDED(hr)) {
        const uint32_t compute_record = 17;
        NodeCPUInput compute_input = {};
        compute_input.EntrypointIndex = 0;
        compute_input.NumRecords = 1;
        compute_input.Records = &compute_record;
        compute_input.RecordStrideInBytes = sizeof(compute_record);
        DispatchGraphDesc compute_dispatch = {};
        compute_dispatch.Mode = 0;
        compute_dispatch.NodeCPUInput = compute_input;
        compute_list->SetProgram(&set_program);
        compute_list->DispatchGraph(&compute_dispatch);
        hr = execute_and_wait(device, compute_queue, compute_base_list);
        if (SUCCEEDED(hr)) {
            void* mapped = nullptr;
            hr = backing->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                const uint32_t* values_ptr =
                    static_cast<const uint32_t*>(mapped);
                cross_queue_dispatch_exact =
                    values_ptr[0] == compute_record + 1u &&
                    values_ptr[1] == (compute_record ^ 0x57475250u);
                backing->Unmap(0, nullptr);
            }
        }
    }

    // Submit the GPU-input consumer before its producer, with no intervening
    // host completion wait. The queue fence must make the new record visible.
    for (unsigned multi = 0; multi < 4 && SUCCEEDED(hr) && cross_queue_dispatch_exact; ++multi) {
        uint32_t* cycle_values = multi == 3 ? node_input_gpu_dependency_values
            : multi == 2 ? cross_queue_multi_values
            : multi ? cross_queue_repeated_values : cross_queue_values;
        ID3D12Fence* dependency = nullptr;
        ID3D12Fence* completion = nullptr;
        HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        hr = event ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         IID_PPV_ARGS(&dependency)) : E_FAIL;
        if (SUCCEEDED(hr))
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&completion));
        NodeGPUInput consumer_input = {};
        consumer_input.EntrypointIndex = multi == 3 ? 2u : 0u;
        consumer_input.NumRecords = 1;
        consumer_input.Records = backing->GetGPUVirtualAddress();
        consumer_input.RecordStrideInBytes = sizeof(uint32_t);
        void* mapped = nullptr;
        if (SUCCEEDED(hr)) hr = gpu_input_desc->Map(0, nullptr, &mapped);
        if (SUCCEEDED(hr) && mapped) {
            std::memcpy(mapped, &consumer_input, sizeof(consumer_input));
            gpu_input_desc->Unmap(0, nullptr);
        }
        if (SUCCEEDED(hr) && multi == 2) {
            // The multi-input descriptor is host-authored; only its record
            // payload is produced by the pending GPU dispatch.
            hr = gpu_multi_input_desc->Map(0, nullptr, &mapped);
            if (SUCCEEDED(hr) && mapped) {
                std::memcpy(static_cast<uint8_t*>(mapped) + 128,
                            &consumer_input, sizeof(consumer_input));
                gpu_multi_input_desc->Unmap(0, nullptr);
            }
        }
        if (SUCCEEDED(hr)) hr = compute_allocator->Reset();
        if (SUCCEEDED(hr)) hr = compute_base_list->Reset(compute_allocator, nullptr);
        if (SUCCEEDED(hr)) {
            SetProgramDesc consumer_program = set_program;
            consumer_program.WorkGraph.BackingMemory.StartAddress =
                node_output->GetGPUVirtualAddress();
            consumer_program.WorkGraph.BackingMemory.SizeInBytes = 8;
            if (multi == 3) {
                std::memcpy(consumer_program.WorkGraph.ProgramIdentifier, input_program_identifier, 32);
                compute_list->SetComputeRootSignature(node_root);
                compute_list->SetComputeRootUnorderedAccessView(0, node_output->GetGPUVirtualAddress());
            }
            DispatchGraphDesc consumer_dispatch = {};
            consumer_dispatch.Mode = multi == 2 ? 3 : 1;
            consumer_dispatch.Raw[0] = multi == 2
                ? gpu_multi_input_desc->GetGPUVirtualAddress() + 160
                : gpu_input_desc->GetGPUVirtualAddress();
            compute_list->SetProgram(&consumer_program);
            compute_list->DispatchGraph(&consumer_dispatch);
            hr = compute_base_list->Close();
        }
        if (SUCCEEDED(hr)) hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = base_list->Reset(allocator, nullptr);
        const uint32_t producer_record = 123 + 111 * multi;
        if (SUCCEEDED(hr)) {
            DispatchGraphDesc producer_dispatch = {};
            producer_dispatch.Mode = 0;
            producer_dispatch.NodeCPUInput.NumRecords = 1;
            producer_dispatch.NodeCPUInput.Records = &producer_record;
            producer_dispatch.NodeCPUInput.RecordStrideInBytes = sizeof(uint32_t);
            list->SetProgram(&set_program);
            list->DispatchGraph(&producer_dispatch);
            hr = base_list->Close();
        }
        if (SUCCEEDED(hr)) hr = compute_queue->Wait(dependency, multi + 1);
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* consumer_lists[] = {compute_base_list};
            compute_queue->ExecuteCommandLists(1, consumer_lists);
            ID3D12CommandList* producer_lists[] = {base_list};
            queue->ExecuteCommandLists(1, producer_lists);
            hr = queue->Signal(dependency, multi + 1);
            if (SUCCEEDED(hr)) hr = compute_queue->Signal(completion, multi + 1);
            if (SUCCEEDED(hr)) hr = completion->SetEventOnCompletion(multi + 1, event);
            if (SUCCEEDED(hr) && WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
                hr = E_FAIL;
            if (SUCCEEDED(hr))
                hr = node_output->ReadFromSubresource(
                    cycle_values, sizeof(cross_queue_values),
                    sizeof(cross_queue_values), 0, nullptr);
            if (SUCCEEDED(hr)) {
                bool& exact = multi == 3 ? node_input_gpu_dependency_exact
                    : multi == 2 ? cross_queue_multi_gpu_dependency_exact
                    : multi ? cross_queue_repeated_gpu_dependency_exact
                            : cross_queue_gpu_dependency_exact;
                if (multi == 3)
                    exact = input_program_ready && cycle_values[0] == 0xabcdef00u + producer_record + 1u &&
                        cycle_values[1] == 0x12345678u;
                else
                    exact = cycle_values[0] == producer_record + 2u &&
                        cycle_values[1] == ((producer_record + 1u) ^ 0x57475250u);
            }
        }
        std::fprintf(stderr, "crossqueue cycle=%u hr=%08lx dependency=%llu completion=%llu\n",
                     multi, static_cast<unsigned long>(hr),
                     dependency ? dependency->GetCompletedValue() : 0,
                     completion ? completion->GetCompletedValue() : 0);
        if (event) CloseHandle(event);
        release(completion);
        release(dependency);
    }

    if (SUCCEEDED(hr) && input_program_ready) {
        node_input_binding_exact = true;
        uint32_t previous[2] = {};
        for (UINT mode = 0; mode < 6 && SUCCEEDED(hr); ++mode) {
            uint32_t value = mode == 0 ? 1u : mode == 1 ? 9u : 37u;
            hr = allocator->Reset();
            if (SUCCEEDED(hr)) hr = base_list->Reset(allocator, nullptr);
            SetProgramDesc program = set_program;
            std::memcpy(program.WorkGraph.ProgramIdentifier, input_program_identifier, 32);
            DispatchGraphDesc dispatch = {};
            dispatch.NodeCPUInput = {mode == 3 ? 0u : 2u, 1, &value, 4};
            uint32_t two_records[2] = {1, 2};
            uint32_t backing_before[4] = {};
            if (mode == 5) {
                dispatch.NodeCPUInput.NumRecords = 2;
                dispatch.NodeCPUInput.Records = two_records;
                program.WorkGraph.Flags = 0;
                if (SUCCEEDED(hr)) hr = backing->ReadFromSubresource(backing_before, 16, 16, 0, nullptr);
            }
            if (mode == 2 || mode == 4) {
                void* mapped = nullptr;
                if (SUCCEEDED(hr)) hr = gpu_records->Map(0, nullptr, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    std::memcpy(mapped, &value, sizeof(value));
                    gpu_records->Unmap(0, nullptr);
                }
                NodeGPUInput input = {};
                input.EntrypointIndex = 2;
                input.NumRecords = 1;
                input.Records = gpu_records->GetGPUVirtualAddress() + (mode == 4 ? 1 : 0);
                input.RecordStrideInBytes = 4;
                if (SUCCEEDED(hr)) hr = gpu_input_desc->Map(0, nullptr, &mapped);
                if (SUCCEEDED(hr) && mapped) {
                    std::memcpy(mapped, &input, sizeof(input));
                    gpu_input_desc->Unmap(0, nullptr);
                }
                dispatch.Mode = 1;
                dispatch.Raw[0] = gpu_input_desc->GetGPUVirtualAddress();
            }
            if (SUCCEEDED(hr)) {
                list->SetComputeRootSignature(node_root);
                list->SetComputeRootUnorderedAccessView(0, node_output->GetGPUVirtualAddress());
                list->SetProgram(&program);
                list->DispatchGraph(&dispatch);
                value = 0xdeadbeefu; // Recorded CPU input must already be owned.
                hr = execute_and_wait(device, queue, base_list);
            }
            uint32_t actual[2] = {};
            if (SUCCEEDED(hr)) hr = node_output->ReadFromSubresource(actual, 8, 8, 0, nullptr);
            if (SUCCEEDED(hr)) {
                if (mode < 3)
                    node_input_binding_exact &= actual[0] == 0xabcdef00u + (mode == 0 ? 1u : mode == 1 ? 9u : 37u) && actual[1] == 0x12345678u;
                else
                    node_input_binding_exact &= std::memcmp(actual, previous, sizeof(actual)) == 0;
                std::memcpy(previous, actual, sizeof(actual));
                if (mode == 5) {
                    uint32_t backing_after[4] = {};
                    hr = backing->ReadFromSubresource(backing_after, 16, 16, 0, nullptr);
                    node_input_binding_exact &= SUCCEEDED(hr) &&
                        std::memcmp(backing_before, backing_after, sizeof(backing_before)) == 0;
                }
            } else node_input_binding_exact = false;
        }
    }

    if (SUCCEEDED(hr) && input_program_ready) {
        uint32_t records[4] = {101, 202, 303, 404};
        hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = base_list->Reset(allocator, nullptr);
        SetProgramDesc program = set_program;
        std::memcpy(program.WorkGraph.ProgramIdentifier, input_program_identifier, 32);
        DispatchGraphDesc dispatch = {};
        dispatch.NodeCPUInput = {0, 1, records, sizeof(records)};
        if (SUCCEEDED(hr)) {
            list->SetComputeRootSignature(node_root);
            list->SetComputeRootUnorderedAccessView(0, node_output->GetGPUVirtualAddress());
            list->SetProgram(&program);
            list->DispatchGraph(&dispatch);
            for (auto &value : records) value = 0;
            hr = execute_and_wait(device, queue, base_list);
        }
        if (SUCCEEDED(hr)) hr = node_output->ReadFromSubresource(node_launch_values, 32, 32, 0, nullptr);
        if (SUCCEEDED(hr)) {
            node_launch_geometry_exact = true;
            for (UINT i = 0; i < 8; ++i)
                node_launch_geometry_exact &= node_launch_values[i] == 101u * (1 + i % 4);
        }
    }

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.workgraph-execution.v1\",\n");
    const bool all_readbacks =
        include_all_properties_ok && work_graph_initialize_exact &&
        readback_ok && gpu_input_readback_ok && multi_cpu_readback_ok &&
        multi_cpu_pointer_free && multi_gpu_readback_ok &&
        multi_node_negative_unchanged && node_local_table_validation_exact &&
        backing_overflow_unchanged && multi_dispatch_ordering_exact &&
        cross_queue_dispatch_exact && cross_queue_gpu_dependency_exact &&
        cross_queue_repeated_gpu_dependency_exact && cross_queue_multi_gpu_dependency_exact &&
        node_shader_bytecode_loaded && dxil_node_shader_readback_exact &&
        dxil_node_shader_uav_binding_exact && node_table_uav_exact &&
        node_table_short_view_unchanged && node_table_null_view_unchanged &&
        dxil_node_shader_gpu_readback_exact &&
        node_multi_bytecode_loaded &&
        node_multi_properties_complete && node_input_layouts_exact && node_input_binding_exact && node_internal_binding_rejected && node_input_gpu_dependency_exact && node_launch_geometry_exact && node_dynamic_grid_rejected && dxil_multi_node_readback_exact &&
        node_multi_cpu_input_exact && node_multi_gpu_input_exact;
    std::printf("  \"pass\": %s,\n", SUCCEEDED(hr) && properties_ok && all_readbacks ? "true" : "false");
    std::printf("  \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::printf("  \"properties_complete\": %s,\n", properties_ok ? "true" : "false");
    std::printf("  \"include_all_properties_complete\": %s,\n",
                include_all_properties_ok ? "true" : "false");
    std::printf("  \"work_graph_initialize_exact\": %s,\n",
                work_graph_initialize_exact ? "true" : "false");
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
    std::printf("  \"multi_node_cpu_pointer_free\": %s,\n",
                multi_cpu_pointer_free ? "true" : "false");
    std::printf("  \"multi_node_negative_unchanged\": %s,\n",
                multi_node_negative_unchanged ? "true" : "false");
    std::printf("  \"multi_dispatch_ordering_exact\": %s,\n",
                multi_dispatch_ordering_exact ? "true" : "false");
    std::printf("  \"cross_queue_dispatch_exact\": %s,\n",
                cross_queue_dispatch_exact ? "true" : "false");
    std::printf("  \"cross_queue_gpu_dependency_exact\": %s,\n",
                cross_queue_gpu_dependency_exact ? "true" : "false");
    std::printf("  \"cross_queue_repeated_gpu_dependency_exact\": %s,\n",
                cross_queue_repeated_gpu_dependency_exact ? "true" : "false");
    std::printf("  \"cross_queue_multi_gpu_dependency_exact\": %s,\n",
                cross_queue_multi_gpu_dependency_exact ? "true" : "false");
    std::printf("  \"cross_queue_multi_values\": [%u, %u],\n",
                cross_queue_multi_values[0], cross_queue_multi_values[1]);
    std::printf("  \"cross_queue_values\": [%u, %u],\n",
                cross_queue_values[0], cross_queue_values[1]);
    std::printf("  \"cross_queue_repeated_values\": [%u, %u],\n",
                cross_queue_repeated_values[0], cross_queue_repeated_values[1]);
    std::printf("  \"node_local_table_validation_exact\": %s,\n",
                node_local_table_validation_exact ? "true" : "false");
    std::printf("  \"backing_overflow_unchanged\": %s,\n",
                backing_overflow_unchanged ? "true" : "false");
    std::printf("  \"node_shader_bytecode_loaded\": %s,\n",
                node_shader_bytecode_loaded ? "true" : "false");
    std::printf("  \"dxil_node_shader_readback_exact\": %s,\n",
                dxil_node_shader_readback_exact ? "true" : "false");
    std::printf("  \"dxil_node_shader_uav_binding_exact\": %s,\n",
                dxil_node_shader_uav_binding_exact ? "true" : "false");
    std::printf("  \"node_table_uav_exact\": %s,\n", node_table_uav_exact ? "true" : "false");
    std::printf("  \"node_table_short_view_unchanged\": %s,\n",
                node_table_short_view_unchanged ? "true" : "false");
    std::printf("  \"node_table_null_view_unchanged\": %s,\n",
                node_table_null_view_unchanged ? "true" : "false");
    std::printf("  \"dxil_node_shader_gpu_readback_exact\": %s,\n",
                dxil_node_shader_gpu_readback_exact ? "true" : "false");
    std::printf("  \"node_multi_bytecode_loaded\": %s,\n",
                node_multi_bytecode_loaded ? "true" : "false");
    std::printf("  \"dxil_multi_node_readback_exact\": %s,\n",
                dxil_multi_node_readback_exact ? "true" : "false");
    std::printf("  \"node_dynamic_grid_rejected\": %s,\n", node_dynamic_grid_rejected ? "true" : "false");
    std::printf("  \"node_launch_geometry_exact\": %s,\n", node_launch_geometry_exact ? "true" : "false");
    std::printf("  \"node_launch_values\": [%u,%u,%u,%u,%u,%u,%u,%u],\n", node_launch_values[0], node_launch_values[1], node_launch_values[2], node_launch_values[3], node_launch_values[4], node_launch_values[5], node_launch_values[6], node_launch_values[7]);
    std::printf("  \"node_input_gpu_dependency_exact\": %s,\n", node_input_gpu_dependency_exact ? "true" : "false");
    std::printf("  \"node_input_gpu_dependency_values\": [%u, %u],\n", node_input_gpu_dependency_values[0], node_input_gpu_dependency_values[1]);
    std::printf("  \"node_internal_binding_rejected\": %s,\n", node_internal_binding_rejected ? "true" : "false");
    std::printf("  \"node_input_binding_exact\": %s,\n", node_input_binding_exact ? "true" : "false");
    std::printf("  \"node_input_layouts_exact\": %s,\n", node_input_layouts_exact ? "true" : "false");
    std::printf("  \"node_multi_properties_complete\": %s,\n",
                node_multi_properties_complete ? "true" : "false");
    std::printf("  \"node_multi_cpu_input_exact\": %s,\n",
                node_multi_cpu_input_exact ? "true" : "false");
    std::printf("  \"node_multi_gpu_input_exact\": %s,\n",
                node_multi_gpu_input_exact ? "true" : "false");
    std::printf("  \"dxil_node_shader_values\": [%u, %u],\n",
                node_shader_values[0], node_shader_values[1]);
    std::printf("  \"dxil_multi_node_values\": [%u, %u],\n",
                dxil_multi_node_values[0], dxil_multi_node_values[1]);
    std::printf("  \"node_multi_cpu_values\": [%u, %u],\n",
                node_multi_cpu_values[0], node_multi_cpu_values[1]);
    std::printf("  \"node_multi_gpu_values\": [%u, %u],\n",
                node_multi_gpu_values[0], node_multi_gpu_values[1]);
    std::printf("  \"multi_node_cpu_values\": [%u, %u, %u, %u, %u, %u, %u, %u],\n",
                multi_cpu_values[0], multi_cpu_values[1], multi_cpu_values[2],
                multi_cpu_values[3], multi_cpu_values[4], multi_cpu_values[5],
                multi_cpu_values[6], multi_cpu_values[7]);
    std::printf("  \"multi_node_gpu_values\": [%u, %u, %u, %u, %u, %u, %u, %u]\n",
                multi_gpu_values[0], multi_gpu_values[1], multi_gpu_values[2],
                multi_gpu_values[3], multi_gpu_values[4], multi_gpu_values[5],
                multi_gpu_values[6], multi_gpu_values[7]);
    std::printf("}\n");

    release(node_multi_properties);
    release(node_multi_state_properties);
    release(node_multi_state);
    release(input_program_state);
    release(node_shader_state_properties);
    release(node_shader_state);
    release(include_all_state);
    release(state_properties);
    release(state);
    release(gpu_multi_input_desc);
    release(gpu_input_desc);
    release(gpu_records);
    release(node_root_blob);
    release(node_root);
    release(node_output);
    release(node_local_table);
    release(backing);
    release(compute_list);
    release(compute_base_list);
    release(compute_allocator);
    release(compute_queue);
    release(list);
    release(base_list);
    release(allocator);
    release(queue);
    release(device5);
    release(device);
    if (module)
        FreeLibrary(module);
    return SUCCEEDED(hr) && properties_ok && include_all_properties_ok &&
                   work_graph_initialize_exact && readback_ok &&
                   gpu_input_readback_ok && multi_cpu_readback_ok &&
                   multi_cpu_pointer_free && multi_gpu_readback_ok &&
                   multi_node_negative_unchanged && node_local_table_validation_exact &&
                   backing_overflow_unchanged && multi_dispatch_ordering_exact &&
                   cross_queue_dispatch_exact && cross_queue_gpu_dependency_exact &&
                   cross_queue_repeated_gpu_dependency_exact && cross_queue_multi_gpu_dependency_exact &&
                   node_shader_bytecode_loaded && dxil_node_shader_readback_exact &&
                   dxil_node_shader_uav_binding_exact && node_table_uav_exact &&
                   node_table_short_view_unchanged && node_table_null_view_unchanged &&
                   dxil_node_shader_gpu_readback_exact && node_multi_bytecode_loaded &&
                   node_multi_properties_complete && node_input_layouts_exact && node_input_binding_exact && node_internal_binding_rejected && node_input_gpu_dependency_exact && node_launch_geometry_exact && node_dynamic_grid_rejected &&
                   dxil_multi_node_readback_exact && node_multi_cpu_input_exact &&
                   node_multi_gpu_input_exact
               ? 0
               : 1;
}
