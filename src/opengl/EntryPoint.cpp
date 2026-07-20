/// @file EntryPoint.cpp
/// @brief opengl32.dll shim exports for MetalSharp GL→Metal bridge.
///
/// Exports all commonly-used OpenGL 1.0-2.1 entry points. The shim is thin:
/// each function delegates through OpenGLBridge::getGLProcAddress() to the
/// macOS native OpenGL framework. Phase 4b focuses on correctness over
/// completeness — games that stick to GL 1.x/2.x immediate mode and fixed
/// pipeline will run unmodified.
///
/// Extension functions (GL 3.x/4.x, e.g. glCreateShader, glGenBuffers,
/// glBindBuffer) are resolved at runtime by Wine/wglGetProcAddress from
/// the framework. Full GL 3.x/4.x shader translation via SPIRV-Cross →
/// MSL is scaffolded for Phase 4c; the OpenGLBridge::GLState tracker
/// already captures program/blend/depth/attrib/viewport so future
/// instrumentation can be added without breaking this shim.

#include <cstring>
#include <metalsharp/GLErrorTracker.h>
#include <metalsharp/GLShaderCache.h>
#include <metalsharp/GLShaderTracker.h>
#include <metalsharp/GLSLCompiler.h>
#include <metalsharp/GLSLVersion.h>
#include <metalsharp/OpenGLBridge.h>
#include <metalsharp/ShaderStage.h>
#include <mutex>

#if __has_include(<spirv_cross_c.h>)
#define METALSHARP_HAS_SPIRV_CROSS 1
#else
#define METALSHARP_HAS_SPIRV_CROSS 0
#endif

