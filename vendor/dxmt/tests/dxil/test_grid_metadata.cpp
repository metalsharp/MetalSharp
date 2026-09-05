// Raw node_chain bitcode plus expected byte offset, component width and count.
#include "llvm_bitcode.hpp"
#include "node_metadata.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace dxmt::dxil;
int main(int argc, char **argv) {
  try {
    if (argc != 5) throw std::runtime_error("usage: test-grid-metadata bitcode offset width count");
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    auto module = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
    if (!module) throw std::runtime_error("bitcode parse failed");
    const auto metadata = nodeShaderMetadata(*module, "firstNode");
    if (!metadata || !metadata->grid_from_record ||
        metadata->grid_byte_offset != std::stoul(argv[2]) ||
        metadata->grid_component_bytes != std::stoul(argv[3]) ||
        metadata->grid_components != std::stoul(argv[4]))
      throw std::runtime_error("grid semantic offset/width/count mismatch");
    const auto copied = *module;
    const auto owned = nodeShaderMetadata(copied, "firstNode");
    if (!owned || owned->grid_byte_offset != metadata->grid_byte_offset ||
        owned->grid_component_bytes != metadata->grid_component_bytes ||
        owned->grid_components != metadata->grid_components)
      throw std::runtime_error("grid semantic ownership lost");
    std::cout << "PASS: grid offset=" << metadata->grid_byte_offset
              << " width=" << metadata->grid_component_bytes
              << " components=" << metadata->grid_components << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
