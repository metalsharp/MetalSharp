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
static bool dispatch_arrays(ID3D12Device5 *device, ID3D12StateObject *state, HMODULE module, uint32_t (&values)[16]) {
    Owned<ID3D12CommandQueue> queue;
    Owned<ID3D12CommandAllocator> allocator;
    Owned<ID3D12GraphicsCommandList> list;
    Owned<dxmt::GraphicsCommandList10Extension> extension;
    Owned<ID3D12Resource> backing, output;
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
    uint32_t zero[16] = {};
    if (SUCCEEDED(hr)) hr = output->WriteToSubresource(0, nullptr, zero, 64, 64);
    using Serialize = HRESULT (WINAPI *)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
    auto serialize = reinterpret_cast<Serialize>(reinterpret_cast<void *>(GetProcAddress(module, "D3D12SerializeRootSignature")));
    D3D12_ROOT_PARAMETER parameter = {}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {}; root_desc.NumParameters = 1; root_desc.pParameters = &parameter;
    if (SUCCEEDED(hr)) hr = serialize ? serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob.p, nullptr) : E_FAIL;
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root.p));
    if (SUCCEEDED(hr)) hr = state->QueryInterface({0x460caac7,0x1d24,0x446a,{0xa1,0x84,0xca,0x67,0xdb,0x49,0x41,0x38}}, reinterpret_cast<void **>(&properties.p));
    Program program;
    if (SUCCEEDED(hr)) {
        using Get = void *(STDMETHODCALLTYPE *)(ID3D12StateObjectProperties *, void *, LPCWSTR);
        auto table = *reinterpret_cast<void ***>(properties.p);
        if (!reinterpret_cast<Get>(table[7])(properties.p, program.identifier, L"arrays")) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        program.backing = {backing->GetGPUVirtualAddress(), 2u << 20};
        extension->SetComputeRootSignature(root.p);
        extension->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress());
        extension->SetProgram(&program);
        uint32_t dense[4][2] = {{0,101},{1,202},{2,303},{3,404}};
        uint32_t sparse[4][2] = {{65536,505},{1,999},{0,888},{UINT32_MAX,777}};
        GraphDispatch dispatch; dispatch.input = {0,4,dense,8};
        extension->DispatchGraph(&dispatch);
        dispatch.input = {1,4,sparse,8};
        extension->DispatchGraph(&dispatch);
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
    const uint32_t expected[16] = {101,202,303,404,505};
    std::fprintf(stderr, "array dispatch hr=%08x values=%u,%u,%u,%u,%u\n", unsigned(hr),values[0],values[1],values[2],values[3],values[4]);
    return SUCCEEDED(hr) && std::memcmp(values, expected, sizeof(values)) == 0;
}
int main(int argc, char **argv) {
    if (argc != 2 && !(argc == 3 && !std::strcmp(argv[2], "--dispatch"))) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    HMODULE module = LoadLibraryW(L"d3d12.dll");
    using Create = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    auto create = module ? reinterpret_cast<Create>(reinterpret_cast<void *>(GetProcAddress(module, "D3D12CreateDevice"))) : nullptr;
    ID3D12Device5 *device = nullptr;
    HRESULT setup = create && !bytes.empty() ? create(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)) : E_FAIL;
    const wchar_t *names[] = {L"array_entry", L"sparse_entry", L"target_zero", L"target_one", L"target_two", L"target_sparse", L"target_three"};
    D3D12_EXPORT_DESC exports[7] = {};
    Node nodes[7] = {};
    for (unsigned i = 0; i < 7; ++i) { exports[i].Name = names[i]; nodes[i].shader.shader = names[i]; }
    D3D12_DXIL_LIBRARY_DESC library = {};
    library.DXILLibrary = {bytes.data(), bytes.size()}; library.NumExports = 7; library.pExports = exports;
    const NodeID entries[] = {{L"array_entry", 0}, {L"sparse_entry", 0}};
    WorkGraph graph = {L"arrays", 0, 2, entries, 7, nodes};
    D3D12_STATE_SUBOBJECT graph_object = {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &graph};
    const D3D12_STATE_SUBOBJECT *graph_objects[] = {&graph_object};
    Generic generic = {L"arrays", 0, nullptr, 1, graph_objects};
    D3D12_STATE_SUBOBJECT objects[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
        {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &generic}};
    D3D12_STATE_OBJECT_DESC description = {static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2, objects};
    ID3D12StateObject *valid = nullptr, *invalid = nullptr;
    HRESULT positive = SUCCEEDED(setup) ? device->CreateStateObject(&description, IID_PPV_ARGS(&valid)) : setup;
    graph.node_count = 6; // Remove required dense element 3; sparse element 65536 stays.
    HRESULT negative = SUCCEEDED(setup) ? device->CreateStateObject(&description, IID_PPV_ARGS(&invalid)) : setup;
    const bool creation_pass = positive == S_OK && valid && negative == E_FAIL && !invalid;
    const bool tested = argc == 3;
    uint32_t values[16] = {};
    const bool exact = creation_pass && tested && dispatch_arrays(device, valid, module, values);
    const bool pass = creation_pass && (!tested || exact);
    std::printf("{\"pass\":%s,\"creation_hr\":%u,\"missing_dense_hr\":%u,\"missing_dense_null\":%s,\"dispatch_tested\":%s,\"readback_exact\":%s,\"values\":[",
        pass ? "true" : "false", unsigned(positive), unsigned(negative), invalid ? "false" : "true", tested ? "true" : "false", exact ? "true" : "false");
    for (unsigned i = 0; i < 16; ++i) std::printf("%s%u", i ? "," : "", values[i]);
    std::printf("]}\n");
    if (invalid) invalid->Release();
    if (valid) valid->Release();
    if (device) device->Release();
    if (module) FreeLibrary(module);
    return pass ? 0 : 1;
}