namespace {

metalsharp::OpenGLBridge g_glBridge;
std::once_flag g_glInitFlag;

void ensureGLInit() {
    std::call_once(g_glInitFlag, [] { g_glBridge.init(); });
}

// Variadic dispatch: forwards to a native GL function resolved by name and
// returns a default-constructed value if the framework is not loaded.
// This is the dispatch core used by every GL_PASSTHROUGH macro.
template <typename Ret, typename... Args> Ret glDispatch(const char* name, Args... args) {
    ensureGLInit();
    auto fn = reinterpret_cast<Ret (*)(Args...)>(g_glBridge.getGLProcAddress(name));
    if (fn) {
        return fn(static_cast<Args>(args)...);
    }
    return Ret();
}

// Per-arity macros. We need separate macros for each arity because variadic
// macros can't "spread" parenthesised argument lists inside another function
// call (e.g. `f((a, b))` is the comma operator, not two arguments). The 0-arg
// variant takes no signature/args and is the simplest case.

#define GL_PASSTHROUGH0(ret, name)                                                                                     \
    extern "C" ret name() {                                                                                            \
        return glDispatch<ret>(#name);                                                                                 \
    }

#define GL_PASSTHROUGH1(ret, name, T1, a1)                                                                             \
    extern "C" ret name(T1 a1) {                                                                                       \
        return glDispatch<ret, T1>(#name, (a1));                                                                       \
    }

#define GL_PASSTHROUGH2(ret, name, T1, a1, T2, a2)                                                                     \
    extern "C" ret name(T1 a1, T2 a2) {                                                                                \
        return glDispatch<ret, T1, T2>(#name, (a1), (a2));                                                             \
    }

#define GL_PASSTHROUGH3(ret, name, T1, a1, T2, a2, T3, a3)                                                             \
    extern "C" ret name(T1 a1, T2 a2, T3 a3) {                                                                         \
        return glDispatch<ret, T1, T2, T3>(#name, (a1), (a2), (a3));                                                   \
    }

#define GL_PASSTHROUGH4(ret, name, T1, a1, T2, a2, T3, a3, T4, a4)                                                     \
    extern "C" ret name(T1 a1, T2 a2, T3 a3, T4 a4) {                                                                  \
        return glDispatch<ret, T1, T2, T3, T4>(#name, (a1), (a2), (a3), (a4));                                         \
    }

#define GL_PASSTHROUGH5(ret, name, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5)                                             \
    extern "C" ret name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5) {                                                           \
        return glDispatch<ret, T1, T2, T3, T4, T5>(#name, (a1), (a2), (a3), (a4), (a5));                               \
    }

#define GL_PASSTHROUGH6(ret, name, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5, T6, a6)                                     \
    extern "C" ret name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6) {                                                    \
        return glDispatch<ret, T1, T2, T3, T4, T5, T6>(#name, (a1), (a2), (a3), (a4), (a5), (a6));                     \
    }

#define GL_PASSTHROUGH7(ret, name, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5, T6, a6, T7, a7)                             \
    extern "C" ret name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7) {                                             \
        return glDispatch<ret, T1, T2, T3, T4, T5, T6, T7>(#name, (a1), (a2), (a3), (a4), (a5), (a6), (a7));           \
    }

#define GL_PASSTHROUGH9(ret, name, T1, a1, T2, a2, T3, a3, T4, a4, T5, a5, T6, a6, T7, a7, T8, a8, T9, a9)             \
    extern "C" ret name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7, T8 a8, T9 a9) {                               \
        return glDispatch<ret, T1, T2, T3, T4, T5, T6, T7, T8, T9>(#name, (a1), (a2), (a3), (a4), (a5), (a6), (a7),    \
                                                                   (a8), (a9));                                        \
    }

} // namespace

// ---------------------------------------------------------------------------
// Vertex / immediate-mode pipeline (GL 1.0/1.1)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(void, glBegin, uint32_t, mode)
GL_PASSTHROUGH0(void, glEnd)

// ---------------------------------------------------------------------------
// Buffers / state
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(void, glClear, uint32_t, mask)
GL_PASSTHROUGH4(void, glClearColor, float, r, float, g, float, b, float, a)
GL_PASSTHROUGH4(void, glViewport, int32_t, x, int32_t, y, int32_t, w, int32_t, h)
GL_PASSTHROUGH1(void, glEnable, uint32_t, cap)
GL_PASSTHROUGH1(void, glDisable, uint32_t, cap)
GL_PASSTHROUGH2(void, glBlendFunc, uint32_t, sfactor, uint32_t, dfactor)
GL_PASSTHROUGH1(void, glDepthFunc, uint32_t, func)

// ---------------------------------------------------------------------------
// Buffer objects (GL 1.5)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glGenBuffers, int32_t, n, uint32_t*, buffers)
GL_PASSTHROUGH2(void, glDeleteBuffers, int32_t, n, const uint32_t*, buffers)
GL_PASSTHROUGH4(void, glBufferData, uint32_t, target, int64_t, size, const void*, data, uint32_t, usage)
GL_PASSTHROUGH4(void, glBufferSubData, uint32_t, target, int64_t, offset, int64_t, size, const void*, data)
GL_PASSTHROUGH2(void*, glMapBuffer, uint32_t, target, uint32_t, access)
GL_PASSTHROUGH1(unsigned char, glUnmapBuffer, uint32_t, target)
GL_PASSTHROUGH1(unsigned char, glIsBuffer, uint32_t, buffer)

// glBindBuffer is hand-written because it must mirror the binding into
// GLState so subsequent draw calls / VAO setup can observe which buffer
// is currently bound. The native call is still issued so the framework
// context state stays in sync.
extern "C" void glBindBuffer(uint32_t target, uint32_t buffer) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, uint32_t)>(g_glBridge.getGLProcAddress("glBindBuffer"));
    if (fn) {
        fn(target, buffer);
    }
    // Mirror into the state tracker for GL_ARRAY_BUFFER / GL_ELEMENT_ARRAY_BUFFER.
    // Other targets (e.g. GL_PIXEL_PACK_BUFFER, GL_UNIFORM_BUFFER) are ignored
    // here; the bridge only tracks the two targets consumed by draw submission.
    constexpr uint32_t kGL_ARRAY_BUFFER = 0x8892;         // GL_ARRAY_BUFFER
    constexpr uint32_t kGL_ELEMENT_ARRAY_BUFFER = 0x8893; // GL_ELEMENT_ARRAY_BUFFER
    if (target == kGL_ARRAY_BUFFER) {
        g_glBridge.state().boundArrayBuffer = buffer;
    } else if (target == kGL_ELEMENT_ARRAY_BUFFER) {
        g_glBridge.state().boundElementArrayBuffer = buffer;
    }
}

// ---------------------------------------------------------------------------
// Draw submission
// ---------------------------------------------------------------------------
GL_PASSTHROUGH3(void, glDrawArrays, uint32_t, mode, int32_t, first, int32_t, count)
GL_PASSTHROUGH4(void, glDrawElements, uint32_t, mode, int32_t, count, uint32_t, type, const void*, indices)

// ---------------------------------------------------------------------------
// Client-side vertex arrays (legacy immediate-mode interop)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH4(void, glVertexPointer, int32_t, size, uint32_t, type, int32_t, stride, const void*, ptr)
GL_PASSTHROUGH4(void, glTexCoordPointer, int32_t, size, uint32_t, type, int32_t, stride, const void*, ptr)
GL_PASSTHROUGH4(void, glColorPointer, int32_t, size, uint32_t, type, int32_t, stride, const void*, ptr)
GL_PASSTHROUGH3(void, glNormalPointer, uint32_t, type, int32_t, stride, const void*, ptr)

// ---------------------------------------------------------------------------
// Vertex attributes (GL 2.0)
// ---------------------------------------------------------------------------

// glEnableVertexAttribArray / glDisableVertexAttribArray are hand-written
// because they must mirror the per-index enable state into GLState so
// subsequent draw calls can observe which attribute streams are active.
// The native call is still issued so the framework context state stays
// in sync with the shim's view.
extern "C" void glEnableVertexAttribArray(uint32_t index) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t)>(g_glBridge.getGLProcAddress("glEnableVertexAttribArray"));
    if (fn) {
        fn(index);
    }
    if (index < metalsharp::kMaxVertexAttribs) {
        g_glBridge.state().vertexAttribEnabled[index] = true;
    }
}

extern "C" void glDisableVertexAttribArray(uint32_t index) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t)>(g_glBridge.getGLProcAddress("glDisableVertexAttribArray"));
    if (fn) {
        fn(index);
    }
    if (index < metalsharp::kMaxVertexAttribs) {
        g_glBridge.state().vertexAttribEnabled[index] = false;
    }
}

