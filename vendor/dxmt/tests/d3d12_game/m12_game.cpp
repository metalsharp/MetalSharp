#define INITGUID
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr UINT kWidth = 800;
static constexpr UINT kHeight = 600;
static constexpr UINT kBackBufferCount = 2;
static constexpr UINT kSparseColorSlot = 3;

static UINT alignUp(UINT value, UINT alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
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

static void pumpMessages() {
  MSG msg = {};
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
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

static bool compileShader(const char *label, const char *source, const char *entry,
                          const char *target, ID3DBlob **blob) {
  ID3DBlob *errors = nullptr;
  HRESULT hr = D3DCompile(source, std::strlen(source), label, nullptr, nullptr,
                          entry, target, 0, 0, blob, &errors);
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
  std::printf("[PASS] %s compiled (%zu bytes)\n", label, (*blob)->GetBufferSize());
  return true;
}

struct PosVertex {
  float x, y, z;
};

struct ColorVertex {
  float r, g, b, a;
};

struct TexVertex {
  float x, y, u, v;
};

struct SceneVertex {
  float x, y, z;
  float r, g, b, a;
};

struct Vec3 {
  float x, y, z;
};

static Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

static float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

static Vec3 normalize(Vec3 v) {
  float len = std::sqrt(dot(v, v));
  if (len <= 0.00001f)
    return {0.0f, 1.0f, 0.0f};
  return mul(v, 1.0f / len);
}

static Vec3 rotateY(Vec3 p, float angle) {
  float c = std::cos(angle);
  float s = std::sin(angle);
  return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
}

static SceneVertex projectVertex(Vec3 world, Vec3 color, Vec3 camera,
                                 Vec3 right, Vec3 up, Vec3 forward) {
  Vec3 rel = sub(world, camera);
  float z = dot(rel, forward);
  if (z < 0.05f)
    z = 0.05f;
  const float fov = 60.0f * 3.1415926535f / 180.0f;
  const float tanHalf = std::tan(fov * 0.5f);
  const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
  float x = dot(rel, right) / (z * tanHalf * aspect);
  float y = dot(rel, up) / (z * tanHalf);
  float depth = (z - 0.1f) / (20.0f - 0.1f);
  if (depth < 0.0f)
    depth = 0.0f;
  if (depth > 1.0f)
    depth = 1.0f;
  return {x, y, depth, color.x, color.y, color.z, 1.0f};
}

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
  ID3D12Resource *backBuffers[kBackBufferCount] = {};
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *cmd = nullptr;
  ID3D12Fence *fence = nullptr;
  HANDLE fenceEvent = nullptr;
  UINT rtvStride = 0;
  uint64_t fenceValue = 0;
};

static bool waitForGpu(Harness &h, DWORD timeoutMs = 10000) {
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
                                  D3D12_RESOURCE_STATES state) {
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

static ReadbackStats analyzeReadback(ID3D12Resource *readback, UINT pitch) {
  ReadbackStats stats = {};
  void *mapped = nullptr;
  HRESULT hr = readback->Map(0, nullptr, &mapped);
  if (FAILED(hr) || !mapped) {
    std::fprintf(stderr, "[FAIL] readback Map: 0x%08lx\n", hr);
    return stats;
  }

  const auto *bytes = static_cast<const uint8_t *>(mapped);
  for (UINT y = 0; y < kHeight; y += 2) {
    const uint8_t *row = bytes + y * pitch;
    for (UINT x = 0; x < kWidth; x += 2) {
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
      if (maxChannel > 80)
        stats.brightPixels++;
      if (maxChannel > 80 && (maxChannel - minChannel) > 20)
        stats.chromaPixels++;
    }
  }

  readback->Unmap(0, nullptr);
  return stats;
}

static bool createHarness(Harness &h) {
  WNDCLASSA wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "M12GameHarness";
  RegisterClassA(&wc);

  h.hwnd = CreateWindowA("M12GameHarness", "m12_game.exe",
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

  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_2,
                          IID_PPV_ARGS(&h.device));
  if (!checkHr("D3D12CreateDevice feature_level=12_2", hr))
    return false;

  D3D12_COMMAND_QUEUE_DESC queueDesc = {};
  queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = h.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&h.queue));
  if (!checkHr("CreateCommandQueue DIRECT", hr))
    return false;

  DXGI_SWAP_CHAIN_DESC1 scDesc = {};
  scDesc.Width = kWidth;
  scDesc.Height = kHeight;
  scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  scDesc.BufferCount = kBackBufferCount;
  scDesc.SampleDesc.Count = 1;
  scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  IDXGISwapChain1 *swap1 = nullptr;
  hr = h.factory->CreateSwapChainForHwnd(h.queue, h.hwnd, &scDesc, nullptr,
                                         nullptr, &swap1);
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
  D3D12_CPU_DESCRIPTOR_HANDLE handle = h.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < kBackBufferCount; i++) {
    hr = h.swapchain->GetBuffer(i, IID_PPV_ARGS(&h.backBuffers[i]));
    if (!checkHr("GetBuffer", hr))
      return false;
    h.device->CreateRenderTargetView(h.backBuffers[i], nullptr, handle);
    handle.ptr += h.rtvStride;
  }

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
  if (h.fenceEvent)
    CloseHandle(h.fenceEvent);
  releaseIf(h.fence);
  releaseIf(h.cmd);
  releaseIf(h.allocator);
  for (auto *&bb : h.backBuffers)
    releaseIf(bb);
  releaseIf(h.rtvHeap);
  releaseIf(h.swapchain);
  releaseIf(h.queue);
  releaseIf(h.device);
  releaseIf(h.factory);
  if (h.hwnd)
    DestroyWindow(h.hwnd);
}

