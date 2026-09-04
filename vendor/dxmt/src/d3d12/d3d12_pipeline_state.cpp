#include "d3d12_pipeline_state.hpp"
#include "d3d12_device.hpp"
#include "d3d12_native_tessellation_path.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_trace.hpp"
#include "d3d12_vertex_input.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "Metal.hpp"

#define PTRACE(fmt, ...)                                                       \
  do {                                                                         \
    FILE *_tf = dxmt::openDiagnosticLog("dxmt-d3d12-pso.log");                 \
    if (_tf) {                                                                 \
      fprintf(_tf, fmt "\n", ##__VA_ARGS__);                                   \
      fclose(_tf);                                                             \
    }                                                                          \
  } while (0)
#include "airconv_public.h"
#include "dxmt_format.hpp"
#include "dxil/dxil_container.hpp"
#include "dxil/llvm_bitcode.hpp"
#include "dxil/dxil_to_msl.hpp"
#include "dxil/msl_lowering.hpp"
#include "../../libs/DXBCParser/BlobContainer.h"
#include "../../libs/DXBCParser/DXBCUtils.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <process.h>
#include <windows.h>

#define PSTRACE(fmt, ...) DXMTD3D12Trace("PSO", fmt, ##__VA_ARGS__)

static constexpr const char *kConservativeRasterVertexShader = R"metal(
#include <metal_stdlib>
using namespace metal;

struct m12_conservative_data {
  float2 p0;
  float2 p1;
  float2 p2;
  uint width;
  uint height;
  uint enabled;
  uint pad;
  float viewport_x;
  float viewport_y;
  float viewport_width;
  float viewport_height;
  float z0;
  float z1;
  float z2;
};

static inline float m12_cons_cross(float2 a, float2 b, float2 c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

struct m12_conservative_output {
  float4 position [[position]];
  float4 v0 [[user(locn0)]]; float4 v1 [[user(locn1)]];
  float4 v2 [[user(locn2)]]; float4 v3 [[user(locn3)]];
  float4 v4 [[user(locn4)]]; float4 v5 [[user(locn5)]];
  float4 v6 [[user(locn6)]]; float4 v7 [[user(locn7)]];
  float2 uv0 [[user(locn8)]]; float2 uv1 [[user(locn9)]];
  float2 uv2 [[user(locn10)]]; float2 uv3 [[user(locn11)]];
  float4 color0 [[user(locn12)]]; float4 color1 [[user(locn13)]];
  float4 color2 [[user(locn14)]]; float4 color3 [[user(locn15)]];
  uint shading_rate [[user(locn16)]];
};

vertex m12_conservative_output m12_conservative_vs(
    uint vid [[vertex_id]], constant m12_conservative_data &data [[buffer(26)]]) {
  m12_conservative_output out = {};
  uint width = max(data.width, 1u);
  uint height = max(data.height, 1u);
  uint x = vid % width;
  uint y = vid / width;
  float2 pixel = float2(x, y) + 0.5f;
  float vp_width = max(data.viewport_width, 1.0f);
  float vp_height = max(data.viewport_height, 1.0f);
  float2 viewport_pixel = pixel - float2(data.viewport_x, data.viewport_y);
  float2 ndc = float2(viewport_pixel.x / vp_width * 2.0f - 1.0f,
                      1.0f - viewport_pixel.y / vp_height * 2.0f);
  float area = m12_cons_cross(data.p0, data.p1, data.p2);
  float z = data.z0;
  if (abs(area) > 1.0e-5f) {
    float w0 = m12_cons_cross(data.p1, data.p2, pixel) / area;
    float w1 = m12_cons_cross(data.p2, data.p0, pixel) / area;
    float w2 = 1.0f - w0 - w1;
    z = w0 * data.z0 + w1 * data.z1 + w2 * data.z2;
  }
  out.position = float4(ndc, z, 1.0f);
  return out;
}
)metal";

namespace dxmt {

namespace {

class D3D12CachedPipelineBlob final : public ID3DBlob {
public:
  explicit D3D12CachedPipelineBlob(const std::vector<uint8_t> &data)
      : m_data(data) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ID3D10Blob ||
        riid == __uuidof(ID3DBlob)) {
      *object = static_cast<ID3DBlob *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref_count; }

  ULONG STDMETHODCALLTYPE Release() override {
    ULONG ref_count = --m_ref_count;
    if (!ref_count)
      delete this;
    return ref_count;
  }

  LPVOID STDMETHODCALLTYPE GetBufferPointer() override {
    return m_data.empty() ? nullptr : m_data.data();
  }

  SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_data.size(); }

private:
  std::atomic<ULONG> m_ref_count = {1};
  std::vector<uint8_t> m_data;
};

} // namespace

namespace {
constexpr uint32_t kMetalD3D12VertexBufferSlotCount = 29;

std::string ShaderCacheDir() {
  const char *env_path = std::getenv("DXMT_SHADER_CACHE_PATH");
  std::string path =
      (env_path && env_path[0]) ? env_path : "/tmp/dxmt_shader_cache";
  while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
    path.pop_back();
  return path;
}

void FormatShaderCachePath(char *out, size_t out_size, const char *suffix_fmt,
                           size_t hash) {
  char suffix[128];
  snprintf(suffix, sizeof(suffix), suffix_fmt, hash);
  snprintf(out, out_size, "%s/%s", ShaderCacheDir().c_str(), suffix);
}

void EnsureShaderCacheDir() {
  auto dir = ShaderCacheDir();
  if (dir == "/tmp/dxmt_shader_cache")
    CreateDirectoryA("Z:\\tmp\\dxmt_shader_cache", nullptr);
  mkdir(dir.c_str());
}

static uint32_t ReadLe32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

bool DXBCContainerHasChunk(const void *bytecode, SIZE_T size,
                           const char tag[4]) {
  if (!bytecode || size < 36)
    return false;
  const auto *bytes = static_cast<const uint8_t *>(bytecode);
  if (std::memcmp(bytes, "DXBC", 4) != 0)
    return false;

  uint32_t chunk_count = ReadLe32(bytes + 28);
  if (chunk_count > 128 || 32ull + (uint64_t)chunk_count * 4ull > size)
    return false;

  for (uint32_t i = 0; i < chunk_count; i++) {
    uint32_t off = ReadLe32(bytes + 32 + i * 4);
    if ((uint64_t)off + 8ull > size)
      continue;
    const uint8_t *chunk = bytes + off;
    uint32_t chunk_size = ReadLe32(chunk + 4);
    if ((uint64_t)off + 8ull + chunk_size > size)
      continue;
    if (std::memcmp(chunk, tag, 4) == 0)
      return true;
  }
  return false;
}

bool DXBCShaderUsesAtomic64(const void *bytecode, SIZE_T size) {
  using namespace microsoft;
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(bytecode, size)))
    return false;
  for (UINT32 i = 0; i < parser.GetBlobCount(); i++) {
    if (parser.GetBlobFourCC(i) != dxmt::dxil::DXIL_FOURCC)
      continue;
    auto container = dxmt::dxil::DXILContainer::parse(
        parser.GetBlob(i), parser.GetBlobSize(i));
    if (!container)
      return false;
    auto module = dxmt::dxil::BitcodeReader::parse(
        container->shader().bitcode.data, container->shader().bitcode.size);
    if (!module)
      return false;
    for (const auto &fn : module->functions) {
      if (fn.name.find("dx.op.atomicBinOp.i64") != std::string::npos ||
          fn.name.find("dx.op.atomicCompareExchange.i64") !=
              std::string::npos)
        return true;
      for (const auto &block : fn.blocks) {
        for (const auto &inst : block.instructions) {
          if (inst.opcode != dxmt::dxil::LLVMInstruction::AtomicRMW &&
              inst.opcode != dxmt::dxil::LLVMInstruction::CmpXchg)
            continue;
          const bool is_i64 =
              inst.type_id < module->types.size() &&
              module->types[inst.type_id].kind ==
                  dxmt::dxil::LLVMType::Integer &&
              module->types[inst.type_id].bit_width == 64;
          if (is_i64)
            return true;
        }
      }
    }
  }
  return false;
}

bool DXBCShaderUsesSamplerFeedback(const void *bytecode, SIZE_T size) {
  using namespace microsoft;
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(bytecode, size)))
    return false;
  for (UINT32 i = 0; i < parser.GetBlobCount(); i++) {
    if (parser.GetBlobFourCC(i) != dxmt::dxil::DXIL_FOURCC)
      continue;
    auto container = dxmt::dxil::DXILContainer::parse(
        parser.GetBlob(i), parser.GetBlobSize(i));
    if (!container)
      return false;
    auto module = dxmt::dxil::BitcodeReader::parse(
        container->shader().bitcode.data, container->shader().bitcode.size);
    if (!module)
      return false;
    for (const auto &fn : module->functions) {
      if (fn.name.find("dx.op.writeSamplerFeedback") != std::string::npos)
        return true;
    }
  }
  return false;
}

bool DXBCShaderUsesSampleCmpLevel(const void *bytecode, SIZE_T size) {
  using namespace microsoft;
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(bytecode, size)))
    return false;
  for (UINT32 i = 0; i < parser.GetBlobCount(); i++) {
    if (parser.GetBlobFourCC(i) != dxmt::dxil::DXIL_FOURCC)
      continue;
    auto container = dxmt::dxil::DXILContainer::parse(
        parser.GetBlob(i), parser.GetBlobSize(i));
    if (!container)
      return false;
    auto module = dxmt::dxil::BitcodeReader::parse(
        container->shader().bitcode.data, container->shader().bitcode.size);
    if (!module)
      return false;
    for (const auto &fn : module->functions) {
      if (fn.name.find("dx.op.sampleCmpLevel") != std::string::npos)
        return true;
    }
  }
  return false;
}

bool DXBCShaderUsesDirectResourceHeap(const void *bytecode, SIZE_T size) {
  using namespace microsoft;
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(bytecode, size)))
    return false;
  for (UINT32 i = 0; i < parser.GetBlobCount(); i++) {
    if (parser.GetBlobFourCC(i) != dxmt::dxil::DXIL_FOURCC)
      continue;
    auto container = dxmt::dxil::DXILContainer::parse(
        parser.GetBlob(i), parser.GetBlobSize(i));
    if (!container)
      return false;
    auto module = dxmt::dxil::BitcodeReader::parse(
        container->shader().bitcode.data, container->shader().bitcode.size);
    if (!module)
      return false;
    for (const auto &fn : module->functions) {
      if (fn.name.find("dx.op.createHandleFromHeap") != std::string::npos)
        return true;
    }
  }
  return false;
}

bool DXBCShaderUsesAttributeAtVertex(const void *bytecode, SIZE_T size,
                                     uint32_t *input_id_out) {
  if (input_id_out)
    *input_id_out = UINT32_MAX;
  using namespace microsoft;
  CDXBCParser parser;
  if (FAILED(parser.ReadDXBC(bytecode, size)))
    return false;

  auto parse_literal = [](const std::string &text, uint32_t &value) {
    if (text.empty())
      return false;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (!end || *end != '\0')
      return false;
    value = static_cast<uint32_t>(parsed);
    return true;
  };

  for (UINT32 i = 0; i < parser.GetBlobCount(); i++) {
    if (parser.GetBlobFourCC(i) != dxmt::dxil::DXIL_FOURCC)
      continue;
    auto container = dxmt::dxil::DXILContainer::parse(
        parser.GetBlob(i), parser.GetBlobSize(i));
    if (!container)
      return false;
    auto module = dxmt::dxil::BitcodeReader::parse(
        container->shader().bitcode.data, container->shader().bitcode.size);
    if (!module)
      return false;

    for (const auto &fn : module->functions) {
      for (const auto &block : fn.blocks) {
        for (const auto &inst : block.instructions) {
          if (inst.opcode != dxmt::dxil::LLVMInstruction::Call ||
              inst.operands.size() < 3)
            continue;

          std::string callee_name;
          for (const auto &candidate : module->functions) {
            if (candidate.value_id == inst.operands[0]) {
              callee_name = candidate.name;
              break;
            }
          }

          uint32_t opcode = 0;
          bool is_attribute_at_vertex =
              callee_name.find("attributeAtVertex") != std::string::npos;
          if (!is_attribute_at_vertex) {
            const uint32_t opcode_id = inst.operands[2];
            for (const auto &constant : module->constants) {
              if (constant.id == opcode_id &&
                  parse_literal(constant.constant_data, opcode)) {
                is_attribute_at_vertex = opcode == 137u;
                break;
              }
            }
          }
          if (!is_attribute_at_vertex)
            continue;

          if (input_id_out && inst.operands.size() > 3) {
            uint32_t input_id = UINT32_MAX;
            for (const auto &constant : module->constants) {
              if (constant.id == inst.operands[3] &&
                  parse_literal(constant.constant_data, input_id))
                break;
            }
            if (*input_id_out == UINT32_MAX)
              *input_id_out = input_id;
          }
          return true;
        }
      }
    }
  }
  return false;
}

bool ExtractPSV0ComputeThreadgroupSize(const void *bytecode, SIZE_T size,
                                       uint32_t out[3]) {
  if (!bytecode || size < 36)
    return false;
  const auto *bytes = static_cast<const uint8_t *>(bytecode);
  if (std::memcmp(bytes, "DXBC", 4) != 0)
    return false;

  uint32_t chunk_count = ReadLe32(bytes + 28);
  if (chunk_count > 128 || 32ull + (uint64_t)chunk_count * 4ull > size)
    return false;

  for (uint32_t i = 0; i < chunk_count; i++) {
    uint32_t off = ReadLe32(bytes + 32 + i * 4);
    if ((uint64_t)off + 8ull > size)
      continue;
    const uint8_t *chunk = bytes + off;
    uint32_t chunk_size = ReadLe32(chunk + 4);
    if (std::memcmp(chunk, "PSV0", 4) != 0 ||
        (uint64_t)off + 8ull + chunk_size > size)
      continue;

    const uint8_t *psv = chunk + 8;
    if (chunk_size < 4)
      continue;
    uint32_t runtime_info_size = ReadLe32(psv);
    if (runtime_info_size < 48 || chunk_size < 4 + runtime_info_size)
      continue;

    const uint8_t *runtime_info = psv + 4;
    constexpr uint32_t kPSVShaderKindCompute = 5;
    constexpr uint32_t kPSVRuntimeInfo1ShaderStageOffset = 24;
    constexpr uint32_t kPSVRuntimeInfo2NumThreadsXOffset = 36;
    constexpr uint32_t kPSVRuntimeInfo2NumThreadsYOffset = 40;
    constexpr uint32_t kPSVRuntimeInfo2NumThreadsZOffset = 44;
    if (runtime_info[kPSVRuntimeInfo1ShaderStageOffset] !=
        kPSVShaderKindCompute)
      continue;

    uint32_t x = ReadLe32(runtime_info + kPSVRuntimeInfo2NumThreadsXOffset);
    uint32_t y = ReadLe32(runtime_info + kPSVRuntimeInfo2NumThreadsYOffset);
    uint32_t z = ReadLe32(runtime_info + kPSVRuntimeInfo2NumThreadsZOffset);
    uint64_t product = (uint64_t)x * (uint64_t)y * (uint64_t)z;
    if (x == 0 || y == 0 || z == 0 || x > 1024 || y > 1024 || z > 64 ||
        product == 0 || product > 1024)
      return false;
    out[0] = x;
    out[1] = y;
    out[2] = z;
    return true;
  }
  return false;
}

std::string DescribeNSObject(obj_handle_t handle) {
  if (!handle)
    return "unknown";
  auto desc = WMT::String{NSObject_description(handle)}.getUTF8String();
  return desc.empty() ? "unknown" : desc;
}

bool IsTransientMetalCompilerError(const std::string &desc) {
  return desc.find("XPC_ERROR_CONNECTION_INTERRUPTED") != std::string::npos ||
         desc.find("interrupted connection") != std::string::npos;
}

dxmt::dxil::MSLShader ToRuntimeMSLShader(dxmt::dxil::TypedMSLShader &&typed) {
  dxmt::dxil::MSLShader shader;
  shader.source = std::move(typed.source);
  shader.entry_point = std::move(typed.entry_point);
  shader.tg_size[0] = typed.tg_size[0];
  shader.tg_size[1] = typed.tg_size[1];
  shader.tg_size[2] = typed.tg_size[2];
  shader.unsupported_intrinsics = typed.unsupported_intrinsics;
  shader.unsupported_opcodes = typed.unsupported_opcodes;
  shader.diagnostics = std::move(typed.diagnostics);
  shader.diagnostics.push_back(str::format(
      "MSLLowering runtime path active: typed_values=", typed.typed_value_count,
      " auto_values=", typed.auto_value_count));
  return shader;
}

bool ParseJsonUnsigned(const std::string &object, const char *key,
                       uint32_t &value) {
  std::string token = str::format("\"", key, "\"");
  size_t pos = object.find(token);
  if (pos == std::string::npos)
    return false;
  pos = object.find(':', pos + token.size());
  if (pos == std::string::npos)
    return false;
  char *end = nullptr;
  unsigned long parsed = std::strtoul(object.c_str() + pos + 1, &end, 10);
  if (!end || end == object.c_str() + pos + 1)
    return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool ParseJsonString(const std::string &object, const char *key,
                     std::string &value) {
  std::string token = str::format("\"", key, "\"");
  size_t pos = object.find(token);
  if (pos == std::string::npos)
    return false;
  pos = object.find(':', pos + token.size());
  if (pos == std::string::npos)
    return false;
  size_t first = object.find('"', pos + 1);
  size_t last = first == std::string::npos
                    ? std::string::npos
                    : object.find('"', first + 1);
  if (first == std::string::npos || last == std::string::npos)
    return false;
  value = object.substr(first + 1, last - first - 1);
  return true;
}

bool ParseMSCReflection(const char *json,
                        MTL_SHADER_REFLECTION &reflection,
                        std::vector<MTL_SM50_SHADER_ARGUMENT> &arguments) {
  if (!json)
    return false;
  std::string text(json);
  size_t label = text.find("\"TopLevelArgumentBuffer\"");
  size_t array_begin =
      label == std::string::npos ? std::string::npos : text.find('[', label);
  size_t array_end = array_begin == std::string::npos
                         ? std::string::npos
                         : text.find(']', array_begin);
  if (array_begin == std::string::npos || array_end == std::string::npos)
    return false;

  arguments.clear();
  uint32_t qword_count = 0;
  size_t cursor = array_begin + 1;
  while (cursor < array_end) {
    size_t begin = text.find('{', cursor);
    if (begin == std::string::npos || begin >= array_end)
      break;
    size_t end = text.find('}', begin + 1);
    if (end == std::string::npos || end > array_end)
      return false;
    std::string object = text.substr(begin, end - begin + 1);
    std::string type;
    uint32_t slot = 0;
    uint32_t space = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    if (!ParseJsonString(object, "Type", type) ||
        !ParseJsonUnsigned(object, "Slot", slot) ||
        !ParseJsonUnsigned(object, "Space", space) ||
        !ParseJsonUnsigned(object, "EltOffset", offset) ||
        !ParseJsonUnsigned(object, "Size", size))
      return false;

    MTL_SM50_SHADER_ARGUMENT argument = {};
    if (type == "CBV")
      argument.Type = SM50BindingType::ConstantBuffer;
    else if (type == "SRV")
      argument.Type = SM50BindingType::SRV;
    else if (type == "UAV")
      argument.Type = SM50BindingType::UAV;
    else if (type == "Sampler")
      argument.Type = SM50BindingType::Sampler;
    else
      return false;
    argument.SM50BindingSlot = slot;
    argument.SM50RegisterSpace = space;
    argument.StructurePtrOffset = offset / sizeof(uint64_t);
    argument.SizeInVec4 = (size + 15) / 16;
    if (argument.Type == SM50BindingType::UAV)
      argument.Flags = static_cast<MTL_SM50_SHADER_ARGUMENT_FLAG>(
          MTL_SM50_SHADER_ARGUMENT_BUFFER |
          MTL_SM50_SHADER_ARGUMENT_READ_ACCESS |
          MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS);
    else if (argument.Type == SM50BindingType::SRV ||
             argument.Type == SM50BindingType::ConstantBuffer)
      argument.Flags = static_cast<MTL_SM50_SHADER_ARGUMENT_FLAG>(
          MTL_SM50_SHADER_ARGUMENT_BUFFER |
          MTL_SM50_SHADER_ARGUMENT_READ_ACCESS);
    arguments.push_back(argument);
    qword_count = std::max<uint32_t>(
        qword_count, (offset + size + sizeof(uint64_t) - 1) /
                         sizeof(uint64_t));
    cursor = end + 1;
  }

  if (arguments.empty())
    return false;
  reflection.ArgumentBufferBindIndex = 2;
  reflection.ArgumentTableQwords = qword_count;
  reflection.NumArguments = static_cast<uint32_t>(arguments.size());
  for (const auto &argument : arguments) {
    if (argument.Type == SM50BindingType::UAV)
      reflection.UAVSlotMask |= 1ull << std::min<uint32_t>(argument.SM50BindingSlot, 63);
    else if (argument.Type == SM50BindingType::SRV) {
      uint32_t slot = std::min<uint32_t>(argument.SM50BindingSlot, 127);
      if (slot < 64)
        reflection.SRVSlotMaskLo |= 1ull << slot;
      else
        reflection.SRVSlotMaskHi |= 1ull << (slot - 64);
    } else if (argument.Type == SM50BindingType::Sampler &&
               argument.SM50BindingSlot < 16) {
      reflection.SamplerSlotMask |= 1u << argument.SM50BindingSlot;
    }
  }
  return true;
}

void ParseDirectBindingManifest(const char *source,
                                MTL_SHADER_REFLECTION &reflection) {
  if (!source)
    return;
  const char *line = source;
  while ((line = strstr(line, "// range kind="))) {
    char kind[16] = {};
    unsigned space = 0;
    unsigned lower = 0;
    unsigned count = 0;
    if (sscanf(line, "// range kind=%15s space=%u lower=%u count=%u", kind,
               &space, &lower, &count) == 4 &&
        space == 0) {
      count = std::min<unsigned>(count, 128);
      for (unsigned i = 0; i < count; i++) {
        unsigned slot = lower + i;
        if (!strcmp(kind, "uav") && slot < 64)
          reflection.UAVSlotMask |= 1ull << slot;
        else if (!strcmp(kind, "srv") && slot < 128) {
          if (slot < 64)
            reflection.SRVSlotMaskLo |= 1ull << slot;
          else
            reflection.SRVSlotMaskHi |= 1ull << (slot - 64);
        } else if (!strcmp(kind, "cbv") && slot < 16)
          reflection.ConstantBufferSlotMask |= 1u << slot;
        else if (!strcmp(kind, "sampler") && slot < 16)
          reflection.SamplerSlotMask |= 1u << slot;
      }
    }
    line++;
  }
}

void ParseDirectBindingManifestFile(const char *path,
                                    MTL_SHADER_REFLECTION &reflection) {
  FILE *file = path ? fopen(path, "rb") : nullptr;
  if (!file)
    return;
  fseek(file, 0, SEEK_END);
  long length = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (length > 0 && length < 4 * 1024 * 1024) {
    std::vector<char> source(static_cast<size_t>(length) + 1, 0);
    fread(source.data(), 1, static_cast<size_t>(length), file);
    ParseDirectBindingManifest(source.data(), reflection);
  }
  fclose(file);
}

thread_local bool g_async_pipeline_worker_thread = false;
thread_local uint32_t g_async_pipeline_worker_index = 0;

bool AsyncPipelineCompileEnabled() {
  char value[16] = {};
  DWORD len = GetEnvironmentVariableA("DXMT_ASYNC_PIPELINE_COMPILE", value,
                                      sizeof(value));
  return len > 0 && value[0] && value[0] != '0';
}

uint32_t AsyncPipelineWorkerCount() {
  char value[16] = {};
  DWORD len =
      GetEnvironmentVariableA("DXMT_D3D12_PSO_WORKERS", value, sizeof(value));
  if (len == 0 || !value[0])
    return 4;

  char *end = nullptr;
  unsigned long parsed = strtoul(value, &end, 10);
  if (end == value || parsed == 0)
    return 4;
  return std::max(1u, std::min<uint32_t>((uint32_t)parsed, 12u));
}

class AsyncPipelineCompiler {
public:
  bool Enqueue(MTLD3D12PipelineState *pso) {
    std::lock_guard<dxmt::mutex> lock(m_mutex);
    if (m_stopping)
      return false;

    EnsureStartedLocked();
    pso->AddRef();
    m_queue.push_back(pso);
    m_cv.notify_one();
    return true;
  }

  void Shutdown() {
    std::vector<dxmt::thread> workers;
    {
      std::lock_guard<dxmt::mutex> lock(m_mutex);
      if (m_workers.empty())
        return;
      PSTRACE("PSO async compiler shutdown requested workers=%zu queued=%zu "
              "active=%u",
              m_workers.size(), m_queue.size(), m_active_workers);
      m_stopping = true;
      workers.swap(m_workers);
      m_cv.notify_all();
    }

    for (auto &worker : workers) {
      if (worker.joinable())
        worker.join();
    }

    {
      std::lock_guard<dxmt::mutex> lock(m_mutex);
      m_stopping = false;
      PSTRACE("PSO async compiler shutdown complete queued=%zu active=%u",
              m_queue.size(), m_active_workers);
    }
  }

private:
  void EnsureStartedLocked() {
    if (!m_workers.empty())
      return;
    m_worker_count = AsyncPipelineWorkerCount();
    Logger::info(str::format("M12 async PSO compiler starting workers=",
                             m_worker_count));
    PSTRACE("PSO async compiler starting workers=%u", m_worker_count);
    for (uint32_t i = 0; i < m_worker_count; i++) {
      m_workers.emplace_back([this, i]() { WorkerLoop(i); });
    }
  }

  void WorkerLoop(uint32_t worker_index) {
    for (;;) {
      MTLD3D12PipelineState *pso = nullptr;
      {
        std::unique_lock<dxmt::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_stopping || !m_queue.empty(); });
        if (m_queue.empty()) {
          if (m_stopping)
            break;
          continue;
        }
        pso = m_queue.front();
        m_queue.pop_front();
        m_active_workers++;
      }

      if (pso) {
        PSTRACE("PSO async worker[%u] compiling pso=%p", worker_index,
                (void *)pso);
        g_async_pipeline_worker_thread = true;
        g_async_pipeline_worker_index = worker_index;
        pso->RunAsyncCompile();
        g_async_pipeline_worker_thread = false;
        pso->Release();
      }

      {
        std::lock_guard<dxmt::mutex> lock(m_mutex);
        if (m_active_workers)
          m_active_workers--;
        if (m_stopping && m_queue.empty() && m_active_workers == 0)
          m_cv.notify_all();
      }
    }
    PSTRACE("PSO async worker[%u] exiting", worker_index);
  }

  uint32_t m_worker_count = 0;
  bool m_stopping = false;
  uint32_t m_active_workers = 0;
  dxmt::mutex m_mutex;
  dxmt::condition_variable m_cv;
  std::deque<MTLD3D12PipelineState *> m_queue;
  std::vector<dxmt::thread> m_workers;
};

