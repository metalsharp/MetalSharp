#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const GUID kDeviceIID = {
    0x189819f1, 0x1db6, 0x4b57,
    {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
static const GUID kDevice2IID = {
    0x30baa41e, 0xb15b, 0x475c,
    {0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28}};

template <typename T> static void safe_release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char *name) {
    T function = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(function) == sizeof(proc),
                  "function pointer size mismatch");
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

static std::string hr_hex(HRESULT hr) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "0x%08lx",
                  static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return text;
}

struct RasterizerDesc2Probe {
    D3D12_FILL_MODE FillMode;
    D3D12_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    FLOAT DepthBias;
    FLOAT DepthBiasClamp;
    FLOAT SlopeScaledDepthBias;
    BOOL DepthClipEnable;
    UINT LineRasterizationMode;
    UINT ForcedSampleCount;
    D3D12_CONSERVATIVE_RASTERIZATION_MODE ConservativeRaster;
};

struct ViewInstanceLocationProbe {
    UINT ViewportArrayIndex;
    UINT RenderTargetArrayIndex;
};

struct ViewInstancingDescProbe {
    UINT ViewInstanceCount;
    const ViewInstanceLocationProbe *pViewInstanceLocations;
    UINT Flags;
};

struct RTFormatArrayProbe {
    DXGI_FORMAT RTFormats[8];
    UINT NumRenderTargets;
};

struct StreamBuilder {
    std::vector<uint8_t> bytes;

    template <typename T> void append(UINT type, const T &value) {
        const size_t aligned = (bytes.size() + 7u) & ~size_t(7u);
        const size_t payload_offset =
            (sizeof(UINT) + alignof(T) - 1u) & ~(alignof(T) - 1u);
        const size_t end = aligned + payload_offset + sizeof(T);
        bytes.resize((end + 7u) & ~size_t(7u));
        std::memcpy(bytes.data() + aligned, &type, sizeof(type));
        std::memcpy(bytes.data() + aligned + payload_offset, &value,
                    sizeof(value));
    }
};

struct InvalidResult {
    const char *name = "";
    HRESULT hr = E_FAIL;
    bool object_null = true;
    bool exact = false;
};

static HRESULT compile_shader(const char *source, const char *entry,
                              const char *target, ID3DBlob **blob) {
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR,
                                        const D3D_SHADER_MACRO *, ID3DInclude *,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                        ID3DBlob **);
    CompileFn compile = load_proc<CompileFn>(compiler, "D3DCompile");
    if (!compile)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3DBlob *errors = nullptr;
    HRESULT hr = compile(source, std::strlen(source), "phase6_invalid.hlsl",
                         nullptr, nullptr, entry, target, 0, 0, blob, &errors);
    safe_release(errors);
    return hr;
}

static HRESULT create_root_signature(ID3D12Device *device,
                                     ID3D12RootSignature **root) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    using SerializeFn = HRESULT(WINAPI *)(
        const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION,
        ID3DBlob **, ID3DBlob **);
    SerializeFn serialize =
        load_proc<SerializeFn>(d3d12, "D3D12SerializeRootSignature");
    if (!serialize)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *blob = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob,
                           &errors);
    safe_release(errors);
    if (SUCCEEDED(hr) && blob)
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                         blob->GetBufferSize(),
                                         IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

