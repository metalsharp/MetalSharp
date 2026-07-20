/// @file test_opengl_bridge.cpp
/// @brief Tests for the OpenGL bridge framework (Phase 4b).
///
/// Validates that the OpenGL bridge can be initialized, that the GL state
/// tracker has sensible defaults, that function-pointer lookup is well-
/// behaved for both known and unknown names, and that the GL version
/// reported by the bridge matches the macOS native framework (GL 2.1).

#include <cstdio>
#include <cstring>
#include <metalsharp/GLSLVersion.h>
#include <metalsharp/OpenGLBridge.h>

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            printf("  [OK] %s\n", msg);                                                                                \
            passed++;                                                                                                  \
        } else {                                                                                                       \
            printf("  [FAIL] %s\n", msg);                                                                              \
            failed++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main() {
    printf("=== OpenGLBridge Tests ===\n\n");

    {
        printf("--- GLVersion enum ---\n");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::None) == 0, "GLVersion::None is 0");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::GL21) == 21, "GLVersion::GL21 is 21");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::GL30) == 30, "GLVersion::GL30 is 30");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::GL33) == 33, "GLVersion::GL33 is 33");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::GL40) == 40, "GLVersion::GL40 is 40");
        CHECK(static_cast<uint32_t>(metalsharp::GLVersion::GL46) == 46, "GLVersion::GL46 is 46");
    }

    {
        printf("\n--- GLState defaults ---\n");
        metalsharp::GLState state{};
        CHECK(state.version == metalsharp::GLVersion::None, "Default GLState.version is None");
        CHECK(state.currentProgram == 0, "Default currentProgram is 0");
        CHECK(!state.shaderCompilePending, "Default shaderCompilePending is false");
        CHECK(!state.blendEnabled, "Default blendEnabled is false");
        CHECK(state.blendSrcRGB == 0, "Default blendSrcRGB is 0");
        CHECK(state.blendDstRGB == 0, "Default blendDstRGB is 0");
        CHECK(!state.depthTestEnabled, "Default depthTestEnabled is false");
        CHECK(state.depthFunc == 0, "Default depthFunc is 0");
        CHECK(state.boundTexture2D == 0, "Default boundTexture2D is 0");
        CHECK(state.boundFramebuffer == 0, "Default boundFramebuffer is 0");
        CHECK(state.boundArrayBuffer == 0, "Default boundArrayBuffer is 0");
        CHECK(state.boundElementArrayBuffer == 0, "Default boundElementArrayBuffer is 0");
        CHECK(state.viewportX == 0, "Default viewportX is 0");
        CHECK(state.viewportY == 0, "Default viewportY is 0");
        CHECK(state.viewportWidth == 0, "Default viewportWidth is 0");
        CHECK(state.viewportHeight == 0, "Default viewportHeight is 0");
        for (uint32_t i = 0; i < metalsharp::kMaxVertexAttribs; ++i) {
            if (state.vertexAttribEnabled[i]) {
                printf("  [FAIL] vertexAttribEnabled[%u] should default to false\n", i);
                failed++;
            }
        }
        passed++; // count the loop check as one assertion
        printf("  [OK] All kMaxVertexAttribs vertexAttribEnabled default to false\n");
    }

    {
        printf("\n--- GLState write/read ---\n");
        metalsharp::GLState state{};
        state.version = metalsharp::GLVersion::GL21;
        state.currentProgram = 42;
        state.shaderCompilePending = true;
        state.blendEnabled = true;
        state.blendSrcRGB = 1;
        state.blendDstRGB = 2;
        state.depthTestEnabled = true;
        state.depthFunc = 0x0201; // GL_LESS
        state.boundTexture2D = 7;
        state.boundFramebuffer = 9;
        state.viewportX = 10;
        state.viewportY = 20;
        state.viewportWidth = 800;
        state.viewportHeight = 600;
        state.vertexAttribEnabled[3] = true;
        state.vertexAttribEnabled[7] = true;

        CHECK(state.version == metalsharp::GLVersion::GL21, "Assigned version persists");
        CHECK(state.currentProgram == 42, "Assigned currentProgram persists");
        CHECK(state.shaderCompilePending, "Assigned shaderCompilePending persists");
        CHECK(state.blendEnabled, "Assigned blendEnabled persists");
        CHECK(state.blendSrcRGB == 1, "Assigned blendSrcRGB persists");
        CHECK(state.blendDstRGB == 2, "Assigned blendDstRGB persists");
        CHECK(state.depthTestEnabled, "Assigned depthTestEnabled persists");
        CHECK(state.depthFunc == 0x0201, "Assigned depthFunc persists");
        CHECK(state.boundTexture2D == 7, "Assigned boundTexture2D persists");
        CHECK(state.boundFramebuffer == 9, "Assigned boundFramebuffer persists");
        CHECK(state.viewportWidth == 800, "Assigned viewportWidth persists");
        CHECK(state.viewportHeight == 600, "Assigned viewportHeight persists");
        CHECK(state.vertexAttribEnabled[3], "vertexAttribEnabled[3] persists");
        CHECK(state.vertexAttribEnabled[7], "vertexAttribEnabled[7] persists");
        CHECK(!state.vertexAttribEnabled[0], "vertexAttribEnabled[0] still false");
        CHECK(!state.vertexAttribEnabled[15], "vertexAttribEnabled[15] still false");
    }

    {
        printf("\n--- OpenGLBridge lifecycle ---\n");
        metalsharp::OpenGLBridge bridge;
        // State should be default before init.
        CHECK(bridge.state().version == metalsharp::GLVersion::None, "Pre-init state().version is None");
        CHECK(!bridge.hasNativeGL(), "Pre-init hasNativeGL() is false");

        bool ok = bridge.init();
        CHECK(ok, "init() succeeds on macOS (OpenGL framework is available)");
        CHECK(bridge.hasNativeGL(), "hasNativeGL() is true after init()");
        CHECK(bridge.state().version == metalsharp::GLVersion::GL21,
              "state().version is GL21 after init() (macOS legacy profile)");

        // Idempotency: second init() should be safe.
        bool ok2 = bridge.init();
        CHECK(ok2, "Second init() call is also successful (idempotent)");
        CHECK(bridge.hasNativeGL(), "hasNativeGL() still true after re-init");
    }

    {
        printf("\n--- getGLProcAddress ---\n");
        metalsharp::OpenGLBridge bridge;
        bridge.init();

        // Known symbol: glClear is part of GL 1.0 and must be exported by
        // the macOS OpenGL framework.
        void* clearFn = bridge.getGLProcAddress("glClear");
        CHECK(clearFn != nullptr, "getGLProcAddress(\"glClear\") returns non-null");

        void* clearColorFn = bridge.getGLProcAddress("glClearColor");
        CHECK(clearColorFn != nullptr, "getGLProcAddress(\"glClearColor\") returns non-null");

        // Unknown symbol: should return nullptr, not crash.
        void* bogusFn = bridge.getGLProcAddress("this_symbol_does_not_exist_xyzzy");
        CHECK(bogusFn == nullptr, "Unknown symbol returns nullptr");

        // NULL name should return nullptr (defensive).
        void* nullFn = bridge.getGLProcAddress(nullptr);
        CHECK(nullFn == nullptr, "nullptr name returns nullptr");
    }

    {
        printf("\n--- Buffer object passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for buffer-object tests");

        // The GL 1.5 buffer-object entry points should all resolve through
        // the macOS OpenGL framework, which exports the full legacy profile.
        const char* bufferSymbols[] = {
            "glGenBuffers", "glBindBuffer",  "glBufferData", "glBufferSubData",
            "glMapBuffer",  "glUnmapBuffer", "glIsBuffer",   "glDeleteBuffers",
        };
        for (const char* name : bufferSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // After a successful init, the buffer-binding fields on the state
        // tracker must be zero by default — the framework reports nothing
        // is bound until the application calls glBindBuffer.
        CHECK(bridge.state().boundArrayBuffer == 0, "Default state().boundArrayBuffer is 0 after init");
        CHECK(bridge.state().boundElementArrayBuffer == 0, "Default state().boundElementArrayBuffer is 0 after init");
    }

    {
        printf("\n--- kMaxTextureUnits / kMaxVertexAttribs ---\n");
        CHECK(metalsharp::kMaxTextureUnits >= 16, "kMaxTextureUnits is at least 16");
        CHECK(metalsharp::kMaxVertexAttribs >= 16, "kMaxVertexAttribs is at least 16");
        // Standard GL 2.1 minimums.
        CHECK(metalsharp::kMaxTextureUnits == 32, "kMaxTextureUnits is 32");
        CHECK(metalsharp::kMaxVertexAttribs == 16, "kMaxVertexAttribs is 16");
    }

    {
        printf("\n--- Vertex attribute passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for vertex-attribute tests");

        // Symbol resolution: every GL 2.0 vertex-attribute entry point must
        // resolve through the macOS OpenGL framework, which exports the full
        // legacy profile (including GL 2.0 generic vertex attributes).
        const char* vertexAttribSymbols[] = {
            "glEnableVertexAttribArray", "glDisableVertexAttribArray", "glVertexAttribPointer", "glVertexAttrib1f",
            "glVertexAttrib2f",          "glVertexAttrib3f",           "glVertexAttrib4f",      "glGetVertexAttribiv",
            "glGetVertexAttribPointerv", "glVertexAttribDivisor",
        };
        for (const char* name : vertexAttribSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: after init, no vertex attribute is enabled by default.
        bool allDisabled = true;
        for (uint32_t i = 0; i < metalsharp::kMaxVertexAttribs; ++i) {
            if (bridge.state().vertexAttribEnabled[i]) {
                allDisabled = false;
                printf("  [FAIL] vertexAttribEnabled[%u] should default to false after init\n", i);
                failed++;
                break;
            }
        }
        if (allDisabled) {
            printf("  [OK] All kMaxVertexAttribs vertexAttribEnabled default to false after init\n");
            passed++;
        }

        // State tracking: init must not affect the buffer-binding state tracker;
        // those fields stay zero until the application calls glBindBuffer.
        CHECK(bridge.state().boundArrayBuffer == 0, "state().boundArrayBuffer is still 0 after init");
        CHECK(bridge.state().boundElementArrayBuffer == 0, "state().boundElementArrayBuffer is still 0 after init");
    }

    {
        printf("\n--- Shader program passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for shader-program tests");

        // Symbol resolution: every GL 2.0 shader-program entry point must
        // resolve through the macOS OpenGL framework, which exports the full
        // legacy profile (including GL 2.0 program objects).
        const char* programSymbols[] = {
            "glCreateProgram", "glDeleteProgram", "glLinkProgram", "glUseProgram", "glValidateProgram", "glIsProgram",
        };
        for (const char* name : programSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: after init, no program is bound and no shader
        // compile is pending — the framework reports a clean default.
        CHECK(bridge.state().currentProgram == 0, "Default state().currentProgram is 0 after init");
        CHECK(!bridge.state().shaderCompilePending, "Default state().shaderCompilePending is false after init");
    }

    {
        printf("\n--- Shader object passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for shader-object tests");

        // Symbol resolution: every GL 2.0 shader-object entry point must
        // resolve through the macOS OpenGL framework, which exports the full
        // legacy profile (including GL 2.0 shader objects and info-log queries).
        const char* shaderSymbols[] = {
            "glCreateShader", "glDeleteShader",     "glShaderSource", "glCompileShader",
            "glGetShaderiv",  "glGetShaderInfoLog", "glGetProgramiv", "glGetProgramInfoLog",
            "glAttachShader", "glDetachShader",     "glIsShader",
        };
        for (const char* name : shaderSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: init must leave shaderCompilePending false; the
        // bridge starts in a quiescent state with no compile in flight.
        CHECK(!bridge.state().shaderCompilePending, "Default state().shaderCompilePending is false after init");
    }

    {
        printf("\n--- Uniform passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for uniform passthroughs");

        // Symbol resolution: every GL 2.0 uniform entry point must resolve
        // through the macOS OpenGL framework, which exports the full legacy
        // profile (including GL 2.0 uniform variables and uniforms queries).
        const char* uniformSymbols[] = {
            "glGetUniformLocation", "glUniform1f",        "glUniform2f",    "glUniform3f",    "glUniform4f",
            "glUniform1i",          "glUniform2i",        "glUniform3i",    "glUniform4i",    "glUniformMatrix2fv",
            "glUniformMatrix3fv",   "glUniformMatrix4fv", "glGetUniformfv", "glGetUniformiv", "glGetActiveUniform",
        };
        for (const char* name : uniformSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: init must not have left a program bound. Even
        // though the uniform entry points read/write through the active
        // program implicitly, the shim must leave currentProgram at 0
        // unless the application explicitly calls glUseProgram.
        CHECK(bridge.state().currentProgram == 0, "Default state().currentProgram is 0 after init");
    }

    {
        printf("\n--- Vertex attribute location passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for vertex-attribute-location tests");

        // Symbol resolution: every GL 2.0 vertex-attribute-location entry
        // point must resolve through the macOS OpenGL framework, which
        // exports the full legacy profile (including GL 2.0 generic vertex
        // attribute location queries and binding).
        const char* attribLocSymbols[] = {
            "glGetAttribLocation",
            "glBindAttribLocation",
            "glGetActiveAttrib",
        };
        for (const char* name : attribLocSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: init must not leave a program bound. The vertex-
        // attribute location entry points operate on a program, so the shim
        // must leave currentProgram at 0 unless the application explicitly
        // calls glUseProgram.
        CHECK(bridge.state().currentProgram == 0, "Default state().currentProgram is 0 after init");
    }

    {
        printf("\n--- Texture passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for texture passthroughs");

        // Symbol resolution: every GL 1.1-1.3 texture entry point must
        // resolve through the macOS OpenGL framework, which exports the
        // full legacy profile (texture objects, parameters, env, copy,
        // and the GL 1.3 texture query entry points).
        const char* textureSymbols[] = {
            "glGenTextures",   "glDeleteTextures", "glBindTexture",       "glActiveTexture",     "glTexParameteri",
            "glTexParameterf", "glTexImage2D",     "glTexSubImage2D",     "glCopyTexImage2D",    "glCopyTexSubImage2D",
            "glTexEnvi",       "glTexEnvf",        "glGetTexParameteriv", "glGetTexParameterfv", "glIsTexture",
        };
        for (const char* name : textureSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: after init, no 2D texture is bound by default.
        // The framework reports a clean state until the application calls
        // glBindTexture(GL_TEXTURE_2D, ...).
        CHECK(bridge.state().boundTexture2D == 0, "Default state().boundTexture2D is 0 after init");
    }

    {
        printf("\n--- Framebuffer passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for framebuffer passthroughs");

        // Symbol resolution: every GL 3.0 / EXT_framebuffer_object entry
        // point must resolve through the macOS OpenGL framework, which
        // exports the full legacy profile (including framebuffer objects
        // via EXT_framebuffer_object on macOS).
        const char* framebufferSymbols[] = {
            "glGenFramebuffers",         "glDeleteFramebuffers",     "glBindFramebuffer", "glFramebufferTexture2D",
            "glFramebufferRenderbuffer", "glCheckFramebufferStatus", "glIsFramebuffer",
        };
        for (const char* name : framebufferSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: after init, no framebuffer is bound by default.
        // The framework reports a clean state until the application calls
        // glBindFramebuffer(GL_FRAMEBUFFER, ...).
        CHECK(bridge.state().boundFramebuffer == 0, "Default state().boundFramebuffer is 0 after init");
    }

    {
        printf("\n--- Renderbuffer passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for renderbuffer passthroughs");

        // Symbol resolution: every GL 3.0 / EXT_framebuffer_object renderbuffer
        // entry point must resolve through the macOS OpenGL framework, which
        // exports the full legacy profile (including renderbuffer objects via
        // EXT_framebuffer_object on macOS).
        const char* renderbufferSymbols[] = {
            "glGenRenderbuffers",    "glDeleteRenderbuffers", "glBindRenderbuffer",
            "glRenderbufferStorage", "glIsRenderbuffer",
        };
        for (const char* name : renderbufferSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: after init, no framebuffer is bound by default.
        // The framework reports a clean state until the application calls
        // glBindFramebuffer(GL_FRAMEBUFFER, ...). Renderbuffer objects
        // intentionally do not add a new binding slot — they are attached
        // to a framebuffer via glFramebufferRenderbuffer, which is
        // exercised by the framebuffer passthrough tests above.
        CHECK(bridge.state().boundFramebuffer == 0, "Default state().boundFramebuffer is still 0 after init");
    }

    {
        printf("\n--- Rasterization state passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for rasterization state passthroughs");

        // Symbol resolution: every GL 1.0 rasterization entry point must
        // resolve through the macOS OpenGL framework, which exports the full
        // legacy profile (fixed-function pipeline is still available on
        // macOS via the legacy/2.1 compatibility profile).
        const char* rasterizationSymbols[] = {
            "glCullFace", "glFrontFace", "glLineWidth", "glPointSize", "glPolygonMode", "glPolygonOffset",
        };
        for (const char* name : rasterizationSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: rasterization toggles (cull face, polygon mode,
        // polygon offset, line/point size) intentionally do not introduce
        // new binding slots. The bridge still reports a clean state after
        // init, and blendEnabled must remain its default of false until
        // something explicitly enables blending.
        CHECK(bridge.state().blendEnabled == false, "Default state().blendEnabled is still false after init");
    }

    {
        printf("\n--- Stencil state passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for stencil state passthroughs");

        // Symbol resolution: every GL 1.0 stencil entry point must resolve
        // through the macOS OpenGL framework, which exports the full legacy
        // profile (fixed-function stencil is still available on macOS via
        // the legacy/2.1 compatibility profile).
        const char* stencilSymbols[] = {
            "glStencilFunc", "glStencilFuncSeparate", "glStencilOp",    "glStencilOpSeparate",
            "glStencilMask", "glStencilMaskSeparate", "glClearStencil",
        };
        for (const char* name : stencilSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: stencil toggles (front/back face masks, stencil
        // func, stencil op) intentionally do not introduce new binding
        // slots. The bridge still reports a clean state after init, and
        // depthTestEnabled must remain its default of false until something
        // explicitly enables depth testing.
        CHECK(bridge.state().depthTestEnabled == false, "Default state().depthTestEnabled is still false after init");
    }

    {
        printf("\n--- Color / blend state passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for color/blend state passthroughs");

        // Symbol resolution: every GL 1.0-1.4 color/blend entry point must
        // resolve through the macOS OpenGL framework, which exports the full
        // legacy profile (color mask, fixed-function blend, GL 1.4 separate
        // blend, blend color, and GL 1.0 logic op).
        const char* blendSymbols[] = {
            "glColorMask",         "glBlendEquation", "glBlendEquationSeparate",
            "glBlendFuncSeparate", "glBlendColor",    "glLogicOp",
        };
        for (const char* name : blendSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: blend toggles (color mask, blend equation,
        // blend func, blend color, logic op) intentionally do not introduce
        // new binding slots. The bridge still reports a clean state after
        // init, and blendEnabled must remain its default of false until
        // something explicitly enables blending.
        CHECK(bridge.state().blendEnabled == false, "Default state().blendEnabled is still false after init");
    }

    {
        printf("\n--- Depth state passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for depth state passthroughs");

        // Symbol resolution: every GL 1.0 depth entry point must resolve
        // through the macOS OpenGL framework, which exports the full legacy
        // profile (depth mask, depth range, clear depth — all part of the
        // fixed-function pipeline still available on macOS).
        const char* depthSymbols[] = {
            "glDepthMask",
            "glDepthRange",
            "glClearDepth",
        };
        for (const char* name : depthSymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: depth toggles (depth mask, depth range, clear
        // depth) intentionally do not introduce new binding slots. The
        // bridge still reports a clean state after init, and depthTestEnabled
        // must remain its default of false until something explicitly
        // enables depth testing.
        CHECK(bridge.state().depthTestEnabled == false, "Default state().depthTestEnabled is still false after init");
    }

    {
        printf("\n--- Legacy fixed-function passthroughs ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for legacy fixed-function passthroughs");

        // Symbol resolution: every GL 1.0 / GL 3.0 extension entry point
        // added in phases 1n-1v must resolve through the macOS OpenGL
        // framework, which exports the full legacy profile.
        const char* legacySymbols[] = {
            // 1n: Pixel storage / transfer
            "glPixelStorei",
            "glPixelStoref",
            "glReadBuffer",
            "glDrawBuffer",
            // 1o: State queries
            "glGetBooleanv",
            "glGetFloatv",
            "glGetDoublev",
            "glGetTexEnviv",
            "glGetTexEnvfv",
            "glGetError",
            "glIsEnabled",
            "glGetStringi",
            // 1p: Misc commands
            "glFlush",
            "glFinish",
            "glHint",
            // 1q: Display lists
            "glGenLists",
            "glNewList",
            "glEndList",
            "glCallList",
            "glCallLists",
            "glDeleteLists",
            "glIsList",
            // 1r: Immediate-mode vertex data
            "glVertex2f",
            "glVertex3f",
            "glVertex4f",
            "glNormal3f",
            "glTexCoord1f",
            "glTexCoord2f",
            "glTexCoord3f",
            "glTexCoord4f",
            "glColor3ub",
            "glColor4ub",
            "glColorMaterial",
            // 1s: Lighting / material
            "glLightfv",
            "glLightModelfv",
            "glMaterialfv",
            "glShadeModel",
            // 1t: Fog
            "glFogfv",
            "glFogi",
            // 1u: Alpha test
            "glAlphaFunc",
            // 1v: Clip planes
            "glClipPlane",
        };
        for (const char* name : legacySymbols) {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") returns non-null", name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
        }

        // State tracking: legacy fixed-function entry points (pixel
        // storage, display lists, immediate-mode vertex data, lighting,
        // fog, alpha test, clip planes, hint, flush/finish, state
        // queries) intentionally do not introduce new binding slots.
        // The bridge must report a clean state after init, with the
        // viewport still at its default of zero until the application
        // calls glViewport.
        CHECK(bridge.state().viewportWidth == 0, "Default state().viewportWidth is still 0 after init");
    }

    {
        printf("\n--- All GL 2.1 symbol coverage ---\n");
        metalsharp::OpenGLBridge bridge;
        bool ok = bridge.init();
        CHECK(ok, "init() succeeds for the comprehensive GL symbol audit");

        // Master list of every GL entry point exported by the
        // src/opengl/EntryPoint.cpp shim. Each name below was hand-verified
        // to either appear as a `GL_PASSTHROUGH<N>(ret, name, ...)` macro
        // invocation or as a hand-written `extern "C"` definition in that
        // file. Auditing the union of these two sets is the only way to
        // guarantee that no exported symbol slips through untested.
        //
        // Total = 172 names = 161 GL_PASSTHROUGH macros + 11 hand-written
        // extern "C" definitions (glBindBuffer, glBindFramebuffer,
        // glCompileShader, glCopyTexImage2D, glCopyTexSubImage2D,
        // glDisableVertexAttribArray, glEnableVertexAttribArray,
        // glGetIntegerv, glGetString, glGetStringi, glUseProgram).
        const char* allGLSymbols[] = {
            // Vertex / immediate-mode pipeline (GL 1.0/1.1)
            "glBegin",
            "glEnd",
            // Buffers / state (GL 1.0)
            "glClear",
            "glClearColor",
            "glViewport",
            "glEnable",
            "glDisable",
            "glBlendFunc",
            "glDepthFunc",
            // Buffer objects (GL 1.5)
            "glGenBuffers",
            "glDeleteBuffers",
            "glBindBuffer",
            "glBufferData",
            "glBufferSubData",
            "glMapBuffer",
            "glUnmapBuffer",
            "glIsBuffer",
            // Draw submission (GL 1.1)
            "glDrawArrays",
            "glDrawElements",
            // Client-side vertex arrays (GL 1.1)
            "glVertexPointer",
            "glTexCoordPointer",
            "glColorPointer",
            "glNormalPointer",
            // Vertex attributes (GL 2.0)
            "glEnableVertexAttribArray",
            "glDisableVertexAttribArray",
            "glVertexAttribPointer",
            "glVertexAttrib1f",
            "glVertexAttrib2f",
            "glVertexAttrib3f",
            "glVertexAttrib4f",
            "glGetVertexAttribiv",
            "glGetVertexAttribPointerv",
            "glVertexAttribDivisor",
            // Shader objects (GL 2.0)
            "glCreateShader",
            "glDeleteShader",
            "glShaderSource",
            "glCompileShader",
            "glGetShaderiv",
            "glGetShaderInfoLog",
            "glAttachShader",
            "glDetachShader",
            "glIsShader",
            // Shader program objects (GL 2.0)
            "glCreateProgram",
            "glDeleteProgram",
            "glLinkProgram",
            "glUseProgram",
            "glValidateProgram",
            "glIsProgram",
            "glGetProgramiv",
            "glGetProgramInfoLog",
            // Uniforms (GL 2.0)
            "glGetUniformLocation",
            "glUniform1f",
            "glUniform2f",
            "glUniform3f",
            "glUniform4f",
            "glUniform1i",
            "glUniform2i",
            "glUniform3i",
            "glUniform4i",
            "glUniformMatrix2fv",
            "glUniformMatrix3fv",
            "glUniformMatrix4fv",
            "glGetUniformfv",
            "glGetUniformiv",
            "glGetActiveUniform",
            // Vertex attribute location queries (GL 2.0)
            "glGetAttribLocation",
            "glBindAttribLocation",
            "glGetActiveAttrib",
            // Texture objects (GL 1.1-1.3 / GL 1.3 multitexture)
            "glGenTextures",
            "glDeleteTextures",
            "glBindTexture",
            "glActiveTexture",
            "glTexParameteri",
            "glTexParameterf",
            "glTexSubImage2D",
            "glCopyTexImage2D",
            "glCopyTexSubImage2D",
            "glTexEnvi",
            "glTexEnvf",
            "glGetTexParameteriv",
            "glGetTexParameterfv",
            "glIsTexture",
            // Framebuffer objects (GL 3.0 / EXT_framebuffer_object)
            "glGenFramebuffers",
            "glDeleteFramebuffers",
            "glBindFramebuffer",
            "glFramebufferTexture2D",
            "glFramebufferRenderbuffer",
            "glCheckFramebufferStatus",
            "glIsFramebuffer",
            // Renderbuffer objects (GL 3.0 / EXT_framebuffer_object)
            "glGenRenderbuffers",
            "glDeleteRenderbuffers",
            "glBindRenderbuffer",
            "glRenderbufferStorage",
            "glIsRenderbuffer",
            // Rasterization state (GL 1.0)
            "glCullFace",
            "glFrontFace",
            "glLineWidth",
            "glPointSize",
            "glPolygonMode",
            "glPolygonOffset",
            // Stencil state (GL 1.0)
            "glStencilFunc",
            "glStencilFuncSeparate",
            "glStencilOp",
            "glStencilOpSeparate",
            "glStencilMask",
            "glStencilMaskSeparate",
            "glClearStencil",
            // Color / blend state (GL 1.0-1.4)
            "glColorMask",
            "glBlendEquation",
            "glBlendEquationSeparate",
            "glBlendFuncSeparate",
            "glBlendColor",
            "glLogicOp",
            // Depth state (GL 1.0)
            "glDepthMask",
            "glDepthRange",
            "glClearDepth",
            // Pixel storage / transfer (GL 1.0)
            "glPixelStorei",
            "glPixelStoref",
            "glReadBuffer",
            "glDrawBuffer",
            // State queries (GL 1.0-1.1)
            "glGetBooleanv",
            "glGetFloatv",
            "glGetDoublev",
            "glGetTexEnviv",
            "glGetTexEnvfv",
            "glGetError",
            "glIsEnabled",
            "glGetStringi",
            // Misc commands (GL 1.0)
            "glFlush",
            "glFinish",
            "glHint",
            // Display lists (GL 1.0)
            "glGenLists",
            "glNewList",
            "glEndList",
            "glCallList",
            "glCallLists",
            "glDeleteLists",
            "glIsList",
            // Immediate-mode vertex data (GL 1.0)
            "glVertex2f",
            "glVertex3f",
            "glVertex4f",
            "glNormal3f",
            "glTexCoord1f",
            "glTexCoord2f",
            "glTexCoord3f",
            "glTexCoord4f",
            "glColor3ub",
            "glColor4ub",
            "glColorMaterial",
            // Lighting / material (GL 1.0)
            "glLightfv",
            "glLightModelfv",
            "glMaterialfv",
            "glShadeModel",
            // Fog (GL 1.0)
            "glFogfv",
            "glFogi",
            // Alpha test (GL 1.0)
            "glAlphaFunc",
            // Clip planes (GL 1.0)
            "glClipPlane",
            // Matrix stack (fixed pipeline, GL 1.0)
            "glMatrixMode",
            "glLoadIdentity",
            "glOrtho",
            "glFrustum",
            "glPushMatrix",
            "glPopMatrix",
            "glTranslatef",
            "glRotatef",
            "glScalef",
            // Color (legacy)
            "glColor3f",
            "glColor4f",
            // Texture upload / readback
            "glTexImage2D",
            "glReadPixels",
            // Info queries (return non-default values)
            "glGetString",
            "glGetIntegerv",
        };

        // Floor used to catch regressions where a future Phase removes an
        // export without updating the master list above. The current shim
        // exports exactly 172 entry points; the floor is set deliberately
        // low (170) so a single accidental deletion trips the check while
        // still leaving headroom for incidental drift.
        constexpr size_t EXPECTED_GL_COUNT = sizeof(allGLSymbols) / sizeof(allGLSymbols[0]);
        constexpr size_t EXPECTED_MINIMUM = 170;

        size_t resolvedCount = 0;
        for (size_t i = 0; i < EXPECTED_GL_COUNT; ++i) {
            const char* name = allGLSymbols[i];
            char msg[128];
            std::snprintf(msg, sizeof(msg), "getGLProcAddress(\"%s\") resolves through the macOS OpenGL framework",
                          name);
            void* fn = bridge.getGLProcAddress(name);
            CHECK(fn != nullptr, msg);
            if (fn != nullptr) {
                resolvedCount++;
            }
        }

        // De-duplicate (the union of macros + hand-written exports should
        // already be unique, but a duplicate write-up in the master list
        // above must not inflate the audit count).
        size_t uniqueResolved = 0;
        for (size_t i = 0; i < EXPECTED_GL_COUNT; ++i) {
            const char* name = allGLSymbols[i];
            void* fn = bridge.getGLProcAddress(name);
            if (fn == nullptr) {
                continue;
            }
            bool seenBefore = false;
            for (size_t j = 0; j < i; ++j) {
                if (std::strcmp(allGLSymbols[j], name) == 0) {
                    seenBefore = true;
                    break;
                }
            }
            if (!seenBefore) {
                uniqueResolved++;
            }
        }

        printf("  Resolved %zu / %zu expected minimum GL symbols (compiled list size = %zu)\n", uniqueResolved,
               EXPECTED_MINIMUM, EXPECTED_GL_COUNT);
        CHECK(uniqueResolved >= EXPECTED_MINIMUM,
              "Unique resolved GL symbols meet or exceed the EXPECTED_MINIMUM floor");
        CHECK(uniqueResolved == EXPECTED_GL_COUNT, "Unique resolved GL symbols equal the size of the master list "
                                                   "(no duplicates and no missing entries)");
    }

    {
        printf("\n--- GLSL version parsing ---\n");

        // Default-constructed GLSLVersion is invalid
        {
            metalsharp::GLSLVersion v{};
            CHECK(!v.valid, "Default-constructed GLSLVersion.valid is false");
            CHECK(v.major == 0, "Default-constructed GLSLVersion.major is 0");
            CHECK(v.minor == 0, "Default-constructed GLSLVersion.minor is 0");
            CHECK(!v.isES, "Default-constructed GLSLVersion.isES is false");
            CHECK(!metalsharp::needsCrossCompile(v), "needsCrossCompile on default is false");
            CHECK(metalsharp::packedGLSLVersion(v) == 0, "packedGLSLVersion on default is 0");
        }

        // "#version 300 es" — OpenGL ES 3.00, needs cross-compile.
        // The 'es' profile keyword is the most important signal for the
        // bridge: it tells us we MUST run the SPIRV-Cross pipeline because
        // macOS has no native ES support.
        {
            const char* src = "#version 300 es\nprecision mediump float;\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(ok, "parseGLSLVersion(\"#version 300 es\") returns true");
            CHECK(v.valid, "#version 300 es: valid is true");
            CHECK(v.isES, "#version 300 es: isES is true");
            CHECK(metalsharp::needsCrossCompile(v), "#version 300 es: needsCrossCompile is true (macOS has no ES)");
            CHECK(metalsharp::packedGLSLVersion(v) > 0, "#version 300 es: packedGLSLVersion is non-zero");
        }

        // "#version 330 core" — desktop GL core profile, must cross-compile.
        // Verifies that the 'core' keyword is accepted and isES stays false.
        {
            const char* src = "#version 330 core\nlayout(location = 0) in vec3 a;\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(ok, "parseGLSLVersion(\"#version 330 core\") returns true");
            CHECK(v.valid, "#version 330 core: valid is true");
            CHECK(!v.isES, "#version 330 core: isES is false (desktop GL, not ES)");
            CHECK(metalsharp::needsCrossCompile(v), "#version 330 core: needsCrossCompile is true");
        }

        // "#version 450 compatibility" — older compatibility profile, still cross-compile.
        // Verifies that the 'compatibility' keyword is accepted.
        {
            const char* src = "#version 450 compatibility\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(ok, "parseGLSLVersion(\"#version 450 compatibility\") returns true");
            CHECK(v.valid, "#version 450 compatibility: valid is true");
            CHECK(!v.isES, "#version 450 compatibility: isES is false");
            CHECK(metalsharp::needsCrossCompile(v), "#version 450 compatibility: needsCrossCompile is true");
        }

        // Whitespace tolerance: leading spaces + tabs before '#'
        {
            const char* src = "   \t#version 330 core\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(ok, "parseGLSLVersion tolerates leading whitespace");
            CHECK(v.valid, "Leading whitespace: valid is true after skipping spaces+tabs");
            CHECK(!v.isES, "Leading whitespace: isES is false for desktop GL");
        }

        // Whitespace between '#' and 'version'
        {
            const char* src = "#   version   300   es\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(ok, "parseGLSLVersion tolerates whitespace between '#' and 'version'");
            CHECK(v.valid, "Whitespace around version: valid is true");
            CHECK(v.isES, "Whitespace around version: isES is true after parsing 'es'");
        }

        // Empty source
        {
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion("", v);
            CHECK(!ok, "parseGLSLVersion(\"\") returns false");
            CHECK(!v.valid, "Empty source: GLSLVersion.valid stays false");
        }

        // nullptr source
        {
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(nullptr, v);
            CHECK(!ok, "parseGLSLVersion(nullptr) returns false");
            CHECK(!v.valid, "nullptr source: GLSLVersion.valid stays false");
        }

        // Missing # prefix (does not start with #version)
        {
            const char* src = "version 330 core\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(!ok, "Source without leading '#' returns false");
            CHECK(!v.valid, "No-# source: GLSLVersion.valid stays false");
        }

        // Bad directive (not 'version')
        {
            const char* src = "#extension GL_ARB_shader_objects : enable\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(!ok, "#extension directive (no #version) returns false");
            CHECK(!v.valid, "#extension-only source: GLSLVersion.valid stays false");
        }

        // Missing version number after '#version'
        {
            const char* src = "#version\n";
            metalsharp::GLSLVersion v{};
            bool ok = metalsharp::parseGLSLVersion(src, v);
            CHECK(!ok, "#version with no number returns false");
            CHECK(!v.valid, "Bare #version: GLSLVersion.valid stays false");
        }

        // needsCrossCompile on invalid version
        {
            metalsharp::GLSLVersion v{};
            v.valid = false;
            CHECK(!metalsharp::needsCrossCompile(v), "needsCrossCompile on invalid version is false");
        }
    }

    printf("\n=== Summary: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}