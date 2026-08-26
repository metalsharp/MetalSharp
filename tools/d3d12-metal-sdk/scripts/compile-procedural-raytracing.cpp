#include <metal_irconverter/metal_irconverter.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> read_bytes(const char *path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

static std::string read_text(const char *path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}

int main(int argc, char **argv) {
  if (argc != 5) {
    std::fprintf(stderr,
                 "usage: compile-procedural-raytracing dxil root-json "
                 "entry-or-synthesis-mode output\n");
    return 2;
  }

  auto bytes = read_bytes(argv[1]);
  auto root_json = read_text(argv[2]);
  if (bytes.empty() || root_json.empty())
    return 3;

  IRError *error = nullptr;
  auto *root_desc =
      IRVersionedRootSignatureDescriptorCreateFromJSON(root_json.c_str());
  auto *root = root_desc
                   ? IRRootSignatureCreateFromDescriptor(root_desc, &error)
                   : nullptr;
  if (!root) {
    std::fprintf(stderr, "root-signature error %u\n",
                 error ? IRErrorGetCode(error) : 999u);
    return 4;
  }

  auto *input = IRObjectCreateFromDXIL(bytes.data(), bytes.size(),
                                       IRBytecodeOwnershipCopy);
  auto *compiler = IRCompilerCreate();
  auto *config = IRRayTracingPipelineConfigurationCreate();
  IRRayTracingPipelineConfigurationSetMaxAttributeSizeInBytes(config, 8);
  IRRayTracingPipelineConfigurationSetMaxRecursiveDepth(config, 2);
  IRRayTracingPipelineConfigurationSetRayGenerationCompilationMode(
      config, IRRayGenerationCompilationVisibleFunction);
  IRRayTracingPipelineConfigurationSetIntersectionFunctionCompilationMode(
      config, IRIntersectionFunctionCompilationVisibleFunction);
  IRCompilerSetRayTracingPipelineConfiguration(compiler, config);
  const std::string mode = argv[3];
  const bool procedural = mode.rfind("procedural_", 0) == 0 ||
                          mode == "@procedural-wrapper";
  IRCompilerSetHitgroupType(
      compiler, procedural ? IRHitGroupTypeProceduralPrimitive
                           : IRHitGroupTypeTriangles);
  IRCompilerSetGlobalRootSignature(compiler, root);
  IRCompilerSetMinimumDeploymentTarget(compiler, IROperatingSystem_macOS,
                                       "15.0.0");

  IRObject *output = nullptr;
  auto *library = IRMetalLibBinaryCreate();
  if (mode == "@procedural-wrapper" || mode == "@triangle-wrapper") {
    if (!IRMetalLibSynthesizeIndirectIntersectionFunction(compiler, library)) {
      std::fprintf(stderr, "intersection wrapper synthesis failed\n");
      return 5;
    }
  } else if (mode == "@ray-dispatch") {
    if (!IRMetalLibSynthesizeIndirectRayDispatchFunction(compiler, library)) {
      std::fprintf(stderr, "ray-dispatch synthesis failed\n");
      return 5;
    }
  } else {
    output = IRCompilerAllocCompileAndLink(compiler, argv[3], input, &error);
    if (!output) {
      std::fprintf(stderr, "procedural shader compile error %u\n",
                   error ? IRErrorGetCode(error) : 999u);
      return 5;
    }
    IRShaderStage stage = IRShaderStageInvalid;
    if (mode == "raygen")
      stage = IRShaderStageRayGeneration;
    else if (mode == "miss_shader")
      stage = IRShaderStageMiss;
    else if (mode == "closest_hit" || mode == "procedural_closest_hit")
      stage = IRShaderStageClosestHit;
    else if (mode == "any_hit")
      stage = IRShaderStageAnyHit;
    else if (mode == "callable_shader")
      stage = IRShaderStageCallable;
    else if (mode == "procedural_intersection")
      stage = IRShaderStageIntersection;
    if (stage == IRShaderStageInvalid) {
      std::fprintf(stderr, "unsupported entry point %s\n", argv[3]);
      return 6;
    }
    if (!IRObjectGetMetalLibBinary(output, stage, library)) {
      std::fprintf(stderr, "procedural shader metallib is missing\n");
      return 6;
    }
  }

  std::vector<uint8_t> metal(IRMetalLibGetBytecodeSize(library));
  IRMetalLibGetBytecode(library, metal.data());
  std::ofstream file(argv[4], std::ios::binary);
  file.write(reinterpret_cast<const char *>(metal.data()), metal.size());

  IRMetalLibBinaryDestroy(library);
  if (output)
    IRObjectDestroy(output);
  IRCompilerDestroy(compiler);
  IRRayTracingPipelineConfigurationDestroy(config);
  IRObjectDestroy(input);
  IRRootSignatureDestroy(root);
  IRVersionedRootSignatureDescriptorRelease(root_desc);
  if (error)
    IRErrorDestroy(error);
  return file ? 0 : 7;
}
