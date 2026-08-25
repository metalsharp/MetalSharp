#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>

extern "C" {
__declspec(dllexport) UINT D3D12SDKVersion = 619;
__declspec(dllexport) char D3D12SDKPath[260] = ".\\D3D12\\";
}

static const GUID kDeviceIid = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID kPayloadGuid = {
    0xf2fb1880, 0x6d85, 0x4883,
    {0xb6, 0x47, 0x5b, 0x4c, 0x4b, 0xcf, 0x27, 0x0d}};
static const GUID kInterfaceGuid = {
    0x0905e917, 0x82b2, 0x4eaa,
    {0xb2, 0x86, 0x50, 0x3d, 0xd2, 0x9f, 0x7f, 0x0e}};

static std::string getenv_string(const char *key) {
  DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
  if (!needed)
    return "";
  std::string value(needed, '\0');
  DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
  if (!written)
    return "";
  value.resize(written);
  return value;
}

static std::string json_escape(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == '\\') output += "\\\\";
    else if (c == '"') output += "\\\"";
    else if (c == '\n') output += "\\n";
    else if (c == '\r') output += "\\r";
    else if (c == '\t') output += "\\t";
    else output += c;
  }
  return output;
}

struct ObjectResult {
  std::string name;
  HRESULT create_hr = E_FAIL;
  HRESULT set_hr = E_FAIL;
  HRESULT size_hr = E_FAIL;
  HRESULT short_hr = E_FAIL;
  HRESULT get_hr = E_FAIL;
  HRESULT interface_set_hr = E_FAIL;
  HRESULT interface_get_hr = E_FAIL;
  HRESULT delete_hr = E_FAIL;
  HRESULT missing_hr = E_FAIL;
  HRESULT set_name_hr = E_FAIL;
  UINT queried_size = 0;
  bool payload_match = false;
  bool source_copy_isolated = false;
  bool short_size_correct = false;
  bool interface_match = false;
  bool pass = false;
};

static ObjectResult test_object(const char *name, HRESULT create_hr,
                                ID3D12Object *object,
                                IUnknown *interface_value) {
  ObjectResult result;
  result.name = name;
  result.create_hr = create_hr;
  if (FAILED(create_hr) || !object)
    return result;

  const uint32_t expected_payload[4] = {
      0x10203040u, 0x55667788u, 0xa5a55a5au, 0xdecafbad};
  uint32_t source_payload[4] = {};
  std::memcpy(source_payload, expected_payload, sizeof(source_payload));
  result.set_hr = object->SetPrivateData(kPayloadGuid, sizeof(source_payload),
                                         source_payload);
  source_payload[0] ^= 0xffffffffu;

  UINT size = 0;
  result.size_hr = object->GetPrivateData(kPayloadGuid, &size, nullptr);
  result.queried_size = size;

  uint32_t short_payload = 0;
  UINT short_size = sizeof(short_payload);
  result.short_hr =
      object->GetPrivateData(kPayloadGuid, &short_size, &short_payload);
  result.short_size_correct = short_size == sizeof(expected_payload);

  uint32_t readback[4] = {};
  UINT readback_size = sizeof(readback);
  result.get_hr =
      object->GetPrivateData(kPayloadGuid, &readback_size, readback);
  result.payload_match =
      readback_size == sizeof(expected_payload) &&
      std::memcmp(readback, expected_payload, sizeof(expected_payload)) == 0;
  result.source_copy_isolated = readback[0] != source_payload[0];

  result.interface_set_hr =
      object->SetPrivateDataInterface(kInterfaceGuid, interface_value);
  IUnknown *readback_interface = nullptr;
  UINT interface_size = sizeof(readback_interface);
  result.interface_get_hr = object->GetPrivateData(
      kInterfaceGuid, &interface_size, &readback_interface);
  result.interface_match =
      readback_interface == interface_value &&
      interface_size == sizeof(readback_interface);
  if (readback_interface)
    readback_interface->Release();

  result.set_name_hr = object->SetName(L"MetalSharp D3D12 object contract");
  result.delete_hr = object->SetPrivateData(kPayloadGuid, 0, nullptr);
  size = 0;
  result.missing_hr = object->GetPrivateData(kPayloadGuid, &size, nullptr);

  result.pass = SUCCEEDED(result.set_hr) && SUCCEEDED(result.size_hr) &&
                result.queried_size == sizeof(expected_payload) &&
                result.short_hr == DXGI_ERROR_MORE_DATA &&
                result.short_size_correct && SUCCEEDED(result.get_hr) &&
                result.payload_match && result.source_copy_isolated &&
                SUCCEEDED(result.interface_set_hr) &&
                SUCCEEDED(result.interface_get_hr) && result.interface_match &&
                SUCCEEDED(result.set_name_hr) && SUCCEEDED(result.delete_hr) &&
                result.missing_hr == DXGI_ERROR_NOT_FOUND;
  return result;
}