static StreamBuilder make_stream(ID3D12RootSignature *root, ID3DBlob *vs,
                                 ID3DBlob *ps,
                                 const RasterizerDesc2Probe &rasterizer,
                                 const ViewInstancingDescProbe *view,
                                 UINT sample_count, bool add_unknown_type) {
    StreamBuilder stream;
    D3D12_BLEND_DESC blend = {};
    for (auto &target : blend.RenderTarget) {
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    UINT sample_mask = UINT_MAX;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_INPUT_LAYOUT_DESC input = {};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    RTFormatArrayProbe formats = {};
    formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    formats.NumRenderTargets = 1;
    DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sample = {sample_count, 0};
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    D3D12_SHADER_BYTECODE vs_bytecode = {vs->GetBufferPointer(),
                                         vs->GetBufferSize()};
    D3D12_SHADER_BYTECODE ps_bytecode = {ps->GetBufferPointer(),
                                         ps->GetBufferSize()};

    stream.append(0, root);
    stream.append(1, vs_bytecode);
    stream.append(2, ps_bytecode);
    stream.append(8, blend);
    stream.append(9, sample_mask);
    stream.append(28, rasterizer);
    stream.append(11, depth);
    stream.append(12, input);
    stream.append(14, topology);
    stream.append(15, formats);
    stream.append(16, dsv_format);
    stream.append(17, sample);
    if (view)
        stream.append(22, *view);
    stream.append(20, flags);
    if (add_unknown_type) {
        UINT payload = 0;
        stream.append(0xffffffffu, payload);
    }
    return stream;
}

static InvalidResult run_case(ID3D12Device2 *device, ID3D12RootSignature *root,
                              ID3DBlob *vs, ID3DBlob *ps, const char *name,
                              const RasterizerDesc2Probe &rasterizer,
                              const ViewInstancingDescProbe *view,
                              UINT sample_count, bool unknown_type,
                              bool require_invalid_arg) {
    InvalidResult result;
    result.name = name;
    if (!device || !root || !vs || !ps)
        return result;
    StreamBuilder stream =
        make_stream(root, vs, ps, rasterizer, view, sample_count, unknown_type);
    D3D12_PIPELINE_STATE_STREAM_DESC desc = {stream.bytes.size(),
                                             stream.bytes.data()};
    ID3D12PipelineState *pso = nullptr;
    result.hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso));
    result.object_null = pso == nullptr;
    result.exact = result.object_null &&
                   (require_invalid_arg ? result.hr == E_INVALIDARG
                                         : FAILED(result.hr));
    safe_release(pso);
    return result;
}

