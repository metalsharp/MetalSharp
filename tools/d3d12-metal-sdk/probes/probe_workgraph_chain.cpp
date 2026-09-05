#define WIN32_LEAN_AND_MEAN
#include "d3d12_command_list_extensions.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <d3d12.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <windows.h>

struct NodeID {
    LPCWSTR Name;
    UINT ArrayIndex;
};
struct ShaderNode {
    LPCWSTR Shader;
    UINT OverridesType;
    const void* Overrides;
};
struct Node {
    UINT NodeType;
    ShaderNode Shader;
};
struct WorkGraphDesc {
    LPCWSTR ProgramName;
    UINT Flags;
    UINT NumEntrypoints;
    const NodeID* Entrypoints;
    UINT NumExplicitlyDefinedNodes;
    const Node* Nodes;
};
struct GenericDesc {
    LPCWSTR ProgramName;
    UINT NumExports;
    LPCWSTR* Exports;
    UINT NumSubobjects;
    const D3D12_STATE_SUBOBJECT* const* Subobjects;
};
struct Range {
    D3D12_GPU_VIRTUAL_ADDRESS StartAddress;
    UINT64 SizeInBytes;
};
struct RangeStride {
    D3D12_GPU_VIRTUAL_ADDRESS StartAddress;
    UINT64 SizeInBytes;
    UINT64 StrideInBytes;
};
struct WorkGraphSet {
    uint64_t ProgramIdentifier[4];
    UINT Flags;
    UINT Padding;
    Range BackingMemory;
    RangeStride NodeLocalRootArgumentsTable;
};
struct SetProgram {
    UINT Type;
    UINT Padding;
    union {
        uint8_t generic[40];
        WorkGraphSet work;
    };
};
struct CPUInput {
    UINT EntrypointIndex;
    UINT NumRecords;
    const void* Records;
    UINT64 RecordStrideInBytes;
};
struct Dispatch {
    UINT Mode;
    UINT Padding;
    CPUInput NodeCPUInput;
};
using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using SerializeRS = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                     ID3DBlob**);
template <class T> void rel(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}
template <class T> T proc(HMODULE m, const char* n) {
    FARPROC p = GetProcAddress(m, n);
    T x = nullptr;
    static_assert(sizeof x == sizeof p);
    std::memcpy(&x, &p, sizeof x);
    return x;
}
static bool read(const char* p, std::vector<uint8_t>& v) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    v.assign(std::istreambuf_iterator<char>(f), {});
    return !v.empty();
}
static D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h = {};
    h.Type = t;
    h.CreationNodeMask = 1;
    h.VisibleNodeMask = 1;
    return h;
}
static D3D12_RESOURCE_DESC buf(UINT64 n, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = n;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}
static HRESULT wait(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12GraphicsCommandList* l,
                    ID3D12Fence* release_producer = nullptr, bool* blocked = nullptr) {
    HRESULT h = l->Close();
    if (FAILED(h))
        return h;
    ID3D12CommandList* ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    ID3D12Fence* f = nullptr;
    h = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f));
    HANDLE e = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!e && SUCCEEDED(h))
        h = HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(h))
        h = q->Signal(f, 1);
    if (SUCCEEDED(h))
        h = f->SetEventOnCompletion(1, e);
    if (release_producer) {
        // The consumer completion must remain pending while the producer is
        // gated. This observes only a fence, never intermediate record data.
        if (SUCCEEDED(h)) {
            const bool pending = WaitForSingleObject(e, 100) == WAIT_TIMEOUT;
            if (blocked)
                *blocked = pending;
            if (!pending)
                h = E_FAIL;
        }
        const HRESULT released = release_producer->Signal(1);
        if (SUCCEEDED(h))
            h = released;
    }
    if (SUCCEEDED(h) && WaitForSingleObject(e, 10000) != WAIT_OBJECT_0)
        h = E_FAIL;
    if (e)
        CloseHandle(e);
    rel(f);
    return h;
}
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "chain";
    const bool zero_grid = std::strcmp(mode, "zero-grid") == 0;
    const bool fixed_consumer_empty = std::strcmp(mode, "fixed-consumer-empty") == 0;
    const bool dynamic_consumer_empty = std::strcmp(mode, "dynamic-consumer-empty") == 0;
    const bool empty_grid = dynamic_consumer_empty || fixed_consumer_empty || std::strcmp(mode, "empty-grid") == 0;
    const bool offset_grid = std::strcmp(mode, "offset-grid") == 0;
    const bool cross_queue_dynamic = std::strcmp(mode, "cross-queue-dynamic") == 0;
    const bool cross_queue = cross_queue_dynamic || std::strcmp(mode, "cross-queue-grid") == 0;
    const bool gpu_copy = cross_queue || std::strcmp(mode, "gpu-copy-grid") == 0;
    const bool gpu_grid = gpu_copy || std::strcmp(mode, "gpu-vector-grid") == 0;
    const bool u16_grid = std::strcmp(mode, "u16-grid") == 0;
    const bool vector_grid = u16_grid || gpu_grid || std::strcmp(mode, "vector-grid") == 0;
    const bool capacity = std::strcmp(mode, "fanout-capacity") == 0;
    const bool fanout = capacity || std::strcmp(mode, "fanout") == 0;
    const bool cycle = std::strcmp(mode, "cycle") == 0;
    const bool oversized_output = std::strcmp(mode, "oversized-output") == 0;
    const bool unsupported_target = oversized_output || cycle || std::strcmp(mode, "unsupported-target") == 0;
    const bool varying_lanes = std::strcmp(mode, "varying-lanes") == 0;
    const bool fixed_consumer = varying_lanes || fixed_consumer_empty || std::strcmp(mode, "fixed-consumer") == 0;
    const bool dynamic_output = std::strcmp(mode, "dynamic-output") == 0;
    const bool dynamic_thread_output = std::strcmp(mode, "dynamic-thread-output") == 0;
    const bool program_isolation = std::strcmp(mode, "program-isolation") == 0;
    const bool dynamic_consumer_repeated = std::strcmp(mode, "dynamic-consumer-repeated") == 0;
    const bool repeated = dynamic_consumer_repeated || program_isolation || std::strcmp(mode, "repeated") == 0;
    const bool dynamic_zero_grids = std::strcmp(mode, "dynamic-consumer-zero-grids") == 0;
    const bool dynamic_consumer_u16 = std::strcmp(mode, "dynamic-consumer-u16") == 0;
    const bool fanout_icb = std::strcmp(mode, "fanout-icb") == 0;
    const bool conditional_icb = std::strcmp(mode, "conditional-icb") == 0;
    const bool dynamic_consumer = fanout_icb || conditional_icb || cross_queue_dynamic || dynamic_consumer_repeated ||
                                  dynamic_consumer_u16 || dynamic_zero_grids || dynamic_consumer_empty ||
                                  std::strcmp(mode, "dynamic-consumer") == 0;
    if (!dynamic_consumer && !repeated && !dynamic_thread_output && !dynamic_output && !zero_grid && !empty_grid &&
        !offset_grid && !vector_grid && !fanout && !unsupported_target && !fixed_consumer && std::strcmp(mode, "chain"))
        return 2;
    HMODULE m = LoadLibraryA("d3d12.dll");
    auto cd = proc<CreateDevice>(m, "D3D12CreateDevice");
    auto srs = proc<SerializeRS>(m, "D3D12SerializeRootSignature");
    ID3D12Device* d = nullptr;
    HRESULT h = cd && srs ? cd(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)) : E_FAIL;
    ID3D12Device5* d5 = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandQueue* producer_queue = nullptr;
    ID3D12CommandAllocator* producer_allocator = nullptr;
    ID3D12GraphicsCommandList* producer_list = nullptr;
    ID3D12Fence *producer_gate = nullptr, *producer_done = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    dxmt::GraphicsCommandList10Extension* x = nullptr;
    ID3D12Resource *back = nullptr, *out = nullptr, *gpu_input = nullptr, *gpu_payload = nullptr;
    ID3DBlob* blob = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12DescriptorHeap* dh = nullptr;
    ID3D12StateObject* state = nullptr;
    ID3D12StateObject* alternate_state = nullptr;
    ID3D12StateObjectProperties* alternate_props = nullptr;
    ID3D12StateObjectProperties* props = nullptr;
    if (SUCCEEDED(h))
        h = d->QueryInterface(IID_PPV_ARGS(&d5));
    D3D12_COMMAND_QUEUE_DESC qd = {};
    if (SUCCEEDED(h))
        h = d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (SUCCEEDED(h))
        h = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    if (SUCCEEDED(h))
        h = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    if (SUCCEEDED(h))
        h = l->QueryInterface(dxmt::kID3D12GraphicsCommandList10, reinterpret_cast<void**>(&x));
    auto uh = heap(D3D12_HEAP_TYPE_UPLOAD);
    if (SUCCEEDED(h)) {
        auto z = buf(2u << 20);
        h = d->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &z, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&back));
    }
    auto def = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (SUCCEEDED(h)) {
        auto z = buf(64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        h = d->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &z, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_PPV_ARGS(&out));
    }
    if (SUCCEEDED(h)) {
        D3D12_ROOT_PARAMETER p = {};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.Descriptor.ShaderRegister = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 1;
        rd.pParameters = &p;
        h = srs(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr);
        if (SUCCEEDED(h))
            h = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    }
    if (SUCCEEDED(h)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = 1;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        h = d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dh));
        if (SUCCEEDED(h)) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.Buffer.NumElements = 16;
            ud.Buffer.StructureByteStride = 4;
            d->CreateUnorderedAccessView(out, nullptr, &ud, dh->GetCPUDescriptorHandleForHeapStart());
        }
    }
    std::vector<uint8_t> cso;
    if (!read(fanout_icb              ? "probe_workgraph_chain_fanout_icb.cso"
              : conditional_icb       ? "probe_workgraph_chain_conditional_icb.cso"
              : cross_queue_dynamic   ? "probe_workgraph_chain_cross_queue_dynamic.cso"
              : dynamic_consumer_u16  ? "probe_workgraph_chain_dynamic_consumer_u16.cso"
              : dynamic_zero_grids    ? "probe_workgraph_chain_dynamic_zero_grids.cso"
              : dynamic_consumer      ? "probe_workgraph_chain_dynamic_consumer.cso"
              : varying_lanes         ? "probe_workgraph_chain_varying_lanes.cso"
              : dynamic_thread_output ? "probe_workgraph_chain_dynamic_thread_output.cso"
              : dynamic_output        ? "probe_workgraph_chain_dynamic_output.cso"
              : fixed_consumer        ? "probe_workgraph_chain_fixed_consumer.cso"
              : oversized_output      ? "probe_workgraph_chain_oversized.cso"
              : u16_grid              ? "probe_workgraph_chain_u16.cso"
              : cycle                 ? "probe_workgraph_chain_cycle.cso"
              : unsupported_target    ? "probe_workgraph_chain_bad_target.cso"
              : fanout                ? "probe_workgraph_chain_fanout.cso"
              : vector_grid           ? "probe_workgraph_chain_vector.cso"
              : offset_grid           ? "probe_workgraph_chain_offset.cso"
                                      : "probe_workgraph_chain.cso",
              cso))
        h = E_FAIL;
    const wchar_t* exports[3] = {L"firstNode", L"secondNode", L"thirdNode"};
    D3D12_EXPORT_DESC ex[3] = {};
    for (int i = 0; i < 3; i++)
        ex[i].Name = exports[i];
    D3D12_DXIL_LIBRARY_DESC lib = {};
    lib.DXILLibrary = {cso.data(), cso.size()};
    lib.NumExports = 3;
    lib.pExports = ex;
    NodeID entry = {L"firstNode", 0};
    Node nodes[3] = {};
    nodes[0].Shader.Shader = L"firstNode";
    nodes[1].Shader.Shader = L"secondNode";
    nodes[2].Shader.Shader = L"thirdNode";
    WorkGraphDesc wg = {L"chain_graph", 0, 1, &entry, 3, nodes};
    D3D12_STATE_SUBOBJECT wgs = {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(13), &wg};
    const D3D12_STATE_SUBOBJECT* wgsubs[] = {&wgs};
    GenericDesc gen = {L"chain_graph", 0, nullptr, 1, wgsubs};
    D3D12_STATE_SUBOBJECT subs[2] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                     {static_cast<D3D12_STATE_SUBOBJECT_TYPE>(29), &gen}};
    D3D12_STATE_OBJECT_DESC sd = {static_cast<D3D12_STATE_OBJECT_TYPE>(4), 2, subs};
    if (SUCCEEDED(h))
        h = d5->CreateStateObject(&sd, IID_PPV_ARGS(&state));
    uint8_t id[32] = {};
    if (SUCCEEDED(h))
        h = state->QueryInterface({0x460caac7, 0x1d24, 0x446a, {0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41, 0x38}},
                                  reinterpret_cast<void**>(&props));
    if (SUCCEEDED(h)) {
        using G = void*(STDMETHODCALLTYPE*)(ID3D12StateObjectProperties*, void*, LPCWSTR);
        auto vt = *reinterpret_cast<void***>(props);
        reinterpret_cast<G>(vt[7])(props, id, L"chain_graph");
    }
    uint8_t alternate_id[32] = {};
    bool identifiers_distinct = false, identifiers_stable = false;
    if (SUCCEEDED(h) && program_isolation) {
        std::vector<uint8_t> alternate_cso;
        if (!read("probe_workgraph_chain_second_program.cso", alternate_cso))
            h = E_FAIL;
        if (SUCCEEDED(h)) {
            lib.DXILLibrary = {alternate_cso.data(), alternate_cso.size()};
            h = d5->CreateStateObject(&sd, IID_PPV_ARGS(&alternate_state));
        }
        if (SUCCEEDED(h))
            h = alternate_state->QueryInterface(
                {0x460caac7, 0x1d24, 0x446a, {0xa1, 0x84, 0xca, 0x67, 0xdb, 0x49, 0x41, 0x38}},
                reinterpret_cast<void**>(&alternate_props));
        if (SUCCEEDED(h)) {
            using G = void*(STDMETHODCALLTYPE*)(ID3D12StateObjectProperties*, void*, LPCWSTR);
            auto get_id = [](ID3D12StateObjectProperties* p, uint8_t* dst) {
                auto vt = *reinterpret_cast<void***>(p);
                reinterpret_cast<G>(vt[7])(p, dst, L"chain_graph");
            };
            uint8_t first_again[32] = {}, second_again[32] = {};
            get_id(alternate_props, alternate_id);
            get_id(props, first_again);
            get_id(alternate_props, second_again);
            identifiers_distinct = std::memcmp(id, alternate_id, 32) != 0;
            identifiers_stable = !std::memcmp(id, first_again, 32) && !std::memcmp(alternate_id, second_again, 32);
        }
    }
    SetProgram sp = {};
    sp.Type = 5;
    std::memcpy(sp.work.ProgramIdentifier, id, 32);
    sp.work.BackingMemory.StartAddress = back ? back->GetGPUVirtualAddress() : 0;
    sp.work.BackingMemory.SizeInBytes = 2u << 20;
    uint32_t input_records[8] = {2, 0, 1, 1, 2, 2, 1, 3};
    if (capacity)
        for (unsigned i = 0; i < 4; ++i)
            input_records[i * 2] = 16;
    if (zero_grid || empty_grid)
        input_records[2] = input_records[6] = 0;
    if (empty_grid)
        input_records[0] = input_records[4] = 0;
    const uint32_t offset_records[16] = {0, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 3, 1, 0, 0};
    const uint32_t vector_records[16] = {0, 2, 2, 1, 1, 1, 1, 2, 2, 2, 1, 1, 3, 1, 1, 1};
    CPUInput ci = {0, 4, input_records, 8};
    if (vector_grid) {
        ci.Records = vector_records;
        ci.RecordStrideInBytes = 16;
    }
    const uint16_t u16_records[16] = {0, 2, 2, 1, 1, 1, 1, 2, 2, 2, 1, 1, 3, 1, 1, 1};
    if (u16_grid) {
        ci.Records = u16_records;
        ci.RecordStrideInBytes = 8;
    }
    if (offset_grid) {
        ci.Records = offset_records;
        ci.RecordStrideInBytes = 16;
    }
    bool payload_mutated_after_recording = false;
    bool consumer_blocked_until_release = false;
    Dispatch dg = {0, 0, ci};
    if (SUCCEEDED(h) && cross_queue) {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        h = d->CreateCommandQueue(&desc, IID_PPV_ARGS(&producer_queue));
        if (SUCCEEDED(h))
            h = d->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&producer_allocator));
        if (SUCCEEDED(h))
            h = d->CreateCommandList(0, desc.Type, producer_allocator, nullptr, IID_PPV_ARGS(&producer_list));
        if (SUCCEEDED(h))
            h = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer_gate));
        if (SUCCEEDED(h))
            h = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer_done));
    }
    if (SUCCEEDED(h) && gpu_copy) {
        const auto desc = buf(64);
        h = d->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&gpu_payload));
    }
    if (SUCCEEDED(h) && gpu_grid) {
        const auto desc = buf(128);
        h = d->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_PPV_ARGS(&gpu_input));
        if (SUCCEEDED(h)) {
            struct GPUInput {
                UINT entry;
                UINT count;
                UINT64 records;
                UINT64 stride;
            };
            static_assert(sizeof(GPUInput) == 24, "GPU input ABI");
            const GPUInput input = {
                0, 4, gpu_copy ? gpu_payload->GetGPUVirtualAddress() : gpu_input->GetGPUVirtualAddress() + 64, 16};
            void* mapped = nullptr;
            h = gpu_input->Map(0, nullptr, &mapped);
            if (SUCCEEDED(h) && mapped) {
                // Empty grids at recording time; payload is published below.
                std::memset(mapped, 0, 128);
                std::memcpy(mapped, &input, sizeof(input));
                gpu_input->Unmap(0, nullptr);
                dg = {};
                dg.Mode = 1;
                const UINT64 address = gpu_input->GetGPUVirtualAddress();
                std::memcpy(&dg.NodeCPUInput, &address, sizeof(address));
            } else
                h = E_FAIL;
        }
    }
    if (SUCCEEDED(h)) {
        void* mapped = nullptr;
        h = back->Map(0, nullptr, &mapped);
        if (SUCCEEDED(h) && mapped) {
            std::memset(mapped, 0, 2u << 20);
            back->Unmap(0, nullptr);
        } else
            h = E_FAIL;
    }
    if (SUCCEEDED(h)) {
        x->SetDescriptorHeaps(1, &dh);
        const UINT zeros[4] = {};
        x->ClearUnorderedAccessViewUint(dh->GetGPUDescriptorHandleForHeapStart(),
                                        dh->GetCPUDescriptorHandleForHeapStart(), out, zeros, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = out;
        x->ResourceBarrier(1, &barrier);
        x->SetComputeRootSignature(rs);
        x->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
        x->SetProgram(&sp);
        if (gpu_copy) {
            ID3D12GraphicsCommandList* copy_list = cross_queue ? producer_list : l;
            copy_list->CopyBufferRegion(gpu_payload, 0, gpu_input, 64, 64);
            D3D12_RESOURCE_BARRIER ready = {};
            ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            ready.Transition.pResource = gpu_payload;
            ready.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ready.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            ready.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            copy_list->ResourceBarrier(1, &ready);
        }
        x->DispatchGraph(&dg);
        if (repeated) {
            uint32_t later_records[8] = {1, 0, 1, 1, 1, 2, 1, 3};
            Dispatch later = {0, 0, {0, 4, later_records, 8}};
            if (program_isolation) {
                SetProgram alternate = sp;
                std::memcpy(alternate.work.ProgramIdentifier, alternate_id, 32);
                x->SetProgram(&alternate);
            }
            x->DispatchGraph(&later);
            // CPU-mode records must already be owned by the recorded list.
            volatile uint32_t* mutation = later_records;
            for (unsigned i = 0; i < 4; ++i)
                mutation[2 * i] = 16;
            payload_mutated_after_recording = true;
        }
        if (gpu_grid) {
            void* mapped = nullptr;
            h = gpu_input->Map(0, nullptr, &mapped);
            if (SUCCEEDED(h) && mapped) {
                std::memcpy(static_cast<uint8_t*>(mapped) + 64, vector_records, sizeof(vector_records));
                payload_mutated_after_recording = true;
                gpu_input->Unmap(0, nullptr);
            } else
                h = E_FAIL;
        }
        if (SUCCEEDED(h) && cross_queue) {
            h = producer_list->Close();
            if (SUCCEEDED(h))
                h = producer_queue->Wait(producer_gate, 1);
            if (SUCCEEDED(h)) {
                ID3D12CommandList* lists[] = {producer_list};
                producer_queue->ExecuteCommandLists(1, lists);
                h = producer_queue->Signal(producer_done, 1);
            }
            if (SUCCEEDED(h))
                h = q->Wait(producer_done, 1);
        }
        if (SUCCEEDED(h))
            h = wait(d, q, l, producer_gate, &consumer_blocked_until_release);
    }
    if (SUCCEEDED(h) && program_isolation) {
        // Both first submissions have completed. Destroy only the second
        // state, then reuse the first identifier without registering it again.
        rel(alternate_props);
        rel(alternate_state);
        h = a->Reset();
        if (SUCCEEDED(h))
            h = l->Reset(a, nullptr);
        if (SUCCEEDED(h)) {
            x->SetComputeRootSignature(rs);
            x->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
            x->SetProgram(&sp);
            x->DispatchGraph(&dg);
            h = wait(d, q, l);
        }
    }
    if (SUCCEEDED(h) && program_isolation) {
        // A retired identifier must not execute its old shader or the
        // library-free reference fallback. Preserve the prior exact result.
        h = a->Reset();
        if (SUCCEEDED(h))
            h = l->Reset(a, nullptr);
        if (SUCCEEDED(h)) {
            SetProgram retired = sp;
            std::memcpy(retired.work.ProgramIdentifier, alternate_id, 32);
            x->SetComputeRootSignature(rs);
            x->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
            x->SetProgram(&retired);
            x->DispatchGraph(&dg);
            h = wait(d, q, l);
        }
    }
    uint32_t values[16] = {};
    if (SUCCEEDED(h))
        h = out->ReadFromSubresource(values, 64, 64, 0, nullptr);
    uint32_t words[20] = {};
    if (SUCCEEDED(h) && back) {
        void* ptr = nullptr;
        h = back->Map(0, nullptr, &ptr);
        if (SUCCEEDED(h) && ptr) {
            std::memcpy(words, ptr, sizeof(words));
            back->Unmap(0, nullptr);
        } else if (SUCCEEDED(h))
            h = E_FAIL;
    }
    uint32_t expected[16] = {18, 5, 18, 5, 4, 2, 4, 2};
    if (zero_grid)
        expected[1] = expected[3] = expected[5] = expected[7] = 0;
    if (empty_grid || unsupported_target)
        std::memset(expected, 0, sizeof(expected));
    if (vector_grid) {
        expected[0] = 36;
        expected[1] = 10;
        expected[4] = 8;
        expected[5] = 4;
    }
    if (fanout) {
        expected[4] = 6;
        expected[5] = 3;
        expected[6] = 6;
        expected[7] = 3;
    }
    if (capacity)
        for (unsigned i = 0; i < 4; ++i) {
            expected[i] = 1040;
            expected[4 + i] = 48;
        }
    if (fixed_consumer && !empty_grid) {
        const uint32_t broadcast_expected[8] = {35904, 17888, 35904, 17888, 64, 32, 64, 32};
        std::memcpy(expected, broadcast_expected, sizeof(broadcast_expected));
    }
    if (dynamic_output)
        expected[0] = expected[2] = expected[4] = expected[6] = 0;
    if (dynamic_thread_output) {
        expected[4] = 2;
        expected[5] = 1;
        expected[6] = 2;
        expected[7] = 1;
    }
    if (varying_lanes) {
        expected[4] = 32;
        expected[5] = 16;
        expected[6] = 32;
        expected[7] = 16;
    }
    if (repeated) {
        const uint32_t repeated_expected[8] = {23, 10, 23, 10, 6, 4, 6, 4};
        std::memcpy(expected, repeated_expected, sizeof(repeated_expected));
    }
    if (program_isolation) {
        const uint32_t isolated_expected[8] = {241, 215, 241, 215, 10, 6, 10, 6};
        std::memcpy(expected, isolated_expected, sizeof(isolated_expected));
    }
    if (dynamic_consumer && !empty_grid) {
        const uint32_t dynamic_expected[8] = {474, 510, 12948, 13020, 12, 12, 24, 24};
        std::memcpy(expected, dynamic_expected, sizeof(dynamic_expected));
    }
    if (dynamic_zero_grids)
        for (unsigned i = 0; i < 3; ++i)
            expected[i] = expected[4 + i] = 0;
    if (dynamic_consumer_repeated) {
        const uint32_t repeated_dynamic[8] = {699, 1020, 19398, 26040, 18, 24, 36, 48};
        std::memcpy(expected, repeated_dynamic, sizeof(repeated_dynamic));
    }
    if (cross_queue_dynamic) {
        const uint32_t cross_expected[8] = {948, 1020, 12948, 13020, 24, 24, 24, 24};
        std::memcpy(expected, cross_expected, sizeof(cross_expected));
    }
    if (conditional_icb)
        expected[0] = expected[2] = expected[4] = expected[6] = 0;
    if (fanout_icb) {
        expected[4] += 2;
        expected[5] += 1;
        expected[6] += 2;
        expected[7] += 1;
    }
    const bool readback_exact = SUCCEEDED(h) && !std::memcmp(values, expected, sizeof(values));
    // Default: six groups allocate two records each, then twelve thread
    // allocations. Vector grids launch nine groups; zero modes launch fewer.
    const bool allocations_exact = SUCCEEDED(h) &&
                                   words[0] == (fanout_icb                           ? 90u
                                                : conditional_icb                    ? 40u
                                                : cross_queue_dynamic                ? 114u
                                                : dynamic_consumer_repeated          ? 62u
                                                : dynamic_zero_grids                 ? 36u
                                                : (dynamic_consumer && !empty_grid)  ? 84u
                                                : program_isolation                  ? 24u
                                                : repeated                           ? 16u
                                                : varying_lanes                      ? 108u
                                                : dynamic_thread_output              ? 18u
                                                : dynamic_output                     ? 8u
                                                : (fixed_consumer && !empty_grid)    ? 204u
                                                : (empty_grid || unsupported_target) ? 0u
                                                : capacity                           ? 320u
                                                : fanout                             ? 30u
                                                : vector_grid                        ? 36u
                                                : zero_grid                          ? 16u
                                                                                     : 24u) &&
                                   words[1] == (fanout_icb                           ? 84u
                                                : conditional_icb                    ? 38u
                                                : cross_queue_dynamic                ? 105u
                                                : dynamic_consumer_repeated          ? 58u
                                                : dynamic_zero_grids                 ? 30u
                                                : (dynamic_consumer && !empty_grid)  ? 78u
                                                : program_isolation                  ? 18u
                                                : repeated                           ? 12u
                                                : varying_lanes                      ? 102u
                                                : dynamic_thread_output              ? 12u
                                                : dynamic_output                     ? 6u
                                                : (fixed_consumer && !empty_grid)    ? 198u
                                                : (empty_grid || unsupported_target) ? 0u
                                                : capacity                           ? 256u
                                                : fanout                             ? 24u
                                                : vector_grid                        ? 27u
                                                : zero_grid                          ? 12u
                                                                                     : 18u);
    bool exact = readback_exact && allocations_exact && (!gpu_grid || payload_mutated_after_recording) &&
                 (!cross_queue || consumer_blocked_until_release) &&
                 (!program_isolation || (identifiers_distinct && identifiers_stable));
    FILE* result = stdout;
    if (result) {
        std::fprintf(result,
                     "{\"schema\":\"metalsharp.workgraph-chain.v1\",\"pass\":%s,"
                     "\"hr\":\"0x%08x\",\"cpu_scheduler\":false,\"readback_exact\":%s,"
                     "\"allocations_exact\":%s,\"gpu_input\":%s,\"post_recording_mutation\":%s,\"queued_gpu_copy\":%s,"
                     "\"cross_queue_dependency\":%s,\"consumer_blocked_until_release\":%s,"
                     "\"identifiers_distinct\":%s,\"identifiers_stable\":%s,\"values\":[",
                     exact ? "true" : "false", (unsigned)h, readback_exact ? "true" : "false",
                     allocations_exact ? "true" : "false", gpu_grid ? "true" : "false",
                     payload_mutated_after_recording ? "true" : "false", gpu_copy ? "true" : "false",
                     cross_queue ? "true" : "false", consumer_blocked_until_release ? "true" : "false",
                     identifiers_distinct ? "true" : "false", identifiers_stable ? "true" : "false");
        for (unsigned i = 0; i < 16; ++i)
            std::fprintf(result, "%s%u", i ? "," : "", values[i]);
        std::fprintf(result, "]}\n");
        if (result != stdout && std::fclose(result))
            exact = false;
    } else
        exact = false;
    // Unblock a queued producer on error paths as well.
    if (producer_gate)
        producer_gate->Signal(1);
    rel(producer_done);
    rel(producer_gate);
    rel(producer_list);
    rel(producer_allocator);
    rel(producer_queue);
    rel(props);
    rel(alternate_props);
    rel(alternate_state);
    rel(state);
    rel(dh);
    rel(rs);
    rel(blob);
    rel(out);
    rel(gpu_payload);
    rel(gpu_input);
    rel(back);
    rel(x);
    rel(l);
    rel(a);
    rel(q);
    rel(d5);
    rel(d);
    if (m)
        FreeLibrary(m);
    return exact ? 0 : 1;
}
