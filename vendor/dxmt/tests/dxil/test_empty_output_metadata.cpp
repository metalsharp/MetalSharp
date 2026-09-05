#include "llvm_bitcode.hpp"
#include "node_metadata.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
using namespace dxmt::dxil;
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  auto parsed = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
  if (!parsed) return 1;
  auto module = *parsed;
  parsed.reset(); bytes.clear();
  for (const char *entry : {"threadProducer", "groupProducer"}) {
    auto metadata = nodeShaderMetadata(module, entry);
    if (!metadata || metadata->outputs.size() != 1) return 1;
    const auto &output = metadata->outputs[0];
    if (!output.empty_output || output.size || output.alignment ||
        output.max_records != 8 || output.node_name != "sink") return 1;
  }
  auto consumer = nodeShaderMetadata(module, "consume");
  if (!consumer || !consumer->input.empty_input || consumer->input.size ||
      consumer->max_input_records != 4) return 1;
  auto invalid = module;
  bool changed = false;
  for (auto &constant : invalid.constants) {
    if (constant.constant_data == "10") {
      constant.constant_data = "9"; // Empty input flags are not an output declaration.
      changed = true;
    }
  }
  if (!changed || nodeShaderMetadata(invalid, "threadProducer")) return 1;
  std::cout << "PASS: empty outputs retain zero-byte layout; input flags reject as outputs\n";
}
