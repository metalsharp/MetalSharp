#pragma once

#include "d3d12.h"
#include <cstdint>

namespace dxmt {

class MTLD3D12Device;
class MTLD3D12Resource;
class MTLD3D12Heap;
class MTLD3D12Fence;

inline constexpr uint32_t kD3D12SharedResourceMagic = 0x4d534852u;
inline constexpr uint32_t kD3D12SharedResourceVersion = 1;
inline constexpr uint32_t kD3D12SharedResourceKindBuffer = 1;
inline constexpr uint32_t kD3D12SharedResourceKindHeap = 2;
inline constexpr uint32_t kD3D12SharedResourceKindFence = 3;
inline constexpr uint64_t kD3D12SharedResourceDataOffset = 4096;
inline constexpr uint64_t kD3D12SharedFenceValueOffset = 128;

// Fixed-layout metadata stored in a named file mapping.  It contains no
// process-local pointers or Metal object handles and can therefore be opened
// by another Wine process.
struct D3D12SharedResourceMetadata {
  uint32_t magic = kD3D12SharedResourceMagic;
  uint32_t version = kD3D12SharedResourceVersion;
  uint32_t kind = kD3D12SharedResourceKindBuffer;
  uint32_t reserved = 0;
  uint64_t mapping_size = 0;
  uint64_t data_offset = kD3D12SharedResourceDataOffset;
  uint64_t data_size = 0;
  D3D12_RESOURCE_DESC resource_desc = {};
  D3D12_HEAP_PROPERTIES heap_properties = {};
  D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
  uint64_t adapter_luid = 0;
  uint64_t reserved_tail[3] = {};
};

static_assert(sizeof(D3D12SharedResourceMetadata) % 8 == 0);

struct D3D12SharedHeapMetadata {
  uint32_t magic = kD3D12SharedResourceMagic;
  uint32_t version = kD3D12SharedResourceVersion;
  uint32_t kind = kD3D12SharedResourceKindHeap;
  uint32_t reserved = 0;
  uint64_t mapping_size = 0;
  uint64_t data_offset = kD3D12SharedResourceDataOffset;
  uint64_t data_size = 0;
  D3D12_HEAP_DESC heap_desc = {};
  uint64_t adapter_luid = 0;
  uint64_t reserved_tail[3] = {};
};

static_assert(sizeof(D3D12SharedHeapMetadata) % 8 == 0);

struct D3D12SharedFenceMetadata {
  uint32_t magic = kD3D12SharedResourceMagic;
  uint32_t version = kD3D12SharedResourceVersion;
  uint32_t kind = kD3D12SharedResourceKindFence;
  uint32_t reserved = 0;
  uint64_t mapping_size = 4096;
  uint64_t value_offset = kD3D12SharedFenceValueOffset;
  uint64_t initial_value = 0;
  D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAG_NONE;
  uint32_t reserved_flags = 0;
  uint64_t adapter_luid = 0;
  uint64_t reserved_tail[3] = {};
};

static_assert(sizeof(D3D12SharedFenceMetadata) % 8 == 0);

HRESULT CreateSharedBufferMapping(MTLD3D12Resource *resource,
                                  const SECURITY_ATTRIBUTES *attributes,
                                  const WCHAR *name, HANDLE *mapping);
HRESULT OpenSharedBufferFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                    ID3D12Resource **resource);
HRESULT CreateSharedHeapMapping(MTLD3D12Heap *heap,
                                const SECURITY_ATTRIBUTES *attributes,
                                const WCHAR *name, HANDLE *mapping);
HRESULT OpenSharedHeapFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                  ID3D12Heap **heap);
HRESULT CreateSharedFenceMapping(MTLD3D12Fence *fence,
                                 const SECURITY_ATTRIBUTES *attributes,
                                 const WCHAR *name, HANDLE *mapping);
HRESULT OpenSharedFenceFromMapping(MTLD3D12Device *device, HANDLE mapping,
                                   ID3D12Fence **fence);

} // namespace dxmt
