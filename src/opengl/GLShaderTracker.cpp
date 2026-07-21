/// @file GLShaderTracker.cpp
/// @brief Implementation of the per-shader / per-program state tracker.
///
/// All public methods are thread-safe via a single std::mutex held on the
/// instance. We do not hold the mutex across the GLSLCompiler call sites —
/// the tracker only owns metadata, and shader / program compilation happens
/// in the caller (EntryPoint.cpp) after the caller has looked up the
/// relevant state.
///
/// Name allocation:
///   * Shader names start at 1 and increment per createShader() call.
///   * Program names start at 1 and increment per createProgram() call.
///   * A zero return value signals failure; GL spec reserves name 0 as the
///     "no object" sentinel, which is exactly what we want here.

#include <metalsharp/GLShaderTracker.h>

#include <algorithm>

namespace metalsharp {

namespace {

// OpenGL shader-type enums. Mirrors the values from gl.h / GL_VERSION_2_0
// and the GL_ARB_compute_shader extension. Defined here as constants so we
// don't need to drag the real gl.h into the bridge code (some hosts might
// not expose it transitively).
constexpr uint32_t kGL_VERTEX_SHADER = 0x8B31;
constexpr uint32_t kGL_FRAGMENT_SHADER = 0x8B30;
constexpr uint32_t kGL_COMPUTE_SHADER = 0x91B9;

/// Map a GL shader-type enum to the internal ShaderStage enum. Unknown
/// enums fall back to Vertex so that tracker entries still have a sensible
/// default; EntryPoint.cpp is expected to gate the cross-compile path on
/// needsCrossCompile, not on the stage.
ShaderStage mapGLShaderType(uint32_t glType) {
    switch (glType) {
    case kGL_VERTEX_SHADER:
        return ShaderStage::Vertex;
    case kGL_FRAGMENT_SHADER:
        return ShaderStage::Pixel;
    case kGL_COMPUTE_SHADER:
        return ShaderStage::Compute;
    default:
        // Geometry / tessellation / mesh / ray-tracing stages are not
        // supported by the OpenGL bridge's MSL translation path; fall
        // back to Vertex so the entry exists but will fail translation.
        return ShaderStage::Vertex;
    }
}

} // namespace

GLShaderTracker& GLShaderTracker::instance() {
    // Meyers singleton: construction is guaranteed thread-safe by C++11.
    static GLShaderTracker tracker;
    return tracker;
}

uint32_t GLShaderTracker::createShader(uint32_t type) {
    std::lock_guard<std::mutex> lock(m_mutex);

    GLShaderState state;
    state.type = type;
    state.stage = mapGLShaderType(type);

    const uint32_t name = m_nextShaderName++;
    m_shaders.emplace(name, std::move(state));
    return name;
}

void GLShaderTracker::deleteShader(uint32_t name) {
    if (name == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_shaders.erase(name);

    // Detach from any program that referenced this shader. We don't delete
    // the program; that's the caller's responsibility via deleteProgram().
    for (auto& kv : m_programs) {
        auto& attached = kv.second;
        attached.erase(std::remove(attached.begin(), attached.end(), name), attached.end());
    }
}

GLShaderState* GLShaderTracker::getShader(uint32_t name) {
    if (name == 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_shaders.find(name);
    if (it == m_shaders.end()) {
        return nullptr;
    }
    return &it->second;
}

uint32_t GLShaderTracker::createProgram() {
    std::lock_guard<std::mutex> lock(m_mutex);

    const uint32_t name = m_nextProgramName++;
    m_programs.emplace(name, std::vector<uint32_t>{});
    return name;
}

void GLShaderTracker::deleteProgram(uint32_t name) {
    if (name == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_programs.erase(name);
}

void GLShaderTracker::attachShader(uint32_t program, uint32_t shader) {
    if (program == 0 || shader == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_programs.find(program);
    if (it == m_programs.end()) {
        return;
    }

    auto& attached = it->second;
    if (std::find(attached.begin(), attached.end(), shader) == attached.end()) {
        attached.push_back(shader);
    }
}

void GLShaderTracker::detachShader(uint32_t program, uint32_t shader) {
    if (program == 0 || shader == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_programs.find(program);
    if (it == m_programs.end()) {
        return;
    }

    auto& attached = it->second;
    attached.erase(std::remove(attached.begin(), attached.end(), shader), attached.end());
}

const std::vector<uint32_t>& GLShaderTracker::getAttachedShaders(uint32_t program) const {
    static const std::vector<uint32_t> kEmpty;
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_programs.find(program);
    if (it == m_programs.end()) {
        return kEmpty;
    }
    return it->second;
}

} // namespace metalsharp