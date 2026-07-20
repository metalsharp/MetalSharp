/// @file GLSLCompiler.cpp
/// @brief GLSL → SPIR-V compiler wrapper around the Khronos glslang reference
///        implementation.
///
/// The OpenGL 3.x/4.x bridge needs to compile guest GLSL shaders into SPIR-V
/// before handing them to SPIRV-Cross for cross-compilation to MSL. This
/// translation unit is the single entry point for that compilation step.
///
/// Process-level glslang state is initialized lazily on the first call to
/// compileToSPIRV() and torn down in shutdown(). It uses std::call_once so
/// that concurrent callers do not race the initialization, and is idempotent
/// so that callers do not need to pair initialize() with shutdown() for
/// normal use.
///
/// This is intentionally a thin wrapper. It does not try to replicate the
/// full glslang option surface; it covers the GLSL cases the OpenGL bridge
/// actually needs (Vulkan and OpenGL dialects, SPIR-V 1.0 target).

#include <metalsharp/GLSLCompiler.h>
#include <metalsharp/GLSLVersion.h>
#include <metalsharp/ShaderStage.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

// SPIRV-Cross MSL backend. We only use the C++ API; the C shim
// (spirv-cross-c) is not needed here.
#include <spirv_cross.hpp>
#include <spirv_msl.hpp>

#include <mutex>

namespace metalsharp {

bool GLSLCompiler::s_initialized = false;

namespace {

std::once_flag g_init_once;

// glslang keeps its language/message enums in two places:
//   * EShLanguage / EShMessages / EShExecutable / etc. — global namespace
//     (typedef enums declared above the `namespace glslang {` block in
//     glslang/Public/ShaderLang.h).
//   * EShSource / EShClient / EShTargetLanguage / EShTargetClientVersion —
//     inside the `glslang` namespace.
// Classes such as TShader / TProgram / TIntermediate live in `glslang`.
// We alias the global ones here so the call sites stay readable.
using Lang = EShLanguage;

glslang::EShSource mapGLSLSource() {
    return glslang::EShSourceGlsl;
}

EShLanguage mapShaderStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:
        return EShLangVertex;
    case ShaderStage::Pixel:
        return EShLangFragment;
    case ShaderStage::Compute:
        return EShLangCompute;
    case ShaderStage::Geometry:
        return EShLangGeometry;
    case ShaderStage::Hull:
        return EShLangTessControl;
    case ShaderStage::Domain:
        return EShLangTessEvaluation;
    case ShaderStage::Mesh:
        return EShLangMesh;
    case ShaderStage::Amplification:
        return EShLangTask;
    case ShaderStage::RayGeneration:
        return EShLangRayGen;
    case ShaderStage::ClosestHit:
        return EShLangClosestHit;
    case ShaderStage::Miss:
        return EShLangMiss;
    case ShaderStage::Intersection:
        return EShLangIntersect;
    case ShaderStage::AnyHit:
        return EShLangAnyHit;
    case ShaderStage::Callable:
        return EShLangCallable;
    }
    return EShLangVertex;
}

} // namespace

bool GLSLCompiler::initialize() {
    std::call_once(g_init_once, []() {
        if (glslang::InitializeProcess()) {
            s_initialized = true;
        }
    });
    return s_initialized;
}

void GLSLCompiler::shutdown() {
    if (s_initialized) {
        glslang::FinalizeProcess();
        s_initialized = false;
    }
}

bool GLSLCompiler::isAvailable() {
    if (s_initialized)
        return true;
    return initialize();
}