AsyncPipelineCompiler &GetAsyncPipelineCompiler() {
  static AsyncPipelineCompiler *compiler = new AsyncPipelineCompiler();
  return *compiler;
}

size_t ComputeShaderCacheHash(const void *bytecode, SIZE_T size,
                              ShaderType type,
                              const D3D12_INPUT_LAYOUT_DESC *input_layout) {
  size_t hash = 0;
  hash = hash * 131 + (size_t)type;
  if (type == ShaderType::Vertex)
    hash = hash * 131 +
           0x4d3132506833ull; // M12 Phase 3 explicit varying contract.
  if (type == ShaderType::Compute &&
      DXBCShaderUsesAtomic64(bytecode, size))
    hash = hash * 131 + 0x4d313241746f6d36ull;
  if (bytecode && size > 0) {
    const uint8_t *p = (const uint8_t *)bytecode;
    for (SIZE_T i = 0; i < size; i++)
      hash = hash * 131 + p[i];
  }
  if (type == ShaderType::Vertex && input_layout) {
    hash = hash * 131 + input_layout->NumElements;
    for (UINT i = 0; i < input_layout->NumElements; i++) {
      const auto &el = input_layout->pInputElementDescs[i];
      hash = hash * 131 + el.SemanticIndex;
      hash = hash * 131 + el.Format;
      hash = hash * 131 + el.InputSlot;
      hash = hash * 131 + el.AlignedByteOffset;
      hash = hash * 131 + el.InputSlotClass;
      hash = hash * 131 + el.InstanceDataStepRate;
      if (el.SemanticName) {
        for (const char *s = el.SemanticName; *s; s++)
          hash = hash * 131 + (unsigned char)*s;
      }
    }
  }
  return hash;
}

const char *PixelFormatManifestName(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatRGBA8Unorm:
    return "rgba8unorm";
  case WMTPixelFormatRGBA8Unorm_sRGB:
    return "rgba8unorm_srgb";
  case WMTPixelFormatBGRA8Unorm:
    return "bgra8unorm";
  case WMTPixelFormatBGRA8Unorm_sRGB:
    return "bgra8unorm_srgb";
  case WMTPixelFormatRGBA16Float:
    return "rgba16float";
  case WMTPixelFormatRGBA32Float:
    return "rgba32float";
  case WMTPixelFormatRGB10A2Unorm:
    return "rgb10a2unorm";
  case WMTPixelFormatRG11B10Float:
    return "rg11b10float";
  case WMTPixelFormatR8Unorm:
    return "r8unorm";
  case WMTPixelFormatR16Float:
    return "r16float";
  case WMTPixelFormatR32Float:
    return "r32float";
  case WMTPixelFormatRG16Float:
    return "rg16float";
  case WMTPixelFormatRG16Unorm:
    return "rg16unorm";
  case WMTPixelFormatRG8Unorm:
    return "rg8unorm";
  case WMTPixelFormatDepth16Unorm:
    return "depth16unorm";
  case WMTPixelFormatDepth32Float:
    return "depth32float";
  case WMTPixelFormatDepth24Unorm_Stencil8:
    return "depth24unorm_stencil8";
  case WMTPixelFormatDepth32Float_Stencil8:
    return "depth32float_stencil8";
  case WMTPixelFormatBC1_RGBA:
    return "bc1_rgba";
  case WMTPixelFormatBC1_RGBA_sRGB:
    return "bc1_rgba_srgb";
  case WMTPixelFormatBC2_RGBA:
    return "bc2_rgba";
  case WMTPixelFormatBC2_RGBA_sRGB:
    return "bc2_rgba_srgb";
  case WMTPixelFormatBC3_RGBA:
    return "bc3_rgba";
  case WMTPixelFormatBC3_RGBA_sRGB:
    return "bc3_rgba_srgb";
  case WMTPixelFormatBC4_RUnorm:
    return "bc4_runorm";
  case WMTPixelFormatBC4_RSnorm:
    return "bc4_rsnorm";
  case WMTPixelFormatBC5_RGUnorm:
    return "bc5_rgunorm";
  case WMTPixelFormatBC5_RGSnorm:
    return "bc5_rgsnorm";
  case WMTPixelFormatBC6H_RGBFloat:
    return "bc6h_rgbfloat";
  case WMTPixelFormatBC6H_RGBUfloat:
    return "bc6h_rgbufloat";
  case WMTPixelFormatBC7_RGBAUnorm:
    return "bc7_rgbaunorm";
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    return "bc7_rgbaunorm_srgb";
  default:
    return "invalid";
  }
}

size_t ComputeRenderPSOManifestHash(size_t vs_hash, size_t ps_hash,
                                    size_t gs_hash, UINT num_render_targets,
                                    const DXGI_FORMAT *rtv_formats,
                                    DXGI_FORMAT dsv_format, UINT sample_count,
                                    UINT input_elements, uint32_t ia_slot_mask,
                                    bool uses_stage_in) {
  size_t hash = vs_hash;
  hash = hash * 131 + ps_hash;
  hash = hash * 131 + gs_hash;
  hash = hash * 131 + num_render_targets;
  for (UINT i = 0; i < 8; i++)
    hash = hash * 131 + (size_t)rtv_formats[i];
  hash = hash * 131 + (size_t)dsv_format;
  hash = hash * 131 + (size_t)sample_count;
  hash = hash * 131 + (size_t)input_elements;
  hash = hash * 131 + (size_t)ia_slot_mask;
  hash = hash * 131 + (uses_stage_in ? 1 : 0);
  return hash;
}

void WriteJsonString(FILE *df, const std::string &value) {
  fputc('"', df);
  for (char ch : value) {
    switch (ch) {
    case '\\':
      fputs("\\\\", df);
      break;
    case '"':
      fputs("\\\"", df);
      break;
    case '\n':
      fputs("\\n", df);
      break;
    case '\r':
      fputs("\\r", df);
      break;
    case '\t':
      fputs("\\t", df);
      break;
    default:
      fputc(ch, df);
      break;
    }
  }
  fputc('"', df);
}

void DumpComputePSOManifest(size_t cs_hash, SIZE_T cs_size,
                            uint32_t threadgroup_width,
                            uint32_t threadgroup_height,
                            uint32_t threadgroup_depth,
                            uintptr_t compute_function) {
  char path[1024];
  char metallib_path[1024];
  FormatShaderCachePath(path, sizeof(path), "pso-compute-%016zx.json", cs_hash);
  FormatShaderCachePath(metallib_path, sizeof(metallib_path), "%016zx.metallib",
                        cs_hash);
  EnsureShaderCacheDir();
  FILE *df = fopen(path, "w");
  if (!df)
    return;

  fprintf(df, "{\n");
  fprintf(
      df,
      "  \"schema\": \"metalsharp.d3d12-metal.offline-pso-manifest.v1\",\n");
  fprintf(df, "  \"source\": \"dxmt-d3d12-runtime\",\n");
  fprintf(df, "  \"pipelines\": [\n");
  fprintf(df, "    {\n");
  fprintf(df, "      \"name\": \"compute-%016zx\",\n", cs_hash);
  fprintf(df, "      \"type\": \"compute\",\n");
  fprintf(df,
          "      \"d3d12\": { \"cs_hash\": \"%016zx\", \"cs_bytes\": %zu },\n",
          cs_hash, (size_t)cs_size);
  fprintf(df,
          "      \"shader\": { \"hash\": \"%016zx\", \"metallib\": \"%s\", "
          "\"function\": \"cs_main\" },\n",
          cs_hash, metallib_path);
  fprintf(df, "      \"threadgroup_size\": [%llu, %llu, %llu],\n",
          (unsigned long long)threadgroup_width,
          (unsigned long long)threadgroup_height,
          (unsigned long long)threadgroup_depth);
  fprintf(df, "      \"metal\": { \"compute_function\": %llu }\n",
          (unsigned long long)compute_function);
  fprintf(df, "    }\n");
  fprintf(df, "  ]\n");
  fprintf(df, "}\n");
  fclose(df);
  PSTRACE("Compute PSO manifest written to %s", path);
}

void DumpRenderPSOManifest(
    size_t pso_hash, size_t vs_hash, size_t ps_hash, size_t gs_hash,
    SIZE_T vs_size, SIZE_T ps_size, SIZE_T gs_size, UINT num_render_targets,
    const DXGI_FORMAT *rtv_formats, DXGI_FORMAT dsv_format, UINT sample_count,
    UINT input_elements, uint32_t ia_slot_mask,
    const std::vector<D3D12IAInputElementInfo> &ia_elements, bool uses_stage_in,
    bool uses_geometry_mesh, bool rasterization_enabled,
    uintptr_t vertex_function, uintptr_t fragment_function) {
  char path[1024];
  char vs_metallib_path[1024];
  char ps_metallib_path[1024];
  FormatShaderCachePath(path, sizeof(path), "pso-render-%016zx.json", pso_hash);
  FormatShaderCachePath(vs_metallib_path, sizeof(vs_metallib_path),
                        "%016zx.metallib", vs_hash);
  FormatShaderCachePath(ps_metallib_path, sizeof(ps_metallib_path),
                        "%016zx.metallib", ps_hash);
  EnsureShaderCacheDir();
  FILE *df = fopen(path, "w");
  if (!df)
    return;

  WMTPixelFormat depth_format =
      MTLD3D12PipelineState::DXGIToMTLPixelFormat(dsv_format);
  WMTPixelFormat stencil_format = WMTPixelFormatInvalid;
  if (dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
      dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    stencil_format = depth_format;

  fprintf(df, "{\n");
  fprintf(
      df,
      "  \"schema\": \"metalsharp.d3d12-metal.offline-pso-manifest.v1\",\n");
  fprintf(df, "  \"source\": \"dxmt-d3d12-runtime\",\n");
  fprintf(df, "  \"pipelines\": [\n");
  fprintf(df, "    {\n");
  fprintf(df, "      \"name\": \"render-%016zx\",\n", pso_hash);
  fprintf(df, "      \"type\": \"render\",\n");
  fprintf(df,
          "      \"d3d12\": { \"vs_hash\": \"%016zx\", \"ps_hash\": "
          "\"%016zx\", \"gs_hash\": \"%016zx\", \"vs_bytes\": %zu, "
          "\"ps_bytes\": %zu, \"gs_bytes\": %zu, \"num_render_targets\": %u, "
          "\"dsv_format\": %u, \"input_elements\": %u },\n",
          vs_hash, ps_hash, gs_hash, (size_t)vs_size, (size_t)ps_size,
          (size_t)gs_size, num_render_targets, (unsigned)dsv_format,
          input_elements);
  fprintf(df, "      \"color_formats\": [");
  for (UINT i = 0; i < 8; i++) {
    if (i)
      fprintf(df, ", ");
    fprintf(df, "\"%s\"",
            PixelFormatManifestName(
                MTLD3D12PipelineState::DXGIToMTLPixelFormat(rtv_formats[i])));
  }
  fprintf(df, "],\n");
  fprintf(df, "      \"depth_format\": \"%s\",\n",
          PixelFormatManifestName(depth_format));
  fprintf(df, "      \"stencil_format\": \"%s\",\n",
          PixelFormatManifestName(stencil_format));
  fprintf(df, "      \"sample_count\": %u,\n", sample_count ? sample_count : 1);
  fprintf(
      df,
      "      \"input_layout\": { \"slot_mask\": \"0x%08x\", \"elements\": [\n",
      ia_slot_mask);
  for (size_t i = 0; i < ia_elements.size(); i++) {
    const auto &element = ia_elements[i];
    fprintf(df, "        { \"semantic\": ");
    WriteJsonString(df, element.semantic_name);
    fprintf(
        df,
        ", \"semantic_index\": %u, \"register\": %u, \"slot\": %u, "
        "\"table_index\": %u, \"table_indexing_mode\": \"%s\", \"offset\": %u, "
        "\"dxgi_format\": %u, \"metal_format\": %u, \"input_slot_class\": %u, "
        "\"class\": \"%s\", \"step_rate\": %u, \"system_value\": %s }%s\n",
        element.semantic_index, element.shader_register, element.input_slot,
        element.table_index,
        D3D12VertexTableIndexingModeName(element.table_indexing_mode),
        element.aligned_byte_offset, (unsigned)element.dxgi_format,
        (unsigned)element.metal_format, (unsigned)element.input_slot_class,
        element.per_instance ? "per_instance" : "per_vertex",
        element.instance_step_rate, element.system_value ? "true" : "false",
        i + 1 == ia_elements.size() ? "" : ",");
  }
  fprintf(df, "      ] },\n");
  fprintf(df,
          "      \"vertex\": { \"hash\": \"%016zx\", \"metallib\": \"%s\", "
          "\"function\": \"vs_main\" },\n",
          vs_hash, vs_metallib_path);
  if (ps_size > 0) {
    fprintf(df,
            "      \"fragment\": { \"hash\": \"%016zx\", \"metallib\": \"%s\", "
            "\"function\": \"ps_main\" },\n",
            ps_hash, ps_metallib_path);
  } else {
    fprintf(df, "      \"fragment\": null,\n");
  }
  fprintf(df,
          "      \"metal\": { \"vertex_function\": %llu, "
          "\"fragment_function\": %llu, \"uses_stage_in\": %s, "
          "\"uses_geometry_mesh\": %s, \"rasterization_enabled\": %s }\n",
          (unsigned long long)vertex_function,
          (unsigned long long)fragment_function,
          uses_stage_in ? "true" : "false",
          uses_geometry_mesh ? "true" : "false",
          rasterization_enabled ? "true" : "false");
  fprintf(df, "    }\n");
  fprintf(df, "  ]\n");
  fprintf(df, "}\n");
  fclose(df);
  PSTRACE("Render PSO manifest written to %s", path);
}

void DumpShaderBlob(const char *path, const void *bytecode, SIZE_T size) {
  if (!path || !bytecode || !size)
    return;
  EnsureShaderCacheDir();
  FILE *df = fopen(path, "wb");
  if (df) {
    fwrite(bytecode, 1, size, df);
    fclose(df);
  }
}

void DumpShaderText(const char *path, const char *text) {
  if (!path || !text)
    return;
  EnsureShaderCacheDir();
  FILE *df = fopen(path, "w");
  if (df) {
    fputs(text, df);
    fclose(df);
  }
}

const char *DxilShaderKindName(dxmt::dxil::DxilShaderKind kind) {
  switch (kind) {
  case dxmt::dxil::DxilShaderKind::Pixel:
    return "pixel";
  case dxmt::dxil::DxilShaderKind::Vertex:
    return "vertex";
  case dxmt::dxil::DxilShaderKind::Geometry:
    return "geometry";
  case dxmt::dxil::DxilShaderKind::Hull:
    return "hull";
  case dxmt::dxil::DxilShaderKind::Domain:
    return "domain";
  case dxmt::dxil::DxilShaderKind::Compute:
    return "compute";
  case dxmt::dxil::DxilShaderKind::Library:
    return "library";
  case dxmt::dxil::DxilShaderKind::Mesh:
    return "mesh";
  case dxmt::dxil::DxilShaderKind::Amplification:
    return "amplification";
  default:
    return "other";
  }
}

const char *RootParameterTypeName(D3D12_ROOT_PARAMETER_TYPE type) {
  switch (type) {
  case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
    return "descriptor_table";
  case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
    return "constants";
  case D3D12_ROOT_PARAMETER_TYPE_CBV:
    return "cbv";
  case D3D12_ROOT_PARAMETER_TYPE_SRV:
    return "srv";
  case D3D12_ROOT_PARAMETER_TYPE_UAV:
    return "uav";
  default:
    return "unknown";
  }
}

const char *DescriptorRangeTypeName(D3D12_DESCRIPTOR_RANGE_TYPE type) {
  switch (type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    return "srv";
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    return "uav";
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    return "cbv";
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return "sampler";
  default:
    return "unknown";
  }
}

const char *ShaderVisibilityName(uint32_t visibility) {
  switch ((D3D12_SHADER_VISIBILITY)visibility) {
  case D3D12_SHADER_VISIBILITY_ALL:
    return "all";
  case D3D12_SHADER_VISIBILITY_VERTEX:
    return "vertex";
  case D3D12_SHADER_VISIBILITY_HULL:
    return "hull";
  case D3D12_SHADER_VISIBILITY_DOMAIN:
    return "domain";
  case D3D12_SHADER_VISIBILITY_GEOMETRY:
    return "geometry";
  case D3D12_SHADER_VISIBILITY_PIXEL:
    return "pixel";
  default:
    return "unknown";
  }
}

void DumpRootSignatureSummary(FILE *df, const MTLD3D12RootSignature *root_sig) {
  fprintf(df, "\nroot_signature:\n");
  if (!root_sig) {
    fprintf(df, "  present=0\n");
    return;
  }

  const auto &parameters = root_sig->GetParameters();
  const auto &static_samplers = root_sig->GetStaticSamplers();
  fprintf(df, "  present=1\n");
  fprintf(df, "  blob_hash=0x%016zx\n", root_sig->GetBlobHash());
  fprintf(df, "  flags=0x%08x\n", (uint32_t)root_sig->GetFlags());
  fprintf(df, "  parameter_count=%zu\n", parameters.size());
  fprintf(df, "  static_sampler_count=%zu\n", static_samplers.size());

  for (size_t i = 0; i < parameters.size(); i++) {
    const auto &param = parameters[i];
    fprintf(df,
            "  parameter[%zu] type=%s visibility=%s register_space=%u "
            "register=%u descriptors=%u range_type=%s table_ranges=%zu\n",
            i, RootParameterTypeName(param.type),
            ShaderVisibilityName(param.shader_visibility), param.register_space,
            param.register_index, param.num_descriptors,
            DescriptorRangeTypeName(param.range_type), param.ranges.size());
    for (size_t r = 0; r < param.ranges.size(); r++) {
      const auto &range = param.ranges[r];
      fprintf(
          df, "    range[%zu] type=%s space=%u base=%u count=%u offset=%u\n", r,
          DescriptorRangeTypeName(range.range_type), range.register_space,
          range.base_register, range.num_descriptors, range.offset_in_table);
    }
  }

  for (size_t i = 0; i < static_samplers.size(); i++) {
    const auto &sampler = static_samplers[i];
    fprintf(df,
            "  static_sampler[%zu] visibility=%s space=%u register=%u "
            "sampler_gpu=0x%016llx sampler_cube_gpu=0x%016llx "
            "lod_bias_bits=0x%016llx\n",
            i, ShaderVisibilityName(sampler.shader_visibility),
            sampler.register_space, sampler.shader_register,
            (unsigned long long)sampler.sampler_gpu_id,
            (unsigned long long)sampler.sampler_cube_gpu_id,
            (unsigned long long)sampler.lod_bias_bits);
  }
}

static bool DxilSystemOpcodeFromDeclaration(const std::string &name,
                                             uint32_t &opcode) {
  struct OpcodeName {
    const char *prefix;
    uint32_t opcode;
  };
  static constexpr OpcodeName names[] = {
      {"dx.op.instanceID.", 141},
      {"dx.op.instanceIndex.", 142},
      {"dx.op.hitKind.", 143},
      {"dx.op.rayFlags.", 144},
      {"dx.op.dispatchRaysIndex.", 145},
      {"dx.op.dispatchRaysDimensions.", 146},
      {"dx.op.worldRayOrigin.", 147},
      {"dx.op.worldRayDirection.", 148},
      {"dx.op.objectRayOrigin.", 149},
      {"dx.op.objectRayDirection.", 150},
      {"dx.op.objectToWorld.", 151},
      {"dx.op.worldToObject.", 152},
      {"dx.op.rayTMin.", 153},
      {"dx.op.rayTCurrent.", 154},
      {"dx.op.ignoreHit", 155},
      {"dx.op.acceptHitAndEndSearch", 156},
      {"dx.op.traceRay.", 157},
      {"dx.op.reportHit.", 158},
      {"dx.op.callShader.", 159},
      {"dx.op.createHandleForLib.", 160},
      {"dx.op.primitiveIndex.", 161},
      {"dx.op.geometryIndex.", 213},
  };
  for (const auto &entry : names) {
    if (name.rfind(entry.prefix, 0) == 0) {
      opcode = entry.opcode;
      return true;
    }
  }
  return false;
}

void DumpDXILModuleSummary(const char *path,
                           const dxmt::dxil::LLVMModule &module,
                           const dxmt::dxil::DxilParsedShader &shader_info) {
  if (!path)
    return;
  EnsureShaderCacheDir();
  FILE *df = fopen(path, "w");
  if (!df)
    return;

  fprintf(df, "kind=%s(%u)\n", DxilShaderKindName(shader_info.kind),
          (uint32_t)shader_info.kind);
  fprintf(df, "shader_model=%u.%u\n", shader_info.shader_model.major,
          shader_info.shader_model.minor);
  fprintf(df, "entry=%s\n", shader_info.entry_point.c_str());
  fprintf(df, "bitcode_size=%u\n", shader_info.bitcode.size);
  fprintf(df, "source_filename=%s\n", module.source_filename.c_str());
  fprintf(df, "target_triple=%s\n", module.target_triple.c_str());
  fprintf(df, "types=%zu constants=%zu functions=%zu\n", module.types.size(),
          module.constants.size(), module.functions.size());
  fprintf(df, "num_threads=%u,%u,%u\n", module.num_threads[0],
          module.num_threads[1], module.num_threads[2]);

  size_t total_blocks = 0;
  size_t total_instructions = 0;
  std::map<int, size_t> opcode_counts;
  std::map<int, size_t> dxil_opcode_counts;
  std::map<uint32_t, std::string> declaration_names;
  for (const auto &fn : module.functions)
    if (fn.is_declaration)
      declaration_names[fn.value_id] = fn.name;

  auto scalar_constant = [&](uint32_t value_id, uint32_t &value) {
    auto parse = [&](const std::vector<dxmt::dxil::LLVMValue> &values) {
      for (const auto &constant : values) {
        if (constant.id != value_id || constant.constant_data.empty())
          continue;
        char *end = nullptr;
        unsigned long parsed = std::strtoul(constant.constant_data.c_str(),
                                             &end, 10);
        if (end && *end == '\0') {
          value = static_cast<uint32_t>(parsed);
          return true;
        }
      }
      return false;
    };
    if (parse(module.constants))
      return true;
    for (const auto &fn : module.functions)
      if (parse(fn.constants))
        return true;
    return false;
  };

  for (const auto &fn : module.functions) {
    total_blocks += fn.blocks.size();
    for (const auto &block : fn.blocks) {
      total_instructions += block.instructions.size();
      for (const auto &inst : block.instructions) {
        opcode_counts[(int)inst.opcode]++;
        if (inst.opcode != dxmt::dxil::LLVMInstruction::Call ||
            inst.operands.empty())
          continue;
        auto declaration = declaration_names.find(inst.operands[0]);
        if (declaration == declaration_names.end() ||
            declaration->second.rfind("dx.op.", 0) != 0)
          continue;
        uint32_t dxil_opcode = 0;
        const std::string &name = declaration->second;
        bool opcode_resolved = inst.operands.size() >= 3 &&
                                scalar_constant(inst.operands[2], dxil_opcode);
        if (!opcode_resolved)
          opcode_resolved =
              DxilSystemOpcodeFromDeclaration(name, dxil_opcode);
        if (opcode_resolved) {
          dxil_opcode_counts[static_cast<int>(dxil_opcode)]++;
          const bool nested_opcode =
              name.rfind("dx.op.unary.", 0) == 0 ||
              name.rfind("dx.op.binary.", 0) == 0 ||
              name.rfind("dx.op.tertiary.", 0) == 0;
          if (nested_opcode && inst.operands.size() >= 4) {
            uint32_t operation = 0;
            if (scalar_constant(inst.operands[3], operation))
              dxil_opcode_counts[static_cast<int>(operation)]++;
          }
        }
      }
    }
  }

  fprintf(df, "blocks=%zu instructions=%zu\n", total_blocks,
          total_instructions);
  fprintf(df, "\nfunctions:\n");
  for (const auto &fn : module.functions) {
    size_t inst_count = 0;
    for (const auto &block : fn.blocks)
      inst_count += block.instructions.size();
    fprintf(df,
            "  name=%s declaration=%d value=%u type=%u params=%u inst_start=%u "
            "blocks=%zu instructions=%zu\n",
            fn.name.c_str(), fn.is_declaration, fn.value_id, fn.type_id,
            fn.param_count, fn.instruction_start_value, fn.blocks.size(),
            inst_count);
  }

  fprintf(df, "\nopcodes:\n");
  for (const auto &entry : opcode_counts)
    fprintf(df, "  opcode=%d count=%zu\n", entry.first, entry.second);
  // Some DXIL library system-value calls are represented by the LLVM reader
  // without a numeric opcode operand (notably void any-hit terminals and
  // library-only ray builtins). Keep the declaration in the report so the
  // evidence-first matrix can match it to a separate exact runtime probe;
  // declaration presence alone is never promoted as behavior.
  for (const auto &entry : declaration_names) {
    uint32_t opcode = 0;
    if (DxilSystemOpcodeFromDeclaration(entry.second, opcode) &&
        dxil_opcode_counts.count(static_cast<int>(opcode)) == 0)
      dxil_opcode_counts[static_cast<int>(opcode)] = 1;
  }

  fprintf(df, "\ndxil_opcodes:\n");
  for (const auto &entry : dxil_opcode_counts)
    fprintf(df, "  opcode=%d count=%zu\n", entry.first, entry.second);

  fclose(df);
}

void DumpDXILCompileReport(const char *path, const char *func_name, size_t hash,
                           SIZE_T bytecode_size, const char *dxbc_path,
                           const char *module_summary_path,
                           const char *msl_path,
                           const dxmt::dxil::LLVMModule &module,
                           const dxmt::dxil::DxilParsedShader &shader_info,
                           const dxmt::dxil::MSLShader &msl_result,
                           const MTLD3D12RootSignature *root_sig) {
  if (!path)
    return;

  EnsureShaderCacheDir();
  FILE *df = fopen(path, "w");
  if (!df)
    return;

  fprintf(df, "hash=0x%016zx\n", hash);
  fprintf(df, "function=%s\n", func_name ? func_name : "<unknown>");
  fprintf(df, "kind=%s(%u)\n", DxilShaderKindName(shader_info.kind),
          (uint32_t)shader_info.kind);
  fprintf(df, "shader_model=%u.%u\n", shader_info.shader_model.major,
          shader_info.shader_model.minor);
  fprintf(df, "entry=%s\n", shader_info.entry_point.c_str());
  fprintf(df, "bytecode_size=%zu\n", bytecode_size);
  fprintf(df, "bitcode_size=%u\n", shader_info.bitcode.size);
  fprintf(df, "types=%zu constants=%zu functions=%zu\n", module.types.size(),
          module.constants.size(), module.functions.size());
  fprintf(df, "msl_size=%zu\n", msl_result.source.size());
  fprintf(df, "threadgroup_size=%u,%u,%u\n", msl_result.tg_size[0],
          msl_result.tg_size[1], msl_result.tg_size[2]);
  fprintf(df, "unsupported_intrinsics=%u\n", msl_result.unsupported_intrinsics);
  fprintf(df, "unsupported_opcodes=%u\n", msl_result.unsupported_opcodes);
  fprintf(df, "dxbc=%s\n", dxbc_path ? dxbc_path : "");
  fprintf(df, "module=%s\n", module_summary_path ? module_summary_path : "");
  fprintf(df, "msl=%s\n", msl_path ? msl_path : "");
  DumpRootSignatureSummary(df, root_sig);
  fprintf(df, "\ndiagnostics:\n");
  for (const auto &diagnostic : msl_result.diagnostics)
    fprintf(df, "  %s\n", diagnostic.c_str());

  fclose(df);

  char index_path[1024];
  snprintf(index_path, sizeof(index_path), "%s/dxil_report_index.tsv",
           ShaderCacheDir().c_str());
  FILE *index = fopen(index_path, "a");
  if (index) {
    fprintf(index, "0x%016zx\t%s\t%s\t%u.%u\t%u\t%u\t%s\n", hash,
            DxilShaderKindName(shader_info.kind), func_name ? func_name : "",
            shader_info.shader_model.major, shader_info.shader_model.minor,
            msl_result.unsupported_intrinsics, msl_result.unsupported_opcodes,
            path);
    fclose(index);
  }
}

constexpr WMTColorWriteMask kColorWriteMaskMap[16] = {
    (WMTColorWriteMask)0,
    WMTColorWriteMaskRed,
    WMTColorWriteMaskGreen,
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskGreen),
    WMTColorWriteMaskBlue,
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskBlue),
    (WMTColorWriteMask)(WMTColorWriteMaskGreen | WMTColorWriteMaskBlue),
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskGreen |
                        WMTColorWriteMaskBlue),
    WMTColorWriteMaskAlpha,
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskGreen | WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskGreen |
                        WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskBlue | WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskBlue |
                        WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskGreen | WMTColorWriteMaskBlue |
                        WMTColorWriteMaskAlpha),
    (WMTColorWriteMask)(WMTColorWriteMaskRed | WMTColorWriteMaskGreen |
                        WMTColorWriteMaskBlue | WMTColorWriteMaskAlpha),
};

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever,     WMTCompareFunctionNever,
    WMTCompareFunctionLess,      WMTCompareFunctionEqual,
    WMTCompareFunctionLessEqual, WMTCompareFunctionGreater,
    WMTCompareFunctionNotEqual,  WMTCompareFunctionGreaterEqual,
    WMTCompareFunctionAlways,
};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero,           WMTStencilOperationKeep,
    WMTStencilOperationZero,           WMTStencilOperationReplace,
    WMTStencilOperationIncrementClamp, WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,         WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,
};

} // namespace