static bool beginFrame(Harness &h, ID3D12PipelineState *pso, UINT &backBufferIndex,
                       D3D12_CPU_DESCRIPTOR_HANDLE &rtv) {
  HRESULT hr = h.allocator->Reset();
  if (!checkHr("CommandAllocator Reset", hr))
    return false;
  hr = h.cmd->Reset(h.allocator, pso);
  if (!checkHr("CommandList Reset", hr))
    return false;

  backBufferIndex = h.swapchain->GetCurrentBackBufferIndex();
  rtv = h.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += backBufferIndex * h.rtvStride;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = h.backBuffers[backBufferIndex];
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  h.cmd->ResourceBarrier(1, &barrier);
  h.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

  D3D12_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth),
                             static_cast<float>(kHeight), 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  h.cmd->RSSetViewports(1, &viewport);
  h.cmd->RSSetScissorRects(1, &scissor);
  return true;
}

static bool endFrame(Harness &h, UINT backBufferIndex, ID3D12Resource *readback,
                     UINT readbackPitch) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = h.backBuffers[backBufferIndex];
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;

  if (readback) {
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    h.cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kWidth;
    dst.PlacedFootprint.Footprint.Height = kHeight;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = readbackPitch;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = h.backBuffers[backBufferIndex];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    h.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  }

  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  h.cmd->ResourceBarrier(1, &barrier);

  HRESULT hr = h.cmd->Close();
  if (!checkHr("CommandList Close", hr))
    return false;
  ID3D12CommandList *lists[] = {h.cmd};
  h.queue->ExecuteCommandLists(1, lists);
  hr = h.swapchain->Present(1, 0);
  if (!checkHr("Present", hr))
    return false;
  return waitForGpu(h);
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

static bool runClearOnly(Harness &h) {
  std::printf("\n=== CASE clear_present ===\n");
  UINT backBufferIndex = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
  if (!beginFrame(h, nullptr, backBufferIndex, rtv))
    return false;
  const float clearColor[] = {0.03f, 0.04f, 0.08f, 1.0f};
  h.cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
  if (!endFrame(h, backBufferIndex, nullptr, 0))
    return false;
  std::printf("[PASS] clear_present\n");
  return true;
}

