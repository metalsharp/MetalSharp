#define WIN32_LEAN_AND_MEAN
#include "d3d12_command_list_extensions.hpp"
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
struct NodeID { LPCWSTR name; UINT index; };
struct ShaderNode { LPCWSTR shader; UINT overrides_type; const void *overrides; };
struct Node { UINT type; ShaderNode shader; };
struct WorkGraph { LPCWSTR name; UINT flags, entry_count; const NodeID *entries; UINT node_count; const Node *nodes; };
struct Generic { LPCWSTR name; UINT export_count; LPCWSTR *exports; UINT subobject_count; const D3D12_STATE_SUBOBJECT *const *subobjects; };
template<class T> struct Owned {
    T *p = nullptr;
    ~Owned() { if (p) p->Release(); }
    T *operator->() const { return p; }
};
struct AddressRange { UINT64 address, size; };
struct AddressStride { UINT64 address, size, stride; };
struct Program {
    UINT type = 5, padding = 0;
    UINT64 identifier[4] = {};
    UINT flags = 1, padding2 = 0;
    AddressRange backing = {};
    AddressStride local = {};
};
struct GraphInput { UINT entry, count; const void *records; UINT64 stride; };
struct GraphDispatch { UINT mode = 0, padding = 0; GraphInput input = {}; };
struct FixtureOptions {
    bool recursion, fanout, early, icb, overdepth, boundary, coalescing, empty_output, zero, mismatch, gpu_headers, gpu_headers_coalescing, gpu_multi_headers, gpu_headers_broadcasting;
    uint32_t gpu_multi_mode = 0;
};
static bool dispatch_arrays(ID3D12Device5 *device, ID3D12StateObject *state, HMODULE module, uint32_t (&values)[16], bool &backing_unchanged, bool &empty_allocation_exact, const FixtureOptions &options) {
    const auto &[recursion, fanout, early, icb, overdepth, boundary, coalescing, empty_output, zero, mismatch, gpu_headers, gpu_headers_coalescing, gpu_multi_headers, gpu_headers_broadcasting, gpu_multi_mode] = options;
    // Coalescing and broadcasting fixtures deliberately share the single-node
    // GPU-header ABI; their compiled node launch type selects the downstream
    // grouping behavior. Keep the distinction explicit rather than silently
    // treating the command-line variants as aliases.
    const bool gpu_header_single_launch =
        gpu_headers_coalescing || gpu_headers_broadcasting;
    const bool gpu_multi_no_work = (gpu_multi_mode >= 1u && gpu_multi_mode <= 6u) || gpu_multi_mode == 9u || gpu_multi_mode == 11u || gpu_multi_mode == 13u;
    Owned<ID3D12CommandQueue> queue;
    Owned<ID3D12CommandAllocator> allocator;
    Owned<ID3D12GraphicsCommandList> list;
    Owned<dxmt::GraphicsCommandList10Extension> extension;
    Owned<ID3D12Resource> backing, output, header_data;
    Owned<ID3D12RootSignature> producer_root;
    Owned<ID3DBlob> producer_blob;
    Owned<ID3D12PipelineState> producer_pipeline;
    Owned<ID3D12RootSignature> root;
    Owned<ID3DBlob> blob;
    Owned<ID3D12Fence> fence;
    Owned<ID3D12StateObjectProperties> properties;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue.p));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.p));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_PPV_ARGS(&list.p));
    if (SUCCEEDED(hr)) hr = list->QueryInterface(dxmt::kID3D12GraphicsCommandList10, reinterpret_cast<void **>(&extension.p));
    auto make_buffer = [&](UINT64 size, bool uav, ID3D12Resource **resource) {
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = uav ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void **>(resource));
    };
    if (SUCCEEDED(hr)) hr = make_buffer(2u << 20, false, &backing.p);
    if (SUCCEEDED(hr)) hr = make_buffer(64, true, &output.p);
    // Cover allocator metadata, record payload slots, and the trailing backing
    // range. A prefix-only sentinel misses writes outside the first entries.
    std::vector<uint32_t> sentinel((2u << 20) / sizeof(uint32_t));
    for (size_t i = 0; i < sentinel.size(); ++i)
        sentinel[i] = 0x31415926u ^ (uint32_t(i) * 0x9e3779b9u);
    if (SUCCEEDED(hr) && (overdepth || gpu_multi_no_work)) {
        void *data = nullptr;
        hr = backing->Map(0, nullptr, &data);
        if (SUCCEEDED(hr)) {
            if (data) std::memcpy(data, sentinel.data(), sentinel.size() * sizeof(uint32_t)); else hr = E_FAIL;
            backing->Unmap(0, nullptr);
        }
    }
    uint32_t initial_values[16] = {};
    if (SUCCEEDED(hr)) hr = output->WriteToSubresource(0, nullptr, initial_values, 64, 64);
    using Serialize = HRESULT (WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    auto serialize = reinterpret_cast<Serialize>(reinterpret_cast<void *>(GetProcAddress(module, "D3D12SerializeRootSignature")));
    D3D12_ROOT_PARAMETER parameter = {}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {}; root_desc.NumParameters = 1; root_desc.pParameters = &parameter;
    if (SUCCEEDED(hr)) hr = serialize ? serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob.p, nullptr) : E_FAIL;
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root.p));
    if (SUCCEEDED(hr)) hr = state->QueryInterface({0x460caac7,0x1d24,0x446a,{0xa1,0x84,0xca,0x67,0xdb,0x49,0x41,0x38}}, reinterpret_cast<void **>(&properties.p));
    if (SUCCEEDED(hr) && gpu_headers) {
        hr = make_buffer(512, true, &header_data.p);
        uint32_t poison[128]; for (auto &word : poison) word = 0xdeadbeefu;
        if (SUCCEEDED(hr)) hr = header_data->WriteToSubresource(0, nullptr, poison, 512, 512);
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 3;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC description = {}; description.NumParameters = 2; description.pParameters = parameters;
        if (SUCCEEDED(hr)) hr = serialize(&description, D3D_ROOT_SIGNATURE_VERSION_1, &producer_blob.p, nullptr);
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, producer_blob->GetBufferPointer(), producer_blob->GetBufferSize(), IID_PPV_ARGS(&producer_root.p));
        std::ifstream shader_file(gpu_multi_headers ? "probe_workgraph_gpu_multi_header_producer.cso" : "probe_workgraph_gpu_header_producer.cso", std::ios::binary);
        std::vector<char> shader((std::istreambuf_iterator<char>(shader_file)), {});
        if (shader.empty()) hr = E_FAIL;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {}; pipeline.pRootSignature = producer_root.p;
        pipeline.CS = {shader.data(), shader.size()};
        if (SUCCEEDED(hr)) hr = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&producer_pipeline.p));
        if (SUCCEEDED(hr)) {
            const uint64_t address = header_data->GetGPUVirtualAddress();
            const uint32_t words[3] = {uint32_t(address), uint32_t(address >> 32), options.gpu_multi_mode};
            list->SetComputeRootSignature(producer_root.p); list->SetPipelineState(producer_pipeline.p);
            list->SetComputeRoot32BitConstants(0, 3, words, 0);
            list->SetComputeRootUnorderedAccessView(1, address);
            list->Dispatch(1,1,1);
            D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = header_data.p;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);
        }
    }
    Program program;
    if (SUCCEEDED(hr)) {
        using Get = void *(STDMETHODCALLTYPE *)(ID3D12StateObjectProperties *, void *, LPCWSTR);
        auto table = *reinterpret_cast<void ***>(properties.p);
        if (!reinterpret_cast<Get>(table[7])(properties.p, program.identifier, L"arrays")) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        program.backing = {backing->GetGPUVirtualAddress(), 2u << 20};
        // Do not request an independent SetProgram initialization when checking
        // that rejected DispatchGraph leaves backing storage untouched.
        if (overdepth || gpu_multi_no_work) program.flags = 0;
        extension->SetComputeRootSignature(root.p);
        extension->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
        extension->SetProgram(&program);
        uint32_t dense[4][2] = {{0,101},{1,202},{2,303},{3,404}};
        uint32_t sparse[4][2] = {{65536,505},{1,999},{0,888},{UINT32_MAX,777}};
        uint32_t recursive_value = empty_output ? (zero ? 0u : 6u) : 1u;
        uint32_t nonrecursive_value = empty_output ? recursive_value : 99u;
        uint32_t recursive_grid[4] = {1,1,1,1}, nonrecursive_grid[4] = {99,1,1,1};
        uint32_t coalesced_values[4] = {1,2,3,4};
        GraphDispatch dispatch; dispatch.input = coalescing ? GraphInput{0,4,coalesced_values,4} : (recursion || empty_output) ? GraphInput{0,1,icb ? recursive_grid : &recursive_value,icb ? 16u : 4u} : GraphInput{0,4,dense,8};
        if (gpu_headers) {
            dispatch.mode = gpu_multi_headers ? 3u : 1u;
            if (gpu_header_single_launch && !gpu_multi_headers) dispatch.mode = 1u;
            const uint64_t address = header_data->GetGPUVirtualAddress();
            std::memcpy(&dispatch.input, &address, sizeof(address));
        }
        extension->DispatchGraph(&dispatch);
        if (!gpu_multi_headers) {
            dispatch.input = (recursion || empty_output) ? GraphInput{1,1,icb ? nonrecursive_grid : &nonrecursive_value,icb ? 16u : 4u} : GraphInput{1,4,sparse,8};
            if (gpu_headers) {
                dispatch.mode = 1u;
                const uint64_t address = header_data->GetGPUVirtualAddress() + 24u;
                std::memcpy(&dispatch.input, &address, sizeof(address));
            }
            extension->DispatchGraph(&dispatch);
        }
        recursive_value = nonrecursive_value = 0;
        std::memset(coalesced_values, 0, sizeof(coalesced_values));
        std::memset(recursive_grid, 0, sizeof(recursive_grid));
        std::memset(nonrecursive_grid, 0, sizeof(nonrecursive_grid));
        std::memset(dense, 0, sizeof(dense)); std::memset(sparse, 0, sizeof(sparse));
        hr = list->Close();
    }
    HANDLE event = nullptr;
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence.p));
    if (SUCCEEDED(hr)) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *commands[] = {list.p}; queue->ExecuteCommandLists(1, commands);
        hr = queue->Signal(fence.p, 1);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
        if (SUCCEEDED(hr) && WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) hr = E_FAIL;
    }
    if (event) CloseHandle(event);
    if (SUCCEEDED(hr)) hr = output->ReadFromSubresource(values, 64, 64, 0, nullptr);
    if (SUCCEEDED(hr) && (overdepth || gpu_multi_no_work)) {
        void *data = nullptr;
        hr = backing->Map(0, nullptr, &data);
        if (SUCCEEDED(hr)) {
            backing_unchanged = data && !std::memcmp(data, sentinel.data(), sentinel.size() * sizeof(uint32_t));
            backing->Unmap(0, nullptr);
        }
    }
    if (SUCCEEDED(hr) && empty_output) {
        void *data = nullptr;
        hr = backing->Map(0, nullptr, &data);
        if (SUCCEEDED(hr)) {
            if (data) {
                uint32_t counters[2]; std::memcpy(counters, data, sizeof(counters));
                empty_allocation_exact = counters[0] == ((zero || mismatch) ? 0u : 6u) && counters[1] == ((zero || mismatch) ? 0u : 1u);
            }
            backing->Unmap(0, nullptr);
        }
    }
    const uint32_t array_expected[16] = {101,202,303,404,505};
    const uint32_t gpu_broadcast_expected[16] = {808,1616,2424,3232,505};
    const uint32_t gpu_coalescing_expected[16] = {101,202,303,404,505,2,4};
    const uint32_t recursion_expected[16] = {4,3,2,1,0,99,14};
    const uint32_t fanout_expected[16] = {32,12,4,1,0,99,14};
    const uint32_t early_expected[16] = {0,0,2,1,0,99,12};
    const uint32_t overdepth_expected[16] = {0,0,0,0,0,99};
    const uint32_t gpu_multi_negative_expected[16] = {};
    const uint32_t gpu_replication_expected[16] = {404,0,0,0,505};
    const uint32_t gpu_replication_coalescing_expected[16] = {404,0,0,0,505,2,4};
    const uint32_t gpu_partial_coalescing_expected[16] = {303,0,0,0,505,2,6};
    const uint32_t gpu_empty_child_expected[16] = {0,0,0,0,505};
    const uint32_t gpu_duplicate_broadcast_expected[16] = {1616,3232,4848,6464};
    const uint32_t gpu_large_table_expected[16] = {3232,6464,9696,12928,2020};
    const uint32_t boundary_expected[16] = {528,496,32,UINT32_MAX,0,99,0xfffffffeu};
    const uint32_t coalescing_expected[16] = {22,18,14,10,0,99,14,0,8,4};
    const uint32_t empty_expected[16] = {12,4,20,3};
    const uint32_t empty_zero_expected[16] = {0,0,0,3};
    const uint32_t mismatch_expected[16] = {};
    const auto *expected = gpu_multi_no_work ? gpu_multi_negative_expected : gpu_multi_mode == 10u ? gpu_empty_child_expected : gpu_multi_mode == 12u ? gpu_duplicate_broadcast_expected : gpu_multi_mode == 14u ? gpu_large_table_expected : gpu_headers_broadcasting ? gpu_broadcast_expected : gpu_multi_mode == 8u ? gpu_partial_coalescing_expected : gpu_multi_mode == 7u ? (gpu_headers_coalescing ? gpu_replication_coalescing_expected : gpu_replication_expected) : mismatch ? mismatch_expected : empty_output ? (zero ? empty_zero_expected : empty_expected) : coalescing ? coalescing_expected : boundary ? boundary_expected : (overdepth ? overdepth_expected : (early ? early_expected : (fanout ? fanout_expected : (recursion ? recursion_expected : (gpu_headers_coalescing ? gpu_coalescing_expected : array_expected)))));
    std::fprintf(stderr, "array dispatch hr=%08x values=%u,%u,%u,%u,%u\n", unsigned(hr),values[0],values[1],values[2],values[3],values[4]);
    return SUCCEEDED(hr) && (!(overdepth || gpu_multi_no_work) || backing_unchanged) && (!empty_output || empty_allocation_exact) && std::memcmp(values, expected, sizeof(values)) == 0;
}
int main(int argc, char **argv) {
    if (argc != 2 && !(argc == 3 && (!std::strcmp(argv[2], "--dispatch") || !std::strcmp(argv[2], "--recursion") || !std::strcmp(argv[2], "--recursion-fanout") || !std::strcmp(argv[2], "--recursion-early") || !std::strcmp(argv[2], "--recursion-icb") || !std::strcmp(argv[2], "--recursion-overdepth") || !std::strcmp(argv[2], "--recursion-boundary") || !std::strcmp(argv[2], "--recursion-coalescing") || !std::strcmp(argv[2], "--empty-output") || !std::strcmp(argv[2], "--empty-output-zero") || !std::strcmp(argv[2], "--empty-output-mismatch") || !std::strcmp(argv[2], "--gpu-headers") || !std::strcmp(argv[2], "--gpu-headers-coalescing") || !std::strcmp(argv[2], "--gpu-headers-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers") || !std::strcmp(argv[2], "--gpu-multi-headers-invalid-entry") || !std::strcmp(argv[2], "--gpu-multi-headers-invalid-capacity") || !std::strcmp(argv[2], "--gpu-multi-headers-invalid-stride") || !std::strcmp(argv[2], "--gpu-multi-headers-invalid-records") || !std::strcmp(argv[2], "--gpu-multi-headers-empty") || !std::strcmp(argv[2], "--gpu-multi-headers-invalid-second") || !std::strcmp(argv[2], "--gpu-multi-headers-replication") || !std::strcmp(argv[2], "--gpu-multi-headers-replication-coalescing") || !std::strcmp(argv[2], "--gpu-multi-headers-partial-coalescing") || !std::strcmp(argv[2], "--gpu-multi-headers-misaligned-table") || !std::strcmp(argv[2], "--gpu-multi-headers-empty-child") || !std::strcmp(argv[2], "--gpu-multi-headers-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers-broadcasting-overflow") || !std::strcmp(argv[2], "--gpu-multi-headers-duplicate-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers-zero-table-stride") || !std::strcmp(argv[2], "--gpu-multi-headers-large-table")))) return 2;
    const bool fanout = argc == 3 && !std::strcmp(argv[2], "--recursion-fanout");
    const bool early = argc == 3 && !std::strcmp(argv[2], "--recursion-early");
    const bool icb = argc == 3 && !std::strcmp(argv[2], "--recursion-icb");
    const bool overdepth = argc == 3 && !std::strcmp(argv[2], "--recursion-overdepth");
    const bool boundary = argc == 3 && !std::strcmp(argv[2], "--recursion-boundary");
    const bool coalescing = argc == 3 && !std::strcmp(argv[2], "--recursion-coalescing");
    const bool recursion = coalescing || fanout || early || icb || overdepth || boundary || (argc == 3 && !std::strcmp(argv[2], "--recursion"));
    const bool zero = argc == 3 && !std::strcmp(argv[2], "--empty-output-zero");
    const bool mismatch = argc == 3 && !std::strcmp(argv[2], "--empty-output-mismatch");
    const bool empty_output = mismatch || zero || (argc == 3 && !std::strcmp(argv[2], "--empty-output"));
    const bool gpu_multi_headers = argc == 3 && std::strncmp(argv[2], "--gpu-multi-headers", 19) == 0;
    const bool gpu_headers_broadcasting = argc == 3 && (!std::strcmp(argv[2], "--gpu-headers-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers-broadcasting-overflow") || !std::strcmp(argv[2], "--gpu-multi-headers-duplicate-broadcasting") || !std::strcmp(argv[2], "--gpu-multi-headers-large-table"));
    const bool gpu_headers = argc == 3 && (!std::strcmp(argv[2], "--gpu-headers") || !std::strcmp(argv[2], "--gpu-headers-coalescing") || gpu_multi_headers || gpu_headers_broadcasting);
    const bool gpu_headers_coalescing = argc == 3 && (!std::strcmp(argv[2], "--gpu-headers-coalescing") || !std::strcmp(argv[2], "--gpu-multi-headers-replication-coalescing") || !std::strcmp(argv[2], "--gpu-multi-headers-partial-coalescing"));
    uint32_t gpu_multi_mode = 0;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-invalid-entry")) gpu_multi_mode = 1;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-invalid-capacity")) gpu_multi_mode = 2;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-invalid-stride")) gpu_multi_mode = 3;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-invalid-records")) gpu_multi_mode = 4;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-empty")) gpu_multi_mode = 5;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-invalid-second")) gpu_multi_mode = 6;
    if (argc == 3 && (!std::strcmp(argv[2], "--gpu-multi-headers-replication") || !std::strcmp(argv[2], "--gpu-multi-headers-replication-coalescing"))) gpu_multi_mode = 7;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-partial-coalescing")) gpu_multi_mode = 8;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-misaligned-table")) gpu_multi_mode = 9;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-empty-child")) gpu_multi_mode = 10;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-broadcasting-overflow")) gpu_multi_mode = 11;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-duplicate-broadcasting")) gpu_multi_mode = 12;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-zero-table-stride")) gpu_multi_mode = 13;
    if (argc == 3 && !std::strcmp(argv[2], "--gpu-multi-headers-large-table")) gpu_multi_mode = 14;
    const FixtureOptions options = {recursion,fanout,early,icb,overdepth,boundary,coalescing,empty_output,zero,mismatch,gpu_headers,gpu_headers_coalescing,gpu_multi_headers,gpu_headers_broadcasting,gpu_multi_mode};
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    HMODULE module = LoadLibraryW(L"d3d12.dll");
    using Create = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    auto create = module ? reinterpret_cast<Create>(reinterpret_cast<void *>(GetProcAddress(module, "D3D12CreateDevice"))) : nullptr;
    ID3D12Device5 *device = nullptr;
    HRESULT setup = create && !bytes.empty() ? create(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)) : E_FAIL;
    const wchar_t *names[] = {L"array_entry", L"sparse_entry", L"target_zero", L"target_one", L"target_two", L"target_sparse", L"target_three"};
    if (recursion) { names[0] = L"recursive"; names[1] = L"nonrecursive"; }
    if (empty_output) { names[0] = L"threadProducer"; names[1] = L"groupProducer"; names[2] = L"consume"; }
    const unsigned node_count = empty_output ? 3u : (recursion ? 2u : 7u);
    D3D12_EXPORT_DESC exports[7] = {};
    Node nodes[7] = {};
    for (unsigned i = 0; i < node_count; ++i) { exports[i].Name = names[i]; nodes[i].shader.shader = names[i]; }
    D3D12_DXIL_LIBRARY_DESC library = {};
    library.DXILLibrary = {bytes.data(), bytes.size()}; library.NumExports = node_count; library.pExports = exports;
    const NodeID entries[] = {{names[0], 0}, {names[1], 0}};
    WorkGraph graph = {L"arrays", 0, 2, entries, node_count, nodes};
    D3D12_STATE_SUBOBJECT graph_object = {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &graph};
    const D3D12_STATE_SUBOBJECT *graph_objects[] = {&graph_object};
    Generic generic = {L"arrays", 0, nullptr, 1, graph_objects};
    D3D12_STATE_SUBOBJECT objects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &generic}};
    D3D12_STATE_OBJECT_DESC description = {static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2, objects};
    ID3D12StateObject *valid = nullptr, *invalid = nullptr;
    HRESULT positive = SUCCEEDED(setup) ? device->CreateStateObject(&description, IID_PPV_ARGS(&valid)) : setup;
    graph.node_count = empty_output ? 2u : 6u; // Remove required dense element 3; sparse element 65536 stays.
    HRESULT negative = !recursion && SUCCEEDED(setup) ? device->CreateStateObject(&description, IID_PPV_ARGS(&invalid)) : S_FALSE;
    const bool creation_pass = positive == S_OK && valid && (recursion || (negative == E_FAIL && !invalid));
    const bool tested = argc == 3;
    uint32_t values[16] = {};
    bool backing_unchanged = false, empty_allocation_exact = false;
    const bool exact = creation_pass && tested && dispatch_arrays(device, valid, module, values, backing_unchanged, empty_allocation_exact, options);
    const bool pass = creation_pass && (!tested || exact);
    std::printf("{\"pass\":%s,\"creation_hr\":%u,\"missing_dense_hr\":%u,\"missing_dense_null\":%s,\"dispatch_tested\":%s,\"readback_exact\":%s,\"recursion\":%s,\"overdepth\":%s,\"backing_unchanged\":%s,\"empty_allocation_exact\":%s,\"gpu_generated_headers\":%s,\"gpu_header_coalescing\":%s,\"gpu_header_broadcasting\":%s,\"gpu_multi_headers\":%s,\"gpu_multi_mode\":%u,\"values\":[",
        pass ? "true" : "false", unsigned(positive), unsigned(negative), invalid ? "false" : "true", tested ? "true" : "false", exact ? "true" : "false", recursion ? "true" : "false", overdepth ? "true" : "false", backing_unchanged ? "true" : "false", empty_allocation_exact ? "true" : "false", gpu_headers ? "true" : "false", gpu_headers_coalescing ? "true" : "false", gpu_headers_broadcasting ? "true" : "false", gpu_multi_headers ? "true" : "false", gpu_multi_mode);
    for (unsigned i = 0; i < 16; ++i) std::printf("%s%u", i ? "," : "", values[i]);
    std::printf("]}\n");
    if (invalid) invalid->Release();
    if (valid) valid->Release();
    if (device) device->Release();
    if (module) FreeLibrary(module);
    return pass ? 0 : 1;
}
