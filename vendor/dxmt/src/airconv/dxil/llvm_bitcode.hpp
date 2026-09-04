#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <unordered_map>

namespace dxmt::dxil {

struct LLVMType {
  enum Kind {
    Void,
    Float,
    Double,
    Integer,
    Pointer,
    Struct,
    Array,
    Vector,
    Function,
  } kind;
  uint32_t bit_width = 0;
  uint32_t address_space = 0;
  std::vector<LLVMType> subtypes;
  std::vector<uint32_t> type_refs;
  bool packed = false;
};

struct LLVMValue {
  enum Kind {
    Undef,
    Constant,
    Instruction,
    Argument,
    BasicBlock,
    Function,
  } kind;
  uint32_t type_id = 0;
  uint32_t id = 0;
  std::string name;
  std::string constant_data;
};

struct LLVMInstruction {
  enum Opcode {
    Ret = 1,
    Br = 2,
    Switch = 3,
    Invoke = 4,
    Unreachable = 8,
    Add = 9,
    Sub = 11,
    Mul = 13,
    UDiv = 15,
    SDiv = 17,
    URem = 19,
    SRem = 21,
    And = 23,
    Or = 24,
    Xor = 25,
    Shl = 26,
    LShr = 27,
    AShr = 28,
    FAdd = 29,
    FSub = 30,
    FMul = 31,
    FDiv = 32,
    FRem = 33,
    FNeg = 34,
    ExtractValue = 42,
    InsertValue = 43,
    ExtractElement = 44,
    InsertElement = 45,
    ShuffleVector = 46,
    BitCast = 53,
    ZExt = 55,
    SExt = 56,
    Trunc = 57,
    FPToUI = 58,
    FPToSI = 59,
    UIToFP = 60,
    SIToFP = 61,
    FPTrunc = 62,
    FPExt = 63,
    PtrToInt = 64,
    IntToPtr = 65,
    ICmp = 68,
    FCmp = 69,
    PHI = 71,
    Call = 72,
    Select = 73,
    GEP = 76,
    Load = 81,
    Store = 82,
    Alloca = 83,
    GetElementPtr = 84,
    AtomicRMW = 85,
    CmpXchg = 86,
  } opcode;

  uint32_t type_id = 0;
  uint32_t result_id = 0;
  std::vector<uint32_t> operands;
  // Modern GEP encodes its source element type independently of the result.
  uint32_t gep_source_type = UINT32_MAX;
};

struct LLVMBasicBlock {
  std::string name;
  std::vector<LLVMInstruction> instructions;
};

struct LLVMFunction {
  std::string name;
  uint32_t value_id = 0;
  uint32_t type_id = 0;
  uint32_t calling_conv = 0;
  bool is_declaration = true;
  uint32_t param_count = 0;
  uint32_t instruction_start_value = 0;
  std::vector<LLVMType> param_types;
  LLVMType return_type;
  std::vector<LLVMBasicBlock> blocks;
  std::vector<uint32_t> block_value_ids;
  std::vector<std::string> attributes;
  std::vector<LLVMValue> constants;
};

struct LLVMGlobal {
  uint32_t value_id = 0;
  uint32_t type_id = 0;
  uint32_t address_space = 0;
  std::string name;
  bool is_constant = false;
};

struct DxilResourceBinding {
  // DXIL resource classes match the class argument to createHandle:
  // SRV=0, UAV=1, CBV=2, Sampler=3.
  uint32_t resource_class = 0;
  uint32_t resource_id = 0;
  uint32_t register_space = 0;
  uint32_t lower_bound = 0;
  uint32_t count = 1;
  uint32_t resource_kind = 0;
  // DXIL ExtPropTags::ElementType.  Zero means the metadata did not carry a
  // typed element declaration (raw buffers and older modules use that form).
  uint32_t element_type = 0;
  uint32_t element_stride = 0;
  uint32_t sample_count = 1;
  bool globally_coherent = false;
  bool has_counter = false;
  bool rasterizer_ordered = false;
};

// Preserve the metadata graph, not just a few recovered resource properties.
// Node operands are nullable, one-based record references; named metadata uses
// zero-based record IDs. Value records refer to the module's LLVM values.
// Keeping these namespaces distinct is necessary for per-entrypoint node IO
// types, routing, launch parameters and forward metadata references.
struct LLVMMetadataRecord {
  enum class Kind { String, Value, Node } kind = Kind::Node;
  std::string string_value;
  uint32_t type_id = 0;
  uint64_t value_id = 0;
  std::vector<uint32_t> operands;
};

struct LLVMModule {
  std::vector<LLVMType> types;
  std::vector<LLVMValue> constants;
  std::vector<LLVMFunction> functions;
  std::vector<LLVMGlobal> globals;
  std::vector<DxilResourceBinding> resource_bindings;
  std::vector<LLVMMetadataRecord> metadata_records;
  std::unordered_map<std::string, std::vector<uint32_t>> named_metadata;
  std::unordered_map<std::string, size_t> function_map;
  std::string source_filename;
  std::string target_triple;
  uint32_t num_threads[3] = {1, 1, 1};

  const LLVMMetadataRecord *metadataOperand(
      const LLVMMetadataRecord &record, size_t index) const {
    if (record.kind != LLVMMetadataRecord::Kind::Node ||
        index >= record.operands.size())
      return nullptr;
    const uint32_t ref = record.operands[index];
    return ref && ref <= metadata_records.size()
               ? &metadata_records[ref - 1]
               : nullptr;
  }
};

class BitcodeReader {
public:
  static std::optional<LLVMModule> parse(const uint8_t *data, uint32_t size);

private:
  BitcodeReader() = default;
};

}
