/// @file OpenGLBridge.h
/// @brief OpenGL bridge framework for opengl32.dll shim.
///
/// Bridges the Windows OpenGL API (opengl32.dll) to the macOS native OpenGL
/// framework. Phase 4b ships a structural framework: the shim DLL, a GL state
/// tracker, and a passthrough delegation path to macOS native OpenGL for
/// immediate GL 2.1 compatibility. Full GL 3.x/4.x shader translation via
/// SPIRV-Cross is scaffolded for Phase 4c — the state tracker captures enough
/// information (programs, blend state, vertex attribs, viewport) to drive a
/// future Metal backend without further instrumentation.

#pragma once

#include <cstdint>
#include <metalsharp/Platform.h>

namespace metalsharp {

/// Maximum tracked texture units
static constexpr uint32_t kMaxTextureUnits = 32;

/// Maximum tracked vertex attrib arrays
static constexpr uint32_t kMaxVertexAttribs = 16;

/// Supported OpenGL version levels reported through glGetString / glGetIntegerv
/// and used internally to gate state tracker behaviour.
enum class GLVersion : uint32_t {
    None = 0,
    GL21 = 21, // OpenGL 2.1 (macOS native legacy profile)
    GL30 = 30, // OpenGL 3.0
    GL33 = 33, // OpenGL 3.3
    GL40 = 40, // OpenGL 4.0
    GL46 = 46, // OpenGL 4.6
};

/// GL state tracker. Tracks the current OpenGL context state so we can
/// translate stateful GL calls into stateless Metal commands later.
struct GLState {
    GLVersion version = GLVersion::None;

    // Current program / shader
    uint32_t currentProgram = 0;
    bool shaderCompilePending = false;

    // Blend state
    bool blendEnabled = false;
    uint32_t blendSrcRGB = 0;
    uint32_t blendDstRGB = 0;

    // Depth state
    bool depthTestEnabled = false;
    uint32_t depthFunc = 0;

    // Current bound objects
    uint32_t boundTexture2D = 0;
    uint32_t boundFramebuffer = 0;
    uint32_t boundArrayBuffer = 0;
    uint32_t boundElementArrayBuffer = 0;

    // Vertex attrib state
    bool vertexAttribEnabled[kMaxVertexAttribs] = {};

    // Viewport
    int32_t viewportX = 0;
    int32_t viewportY = 0;
    int32_t viewportWidth = 0;
    int32_t viewportHeight = 0;
};

/// Bridge between Windows opengl32.dll callers and macOS native OpenGL.
///
/// In Phase 4b, the bridge loads the system OpenGL framework and resolves
/// native GL function pointers via @ref getGLProcAddress. The opengl32 shim
/// (see src/opengl/EntryPoint.cpp) uses these pointers to transparently
/// forward GL 1.0/1.1/2.1 calls to native GL. Future phases will layer in
/// GL 3.x/4.x shader translation (SPIRV-Cross → MSL) and Metal draw
/// emission driven by @ref GLState.
///
/// @note OpenGL compatibility profile (fixed-function + shaders mixing)
/// is partially supported. Fixed-function features (matrix stack, lighting,
/// fog, alpha test) are forwarded to macOS native OpenGL 2.1 where available.
/// When SPIRV-Cross shader translation is active (GLSL > 1.20), the fixed-
/// function pipeline is not available — applications must use core-profile
/// shader-only rendering. Full compatibility profile emulation via uniform
/// injection into translated shaders is planned for a future release.
class OpenGLBridge {
  public:
    OpenGLBridge();
    ~OpenGLBridge();

    /// Initialize the bridge. On macOS, loads the system OpenGL framework
    /// and primes the GL state tracker with sensible defaults. Idempotent.
    /// @return true on success; false if the OpenGL framework could not be
    /// loaded (the bridge remains usable as a stub but native GL calls
    /// will return null pointers).
    bool init();

    /// Get the GL state tracker.
    GLState& state() { return m_state; }
    const GLState& state() const { return m_state; }

    /// Check if native GL is available (macOS has it, though deprecated).
    bool hasNativeGL() const { return m_hasNativeGL; }

    /// Get a native GL function pointer by name. Returns nullptr if the
    /// framework is not loaded or the symbol is not found.
    void* getGLProcAddress(const char* name);

  private:
    struct Impl;
    GLState m_state;
    bool m_hasNativeGL = false;
    Impl* m_impl = nullptr;
};

} // namespace metalsharp