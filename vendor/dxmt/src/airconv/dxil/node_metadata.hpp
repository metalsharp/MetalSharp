#pragma once
#include "llvm_bitcode.hpp"
#include <charconv>
#include <limits>

namespace dxmt::dxil {

struct NodeInputLayout {
  uint32_t size = 0;
  uint32_t alignment = 0;
  uint32_t max_records = 0;
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
  const LLVMMetadataRecord *max_records_record = nullptr;
  NodeInputLayout layout;
  if (!type || !tag(module, *type, 0, size_record) ||
      !tag(module, *type, 2, alignment_record) ||
      !integer(module, size_record, layout.size) ||
      !integer(module, alignment_record, layout.alignment) ||
      !tag(module, *input, 3, max_records_record) ||
      (max_records_record && !integer(module, max_records_record,
                                      layout.max_records)) ||
      !layout.alignment || (layout.alignment & (layout.alignment - 1)) ||
      layout.size > std::numeric_limits<uint32_t>::max() - 3u)
    return std::nullopt;
  if (!layout.size) return NodeInputLayout{};
  layout.size = (layout.size + 3u) & ~3u;
  if (layout.alignment < 4) layout.alignment = 4;
  return layout;
}

struct NodeShaderMetadata {
  NodeInputLayout input;
  uint32_t max_input_records = 0;
  uint32_t launch_type = 0;
  uint32_t threads[3] = {1, 1, 1};
  uint32_t grid[3] = {1, 1, 1};
};

inline std::optional<NodeShaderMetadata> nodeShaderMetadata(
    const LLVMModule &module, const std::string &entrypoint) {
  using namespace node_metadata_detail;
  const auto input = nodeInputLayout(module, entrypoint);
  if (!input) return std::nullopt;
  NodeShaderMetadata result;
  result.input = *input;
  result.max_input_records = result.input.max_records;
  const auto &entries = module.named_metadata.at("dx.entryPoints");
  const LLVMMetadataRecord *properties = nullptr;
  for (uint32_t id : entries) {
    const auto &entry = module.metadata_records[id];
    const auto *name = module.metadataOperand(entry, 1);
    if (name && name->kind == LLVMMetadataRecord::Kind::String &&
        name->string_value == entrypoint)
      properties = module.metadataOperand(entry, 4);
  }
  if (!properties) return std::nullopt;
  const LLVMMetadataRecord *launch = nullptr, *threads = nullptr, *grid = nullptr;
  if (!tag(module, *properties, 13, launch) || !integer(module, launch, result.launch_type) ||
      result.launch_type < 1 || result.launch_type > 3 ||
      !tag(module, *properties, 4, threads) || !tag(module, *properties, 18, grid))
    return std::nullopt;
  // A broadcasting node without a fixed grid needs record-driven grid decoding,
  // which must not be replaced by the max grid or an invented one-group launch.
  if (result.launch_type == 1 && !grid) return std::nullopt;
  if (result.launch_type != 3 && !threads) return std::nullopt;
  for (unsigned i = 0; i < 3; ++i) {
    if (threads && (threads->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*threads, i), result.threads[i])))
      return std::nullopt;
    if (grid && (grid->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*grid, i), result.grid[i])))
      return std::nullopt;
    if (!result.threads[i] || result.threads[i] > 1024 ||
        !result.grid[i] || result.grid[i] > 65535) return std::nullopt;
  }
  if (uint64_t(result.threads[0]) * result.threads[1] * result.threads[2] > 1024)
    return std::nullopt;
  return result;
}
} // namespace dxmt::dxil