int main() {
    const char *hlsl = R"HLSL(
struct VSOut { float4 position : SV_Position; };
VSOut vs(uint id : SV_VertexID) {
  VSOut o;
  o.position = id == 0 ? float4(-1.0, -1.0, 0.0, 1.0) :
               (id == 1 ? float4(3.0, -1.0, 0.0, 1.0) :
                          float4(-1.0, 3.0, 0.0, 1.0));
  return o;
}
float4 ps(VSOut input) : SV_Target0 { return float4(1.0, 0.0, 0.0, 1.0); }
)HLSL";
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    auto create_device = load_proc<HRESULT(WINAPI *)(IUnknown *,
                                                       D3D_FEATURE_LEVEL,
                                                       REFIID, void **)>(
        d3d12, "D3D12CreateDevice");
    ID3D12Device *device = nullptr;
    HRESULT create_hr = create_device
                            ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0,
                                            kDeviceIID,
                                            reinterpret_cast<void **>(&device))
                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ID3D12Device2 *device2 = nullptr;
    HRESULT device2_hr = SUCCEEDED(create_hr)
                             ? device->QueryInterface(
                                   kDevice2IID,
                                   reinterpret_cast<void **>(&device2))
                             : E_FAIL;
    ID3D12RootSignature *root = nullptr;
    HRESULT root_hr = SUCCEEDED(create_hr)
                          ? create_root_signature(device, &root)
                          : E_FAIL;
    ID3DBlob *vs = nullptr;
    ID3DBlob *ps = nullptr;
    HRESULT vs_hr = compile_shader(hlsl, "vs", "vs_5_0", &vs);
    HRESULT ps_hr = compile_shader(hlsl, "ps", "ps_5_0", &ps);

    RasterizerDesc2Probe valid_rasterizer = {};
    valid_rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    valid_rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    valid_rasterizer.DepthClipEnable = TRUE;
    valid_rasterizer.LineRasterizationMode = 0;
    ViewInstanceLocationProbe one_location = {0, 0};
    ViewInstancingDescProbe valid_view = {1, &one_location, 0};
    StreamBuilder valid_stream =
        make_stream(root, vs, ps, valid_rasterizer, &valid_view, 1, false);
    D3D12_PIPELINE_STATE_STREAM_DESC valid_desc = {valid_stream.bytes.size(),
                                                   valid_stream.bytes.data()};
    ID3D12PipelineState *valid_pso = nullptr;
    HRESULT valid_hr = (device2 && root && vs && ps)
                           ? device2->CreatePipelineState(
                                 &valid_desc, IID_PPV_ARGS(&valid_pso))
                           : E_FAIL;
    const bool valid_object = valid_pso != nullptr;
    safe_release(valid_pso);

    RasterizerDesc2Probe invalid_line = valid_rasterizer;
    invalid_line.LineRasterizationMode = 4;
    ViewInstanceLocationProbe five_locations[5] = {};
    ViewInstancingDescProbe too_many_views = {5, five_locations, 0};
    ViewInstancingDescProbe invalid_flags = {1, &one_location, 2};
    ViewInstancingDescProbe missing_locations = {1, nullptr, 0};

    std::vector<InvalidResult> cases;
    cases.push_back(run_case(device2, root, vs, ps, "rasterizer2_line_mode_4",
                             invalid_line, &valid_view, 1, false, true));
    cases.push_back(run_case(device2, root, vs, ps, "view_instance_count_5",
                             valid_rasterizer, &too_many_views, 1, false, true));
    cases.push_back(run_case(device2, root, vs, ps, "view_instance_flags_2",
                             valid_rasterizer, &invalid_flags, 1, false, true));
    cases.push_back(run_case(device2, root, vs, ps,
                             "view_instance_locations_missing", valid_rasterizer,
                             &missing_locations, 1, false, true));
    cases.push_back(run_case(device2, root, vs, ps, "sample_count_3",
                             valid_rasterizer, &valid_view, 3, false, false));
    cases.push_back(run_case(device2, root, vs, ps, "unknown_subobject_type",
                             valid_rasterizer, &valid_view, 1, true, true));

    bool pass = SUCCEEDED(create_hr) && SUCCEEDED(device2_hr) &&
                SUCCEEDED(root_hr) && SUCCEEDED(vs_hr) && SUCCEEDED(ps_hr) &&
                SUCCEEDED(valid_hr) && valid_object;
    for (const auto &result : cases)
        pass = pass && result.exact;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12.phase6-invalid-descriptors.v1\",\n");
    std::printf("  \"create_hr\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("  \"device2_hr\": \"%s\",\n", hr_hex(device2_hr).c_str());
    std::printf("  \"root_hr\": \"%s\",\n", hr_hex(root_hr).c_str());
    std::printf("  \"shader_compile_hr\": [\"%s\", \"%s\"],\n",
                hr_hex(vs_hr).c_str(), hr_hex(ps_hr).c_str());
    std::printf("  \"valid_pipeline\": {\"hr\": \"%s\", \"object_null\": %s, \"exact\": %s},\n",
                hr_hex(valid_hr).c_str(), valid_object ? "false" : "true",
                (SUCCEEDED(valid_hr) && valid_object) ? "true" : "false");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto &result = cases[i];
        std::printf("    {\"name\": \"%s\", \"hr\": \"%s\", \"object_null\": %s, \"exact\": %s}%s\n",
                    result.name, hr_hex(result.hr).c_str(),
                    result.object_null ? "true" : "false",
                    result.exact ? "true" : "false",
                    i + 1 == cases.size() ? "" : ",");
    }
    std::printf("  ],\n  \"pass\": %s,\n  \"bounded_wait_ms\": 0,\n"
                "  \"provider\": \"d3d12_pipeline_stream_validation\"\n}\n",
                pass ? "true" : "false");
    std::fflush(stdout);

    safe_release(ps);
    safe_release(vs);
    safe_release(root);
    safe_release(device2);
    safe_release(device);
    return pass ? 0 : 1;
}
