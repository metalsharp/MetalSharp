#define INITGUID
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr UINT kWidth = 1280;
static constexpr UINT kHeight = 720;
static constexpr UINT kTextureSize = 512;
static constexpr UINT kBackBufferCount = 2;
static constexpr UINT kInstanceCount = 144;
static constexpr UINT kMaxSceneVertices = 20000;
static constexpr UINT kRunSeconds = 15;
static constexpr float kSplashSeconds = 5.0f;

static bool envEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && value[0] && value[0] != '0';
}

static float envFloat(const char *name, float fallback) {
  const char *value = std::getenv(name);
  if (!value || !value[0])
    return fallback;
  char *end = nullptr;
  float parsed = std::strtof(value, &end);
  return end && end != value ? parsed : fallback;
}

static UINT alignUp(UINT value, UINT alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

template <typename T>
static void releaseIf(T *&ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

static bool checkHr(const char *label, HRESULT hr) {
  if (SUCCEEDED(hr)) {
    std::printf("[PASS] %s\n", label);
    return true;
  }
  std::fprintf(stderr, "[FAIL] %s: 0x%08lx\n", label, hr);
  return false;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool pumpMessages() {
  MSG msg = {};
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT)
      return false;
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return true;
}

struct Mat4 {
  float m[16];
};

struct Vec3 {
  float x, y, z;
};

static Vec3 add(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3 sub(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 mul(Vec3 a, float s) {
  return {a.x * s, a.y * s, a.z * s};
}

static float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

static Vec3 normalize(Vec3 v) {
  float len = std::sqrt(dot(v, v));
  if (len < 0.00001f)
    return {0.0f, 1.0f, 0.0f};
  float inv = 1.0f / len;
  return {v.x * inv, v.y * inv, v.z * inv};
}

static Mat4 identity() {
  Mat4 r = {};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

static Mat4 mul(Mat4 a, Mat4 b) {
  Mat4 r = {};
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      for (int k = 0; k < 4; k++)
        r.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
    }
  }
  return r;
}

static Mat4 transpose(Mat4 a) {
  Mat4 r = {};
  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++)
      r.m[row * 4 + col] = a.m[col * 4 + row];
  }
  return r;
}

static Mat4 perspective(float fovy, float aspect, float zn, float zf) {
  Mat4 r = {};
  float y = 1.0f / std::tan(fovy * 0.5f);
  float x = y / aspect;
  r.m[0] = x;
  r.m[5] = y;
  r.m[10] = zf / (zf - zn);
  r.m[11] = 1.0f;
  r.m[14] = (-zn * zf) / (zf - zn);
  return r;
}

static Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
  Vec3 z = normalize(sub(target, eye));
  Vec3 x = normalize(cross(up, z));
  Vec3 y = cross(z, x);
  Mat4 r = identity();
  r.m[0] = x.x;
  r.m[4] = x.y;
  r.m[8] = x.z;
  r.m[1] = y.x;
  r.m[5] = y.y;
  r.m[9] = y.z;
  r.m[2] = z.x;
  r.m[6] = z.y;
  r.m[10] = z.z;
  r.m[12] = -dot(x, eye);
  r.m[13] = -dot(y, eye);
  r.m[14] = -dot(z, eye);
  return r;
}

static bool compileShader(const char *label, const char *source, const char *entry,
                          const char *target, ID3DBlob **blob) {
  UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
  ID3DBlob *errors = nullptr;
  HRESULT hr = D3DCompile(source, std::strlen(source), label, nullptr, nullptr,
                          entry, target, flags, 0, blob, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fprintf(stderr, "[FAIL] %s compile: %s\n", label,
                   static_cast<const char *>(errors->GetBufferPointer()));
    else
      std::fprintf(stderr, "[FAIL] %s compile: 0x%08lx\n", label, hr);
    releaseIf(errors);
    return false;
  }
  releaseIf(errors);
  if (envEnabled("VKD3D_STRESS_DUMP_SHADER_DISASM")) {
    ID3DBlob *disasm = nullptr;
    if (SUCCEEDED(D3DDisassemble((*blob)->GetBufferPointer(),
                                 (*blob)->GetBufferSize(), 0, nullptr,
                                 &disasm)) &&
        disasm) {
      std::printf("=== disasm %s ===\n%.*s\n=== end disasm %s ===\n", label,
                  static_cast<int>(disasm->GetBufferSize()),
                  static_cast<const char *>(disasm->GetBufferPointer()),
                  label);
    }
    releaseIf(disasm);
  }
  std::printf("[PASS] %s compiled (%zu bytes)\n", label, (*blob)->GetBufferSize());
  return true;
}

static bool logLoadLibrary(const char *name, bool required) {
  HMODULE module = LoadLibraryA(name);
  if (module) {
    std::printf("[PASS] runtime dependency %s\n", name);
    FreeLibrary(module);
    return true;
  }
  std::printf(required ? "[FAIL] runtime dependency %s missing\n"
                       : "[INFO] optional runtime dependency %s missing\n",
              name);
  return !required;
}

static void logRuntimePrereqs() {
  std::printf("=== vkd3d prerequisite scan ===\n");
  logLoadLibrary("d3d12.dll", true);
  logLoadLibrary("dxgi.dll", true);
  logLoadLibrary("d3dcompiler_47.dll", true);
  logLoadLibrary("dxcompiler.dll", false);
  logLoadLibrary("dxil.dll", false);
}

static void logFeatureQueryFailed(const char *name, HRESULT hr) {
  std::printf("[INFO] featureQuery %s failed hr=0x%08lx\n", name, hr);
}

struct SceneConstants {
  Mat4 viewProj;
  float time;
  float frame;
  float width;
  float height;
  float pad[44];
};

struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
  float r, g, b, a;
};

struct Instance {
  float ox, oy, oz, scale;
  float r, g, b, phase;
};

struct ReadbackStats {
  uint32_t brightPixels = 0;
  uint32_t chromaPixels = 0;
  uint64_t checksum = 1469598103934665603ull;
};

struct Harness {
  HWND hwnd = nullptr;
  IDXGIFactory4 *factory = nullptr;
  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  IDXGISwapChain3 *swapchain = nullptr;
  ID3D12DescriptorHeap *rtvHeap = nullptr;
  ID3D12DescriptorHeap *dsvHeap = nullptr;
  ID3D12DescriptorHeap *srvHeap = nullptr;
  ID3D12Resource *backBuffers[kBackBufferCount] = {};
  ID3D12Resource *depth = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *cmd = nullptr;
  ID3D12Fence *fence = nullptr;
  HANDLE fenceEvent = nullptr;
  UINT rtvStride = 0;
  UINT cbvSrvUavStride = 0;
  uint64_t fenceValue = 0;
};

static void logHardFeatureCaps(Harness &h) {
  D3D_FEATURE_LEVEL requestedLevels[] = {
      D3D_FEATURE_LEVEL_12_2,
      D3D_FEATURE_LEVEL_12_1,
      D3D_FEATURE_LEVEL_12_0,
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
  };
  D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
  featureLevels.NumFeatureLevels = sizeof(requestedLevels) / sizeof(requestedLevels[0]);
  featureLevels.pFeatureLevelsRequested = requestedLevels;
  if (SUCCEEDED(h.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels,
                                              sizeof(featureLevels)))) {
    std::printf("[INFO] featureLevels requested=12_2,12_1,12_0,11_1,11_0 max=0x%x\n",
                static_cast<unsigned>(featureLevels.MaxSupportedFeatureLevel));
    if (featureLevels.MaxSupportedFeatureLevel < D3D_FEATURE_LEVEL_12_0)
      std::printf("[INFO] game-style requirement not met: D3D feature level 12_0+\n");
  }

  D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
  HRESULT featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                    sizeof(options));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options tiledResourcesTier=%u resourceBindingTier=%u "
                "typedUavAdditionalFormats=%u conservativeRasterizationTier=%u "
                "resourceHeapTier=%u rov=%u logicOp=%u\n",
                static_cast<unsigned>(options.TiledResourcesTier),
                static_cast<unsigned>(options.ResourceBindingTier),
                static_cast<unsigned>(options.TypedUAVLoadAdditionalFormats),
                static_cast<unsigned>(options.ConservativeRasterizationTier),
                static_cast<unsigned>(options.ResourceHeapTier),
                static_cast<unsigned>(options.ROVsSupported),
                static_cast<unsigned>(options.OutputMergerLogicOp));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS", featureHr);
  }

  D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSig = {};
  rootSig.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rootSig,
                                            sizeof(rootSig));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] rootSignature highest=0x%x\n",
                static_cast<unsigned>(rootSig.HighestVersion));
  } else {
    logFeatureQueryFailed("ROOT_SIGNATURE", featureHr);
  }

  D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch,
                                            sizeof(arch));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] architecture tileBased=%u uma=%u cacheCoherentUma=%u\n",
                static_cast<unsigned>(arch.TileBasedRenderer),
                static_cast<unsigned>(arch.UMA),
                static_cast<unsigned>(arch.CacheCoherentUMA));
  } else {
    logFeatureQueryFailed("ARCHITECTURE", featureHr);
  }

  D3D12_FEATURE_DATA_ARCHITECTURE1 arch1 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch1,
                                            sizeof(arch1));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] architecture1 tileBased=%u uma=%u cacheCoherentUma=%u isolatedMmu=%u\n",
                static_cast<unsigned>(arch1.TileBasedRenderer),
                static_cast<unsigned>(arch1.UMA),
                static_cast<unsigned>(arch1.CacheCoherentUMA),
                static_cast<unsigned>(arch1.IsolatedMMU));
  } else {
    logFeatureQueryFailed("ARCHITECTURE1", featureHr);
  }

  D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpuVa = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,
                                            &gpuVa, sizeof(gpuVa));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] gpuVa bitsPerResource=%u bitsPerProcess=%u\n",
                gpuVa.MaxGPUVirtualAddressBitsPerResource,
                gpuVa.MaxGPUVirtualAddressBitsPerProcess);
  } else {
    logFeatureQueryFailed("GPU_VIRTUAL_ADDRESS_SUPPORT", featureHr);
  }

  const DXGI_FORMAT formats[] = {
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT,
      DXGI_FORMAT_R11G11B10_FLOAT,
      DXGI_FORMAT_R32_UINT,
      DXGI_FORMAT_D32_FLOAT,
      DXGI_FORMAT_D24_UNORM_S8_UINT,
  };
  for (DXGI_FORMAT format : formats) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = format;
    featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                              &support, sizeof(support));
    if (SUCCEEDED(featureHr)) {
      std::printf("[INFO] formatSupport fmt=%u support1=0x%x support2=0x%x\n",
                  static_cast<unsigned>(format),
                  static_cast<unsigned>(support.Support1),
                  static_cast<unsigned>(support.Support2));
    } else {
      char name[64] = {};
      std::snprintf(name, sizeof(name), "FORMAT_SUPPORT_%u", static_cast<unsigned>(format));
      logFeatureQueryFailed(name, featureHr);
    }
  }

  const UINT sampleCounts[] = {2u, 4u, 8u};
  for (UINT samples : sampleCounts) {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa = {};
    msaa.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    msaa.SampleCount = samples;
    msaa.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                              &msaa, sizeof(msaa));
    if (SUCCEEDED(featureHr)) {
      std::printf("[INFO] msaa fmt=R8G8B8A8 samples=%u qualityLevels=%u\n",
                  samples, msaa.NumQualityLevels);
    } else {
      char name[64] = {};
      std::snprintf(name, sizeof(name), "MSAA_%u", samples);
      logFeatureQueryFailed(name, featureHr);
    }
  }

