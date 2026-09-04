#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR,
                                    const D3D_SHADER_MACRO *, ID3DInclude *,
                                    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **,
                                    ID3DBlob **);

template <typename T> static T load_proc(HMODULE module, const char *name) {
  T function = nullptr;
  FARPROC proc = module ? GetProcAddress(module, name) : nullptr;
  static_assert(sizeof(function) == sizeof(proc));
  std::memcpy(&function, &proc, sizeof(function));
  return function;
}

static bool write_file(const char *name, ID3DBlob *blob) {
  if (!blob)
    return false;
  std::ofstream file(name, std::ios::binary);
  if (!file)
    return false;
  file.write(static_cast<const char *>(blob->GetBufferPointer()),
             static_cast<std::streamsize>(blob->GetBufferSize()));
  return file.good();
}

static bool compile_pair(CompileFn compile, const char *source,
                        const char *vs_name, const char *gs_name) {
  ID3DBlob *vs = nullptr;
  ID3DBlob *gs = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT hr = compile(source, std::strlen(source), "phase6_geometry.hlsl",
                       nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs,
                       &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
                   static_cast<const char *>(errors->GetBufferPointer()));
    if (errors)
      errors->Release();
    if (vs)
      vs->Release();
    return false;
  }
  if (errors)
    errors->Release();
  errors = nullptr;
  hr = compile(source, std::strlen(source), "phase6_geometry.hlsl", nullptr,
                nullptr, "gs_main", "gs_5_0", 0, 0, &gs, &errors);
  if (FAILED(hr)) {
    if (errors)
      std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
                   static_cast<const char *>(errors->GetBufferPointer()));
    if (errors)
      errors->Release();
    vs->Release();
    if (gs)
      gs->Release();
    return false;
  }
  if (errors)
    errors->Release();
  const bool ok = write_file(vs_name, vs) && write_file(gs_name, gs);
  vs->Release();
  gs->Release();
  return ok;
}

int main() {
  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  CompileFn compile = load_proc<CompileFn>(compiler, "D3DCompile");
  if (!compile)
    return 2;
  const char *attribute_source = R"HLSL(
struct VSIn { float3 position : ATTRIBUTE0; uint attribute13 : ATTRIBUTE13; };
struct VSOut { float4 position : SV_POSITION; };
VSOut vs_main(VSIn input) { VSOut output; output.position = float4(input.position, 1.0); return output; }
[maxvertexcount(3)]
void gs_main(triangle VSOut input[3], inout TriangleStream<VSOut> output) { output.Append(input[0]); output.Append(input[1]); output.Append(input[2]); output.RestartStrip(); }
)HLSL";
  const char *float2_source = R"HLSL(
struct VSIn { float2 position : ATTRIBUTE0; };
struct VSOut { float4 position : SV_POSITION; };
VSOut vs_main(VSIn input) { VSOut output; output.position = float4(input.position, 0.0, 1.0); return output; }
[maxvertexcount(3)]
void gs_main(triangle VSOut input[3], inout TriangleStream<VSOut> output) { output.Append(input[0]); output.Append(input[1]); output.Append(input[2]); output.RestartStrip(); }
)HLSL";
  const char *pixel_source = R"HLSL(
float4 ps_main() : SV_TARGET { return float4(1.0, 0.0, 0.0, 1.0); }
)HLSL";
  ID3DBlob *pixel = nullptr;
  ID3DBlob *errors = nullptr;
  HRESULT pixel_hr = compile(pixel_source, std::strlen(pixel_source),
                             "phase6_geometry_pixel.hlsl", nullptr, nullptr,
                             "ps_main", "ps_5_0", 0, 0, &pixel, &errors);
  if (errors)
    errors->Release();
  const bool pixel_ok = SUCCEEDED(pixel_hr) && write_file(
      "phase6_geometry_pixel.dxbc", pixel);
  if (pixel)
    pixel->Release();
  return compile_pair(compile, attribute_source,
                      "phase6_geometry_attribute_vs.dxbc",
                      "phase6_geometry_attribute_gs.dxbc") &&
                 compile_pair(compile, float2_source,
                              "phase6_geometry_float2_vs.dxbc",
                              "phase6_geometry_float2_gs.dxbc") &&
                 pixel_ok
             ? 0
             : 1;
}