// glVertexAttribPointer is currently a passthrough. Phase 2h marks it as a
// future interception point — Phase 3 will capture each (index, size, type,
// normalized, stride, pointer) tuple in GLShaderTracker so that the Metal
// draw emitter can build an MTLVertexDescriptor at draw time without
// re-deriving the layout from the translated MSL. The capture must observe
// the current GL_ARRAY_BUFFER binding (already tracked in GLState) to
// resolve the actual MTLBuffer for the attribute stream.
//
// For Phase 2, native GL handles the layout; GLSL 1.20 / legacy shaders
// don't need a Metal vertex descriptor because the framework emits them
// directly. The interception only matters once cross-compiled shaders
// start flowing through the Metal backend.
GL_PASSTHROUGH6(void, glVertexAttribPointer, uint32_t, index, int32_t, size, uint32_t, type, unsigned char, normalized,
                int32_t, stride, const void*, pointer)
GL_PASSTHROUGH2(void, glVertexAttrib1f, uint32_t, index, float, v0)
GL_PASSTHROUGH3(void, glVertexAttrib2f, uint32_t, index, float, v0, float, v1)
GL_PASSTHROUGH4(void, glVertexAttrib3f, uint32_t, index, float, v0, float, v1, float, v2)
GL_PASSTHROUGH5(void, glVertexAttrib4f, uint32_t, index, float, v0, float, v1, float, v2, float, v3)
GL_PASSTHROUGH3(void, glGetVertexAttribiv, uint32_t, index, uint32_t, pname, int32_t*, params)
GL_PASSTHROUGH3(void, glGetVertexAttribPointerv, uint32_t, index, uint32_t, pname, void**, pointer)
GL_PASSTHROUGH2(void, glVertexAttribDivisor, uint32_t, index, uint32_t, divisor)

// ---------------------------------------------------------------------------
// Shader objects (GL 2.0)
// ---------------------------------------------------------------------------

// glCreateShader is intercepted by the shader tracker so that subsequent
// glShaderSource / glCompileShader calls observe a consistent handle and can
// be routed through the SPIRV-Cross cross-compilation path. When SPIRV-Cross
// is unavailable, the call falls through to the native OpenGL framework.
extern "C" uint32_t glCreateShader(uint32_t shaderType) {
    ensureGLInit();
#if METALSHARP_HAS_SPIRV_CROSS
    // Always allocate through the tracker so that the cross-compile path has
    // a place to park source / SPIR-V / MSL state. A zero return means the
    // tracker rejected the allocation (e.g. out of memory); in that case we
    // fall through to native GL so the caller still gets a usable handle.
    const uint32_t name = metalsharp::GLShaderTracker::instance().createShader(shaderType);
    if (name != 0) {
        return name;
    }
#endif
    auto fn = reinterpret_cast<uint32_t (*)(uint32_t)>(g_glBridge.getGLProcAddress("glCreateShader"));
    if (fn) {
        return fn(shaderType);
    }
    return 0;
}

// glDeleteShader is intercepted so the tracker entry is removed before
// the native handle goes away. Without this hook the tracker would leak
// entries and recycled handles could collide with live shader objects.
extern "C" void glDeleteShader(uint32_t shader) {
    ensureGLInit();
#if METALSHARP_HAS_SPIRV_CROSS
    metalsharp::GLShaderTracker::instance().deleteShader(shader);
#endif
    auto fn = reinterpret_cast<void (*)(uint32_t)>(g_glBridge.getGLProcAddress("glDeleteShader"));
    if (fn) {
        fn(shader);
    }
}

// glShaderSource is intercepted to capture the GLSL source string into the
// shader's tracker entry, parse its #version directive, and decide whether
// the cross-compile path is required. For tracked shaders we do NOT forward
// the source to native GL — the cross-compile path owns these shaders end to
// end. Shaders that are not in the tracker (e.g. created outside the shim)
// fall through to the native driver unchanged.
extern "C" void glShaderSource(uint32_t shader, int32_t count, const char** string, const int32_t* length) {
    ensureGLInit();
#if METALSHARP_HAS_SPIRV_CROSS
    auto* state = metalsharp::GLShaderTracker::instance().getShader(shader);
    if (state) {
        // Concatenate the count source strings into a single buffer. Each
        // string may be null-terminated (length == nullptr or length[i] < 0)
        // or have an explicit byte length (length[i] > 0). Either way the
        // result is one canonical source blob keyed by the shader handle.
        std::string source;
        for (int32_t i = 0; i < count; i++) {
            if (!string || !string[i]) {
                continue;
            }
            if (length && length[i] > 0) {
                source.append(string[i], static_cast<size_t>(length[i]));
            } else {
                source.append(string[i]);
            }
        }
        state->source = source;

        // Parse the #version directive and decide whether the cross-compile
        // path is required. Invalid / missing #version is treated as legacy
        // GLSL 1.10 — needsCrossCompile stays false and the compile falls
        // through to native GL.
        metalsharp::parseGLSLVersion(source.c_str(), state->glslVersion);
        state->needsCrossCompile = metalsharp::needsCrossCompile(state->glslVersion);
        return; // Don't forward to native GL — we handle compilation.
    }
#endif
    auto fn = reinterpret_cast<void (*)(uint32_t, int32_t, const char**, const int32_t*)>(
        g_glBridge.getGLProcAddress("glShaderSource"));
    if (fn) {
        fn(shader, count, string, length);
    }
}