static void append_result(std::vector<ObjectResult> &results, const char *name,
                          HRESULT hr, ID3D12Object *object,
                          IUnknown *interface_value) {
  results.push_back(test_object(name, hr, object, interface_value));
  if (object)
    object->Release();
}

int main() {
  const std::string profile = getenv_string("D3D12_METAL_SDK_PROFILE");
  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  using CreateDeviceFn =
      HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
  using SerializeRootSignatureFn = HRESULT(WINAPI *)(
      const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
      ID3DBlob **, ID3DBlob **);
  auto create_device = reinterpret_cast<CreateDeviceFn>(
      reinterpret_cast<void *>(
          d3d12 ? GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr));
  auto serialize_root_signature = reinterpret_cast<SerializeRootSignatureFn>(
      reinterpret_cast<void *>(d3d12 ? GetProcAddress(
                                           d3d12,
                                           "D3D12SerializeRootSignature")
                                     : nullptr));

  ID3D12Device *device = nullptr;
  HRESULT device_hr =
      create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                    kDeviceIid,
                                    reinterpret_cast<void **>(&device))
                    : E_NOINTERFACE;
  std::vector<ObjectResult> results;
  if (device) {
    device->AddRef();
    append_result(results, "device", device_hr, device, device);

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    append_result(results, "command_queue", hr, queue, device);

    ID3D12CommandAllocator *allocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        IID_PPV_ARGS(&allocator));
    if (allocator)
      allocator->AddRef();
    append_result(results, "command_allocator", hr, allocator, device);

    ID3D12GraphicsCommandList *command_list = nullptr;
    HRESULT list_hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
        IID_PPV_ARGS(&command_list));
    append_result(results, "command_list", list_hr, command_list, device);
    if (allocator)
      allocator->Release();

    ID3D12Fence *fence = nullptr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    append_result(results, "fence", hr, fence, device);

    D3D12_DESCRIPTOR_HEAP_DESC descriptor_heap_desc = {};
    descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptor_heap_desc.NumDescriptors = 8;
    ID3D12DescriptorHeap *descriptor_heap = nullptr;
    hr = device->CreateDescriptorHeap(&descriptor_heap_desc,
                                      IID_PPV_ARGS(&descriptor_heap));
    append_result(results, "descriptor_heap", hr, descriptor_heap, device);

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = 64 * 1024;
    heap_desc.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_desc.Properties.CreationNodeMask = 1;
    heap_desc.Properties.VisibleNodeMask = 1;
    heap_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap *heap = nullptr;
    hr = device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap));
    append_result(results, "heap", hr, heap, device);

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload.CreationNodeMask = 1;
    upload.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 4096;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    hr = device->CreateCommittedResource(
        &upload, D3D12_HEAP_FLAG_NONE, &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
    append_result(results, "resource", hr, resource, device);

    D3D12_QUERY_HEAP_DESC query_desc = {};
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_desc.Count = 4;
    ID3D12QueryHeap *query_heap = nullptr;
    hr = device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&query_heap));
    append_result(results, "query_heap", hr, query_heap, device);

    D3D12_INDIRECT_ARGUMENT_DESC indirect_argument = {};
    indirect_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {};
    signature_desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature_desc.NumArgumentDescs = 1;
    signature_desc.pArgumentDescs = &indirect_argument;
    ID3D12CommandSignature *signature = nullptr;
    hr = device->CreateCommandSignature(&signature_desc, nullptr,
                                        IID_PPV_ARGS(&signature));
    append_result(results, "command_signature", hr, signature, device);

    ID3DBlob *root_blob = nullptr;
    ID3DBlob *root_error = nullptr;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    HRESULT serialize_hr = serialize_root_signature
                               ? serialize_root_signature(
                                     &root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &root_blob, &root_error)
                               : E_NOINTERFACE;
    ID3D12RootSignature *root_signature = nullptr;
    hr = SUCCEEDED(serialize_hr) && root_blob
             ? device->CreateRootSignature(
                   0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                   IID_PPV_ARGS(&root_signature))
             : serialize_hr;
    append_result(results, "root_signature", hr, root_signature, device);
    if (root_error)
      root_error->Release();
    if (root_blob)
      root_blob->Release();

    ID3D12Device1 *device1 = nullptr;
    HRESULT device1_hr = device->QueryInterface(IID_PPV_ARGS(&device1));
    ID3D12PipelineLibrary *pipeline_library = nullptr;
    hr = device1 ? device1->CreatePipelineLibrary(
                       nullptr, 0, IID_PPV_ARGS(&pipeline_library))
                 : device1_hr;
    append_result(results, "pipeline_library", hr, pipeline_library, device);
    if (device1)
      device1->Release();

    ID3D12Device9 *device9 = nullptr;
    HRESULT device9_hr = device->QueryInterface(IID_PPV_ARGS(&device9));
    ID3D12ShaderCacheSession *shader_cache = nullptr;
    D3D12_SHADER_CACHE_SESSION_DESC cache_desc = {};
    cache_desc.Mode = D3D12_SHADER_CACHE_MODE_MEMORY;
    cache_desc.Flags = D3D12_SHADER_CACHE_FLAG_NONE;
    hr = device9 ? device9->CreateShaderCacheSession(
                       &cache_desc, IID_PPV_ARGS(&shader_cache))
                 : device9_hr;
    append_result(results, "shader_cache_session", hr, shader_cache, device);
    if (device9)
      device9->Release();
  }

  bool pass = SUCCEEDED(device_hr) && !results.empty();
  for (const auto &result : results)
    pass = pass && result.pass;

  std::printf("{\n");
  std::printf("  \"schema\": \"metalsharp.d3d12-metal.probe-object-contracts.v1\",\n");
  std::printf("  \"profile\": \"%s\",\n", json_escape(profile).c_str());
  std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
  std::printf("  \"object_count\": %zu,\n", results.size());
  std::printf("  \"objects\": [\n");
  for (size_t i = 0; i < results.size(); ++i) {
    const auto &r = results[i];
    std::printf("    {\"name\": \"%s\", \"pass\": %s, \"create_hr\": \"0x%08lx\", \"set_hr\": \"0x%08lx\", \"size_hr\": \"0x%08lx\", \"short_hr\": \"0x%08lx\", \"get_hr\": \"0x%08lx\", \"queried_size\": %u, \"payload_match\": %s, \"source_copy_isolated\": %s, \"short_size_correct\": %s, \"interface_match\": %s, \"delete_hr\": \"0x%08lx\", \"missing_hr\": \"0x%08lx\", \"set_name_hr\": \"0x%08lx\"}%s\n",
                json_escape(r.name).c_str(), r.pass ? "true" : "false",
                static_cast<unsigned long>(static_cast<uint32_t>(r.create_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.set_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.size_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.short_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.get_hr)),
                r.queried_size, r.payload_match ? "true" : "false",
                r.source_copy_isolated ? "true" : "false",
                r.short_size_correct ? "true" : "false",
                r.interface_match ? "true" : "false",
                static_cast<unsigned long>(static_cast<uint32_t>(r.delete_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.missing_hr)),
                static_cast<unsigned long>(static_cast<uint32_t>(r.set_name_hr)),
                i + 1 == results.size() ? "" : ",");
  }
  std::printf("  ]\n");
  std::printf("}\n");
  std::fflush(stdout);

  if (device)
    device->Release();
  if (d3d12)
    FreeLibrary(d3d12);
  TerminateProcess(GetCurrentProcess(), pass ? 0 : 1);
}
