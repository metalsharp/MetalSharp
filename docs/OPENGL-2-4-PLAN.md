# OpenGL 2-4 Bridge: Full Implementation Plan

## Summary

Phase 4b delivered a structural framework — `GLState` tracker, `OpenGLBridge` (dlopen macOS GL), ~35 immediate-mode passthroughs, WGL sentinels, and 54 unit tests. This plan covers what remains to fully support OpenGL 2.1 through 4.6 on macOS via passthrough-to-native-GL (2.1) and SPIRV-Cross → Metal translation (3.x+).

## Current State

| Layer | Status |
|---|---|
| GL 1.x/2.1 immediate-mode passthroughs | 35/200 functions exported |
| GLState tracker | Blend, depth, attribs, viewport, bound objects |
| WGL context | Sentinel handles only (no real NSOpenGLContext) |
| Shader translation | Not started |
| Metal draw emission | Not started |
| Tests | 54 infrastructure-only checks |
| Real game validation | None |

## Phases

### Phase 1: Complete GL 2.1 Coverage

**Goal**: Every GL 2.1 function is exported by `opengl32.dll` shim and forwarded to macOS native GL. No game should crash on a missing symbol.

**Why this matters**: Games that use GL 2.1 fixed-function (common in indie/FNA/Wine titles) will "just work" once all symbols are present. Currently any use of `glGenBuffers`, `glUniform*`, `glUseProgram`, etc. is a null function pointer → segfault.

