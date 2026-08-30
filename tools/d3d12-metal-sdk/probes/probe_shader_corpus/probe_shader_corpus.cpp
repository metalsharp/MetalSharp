#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>

static const GUID IID_D3D12DeviceProbe = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                                       ID3DBlob**, ID3DBlob**);
using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

template <typename T> static void safe_release(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

template <typename T> static T load_proc(HMODULE module, const char* name) {
    T fn = nullptr;
    FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
    static_assert(sizeof(fn) == sizeof(proc), "function pointer size mismatch");
    std::memcpy(&fn, &proc, sizeof(fn));
    return fn;
}

static std::string getenv_string(const char* key) {
    DWORD needed = GetEnvironmentVariableA(key, nullptr, 0);
    if (needed == 0)
        return "";
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(key, value.data(), needed);
    if (written == 0)
        return "";
    value.resize(written);
    return value;
}

static std::string json_escape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

static std::string hr_hex(HRESULT hr) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    return buffer;
}

static bool write_text_file(const char* path, const char* text) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    DWORD size = static_cast<DWORD>(std::strlen(text));
    bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

static std::string read_text_file(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return "";

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return "";
    }

    std::string out(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(read);
    CloseHandle(file);
    return out;
}

static std::vector<uint8_t> read_binary_file(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }

    std::vector<uint8_t> out(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    out.resize(read);
    CloseHandle(file);
    return out;
}

static DWORD run_process_wait(std::string command_line) {
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    BOOL created = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                  nullptr, &startup, &process);
    if (!created)
        return 0xffffffffu;

    WaitForSingleObject(process.hProcess, 30000);
    DWORD exit_code = 0xffffffffu;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code;
}

static D3D12_BLEND_DESC default_blend_desc() {
    D3D12_BLEND_DESC desc = {};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    for (auto& rt : desc.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return desc;
}

static D3D12_RASTERIZER_DESC default_rasterizer_desc() {
    D3D12_RASTERIZER_DESC desc = {};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

static D3D12_DEPTH_STENCIL_DESC default_depth_stencil_desc() {
    D3D12_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    return desc;
}

static HRESULT create_corpus_root_signature(ID3D12Device* device, D3D12SerializeRootSignatureFn serialize,
                                            ID3D12RootSignature** root, std::string& errors) {
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 5;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.Num32BitValues = 4;
    params[3].Constants.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error_blob);
    if (error_blob) {
        errors.assign(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
        error_blob->Release();
    }
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(root));
    safe_release(blob);
    return hr;
}

struct CorpusCase {
    const char* name;
    const char* category;
    const char* entry;
    const char* target;
    bool expected_rejection;
    bool waveops_case;
    bool expected_pso_rejection = false;
};

struct CaseResult {
    std::string name;
    std::string category;
    std::string target;
    bool expected_rejection = false;
    bool waveops_case = false;
    bool expected_pso_rejection = false;
    bool compile_ok = false;
    bool dxil_blob = false;
    bool pso_created = false;
    bool geometry_compile_ok = false;
    bool geometry_pso_created = false;
    HRESULT geometry_pso_hr = E_FAIL;
    bool case_pass = false;
    DWORD dxc_exit_code = 0xffffffffu;
    HRESULT pso_hr = E_FAIL;
    size_t dxil_size = 0;
    std::string detail;
};

static CaseResult run_dxc_case(ID3D12Device* device, ID3D12RootSignature* root, const CorpusCase& corpus_case) {
    CaseResult result;
    result.name = corpus_case.name;
    result.category = corpus_case.category;
    result.target = corpus_case.target;
    result.expected_rejection = corpus_case.expected_rejection;
    result.waveops_case = corpus_case.waveops_case;
    result.expected_pso_rejection = corpus_case.expected_pso_rejection;

    const std::string base = std::string("Z:\\tmp\\dxmt_shader_corpus_") + corpus_case.name;
    const std::string dxil_path = base + ".dxil";
    const std::string error_path = base + ".err";
    DeleteFileA(dxil_path.c_str());
    DeleteFileA(error_path.c_str());

    std::string command = "dxc.exe -nologo -T ";
    command += corpus_case.target;
    command += " -E ";
    command += corpus_case.entry;
    command += " -HV 2021 -Od -Fo ";
    command += dxil_path;
    command += " -Fe ";
    command += error_path;
    command += " Z:\\tmp\\dxmt_shader_corpus.hlsl";

    result.dxc_exit_code = run_process_wait(command);
    std::vector<uint8_t> dxil = read_binary_file(dxil_path.c_str());
    result.dxil_blob = !dxil.empty();
    result.dxil_size = dxil.size();
    result.compile_ok = result.dxc_exit_code == 0 && result.dxil_blob;

    if (result.compile_ok && device && root) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.CS.pShaderBytecode = dxil.data();
        desc.CS.BytecodeLength = dxil.size();
        ID3D12PipelineState* pso = nullptr;
        result.pso_hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        result.pso_created = SUCCEEDED(result.pso_hr) && pso;
        safe_release(pso);
    }

    std::string dxc_errors = read_text_file(error_path.c_str());
    if (result.expected_rejection) {
        result.case_pass = !result.compile_ok;
        result.detail = result.case_pass ? "unsupported shader-model target was deterministically rejected"
                                         : "unsupported shader-model target unexpectedly compiled";
    } else if (result.expected_pso_rejection) {
        result.case_pass = result.compile_ok && !result.pso_created && FAILED(result.pso_hr);
        result.detail = result.case_pass
                            ? "DXIL compiled, but the unsupported counter configuration was rejected at PSO creation"
                            : "unsupported counter configuration unexpectedly produced a linked PSO";
    } else {
        result.case_pass = result.compile_ok && result.pso_created;
        if (!result.compile_ok)
            result.detail = dxc_errors.empty() ? "DXC did not produce a DXIL blob" : dxc_errors;
        else if (!result.pso_created)
            result.detail = "compiled to DXIL, but no linked compute PSO was created";
        else if (result.waveops_case)
            result.detail = "compiled and linked; WaveOps runtime correctness remains gated by probe_wave_ops";
        else
            result.detail = "compiled and linked through the primary DXIL compute PSO path";
    }

    return result;
}