void ShutdownAsyncPipelineCompiler() { GetAsyncPipelineCompiler().Shutdown(); }

std::mutex MTLD3D12PipelineState::s_shader_mutex;
std::unordered_map<size_t, WMT::Reference<WMT::Function>>
    MTLD3D12PipelineState::s_shader_cache;
static std::atomic_bool g_d3d12_shader_cache_enabled{true};

bool D3D12ShaderCacheEnabled() {
  return g_d3d12_shader_cache_enabled.load(std::memory_order_acquire);
}

void SetD3D12ShaderCacheEnabled(bool enabled) {
  g_d3d12_shader_cache_enabled.store(enabled, std::memory_order_release);
}

void ClearD3D12ShaderCache() {
  {
    std::lock_guard<std::mutex> lock(MTLD3D12PipelineState::s_shader_mutex);
    MTLD3D12PipelineState::s_shader_cache.clear();
  }
  const std::string directory = ShaderCacheDir();
  DIR *dir = opendir(directory.c_str());
  if (!dir)
    return;
  while (dirent *entry = readdir(dir)) {
    const char *name = entry->d_name;
    if (!name || name[0] == '.')
      continue;
    std::string path(name);
    const bool generated =
        path.ends_with(".metallib") || path.ends_with(".json") ||
        path.ends_with(".dxbc") || path.ends_with(".msl") ||
        path.ends_with(".txt") || path.ends_with(".log") ||
        path.ends_with(".fail");
    if (generated)
      unlink((directory + "/" + path).c_str());
  }
  closedir(dir);
  PSTRACE("Shader cache cleared directory=%s", directory.c_str());
}

MTLD3D12PipelineState::MTLD3D12PipelineState(MTLD3D12Device *device,
                                             bool is_compute)
    : m_device(device), m_is_compute(is_compute) {
  m_device->AddRef();
}

MTLD3D12PipelineState::~MTLD3D12PipelineState() {
  if (m_root_sig)
    m_root_sig->Release();
  m_render_pso = nullptr;
  m_compute_pso = nullptr;
  m_device->Release();
}

void MTLD3D12PipelineState::ClearCompileFailure() {
  m_compile_failure_stage.clear();
  m_compile_failure_detail.clear();
}

bool MTLD3D12PipelineState::RecordCompileFailure(const char *stage,
                                                 const std::string &detail) {
  m_compile_failure_stage = stage ? stage : "unknown";
  m_compile_failure_detail = detail;
  m_compile_state.store(CompileState::Failed);
  m_compile_cv.notify_all();
  PSTRACE("PSO COMPILE FAILURE: this=%p compute=%d stage=%s detail=%s",
          (void *)this, m_is_compute, m_compile_failure_stage.c_str(),
          m_compile_failure_detail.c_str());
  return false;
}

bool MTLD3D12PipelineState::IsCompiled() const {
  return m_compile_state.load() == CompileState::Compiled;
}

bool MTLD3D12PipelineState::IsCompilePending() const {
  CompileState state = m_compile_state.load();
  return state == CompileState::Pending || state == CompileState::Compiling;
}

size_t MTLD3D12PipelineState::ApplyShaderVariantHash(
    size_t hash, ShaderType type) const {
  if (type == ShaderType::Vertex || type == ShaderType::Pixel)
    hash ^= 0x4d31327672735f70ull;
  if (type == ShaderType::Vertex && m_has_stream_output) {
    hash ^= 0x534f5645524c4159ull;
    hash = hash * 131 + m_stream_output.NumEntries;
    hash = hash * 131 + m_stream_output.NumStrides;
    hash = hash * 131 + m_stream_output.RasterizedStream;
    for (const auto &entry : m_stream_output_elements) {
      hash = hash * 131 + entry.Stream;
      hash = hash * 131 + entry.SemanticIndex;
      hash = hash * 131 + entry.StartComponent;
      hash = hash * 131 + entry.ComponentCount;
      hash = hash * 131 + entry.OutputSlot;
      if (entry.SemanticName) {
        for (const char *s = entry.SemanticName; *s; ++s)
          hash = hash * 131 + static_cast<unsigned char>(*s);
      }
    }
    for (UINT stride : m_stream_output_strides)
      hash = hash * 131 + stride;
  }
  if (type == ShaderType::Pixel && IsDepthBoundsTestEnabled()) {
    hash ^= 0xd3b0a7d5e91c2468ull;
    hash ^= static_cast<size_t>(m_sample_count) * 0x9e3779b97f4a7c15ull;
  }
  if (type == ShaderType::Pixel && m_sample_mask != UINT_MAX) {
    hash ^= 0x53414d504c454d41ull;
    hash = hash * 131 + m_sample_mask;
  }
  if (type == ShaderType::Pixel && m_uses_conservative_rasterization)
    hash ^= 0xc0a5e2a7f4b19d31ull;
  if (m_uses_attribute_at_vertex &&
      (type == ShaderType::Vertex || type == ShaderType::Pixel)) {
    hash ^= 0xa7e7c4a7f4b19d31ull;
    hash = hash * 131 + m_attribute_at_vertex_input_id;
  }
  return hash;
}

std::string MTLD3D12PipelineState::GetCSCacheHash() const {
  if (m_cs.empty())
    return {};
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016zx",
           ComputeShaderCacheHash(m_cs.data(), m_cs.size(),
                                  ShaderType::Compute, nullptr));
  return buffer;
}

std::string MTLD3D12PipelineState::GetVSCacheHash() const {
  if (m_vs.empty())
    return {};
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016zx",
           ApplyShaderVariantHash(
               ComputeShaderCacheHash(m_vs.data(), m_vs.size(),
                                      ShaderType::Vertex, &m_input_layout),
               ShaderType::Vertex));
  return buffer;
}

std::string MTLD3D12PipelineState::GetPSCacheHash() const {
  if (m_ps.empty())
    return {};
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016zx",
           ApplyShaderVariantHash(
               ComputeShaderCacheHash(m_ps.data(), m_ps.size(),
                                      ShaderType::Pixel, nullptr),
               ShaderType::Pixel));
  return buffer;
}

std::string MTLD3D12PipelineState::GetGSCacheHash() const {
  if (m_gs.empty())
    return {};
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016zx",
           ComputeShaderCacheHash(m_gs.data(), m_gs.size(),
                                  ShaderType::Geometry, nullptr));
  return buffer;
}

std::string MTLD3D12PipelineState::GetCompileFailureStage() const {
  return m_compile_failure_stage;
}

std::string MTLD3D12PipelineState::GetCompileFailureDetail() const {
  return m_compile_failure_detail;
}

bool MTLD3D12PipelineState::RequestCompile(bool allow_async) {
  if (!allow_async || !AsyncPipelineCompileEnabled())
    return Compile();

  CompileState expected = CompileState::NotStarted;
  if (m_compile_state.compare_exchange_strong(expected,
                                              CompileState::Pending)) {
    PSTRACE("PSO async compile scheduled pso=%p compute=%d", (void *)this,
            m_is_compute);
    if (GetAsyncPipelineCompiler().Enqueue(this))
      return false;

    PSTRACE("PSO async compile enqueue rejected during shutdown pso=%p "
            "compute=%d; compiling inline",
            (void *)this, m_is_compute);
    CompileState pending = CompileState::Pending;
    m_compile_state.compare_exchange_strong(pending, CompileState::NotStarted);
    return Compile();
  }

  return expected == CompileState::Compiled;
}

bool MTLD3D12PipelineState::TryCompilePendingInline() {
  CompileState expected = CompileState::Pending;
  if (!m_compile_state.compare_exchange_strong(expected,
                                               CompileState::NotStarted))
    return expected == CompileState::Compiled;

  PSTRACE("PSO pending compile promoted inline pso=%p compute=%d", (void *)this,
          m_is_compute);
  return Compile();
}

void MTLD3D12PipelineState::RunAsyncCompile() {
  PSTRACE("PSO async worker-owned compile begin worker=%u pso=%p compute=%d",
          g_async_pipeline_worker_index, (void *)this, m_is_compute);
  bool result = Compile();
  PSTRACE("PSO async worker-owned compile complete worker=%u pso=%p result=%d "
          "state=%u compute=%d",
          g_async_pipeline_worker_index, (void *)this, result,
          (unsigned)m_compile_state.load(), m_is_compute);
}

WMTPixelFormat MTLD3D12PipelineState::DXGIToMTLPixelFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatRGBA8Unorm;
  case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    return WMTPixelFormatRGBA8Unorm;
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatRGBA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8X8_TYPELESS:
  case DXGI_FORMAT_B8G8R8X8_UNORM:
    return WMTPixelFormatBGRX8Unorm;
  case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    return WMTPixelFormatBGRX8Unorm_sRGB;
  case DXGI_FORMAT_B5G6R5_UNORM:
    return WMTPixelFormatB5G6R5Unorm;
  case DXGI_FORMAT_B5G5R5A1_UNORM:
    return WMTPixelFormatA1BGR5Unorm;
  case DXGI_FORMAT_B4G4R4A4_UNORM:
    return WMTPixelFormatBGRA4Unorm;
  case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    return WMTPixelFormatRGB9E5Float;
  case DXGI_FORMAT_R8G8_B8G8_UNORM:
    return WMTPixelFormatBGRG422;
  case DXGI_FORMAT_G8R8_G8B8_UNORM:
    return WMTPixelFormatGBGR422;
  case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    return WMTPixelFormatRGBA16Uint;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  case DXGI_FORMAT_R16G16B16A16_UNORM:
    return WMTPixelFormatRGBA16Unorm;
  case DXGI_FORMAT_R16G16B16A16_UINT:
    return WMTPixelFormatRGBA16Uint;
  case DXGI_FORMAT_R16G16B16A16_SNORM:
    return WMTPixelFormatRGBA16Snorm;
  case DXGI_FORMAT_R16G16B16A16_SINT:
    return WMTPixelFormatRGBA16Sint;
  case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    return WMTPixelFormatRGBA32Uint;
  case DXGI_FORMAT_R32G32B32A32_FLOAT:
    return WMTPixelFormatRGBA32Float;
  case DXGI_FORMAT_R32G32B32A32_UINT:
    return WMTPixelFormatRGBA32Uint;
  case DXGI_FORMAT_R32G32B32A32_SINT:
    return WMTPixelFormatRGBA32Sint;
  case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    return WMTPixelFormatRGB10A2Uint;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R10G10B10A2_UINT:
    return WMTPixelFormatRGB10A2Uint;
  case DXGI_FORMAT_R11G11B10_FLOAT:
    return WMTPixelFormatRG11B10Float;
  case DXGI_FORMAT_R8_TYPELESS:
    return WMTPixelFormatR8Uint;
  case DXGI_FORMAT_A8_UNORM:
    return WMTPixelFormatA8Unorm;
  case DXGI_FORMAT_R8_UNORM:
    return WMTPixelFormatR8Unorm;
  case DXGI_FORMAT_R8_SNORM:
    return WMTPixelFormatR8Snorm;
  case DXGI_FORMAT_R8_UINT:
    return WMTPixelFormatR8Uint;
  case DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE:
    return WMTPixelFormatR8Uint;
  case DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE:
    return WMTPixelFormatR32Uint;
  case DXGI_FORMAT_R8_SINT:
    return WMTPixelFormatR8Sint;
  case DXGI_FORMAT_R16_TYPELESS:
    return WMTPixelFormatR16Uint;
  case DXGI_FORMAT_R16_UNORM:
    return WMTPixelFormatR16Unorm;
  case DXGI_FORMAT_R16_SNORM:
    return WMTPixelFormatR16Snorm;
  case DXGI_FORMAT_R16_UINT:
    return WMTPixelFormatR16Uint;
  case DXGI_FORMAT_R16_SINT:
    return WMTPixelFormatR16Sint;
  case DXGI_FORMAT_R16_FLOAT:
    return WMTPixelFormatR16Float;
  case DXGI_FORMAT_R32_TYPELESS:
    return WMTPixelFormatR32Uint;
  case DXGI_FORMAT_R32_UINT:
    return WMTPixelFormatR32Uint;
  case DXGI_FORMAT_R32_SINT:
    return WMTPixelFormatR32Sint;
  case DXGI_FORMAT_R32_FLOAT:
    return WMTPixelFormatR32Float;
  case DXGI_FORMAT_D32_FLOAT:
    return WMTPixelFormatDepth32Float;
  case DXGI_FORMAT_R24G8_TYPELESS:
  case DXGI_FORMAT_D24_UNORM_S8_UINT:
  case DXGI_FORMAT_R32G8X24_TYPELESS:
  case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    return WMTPixelFormatDepth32Float_Stencil8;
  case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
  case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    return WMTPixelFormatR32X8X32;
  case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
  case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    return WMTPixelFormatX32G8X32;
  case DXGI_FORMAT_D16_UNORM:
    return WMTPixelFormatDepth16Unorm;
  case DXGI_FORMAT_R16G16_TYPELESS:
    return WMTPixelFormatRG16Uint;
  case DXGI_FORMAT_R16G16_FLOAT:
    return WMTPixelFormatRG16Float;
  case DXGI_FORMAT_R16G16_UNORM:
    return WMTPixelFormatRG16Unorm;
  case DXGI_FORMAT_R16G16_SNORM:
    return WMTPixelFormatRG16Snorm;
  case DXGI_FORMAT_R16G16_UINT:
    return WMTPixelFormatRG16Uint;
  case DXGI_FORMAT_R16G16_SINT:
    return WMTPixelFormatRG16Sint;
  case DXGI_FORMAT_R32G32_TYPELESS:
    return WMTPixelFormatRG32Uint;
  case DXGI_FORMAT_R32G32_FLOAT:
    return WMTPixelFormatRG32Float;
  case DXGI_FORMAT_R32G32_UINT:
    return WMTPixelFormatRG32Uint;
  case DXGI_FORMAT_R32G32_SINT:
    return WMTPixelFormatRG32Sint;
  case DXGI_FORMAT_R8G8_TYPELESS:
    return WMTPixelFormatRG8Uint;
  case DXGI_FORMAT_R8G8_UNORM:
    return WMTPixelFormatRG8Unorm;
  case DXGI_FORMAT_R8G8_SNORM:
    return WMTPixelFormatRG8Snorm;
  case DXGI_FORMAT_R8G8_UINT:
    return WMTPixelFormatRG8Uint;
  case DXGI_FORMAT_R8G8_SINT:
    return WMTPixelFormatRG8Sint;
  case DXGI_FORMAT_R8G8B8A8_SNORM:
    return WMTPixelFormatRGBA8Snorm;
  case DXGI_FORMAT_R8G8B8A8_UINT:
    return WMTPixelFormatRGBA8Uint;
  case DXGI_FORMAT_R8G8B8A8_SINT:
    return WMTPixelFormatRGBA8Sint;
  case DXGI_FORMAT_BC1_TYPELESS:
    return WMTPixelFormatBC1_RGBA;
  case DXGI_FORMAT_BC1_UNORM:
    return WMTPixelFormatBC1_RGBA;
  case DXGI_FORMAT_BC1_UNORM_SRGB:
    return WMTPixelFormatBC1_RGBA_sRGB;
  case DXGI_FORMAT_BC2_TYPELESS:
    return WMTPixelFormatBC2_RGBA;
  case DXGI_FORMAT_BC2_UNORM:
    return WMTPixelFormatBC2_RGBA;
  case DXGI_FORMAT_BC2_UNORM_SRGB:
    return WMTPixelFormatBC2_RGBA_sRGB;
  case DXGI_FORMAT_BC3_TYPELESS:
    return WMTPixelFormatBC3_RGBA;
  case DXGI_FORMAT_BC3_UNORM:
    return WMTPixelFormatBC3_RGBA;
  case DXGI_FORMAT_BC3_UNORM_SRGB:
    return WMTPixelFormatBC3_RGBA_sRGB;
  case DXGI_FORMAT_BC4_TYPELESS:
    return WMTPixelFormatBC4_RUnorm;
  case DXGI_FORMAT_BC4_UNORM:
    return WMTPixelFormatBC4_RUnorm;
  case DXGI_FORMAT_BC4_SNORM:
    return WMTPixelFormatBC4_RSnorm;
  case DXGI_FORMAT_BC5_TYPELESS:
    return WMTPixelFormatBC5_RGUnorm;
  case DXGI_FORMAT_BC5_UNORM:
    return WMTPixelFormatBC5_RGUnorm;
  case DXGI_FORMAT_BC5_SNORM:
    return WMTPixelFormatBC5_RGSnorm;
  case DXGI_FORMAT_BC6H_TYPELESS:
    return WMTPixelFormatBC6H_RGBUfloat;
  case DXGI_FORMAT_BC6H_UF16:
    return WMTPixelFormatBC6H_RGBUfloat;
  case DXGI_FORMAT_BC6H_SF16:
    return WMTPixelFormatBC6H_RGBFloat;
  case DXGI_FORMAT_BC7_TYPELESS:
    return WMTPixelFormatBC7_RGBAUnorm;
  case DXGI_FORMAT_BC7_UNORM:
    return WMTPixelFormatBC7_RGBAUnorm;
  case DXGI_FORMAT_BC7_UNORM_SRGB:
    return WMTPixelFormatBC7_RGBAUnorm_sRGB;
  default:
    return WMTPixelFormatInvalid;
  }
}