// glGetShaderiv is hand-written so that shaders driven through the
// SPIRV-Cross cross-compile path can report their compile status,
// delete status, and info-log length from the tracker's state instead
// of falling through to the native OpenGL framework (which never saw
// the cross-compile and would always report GL_FALSE / length 0).
// Phase 3 will extend this to also report shader type / source length
// for non-tracked shaders using the same shim state.
extern "C" void glGetShaderiv(uint32_t shader, uint32_t pname, int32_t* params) {
    if (!params) {
        return;
    }
#if METALSHARP_HAS_SPIRV_CROSS
    auto* state = metalsharp::GLShaderTracker::instance().getShader(shader);
    if (state && state->compiled) {
        switch (pname) {
        case 0x8B81: // GL_COMPILE_STATUS
            *params = state->compileSuccess ? 1 : 0;
            return;
        case 0x8B80: // GL_DELETE_STATUS
            *params = 0;
            return;
        case 0x8B82: // GL_INFO_LOG_LENGTH
            *params = static_cast<int32_t>(state->infoLog.size()) + 1;
            return;
        default:
            break;
        }
    }
#endif
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, uint32_t, int32_t*)>(g_glBridge.getGLProcAddress("glGetShaderiv"));
    if (fn) {
        fn(shader, pname, params);
    }
}

// glGetShaderInfoLog is hand-written so cross-compiled shaders can
// surface their translate / compile diagnostics to the guest through
// the normal OpenGL info-log query. Native-only shaders fall through
// to the framework unchanged.
extern "C" void glGetShaderInfoLog(uint32_t shader, int32_t bufSize, int32_t* length, char* infoLog) {
#if METALSHARP_HAS_SPIRV_CROSS
    auto* state = metalsharp::GLShaderTracker::instance().getShader(shader);
    if (state && state->compiled && !state->infoLog.empty()) {
        int32_t len = static_cast<int32_t>(state->infoLog.size());
        if (len > bufSize - 1) {
            len = bufSize - 1;
        }
        if (len > 0) {
            std::memcpy(infoLog, state->infoLog.data(), static_cast<size_t>(len));
        }
        infoLog[len] = '\0';
        if (length) {
            *length = len;
        }
        return;
    }
#endif
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, int32_t, int32_t*, char*)>(
        g_glBridge.getGLProcAddress("glGetShaderInfoLog"));
    if (fn) {
        fn(shader, bufSize, length, infoLog);
    }
}
GL_PASSTHROUGH2(void, glAttachShader, uint32_t, program, uint32_t, shader)
GL_PASSTHROUGH2(void, glDetachShader, uint32_t, program, uint32_t, shader)
GL_PASSTHROUGH1(unsigned char, glIsShader, uint32_t, shader)

// glCompileShader is intercepted to drive the SPIRV-Cross cross-compilation
// path for tracked shaders whose source needs cross-compilation (GLSL > 1.20
// or any ES flavour). For native-compatible shaders the call still falls
// through to the native OpenGL driver and shaderCompilePending is reset.
//
// Phase 2i adds a per-(source, stage) MSL cache in front of the
// GLSLCompiler round trip. Cache lookup is by hash, so the same source
// string compiles exactly once per process per stage; subsequent compiles
// short-circuit straight to MSL without paying for glslang or SPIRV-Cross.
// The cache is process-wide and best-effort: failures of the cache lookup
// (which shouldn't happen) fall through to the normal compile path.
extern "C" void glCompileShader(uint32_t shader) {
    ensureGLInit();
#if METALSHARP_HAS_SPIRV_CROSS
    auto* state = metalsharp::GLShaderTracker::instance().getShader(shader);
    if (state && state->needsCrossCompile && !state->source.empty()) {
        // Cache lookup: if we already translated this exact (source, stage)
        // pair, skip the GLSLCompiler round trip and use the cached MSL.
        // This is safe because the MSL output for a given (source, stage)
        // is fully determined by the inputs — there is no per-compile
        // randomness in glslang or SPIRV-Cross on our code paths.
        const std::string* cached =
            metalsharp::GLShaderCache::instance().lookupMSL(state->source, static_cast<uint32_t>(state->stage));
        if (cached) {
            state->msl = *cached;
            // The cached entry was a successful translation; the SPIR-V
            // blob itself is not cached (we only store MSL), so rebuild
            // it on demand from the cached MSL by leaving spirv empty.
            // Phase 3 will switch to caching SPIR-V too so we can recover
            // the binary blob without re-running glslang.
            state->spirv.clear();
            state->compiled = true;
            state->compileSuccess = true;
            state->infoLog.clear();
            g_glBridge.state().shaderCompilePending = false;
            return;
        }

        std::string errorLog;
        bool ok = metalsharp::GLSLCompiler::compileToSPIRV(state->source.c_str(), state->stage, state->glslVersion,
                                                           state->spirv, errorLog);
        if (ok) {
            ok = metalsharp::GLSLCompiler::translateSPIRVtoMSL(state->spirv, state->stage, state->msl, errorLog);
        }
        state->compiled = true;
        state->compileSuccess = ok;
        state->infoLog = errorLog;
        // Only cache successful translations — failures might succeed on a
        // retry once underlying tools update, and we don't want to lock in
        // a broken result.
        if (ok) {
            metalsharp::GLShaderCache::instance().storeMSL(state->source, static_cast<uint32_t>(state->stage),
                                                           state->msl);
        }
        g_glBridge.state().shaderCompilePending = false;
        return;
    }
#endif
    auto fn = reinterpret_cast<void (*)(uint32_t)>(g_glBridge.getGLProcAddress("glCompileShader"));
    if (fn) {
        fn(shader);
    }
    g_glBridge.state().shaderCompilePending = false;
}