static CaseResult run_sm5_graphics_case(ID3D12Device* device, ID3D12RootSignature* root, D3DCompileFn compile) {
    CaseResult result;
    result.name = "sm50_graphics_baseline";
    result.category = "sm50_baseline";
    result.target = "vs_5_0+ps_5_0";

    const char* hlsl = R"(
struct VSOut {
  float4 pos : SV_POSITION;
  float4 color : COLOR0;
};

[maxvertexcount(3)]
void gs_main(triangle VSOut input[3], inout TriangleStream<VSOut> output) {
  for (uint i = 0; i < 3; ++i)
    output.Append(input[i]);
}

VSOut vs_main(uint vertex_id : SV_VertexID) {
  float2 positions[3] = {
    float2(0.0, 0.5),
    float2(0.5, -0.5),
    float2(-0.5, -0.5)
  };
  VSOut output;
  output.pos = float4(positions[vertex_id], 0.0, 1.0);
  output.color = float4(vertex_id == 0, vertex_id == 1, vertex_id == 2, 1.0);
  return output;
}

float4 ps_main(VSOut input) : SV_Target0 {
  return input.color;
}
)";

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* gs = nullptr;
    ID3DBlob* vs_errors = nullptr;
    ID3DBlob* ps_errors = nullptr;
    ID3DBlob* gs_errors = nullptr;
    HRESULT vs_hr = compile ? compile(hlsl, std::strlen(hlsl), "probe_shader_corpus_sm50.hlsl", nullptr, nullptr,
                                      "vs_main", "vs_5_0", 0, 0, &vs, &vs_errors)
                            : E_FAIL;
    HRESULT ps_hr = compile ? compile(hlsl, std::strlen(hlsl), "probe_shader_corpus_sm50.hlsl", nullptr, nullptr,
                                      "ps_main", "ps_5_0", 0, 0, &ps, &ps_errors)
                            : E_FAIL;
    HRESULT gs_hr = compile ? compile(hlsl, std::strlen(hlsl), "probe_shader_corpus_sm50.hlsl", nullptr, nullptr,
                                      "gs_main", "gs_5_0", 0, 0, &gs, &gs_errors)
                            : E_FAIL;
    result.compile_ok = SUCCEEDED(vs_hr) && vs && SUCCEEDED(ps_hr) && ps;
    result.geometry_compile_ok = SUCCEEDED(gs_hr) && gs;
    result.dxil_blob = false;
    result.dxc_exit_code = 0;

    if (result.compile_ok && device && root) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.BlendState = default_blend_desc();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = default_rasterizer_desc();
        desc.DepthStencilState = default_depth_stencil_desc();
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ID3D12PipelineState* pso = nullptr;
        result.pso_hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
        result.pso_created = SUCCEEDED(result.pso_hr) && pso;
        safe_release(pso);
    }

    if (result.geometry_compile_ok && result.compile_ok && device && root) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root;
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        desc.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
        desc.BlendState = default_blend_desc();
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState = default_rasterizer_desc();
        desc.DepthStencilState = default_depth_stencil_desc();
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        ID3D12PipelineState* geometry_pso = nullptr;
        result.geometry_pso_hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&geometry_pso));
        result.geometry_pso_created = SUCCEEDED(result.geometry_pso_hr) && geometry_pso;
        safe_release(geometry_pso);
    }

    result.case_pass = result.compile_ok && result.pso_created && result.geometry_compile_ok &&
                       result.geometry_pso_created;
    if (!result.compile_ok || !result.geometry_compile_ok) {
        std::string errors;
        if (vs_errors)
            errors.append(static_cast<const char*>(vs_errors->GetBufferPointer()), vs_errors->GetBufferSize());
        if (ps_errors)
            errors.append(static_cast<const char*>(ps_errors->GetBufferPointer()), ps_errors->GetBufferSize());
        if (gs_errors)
            errors.append(static_cast<const char*>(gs_errors->GetBufferPointer()), gs_errors->GetBufferSize());
        result.detail = errors.empty() ? "d3dcompiler_47 did not produce SM 5.0 graphics-stage blobs" : errors;
    } else if (!result.pso_created || !result.geometry_pso_created) {
        result.detail = "SM 5.0 graphics stages compiled, but graphics PSO linking failed";
    } else {
        result.detail = "SM 5.0 vertex/pixel/geometry stages compiled and linked";
    }

    safe_release(vs);
    safe_release(ps);
    safe_release(gs);
    safe_release(vs_errors);
    safe_release(ps_errors);
    safe_release(gs_errors);
    return result;
}

