#pragma once
#include "d3d12_node_dispatch_abi.hpp"
#include <algorithm>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace dxmt {
struct NodeRoutingTarget {
  std::string name;
  uint32_t array_index;
  uint32_t max_recursion_depth = 0;
};
struct NodeRoutingOutput {
  uint32_t source_node;
  uint32_t metadata_index;
  std::string target_name;
  uint32_t target_base_index = 0;
  uint32_t array_size = 1;
  bool is_array = false;
  bool allow_sparse = false;
};
inline constexpr uint32_t kNodeRouteArray = 1u;
inline constexpr uint32_t kNodeRouteSparse = 2u;

// Build only declared/available routes, never array_size entries. Unbounded
// sparse arrays therefore cost space proportional to actual graph nodes.
// Row tokens are row-index + 1; zero is invalid. A base row at array index zero
// retains an array descriptor even when sparse element zero has no target.
inline std::optional<std::vector<D3D12NodeOutputRoute>> buildNodeOutputRoutes(
    const std::vector<NodeRoutingTarget> &nodes,
    const std::vector<NodeRoutingOutput> &outputs) {
  if (nodes.size() > UINT32_MAX) return std::nullopt;
  try {
    std::set<std::pair<std::string, uint32_t>> identities;
    // The specification charges gaps in each named node array against the
    // graph budget, independently of our compact physical table. Each node's
    // declared recursion depth adds slots even when its name shares a span.
    std::map<std::string, uint32_t> spans;
    uint64_t node_budget = 0;
    for (const auto &node : nodes) {
      if (node.name.empty() || node.array_index >= 0xffffffu ||
          !identities.emplace(node.name, node.array_index).second)
        return std::nullopt;
      node_budget += node.max_recursion_depth;
      if (node_budget > 0xffffffu) return std::nullopt;
      auto &span = spans[node.name];
      const uint32_t required = node.array_index + 1u;
      if (required > span) {
        node_budget += required - span;
        span = required;
        if (node_budget > 0xffffffu) return std::nullopt;
      }
    }
    std::set<std::pair<uint32_t, uint32_t>> declarations;
    std::vector<D3D12NodeOutputRoute> routes;
    for (const auto &output : outputs) {
      if (output.source_node >= nodes.size() || output.target_name.empty() ||
          !output.array_size || (!output.is_array && output.array_size != 1) ||
          (output.is_array && output.array_size == UINT32_MAX && !output.allow_sparse) ||
          !declarations.emplace(output.source_node, output.metadata_index).second)
        return std::nullopt;
      std::vector<std::pair<uint32_t, uint32_t>> targets;
      for (size_t target = 0; target < nodes.size(); ++target) {
        const auto &node = nodes[target];
        if (node.name != output.target_name || node.array_index < output.target_base_index) continue;
        const uint32_t index = node.array_index - output.target_base_index;
        const bool unbounded = output.is_array && output.allow_sparse && output.array_size == UINT32_MAX;
        if (!unbounded && index >= output.array_size) continue;
        if (target == output.source_node && !nodes[target].max_recursion_depth)
          return std::nullopt;
        targets.emplace_back(index, static_cast<uint32_t>(target));
      }
      if (!output.allow_sparse && targets.size() != output.array_size)
        return std::nullopt;
      if (std::none_of(targets.begin(), targets.end(), [](auto target) { return target.first == 0; }))
        targets.emplace_back(0, UINT32_MAX);
      // An invalid route preserves descriptor identity for sparse misses;
      // it must not alias a populated element zero.
      targets.emplace_back(UINT32_MAX, UINT32_MAX);
      for (auto target : targets) {
        if (routes.size() == UINT32_MAX) return std::nullopt;
        routes.push_back({output.source_node, output.metadata_index, target.first,
                          output.array_size, target.second,
                          (output.is_array ? kNodeRouteArray : 0u) |
                          (output.allow_sparse ? kNodeRouteSparse : 0u)});
      }
    }
    std::sort(routes.begin(), routes.end(), [](const auto &a, const auto &b) {
      return std::tie(a.source_node, a.metadata_index, a.array_index) <
             std::tie(b.source_node, b.metadata_index, b.array_index);
    });
    return routes;
  } catch (...) {
    return std::nullopt;
  }
}
} // namespace dxmt
