// Pass raw bitcode compiled from probes/probe_workgraph/node_chain.hlsl.
#include "llvm_bitcode.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
using namespace dxmt::dxil;
int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("expected raw bitcode path");
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    auto module = BitcodeReader::parse(bytes.data(), static_cast<uint32_t>(bytes.size()));
    if (!module) throw std::runtime_error("bitcode parse failed");
    std::set<uint32_t> indices;
    auto check = [&](const std::vector<LLVMValue> &constants) {
      for (const auto &c : constants) {
        if (!c.is_gep) continue;
        if (!c.constant_data.empty() || c.gep_operands.size() != 3 ||
            c.gep_source_type >= module->types.size() ||
            module->types[c.gep_source_type].kind != LLVMType::Array)
          throw std::runtime_error("constant GEP lost typed operands");
        bool group_base = false;
        for (const auto &g : module->globals)
          group_base |= g.value_id == c.gep_operands[0] && g.address_space == 3;
        if (!group_base) throw std::runtime_error("constant GEP lost global base");
        auto literal = [&](uint32_t id) -> unsigned {
          for (const auto &v : constants)
            if (v.id == id) return std::stoul(v.constant_data);
          for (const auto &v : module->constants)
            if (v.id == id) return std::stoul(v.constant_data);
          throw std::runtime_error("constant GEP index unresolved");
        };
        if (literal(c.gep_operands[1]) != 0)
          throw std::runtime_error("unexpected outer GEP index");
        indices.insert(literal(c.gep_operands[2]));
      }
    };
    check(module->constants);
    for (const auto &f : module->functions) check(f.constants);
    if (indices != std::set<uint32_t>{0, 1, 2, 3})
      throw std::runtime_error("four distinct groupshared addresses not retained");
    std::cout << "PASS: typed constant GEPs retain groupshared offsets 0,4,8,12\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
