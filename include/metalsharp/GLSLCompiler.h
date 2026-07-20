#pragma once
#include <cstdint>
#include <metalsharp/ShaderStage.h>
#include <string>
#include <vector>

namespace metalsharp {

struct GLSLVersion;

/// @note GL 4.x tessellation, geometry, and compute shaders are supported
/// via SPIRV-Cross translation. Tessellation control/evaluation shaders map
/// to Metal post-tessellation vertex shaders. Geometry shaders with limited
/// output topology map to Metal mesh shaders or are emulated via vertex
/// amplification. Compute shaders map directly to Metal kernel functions.
/// Transform feedback is not yet supported.
class GLSLCompiler {
  public:
    static bool initialize();
    static void shutdown();
    static bool isAvailable();

    static bool compileToSPIRV(const char* source, ShaderStage stage, const GLSLVersion& version,
                               std::vector<uint32_t>& spirvOut, std::string& errorLog);

    /// Translate SPIR-V binary to Metal Shading Language (MSL) source via
    /// the SPIRV-Cross MSL backend. The compiler instance is created
    /// transiently for each call; the function does not depend on glslang
    /// initialization state and is safe to call from any thread.
    ///
    /// The MSL output uses stage-stable entry-point names ("vertex_main",
    /// "fragment_main", "kernel_main") so downstream Metal pipeline code can
    /// look them up by name without having to parse the SPIR-V first.
    ///
    /// Only Vertex, Pixel, and Compute stages are supported. Other stages
    /// (geometry, tessellation, mesh, ray-tracing) are rejected up-front
    /// because the OpenGL bridge never feeds them to this path.
    ///
    /// @param spirv      Vector of 32-bit SPIR-V words (from compileToSPIRV)
    /// @param stage      Shader stage (controls the MSL entry-point name)
    /// @param mslOut     Output MSL source string; cleared on entry
    /// @param errorLog   Translation error/warning messages
    /// @return true on success; false if translation failed or stage is unsupported
    static bool translateSPIRVtoMSL(const std::vector<uint32_t>& spirv, ShaderStage stage, std::string& mslOut,
                                    std::string& errorLog);

  private:
    static bool s_initialized;
};

} // namespace metalsharp