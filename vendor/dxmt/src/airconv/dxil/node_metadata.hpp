#pragma once
#include "llvm_bitcode.hpp"
#include <charconv>
#include <limits>

namespace dxmt::dxil {

struct NodeInputLayout {
  uint32_t size = 0;
  uint32_t alignment = 0;
};

namespace node_metadata_detail {
inline bool integer(const LLVMModule &module, const LLVMMetadataRecord *record,
                    uint32_t &value) {
  if (!record || record->kind != LLVMMetadataRecord::Kind::Value) return false;
  for (const auto &constant : module.constants) {
    if (constant.id != record->value_id) continue;
    const auto &text = constant.constant_data;
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
  }
  return false;
}

// Missing tags are valid; duplicate tags, malformed keys, and dangling value
// references are not. Unknown well-formed tags remain in the module graph.
inline bool tag(const LLVMModule &module, const LLVMMetadataRecord &record,
                uint32_t key, const LLVMMetadataRecord *&result) {
  result = nullptr;
  if (record.kind != LLVMMetadataRecord::Kind::Node || record.operands.size() % 2)
    return false;
  bool found = false;
  for (size_t i = 0; i < record.operands.size(); i += 2) {
    uint32_t candidate = 0;
    if (!integer(module, module.metadataOperand(record, i), candidate)) return false;
    const auto *value = module.metadataOperand(record, i + 1);
    if (record.operands[i + 1] && !value) return false;
    if (candidate != key) continue;
    if (found) return false;
    found = true;
    result = value;
  }
  return true;
}
} // namespace node_metadata_detail

// Layout expected by DispatchGraph, including its DWORD minimum granularity.
// No input descriptor (or an explicitly empty input) has layout {0,0}.
// This does not derive graph allocation or scheduling requirements.
inline std::optional<NodeInputLayout> nodeInputLayout(
    const LLVMModule &module, const std::string &entrypoint) {
  using namespace node_metadata_detail;
  const auto named = module.named_metadata.find("dx.entryPoints");
  if (named == module.named_metadata.end()) return std::nullopt;
  const LLVMMetadataRecord *properties = nullptr;
  bool found = false;
  for (uint32_t id : named->second) {
    if (id >= module.metadata_records.size()) return std::nullopt;
    const auto &entry = module.metadata_records[id];
    const auto *name = module.metadataOperand(entry, 1);
    if (!name || name->kind != LLVMMetadataRecord::Kind::String ||
        name->string_value != entrypoint) continue;
    if (found) return std::nullopt;
    found = true;
    properties = module.metadataOperand(entry, 4);
  }
  if (!properties) return std::nullopt;
  const LLVMMetadataRecord *kind = nullptr, *inputs = nullptr;
  uint32_t shader_kind = 0;
  if (!tag(module, *properties, 8, kind) || !integer(module, kind, shader_kind) ||
      shader_kind != 15 || !tag(module, *properties, 20, inputs))
    return std::nullopt;
  if (!inputs) return NodeInputLayout{};
  if (inputs->kind != LLVMMetadataRecord::Kind::Node) return std::nullopt;
  if (inputs->operands.empty()) return NodeInputLayout{};
  if (inputs->operands.size() != 1) return std::nullopt;
  const auto *input = module.metadataOperand(*inputs, 0);
  const LLVMMetadataRecord *flags_record = nullptr, *type = nullptr;
  uint32_t flags = 0;
  if (!input || !tag(module, *input, 1, flags_record) ||
      !integer(module, flags_record, flags) || !(flags & 1u) ||
      !tag(module, *input, 2, type)) return std::nullopt;
  if (flags & 8u) return NodeInputLayout{};
  const LLVMMetadataRecord *size_record = nullptr, *alignment_record = nullptr;
  NodeInputLayout layout;
  if (!type || !tag(module, *type, 0, size_record) ||
      !tag(module, *type, 2, alignment_record) ||
      !integer(module, size_record, layout.size) ||
      !integer(module, alignment_record, layout.alignment) ||
      !layout.alignment || (layout.alignment & (layout.alignment - 1)) ||
      layout.size > std::numeric_limits<uint32_t>::max() - 3u)
    return std::nullopt;
  if (!layout.size) return NodeInputLayout{};
  layout.size = (layout.size + 3u) & ~3u;
  if (layout.alignment < 4) layout.alignment = 4;
  return layout;
}
} // namespace dxmt::dxil
