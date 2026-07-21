/// @file test_metal_renderer.mm
/// @brief Tests for the GL→Metal draw emitter (Phases 3a-3l).
///
/// Phase 3a/3b/3c exercises the basic renderer surface — device init,
/// buffer handle allocation, render pass begin/end. Phase 3d-3k exercise
/// the extended surface (vertex layout, uniform buffers, texture
/// creation, viewport/scissor, finish). The final "full pipeline" test
/// drives the GLSL→SPIRV-Cross→MSL translate path via GLSLCompiler and
/// feeds the resulting MSL into renderer.createPipeline().

#include <cstdio>
#import <Metal/Metal.h>
#include <metalsharp/GLMetalRenderer.h>
#include <metalsharp/GLShaderTracker.h>
#include <metalsharp/GLSLCompiler.h>
#include <metalsharp/GLSLVersion.h>
#include <metalsharp/OpenGLBridge.h>
#include <metalsharp/ShaderStage.h>

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
    printf("=== GLMetalRenderer Tests ===\n\n");

    // Initialization once for everything below.
    metalsharp::GLSLCompiler::initialize();

    metalsharp::GLMetalRenderer renderer;
    const bool initOk = renderer.init();
    CHECK(initOk, "init() succeeded");
    CHECK(renderer.isAvailable(), "isAvailable() true");

    if (!initOk) {
        printf("\n=== Summary: %d passed, %d failed ===\n", passed, failed);
        return failed ? 1 : 0;
    }

    // ---- Buffer / render pass (Phase 3a-3c baseline) ------------------------
    {
        printf("--- Buffer / render pass ---\n");
        float vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        uint64_t buf = renderer.createBuffer(vertices, sizeof(vertices));
        char bufMsg[64];
        std::snprintf(bufMsg, sizeof(bufMsg), "createBuffer returned handle %llu", buf);
        CHECK(buf > 0, bufMsg);

        auto& tracker = metalsharp::GLShaderTracker::instance();
        uint32_t vs = tracker.createShader(0x8B31); // GL_VERTEX_SHADER
        uint32_t fs = tracker.createShader(0x8B30); // GL_FRAGMENT_SHADER
        CHECK(vs > 0 && fs > 0, "createShader handles valid");

        renderer.beginRenderPass(256, 256);
        renderer.endRenderPass();
        CHECK(true, "beginRenderPass/endRenderPass no crash");
    }

    // ---- Phase 3h: Viewport / Scissor ---------------------------------------
    {
        printf("\n--- Viewport/Scissor ---\n");
        renderer.beginRenderPass(256, 256);
        renderer.setViewport(0, 0, 256, 256);
        renderer.setScissor(0, 0, 256, 256);
        renderer.endRenderPass();
        CHECK(true, "setViewport/setScissor inside render pass no crash");
    }

    // ---- Phase 3e: Uniform buffer -------------------------------------------
    {
        printf("\n--- Uniform buffer ---\n");
        renderer.beginRenderPass(128, 128);
        struct Uniforms {
            float color[4];
            float mvp[16];
        } u{};
        for (int i = 0; i < 4; ++i)
            u.color[i] = 1.0f;
        for (int i = 0; i < 16; ++i)
            u.mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        renderer.updateUniformBuffer(0, &u, sizeof(u));
        // Update the same binding again — exercises the find-and-reuse path.
        renderer.updateUniformBuffer(0, &u, sizeof(u));
        // Different binding slot
        renderer.updateUniformBuffer(1, &u, sizeof(u) / 2);
        renderer.endRenderPass();
        CHECK(true, "updateUniformBuffer did not crash (3 updates across 2 bindings)");
    }

    // ---- Phase 3f: Texture create -------------------------------------------
    {
        printf("\n--- Texture create ---\n");
        // 64x64 BGRA8 gradient. Using raw bytes gives deterministic content.
        const uint32_t kWidth = 64;
        const uint32_t kHeight = 64;
        std::vector<uint8_t> pixels;
        pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                const size_t idx = (static_cast<size_t>(y) * kWidth + x) * 4;
                pixels[idx + 0] = static_cast<uint8_t>((x * 4) & 0xFF); // B
                pixels[idx + 1] = static_cast<uint8_t>((y * 4) & 0xFF); // G
                pixels[idx + 2] = static_cast<uint8_t>(0);              // R
                pixels[idx + 3] = 255;                                  // A
            }
        }
        const uint64_t tex = renderer.createTexture(kWidth, kHeight, pixels.data());
        char texMsg[80];
        std::snprintf(texMsg, sizeof(texMsg), "createTexture(64,64) returned handle %llu", tex);
        CHECK(tex > 0, texMsg);

        // Bind the texture to a slot while inside a render pass.
        renderer.beginRenderPass(128, 128);
        renderer.bindTexture(tex, 0);
        renderer.bindTexture(tex, 1);
        renderer.endRenderPass();
        CHECK(true, "bindTexture no crash");

        // Unknown handle — must be a no-op, not a crash.
        renderer.beginRenderPass(64, 64);
        renderer.bindTexture(0xDEADBEEFCAFEBABEull, 2);
        renderer.endRenderPass();
        CHECK(true, "bindTexture with unknown handle no crash");
    }

    // ---- Phase 3k: Synchronization ------------------------------------------
    {
        printf("\n--- Finish ---\n");
        metalsharp::GLMetalRenderer r2;
        const bool r2ok = r2.init();
        CHECK(r2ok, "second renderer init for finish test");
        if (r2ok) {
            r2.beginRenderPass(64, 64);
            r2.endRenderPass();
            r2.finish();
            CHECK(true, "finish() no crash");
            // finish() with no outstanding work is a no-op (no crash).
            r2.finish();
            CHECK(true, "finish() with no in-flight work no crash");
        }

        // Also exercise flush() before finish(): encode a command buffer,
        // flush (commits, no wait), then finish (no-op since flush cleared).
        metalsharp::GLMetalRenderer r3;
        const bool r3ok = r3.init();
        CHECK(r3ok, "third renderer init for flush test");
        if (r3ok) {
            r3.beginRenderPass(32, 32);
            r3.endRenderPass();
            r3.flush();
            CHECK(true, "flush() no crash after endRenderPass()");
        }
    }

    // ---- Phase 3l: Full pipeline (vertex + fragment shaders through tracker)
    {
        printf("\n--- Full pipeline test ---\n");
        const char* vsSrc = "#version 450 core\n"
                            "layout(location = 0) in vec2 pos;\n"
                            "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
        const char* fsSrc = "#version 450 core\n"
                            "layout(location = 0) out vec4 color;\n"
                            "void main() { color = vec4(1.0, 0.0, 0.0, 1.0); }\n";

        auto& tracker = metalsharp::GLShaderTracker::instance();
        const uint32_t vs = tracker.createShader(0x8B31); // GL_VERTEX_SHADER
        const uint32_t fs = tracker.createShader(0x8B30); // GL_FRAGMENT_SHADER
        CHECK(vs != 0 && fs != 0, "Created VS and FS handles in tracker");

        metalsharp::GLShaderState* vsState = tracker.getShader(vs);
        metalsharp::GLShaderState* fsState = tracker.getShader(fs);
        if (!vsState || !fsState) {
            CHECK(false, "tracker.getShader returned non-null for both shaders");
        } else {
            vsState->source = vsSrc;
            fsState->source = fsSrc;
            metalsharp::parseGLSLVersion(vsState->source.c_str(), vsState->glslVersion);
            metalsharp::parseGLSLVersion(fsState->source.c_str(), fsState->glslVersion);
            vsState->needsCrossCompile = metalsharp::needsCrossCompile(vsState->glslVersion);
            fsState->needsCrossCompile = metalsharp::needsCrossCompile(fsState->glslVersion);

            std::string err;
            bool vsOk = metalsharp::GLSLCompiler::compileToSPIRV(vsState->source.c_str(), vsState->stage,
                                                                 vsState->glslVersion, vsState->spirv, err);
            if (vsOk) {
                vsOk = metalsharp::GLSLCompiler::translateSPIRVtoMSL(vsState->spirv, vsState->stage, vsState->msl, err);
            }
            vsState->compiled = true;
            vsState->compileSuccess = vsOk;
            vsState->infoLog = err;

            std::string err2;
            bool fsOk = metalsharp::GLSLCompiler::compileToSPIRV(fsState->source.c_str(), fsState->stage,
                                                                 fsState->glslVersion, fsState->spirv, err2);
            if (fsOk) {
                fsOk =
                    metalsharp::GLSLCompiler::translateSPIRVtoMSL(fsState->spirv, fsState->stage, fsState->msl, err2);
            }
            fsState->compiled = true;
            fsState->compileSuccess = fsOk;
            fsState->infoLog = err2;

            CHECK(vsState->compileSuccess, "Vertex shader cross-compiled to MSL");
            CHECK(fsState->compileSuccess, "Fragment shader cross-compiled to MSL");
            CHECK(!vsState->msl.empty(), "Vertex MSL output is non-empty");
            CHECK(!fsState->msl.empty(), "Fragment MSL output is non-empty");
            CHECK(vsState->msl.find("vertex_main") != std::string::npos, "Vertex MSL contains entry-point vertex_main");
            CHECK(fsState->msl.find("fragment_main") != std::string::npos,
                  "Fragment MSL contains entry-point fragment_main");

            // Hand them off to the renderer.
            metalsharp::GLMetalRenderer pipelineR;
            const bool pipOk = pipelineR.init();
            CHECK(pipOk, "Pipeline renderer init");
            if (pipOk) {
                metalsharp::GLState glState{};
                glState.blendEnabled = false;

                // Vertex shader declares "layout(location = 0) in vec2 pos",
                // so we must register a matching vertex layout before
                // createPipeline() — otherwise Metal rejects the pipeline
                // with "Vertex function has input attributes but no vertex
                // descriptor was set".
                const uint32_t attrOffsets[1] = {0};
                const uint32_t attrFormats[1] = {MTLVertexFormatFloat2};
                pipelineR.setVertexLayout(sizeof(float) * 2, attrOffsets, attrFormats, 1);

                const bool pipeCreated = pipelineR.createPipeline(*vsState, *fsState, glState);
                CHECK(pipeCreated, "createPipeline succeeded for cross-compiled shaders");

                pipelineR.beginRenderPass(128, 128);
                pipelineR.usePipeline();
                pipelineR.endRenderPass();
                pipelineR.finish();
                CHECK(true, "full render pass + usePipeline + finish no crash");
            }
        }
    }

    // ---- Phase 3d: setVertexLayout ------------------------------------------
    {
        printf("\n--- setVertexLayout ---\n");
        metalsharp::GLMetalRenderer vr;
        CHECK(vr.init(), "renderer for vertex-layout test");
        if (vr.isAvailable()) {
            const uint32_t offsets[2] = {0, 8};
            const uint32_t formats[2] = {
                MTLVertexFormatFloat2,
                MTLVertexFormatFloat2,
            };
            vr.setVertexLayout(16, offsets, formats, 2);
            CHECK(true, "setVertexLayout accepted 2 attributes / stride 16");

            // Re-applying with a different layout overwrites the previous one.
            const uint32_t offsetsSingle[1] = {0};
            const uint32_t formatsSingle[1] = {MTLVertexFormatFloat3};
            vr.setVertexLayout(12, offsetsSingle, formatsSingle, 1);
            CHECK(true, "setVertexLayout overwrite with 1 attribute / stride 12");

            // nullptr + count==0 means "clear layout" (stride resets to 0
            // and the createPipeline path skips the vertex-descriptor copy).
            vr.setVertexLayout(0, nullptr, nullptr, 0);
            CHECK(true, "setVertexLayout(nullptr, nullptr, 0) is a safe no-op");
        }
    }

    printf("\n=== Summary: %d passed, %d failed ===\n", passed, failed);

    metalsharp::GLSLCompiler::shutdown();

    return failed ? 1 : 0;
}