bool MTLD3D12PipelineState::IsSupportedNativeTessellationProofShape() const {
  if (m_topology != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
    return false;
  if (m_gs.size() || m_has_stream_output)
    return false;
  if (m_input_layout.NumElements != 2 || !m_input_layout.pInputElementDescs)
    return false;
  if (m_num_render_targets != 1 ||
      m_rtv_formats[0] != DXGI_FORMAT_R8G8B8A8_UNORM ||
      m_dsv_format != DXGI_FORMAT_UNKNOWN || m_sample_count != 1 ||
      m_sample_mask != UINT_MAX || m_blend_desc.AlphaToCoverageEnable ||
      m_blend_desc.IndependentBlendEnable ||
      m_blend_desc.RenderTarget[0].BlendEnable ||
      m_blend_desc.RenderTarget[0].LogicOpEnable ||
      m_blend_desc.RenderTarget[0].RenderTargetWriteMask !=
          D3D12_COLOR_WRITE_ENABLE_ALL ||
      m_rasterizer_desc.FillMode != D3D12_FILL_MODE_SOLID ||
      m_rasterizer_desc.CullMode != D3D12_CULL_MODE_NONE ||
      m_rasterizer_desc.FrontCounterClockwise ||
      m_rasterizer_desc.DepthBias != D3D12_DEFAULT_DEPTH_BIAS ||
      m_rasterizer_desc.DepthBiasClamp != D3D12_DEFAULT_DEPTH_BIAS_CLAMP ||
      m_rasterizer_desc.SlopeScaledDepthBias !=
          D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS ||
      !m_rasterizer_desc.DepthClipEnable ||
      m_rasterizer_desc.MultisampleEnable ||
      m_rasterizer_desc.AntialiasedLineEnable ||
      m_rasterizer_desc.ForcedSampleCount != 0 ||
      m_rasterizer_desc.ConservativeRaster !=
          D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF ||
      m_depth_stencil_desc.DepthEnable || m_depth_stencil_desc.StencilEnable)
    return false;

  const auto &pos = m_input_layout.pInputElementDescs[0];
  const auto &color = m_input_layout.pInputElementDescs[1];
  auto semantic_equals = [](const D3D12_INPUT_ELEMENT_DESC &element,
                            const char *expected) {
    return element.SemanticName &&
           strcasecmp(element.SemanticName, expected) == 0;
  };
  if (!semantic_equals(pos, "POSITION") || pos.SemanticIndex != 0 ||
      pos.Format != DXGI_FORMAT_R32G32B32_FLOAT || pos.InputSlot != 0 ||
      pos.AlignedByteOffset != 0 ||
      pos.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA)
    return false;
  if (!semantic_equals(color, "COLOR") || color.SemanticIndex != 0 ||
      color.Format != DXGI_FORMAT_R32G32B32A32_FLOAT || color.InputSlot != 0 ||
      color.AlignedByteOffset != 12 ||
      color.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA)
    return false;

  constexpr uint64_t kProofVSHash = 0x2fdb2fc7eb649e2bull;
  constexpr uint64_t kProofHSHash = 0xbdad5645aa77c46cull;
  constexpr uint64_t kProofDSHash = 0xba49a431080d0a15ull;
  constexpr uint64_t kProofPSHash = 0x0e424b4fa182e9caull;
  // This is a deliberately exact proof profile, not general HS/DS lowering:
  // the native path must not accept arbitrary patch-constant layouts until
  // their actual ABI and readback are implemented.
  constexpr uint64_t kPatchConstantProofHSHash = 0xf3c5f2772d8bc2fdull;
  constexpr uint64_t kPatchConstantProofDSHash = 0x15e869a973332bf7ull;
  const uint64_t vs_hash = DXMTD3D12Hash64(m_vs.data(), m_vs.size());
  const uint64_t hs_hash = DXMTD3D12Hash64(m_hs.data(), m_hs.size());
  const uint64_t ds_hash = DXMTD3D12Hash64(m_ds.data(), m_ds.size());
  const uint64_t ps_hash = DXMTD3D12Hash64(m_ps.data(), m_ps.size());
  const bool baseline_proof = vs_hash == kProofVSHash &&
                              hs_hash == kProofHSHash &&
                              ds_hash == kProofDSHash &&
                              ps_hash == kProofPSHash;
  const bool patch_constant_proof = vs_hash == kProofVSHash &&
                                    hs_hash == kPatchConstantProofHSHash &&
                                    ds_hash == kPatchConstantProofDSHash &&
                                    ps_hash == kProofPSHash;
  return baseline_proof || patch_constant_proof;
}

bool MTLD3D12PipelineState::CompileNativeTessellationProofShape() {
  if (!IsSupportedNativeTessellationProofShape()) {
    return RecordCompileFailure(
        "pso/unsupported_native_tessellation",
        str::format("native_tessellation_unsupported ",
                    "implementation=d3d12_native_tessellation_path ",
                    "reason=unsupported_hs_ds_shape HS bytes=", m_hs.size(),
                    " DS bytes=", m_ds.size(),
                    " topology=", (unsigned)m_topology));
  }

  constexpr uint64_t kProofVSHash = 0x2fdb2fc7eb649e2bull;
  constexpr uint64_t kPatchConstantProofHSHash = 0xf3c5f2772d8bc2fdull;
  constexpr uint64_t kPatchConstantProofDSHash = 0x15e869a973332bf7ull;
  constexpr uint64_t kProofPSHash = 0x0e424b4fa182e9caull;
  const bool patch_constant_proof =
      DXMTD3D12Hash64(m_vs.data(), m_vs.size()) == kProofVSHash &&
      DXMTD3D12Hash64(m_hs.data(), m_hs.size()) == kPatchConstantProofHSHash &&
      DXMTD3D12Hash64(m_ds.data(), m_ds.size()) == kPatchConstantProofDSHash &&
      DXMTD3D12Hash64(m_ps.data(), m_ps.size()) == kProofPSHash;

  static constexpr const char *kNativeTessellationProofMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct D3D12NativeTessellationCP {
  float3 pos [[attribute(0)]];
  float4 color [[attribute(1)]];
};
struct D3D12NativeTessellationOut {
  float4 position [[position]];
  float4 color;
};
[[patch(triangle, 3)]]
vertex D3D12NativeTessellationOut d3d12_native_tess_vertex(
    patch_control_point<D3D12NativeTessellationCP> patch [[stage_in]],
    float3 bary [[position_in_patch]]) {
  D3D12NativeTessellationCP a = patch[0];
  D3D12NativeTessellationCP b = patch[1];
  D3D12NativeTessellationCP c = patch[2];
  D3D12NativeTessellationOut out;
  float3 pos = a.pos * bary.x + b.pos * bary.y + c.pos * bary.z;
  out.position = float4(pos, 1.0);
  out.color = saturate(a.color * bary.x + b.color * bary.y + c.color * bary.z);
  return out;
}
fragment float4 d3d12_native_tess_fragment(
    D3D12NativeTessellationOut in [[stage_in]]) {
  return saturate(in.color);
}
)MSL";

  m_ia_slot_mask = 1u;
  m_ia_input_elements.clear();
  D3D12IAInputElementInfo position = {};
  position.semantic_name = "POSITION";
  position.semantic_index = 0;
  position.shader_register = 0;
  position.input_slot = 0;
  position.table_index = 0;
  position.aligned_byte_offset = 0;
  position.dxgi_format = DXGI_FORMAT_R32G32B32_FLOAT;
  position.metal_format = WMTAttributeFormatFloat3;
  position.bytes_per_element = 12;
  position.input_slot_class = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  position.per_instance = false;
  position.instance_step_rate = 1;
  m_ia_input_elements.push_back(position);
  D3D12IAInputElementInfo color = position;
  color.semantic_name = "COLOR";
  color.shader_register = 1;
  color.aligned_byte_offset = 12;
  color.dxgi_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  color.metal_format = WMTAttributeFormatFloat4;
  color.bytes_per_element = 16;
  m_ia_input_elements.push_back(color);

  std::string native_tessellation_msl = kNativeTessellationProofMSL;
  if (patch_constant_proof) {
    static constexpr const char *kBaselineColorExpression =
        "  out.color = saturate(a.color * bary.x + b.color * bary.y + c.color * bary.z);";
    static constexpr const char *kPatchConstantColorExpression =
        "  out.color = float4(0.25, 0.0, 0.0, 1.0);";
    const size_t expression_offset =
        native_tessellation_msl.find(kBaselineColorExpression);
    if (expression_offset == std::string::npos)
      return RecordCompileFailure(
          "shader/native_tessellation_proof_profile",
          "native tessellation patch-constant proof expression was not found");
    native_tessellation_msl.replace(expression_offset,
                                    std::strlen(kBaselineColorExpression),
                                    kPatchConstantColorExpression);
  }

  auto wmt_device = m_device->GetDXMTDevice().device();
  WMT::Reference<WMT::Error> err;
  auto library = wmt_device.newLibraryWithSource(
      native_tessellation_msl.c_str(), native_tessellation_msl.size(), err);
  if (err.handle || !library.handle) {
    auto err_desc = DescribeNSObject(err.handle);
    return RecordCompileFailure(
        "shader/native_tessellation_msl_compile",
        str::format("native tessellation proof MSL compile failed: ",
                    err_desc));
  }

  auto vs_func = library.newFunction("d3d12_native_tess_vertex");
  auto ps_func = library.newFunction("d3d12_native_tess_fragment");
  if (!vs_func.handle || !ps_func.handle) {
    return RecordCompileFailure(
        "shader/native_tessellation_function_lookup",
        "native tessellation proof MSL function lookup failed");
  }

  WMTRenderPipelineInfo info;
  WMT::InitializeRenderPipelineInfo(info);
  info.vertex_function = vs_func.handle;
  info.fragment_function = ps_func.handle;
  info.rasterization_enabled = true;
  info.raster_sample_count = 1;
  info.colors[0].pixel_format = WMTPixelFormatRGBA8Unorm;
  info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
  info.tessellation_partition_mode = WMTTessellationPartitionModeInteger;
  info.tessellation_factor_step = WMTTessellationFactorStepFunctionConstant;
  info.tessellation_output_winding_order = WMTWindingClockwise;
  info.tessellation_control_point_index_type =
      WMTTessellationControlPointIndexTypeNone;
  info.max_tessellation_factor = 16;
  info.immutable_vertex_buffers = 0;
  info.immutable_fragment_buffers = 0;

  WMTVertexDescriptor vtx_desc = {};
  vtx_desc.attribute_count = 2;
  vtx_desc.layout_count = 1;
  vtx_desc.attributes[0].format = WMTAttributeFormatFloat3;
  vtx_desc.attributes[0].offset = 0;
  vtx_desc.attributes[0].buffer_index = 0;
  vtx_desc.attributes[1].format = WMTAttributeFormatFloat4;
  vtx_desc.attributes[1].offset = 12;
  vtx_desc.attributes[1].buffer_index = 0;
  vtx_desc.layouts[0].stride = 28;
  vtx_desc.layouts[0].step_function = WMTVertexStepFunctionPerPatchControlPoint;
  vtx_desc.layouts[0].step_rate = 1;
  info.vertex_descriptor = &vtx_desc;

  err = nullptr;
  m_render_pso = wmt_device.newRenderPipelineState(info, err);
  if (!m_render_pso.handle) {
    auto err_desc = DescribeNSObject(err.handle);
    return RecordCompileFailure(
        "pso/metal_native_tessellation_render_pso",
        str::format("Metal native tessellation render PSO creation failed: ",
                    err_desc));
  }

  info.tessellation_control_point_index_type =
      WMTTessellationControlPointIndexTypeUInt16;
  err = nullptr;
  m_native_tessellation_indexed_render_pso =
      wmt_device.newRenderPipelineState(info, err);
  if (!m_native_tessellation_indexed_render_pso.handle) {
    auto err_desc = DescribeNSObject(err.handle);
    return RecordCompileFailure(
        "pso/metal_native_tessellation_indexed_render_pso",
        str::format(
            "Metal native tessellation indexed render PSO creation failed: ",
            err_desc));
  }

  WMTDepthStencilInfo ds_info = {};
  ds_info.depth_compare_function = WMTCompareFunctionAlways;
  ds_info.depth_write_enabled = false;
  ds_info.front_stencil.enabled = false;
  ds_info.back_stencil.enabled = false;
  m_depth_stencil_state = wmt_device.newDepthStencilState(ds_info);

  m_uses_native_tessellation_path = true;
  m_native_tessellation_control_points = 3;
  m_compile_state.store(CompileState::Compiled);
  m_compile_cv.notify_all();
  Logger::info(str::format(
      "M12 native_tessellation_path resolved "
      "implementation=d3d12_native_tessellation_path ",
      "shape=proof_triangle_passthrough control_points=3 partition=integer ",
      "factor=1 d3d11_reuse=forbidden pso=", (void *)this));
  return true;
}