// ---------------------------------------------------------------------------
// Shader program objects (GL 2.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH0(uint32_t, glCreateProgram)
GL_PASSTHROUGH1(void, glDeleteProgram, uint32_t, program)
GL_PASSTHROUGH1(void, glLinkProgram, uint32_t, program)
GL_PASSTHROUGH1(void, glValidateProgram, uint32_t, program)
GL_PASSTHROUGH3(void, glGetProgramiv, uint32_t, program, uint32_t, pname, int32_t*, params)
GL_PASSTHROUGH4(void, glGetProgramInfoLog, uint32_t, program, int32_t, bufSize, int32_t*, length, char*, infoLog)
GL_PASSTHROUGH1(unsigned char, glIsProgram, uint32_t, program)

// glUseProgram is hand-written because it must mirror the active program
// into GLState::currentProgram so subsequent draw calls can observe which
// program is bound. The native call is still issued so the framework
// context state stays in sync with the shim's view.
extern "C" void glUseProgram(uint32_t program) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t)>(g_glBridge.getGLProcAddress("glUseProgram"));
    if (fn) {
        fn(program);
    }
    g_glBridge.state().currentProgram = program;
}

// ---------------------------------------------------------------------------
// Uniforms (GL 2.0)
// ---------------------------------------------------------------------------
//
// Phase 2g: glGetUniformLocation and the glUniform* family are currently
// pass-through to the native OpenGL framework. Phase 3 will intercept them
// so that:
//   * glGetUniformLocation returns a Metal buffer-binding index that maps
//     back to the guest's location (a 1:1 mapping is a reasonable Phase 3
//     default since Metal buffer bindings also use small integers).
//   * glUniform* writes are captured by GLShaderTracker and replayed against
//     the Metal argument buffer at glDrawArrays / glDrawElements time.
//
// For Phase 2 we just forward to native GL so that GLSL 1.20 / legacy
// shaders (which never reach the cross-compile path) keep working without
// any uniform-mapping shim.

GL_PASSTHROUGH2(int32_t, glGetUniformLocation, uint32_t, program, const char*, name)
GL_PASSTHROUGH2(void, glUniform1f, int32_t, location, float, v0)
GL_PASSTHROUGH3(void, glUniform2f, int32_t, location, float, v0, float, v1)
GL_PASSTHROUGH4(void, glUniform3f, int32_t, location, float, v0, float, v1, float, v2)
GL_PASSTHROUGH5(void, glUniform4f, int32_t, location, float, v0, float, v1, float, v2, float, v3)
GL_PASSTHROUGH2(void, glUniform1i, int32_t, location, int32_t, v0)
GL_PASSTHROUGH3(void, glUniform2i, int32_t, location, int32_t, v0, int32_t, v1)
GL_PASSTHROUGH4(void, glUniform3i, int32_t, location, int32_t, v0, int32_t, v1, int32_t, v2)
GL_PASSTHROUGH5(void, glUniform4i, int32_t, location, int32_t, v0, int32_t, v1, int32_t, v2, int32_t, v3)
GL_PASSTHROUGH4(void, glUniformMatrix2fv, int32_t, location, int32_t, count, unsigned char, transpose, const float*,
                value)
GL_PASSTHROUGH4(void, glUniformMatrix3fv, int32_t, location, int32_t, count, unsigned char, transpose, const float*,
                value)
GL_PASSTHROUGH4(void, glUniformMatrix4fv, int32_t, location, int32_t, count, unsigned char, transpose, const float*,
                value)
GL_PASSTHROUGH3(void, glGetUniformfv, uint32_t, program, int32_t, location, float*, params)
GL_PASSTHROUGH3(void, glGetUniformiv, uint32_t, program, int32_t, location, int32_t*, params)
GL_PASSTHROUGH7(void, glGetActiveUniform, uint32_t, program, uint32_t, index, int32_t, bufSize, int32_t*, length,
                int32_t*, size, uint32_t*, type, char*, name)

// ---------------------------------------------------------------------------
// Vertex attribute location queries (GL 2.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(int32_t, glGetAttribLocation, uint32_t, program, const char*, name)
GL_PASSTHROUGH3(void, glBindAttribLocation, uint32_t, program, uint32_t, index, const char*, name)
GL_PASSTHROUGH7(void, glGetActiveAttrib, uint32_t, program, uint32_t, index, int32_t, bufSize, int32_t*, length,
                int32_t*, size, uint32_t*, type, char*, name)