static bool createSparseColorPso(Harness &h, ID3D12RootSignature **rootSig,
                                 ID3D12PipelineState **pso) {
  static const char *vs =
      "struct VS_IN {\n"
      "  float3 pos : POSITION;\n"
      "  float4 col : COLOR0;\n"
      "};\n"
      "struct VS_OUT {\n"
      "  float4 pos : SV_Position;\n"
      "  float4 col : COLOR0;\n"
      "};\n"
      "VS_OUT main(VS_IN i) {\n"
      "  VS_OUT o;\n"
      "  o.pos = float4(i.pos, 1.0);\n"
      "  o.col = i.col;\n"
      "  return o;\n"
      "}\n";
  static const char *ps =
      "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target {\n"
      "  return col;\n"
      "}\n";

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;
  if (!compileShader("sparse-color VS", vs, "main", "vs_5_0", &vsBlob) ||
      !compileShader("sparse-color PS", ps, "main", "ps_5_0", &psBlob)) {
    releaseIf(vsBlob);
    releaseIf(psBlob);
    return false;
  }

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (!createRootSignature(h, rsDesc, rootSig)) {
    releaseIf(vsBlob);
    releaseIf(psBlob);
    return false;
  }

  D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, kSparseColorSlot, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = *rootSig;
  psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
  psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
  psoDesc.InputLayout = {layout, 2};
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psoDesc.SampleDesc.Count = 1;
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.DepthStencilState.DepthEnable = FALSE;
  psoDesc.DepthStencilState.StencilEnable = FALSE;

  HRESULT hr = h.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pso));
  releaseIf(vsBlob);
  releaseIf(psBlob);
  return checkHr("CreateGraphicsPipelineState sparse-color", hr);
}

