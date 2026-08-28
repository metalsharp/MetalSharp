#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

static std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
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

static void print_hr(const char* key, HRESULT hr, bool trailing_comma = true) {
    std::printf("    \"%s\": \"0x%08lx\"%s\n", key, static_cast<unsigned long>(hr), trailing_comma ? "," : "");
}

static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

static D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

static HRESULT create_queue(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue** queue) {
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;
    return device ? device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue)) : E_FAIL;
}

static HRESULT create_allocator_and_list(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type,
                                         ID3D12CommandAllocator** allocator, ID3D12GraphicsCommandList** list) {
    HRESULT allocator_hr = device ? device->CreateCommandAllocator(type, IID_PPV_ARGS(allocator)) : E_FAIL;
    HRESULT list_hr = (device && SUCCEEDED(allocator_hr))
                          ? device->CreateCommandList(0, type, *allocator, nullptr, IID_PPV_ARGS(list))
                          : E_FAIL;
    return FAILED(allocator_hr) ? allocator_hr : list_hr;
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)) : E_FAIL;

    ID3D12CommandQueue* direct_queue = nullptr;
    ID3D12CommandQueue* present_queue = nullptr;
    ID3D12CommandQueue* compute_queue = nullptr;
    ID3D12CommandQueue* copy_queue = nullptr;
    HRESULT direct_queue_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &direct_queue);
    HRESULT present_queue_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &present_queue);
    HRESULT compute_queue_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &compute_queue);
    HRESULT copy_queue_hr = create_queue(device, D3D12_COMMAND_LIST_TYPE_COPY, &copy_queue);

    ID3D12CommandAllocator* render_allocator = nullptr;
    ID3D12GraphicsCommandList* render_list = nullptr;
    ID3D12CommandAllocator* present_allocator = nullptr;
    ID3D12GraphicsCommandList* present_list = nullptr;
    ID3D12CommandAllocator* compute_allocator = nullptr;
    ID3D12GraphicsCommandList* compute_list = nullptr;
    ID3D12CommandAllocator* copy_allocator = nullptr;
    ID3D12GraphicsCommandList* copy_list = nullptr;
    HRESULT render_objects_hr =
        create_allocator_and_list(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &render_allocator, &render_list);
    HRESULT present_objects_hr =
        create_allocator_and_list(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &present_allocator, &present_list);
    HRESULT compute_objects_hr =
        create_allocator_and_list(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &compute_allocator, &compute_list);
    HRESULT copy_objects_hr =
        create_allocator_and_list(device, D3D12_COMMAND_LIST_TYPE_COPY, &copy_allocator, &copy_list);

    ID3D12Fence* copy_fence = nullptr;
    ID3D12Fence* render_fence = nullptr;
    ID3D12Fence* compute_fence = nullptr;
    ID3D12Fence* present_fence = nullptr;
    ID3D12Fence* dxgi_block_fence = nullptr;
    HRESULT copy_fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence)) : E_FAIL;
    HRESULT render_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_fence)) : E_FAIL;
    HRESULT compute_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&compute_fence)) : E_FAIL;
    HRESULT present_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence)) : E_FAIL;
    HRESULT dxgi_block_fence_hr =
        device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dxgi_block_fence)) : E_FAIL;

    const UINT64 buffer_bytes = 4096;
    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC buffer = buffer_desc(buffer_bytes);

    ID3D12Resource* upload_buffer = nullptr;
    ID3D12Resource* copy_buffer = nullptr;
    ID3D12Resource* render_buffer = nullptr;
    ID3D12Resource* readback_buffer = nullptr;
    ID3D12Resource* timestamp_readback = nullptr;
    ID3D12QueryHeap* timestamp_heap = nullptr;
    HRESULT upload_buffer_hr = device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&upload_buffer))
                                      : E_FAIL;
    HRESULT copy_buffer_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy_buffer))
               : E_FAIL;
    HRESULT render_buffer_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&render_buffer))
               : E_FAIL;
    HRESULT readback_buffer_hr = device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                          IID_PPV_ARGS(&readback_buffer))
                                        : E_FAIL;
    D3D12_RESOURCE_DESC timestamp_buffer_desc = buffer_desc(2 * sizeof(UINT64));
    HRESULT timestamp_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &timestamp_buffer_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&timestamp_readback))
               : E_FAIL;
    D3D12_QUERY_HEAP_DESC timestamp_heap_desc = {};
    timestamp_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    timestamp_heap_desc.Count = 2;
    HRESULT timestamp_heap_hr =
        device ? device->CreateQueryHeap(&timestamp_heap_desc, IID_PPV_ARGS(&timestamp_heap)) : E_FAIL;
    HRESULT invalid_make_resident_hr = device ? device->MakeResident(1, nullptr) : E_FAIL;
    HRESULT invalid_evict_hr = device ? device->Evict(1, nullptr) : E_FAIL;
    ID3D12Pageable* null_pageable[] = {nullptr};
    HRESULT null_make_resident_entry_hr = device ? device->MakeResident(1, null_pageable) : E_FAIL;
    HRESULT null_evict_entry_hr = device ? device->Evict(1, null_pageable) : E_FAIL;

    uint8_t* upload_ptr = nullptr;
    HRESULT map_upload_hr =
        upload_buffer ? upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr)) : E_FAIL;
    if (SUCCEEDED(map_upload_hr) && upload_ptr) {
        for (UINT64 i = 0; i < buffer_bytes; ++i)
            upload_ptr[i] = static_cast<uint8_t>((i * 29u + 11u) & 0xffu);
        upload_buffer->Unmap(0, nullptr);
    }

    if (copy_list && upload_buffer && copy_buffer)
        copy_list->CopyBufferRegion(copy_buffer, 0, upload_buffer, 0, buffer_bytes);
    if (copy_list && timestamp_heap && timestamp_readback && upload_buffer && copy_buffer) {
        copy_list->EndQuery(timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        copy_list->CopyBufferRegion(copy_buffer, 0, upload_buffer, 0, 16);
        copy_list->EndQuery(timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        copy_list->ResolveQueryData(timestamp_heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timestamp_readback, 0);
    }

    if (render_list && copy_buffer && render_buffer) {
        D3D12_RESOURCE_BARRIER copy_to_src =
            transition_barrier(copy_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        render_list->ResourceBarrier(1, &copy_to_src);
        render_list->CopyBufferRegion(render_buffer, 0, copy_buffer, 0, buffer_bytes);
    }

    if (present_list && render_buffer && readback_buffer) {
        D3D12_RESOURCE_BARRIER render_to_src =
            transition_barrier(render_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        present_list->ResourceBarrier(1, &render_to_src);
        present_list->CopyBufferRegion(readback_buffer, 0, render_buffer, 0, buffer_bytes);
    }

    HRESULT copy_close_hr = copy_list ? copy_list->Close() : E_FAIL;
    HRESULT render_close_hr = render_list ? render_list->Close() : E_FAIL;
    HRESULT compute_close_hr = compute_list ? compute_list->Close() : E_FAIL;
    HRESULT present_close_hr = present_list ? present_list->Close() : E_FAIL;

    HRESULT copy_execute_hr = E_FAIL;
    HRESULT copy_signal_hr = E_FAIL;
    HRESULT render_wait_hr = E_FAIL;
    HRESULT render_execute_hr = E_FAIL;
    HRESULT render_signal_hr = E_FAIL;
    HRESULT compute_wait_hr = E_FAIL;
    HRESULT compute_execute_hr = E_FAIL;
    HRESULT compute_signal_hr = E_FAIL;
    HRESULT present_wait_hr = E_FAIL;
    HRESULT present_execute_hr = E_FAIL;
    HRESULT present_signal_hr = E_FAIL;
    HRESULT cpu_wait_hr = E_FAIL;
    HRESULT copy_allocator_reset_hr = E_FAIL;
    HRESULT render_allocator_reset_hr = E_FAIL;
    HRESULT compute_allocator_reset_hr = E_FAIL;
    HRESULT present_allocator_reset_hr = E_FAIL;
    HRESULT copy_list_reset_hr = E_FAIL;
    HRESULT render_list_reset_hr = E_FAIL;
    HRESULT compute_list_reset_hr = E_FAIL;
    HRESULT present_list_reset_hr = E_FAIL;

    if (copy_queue && copy_list && copy_fence && SUCCEEDED(copy_close_hr)) {
        ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(copy_list)};
        copy_queue->ExecuteCommandLists(1, lists);
        copy_execute_hr = S_OK;
        copy_signal_hr = copy_queue->Signal(copy_fence, 1);
    }

    if (direct_queue && render_list && copy_fence && render_fence && SUCCEEDED(render_close_hr)) {
        render_wait_hr = direct_queue->Wait(copy_fence, 1);
        ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(render_list)};
        direct_queue->ExecuteCommandLists(1, lists);
        render_execute_hr = S_OK;
        render_signal_hr = direct_queue->Signal(render_fence, 2);
    }

    if (compute_queue && compute_list && render_fence && compute_fence && SUCCEEDED(compute_close_hr)) {
        compute_wait_hr = compute_queue->Wait(render_fence, 2);
        ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(compute_list)};
        compute_queue->ExecuteCommandLists(1, lists);
        compute_execute_hr = S_OK;
        compute_signal_hr = compute_queue->Signal(compute_fence, 3);
    }

    if (present_queue && present_list && compute_fence && present_fence && SUCCEEDED(present_close_hr)) {
        present_wait_hr = present_queue->Wait(compute_fence, 3);
        ID3D12CommandList* lists[] = {reinterpret_cast<ID3D12CommandList*>(present_list)};
        present_queue->ExecuteCommandLists(1, lists);
        present_execute_hr = S_OK;
        present_signal_hr = present_queue->Signal(present_fence, 4);
    }

    IDXGIDevice3* dxgi_device3 = nullptr;
    HRESULT dxgi_device3_qi_hr = device ? device->QueryInterface(IID_PPV_ARGS(&dxgi_device3)) : E_FAIL;
    INT default_gpu_priority = INT_MIN;
    HRESULT get_default_gpu_priority_hr =
        dxgi_device3 ? dxgi_device3->GetGPUThreadPriority(&default_gpu_priority) : E_NOINTERFACE;
    HRESULT set_gpu_priority_hr = dxgi_device3 ? dxgi_device3->SetGPUThreadPriority(-7) : E_NOINTERFACE;
    INT updated_gpu_priority = INT_MIN;
    HRESULT get_updated_gpu_priority_hr =
        dxgi_device3 ? dxgi_device3->GetGPUThreadPriority(&updated_gpu_priority) : E_NOINTERFACE;
    HRESULT set_invalid_gpu_priority_hr = dxgi_device3 ? dxgi_device3->SetGPUThreadPriority(-8) : E_NOINTERFACE;
    HRESULT get_null_gpu_priority_hr = dxgi_device3 ? dxgi_device3->GetGPUThreadPriority(nullptr) : E_NOINTERFACE;
    UINT default_frame_latency = UINT_MAX;
    HRESULT get_default_frame_latency_hr =
        dxgi_device3 ? dxgi_device3->GetMaximumFrameLatency(&default_frame_latency) : E_NOINTERFACE;
    HRESULT set_frame_latency_hr = dxgi_device3 ? dxgi_device3->SetMaximumFrameLatency(7) : E_NOINTERFACE;
    UINT updated_frame_latency = UINT_MAX;
    HRESULT get_updated_frame_latency_hr =
        dxgi_device3 ? dxgi_device3->GetMaximumFrameLatency(&updated_frame_latency) : E_NOINTERFACE;
    HRESULT reset_frame_latency_hr = dxgi_device3 ? dxgi_device3->SetMaximumFrameLatency(0) : E_NOINTERFACE;
    UINT reset_frame_latency = UINT_MAX;
    HRESULT get_reset_frame_latency_hr =
        dxgi_device3 ? dxgi_device3->GetMaximumFrameLatency(&reset_frame_latency) : E_NOINTERFACE;
    HRESULT set_invalid_frame_latency_hr = dxgi_device3 ? dxgi_device3->SetMaximumFrameLatency(17) : E_NOINTERFACE;
    HRESULT zero_residency_hr =
        dxgi_device3 ? dxgi_device3->QueryResourceResidency(nullptr, nullptr, 0) : E_NOINTERFACE;
    HRESULT zero_offer_hr =
        dxgi_device3 ? dxgi_device3->OfferResources(0, nullptr, DXGI_OFFER_RESOURCE_PRIORITY_NORMAL) : E_NOINTERFACE;
    HRESULT invalid_offer_priority_hr =
        dxgi_device3 ? dxgi_device3->OfferResources(0, nullptr, static_cast<DXGI_OFFER_RESOURCE_PRIORITY>(0))
                     : E_NOINTERFACE;
    HRESULT zero_reclaim_hr = dxgi_device3 ? dxgi_device3->ReclaimResources(0, nullptr, nullptr) : E_NOINTERFACE;
    HRESULT enqueue_null_event_hr = dxgi_device3 ? dxgi_device3->EnqueueSetEvent(nullptr) : E_NOINTERFACE;
    HANDLE enqueue_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE enqueue_event_observer = nullptr;
    BOOL duplicate_enqueue_event =
        enqueue_event && DuplicateHandle(GetCurrentProcess(), enqueue_event, GetCurrentProcess(),
                                         &enqueue_event_observer, 0, FALSE, DUPLICATE_SAME_ACCESS);
    HRESULT dxgi_queue_block_hr = copy_queue && dxgi_block_fence ? copy_queue->Wait(dxgi_block_fence, 1) : E_FAIL;
    HRESULT enqueue_set_event_hr = dxgi_device3 && enqueue_event && SUCCEEDED(dxgi_queue_block_hr)
                                       ? dxgi_device3->EnqueueSetEvent(enqueue_event)
                                       : E_FAIL;
    if (enqueue_event)
        CloseHandle(enqueue_event);
    DWORD enqueue_initial_wait = enqueue_event_observer ? WaitForSingleObject(enqueue_event_observer, 0) : WAIT_FAILED;
    HRESULT dxgi_block_release_hr = dxgi_block_fence ? dxgi_block_fence->Signal(1) : E_FAIL;
    DWORD enqueue_final_wait = enqueue_event_observer && SUCCEEDED(dxgi_block_release_hr)
                                   ? WaitForSingleObject(enqueue_event_observer, 15000)
                                   : WAIT_FAILED;
    if (enqueue_event_observer)
        CloseHandle(enqueue_event_observer);

    HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (present_fence && event_handle && SUCCEEDED(present_signal_hr)) {
        cpu_wait_hr = present_fence->SetEventOnCompletion(4, event_handle);
        if (SUCCEEDED(cpu_wait_hr))
            WaitForSingleObject(event_handle, 15000);
    }
    if (event_handle)
        CloseHandle(event_handle);

    if (copy_allocator)
        copy_allocator_reset_hr = copy_allocator->Reset();
    if (render_allocator)
        render_allocator_reset_hr = render_allocator->Reset();
    if (compute_allocator)
        compute_allocator_reset_hr = compute_allocator->Reset();
    if (present_allocator)
        present_allocator_reset_hr = present_allocator->Reset();
    if (copy_list && SUCCEEDED(copy_allocator_reset_hr))
        copy_list_reset_hr = copy_list->Reset(copy_allocator, nullptr);
    if (render_list && SUCCEEDED(render_allocator_reset_hr))
        render_list_reset_hr = render_list->Reset(render_allocator, nullptr);
    if (compute_list && SUCCEEDED(compute_allocator_reset_hr))
        compute_list_reset_hr = compute_list->Reset(compute_allocator, nullptr);
    if (present_list && SUCCEEDED(present_allocator_reset_hr))
        present_list_reset_hr = present_list->Reset(present_allocator, nullptr);

    uint8_t* readback_ptr = nullptr;
    HRESULT map_readback_hr =
        readback_buffer ? readback_buffer->Map(0, nullptr, reinterpret_cast<void**>(&readback_ptr)) : E_FAIL;
    bool readback_ok = SUCCEEDED(map_readback_hr) && readback_ptr;
    if (readback_ok) {
        for (UINT64 i = 0; i < buffer_bytes; ++i) {
            if (readback_ptr[i] != static_cast<uint8_t>((i * 29u + 11u) & 0xffu)) {
                readback_ok = false;
                break;
            }
        }
        readback_buffer->Unmap(0, nullptr);
    }

    UINT64 copy_timestamps[2] = {};
    UINT64* timestamp_ptr = nullptr;
    D3D12_RANGE timestamp_range = {0, sizeof(copy_timestamps)};
    HRESULT timestamp_map_hr =
        timestamp_readback ? timestamp_readback->Map(0, &timestamp_range, reinterpret_cast<void**>(&timestamp_ptr))
                           : E_FAIL;
    bool copy_timestamps_ok = SUCCEEDED(timestamp_map_hr) && timestamp_ptr;
    if (copy_timestamps_ok) {
        copy_timestamps[0] = timestamp_ptr[0];
        copy_timestamps[1] = timestamp_ptr[1];
        copy_timestamps_ok = copy_timestamps[0] != 0 && copy_timestamps[1] >= copy_timestamps[0];
        D3D12_RANGE written = {0, 0};
        timestamp_readback->Unmap(0, &written);
    }

    UINT64 copy_completed = copy_fence ? copy_fence->GetCompletedValue() : 0;
    UINT64 render_completed = render_fence ? render_fence->GetCompletedValue() : 0;
    UINT64 compute_completed = compute_fence ? compute_fence->GetCompletedValue() : 0;
    UINT64 present_completed = present_fence ? present_fence->GetCompletedValue() : 0;

    D3D12_COMMAND_QUEUE_DESC direct_desc = {};
    D3D12_COMMAND_QUEUE_DESC present_desc = {};
    D3D12_COMMAND_QUEUE_DESC compute_desc = {};
    D3D12_COMMAND_QUEUE_DESC copy_desc = {};
    if (direct_queue)
        direct_queue->GetDesc(&direct_desc);
    if (present_queue)
        present_queue->GetDesc(&present_desc);
    if (compute_queue)
        compute_queue->GetDesc(&compute_desc);
    if (copy_queue)
        copy_queue->GetDesc(&copy_desc);

    UINT64 direct_frequency = 0;
    UINT64 direct_gpu_clock = 0;
    UINT64 direct_cpu_clock = 0;
    HRESULT timestamp_frequency_hr = direct_queue ? direct_queue->GetTimestampFrequency(&direct_frequency) : E_FAIL;
    HRESULT clock_calibration_hr =
        direct_queue ? direct_queue->GetClockCalibration(&direct_gpu_clock, &direct_cpu_clock) : E_FAIL;

    bool queue_types_ok =
        direct_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && present_desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
        compute_desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && copy_desc.Type == D3D12_COMMAND_LIST_TYPE_COPY;
    bool fences_ok = copy_completed >= 1 && render_completed >= 2 && compute_completed >= 3 && present_completed >= 4;
    bool pass =
        SUCCEEDED(create_hr) && SUCCEEDED(direct_queue_hr) && SUCCEEDED(present_queue_hr) &&
        SUCCEEDED(compute_queue_hr) && SUCCEEDED(copy_queue_hr) && SUCCEEDED(render_objects_hr) &&
        SUCCEEDED(present_objects_hr) && SUCCEEDED(compute_objects_hr) && SUCCEEDED(copy_objects_hr) &&
        SUCCEEDED(copy_fence_hr) && SUCCEEDED(render_fence_hr) && SUCCEEDED(compute_fence_hr) &&
        SUCCEEDED(present_fence_hr) && SUCCEEDED(upload_buffer_hr) && SUCCEEDED(copy_buffer_hr) &&
        SUCCEEDED(dxgi_block_fence_hr) && SUCCEEDED(dxgi_device3_qi_hr) && SUCCEEDED(get_default_gpu_priority_hr) &&
        default_gpu_priority == 0 && SUCCEEDED(set_gpu_priority_hr) && SUCCEEDED(get_updated_gpu_priority_hr) &&
        updated_gpu_priority == -7 && set_invalid_gpu_priority_hr == E_INVALIDARG &&
        get_null_gpu_priority_hr == E_POINTER && SUCCEEDED(get_default_frame_latency_hr) &&
        default_frame_latency == 3 && SUCCEEDED(set_frame_latency_hr) && SUCCEEDED(get_updated_frame_latency_hr) &&
        updated_frame_latency == 7 && SUCCEEDED(reset_frame_latency_hr) && SUCCEEDED(get_reset_frame_latency_hr) &&
        reset_frame_latency == 3 && set_invalid_frame_latency_hr == E_INVALIDARG && SUCCEEDED(zero_residency_hr) &&
        SUCCEEDED(zero_offer_hr) && invalid_offer_priority_hr == E_INVALIDARG && SUCCEEDED(zero_reclaim_hr) &&
        invalid_make_resident_hr == E_INVALIDARG && invalid_evict_hr == E_INVALIDARG &&
        null_make_resident_entry_hr == E_INVALIDARG && null_evict_entry_hr == E_INVALIDARG &&
        enqueue_null_event_hr == E_INVALIDARG && duplicate_enqueue_event && SUCCEEDED(dxgi_queue_block_hr) &&
        SUCCEEDED(enqueue_set_event_hr) && enqueue_initial_wait == WAIT_TIMEOUT && SUCCEEDED(dxgi_block_release_hr) &&
        enqueue_final_wait == WAIT_OBJECT_0 && SUCCEEDED(render_buffer_hr) && SUCCEEDED(readback_buffer_hr) &&
        SUCCEEDED(map_upload_hr) && SUCCEEDED(copy_close_hr) && SUCCEEDED(render_close_hr) &&
        SUCCEEDED(compute_close_hr) && SUCCEEDED(present_close_hr) && SUCCEEDED(copy_execute_hr) &&
        SUCCEEDED(copy_signal_hr) && SUCCEEDED(render_wait_hr) && SUCCEEDED(render_execute_hr) &&
        SUCCEEDED(render_signal_hr) && SUCCEEDED(compute_wait_hr) && SUCCEEDED(compute_execute_hr) &&
        SUCCEEDED(compute_signal_hr) && SUCCEEDED(present_wait_hr) && SUCCEEDED(present_execute_hr) &&
        SUCCEEDED(present_signal_hr) && SUCCEEDED(cpu_wait_hr) && SUCCEEDED(copy_allocator_reset_hr) &&
        SUCCEEDED(render_allocator_reset_hr) && SUCCEEDED(compute_allocator_reset_hr) &&
        SUCCEEDED(present_allocator_reset_hr) && SUCCEEDED(copy_list_reset_hr) && SUCCEEDED(render_list_reset_hr) &&
        SUCCEEDED(compute_list_reset_hr) && SUCCEEDED(present_list_reset_hr) && SUCCEEDED(map_readback_hr) &&
        readback_ok && queue_types_ok && fences_ok && SUCCEEDED(timestamp_frequency_hr) &&
        SUCCEEDED(clock_calibration_hr) && SUCCEEDED(timestamp_heap_hr) && SUCCEEDED(timestamp_readback_hr) &&
        copy_timestamps_ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-queues.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"device_create\": {\n");
    print_hr("hr", create_hr, false);
    std::printf("  },\n");
    std::printf("  \"queues\": {\n");
    print_hr("direct_create", direct_queue_hr);
    print_hr("present_create", present_queue_hr);
    print_hr("compute_create", compute_queue_hr);
    print_hr("copy_create", copy_queue_hr);
    std::printf("    \"direct_type\": %u,\n", direct_desc.Type);
    std::printf("    \"present_type\": %u,\n", present_desc.Type);
    std::printf("    \"compute_type\": %u,\n", compute_desc.Type);
    std::printf("    \"copy_type\": %u,\n", copy_desc.Type);
    print_hr("timestamp_frequency", timestamp_frequency_hr);
    print_hr("clock_calibration", clock_calibration_hr);
    std::printf("    \"timestamp_frequency_value\": %" PRIu64 ",\n", direct_frequency);
    std::printf("    \"copy_timestamp_heap_hr\": \"0x%08lx\",\n", static_cast<unsigned long>(timestamp_heap_hr));
    std::printf("    \"copy_timestamp_readback_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(timestamp_readback_hr));
    std::printf("    \"copy_timestamp_map_hr\": \"0x%08lx\",\n", static_cast<unsigned long>(timestamp_map_hr));
    std::printf("    \"copy_timestamp_values\": [%" PRIu64 ",%" PRIu64 "],\n", copy_timestamps[0], copy_timestamps[1]);
    std::printf("    \"copy_timestamps_verified\": %s,\n", copy_timestamps_ok ? "true" : "false");
    std::printf("    \"clock_gpu\": %" PRIu64 ",\n", direct_gpu_clock);
    std::printf("    \"clock_cpu\": %" PRIu64 "\n", direct_cpu_clock);
    std::printf("  },\n");
    std::printf("  \"command_lists\": {\n");
    print_hr("render_objects", render_objects_hr);
    print_hr("present_objects", present_objects_hr);
    print_hr("compute_objects", compute_objects_hr);
    print_hr("copy_objects", copy_objects_hr);
    print_hr("render_close", render_close_hr);
    print_hr("present_close", present_close_hr);
    print_hr("compute_close", compute_close_hr);
    print_hr("copy_close", copy_close_hr);
    print_hr("copy_allocator_reset", copy_allocator_reset_hr);
    print_hr("render_allocator_reset", render_allocator_reset_hr);
    print_hr("compute_allocator_reset", compute_allocator_reset_hr);
    print_hr("present_allocator_reset", present_allocator_reset_hr);
    print_hr("copy_list_reset", copy_list_reset_hr);
    print_hr("render_list_reset", render_list_reset_hr);
    print_hr("compute_list_reset", compute_list_reset_hr);
    print_hr("present_list_reset", present_list_reset_hr, false);
    std::printf("  },\n");
    std::printf("  \"synchronization\": {\n");
    print_hr("copy_execute", copy_execute_hr);
    print_hr("copy_signal", copy_signal_hr);
    print_hr("render_wait", render_wait_hr);
    print_hr("render_execute", render_execute_hr);
    print_hr("render_signal", render_signal_hr);
    print_hr("compute_wait", compute_wait_hr);
    print_hr("compute_execute", compute_execute_hr);
    print_hr("compute_signal", compute_signal_hr);
    print_hr("present_wait", present_wait_hr);
    print_hr("present_execute", present_execute_hr);
    print_hr("present_signal", present_signal_hr);
    print_hr("cpu_wait", cpu_wait_hr);
    print_hr("dxgi_device3_qi", dxgi_device3_qi_hr);
    print_hr("get_default_gpu_priority", get_default_gpu_priority_hr);
    print_hr("set_gpu_priority", set_gpu_priority_hr);
    print_hr("get_updated_gpu_priority", get_updated_gpu_priority_hr);
    print_hr("set_invalid_gpu_priority", set_invalid_gpu_priority_hr);
    print_hr("get_null_gpu_priority", get_null_gpu_priority_hr);
    print_hr("get_default_frame_latency", get_default_frame_latency_hr);
    print_hr("set_frame_latency", set_frame_latency_hr);
    print_hr("get_updated_frame_latency", get_updated_frame_latency_hr);
    print_hr("reset_frame_latency_hr", reset_frame_latency_hr);
    print_hr("get_reset_frame_latency", get_reset_frame_latency_hr);
    print_hr("set_invalid_frame_latency", set_invalid_frame_latency_hr);
    print_hr("zero_resource_residency", zero_residency_hr);
    print_hr("zero_resource_offer", zero_offer_hr);
    print_hr("invalid_offer_priority", invalid_offer_priority_hr);
    print_hr("zero_resource_reclaim", zero_reclaim_hr);
    print_hr("invalid_make_resident", invalid_make_resident_hr);
    print_hr("invalid_evict", invalid_evict_hr);
    print_hr("null_make_resident_entry", null_make_resident_entry_hr);
    print_hr("null_evict_entry", null_evict_entry_hr);
    print_hr("enqueue_null_event", enqueue_null_event_hr);
    print_hr("dxgi_queue_block", dxgi_queue_block_hr);
    print_hr("enqueue_set_event", enqueue_set_event_hr);
    print_hr("dxgi_block_release", dxgi_block_release_hr);
    std::printf("    \"enqueue_event_handle_duplicated\": %s,\n", duplicate_enqueue_event ? "true" : "false");
    std::printf("    \"enqueue_event_blocked_before_queue_completion\": %s,\n",
                enqueue_initial_wait == WAIT_TIMEOUT ? "true" : "false");
    std::printf("    \"enqueue_event_signaled_after_all_queues\": %s,\n",
                enqueue_final_wait == WAIT_OBJECT_0 ? "true" : "false");
    std::printf("    \"default_gpu_priority\": %d,\n", default_gpu_priority);
    std::printf("    \"updated_gpu_priority\": %d,\n", updated_gpu_priority);
    std::printf("    \"default_frame_latency\": %u,\n", default_frame_latency);
    std::printf("    \"updated_frame_latency\": %u,\n", updated_frame_latency);
    std::printf("    \"reset_frame_latency_value\": %u,\n", reset_frame_latency);
    std::printf("    \"copy_completed\": %" PRIu64 ",\n", copy_completed);
    std::printf("    \"render_completed\": %" PRIu64 ",\n", render_completed);
    std::printf("    \"compute_completed\": %" PRIu64 ",\n", compute_completed);
    std::printf("    \"present_completed\": %" PRIu64 "\n", present_completed);
    std::printf("  },\n");
    std::printf("  \"readback\": {\n");
    print_hr("upload_map", map_upload_hr);
    print_hr("readback_map", map_readback_hr);
    std::printf("    \"verified\": %s\n", readback_ok ? "true" : "false");
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
