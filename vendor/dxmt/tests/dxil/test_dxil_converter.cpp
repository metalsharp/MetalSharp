#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <functional>

#include "airconv/dxil/dxil_container.hpp"
#include "airconv/dxil/llvm_bitcode.hpp"
#include "airconv/dxil/dxil_to_msl.hpp"

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

struct TestCase {
    std::string name;
    std::string path;
    dxmt::dxil::DxilShaderKind expected_kind;
    bool expect_msl_nonempty;
};

static void report_pass(const std::string &name) {
    fprintf(stdout, "  [PASS] %s\n", name.c_str());
    g_pass++;
}

static void report_fail(const std::string &name, const char *reason) {
    fprintf(stdout, "  [FAIL] %s: %s\n", name.c_str(), reason);
    g_fail++;
}

static void report_skip(const std::string &name, const char *reason) {
    fprintf(stdout, "  [SKIP] %s: %s\n", name.c_str(), reason);
    g_skip++;
}

static std::optional<std::vector<uint8_t>> readBinaryFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return std::nullopt;
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f)
        return std::nullopt;
    return data;
}

static std::optional<std::vector<uint8_t>> extractDXILFromDXBC(const uint8_t *data, size_t size) {
    if (size < 8)
        return std::nullopt;

    uint32_t magic = *reinterpret_cast<const uint32_t*>(data);
    if (magic == dxmt::dxil::DXIL_FOURCC)
        return std::vector<uint8_t>(data, data + size);

    if (magic != dxmt::dxil::DXBC_FOURCC)
        return std::nullopt;

    if (size < 32)
        return std::nullopt;

    uint32_t container_size = *reinterpret_cast<const uint32_t*>(data + 24);
    uint32_t num_chunks = *reinterpret_cast<const uint32_t*>(data + 28);

    if (container_size > size)
        container_size = (uint32_t)size;

    for (uint32_t i = 0; i < num_chunks; i++) {
        uint32_t ptr_offset = 32 + i * 4;
        if (ptr_offset + 4 > container_size)
            break;
        uint32_t chunk_offset = *reinterpret_cast<const uint32_t*>(data + ptr_offset);
        if (chunk_offset + 8 > container_size)
            break;

        uint32_t fourcc = *reinterpret_cast<const uint32_t*>(data + chunk_offset);
        uint32_t chunk_size = *reinterpret_cast<const uint32_t*>(data + chunk_offset + 4);

        if (fourcc == dxmt::dxil::DXIL_FOURCC) {
            if (chunk_offset + 8 + chunk_size <= size) {
                return std::vector<uint8_t>(data + chunk_offset + 8,
                                            data + chunk_offset + 8 + chunk_size);
            }
        }
    }

    return std::nullopt;
}

static std::string g_msl_dump_dir;
static int g_msl_compile_ok = 0;
static int g_msl_compile_fail = 0;
static int g_msl_compile_skip = 0;

static bool runConverterTest(const TestCase &tc) {
    auto raw = readBinaryFile(tc.path);
    if (!raw) {
        report_fail(tc.name, "cannot read file");
        return false;
    }

    auto blob = extractDXILFromDXBC(raw->data(), raw->size());
    if (!blob) {
        report_skip(tc.name, "no DXIL chunk in DXBC container");
        return true;
    }

    if (blob->size() < 20) {
        report_fail(tc.name, "DXIL blob too small");
        return false;
    }

    auto container = dxmt::dxil::DXILContainer::parse(blob->data(), blob->size());
    if (!container) {
        report_fail(tc.name, "DXIL container parse failed");
        return false;
    }

    const auto &shader = container->shader();
    if (shader.bitcode.size == 0) {
        report_fail(tc.name, "empty bitcode");
        return false;
    }

    if (shader.kind != tc.expected_kind) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected kind %u, got %u",
                 (uint32_t)tc.expected_kind, (uint32_t)shader.kind);
        report_fail(tc.name, buf);
        return false;
    }

    auto module = dxmt::dxil::BitcodeReader::parse(shader.bitcode.data, shader.bitcode.size);
    if (!module) {
        report_fail(tc.name, "bitcode parse failed");
        return false;
    }

    if (module->functions.empty()) {
        report_fail(tc.name, "no functions parsed");
        return false;
    }

    auto msl = dxmt::dxil::DXILToMSL::convert(*module, shader);
    if (!msl) {
        report_fail(tc.name, "MSL conversion failed");
        return false;
    }

    if (tc.expect_msl_nonempty && msl->source.empty()) {
        report_fail(tc.name, "MSL source is empty");
        return false;
    }

    bool has_entry = msl->source.find(msl->entry_point) != std::string::npos;
    if (!has_entry) {
        char buf[256];
        snprintf(buf, sizeof(buf), "entry point '%s' not in MSL source",
                 msl->entry_point.c_str());
        report_fail(tc.name, buf);
        return false;
    }

    if (shader.kind == dxmt::dxil::DxilShaderKind::Compute) {
        if (msl->tg_size[0] == 0 || msl->tg_size[1] == 0 || msl->tg_size[2] == 0) {
            report_fail(tc.name, "compute shader has zero threadgroup size");
            return false;
        }
    }

    if (msl->unsupported_opcodes > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u unsupported opcodes", msl->unsupported_opcodes);
    }

    if (!g_msl_dump_dir.empty()) {
        std::string stem = tc.path;
        auto slash = stem.rfind('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        auto dot = stem.rfind('.');
        if (dot != std::string::npos) stem = stem.substr(0, dot);
        std::string msl_path = g_msl_dump_dir + "/" + stem + ".metal";
        std::ofstream msl_file(msl_path);
        if (msl_file.is_open()) {
            msl_file << msl->source;
            msl_file.close();
        }
    }

    report_pass(tc.name);
    return true;
}