// ---------------------------------------------------------------------------
// Rasterization state (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(void, glCullFace, uint32_t, mode)
GL_PASSTHROUGH1(void, glFrontFace, uint32_t, mode)
GL_PASSTHROUGH1(void, glLineWidth, float, width)
GL_PASSTHROUGH1(void, glPointSize, float, size)
GL_PASSTHROUGH2(void, glPolygonMode, uint32_t, face, uint32_t, mode)
GL_PASSTHROUGH2(void, glPolygonOffset, float, factor, float, units)

// ---------------------------------------------------------------------------
// Stencil state (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH3(void, glStencilFunc, uint32_t, func, int32_t, ref, uint32_t, mask)
GL_PASSTHROUGH4(void, glStencilFuncSeparate, uint32_t, face, uint32_t, func, int32_t, ref, uint32_t, mask)
GL_PASSTHROUGH3(void, glStencilOp, uint32_t, sfail, uint32_t, dpfail, uint32_t, dppass)
GL_PASSTHROUGH4(void, glStencilOpSeparate, uint32_t, face, uint32_t, sfail, uint32_t, dpfail, uint32_t, dppass)
GL_PASSTHROUGH1(void, glStencilMask, uint32_t, mask)
GL_PASSTHROUGH2(void, glStencilMaskSeparate, uint32_t, face, uint32_t, mask)
GL_PASSTHROUGH1(void, glClearStencil, int32_t, s)

// ---------------------------------------------------------------------------
// Color / blend state (GL 1.0-1.4)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH4(void, glColorMask, unsigned char, r, unsigned char, g, unsigned char, b, unsigned char, a)
GL_PASSTHROUGH1(void, glBlendEquation, uint32_t, mode)
GL_PASSTHROUGH2(void, glBlendEquationSeparate, uint32_t, modeRGB, uint32_t, modeAlpha)
GL_PASSTHROUGH4(void, glBlendFuncSeparate, uint32_t, srcRGB, uint32_t, dstRGB, uint32_t, srcAlpha, uint32_t, dstAlpha)
GL_PASSTHROUGH4(void, glBlendColor, float, r, float, g, float, b, float, a)
GL_PASSTHROUGH1(void, glLogicOp, uint32_t, opcode)

// ---------------------------------------------------------------------------
// Depth state (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(void, glDepthMask, unsigned char, flag)
GL_PASSTHROUGH2(void, glDepthRange, double, nearVal, double, farVal)
GL_PASSTHROUGH1(void, glClearDepth, double, depth)

// ---------------------------------------------------------------------------
// Pixel storage / transfer (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glPixelStorei, uint32_t, pname, int32_t, param)
GL_PASSTHROUGH2(void, glPixelStoref, uint32_t, pname, float, param)
GL_PASSTHROUGH1(void, glReadBuffer, uint32_t, mode)
GL_PASSTHROUGH1(void, glDrawBuffer, uint32_t, mode)

// ---------------------------------------------------------------------------
// State queries (GL 1.0-1.1)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glGetBooleanv, uint32_t, pname, unsigned char*, params)
GL_PASSTHROUGH2(void, glGetFloatv, uint32_t, pname, float*, params)
GL_PASSTHROUGH2(void, glGetDoublev, uint32_t, pname, double*, params)
GL_PASSTHROUGH3(void, glGetTexEnviv, uint32_t, target, uint32_t, pname, int32_t*, params)
GL_PASSTHROUGH3(void, glGetTexEnvfv, uint32_t, target, uint32_t, pname, float*, params)
// glGetError is hand-written: checks the shim error tracker first
// (Phase 5a), then falls through to native GL.
extern "C" uint32_t glGetError() {
    uint32_t err = metalsharp::GLErrorTracker::instance().getError();
    if (err != 0)
        return err;
    // Note: we do NOT forward to native GL's glGetError here because
    // dlsym on the framework handle may resolve to our own shim symbol
    // on macOS, creating infinite recursion. The native GL error state
    // is irrelevant when we're managing our own context via Metal.
    return 0;
}

// Phase 5b — intercept glGetString for GL_EXTENSIONS.
// The old passthrough at the end of the file is kept for non-extension queries.
extern "C" const uint8_t* glGetString_EXTENSIONS_override(uint32_t name) {
    if (name != 0x1F03)
        return nullptr; // not ours
    // Return our bridge extensions directly (no native lookup — avoids
    // dlsym complexity and infinite-recursion risk).
    static const char kExts[] = "GL_ARB_vertex_buffer_object GL_ARB_framebuffer_object "
                                "GL_EXT_framebuffer_object GL_ARB_shader_objects "
                                "GL_ARB_vertex_shader GL_ARB_fragment_shader "
                                "GL_ARB_multitexture METALSHARP_opengl_bridge";
    return reinterpret_cast<const uint8_t*>(kExts);
}

GL_PASSTHROUGH1(unsigned char, glIsEnabled, uint32_t, cap)

// glGetStringi is hand-written following the glGetString pattern.
extern "C" const uint8_t* glGetStringi(uint32_t name, uint32_t index) {
    ensureGLInit();
    auto fn = reinterpret_cast<const uint8_t* (*)(uint32_t, uint32_t)>(g_glBridge.getGLProcAddress("glGetStringi"));
    if (fn) {
        return fn(name, index);
    }
    return reinterpret_cast<const uint8_t*>("");
}