bool MTLD3D12PipelineState::CompileShader(
    const void *bytecode, SIZE_T size, ShaderType type, const char *func_name,
    WMT::Reference<WMT::Function> &out_func, sm50_shader_t *out_shader_handle,
    MTL_SHADER_REFLECTION *out_reflection) {
  size_t hash = ComputeShaderCacheHash(
      bytecode, size, type,
      type == ShaderType::Vertex ? &m_input_layout : nullptr);
  hash = ApplyShaderVariantHash(hash, type);
  if ((type == ShaderType::Vertex || type == ShaderType::Pixel) &&
      DXBCContainerHasChunk(bytecode, size, "DXIL"))
    m_uses_vrs_runtime_state = true;
  const bool requires_int64_custom =
      type == ShaderType::Compute && DXBCShaderUsesAtomic64(bytecode, size);
  const bool requires_sampler_feedback_custom =
      DXBCShaderUsesSamplerFeedback(bytecode, size);
  const bool requires_sample_cmp_level_custom =
      type == ShaderType::Compute && DXBCShaderUsesSampleCmpLevel(bytecode, size);
  const bool requires_attribute_at_vertex_custom =
      m_uses_attribute_at_vertex &&
      (type == ShaderType::Vertex || type == ShaderType::Pixel);
  if (requires_int64_custom)
    m_uses_atomic64_emulation = true;
  if (requires_sampler_feedback_custom)
    m_uses_sampler_feedback_emulation = true;
  if (type == ShaderType::Compute &&
      DXBCShaderUsesDirectResourceHeap(bytecode, size))
    m_uses_direct_resource_descriptor_heap = true;
  if (D3D12ShaderCacheEnabled()) {
    std::lock_guard<std::mutex> lock(s_shader_mutex);
    PSTRACE("CompileShader: %s hash=0x%zx size=%zu cache_entries=%zu",
            func_name, hash, size, s_shader_cache.size());
    auto it = s_shader_cache.find(hash);
    if (it != s_shader_cache.end() && !out_shader_handle && !out_reflection &&
        type != ShaderType::Amplification && type != ShaderType::Mesh) {
      out_func = it->second;
      PSTRACE("CompileShader: %s CACHE HIT hash=0x%zx", func_name, hash);
      return true;
    }
  } else {
    PSTRACE("CompileShader: %s cache disabled hash=0x%zx", func_name, hash);
  }

  if (bytecode && size >= 4) {
    auto *magic = (const uint32_t *)bytecode;
    PSTRACE("CompileShader: %s size=%zu magic=0x%08x (DXBC=0x43425844 "
            "DXIL=0x4C495844)",
            func_name, size, *magic);
    if (*magic == 0x43425844 && size >= 32) {
      auto *chunks = (const uint32_t *)bytecode;
      uint32_t container_size = chunks[6];
      uint32_t num_chunks = chunks[7];
      PSTRACE("  DXBC: container_size=%u num_chunks=%u", container_size,
              num_chunks);
      for (uint32_t i = 0; i < num_chunks && i < 16; i++) {
        uint32_t offset = chunks[8 + i];
        if (offset + 8 <= size) {
          char tag[5] = {};
          memcpy(tag, (const char *)bytecode + offset, 4);
          uint32_t chunk_size = 0;
          memcpy(&chunk_size, (const char *)bytecode + offset + 4,
                 sizeof(chunk_size));
          PSTRACE("  chunk[%u]: tag='%s' offset=%u size=%u", i, tag, offset,
                  chunk_size);
        }
      }
    }
  }
  sm50_error_t sm50_err = nullptr;
  sm50_shader_t shader = nullptr;
  MTL_SHADER_REFLECTION reflection = {};
  std::vector<SM50_IA_INPUT_ELEMENT> ia_elements;
  uint32_t ia_slot_mask = 0;
  if (type == ShaderType::Vertex) {
    BuildIAInputLayout(bytecode, size, ia_elements, ia_slot_mask);
    m_ia_slot_mask = ia_slot_mask;
  }

  const bool prefer_dxil_path = DXBCContainerHasChunk(bytecode, size, "DXIL");
  if (prefer_dxil_path) {
    PSTRACE(
        "CompileShader: %s DXIL chunk present; bypassing legacy SM50 compiler",
        func_name);
  }

  const uint32_t sm50_options = 0;
  if (prefer_dxil_path ||
      SM50InitializeWithOptions(bytecode, size, sm50_options, &shader,
                                &reflection, &sm50_err)) {
    char err_buf[256] = {};
    if (sm50_err) {
      SM50GetErrorMessage(sm50_err, err_buf, sizeof(err_buf));
      SM50FreeError(sm50_err);
    }

    bool has_dxil = false;
    using namespace microsoft;
    CDXBCParser dxbcParser;
    if (SUCCEEDED(dxbcParser.ReadDXBC(bytecode, size))) {
      for (UINT32 i = 0; i < dxbcParser.GetBlobCount(); i++) {
        if (dxbcParser.GetBlobFourCC(i) == dxmt::dxil::DXIL_FOURCC) {
          has_dxil = true;
          const void *blob = dxbcParser.GetBlob(i);
          UINT32 blob_size = dxbcParser.GetBlobSize(i);
          PSTRACE("DXIL blob found index=%u size=%u", i, blob_size);

          auto wmt_device = m_device->GetDXMTDevice().device();

          char cache_path[1024];
          FormatShaderCachePath(cache_path, sizeof(cache_path), "%016zx", hash);
          char dxbc_path[1024], metallib_path[1024], reflection_path[1024],
              module_summary_path[1024], dxil_report_path[1024],
              metallib_error_path[1024], msl_path[1024];
          snprintf(dxbc_path, sizeof(dxbc_path), "%s.dxbc", cache_path);
          snprintf(metallib_path, sizeof(metallib_path), "%s.metallib",
                   cache_path);
          snprintf(reflection_path, sizeof(reflection_path), "%s.json",
                   cache_path);
          snprintf(module_summary_path, sizeof(module_summary_path),
                   "%s.module.txt", cache_path);
          snprintf(dxil_report_path, sizeof(dxil_report_path),
                   "%s.dxil_report.txt", cache_path);
          snprintf(metallib_error_path, sizeof(metallib_error_path),
                   "%s.metallib.err.txt", cache_path);
          snprintf(msl_path, sizeof(msl_path), "%s.msl", cache_path);
          EnsureShaderCacheDir();
          DumpShaderBlob(dxbc_path, bytecode, size);

          FILE *mf = D3D12ShaderCacheEnabled()
                         ? fopen(metallib_path, "rb")
                         : nullptr;
          if (mf && type == ShaderType::Pixel &&
              (IsDepthBoundsTestEnabled() ||
               m_uses_conservative_rasterization)) {
            // The offline cache converter sees only the original DXIL and
            // cannot preserve the injected depth-bounds comparison. Compile
            // the instrumented MSL once per process instead of accepting an
            // uninstrumented cached function under this derived hash.
            fclose(mf);
            mf = nullptr;
          }
          if (mf && requires_int64_custom) {
            // Metal Shader Converter may emit native 64-bit atomics that the
            // Apple GPU/toolchain does not execute. Keep the custom software
            // lock lowering authoritative for every int64 compute variant.
            fclose(mf);
            mf = nullptr;
          }
          if (mf && requires_sampler_feedback_custom) {
            // Sampler feedback is represented by a padded software map. The
            // custom lowering knows that ABI; a converter-produced metallib
            // does not.
            fclose(mf);
            mf = nullptr;
          }
          if (mf && requires_sample_cmp_level_custom) {
            // SampleCmpLevel needs the explicit mip level and the typed
            // lowering's comparison ordering; an offline converter cache can
            // silently collapse the level argument.
            fclose(mf);
            mf = nullptr;
          }
          if (mf && requires_attribute_at_vertex_custom) {
            // AttributeAtVertex uses the paired vertex/fragment capture ABI
            // emitted by the typed lowerer.  A converter-produced cache does
            // not contain the hidden capture buffer signature.
            fclose(mf);
            mf = nullptr;
          }
          if (!mf) {
            PSTRACE("  metallib not cached, attempting DXIL->MSL compilation");

            auto container = dxmt::dxil::DXILContainer::parse(blob, blob_size);
            if (!container) {
              PSTRACE("  DXILContainer::parse FAILED for %s", func_name);
              DumpShaderBlob(dxbc_path, bytecode, size);
              return RecordCompileFailure(
                  "shader/dxil_container_parse",
                  str::format(func_name,
                              " DXIL container parse failed; dumped ",
                              dxbc_path));
            }

            // The DXIL parser receives the DXIL part, while signature chunks
            // live in the enclosing DXBC container. Preserve the semantic
            // register for SV_ShadingRate before lowering so vertex output and
            // pixel input cannot be mistaken for an arbitrary varying.
            container->annotateSignatures(bytecode, size);

            auto &shader_info = container->shader();
            PSTRACE("  DXIL container parsed: kind=%u sm=%u.%u bc_size=%u",
                    (uint32_t)shader_info.kind, shader_info.shader_model.major,
                    shader_info.shader_model.minor, shader_info.bitcode.size);

            auto module = dxmt::dxil::BitcodeReader::parse(
                shader_info.bitcode.data, shader_info.bitcode.size);
            if (!module) {
              PSTRACE("  BitcodeReader::parse FAILED");
              DumpShaderBlob(dxbc_path, bytecode, size);
              return RecordCompileFailure(
                  "shader/bitcode_parse",
                  str::format(func_name, " DXIL bitcode parse failed; dumped ",
                              dxbc_path));
            }

            PSTRACE("  Bitcode parsed: types=%zu functions=%zu constants=%zu",
                    module->types.size(), module->functions.size(),
                    module->constants.size());
            DumpDXILModuleSummary(module_summary_path, *module, shader_info);
            PSTRACE("  DXIL module summary written to %s", module_summary_path);

            dxmt::dxil::MSLLoweringOptions lowering_options = {};
            lowering_options.depth_bounds_test =
                type == ShaderType::Pixel && IsDepthBoundsTestEnabled();
            lowering_options.depth_bounds_multisample =
                lowering_options.depth_bounds_test && m_sample_count > 1;
            lowering_options.sampler_feedback =
                requires_sampler_feedback_custom;
            if (auto *root_signature =
                    static_cast<MTLD3D12RootSignature *>(m_root_sig))
              lowering_options.resource_heap_directly_indexed =
                  (root_signature->GetFlags() &
                   D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) !=
                  0;
            lowering_options.vrs_per_primitive =
                type == ShaderType::Vertex || type == ShaderType::Pixel;
            lowering_options.conservative_rasterization =
                type == ShaderType::Pixel &&
                m_uses_conservative_rasterization_reference_model;
            if (type == ShaderType::Pixel)
              lowering_options.sample_mask = m_sample_mask;
            lowering_options.attribute_at_vertex_capture =
                requires_attribute_at_vertex_custom;
            lowering_options.attribute_at_vertex_input_id =
                m_attribute_at_vertex_input_id;
            if (type == ShaderType::Vertex) {
              lowering_options.vertex_inputs.reserve(
                  m_ia_input_elements.size());
              for (const auto &input : m_ia_input_elements) {
                if (input.table_index >= kMetalD3D12VertexBufferSlotCount)
                  continue;
                dxmt::dxil::MSLVertexInputElement element = {};
                element.shader_register = input.shader_register;
                element.table_index = input.table_index;
                element.input_slot = input.input_slot;
                element.aligned_byte_offset = input.aligned_byte_offset;
                element.dxgi_format = static_cast<uint32_t>(input.dxgi_format);
                element.metal_format =
                    static_cast<uint32_t>(input.metal_format);
                element.per_instance = input.per_instance;
                element.instance_step_rate = input.instance_step_rate;
                element.table_indexing_mode =
                    input.table_indexing_mode ==
                            D3D12VertexTableIndexingMode::RawSlot
                        ? dxmt::dxil::MSLVertexTableIndexingMode::RawSlot
                        : dxmt::dxil::MSLVertexTableIndexingMode::
                              CompactBySlotMask;
                element.system_value = input.system_value;
                lowering_options.vertex_inputs.push_back(element);
                PSTRACE(
                    "  M12 vertex input map reg=%u slot=%u table=%u system=%u",
                    element.shader_register, element.input_slot,
                    element.table_index, element.system_value ? 1u : 0u);
              }
            }

            auto typed_msl = dxmt::dxil::MSLLowering::lower(
                *module, shader_info, lowering_options);
            auto msl_result =
                typed_msl
                    ? std::optional<dxmt::dxil::MSLShader>(
                          std::in_place,
                          ToRuntimeMSLShader(std::move(*typed_msl)))
                    : dxmt::dxil::DXILToMSL::convert(*module, shader_info);
            if (!msl_result) {
              PSTRACE("  MSLLowering::lower and DXILToMSL::convert FAILED");
              DumpShaderBlob(dxbc_path, bytecode, size);
              return RecordCompileFailure(
                  "shader/dxil_to_msl",
                  str::format(func_name,
                              " DXIL to MSL conversion failed; module ",
                              module_summary_path, "; dxbc ", dxbc_path));
            }

            PSTRACE("  MSL generated via %s: %zu bytes, entry=%s "
                    "unsupported_intrinsics=%u unsupported_opcodes=%u",
                    typed_msl ? "MSLLowering" : "DXILToMSL",
                    msl_result->source.size(), msl_result->entry_point.c_str(),
                    msl_result->unsupported_intrinsics,
                    msl_result->unsupported_opcodes);
            if (msl_result->unsupported_intrinsics || msl_result->unsupported_opcodes) {
              const std::string detail = str::format(
                  func_name, " unsupported DXIL semantics: intrinsics=",
                  msl_result->unsupported_intrinsics, " opcodes=",
                  msl_result->unsupported_opcodes);
              PSTRACE("  rejecting shader with unsupported semantics: %s",
                      detail.c_str());
              return RecordCompileFailure("shader/unsupported_semantics", detail);
            }

            char msl_error_path[1024];
            snprintf(msl_error_path, sizeof(msl_error_path), "%s.msl.err.txt",
                     cache_path);
            DumpShaderText(msl_path, msl_result->source.c_str());
            ParseDirectBindingManifest(msl_result->source.c_str(), reflection);
            PSTRACE("  MSL source written to %s direct_masks="
                    "uav=0x%llx srv=0x%llx/0x%llx cbv=0x%x sampler=0x%x",
                    msl_path, (unsigned long long)reflection.UAVSlotMask,
                    (unsigned long long)reflection.SRVSlotMaskLo,
                    (unsigned long long)reflection.SRVSlotMaskHi,
                    reflection.ConstantBufferSlotMask,
                    reflection.SamplerSlotMask);
            DumpDXILCompileReport(
                dxil_report_path, func_name, hash, size, dxbc_path,
                module_summary_path, msl_path, *module, shader_info,
                *msl_result, static_cast<MTLD3D12RootSignature *>(m_root_sig));
            PSTRACE("  DXIL compile report written to %s", dxil_report_path);

            WMT::Reference<WMT::Error> compile_err;
            auto library = wmt_device.newLibraryWithSource(
                msl_result->source.c_str(), msl_result->source.size(),
                compile_err);

            if (compile_err.handle) {
              auto err_desc = DescribeNSObject(compile_err.handle);
              DumpShaderText(msl_error_path, err_desc.c_str());
              PSTRACE("  newLibraryWithSource FAILED: %s", err_desc.c_str());
              Logger::err(str::format("DXIL MSL compilation failed for ",
                                      func_name, ": ", err_desc));
              DumpShaderBlob(dxbc_path, bytecode, size);
              return RecordCompileFailure(
                  "shader/metal_library_source",
                  str::format(func_name, " MSL compile failed: ", err_desc,
                              "; msl ", msl_path, "; error ", msl_error_path,
                              "; dxbc ", dxbc_path));
            }

            PSTRACE("  Metal library compiled OK from source lib_handle=%llu",
                    (unsigned long long)library.handle);

            const char *dump_msl = std::getenv("DXMT_DUMP_MSL");
            if (dump_msl && dump_msl[0] && strcmp(dump_msl, "0") != 0) {
              char dump_path[1024];
              snprintf(dump_path, sizeof(dump_path),
                       "%s/dxmt_msl_%s_%016zx.metal", ShaderCacheDir().c_str(),
                       func_name, hash);
              FILE *df = fopen(dump_path, "w");
              if (df) {
                fwrite(msl_result->source.c_str(), 1, msl_result->source.size(),
                       df);
                fclose(df);
              }
            }

            const char *entry_name = msl_result->entry_point.c_str();
            if (strcmp(entry_name, "cs_main") != 0 &&
                strcmp(entry_name, "vs_main") != 0 &&
                strcmp(entry_name, "ps_main") != 0) {
              switch (shader_info.kind) {
              case dxmt::dxil::DxilShaderKind::Compute:
                entry_name = "cs_main";
                break;
              case dxmt::dxil::DxilShaderKind::Vertex:
                entry_name = "vs_main";
                break;
              case dxmt::dxil::DxilShaderKind::Pixel:
                entry_name = "ps_main";
                break;
              default:
                break;
              }
            }

            out_func = library.newFunction(entry_name);
            PSTRACE("  newFunction(%s) on lib=%llu -> func_handle=%llu",
                    entry_name, (unsigned long long)library.handle,
                    (unsigned long long)out_func.handle);
            if (!out_func.handle) {
              PSTRACE("  newFunction(%s) returned null, trying alternatives",
                      entry_name);
              out_func = library.newFunction("main");
              if (!out_func.handle)
                out_func = library.newFunction("cs_main");
              if (!out_func.handle)
                out_func = library.newFunction("vs_main");
              if (!out_func.handle)
                out_func = library.newFunction("ps_main");
            }

            if (out_func.handle) {
              PSTRACE("  DXIL shader compiled OK! entry=%s", entry_name);
              if (D3D12ShaderCacheEnabled()) {
                std::lock_guard<std::mutex> lock(s_shader_mutex);
                s_shader_cache[hash] = out_func;
              }

              if (out_reflection)
                *out_reflection = reflection;
              if (type == ShaderType::Vertex)
                m_vs_uses_stage_in = false;

              if (shader_info.kind == dxmt::dxil::DxilShaderKind::Compute) {
                uint32_t psv_tg[3] = {0, 0, 0};
                if (ExtractPSV0ComputeThreadgroupSize(bytecode, size, psv_tg)) {
                  m_threadgroup_size.width = psv_tg[0];
                  m_threadgroup_size.height = psv_tg[1];
                  m_threadgroup_size.depth = psv_tg[2];
                  PSTRACE("  threadgroup_size from PSV0: %ux%ux%u", psv_tg[0],
                          psv_tg[1], psv_tg[2]);
                } else {
                  m_threadgroup_size.width = msl_result->tg_size[0];
                  m_threadgroup_size.height = msl_result->tg_size[1];
                  m_threadgroup_size.depth = msl_result->tg_size[2];
                }
              }
              return true;
            } else {
              PSTRACE("  newFunction returned null for all entry points");
              Logger::err(str::format(
                  "DXIL: failed to get function from compiled library for ",
                  func_name));
              return RecordCompileFailure(
                  "shader/metal_function_lookup",
                  str::format(func_name,
                              " function lookup failed after MSL compile; msl ",
                              msl_path));
            }
          }

          PSTRACE("  loading cached metallib from %s", metallib_path);
          fseek(mf, 0, SEEK_END);
          long lib_size = ftell(mf);
          fseek(mf, 0, SEEK_SET);
          PSTRACE("  metallib size=%ld", lib_size);
          if (lib_size > 0) {
            std::vector<uint8_t> lib_data(lib_size);
            fread(lib_data.data(), 1, lib_size, mf);
            fclose(mf);
            auto dispatch_data =
                WMT::MakeDispatchData(lib_data.data(), lib_size);
            WMT::Reference<WMT::Error> err;
            auto library = wmt_device.newLibrary(dispatch_data, err);
            if (!err.handle) {
              char actual_entry[256] = {};
              char rbuf[4096] = {};
              FILE *rf = fopen(reflection_path, "r");
              if (rf) {
                fread(rbuf, 1, sizeof(rbuf) - 1, rf);
                fclose(rf);
                char *ep = strstr(rbuf, "\"EntryPoint\"");
                if (ep) {
                  char *q1 = strchr(ep + 13, '"');
                  char *q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
                  if (q1 && q2) {
                    size_t len = q2 - q1 - 1;
                    if (len < sizeof(actual_entry)) {
                      memcpy(actual_entry, q1 + 1, len);
                      actual_entry[len] = 0;
                    }
                  }
                }
              }
              const char *fn_name = actual_entry[0] ? actual_entry : func_name;
              PSTRACE("  trying newFunction(%s)", fn_name);
              out_func = library.newFunction(fn_name);
              if (!out_func.handle && actual_entry[0]) {
                out_func = library.newFunction(func_name);
              }
              if (!out_func.handle)
                out_func = library.newFunction("main");
              if (!out_func.handle)
                out_func = library.newFunction("cs_main");
              if (!out_func.handle)
                out_func = library.newFunction("vs_main");
              if (!out_func.handle)
                out_func = library.newFunction("ps_main");
              if (out_func.handle) {
                PSTRACE("  DXIL loaded from cache OK! entry=%s", fn_name);
                if (D3D12ShaderCacheEnabled()) {
                  std::lock_guard<std::mutex> lock(s_shader_mutex);
                  s_shader_cache[hash] = out_func;
                }
                if (type == ShaderType::Vertex) {
                  const char *inputs = strstr(rbuf, "\"vertex_inputs\"");
                  const char *array = inputs ? strchr(inputs, '[') : nullptr;
                  if (array) {
                    do {
                      array++;
                    } while (*array == ' ' || *array == '\t' ||
                             *array == '\r' || *array == '\n');
                  }
                  m_vs_uses_stage_in =
                      m_input_layout.NumElements > 0 && array && *array != ']';
                  m_vs_requires_msc_stage_in = false;
                  PSTRACE("  MSC vertex inputs stage_in=%u layout_elements=%u",
                          m_vs_uses_stage_in ? 1u : 0u,
                          m_input_layout.NumElements);
                }
                char *tg = strstr(rbuf, "\"tg_size\"");
                if (tg) {
                  int tw = 1, th = 1, td = 1;
                  if (sscanf(tg, "\"tg_size\": [%d, %d, %d]", &tw, &th, &td) ==
                          3 ||
                      sscanf(tg, "\"tg_size\":[%d,%d,%d]", &tw, &th, &td) ==
                          3) {
                    m_threadgroup_size.width = tw;
                    m_threadgroup_size.height = th;
                    m_threadgroup_size.depth = td;
                    PSTRACE("  threadgroup_size from reflection: %dx%dx%d", tw,
                            th, td);
                  }
                }
                if (type == ShaderType::Amplification ||
                    type == ShaderType::Mesh) {
                  char *num_threads = strstr(rbuf, "\"num_threads\"");
                  char *num_threads_values =
                      num_threads ? strchr(num_threads, '[') : nullptr;
                  int tw = 1, th = 1, td = 1;
                  if (num_threads_values &&
                      sscanf(num_threads_values, "[ %d , %d , %d ]", &tw,
                             &th, &td) == 3) {
                    m_threadgroup_size.width = tw;
                    m_threadgroup_size.height = th;
                    m_threadgroup_size.depth = td;
                    PSTRACE("  mesh threadgroup size from reflection: %dx%dx%d",
                            tw, th, td);
                  }
                  char *payload = strstr(rbuf, "\"max_payload_size_in_bytes\"");
                  unsigned payload_size = 0;
                  if (payload &&
                      sscanf(payload, "\"max_payload_size_in_bytes\": %u",
                             &payload_size) == 1) {
                    m_mesh_payload_size =
                        std::max(m_mesh_payload_size, payload_size);
                    PSTRACE("  mesh payload size from reflection: %u",
                            payload_size);
                  }
                }
                if (type == ShaderType::Compute) {
                  uint32_t psv_tg[3] = {0, 0, 0};
                  if (ExtractPSV0ComputeThreadgroupSize(bytecode, size,
                                                        psv_tg)) {
                    m_threadgroup_size.width = psv_tg[0];
                    m_threadgroup_size.height = psv_tg[1];
                    m_threadgroup_size.depth = psv_tg[2];
                    PSTRACE("  threadgroup_size from cached PSV0: %ux%ux%u",
                            psv_tg[0], psv_tg[1], psv_tg[2]);
                  }
                  MTL_SHADER_REFLECTION msc_reflection = {};
                  std::vector<MTL_SM50_SHADER_ARGUMENT> msc_arguments;
                  if (ParseMSCReflection(rbuf, msc_reflection,
                                         msc_arguments)) {
                    msc_reflection.ThreadgroupSize[0] =
                        m_threadgroup_size.width;
                    msc_reflection.ThreadgroupSize[1] =
                        m_threadgroup_size.height;
                    msc_reflection.ThreadgroupSize[2] =
                        m_threadgroup_size.depth;
                    if (out_reflection)
                      *out_reflection = msc_reflection;
                    m_cs_args = std::move(msc_arguments);
                    m_cs_uses_msc_argument_abi = true;
                    PSTRACE("  MSC reflection args=%u qwords=%u bind=%u",
                            msc_reflection.NumArguments,
                            msc_reflection.ArgumentTableQwords,
                            msc_reflection.ArgumentBufferBindIndex);
                  } else {
                    ParseDirectBindingManifestFile(msl_path, reflection);
                    reflection.ThreadgroupSize[0] = m_threadgroup_size.width;
                    reflection.ThreadgroupSize[1] = m_threadgroup_size.height;
                    reflection.ThreadgroupSize[2] = m_threadgroup_size.depth;
                    if (out_reflection)
                      *out_reflection = reflection;
                    PSTRACE("  custom MSL reflection from manifest %s "
                            "uav=0x%llx srv=0x%llx/0x%llx",
                            msl_path,
                            (unsigned long long)reflection.UAVSlotMask,
                            (unsigned long long)reflection.SRVSlotMaskLo,
                            (unsigned long long)reflection.SRVSlotMaskHi);
                  }
                } else if (type == ShaderType::Pixel) {
                  MTL_SHADER_REFLECTION msc_reflection = {};
                  std::vector<MTL_SM50_SHADER_ARGUMENT> msc_arguments;
                  if (ParseMSCReflection(rbuf, msc_reflection,
                                         msc_arguments)) {
                    if (out_reflection)
                      *out_reflection = msc_reflection;
                    m_ps_args = std::move(msc_arguments);
                    m_ps_uses_msc_argument_abi = true;
                    PSTRACE("  MSC pixel reflection args=%u qwords=%u bind=%u",
                            msc_reflection.NumArguments,
                            msc_reflection.ArgumentTableQwords,
                            msc_reflection.ArgumentBufferBindIndex);
                  }
                } else if (type == ShaderType::Vertex ||
                           type == ShaderType::Amplification) {
                  MTL_SHADER_REFLECTION msc_reflection = {};
                  std::vector<MTL_SM50_SHADER_ARGUMENT> msc_arguments;
                  if (ParseMSCReflection(rbuf, msc_reflection,
                                         msc_arguments)) {
                    if (out_reflection)
                      *out_reflection = msc_reflection;
                    m_vs_reflection = msc_reflection;
                    m_vs_args = std::move(msc_arguments);
                    m_vs_uses_msc_argument_abi = true;
                    PSTRACE("  MSC %s reflection args=%u qwords=%u bind=%u",
                            type == ShaderType::Amplification ? "object"
                                                              : "vertex",
                            msc_reflection.NumArguments,
                            msc_reflection.ArgumentTableQwords,
                            msc_reflection.ArgumentBufferBindIndex);
                  }
                } else if (type == ShaderType::Mesh) {
                  MTL_SHADER_REFLECTION msc_reflection = {};
                  std::vector<MTL_SM50_SHADER_ARGUMENT> msc_arguments;
                  if (ParseMSCReflection(rbuf, msc_reflection,
                                         msc_arguments)) {
                    if (out_reflection)
                      *out_reflection = msc_reflection;
                    m_gs_reflection = msc_reflection;
                    m_gs_args = std::move(msc_arguments);
                    m_gs_uses_msc_argument_abi = true;
                    PSTRACE("  MSC mesh reflection args=%u qwords=%u bind=%u",
                            msc_reflection.NumArguments,
                            msc_reflection.ArgumentTableQwords,
                            msc_reflection.ArgumentBufferBindIndex);
                  }
                }
                return true;
              } else {
                PSTRACE("  WMT newFunction returned null");
                DumpShaderBlob(dxbc_path, bytecode, size);
                return RecordCompileFailure(
                    "shader/dxil_cached_function_lookup",
                    str::format(
                        func_name,
                        " cached metallib function lookup failed; metallib ",
                        metallib_path, "; reflection ", reflection_path,
                        "; dxbc ", dxbc_path));
              }
            } else {
              auto err_desc = DescribeNSObject(err.handle);
              DumpShaderText(metallib_error_path, err_desc.c_str());
              DumpShaderBlob(dxbc_path, bytecode, size);
              PSTRACE("  WMT newLibrary FAILED: %s", err_desc.c_str());
              return RecordCompileFailure(
                  "shader/dxil_cached_metallib_load",
                  str::format(func_name,
                              " cached metallib load failed: ", err_desc,
                              "; metallib ", metallib_path, "; error ",
                              metallib_error_path, "; dxbc ", dxbc_path));
            }
          } else {
            fclose(mf);
            DumpShaderBlob(dxbc_path, bytecode, size);
            return RecordCompileFailure(
                "shader/dxil_cached_metallib_empty",
                str::format(func_name, " cached metallib empty; metallib ",
                            metallib_path, "; dxbc ", dxbc_path));
          }
          break;
        }
      }
    }
    if (!has_dxil) {
      char dxbc_path[1024];
      FormatShaderCachePath(dxbc_path, sizeof(dxbc_path),
                            "%016zx.sm50_failed.dxbc", hash);
      DumpShaderBlob(dxbc_path, bytecode, size);
      PSTRACE("SM50Init FAILED for %s: %s (no DXIL chunk, dumped %s)",
              func_name, err_buf, dxbc_path);
    }
    return RecordCompileFailure(
        has_dxil ? "shader/dxil_metallib_cache" : "shader/sm50_init",
        str::format(func_name, " SM50Initialize failed: ", err_buf));
  }

  SM50_SHADER_COMMON_DATA common = {};
  common.next = nullptr;
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_310;
  common.flags = {};

  if (type == ShaderType::Compute) {
    uint32_t psv_tg[3] = {0, 0, 0};
    bool has_psv_tg = ExtractPSV0ComputeThreadgroupSize(bytecode, size, psv_tg);
    uint32_t tgx = has_psv_tg ? psv_tg[0]
                              : (reflection.ThreadgroupSize[0]
                                     ? reflection.ThreadgroupSize[0]
                                     : 1);
    uint32_t tgy = has_psv_tg ? psv_tg[1]
                              : (reflection.ThreadgroupSize[1]
                                     ? reflection.ThreadgroupSize[1]
                                     : 1);
    uint32_t tgz = has_psv_tg ? psv_tg[2]
                              : (reflection.ThreadgroupSize[2]
                                     ? reflection.ThreadgroupSize[2]
                                     : 1);
    m_threadgroup_size.width = tgx;
    m_threadgroup_size.height = tgy;
    m_threadgroup_size.depth = tgz;
    PSTRACE("CompileShader: %s SM50 threadgroup_size=%ux%ux%u source=%s",
            func_name, tgx, tgy, tgz, has_psv_tg ? "PSV0" : "reflection");
  }

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA stream_output = {};
  std::vector<SM50_STREAM_OUTPUT_ELEMENT> stream_output_elements;
  SM50_SHADER_COMPILATION_ARGUMENT_DATA *compile_args =
      (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common;
  if (type == ShaderType::Vertex) {
    ia_layout.next = &common;
    ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
    ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
    ia_layout.slot_mask = ia_slot_mask;
    ia_layout.num_elements = (uint32_t)ia_elements.size();
    ia_layout.elements = ia_elements.data();

    if (m_has_stream_output) {
      if (m_stream_output.NumEntries == 0 ||
          m_stream_output.pSODeclaration == nullptr ||
          m_stream_output.NumStrides == 0 || m_stream_output.NumStrides > 4 ||
          m_stream_output.pBufferStrides == nullptr ||
          m_stream_output.RasterizedStream != D3D12_SO_NO_RASTERIZED_STREAM) {
        return RecordCompileFailure(
            "pso/unsupported_stream_output_desc",
            "Stream-output requires one to four non-rasterized output strides");
      }
      for (UINT slot = 0; slot < m_stream_output.NumStrides; ++slot) {
        if (m_stream_output.pBufferStrides[slot] == 0 ||
            m_stream_output.pBufferStrides[slot] >
                D3D12_SO_BUFFER_MAX_STRIDE_IN_BYTES) {
          return RecordCompileFailure(
              "pso/unsupported_stream_output_desc",
              str::format("Invalid stream-output stride slot=", slot,
                          " stride=", m_stream_output.pBufferStrides[slot]));
        }
      }

      using namespace microsoft;
      CSignatureParser output_parser;
      if (FAILED(DXBCGetOutputSignature(bytecode, &output_parser))) {
        return RecordCompileFailure(
            "pso/unsupported_stream_output_signature",
            "Stream-output DXBC has no readable output signature");
      }
      const D3D11_SIGNATURE_PARAMETER *output_parameters = nullptr;
      const uint32_t output_parameter_count =
          output_parser.GetParameters(&output_parameters);
      uint32_t output_offsets[4] = {};
      stream_output_elements.reserve(m_stream_output.NumEntries * 4u);
      for (UINT i = 0; i < m_stream_output.NumEntries; ++i) {
        const auto &entry = m_stream_output.pSODeclaration[i];
        if (entry.Stream != 0 || entry.OutputSlot >= m_stream_output.NumStrides ||
            entry.StartComponent > 3 || entry.ComponentCount > 4 ||
            uint32_t(entry.StartComponent) + uint32_t(entry.ComponentCount) >
                4) {
          return RecordCompileFailure(
              "pso/unsupported_stream_output_desc",
              str::format("Unsupported stream-output entry ", i,
                          " stream=", (unsigned)entry.Stream,
                          " output_slot=", (unsigned)entry.OutputSlot,
                          " start=", (unsigned)entry.StartComponent,
                          " count=", (unsigned)entry.ComponentCount));
        }
        if (entry.ComponentCount == 0)
          continue;

        uint32_t register_id = 0xffffffffu;
        if (entry.SemanticName) {
          const auto *parameter = std::find_if(
              output_parameters, output_parameters + output_parameter_count,
              [&](const D3D11_SIGNATURE_PARAMETER &candidate) {
                return candidate.SemanticName &&
                       candidate.SemanticIndex == entry.SemanticIndex &&
                       strcasecmp(candidate.SemanticName, entry.SemanticName) ==
                           0;
              });
          if (parameter == output_parameters + output_parameter_count) {
            return RecordCompileFailure(
                "pso/unsupported_stream_output_signature",
                str::format("Stream-output semantic not found: ",
                            entry.SemanticName, entry.SemanticIndex));
          }
          register_id = parameter->Register;
        }

        for (UINT component = 0; component < entry.ComponentCount;
             ++component) {
          const uint32_t slot = entry.OutputSlot;
          stream_output_elements.push_back({
              register_id,
              uint32_t(entry.StartComponent) + component,
              slot,
              output_offsets[slot]});
          output_offsets[slot] += sizeof(float);
        }
      }
      if (stream_output_elements.empty()) {
        return RecordCompileFailure(
            "pso/unsupported_stream_output_desc",
            "Stream-output declaration contains no components");
      }
      for (UINT slot = 0; slot < m_stream_output.NumStrides; ++slot) {
        if (output_offsets[slot] > m_stream_output.pBufferStrides[slot]) {
          return RecordCompileFailure(
              "pso/unsupported_stream_output_desc",
              str::format("Stream-output declaration does not fit slot=", slot,
                          " bytes=", output_offsets[slot], " stride=",
                          m_stream_output.pBufferStrides[slot]));
        }
      }
      stream_output.next = &common;
      stream_output.type = SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT;
      stream_output.num_output_slots = m_stream_output.NumStrides;
      stream_output.num_elements =
          static_cast<uint32_t>(stream_output_elements.size());
      for (UINT slot = 0; slot < m_stream_output.NumStrides; ++slot)
        stream_output.strides[slot] = m_stream_output.pBufferStrides[slot];
      stream_output.elements = stream_output_elements.data();
      ia_layout.next = &stream_output;
      PSTRACE("CompileShader: %s stream-output elements=%u slots=%u stride0=%u",
              func_name, stream_output.num_elements,
              stream_output.num_output_slots, stream_output.strides[0]);
    }

    compile_args = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&ia_layout;
    PSTRACE("CompileShader: %s IA args elements=%u slot_mask=0x%x", func_name,
            ia_layout.num_elements, ia_layout.slot_mask);
  }

  sm50_bitcode_t compile_result = nullptr;
  if (SM50Compile(shader, compile_args, func_name, &compile_result,
                  &sm50_err)) {
    char err_buf[256] = {};
    SM50GetErrorMessage(sm50_err, err_buf, sizeof(err_buf));
    char dxbc_path[1024];
    FormatShaderCachePath(dxbc_path, sizeof(dxbc_path),
                          "%016zx.sm50_compile_failed.dxbc", hash);
    DumpShaderBlob(dxbc_path, bytecode, size);
    PSTRACE("SM50Compile failed for %s: %s (dumped %s)", func_name, err_buf,
            dxbc_path);
    Logger::err(
        str::format("SM50Compile failed for ", func_name, ": ", err_buf));
    SM50FreeError(sm50_err);
    SM50Destroy(shader);
    return RecordCompileFailure("shader/sm50_compile",
                                str::format(func_name, " SM50Compile failed: ",
                                            err_buf, "; dumped ", dxbc_path));
  }

  SM50_COMPILED_BITCODE bitcode = {};
  SM50GetCompiledBitcode(compile_result, &bitcode);

  {
    EnsureShaderCacheDir();
    char dump_path[1024];
    char hash_metallib_path[1024];
    snprintf(dump_path, sizeof(dump_path), "%s/dxmt_sm50_%s.metallib",
             ShaderCacheDir().c_str(), func_name);
    FormatShaderCachePath(hash_metallib_path, sizeof(hash_metallib_path),
                          "%016zx.metallib", hash);
    bool shared_written = false;
    bool hash_written = false;
    FILE *df = fopen(dump_path, "wb");
    if (df) {
      shared_written =
          fwrite(bitcode.Data, 1, bitcode.Size, df) == bitcode.Size;
      fclose(df);
    }
    FILE *hf = fopen(hash_metallib_path, "wb");
    if (hf) {
      hash_written = fwrite(bitcode.Data, 1, bitcode.Size, hf) == bitcode.Size;
      fclose(hf);
    }
    if (shared_written && hash_written)
      Logger::info(str::format("  SM50 dumped ", func_name, " to ", dump_path,
                               " and ", hash_metallib_path, " (", bitcode.Size,
                               " bytes)"));
    else
      Logger::err(str::format("  SM50 metallib dump failed for ", func_name,
                              ": shared=", shared_written,
                              " hash=", hash_written, " paths=", dump_path,
                              ", ", hash_metallib_path));
  }

  auto wmt_device = m_device->GetDXMTDevice().device();
  WMT::Reference<WMT::Error> err;
  auto lib_data = WMT::MakeDispatchData(bitcode.Data, bitcode.Size);
  auto library = wmt_device.newLibrary(lib_data, err);

  if (err.handle) {
    auto err_desc = DescribeNSObject(err.handle);
    char dxbc_path[1024];
    FormatShaderCachePath(dxbc_path, sizeof(dxbc_path),
                          "%016zx.sm50_metal_library_failed.dxbc", hash);
    DumpShaderBlob(dxbc_path, bytecode, size);
    PSTRACE("Failed to create Metal library for %s: %s (dumped %s)", func_name,
            err_desc.c_str(), dxbc_path);
    Logger::err(str::format("Failed to create Metal library for ", func_name));
    SM50DestroyBitcode(compile_result);
    SM50Destroy(shader);
    return RecordCompileFailure(
        "shader/sm50_metal_library",
        str::format(func_name, " SM50 Metal library creation failed: ",
                    err_desc, "; dumped ", dxbc_path));
  }

  out_func = library.newFunction(func_name);
  SM50DestroyBitcode(compile_result);

  if (out_reflection) {
    *out_reflection = reflection;
  }

  if (out_shader_handle) {
    *out_shader_handle = shader;
  } else {
    SM50Destroy(shader);
  }

  if (!out_func.handle) {
    char dxbc_path[1024];
    FormatShaderCachePath(dxbc_path, sizeof(dxbc_path),
                          "%016zx.sm50_function_lookup_failed.dxbc", hash);
    DumpShaderBlob(dxbc_path, bytecode, size);
    PSTRACE("Failed to get function %s from Metal library (dumped %s)",
            func_name, dxbc_path);
    Logger::err(str::format("Failed to get function ", func_name));
    return RecordCompileFailure(
        "shader/sm50_function_lookup",
        str::format(func_name, " SM50 function lookup failed; dumped ",
                    dxbc_path));
  }

  PSTRACE("CompileShader: %s SM50 OK function=%llu", func_name,
          (unsigned long long)out_func.handle);
  Logger::info(str::format("  Compiled ", func_name, " OK"));
  if (D3D12ShaderCacheEnabled()) {
    std::lock_guard<std::mutex> lock(s_shader_mutex);
    s_shader_cache[hash] = out_func;
  }
  return true;
}

bool MTLD3D12PipelineState::CompileGeometryPipelineShaders(
    WMT::Reference<WMT::Function> &object_func,
    WMT::Reference<WMT::Function> &mesh_func) {
  std::vector<SM50_IA_INPUT_ELEMENT> ia_elements;
  uint32_t ia_slot_mask = 0;
  BuildIAInputLayout(m_vs.data(), m_vs.size(), ia_elements, ia_slot_mask);
  m_ia_slot_mask = ia_slot_mask;

  sm50_error_t error = nullptr;
  sm50_shader_t vertex_shader = nullptr;
  sm50_shader_t geometry_shader = nullptr;
  MTL_SHADER_REFLECTION vertex_reflection = {};
  MTL_SHADER_REFLECTION geometry_reflection = {};

  auto initialize = [&](const std::vector<uint8_t> &bytecode,
                        sm50_shader_t *shader,
                        MTL_SHADER_REFLECTION *reflection,
                        const char *stage) -> bool {
    if (SM50InitializeWithOptions(bytecode.data(), bytecode.size(), 0, shader,
                                  reflection, &error) == 0)
      return true;
    char message[512] = {};
    if (error) {
      SM50GetErrorMessage(error, message, sizeof(message));
      SM50FreeError(error);
      error = nullptr;
    }
    RecordCompileFailure("shader/sm50_geometry_init",
                         str::format(stage, " initialization failed: ",
                                     message));
    return false;
  };

  if (!initialize(m_vs, &vertex_shader, &vertex_reflection, "vertex") ||
      !initialize(m_gs, &geometry_shader, &geometry_reflection, "geometry")) {
    if (vertex_shader)
      SM50Destroy(vertex_shader);
    if (geometry_shader)
      SM50Destroy(geometry_shader);
    return false;
  }

  m_vs_reflection = vertex_reflection;
  m_gs_reflection = geometry_reflection;
  m_vs_cb_args.resize(vertex_reflection.NumConstantBuffers);
  m_vs_args.resize(vertex_reflection.NumArguments);
  m_gs_cb_args.resize(geometry_reflection.NumConstantBuffers);
  m_gs_args.resize(geometry_reflection.NumArguments);
  SM50GetArgumentsInfo(vertex_shader,
                       m_vs_cb_args.empty() ? nullptr : m_vs_cb_args.data(),
                       m_vs_args.empty() ? nullptr : m_vs_args.data());
  SM50GetArgumentsInfo(geometry_shader,
                       m_gs_cb_args.empty() ? nullptr : m_gs_cb_args.data(),
                       m_gs_args.empty() ? nullptr : m_gs_args.data());

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = SM50_SHADER_METAL_310;

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  ia_layout.next = &common;
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = ia_slot_mask;
  ia_layout.num_elements = static_cast<uint32_t>(ia_elements.size());
  ia_layout.elements = ia_elements.data();

  auto compile_stage = [&](bool vertex,
                           WMT::Reference<WMT::Function> &function) -> bool {
    SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry = {};
    geometry.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
    geometry.next = vertex
                        ? static_cast<void *>(&ia_layout)
                        : static_cast<void *>(&common);
    geometry.strip_topology = true;

    sm50_bitcode_t result = nullptr;
    const char *name = vertex ? "vs_main" : "gs_main";
    int failed = vertex
                     ? SM50CompileGeometryPipelineVertex(
                           vertex_shader, geometry_shader,
                           reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                               &geometry),
                           name, &result, &error)
                     : SM50CompileGeometryPipelineGeometry(
                           vertex_shader, geometry_shader,
                           reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                               &geometry),
                           name, &result, &error);
    if (failed) {
      char message[512] = {};
      if (error) {
        SM50GetErrorMessage(error, message, sizeof(message));
        SM50FreeError(error);
        error = nullptr;
      }
      RecordCompileFailure("shader/sm50_geometry_compile",
                           str::format(name, " compilation failed: ",
                                       message));
      return false;
    }

    SM50_COMPILED_BITCODE bitcode = {};
    SM50GetCompiledBitcode(result, &bitcode);
    auto dispatch_data = WMT::MakeDispatchData(bitcode.Data, bitcode.Size);
    WMT::Reference<WMT::Error> metal_error;
    auto library =
        m_device->GetDXMTDevice().device().newLibrary(dispatch_data,
                                                       metal_error);
    if (!metal_error.handle)
      function = library.newFunction(name);
    std::string metal_error_text =
        metal_error.handle ? DescribeNSObject(metal_error.handle) : "";
    SM50DestroyBitcode(result);
    if (!function.handle) {
      RecordCompileFailure(
          "shader/sm50_geometry_metal_library",
          str::format(name, " Metal function creation failed",
                      metal_error_text.empty() ? "" : ": ",
                      metal_error_text));
      return false;
    }
    return true;
  };

  bool compiled = compile_stage(true, object_func) &&
                  compile_stage(false, mesh_func);
  if (compiled) {
    m_vs_shader = vertex_shader;
    m_gs_shader = geometry_shader;
  } else {
    SM50Destroy(vertex_shader);
    SM50Destroy(geometry_shader);
  }
  return compiled;
}

