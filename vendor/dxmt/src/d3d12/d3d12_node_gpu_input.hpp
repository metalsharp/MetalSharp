#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct D3D12NodeGPUInputHeader {
  uint32_t entrypoint_index, record_count;
  uint64_t records, record_stride;
};
static_assert(sizeof(D3D12NodeGPUInputHeader) == 24);
static_assert(offsetof(D3D12NodeGPUInputHeader, records) == 8);
static_assert(offsetof(D3D12NodeGPUInputHeader, record_stride) == 16);

struct D3D12NodeGPUAddressRange { uint64_t address, length; };
struct D3D12NodeGPUAddressBound { uint64_t address, prefix_end; };
static_assert(sizeof(D3D12NodeGPUAddressBound) == 16);

struct D3D12NodeGPUEntryLayout {
  uint32_t record_size = 0;
  uint32_t record_alignment = 0;
  uint32_t available = 0;
  uint32_t launch_type = 0;
  uint32_t threads[3] = {1, 1, 1};
  uint32_t max_records = 1;
  uint32_t grid[3] = {1, 1, 1};
  uint32_t grid_from_record = 0;
};
static_assert(sizeof(D3D12NodeGPUEntryLayout) == 48);

// Prefix maxima preserve containing aliases without allowing a query to span
// two adjacent allocations. GPU lookup can binary-search the same table.
inline std::optional<std::vector<D3D12NodeGPUAddressBound>>
BuildNodeGPUAddressBounds(std::vector<D3D12NodeGPUAddressRange> ranges) {
  try {
    for (const auto &range : ranges)
      if (!range.address || !range.length || range.length > UINT64_MAX - range.address)
        return std::nullopt;
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) {
      return a.address != b.address ? a.address < b.address : a.length > b.length;
    });
    std::vector<D3D12NodeGPUAddressBound> bounds;
    bounds.reserve(ranges.size());
    uint64_t end = 0;
    for (const auto &range : ranges) {
      end = std::max(end, range.address + range.length);
      bounds.push_back({range.address, end});
    }
    return bounds;
  } catch (...) {
    return std::nullopt;
  }
}
inline bool NodeGPUAddressContains(const std::vector<D3D12NodeGPUAddressBound> &bounds,
                                   uint64_t address, uint64_t length) {
  auto next = std::upper_bound(bounds.begin(), bounds.end(), address,
      [](uint64_t value, const auto &bound) { return value < bound.address; });
  if (next == bounds.begin()) return false;
  const uint64_t end = (--next)->prefix_end;
  return address <= end && length <= end - address;
}

// Validate only the occupied span, not trailing stride padding. A zero stride
// legally replicates one record; empty input ignores both pointer and stride.
inline bool ValidateNodeGPUInput(const D3D12NodeGPUInputHeader &header,
    const std::vector<D3D12NodeGPUEntryLayout> &entries,
    const std::vector<D3D12NodeGPUAddressBound> &bounds) {
  if (header.entrypoint_index >= entries.size()) return false;
  const auto &entry = entries[header.entrypoint_index];
  if (!entry.available) return false;
  if (!header.record_count || !entry.record_size) return true;
  const uint64_t alignment = std::max<uint64_t>(entry.record_alignment, 4u);
  if (!entry.record_alignment || (alignment & (alignment - 1u)) || !header.records ||
      header.records % alignment || header.record_stride % alignment ||
      (header.record_stride && header.record_stride < entry.record_size)) return false;
  const uint64_t last = uint64_t(header.record_count) - 1u;
  if (last && header.record_stride > (UINT64_MAX - entry.record_size) / last) return false;
  return NodeGPUAddressContains(bounds, header.records, last * header.record_stride + entry.record_size);
}
