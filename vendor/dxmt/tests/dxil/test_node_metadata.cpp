// Standalone host test: pass raw DXIL bitcode compiled from
// tools/d3d12-metal-sdk/probes/probe_workgraph/node_input_records.hlsl.
#include "llvm_bitcode.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace dxmt::dxil;
using Record = LLVMMetadataRecord;

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
static const Record *operand(const LLVMModule &m, const Record &r, size_t i) {
  return m.metadataOperand(r, i);
}
static uint32_t integer(const LLVMModule &m, const Record *r) {
  require(r && r->kind == Record::Kind::Value, "missing integer metadata");
  for (const auto &c : m.constants)
    if (c.id == r->value_id) return static_cast<uint32_t>(std::stoll(c.constant_data));
  throw std::runtime_error("unresolved metadata value");
}
static const Record *tag(const LLVMModule &m, const Record &r, uint32_t key) {
  require(r.kind == Record::Kind::Node && r.operands.size() % 2 == 0,
          "malformed metadata tag tuple");
  for (size_t i = 0; i < r.operands.size(); i += 2)
    if (integer(m, operand(m, r, i)) == key) return operand(m, r, i + 1);
  return nullptr;
}
static void check(const LLVMModule &m, const char *name, uint32_t size,
                  uint32_t threads, uint32_t grid) {
  const auto named = m.named_metadata.find("dx.entryPoints");
  require(named != m.named_metadata.end(), "entrypoint metadata lost");
  const Record *properties = nullptr;
  for (uint32_t id : named->second) {
    require(id < m.metadata_records.size(), "invalid named metadata ID");
    const auto &entry = m.metadata_records[id];
    const auto *entry_name = operand(m, entry, 1);
    if (entry_name && entry_name->kind == Record::Kind::String &&
        entry_name->string_value == name) properties = operand(m, entry, 4);
  }
  require(properties, "selected entrypoint missing");
  require(integer(m, tag(m, *properties, 13)) == 1, "launch type lost");
  const auto *group = tag(m, *properties, 4);
  const auto *dispatch = tag(m, *properties, 18);
  require(group && dispatch, "launch dimensions missing");
  for (size_t i = 0; i < 3; ++i) {
    require(integer(m, operand(m, *group, i)) == (i ? 1 : threads), "wrong numthreads");
    require(integer(m, operand(m, *dispatch, i)) == (i ? 1 : grid), "wrong dispatch grid");
  }
  const auto *inputs = tag(m, *properties, 20);
  require(inputs, "node inputs lost");
  const auto *input = operand(m, *inputs, 0);
  require(input, "input descriptor lost");
  const auto *type = tag(m, *input, 2);
  require(type, "record type lost");
  require(integer(m, tag(m, *type, 0)) == size, "wrong record size");
  require(integer(m, tag(m, *type, 2)) == 4, "wrong record alignment");
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "usage: test_node_metadata fixture.bc");
    std::ifstream in(argv[1], std::ios::binary);
    require(bool(in), "fixture unavailable");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    auto parsed = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
    require(bool(parsed), "fixture parse failed");
    // Check ownership after the original module and byte buffer are gone.
    LLVMModule module = *parsed;
    parsed.reset();
    bytes.clear();
    check(module, "node_main", 4, 1, 1);
    check(module, "node_vector", 16, 4, 2);
    require(!operand(module, Record{}, 0), "missing operand not rejected");
    Record invalid;
    invalid.operands = {0, static_cast<uint32_t>(module.metadata_records.size() + 1)};
    require(!operand(module, invalid, 0) && !operand(module, invalid, 1),
            "null/out-of-range metadata reference accepted");
    std::cout << "PASS: owned per-entrypoint node size/alignment and launch metadata\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