int main() {
    const char* hlsl_path = "Z:\\tmp\\dxmt_shader_corpus.hlsl";
    const char* hlsl = R"(
RWByteAddressBuffer out_uav : register(u0);
AppendStructuredBuffer<uint> append_output : register(u1);
RWStructuredBuffer<uint4> out_structured : register(u2);
AppendStructuredBuffer<uint> append_output2 : register(u3);
ByteAddressBuffer raw_inputs[2] : register(t0);
StructuredBuffer<uint4> structured_inputs : register(t2);
Buffer<uint4> typed_inputs : register(t3);
Texture2D<float4> tex : register(t4);
SamplerState smp : register(s0);

cbuffer RootConstants : register(b0) {
  uint selector;
  uint addend;
  uint multiplier;
  uint pad0;
};

[numthreads(4, 1, 1)]
void cs_sm60(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, id.x + 60u);
}

[numthreads(4, 1, 1)]
void cs_sm61(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, firstbithigh((id.x + 1u) << 4));
}

[numthreads(4, 1, 1)]
void cs_sm62(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, (id.x + 62u) ^ addend);
}

[numthreads(4, 1, 1)]
void cs_sm63(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, countbits((id.x + 1u) * 0x1111u));
}

[numthreads(4, 1, 1)]
void cs_sm64(uint3 id : SV_DispatchThreadID) {
  uint64_t wide = ((uint64_t)(id.x + 1u) << 32) | (uint64_t)addend;
  out_uav.Store(id.x * 8, (uint)(wide & 0xffffffffull));
  out_uav.Store(id.x * 8 + 4, (uint)(wide >> 32));
}

[numthreads(4, 1, 1)]
void cs_sm65(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, reversebits(id.x + 1u));
}

[numthreads(4, 1, 1)]
void cs_sm66(uint3 id : SV_DispatchThreadID) {
  uint value = WaveGetLaneIndex() + WaveGetLaneCount();
  out_uav.Store(id.x * 4, value);
}

[numthreads(4, 1, 1)]
void cs_sm67(uint3 id : SV_DispatchThreadID) {
  uint4 vector = uint4(id.x + 1u, id.x + 2u, id.x + 3u, id.x + 4u);
  uint64_t wide = (uint64_t(vector.x) << 32) | uint64_t(vector.x + 67u);
  out_uav.Store(id.x * 4, uint(wide));
}

[numthreads(4, 1, 1)]
void cs_sm68(uint3 id : SV_DispatchThreadID) {
  uint64_t wide = uint64_t(id.x + 1u) * uint64_t(68u);
  out_uav.Store(id.x * 4, uint(wide));
}

