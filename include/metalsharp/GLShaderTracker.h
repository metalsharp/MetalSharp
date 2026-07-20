/// @file GLShaderTracker.h
/// @brief Per-shader and per-program state tracker for the opengl32 shim.
///
/// Phase 2e hooks glCreateShader / glShaderSource / glCompileShader so the
/// shim can capture guest GLSL source, parse its #version directive, decide
/// whether it needs cross-compilation, and (when needed) drive
/// GLSLCompiler::compileToSPIRV + GLSLCompiler::translateSPIRVtoMSL in place
/// of the macOS native OpenGL compiler. The MSL output is parked in the
/// shader's tracker entry; downstream phases will turn that into Metal
/// pipeline state when glLinkProgram / glUseProgram are intercepted.
///
/// The tracker is intentionally lightweight:
///   * One process-wide singleton (GL is per-process, not per-thread).
///   * std::unordered_map for both shader and program object handles.
///   * Names are allocated from a monotonically increasing counter starting
///     at 1 so a zero return value can mean "create failed" / "no entry".
///   * No background threads; every public method is synchronous and
///     thread-safe via a single std::mutex.
///
/// Native macOS OpenGL 2.1 only supports GLSL 1.20, so any source that
/// reports #version 130 or higher (or any "es" flavour) is routed through
/// the SPIRV-Cross cross-compilation path. GLSL 1.10/1.20 sources still
/// fall through to the native compiler.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <metalsharp/GLSLVersion.h>
#include <metalsharp/ShaderStage.h>

namespace metalsharp {

/// Per-shader tracking state. One instance per live glCreateShader handle.
///
/// Lifecycle:
///   1. glCreateShader() -> createShader() produces a fresh GLShaderState
///      with stage derived from the GL enum (e.g. GL_VERTEX_SHADER).
///   2. glShaderSource() concatenates the source strings, parses #version,
///      and flips needsCrossCompile based on needsCrossCompile().
///   3. glCompileShader() drives GLSLCompiler::compileToSPIRV +
///      GLSLCompiler::translateSPIRVtoMSL when needsCrossCompile is true
///      and stores the SPIR-V / MSL outputs here.
///   4. glDeleteShader() removes the entry; downstream callers see getShader()
///      return nullptr for the recycled name.
struct GLShaderState {
    uint32_t type = 0;                       // GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, ...
    ShaderStage stage = ShaderStage::Vertex; // Internal stage enum
    std::string source;                      // Concatenated GLSL source (post glShaderSource)
    GLSLVersion glslVersion;                 // Parsed #version directive
    bool needsCrossCompile = false;
    std::vector<uint32_t> spirv; // Compiled SPIR-V binary (only when cross-compiled)
    std::string msl;             // Translated MSL source (only when cross-compiled)
    bool compiled = false;       // glCompileShader has been called at least once
    bool compileSuccess = false; // True iff cross-compile + SPIRV->MSL both succeeded
    std::string infoLog;         // Compile / translate error / warning messages
};

/// Thread-safe tracker for shader and program objects created through the
/// opengl32 shim. Process-wide singleton accessed via instance().
class GLShaderTracker {
  public:
    /// Returns the process-wide tracker.
    static GLShaderTracker& instance();

    // ----- Shader lifecycle ----------------------------------------------------

    /// Allocates a fresh shader handle and inserts a fresh GLShaderState.
    /// @param type  GL shader type enum (GL_VERTEX_SHADER / GL_FRAGMENT_SHADER /
    ///              GL_COMPUTE_SHADER). The internal ShaderStage is derived
    ///              from this value; unknown enums fall back to Vertex.
    /// @return Newly allocated handle (>0), or 0 on failure (out-of-memory or
    ///         unsupported shader type).
    uint32_t createShader(uint32_t type);

    /// Removes the shader entry if present. Silently no-ops for unknown names.
    /// Also detaches the shader from any program that references it so that
    /// dangling attachments don't survive deletion.
    void deleteShader(uint32_t name);

    /// Returns the tracked state for a shader, or nullptr if the handle is
    /// unknown. Callers should null-check before dereferencing.
    GLShaderState* getShader(uint32_t name);

    // ----- Program lifecycle ---------------------------------------------------

    /// Allocates a fresh program handle and inserts an empty attachment list.
    /// @return Newly allocated handle (>0), or 0 on failure.
    uint32_t createProgram();

    /// Removes the program entry if present. Attached shaders are not deleted;
    /// they remain tracked until glDeleteShader is called explicitly.
    void deleteProgram(uint32_t name);

    /// Records that `shader` is attached to `program`. No-op if either handle
    /// is unknown.
    void attachShader(uint32_t program, uint32_t shader);

    /// Removes `shader` from `program`'s attachment list. No-op if either
    /// handle is unknown.
    void detachShader(uint32_t program, uint32_t shader);

    /// Returns the list of shader handles attached to `program`. Empty vector
    /// if the program is unknown (instead of throwing).
    const std::vector<uint32_t>& getAttachedShaders(uint32_t program) const;

  private:
    GLShaderTracker() = default;

    // Disable copy/move — singleton owns its maps.
    GLShaderTracker(const GLShaderTracker&) = delete;
    GLShaderTracker& operator=(const GLShaderTracker&) = delete;

    mutable std::mutex m_mutex;
    std::unordered_map<uint32_t, GLShaderState> m_shaders;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_programs;
    uint32_t m_nextShaderName = 1;
    uint32_t m_nextProgramName = 1;
};

} // namespace metalsharp