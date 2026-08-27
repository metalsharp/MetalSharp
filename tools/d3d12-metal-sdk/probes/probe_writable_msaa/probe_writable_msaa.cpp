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
)";

  const bool source_ok = write_file(hlsl_path, hlsl);
  DeleteFileA(store_dxil_path);
  DeleteFileA(load_dxil_path);
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

  ID3D12CommandQueue *queue = nullptr;
  ID3D12CommandAllocator *allocator = nullptr;
  ID3D12GraphicsCommandList *list = nullptr;
  ID3D12DescriptorHeap *heap = nullptr;
  ID3D12Resource *target = nullptr;
  ID3D12Resource *target_array = nullptr;
  ID3D12Resource *outbuf = nullptr;
  ID3D12Resource *readback = nullptr;
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

  HRESULT execute_hr = E_FAIL;
  if (SUCCEEDED(readback_hr) && store_pso && load_pso) {
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
  HRESULT map_hr = E_FAIL;
  if (SUCCEEDED(execute_hr) && readback) {
    void *mapped = nullptr;
    D3D12_RANGE read_range = {0, 32};
    map_hr = readback->Map(0, &read_range, &mapped);
    if (SUCCEEDED(map_hr) && mapped)
      std::memcpy(values, mapped, sizeof(values));
    if (mapped)
      readback->Unmap(0, nullptr);
  }

  const bool values_ok = values[0] == 100 && values[1] == 101 &&
                         values[2] == 102 && values[3] == 103 &&
                         values[4] == 200 && values[5] == 201 &&
                         values[6] == 202 && values[7] == 203;
  const bool pass = source_ok && dxc_store == 0 && dxc_load == 0 &&
                    !store_dxil.empty() && !load_dxil.empty() &&
                    SUCCEEDED(create_hr) && SUCCEEDED(root_serialize_hr) &&
                    SUCCEEDED(root_create_hr) && SUCCEEDED(store_pso_hr) &&
                    SUCCEEDED(load_pso_hr) && SUCCEEDED(queue_hr) &&
                    SUCCEEDED(allocator_hr) && SUCCEEDED(list_hr) &&
                    SUCCEEDED(heap_hr) && SUCCEEDED(target_hr) &&
                    SUCCEEDED(target_array_hr) && SUCCEEDED(outbuf_hr) &&
                    SUCCEEDED(readback_hr) &&
                    SUCCEEDED(execute_hr) && SUCCEEDED(map_hr) && values_ok;

  std::printf("{\n");
  std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-writable-msaa.v1\",\n");
  std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
  std::printf("  \"source\": %s,\n", source_ok ? "true" : "false");
  std::printf("  \"dxc_store_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_store));
  std::printf("  \"dxc_load_exit\": %lu,\n",
              static_cast<unsigned long>(dxc_load));
  std::printf("  \"store_dxil_size\": %zu,\n", store_dxil.size());
  std::printf("  \"load_dxil_size\": %zu,\n", load_dxil.size());
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
  std::printf("  \"target_create_hr\": \"%s\",\n",
              hr_hex(target_hr).c_str());
  std::printf("  \"target_array_create_hr\": \"%s\",\n",
              hr_hex(target_array_hr).c_str());
  std::printf("  \"execute_hr\": \"%s\",\n",
              hr_hex(execute_hr).c_str());
  std::printf("  \"readback_map_hr\": \"%s\",\n",
              hr_hex(map_hr).c_str());
  std::printf("  \"values\": [%u, %u, %u, %u, %u, %u, %u, %u],\n",
              values[0], values[1], values[2], values[3], values[4], values[5],
              values[6], values[7]);
  std::printf("  \"values_verified\": %s\n",
              values_ok ? "true" : "false");
  std::printf("}\n");
  std::fflush(stdout);

  safe_release(readback);
  safe_release(outbuf);
  safe_release(target_array);
  safe_release(target);
  safe_release(heap);
  safe_release(list);
  safe_release(allocator);
  safe_release(queue);
  safe_release(load_pso);
  safe_release(store_pso);
  safe_release(root);
  safe_release(root_blob);
  safe_release(device);
  TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
  return pass ? 0 : 1;
}