[numthreads(4, 1, 1)]
void cs_sm69(uint3 id : SV_DispatchThreadID) {
  float value = 69.0f + float(id.x) * 1.5f;
  out_uav.Store(id.x * 4, uint(value));
}

[numthreads(4, 1, 1)]
void cs_resource_indexing(uint3 id : SV_DispatchThreadID) {
  uint index = selector & 1u;
  out_uav.Store(id.x * 4, raw_inputs[index].Load(id.x * 4) + addend);
}

[numthreads(4, 1, 1)]
void cs_append_counter(uint3 id : SV_DispatchThreadID) {
  append_output.Append(id.x + 1u);
}

[numthreads(4, 1, 1)]
void cs_two_append_counters(uint3 id : SV_DispatchThreadID) {
  append_output.Append(id.x + 1u);
  append_output2.Append(id.x + 5u);
}

[numthreads(4, 1, 1)]
void cs_uav_writes(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, id.x * multiplier + addend);
}

[numthreads(4, 1, 1)]
void cs_typed_structured_buffers(uint3 id : SV_DispatchThreadID) {
  uint4 v = structured_inputs[id.x] + typed_inputs[id.x] + uint4(addend, 1u, 2u, 3u);
  out_structured[id.x] = v;
}

[numthreads(4, 1, 1)]
void cs_texture_sampling(uint3 id : SV_DispatchThreadID) {
  float4 sample = tex.SampleLevel(smp, float2(0.5, 0.5), 0.0);
  out_uav.Store(id.x * 4, (uint)(sample.x * 255.0 + 0.5) + id.x);
}

