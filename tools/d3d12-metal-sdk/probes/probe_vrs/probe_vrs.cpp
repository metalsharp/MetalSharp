#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

static const GUID IID_D3D12DeviceProbe = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                          REFIID, void **);
using SerializeRootSignatureFn = HRESULT(WINAPI *)(
    const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
    ID3DBlob **);

template <typename T> static void release(T *&value) {
  if (value) {
    value->Release();
    value = nullptr;
  }
}

template <typename T> static T proc(HMODULE module, const char *name) {
  FARPROC address = module ? GetProcAddress(module, name) : nullptr;
  T result = nullptr;
  static_assert(sizeof(result) == sizeof(address), "function pointer size mismatch");
  std::memcpy(&result, &address, sizeof(result));
  return result;
}

static std::string hr_hex(HRESULT value) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
  return buffer;
}

static bool write_file(const char *path, const char *text) {
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  DWORD size = static_cast<DWORD>(std::strlen(text));
  bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
  CloseHandle(file);
  return ok;
}

static std::vector<uint8_t> read_file(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > 16 * 1024 * 1024) {
    CloseHandle(file);
    return {};
  }
  std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read,
                     nullptr) &&
            read == data.size();
  CloseHandle(file);
  if (!ok)
    return {};
  return data;
}

static DWORD run_process(const char *command_line) {
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  std::vector<char> command(command_line,
                            command_line + std::strlen(command_line) + 1);
  if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    return UINT32_MAX;
  DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = UINT32_MAX;
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exit_code;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES result = {};
  result.Type = type;
  result.CreationNodeMask = 1;
  result.VisibleNodeMask = 1;
  return result;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 size) {
  D3D12_RESOURCE_DESC result = {};
  result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  result.Width = size;
  result.Height = 1;
  result.DepthOrArraySize = 1;
  result.MipLevels = 1;
  result.SampleDesc.Count = 1;
  result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  result.Flags = D3D12_RESOURCE_FLAG_NONE;
  return result;
}

static HRESULT execute_and_wait(ID3D12Device *device,
                                ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
  HRESULT hr = list->Close();
  if (FAILED(hr))
    return hr;
  ID3D12CommandList *lists[] = {list};
  queue->ExecuteCommandLists(1, lists);
  ID3D12Fence *fence = nullptr;
  HANDLE event = nullptr;
  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (SUCCEEDED(hr))
    hr = queue->Signal(fence, 1);
  if (SUCCEEDED(hr)) {
    event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event)
      hr = HRESULT_FROM_WIN32(GetLastError());
  }
  if (SUCCEEDED(hr))
    hr = fence->SetEventOnCompletion(1, event);
  if (SUCCEEDED(hr) && WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (event)
    CloseHandle(event);
  release(fence);
  return hr;
}

static uint32_t count_nonzero(ID3D12Resource *readback) {
  if (!readback)
    return 0;
  uint8_t *data = nullptr;
  D3D12_RANGE range = {0, 64u * 64u * 4u};
  if (FAILED(readback->Map(0, &range, reinterpret_cast<void **>(&data))) ||
      !data)
    return 0;
  uint32_t count = 0;
  for (uint32_t y = 0; y < 64; ++y) {
    const uint32_t *row = reinterpret_cast<const uint32_t *>(data + y * 256);
    for (uint32_t x = 0; x < 64; ++x)
      count += row[x] != 0;
  }
  readback->Unmap(0, nullptr);
  return count;
}