#if defined(D3D12_FEATURE_D3D12_OPTIONS1)
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                            sizeof(options1));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options1 waveOps=%u waveLaneMin=%u waveLaneMax=%u "
                "totalLaneCount=%u expandedComputeResources=%u int64ShaderOps=%u\n",
                static_cast<unsigned>(options1.WaveOps),
                options1.WaveLaneCountMin,
                options1.WaveLaneCountMax,
                options1.TotalLaneCount,
                static_cast<unsigned>(options1.ExpandedComputeResourceStates),
                static_cast<unsigned>(options1.Int64ShaderOps));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS1", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_SHADER_MODEL)
  D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
  shaderModel.HighestShaderModel = D3D_HIGHEST_SHADER_MODEL;
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                            sizeof(shaderModel));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] shaderModel highest=0x%x\n",
                static_cast<unsigned>(shaderModel.HighestShaderModel));
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_0)
      std::printf("[INFO] UE-style SM6 gate not met: Lumen/Nanite/VSM path would be disabled\n");
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6)
      std::printf("[INFO] UE-style SM6.6 atomics gate not met: Nanite/VSM full path would be disabled\n");
  } else {
    logFeatureQueryFailed("SHADER_MODEL", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS4)
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4,
                                            sizeof(options4));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options4 native16BitShaderOps=%u msaa64KBAligned=%u sharedResourceTier=%u\n",
                static_cast<unsigned>(options4.Native16BitShaderOpsSupported),
                static_cast<unsigned>(options4.MSAA64KBAlignedTextureSupported),
                static_cast<unsigned>(options4.SharedResourceCompatibilityTier));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS4", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS5)
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                            sizeof(options5));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options5 renderPassTier=%u raytracingTier=%u srvOnlyTiledTier3=%u\n",
                static_cast<unsigned>(options5.RenderPassesTier),
                static_cast<unsigned>(options5.RaytracingTier),
                static_cast<unsigned>(options5.SRVOnlyTiledResourceTier3));
    if (options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
      std::printf("[INFO] UE-style hardware ray tracing gate not met: HW Lumen/path tracer disabled\n");
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS5", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS6)
  D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6,
                                            sizeof(options6));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options6 vrsTier=%u additionalRates=%u perPrimitiveVrs=%u tileSize=%u backgroundProcessing=%u\n",
                static_cast<unsigned>(options6.VariableShadingRateTier),
                static_cast<unsigned>(options6.AdditionalShadingRatesSupported),
                static_cast<unsigned>(options6.PerPrimitiveShadingRateSupportedWithViewportIndexing),
                options6.ShadingRateImageTileSize,
                static_cast<unsigned>(options6.BackgroundProcessingSupported));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS6", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS7)
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                            sizeof(options7));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options7 meshShaderTier=%u samplerFeedbackTier=%u\n",
                static_cast<unsigned>(options7.MeshShaderTier),
                static_cast<unsigned>(options7.SamplerFeedbackTier));
    if (options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
      std::printf("[INFO] UE-style mesh shader gate not met: mesh shader path disabled\n");
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS7", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS9)
  D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &options9,
                                            sizeof(options9));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options9 meshStats=%u meshFullRTArray=%u atomicInt64Typed=%u "
                "atomicInt64GroupShared=%u meshDerivatives=%u waveMmaTier=%u\n",
                static_cast<unsigned>(options9.MeshShaderPipelineStatsSupported),
                static_cast<unsigned>(options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex),
                static_cast<unsigned>(options9.AtomicInt64OnTypedResourceSupported),
                static_cast<unsigned>(options9.AtomicInt64OnGroupSharedSupported),
                static_cast<unsigned>(options9.DerivativesInMeshAndAmplificationShadersSupported),
                static_cast<unsigned>(options9.WaveMMATier));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS9", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS11)
  D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11,
                                            sizeof(options11));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options11 atomicInt64DescriptorHeapResource=%u\n",
                static_cast<unsigned>(options11.AtomicInt64OnDescriptorHeapResourceSupported));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS11", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS12)
  D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                            sizeof(options12));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options12 enhancedBarriers=%u relaxedFormatCasting=%u msCulledStats=%d\n",
                static_cast<unsigned>(options12.EnhancedBarriersSupported),
                static_cast<unsigned>(options12.RelaxedFormatCastingSupported),
                static_cast<int>(options12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS12", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS13)
  D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS13, &options13,
                                            sizeof(options13));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options13 unrestrictedVertexAlignment=%u alphaBlendFactor=%u "
                "textureCopyBetweenDimensions=%u invertedViewportY=%u invertedViewportZ=%u\n",
                static_cast<unsigned>(options13.UnrestrictedVertexElementAlignmentSupported),
                static_cast<unsigned>(options13.AlphaBlendFactorSupported),
                static_cast<unsigned>(options13.TextureCopyBetweenDimensionsSupported),
                static_cast<unsigned>(options13.InvertedViewportHeightFlipsYSupported),
                static_cast<unsigned>(options13.InvertedViewportDepthFlipsZSupported));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS13", featureHr);
  }
#endif

#if defined(D3D12_FEATURE_D3D12_OPTIONS16)
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
  featureHr = h.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16,
                                            sizeof(options16));
  if (SUCCEEDED(featureHr)) {
    std::printf("[INFO] options16 gpuUploadHeap=%u dynamicDepthBias=%u\n",
                static_cast<unsigned>(options16.GPUUploadHeapSupported),
                static_cast<unsigned>(options16.DynamicDepthBiasSupported));
  } else {
    logFeatureQueryFailed("D3D12_OPTIONS16", featureHr);
  }
#endif
}

static bool waitForGpu(Harness &h, DWORD timeoutMs = 15000) {
  h.fenceValue++;
  HRESULT hr = h.queue->Signal(h.fence, h.fenceValue);
  if (!checkHr("queue Signal", hr))
    return false;
  if (h.fence->GetCompletedValue() >= h.fenceValue)
    return true;
  hr = h.fence->SetEventOnCompletion(h.fenceValue, h.fenceEvent);
  if (!checkHr("fence SetEventOnCompletion", hr))
    return false;
  DWORD wait = WaitForSingleObject(h.fenceEvent, timeoutMs);
  if (wait != WAIT_OBJECT_0) {
    std::fprintf(stderr, "[FAIL] fence wait timed out (%lu ms)\n",
                 static_cast<unsigned long>(timeoutMs));
    return false;
  }
  return true;
}

static ID3D12Resource *makeBuffer(Harness &h, D3D12_HEAP_TYPE heapType, UINT64 size,
                                  D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = heapType;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;

  ID3D12Resource *resource = nullptr;
  HRESULT hr = h.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  state, nullptr,
                                                  IID_PPV_ARGS(&resource));
  if (FAILED(hr))
    std::fprintf(stderr, "[FAIL] CreateCommittedResource buffer %llu: 0x%08lx\n",
                 static_cast<unsigned long long>(size), hr);
  return resource;
}

template <typename T>
static ID3D12Resource *makeUploadBuffer(Harness &h, const T *data, size_t bytes,
                                        const char *label) {
  ID3D12Resource *resource = makeBuffer(h, D3D12_HEAP_TYPE_UPLOAD, bytes,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!resource)
    return nullptr;
  void *mapped = nullptr;
  HRESULT hr = resource->Map(0, nullptr, &mapped);
  if (!checkHr(label, hr)) {
    releaseIf(resource);
    return nullptr;
  }
  std::memcpy(mapped, data, bytes);
  resource->Unmap(0, nullptr);
  return resource;
}

