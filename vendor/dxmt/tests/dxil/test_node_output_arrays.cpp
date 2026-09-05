// Raw DXIL from probes/probe_workgraph/node_output_arrays.hlsl.
#include "llvm_bitcode.hpp"
#include "node_metadata.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace dxmt::dxil;
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
  try {
    require(argc == 2, "expected bitcode path");
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    auto parsed = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
    require(bool(parsed), "parse failed");
    LLVMModule module = *parsed;
    parsed.reset(); bytes.clear();
    const auto finite = nodeShaderMetadata(module, "array_entry");
    const auto sparse = nodeShaderMetadata(module, "sparse_entry");
    require(finite && finite->outputs.size() == 1, "finite array missing");
    require(sparse && sparse->outputs.size() == 1, "sparse array missing");
    const auto &a = finite->outputs[0]; const auto &b = sparse->outputs[0];
    require(a.is_array && a.array_size == 4 && !a.allow_sparse && a.node_name == "array_target" && a.size == 8,
            "finite array layout lost");
    require(b.is_array && b.array_size == UINT32_MAX && b.allow_sparse && b.node_name == "sparse_target" && b.size == 8,
            "unbounded sparse declaration truncated");
    const auto target_metadata = nodeShaderMetadata(module, "target_sparse");
    require(target_metadata && target_metadata->node_name == "sparse_target" &&
            target_metadata->node_array_index == 65536, "declared NodeID lost");
    bool high_index = false;
    for (auto id : module.named_metadata.at("dx.entryPoints")) {
      const auto &entry = module.metadata_records[id];
      const auto *name = module.metadataOperand(entry, 1);
      if (!name || name->string_value != "target_sparse") continue;
      const auto *properties = module.metadataOperand(entry, 4);
      const LLVMMetadataRecord *node = nullptr;
      uint32_t index = 0;
      require(properties && node_metadata_detail::tag(module, *properties, 15, node) && node &&
              node_metadata_detail::integerBits(module, module.metadataOperand(*node, 1), index),
              "target index missing");
      high_index = index == 65536;
    }
    require(high_index, "full-width target index lost");
    std::cout << "PASS: finite and unbounded sparse output arrays retain full-width metadata\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