static HRESULT record_draw(ID3D12GraphicsCommandList *list,
                           ID3D12GraphicsCommandList5 *list5,
                           ID3D12RootSignature *root,
                           ID3D12PipelineState *pso, ID3D12Resource *target,
                           D3D12_SHADING_RATE base_shading_rate,
                           ID3D12Resource *shading_rate_image,
                           ID3D12Resource *readback,
                           ID3D12Resource *vertex_buffer,
                           ID3D12DescriptorHeap *rtv_heap,
                           const D3D12_SHADING_RATE_COMBINER *combiners_arg =
                               nullptr) {
  if (base_shading_rate != D3D12_SHADING_RATE_1X1 ||
      shading_rate_image || combiners_arg) {
    const D3D12_SHADING_RATE_COMBINER default_combiners[2] = {
        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    list5->RSSetShadingRate(base_shading_rate,
                            combiners_arg ? combiners_arg : default_combiners);
  }
  if (shading_rate_image)
    list5->RSSetShadingRateImage(shading_rate_image);
  list->SetPipelineState(pso);
  list->SetGraphicsRootSignature(root);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const float clear_color[4] = {0, 0, 0, 0};
  list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
  D3D12_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
  D3D12_RECT scissor = {0, 0, 64, 64};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
  vbv.SizeInBytes = 36;
  vbv.StrideInBytes = 12;
  list->IASetVertexBuffers(0, 1, &vbv);
  list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER target_barrier = {};
  target_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  target_barrier.Transition.pResource = target;
  target_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  target_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  target_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list->ResourceBarrier(1, &target_barrier);
  D3D12_TEXTURE_COPY_LOCATION source = {};
  source.pResource = target;
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination = {};
  destination.pResource = readback;
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  destination.PlacedFootprint.Footprint.Width = 64;
  destination.PlacedFootprint.Footprint.Height = 64;
  destination.PlacedFootprint.Footprint.Depth = 1;
  destination.PlacedFootprint.Footprint.RowPitch = 256;
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  return S_OK;
}

int main() {
  const char *shader_path = "Z:\\tmp\\metalsharp_vrs.hlsl";
  const char *vs_path = "Z:\\tmp\\metalsharp_vrs_vs.dxil";
  const char *ps_path = "Z:\\tmp\\metalsharp_vrs_ps.dxil";
  const char *source = R"(
struct VSIn { float3 position : POSITION; };
struct VSOut { float4 position : SV_Position; };
VSOut vs_main(VSIn input) {
  VSOut output;
  output.position = float4(input.position, 1.0);
  return output;
}
float4 ps_main(VSOut input) : SV_Target0 {
  return float4(1.0, 0.0, 0.0, 1.0);
}
)";
  bool source_ok = write_file(shader_path, source);
  DeleteFileA(vs_path);
  DeleteFileA(ps_path);
  DWORD vs_dxc = run_process(
      "dxc.exe -nologo -T vs_6_0 -E vs_main -HV 2021 -Fo "
      "Z:\\tmp\\metalsharp_vrs_vs.dxil Z:\\tmp\\metalsharp_vrs.hlsl");
  DWORD ps_dxc = run_process(
      "dxc.exe -nologo -T ps_6_0 -E ps_main -HV 2021 -Fo "
      "Z:\\tmp\\metalsharp_vrs_ps.dxil Z:\\tmp\\metalsharp_vrs.hlsl");
  std::vector<uint8_t> vs = read_file(vs_path);
  std::vector<uint8_t> ps = read_file(ps_path);
  DeleteFileA(vs_path);
  DeleteFileA(ps_path);

  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  auto create_device = proc<CreateDeviceFn>(d3d12, "D3D12CreateDevice");
  auto serialize = proc<SerializeRootSignatureFn>(d3d12,
                                                  "D3D12SerializeRootSignature");
  ID3D12Device *device = nullptr;
  HRESULT device_hr = create_device
                          ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                          IID_D3D12DeviceProbe,
                                          reinterpret_cast<void **>(&device))
                          : E_FAIL;
  ID3DBlob *root_blob = nullptr;
  ID3DBlob *root_error = nullptr;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  HRESULT root_serialize_hr =
      serialize && device
          ? serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_blob,
                      &root_error)
          : E_FAIL;
  if (root_error)
    root_error->Release();
  ID3D12RootSignature *root = nullptr;
  HRESULT root_hr = SUCCEEDED(root_serialize_hr) && root_blob
                        ? device->CreateRootSignature(
                              0, root_blob->GetBufferPointer(),
                              root_blob->GetBufferSize(), IID_PPV_ARGS(&root))
                        : E_FAIL;

  D3D12_INPUT_ELEMENT_DESC input = {
      "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root;
  pso_desc.VS = {vs.data(), vs.size()};
  pso_desc.PS = {ps.data(), ps.size()};
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso_desc.RasterizerState.DepthClipEnable = TRUE;
  pso_desc.InputLayout = {&input, 1};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;
  ID3D12PipelineState *pso = nullptr;
  HRESULT pso_hr = device && root && !vs.empty() && !ps.empty()
                       ? device->CreateGraphicsPipelineState(
                             &pso_desc, IID_PPV_ARGS(&pso))
                       : E_FAIL;

  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12GraphicsCommandList5 *list5 = nullptr;
  ID3D12Resource *target = nullptr;
  ID3D12Resource *shading_rate_image = nullptr;
  ID3D12Resource *shading_rate_upload = nullptr;
  ID3D12Resource *readback = nullptr;
  ID3D12Resource *vertex_buffer = nullptr;
  ID3D12DescriptorHeap *rtv_heap = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  HRESULT queue_hr = device ? device->CreateCommandQueue(
                                  &queue_desc, IID_PPV_ARGS(&queue))
                            : E_FAIL;
  HRESULT allocator_hr = SUCCEEDED(queue_hr)
                             ? device->CreateCommandAllocator(
                                   D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   IID_PPV_ARGS(&allocator))
                             : E_FAIL;
  HRESULT list_hr = SUCCEEDED(allocator_hr)
                        ? device->CreateCommandList(
                              0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
                              nullptr, IID_PPV_ARGS(&list))
                        : E_FAIL;
  HRESULT list5_hr = list ? list->QueryInterface(IID_PPV_ARGS(&list5)) : E_FAIL;
  D3D12_RESOURCE_DESC target_desc = {};
  target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target_desc.Width = 64;
  target_desc.Height = 64;
  target_desc.DepthOrArraySize = 1;
  target_desc.MipLevels = 1;
  target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  target_desc.SampleDesc.Count = 1;
  target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT target_hr = SUCCEEDED(list5_hr)
                          ? device->CreateCommittedResource(
                                &default_heap, D3D12_HEAP_FLAG_NONE,
                                &target_desc,
                                D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                IID_PPV_ARGS(&target))
                          : E_FAIL;
  D3D12_RESOURCE_DESC shading_rate_desc = {};
  shading_rate_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  shading_rate_desc.Width = 8;
  shading_rate_desc.Height = 8;
  shading_rate_desc.DepthOrArraySize = 1;
  shading_rate_desc.MipLevels = 1;
  shading_rate_desc.Format = DXGI_FORMAT_R8_UINT;
  shading_rate_desc.SampleDesc.Count = 1;
  shading_rate_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  shading_rate_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  HRESULT shading_rate_image_hr = SUCCEEDED(target_hr)
                                       ? device->CreateCommittedResource(
                                             &default_heap,
                                             D3D12_HEAP_FLAG_NONE,
                                             &shading_rate_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr,
                                             IID_PPV_ARGS(&shading_rate_image))
                                       : E_FAIL;
  D3D12_HEAP_PROPERTIES readback_heap =
      heap_properties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC readback_desc = buffer_desc(16384);
  HRESULT readback_hr = SUCCEEDED(target_hr)
                            ? device->CreateCommittedResource(
                                  &readback_heap, D3D12_HEAP_FLAG_NONE,
                                  &readback_desc,
                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&readback))
                            : E_FAIL;
  const float vertices[9] = {-1, -1, 0, 3, -1, 0, -1, 3, 0};
  D3D12_HEAP_PROPERTIES upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  D3D12_RESOURCE_DESC vertex_desc = buffer_desc(sizeof(vertices));
  HRESULT vertex_hr = SUCCEEDED(readback_hr)
                          ? device->CreateCommittedResource(
                                &upload_heap, D3D12_HEAP_FLAG_NONE,
                                &vertex_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                nullptr, IID_PPV_ARGS(&vertex_buffer))
                          : E_FAIL;
  if (SUCCEEDED(vertex_hr)) {
    void *mapped = nullptr;
    D3D12_RANGE range = {0, 0};
    vertex_hr = vertex_buffer->Map(0, &range, &mapped);
    if (SUCCEEDED(vertex_hr) && mapped) {
      std::memcpy(mapped, vertices, sizeof(vertices));
      vertex_buffer->Unmap(0, nullptr);
    }
  }
  const uint8_t shading_rate_values[64] = {
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,
      5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5};
  D3D12_RESOURCE_DESC shading_rate_upload_desc = buffer_desc(2048);
  HRESULT shading_rate_upload_hr = SUCCEEDED(vertex_hr)
                                       ? device->CreateCommittedResource(
                                             &upload_heap, D3D12_HEAP_FLAG_NONE,
                                             &shading_rate_upload_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr,
                                             IID_PPV_ARGS(&shading_rate_upload))
                                       : E_FAIL;
  if (SUCCEEDED(shading_rate_upload_hr) && shading_rate_upload) {
    void *mapped = nullptr;
    D3D12_RANGE range = {0, 0};
    shading_rate_upload_hr = shading_rate_upload->Map(0, &range, &mapped);
    if (SUCCEEDED(shading_rate_upload_hr) && mapped) {
      for (uint32_t y = 0; y < 8; ++y)
        std::memcpy(static_cast<uint8_t *>(mapped) + y * 256,
                    shading_rate_values + y * 8, 8);
      shading_rate_upload->Unmap(0, nullptr);
    }
  }
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = 1;
  HRESULT rtv_hr = SUCCEEDED(vertex_hr)
                       ? device->CreateDescriptorHeap(&rtv_desc,
                                                      IID_PPV_ARGS(&rtv_heap))
                       : E_FAIL;
  if (SUCCEEDED(rtv_hr))
    device->CreateRenderTargetView(
        target, nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());

  HRESULT baseline_record_hr =
      SUCCEEDED(rtv_hr) ? record_draw(
          list, list5, root, pso, target, D3D12_SHADING_RATE_1X1, nullptr,
          readback, vertex_buffer, rtv_heap)
                        : E_FAIL;
  HRESULT baseline_execute_hr =
      SUCCEEDED(baseline_record_hr)
          ? execute_and_wait(device, queue, list)
          : E_FAIL;
  uint32_t baseline_pixels = count_nonzero(readback);

  HRESULT reset_hr = SUCCEEDED(baseline_execute_hr)
                         ? allocator->Reset()
                         : E_FAIL;
  if (SUCCEEDED(reset_hr))
    reset_hr = list->Reset(allocator, nullptr);
  if (SUCCEEDED(reset_hr)) {
    D3D12_RESOURCE_BARRIER restore_barrier = {};
    restore_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore_barrier.Transition.pResource = target;
    restore_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    restore_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    restore_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &restore_barrier);
  }
  const D3D12_SHADING_RATE_COMBINER max_combiners[2] = {
      D3D12_SHADING_RATE_COMBINER_MAX,
      D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
  HRESULT vrs_record_hr =
      SUCCEEDED(reset_hr) ? record_draw(
          list, list5, root, pso, target, D3D12_SHADING_RATE_2X2, nullptr,
          readback, vertex_buffer, rtv_heap, max_combiners)
                          : E_FAIL;
  HRESULT vrs_execute_hr =
      SUCCEEDED(vrs_record_hr) ? execute_and_wait(device, queue, list) : E_FAIL;
  uint32_t vrs_pixels = count_nonzero(readback);

  HRESULT image_reset_hr = SUCCEEDED(vrs_execute_hr)
                               ? allocator->Reset()
                               : E_FAIL;
  if (SUCCEEDED(image_reset_hr))
    image_reset_hr = list->Reset(allocator, nullptr);
  bool image_copy_recorded = false;
  if (SUCCEEDED(image_reset_hr)) {
    D3D12_RESOURCE_BARRIER restore_barrier = {};
    restore_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    restore_barrier.Transition.pResource = target;
    restore_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    restore_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    restore_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &restore_barrier);
    D3D12_TEXTURE_COPY_LOCATION image_source = {};
    image_source.pResource = shading_rate_upload;
    image_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    image_source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
    image_source.PlacedFootprint.Footprint.Width = 8;
    image_source.PlacedFootprint.Footprint.Height = 8;
    image_source.PlacedFootprint.Footprint.Depth = 1;
    image_source.PlacedFootprint.Footprint.RowPitch = 256;
    D3D12_TEXTURE_COPY_LOCATION image_destination = {};
    image_destination.pResource = shading_rate_image;
    image_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&image_destination, 0, 0, 0, &image_source,
                            nullptr);
    D3D12_RESOURCE_BARRIER image_barrier = {};
    image_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    image_barrier.Transition.pResource = shading_rate_image;
    image_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    image_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    image_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    list->ResourceBarrier(1, &image_barrier);
    image_copy_recorded = true;
  }
  HRESULT image_record_hr =
      SUCCEEDED(image_reset_hr) && image_copy_recorded
          ? record_draw(list, list5, root, pso, target,
                        D3D12_SHADING_RATE_1X1, shading_rate_image, readback,
                        vertex_buffer, rtv_heap)
          : E_FAIL;
  HRESULT image_execute_hr =
      SUCCEEDED(image_record_hr) ? execute_and_wait(device, queue, list)
                                 : E_FAIL;
  uint32_t image_pixels = count_nonzero(readback);

  const D3D12_SHADING_RATE rate_matrix_rates[] = {
      D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1,
      D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_4X2,
      D3D12_SHADING_RATE_4X4};
  const char *rate_matrix_names[] = {"1x2", "2x1", "2x4", "4x2", "4x4"};
  uint32_t rate_matrix_pixels[5] = {};
  bool rate_matrix_ok = true;
  HRESULT rate_matrix_last_hr = image_execute_hr;
  for (size_t i = 0; i < 5; ++i) {
    HRESULT matrix_reset_hr = SUCCEEDED(rate_matrix_last_hr)
                                  ? allocator->Reset()
                                  : E_FAIL;
    if (SUCCEEDED(matrix_reset_hr))
      matrix_reset_hr = list->Reset(allocator, nullptr);
    if (SUCCEEDED(matrix_reset_hr)) {
      D3D12_RESOURCE_BARRIER restore_barrier = {};
      restore_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      restore_barrier.Transition.pResource = target;
      restore_barrier.Transition.Subresource =
          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      restore_barrier.Transition.StateBefore =
          D3D12_RESOURCE_STATE_COPY_SOURCE;
      restore_barrier.Transition.StateAfter =
          D3D12_RESOURCE_STATE_RENDER_TARGET;
      list->ResourceBarrier(1, &restore_barrier);
    }
    HRESULT matrix_record_hr =
        SUCCEEDED(matrix_reset_hr)
            ? record_draw(list, list5, root, pso, target,
                          rate_matrix_rates[i], nullptr, readback,
                          vertex_buffer, rtv_heap)
            : E_FAIL;
    HRESULT matrix_execute_hr =
        SUCCEEDED(matrix_record_hr) ? execute_and_wait(device, queue, list)
                                    : E_FAIL;
    rate_matrix_pixels[i] = count_nonzero(readback);
    rate_matrix_ok &= SUCCEEDED(matrix_reset_hr) &&
                      SUCCEEDED(matrix_record_hr) &&
                      SUCCEEDED(matrix_execute_hr) &&
                      rate_matrix_pixels[i] > 0 &&
                      rate_matrix_pixels[i] < baseline_pixels;
    rate_matrix_last_hr = matrix_execute_hr;
  }

  const bool passed = source_ok && vs_dxc == 0 && ps_dxc == 0 && !vs.empty() &&
                      !ps.empty() && SUCCEEDED(device_hr) &&
                      SUCCEEDED(root_serialize_hr) && SUCCEEDED(root_hr) &&
                      SUCCEEDED(pso_hr) && SUCCEEDED(queue_hr) &&
                      SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                      SUCCEEDED(list5_hr) && SUCCEEDED(target_hr) &&
                      SUCCEEDED(shading_rate_image_hr) &&
                      SUCCEEDED(readback_hr) && SUCCEEDED(vertex_hr) &&
                      SUCCEEDED(shading_rate_upload_hr) &&
                      SUCCEEDED(rtv_hr) && SUCCEEDED(baseline_record_hr) &&
                      SUCCEEDED(baseline_execute_hr) && baseline_pixels == 4096 &&
                      SUCCEEDED(reset_hr) && SUCCEEDED(vrs_record_hr) &&
                      SUCCEEDED(vrs_execute_hr) && vrs_pixels > 0 &&
                      vrs_pixels < baseline_pixels &&
                      SUCCEEDED(image_reset_hr) && image_copy_recorded &&
                      SUCCEEDED(image_record_hr) && SUCCEEDED(image_execute_hr) &&
                      image_pixels > 0 && image_pixels < baseline_pixels &&
                      image_pixels == vrs_pixels && rate_matrix_ok &&
                      SUCCEEDED(rate_matrix_last_hr);
  std::printf("{\n");
  std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-vrs.v1\",\n");
  std::printf("  \"pass\": %s,\n", passed ? "true" : "false");
  std::printf("  \"source\": %s,\n", source_ok ? "true" : "false");
  std::printf("  \"vs_dxc_exit\": %lu,\n", (unsigned long)vs_dxc);
  std::printf("  \"ps_dxc_exit\": %lu,\n", (unsigned long)ps_dxc);
  std::printf("  \"device_hr\": \"%s\",\n", hr_hex(device_hr).c_str());
  std::printf("  \"root_serialize_hr\": \"%s\",\n",
              hr_hex(root_serialize_hr).c_str());
  std::printf("  \"root_create_hr\": \"%s\",\n", hr_hex(root_hr).c_str());
  std::printf("  \"pso_hr\": \"%s\",\n", hr_hex(pso_hr).c_str());
  std::printf("  \"shading_rate_image_hr\": \"%s\",\n",
              hr_hex(shading_rate_image_hr).c_str());
  std::printf("  \"shading_rate_upload_hr\": \"%s\",\n",
              hr_hex(shading_rate_upload_hr).c_str());
  std::printf("  \"baseline_record_hr\": \"%s\",\n",
              hr_hex(baseline_record_hr).c_str());
  std::printf("  \"baseline_execute_hr\": \"%s\",\n",
              hr_hex(baseline_execute_hr).c_str());
  std::printf("  \"baseline_pixels\": %u,\n", baseline_pixels);
  std::printf("  \"reset_hr\": \"%s\",\n", hr_hex(reset_hr).c_str());
  std::printf("  \"vrs_record_hr\": \"%s\",\n", hr_hex(vrs_record_hr).c_str());
  std::printf("  \"vrs_execute_hr\": \"%s\",\n",
              hr_hex(vrs_execute_hr).c_str());
  std::printf("  \"vrs_rate\": \"2x2\",\n");
  std::printf("  \"vrs_combiners\": [\"MAX\", \"PASSTHROUGH\"],\n");
  std::printf("  \"vrs_pixels\": %u,\n", vrs_pixels);
  std::printf("  \"image_reset_hr\": \"%s\",\n",
              hr_hex(image_reset_hr).c_str());
  std::printf("  \"image_copy_recorded\": %s,\n",
              image_copy_recorded ? "true" : "false");
  std::printf("  \"image_record_hr\": \"%s\",\n",
              hr_hex(image_record_hr).c_str());
  std::printf("  \"image_execute_hr\": \"%s\",\n",
              hr_hex(image_execute_hr).c_str());
  std::printf("  \"image_rate\": \"2x2\",\n");
  std::printf("  \"image_pixels\": %u,\n", image_pixels);
  std::printf("  \"rate_reduced\": %s,\n",
              (vrs_pixels > 0 && vrs_pixels < baseline_pixels) ? "true"
                                                                 : "false");
  std::printf("  \"image_rate_reduced\": %s,\n",
              (image_pixels > 0 && image_pixels < baseline_pixels &&
               image_pixels == vrs_pixels)
                  ? "true"
                  : "false");
  std::printf("  \"rate_matrix\": [");
  for (size_t i = 0; i < 5; ++i) {
    if (i)
      std::printf(", ");
    std::printf("{\"rate\":\"%s\",\"pixels\":%u}",
                rate_matrix_names[i], rate_matrix_pixels[i]);
  }
  std::printf("],\n");
  std::printf("  \"rate_matrix_verified\": %s,\n",
              rate_matrix_ok ? "true" : "false");
  std::printf("  \"rate_matrix_last_hr\": \"%s\"\n",
              hr_hex(rate_matrix_last_hr).c_str());
  std::printf("}\n");
  std::fflush(stdout);

  release(rtv_heap);
  release(vertex_buffer);
  release(readback);
  release(shading_rate_upload);
  release(shading_rate_image);
  release(target);
  release(list5);
  release(list);
  release(allocator);
  release(queue);
  release(pso);
  release(root);
  release(root_blob);
  release(device);
  if (d3d12)
    FreeLibrary(d3d12);
  return passed ? 0 : 1;
}