static ID3D12Resource *makeTexture2D(Harness &h, DXGI_FORMAT format, UINT width,
                                     UINT height, D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags,
                                     const D3D12_CLEAR_VALUE *clearValue = nullptr) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap.CreationNodeMask = 1;
  heap.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = flags;

  ID3D12Resource *resource = nullptr;
  HRESULT hr = h.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  state, clearValue,
                                                  IID_PPV_ARGS(&resource));
  if (FAILED(hr))
    std::fprintf(stderr, "[FAIL] CreateCommittedResource texture %ux%u: 0x%08lx\n",
                 width, height, hr);
  return resource;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv(Harness &h, UINT index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = h.srvHeap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += index * h.cbvSrvUavStride;
  return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpuSrv(Harness &h, UINT index) {
  D3D12_GPU_DESCRIPTOR_HANDLE handle = h.srvHeap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += index * h.cbvSrvUavStride;
  return handle;
}

static bool createHarness(Harness &h) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "VKD3DStressHarness";
  RegisterClassA(&wc);

  h.hwnd = CreateWindowA("VKD3DStressHarness", "vkd3d_stress_game.exe",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                         CW_USEDEFAULT, kWidth, kHeight, nullptr, nullptr,
                         wc.hInstance, nullptr);
  if (!h.hwnd) {
    std::fprintf(stderr, "[FAIL] CreateWindowA\n");
    return false;
  }
  std::printf("[PASS] Window %ux%u\n", kWidth, kHeight);

  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&h.factory));
  if (!checkHr("CreateDXGIFactory2", hr))
    return false;
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&h.device));
  if (SUCCEEDED(hr)) {
    std::printf("[PASS] D3D12CreateDevice feature_level=12_0\n");
  } else {
    std::printf("[INFO] D3D12CreateDevice feature_level=12_0 failed: 0x%08lx; "
                "falling back so the stress harness can keep probing\n",
                hr);
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&h.device));
    if (!checkHr("D3D12CreateDevice feature_level=11_0 fallback", hr))
      return false;
  }
  logHardFeatureCaps(h);

  D3D12_COMMAND_QUEUE_DESC qdesc = {};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = h.device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&h.queue));
  if (!checkHr("CreateCommandQueue DIRECT", hr))
    return false;

  DXGI_SWAP_CHAIN_DESC1 sc = {};
  sc.Width = kWidth;
  sc.Height = kHeight;
  sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = kBackBufferCount;
  sc.SampleDesc.Count = 1;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1 *swap1 = nullptr;
  hr = h.factory->CreateSwapChainForHwnd(h.queue, h.hwnd, &sc, nullptr, nullptr,
                                         &swap1);
  if (!checkHr("CreateSwapChainForHwnd", hr))
    return false;
  hr = swap1->QueryInterface(IID_PPV_ARGS(&h.swapchain));
  releaseIf(swap1);
  if (!checkHr("IDXGISwapChain3", hr))
    return false;

  D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
  rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvDesc.NumDescriptors = kBackBufferCount;
  hr = h.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&h.rtvHeap));
  if (!checkHr("Create RTV heap", hr))
    return false;
  h.rtvStride = h.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = h.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < kBackBufferCount; i++) {
    hr = h.swapchain->GetBuffer(i, IID_PPV_ARGS(&h.backBuffers[i]));
    if (!checkHr("GetBuffer", hr))
      return false;
    h.device->CreateRenderTargetView(h.backBuffers[i], nullptr, rtv);
    rtv.ptr += h.rtvStride;
  }

  D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
  dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvDesc.NumDescriptors = 1;
  hr = h.device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&h.dsvHeap));
  if (!checkHr("Create DSV heap", hr))
    return false;

  D3D12_CLEAR_VALUE depthClear = {};
  depthClear.Format = DXGI_FORMAT_D32_FLOAT;
  depthClear.DepthStencil.Depth = 1.0f;
  h.depth = makeTexture2D(h, DXGI_FORMAT_D32_FLOAT, kWidth, kHeight,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE,
                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &depthClear);
  if (!h.depth)
    return false;
  h.device->CreateDepthStencilView(h.depth, nullptr,
                                   h.dsvHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
  srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvDesc.NumDescriptors = 4;
  srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = h.device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&h.srvHeap));
  if (!checkHr("Create CBV/SRV/UAV heap", hr))
    return false;
  h.cbvSrvUavStride =
      h.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  hr = h.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&h.allocator));
  if (!checkHr("CreateCommandAllocator DIRECT", hr))
    return false;
  hr = h.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.allocator,
                                   nullptr, IID_PPV_ARGS(&h.cmd));
  if (!checkHr("CreateCommandList DIRECT", hr))
    return false;
  h.cmd->Close();

  hr = h.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h.fence));
  if (!checkHr("CreateFence", hr))
    return false;
  h.fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!h.fenceEvent) {
    std::fprintf(stderr, "[FAIL] CreateEventA\n");
    return false;
  }
  return true;
}

static void destroyHarness(Harness &h) {
  waitForGpu(h, 1000);
  if (h.fenceEvent)
    CloseHandle(h.fenceEvent);
  releaseIf(h.fence);
  releaseIf(h.cmd);
  releaseIf(h.allocator);
  releaseIf(h.depth);
  for (auto *&bb : h.backBuffers)
    releaseIf(bb);
  releaseIf(h.srvHeap);
  releaseIf(h.dsvHeap);
  releaseIf(h.rtvHeap);
  releaseIf(h.swapchain);
  releaseIf(h.queue);
  releaseIf(h.device);
  releaseIf(h.factory);
  if (h.hwnd)
    DestroyWindow(h.hwnd);
}

static bool createRootSignature(Harness &h, const D3D12_ROOT_SIGNATURE_DESC &desc,
                                ID3D12RootSignature **rootSig) {
  ID3DBlob *blob = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &blob, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fprintf(stderr, "[FAIL] SerializeRootSignature: %s\n",
                   static_cast<const char *>(errors->GetBufferPointer()));
    else
      std::fprintf(stderr, "[FAIL] SerializeRootSignature: 0x%08lx\n", hr);
    releaseIf(errors);
    return false;
  }
  releaseIf(errors);
  hr = h.device->CreateRootSignature(0, blob->GetBufferPointer(),
                                     blob->GetBufferSize(), IID_PPV_ARGS(rootSig));
  releaseIf(blob);
  return checkHr("CreateRootSignature", hr);
}

static const char *kComputeShader =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "RWTexture2D<float4> outTex : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID) {\n"
    "  if (tid.x >= 512 || tid.y >= 512) return;\n"
    "  float2 uv = (float2(tid.xy) + 0.5) / 512.0;\n"
    "  float a = sin((uv.x * 31.0 + time) * 2.3) + cos((uv.y * 29.0 - time) * 1.7);\n"
    "  float b = sin((uv.x + uv.y + time * 0.19) * 57.0);\n"
    "  float rings = sin(length(uv - 0.5) * 90.0 - time * 7.0);\n"
    "  float3 c = 0.5 + 0.5 * sin(float3(a, b, rings) + float3(0.0, 2.1, 4.2));\n"
    "  outTex[tid.xy] = float4(c, 1.0);\n"
    "}\n";

static const char *kSceneVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float phase = i.inst1.w + time * (0.7 + frac(iid * 0.017));\n"
    "  float s = sin(phase);\n"
    "  float c = cos(phase);\n"
    "  float3 p = i.pos * i.inst0.w;\n"
    "  float3 rotated = float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);\n"
    "  float wave = sin(time * 2.0 + iid * 0.41) * 0.18;\n"
    "  float3 world = rotated + float3(i.inst0.x, i.inst0.y + wave, i.inst0.z);\n"
    "  if (i.color.b > 0.45 && i.color.r < 0.25 && i.pos.y < -0.7) {\n"
    "    world.y += sin(i.pos.x * 1.8 + time * 2.1) * 0.10 + cos(i.pos.z * 2.4 - time * 1.7) * 0.07;\n"
    "  }\n"
    "  float3 n = normalize(float3(i.normal.x * c + i.normal.z * s, i.normal.y, -i.normal.x * s + i.normal.z * c));\n"
    "  VS_OUT o;\n"
    "  o.pos = mul(float4(world, 1.0), viewProj);\n"
    "  o.normal = n;\n"
    "  o.uv = i.uv * (2.0 + frac(iid * 0.13));\n"
    "  o.color = float4(i.color.rgb * i.inst1.rgb, 1.0);\n"
    "  o.world = world;\n"
    "  return o;\n"
    "}\n";

static const char *kSceneClipVs =
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float2 p[3] = { float2(-1.0,-1.0), float2(-1.0,3.0), float2(3.0,-1.0) };\n"
    "  VS_OUT o;\n"
    "  o.pos = float4(p[vid % 3], 0.0, 1.0);\n"
    "  o.normal = float3(0.0, 0.0, -1.0);\n"
    "  o.uv = p[vid % 3] * 0.5 + 0.5;\n"
    "  o.color = float4(1.0, 0.16, 0.82, 1.0);\n"
    "  o.world = float3(0.0, 0.0, 0.0);\n"
    "  return o;\n"
    "}\n";

static const char *kSceneClipCbufferVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float2 p[3] = { float2(-1.0,-1.0), float2(-1.0,3.0), float2(3.0,-1.0) };\n"
    "  float cbufOk = (abs(viewProj[0][0]) > 0.001 && abs(width) > 1.0) ? 1.0 : 0.0;\n"
    "  VS_OUT o;\n"
    "  o.pos = float4(p[vid % 3], 0.0, 1.0);\n"
    "  o.normal = float3(0.0, 0.0, -1.0);\n"
    "  o.uv = p[vid % 3] * 0.5 + 0.5;\n"
    "  o.color = lerp(float4(0.0, 1.0, 0.15, 1.0), float4(1.0, 0.16, 0.82, 1.0), cbufOk);\n"
    "  o.world = float3(viewProj[0][0], width, height);\n"
    "  return o;\n"
    "}\n";

static const char *kSceneCbufferGateVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float2 p[3] = { float2(-1.0,-1.0), float2(-1.0,3.0), float2(3.0,-1.0) };\n"
    "  float ok = (abs(viewProj[0][0] - 0.88294816) < 0.25 && abs(viewProj[1][1] - 1.42242265) < 0.35 && abs(viewProj[3][3] - 16.55294609) < 2.0 && abs(width - 1280.0) < 2.0) ? 1.0 : 0.0;\n"
    "  VS_OUT o;\n"
    "  o.pos = float4(p[vid % 3] + float2(4.0 * (1.0 - ok), 0.0), 0.0, 1.0);\n"
    "  o.normal = float3(0.0, 0.0, -1.0);\n"
    "  o.uv = p[vid % 3] * 0.5 + 0.5;\n"
    "  o.color = float4(ok, 0.1, 1.0 - ok, 1.0);\n"
    "  o.world = float3(viewProj[0][0], viewProj[1][1], viewProj[3][3]);\n"
    "  return o;\n"
    "}\n";

static const char *kSceneWorldVs =
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float3 p = i.pos * i.inst0.w;\n"
    "  float3 world = p + float3(i.inst0.x, i.inst0.y, i.inst0.z);\n"
    "  VS_OUT o;\n"
    "  o.pos = float4(world.x / 14.0, world.y / 8.0, 0.0, 1.0);\n"
    "  o.normal = i.normal;\n"
    "  o.uv = i.uv;\n"
    "  o.color = float4(i.color.rgb * i.inst1.rgb, 1.0);\n"
    "  o.world = world;\n"
    "  return o;\n"
    "}\n";

static const char *kSceneManualMulVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float phase = i.inst1.w + time * (0.7 + frac(iid * 0.017));\n"
    "  float s = sin(phase);\n"
    "  float c = cos(phase);\n"
    "  float3 p = i.pos * i.inst0.w;\n"
    "  float3 rotated = float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);\n"
    "  float wave = sin(time * 2.0 + iid * 0.41) * 0.18;\n"
    "  float3 world = rotated + float3(i.inst0.x, i.inst0.y + wave, i.inst0.z);\n"
    "  float3 n = normalize(float3(i.normal.x * c + i.normal.z * s, i.normal.y, -i.normal.x * s + i.normal.z * c));\n"
    "  float4 w = float4(world, 1.0);\n"
    "  VS_OUT o;\n"
    "  o.pos = float4(dot(w, viewProj[0]), dot(w, viewProj[1]), dot(w, viewProj[2]), dot(w, viewProj[3]));\n"
    "  o.normal = n;\n"
    "  o.uv = i.uv * (2.0 + frac(iid * 0.13));\n"
    "  o.color = float4(i.color.rgb * i.inst1.rgb, 1.0);\n"
    "  o.world = world;\n"
    "  return o;\n"
    "}\n";

