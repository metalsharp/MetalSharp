#include <metal_irconverter/metal_irconverter.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_bytes(const char *path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

static void print_error(const char *operation, const IRError *error) {
    std::fprintf(stderr, "%s failed: %u\n", operation,
                 error ? IRErrorGetCode(error) : 999u);
}

int main(int argc, char **argv) {
    const char *input_path = nullptr;
    const char *output_path = nullptr;
    const char *reflection_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--output-reflection-file" && i + 1 < argc) {
            reflection_path = argv[++i];
        } else if (arg.rfind("--output-reflection-file=", 0) == 0) {
            reflection_path = argv[i] + 25;
        } else if (arg.rfind("--", 0) != 0) {
            input_path = argv[i];
        }
    }
    if (!input_path || !output_path || !reflection_path)
        return 2;

    const auto bytecode = read_bytes(input_path);
    if (bytecode.empty())
        return 3;
    IRObject *input = IRObjectCreateFromDXIL(
        bytecode.data(), bytecode.size(), IRBytecodeOwnershipCopy);
    if (!input)
        return 4;

    const IRShaderStage stage = IRObjectGetMetalIRShaderStage(input);
    const char *entry = nullptr;
    if (stage == IRShaderStageAmplification)
        entry = "as_main";
    else if (stage == IRShaderStageMesh)
        entry = "ms_main";
    else {
        IRObjectDestroy(input);
        return 8;
    }

    IRCompiler *compiler = IRCompilerCreate();
    if (!compiler) {
        IRObjectDestroy(input);
        return 5;
    }
    IRCompilerSetMinimumDeploymentTarget(compiler, IROperatingSystem_macOS,
                                         "15.0.0");
    IRError *error = nullptr;
    IRObject *output =
        IRCompilerAllocCompileAndLink(compiler, entry, input, &error);
    if (!output) {
        print_error("IRCompilerAllocCompileAndLink", error);
        if (error)
            IRErrorDestroy(error);
        IRCompilerDestroy(compiler);
        IRObjectDestroy(input);
        return 6;
    }

    IRMetalLibBinary *library = IRMetalLibBinaryCreate();
    if (!library || !IRObjectGetMetalLibBinary(output, stage, library)) {
        print_error("IRObjectGetMetalLibBinary", error);
        if (library)
            IRMetalLibBinaryDestroy(library);
        IRObjectDestroy(output);
        IRCompilerDestroy(compiler);
        IRObjectDestroy(input);
        return 7;
    }
    std::vector<uint8_t> metal(IRMetalLibGetBytecodeSize(library));
    IRMetalLibGetBytecode(library, metal.data());
    std::ofstream output_file(output_path, std::ios::binary);
    output_file.write(reinterpret_cast<const char *>(metal.data()), metal.size());

    IRShaderReflection *reflection = IRShaderReflectionCreate();
    const bool reflection_ok =
        reflection && IRObjectGetReflection(output, stage, reflection);
    const char *reflection_json =
        reflection_ok ? IRShaderReflectionCopyJSONString(reflection) : nullptr;
    std::ofstream reflection_file(reflection_path);
    if (reflection_json)
        reflection_file << reflection_json;

    if (reflection_json)
        IRShaderReflectionReleaseString(reflection_json);
    if (reflection)
        IRShaderReflectionDestroy(reflection);
    IRMetalLibBinaryDestroy(library);
    IRObjectDestroy(output);
    IRCompilerDestroy(compiler);
    IRObjectDestroy(input);
    if (error)
        IRErrorDestroy(error);
    return output_file && reflection_file && reflection_ok ? 0 : 9;
}