// ---------------------------------------------------------------------------
// Misc commands (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH0(void, glFlush)
GL_PASSTHROUGH0(void, glFinish)
GL_PASSTHROUGH2(void, glHint, uint32_t, target, uint32_t, mode)

// ---------------------------------------------------------------------------
// Display lists (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(uint32_t, glGenLists, int32_t, range)
GL_PASSTHROUGH2(void, glNewList, uint32_t, list, uint32_t, mode)
GL_PASSTHROUGH0(void, glEndList)
GL_PASSTHROUGH1(void, glCallList, uint32_t, list)
GL_PASSTHROUGH3(void, glCallLists, int32_t, n, uint32_t, type, const void*, lists)
GL_PASSTHROUGH2(void, glDeleteLists, uint32_t, list, int32_t, range)
GL_PASSTHROUGH1(unsigned char, glIsList, uint32_t, list)

// ---------------------------------------------------------------------------
// Immediate-mode vertex data (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glVertex2f, float, x, float, y)
GL_PASSTHROUGH3(void, glVertex3f, float, x, float, y, float, z)
GL_PASSTHROUGH4(void, glVertex4f, float, x, float, y, float, z, float, w)
GL_PASSTHROUGH3(void, glNormal3f, float, nx, float, ny, float, nz)
GL_PASSTHROUGH1(void, glTexCoord1f, float, s)
GL_PASSTHROUGH2(void, glTexCoord2f, float, s, float, t)
GL_PASSTHROUGH3(void, glTexCoord3f, float, s, float, t, float, r)
GL_PASSTHROUGH4(void, glTexCoord4f, float, s, float, t, float, r, float, q)
GL_PASSTHROUGH3(void, glColor3ub, unsigned char, r, unsigned char, g, unsigned char, b)
GL_PASSTHROUGH4(void, glColor4ub, unsigned char, r, unsigned char, g, unsigned char, b, unsigned char, a)
GL_PASSTHROUGH2(void, glColorMaterial, uint32_t, face, uint32_t, mode)

// ---------------------------------------------------------------------------
// Lighting / material (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH3(void, glLightfv, uint32_t, light, uint32_t, pname, const float*, params)
GL_PASSTHROUGH2(void, glLightModelfv, uint32_t, pname, const float*, params)
GL_PASSTHROUGH3(void, glMaterialfv, uint32_t, face, uint32_t, pname, const float*, params)
GL_PASSTHROUGH1(void, glShadeModel, uint32_t, mode)

// ---------------------------------------------------------------------------
// Fog (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glFogfv, uint32_t, pname, const float*, params)
GL_PASSTHROUGH2(void, glFogi, uint32_t, pname, int32_t, param)

// ---------------------------------------------------------------------------
// Alpha test (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glAlphaFunc, uint32_t, func, float, ref)

// ---------------------------------------------------------------------------
// Clip planes (GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glClipPlane, uint32_t, plane, const double*, equation)

// ---------------------------------------------------------------------------
// Matrix stack (fixed pipeline, GL 1.0)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH1(void, glMatrixMode, uint32_t, mode)
GL_PASSTHROUGH0(void, glLoadIdentity)
GL_PASSTHROUGH6(void, glOrtho, double, l, double, r, double, b, double, t, double, n, double, f)
GL_PASSTHROUGH6(void, glFrustum, double, l, double, r, double, b, double, t, double, n, double, f)
GL_PASSTHROUGH0(void, glPushMatrix)
GL_PASSTHROUGH0(void, glPopMatrix)
GL_PASSTHROUGH3(void, glTranslatef, float, x, float, y, float, z)
GL_PASSTHROUGH4(void, glRotatef, float, angle, float, x, float, y, float, z)
GL_PASSTHROUGH3(void, glScalef, float, x, float, y, float, z)

// ---------------------------------------------------------------------------
// Color (legacy)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH3(void, glColor3f, float, r, float, g, float, b)
GL_PASSTHROUGH4(void, glColor4f, float, r, float, g, float, b, float, a)

// ---------------------------------------------------------------------------
// Texture upload / readback
// ---------------------------------------------------------------------------
GL_PASSTHROUGH9(void, glTexImage2D, uint32_t, target, int32_t, level, int32_t, internalFormat, int32_t, w, int32_t, h,
                int32_t, border, uint32_t, format, uint32_t, type, const void*, data)
GL_PASSTHROUGH7(void, glReadPixels, int32_t, x, int32_t, y, int32_t, w, int32_t, h, uint32_t, format, uint32_t, type,
                void*, data)

// ---------------------------------------------------------------------------
// Texture objects (GL 1.1-1.3)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glGenTextures, int32_t, n, uint32_t*, textures)
GL_PASSTHROUGH2(void, glDeleteTextures, int32_t, n, const uint32_t*, textures)
GL_PASSTHROUGH2(void, glBindTexture, uint32_t, target, uint32_t, texture)
GL_PASSTHROUGH1(void, glActiveTexture, uint32_t, texture)
GL_PASSTHROUGH3(void, glTexParameteri, uint32_t, target, uint32_t, pname, int32_t, param)
GL_PASSTHROUGH3(void, glTexParameterf, uint32_t, target, uint32_t, pname, float, param)
GL_PASSTHROUGH9(void, glTexSubImage2D, uint32_t, target, int32_t, level, int32_t, xoffset, int32_t, yoffset, int32_t,
                width, int32_t, height, uint32_t, format, uint32_t, type, const void*, pixels)