- [ ] **1a — Buffer objects** (`glGenBuffers`, `glDeleteBuffers`, `glBindBuffer`, `glBufferData`, `glBufferSubData`, `glMapBuffer`, `glUnmapBuffer`, `glIsBuffer`)
- [ ] **1b — Vertex attributes** (`glVertexAttribPointer`, `glEnableVertexAttribArray`, `glDisableVertexAttribArray`, `glVertexAttrib1f/2f/3f/4f`, `glGetVertexAttribiv`, `glGetVertexAttribPointerv`)
- [ ] **1c — Shader program objects** (`glCreateProgram`, `glDeleteProgram`, `glLinkProgram`, `glUseProgram`, `glValidateProgram`, `glIsProgram`)
- [ ] **1d — Shader objects** (`glCreateShader`, `glDeleteShader`, `glShaderSource`, `glCompileShader`, `glGetShaderiv`, `glGetShaderInfoLog`, `glGetProgramiv`, `glGetProgramInfoLog`, `glAttachShader`, `glDetachShader`, `glIsShader`)
- [ ] **1e — Uniforms** (`glGetUniformLocation`, `glUniform1f/2f/3f/4f`, `glUniform1i/2i/3i/4i`, `glUniformMatrix2fv/3fv/4fv`, `glGetActiveUniform`, `glGetUniformfv`, `glGetUniformiv`)
- [ ] **1f — Vertex attributes (location queries)** (`glGetAttribLocation`, `glBindAttribLocation`, `glGetActiveAttrib`)
- [ ] **1g — Textures** (`glGenTextures`, `glDeleteTextures`, `glActiveTexture`, `glTexParameteri`, `glTexParameterf`, `glTexSubImage2D`, `glCopyTexImage2D`, `glCopyTexSubImage2D`, `glTexEnvi`, `glTexEnvf`, `glGetTexParameteriv`, `glGetTexParameterfv`, `glIsTexture`)
- [ ] **1h — Framebuffer objects** (`glGenFramebuffers`, `glDeleteFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D`, `glFramebufferRenderbuffer`, `glCheckFramebufferStatus`, `glIsFramebuffer`)
- [ ] **1i — Renderbuffer objects** (`glGenRenderbuffers`, `glDeleteRenderbuffers`, `glBindRenderbuffer`, `glRenderbufferStorage`, `glIsRenderbuffer`)
- [ ] **1j — Rasterization state** (`glCullFace`, `glFrontFace`, `glLineWidth`, `glPointSize`, `glPolygonMode`, `glPolygonOffset`)
- [ ] **1k — Stencil state** (`glStencilFunc`, `glStencilFuncSeparate`, `glStencilOp`, `glStencilOpSeparate`, `glStencilMask`, `glStencilMaskSeparate`, `glClearStencil`)
- [ ] **1l — Color/blend state** (`glColorMask`, `glBlendEquation`, `glBlendEquationSeparate`, `glBlendFuncSeparate`, `glBlendColor`, `glLogicOp`)
- [ ] **1m — Depth state** (`glDepthMask`, `glDepthRange`, `glClearDepth`)
- [ ] **1n — Pixel storage & transfer** (`glPixelStorei`, `glPixelStoref`, `glReadBuffer`, `glDrawBuffer`)
- [ ] **1o — State queries** (`glGetBooleanv`, `glGetFloatv`, `glGetDoublev`, `glGetError`, `glGetStringi` — GL 3.0 extension)
- [ ] **1p — Misc commands** (`glFlush`, `glFinish`, `glHint`, `glScissor`, `glIsEnabled`)
- [ ] **1q — Display lists** (`glGenLists`, `glNewList`, `glEndList`, `glCallList`, `glCallLists`, `glDeleteLists`, `glIsList` — many games use these for UI)
- [ ] **1r — Immediate-mode vertex data** (`glVertex2f/3f/4f`, `glNormal3f`, `glTexCoord1f/2f/3f/4f`, `glMultiTexCoord*` — ARB, `glColor3ub/4ub`, `glColorMaterial`)
- [ ] **1s — Lighting/material (fixed-function)** (`glLightfv`, `glLightModelfv`, `glMaterialfv`, `glShadeModel`, `glEnable`/`glDisable` for `GL_LIGHTING`, `GL_LIGHT0-7`, `GL_NORMALIZE`, `GL_COLOR_MATERIAL`)
- [ ] **1t — Fog** (`glFogfv`, `glFogi`, `glEnable(GL_FOG)`)
- [ ] **1u — Alpha test** (`glAlphaFunc`, `glEnable(GL_ALPHA_TEST)`)
- [ ] **1v — Clip planes** (`glClipPlane`, `glEnable(GL_CLIP_PLANE0-5)`)
- [ ] **1w — Macros: add `GL_PASSTHROUGH5`, `GL_PASSTHROUGH8`** to EntryPoint.cpp (needed for e.g. `glFrustum` style params)
- [ ] **1x — Tests: verify all 200+ GL 2.1 functions resolve via `getGLProcAddress`** (doesn't need draw, just symbol lookup)
- [ ] **1y — CI: build + symbol-resolve test passes**

---

### Phase 2: GLSL → MSL Shader Translation (GL 3.3+/4.x Core Profile)

**Goal**: Games using GLSL 1.30+ shaders (GL 3.3 core profile) have their shaders transparently translated to MSL at runtime via SPIRV-Cross.

**Why this matters**: macOS native GL is capped at GLSL 1.20 (GL 2.1). Any game that requires GL 3.3+ core profile shaders will fail with GLSL compile errors. SPIRV-Cross provides GLSL → SPIR-V → MSL translation.

- [ ] **2a — Integrate SPIRV-Cross source** (vendor as submodule under `vendor/spirv-cross/`; build as CMake static library `spirv_cross_glsl` + `spirv_cross_msl`)
- [ ] **2b — GLSL version detection** (parse `#version` directive from `glShaderSource`; < 130 delegates to native GL, >= 130 triggers cross-compile path)
- [ ] **2c — GLSL → SPIR-V compilation** (link `glslang` / `libshaderc`, or use system `glslangValidator` if available; compile GLSL source to SPIR-V binary)
- [ ] **2d — SPIR-V → MSL translation** (SPIRV-Cross `CompilerMSL`: translate SPIR-V binary to MSL source string, handle vertex/fragment entry points, map uniform buffers to Metal buffer bindings, map samplers to Metal texture bindings)
- [ ] **2e — Shader object intercept** (`glCreateShader` → allocate internal struct; `glShaderSource` → store source; `glCompileShader` → run cross-compile pipeline or forward to native GL depending on version; `glGetShaderiv(GL_COMPILE_STATUS)` → report cross-compile result)
- [ ] **2f — Program object intercept** (`glCreateProgram` → allocate internal struct with list of attached shaders; `glAttachShader` → track; `glLinkProgram` → create `MTLRenderPipelineDescriptor`, set vertex/fragment MSL functions, set vertex descriptor, set color/depth/stencil attachment formats; `glUseProgram` → bind pipeline state)
- [ ] **2g — Uniform mapping** (map `glUniform*` calls to Metal buffer writes; `glGetUniformLocation` → return Metal buffer binding index; `glGetActiveUniform` / `glGetUniformBlockIndex` → report Metal buffer layout)
- [ ] **2h — Vertex attribute mapping** (map `glVertexAttribPointer` calls to `MTLVertexDescriptor`; materialize layout at `glLinkProgram` time; handle interleaved vs separate arrays)
- [ ] **2i — Shader cache** (hash GLSL source + compile options; cache compiled MSL + `MTLLibrary` in `ShaderCache`; cache `MTLRenderPipelineState` in `PipelineCache` — reuse existing `ShaderCache.cpp` and `PipelineCache.cpp`)
- [ ] **2j — Error reporting** (GLSL compile errors → `glGetShaderInfoLog` returns cross-compile error message; link errors → SPIRV-Cross warnings as infolog output)
- [ ] **2k — Tests** (unit tests: GLSL 1.20 → native passthrough; GLSL 1.50 minimal vertex/fragment → MSL output; compile error → infolog; uniform location mapping; vertex attrib layout; shader cache hit/miss; program relink)
- [ ] **2l — CI: build with spirv-cross + tests pass**

---

### Phase 3: Metal Draw Emitter

**Goal**: GL draw calls produce actual Metal rendering. `glDrawArrays`/`glDrawElements` encode Metal draw commands using the translated pipeline state and bound resources.

**Why this matters**: Without this, all the shader translation in Phase 2 produces correct MSL but never renders anything on screen.

- [ ] **3a — Context-to-device bridge** (create `MTLDevice` from the active `NSOpenGLContext` or the default system device; each GL context owns a `MTLCommandQueue`; track active drawable via CAMetalLayer or offscreen texture)
- [ ] **3b — Draw call emission** (`glDrawArrays` → `MTLRenderCommandEncoder drawPrimitives`; `glDrawElements` → `drawIndexedPrimitives`; `glDrawRangeElements` → indexed draw; `glMultiDrawArrays` / `glMultiDrawElements` → loop of draws)
- [ ] **3c — State → pipeline descriptor** (translate `GLState` blend/depth/stencil/rasterizer settings to `MTLRenderPipelineDescriptor`; handle `glColorMask`, `glDepthMask`, `glStencilMask` as write masks; `glBlendEquation`/`glBlendFunc` → `MTLBlendDescriptor`; `glCullFace`/`glFrontFace` → `MTLCullMode`/`MTLWinding`; `glPolygonMode` → `MTLTriangleFillMode` (lines/polygons require primitive restart or geometry shader emulation))
- [ ] **3d — Vertex buffer binding** (translate `glVertexAttribPointer` state to `MTLBuffer` bindings at render-encoder time; handle VBO + client-side vertex arrays; honor `glVertexAttribDivisor` for instancing)
- [ ] **3e — Uniform buffer upload** (allocate `MTLBuffer` per-program for uniform blocks; update at `glUniform*` call time or batch at draw; handle default uniform block for GLSL < 1.40 programs)
- [ ] **3f — Texture binding** (map bound GL texture to `MTLTexture`; handle `glTexImage2D` → Metal texture creation; `glTexSubImage2D` → `MTLTexture replaceRegion`; `glGenerateMipmap` → `MTLBlitCommandEncoder generateMipmaps`; sampler state → `MTLSamplerState` from `glTexParameteri` settings)
- [ ] **3g — Framebuffer / render pass** (`glBindFramebuffer(GL_FRAMEBUFFER, 0)` → present to drawable; bound FBO → `MTLRenderPassDescriptor` with color/depth/stencil attachments; `glClear` → load-action clear; `glBlitFramebuffer` → `MTLBlitCommandEncoder`)
- [ ] **3h — Viewport / scissor** (`glViewport` → `MTLRenderCommandEncoder setViewport`; `glScissor` → `setScissorRect`; `glDepthRange` → viewport depth range)
- [ ] **3i — ReadPixels** (`glReadPixels` → `MTLTexture getBytes` or `MTLBlitCommandEncoder synchronizeResource` + read; handle PBO async reads if bound)
- [ ] **3j — Swap / present** (`wglSwapBuffers` → `MTLCommandBuffer presentDrawable` + commit; handled in Phase 4 WGL context)
- [ ] **3k — Synchronization** (`glFlush` → commit command buffer and wait until completed; `glFinish` → commit and wait; `glClientWaitSync`/`glFenceSync` if GL 3.2+ fence objects are used)
- [ ] **3l — Tests** (unit: clear color + read pixels; triangle with GLSL 1.50 cross-compiled shader; texture upload + sample in shader; FBO render-to-texture; vertex buffer bindings; uniform updates; viewport/scissor; blend modes; depth test; stencil test)
- [ ] **3m — CI: Metal draw test output compared to golden image (or pixel checksum)**

---

### Phase 4: Real WGL Context Management

**Goal**: Replace WGL sentinel handles with real `NSOpenGLContext` objects that can drive Metal rendering and interoperate with Wine's context management.

**Why this matters**: Current sentinel handles mean any WGL-dependent code path goes through a fake context. Real `NSOpenGLContext` objects are required for Metal interop (`[[CAMetalLayer alloc] init]` needs a GL context in some setups) and for proper Wine integration.

- [ ] **4a — Pixel format descriptor** (`wglChoosePixelFormat` / `wglDescribePixelFormat` → translate requested format to `NSOpenGLPixelFormat` attributes: color/depth/stencil/accum bits, double-buffer, multisample, stereo, etc.)
- [ ] **4b — Context creation** (`wglCreateContext` → `[[NSOpenGLContext alloc] initWithFormat:pixelFormat shareContext:nil]`; `wglCreateContextAttribsARB` → GL 3.2+ core profile context via `NSOpenGLProfileVersion3_2Core`)
- [ ] **4c — Context binding** (`wglMakeCurrent` → `[context makeCurrentContext]`; track current context per thread for GL state queries)
- [ ] **4d — Context sharing** (`wglShareLists` → use `NSOpenGLContext` share context parameter; share Metal device + shader cache between contexts)
- [ ] **4e — Swap / vsync** (`wglSwapBuffers` → present Metal drawable; `wglSwapIntervalEXT` → `[context setValues:&swapInt forParameter:NSOpenGLContextParameterSwapInterval]`)
- [ ] **4f — PBuffers** (`wglCreatePbufferARB` → `NSOpenGLPixelBuffer` or offscreen texture as fallback; `wglGetPbufferDCARB`/`wglReleasePbufferDCARB` stubs)
- [ ] **4g — Wine interop** (Wine's `opengl32.dll` → `winemac.drv` chain: detect if running inside Wine; if so, delegate context management to Wine rather than creating our own; only create native contexts for direct GL loading)
- [ ] **4h — Tests** (create context → make current → get proc address for current context; multi-context isolation; context sharing: texture created in ctx1 usable in ctx2; pixel format attribute matching)

---

### Phase 5: Compatibility, Polish & Real-Game Validation

**Goal**: End-to-end validation against real GL-based games. Fix edge cases discovered during testing.

- [ ] **5a — Error state machine** (`glGetError` → track errors from invalid operations; set `GL_INVALID_ENUM`, `GL_INVALID_VALUE`, `GL_INVALID_OPERATION`, `GL_OUT_OF_MEMORY` appropriately; don't crash on errors — games often call GL functions before init and rely on error codes)
- [ ] **5b — GL extension string** (`glGetString(GL_EXTENSIONS)` → report the intersection of macOS native GL extensions + Metal-backed extensions we support; `glewInit` / `gladLoadGL` must succeed)
- [ ] **5c — Multi-threading** (GL contexts are thread-local by spec; WGL allows multiple threads with different current contexts; protect shared state (shader cache, device) with mutex; per-context command queue isolation)
- [ ] **5d — GL 4.x features** (tessellation shaders, geometry shaders, compute shaders — translated via SPIRV-Cross to Metal equivalents; transform feedback → Metal vertex shader with buffer writes; indirect draws → `MTLIndirectCommandBuffer` or fallback CPU read)
- [ ] **5e — Compatibility profile emulation** (GL 3.0+ compatibility profiles allow mixing fixed-function with shaders; Metal doesn't have fixed-function — implement as shader-generated path: `glMatrixMode`/`glLoadMatrix` → injection of uniform matrix into translated vertex shader; `glEnable(GL_LIGHTING)` → lighting uniform block)
- [ ] **5f — Performance** (batch uniform updates; reuse command encoders across consecutive draws; avoid redundant pipeline state reconstruction; use MTLHeap for transient allocations)
- [ ] **5g — Real game smoke tests** (test with at least 3 GL-based games: one FNA title via SDL bridge, one older Wine title using GL 2.1, one newer title using GL 3.3+ core profile; report per-phase results)
- [ ] **5h — CI: full test suite passes (symbol resolution, shader compilation, draw correctness, pixel checksums)**

---

## Dependencies

```
Phase 1 ──► Phase 2 ──► Phase 3 ──► Phase 5
                │            │
                └── Phase 4 ─┘ (can run in parallel with Phase 2-3)
```

Phase 1 is fully independent. Phases 2 and 3 are tightly coupled (shader translation without draw emission is useless; draw emission needs translated shaders). Phase 4 is mostly independent of Phase 2/3 but should land before Phase 5.

## Risk Areas

| Risk | Mitigation |
|---|---|
| SPIRV-Cross MSL output doesn't match GLSL semantics perfectly | Test with GLSL conformance suite subset; fallback to Apple GLSL compiler on macOS < 14 |
| macOS OpenGL deprecation removes framework entirely in future macOS | Phase 3 Metal backend is the migration path — once Phase 3 lands, Phase 1 passthroughs become fallback only |
| Wine's own opengl32.dll conflicts with our shim | Phase 4g: detect Wine, delegate context to Wine, only intercept GL calls for shader translation |
| Performance of runtime shader translation | Phase 2i shader cache: translate once per shader, not per frame; SDL_ShaderCross has offline compilation support |
| GL 4.x features (tessellation, geometry, compute) have no 1:1 Metal equivalents | Phase 5d: SPIRV-Cross handles most translations; remaining gaps identified in testing |
| Compatibility profile games (mix fixed-function + shaders) | Phase 5e: uniform injection at shader boundary; some fixed-function features (fog, alpha test, clip planes) emulated in shader |

## AI disclosure
- [x] AI tools were used (planning and document structure)