static bool runSparseVertexCases(Harness &h) {
  std::printf("\n=== CASE sparse_vertex_draws ===\n");

  const PosVertex positions[] = {
      {-0.65f, -0.55f, 0.0f},
      {0.65f, -0.55f, 0.0f},
      {0.0f, 0.65f, 0.0f},
      {-0.7f, 0.7f, 0.0f},
      {0.7f, 0.7f, 0.0f},
      {0.7f, -0.7f, 0.0f},
      {-0.7f, -0.7f, 0.0f},
  };
  const ColorVertex colors[] = {
      {1.0f, 0.2f, 0.2f, 1.0f},
      {0.2f, 1.0f, 0.2f, 1.0f},
      {0.2f, 0.4f, 1.0f, 1.0f},
      {1.0f, 0.0f, 0.0f, 1.0f},
      {0.0f, 1.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 1.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
  };
  const uint16_t indices[] = {3, 4, 5, 3, 5, 6};

  ID3D12Resource *posBuffer =
      makeUploadBuffer(h, positions, sizeof(positions), "Map sparse position VB");
  ID3D12Resource *colorBuffer =
      makeUploadBuffer(h, colors, sizeof(colors), "Map sparse color VB");
  ID3D12Resource *indexBuffer =
      makeUploadBuffer(h, indices, sizeof(indices), "Map index buffer");
  if (!posBuffer || !colorBuffer || !indexBuffer)
    return false;

  D3D12_VERTEX_BUFFER_VIEW vbv[2] = {};
  vbv[0].BufferLocation = posBuffer->GetGPUVirtualAddress();
  vbv[0].StrideInBytes = sizeof(PosVertex);
  vbv[0].SizeInBytes = sizeof(positions);
  vbv[1].BufferLocation = colorBuffer->GetGPUVirtualAddress();
  vbv[1].StrideInBytes = sizeof(ColorVertex);
  vbv[1].SizeInBytes = sizeof(colors);

  D3D12_INDEX_BUFFER_VIEW ibv = {};
  ibv.BufferLocation = indexBuffer->GetGPUVirtualAddress();
  ibv.SizeInBytes = sizeof(indices);
  ibv.Format = DXGI_FORMAT_R16_UINT;

  ID3D12RootSignature *rootSig = nullptr;
  ID3D12PipelineState *pso = nullptr;
  bool ok = createSparseColorPso(h, &rootSig, &pso);

  const UINT pitch = alignUp(kWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  ID3D12Resource *readback = makeBuffer(h, D3D12_HEAP_TYPE_READBACK,
                                        static_cast<UINT64>(pitch) * kHeight,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
  if (!readback)
    ok = false;

  if (ok) {
    UINT backBufferIndex = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    ok = beginFrame(h, pso, backBufferIndex, rtv);
    if (ok) {
      const float clearColor[] = {0.02f, 0.02f, 0.05f, 1.0f};
      h.cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
      h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      h.cmd->IASetVertexBuffers(0, 1, &vbv[0]);
      h.cmd->IASetVertexBuffers(kSparseColorSlot, 1, &vbv[1]);
      h.cmd->DrawInstanced(3, 1, 0, 0);
      h.cmd->IASetIndexBuffer(&ibv);
      h.cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
      ok = endFrame(h, backBufferIndex, readback, pitch);
    }
  }

  if (ok) {
    ReadbackStats stats = analyzeReadback(readback, pitch);
    std::printf("[INFO] sparse_vertex_draws bright=%u chroma=%u checksum=0x%016llx\n",
                stats.brightPixels, stats.chromaPixels,
                static_cast<unsigned long long>(stats.checksum));
    if (stats.chromaPixels < 500) {
      std::fprintf(stderr, "[FAIL] sparse_vertex_draws readback did not find enough colored pixels\n");
      ok = false;
    }
  }

  releaseIf(readback);
  releaseIf(pso);
  releaseIf(rootSig);
  releaseIf(indexBuffer);
  releaseIf(colorBuffer);
  releaseIf(posBuffer);
  if (ok)
    std::printf("[PASS] sparse_vertex_draws\n");
  return ok;
}

static bool runTextureDescriptorCase(Harness &h) {
  std::printf("\n=== CASE texture_descriptor_draw ===\n");

  ID3D12DescriptorHeap *srvHeap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
  srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srvHeapDesc.NumDescriptors = 1;
  srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT hr = h.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
  if (!checkHr("Create shader-visible SRV heap", hr))
    return false;

  ID3D12DescriptorHeap *samplerHeap = nullptr;
  D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
  samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  samplerHeapDesc.NumDescriptors = 1;
  samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = h.device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&samplerHeap));
  if (!checkHr("Create shader-visible sampler heap", hr))
    return false;

  D3D12_SAMPLER_DESC samplerDesc = {};
  samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
  h.device->CreateSampler(&samplerDesc, samplerHeap->GetCPUDescriptorHandleForHeapStart());

  D3D12_HEAP_PROPERTIES defaultHeap = {};
  defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC textureDesc = {};
  textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  textureDesc.Width = 64;
  textureDesc.Height = 64;
  textureDesc.DepthOrArraySize = 1;
  textureDesc.MipLevels = 1;
  textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  textureDesc.SampleDesc.Count = 1;

  ID3D12Resource *texture = nullptr;
  hr = h.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
                                         &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                         nullptr, IID_PPV_ARGS(&texture));
  if (!checkHr("Create texture resource", hr))
    return false;

  uint32_t texels[64 * 64] = {};
  for (UINT y = 0; y < 64; y++) {
    for (UINT x = 0; x < 64; x++) {
      texels[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xff20d850u : 0xffe04030u;
    }
  }
  ID3D12Resource *textureUpload =
      makeUploadBuffer(h, texels, sizeof(texels), "Map texture upload");
  if (!textureUpload)
    return false;

  hr = h.allocator->Reset();
  if (!checkHr("texture upload allocator Reset", hr))
    return false;
  hr = h.cmd->Reset(h.allocator, nullptr);
  if (!checkHr("texture upload command Reset", hr))
    return false;

  D3D12_TEXTURE_COPY_LOCATION src = {};
  src.pResource = textureUpload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  src.PlacedFootprint.Footprint.Width = 64;
  src.PlacedFootprint.Footprint.Height = 64;
  src.PlacedFootprint.Footprint.Depth = 1;
  src.PlacedFootprint.Footprint.RowPitch = 64 * 4;

  D3D12_TEXTURE_COPY_LOCATION dst = {};
  dst.pResource = texture;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  h.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

  D3D12_RESOURCE_BARRIER textureBarrier = {};
  textureBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  textureBarrier.Transition.pResource = texture;
  textureBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  textureBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  h.cmd->ResourceBarrier(1, &textureBarrier);
  hr = h.cmd->Close();
  if (!checkHr("texture upload command Close", hr))
    return false;
  ID3D12CommandList *uploadLists[] = {h.cmd};
  h.queue->ExecuteCommandLists(1, uploadLists);
  if (!waitForGpu(h))
    return false;

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  h.device->CreateShaderResourceView(texture, &srvDesc,
                                     srvHeap->GetCPUDescriptorHandleForHeapStart());

  static const char *vs =
      "struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
      "struct VS_OUT { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
      "VS_OUT main(VS_IN i) {\n"
      "  VS_OUT o;\n"
      "  o.pos = float4(i.pos, 0.0, 1.0);\n"
      "  o.uv = i.uv;\n"
      "  return o;\n"
      "}\n";
  static const char *ps =
      "Texture2D<float4> tex0 : register(t0);\n"
      "SamplerState samp0 : register(s0);\n"
      "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
      "  return tex0.Sample(samp0, uv);\n"
      "}\n";

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;
  bool ok = compileShader("texture VS", vs, "main", "vs_5_0", &vsBlob) &&
            compileShader("texture PS", ps, "main", "ps_5_0", &psBlob);

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[1].DescriptorTable.NumDescriptorRanges = 1;
  params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.NumParameters = 2;
  rsDesc.pParameters = params;
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ID3D12RootSignature *rootSig = nullptr;
  ID3D12PipelineState *pso = nullptr;
  if (ok)
    ok = createRootSignature(h, rsDesc, &rootSig);

  D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  if (ok) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig;
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.InputLayout = {layout, 2};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    hr = h.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    ok = checkHr("CreateGraphicsPipelineState texture", hr);
  }

  const TexVertex vertices[] = {
      {-0.8f, -0.8f, 0.0f, 1.0f},
      {0.8f, -0.8f, 1.0f, 1.0f},
      {0.0f, 0.8f, 0.5f, 0.0f},
  };
  ID3D12Resource *vb = makeUploadBuffer(h, vertices, sizeof(vertices), "Map texture VB");
  if (!vb)
    ok = false;

  const UINT pitch = alignUp(kWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  ID3D12Resource *readback = makeBuffer(h, D3D12_HEAP_TYPE_READBACK,
                                        static_cast<UINT64>(pitch) * kHeight,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
  if (!readback)
    ok = false;

  if (ok) {
    UINT backBufferIndex = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    ok = beginFrame(h, pso, backBufferIndex, rtv);
    if (ok) {
      const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
      h.cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
      D3D12_VERTEX_BUFFER_VIEW vbv = {};
      vbv.BufferLocation = vb->GetGPUVirtualAddress();
      vbv.StrideInBytes = sizeof(TexVertex);
      vbv.SizeInBytes = sizeof(vertices);
      ID3D12DescriptorHeap *heaps[] = {srvHeap, samplerHeap};
      h.cmd->SetDescriptorHeaps(2, heaps);
      h.cmd->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
      h.cmd->SetGraphicsRootDescriptorTable(1, samplerHeap->GetGPUDescriptorHandleForHeapStart());
      h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      h.cmd->IASetVertexBuffers(0, 1, &vbv);
      h.cmd->DrawInstanced(3, 1, 0, 0);
      ok = endFrame(h, backBufferIndex, readback, pitch);
    }
  }

  if (ok) {
    ReadbackStats stats = analyzeReadback(readback, pitch);
    std::printf("[INFO] texture_descriptor_draw bright=%u chroma=%u checksum=0x%016llx\n",
                stats.brightPixels, stats.chromaPixels,
                static_cast<unsigned long long>(stats.checksum));
    if (stats.chromaPixels < 500) {
      std::fprintf(stderr, "[FAIL] texture_descriptor_draw readback did not find enough textured pixels\n");
      ok = false;
    }
  }

  releaseIf(readback);
  releaseIf(vb);
  releaseIf(pso);
  releaseIf(rootSig);
  releaseIf(psBlob);
  releaseIf(vsBlob);
  releaseIf(textureUpload);
  releaseIf(texture);
  releaseIf(samplerHeap);
  releaseIf(srvHeap);
  if (ok)
    std::printf("[PASS] texture_descriptor_draw\n");
  return ok;
}

static bool createScenePso(Harness &h, ID3D12RootSignature **rootSig,
                           ID3D12PipelineState **pso) {
  static const char *vs =
      "struct VS_IN { float3 pos : POSITION; float4 col : COLOR0; };\n"
      "struct VS_OUT { float4 pos : SV_Position; float4 col : COLOR0; };\n"
      "VS_OUT main(VS_IN i) {\n"
      "  VS_OUT o;\n"
      "  o.pos = float4(i.pos, 1.0);\n"
      "  o.col = i.col;\n"
      "  return o;\n"
      "}\n";
  static const char *ps =
      "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target {\n"
      "  return col;\n"
      "}\n";

  ID3DBlob *vsBlob = nullptr;
  ID3DBlob *psBlob = nullptr;
  if (!compileShader("cube VS", vs, "main", "vs_5_0", &vsBlob) ||
      !compileShader("cube PS", ps, "main", "ps_5_0", &psBlob)) {
    releaseIf(vsBlob);
    releaseIf(psBlob);
    return false;
  }

  D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
  rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  if (!createRootSignature(h, rsDesc, rootSig)) {
    releaseIf(vsBlob);
    releaseIf(psBlob);
    return false;
  }

  D3D12_INPUT_ELEMENT_DESC layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = *rootSig;
  psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
  psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
  psoDesc.InputLayout = {layout, 2};
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  psoDesc.SampleDesc.Count = 1;
  psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  psoDesc.DepthStencilState.DepthEnable = TRUE;
  psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
  psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
  psoDesc.DepthStencilState.StencilEnable = FALSE;

  HRESULT hr = h.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pso));
  releaseIf(vsBlob);
  releaseIf(psBlob);
  return checkHr("CreateGraphicsPipelineState cube", hr);
}

