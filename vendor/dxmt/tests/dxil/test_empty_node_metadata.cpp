// Pass raw node_chain bitcode built with EMPTY_ENTRY or EMPTY_MULTI, then expected MaxRecords.
#include "llvm_bitcode.hpp"
#include "node_metadata.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace dxmt::dxil;
static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 3, "usage: test-empty-node-metadata bitcode max-records");
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    auto parsed = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
    require(bool(parsed), "parse failed");
    LLVMModule module = *parsed;
    parsed.reset(); bytes.clear();
    const auto metadata = nodeShaderMetadata(module, "firstNode");
    const uint32_t expected = std::stoul(argv[2]);
    require(metadata && metadata->input.empty_input && !metadata->input.size &&
            !metadata->input.alignment && metadata->launch_type == 2 &&
            metadata->max_input_records == expected, "empty input batch metadata lost");
    bool checked_zero = false;
    for (auto id : module.named_metadata.at("dx.entryPoints")) {
      const auto &entry = module.metadata_records[id];
      const auto *name = module.metadataOperand(entry, 1);
      if (!name || name->string_value != "firstNode") continue;
      const auto *properties = module.metadataOperand(entry, 4);
      const LLVMMetadataRecord *inputs = nullptr, *maximum = nullptr;
      require(properties && node_metadata_detail::tag(module, *properties, 20, inputs) && inputs,
              "inputs missing");
      const auto *input = module.metadataOperand(*inputs, 0);
      require(input && node_metadata_detail::tag(module, *input, 3, maximum), "max-record tag invalid");
      if (!maximum) continue;
      for (const auto &constant : module.constants) {
        if (constant.constant_data != "0") continue;
        auto invalid = module;
        invalid.metadata_records[maximum - module.metadata_records.data()].value_id = constant.id;
        require(!nodeInputLayout(invalid, "firstNode"), "explicit zero MaxRecords accepted");
        checked_zero = true;
        break;
      }
    }
    require(expected == 1 || checked_zero, "zero-count rejection not exercised");
    std::cout << "PASS: owned empty input MaxRecords=" << expected << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