static const char *kSceneScalarMulVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float phase = i.inst1.w + time * (0.7 + frac(iid * 0.017));\n"
    "  float s = sin(phase);\n"
    "  float c = cos(phase);\n"
    "  float3 p = i.pos * i.inst0.w;\n"
    "  float3 rotated = float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);\n"
    "  float wave = sin(time * 2.0 + iid * 0.41) * 0.18;\n"
    "  float3 world = rotated + float3(i.inst0.x, i.inst0.y + wave, i.inst0.z);\n"
    "  float3 n = normalize(float3(i.normal.x * c + i.normal.z * s, i.normal.y, -i.normal.x * s + i.normal.z * c));\n"
    "  float4 w = float4(world, 1.0);\n"
    "  float4 clip;\n"
    "  clip.x = w.x * viewProj[0][0] + w.y * viewProj[1][0] + w.z * viewProj[2][0] + w.w * viewProj[3][0];\n"
    "  clip.y = w.x * viewProj[0][1] + w.y * viewProj[1][1] + w.z * viewProj[2][1] + w.w * viewProj[3][1];\n"
    "  clip.z = w.x * viewProj[0][2] + w.y * viewProj[1][2] + w.z * viewProj[2][2] + w.w * viewProj[3][2];\n"
    "  clip.w = w.x * viewProj[0][3] + w.y * viewProj[1][3] + w.z * viewProj[2][3] + w.w * viewProj[3][3];\n"
    "  VS_OUT o;\n"
    "  o.pos = clip;\n"
    "  o.normal = n;\n"
    "  o.uv = i.uv * (2.0 + frac(iid * 0.13));\n"
    "  o.color = float4(i.color.rgb * i.inst1.rgb, 1.0);\n"
    "  o.world = world;\n"
    "  return o;\n"
    "}\n";