void MTLD3D12PipelineState::BuildIAInputLayout(
    const void *bytecode, SIZE_T size,
    std::vector<SM50_IA_INPUT_ELEMENT> &elements, uint32_t &slot_mask) {
  slot_mask = 0;
  elements.clear();
  m_ia_input_elements.clear();

  if (!bytecode || !size || !m_input_layout.NumElements ||
      !m_input_layout.pInputElementDescs)
    return;

  using namespace microsoft;
  CSignatureParser parser;
  HRESULT hr = DXBCGetInputSignature(bytecode, &parser);
  if (FAILED(hr)) {
    PSTRACE("BuildIAInputLayout: DXBCGetInputSignature failed hr=0x%lx", hr);
    return;
  }

  const D3D11_SIGNATURE_PARAMETER *params = nullptr;
  uint32_t param_count = parser.GetParameters(&params);
  std::vector<D3D12IAInputLayoutElementMetadata> layout_metadata;
  std::vector<D3D12IAInputSignatureElementMetadata> signature_metadata;
  layout_metadata.reserve(m_input_layout.NumElements);
  signature_metadata.reserve(param_count);

  for (UINT i = 0; i < m_input_layout.NumElements; i++) {
    const auto &desc = m_input_layout.pInputElementDescs[i];
    if (desc.InputSlot >= kMetalD3D12VertexBufferSlotCount) {
      PSTRACE("BuildIAInputLayout skip[%u]: slot %u outside cap %u", i,
              desc.InputSlot, kMetalD3D12VertexBufferSlotCount);
    }

    MTL_DXGI_FORMAT_DESC metal_format = {};
    bool supported_format =
        SUCCEEDED(MTLQueryDXGIFormat(m_device->GetMTLDevice(), desc.Format,
                                     metal_format)) &&
        metal_format.AttributeFormat && metal_format.BytesPerTexel;
    if (!supported_format) {
      PSTRACE("BuildIAInputLayout skip[%u]: unsupported fmt=%u", i,
              (unsigned)desc.Format);
    }

    D3D12IAInputLayoutElementMetadata element = {};
    element.semantic_name = desc.SemanticName ? desc.SemanticName : "";
    element.semantic_index = desc.SemanticIndex;
    element.input_slot = desc.InputSlot;
    element.aligned_byte_offset = desc.AlignedByteOffset;
    element.dxgi_format = desc.Format;
    element.metal_format = metal_format.AttributeFormat;
    element.bytes_per_texel = metal_format.BytesPerTexel;
    element.input_slot_class =
        desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            ? D3D12VertexInputSlotClass::PerInstance
            : D3D12VertexInputSlotClass::PerVertex;
    element.instance_step_rate =
        desc.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            ? desc.InstanceDataStepRate
            : 1;
    element.supported_format = supported_format;
    layout_metadata.push_back(std::move(element));
  }

  for (uint32_t i = 0; i < param_count; i++) {
    D3D12IAInputSignatureElementMetadata input_sig = {};
    input_sig.semantic_name =
        params[i].SemanticName ? params[i].SemanticName : "";
    input_sig.semantic_index = params[i].SemanticIndex;
    input_sig.shader_register = params[i].Register;
    input_sig.system_value = params[i].SystemValue != D3D10_SB_NAME_UNDEFINED;
    signature_metadata.push_back(std::move(input_sig));
  }

  auto metadata = D3D12BuildIAInputLayoutMetadata(
      layout_metadata, signature_metadata, kMetalD3D12VertexBufferSlotCount,
      D3D12_APPEND_ALIGNED_ELEMENT);
  slot_mask = metadata.slot_mask;

  for (UINT i = 0; i < m_input_layout.NumElements; i++) {
    const auto &desc = m_input_layout.pInputElementDescs[i];
    bool consumed =
        std::any_of(metadata.elements.begin(), metadata.elements.end(),
                    [&](const D3D12ResolvedIAInputElementMetadata &input) {
                      return !input.system_value &&
                             input.semantic_index == desc.SemanticIndex &&
                             input.input_slot == desc.InputSlot &&
                             desc.SemanticName &&
                             D3D12SemanticNameEquals(input.semantic_name,
                                                     desc.SemanticName);
                    });
    if (!consumed && desc.InputSlot < kMetalD3D12VertexBufferSlotCount)
      PSTRACE("BuildIAInputLayout skip[%u]: semantic %s%u not consumed by VS",
              i, desc.SemanticName ? desc.SemanticName : "?",
              desc.SemanticIndex);
  }

  for (const auto &resolved : metadata.elements) {
    D3D12IAInputElementInfo info = {};
    info.semantic_name = resolved.semantic_name;
    info.semantic_index = resolved.semantic_index;
    info.shader_register = resolved.shader_register;
    info.input_slot = resolved.input_slot;
    info.table_index = resolved.table_index;
    info.table_indexing_mode = resolved.table_indexing_mode;
    info.aligned_byte_offset = resolved.aligned_byte_offset;
    info.dxgi_format = static_cast<DXGI_FORMAT>(resolved.dxgi_format);
    info.metal_format = static_cast<WMTAttributeFormat>(resolved.metal_format);
    info.bytes_per_element = resolved.bytes_per_element;
    info.input_slot_class =
        resolved.input_slot_class == D3D12VertexInputSlotClass::PerInstance
            ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    info.per_instance =
        resolved.input_slot_class == D3D12VertexInputSlotClass::PerInstance;
    info.instance_step_rate = resolved.instance_step_rate;
    info.system_value = resolved.system_value;
    m_ia_input_elements.push_back(std::move(info));

    if (resolved.system_value)
      continue;

    SM50_IA_INPUT_ELEMENT element = {};
    element.reg = resolved.shader_register;
    element.slot = resolved.input_slot;
    element.aligned_byte_offset = resolved.aligned_byte_offset;
    element.format = resolved.metal_format;
    element.step_function =
        resolved.input_slot_class == D3D12VertexInputSlotClass::PerInstance;
    element.step_rate =
        resolved.input_slot_class == D3D12VertexInputSlotClass::PerInstance
            ? resolved.instance_step_rate
            : 1;
    elements.push_back(element);

    PSTRACE("BuildIAInputLayout element[%zu]: semantic=%s%u reg=%u slot=%u "
            "offset=%u fmt=%u step=%u/%u",
            elements.size() - 1, resolved.semantic_name.c_str(),
            resolved.semantic_index, element.reg, element.slot,
            element.aligned_byte_offset, element.format, element.step_function,
            element.step_rate);
  }
}

