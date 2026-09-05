#pragma once
#include <cstdint>
#include <cstddef>

// Internal node-input ABI. Output records use the same work-graph backing
// allocation only when it is large enough for the bounded native allocator
// below; user UAVs remain a separate binding.
inline constexpr uint32_t kNodeOutputRecordStride = 256;
inline constexpr uint32_t kNodeOutputRecordBase = 8192;
inline constexpr uint32_t kNodeOutputRecordSlots = 4096;
inline constexpr uint32_t kNodeOutputAllocationBase = 32;
inline constexpr uint32_t kNodeOutputAllocationStride = 16;
inline constexpr uint32_t kNodeOutputMaxAllocations = 256;
inline constexpr uint32_t kNodeOutputBackingBytes =
    kNodeOutputRecordBase + kNodeOutputRecordStride * kNodeOutputRecordSlots;

struct D3D12NodeInputContext {
  uint32_t version = 1;
  uint32_t record_count = 0;
  uint64_t record_stride = 0;
  uint64_t record_size = 0;
  uint64_t byte_length = 0;
};
static_assert(sizeof(D3D12NodeInputContext) == 32);
static_assert(offsetof(D3D12NodeInputContext, record_stride) == 8);
static_assert(offsetof(D3D12NodeInputContext, byte_length) == 24);

// Version 3 describes a GPU-produced descriptor stream. The scheduler fills
// record_count/byte_length after compacting published output allocations; the
// consumer shader reads the stream using its group index, so no CPU readback
// or record-by-record host scheduling is involved.
struct D3D12NodeDynamicInputContext {
  uint32_t version = 3;
  uint32_t record_count = 0;
  uint64_t record_stride = sizeof(uint32_t) * 2;
  uint64_t record_size = 0;
  uint64_t byte_length = 0;
  uint32_t batch_size = 1;
  // Broadcasting groups per record on X. Flattened fixed grids use this to
  // select a record; per-record ICB dispatches resolve to descriptor zero.
  // Zero retains the thread/coalescing mapping (or an empty broadcast grid).
  uint32_t reserved = 0;
};
static_assert(sizeof(D3D12NodeDynamicInputContext) == 40);
static_assert(offsetof(D3D12NodeDynamicInputContext, batch_size) == 32);
static_assert(offsetof(D3D12NodeDynamicInputContext, reserved) == 36);
