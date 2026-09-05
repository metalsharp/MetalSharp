#include "llvm_bitcode.hpp"
#include "node_metadata.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  std::ifstream file(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  auto parsed = dxmt::dxil::BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
  if (!parsed) return 1;
  auto module = *parsed;
  parsed.reset(); bytes.clear();
  auto recursive = dxmt::dxil::nodeShaderMetadata(module, "recursive");
  auto nonrecursive = dxmt::dxil::nodeShaderMetadata(module, "nonrecursive");
  if (!recursive || !nonrecursive || recursive->max_recursion_depth != 3 ||
      nonrecursive->max_recursion_depth != 3 || recursive->outputs.size() != 1 ||
      recursive->outputs[0].node_name != recursive->node_name || !nonrecursive->outputs.empty())
    return 1;
  std::cout << "PASS: owned recursion declaration retained independently of self-edge presence\n";
}
