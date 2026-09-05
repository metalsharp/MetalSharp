#pragma once
namespace dxmt::dxil {
inline constexpr const char kNodeRoutingMSL[] = R"MSL(
struct m12_node_route {
  uint source_node, metadata_index, array_index, array_size, target_node, flags;
};
struct m12_node_routing_context {
  uint version, count;
  ulong stride, size, length;
  uint batch_size, reserved;
  device const m12_node_route *routes;
  uint route_count, source_node;
};
struct m12_node_recursion_context {
  m12_node_routing_context routing;
  uint remaining_levels, reserved;
};
static_assert(sizeof(m12_node_recursion_context) == 64, "recursion context ABI");
static_assert(sizeof(m12_node_route) == 24, "route ABI");
static_assert(sizeof(m12_node_routing_context) == 56, "routing context ABI");
static inline device const m12_node_routing_context *m12_node_route_context(device const char *raw) {
  if (raw == nullptr) return nullptr;
  uint version = *reinterpret_cast<device const uint *>(raw);
  if (version != 4u && version != 5u && version != 6u && version != 7u && version != 9u) return nullptr;
  auto c = reinterpret_cast<device const m12_node_routing_context *>(raw);
  return c->routes != nullptr && c->route_count != 0u ? c : nullptr;
}
static inline uint m12_node_route_find(device const m12_node_routing_context *c, uint metadata, uint index) {
  if (c == nullptr) return 0u;
  uint lo = 0u, hi = c->route_count;
  while (lo < hi) {
    uint mid = lo + (hi - lo) / 2u;
    auto row = c->routes[mid];
    bool before = row.source_node < c->source_node ||
      (row.source_node == c->source_node && (row.metadata_index < metadata ||
       (row.metadata_index == metadata && row.array_index < index)));
    if (before) lo = mid + 1u; else hi = mid;
  }
  if (lo == c->route_count) return 0u;
  auto row = c->routes[lo];
  return row.source_node == c->source_node && row.metadata_index == metadata &&
         row.array_index == index ? lo + 1u : 0u;
}
static inline uint m12_node_route_index(device const m12_node_routing_context *c, uint handle, uint index) {
  if (c == nullptr || handle == 0u || handle > c->route_count) return 0u;
  auto base = c->routes[handle - 1u];
  if (base.source_node != c->source_node) return 0u;
  bool unbounded = (base.flags & 3u) == 3u && base.array_size == 0xffffffffu;
  uint found = (unbounded || index < base.array_size) ? m12_node_route_find(c, base.metadata_index, index) : 0u;
  return found != 0u ? found : m12_node_route_find(c, base.metadata_index, 0xffffffffu);
}
static inline bool m12_node_route_valid(device const m12_node_routing_context *c, uint handle) {
  if (c == nullptr || handle == 0u || handle > c->route_count) return false;
  auto row = c->routes[handle - 1u];
  if (row.source_node != c->source_node || row.target_node == 0xffffffffu) return false;
  if (row.target_node == c->source_node && (c->version == 6u || c->version == 7u || c->version == 9u))
    return reinterpret_cast<device const m12_node_recursion_context *>(c)->remaining_levels != 0u;
  return true;
}
static inline uint m12_node_remaining_levels(device const char *raw) {
  auto c = m12_node_route_context(raw);
  if (c == nullptr || (c->version != 6u && c->version != 7u && c->version != 9u)) return 0u;
  return reinterpret_cast<device const m12_node_recursion_context *>(c)->remaining_levels;
}
)MSL";
} // namespace dxmt::dxil