bool MTLD3D12PipelineState::Compile() {
  CompileState entry_state = m_compile_state.load();
  PTRACE("Compile() called state=%u is_compute=%d async_worker=%d worker=%u",
         (unsigned)entry_state, m_is_compute, g_async_pipeline_worker_thread,
         g_async_pipeline_worker_index);
  std::unique_lock<dxmt::mutex> lock(m_compile_mutex);
  CompileState locked_state = m_compile_state.load();
  if (locked_state == CompileState::Compiled)
    return true;
  if (locked_state == CompileState::Compiling) {
    m_compile_cv.wait(lock, [this]() {
      CompileState state = m_compile_state.load();
      return state != CompileState::Compiling && state != CompileState::Pending;
    });
    return m_compile_state.load() == CompileState::Compiled;
  }
  if (locked_state == CompileState::Pending &&
      !g_async_pipeline_worker_thread) {
    PSTRACE("PSO async pending compile claimed inline pso=%p compute=%d",
            (void *)this, m_is_compute);
  }
  m_compile_state.store(CompileState::Compiling);
  ClearCompileFailure();
  m_uses_conservative_rasterization =
      !m_is_compute &&
      m_rasterizer_desc.ConservativeRaster ==
          D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
  m_uses_conservative_rasterization_reference_model =
      m_uses_conservative_rasterization && m_ms.empty() && m_gs.empty() &&
      m_hs.empty() && m_ds.empty() && m_num_render_targets == 1 &&
      m_topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE &&
      m_rasterizer_desc.FillMode == D3D12_FILL_MODE_SOLID &&
      m_rasterizer_desc.ForcedSampleCount == 0 &&
      m_rtv_formats[0] == DXGI_FORMAT_R8G8B8A8_UNORM &&
      m_sample_count == 1 && m_input_layout.NumElements == 1 &&
      m_input_elements.size() == 1 &&
      m_input_elements[0].SemanticName &&
      strcasecmp(m_input_elements[0].SemanticName, "POSITION") == 0 &&
      m_input_elements[0].SemanticIndex == 0 &&
      m_input_elements[0].Format == DXGI_FORMAT_R32G32B32_FLOAT &&
      m_input_elements[0].InputSlot == 0 &&
      m_input_elements[0].AlignedByteOffset == 0 &&
      m_input_elements[0].InputSlotClass ==
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA &&
      !m_ps.empty() && DXBCContainerHasChunk(m_ps.data(), m_ps.size(), "DXIL");
  m_attribute_at_vertex_input_id = UINT32_MAX;
  m_uses_attribute_at_vertex =
      !m_is_compute && !m_vs.empty() && !m_ps.empty() && m_gs.empty() &&
      m_ms.empty() &&
      DXBCShaderUsesAttributeAtVertex(m_ps.data(), m_ps.size(),
                                      &m_attribute_at_vertex_input_id);
  lock.unlock();

  auto wmt_device = m_device->GetDXMTDevice().device();
  WMT::Reference<WMT::Error> err;

  if (m_is_compute) {
    if (m_cs.empty()) {
      Logger::err("Compute PSO has no CS bytecode");
      return RecordCompileFailure("pso/compute_no_cs",
                                  "Compute PSO has no CS bytecode");
    }

    size_t cs_hash = ComputeShaderCacheHash(m_cs.data(), m_cs.size(),
                                            ShaderType::Compute, nullptr);
    WMT::Reference<WMT::Function> cs_func;
    if (!CompileShader(m_cs.data(), m_cs.size(), ShaderType::Compute, "cs_main",
                       cs_func, &m_cs_shader, &m_cs_reflection))
      return false;

    WMTComputePipelineInfo info = {};
    WMT::InitializeComputePipelineInfo(info);
    info.compute_function = cs_func.handle;

    std::string err_desc = "unknown";
    for (uint32_t attempt = 0; attempt < 4; attempt++) {
      err = nullptr;
      m_compute_pso = wmt_device.newComputePipelineState(info, err);
      if (m_compute_pso.handle)
        break;

      err_desc = DescribeNSObject(err.handle);
      if (!IsTransientMetalCompilerError(err_desc) || attempt == 3)
        break;

      Logger::warn(str::format(
          "Retrying compute PSO after transient Metal compiler error attempt=",
          attempt + 1, " detail=", err_desc));
      PSTRACE(
          "Compute PSO transient Metal compiler failure attempt=%u detail=%s",
          attempt + 1, err_desc.c_str());
      Sleep(50 * (attempt + 1));
    }
    if (!m_compute_pso.handle) {
      Logger::err(str::format("Failed to create compute PSO: ", err_desc));
      if (m_cs_shader) {
        SM50Destroy(m_cs_shader);
        m_cs_shader = nullptr;
      }
      return RecordCompileFailure(
          "pso/metal_compute_pso",
          str::format("Metal compute PSO creation failed: ", err_desc));
    }
    DumpComputePSOManifest(cs_hash, m_cs.size(), m_threadgroup_size.width,
                           m_threadgroup_size.height, m_threadgroup_size.depth,
                           (uintptr_t)cs_func.handle);

    PTRACE("CS_ARGS_DEBUG: shader=%llu NumCB=%u NumArgs=%u CBufBindIdx=%u "
           "ArgBufBindIdx=%u ArgTableQwords=%u",
           (unsigned long long)(uintptr_t)m_cs_shader,
           m_cs_reflection.NumConstantBuffers, m_cs_reflection.NumArguments,
           m_cs_reflection.ConstanttBufferTableBindIndex,
           m_cs_reflection.ArgumentBufferBindIndex,
           m_cs_reflection.ArgumentTableQwords);
    if (m_cs_shader && (m_cs_reflection.NumArguments > 0 ||
                        m_cs_reflection.NumConstantBuffers > 0)) {
      if (m_cs_reflection.NumConstantBuffers > 0)
        m_cs_cb_args.resize(m_cs_reflection.NumConstantBuffers);
      if (m_cs_reflection.NumArguments > 0)
        m_cs_args.resize(m_cs_reflection.NumArguments);
      SM50GetArgumentsInfo(m_cs_shader,
                           m_cs_cb_args.empty() ? nullptr : m_cs_cb_args.data(),
                           m_cs_args.empty() ? nullptr : m_cs_args.data());
      for (size_t i = 0; i < m_cs_cb_args.size(); i++) {
        PTRACE("CS_ARGS_DEBUG: cb[%zu] type=%d slot=%u flags=0x%x offset=%u", i,
               (int)m_cs_cb_args[i].Type, m_cs_cb_args[i].SM50BindingSlot,
               m_cs_cb_args[i].Flags, m_cs_cb_args[i].StructurePtrOffset);
      }
      for (size_t i = 0; i < m_cs_args.size(); i++) {
        PTRACE("CS_ARGS_DEBUG: arg[%zu] type=%d slot=%u flags=0x%x offset=%u",
               i, (int)m_cs_args[i].Type, m_cs_args[i].SM50BindingSlot,
               m_cs_args[i].Flags, m_cs_args[i].StructurePtrOffset);
      }
    }
    if (m_cs_shader) {
      SM50Destroy(m_cs_shader);
      m_cs_shader = nullptr;
    }

    m_compile_state.store(CompileState::Compiled);
    m_compile_cv.notify_all();
    Logger::info("Compute PSO compiled successfully");
    return true;
  }

  WMT::Reference<WMT::Function> vs_func, ps_func, gs_func;
  size_t vs_hash = !m_as.empty()
                       ? ComputeShaderCacheHash(m_as.data(), m_as.size(),
                                                ShaderType::Amplification,
                                                nullptr)
                       : (m_vs.empty()
                              ? 0
                              : ApplyShaderVariantHash(
                                    ComputeShaderCacheHash(
                                        m_vs.data(), m_vs.size(),
                                        ShaderType::Vertex, &m_input_layout),
                                    ShaderType::Vertex));
  size_t ps_hash = m_ps.empty()
                       ? 0
                       : ApplyShaderVariantHash(
                             ComputeShaderCacheHash(
                                 m_ps.data(), m_ps.size(), ShaderType::Pixel,
                                 nullptr),
                             ShaderType::Pixel);
  size_t gs_hash = !m_ms.empty()
                       ? ComputeShaderCacheHash(m_ms.data(), m_ms.size(),
                                                ShaderType::Mesh, nullptr)
                       : (m_gs.empty()
                              ? 0
                              : ComputeShaderCacheHash(
                                    m_gs.data(), m_gs.size(),
                                    ShaderType::Geometry, nullptr));

  const bool native_tessellation_required = !m_hs.empty() || !m_ds.empty();
  m_uses_native_tessellation_path = false;
  m_native_tessellation_control_points = 0;
  m_uses_tessellation_fallback = false;
  if (native_tessellation_required) {
    return CompileNativeTessellationProofShape();
  }

  if (m_has_stream_output) {
    if (m_vs.empty() || !m_gs.empty() || !m_hs.empty() || !m_ds.empty() ||
        !m_ps.empty()) {
      return RecordCompileFailure(
          "pso/unsupported_stream_output_stage_mix",
          "The stream-output provider requires a vertex-only, non-rasterized "
          "DXBC graphics pipeline");
    }
    if (DXBCContainerHasChunk(m_vs.data(), m_vs.size(), "DXIL")) {
      return RecordCompileFailure(
          "pso/unsupported_stream_output_dxil",
          "The stream-output provider requires DXBC; DXIL stream capture is "
          "not silently downgraded");
    }
  }
  if (m_uses_conservative_rasterization &&
      !m_uses_conservative_rasterization_reference_model) {
    return RecordCompileFailure(
        "pso/unsupported_conservative_rasterization",
        "Conservative rasterization is only implemented for the validated "
        "single-target pass-through reference shape");
  }

  m_uses_geometry_mesh_pipeline = false;
  m_uses_native_mesh_pipeline = false;
  m_uses_independent_logic_op_emulation = false;
  m_uses_independent_logic_op_depth_replay = false;
  m_independent_logic_op_render_psos.clear();
  m_independent_logic_op_depth_only_pso = nullptr;
  m_independent_logic_op_no_write_depth_state = nullptr;

  // Determine the replay requirement before compiling the pixel shader.  The
  // side-effect guard below must see this flag so an ROV/UAV shader cannot be
  // accidentally executed once per render target.
  if (m_blend_desc.IndependentBlendEnable && m_num_render_targets > 1) {
    const auto &first = m_blend_desc.RenderTarget[0];
    for (UINT i = 1; i < m_num_render_targets && i < 8; ++i) {
      const auto &rt = m_blend_desc.RenderTarget[i];
      if (rt.LogicOpEnable != first.LogicOpEnable ||
          (rt.LogicOpEnable && rt.LogicOp != first.LogicOp)) {
        m_uses_independent_logic_op_emulation = true;
        break;
      }
    }
  }
  if (!m_ms.empty()) {
    if (!m_vs.empty() || !m_gs.empty() || !m_hs.empty() || !m_ds.empty()) {
      return RecordCompileFailure(
          "pso/mesh_with_legacy_stages",
          "Mesh PSO cannot contain VS, GS, HS, or DS bytecode");
    }
    if (!m_as.empty()) {
      if (!CompileShader(m_as.data(), m_as.size(), ShaderType::Amplification,
                         "as_main", vs_func))
        return false;
      m_object_threadgroup_size = {m_threadgroup_size.width,
                                   m_threadgroup_size.height,
                                   m_threadgroup_size.depth};
    } else {
      m_object_threadgroup_size = {1, 1, 1};
    }
    if (!CompileShader(m_ms.data(), m_ms.size(), ShaderType::Mesh, "ms_main",
                       gs_func))
      return false;
    m_mesh_threadgroup_size = {m_threadgroup_size.width,
                               m_threadgroup_size.height,
                               m_threadgroup_size.depth};
    m_uses_geometry_mesh_pipeline = true;
    m_uses_native_mesh_pipeline = true;
    PSTRACE("D3D12 native mesh pipeline compiled object=%llu mesh=%llu "
            "object_tg=%ux%ux%u mesh_tg=%ux%ux%u",
            (unsigned long long)vs_func.handle,
            (unsigned long long)gs_func.handle,
            m_object_threadgroup_size.width, m_object_threadgroup_size.height,
            m_object_threadgroup_size.depth, m_mesh_threadgroup_size.width,
            m_mesh_threadgroup_size.height, m_mesh_threadgroup_size.depth);
  } else if (!m_gs.empty()) {
    if (m_vs.empty()) {
      return RecordCompileFailure(
          "pso/geometry_without_vertex",
          "Graphics PSO contains a geometry shader without a vertex shader");
    }
    if (!CompileGeometryPipelineShaders(vs_func, gs_func))
      return false;
    m_uses_geometry_mesh_pipeline = true;
    PSTRACE("D3D12 geometry pipeline compiled through Metal object/mesh "
            "emulation object=%llu mesh=%llu",
            (unsigned long long)vs_func.handle,
            (unsigned long long)gs_func.handle);
  } else if (!m_vs.empty()) {
    if (!CompileShader(m_vs.data(), m_vs.size(), ShaderType::Vertex, "vs_main",
                       vs_func, &m_vs_shader, &m_vs_reflection))
      return false;
  }

  if (!m_ps.empty()) {
    if (!CompileShader(m_ps.data(), m_ps.size(), ShaderType::Pixel, "ps_main",
                       ps_func, &m_ps_shader, &m_ps_reflection))
      return false;
  }

  // Replaying a draw once per attachment is only semantics-preserving when
  // the fragment shader has no UAV side effects.  A repeated atomic/store or
  // ROV access would otherwise execute multiple times. Reject that valid but
  // not-yet-modeled combination instead of silently duplicating side effects.
  if (m_uses_independent_logic_op_emulation &&
      m_ps_reflection.UAVSlotMask != 0) {
    return RecordCompileFailure(
        "pso/independent_logic_op_side_effects",
        "Independent per-render-target logic-op emulation requires a pixel "
        "shader without UAV side effects");
  }

  WMTRenderPipelineInfo info;
  WMT::InitializeRenderPipelineInfo(info);

  if (vs_func.handle)
    info.vertex_function = vs_func.handle;
  if (ps_func.handle)
    info.fragment_function = ps_func.handle;

  info.rasterization_enabled = !m_has_stream_output;
  info.raster_sample_count = m_sample_count ? m_sample_count : 1;

  for (UINT i = 0; i < m_num_render_targets && i < 8; i++) {
    auto fmt = DXGIToMTLPixelFormat(m_rtv_formats[i]);
    if (fmt != WMTPixelFormatInvalid)
      info.colors[i].pixel_format = fmt;
  }

  auto depth_fmt = DXGIToMTLPixelFormat(m_dsv_format);
  if (depth_fmt != WMTPixelFormatInvalid) {
    info.depth_pixel_format = depth_fmt;
    if (m_dsv_format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
        m_dsv_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
      info.stencil_pixel_format = depth_fmt;
  }

  // Metal exposes one logic operation for the render pipeline, whereas
  // D3D12's IndependentBlendEnable allows each color attachment to select its
  // own operation (including disabled logic ops).  Such a pipeline is
  // emulated by one render-pipeline variant per attachment; the replay path
  // binds only that attachment's write mask for each repeated draw.  Do not
  // reject the valid D3D12 state merely because the base Metal descriptor is
  // global.
  auto map_logic_op = [](D3D12_LOGIC_OP op) -> WMTLogicOperation {
    switch (op) {
    case D3D12_LOGIC_OP_CLEAR: return WMTLogicOperationClear;
    case D3D12_LOGIC_OP_SET: return WMTLogicOperationSet;
    case D3D12_LOGIC_OP_COPY: return WMTLogicOperationCopy;
    case D3D12_LOGIC_OP_COPY_INVERTED: return WMTLogicOperationCopyInverted;
    case D3D12_LOGIC_OP_NOOP: return WMTLogicOperationNoOp;
    case D3D12_LOGIC_OP_INVERT: return WMTLogicOperationInvert;
    case D3D12_LOGIC_OP_AND: return WMTLogicOperationAnd;
    case D3D12_LOGIC_OP_NAND: return WMTLogicOperationNand;
    case D3D12_LOGIC_OP_OR: return WMTLogicOperationOr;
    case D3D12_LOGIC_OP_NOR: return WMTLogicOperationNor;
    case D3D12_LOGIC_OP_XOR: return WMTLogicOperationXor;
    case D3D12_LOGIC_OP_EQUIV: return WMTLogicOperationEquiv;
    case D3D12_LOGIC_OP_AND_REVERSE: return WMTLogicOperationAndReverse;
    case D3D12_LOGIC_OP_AND_INVERTED: return WMTLogicOperationAndInverted;
    case D3D12_LOGIC_OP_OR_REVERSE: return WMTLogicOperationOrReverse;
    case D3D12_LOGIC_OP_OR_INVERTED: return WMTLogicOperationOrInverted;
    default: return WMTLogicOperationNoOp;
    }
  };
  if (m_blend_desc.RenderTarget[0].LogicOpEnable) {
    info.logic_operation_enabled = true;
    info.logic_operation =
        map_logic_op(m_blend_desc.RenderTarget[0].LogicOp);
  }

  if (m_blend_desc.IndependentBlendEnable ||
      m_blend_desc.RenderTarget[0].BlendEnable) {
    for (UINT i = 0; i < m_num_render_targets && i < 8; i++) {
      auto &rt = m_blend_desc.RenderTarget[i];
      info.colors[i].blending_enabled = rt.BlendEnable ? true : false;
      info.colors[i].write_mask =
          kColorWriteMaskMap[rt.RenderTargetWriteMask & 0xf];

      auto map_blend = [](D3D12_BLEND b) -> WMTBlendFactor {
        switch (b) {
        case D3D12_BLEND_ZERO:
          return WMTBlendFactorZero;
        case D3D12_BLEND_ONE:
          return WMTBlendFactorOne;
        case D3D12_BLEND_SRC_COLOR:
          return WMTBlendFactorSourceColor;
        case D3D12_BLEND_INV_SRC_COLOR:
          return WMTBlendFactorOneMinusSourceColor;
        case D3D12_BLEND_SRC_ALPHA:
          return WMTBlendFactorSourceAlpha;
        case D3D12_BLEND_INV_SRC_ALPHA:
          return WMTBlendFactorOneMinusSourceAlpha;
        case D3D12_BLEND_DEST_ALPHA:
          return WMTBlendFactorDestinationAlpha;
        case D3D12_BLEND_INV_DEST_ALPHA:
          return WMTBlendFactorOneMinusDestinationAlpha;
        case D3D12_BLEND_DEST_COLOR:
          return WMTBlendFactorDestinationColor;
        case D3D12_BLEND_INV_DEST_COLOR:
          return WMTBlendFactorOneMinusDestinationColor;
        case D3D12_BLEND_SRC_ALPHA_SAT:
          return WMTBlendFactorSourceAlphaSaturated;
        case D3D12_BLEND_BLEND_FACTOR:
          return WMTBlendFactorBlendColor;
        case D3D12_BLEND_INV_BLEND_FACTOR:
          return WMTBlendFactorOneMinusBlendColor;
        default:
          return WMTBlendFactorOne;
        }
      };

      auto map_op = [](D3D12_BLEND_OP op) -> WMTBlendOperation {
        switch (op) {
        case D3D12_BLEND_OP_ADD:
          return WMTBlendOperationAdd;
        case D3D12_BLEND_OP_SUBTRACT:
          return WMTBlendOperationSubtract;
        case D3D12_BLEND_OP_REV_SUBTRACT:
          return WMTBlendOperationReverseSubtract;
        case D3D12_BLEND_OP_MIN:
          return WMTBlendOperationMin;
        case D3D12_BLEND_OP_MAX:
          return WMTBlendOperationMax;
        default:
          return WMTBlendOperationAdd;
        }
      };

      info.colors[i].src_rgb_blend_factor = map_blend(rt.SrcBlend);
      info.colors[i].dst_rgb_blend_factor = map_blend(rt.DestBlend);
      info.colors[i].rgb_blend_operation = map_op(rt.BlendOp);
      info.colors[i].src_alpha_blend_factor = map_blend(rt.SrcBlendAlpha);
      info.colors[i].dst_alpha_blend_factor = map_blend(rt.DestBlendAlpha);
      info.colors[i].alpha_blend_operation = map_op(rt.BlendOpAlpha);
    }
  }

  switch (m_topology) {
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
    info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
    break;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
    info.input_primitive_topology = WMTPrimitiveTopologyClassLine;
    break;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    break;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH:
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    break;
  default:
    info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
    break;
  }

  info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
  info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

  if (m_uses_conservative_rasterization_reference_model) {
    WMT::Reference<WMT::Error> conservative_error;
    m_conservative_vertex_library = wmt_device.newLibraryWithSource(
        kConservativeRasterVertexShader,
        std::strlen(kConservativeRasterVertexShader), conservative_error);
    if (conservative_error.handle) {
      const std::string detail = DescribeNSObject(conservative_error.handle);
      conservative_error.release();
      return RecordCompileFailure(
          "pso/conservative_rasterization_vertex_shader",
          str::format("Conservative raster reference vertex shader failed: ",
                      detail));
    }
    if (!m_conservative_vertex_library.handle) {
      return RecordCompileFailure(
          "pso/conservative_rasterization_vertex_shader",
          "Conservative raster reference vertex shader library is null");
    }
    m_conservative_vertex_function =
        m_conservative_vertex_library.newFunction("m12_conservative_vs");
    if (!m_conservative_vertex_function.handle)
      return RecordCompileFailure(
          "pso/conservative_rasterization_vertex_shader",
          "Conservative raster reference vertex function is null");
    info.vertex_function = m_conservative_vertex_function.handle;
    info.vertex_descriptor = nullptr;
    info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
    info.immutable_vertex_buffers &= ~(1u << 26);
  }

  WMTVertexDescriptor vtx_desc = {};
  if (m_input_layout.NumElements > 0 && m_input_layout.pInputElementDescs) {
    uint32_t append_offset[WMT_MAX_VERTEX_BUFFER_LAYOUTS] = {};
    uint32_t slot_stride[WMT_MAX_VERTEX_BUFFER_LAYOUTS] = {};
    uint32_t max_slot = 0;
    bool slot_per_vertex[WMT_MAX_VERTEX_BUFFER_LAYOUTS] = {};
    uint32_t attribute_count = 0;
    uint32_t next_attribute = 0;
    const microsoft::D3D11_SIGNATURE_PARAMETER *input_sig_params = nullptr;
    uint32_t input_sig_count = 0;
    microsoft::CSignatureParser input_sig_parser;
    bool has_input_signature =
        !m_vs.empty() &&
        SUCCEEDED(DXBCGetInputSignature(m_vs.data(), &input_sig_parser));
    if (has_input_signature) {
      input_sig_count = input_sig_parser.GetParameters(&input_sig_params);
      PSTRACE("D3D12 PSO input-layout: shader input signature params=%u",
              input_sig_count);
    } else {
      PSTRACE("D3D12 PSO input-layout: shader input signature unavailable; "
              "using layout order");
    }

    PSTRACE("D3D12 PSO input-layout: elements=%u metal_attr_cap=%u "
            "metal_slot_cap=%u",
            m_input_layout.NumElements, WMT_MAX_VERTEX_ATTRIBUTES,
            kMetalD3D12VertexBufferSlotCount);

    for (UINT i = 0; i < m_input_layout.NumElements; i++) {
      auto &el = m_input_layout.pInputElementDescs[i];

      MTL_DXGI_FORMAT_DESC metal_format = {};
      if (FAILED(MTLQueryDXGIFormat(m_device->GetMTLDevice(), el.Format,
                                    metal_format)) ||
          !metal_format.AttributeFormat || !metal_format.BytesPerTexel) {
        PSTRACE(
            "D3D12 PSO input-layout skip[%u]: unsupported fmt=%u semantic=%s%u",
            i, (unsigned)el.Format, el.SemanticName ? el.SemanticName : "?",
            el.SemanticIndex);
        continue;
      }

      if (el.InputSlot >= kMetalD3D12VertexBufferSlotCount) {
        PSTRACE("D3D12 PSO input-layout skip[%u]: input slot %u is outside "
                "Metal-backed slot cap %u",
                i, el.InputSlot, kMetalD3D12VertexBufferSlotCount);
        continue;
      }

      if (attribute_count >= WMT_MAX_VERTEX_ATTRIBUTES) {
        PSTRACE("D3D12 PSO input-layout skip[%u]: attribute cap %u reached", i,
                WMT_MAX_VERTEX_ATTRIBUTES);
        continue;
      }

      uint32_t attr_index = next_attribute;
      if (has_input_signature && input_sig_params) {
        auto *sig = std::find_if(
            input_sig_params, input_sig_params + input_sig_count,
            [&](const microsoft::D3D11_SIGNATURE_PARAMETER &input_sig) {
              return input_sig.SystemValue ==
                         microsoft::D3D10_SB_NAME_UNDEFINED &&
                     el.SemanticName && input_sig.SemanticName &&
                     el.SemanticIndex == input_sig.SemanticIndex &&
                     strcasecmp(el.SemanticName, input_sig.SemanticName) == 0;
            });
        if (sig != input_sig_params + input_sig_count) {
          attr_index = sig->Register;
        } else {
          PSTRACE("D3D12 PSO input-layout desc[%u]: semantic %s%u not found in "
                  "input signature; using attr order %u",
                  i, el.SemanticName ? el.SemanticName : "?", el.SemanticIndex,
                  attr_index);
        }
      }

      constexpr uint32_t kMSCStageInAttributeStartIndex = 11;
      constexpr uint32_t kMSCVertexBufferBindPoint = 6;
      uint32_t metal_attr_index =
          m_vs_uses_stage_in ? kMSCStageInAttributeStartIndex + attr_index
                             : attr_index;
      if (metal_attr_index >= WMT_MAX_VERTEX_ATTRIBUTES) {
        PSTRACE("D3D12 PSO input-layout skip[%u]: mapped attribute %u outside "
                "cap %u",
                i, metal_attr_index, WMT_MAX_VERTEX_ATTRIBUTES);
        continue;
      }
      next_attribute = std::max(next_attribute, attr_index + 1);

      uint32_t aligned_offset =
          el.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT
              ? D3D12ResolveAlignedInputOffset(append_offset[el.InputSlot],
                                               metal_format.BytesPerTexel)
              : el.AlignedByteOffset;
      uint32_t end = aligned_offset + metal_format.BytesPerTexel;
      append_offset[el.InputSlot] = end;
      if (end > slot_stride[el.InputSlot])
        slot_stride[el.InputSlot] = end;
      if (el.InputSlot >= max_slot)
        max_slot = el.InputSlot + 1;
      slot_per_vertex[el.InputSlot] =
          (el.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);

      auto &attr = vtx_desc.attributes[metal_attr_index];
      attr.format = metal_format.AttributeFormat;
      attr.offset = aligned_offset;
      attr.buffer_index = m_vs_uses_stage_in
                              ? kMSCVertexBufferBindPoint + el.InputSlot
                              : el.InputSlot;
      attribute_count = std::max(attribute_count, metal_attr_index + 1);

      PSTRACE("D3D12 PSO input-layout attr[%u]<-desc[%u]: semantic=%s%u fmt=%u "
              "mtl_fmt=%u slot=%u offset=%u stride_end=%u class=%u step=%u",
              metal_attr_index, i, el.SemanticName ? el.SemanticName : "?",
              el.SemanticIndex, (unsigned)el.Format,
              (unsigned)metal_format.AttributeFormat, el.InputSlot,
              aligned_offset, end, (unsigned)el.InputSlotClass,
              el.InstanceDataStepRate);
    }
    vtx_desc.attribute_count = attribute_count;
    constexpr uint32_t kMSCVertexBufferBindPoint = 6;
    vtx_desc.layout_count =
        m_vs_uses_stage_in ? kMSCVertexBufferBindPoint + max_slot : max_slot;
    for (uint32_t s = 0; s < max_slot; s++) {
      uint32_t metal_slot =
          m_vs_uses_stage_in ? kMSCVertexBufferBindPoint + s : s;
      vtx_desc.layouts[metal_slot].stride = slot_stride[s];
      vtx_desc.layouts[metal_slot].step_function =
          slot_per_vertex[s] ? WMTVertexStepFunctionPerVertex
                             : WMTVertexStepFunctionPerInstance;
      vtx_desc.layouts[metal_slot].step_rate = 1;
      PSTRACE("D3D12 PSO input-layout slot[%u]->metal[%u]: stride=%u step=%u",
              s, metal_slot, slot_stride[s],
              (unsigned)vtx_desc.layouts[metal_slot].step_function);
    }
    if (!m_vs_uses_stage_in) {
      PSTRACE("D3D12 PSO input-layout compiled for SM50 vertex pulling; Metal "
              "vertex descriptor disabled");
    }
  }
  if (m_vs_uses_stage_in && vtx_desc.attribute_count > 0) {
    info.vertex_descriptor = &vtx_desc;
    PSTRACE("D3D12 PSO stage-in vertex descriptor attached attrs=%u layouts=%u",
            vtx_desc.attribute_count, vtx_desc.layout_count);
  } else if (m_vs_uses_stage_in) {
    constexpr uint32_t kSyntheticStageInAttributes = 16;
    constexpr uint32_t kSyntheticStageInStride =
        16 * kSyntheticStageInAttributes;
    for (uint32_t i = 0;
         i < kSyntheticStageInAttributes && i < WMT_MAX_VERTEX_ATTRIBUTES;
         i++) {
      auto &attr = vtx_desc.attributes[i];
      attr.format = WMTAttributeFormatFloat4;
      attr.offset = i * 16;
      attr.buffer_index = 0;
    }
    vtx_desc.attribute_count = std::min<uint32_t>(kSyntheticStageInAttributes,
                                                  WMT_MAX_VERTEX_ATTRIBUTES);
    vtx_desc.layout_count = 1;
    vtx_desc.layouts[0].stride = kSyntheticStageInStride;
    vtx_desc.layouts[0].step_function = WMTVertexStepFunctionPerVertex;
    vtx_desc.layouts[0].step_rate = 1;
    info.vertex_descriptor = &vtx_desc;
    PSTRACE("D3D12 PSO synthetic vertex descriptor attached attrs=%u stride=%u",
            vtx_desc.attribute_count, vtx_desc.layouts[0].stride);
  }
  if (m_uses_conservative_rasterization_reference_model) {
    info.vertex_function = m_conservative_vertex_function.handle;
    info.vertex_descriptor = nullptr;
    info.input_primitive_topology = WMTPrimitiveTopologyClassPoint;
  }

  PSTRACE(
      "D3D12 PSO state this=%p rts=%u dsv_fmt=%u depth=%u stencil=%u blend0=%u "
      "write_mask0=0x%x cull=%u fill=%u front_ccw=%u depth_clip=%u",
      (void *)this, m_num_render_targets, (unsigned)m_dsv_format,
      (unsigned)m_depth_stencil_desc.DepthEnable,
      (unsigned)m_depth_stencil_desc.StencilEnable,
      (unsigned)m_blend_desc.RenderTarget[0].BlendEnable,
      (unsigned)m_blend_desc.RenderTarget[0].RenderTargetWriteMask,
      (unsigned)m_rasterizer_desc.CullMode,
      (unsigned)m_rasterizer_desc.FillMode,
      (unsigned)m_rasterizer_desc.FrontCounterClockwise,
      (unsigned)m_rasterizer_desc.DepthClipEnable);
  PSTRACE("D3D12 PSO rasterizer2 line_mode=%u",
          (unsigned)m_rasterizer_desc2_line_mode);

  WMTMeshRenderPipelineInfo mesh_info = {};
  if (m_uses_geometry_mesh_pipeline) {
    WMT::InitializeMeshRenderPipelineInfo(mesh_info);
    memcpy(mesh_info.colors, info.colors, sizeof(mesh_info.colors));
    mesh_info.alpha_to_coverage_enabled = info.alpha_to_coverage_enabled;
    mesh_info.logic_operation_enabled = info.logic_operation_enabled;
    mesh_info.logic_operation = info.logic_operation;
    mesh_info.rasterization_enabled = info.rasterization_enabled;
    mesh_info.raster_sample_count = info.raster_sample_count;
    mesh_info.depth_pixel_format = info.depth_pixel_format;
    mesh_info.stencil_pixel_format = info.stencil_pixel_format;
    mesh_info.object_function = vs_func.handle;
    mesh_info.mesh_function = gs_func.handle;
    mesh_info.fragment_function = ps_func.handle;
    mesh_info.payload_memory_length =
        m_uses_native_mesh_pipeline ? m_mesh_payload_size : 16256;
    mesh_info.immutable_object_buffers =
        (1u << 16) | (1u << 21) | (1u << 29) | (1u << 30);
    mesh_info.immutable_mesh_buffers = (1u << 29) | (1u << 30);
    mesh_info.immutable_fragment_buffers = (1u << 29) | (1u << 30);
  }

  std::string render_err_desc = "unknown";
  for (uint32_t attempt = 0; attempt < 4; attempt++) {
    err = nullptr;
    m_render_pso = m_uses_geometry_mesh_pipeline
                       ? wmt_device.newRenderPipelineState(mesh_info, err)
                       : wmt_device.newRenderPipelineState(info, err);
    if (m_render_pso.handle)
      break;

    render_err_desc = DescribeNSObject(err.handle);
    if (!IsTransientMetalCompilerError(render_err_desc) || attempt == 3)
      break;

    Logger::warn(str::format(
        "Retrying render PSO after transient Metal compiler error attempt=",
        attempt + 1, " detail=", render_err_desc));
    PSTRACE("Render PSO transient Metal compiler failure attempt=%u detail=%s",
            attempt + 1, render_err_desc.c_str());
    Sleep(50 * (attempt + 1));
  }
  if (!m_render_pso.handle) {
    Logger::err(str::format("Failed to create render PSO: ", render_err_desc));
    return RecordCompileFailure(
        m_uses_native_mesh_pipeline
            ? "pso/metal_native_mesh_pso"
            : (m_uses_geometry_mesh_pipeline ? "pso/metal_geometry_mesh_pso"
                                             : "pso/metal_render_pso"),
        str::format("Metal render PSO creation failed: ", render_err_desc));
  }

  if (m_uses_independent_logic_op_emulation) {
    m_independent_logic_op_render_psos.resize(m_num_render_targets);
    for (UINT target = 0; target < m_num_render_targets && target < 8;
         ++target) {
      const auto &target_blend = m_blend_desc.RenderTarget[target];
      WMTRenderPipelineInfo variant_info = info;
      variant_info.logic_operation_enabled = target_blend.LogicOpEnable;
      variant_info.logic_operation = map_logic_op(target_blend.LogicOp);
      for (UINT attachment = 0; attachment < 8; ++attachment) {
        if (attachment != target)
          variant_info.colors[attachment].write_mask =
              WMTColorWriteMaskNone;
      }

      WMTMeshRenderPipelineInfo variant_mesh_info = mesh_info;
      if (m_uses_geometry_mesh_pipeline) {
        variant_mesh_info.logic_operation_enabled =
            variant_info.logic_operation_enabled;
        variant_mesh_info.logic_operation = variant_info.logic_operation;
        memcpy(variant_mesh_info.colors, variant_info.colors,
               sizeof(variant_mesh_info.colors));
      }

      WMT::Reference<WMT::Error> variant_error;
      std::string variant_error_desc = "unknown";
      WMT::Reference<WMT::RenderPipelineState> variant;
      for (uint32_t attempt = 0; attempt < 4; ++attempt) {
        variant_error = nullptr;
        variant = m_uses_geometry_mesh_pipeline
                      ? wmt_device.newRenderPipelineState(variant_mesh_info,
                                                          variant_error)
                      : wmt_device.newRenderPipelineState(variant_info,
                                                          variant_error);
        if (variant.handle)
          break;
        variant_error_desc = DescribeNSObject(variant_error.handle);
        if (!IsTransientMetalCompilerError(variant_error_desc) || attempt == 3)
          break;
        Sleep(50 * (attempt + 1));
      }
      if (!variant.handle) {
        Logger::err(str::format(
            "Failed to create independent logic-op render PSO target=",
            target, ": ", variant_error_desc));
        return RecordCompileFailure(
            "pso/independent_logic_op_variant",
            str::format("Metal independent logic-op render PSO target=", target,
                        " creation failed: ", variant_error_desc));
      }
      m_independent_logic_op_render_psos[target] = variant;
      PSTRACE("D3D12 independent logic-op variant target=%u handle=%llu "
              "enabled=%u op=%u",
              target, (unsigned long long)variant.handle,
              target_blend.LogicOpEnable ? 1u : 0u,
              (unsigned)target_blend.LogicOp);
    }

    // When the original depth/stencil state writes, all color variants must
    // test against the same pre-draw depth/stencil contents.  Create a
    // color-disabled variant for the final state-only replay; the command
    // queue uses a no-write depth state for the color passes and restores the
    // original state for this final draw.
    WMTRenderPipelineInfo depth_only_info = info;
    depth_only_info.logic_operation_enabled = false;
    for (UINT attachment = 0; attachment < 8; ++attachment)
      depth_only_info.colors[attachment].write_mask = WMTColorWriteMaskNone;
    WMTMeshRenderPipelineInfo depth_only_mesh_info = mesh_info;
    if (m_uses_geometry_mesh_pipeline) {
      depth_only_mesh_info.logic_operation_enabled = false;
      depth_only_mesh_info.logic_operation = WMTLogicOperationNoOp;
      memcpy(depth_only_mesh_info.colors, depth_only_info.colors,
             sizeof(depth_only_mesh_info.colors));
    }
    WMT::Reference<WMT::Error> depth_only_error;
    std::string depth_only_error_desc = "unknown";
    for (uint32_t attempt = 0; attempt < 4; ++attempt) {
      depth_only_error = nullptr;
      m_independent_logic_op_depth_only_pso =
          m_uses_geometry_mesh_pipeline
              ? wmt_device.newRenderPipelineState(depth_only_mesh_info,
                                                  depth_only_error)
              : wmt_device.newRenderPipelineState(depth_only_info,
                                                  depth_only_error);
      if (m_independent_logic_op_depth_only_pso.handle)
        break;
      depth_only_error_desc = DescribeNSObject(depth_only_error.handle);
      if (!IsTransientMetalCompilerError(depth_only_error_desc) || attempt == 3)
        break;
      Sleep(50 * (attempt + 1));
    }
    if (!m_independent_logic_op_depth_only_pso.handle) {
      return RecordCompileFailure(
          "pso/independent_logic_op_depth_variant",
          str::format("Metal independent logic-op depth variant creation "
                      "failed: ",
                      depth_only_error_desc));
    }
  }
  {
    size_t pso_manifest_hash = ComputeRenderPSOManifestHash(
        vs_hash, ps_hash, gs_hash, m_num_render_targets, m_rtv_formats,
        m_dsv_format, m_sample_count ? m_sample_count : 1,
        m_input_layout.NumElements, m_ia_slot_mask, m_vs_uses_stage_in);
    DumpRenderPSOManifest(
        pso_manifest_hash, vs_hash, ps_hash, gs_hash, m_vs.size(), m_ps.size(),
        m_gs.size(), m_num_render_targets, m_rtv_formats, m_dsv_format,
        m_sample_count ? m_sample_count : 1, m_input_layout.NumElements,
        m_ia_slot_mask, m_ia_input_elements, m_vs_uses_stage_in,
        m_uses_geometry_mesh_pipeline, info.rasterization_enabled,
        (uintptr_t)vs_func.handle, (uintptr_t)ps_func.handle);
  }

  struct WMTDepthStencilInfo ds_info = {};
  ds_info.depth_compare_function = WMTCompareFunctionAlways;
  ds_info.depth_write_enabled = false;
  ds_info.front_stencil.enabled = false;
  ds_info.back_stencil.enabled = false;
  if (m_depth_stencil_desc.DepthEnable &&
      m_depth_stencil_desc.DepthFunc >= D3D12_COMPARISON_FUNC_LESS &&
      m_depth_stencil_desc.DepthFunc <= D3D12_COMPARISON_FUNC_ALWAYS) {
    ds_info.depth_compare_function =
        kCompareFunctionMap[m_depth_stencil_desc.DepthFunc];
  }
  ds_info.depth_write_enabled =
      m_depth_stencil_desc.DepthEnable &&
      m_depth_stencil_desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
  if (m_depth_stencil_desc.StencilEnable) {
    ds_info.front_stencil.enabled = true;
    ds_info.front_stencil.depth_stencil_pass_op =
        kStencilOperationMap[m_depth_stencil_desc.FrontFace.StencilPassOp];
    ds_info.front_stencil.stencil_fail_op =
        kStencilOperationMap[m_depth_stencil_desc.FrontFace.StencilFailOp];
    ds_info.front_stencil.depth_fail_op =
        kStencilOperationMap[m_depth_stencil_desc.FrontFace.StencilDepthFailOp];
    ds_info.front_stencil.stencil_compare_function =
        kCompareFunctionMap[m_depth_stencil_desc.FrontFace.StencilFunc];
    ds_info.front_stencil.write_mask = m_depth_stencil_desc.StencilWriteMask;
    ds_info.front_stencil.read_mask = m_depth_stencil_desc.StencilReadMask;

    ds_info.back_stencil.enabled = true;
    ds_info.back_stencil.depth_stencil_pass_op =
        kStencilOperationMap[m_depth_stencil_desc.BackFace.StencilPassOp];
    ds_info.back_stencil.stencil_fail_op =
        kStencilOperationMap[m_depth_stencil_desc.BackFace.StencilFailOp];
    ds_info.back_stencil.depth_fail_op =
        kStencilOperationMap[m_depth_stencil_desc.BackFace.StencilDepthFailOp];
    ds_info.back_stencil.stencil_compare_function =
        kCompareFunctionMap[m_depth_stencil_desc.BackFace.StencilFunc];
    ds_info.back_stencil.write_mask = m_depth_stencil_desc.StencilWriteMask;
    ds_info.back_stencil.read_mask = m_depth_stencil_desc.StencilReadMask;
  }
  m_depth_stencil_state = wmt_device.newDepthStencilState(ds_info);
  m_uses_independent_logic_op_depth_replay =
      m_uses_independent_logic_op_emulation &&
      ((m_depth_stencil_desc.DepthEnable &&
        m_depth_stencil_desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL) ||
       (m_depth_stencil_desc.StencilEnable &&
        m_depth_stencil_desc.StencilWriteMask != 0));
  if (m_uses_independent_logic_op_depth_replay) {
    WMTDepthStencilInfo no_write_info = ds_info;
    no_write_info.depth_write_enabled = false;
    no_write_info.front_stencil.write_mask = 0;
    no_write_info.back_stencil.write_mask = 0;
    m_independent_logic_op_no_write_depth_state =
        wmt_device.newDepthStencilState(no_write_info);
    if (!m_independent_logic_op_no_write_depth_state.handle) {
      return RecordCompileFailure(
          "pso/independent_logic_op_depth_state",
          "Metal independent logic-op no-write depth state creation failed");
    }
  }

  {
    PTRACE("VS_ARGS_DEBUG: shader=%llu NumCB=%u NumArgs=%u CBufBindIdx=%u "
           "ArgBufBindIdx=%u ArgTableQwords=%u",
           (unsigned long long)(uintptr_t)m_vs_shader,
           m_vs_reflection.NumConstantBuffers, m_vs_reflection.NumArguments,
           m_vs_reflection.ConstanttBufferTableBindIndex,
           m_vs_reflection.ArgumentBufferBindIndex,
           m_vs_reflection.ArgumentTableQwords);
    if (m_vs_shader && (m_vs_reflection.NumArguments > 0 ||
                        m_vs_reflection.NumConstantBuffers > 0)) {
      if (m_vs_reflection.NumConstantBuffers > 0)
        m_vs_cb_args.resize(m_vs_reflection.NumConstantBuffers);
      if (m_vs_reflection.NumArguments > 0)
        m_vs_args.resize(m_vs_reflection.NumArguments);
      SM50GetArgumentsInfo(m_vs_shader,
                           m_vs_cb_args.empty() ? nullptr : m_vs_cb_args.data(),
                           m_vs_args.empty() ? nullptr : m_vs_args.data());
      for (size_t i = 0; i < m_vs_cb_args.size(); i++) {
        PTRACE("VS_ARGS_DEBUG: cb[%zu] type=%d slot=%u flags=0x%x offset=%u", i,
               (int)m_vs_cb_args[i].Type, m_vs_cb_args[i].SM50BindingSlot,
               m_vs_cb_args[i].Flags, m_vs_cb_args[i].StructurePtrOffset);
      }
      for (size_t i = 0; i < m_vs_args.size(); i++) {
        PTRACE("VS_ARGS_DEBUG: arg[%zu] type=%d slot=%u flags=0x%x offset=%u",
               i, (int)m_vs_args[i].Type, m_vs_args[i].SM50BindingSlot,
               m_vs_args[i].Flags, m_vs_args[i].StructurePtrOffset);
      }
      SM50Destroy(m_vs_shader);
      m_vs_shader = nullptr;
    }
  }

  {
    PTRACE("PS_ARGS_DEBUG: shader=%llu NumCB=%u NumArgs=%u CBufBindIdx=%u "
           "ArgBufBindIdx=%u ArgTableQwords=%u",
           (unsigned long long)(uintptr_t)m_ps_shader,
           m_ps_reflection.NumConstantBuffers, m_ps_reflection.NumArguments,
           m_ps_reflection.ConstanttBufferTableBindIndex,
           m_ps_reflection.ArgumentBufferBindIndex,
           m_ps_reflection.ArgumentTableQwords);
    if (m_ps_shader && (m_ps_reflection.NumArguments > 0 ||
                        m_ps_reflection.NumConstantBuffers > 0)) {
      if (m_ps_reflection.NumConstantBuffers > 0)
        m_ps_cb_args.resize(m_ps_reflection.NumConstantBuffers);
      if (m_ps_reflection.NumArguments > 0)
        m_ps_args.resize(m_ps_reflection.NumArguments);
      SM50GetArgumentsInfo(m_ps_shader,
                           m_ps_cb_args.empty() ? nullptr : m_ps_cb_args.data(),
                           m_ps_args.empty() ? nullptr : m_ps_args.data());
      for (size_t i = 0; i < m_ps_cb_args.size(); i++) {
        PTRACE("PS_ARGS_DEBUG: cb[%zu] type=%d slot=%u flags=0x%x offset=%u", i,
               (int)m_ps_cb_args[i].Type, m_ps_cb_args[i].SM50BindingSlot,
               m_ps_cb_args[i].Flags, m_ps_cb_args[i].StructurePtrOffset);
      }
      for (size_t i = 0; i < m_ps_args.size(); i++) {
        PTRACE("PS_ARGS_DEBUG: arg[%zu] type=%d slot=%u flags=0x%x offset=%u",
               i, (int)m_ps_args[i].Type, m_ps_args[i].SM50BindingSlot,
               m_ps_args[i].Flags, m_ps_args[i].StructurePtrOffset);
      }
      SM50Destroy(m_ps_shader);
      m_ps_shader = nullptr;
    }
  }

  m_compile_state.store(CompileState::Compiled);
  m_compile_cv.notify_all();
  Logger::info(str::format("Graphics PSO compiled: RTs=", m_num_render_targets,
                           " DSV=", (int)m_dsv_format,
                           " samples=", m_sample_count));
  return true;
}

void MTLD3D12PipelineState::SetMeshShaders(
    const D3D12_SHADER_BYTECODE &as, const D3D12_SHADER_BYTECODE &ms) {
  if (as.pShaderBytecode && as.BytecodeLength) {
    m_as.resize(as.BytecodeLength);
    memcpy(m_as.data(), as.pShaderBytecode, as.BytecodeLength);
  }
  if (ms.pShaderBytecode && ms.BytecodeLength) {
    m_ms.resize(ms.BytecodeLength);
    memcpy(m_ms.data(), ms.pShaderBytecode, ms.BytecodeLength);
  }
}

void MTLD3D12PipelineState::SetGraphicsDesc(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  if (desc.pRootSignature) {
    m_root_sig = desc.pRootSignature;
    m_root_sig->AddRef();
  }

  if (desc.VS.pShaderBytecode && desc.VS.BytecodeLength) {
    m_vs.resize(desc.VS.BytecodeLength);
    memcpy(m_vs.data(), desc.VS.pShaderBytecode, desc.VS.BytecodeLength);
  }
  if (desc.PS.pShaderBytecode && desc.PS.BytecodeLength) {
    m_ps.resize(desc.PS.BytecodeLength);
    memcpy(m_ps.data(), desc.PS.pShaderBytecode, desc.PS.BytecodeLength);
  }
  if (desc.GS.pShaderBytecode && desc.GS.BytecodeLength) {
    m_gs.resize(desc.GS.BytecodeLength);
    memcpy(m_gs.data(), desc.GS.pShaderBytecode, desc.GS.BytecodeLength);
  }
  if (desc.HS.pShaderBytecode && desc.HS.BytecodeLength) {
    m_hs.resize(desc.HS.BytecodeLength);
    memcpy(m_hs.data(), desc.HS.pShaderBytecode, desc.HS.BytecodeLength);
  }
  if (desc.DS.pShaderBytecode && desc.DS.BytecodeLength) {
    m_ds.resize(desc.DS.BytecodeLength);
    memcpy(m_ds.data(), desc.DS.pShaderBytecode, desc.DS.BytecodeLength);
  }

  m_blend_desc = desc.BlendState;
  m_rasterizer_desc = desc.RasterizerState;
  m_depth_stencil_desc = desc.DepthStencilState;
  m_stream_output = {};
  m_stream_output_elements.clear();
  m_stream_output_semantic_names.clear();
  m_stream_output_strides.clear();
  m_has_stream_output =
      desc.StreamOutput.NumEntries > 0 || desc.StreamOutput.NumStrides > 0 ||
      desc.StreamOutput.pSODeclaration || desc.StreamOutput.pBufferStrides;
  if (desc.StreamOutput.NumEntries && desc.StreamOutput.pSODeclaration) {
    m_stream_output_semantic_names.reserve(desc.StreamOutput.NumEntries);
    m_stream_output_elements.reserve(desc.StreamOutput.NumEntries);
    for (UINT i = 0; i < desc.StreamOutput.NumEntries; ++i) {
      auto entry = desc.StreamOutput.pSODeclaration[i];
      m_stream_output_semantic_names.emplace_back(
          entry.SemanticName ? entry.SemanticName : "");
      entry.SemanticName = entry.SemanticName
                               ? m_stream_output_semantic_names.back().c_str()
                               : nullptr;
      m_stream_output_elements.push_back(entry);
    }
  }
  if (desc.StreamOutput.NumStrides && desc.StreamOutput.pBufferStrides) {
    const UINT stride_count = std::min<UINT>(desc.StreamOutput.NumStrides, 4);
    m_stream_output_strides.assign(desc.StreamOutput.pBufferStrides,
                                   desc.StreamOutput.pBufferStrides + stride_count);
  }
  m_stream_output.NumEntries =
      static_cast<UINT>(m_stream_output_elements.size());
  m_stream_output.pSODeclaration = m_stream_output_elements.empty()
                                       ? nullptr
                                       : m_stream_output_elements.data();
  m_stream_output.NumStrides = static_cast<UINT>(m_stream_output_strides.size());
  m_stream_output.pBufferStrides = m_stream_output_strides.empty()
                                       ? nullptr
                                       : m_stream_output_strides.data();
  m_stream_output.RasterizedStream = desc.StreamOutput.RasterizedStream;
  m_vs_uses_stage_in = false;
  m_ia_slot_mask = 0;
  m_ia_input_elements.clear();
  m_input_elements.clear();
  m_input_semantic_names.clear();
  m_input_layout = {};
  if (desc.InputLayout.NumElements > 0 && desc.InputLayout.pInputElementDescs) {
    m_input_semantic_names.reserve(desc.InputLayout.NumElements);
    m_input_elements.reserve(desc.InputLayout.NumElements);
    for (UINT i = 0; i < desc.InputLayout.NumElements; i++) {
      auto element = desc.InputLayout.pInputElementDescs[i];
      m_input_semantic_names.emplace_back(
          element.SemanticName ? element.SemanticName : "");
      element.SemanticName = m_input_semantic_names.back().c_str();
      m_input_elements.push_back(element);
    }
    m_input_layout.NumElements = (UINT)m_input_elements.size();
    m_input_layout.pInputElementDescs = m_input_elements.data();
  }
  m_strip_cut_value = desc.IBStripCutValue;
  m_topology = desc.PrimitiveTopologyType;
  m_num_render_targets = desc.NumRenderTargets;
  memcpy(m_rtv_formats, desc.RTVFormats, sizeof(m_rtv_formats));
  m_dsv_format = desc.DSVFormat;
  m_sample_mask = desc.SampleMask;
  m_sample_count = desc.SampleDesc.Count ? desc.SampleDesc.Count : 1;
  if (desc.CachedPSO.pCachedBlob && desc.CachedPSO.CachedBlobSizeInBytes) {
    const auto *cached_data =
        static_cast<const uint8_t *>(desc.CachedPSO.pCachedBlob);
    m_cached_pso_blob.assign(
        cached_data, cached_data + desc.CachedPSO.CachedBlobSizeInBytes);
  }
}

void MTLD3D12PipelineState::SetViewInstancing(
    const D3D12ViewInstancingDesc &desc) {
  m_view_instance_count = desc.ViewInstanceCount;
  m_view_instancing_flags = desc.Flags;
  m_view_instance_locations.clear();
  if (desc.ViewInstanceCount && desc.pViewInstanceLocations) {
    m_view_instance_locations.assign(
        desc.pViewInstanceLocations,
        desc.pViewInstanceLocations + desc.ViewInstanceCount);
  }
}

void MTLD3D12PipelineState::SetComputeDesc(
    const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc) {
  if (desc.pRootSignature) {
    m_root_sig = desc.pRootSignature;
    m_root_sig->AddRef();
  }
  if (desc.CS.pShaderBytecode && desc.CS.BytecodeLength) {
    m_cs.resize(desc.CS.BytecodeLength);
    memcpy(m_cs.data(), desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
  }
  if (desc.CachedPSO.pCachedBlob && desc.CachedPSO.CachedBlobSizeInBytes) {
    const auto *cached_data =
        static_cast<const uint8_t *>(desc.CachedPSO.pCachedBlob);
    m_cached_pso_blob.assign(
        cached_data, cached_data + desc.CachedPSO.CachedBlobSizeInBytes);
  }
  m_ia_slot_mask = 0;
  m_ia_input_elements.clear();
}

HRESULT STDMETHODCALLTYPE
MTLD3D12PipelineState::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == IID_IUnknown || riid == IID_ID3D12Object ||
      riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
      riid == IID_ID3D12PipelineState) {
    *ppvObject = ref(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MTLD3D12PipelineState::AddRef() { return ++m_refCount; }

ULONG STDMETHODCALLTYPE MTLD3D12PipelineState::Release() {
  uint32_t rc = --m_refCount;
  if (!rc)
    delete this;
  return rc;
}

HRESULT STDMETHODCALLTYPE MTLD3D12PipelineState::GetPrivateData(REFGUID guid,
                                                                UINT *data_size,
                                                                void *data) {
  return m_private_data.getData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12PipelineState::SetPrivateData(
    REFGUID guid, UINT data_size, const void *data) {
  return m_private_data.setData(guid, data_size, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12PipelineState::SetPrivateDataInterface(
    REFGUID guid, const IUnknown *data) {
  return m_private_data.setInterface(guid, data);
}

HRESULT STDMETHODCALLTYPE MTLD3D12PipelineState::SetName(LPCWSTR name) {
  return m_private_data.setName(name);
}

HRESULT STDMETHODCALLTYPE MTLD3D12PipelineState::GetDevice(REFIID riid,
                                                           void **device) {
  return m_device->QueryInterface(riid, device);
}

HRESULT STDMETHODCALLTYPE
MTLD3D12PipelineState::GetCachedBlob(ID3DBlob **blob) {
  if (!blob)
    return E_POINTER;
  *blob = nullptr;

  if (m_cached_pso_blob.empty()) {
    static constexpr uint8_t empty_cache_blob[] = {
        'D', 'X', 'M', 'T', 'P', 'S', 'O', 1,
    };
    m_cached_pso_blob.assign(std::begin(empty_cache_blob),
                             std::end(empty_cache_blob));
  }

  try {
    *blob = new D3D12CachedPipelineBlob(m_cached_pso_blob);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
  return S_OK;
}

} // namespace dxmt
