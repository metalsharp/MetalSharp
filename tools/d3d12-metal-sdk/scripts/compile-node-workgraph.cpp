#include "vendor/dxmt/src/airconv/dxil/dxil_container.hpp"
#include "vendor/dxmt/src/airconv/dxil/llvm_bitcode.hpp"
#include "vendor/dxmt/src/airconv/dxil/msl_lowering.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dxmt::dxil;

static bool read_bytes(const char *path, std::vector<uint8_t> &bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    bytes.assign(std::istreambuf_iterator<char>(file), {});
    return !bytes.empty();
}

static bool extract_dxil(const std::vector<uint8_t> &input,
                         std::vector<uint8_t> &dxil) {
    if (input.size() >= 8 &&
        *reinterpret_cast<const uint32_t *>(input.data()) == DXIL_FOURCC) {
        dxil = input;
        return true;
    }
    if (input.size() < 32 ||
        *reinterpret_cast<const uint32_t *>(input.data()) != DXBC_FOURCC)
        return false;
    const uint32_t container_size = std::min<uint32_t>(
        *reinterpret_cast<const uint32_t *>(input.data() + 24),
        static_cast<uint32_t>(input.size()));
    const uint32_t part_count =
        *reinterpret_cast<const uint32_t *>(input.data() + 28);
    if (part_count > 128 || 32u + part_count * 4u > container_size)
        return false;
    for (uint32_t i = 0; i < part_count; ++i) {
        const uint32_t offset =
            *reinterpret_cast<const uint32_t *>(input.data() + 32u + i * 4u);
        if (offset > container_size || offset + 8u > container_size)
            continue;
        const uint32_t part_size =
            *reinterpret_cast<const uint32_t *>(input.data() + offset + 4u);
        if (part_size > container_size - offset - 8u ||
            *reinterpret_cast<const uint32_t *>(input.data() + offset) !=
                DXIL_FOURCC)
            continue;
        dxil.assign(input.begin() + offset + 8,
                    input.begin() + offset + 8 + part_size);
        return true;
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: compile-node-workgraph <dxbc-or-dxil> "
                     "<node-entry> <output-metal>\n");
        return 2;
    }

    std::vector<uint8_t> input;
    std::vector<uint8_t> dxil;
    if (!read_bytes(argv[1], input) || !extract_dxil(input, dxil)) {
        std::fprintf(stderr, "failed to read or extract DXIL: %s\n", argv[1]);
        return 3;
    }
    auto container = DXILContainer::parse(dxil.data(), dxil.size());
    if (!container) {
        std::fprintf(stderr, "DXIL container parse failed\n");
        return 4;
    }
    auto shader = container->shader();
    shader.kind = DxilShaderKind::Node;
    shader.entry_point = argv[2];
    auto module = BitcodeReader::parse(shader.bitcode.data, shader.bitcode.size);
    if (!module) {
        std::fprintf(stderr, "DXIL bitcode parse failed\n");
        return 5;
    }

    MSLLoweringOptions options = {};
    options.entry_point = shader.entry_point;
    auto lowered = MSLLowering::lower(*module, shader, options);
    if (!lowered) {
        std::fprintf(stderr, "node lowering failed\n");
        return 6;
    }
    if (lowered->unsupported_intrinsics || lowered->unsupported_opcodes) {
        std::fprintf(stderr, "node lowering rejected unsupported semantics: "
                             "intrinsics=%u opcodes=%u\n",
                     lowered->unsupported_intrinsics,
                     lowered->unsupported_opcodes);
        for (const auto &diagnostic : lowered->diagnostics)
            std::fprintf(stderr, "  %s\n", diagnostic.c_str());
        return 7;
    }

    std::ofstream output(argv[3]);
    if (!output) {
        std::fprintf(stderr, "failed to open output: %s\n", argv[3]);
        return 8;
    }
    output << lowered->source;
    return output ? 0 : 9;
}
