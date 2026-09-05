#pragma once
#include "llvm_bitcode.hpp"
#include <charconv>
#include <limits>
#include <string>
#include <vector>

namespace dxmt::dxil {

struct NodeInputLayout {
  uint32_t size = 0;
  uint32_t alignment = 0;
  uint32_t max_records = 0;
};

// A node output is identified by its metadata ordinal within the entrypoint.
// The destination name and array index are retained so command replay can
// route GPU-published records without guessing from the node array order.
struct NodeOutputLayout {
  std::string node_name;
  uint32_t array_index = 0;
  uint32_t metadata_index = 0;
  uint32_t size = 0;
  uint32_t alignment = 0;
  uint32_t max_records = 0;
  uint32_t flags = 0;
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
  uint32_t max_grid[3] = {1, 1, 1};
  bool grid_from_record = false;
  uint32_t grid_byte_offset = 0;
  uint32_t grid_components = 0;
  uint32_t grid_component_bytes = 0;
  std::vector<NodeOutputLayout> outputs;
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
  const LLVMMetadataRecord *max_grid = nullptr;
  if (!tag(module, *properties, 13, launch) || !integer(module, launch, result.launch_type) ||
      result.launch_type < 1 || result.launch_type > 3 ||
      !tag(module, *properties, 4, threads) || !tag(module, *properties, 18, grid) ||
      !tag(module, *properties, 22, max_grid))
    return std::nullopt;
  // A broadcasting node may carry either a fixed NodeDispatchGrid or a
  // bounded record-driven NodeMaxDispatchGrid. Never substitute the maximum
  // for the actual per-record grid; replay builds indirect arguments on GPU.
  if (result.launch_type == 1 && !grid && !max_grid) return std::nullopt;
  if (result.launch_type != 3 && !threads) return std::nullopt;
  result.grid_from_record = result.launch_type == 1 && !grid;
  if (result.grid_from_record) {
    const LLVMMetadataRecord *inputs = nullptr, *type = nullptr, *semantic = nullptr;
    if (!tag(module, *properties, 20, inputs) || !inputs || inputs->operands.size() != 1)
      return std::nullopt;
    const auto *input_record = module.metadataOperand(*inputs, 0);
    uint32_t component_type = 0;
    if (!input_record || !tag(module, *input_record, 2, type) || !type ||
        !tag(module, *type, 1, semantic) || !semantic || semantic->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*semantic, 0), result.grid_byte_offset) ||
        !integer(module, module.metadataOperand(*semantic, 1), component_type) ||
        !integer(module, module.metadataOperand(*semantic, 2), result.grid_components) ||
        (component_type != 3u && component_type != 5u) ||
        result.grid_components < 1u || result.grid_components > 3u)
      return std::nullopt;
    result.grid_component_bytes = component_type == 3u ? 2u : 4u;
    if (result.grid_byte_offset % result.grid_component_bytes ||
        result.grid_byte_offset > result.input.size ||
        result.grid_components * result.grid_component_bytes > result.input.size - result.grid_byte_offset)
      return std::nullopt;
  }
  for (unsigned i = 0; i < 3; ++i) {
    if (threads && (threads->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*threads, i), result.threads[i])))
      return std::nullopt;
    if (grid && (grid->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*grid, i), result.grid[i])))
      return std::nullopt;
    if (max_grid && (max_grid->operands.size() != 3 ||
        !integer(module, module.metadataOperand(*max_grid, i), result.max_grid[i])))
      return std::nullopt;
    if (!result.threads[i] || result.threads[i] > 1024 ||
        (grid && (!result.grid[i] || result.grid[i] > 65535)) ||
        (max_grid && (!result.max_grid[i] || result.max_grid[i] > 65535)))
      return std::nullopt;
    if (result.grid_from_record)
      result.grid[i] = 1;
    else if (!grid)
      result.grid[i] = result.max_grid[i];
    if (!max_grid)
      result.max_grid[i] = result.grid[i];
  }
  if (uint64_t(result.threads[0]) * result.threads[1] * result.threads[2] > 1024)
    return std::nullopt;

  const LLVMMetadataRecord *outputs = nullptr;
  if (!tag(module, *properties, 21, outputs))
    return std::nullopt;
  if (outputs) {
    if (outputs->kind != LLVMMetadataRecord::Kind::Node)
      return std::nullopt;
    try {
      result.outputs.reserve(outputs->operands.size());
    } catch (...) {
      return std::nullopt;
    }
    for (size_t output_index = 0; output_index < outputs->operands.size();
         ++output_index) {
      const auto *output = module.metadataOperand(*outputs, output_index);
      const LLVMMetadataRecord *flags_record = nullptr;
      const LLVMMetadataRecord *type = nullptr;
      const LLVMMetadataRecord *max_records_record = nullptr;
      const LLVMMetadataRecord *node_id = nullptr;
      NodeOutputLayout layout;
      layout.metadata_index = static_cast<uint32_t>(output_index);
      if (!output || !tag(module, *output, 1, flags_record) ||
          !integer(module, flags_record, layout.flags) ||
          !tag(module, *output, 2, type) || !type ||
          !tag(module, *type, 0, flags_record) ||
          !integer(module, flags_record, layout.size) ||
          !tag(module, *type, 2, flags_record) ||
          !integer(module, flags_record, layout.alignment) ||
          !tag(module, *output, 3, max_records_record) ||
          (max_records_record &&
           !integer(module, max_records_record, layout.max_records)) ||
          !tag(module, *output, 0, node_id) || !node_id ||
          node_id->kind != LLVMMetadataRecord::Kind::Node ||
          node_id->operands.size() != 2)
        return std::nullopt;
      const auto *name = module.metadataOperand(*node_id, 0);
      const auto *array_index = module.metadataOperand(*node_id, 1);
      if (!name || name->kind != LLVMMetadataRecord::Kind::String ||
          name->string_value.empty() || !integer(module, array_index,
                                                  layout.array_index) ||
          !layout.alignment || (layout.alignment & (layout.alignment - 1)) ||
          layout.size > std::numeric_limits<uint32_t>::max() - 3u)
        return std::nullopt;
      layout.node_name = name->string_value;
      layout.size = (layout.size + 3u) & ~3u;
      if (layout.alignment < 4)
        layout.alignment = 4;
      result.outputs.push_back(std::move(layout));
    }
  }
  return result;
}
} // namespace dxmt::dxil
