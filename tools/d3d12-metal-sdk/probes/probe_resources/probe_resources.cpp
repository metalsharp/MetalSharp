#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <dxgiformat.h>

struct FormatProbe {
    const char* name;
    DXGI_FORMAT format;
    HRESULT hr = E_FAIL;
    UINT support1 = 0;
    UINT support2 = 0;
};

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

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

static void print_hr(const char* key, HRESULT hr, bool comma = true) {
    std::printf("    \"%s\": \"0x%08lx\"%s\n", key, static_cast<unsigned long>(static_cast<uint32_t>(hr)),
                comma ? "," : "");
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

static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = bytes;
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

static D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
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

static void print_format_json(const FormatProbe& probe, bool last) {
    std::printf("    \"%s\": {\n", probe.name);
    std::printf("      \"hr\": \"0x%08lx\",\n", static_cast<unsigned long>(static_cast<uint32_t>(probe.hr)));
    std::printf("      \"support1\": %u,\n", probe.support1);
    std::printf("      \"support2\": %u,\n", probe.support2);
    std::printf("      \"render_target\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ? "true" : "false");
    std::printf("      \"depth_stencil\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) ? "true" : "false");
    std::printf("      \"shader_sample\": %s,\n",
                (probe.support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ? "true" : "false");
    std::printf("      \"typed_uav_load\": %s\n",
                (probe.support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) ? "true" : "false");
    std::printf("    }%s\n", last ? "" : ",");
}

int main() {
    std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto create_device = reinterpret_cast<CreateDeviceFn>(
        reinterpret_cast<void*>(d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : E_FAIL;

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT queue_hr = device ? device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)) : E_FAIL;
    HRESULT allocator_hr =
        device ? device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) : E_FAIL;
    HRESULT list_hr =
        device ? device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))
               : E_FAIL;
    HRESULT fence_hr = device ? device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) : E_FAIL;

    const UINT64 buffer_bytes = 4096;
    D3D12_HEAP_PROPERTIES upload_heap = heap_props(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES default_heap = heap_props(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES readback_heap = heap_props(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC buffer = buffer_desc(buffer_bytes);
    ID3D12Resource* upload_buffer = nullptr;
    ID3D12Resource* default_buffer = nullptr;
    ID3D12Resource* readback_buffer = nullptr;
    ID3D12Resource* shared_open_buffer = nullptr;
    ID3D12Resource* shared_named_open_buffer = nullptr;
    ID3D12Resource* unknown_open_buffer = nullptr;
    HANDLE shared_handle = nullptr;
    HANDLE shared_named_handle = nullptr;
    HRESULT shared_create_hr = E_FAIL;
    HRESULT shared_open_hr = E_FAIL;
    HRESULT shared_open_named_hr = E_FAIL;
    HRESULT shared_unknown_hr = E_FAIL;
    HRESULT shared_missing_name_hr = E_FAIL;
    D3D12_RESOURCE_DESC default_buffer_desc = {};
    D3D12_GPU_VIRTUAL_ADDRESS upload_gpu_va = 0;
    D3D12_GPU_VIRTUAL_ADDRESS default_gpu_va = 0;
    bool command_resource_lifetime_ok = false;
    HRESULT upload_buffer_hr = device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                        IID_PPV_ARGS(&upload_buffer))
                                      : E_FAIL;
    HRESULT default_buffer_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&default_buffer))
               : E_FAIL;
    HRESULT readback_buffer_hr = device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                          IID_PPV_ARGS(&readback_buffer))
                                        : E_FAIL;
    if (device && default_buffer) {
        shared_create_hr = device->CreateSharedHandle(
            default_buffer, nullptr, GENERIC_ALL, L"metalsharp-probe-buffer",
            &shared_handle);
        if (SUCCEEDED(shared_create_hr))
            shared_open_hr = device->OpenSharedHandle(
                shared_handle, IID_PPV_ARGS(&shared_open_buffer));
        shared_open_named_hr = device->OpenSharedHandleByName(
            L"metalsharp-probe-buffer", GENERIC_ALL, &shared_named_handle);
        if (SUCCEEDED(shared_open_named_hr))
            shared_open_named_hr = device->OpenSharedHandle(
                shared_named_handle, IID_PPV_ARGS(&shared_named_open_buffer));
        HANDLE unknown_shared_handle =
            CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (unknown_shared_handle) {
            shared_unknown_hr = device->OpenSharedHandle(
                unknown_shared_handle, IID_PPV_ARGS(&unknown_open_buffer));
            CloseHandle(unknown_shared_handle);
        }
        HANDLE missing_name_handle = nullptr;
        shared_missing_name_hr = device->OpenSharedHandleByName(
            L"metalsharp-probe-missing", GENERIC_ALL, &missing_name_handle);
        if (missing_name_handle)
            CloseHandle(missing_name_handle);
    }

    uint8_t* upload_ptr = nullptr;
    HRESULT map_upload_hr =
        upload_buffer ? upload_buffer->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr)) : E_FAIL;
    if (SUCCEEDED(map_upload_hr) && upload_ptr) {
        for (UINT64 i = 0; i < buffer_bytes; ++i)
            upload_ptr[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
        upload_buffer->Unmap(0, nullptr);
    }

    if (list && upload_buffer && default_buffer && readback_buffer) {
        list->CopyBufferRegion(default_buffer, 0, upload_buffer, 0, buffer_bytes);
        D3D12_RESOURCE_BARRIER barrier =
            transition_barrier(default_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(readback_buffer, 0, default_buffer, 0, buffer_bytes);
        default_buffer_desc = default_buffer->GetDesc();
        upload_gpu_va = upload_buffer->GetGPUVirtualAddress();
        default_gpu_va = default_buffer->GetGPUVirtualAddress();
        // The command list owns the recorded resources. Releasing the caller's
        // references here makes the first copy a real lifetime test rather
        // than relying on cleanup after execution.
        upload_buffer->Release();
        upload_buffer = nullptr;
        default_buffer->Release();
        default_buffer = nullptr;
        command_resource_lifetime_ok = true;
    }

    ID3D12Resource* texture = nullptr;
    ID3D12Resource* texture_upload = nullptr;
    ID3D12Resource* texture_readback = nullptr;
    ID3D12Heap* sparse_heap = nullptr;
    ID3D12Resource* reserved_texture = nullptr;
    ID3D12Resource* sparse_upload = nullptr;
    ID3D12Resource* sparse_readback = nullptr;
    ID3D12Resource* sparse_unmapped_readback = nullptr;
    HRESULT sparse_heap_hr = E_FAIL;
    HRESULT reserved_texture_hr = E_FAIL;
    HRESULT sparse_upload_hr = E_FAIL;
    HRESULT sparse_readback_hr = E_FAIL;
    HRESULT sparse_unmapped_readback_hr = E_FAIL;
    HRESULT sparse_unmap_close_hr = E_FAIL;
    HRESULT sparse_unmap_execute_hr = E_FAIL;
    HRESULT sparse_unmap_signal_hr = E_FAIL;
    HRESULT sparse_unmap_wait_hr = E_FAIL;
    HRESULT sparse_tiling_hr = E_FAIL;
    UINT sparse_total_tiles = 0;
    D3D12_PACKED_MIP_INFO sparse_packed_mips = {};
    D3D12_TILE_SHAPE sparse_tile_shape = {};
    D3D12_SUBRESOURCE_TILING sparse_tiling[2] = {};
    UINT sparse_tiling_count = 2;
    const UINT64 sparse_tile_size = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    const UINT64 sparse_tile_bytes = 2 * sparse_tile_size;
    D3D12_RESOURCE_DESC tex_desc = texture_desc(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM);
    UINT64 texture_upload_bytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT texture_footprint = {};
    UINT texture_rows = 0;
    UINT64 texture_row_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &texture_footprint, &texture_rows, &texture_row_bytes,
                                      &texture_upload_bytes);
    D3D12_RESOURCE_DESC texture_staging_desc = buffer_desc(texture_upload_bytes);
    HRESULT texture_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture))
               : E_FAIL;
    HRESULT texture_upload_hr =
        device
            ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &texture_staging_desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&texture_upload))
            : E_FAIL;
    HRESULT texture_readback_hr =
        device
            ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &texture_staging_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture_readback))
            : E_FAIL;
    uint8_t* texture_upload_ptr = nullptr;
    HRESULT texture_map_hr =
        texture_upload ? texture_upload->Map(0, nullptr, reinterpret_cast<void**>(&texture_upload_ptr)) : E_FAIL;
    if (SUCCEEDED(texture_map_hr) && texture_upload_ptr) {
        std::memset(texture_upload_ptr, 0, static_cast<size_t>(texture_upload_bytes));
        for (UINT y = 0; y < 4; ++y) {
            for (UINT x = 0; x < 4; ++x) {
                size_t offset =
                    static_cast<size_t>(texture_footprint.Offset + y * texture_footprint.Footprint.RowPitch + x * 4);
                texture_upload_ptr[offset + 0] = static_cast<uint8_t>(x * 40);
                texture_upload_ptr[offset + 1] = static_cast<uint8_t>(y * 40);
                texture_upload_ptr[offset + 2] = 0xa5;
                texture_upload_ptr[offset + 3] = 0xff;
            }
        }
        texture_upload->Unmap(0, nullptr);
    }
    if (list && texture && texture_upload && texture_readback) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = texture_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = texture_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER texture_barrier =
            transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &texture_barrier);
        D3D12_TEXTURE_COPY_LOCATION readback_dst = {};
        readback_dst.pResource = texture_readback;
        readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readback_dst.PlacedFootprint = texture_footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = texture;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        texture_src.SubresourceIndex = 0;
        list->CopyTextureRegion(&readback_dst, 0, 0, 0, &texture_src, nullptr);
    }

    D3D12_HEAP_DESC sparse_heap_desc = {};
    sparse_heap_desc.SizeInBytes = sparse_tile_bytes;
    sparse_heap_desc.Properties = default_heap;
    sparse_heap_desc.Flags = D3D12_HEAP_FLAG_NONE;
    sparse_heap_hr = device ? device->CreateHeap(
                                  &sparse_heap_desc, IID_PPV_ARGS(&sparse_heap))
                            : E_FAIL;
    D3D12_RESOURCE_DESC reserved_desc = texture_desc(
        128, 128, DXGI_FORMAT_R8G8B8A8_UNORM);
    reserved_desc.DepthOrArraySize = 2;
    reserved_texture_hr =
        device ? device->CreateReservedResource(
                     &reserved_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&reserved_texture))
               : E_FAIL;
    if (device && reserved_texture) {
        sparse_tiling_hr = S_OK;
        device->GetResourceTiling(
            reserved_texture, &sparse_total_tiles, &sparse_packed_mips,
            &sparse_tile_shape, &sparse_tiling_count, 0, sparse_tiling);
    }
    D3D12_RESOURCE_DESC sparse_buffer_desc = buffer_desc(sparse_tile_bytes);
    sparse_upload_hr =
        device ? device->CreateCommittedResource(
                     &upload_heap, D3D12_HEAP_FLAG_NONE, &sparse_buffer_desc,
                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                     IID_PPV_ARGS(&sparse_upload))
               : E_FAIL;
    sparse_readback_hr =
        device ? device->CreateCommittedResource(
                     &readback_heap, D3D12_HEAP_FLAG_NONE, &sparse_buffer_desc,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&sparse_readback))
               : E_FAIL;
    sparse_unmapped_readback_hr =
        device ? device->CreateCommittedResource(
                     &readback_heap, D3D12_HEAP_FLAG_NONE, &sparse_buffer_desc,
                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                     IID_PPV_ARGS(&sparse_unmapped_readback))
               : E_FAIL;
    uint8_t *sparse_upload_ptr = nullptr;
    HRESULT sparse_upload_map_hr =
        sparse_upload ? sparse_upload->Map(
                            0, nullptr,
                            reinterpret_cast<void **>(&sparse_upload_ptr))
                      : E_FAIL;
    bool sparse_copy_ok = false;
    if (SUCCEEDED(sparse_upload_map_hr) && sparse_upload_ptr) {
        for (UINT64 i = 0; i < sparse_tile_bytes; i++)
            sparse_upload_ptr[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xffu);
        sparse_upload->Unmap(0, nullptr);
    }
    if (queue && sparse_heap && reserved_texture && sparse_upload &&
        sparse_readback && sparse_total_tiles == 2 && sparse_tiling_count == 2 &&
        sparse_tile_shape.WidthInTexels == 128 &&
        sparse_tile_shape.HeightInTexels == 128) {
        D3D12_TILED_RESOURCE_COORDINATE coordinates[2] = {};
        coordinates[1].Subresource = 1;
        D3D12_TILE_REGION_SIZE region_sizes[2] = {};
        region_sizes[0].NumTiles = 1;
        region_sizes[1].NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS range_flags[2] = {
            D3D12_TILE_RANGE_FLAG_NONE, D3D12_TILE_RANGE_FLAG_NONE};
        UINT heap_offsets[2] = {0, 1};
        UINT range_tile_counts[2] = {1, 1};
        queue->UpdateTileMappings(
            reserved_texture, 2, coordinates, region_sizes, sparse_heap, 2,
            range_flags, heap_offsets, range_tile_counts,
            D3D12_TILE_MAPPING_FLAG_NONE);
        list->CopyTiles(
            reserved_texture, &coordinates[0], &region_sizes[0], sparse_upload,
            0, D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        list->CopyTiles(
            reserved_texture, &coordinates[1], &region_sizes[1], sparse_upload,
            sparse_tile_size,
            D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        D3D12_RESOURCE_BARRIER sparse_barrier = transition_barrier(
            reserved_texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &sparse_barrier);
        list->CopyTiles(
            reserved_texture, &coordinates[0], &region_sizes[0], sparse_readback,
            0, D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        list->CopyTiles(
            reserved_texture, &coordinates[1], &region_sizes[1], sparse_readback,
            sparse_tile_size,
            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    }

    ID3D12Resource* bc_texture = nullptr;
    ID3D12Resource* bc_upload = nullptr;
    ID3D12Resource* bc_readback = nullptr;
    D3D12_RESOURCE_DESC bc_desc = texture_desc(7, 5, DXGI_FORMAT_BC1_UNORM);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT bc_footprint = {};
    UINT bc_rows = 0;
    UINT64 bc_row_bytes = 0;
    UINT64 bc_total_bytes = 0;
    if (device)
        device->GetCopyableFootprints(&bc_desc, 0, 1, 0, &bc_footprint,
                                      &bc_rows, &bc_row_bytes, &bc_total_bytes);
    D3D12_RESOURCE_DESC bc_staging_desc = buffer_desc(bc_total_bytes);
    HRESULT bc_texture_hr =
        device ? device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                                                 &bc_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&bc_texture))
               : E_FAIL;
    HRESULT bc_upload_hr =
        device ? device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                 &bc_staging_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&bc_upload))
               : E_FAIL;
    HRESULT bc_readback_hr =
        device ? device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
                                                 &bc_staging_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&bc_readback))
               : E_FAIL;
    uint8_t* bc_upload_ptr = nullptr;
    HRESULT bc_upload_map_hr =
        bc_upload ? bc_upload->Map(0, nullptr, reinterpret_cast<void**>(&bc_upload_ptr)) : E_FAIL;
    if (SUCCEEDED(bc_upload_map_hr) && bc_upload_ptr) {
        std::memset(bc_upload_ptr, 0, static_cast<size_t>(bc_total_bytes));
        for (UINT row = 0; row < bc_rows; ++row) {
            for (UINT64 byte = 0; byte < bc_row_bytes; ++byte) {
                size_t offset = static_cast<size_t>(bc_footprint.Offset +
                                                    row * bc_footprint.Footprint.RowPitch + byte);
                bc_upload_ptr[offset] = static_cast<uint8_t>(0x31u + row * 17u + byte);
            }
        }
        bc_upload->Unmap(0, nullptr);
    }
    if (list && bc_texture && bc_upload && bc_readback) {
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = bc_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = bc_footprint;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = bc_texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier =
            transition_barrier(bc_texture, D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION readback_dst = {};
        readback_dst.pResource = bc_readback;
        readback_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readback_dst.PlacedFootprint = bc_footprint;
        D3D12_TEXTURE_COPY_LOCATION texture_src = {};
        texture_src.pResource = bc_texture;
        texture_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&readback_dst, 0, 0, 0, &texture_src, nullptr);
    }

    HRESULT close_hr = list ? list->Close() : E_FAIL;
    HRESULT execute_hr = E_FAIL;
    HRESULT signal_hr = E_FAIL;
    HRESULT wait_hr = E_FAIL;
    if (queue && list && fence && SUCCEEDED(close_hr)) {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        execute_hr = S_OK;
        signal_hr = queue->Signal(fence, 1);
        HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (event_handle && SUCCEEDED(signal_hr)) {
            wait_hr = fence->SetEventOnCompletion(1, event_handle);
            if (SUCCEEDED(wait_hr))
                WaitForSingleObject(event_handle, 15000);
        }
    }
    bool sparse_unmapped_zero_ok = false;
    if (queue && list && allocator && fence && reserved_texture &&
        sparse_unmapped_readback && SUCCEEDED(wait_hr)) {
        D3D12_TILED_RESOURCE_COORDINATE coordinates[2] = {};
        coordinates[1].Subresource = 1;
        D3D12_TILE_REGION_SIZE region_sizes[2] = {};
        region_sizes[0].NumTiles = 1;
        region_sizes[1].NumTiles = 1;
        D3D12_TILE_RANGE_FLAGS range_flags[2] = {
            D3D12_TILE_RANGE_FLAG_NULL, D3D12_TILE_RANGE_FLAG_NULL};
        UINT range_tile_counts[2] = {1, 1};
        queue->UpdateTileMappings(
            reserved_texture, 2, coordinates, region_sizes, nullptr, 2,
            range_flags, nullptr, range_tile_counts,
            D3D12_TILE_MAPPING_FLAG_NONE);
        sparse_unmap_close_hr = list->Reset(allocator, nullptr);
        if (SUCCEEDED(sparse_unmap_close_hr)) {
            list->CopyTiles(
                reserved_texture, &coordinates[0], &region_sizes[0],
                sparse_unmapped_readback, 0,
                D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            list->CopyTiles(
                reserved_texture, &coordinates[1], &region_sizes[1],
                sparse_unmapped_readback, sparse_tile_size,
                D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
            sparse_unmap_close_hr = list->Close();
        }
        if (SUCCEEDED(sparse_unmap_close_hr)) {
            ID3D12CommandList *lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            sparse_unmap_execute_hr = S_OK;
            sparse_unmap_signal_hr = queue->Signal(fence, 2);
            HANDLE event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            if (event_handle && SUCCEEDED(sparse_unmap_signal_hr)) {
                sparse_unmap_wait_hr =
                    fence->SetEventOnCompletion(2, event_handle);
                if (SUCCEEDED(sparse_unmap_wait_hr) &&
                    WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
                    sparse_unmap_wait_hr = E_FAIL;
            }
        }
    }

    uint8_t* readback_ptr = nullptr;
    HRESULT map_readback_hr =
        readback_buffer ? readback_buffer->Map(0, nullptr, reinterpret_cast<void**>(&readback_ptr)) : E_FAIL;
    bool buffer_copy_ok = SUCCEEDED(map_readback_hr) && readback_ptr;
    if (buffer_copy_ok) {
        for (UINT64 i = 0; i < buffer_bytes; ++i) {
            if (readback_ptr[i] != static_cast<uint8_t>((i * 17u + 3u) & 0xffu)) {
                buffer_copy_ok = false;
                break;
            }
        }
        readback_buffer->Unmap(0, nullptr);
    }

    uint8_t* texture_readback_ptr = nullptr;
    HRESULT texture_readback_map_hr =
        texture_readback ? texture_readback->Map(0, nullptr, reinterpret_cast<void**>(&texture_readback_ptr)) : E_FAIL;
    bool texture_copy_ok = SUCCEEDED(texture_readback_map_hr) && texture_readback_ptr;
    if (texture_copy_ok) {
        for (UINT y = 0; y < 4; ++y) {
            for (UINT x = 0; x < 4; ++x) {
                size_t offset =
                    static_cast<size_t>(texture_footprint.Offset + y * texture_footprint.Footprint.RowPitch + x * 4);
                if (texture_readback_ptr[offset + 0] != static_cast<uint8_t>(x * 40) ||
                    texture_readback_ptr[offset + 1] != static_cast<uint8_t>(y * 40) ||
                    texture_readback_ptr[offset + 2] != 0xa5 || texture_readback_ptr[offset + 3] != 0xff) {
                    texture_copy_ok = false;
                    break;
                }
            }
        }
        texture_readback->Unmap(0, nullptr);
    }

    uint8_t* bc_readback_ptr = nullptr;
    HRESULT bc_readback_map_hr =
        bc_readback ? bc_readback->Map(0, nullptr,
                                      reinterpret_cast<void**>(&bc_readback_ptr))
                    : E_FAIL;
    bool bc_copy_ok = SUCCEEDED(bc_readback_map_hr) && bc_readback_ptr &&
                      bc_rows == 2 && bc_row_bytes == 16;
    if (SUCCEEDED(bc_readback_map_hr) && bc_readback_ptr) {
        if (bc_copy_ok) {
            for (UINT row = 0; row < bc_rows; ++row) {
                for (UINT64 byte = 0; byte < bc_row_bytes; ++byte) {
                    size_t offset = static_cast<size_t>(bc_footprint.Offset +
                                                        row * bc_footprint.Footprint.RowPitch + byte);
                    uint8_t expected = static_cast<uint8_t>(0x31u + row * 17u + byte);
                    if (bc_readback_ptr[offset] != expected) {
                        bc_copy_ok = false;
                        break;
                    }
                }
            }
        }
        D3D12_RANGE written = {0, 0};
        bc_readback->Unmap(0, &written);
    }

    uint8_t *sparse_readback_ptr = nullptr;
    HRESULT sparse_readback_map_hr =
        sparse_readback
            ? sparse_readback->Map(
                  0, nullptr, reinterpret_cast<void **>(&sparse_readback_ptr))
            : E_FAIL;
    if (SUCCEEDED(sparse_readback_map_hr) && sparse_readback_ptr) {
        sparse_copy_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (sparse_readback_ptr[i] !=
                static_cast<uint8_t>((i * 29u + 7u) & 0xffu)) {
                sparse_copy_ok = false;
                break;
            }
        }
        sparse_readback->Unmap(0, nullptr);
    }
    uint8_t *sparse_unmapped_ptr = nullptr;
    HRESULT sparse_unmapped_map_hr =
        sparse_unmapped_readback
            ? sparse_unmapped_readback->Map(
                  0, nullptr, reinterpret_cast<void **>(&sparse_unmapped_ptr))
            : E_FAIL;
    if (SUCCEEDED(sparse_unmapped_map_hr) && sparse_unmapped_ptr) {
        sparse_unmapped_zero_ok = true;
        for (UINT64 i = 0; i < sparse_tile_bytes; i++) {
            if (sparse_unmapped_ptr[i] != 0) {
                sparse_unmapped_zero_ok = false;
                break;
            }
        }
        sparse_unmapped_readback->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_DESC texture_roundtrip_desc = texture ? texture->GetDesc() : D3D12_RESOURCE_DESC{};

    std::vector<FormatProbe> formats = {
        {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM},
        {"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM},
        {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT},
        {"D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT},
        {"D32_FLOAT", DXGI_FORMAT_D32_FLOAT},
        {"R32_UINT", DXGI_FORMAT_R32_UINT},
    };
    for (auto& format : formats) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
        support.Format = format.format;
        format.hr =
            device ? device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)) : E_FAIL;
        format.support1 = support.Support1;
        format.support2 = support.Support2;
    }

    bool format_support_ok = true;
    for (const auto& format : formats) {
        if (FAILED(format.hr))
            format_support_ok = false;
    }

    const bool shared_handle_roundtrip =
        SUCCEEDED(shared_create_hr) && SUCCEEDED(shared_open_hr) &&
        SUCCEEDED(shared_open_named_hr) && shared_handle &&
        shared_named_handle && shared_open_buffer &&
        shared_named_open_buffer &&
        shared_open_buffer->GetGPUVirtualAddress() == default_gpu_va &&
        shared_named_open_buffer->GetGPUVirtualAddress() == default_gpu_va &&
        shared_unknown_hr == DXGI_ERROR_INVALID_CALL &&
        shared_missing_name_hr == DXGI_ERROR_NOT_FOUND;
    if (shared_handle)
        CloseHandle(shared_handle);
    if (shared_named_handle)
        CloseHandle(shared_named_handle);

    bool pass = SUCCEEDED(create_hr) && SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                SUCCEEDED(fence_hr) && SUCCEEDED(upload_buffer_hr) && SUCCEEDED(default_buffer_hr) &&
                SUCCEEDED(readback_buffer_hr) && SUCCEEDED(map_upload_hr) && SUCCEEDED(close_hr) &&
                SUCCEEDED(execute_hr) && SUCCEEDED(signal_hr) && SUCCEEDED(wait_hr) && SUCCEEDED(map_readback_hr) &&
                buffer_copy_ok && SUCCEEDED(texture_hr) && SUCCEEDED(texture_upload_hr) &&
                SUCCEEDED(texture_readback_hr) && SUCCEEDED(texture_map_hr) && SUCCEEDED(texture_readback_map_hr) &&
                texture_copy_ok && SUCCEEDED(bc_texture_hr) && SUCCEEDED(bc_upload_hr) &&
                SUCCEEDED(bc_readback_hr) && SUCCEEDED(bc_upload_map_hr) &&
                SUCCEEDED(bc_readback_map_hr) && bc_copy_ok &&
                SUCCEEDED(sparse_heap_hr) && SUCCEEDED(reserved_texture_hr) &&
                SUCCEEDED(sparse_tiling_hr) && SUCCEEDED(sparse_upload_hr) && SUCCEEDED(sparse_readback_hr) &&
                SUCCEEDED(sparse_upload_map_hr) &&
                SUCCEEDED(sparse_readback_map_hr) && sparse_copy_ok &&
                SUCCEEDED(sparse_unmapped_readback_hr) &&
                SUCCEEDED(sparse_unmap_close_hr) &&
                SUCCEEDED(sparse_unmap_execute_hr) &&
                SUCCEEDED(sparse_unmap_signal_hr) &&
                SUCCEEDED(sparse_unmap_wait_hr) &&
                SUCCEEDED(sparse_unmapped_map_hr) &&
                sparse_unmapped_zero_ok && command_resource_lifetime_ok &&
                sparse_total_tiles == 2 && sparse_tiling_count == 2 &&
                sparse_tile_shape.WidthInTexels == 128 &&
                sparse_tile_shape.HeightInTexels == 128 &&
                sparse_tiling[0].WidthInTiles == 1 &&
                sparse_tiling[0].HeightInTiles == 1 &&
                sparse_tiling[1].WidthInTiles == 1 &&
                sparse_tiling[1].HeightInTiles == 1 &&
                default_buffer_desc.Width == buffer_bytes && texture_roundtrip_desc.Width == 4 &&
                texture_roundtrip_desc.Height == 4 && upload_gpu_va != 0 && default_gpu_va != 0 &&
                shared_handle_roundtrip && format_support_ok;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-resources.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"device_create\": {\n");
    print_hr("hr", create_hr, false);
    std::printf("  },\n");
    std::printf("  \"command_execution\": {\n");
    print_hr("queue", queue_hr);
    print_hr("allocator", allocator_hr);
    print_hr("list", list_hr);
    print_hr("fence", fence_hr);
    print_hr("close", close_hr);
    print_hr("execute", execute_hr);
    print_hr("signal", signal_hr);
    print_hr("wait", wait_hr, false);
    std::printf("  },\n");
    std::printf("  \"buffers\": {\n");
    print_hr("upload_create", upload_buffer_hr);
    print_hr("default_create", default_buffer_hr);
    print_hr("readback_create", readback_buffer_hr);
    print_hr("upload_map", map_upload_hr);
    print_hr("readback_map", map_readback_hr);
    std::printf("    \"copy_verified\": %s,\n", buffer_copy_ok ? "true" : "false");
    std::printf("    \"default_desc_width\": %llu,\n", static_cast<unsigned long long>(default_buffer_desc.Width));
    std::printf("    \"upload_gpu_va_nonzero\": %s,\n", upload_gpu_va != 0 ? "true" : "false");
    std::printf("    \"default_gpu_va_nonzero\": %s,\n", default_gpu_va != 0 ? "true" : "false");
    std::printf("    \"command_resource_lifetime_verified\": %s\n",
                command_resource_lifetime_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"shared_handles\": {\n");
    print_hr("create", shared_create_hr);
    print_hr("open", shared_open_hr);
    print_hr("open_by_name", shared_open_named_hr);
    print_hr("unknown_handle", shared_unknown_hr);
    print_hr("missing_name", shared_missing_name_hr);
    std::printf("    \"roundtrip_verified\": %s\n",
                shared_handle_roundtrip ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"textures\": {\n");
    print_hr("texture_create", texture_hr);
    print_hr("upload_create", texture_upload_hr);
    print_hr("readback_create", texture_readback_hr);
    print_hr("upload_map", texture_map_hr);
    print_hr("readback_map", texture_readback_map_hr);
    std::printf("    \"copy_verified\": %s,\n", texture_copy_ok ? "true" : "false");
    std::printf("    \"width\": %llu,\n", static_cast<unsigned long long>(texture_roundtrip_desc.Width));
    std::printf("    \"height\": %u,\n", texture_roundtrip_desc.Height);
    std::printf("    \"row_pitch\": %u,\n", texture_footprint.Footprint.RowPitch);
    std::printf("    \"upload_bytes\": %llu,\n", static_cast<unsigned long long>(texture_upload_bytes));
    std::printf("    \"unaligned_bc1_create_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_texture_hr)));
    std::printf("    \"unaligned_bc1_upload_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_upload_hr)));
    std::printf("    \"unaligned_bc1_readback_hr\": \"0x%08lx\",\n",
                static_cast<unsigned long>(static_cast<uint32_t>(bc_readback_hr)));
    std::printf("    \"unaligned_bc1_copy_verified\": %s,\n",
                bc_copy_ok ? "true" : "false");
    std::printf("    \"unaligned_bc1_width\": 7,\n");
    std::printf("    \"unaligned_bc1_height\": 5,\n");
    std::printf("    \"unaligned_bc1_rows\": %u,\n", bc_rows);
    std::printf("    \"unaligned_bc1_row_bytes\": %llu\n",
                static_cast<unsigned long long>(bc_row_bytes));
    std::printf("  },\n");
    std::printf("  \"sparse\": {\n");
    print_hr("heap_create", sparse_heap_hr);
    print_hr("reserved_resource_create", reserved_texture_hr);
    print_hr("upload_create", sparse_upload_hr);
    print_hr("readback_create", sparse_readback_hr);
    print_hr("unmapped_readback_create", sparse_unmapped_readback_hr);
    print_hr("upload_map", sparse_upload_map_hr);
    print_hr("readback_map", sparse_readback_map_hr);
    print_hr("unmap_close", sparse_unmap_close_hr);
    print_hr("unmap_execute", sparse_unmap_execute_hr);
    print_hr("unmap_signal", sparse_unmap_signal_hr);
    print_hr("unmap_wait", sparse_unmap_wait_hr);
    print_hr("unmapped_readback_map", sparse_unmapped_map_hr);
    std::printf("    \"total_tiles\": %u,\n", sparse_total_tiles);
    std::printf("    \"tiling_count\": %u,\n", sparse_tiling_count);
    std::printf("    \"tile_shape\": [%u, %u, %u],\n",
                sparse_tile_shape.WidthInTexels,
                sparse_tile_shape.HeightInTexels,
                sparse_tile_shape.DepthInTexels);
    std::printf("    \"subresource_tiling\": [[%u, %u, %u, %u], [%u, %u, %u, %u]],\n",
                sparse_tiling[0].WidthInTiles, sparse_tiling[0].HeightInTiles,
                sparse_tiling[0].DepthInTiles,
                sparse_tiling[0].StartTileIndexInOverallResource,
                sparse_tiling[1].WidthInTiles, sparse_tiling[1].HeightInTiles,
                sparse_tiling[1].DepthInTiles,
                sparse_tiling[1].StartTileIndexInOverallResource);
    std::printf("    \"copy_verified\": %s,\n",
                sparse_copy_ok ? "true" : "false");
    std::printf("    \"unmapped_zero_verified\": %s\n",
                sparse_unmapped_zero_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"formats\": {\n");
    for (size_t i = 0; i < formats.size(); ++i)
        print_format_json(formats[i], i + 1 == formats.size());
    std::printf("  }\n");
    std::printf("}\n");

    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