[numthreads(4, 1, 1)]
void cs_root_constants(uint3 id : SV_DispatchThreadID) {
  out_uav.Store(id.x * 4, (id.x + addend) * multiplier + selector);
}
)";

    bool hlsl_written = write_text_file(hlsl_path, hlsl);

    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    HMODULE d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
    HMODULE dxcompiler = LoadLibraryA("dxcompiler.dll");
    HMODULE dxil = LoadLibraryA("dxil.dll");
    auto create_device = load_proc<D3D12CreateDeviceFn>(d3d12, "D3D12CreateDevice");
    auto serialize = load_proc<D3D12SerializeRootSignatureFn>(d3d12, "D3D12SerializeRootSignature");
    auto compile = load_proc<D3DCompileFn>(d3dcompiler, "D3DCompile");

    ID3D12Device* device = nullptr;
    HRESULT create_hr = create_device ? create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_D3D12DeviceProbe,
                                                      reinterpret_cast<void**>(&device))
                                      : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ID3D12RootSignature* root = nullptr;
    std::string root_errors;
    HRESULT root_hr = (device && serialize) ? create_corpus_root_signature(device, serialize, &root, root_errors)
                                            : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const CorpusCase corpus_cases[] = {
        {"sm60_compute_baseline", "sm60_through_sm66_progression", "cs_sm60", "cs_6_0", false, false},
        {"sm61_compute_baseline", "sm60_through_sm66_progression", "cs_sm61", "cs_6_1", false, false},
        {"sm62_native16_probe", "sm60_through_sm66_progression", "cs_sm62", "cs_6_2", false, false},
        {"sm63_bit_ops_probe", "sm60_through_sm66_progression", "cs_sm63", "cs_6_3", false, false},
        {"sm64_int64_probe", "sm60_through_sm66_progression", "cs_sm64", "cs_6_4", false, false},
        {"sm65_bit_ops_probe", "sm60_through_sm66_progression", "cs_sm65", "cs_6_5", false, false},
        {"sm66_waveops_compile_link", "waveops", "cs_sm66", "cs_6_6", false, true},
        {"sm67_vector_int64", "sm67_through_sm69_progression", "cs_sm67", "cs_6_7", false, false},
        {"sm68_vector_arithmetic", "sm67_through_sm69_progression", "cs_sm68", "cs_6_8", false, false},
        {"sm69_integer_float_mix", "sm67_through_sm69_progression", "cs_sm69", "cs_6_9", false, false},
        {"resource_indexing", "resource_indexing", "cs_resource_indexing", "cs_6_0", false, false},
        {"append_counter_link", "append_counter_link", "cs_append_counter", "cs_6_0", false, false},
        {"two_counter_fail_closed", "counter_fail_closed", "cs_two_append_counters", "cs_6_0", false, false, true},
        {"uav_writes", "uav_writes", "cs_uav_writes", "cs_6_0", false, false},
        {"typed_structured_buffers", "typed_and_structured_buffers", "cs_typed_structured_buffers", "cs_6_0", false,
         false},
        {"texture_sampling", "texture_sampling", "cs_texture_sampling", "cs_6_0", false, false},
        {"root_constants", "root_constants", "cs_root_constants", "cs_6_0", false, false},
        {"unsupported_shader_model", "unsupported_feature_rejection", "cs_sm60", "cs_9_9", true, false},
    };

    std::vector<CaseResult> results;
    results.push_back(run_sm5_graphics_case(device, root, compile));
    if (hlsl_written) {
        for (const auto& corpus_case : corpus_cases)
            results.push_back(run_dxc_case(device, root, corpus_case));
    }

    bool entrypoints_ok = d3d12 && d3dcompiler && dxcompiler && dxil && create_device && serialize && compile &&
                          SUCCEEDED(create_hr) && SUCCEEDED(root_hr);
    bool corpus_complete = hlsl_written && !results.empty();
    bool required_cases_pass = corpus_complete;
    bool sm50_baseline = false;
    bool sm60_to_sm66 = true;
    bool sm67_to_sm69 = true;
    bool resource_indexing = false;
    bool uav_writes = false;
    bool typed_structured_buffers = false;
    bool texture_sampling = false;
    bool root_constants = false;
    bool waveops_compile_link = false;
    bool unsupported_rejection = false;
    bool append_counter_link = false;
    bool counter_fail_closed = false;

    for (const auto& result : results) {
        required_cases_pass = required_cases_pass && result.case_pass;
        if (result.category == "sm50_baseline")
            sm50_baseline = result.case_pass;
        if (result.category == "sm60_through_sm66_progression")
            sm60_to_sm66 = sm60_to_sm66 && result.case_pass;
        if (result.category == "sm67_through_sm69_progression")
            sm67_to_sm69 = sm67_to_sm69 && result.case_pass;
        if (result.category == "resource_indexing")
            resource_indexing = result.case_pass;
        if (result.category == "uav_writes")
            uav_writes = result.case_pass;
        if (result.category == "typed_and_structured_buffers")
            typed_structured_buffers = result.case_pass;
        if (result.category == "texture_sampling")
            texture_sampling = result.case_pass;
        if (result.category == "root_constants")
            root_constants = result.case_pass;
        if (result.category == "waveops")
            waveops_compile_link = result.case_pass;
        if (result.category == "unsupported_feature_rejection")
            unsupported_rejection = result.case_pass;
        if (result.category == "append_counter_link")
            append_counter_link = result.case_pass;
        if (result.category == "counter_fail_closed")
            counter_fail_closed = result.case_pass;
    }

    bool synthetic_shader_corpus_proven =
        entrypoints_ok && required_cases_pass && sm50_baseline && sm60_to_sm66 && sm67_to_sm69 && resource_indexing && uav_writes &&
        typed_structured_buffers && texture_sampling && root_constants && waveops_compile_link && unsupported_rejection &&
        append_counter_link && counter_fail_closed;
    bool pass = synthetic_shader_corpus_proven;

    std::printf("{\n");
    std::printf("  \"schema\": \"metalsharp.d3d12-metal.shader-corpus.v1\",\n");
    std::printf("  \"profile\": \"%s\",\n", json_escape(getenv_string("D3D12_METAL_SDK_PROFILE")).c_str());
    std::printf("  \"pass\": %s,\n", pass ? "true" : "false");
    std::printf("  \"entrypoints\": {\n");
    std::printf("    \"d3d12_loaded\": %s,\n", d3d12 ? "true" : "false");
    std::printf("    \"d3dcompiler_47_loaded\": %s,\n", d3dcompiler ? "true" : "false");
    std::printf("    \"dxcompiler_loaded\": %s,\n", dxcompiler ? "true" : "false");
    std::printf("    \"dxil_loaded\": %s,\n", dxil ? "true" : "false");
    std::printf("    \"D3D12CreateDevice\": %s,\n", create_device ? "true" : "false");
    std::printf("    \"D3D12SerializeRootSignature\": %s,\n", serialize ? "true" : "false");
    std::printf("    \"D3DCompile\": %s,\n", compile ? "true" : "false");
    std::printf("    \"complete\": %s\n", entrypoints_ok ? "true" : "false");
    std::printf("  },\n");
    std::printf("  \"device\": {\n");
    std::printf("    \"create_hr\": \"%s\",\n", hr_hex(create_hr).c_str());
    std::printf("    \"root_signature_hr\": \"%s\",\n", hr_hex(root_hr).c_str());
    std::printf("    \"root_signature_errors\": \"%s\"\n", json_escape(root_errors).c_str());
    std::printf("  },\n");
    std::printf("  \"summary\": {\n");
    std::printf("    \"synthetic_shader_corpus_proven\": %s,\n", synthetic_shader_corpus_proven ? "true" : "false");
    std::printf("    \"sm50_baseline\": %s,\n", sm50_baseline ? "true" : "false");
    std::printf("    \"sm60_to_sm66_progression\": %s,\n", sm60_to_sm66 ? "true" : "false");
    std::printf("    \"sm67_to_sm69_progression\": %s,\n", sm67_to_sm69 ? "true" : "false");
    std::printf("    \"resource_indexing\": %s,\n", resource_indexing ? "true" : "false");
    std::printf("    \"uav_writes\": %s,\n", uav_writes ? "true" : "false");
    std::printf("    \"typed_and_structured_buffers\": %s,\n", typed_structured_buffers ? "true" : "false");
    std::printf("    \"texture_sampling\": %s,\n", texture_sampling ? "true" : "false");
    std::printf("    \"root_constants\": %s,\n", root_constants ? "true" : "false");
    std::printf("    \"waveops_compile_link\": %s,\n", waveops_compile_link ? "true" : "false");
    std::printf("    \"waveops_runtime_gated_by_probe_wave_ops\": true,\n");
    std::printf("    \"unsupported_feature_rejection\": %s,\n", unsupported_rejection ? "true" : "false");
    std::printf("    \"append_counter_link\": %s,\n", append_counter_link ? "true" : "false");
    std::printf("    \"counter_fail_closed\": %s,\n", counter_fail_closed ? "true" : "false");
    std::printf("    \"unsupported_counter_rejection\": false,\n");
    std::printf("    \"title_captures_gating\": false\n");
    std::printf("  },\n");
    std::printf("  \"cases\": [\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& result = results[i];
        std::printf("    {\n");
        std::printf("      \"name\": \"%s\",\n", json_escape(result.name).c_str());
        std::printf("      \"category\": \"%s\",\n", json_escape(result.category).c_str());
        std::printf("      \"target\": \"%s\",\n", json_escape(result.target).c_str());
        std::printf("      \"expected_rejection\": %s,\n", result.expected_rejection ? "true" : "false");
        std::printf("      \"waveops_case\": %s,\n", result.waveops_case ? "true" : "false");
        std::printf("      \"expected_pso_rejection\": %s,\n", result.expected_pso_rejection ? "true" : "false");
        std::printf("      \"compile_ok\": %s,\n", result.compile_ok ? "true" : "false");
        std::printf("      \"dxil_blob\": %s,\n", result.dxil_blob ? "true" : "false");
        std::printf("      \"dxil_size\": %zu,\n", result.dxil_size);
        std::printf("      \"dxc_exit_code\": %lu,\n", static_cast<unsigned long>(result.dxc_exit_code));
        std::printf("      \"pso_created\": %s,\n", result.pso_created ? "true" : "false");
        std::printf("      \"pso_hr\": \"%s\",\n", hr_hex(result.pso_hr).c_str());
        std::printf("      \"geometry_compile_ok\": %s,\n", result.geometry_compile_ok ? "true" : "false");
        std::printf("      \"geometry_pso_created\": %s,\n", result.geometry_pso_created ? "true" : "false");
        std::printf("      \"geometry_pso_hr\": \"%s\",\n", hr_hex(result.geometry_pso_hr).c_str());
        std::printf("      \"case_pass\": %s,\n", result.case_pass ? "true" : "false");
        std::printf("      \"detail\": \"%s\"\n", json_escape(result.detail).c_str());
        std::printf("    }%s\n", i + 1 == results.size() ? "" : ",");
    }
    std::printf("  ]\n");
    std::printf("}\n");

    std::fflush(stdout);
    // Wine/MinGW can assert during late CRT condition-variable teardown after
    // the DXMT worker stack has already produced the contract JSON.
    TerminateProcess(GetCurrentProcess(), pass ? 0u : 1u);
    safe_release(root);
    safe_release(device);
    return 0;
}