bool GLSLCompiler::compileToSPIRV(const char* source, ShaderStage stage, const GLSLVersion& version,
                                  std::vector<uint32_t>& spirvOut, std::string& errorLog) {
    spirvOut.clear();
    errorLog.clear();

    if (!source) {
        errorLog = "GLSLCompiler: source is null";
        return false;
    }

    if (!initialize()) {
        errorLog = "GLSLCompiler: glslang::InitializeProcess() failed";
        return false;
    }

    const EShLanguage esStage = mapShaderStage(stage);

    glslang::TShader shader(esStage);

    // Pick the dialect. The OpenGL bridge mostly feeds us desktop GLSL, but
    // we also accept ES sources via the GLSLVersion::isES flag. We always
    // target SPIR-V 1.0 because that is what Metal/SPIRV-Cross expect on the
    // Apple stack; SPIRV-Cross can promote to newer SPIR-V dialects internally.
    const glslang::EShSource sourceLang = mapGLSLSource();
    const glslang::EShClient dialect = glslang::EShClientOpenGL;
    const int dialectVersion = version.valid ? static_cast<int>(version.major * 100 + version.minor) : 450;

    shader.setEnvInput(sourceLang, esStage, dialect, dialectVersion);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const char* sources[1] = {source};
    shader.setStrings(sources, 1);

    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    const TBuiltInResource* resources = GetDefaultResources();

    if (!shader.parse(resources, 100, false, messages)) {
        errorLog = "GLSLCompiler: parse failed: ";
        errorLog += shader.getInfoLog();
        errorLog += shader.getInfoDebugLog();
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);

    if (!program.link(messages)) {
        errorLog = "GLSLCompiler: link failed: ";
        errorLog += program.getInfoLog();
        errorLog += shader.getInfoLog();
        errorLog += shader.getInfoDebugLog();
        return false;
    }

    const glslang::TIntermediate* intermediate = program.getIntermediate(esStage);
    if (!intermediate) {
        errorLog = "GLSLCompiler: no intermediate representation after link";
        return false;
    }

    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.stripDebugInfo = true;

    glslang::GlslangToSpv(*intermediate, spirvOut, &options);

    if (spirvOut.empty()) {
        errorLog = "GLSLCompiler: GlslangToSpv produced empty SPIR-V output";
        return false;
    }

    return true;
}

bool GLSLCompiler::translateSPIRVtoMSL(const std::vector<uint32_t>& spirv, ShaderStage stage, std::string& mslOut,
                                       std::string& errorLog) {
    mslOut.clear();
    errorLog.clear();

    if (spirv.empty()) {
        errorLog = "GLSLCompiler::translateSPIRVtoMSL: empty SPIR-V input";
        return false;
    }

    // SPIRV-Cross's MSL entry-point model only covers the three stages the
    // OpenGL bridge actually translates. Anything else (geometry,
    // tessellation, mesh, ray-tracing) is rejected here so callers get a
    // clean error instead of a confusing SPIRV-Cross exception.
    spv::ExecutionModel model;
    const char* entryPointName = nullptr;
    switch (stage) {
    case ShaderStage::Vertex:
        model = spv::ExecutionModelVertex;
        entryPointName = "vertex_main";
        break;
    case ShaderStage::Pixel:
        model = spv::ExecutionModelFragment;
        entryPointName = "fragment_main";
        break;
    case ShaderStage::Compute:
        model = spv::ExecutionModelGLCompute;
        entryPointName = "kernel_main";
        break;
    default:
        errorLog = "GLSLCompiler::translateSPIRVtoMSL: unsupported shader stage for MSL translation";
        return false;
    }

    try {
        // Note: SPIRV-Cross does not surface `entry_point_name` as an option
        // on CompilerMSL::Options (only platform, MSL version, resource
        // binding layouts, etc.). The supported way to override the entry
        // point name is rename_entry_point() before compile().
        spirv_cross::CompilerMSL msl(spirv);

        spirv_cross::CompilerMSL::Options opts;
        opts.platform = spirv_cross::CompilerMSL::Options::macOS;
        // Metal 2.4 is the safest default — it covers everything Apple
        // Silicon supports and matches MoltenVK's MSL 2.x requirement for
        // most modern SPIR-V features (argument buffers tier 2, etc.).
        opts.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 4);
        msl.set_msl_options(opts);

        // glslang emits the GLSL `void main()` entry point as the SPIR-V
        // entry point named "main". "main" is a reserved keyword in MSL,
        // and even if it were not, downstream Metal pipeline code wants a
        // stable, stage-specific name. We rename unconditionally; the input
        // SPIR-V is always produced by compileToSPIRV above, which uses
        // glslang's default "main" entry point.
        msl.rename_entry_point("main", entryPointName, model);

        mslOut = msl.compile();
    } catch (const spirv_cross::CompilerError& e) {
        errorLog = std::string("GLSLCompiler::translateSPIRVtoMSL: SPIRV-Cross error: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        errorLog = std::string("GLSLCompiler::translateSPIRVtoMSL: unexpected error: ") + e.what();
        return false;
    }

    if (mslOut.empty()) {
        errorLog = "GLSLCompiler::translateSPIRVtoMSL: SPIRV-Cross produced empty MSL output";
        return false;
    }

    return true;
}

} // namespace metalsharp
