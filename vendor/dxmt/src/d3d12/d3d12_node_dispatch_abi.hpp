#pragma once
#include <cstdint>
#include <cstddef>

// Internal node-input ABI. This is not the output-record allocator/scheduler
// ABI. Keep input and output storage distinct, including for RW node inputs.
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
