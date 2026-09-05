#include "../../src/d3d12/d3d12_node_gpu_input.hpp"
#include "../../src/d3d12/d3d12_node_dispatch_abi.hpp"
#include <iostream>
#include <stdexcept>
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    require(kNodeOutputAllocationBase == 32u, "allocator descriptor base drifted");
    require(kNodeOutputAllocationStride == 16u, "allocator descriptor stride drifted");
    require(sizeof(D3D12NodeDynamicInputContext) == 40u, "dynamic context ABI drifted");
    require(offsetof(D3D12NodeDynamicInputContext, byte_length) == 24u,
            "dynamic context byte-length offset drifted");
    auto bounds = BuildNodeGPUAddressBounds({{100,1000},{200,10},{100,20},{1100,100}});
    require(bool(bounds), "valid aliases rejected");
    require(NodeGPUAddressContains(*bounds, 205, 500), "short alias hides containing allocation");
    require(NodeGPUAddressContains(*bounds, 100, 1000), "exact allocation span rejected");
    require(!NodeGPUAddressContains(*bounds, 1090, 20), "adjacent allocations incorrectly joined");
    require(!NodeGPUAddressContains(*bounds, 99, 1), "before-start address accepted");
    require(!NodeGPUAddressContains(*bounds, 1200, 1), "past-end address accepted");
    require(!NodeGPUAddressContains(*bounds, 100, UINT64_MAX), "wrapping query accepted");
    require(!BuildNodeGPUAddressBounds({{UINT64_MAX-2,4}}), "wrapping allocation accepted");
    require(!BuildNodeGPUAddressBounds({{0,16}}), "null allocation accepted");
    require(!BuildNodeGPUAddressBounds({{1,0}}), "empty allocation accepted");
    auto reversed = BuildNodeGPUAddressBounds({{1100,100},{100,20},{200,10},{100,1000}});
    require(reversed && reversed->size() == bounds->size(), "permutation changed table size");
    for (size_t i = 0; i < bounds->size(); ++i)
      require((*bounds)[i].address == (*reversed)[i].address &&
              (*bounds)[i].prefix_end == (*reversed)[i].prefix_end, "table depends on registration order");
    const auto payload = BuildNodeGPUAddressBounds({{4096,24}});
    const std::vector<D3D12NodeGPUEntryLayout> entries = {{8,4,1,0},{0,0,1,0},{8,4,0,0}};
    require(payload && ValidateNodeGPUInput({0,2,4096,16}, entries, *payload),
            "last-record trailing padding incorrectly required");
    require(ValidateNodeGPUInput({0,UINT32_MAX,4096,0}, entries, *payload),
            "zero-stride replication rejected");
    require(ValidateNodeGPUInput({1,99,UINT64_MAX,UINT64_MAX}, entries, *payload),
            "empty input pointer/stride not ignored");
    require(ValidateNodeGPUInput({0,0,0,UINT64_MAX}, entries, *payload), "zero work rejected");
    require(!ValidateNodeGPUInput({0,3,4096,16}, entries, *payload), "payload overrun accepted");
    require(!ValidateNodeGPUInput({0,2,4096,4}, entries, *payload), "short stride accepted");
    require(!ValidateNodeGPUInput({0,1,4097,8}, entries, *payload), "misaligned payload accepted");
    require(!ValidateNodeGPUInput({0,UINT32_MAX,4096,UINT64_MAX-3}, entries, *payload),
            "span overflow accepted");
    require(!ValidateNodeGPUInput({2,1,4096,8}, entries, *payload), "unavailable entry accepted");
    require(!ValidateNodeGPUInput({3,0,0,0}, entries, *payload), "unknown entry accepted");
    std::cout << "PASS: alias-aware GPU input bounds and header validation\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
