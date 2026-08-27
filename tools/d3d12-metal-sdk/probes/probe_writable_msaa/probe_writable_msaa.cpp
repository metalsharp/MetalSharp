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

using D3D12CreateDeviceFn = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
                                                REFIID, void **);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI *)(
    const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
    ID3DBlob **);

template <typename T> static void safe_release(T *&object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
  T function = nullptr;
  FARPROC address = module ? GetProcAddress(module, name) : nullptr;
  static_assert(sizeof(function) == sizeof(address), "function pointer size mismatch");
  std::memcpy(&function, &address, sizeof(function));
  return function;
}

static std::string hr_hex(HRESULT value) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lx",
                static_cast<unsigned long>(static_cast<uint32_t>(value)));
  return buffer;
}

static bool write_file(const char *path, const char *data) {
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(std::strlen(data));
  const bool ok = WriteFile(file, data, size, &written, nullptr) &&
                  written == size;
  CloseHandle(file);
  return ok;
}

static std::vector<uint8_t> read_file(const char *path) {
  HANDLE file = CreateFileA(path, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE |
                                FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
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
  const bool ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()),
                           &read, nullptr) &&
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
    return 0xffffffffu;
  const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = 0xffffffffu;
  if (wait == WAIT_OBJECT_0)
    GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exit_code;
}

static D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES properties = {};
  properties.Type = type;
  properties.CreationNodeMask = 1;
  properties.VisibleNodeMask = 1;
  return properties;
}

static D3D12_RESOURCE_DESC buffer_desc(UINT64 width) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = width;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return desc;
}

static HRESULT execute_and_wait(ID3D12Device *device,
                                ID3D12CommandQueue *queue,
                                ID3D12GraphicsCommandList *list) {
  HRESULT result = list->Close();
  if (FAILED(result))
    return result;
  ID3D12CommandList *lists[] = {list};
  queue->ExecuteCommandLists(1, lists);

  ID3D12Fence *fence = nullptr;
  result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                               IID_PPV_ARGS(&fence));
  if (FAILED(result))
    return result;
  result = queue->Signal(fence, 1);
  HANDLE event_handle = nullptr;
  if (SUCCEEDED(result))
    event_handle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (SUCCEEDED(result) && !event_handle)
    result = HRESULT_FROM_WIN32(GetLastError());
  if (SUCCEEDED(result))
    result = fence->SetEventOnCompletion(1, event_handle);
  if (SUCCEEDED(result) &&
      WaitForSingleObject(event_handle, 15000) != WAIT_OBJECT_0)
    result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  if (event_handle)
    CloseHandle(event_handle);
  safe_release(fence);
  return result;
}