static std::string shaderKindName(dxmt::dxil::DxilShaderKind kind) {
    switch (kind) {
    case dxmt::dxil::DxilShaderKind::Compute: return "compute";
    case dxmt::dxil::DxilShaderKind::Vertex: return "vertex";
    case dxmt::dxil::DxilShaderKind::Pixel: return "pixel";
    case dxmt::dxil::DxilShaderKind::Geometry: return "geometry";
    case dxmt::dxil::DxilShaderKind::Hull: return "hull";
    case dxmt::dxil::DxilShaderKind::Domain: return "domain";
    default: return "unknown";
    }
}

static dxmt::dxil::DxilShaderKind classifyModuleFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open())
        return dxmt::dxil::DxilShaderKind::Invalid;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("kind=compute") != std::string::npos)
            return dxmt::dxil::DxilShaderKind::Compute;
        if (line.find("kind=vertex") != std::string::npos)
            return dxmt::dxil::DxilShaderKind::Vertex;
        if (line.find("kind=pixel") != std::string::npos)
            return dxmt::dxil::DxilShaderKind::Pixel;
        if (line.find("kind=geometry") != std::string::npos)
            return dxmt::dxil::DxilShaderKind::Geometry;
    }
    return dxmt::dxil::DxilShaderKind::Invalid;
}

int main(int argc, char **argv) {
    fprintf(stdout, "=== DXIL Converter Test Suite ===\n\n");

    std::string cache_dir;
    bool compile_metal = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dump-msl" && i + 1 < argc) {
            g_msl_dump_dir = argv[++i];
        } else if (arg == "--compile-metal") {
            compile_metal = true;
        } else if (cache_dir.empty()) {
            cache_dir = arg;
        }
    }
    if (cache_dir.empty()) {
        const char *home = getenv("HOME");
        if (home) {
            cache_dir = std::string(home) +
                "/.metalsharp/runtime/shader-cache-test";
        }
    }

    std::string built_in_dir;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--built-in" && i + 1 < argc) {
            built_in_dir = argv[++i];
        }
    }

    std::vector<TestCase> tests;

    if (!built_in_dir.empty()) {
        for (const auto &entry : fs::directory_iterator(built_in_dir)) {
            if (entry.path().extension() == ".dxbc") {
                auto stem = entry.path().stem().string();
                auto module_path = built_in_dir + "/" + stem + ".module.txt";
                auto kind = classifyModuleFile(module_path);
                if (kind != dxmt::dxil::DxilShaderKind::Invalid) {
                    tests.push_back({
                        stem + " (" + shaderKindName(kind) + ")",
                        entry.path().string(),
                        kind,
                        true
                    });
                }
            }
        }
    }

    if (!cache_dir.empty() && fs::exists(cache_dir)) {
        for (const auto &entry : fs::directory_iterator(cache_dir)) {
            if (entry.path().extension() == ".dxbc") {
                auto stem = entry.path().stem().string();
                auto module_path = cache_dir + "/" + stem + ".module.txt";
                auto kind = classifyModuleFile(module_path);
                if (kind != dxmt::dxil::DxilShaderKind::Invalid) {
                    tests.push_back({
                        stem + " (" + shaderKindName(kind) + ")",
                        entry.path().string(),
                        kind,
                        true
                    });
                }
            }
        }
    }

    if (tests.empty()) {
        fprintf(stdout, "No test shaders found.\n");
        fprintf(stdout, "Usage: %s <shader_cache_dir> [additional_dir]\n", argv[0]);
        fprintf(stdout, "\nRun against Subnautica 2 shader cache:\n");
        fprintf(stdout, "  %s /path/to/.metalsharp-cache/shader-cache/vkd3d/1962700\n", argv[0]);
        return 1;
    }

    fprintf(stdout, "Running %zu tests...\n\n", tests.size());

    for (const auto &tc : tests) {
        runConverterTest(tc);
    }

    fprintf(stdout, "\n=== Results: %d pass, %d fail, %d skip (total %zu) ===\n",
            g_pass, g_fail, g_skip, tests.size());

    if (compile_metal && !g_msl_dump_dir.empty()) {
        fprintf(stdout, "\n=== Metal Compilation Test ===\n");
        fs::create_directories(g_msl_dump_dir + "/errors");
        for (const auto &entry : fs::directory_iterator(g_msl_dump_dir)) {
            if (entry.path().extension() != ".metal") continue;
            auto stem = entry.path().stem().string();
            std::string cmd = "xcrun -sdk macosx metal -std=metal3.0 -Wno-unused-variable "
                "-Wno-unused-function -Wno-implicit-function-declaration "
                "-Wno-incompatible-pointer-types -Wno-int-conversion "
                "-c " + entry.path().string() + " -o /dev/null 2>" +
                g_msl_dump_dir + "/errors/" + stem + ".err";
            int ret = system(cmd.c_str());
            if (ret == 0) {
                g_msl_compile_ok++;
            } else {
                g_msl_compile_fail++;
                fprintf(stdout, "  [METAL FAIL] %s\n", stem.c_str());
            }
        }
        fprintf(stdout, "\n=== Metal: %d ok, %d fail ===\n", g_msl_compile_ok, g_msl_compile_fail);
    }

    return g_fail > 0 ? 1 : 0;
}
