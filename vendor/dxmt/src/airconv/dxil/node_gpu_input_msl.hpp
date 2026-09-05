#pragma once
namespace dxmt::dxil {
inline constexpr const char kNodeGPUInputMSL[] = R"MSL(
struct m12_node_gpu_input_header {
  uint entrypoint_index, record_count;
  ulong records, record_stride;
};
struct m12_node_gpu_address_bound { ulong address, prefix_end; };
struct m12_node_gpu_entry_layout {
  uint record_size, record_alignment, available, launch_type;
  uint threads_x, threads_y, threads_z, max_records;
  uint grid_x, grid_y, grid_z, grid_from_record;
};
static_assert(sizeof(m12_node_gpu_entry_layout) == 48, "entry layout ABI");
static_assert(sizeof(m12_node_gpu_input_header) == 24, "GPU header ABI");
static_assert(sizeof(m12_node_gpu_address_bound) == 16, "GPU address bound ABI");
static inline bool m12_node_gpu_address_contains(
    device const m12_node_gpu_address_bound *bounds, uint count,
    ulong address, ulong length) {
  if (bounds == nullptr || count == 0u) return false;
  uint lo = 0u, hi = count;
  while (lo < hi) {
    uint mid = lo + (hi - lo) / 2u;
    if (bounds[mid].address <= address) lo = mid + 1u;
    else hi = mid;
  }
  if (lo == 0u) return false;
  ulong end = bounds[lo - 1u].prefix_end;
  return address <= end && length <= end - address;
}
static inline bool m12_node_gpu_input_valid(m12_node_gpu_input_header header,
    device const m12_node_gpu_entry_layout *entries, uint entry_count,
    device const m12_node_gpu_address_bound *bounds, uint bound_count) {
  if (entries == nullptr || header.entrypoint_index >= entry_count) return false;
  auto entry = entries[header.entrypoint_index];
  if (entry.available == 0u) return false;
  if (header.record_count == 0u || entry.record_size == 0u) return true;
  ulong alignment = max(ulong(entry.record_alignment), 4ul);
  if (entry.record_alignment == 0u || (alignment & (alignment - 1ul)) != 0ul ||
      header.records == 0ul || header.records % alignment != 0ul ||
      header.record_stride % alignment != 0ul ||
      (header.record_stride != 0ul && header.record_stride < ulong(entry.record_size))) return false;
  ulong last = ulong(header.record_count) - 1ul;
  if (last != 0ul && header.record_stride > (0xfffffffffffffffful - ulong(entry.record_size)) / last) return false;
  return m12_node_gpu_address_contains(bounds, bound_count, header.records,
      last * header.record_stride + ulong(entry.record_size));
}
)MSL";
} // namespace dxmt::dxil