static const char *kSceneHardcodedMatrixVs =
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct VS_IN {\n"
    "  float3 pos : POSITION;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float4 inst0 : TEXCOORD1;\n"
    "  float4 inst1 : TEXCOORD2;\n"
    "};\n"
    "struct VS_OUT {\n"
    "  float4 pos : SV_Position;\n"
    "  float3 normal : NORMAL;\n"
    "  float2 uv : TEXCOORD0;\n"
    "  float4 color : COLOR0;\n"
    "  float3 world : TEXCOORD3;\n"
    "};\n"
    "VS_OUT main(VS_IN i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  float phase = i.inst1.w + time * (0.7 + frac(iid * 0.017));\n"
    "  float s = sin(phase);\n"
    "  float c = cos(phase);\n"
    "  float3 p = i.pos * i.inst0.w;\n"
    "  float3 rotated = float3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);\n"
    "  float wave = sin(time * 2.0 + iid * 0.41) * 0.18;\n"
    "  float3 world = rotated + float3(i.inst0.x, i.inst0.y + wave, i.inst0.z);\n"
    "  float3 n = normalize(float3(i.normal.x * c + i.normal.z * s, i.normal.y, -i.normal.x * s + i.normal.z * c));\n"
    "  float4 w = float4(world, 1.0);\n"
    "  float4 clip;\n"
    "  clip.x = w.x * 0.88294816 + w.y * 0.0 + w.z * 0.0 + w.w * 0.0;\n"
    "  clip.y = w.x * 0.0 + w.y * 1.42242265 + w.z * 0.66379720 + w.w * 0.0;\n"
    "  clip.z = w.x * 0.0 + w.y * -0.42341474 + w.z * 0.90731728 + w.w * 16.47353745;\n"
    "  clip.w = w.x * 0.0 + w.y * -0.42288548 + w.z * 0.90618312 + w.w * 16.55294609;\n"
    "  VS_OUT o;\n"
    "  o.pos = clip;\n"
    "  o.normal = n;\n"
    "  o.uv = i.uv * (2.0 + frac(iid * 0.13));\n"
    "  o.color = float4(i.color.rgb * i.inst1.rgb, 1.0);\n"
    "  o.world = world;\n"
    "  return o;\n"
    "}\n";

static const char *kScenePs =
    "Texture2D baseTex : register(t0);\n"
    "Texture2D computeTex : register(t1);\n"
    "SamplerState samp0 : register(s0);\n"
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "struct PS_IN { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR0; float3 world : TEXCOORD3; };\n"
    "float4 main(PS_IN i) : SV_Target {\n"
    "  float3 n = normalize(i.normal);\n"
    "  float3 sunDir = normalize(float3(0.42, 0.70, -0.58));\n"
    "  float3 fillDir = normalize(float3(-0.9, 0.28, 0.5));\n"
    "  float longShadow = saturate(1.0 - smoothstep(-0.2, 1.9, i.world.y)) * smoothstep(-5.0, 9.0, i.world.z);\n"
    "  float diffuse = saturate(dot(n, sunDir)) * 1.12 + saturate(dot(n, fillDir)) * 0.28;\n"
    "  float2 uv0 = frac(i.uv + float2(time * 0.018, -time * 0.014));\n"
    "  float2 uv1 = frac(i.world.xz * 0.075 + float2(time * 0.035, time * 0.022));\n"
    "  float3 tex0 = baseTex.Sample(samp0, uv0).rgb;\n"
    "  float3 tex1 = computeTex.Sample(samp0, uv1).rgb;\n"
    "  float water = step(0.42, i.color.b) * step(i.color.r, 0.30);\n"
    "  float sky = step(0.50, i.color.b) * step(0.72, i.color.r);\n"
    "  float sun = step(0.72, i.color.r) * step(i.color.b, 0.22);\n"
    "  float sand = step(0.55, i.color.r) * step(0.36, i.color.g) * step(i.color.b, 0.55) * (1.0 - sun);\n"
    "  float3 waterTint = float3(0.00, 0.48, 0.82) + tex1.bgr * 0.28;\n"
    "  float waveLine = smoothstep(0.72, 1.0, sin(i.world.x * 2.7 + time * 2.9) * 0.5 + 0.5) * smoothstep(0.20, 1.0, water);\n"
    "  float foam = water * smoothstep(0.82, 1.0, sin(i.world.x * 5.3 + i.world.z * 2.4 - time * 4.1) * 0.5 + 0.5);\n"
    "  float3 sunrise = float3(1.0, 0.56, 0.22) * saturate(1.0 - abs(i.world.x) * 0.020);\n"
    "  float sunGlow = saturate(1.0 - length(i.world.xy - float2(0.0, 4.2)) * 0.13) * smoothstep(5.0, 10.5, i.world.z);\n"
    "  float3 color = i.color.rgb * (0.26 + diffuse) + tex0 * 0.12 + tex1 * (0.15 + water * 0.34);\n"
    "  color = lerp(color, waterTint * (0.78 + diffuse * 0.38) + foam * 0.32 + waveLine * float3(0.18, 0.72, 0.92), water * 0.72);\n"
    "  color = lerp(color, float3(0.35, 0.62, 0.95) + sunrise * 0.55 + tex1.gbr * 0.045, sky * 0.82);\n"
    "  color = lerp(color, float3(1.0, 0.52, 0.12) + tex1.rgb * 0.08, sun * 0.85);\n"
    "  color = lerp(color, i.color.rgb * (0.85 + diffuse * 0.34) + tex0 * 0.08, sand * 0.48);\n"
    "  color += sunrise * sunGlow * 0.95;\n"
    "  color *= lerp(1.0 - longShadow * 0.26, 1.10, water);\n"
    "  [unroll] for (int k = 0; k < 8; ++k) {\n"
    "    float f = sin(dot(color, float3(3.1 + k, 2.2, 1.7)) + time * (0.2 + k * 0.03));\n"
    "    color = saturate(color * (0.94 + 0.026 * f) + tex1.bgr * (0.011 + water * 0.010));\n"
    "  }\n"
    "  return float4(color, 1.0);\n"
    "}\n";

static const char *kSceneBrightPs =
    "struct PS_IN { float4 pos : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; float4 color : COLOR0; float3 world : TEXCOORD3; };\n"
    "float4 main(PS_IN i) : SV_Target {\n"
    "  return float4(saturate(i.color.rgb + float3(0.12, 0.05, 0.10)), 1.0);\n"
    "}\n";

static const char *kPostVs =
    "struct OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "OUT main(uint vid : SV_VertexID) {\n"
    "  float2 p[4] = { float2(-1,-1), float2(-1,1), float2(1,-1), float2(1,1) };\n"
    "  float2 uv[4] = { float2(0,1), float2(0,0), float2(1,1), float2(1,0) };\n"
    "  OUT o; o.pos = float4(p[vid], 0.0, 1.0); o.uv = uv[vid]; return o;\n"
    "}\n";

static const char *kPostPs =
    "Texture2D baseTex : register(t0);\n"
    "Texture2D computeTex : register(t1);\n"
    "SamplerState samp0 : register(s0);\n"
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float2 wobble = uv + 0.006 * sin(float2(uv.y, uv.x) * 70.0 + time);\n"
    "  float3 a = computeTex.Sample(samp0, wobble).rgb;\n"
    "  float3 b = baseTex.Sample(samp0, frac(uv * 4.0 + time * 0.02)).rgb;\n"
    "  float scan = 0.018 * sin(pos.y * 0.75 + time * 12.0);\n"
    "  float vignette = saturate(dot(uv - 0.5, uv - 0.5) * 1.6);\n"
    "  return float4(saturate(a * 0.075 + b * 0.035 + scan + vignette * 0.018), 0.18);\n"
    "}\n";

static const char *kBrightPostPs =
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  return float4(1.0, 0.18, 0.85, 1.0);\n"
    "}\n";

static const char *kSplashPs =
    "Texture2D baseTex : register(t0);\n"
    "Texture2D computeTex : register(t1);\n"
    "SamplerState samp0 : register(s0);\n"
    "cbuffer Scene : register(b0) { row_major float4x4 viewProj; float time; float frame; float width; float height; };\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float2 centered = uv - 0.5;\n"
    "  float3 logo = baseTex.Sample(samp0, uv).rgb;\n"
    "  float3 noise = computeTex.Sample(samp0, frac(uv * 0.55 + time * float2(0.025, -0.018))).rgb;\n"
    "  float vignette = saturate(1.20 - dot(centered, centered) * 1.85);\n"
    "  float scan = 0.018 * sin(pos.y * 1.45 + time * 18.0);\n"
    "  float title = smoothstep(0.86, 0.99, min(min(logo.r, logo.g), logo.b));\n"
    "  float panel = smoothstep(0.18, 0.10, abs(centered.y)) * smoothstep(0.46, 0.39, abs(centered.x));\n"
    "  float2 rayVec = float2(centered.x + 0.30, centered.y - 0.06);\n"
    "  float ray = pow(saturate(dot(rayVec / max(length(rayVec), 0.001), normalize(float2(1.0, -0.16)))), 24.0);\n"
    "  float border = step(0.47, abs(centered.x)) + step(0.43, abs(centered.y));\n"
    "  float3 color = lerp(float3(0.015, 0.030, 0.060), float3(0.10, 0.28, 0.45), uv.y);\n"
    "  color += noise * 0.060 + ray * float3(0.95, 0.43, 0.12) * 0.34 + scan;\n"
    "  color = lerp(color, float3(0.02, 0.05, 0.08), panel * 0.72);\n"
    "  color = lerp(color, float3(1.0, 0.92, 0.76), title);\n"
    "  color = lerp(color, float3(0.95, 0.08, 0.10), saturate(border * 0.10));\n"
    "  color *= vignette;\n"
    "  return float4(color, 1.0);\n"
    "}\n";

static bool createComputePipeline(Harness &h, ID3D12RootSignature **root,
                                  ID3D12PipelineState **pso) {
  ID3DBlob *cs = nullptr;
  if (!compileShader("stress compute CS", kComputeShader, "main", "cs_5_0", &cs))
    return false;

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs = {};
  rs.NumParameters = 2;
  rs.pParameters = params;
  if (!createRootSignature(h, rs, root)) {
    releaseIf(cs);
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = *root;
  desc.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
  HRESULT hr = h.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(pso));
  releaseIf(cs);
  return checkHr("CreateComputePipelineState stress", hr);
}

static bool createGraphicsPipeline(Harness &h, bool post, ID3D12RootSignature **root,
                                   ID3D12PipelineState **pso,
                                   const char *overridePs = nullptr,
                                   const char *overrideVs = nullptr) {
  ID3DBlob *vs = nullptr;
  ID3DBlob *ps = nullptr;
  if (!compileShader(post ? "stress post VS" : "stress scene VS",
                     overrideVs ? overrideVs : (post ? kPostVs : kSceneVs),
                     "main", "vs_5_0", &vs) ||
      !compileShader(post ? "stress post PS" : "stress scene PS",
                     overridePs ? overridePs : (post ? kPostPs : kScenePs),
                     "main", "ps_5_0", &ps)) {
    releaseIf(vs);
    releaseIf(ps);
    return false;
  }

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 2;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &range;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
  sampler.ShaderRegister = 0;
  sampler.RegisterSpace = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;

  D3D12_ROOT_SIGNATURE_DESC rs = {};
  rs.NumParameters = 2;
  rs.pParameters = params;
  rs.NumStaticSamplers = 1;
  rs.pStaticSamplers = &sampler;
  rs.Flags = post ? D3D12_ROOT_SIGNATURE_FLAG_NONE
                  : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (!createRootSignature(h, rs, root)) {
    releaseIf(vs);
    releaseIf(ps);
    return false;
  }

  D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,
       D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
      {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16,
       D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
  };

  D3D12_INPUT_LAYOUT_DESC inputLayout = {};
  if (!post) {
    inputLayout.pInputElementDescs = layout;
    inputLayout.NumElements = 6;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = *root;
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.InputLayout = inputLayout;
  desc.PrimitiveTopologyType = post ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
                                    : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.SampleMask = UINT_MAX;
  const bool sceneNoCull = !post && envEnabled("VKD3D_STRESS_SCENE_NO_CULL");
  const bool sceneNoDepth = !post && envEnabled("VKD3D_STRESS_SCENE_NO_DEPTH");
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode =
      (post || sceneNoCull) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  if (post) {
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  }
  desc.DepthStencilState.DepthEnable = (!post && !sceneNoDepth) ? TRUE : FALSE;
  desc.DepthStencilState.DepthWriteMask =
      (post || sceneNoDepth) ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
  desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.DSVFormat = (post || sceneNoDepth) ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_D32_FLOAT;

  HRESULT hr = h.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso));
  releaseIf(vs);
  releaseIf(ps);
  return checkHr(overridePs ? "CreateGraphicsPipelineState splash"
                            : (post ? "CreateGraphicsPipelineState post"
                                    : "CreateGraphicsPipelineState scene"),
                 hr);
}

static void pushVertex(Vertex *verts, UINT &count, Vec3 p, Vec3 n, float u, float v,
                       float r, float g, float b) {
  if (count >= kMaxSceneVertices)
    return;
  verts[count++] = {p.x, p.y, p.z, n.x, n.y, n.z, u, v, r, g, b, 1.0f};
}

static void pushTri(Vertex *verts, UINT &count, Vec3 a, Vec3 b, Vec3 c,
                    float r, float g, float bl) {
  Vec3 n = normalize(cross(sub(b, a), sub(c, a)));
  pushVertex(verts, count, a, n, 0.0f, 1.0f, r, g, bl);
  pushVertex(verts, count, b, n, 0.0f, 0.0f, r, g, bl);
  pushVertex(verts, count, c, n, 1.0f, 0.0f, r, g, bl);
}

static void pushQuad(Vertex *verts, UINT &count, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
                     float r, float g, float bl) {
  Vec3 n = normalize(cross(sub(b, a), sub(c, a)));
  pushVertex(verts, count, a, n, 0.0f, 1.0f, r, g, bl);
  pushVertex(verts, count, b, n, 0.0f, 0.0f, r, g, bl);
  pushVertex(verts, count, c, n, 1.0f, 0.0f, r, g, bl);
  pushVertex(verts, count, a, n, 0.0f, 1.0f, r, g, bl);
  pushVertex(verts, count, c, n, 1.0f, 0.0f, r, g, bl);
  pushVertex(verts, count, d, n, 1.0f, 1.0f, r, g, bl);
}

static void pushDisc(Vertex *verts, UINT &count, Vec3 center, float radius,
                     float r, float g, float bl) {
  Vec3 n = {0.0f, 0.0f, -1.0f};
  for (int i = 0; i < 48; i++) {
    float a0 = static_cast<float>(i) * 6.283185307f / 48.0f;
    float a1 = static_cast<float>(i + 1) * 6.283185307f / 48.0f;
    Vec3 p0 = {center.x + std::cos(a0) * radius, center.y + std::sin(a0) * radius,
               center.z};
    Vec3 p1 = {center.x + std::cos(a1) * radius, center.y + std::sin(a1) * radius,
               center.z};
    pushVertex(verts, count, center, n, 0.5f, 0.5f, r, g, bl);
    pushVertex(verts, count, p0, n, 0.0f, 1.0f, r, g, bl);
    pushVertex(verts, count, p1, n, 1.0f, 1.0f, r, g, bl);
  }
}

static void pushBox(Vertex *verts, UINT &count, Vec3 minp, Vec3 maxp, float r,
                    float g, float bl) {
  Vec3 p000 = {minp.x, minp.y, minp.z};
  Vec3 p001 = {minp.x, minp.y, maxp.z};
  Vec3 p010 = {minp.x, maxp.y, minp.z};
  Vec3 p011 = {minp.x, maxp.y, maxp.z};
  Vec3 p100 = {maxp.x, minp.y, minp.z};
  Vec3 p101 = {maxp.x, minp.y, maxp.z};
  Vec3 p110 = {maxp.x, maxp.y, minp.z};
  Vec3 p111 = {maxp.x, maxp.y, maxp.z};
  pushQuad(verts, count, p000, p010, p110, p100, r, g, bl);
  pushQuad(verts, count, p101, p111, p011, p001, r, g, bl);
  pushQuad(verts, count, p001, p011, p010, p000, r * 0.86f, g * 0.86f, bl * 0.86f);
  pushQuad(verts, count, p100, p110, p111, p101, r * 0.92f, g * 0.92f, bl * 0.92f);
  pushQuad(verts, count, p010, p011, p111, p110, r * 1.08f, g * 1.08f, bl * 1.08f);
  pushQuad(verts, count, p001, p000, p100, p101, r * 0.78f, g * 0.78f, bl * 0.78f);
}

static void pushSky(Vertex *verts, UINT &count) {
  pushQuad(verts, count, {-28.0f, -1.5f, 12.5f}, {-28.0f, 9.5f, 12.5f},
           {28.0f, 9.5f, 12.5f}, {28.0f, -1.5f, 12.5f}, 0.78f, 0.62f,
           0.58f);
  pushQuad(verts, count, {-28.0f, 2.2f, 12.35f}, {-28.0f, 9.8f, 12.35f},
           {28.0f, 9.8f, 12.35f}, {28.0f, 2.2f, 12.35f}, 0.36f, 0.54f,
           0.88f);
}

static void pushSunRays(Vertex *verts, UINT &count) {
  Vec3 center = {0.0f, 4.2f, 8.72f};
  for (int i = 0; i < 18; i++) {
    float a = static_cast<float>(i) * 6.283185307f / 18.0f;
    float a0 = a - 0.035f - 0.018f * static_cast<float>(i & 1);
    float a1 = a + 0.035f + 0.011f * static_cast<float>((i + 1) & 1);
    float inner = 1.65f;
    float outer = 3.0f + 0.75f * std::sin(static_cast<float>(i) * 1.31f);
    Vec3 p0 = {center.x + std::cos(a0) * inner, center.y + std::sin(a0) * inner,
               center.z};
    Vec3 p1 = {center.x + std::cos(a) * outer, center.y + std::sin(a) * outer,
               center.z};
    Vec3 p2 = {center.x + std::cos(a1) * inner, center.y + std::sin(a1) * inner,
               center.z};
    pushTri(verts, count, p0, p1, p2, 1.0f, 0.58f, 0.16f);
  }
}

static void pushBeachWater(Vertex *verts, UINT &count) {
  const int cols = 64;
  const int rows = 32;
  const float x0 = -22.0f;
  const float x1 = 22.0f;
  const float z0 = -16.0f;
  const float z1 = 3.5f;
  for (int y = 0; y < rows; y++) {
    float v0 = static_cast<float>(y) / rows;
    float v1 = static_cast<float>(y + 1) / rows;
    for (int x = 0; x < cols; x++) {
      float u0 = static_cast<float>(x) / cols;
      float u1 = static_cast<float>(x + 1) / cols;
      Vec3 a = {x0 + (x1 - x0) * u0, -1.10f, z0 + (z1 - z0) * v0};
      Vec3 b = {x0 + (x1 - x0) * u0, -1.10f, z0 + (z1 - z0) * v1};
      Vec3 c = {x0 + (x1 - x0) * u1, -1.10f, z0 + (z1 - z0) * v1};
      Vec3 d = {x0 + (x1 - x0) * u1, -1.10f, z0 + (z1 - z0) * v0};
      float stripe = 0.035f * static_cast<float>((x + y) & 1);
      float depth = 0.10f * v0;
      pushQuad(verts, count, a, b, c, d, 0.02f + stripe, 0.34f + depth + stripe,
               0.64f + depth + stripe);
    }
  }
}

static void pushFoam(Vertex *verts, UINT &count) {
  for (int i = 0; i < 18; i++) {
    float x = -20.5f + static_cast<float>(i) * 2.45f;
    float z = 2.52f + 0.22f * std::sin(static_cast<float>(i) * 0.83f);
    float w = 1.35f + 0.42f * std::sin(static_cast<float>(i) * 1.7f);
    Vec3 a = {x, -0.985f, z};
    Vec3 b = {x + 0.24f, -0.982f, z + 0.38f};
    Vec3 c = {x + w, -0.984f, z + 0.30f};
    Vec3 d = {x + w - 0.18f, -0.986f, z - 0.12f};
    pushQuad(verts, count, a, b, c, d, 0.78f, 0.94f, 0.95f);
  }
}

static void pushPalmTrunk(Vertex *verts, UINT &count) {
  const int levels = 12;
  const int sides = 8;
  for (int y = 0; y < levels; y++) {
    float t0 = static_cast<float>(y) / levels;
    float t1 = static_cast<float>(y + 1) / levels;
    Vec3 c0 = {-6.8f + t0 * 0.9f, -1.0f + t0 * 5.7f, 1.4f - t0 * 0.35f};
    Vec3 c1 = {-6.8f + t1 * 0.9f, -1.0f + t1 * 5.7f, 1.4f - t1 * 0.35f};
    float r0 = 0.34f - t0 * 0.12f;
    float r1 = 0.34f - t1 * 0.12f;
    for (int s = 0; s < sides; s++) {
      float a0 = static_cast<float>(s) * 6.283185307f / sides;
      float a1 = static_cast<float>(s + 1) * 6.283185307f / sides;
      Vec3 p0 = add(c0, {std::cos(a0) * r0, 0.0f, std::sin(a0) * r0});
      Vec3 p1 = add(c1, {std::cos(a0) * r1, 0.0f, std::sin(a0) * r1});
      Vec3 p2 = add(c1, {std::cos(a1) * r1, 0.0f, std::sin(a1) * r1});
      Vec3 p3 = add(c0, {std::cos(a1) * r0, 0.0f, std::sin(a1) * r0});
      pushQuad(verts, count, p0, p1, p2, p3, 0.42f, 0.20f, 0.09f);
    }
  }
}

static void pushPalmFronds(Vertex *verts, UINT &count) {
  Vec3 top = {-5.9f, 4.85f, 1.05f};
  for (int i = 0; i < 18; i++) {
    float a = static_cast<float>(i) * 6.283185307f / 18.0f;
    float len = 2.35f + 0.72f * std::sin(static_cast<float>(i) * 1.7f);
    Vec3 dir = {std::cos(a), 0.18f * std::sin(a * 2.0f), std::sin(a)};
    Vec3 side = normalize(cross(dir, {0.0f, 1.0f, 0.0f}));
    Vec3 tip = add(top, add(mul(dir, len), {0.0f, -0.35f, 0.0f}));
    Vec3 mid = add(top, mul(dir, len * 0.52f));
    Vec3 a0 = add(top, mul(side, 0.20f));
    Vec3 a1 = sub(top, mul(side, 0.20f));
    Vec3 b0 = add(mid, mul(side, 0.36f));
    Vec3 b1 = sub(mid, mul(side, 0.36f));
    pushTri(verts, count, a0, b0, tip, 0.08f, 0.48f, 0.12f);
    pushTri(verts, count, a1, tip, b1, 0.05f, 0.36f, 0.08f);
    Vec3 barb0 = add(mid, add(mul(dir, 0.25f), mul(side, 0.62f)));
    Vec3 barb1 = add(mid, add(mul(dir, 0.45f), mul(side, -0.62f)));
    pushTri(verts, count, mid, barb0, tip, 0.10f, 0.56f, 0.15f);
    pushTri(verts, count, mid, tip, barb1, 0.04f, 0.30f, 0.06f);
  }
}

static void pushBoat(Vertex *verts, UINT &count) {
  pushQuad(verts, count, {-2.35f, -0.72f, -3.25f}, {-1.55f, -0.12f, -3.55f},
           {1.55f, -0.12f, -3.55f}, {2.35f, -0.72f, -3.25f}, 0.58f, 0.18f, 0.07f);
  pushQuad(verts, count, {-2.10f, -0.76f, -2.58f}, {-1.35f, -0.20f, -2.34f},
           {1.35f, -0.20f, -2.34f}, {2.10f, -0.76f, -2.58f}, 0.42f, 0.12f, 0.05f);
  pushQuad(verts, count, {-2.35f, -0.72f, -3.25f}, {-2.10f, -0.76f, -2.58f},
           {-1.35f, -0.20f, -2.34f}, {-1.55f, -0.12f, -3.55f}, 0.74f, 0.30f, 0.12f);
  pushQuad(verts, count, {1.55f, -0.12f, -3.55f}, {1.35f, -0.20f, -2.34f},
           {2.10f, -0.76f, -2.58f}, {2.35f, -0.72f, -3.25f}, 0.36f, 0.10f, 0.04f);
  pushBox(verts, count, {-0.75f, -0.16f, -3.08f}, {0.78f, 0.38f, -2.55f},
          0.72f, 0.52f, 0.32f);
  pushBox(verts, count, {-0.07f, -0.10f, -3.02f}, {0.08f, 2.22f, -2.82f},
          0.20f, 0.12f, 0.07f);
  pushTri(verts, count, {0.10f, 2.10f, -2.86f}, {0.10f, 0.10f, -2.86f},
          {1.55f, 0.45f, -2.86f}, 1.0f, 0.90f, 0.66f);
  pushTri(verts, count, {-0.12f, 1.92f, -2.90f}, {-0.12f, 0.10f, -2.90f},
          {-1.38f, 0.34f, -2.90f}, 0.90f, 0.82f, 0.62f);
  pushTri(verts, count, {-2.35f, -0.72f, -2.88f}, {-2.92f, -0.56f, -2.92f},
          {-2.10f, -0.40f, -2.70f}, 0.64f, 0.20f, 0.08f);
  pushTri(verts, count, {2.35f, -0.72f, -2.88f}, {2.92f, -0.56f, -2.92f},
          {2.10f, -0.40f, -2.70f}, 0.36f, 0.10f, 0.04f);
}

static void pushJaggedRocks(Vertex *verts, UINT &count) {
  for (int i = 0; i < 16; i++) {
    float x = -12.0f + static_cast<float>(i) * 1.55f;
    float z = 2.9f + 0.95f * std::sin(static_cast<float>(i) * 0.73f);
    float h = 0.25f + 0.55f * static_cast<float>((i * 5) % 9) / 8.0f;
    Vec3 base0 = {x - 0.42f, -0.98f, z - 0.25f};
    Vec3 base1 = {x + 0.50f, -0.98f, z - 0.12f};
    Vec3 base2 = {x + 0.22f, -0.98f, z + 0.46f};
    Vec3 base3 = {x - 0.55f, -0.98f, z + 0.20f};
    Vec3 tip = {x + 0.16f * std::sin(static_cast<float>(i)), -0.98f + h, z + 0.08f};
    float shade = 0.45f + 0.20f * static_cast<float>(i & 1);
    pushTri(verts, count, base0, base1, tip, shade, shade * 0.88f, shade * 0.72f);
    pushTri(verts, count, base1, base2, tip, shade * 0.72f, shade * 0.70f, shade * 0.64f);
    pushTri(verts, count, base2, base3, tip, shade * 0.52f, shade * 0.56f, shade * 0.55f);
    pushTri(verts, count, base3, base0, tip, shade * 0.64f, shade * 0.60f, shade * 0.50f);
  }
}

static UINT makeBeachScene(Vertex *verts) {
  UINT count = 0;
  pushSky(verts, count);
  pushSunRays(verts, count);
  pushBeachWater(verts, count);
  pushFoam(verts, count);
  pushQuad(verts, count, {-22.0f, -1.02f, 3.0f}, {-22.0f, -1.02f, 16.0f},
           {22.0f, -1.02f, 16.0f}, {22.0f, -1.02f, 3.0f}, 0.82f, 0.68f, 0.38f);
  pushQuad(verts, count, {-22.0f, -1.03f, 2.4f}, {-22.0f, -1.02f, 4.2f},
           {22.0f, -1.02f, 4.2f}, {22.0f, -1.03f, 2.4f}, 0.93f, 0.82f, 0.52f);
  pushJaggedRocks(verts, count);
  pushDisc(verts, count, {0.0f, 4.2f, 8.55f}, 1.65f, 1.0f, 0.43f, 0.12f);
  pushPalmTrunk(verts, count);
  pushPalmFronds(verts, count);
  pushBoat(verts, count);
  std::printf("[INFO] beach scene vertices=%u\n", count);
  return count;
}

static const uint8_t *glyphColumns(char ch) {
  static const uint8_t S[5] = {0x46, 0x49, 0x49, 0x49, 0x31};
  static const uint8_t T[5] = {0x01, 0x01, 0x7f, 0x01, 0x01};
  static const uint8_t R[5] = {0x7f, 0x09, 0x19, 0x29, 0x46};
  static const uint8_t E[5] = {0x7f, 0x49, 0x49, 0x49, 0x41};
  static const uint8_t A[5] = {0x7e, 0x09, 0x09, 0x09, 0x7e};
  static const uint8_t I[5] = {0x41, 0x41, 0x7f, 0x41, 0x41};
  static const uint8_t N[5] = {0x7f, 0x06, 0x18, 0x60, 0x7f};
  static const uint8_t G[5] = {0x3e, 0x41, 0x49, 0x49, 0x7a};
  static const uint8_t blank[5] = {0, 0, 0, 0, 0};
  switch (ch) {
  case 'S': return S;
  case 'T': return T;
  case 'R': return R;
  case 'E': return E;
  case 'A': return A;
  case 'I': return I;
  case 'N': return N;
  case 'G': return G;
  default: return blank;
  }
}

static void drawText(uint32_t *texels, int ox, int oy, int scale, const char *text) {
  int cursor = ox;
  for (const char *p = text; *p; ++p) {
    if (*p == ' ') {
      cursor += 4 * scale;
      continue;
    }
    const uint8_t *cols = glyphColumns(*p);
    for (int x = 0; x < 5; x++) {
      for (int y = 0; y < 7; y++) {
        if (!(cols[x] & (1u << y)))
          continue;
        for (int yy = 0; yy < scale; yy++) {
          for (int xx = 0; xx < scale; xx++) {
            int tx = cursor + x * scale + xx;
            int ty = oy + y * scale + yy;
            if (tx >= 0 && tx < static_cast<int>(kTextureSize) &&
                ty >= 0 && ty < static_cast<int>(kTextureSize))
              texels[ty * kTextureSize + tx] = 0xffffffffu;
          }
        }
      }
    }
    cursor += 6 * scale;
  }
}

static ReadbackStats analyzeReadback(ID3D12Resource *readback, UINT pitch) {
  ReadbackStats stats = {};
  void *mapped = nullptr;
  HRESULT hr = readback->Map(0, nullptr, &mapped);
  if (FAILED(hr) || !mapped) {
    std::fprintf(stderr, "[FAIL] readback Map: 0x%08lx\n", hr);
    return stats;
  }
  const auto *bytes = static_cast<const uint8_t *>(mapped);
  for (UINT y = 0; y < kHeight; y += 3) {
    const uint8_t *row = bytes + y * pitch;
    for (UINT x = 0; x < kWidth; x += 3) {
      const uint8_t *p = row + x * 4;
      uint8_t maxChannel = p[0] > p[1] ? p[0] : p[1];
      maxChannel = maxChannel > p[2] ? maxChannel : p[2];
      uint8_t minChannel = p[0] < p[1] ? p[0] : p[1];
      minChannel = minChannel < p[2] ? minChannel : p[2];
      stats.checksum ^= p[0];
      stats.checksum *= 1099511628211ull;
      stats.checksum ^= p[1];
      stats.checksum *= 1099511628211ull;
      stats.checksum ^= p[2];
      stats.checksum *= 1099511628211ull;
      if (maxChannel > 70)
        stats.brightPixels++;
      if (maxChannel > 80 && (maxChannel - minChannel) > 18)
        stats.chromaPixels++;
    }
  }
  readback->Unmap(0, nullptr);
  return stats;
}

static void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  cmd->ResourceBarrier(1, &b);
}