static void writeCubeScene(SceneVertex *vertices, float angle) {
  const Vec3 camera = {3.2f, 2.4f, -5.4f};
  const Vec3 target = {0.0f, 0.05f, 0.0f};
  const Vec3 forward = normalize(sub(target, camera));
  const Vec3 right = normalize(cross({0.0f, 1.0f, 0.0f}, forward));
  const Vec3 up = cross(forward, right);
  const Vec3 light = normalize({-0.45f, -0.85f, -0.32f});
  const float floorY = -1.25f;

  const Vec3 cube[8] = {
      {-1.0f, -1.0f, -1.0f}, {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f},   {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f},  {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f},    {-1.0f, 1.0f, 1.0f},
  };
  const uint8_t faces[6][4] = {
      {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
      {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
  };
  const Vec3 faceColors[6] = {
      {1.0f, 0.06f, 0.04f}, {0.05f, 0.95f, 0.12f},
      {0.08f, 0.28f, 1.0f}, {1.0f, 0.85f, 0.08f},
      {0.9f, 0.12f, 1.0f},  {0.08f, 0.9f, 0.95f},
  };

  auto transformCube = [&](Vec3 p) -> Vec3 {
    Vec3 r = rotateY(p, angle);
    float wobble = std::sin(angle * 1.7f) * 0.12f;
    return {r.x, r.y + wobble, r.z};
  };
  auto shade = [&](Vec3 base, Vec3 normal) {
    float ndl = dot(normalize(normal), mul(light, -1.0f));
    if (ndl < 0.0f)
      ndl = 0.0f;
    float intensity = 0.24f + ndl * 0.76f;
    return Vec3{base.x * intensity, base.y * intensity, base.z * intensity};
  };
  auto shadowPoint = [&](Vec3 p) {
    float denom = light.y;
    if (std::fabs(denom) < 0.01f)
      denom = -0.01f;
    float t = (p.y - (floorY + 0.018f)) / -denom;
    Vec3 s = add(p, mul(light, t));
    s.y = floorY + 0.018f;
    return s;
  };

  UINT out = 0;
  const Vec3 floorColor = {0.18f, 0.20f, 0.23f};
  const Vec3 floorVerts[4] = {
      {-3.3f, floorY, -2.4f}, {3.3f, floorY, -2.4f},
      {3.3f, floorY, 2.8f},   {-3.3f, floorY, 2.8f},
  };
  for (UINT i = 0; i < 4; i++)
    vertices[out++] = projectVertex(floorVerts[i], floorColor, camera, right, up, forward);

  for (UINT f = 0; f < 6; f++) {
    Vec3 p0 = transformCube(cube[faces[f][0]]);
    Vec3 p1 = transformCube(cube[faces[f][1]]);
    Vec3 p2 = transformCube(cube[faces[f][2]]);
    Vec3 normal = normalize(cross(sub(p1, p0), sub(p2, p0)));
    Vec3 color = shade(faceColors[f], normal);
    for (UINT i = 0; i < 4; i++)
      vertices[out++] = projectVertex(transformCube(cube[faces[f][i]]), color,
                                      camera, right, up, forward);
  }

  const Vec3 shadowColor = {0.015f, 0.018f, 0.022f};
  for (UINT f = 0; f < 6; f++) {
    for (UINT i = 0; i < 4; i++) {
      Vec3 p = transformCube(cube[faces[f][i]]);
      vertices[out++] = projectVertex(shadowPoint(p), shadowColor, camera,
                                      right, up, forward);
    }
  }
}

static bool runCubeStress(Harness &h, float seconds) {
  std::printf("\n=== CASE rgb_cube_10s ===\n");
  if (seconds < 0.25f)
    seconds = 0.25f;

  ID3D12RootSignature *rootSig = nullptr;
  ID3D12PipelineState *pso = nullptr;
  bool ok = createScenePso(h, &rootSig, &pso);

  static constexpr UINT kSceneVertices = 4 + 24 + 24;
  static constexpr UINT kSceneIndices = 6 + 36 + 36;
  ID3D12Resource *vb = makeBuffer(h, D3D12_HEAP_TYPE_UPLOAD,
                                  sizeof(SceneVertex) * kSceneVertices,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  ID3D12Resource *ib = makeBuffer(h, D3D12_HEAP_TYPE_UPLOAD,
                                  sizeof(uint16_t) * kSceneIndices,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!vb || !ib)
    ok = false;

  SceneVertex *mappedVertices = nullptr;
  if (ok) {
    HRESULT hr = vb->Map(0, nullptr, reinterpret_cast<void **>(&mappedVertices));
    ok = checkHr("Map cube VB", hr);
  }

  uint16_t *mappedIndices = nullptr;
  if (ok) {
    HRESULT hr = ib->Map(0, nullptr, reinterpret_cast<void **>(&mappedIndices));
    ok = checkHr("Map cube IB", hr);
  }

  if (ok) {
    uint16_t indices[kSceneIndices] = {};
    UINT o = 0;
    auto quad = [&](uint16_t base) {
      indices[o++] = base + 0; indices[o++] = base + 1; indices[o++] = base + 2;
      indices[o++] = base + 0; indices[o++] = base + 2; indices[o++] = base + 3;
    };
    quad(0);
    for (UINT f = 0; f < 6; f++)
      quad(static_cast<uint16_t>(4 + f * 4));
    for (UINT f = 0; f < 6; f++)
      quad(static_cast<uint16_t>(28 + f * 4));
    std::memcpy(mappedIndices, indices, sizeof(indices));
    ib->Unmap(0, nullptr);
  }

  ID3D12DescriptorHeap *dsvHeap = nullptr;
  ID3D12Resource *depth = nullptr;
  if (ok) {
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    HRESULT hr = h.device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));
    ok = checkHr("Create DSV heap", hr);
  }
  if (ok) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    HRESULT hr = h.device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear, IID_PPV_ARGS(&depth));
    ok = checkHr("Create depth texture", hr);
  }
  if (ok)
    h.device->CreateDepthStencilView(depth, nullptr,
                                     dsvHeap->GetCPUDescriptorHandleForHeapStart());

  const UINT pitch = alignUp(kWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  ID3D12Resource *readback = makeBuffer(h, D3D12_HEAP_TYPE_READBACK,
                                        static_cast<UINT64>(pitch) * kHeight,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
  if (!readback)
    ok = false;

  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  if (vb) {
    vbv.BufferLocation = vb->GetGPUVirtualAddress();
    vbv.StrideInBytes = sizeof(SceneVertex);
    vbv.SizeInBytes = sizeof(SceneVertex) * kSceneVertices;
  }
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  if (ib) {
    ibv.BufferLocation = ib->GetGPUVirtualAddress();
    ibv.SizeInBytes = sizeof(uint16_t) * kSceneIndices;
    ibv.Format = DXGI_FORMAT_R16_UINT;
  }

  DWORD start = GetTickCount();
  DWORD durationMs = static_cast<DWORD>(seconds * 1000.0f);
  UINT frame = 0;
  while (ok) {
    DWORD now = GetTickCount();
    DWORD elapsed = now - start;
    if (elapsed > durationMs && frame > 0)
      break;

    float t = static_cast<float>(elapsed) / 1000.0f;
    writeCubeScene(mappedVertices, t * 1.25f);

    UINT backBufferIndex = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
    ok = beginFrame(h, pso, backBufferIndex, rtv);
    if (!ok)
      break;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    const float clearColor[] = {0.035f, 0.045f, 0.065f, 1.0f};
    h.cmd->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    h.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    h.cmd->SetPipelineState(pso);
    h.cmd->SetGraphicsRootSignature(rootSig);
    h.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    h.cmd->IASetVertexBuffers(0, 1, &vbv);
    h.cmd->IASetIndexBuffer(&ibv);
    h.cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
    h.cmd->DrawIndexedInstanced(36, 1, 42, 0, 0);
    h.cmd->DrawIndexedInstanced(36, 1, 6, 0, 0);
    ok = endFrame(h, backBufferIndex,
                  elapsed + 34 >= durationMs ? readback : nullptr, pitch);
    frame++;
    pumpMessages();
  }

  if (mappedVertices)
    vb->Unmap(0, nullptr);

  if (ok) {
    ReadbackStats stats = analyzeReadback(readback, pitch);
    std::printf("[INFO] rgb_cube_10s frames=%u bright=%u chroma=%u checksum=0x%016llx\n",
                frame, stats.brightPixels, stats.chromaPixels,
                static_cast<unsigned long long>(stats.checksum));
    if (frame < 20 || stats.chromaPixels < 1000) {
      std::fprintf(stderr, "[FAIL] rgb_cube_10s did not produce enough lit RGB pixels\n");
      ok = false;
    }
  }

  releaseIf(readback);
  releaseIf(depth);
  releaseIf(dsvHeap);
  releaseIf(ib);
  releaseIf(vb);
  releaseIf(pso);
  releaseIf(rootSig);
  if (ok)
    std::printf("[PASS] rgb_cube_10s\n");
  return ok;
}

int main(int argc, char **argv) {
  int loops = 1;
  float seconds = 10.0f;
  bool quickChecks = false;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--loops") && i + 1 < argc)
      loops = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
      seconds = static_cast<float>(std::atof(argv[++i]));
    else if (!std::strcmp(argv[i], "--quick-checks"))
      quickChecks = true;
  }
  if (loops < 1)
    loops = 1;

  std::printf("=== m12_game.exe DX12 harness ===\n");
  std::printf("[INFO] loops=%d seconds=%.2f width=%u height=%u sparse_color_slot=%u\n",
              loops, seconds, kWidth, kHeight, kSparseColorSlot);

  Harness h = {};
  if (!createHarness(h)) {
    destroyHarness(h);
    return 1;
  }

  int fail = 0;
  for (int loop = 0; loop < loops; loop++) {
    std::printf("\n=== LOOP %d/%d ===\n", loop + 1, loops);
    if (quickChecks) {
      if (!runClearOnly(h))
        fail++;
      if (!runSparseVertexCases(h))
        fail++;
      if (!runTextureDescriptorCase(h))
        fail++;
    }
    if (!runCubeStress(h, seconds))
      fail++;
    pumpMessages();
  }

  waitForGpu(h);
  destroyHarness(h);

  if (fail) {
    std::fprintf(stderr, "\n=== m12_game.exe FAIL cases=%d ===\n", fail);
    std::fflush(stdout);
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
  }

  std::printf("\n=== m12_game.exe PASS ===\n");
  std::fflush(stdout);
  std::fflush(stderr);
  TerminateProcess(GetCurrentProcess(), 0);
}