int main() {
  const char *hlsl_path = "Z:\\tmp\\dxmt_writable_msaa.hlsl";
  const char *store_dxil_path = "Z:\\tmp\\dxmt_writable_msaa_store.dxil";
  const char *load_dxil_path = "Z:\\tmp\\dxmt_writable_msaa_load.dxil";
  const char *graphics_vs_dxil_path = "Z:\\tmp\\dxmt_writable_msaa_graphics_vs.dxil";
  const char *graphics_ps_dxil_path = "Z:\\tmp\\dxmt_writable_msaa_graphics_ps.dxil";
  const char *hlsl = R"(
RWTexture2DMS<float4> target : register(u0);
RWTexture2DMSArray<float4, 4> target_array : register(u1);
RWByteAddressBuffer outbuf : register(u2);

[numthreads(4,1,1)]
void store(uint3 id : SV_DispatchThreadID) {
  uint sample = id.x;
  target.sample[sample][uint2(0,0)] =
      float4((float)(100 + sample), 0.0, 0.0, 1.0);
  target_array.sample[sample][uint3(0,0,1)] =
      float4((float)(200 + sample), 0.0, 0.0, 1.0);
}

[numthreads(4,1,1)]
void load(uint3 id : SV_DispatchThreadID) {
  uint sample = id.x;
  float4 value = target.Load(uint2(0,0), sample);
  float4 array_value = target_array.Load(uint3(0,0,1), sample);
  outbuf.Store(sample * 4, (uint)value.x);
  outbuf.Store((sample + 4) * 4, (uint)array_value.x);
  if (sample == 0) {
    target.sample[0][uint2(0,0)] = value;
    target_array.sample[0][uint3(0,0,1)] = array_value;
  }
}

struct GraphicsVSIn { float3 position : POSITION; };
struct GraphicsVSOut { float4 position : SV_Position; };
GraphicsVSOut graphics_vs(GraphicsVSIn input) {
  GraphicsVSOut output;
  output.position = float4(input.position, 1.0);
  return output;
}
float4 graphics_ps(GraphicsVSOut input) : SV_Target0 {
  target.sample[0][uint2(0,0)] = float4(300.0, 0.0, 0.0, 1.0);
  target_array.sample[0][uint3(0,0,1)] = float4(400.0, 0.0, 0.0, 1.0);
  return float4(0.0, 1.0, 0.0, 1.0);
}
)";

  const bool source_ok = write_file(hlsl_path, hlsl);
  DeleteFileA(store_dxil_path);
  DeleteFileA(load_dxil_path);
  DeleteFileA(graphics_vs_dxil_path);
  DeleteFileA(graphics_ps_dxil_path);
  const DWORD dxc_store = run_process(
      "dxc.exe -nologo -T cs_6_7 -E store -HV 2021 -Fo "
      "Z:\\tmp\\dxmt_writable_msaa_store.dxil "
      "Z:\\tmp\\dxmt_writable_msaa.hlsl");
  const std::vector<uint8_t> store_dxil = read_file(store_dxil_path);
  DeleteFileA(store_dxil_path);
  const DWORD dxc_load = run_process(
      "dxc.exe -nologo -T cs_6_7 -E load -HV 2021 -Fo "
      "Z:\\tmp\\dxmt_writable_msaa_load.dxil "
      "Z:\\tmp\\dxmt_writable_msaa.hlsl");
  const std::vector<uint8_t> load_dxil = read_file(load_dxil_path);
  DeleteFileA(load_dxil_path);
  const DWORD dxc_graphics_vs = run_process(
      "dxc.exe -nologo -T vs_6_0 -E graphics_vs -HV 2021 -Fo "
      "Z:\\tmp\\dxmt_writable_msaa_graphics_vs.dxil "
      "Z:\\tmp\\dxmt_writable_msaa.hlsl");
  const std::vector<uint8_t> graphics_vs_dxil = read_file(graphics_vs_dxil_path);
  DeleteFileA(graphics_vs_dxil_path);
  const DWORD dxc_graphics_ps = run_process(
      "dxc.exe -nologo -T ps_6_7 -E graphics_ps -HV 2021 -Fo "
      "Z:\\tmp\\dxmt_writable_msaa_graphics_ps.dxil "
      "Z:\\tmp\\dxmt_writable_msaa.hlsl");
  const std::vector<uint8_t> graphics_ps_dxil = read_file(graphics_ps_dxil_path);
  DeleteFileA(graphics_ps_dxil_path);

  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  auto create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
  auto serialize_root =
      load_proc<D3D12SerializeRootSignatureFn>(d3d12,
                                               "D3D12SerializeRootSignature");

  ID3D12Device *device = nullptr;
  HRESULT create_hr = create_device
                          ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                          IID_D3D12DeviceProbe,
                                          reinterpret_cast<void **>(&device))
                          : E_FAIL;

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  range.NumDescriptors = 3;
  range.BaseShaderRegister = 0;
  D3D12_ROOT_PARAMETER parameter = {};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable.NumDescriptorRanges = 1;
  parameter.DescriptorTable.pDescriptorRanges = &range;
  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 1;
  root_desc.pParameters = &parameter;

  ID3DBlob *root_blob = nullptr;
  ID3DBlob *root_errors = nullptr;
  HRESULT root_serialize_hr =
      serialize_root && device
          ? serialize_root(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                           &root_blob, &root_errors)
          : E_FAIL;
  if (root_errors)
    root_errors->Release();

  ID3D12RootSignature *root = nullptr;
  HRESULT root_create_hr =
      SUCCEEDED(root_serialize_hr) && root_blob
          ? device->CreateRootSignature(0, root_blob->GetBufferPointer(),
                                        root_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&root))
          : E_FAIL;

  D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = {};
  pipeline_desc.pRootSignature = root;
  pipeline_desc.CS.pShaderBytecode = store_dxil.data();
  pipeline_desc.CS.BytecodeLength = store_dxil.size();
  ID3D12PipelineState *store_pso = nullptr;
  ID3D12PipelineState *graphics_pso = nullptr;
  HRESULT store_pso_hr =
      device && root && !store_dxil.empty()
          ? device->CreateComputePipelineState(&pipeline_desc,
                                               IID_PPV_ARGS(&store_pso))
          : E_FAIL;
  pipeline_desc.CS.pShaderBytecode = load_dxil.data();
  pipeline_desc.CS.BytecodeLength = load_dxil.size();
  ID3D12PipelineState *load_pso = nullptr;
  HRESULT load_pso_hr =
      device && root && !load_dxil.empty()
          ? device->CreateComputePipelineState(&pipeline_desc,
                                               IID_PPV_ARGS(&load_pso))
          : E_FAIL;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_pipeline_desc = {};
  graphics_pipeline_desc.pRootSignature = root;
  graphics_pipeline_desc.VS.pShaderBytecode = graphics_vs_dxil.data();
  graphics_pipeline_desc.VS.BytecodeLength = graphics_vs_dxil.size();
  graphics_pipeline_desc.PS.pShaderBytecode = graphics_ps_dxil.data();
  graphics_pipeline_desc.PS.BytecodeLength = graphics_ps_dxil.size();
  graphics_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics_pipeline_desc.SampleMask = UINT_MAX;
  graphics_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  graphics_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  graphics_pipeline_desc.RasterizerState.DepthClipEnable = TRUE;
  graphics_pipeline_desc.DepthStencilState.DepthEnable = TRUE;
  graphics_pipeline_desc.DepthStencilState.DepthWriteMask =
      D3D12_DEPTH_WRITE_MASK_ALL;
  graphics_pipeline_desc.DepthStencilState.DepthFunc =
      D3D12_COMPARISON_FUNC_ALWAYS;
  graphics_pipeline_desc.DepthStencilState.StencilEnable = FALSE;
  graphics_pipeline_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  D3D12_INPUT_ELEMENT_DESC graphics_input = {
      "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
      D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  graphics_pipeline_desc.InputLayout = {&graphics_input, 1};
  graphics_pipeline_desc.PrimitiveTopologyType =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics_pipeline_desc.NumRenderTargets = 1;
  graphics_pipeline_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  graphics_pipeline_desc.SampleDesc.Count = 1;
  HRESULT graphics_pso_hr =
      device && root && !graphics_vs_dxil.empty() && !graphics_ps_dxil.empty()
          ? device->CreateGraphicsPipelineState(&graphics_pipeline_desc,
                                                 IID_PPV_ARGS(&graphics_pso))
          : E_FAIL;

  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12DescriptorHeap *heap = nullptr;
  ID3D12DescriptorHeap *rtv_heap = nullptr;
  ID3D12DescriptorHeap *dsv_heap = nullptr;
  ID3D12Resource *target = nullptr;
  ID3D12Resource *target_array = nullptr;
  ID3D12Resource *resolve_target = nullptr;
  ID3D12Resource *resolve_array_target = nullptr;
  ID3D12Resource *resolve_readback = nullptr;
  ID3D12Resource *resolve_array_readback = nullptr;
  ID3D12Resource *outbuf = nullptr;
  ID3D12Resource *readback = nullptr;
  ID3D12Resource *graphics_target = nullptr;
  ID3D12Resource *graphics_readback = nullptr;
  ID3D12Resource *depth_target = nullptr;
  ID3D12Resource *vertex_buffer = nullptr;
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  HRESULT queue_hr = device
                         ? device->CreateCommandQueue(
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
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = 3;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  HRESULT heap_hr = SUCCEEDED(list_hr)
                        ? device->CreateDescriptorHeap(&heap_desc,
                                                       IID_PPV_ARGS(&heap))
                        : E_FAIL;

  D3D12_RESOURCE_DESC target_desc = {};
  target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  target_desc.Width = 1;
  target_desc.Height = 1;
  target_desc.DepthOrArraySize = 1;
  target_desc.MipLevels = 1;
  target_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  target_desc.SampleDesc.Count = 4;
  target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  D3D12_HEAP_PROPERTIES default_heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
  HRESULT target_hr = SUCCEEDED(heap_hr)
                          ? device->CreateCommittedResource(
                                &default_heap, D3D12_HEAP_FLAG_NONE,
                                &target_desc,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                IID_PPV_ARGS(&target))
                          : E_FAIL;
  D3D12_RESOURCE_DESC target_array_desc = target_desc;
  target_array_desc.DepthOrArraySize = 2;
  HRESULT target_array_hr = SUCCEEDED(target_hr)
                                ? device->CreateCommittedResource(
                                      &default_heap, D3D12_HEAP_FLAG_NONE,
                                      &target_array_desc,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      nullptr, IID_PPV_ARGS(&target_array))
                                : E_FAIL;
  D3D12_RESOURCE_DESC resolve_desc = target_desc;
  resolve_desc.SampleDesc.Count = 1;
  resolve_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  HRESULT resolve_target_hr = SUCCEEDED(target_array_hr)
                                  ? device->CreateCommittedResource(
                                        &default_heap, D3D12_HEAP_FLAG_NONE,
                                        &resolve_desc,
                                        D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                        nullptr,
                                        IID_PPV_ARGS(&resolve_target))
                                  : E_FAIL;
  D3D12_RESOURCE_DESC resolve_array_desc = resolve_desc;
  resolve_array_desc.DepthOrArraySize = 2;
  HRESULT resolve_array_target_hr = SUCCEEDED(resolve_target_hr)
                                        ? device->CreateCommittedResource(
                                              &default_heap,
                                              D3D12_HEAP_FLAG_NONE,
                                              &resolve_array_desc,
                                              D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                              nullptr,
                                              IID_PPV_ARGS(&resolve_array_target))
                                        : E_FAIL;
  D3D12_RESOURCE_DESC output_desc = buffer_desc(32);
  HRESULT outbuf_hr = SUCCEEDED(target_array_hr)
                          ? device->CreateCommittedResource(
                                &default_heap, D3D12_HEAP_FLAG_NONE,
                                &output_desc,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                IID_PPV_ARGS(&outbuf))
                          : E_FAIL;
  D3D12_HEAP_PROPERTIES readback_heap =
      heap_properties(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC readback_desc = buffer_desc(16);
  readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  HRESULT readback_hr = SUCCEEDED(outbuf_hr)
                            ? device->CreateCommittedResource(
                                  &readback_heap, D3D12_HEAP_FLAG_NONE,
                                  &readback_desc,
                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&readback))
                            : E_FAIL;

  D3D12_RESOURCE_DESC graphics_target_desc = {};
  graphics_target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  graphics_target_desc.Width = 1;
  graphics_target_desc.Height = 1;
  graphics_target_desc.DepthOrArraySize = 1;
  graphics_target_desc.MipLevels = 1;
  graphics_target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  graphics_target_desc.SampleDesc.Count = 1;
  graphics_target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  graphics_target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  HRESULT graphics_target_hr = SUCCEEDED(readback_hr)
                                    ? device->CreateCommittedResource(
                                          &default_heap, D3D12_HEAP_FLAG_NONE,
                                          &graphics_target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          nullptr,
                                          IID_PPV_ARGS(&graphics_target))
                                    : E_FAIL;
  D3D12_RESOURCE_DESC depth_desc = graphics_target_desc;
  depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depth_clear = {};
  depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
  depth_clear.DepthStencil.Depth = 1.0f;
  depth_clear.DepthStencil.Stencil = 0;
  HRESULT depth_target_hr = SUCCEEDED(graphics_target_hr)
                                ? device->CreateCommittedResource(
                                      &default_heap, D3D12_HEAP_FLAG_NONE,
                                      &depth_desc,
                                      D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                      &depth_clear,
                                      IID_PPV_ARGS(&depth_target))
                                : E_FAIL;
  D3D12_HEAP_PROPERTIES upload_heap =
      heap_properties(D3D12_HEAP_TYPE_UPLOAD);
  const float graphics_vertices[9] = {
      -1.0f, -1.0f, 0.0f,
       3.0f, -1.0f, 0.0f,
      -1.0f,  3.0f, 0.0f,
  };
  D3D12_RESOURCE_DESC vertex_desc = buffer_desc(sizeof(graphics_vertices));
  vertex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  HRESULT vertex_buffer_hr = SUCCEEDED(graphics_target_hr)
                                 ? device->CreateCommittedResource(
                                       &upload_heap, D3D12_HEAP_FLAG_NONE,
                                       &vertex_desc,
                                       D3D12_RESOURCE_STATE_GENERIC_READ,
                                       nullptr,
                                       IID_PPV_ARGS(&vertex_buffer))
                                 : E_FAIL;
  if (SUCCEEDED(vertex_buffer_hr) && vertex_buffer) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 0};
    vertex_buffer_hr = vertex_buffer->Map(0, &read_range, &mapped);
    if (SUCCEEDED(vertex_buffer_hr) && mapped) {
      std::memcpy(mapped, graphics_vertices, sizeof(graphics_vertices));
      D3D12_RANGE written = {0, sizeof(graphics_vertices)};
      vertex_buffer->Unmap(0, &written);
    }
  }
  D3D12_RESOURCE_DESC graphics_readback_desc = buffer_desc(512);
  graphics_readback_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  HRESULT graphics_readback_hr = SUCCEEDED(vertex_buffer_hr)
                                     ? device->CreateCommittedResource(
                                           &readback_heap,
                                           D3D12_HEAP_FLAG_NONE,
                                           &graphics_readback_desc,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr,
                                           IID_PPV_ARGS(&graphics_readback))
                                     : E_FAIL;
  HRESULT resolve_readback_hr = SUCCEEDED(graphics_readback_hr)
                                    ? device->CreateCommittedResource(
                                          &readback_heap, D3D12_HEAP_FLAG_NONE,
                                          &graphics_readback_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr,
                                          IID_PPV_ARGS(&resolve_readback))
                                    : E_FAIL;
  HRESULT resolve_array_readback_hr = SUCCEEDED(resolve_readback_hr)
                                          ? device->CreateCommittedResource(
                                                &readback_heap,
                                                D3D12_HEAP_FLAG_NONE,
                                                &graphics_readback_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr,
                                                IID_PPV_ARGS(&resolve_array_readback))
                                          : E_FAIL;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  HRESULT rtv_heap_hr = SUCCEEDED(graphics_readback_hr)
                            ? device->CreateDescriptorHeap(
                                  &rtv_heap_desc, IID_PPV_ARGS(&rtv_heap))
                            : E_FAIL;
  if (SUCCEEDED(rtv_heap_hr) && rtv_heap) {
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(
        graphics_target, &rtv_desc, rtv_heap->GetCPUDescriptorHandleForHeapStart());
  }
  D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {};
  dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv_heap_desc.NumDescriptors = 1;
  HRESULT dsv_heap_hr = SUCCEEDED(rtv_heap_hr)
                            ? device->CreateDescriptorHeap(
                                  &dsv_heap_desc, IID_PPV_ARGS(&dsv_heap))
                            : E_FAIL;
  if (SUCCEEDED(dsv_heap_hr) && dsv_heap) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(
        depth_target, &dsv_desc, dsv_heap->GetCPUDescriptorHandleForHeapStart());
  }

  HRESULT execute_hr = E_FAIL;
  if (SUCCEEDED(readback_hr) && store_pso && load_pso && graphics_pso &&
      SUCCEEDED(graphics_target_hr) && SUCCEEDED(depth_target_hr) &&
      SUCCEEDED(graphics_readback_hr) && SUCCEEDED(resolve_readback_hr) &&
      SUCCEEDED(resolve_array_readback_hr) &&
      SUCCEEDED(rtv_heap_hr) && SUCCEEDED(dsv_heap_hr) &&
      SUCCEEDED(resolve_target_hr) && SUCCEEDED(resolve_array_target_hr) &&
      vertex_buffer) {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC target_uav = {};
    target_uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(target, nullptr, &target_uav, cpu);
    cpu.ptr += device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC target_array_uav = {};
    target_array_uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    target_array_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    target_array_uav.Texture2DArray.ArraySize = 2;
    device->CreateUnorderedAccessView(target_array, nullptr,
                                      &target_array_uav, cpu);
    cpu.ptr += device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC outbuf_uav = {};
    outbuf_uav.Format = DXGI_FORMAT_R32_TYPELESS;
    outbuf_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    outbuf_uav.Buffer.NumElements = 8;
    outbuf_uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(outbuf, nullptr, &outbuf_uav, cpu);

    ID3D12DescriptorHeap *heaps[] = {heap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root);
    list->SetComputeRootDescriptorTable(
        0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetPipelineState(store_pso);
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER target_uav_barrier = {};
    target_uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    target_uav_barrier.UAV.pResource = target;
    list->ResourceBarrier(1, &target_uav_barrier);

    list->SetGraphicsRootSignature(root);
    list->SetPipelineState(graphics_pso);
    D3D12_CPU_DESCRIPTOR_HANDLE graphics_rtv =
        rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE graphics_dsv =
        dsv_heap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &graphics_rtv, FALSE, &graphics_dsv);
    list->ClearRenderTargetView(graphics_rtv, clear_color, 0, nullptr);
    list->ClearDepthStencilView(graphics_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0,
                                0, nullptr);
    D3D12_VIEWPORT viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, 1, 1};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    vertex_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
    vertex_view.SizeInBytes = sizeof(graphics_vertices);
    vertex_view.StrideInBytes = sizeof(float) * 3;
    list->IASetVertexBuffers(0, 1, &vertex_view);
    list->SetGraphicsRootDescriptorTable(
        0, heap->GetGPUDescriptorHandleForHeapStart());
    list->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER graphics_target_barrier = {};
    graphics_target_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    graphics_target_barrier.Transition.pResource = graphics_target;
    graphics_target_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    graphics_target_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    graphics_target_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &graphics_target_barrier);
    D3D12_TEXTURE_COPY_LOCATION graphics_src = {};
    graphics_src.pResource = graphics_target;
    graphics_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION graphics_dst = {};
    graphics_dst.pResource = graphics_readback;
    graphics_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    graphics_dst.PlacedFootprint.Offset = 0;
    graphics_dst.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;
    graphics_dst.PlacedFootprint.Footprint.Width = 1;
    graphics_dst.PlacedFootprint.Footprint.Height = 1;
    graphics_dst.PlacedFootprint.Footprint.Depth = 1;
    graphics_dst.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&graphics_dst, 0, 0, 0, &graphics_src, nullptr);
    list->ResolveSubresource(resolve_target, 0, target, 0,
                             DXGI_FORMAT_R32G32B32A32_FLOAT);
    D3D12_RESOURCE_BARRIER resolve_barrier = {};
    resolve_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    resolve_barrier.Transition.pResource = resolve_target;
    resolve_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    resolve_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RESOLVE_DEST;
    resolve_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &resolve_barrier);
    D3D12_TEXTURE_COPY_LOCATION resolve_src = {};
    resolve_src.pResource = resolve_target;
    resolve_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION resolve_dst = {};
    resolve_dst.pResource = resolve_readback;
    resolve_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    resolve_dst.PlacedFootprint.Offset = 0;
    resolve_dst.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_R32G32B32A32_FLOAT;
    resolve_dst.PlacedFootprint.Footprint.Width = 1;
    resolve_dst.PlacedFootprint.Footprint.Height = 1;
    resolve_dst.PlacedFootprint.Footprint.Depth = 1;
    resolve_dst.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&resolve_dst, 0, 0, 0, &resolve_src, nullptr);

    list->ResolveSubresource(resolve_array_target, 1, target_array, 1,
                             DXGI_FORMAT_R32G32B32A32_FLOAT);
    D3D12_RESOURCE_BARRIER resolve_array_barrier = {};
    resolve_array_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    resolve_array_barrier.Transition.pResource = resolve_array_target;
    resolve_array_barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    resolve_array_barrier.Transition.StateBefore =
        D3D12_RESOURCE_STATE_RESOLVE_DEST;
    resolve_array_barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &resolve_array_barrier);
    D3D12_TEXTURE_COPY_LOCATION resolve_array_src = {};
    resolve_array_src.pResource = resolve_array_target;
    resolve_array_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    resolve_array_src.SubresourceIndex = 1;
    D3D12_TEXTURE_COPY_LOCATION resolve_array_dst = {};
    resolve_array_dst.pResource = resolve_array_readback;
    resolve_array_dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    resolve_array_dst.PlacedFootprint.Offset = 0;
    resolve_array_dst.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_R32G32B32A32_FLOAT;
    resolve_array_dst.PlacedFootprint.Footprint.Width = 1;
    resolve_array_dst.PlacedFootprint.Footprint.Height = 1;
    resolve_array_dst.PlacedFootprint.Footprint.Depth = 1;
    resolve_array_dst.PlacedFootprint.Footprint.RowPitch = 256;
    list->CopyTextureRegion(&resolve_array_dst, 0, 0, 0, &resolve_array_src,
                            nullptr);

    D3D12_RESOURCE_BARRIER target_after_graphics = {};
    target_after_graphics.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    target_after_graphics.UAV.pResource = target;
    list->ResourceBarrier(1, &target_after_graphics);
    list->SetComputeRootSignature(root);
    list->SetComputeRootDescriptorTable(
        0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetPipelineState(load_pso);
    list->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barriers[0].UAV.pResource = target;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = outbuf;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(2, barriers);
    list->CopyResource(readback, outbuf);
    execute_hr = execute_and_wait(device, queue, list);
  }

  uint32_t values[8] = {};
  uint32_t graphics_color = 0;
  float resolve_value = 0.0f;
  float resolve_array_value = 0.0f;
  HRESULT map_hr = E_FAIL;
  HRESULT graphics_map_hr = E_FAIL;
  HRESULT resolve_map_hr = E_FAIL;
  HRESULT resolve_array_map_hr = E_FAIL;
  if (SUCCEEDED(execute_hr) && readback) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 32};
    map_hr = readback->Map(0, &read_range, &mapped);
    if (SUCCEEDED(map_hr) && mapped)
      std::memcpy(values, mapped, sizeof(values));
    if (mapped)
      readback->Unmap(0, nullptr);
  }

  if (SUCCEEDED(execute_hr) && graphics_readback) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 256};
    graphics_map_hr = graphics_readback->Map(
        0, &read_range, &mapped);
    if (SUCCEEDED(graphics_map_hr) && mapped)
      std::memcpy(&graphics_color, mapped, sizeof(graphics_color));
    if (mapped)
      graphics_readback->Unmap(0, nullptr);
  }

  const bool values_ok = values[0] == 300 && values[1] == 101 &&
                         values[2] == 102 && values[3] == 103 &&
                         values[4] == 400 && values[5] == 201 &&
                         values[6] == 202 && values[7] == 203;
  if (SUCCEEDED(execute_hr) && resolve_readback) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 256};
    resolve_map_hr = resolve_readback->Map(0, &read_range, &mapped);
    if (SUCCEEDED(resolve_map_hr) && mapped)
      std::memcpy(&resolve_value, mapped, sizeof(resolve_value));
    if (mapped)
      resolve_readback->Unmap(0, nullptr);
  }

  if (SUCCEEDED(execute_hr) && resolve_array_readback) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 256};
    resolve_array_map_hr = resolve_array_readback->Map(
        0, &read_range, &mapped);
    if (SUCCEEDED(resolve_array_map_hr) && mapped)
      std::memcpy(&resolve_array_value, mapped, sizeof(resolve_array_value));
    if (mapped)
      resolve_array_readback->Unmap(0, nullptr);
  }

  const bool graphics_color_ok = graphics_color == 0xff00ff00u;
  const bool resolve_value_ok = resolve_value == 151.5f;
  const bool resolve_array_value_ok = resolve_array_value == 251.5f;
  const bool pass = source_ok && dxc_store == 0 && dxc_load == 0 &&
                    dxc_graphics_vs == 0 && dxc_graphics_ps == 0 &&
                    !store_dxil.empty() && !load_dxil.empty() &&
                    !graphics_vs_dxil.empty() && !graphics_ps_dxil.empty() &&
                    SUCCEEDED(create_hr) && SUCCEEDED(root_serialize_hr) &&
                    SUCCEEDED(root_create_hr) && SUCCEEDED(store_pso_hr) &&
                    SUCCEEDED(load_pso_hr) && SUCCEEDED(graphics_pso_hr) &&
                    SUCCEEDED(queue_hr) && SUCCEEDED(allocator_hr) &&
                    SUCCEEDED(list_hr) && SUCCEEDED(heap_hr) &&
                    SUCCEEDED(target_hr) && SUCCEEDED(target_array_hr) &&
                    SUCCEEDED(outbuf_hr) && SUCCEEDED(readback_hr) &&
                    SUCCEEDED(resolve_target_hr) &&
                    SUCCEEDED(resolve_array_target_hr) &&
                    SUCCEEDED(graphics_target_hr) &&
                    SUCCEEDED(depth_target_hr) &&
                    SUCCEEDED(vertex_buffer_hr) &&
                    SUCCEEDED(graphics_readback_hr) &&
                    SUCCEEDED(resolve_readback_hr) &&
                    SUCCEEDED(resolve_array_readback_hr) &&
                    SUCCEEDED(rtv_heap_hr) && SUCCEEDED(dsv_heap_hr) &&
                    SUCCEEDED(execute_hr) && SUCCEEDED(map_hr) && values_ok &&
                    SUCCEEDED(graphics_map_hr) && graphics_color_ok &&
                    SUCCEEDED(resolve_map_hr) && resolve_value_ok &&
                    SUCCEEDED(resolve_array_map_hr) &&
                    resolve_array_value_ok;

  std::printf("{\n");
  std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-writable-msaa.v1\",\n");
  std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
  std::printf("  \"source\": %s,\n", source_ok ? "true" : "false");
  std::printf("  \"dxc_store_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_store));
  std::printf("  \"dxc_load_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_load));
  std::printf("  \"dxc_graphics_vs_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_graphics_vs));
  std::printf("  \"dxc_graphics_ps_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_graphics_ps));
  std::printf("  \"store_dxil_size\": %zu,\n", store_dxil.size());
  std::printf("  \"load_dxil_size\": %zu,\n", load_dxil.size());
  std::printf("  \"graphics_vs_dxil_size\": %zu,\n", graphics_vs_dxil.size());
  std::printf("  \"graphics_ps_dxil_size\": %zu,\n", graphics_ps_dxil.size());
  std::printf("  \"device_create_hr\": \"%s\",\n",
              hr_hex(create_hr).c_str());
  std::printf("  \"root_serialize_hr\": \"%s\",\n",
              hr_hex(root_serialize_hr).c_str());
  std::printf("  \"root_create_hr\": \"%s\",\n",
              hr_hex(root_create_hr).c_str());
  std::printf("  \"store_pso_hr\": \"%s\",\n",
              hr_hex(store_pso_hr).c_str());
  std::printf("  \"load_pso_hr\": \"%s\",\n",
              hr_hex(load_pso_hr).c_str());
  std::printf("  \"graphics_pso_hr\": \"%s\",\n",
              hr_hex(graphics_pso_hr).c_str());
  std::printf("  \"target_create_hr\": \"%s\",\n",
              hr_hex(target_hr).c_str());
  std::printf("  \"target_array_create_hr\": \"%s\",\n",
              hr_hex(target_array_hr).c_str());
  std::printf("  \"resolve_target_hr\": \"%s\",\n",
              hr_hex(resolve_target_hr).c_str());
  std::printf("  \"resolve_array_target_hr\": \"%s\",\n",
              hr_hex(resolve_array_target_hr).c_str());
  std::printf("  \"resolve_readback_hr\": \"%s\",\n",
              hr_hex(resolve_readback_hr).c_str());
  std::printf("  \"resolve_array_readback_hr\": \"%s\",\n",
              hr_hex(resolve_array_readback_hr).c_str());
  std::printf("  \"execute_hr\": \"%s\",\n",
              hr_hex(execute_hr).c_str());
  std::printf("  \"readback_map_hr\": \"%s\",\n",
              hr_hex(map_hr).c_str());
  std::printf("  \"graphics_target_hr\": \"%s\",\n",
              hr_hex(graphics_target_hr).c_str());
  std::printf("  \"depth_target_hr\": \"%s\",\n",
              hr_hex(depth_target_hr).c_str());
  std::printf("  \"vertex_buffer_hr\": \"%s\",\n",
              hr_hex(vertex_buffer_hr).c_str());
  std::printf("  \"graphics_readback_hr\": \"%s\",\n",
              hr_hex(graphics_readback_hr).c_str());
  std::printf("  \"rtv_heap_hr\": \"%s\",\n",
              hr_hex(rtv_heap_hr).c_str());
  std::printf("  \"dsv_heap_hr\": \"%s\",\n",
              hr_hex(dsv_heap_hr).c_str());
  std::printf("  \"graphics_map_hr\": \"%s\",\n",
              hr_hex(graphics_map_hr).c_str());
  std::printf("  \"resolve_map_hr\": \"%s\",\n",
              hr_hex(resolve_map_hr).c_str());
  std::printf("  \"resolve_value\": %.1f,\n", resolve_value);
  std::printf("  \"resolve_value_verified\": %s,\n",
              resolve_value_ok ? "true" : "false");
  std::printf("  \"resolve_array_map_hr\": \"%s\",\n",
              hr_hex(resolve_array_map_hr).c_str());
  std::printf("  \"resolve_array_value\": %.1f,\n", resolve_array_value);
  std::printf("  \"resolve_array_value_verified\": %s,\n",
              resolve_array_value_ok ? "true" : "false");
  std::printf("  \"values\": [%u, %u, %u, %u, %u, %u, %u, %u],\n",
              values[0], values[1], values[2], values[3], values[4], values[5],
              values[6], values[7]);
  std::printf("  \"values_verified\": %s,\n",
              values_ok ? "true" : "false");
  std::printf("  \"graphics_color\": \"0x%08x\",\n", graphics_color);
  std::printf("  \"graphics_color_verified\": %s\n",
              graphics_color_ok ? "true" : "false");
  std::printf("}\n");
  std::fflush(stdout);

  safe_release(graphics_readback);
  safe_release(resolve_array_readback);
  safe_release(resolve_readback);
  safe_release(vertex_buffer);
  safe_release(depth_target);
  safe_release(graphics_target);
  safe_release(dsv_heap);
  safe_release(rtv_heap);
  safe_release(readback);
  safe_release(outbuf);
  safe_release(resolve_target);
  safe_release(target_array);
  safe_release(target);
  safe_release(heap);
  safe_release(list);
  safe_release(allocator);
  safe_release(queue);
  safe_release(graphics_pso);
  safe_release(load_pso);
  safe_release(store_pso);
  safe_release(root);
  safe_release(root_blob);
  safe_release(device);
  TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
  return pass ? 0 : 1;
}