static bool runStress(Harness &h, UINT seconds) {
  float splashSeconds = envFloat("VKD3D_STRESS_SPLASH_SECONDS", kSplashSeconds);
  const bool sceneOnly = envEnabled("VKD3D_STRESS_SCENE_ONLY");
  const bool postOnly = envEnabled("VKD3D_STRESS_POST_ONLY");
  const bool skipPost = envEnabled("VKD3D_STRESS_SKIP_POST");
  const bool brightPost = envEnabled("VKD3D_STRESS_BRIGHT_POST");
  const bool sceneNoDepth = envEnabled("VKD3D_STRESS_SCENE_NO_DEPTH");
  const bool sceneBrightPs = envEnabled("VKD3D_STRESS_SCENE_BRIGHT_PS");
  const bool sceneClipVs = envEnabled("VKD3D_STRESS_SCENE_CLIP_VS");
  const bool sceneClipCbufferVs = envEnabled("VKD3D_STRESS_SCENE_CLIP_CBUFFER_VS");
  const bool sceneCbufferGateVs = envEnabled("VKD3D_STRESS_SCENE_CBUFFER_GATE_VS");
  const bool sceneWorldVs = envEnabled("VKD3D_STRESS_SCENE_WORLD_VS");
  const bool sceneManualMulVs = envEnabled("VKD3D_STRESS_SCENE_MANUAL_MUL_VS");
  const bool sceneScalarMulVs = envEnabled("VKD3D_STRESS_SCENE_SCALAR_MUL_VS");
  const bool sceneHardcodedMatrixVs = envEnabled("VKD3D_STRESS_SCENE_HARDCODED_MATRIX_VS");
  const bool skipReadback = envEnabled("VKD3D_STRESS_SKIP_READBACK");
  if (envEnabled("VKD3D_STRESS_SCENE_ONLY"))
    splashSeconds = 0.0f;
  std::printf("[INFO] stress config splash_seconds=%.3f scene_only=%u post_only=%u skip_post=%u bright_post=%u scene_no_depth=%u scene_bright_ps=%u scene_clip_vs=%u scene_clip_cbuffer_vs=%u scene_cbuffer_gate_vs=%u scene_world_vs=%u scene_manual_mul_vs=%u scene_scalar_mul_vs=%u scene_hardcoded_matrix_vs=%u skip_readback=%u\n",
              splashSeconds, sceneOnly ? 1u : 0u, postOnly ? 1u : 0u,
              skipPost ? 1u : 0u, brightPost ? 1u : 0u,
              sceneNoDepth ? 1u : 0u, sceneBrightPs ? 1u : 0u,
              sceneClipVs ? 1u : 0u, sceneClipCbufferVs ? 1u : 0u,
              sceneCbufferGateVs ? 1u : 0u,
              sceneWorldVs ? 1u : 0u, sceneManualMulVs ? 1u : 0u,
              sceneScalarMulVs ? 1u : 0u,
              sceneHardcodedMatrixVs ? 1u : 0u,
              skipReadback ? 1u : 0u);

  Vertex sceneVertices[kMaxSceneVertices] = {};
  UINT sceneVertexCount = makeBeachScene(sceneVertices);
  Vertex microTriangle[3] = {
      {-0.025f, -0.020f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.1f, 0.2f, 1.0f},
      {0.030f, -0.015f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 0.0f, 0.1f, 1.0f, 0.3f, 1.0f},
      {0.002f, 0.035f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f, 0.2f, 0.4f, 1.0f, 1.0f},
  };

  Instance instances[kInstanceCount] = {};
  for (UINT i = 0; i < kInstanceCount; i++) {
    UINT x = i % 12;
    UINT z = i / 12;
    float fx = static_cast<float>(x) - 5.5f;
    float fz = static_cast<float>(z) - 5.5f;
    instances[i] = {fx * 1.45f,
                    std::sin(static_cast<float>(i) * 0.37f) * 0.5f,
                    fz * 1.45f,
                    0.28f + 0.06f * static_cast<float>((i % 5)),
                    0.35f + 0.65f * static_cast<float>((i * 13) % 17) / 16.0f,
                    0.25f + 0.75f * static_cast<float>((i * 7) % 19) / 18.0f,
                    0.30f + 0.70f * static_cast<float>((i * 3) % 23) / 22.0f,
                    static_cast<float>(i) * 0.19f};
  }
  instances[0] = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f};

  uint32_t texels[kTextureSize * kTextureSize] = {};
  for (UINT y = 0; y < kTextureSize; y++) {
    for (UINT x = 0; x < kTextureSize; x++) {
      uint8_t weave = static_cast<uint8_t>(((x * 13u + y * 7u + (x ^ y)) & 31u) * 2u);
      uint8_t r = static_cast<uint8_t>(24u + (x * 72u) / (kTextureSize - 1) + weave);
      uint8_t g = static_cast<uint8_t>(34u + (y * 84u) / (kTextureSize - 1) + weave);
      uint8_t b = static_cast<uint8_t>(58u + (((x ^ y) & 63u) * 2u));
      texels[y * kTextureSize + x] =
          0xff000000u | (static_cast<uint32_t>(b) << 16) |
          (static_cast<uint32_t>(g) << 8) | r;
    }
  }
  drawText(texels, 82, 232, 3, "STRESS TEST STARTING");

  ID3D12Resource *vertexBuffer =
      makeUploadBuffer(h, sceneVertices, sceneVertexCount * sizeof(Vertex),
                       "Map stress beach scene vertex buffer");
  ID3D12Resource *microVertexBuffer =
      makeUploadBuffer(h, microTriangle, sizeof(microTriangle), "Map stress micro-triangle VB");
  ID3D12Resource *instanceBuffer =
      makeUploadBuffer(h, instances, sizeof(instances), "Map stress instance buffer");
  ID3D12Resource *constantBuffer =
      makeBuffer(h, D3D12_HEAP_TYPE_UPLOAD, alignUp(sizeof(SceneConstants), 256),
                 D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *baseTexture =
      makeTexture2D(h, DXGI_FORMAT_R8G8B8A8_UNORM, kTextureSize, kTextureSize,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource *computeTexture =
      makeTexture2D(h, DXGI_FORMAT_R8G8B8A8_UNORM, kTextureSize, kTextureSize,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  if (!vertexBuffer || !microVertexBuffer || !instanceBuffer || !constantBuffer ||
      !baseTexture || !computeTexture)
    return false;

  UINT64 uploadSize = 0;
  D3D12_RESOURCE_DESC texDesc = baseTexture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT numRows = 0;
  UINT64 rowSize = 0;
  h.device->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows,
                                  &rowSize, &uploadSize);
  ID3D12Resource *textureUpload =
      makeBuffer(h, D3D12_HEAP_TYPE_UPLOAD, uploadSize, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!textureUpload)
    return false;
  void *mapped = nullptr;
  if (!checkHr("Map texture upload", textureUpload->Map(0, nullptr, &mapped)))
    return false;
  auto *uploadDst = static_cast<uint8_t *>(mapped) + footprint.Offset;
  for (UINT y = 0; y < kTextureSize; y++)
    std::memcpy(uploadDst + y * footprint.Footprint.RowPitch, texels + y * kTextureSize,
                kTextureSize * sizeof(uint32_t));
  textureUpload->Unmap(0, nullptr);

  SceneConstants *cbMapped = nullptr;
  if (!checkHr("Map scene constants", constantBuffer->Map(0, nullptr,
                                                          reinterpret_cast<void **>(&cbMapped))))
    return false;

  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  h.device->CreateShaderResourceView(baseTexture, &srv, cpuSrv(h, 0));
  h.device->CreateShaderResourceView(computeTexture, &srv, cpuSrv(h, 1));

  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  h.device->CreateUnorderedAccessView(computeTexture, nullptr, &uav, cpuSrv(h, 2));

  ID3D12RootSignature *computeRoot = nullptr;
  ID3D12PipelineState *computePso = nullptr;
  ID3D12RootSignature *sceneRoot = nullptr;
  ID3D12PipelineState *scenePso = nullptr;
  ID3D12RootSignature *postRoot = nullptr;
  ID3D12PipelineState *postPso = nullptr;
  ID3D12RootSignature *splashRoot = nullptr;
  ID3D12PipelineState *splashPso = nullptr;
  if (!createComputePipeline(h, &computeRoot, &computePso) ||
      !createGraphicsPipeline(h, false, &sceneRoot, &scenePso,
                              sceneBrightPs ? kSceneBrightPs : nullptr,
                             sceneHardcodedMatrixVs
                                 ? kSceneHardcodedMatrixVs
                                 : (sceneScalarMulVs
                                        ? kSceneScalarMulVs
                                        : (sceneManualMulVs
                                               ? kSceneManualMulVs
                                               : (sceneWorldVs
                                                      ? kSceneWorldVs
                                                      : (sceneClipCbufferVs
                                                             ? kSceneClipCbufferVs
                                                             : (sceneCbufferGateVs
                                                                    ? kSceneCbufferGateVs
                                                                    : (sceneClipVs ? kSceneClipVs : nullptr))))))) ||
      !createGraphicsPipeline(h, true, &postRoot, &postPso,
                              brightPost ? kBrightPostPs : nullptr) ||
      !createGraphicsPipeline(h, true, &splashRoot, &splashPso, kSplashPs))
    return false;

  D3D12_VERTEX_BUFFER_VIEW vbvs[2] = {};
  vbvs[0].BufferLocation = vertexBuffer->GetGPUVirtualAddress();
  vbvs[0].SizeInBytes = sceneVertexCount * sizeof(Vertex);
  vbvs[0].StrideInBytes = sizeof(Vertex);
  vbvs[1].BufferLocation = instanceBuffer->GetGPUVirtualAddress();
  vbvs[1].SizeInBytes = sizeof(instances);
  vbvs[1].StrideInBytes = sizeof(Instance);
  D3D12_VERTEX_BUFFER_VIEW microVbvs[2] = {};
  microVbvs[0].BufferLocation = microVertexBuffer->GetGPUVirtualAddress();
  microVbvs[0].SizeInBytes = sizeof(microTriangle);
  microVbvs[0].StrideInBytes = sizeof(Vertex);
  microVbvs[1] = vbvs[1];

  const UINT readbackPitch = alignUp(kWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  ID3D12Resource *readback =
      makeBuffer(h, D3D12_HEAP_TYPE_READBACK, static_cast<UINT64>(readbackPitch) * kHeight,
                 D3D12_RESOURCE_STATE_COPY_DEST);
  if (!readback)
    return false;

  HRESULT hr = h.allocator->Reset();
  if (!checkHr("Upload allocator Reset", hr))
    return false;
  hr = h.cmd->Reset(h.allocator, nullptr);
  if (!checkHr("Upload command list Reset", hr))
    return false;
  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = baseTexture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = textureUpload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  h.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  barrier(h.cmd, baseTexture, D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  hr = h.cmd->Close();
  if (!checkHr("Upload command list Close", hr))
    return false;
  ID3D12CommandList *uploadLists[] = {h.cmd};
  h.queue->ExecuteCommandLists(1, uploadLists);
  if (!waitForGpu(h))
    return false;

  LARGE_INTEGER freq = {};
  LARGE_INTEGER start = {};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);

  UINT frame = 0;
  bool ok = true;
  while (ok && pumpMessages()) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    float elapsed = static_cast<float>(now.QuadPart - start.QuadPart) /
                    static_cast<float>(freq.QuadPart);
    if (elapsed >= static_cast<float>(seconds))
      break;

    Vec3 eye = {std::sin(elapsed * 0.37f) * 12.0f, 7.0f,
                -18.0f + std::cos(elapsed * 0.21f) * 3.0f};
    Mat4 vp = mul(lookAt(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}),
                  perspective(65.0f * 3.1415926535f / 180.0f,
                              static_cast<float>(kWidth) / static_cast<float>(kHeight),
                              0.1f, 80.0f));
    if (envEnabled("VKD3D_STRESS_TRANSPOSE_VIEWPROJ"))
      vp = transpose(vp);
    cbMapped->viewProj = vp;
    cbMapped->time = elapsed;
    cbMapped->frame = static_cast<float>(frame);
    cbMapped->width = static_cast<float>(kWidth);
    cbMapped->height = static_cast<float>(kHeight);

    hr = h.allocator->Reset();
    ok = checkHr("Frame allocator Reset", hr);
    if (!ok)
      break;
    hr = h.cmd->Reset(h.allocator, nullptr);
    ok = checkHr("Frame command list Reset", hr);
    if (!ok)
      break;

    ID3D12DescriptorHeap *heaps[] = {h.srvHeap};
    h.cmd->SetDescriptorHeaps(1, heaps);

    h.cmd->SetComputeRootSignature(computeRoot);
    h.cmd->SetPipelineState(computePso);
    h.cmd->SetComputeRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
    h.cmd->SetComputeRootDescriptorTable(1, gpuSrv(h, 2));
    h.cmd->Dispatch((kTextureSize + 7) / 8, (kTextureSize + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = computeTexture;
    h.cmd->ResourceBarrier(1, &uavBarrier);
    barrier(h.cmd, computeTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    UINT backBufferIndex = h.swapchain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = h.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += backBufferIndex * h.rtvStride;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = h.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    barrier(h.cmd, h.backBuffers[backBufferIndex], D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float clearColor[] = {0.015f, 0.018f, 0.028f, 1.0f};
    h.cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    h.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth),
                               static_cast<float>(kHeight), 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    h.cmd->RSSetViewports(1, &viewport);
    h.cmd->RSSetScissorRects(1, &scissor);

    const bool inSplash = elapsed < splashSeconds;
    if (inSplash) {
      h.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      h.cmd->SetGraphicsRootSignature(splashRoot);
      h.cmd->SetPipelineState(splashPso);
      h.cmd->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
      h.cmd->SetGraphicsRootDescriptorTable(1, gpuSrv(h, 0));
      h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      h.cmd->DrawInstanced(4, 1, 0, 0);
    } else {
      if (frame == 0 || (frame % 30) == 0) {
        std::printf("[INFO] stress scene record frame=%u elapsed=%.3f expected_draws=10 vertices=%u instances=%u\n",
                    frame, elapsed, sceneVertexCount, kInstanceCount);
      }
      if (postOnly || sceneNoDepth) {
        h.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
      } else {
        h.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
      }
      if (!postOnly) {
        h.cmd->SetGraphicsRootSignature(sceneRoot);
        h.cmd->SetPipelineState(scenePso);
        h.cmd->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
        h.cmd->SetGraphicsRootDescriptorTable(1, gpuSrv(h, 0));
        h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        h.cmd->IASetVertexBuffers(0, 2, vbvs);
        h.cmd->DrawInstanced(sceneVertexCount, 1, 0, 0);
        h.cmd->IASetVertexBuffers(0, 2, microVbvs);
        for (UINT batch = 0; batch < 8; batch++)
          h.cmd->DrawInstanced(3, kInstanceCount, 0, 0);
      }

      if (!skipPost) {
        h.cmd->SetGraphicsRootSignature(postRoot);
        h.cmd->SetPipelineState(postPso);
        h.cmd->SetGraphicsRootConstantBufferView(0, constantBuffer->GetGPUVirtualAddress());
        h.cmd->SetGraphicsRootDescriptorTable(1, gpuSrv(h, 0));
        h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        h.cmd->DrawInstanced(4, 1, 0, 0);
      }
    }

    barrier(h.cmd, computeTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!skipReadback && (frame % 30) == 0) {
      barrier(h.cmd, h.backBuffers[backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET,
              D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION copyDst = {};
      copyDst.pResource = readback;
      copyDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      copyDst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      copyDst.PlacedFootprint.Footprint.Width = kWidth;
      copyDst.PlacedFootprint.Footprint.Height = kHeight;
      copyDst.PlacedFootprint.Footprint.Depth = 1;
      copyDst.PlacedFootprint.Footprint.RowPitch = readbackPitch;
      D3D12_TEXTURE_COPY_LOCATION copySrc = {};
      copySrc.pResource = h.backBuffers[backBufferIndex];
      copySrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      h.cmd->CopyTextureRegion(&copyDst, 0, 0, 0, &copySrc, nullptr);
      barrier(h.cmd, h.backBuffers[backBufferIndex], D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_PRESENT);
    } else {
      barrier(h.cmd, h.backBuffers[backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET,
              D3D12_RESOURCE_STATE_PRESENT);
    }

    hr = h.cmd->Close();
    ok = checkHr("Frame command list Close", hr);
    if (!ok)
      break;
    ID3D12CommandList *lists[] = {h.cmd};
    h.queue->ExecuteCommandLists(1, lists);
    if (!skipReadback && envEnabled("VKD3D_STRESS_READBACK_BEFORE_PRESENT") && (frame % 30) == 0) {
      ReadbackStats stats = analyzeReadback(readback, readbackPitch);
      std::printf("[INFO] stress pre-present frame=%u phase=%s bright=%u chroma=%u checksum=0x%016llx\n",
                  frame, inSplash ? "splash" : "scene", stats.brightPixels,
                  stats.chromaPixels,
                  static_cast<unsigned long long>(stats.checksum));
    }
    hr = h.swapchain->Present(1, 0);
    ok = checkHr("Present", hr) && waitForGpu(h);
    if (!ok)
      break;

    if (!skipReadback && (frame % 30) == 0) {
      ReadbackStats stats = analyzeReadback(readback, readbackPitch);
      std::printf("[INFO] stress frame=%u phase=%s bright=%u chroma=%u checksum=0x%016llx\n",
                  frame, inSplash ? "splash" : "scene", stats.brightPixels, stats.chromaPixels,
                  static_cast<unsigned long long>(stats.checksum));
      if (!inSplash && (stats.brightPixels < 1000 || stats.chromaPixels < 500)) {
        std::fprintf(stderr, "[FAIL] stress readback too dark/flat\n");
        ok = false;
        break;
      }
    }
    frame++;
  }

  if (ok)
    std::printf("[PASS] vkd3d_stress_game frames=%u seconds=%u\n", frame, seconds);

  constantBuffer->Unmap(0, nullptr);
  releaseIf(readback);
  releaseIf(splashPso);
  releaseIf(splashRoot);
  releaseIf(postPso);
  releaseIf(postRoot);
  releaseIf(scenePso);
  releaseIf(sceneRoot);
  releaseIf(computePso);
  releaseIf(computeRoot);
  releaseIf(textureUpload);
  releaseIf(computeTexture);
  releaseIf(baseTexture);
  releaseIf(constantBuffer);
  releaseIf(instanceBuffer);
  releaseIf(microVertexBuffer);
  releaseIf(vertexBuffer);
  return ok;
}

int main(int argc, char **argv) {
  UINT seconds = kRunSeconds;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
      seconds = static_cast<UINT>(std::strtoul(argv[++i], nullptr, 10));
  }
  if (seconds == 0)
    seconds = kRunSeconds;

  logRuntimePrereqs();
  std::printf("=== vkd3d_stress_game.exe start seconds=%u ===\n", seconds);
  Harness h = {};
  bool ok = createHarness(h) && runStress(h, seconds);
  destroyHarness(h);
  if (ok) {
    std::printf("=== vkd3d_stress_game.exe PASS ===\n");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  std::fprintf(stderr, "=== vkd3d_stress_game.exe FAIL ===\n");
  std::fflush(stderr);
  TerminateProcess(GetCurrentProcess(), 1);
}