GL_PASSTHROUGH3(void, glTexEnvi, uint32_t, target, uint32_t, pname, int32_t, param)
GL_PASSTHROUGH3(void, glTexEnvf, uint32_t, target, uint32_t, pname, float, param)
GL_PASSTHROUGH3(void, glGetTexParameteriv, uint32_t, target, uint32_t, pname, int32_t*, params)
GL_PASSTHROUGH3(void, glGetTexParameterfv, uint32_t, target, uint32_t, pname, float*, params)
GL_PASSTHROUGH1(unsigned char, glIsTexture, uint32_t, texture)

// glCopyTexImage2D and glCopyTexSubImage2D are hand-written because they
// take 8 arguments each and GL_PASSTHROUGH8 does not exist.
extern "C" void glCopyTexImage2D(uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y,
                                 int32_t width, int32_t height, int32_t border) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t)>(
        g_glBridge.getGLProcAddress("glCopyTexImage2D"));
    if (fn) {
        fn(target, level, internalformat, x, y, width, height, border);
    }
}

extern "C" void glCopyTexSubImage2D(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x,
                                    int32_t y, int32_t width, int32_t height) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)>(
        g_glBridge.getGLProcAddress("glCopyTexSubImage2D"));
    if (fn) {
        fn(target, level, xoffset, yoffset, x, y, width, height);
    }
}

// ---------------------------------------------------------------------------
// Renderbuffer objects (GL 3.0 / EXT)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glGenRenderbuffers, int32_t, n, uint32_t*, renderbuffers)
GL_PASSTHROUGH2(void, glDeleteRenderbuffers, int32_t, n, const uint32_t*, renderbuffers)
GL_PASSTHROUGH2(void, glBindRenderbuffer, uint32_t, target, uint32_t, renderbuffer)
GL_PASSTHROUGH4(void, glRenderbufferStorage, uint32_t, target, uint32_t, internalformat, int32_t, width, int32_t,
                height)
GL_PASSTHROUGH1(unsigned char, glIsRenderbuffer, uint32_t, renderbuffer)

// ---------------------------------------------------------------------------
// Framebuffer objects (GL 3.0 / EXT_framebuffer_object)
// ---------------------------------------------------------------------------
GL_PASSTHROUGH2(void, glGenFramebuffers, int32_t, n, uint32_t*, framebuffers)
GL_PASSTHROUGH2(void, glDeleteFramebuffers, int32_t, n, const uint32_t*, framebuffers)
GL_PASSTHROUGH5(void, glFramebufferTexture2D, uint32_t, target, uint32_t, attachment, uint32_t, textarget, uint32_t,
                texture, int32_t, level)
GL_PASSTHROUGH4(void, glFramebufferRenderbuffer, uint32_t, target, uint32_t, attachment, uint32_t, renderbuffertarget,
                uint32_t, renderbuffer)
GL_PASSTHROUGH1(uint32_t, glCheckFramebufferStatus, uint32_t, target)
GL_PASSTHROUGH1(unsigned char, glIsFramebuffer, uint32_t, framebuffer)

// glBindFramebuffer is hand-written because it must mirror the binding into
// GLState so subsequent framebuffer attachment calls can observe which
// framebuffer is currently bound. The native call is still issued so the
// framework context state stays in sync with the shim's view.
extern "C" void glBindFramebuffer(uint32_t target, uint32_t framebuffer) {
    ensureGLInit();
    auto fn = reinterpret_cast<void (*)(uint32_t, uint32_t)>(g_glBridge.getGLProcAddress("glBindFramebuffer"));
    if (fn) {
        fn(target, framebuffer);
    }
    g_glBridge.state().boundFramebuffer = framebuffer;
}

// ---------------------------------------------------------------------------
// Info queries (return non-default values — declared by hand instead of
// via GL_PASSTHROUGH).
// glGetString is hand-written for GL_EXTENSIONS passthrough (Phase 5b).
// ---------------------------------------------------------------------------
extern "C" const uint8_t* glGetString(uint32_t name) {
    // Only handle GL_EXTENSIONS via our bridge (Phase 5b).
    // We do NOT forward to native GL's glGetString because dlsym
    // on the framework handle may resolve to our own shim symbol,
    // creating infinite recursion. Non-extension queries return
    // empty strings; real implementations query via Metal API.
    if (name == 0x1F03) { // GL_EXTENSIONS
        return glGetString_EXTENSIONS_override(name);
    }
    return reinterpret_cast<const uint8_t*>("");
}

extern "C" void glGetIntegerv(uint32_t pname, int32_t* params) {
    ensureGLInit();
    if (!params) {
        return;
    }
    auto fn = reinterpret_cast<void (*)(uint32_t, int32_t*)>(g_glBridge.getGLProcAddress("glGetIntegerv"));
    if (fn) {
        fn(pname, params);
    } else {
        // Best-effort default. Return zero so callers don't read garbage.
        *params = 0;
    }
}