#pragma once
#include "llvm_bitcode.hpp"
#include <limits>

namespace dxmt::dxil {
struct NodeRecordTypeLayout {
  uint64_t size = 0;
  uint64_t alignment = 1;
  std::vector<uint64_t> members;
};

// DXIL node records use scalar-aligned aggregates (HLSL vectors are lowered
// to arrays). Do not apply this to Metal private scratch or arbitrary LLVM
// target layouts. Unsupported types/cycles fail rather than inventing offsets.
inline std::optional<NodeRecordTypeLayout> nodeRecordTypeLayout(
    const LLVMModule &module, uint32_t id, unsigned depth = 0) {
  if (id >= module.types.size() || depth > 64) return std::nullopt;
  const auto &type = module.types[id];
  NodeRecordTypeLayout result;
  switch (type.kind) {
  case LLVMType::Integer:
    if (type.bit_width == 1) return NodeRecordTypeLayout{4, 4, {}};
    if (type.bit_width != 8 && type.bit_width != 16 &&
        type.bit_width != 32 && type.bit_width != 64) return std::nullopt;
    return NodeRecordTypeLayout{type.bit_width / 8u, type.bit_width / 8u, {}};
  case LLVMType::Float:
    if (type.bit_width != 16 && type.bit_width != 32) return std::nullopt;
    return NodeRecordTypeLayout{type.bit_width / 8u, type.bit_width / 8u, {}};
  case LLVMType::Double:
    return NodeRecordTypeLayout{8, 8, {}};
  case LLVMType::Array: {
    if (type.type_refs.size() != 1) return std::nullopt;
    const auto element = nodeRecordTypeLayout(module, type.type_refs[0], depth + 1);
    if (!element || (type.bit_width &&
        element->size > std::numeric_limits<uint64_t>::max() / type.bit_width))
      return std::nullopt;
    return NodeRecordTypeLayout{element->size * type.bit_width, element->alignment, {}};
  }
  case LLVMType::Struct:
    if (type.packed) return std::nullopt;
    for (uint32_t member : type.type_refs) {
      const auto layout = nodeRecordTypeLayout(module, member, depth + 1);
      if (!layout) return std::nullopt;
      if (layout->alignment > result.alignment) result.alignment = layout->alignment;
      const uint64_t mask = layout->alignment - 1;
      if (result.size > std::numeric_limits<uint64_t>::max() - mask) return std::nullopt;
      result.size = (result.size + mask) & ~mask;
      result.members.push_back(result.size);
      if (layout->size > std::numeric_limits<uint64_t>::max() - result.size) return std::nullopt;
      result.size += layout->size;
    }
    if (result.size > std::numeric_limits<uint64_t>::max() - (result.alignment - 1))
      return std::nullopt;
    result.size = (result.size + result.alignment - 1) & ~(result.alignment - 1);
    return result;
  default:
    return std::nullopt;
  }
}
} // namespace dxmt::dxil
